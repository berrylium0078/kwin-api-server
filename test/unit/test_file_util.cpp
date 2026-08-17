// SPDX-License-Identifier: MIT
#include "file_util.hpp"

#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include "test_main.hpp" // TEST / CHECK / CHECK_EQ

namespace {

std::filesystem::path make_temp_dir() {
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec) /
               ("kas-file-util-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path);
    out << content;
}

} // namespace

TEST(copy_file_overwrite_basic) {
    auto dir = make_temp_dir();
    auto src = dir / "src.js";
    auto dst = dir / "sub" / "dst.js";
    write_file(src, "console.log('hi');\n");

    std::string err = kas::copy_file_overwrite(src, dst);
    CHECK(err.empty());

    std::ifstream in(dst);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK_EQ(content, "console.log('hi');\n");

    std::filesystem::remove_all(dir);
}

TEST(copy_file_overwrite_replaces) {
    auto dir = make_temp_dir();
    auto src = dir / "src.js";
    auto dst = dir / "dst.js";
    write_file(src, "new");
    write_file(dst, "old");

    std::string err = kas::copy_file_overwrite(src, dst);
    CHECK(err.empty());

    std::ifstream in(dst);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK_EQ(content, "new");

    std::filesystem::remove_all(dir);
}

TEST(copy_file_overwrite_missing_source) {
    auto dir = make_temp_dir();
    std::string err = kas::copy_file_overwrite(dir / "nope.js", dir / "dst.js");
    CHECK(!err.empty());
    std::filesystem::remove_all(dir);
}

TEST(stage_kwinscript_copies_into_workdir) {
    auto dir = make_temp_dir();
    auto src = dir / "bundle.js";
    write_file(src, "print('bundled');\n");

    std::string err = kas::stage_kwinscript(src, dir);
    CHECK(err.empty());
    CHECK(std::filesystem::exists(dir / "kwinscript.js"));
    CHECK_EQ(std::filesystem::file_size(dir / "kwinscript.js"),
             std::filesystem::file_size(src));

    std::filesystem::remove_all(dir);
}

TEST(stage_kwinscript_same_path_is_noop) {
    auto dir = make_temp_dir();
    auto script = dir / "kwinscript.js";
    write_file(script, "print('x');\n");

    // src == dst (KWIN_SCRIPT_PATH points at the working copy): must succeed
    // without trying to copy a file onto itself.
    std::string err = kas::stage_kwinscript(script, dir);
    CHECK(err.empty());

    std::filesystem::remove_all(dir);
}
