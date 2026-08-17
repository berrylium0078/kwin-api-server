// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace kas {

// Runtime configuration of the daemon, resolved from environment variables
// (and CLI flags). See README.md for the full list of variables.
struct Config {
    // D-Bus well-known name to register on the session bus (KWIN_API_SERVICE_NAME).
    std::string service_name = "org.example.KwinApiServer";
    // Absolute path of the bundled KWin script to load (KWIN_SCRIPT_PATH).
    std::filesystem::path script_path;
    // pluginName handed to org.kde.kwin.Scripting.loadScript (KWIN_PLUGIN_NAME).
    std::string plugin_name;
    // Working directory: service.socket and the copied kwinscript.js live here
    // (KWIN_WORK_DIR; defaults to the current working directory, which systemd
    // points at its RuntimeDirectory so both files are cleaned up by systemd).
    std::filesystem::path work_dir;
    // How many times loadScript is retried while KWin is not yet reachable
    // (KWIN_LOAD_RETRIES; the unit's After=graphical-session.target may race).
    int load_retries = 30;
    // Delay between retries in milliseconds (KWIN_LOAD_RETRY_DELAY_MS).
    int load_retry_delay_ms = 1000;
    // Maximum number of concurrently connected unix-socket clients
    // (KWIN_MAX_CLIENTS).
    int max_clients = 64;
    // Verbose logging (KWIN_DEBUG=1).
    bool debug = false;

    // --check: verify socket + D-Bus name, skip KWin interaction, exit 0.
    bool check_only = false;
    bool show_help = false;
    bool show_version = false;

    // Configuration/validation errors (empty when the config is usable).
    std::vector<std::string> errors;
};

// Resolve a Config from an environment map plus CLI arguments. Pure function,
// so it is unit-testable without touching the real environment.
Config load_config(const std::map<std::string, std::string>& env,
                   const std::vector<std::string>& args);

// True when `name` is a syntactically valid D-Bus well-known name
// (e.g. "org.example.KwinApiServer").
bool is_valid_dbus_name(const std::string& name);

// Derive the daemon's own D-Bus object path from its service name, e.g.
// org.example.KwinApiServer -> /org/example/KwinApiServer.
std::string object_path_for_name(const std::string& name);

} // namespace kas
