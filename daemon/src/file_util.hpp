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
// together with the working directory when the service stops. If src already
// equals the destination (e.g. the unit points KWIN_SCRIPT_PATH directly at
// the working copy) this is a no-op. Returns an empty string on success.
std::string stage_kwinscript(const std::filesystem::path& script_path,
                             const std::filesystem::path& work_dir);

} // namespace kas
