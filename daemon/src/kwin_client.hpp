// SPDX-License-Identifier: MIT
#pragma once

#include <string>

#include <systemd/sd-bus.h>

namespace kas {

struct LoadResult {
    int id = -1;              // script id returned by KWin (>= 0 on success, -1 = failure)
    bool name_missing = false; // org.kde.KWin not reachable yet (retryable)
    std::string error;        // human-readable error (empty on success)
};

// Client for KWin's scripting D-Bus API (org.kde.KWin). The daemon uses it to
// load the staged kwinscript.js, run it and — on shutdown — unload it:
//
//   org.kde.kwin.Scripting.loadScript(filePath: s, pluginName: s) -> i
//   org.kde.kwin.Script.run()                        on /Scripting/Script<id>
//   org.kde.kwin.Scripting.unloadScript(pluginName: s) -> b
class KwinClient {
public:
    explicit KwinClient(sd_bus* bus) : bus_(bus) {}

    LoadResult load_script(const std::string& file_path, const std::string& plugin_name);
    bool run_script(int script_id);
    bool unload_script(const std::string& plugin_name);

private:
    sd_bus* bus_;
};

} // namespace kas
