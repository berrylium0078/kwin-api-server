// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <string>

namespace kas {

// Copy `src` to `dst`, overwriting an existing file. Parent directories of
// `dst` are created if missing. Returns an empty string on success, otherwise
// a human-readable error message.
std::string copy_file_overwrite(const std::filesystem::path& src,
                                const std::filesystem::path& dst);

// Stage the configured KWin script as <work_dir>/kwinscript.js — the copy the
// daemon hands to org.kde.kwin.Scripting.loadScript and that systemd removes
// together with the working directory when the service stops. Before writing,
// the @DAEMON_DBUS_SERVICE@ placeholder inside the bundle (see
// kwinscript/src/index.ts) is rewritten to the daemon's runtime D-Bus service
// name, so the script never hardcodes it. src may equal the destination
// (e.g. the unit points KWIN_SCRIPT_PATH directly at the working copy); the
// file is read first and then rewritten in place. Returns an empty string on
// success, otherwise a human-readable error message.
std::string stage_kwinscript(const std::filesystem::path& script_path,
                             const std::filesystem::path& work_dir,
                             const std::string& service_name);

} // namespace kas
