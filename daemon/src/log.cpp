// SPDX-License-Identifier: MIT
#include "log.hpp"

#include <cstdio>
#include <ctime>
#include <unistd.h>

namespace kas {

namespace {
bool g_debug = false;

void emit(const char* level, const char* color, const std::string& message) {
    char ts[32] = {0};
    std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
    if (::localtime_r(&now, &tm_buf)) {
        std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm_buf);
    }
    std::fprintf(stderr, "\033[%sm%s kwin-api-server[%d] %s\033[0m %s\n",
                 color, ts, static_cast<int>(::getpid()), level, message.c_str());
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
