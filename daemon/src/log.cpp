// SPDX-License-Identifier: MIT
#include "log.hpp"

#include <cstdio>

namespace kas {

namespace {
bool g_debug = false;

// Deliberately no timestamp / process name in the line: when the daemon runs
// under systemd, the journal already prefixes every line with those
// (e.g. `systemctl status` shows them), so repeating them here would be
// redundant noise.
void emit(const char* level, const char* color, const std::string& message) {
    std::fprintf(stderr, "\033[%sm%s\033[0m %s\n", color, level, message.c_str());
    std::fflush(stderr);
}
} // namespace

void log_set_debug(bool enabled) { g_debug = enabled; }
bool log_debug_enabled() { return g_debug; }

void log_debug(const std::string& message) {
    if (g_debug) {
        emit("debug", "90", message);
    }
}
void log_info(const std::string& message) { emit("info", "36", message); }
void log_warn(const std::string& message) { emit("warn", "33", message); }
void log_error(const std::string& message) { emit("error", "31", message); }

} // namespace kas
