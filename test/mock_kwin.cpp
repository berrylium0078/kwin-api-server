// SPDX-License-Identifier: MIT
//
// mock_kwin: a minimal stand-in for org.kde.KWin's scripting D-Bus API, used
// by test/integration.sh to exercise kwin-api-daemon end-to-end without a
// real KWin session. It registers the org.kde.KWin name on the session bus
// (DBUS_SESSION_BUS_ADDRESS) and logs every script-related call to stdout:
//
//   READY
//   CALL loadScript <filePath> <pluginName>
//   CALL run
//   CALL unloadScript <pluginName>
//
// loadScript always returns script id 0, and a vtable is registered at
// /Scripting/Script0 to answer Script.run().

#include <cstdio>
#include <cstring>
#include <string>

#include <systemd/sd-bus.h>

namespace {

int method_load_script(sd_bus_message* message, void* /*userdata*/, sd_bus_error* /*error*/) {
    const char* file_path = nullptr;
    const char* plugin_name = nullptr;
    int r = sd_bus_message_read(message, "ss", &file_path, &plugin_name);
    if (r < 0) {
        return r;
    }
    std::printf("CALL loadScript %s %s\n", file_path, plugin_name);
    std::fflush(stdout);
    return sd_bus_reply_method_return(message, "i", 0);
}

int method_run(sd_bus_message* message, void* /*userdata*/, sd_bus_error* /*error*/) {
    std::printf("CALL run\n");
    std::fflush(stdout);
    return sd_bus_reply_method_return(message, "");
}

int method_unload_script(sd_bus_message* message, void* /*userdata*/, sd_bus_error* /*error*/) {
    const char* plugin_name = nullptr;
    sd_bus_message_read(message, "s", &plugin_name);
    std::printf("CALL unloadScript %s\n", plugin_name ? plugin_name : "(null)");
    std::fflush(stdout);
    return sd_bus_reply_method_return(message, "b", 1);
}

const sd_bus_vtable kScriptingVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("loadScript", "ss", "i", method_load_script, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("unloadScript", "s", "b", method_unload_script, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

const sd_bus_vtable kScriptVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("run", "", "", method_run, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

} // namespace

int main() {
    sd_bus* bus = nullptr;
    int r = sd_bus_open_user(&bus);
    if (r < 0) {
        std::fprintf(stderr, "mock_kwin: sd_bus_open_user: %s\n", std::strerror(-r));
        return 1;
    }
    sd_bus_set_exit_on_disconnect(bus, 0);

    r = sd_bus_request_name(bus, "org.kde.KWin", 0);
    if (r < 0) {
        std::fprintf(stderr, "mock_kwin: cannot acquire org.kde.KWin: %s\n",
                     std::strerror(-r));
        sd_bus_unref(bus);
        return 1;
    }
    sd_bus_add_object_vtable(bus, nullptr, "/Scripting", "org.kde.kwin.Scripting",
                             kScriptingVtable, nullptr);
    // The daemon calls /Scripting/Script<id>.run(); mock loadScript returns 0.
    sd_bus_add_object_vtable(bus, nullptr, "/Scripting/Script0", "org.kde.kwin.Script",
                             kScriptVtable, nullptr);

    std::printf("READY\n");
    std::fflush(stdout);

    for (;;) {
        r = sd_bus_process(bus, nullptr);
        if (r < 0) {
            if (r == -ECONNRESET) {
                break; // bus went away
            }
            std::fprintf(stderr, "mock_kwin: sd_bus_process: %s\n", std::strerror(-r));
            break;
        }
        if (r > 0) {
            continue;
        }
        r = sd_bus_wait(bus, UINT64_C(1000) * 1000); // 1 s
        if (r < 0 && r != -EINTR) {
            break;
        }
    }
    sd_bus_unref(bus);
    return 0;
}
