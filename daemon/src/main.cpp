// SPDX-License-Identifier: MIT
//
// kwin-api-daemon — a Linux desktop background service that ties together
// a unix socket, a session-bus D-Bus service and KWin scripting, all driven
// from one sd-event loop:
//
//   1. bind() service.socket in the working directory (under systemd this is
//      RuntimeDirectory=%t/kwin-api-server, so systemd cleans the file up);
//   2. register a D-Bus service name (KWIN_API_SERVICE_NAME) on the session
//      bus — D-Bus releases the name when the connection closes;
//   3. copy the bundled KWin script (KWIN_SCRIPT_PATH) to kwinscript.js in
//      the working directory and load/run it through KWin's scripting API:
//        org.kde.kwin.Scripting.loadScript(filePath, pluginName) -> id
//        /Scripting/Script<id> org.kde.kwin.Script.run()
//      pluginName comes from KWIN_PLUGIN_NAME. On shutdown the script is
//      unloaded again (unloadScript) — and the systemd unit's ExecStopPost
//      does the same as a safety net for SIGKILL/crash.
//
// All socket and D-Bus I/O is multiplexed concurrently (non-blocking) by the
// same sd_event loop via libsystemd's sd-bus and sd-event.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <signal.h>
#include <string>
#include <vector>

#include <sys/signalfd.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>
#include <time.h>
#include <unistd.h>

// `environ` is only declared by <unistd.h> under _GNU_SOURCE/_DEFAULT_SOURCE;
// declare it explicitly so the file also compiles with strict -std=c++20.
extern char** environ;

#include "config.hpp"
#include "dbus_service.hpp"
#include "file_util.hpp"
#include "kwin_client.hpp"
#include "log.hpp"
#include "socket_server.hpp"

#ifndef KAS_VERSION
#define KAS_VERSION "unknown"
#endif

