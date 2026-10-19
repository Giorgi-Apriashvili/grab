#include "space.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)
#else
#include <cstdlib>
#endif

using namespace grab;

#ifndef NDEBUG
namespace {

void set_env(const char* name, const char* value) {
#ifdef _WIN32
    SetEnvironmentVariableA(name, value);
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

} // namespace
#endif

TEST_CASE("whether a download fits, with what is queued on the drive") {
    CHECK(space::fits(10, 0, 10));
    CHECK(space::fits(4, 6, 10));
    CHECK_FALSE(space::fits(4, 7, 10));
    CHECK_FALSE(space::fits(11, 0, 10));
    CHECK(space::fits(0, 0, 0));
    CHECK_FALSE(space::fits(UINT64_MAX, 1, UINT64_MAX)); // no overflow
    CHECK(space::shortage("E:", 48ULL << 30, 31ULL << 30) == "not enough free space on E: (48.0 GiB needed, 31.0 GiB free)");
}

TEST_CASE("the drive of a folder that may not exist yet") {
    const auto base = std::filesystem::temp_directory_path() / "grab-test-space";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    const auto missing = base / "not" / "made" / "yet";
    CHECK(space::drive_of(missing) == space::drive_of(base)); // from the nearest folder there is
    CHECK(space::free_bytes(missing).has_value());
    CHECK(space::free_bytes(missing) == space::free_bytes(base));
#ifdef _WIN32
    CHECK(space::drive_of(std::filesystem::path(L"C:\\Windows\\no\\such\\folder")) == "C:");
#endif
#ifndef NDEBUG
    // Debug builds: a full drive to test against, without filling one.
    set_env("GRAB_TEST_FREE_BYTES", "1234");
    CHECK(space::free_bytes(base) == 1234);
    set_env("GRAB_TEST_FREE_BYTES", "lots");
    CHECK(space::free_bytes(base) != 1234); // not a number: ignored
    set_env("GRAB_TEST_FREE_BYTES", nullptr);
#endif
    std::filesystem::remove_all(base);
}

TEST_CASE("what a file still needs on disk") {
    const auto base = std::filesystem::temp_directory_path() / "grab-test-space-needed";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    const auto target = base / "a.mkv";
    CHECK(space::still_needed(target, 100) == 100); // nothing there
    {
        std::ofstream out(target, std::ios::binary);
        out << std::string(100, 'x');
    }
    CHECK(space::still_needed(target, 100) == 0);   // a complete copy
    CHECK(space::still_needed(target, 500) == 500); // another file of that name: fetched again
    std::filesystem::remove_all(base);
}
