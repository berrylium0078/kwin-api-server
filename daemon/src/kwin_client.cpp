// SPDX-License-Identifier: MIT
#include "kwin_client.hpp"

#include <cstring>
#include <string>

#include "log.hpp"

namespace kas {

namespace {
constexpr const char* kKwinBusName = "org.kde.KWin";
constexpr const char* kScriptingPath = "/Scripting";
constexpr const char* kScriptingInterface = "org.kde.kwin.Scripting";
constexpr const char* kScriptInterface = "org.kde.kwin.Script";

LoadResult make_error(const sd_bus_error& error) {
    LoadResult res;
    res.name_missing =
        sd_bus_error_has_name(&error, "org.freedesktop.DBus.Error.NameHasNoOwner") ||
        sd_bus_error_has_name(&error, "org.freedesktop.DBus.Error.ServiceUnknown");
    res.error = std::string(error.name ? error.name : "error") + ": " +
                (error.message ? error.message : "no message");
    return res;
}
} // namespace

LoadResult KwinClient::load_script(const std::string& file_path,
                                   const std::string& plugin_name) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    int r = sd_bus_call_method(bus_, kKwinBusName, kScriptingPath, kScriptingInterface,
                               "loadScript", &error, &reply, "ss", file_path.c_str(),
                               plugin_name.c_str());
    if (r < 0) {
        LoadResult res = make_error(error);
        sd_bus_error_free(&error);
        return res;
    }

    int id = -1;
    r = sd_bus_message_read(reply, "i", &id);
    sd_bus_message_unref(reply);
    if (r < 0) {
        LoadResult res;
        res.error = "cannot read loadScript reply: " + std::string(std::strerror(-r));
        return res;
    }
    return LoadResult{id, false, {}};
}

bool KwinClient::run_script(int script_id) {
    std::string path = std::string(kScriptingPath) + "/Script" + std::to_string(script_id);
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(bus_, kKwinBusName, path.c_str(), kScriptInterface, "run",
                               &error, nullptr, "");
    if (r < 0) {
        log_error("org.kde.kwin.Script.run(" + path + "): " +
                  std::string(error.name ? error.name : "error") + ": " +
                  (error.message ? error.message : "no message"));
        sd_bus_error_free(&error);
        return false;
    }
    return true;
}

// unloadScript on shutdown is best-effort (the unit's ExecStopPost retries it
// if the daemon dies hard), so the message is sent fire-and-forget: no reply
// is waited for or read. This keeps the shutdown path free of any synchronous
// reply handling after the event loop has stopped.
bool KwinClient::unload_script(const std::string& plugin_name) {
    sd_bus_message* message = nullptr;
    int r = sd_bus_message_new_method_call(bus_, &message, kKwinBusName, kScriptingPath,
                                           kScriptingInterface, "unloadScript");
    if (r < 0) {
        log_warn("unloadScript(" + plugin_name + "): " + std::string(std::strerror(-r)));
        return false;
    }
    r = sd_bus_message_append(message, "s", plugin_name.c_str());
    if (r < 0) {
        sd_bus_message_unref(message);
        log_warn("unloadScript(" + plugin_name + "): cannot append args: " +
                 std::string(std::strerror(-r)));
        return false;
    }
    r = sd_bus_send(bus_, message, nullptr);
    sd_bus_message_unref(message);
    if (r < 0) {
        log_warn("unloadScript(" + plugin_name + "): send failed: " +
                 std::string(std::strerror(-r)));
        return false;
    }
    r = sd_bus_flush(bus_);
    if (r < 0) {
        log_warn("unloadScript(" + plugin_name + "): flush failed: " +
                 std::string(std::strerror(-r)));
        return false;
    }
    return true;
}

} // namespace kas