namespace {

void print_usage(FILE* out) {
    std::fprintf(out,
        "Usage: kwin-api-daemon [options]\n"
        "\n"
        "A desktop background service binding a unix socket, a session-bus\n"
        "D-Bus service and a KWin script, all on one sd-event loop.\n"
        "\n"
        "Options:\n"
        "  -h, --help     show this help and exit\n"
        "  -V, --version  show the version and exit\n"
        "  --check        verify socket + D-Bus name registration, then exit 0\n"
        "\n"
        "Environment:\n"
        "  KWIN_API_SERVICE_NAME  D-Bus name to register (default org.example.KwinApiServer)\n"
        "  KWIN_SCRIPT_PATH       path of the bundled kwinscript.js (required)\n"
        "  KWIN_PLUGIN_NAME       pluginName for KWin scripting (required)\n"
        "  KWIN_WORK_DIR          working directory (default: cwd; under systemd this is\n"
        "                         the RuntimeDirectory, cleaned up by systemd)\n"
        "  KWIN_LOAD_RETRIES      loadScript retries while KWin is unreachable (default 30)\n"
        "  KWIN_LOAD_RETRY_DELAY_MS  delay between retries in ms (default 1000)\n"
        "  KWIN_MAX_CLIENTS       max concurrent unix-socket clients (default 64)\n"
        "  KWIN_DEBUG=1           verbose logging\n");
}

void sleep_ms(int ms) {
    struct timespec ts{};
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
    nanosleep(&ts, nullptr);
}

int on_signal(sd_event_source* /*source*/, const struct signalfd_siginfo* info,
              void* userdata) {
    auto* event = static_cast<sd_event*>(userdata);
    kas::log_info("received signal " + std::to_string(info->ssi_signo) + ", shutting down");
    // Note: sd_bus_set_close_on_exit(bus, 0) keeps the D-Bus connection usable
    // after the loop returns, so the shutdown path can still call
    // org.kde.kwin.Scripting.unloadScript().
    sd_event_exit(event, 0);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    // A client that dies while we write to it must not kill the daemon.
    ::signal(SIGPIPE, SIG_IGN);

    // sd_event_add_signal() uses signalfd, which only sees *blocked* signals.
    // Block SIGTERM/SIGINT up front; the event loop then delivers them to our
    // handler instead of the default disposition (which would kill the daemon).
    sigset_t signal_mask;
    sigemptyset(&signal_mask);
    sigaddset(&signal_mask, SIGTERM);
    sigaddset(&signal_mask, SIGINT);
    ::sigprocmask(SIG_BLOCK, &signal_mask, nullptr);

    // Snapshot the environment into a map (keeps load_config pure/testable).
    std::map<std::string, std::string> env;
    for (char** e = environ; e && *e; ++e) {
        std::string entry(*e);
        size_t eq = entry.find('=');
        if (eq != std::string::npos) {
            env[entry.substr(0, eq)] = entry.substr(eq + 1);
        }
    }
    std::vector<std::string> args(argv + 1, argv + argc);

    kas::Config config = kas::load_config(env, args);
    kas::log_set_debug(config.debug);

    if (config.show_help) {
        print_usage(stdout);
        return 0;
    }
    if (config.show_version) {
        std::printf("kwin-api-daemon %s\n", KAS_VERSION);
        return 0;
    }
    if (!config.errors.empty()) {
        for (const auto& error : config.errors) {
            kas::log_error("configuration error: " + error);
        }
        std::fprintf(stderr, "run 'kwin-api-daemon --help' for usage\n");
        return 1;
    }

    kas::log_info("starting kwin-api-daemon " KAS_VERSION);
    kas::log_debug("service name: " + config.service_name);
    kas::log_debug("working directory: " + config.work_dir.string());

    // One event loop drives everything below.
    sd_event* event = nullptr;
    int r = sd_event_default(&event);
    if (r < 0) {
        kas::log_error("sd_event_default: " + std::string(std::strerror(-r)));
        return 1;
    }

    // 1) unix socket: bind() service.socket (systemd cleans it up)
    kas::SocketServer socket_server(config.work_dir / "service.socket", config.max_clients);
    socket_server.set_status_callback(
        [&config] { return "service=" + config.service_name; });
    r = socket_server.start(event);
    if (r < 0) {
        kas::log_error("cannot bind socket: " + socket_server.socket_path());
        sd_event_unref(event);
        return 1;
    }
    kas::log_info("socket bound: " + socket_server.socket_path());

    // 2) session-bus D-Bus service (name released when the connection closes)
    kas::DbusService dbus(config);
    r = dbus.open();
    if (r < 0) {
        socket_server.stop();
        sd_event_unref(event);
        return 1;
    }
    r = dbus.request_name();
    if (r < 0) {
        dbus.close();
        socket_server.stop();
        sd_event_unref(event);
        return 1;
    }
    r = dbus.attach(event);
    if (r < 0) {
        dbus.close();
        socket_server.stop();
        sd_event_unref(event);
        return 1;
    }
    kas::log_info("D-Bus name registered: " + config.service_name +
                  " (object path " + dbus.object_path() + ")");

    // --check: infrastructure verified, nothing else to do.
    if (config.check_only) {
        kas::log_info("check ok: socket bound and D-Bus name registered");
        dbus.close();
        socket_server.stop();
        sd_event_unref(event);
        return 0;
    }

    // 3) stage the script as kwinscript.js in the working directory
    if (!std::filesystem::exists(config.script_path) ||
        !std::filesystem::is_regular_file(config.script_path)) {
        kas::log_error("KWIN_SCRIPT_PATH does not point to a file: " +
                       config.script_path.string());
        dbus.close();
        socket_server.stop();
        sd_event_unref(event);
        return 1;
    }
    std::string stage_error = kas::stage_kwinscript(config.script_path, config.work_dir);
    if (!stage_error.empty()) {
        kas::log_error("cannot stage kwinscript.js: " + stage_error);
        dbus.close();
        socket_server.stop();
        sd_event_unref(event);
        return 1;
    }
    const std::filesystem::path staged = std::filesystem::absolute(config.work_dir / "kwinscript.js");
    kas::log_info("kwinscript staged at " + staged.string());

    // 4) load & run the script in KWin (retry while KWin is still starting)
    kas::KwinClient kwin(dbus.bus());
    kas::LoadResult load;
    for (int attempt = 1; attempt <= config.load_retries + 1; ++attempt) {
        load = kwin.load_script(staged.string(), config.plugin_name);
        if (load.id >= 0) {
            break;
        }
        if (load.name_missing && attempt <= config.load_retries) {
            kas::log_warn("org.kde.KWin not reachable (attempt " + std::to_string(attempt) +
                          "/" + std::to_string(config.load_retries) + "), retrying in " +
                          std::to_string(config.load_retry_delay_ms) + " ms");
            sleep_ms(config.load_retry_delay_ms);
            continue;
        }
        break;
    }
    if (load.id < 0) {
        kas::log_error("org.kde.kwin.Scripting.loadScript failed: " + load.error);
        dbus.close();
        socket_server.stop();
        sd_event_unref(event);
        return 1;
    }
    kas::log_info("KWin script loaded: id=" + std::to_string(load.id) +
                  " plugin=" + config.plugin_name);

    if (!kwin.run_script(load.id)) {
        kas::log_error("org.kde.kwin.Script.run failed, unloading script");
        kwin.unload_script(config.plugin_name);
        dbus.close();
        socket_server.stop();
        sd_event_unref(event);
        return 1;
    }
    kas::log_info("KWin script running");

    // 5) serve until SIGTERM/SIGINT
    sd_event_source* sigterm_source = nullptr;
    sd_event_source* sigint_source = nullptr;
    sd_event_add_signal(event, &sigterm_source, SIGTERM, on_signal, event);
    sd_event_add_signal(event, &sigint_source, SIGINT, on_signal, event);

    kas::log_info("serving (SIGTERM/SIGINT to stop)");
    r = sd_event_loop(event);

    // 6) graceful shutdown: unload the script, release the name, close socket
    if (r < 0) {
        kas::log_error("event loop error: " + std::string(std::strerror(-r)));
    }
    kas::log_debug("shutdown: unloading script");
    // The bus stays open after the loop (sd_bus_set_close_on_exit(0)), so the
    // fire-and-forget unloadScript() message can still be sent and flushed.
    if (kwin.unload_script(config.plugin_name)) {
        kas::log_info("KWin script unloaded");
    } else {
        kas::log_warn("unloadScript failed (the unit's ExecStopPost retries it)");
    }
    kas::log_debug("shutdown: closing bus");
    dbus.close();
    kas::log_debug("shutdown: stopping socket server");
    socket_server.stop();
    kas::log_debug("shutdown: freeing event loop");
    sd_event_unref(event);
    kas::log_info("exiting");
    return r < 0 ? 1 : 0;
}
