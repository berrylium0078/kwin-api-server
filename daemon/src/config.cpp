// SPDX-License-Identifier: MIT
#include "config.hpp"

#include <algorithm>
#include <cctype>

namespace kas {

namespace {

const char* kEnvServiceName = "KWIN_API_SERVICE_NAME";
const char* kEnvScriptPath = "KWIN_SCRIPT_PATH";
const char* kEnvPluginName = "KWIN_PLUGIN_NAME";
const char* kEnvWorkDir = "KWIN_WORK_DIR";
const char* kEnvLoadRetries = "KWIN_LOAD_RETRIES";
const char* kEnvLoadRetryDelayMs = "KWIN_LOAD_RETRY_DELAY_MS";
const char* kEnvMaxClients = "KWIN_MAX_CLIENTS";
const char* kEnvRxBufferCap = "KWIN_RX_BUFFER_CAP";
const char* kEnvTxBufferCap = "KWIN_TX_BUFFER_CAP";
const char* kEnvDebug = "KWIN_DEBUG";

// Smallest non-zero buffer capacity we accept. The proto buffers only need a
// handful of bytes (RxBuffer >= 2, TxBuffer >= 5), but a single message can be
// up to 1 MB and anything below 64 bytes is far too small to be useful; more
// importantly, passing a too-small capacity to proto::Client would throw
// std::invalid_argument (uncaught -> terminate), so reject it at config time.
constexpr size_t kMinBufferCap = 64;

std::string get_env(const std::map<std::string, std::string>& env, const char* key) {
    auto it = env.find(key);
    return it == env.end() ? std::string{} : it->second;
}

// Parse a non-negative integer; on failure push an error and return fallback.
int parse_int(const std::string& value, int fallback, const std::string& name,
              Config& cfg) {
    if (value.empty()) {
        return fallback;
    }
    try {
        size_t pos = 0;
        long parsed = std::stol(value, &pos);
        if (pos != value.size() || parsed < 0) {
            cfg.errors.push_back(name + " must be a non-negative integer, got \"" + value + "\"");
            return fallback;
        }
        return static_cast<int>(parsed);
    } catch (const std::exception&) {
        cfg.errors.push_back(name + " must be a non-negative integer, got \"" + value + "\"");
        return fallback;
    }
}

// Parse a buffer capacity in bytes: 0 (default) or >= kMinBufferCap.
// Returns 0 on error (an entry is pushed to cfg.errors).
size_t parse_buffer_cap(const std::string& value, const std::string& name, Config& cfg) {
    if (value.empty()) {
        return 0;
    }
    try {
        size_t pos = 0;
        long parsed = std::stol(value, &pos);
        if (pos != value.size() || parsed < 0) {
            cfg.errors.push_back(name + " must be a non-negative integer, got \"" + value + "\"");
            return 0;
        }
        if (parsed != 0 && static_cast<size_t>(parsed) < kMinBufferCap) {
            cfg.errors.push_back(name + " must be 0 (default) or at least " +
                                 std::to_string(kMinBufferCap) + ", got \"" + value + "\"");
            return 0;
        }
        return static_cast<size_t>(parsed);
    } catch (const std::exception&) {
        cfg.errors.push_back(name + " must be a non-negative integer, got \"" + value + "\"");
        return 0;
    }
}

} // namespace

bool is_valid_dbus_name(const std::string& name) {
    if (name.empty() || name.size() > 255) {
        return false;
    }
    // Per the D-Bus specification, well-known names must contain at least one
    // '.'; unique names additionally start with ':' (not valid here).
    if (name.find('.') == std::string::npos) {
        return false;
    }
    // Well-known names: elements separated by '.', each element non-empty,
    // starting with a letter or '_', containing [A-Za-z0-9_-].
    bool expect_element_start = true;
    for (char c : name) {
        if (c == '.') {
            if (expect_element_start) {
                return false; // empty element (leading '.', trailing '.', or "..")
            }
            expect_element_start = true;
            continue;
        }
        if (expect_element_start) {
            if (!(std::isalpha(static_cast<unsigned char>(c)) || c == '_')) {
                return false;
            }
            expect_element_start = false;
        } else if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) {
            return false;
        }
    }
    return !expect_element_start;
}

std::string object_path_for_name(const std::string& name) {
    std::string path = "/";
    for (char c : name) {
        path.push_back(c == '.' ? '/' : c);
    }
    return path;
}

Config load_config(const std::map<std::string, std::string>& env,
                   const std::vector<std::string>& args) {
    Config cfg;

    // --- CLI flags ---------------------------------------------------------
    for (const auto& arg : args) {
        if (arg == "-h" || arg == "--help") {
            cfg.show_help = true;
        } else if (arg == "-V" || arg == "--version") {
            cfg.show_version = true;
        } else if (arg == "--check") {
            cfg.check_only = true;
        } else {
            cfg.errors.push_back("unknown argument: " + arg);
        }
    }

    // --- environment -------------------------------------------------------
    cfg.service_name = get_env(env, kEnvServiceName);
    if (cfg.service_name.empty()) {
        cfg.service_name = "org.example.KwinApiServer";
    }
    cfg.script_path = get_env(env, kEnvScriptPath);
    cfg.plugin_name = get_env(env, kEnvPluginName);
    cfg.work_dir = get_env(env, kEnvWorkDir);
    if (cfg.work_dir.empty()) {
        cfg.work_dir = std::filesystem::current_path();
    }
    cfg.load_retries = parse_int(get_env(env, kEnvLoadRetries), cfg.load_retries,
                                 kEnvLoadRetries, cfg);
    cfg.load_retry_delay_ms = parse_int(get_env(env, kEnvLoadRetryDelayMs),
                                        cfg.load_retry_delay_ms, kEnvLoadRetryDelayMs, cfg);
    cfg.max_clients = parse_int(get_env(env, kEnvMaxClients), cfg.max_clients,
                                kEnvMaxClients, cfg);
    cfg.rx_buffer_cap = parse_buffer_cap(get_env(env, kEnvRxBufferCap), kEnvRxBufferCap, cfg);
    cfg.tx_buffer_cap = parse_buffer_cap(get_env(env, kEnvTxBufferCap), kEnvTxBufferCap, cfg);
    std::string debug = get_env(env, kEnvDebug);
    cfg.debug = (debug == "1" || debug == "true" || debug == "yes");

    // --- validation --------------------------------------------------------
    if (!is_valid_dbus_name(cfg.service_name)) {
        cfg.errors.push_back(std::string(kEnvServiceName) + " is not a valid D-Bus well-known name: \"" +
                             cfg.service_name + "\"");
    }
    // --check only verifies socket + D-Bus name, so the KWin-related
    // variables are not required in that mode.
    if (!cfg.check_only) {
        if (cfg.script_path.empty()) {
            cfg.errors.push_back(std::string(kEnvScriptPath) + " is required (path of the bundled kwinscript.js)");
        }
        if (cfg.plugin_name.empty()) {
            cfg.errors.push_back(std::string(kEnvPluginName) + " is required (KWin plugin name)");
        }
    }
    if (cfg.max_clients < 1) {
        cfg.errors.push_back(std::string(kEnvMaxClients) + " must be >= 1");
    }
    return cfg;
}

} // namespace kas
