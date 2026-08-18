// SPDX-License-Identifier: MIT
#include "file_util.hpp"

#include <fstream>
#include <string>
#include <system_error>

namespace kas {

namespace {

// Placeholder inside the kwinscript bundle that stage_kwinscript rewrites to
// the daemon's runtime service name (see kwinscript/src/index.ts).
constexpr const char* kDaemonServicePlaceholder = "@DAEMON_DBUS_SERVICE@";

std::string replace_all(std::string content, const std::string& needle,
                        const std::string& replacement) {
    if (needle.empty()) {
        return content;
    }
    std::string out;
    out.reserve(content.size() + replacement.size());
    std::size_t pos = 0;
    for (;;) {
        const std::size_t hit = content.find(needle, pos);
        if (hit == std::string::npos) {
            out.append(content, pos, std::string::npos);
            break;
        }
        out.append(content, pos, hit - pos);
        out += replacement;
        pos = hit + needle.size();
    }
    return out;
}

} // namespace

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
                             const std::filesystem::path& work_dir,
                             const std::string& service_name) {
    std::error_code ec;
    const std::filesystem::path src = std::filesystem::absolute(script_path, ec);
    const std::filesystem::path dst = std::filesystem::absolute(work_dir / "kwinscript.js", ec);

    // Read the whole bundle first: this also lets us rewrite the staged copy
    // in place when src == dst (KWIN_SCRIPT_PATH pointing at the working copy).
    std::ifstream in(src, std::ios::binary);
    if (!in) {
        return "cannot open " + src.string();
    }
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    if (in.bad()) {
        return "cannot read " + src.string();
    }

    // Substitute the daemon service-name placeholder with the runtime value.
    content = replace_all(std::move(content), kDaemonServicePlaceholder, service_name);

    std::filesystem::create_directories(dst.parent_path(), ec);
    if (ec) {
        return "cannot create directory " + dst.parent_path().string() + ": " + ec.message();
    }
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out) {
        return "cannot open " + dst.string();
    }
    out << content;
    out.close();
    if (!out) {
        return "cannot write " + dst.string();
    }
    return {};
}

} // namespace kas
