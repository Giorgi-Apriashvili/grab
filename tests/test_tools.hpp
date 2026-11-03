#pragma once

// The programs the tests compare grab with, which grab itself doesn't ship: rclone 1.75.1 in
// build\<preset>\test-tools (tests/CMakeLists.txt, GRAB_TEST_TOOLS), one folder above
// build\<preset>\tests where grab_tests.exe runs. A case that needs it is skipped without it.

#include "util.hpp"

#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>

namespace grab::test {

inline constexpr std::string_view no_test_rclone =
    "no test-tools\\rclone.exe next to the build (GRAB_TEST_TOOLS)";

// The test rclone when it is there.
inline std::optional<std::filesystem::path> test_rclone() {
    auto rclone = util::self_exe_path().parent_path().parent_path() / "test-tools" / "rclone.exe";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(rclone, ec)) return std::nullopt;
    return rclone;
}

} // namespace grab::test
