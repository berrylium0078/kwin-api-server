// SPDX-License-Identifier: MIT
#include "file_util.hpp"

#include <system_error>

namespace kas {

std::string copy_file_overwrite(const std::filesystem::path& src,
                                const std::filesystem::path& dst) {
    try {
        std::filesystem::create_directories(dst.parent_path());
        std::filesystem::copy_file(src, dst,
                                   std::filesystem::copy_options::overwrite_existing);
        return {};
    } catch (const std::filesystem::filesystem_error& e) {
        return e.what();
    } catch (const std::system_error& e) {
        return e.what();
    }
}

std::string stage_kwinscript(const std::filesystem::path& script_path,
                             const std::filesystem::path& work_dir) {
    std::error_code ec;
    const std::filesystem::path src = std::filesystem::absolute(script_path, ec);
    const std::filesystem::path dst = std::filesystem::absolute(work_dir / "kwinscript.js", ec);
    if (src == dst) {
        return {}; // already in place (KWIN_SCRIPT_PATH points at the working copy)
    }
    return copy_file_overwrite(src, dst);
}

} // namespace kas
