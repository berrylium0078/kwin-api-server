// SPDX-License-Identifier: MIT
#pragma once

#include <string>

namespace kas {

// Minimal leveled logger. All output goes to stderr, which systemd captures
// into the journal when the daemon runs as a systemd service.
void log_set_debug(bool enabled);
bool log_debug_enabled();

void log_debug(const std::string& message);
void log_info(const std::string& message);
void log_warn(const std::string& message);
void log_error(const std::string& message);

} // namespace kas
