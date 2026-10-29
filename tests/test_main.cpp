#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)
#else
#include <cstdlib>
#include <unistd.h>
#endif

// The tests never touch the real %APPDATA%\grab or %LOCALAPPDATA%\grab (servers, passwords, host
// keys, file lists), the user's home (a key_file's ~) or the user's own rclone.conf
// ($RCLONE_CONFIG): every place grab looks for its settings points at a fresh scratch folder of
// this run before any test starts, and grab's own variables that pick a configuration are cleared
// (test_config.cpp checks the defaults land there). A test that needs the real thing names it
// itself (the live cases' GRAB_TEST_* variables).
int main(int argc, char** argv) {
#ifdef _WIN32
    const auto pid = std::to_string(GetCurrentProcessId());
#else
    const auto pid = std::to_string(getpid());
#endif
    const auto home = std::filesystem::temp_directory_path() / ("grab-tests-home-" + pid);
    std::error_code ec;
    std::filesystem::remove_all(home, ec);
    for (const char* dir : {"Roaming", "Local", "Home"}) std::filesystem::create_directories(home / dir, ec);
#ifdef _WIN32
    SetEnvironmentVariableW(L"APPDATA", (home / "Roaming").c_str());
    SetEnvironmentVariableW(L"LOCALAPPDATA", (home / "Local").c_str());
    SetEnvironmentVariableW(L"HOME", (home / "Home").c_str());
    SetEnvironmentVariableW(L"USERPROFILE", (home / "Home").c_str());
    for (const wchar_t* name : {L"HOMEDRIVE", L"HOMEPATH", L"RCLONE_CONFIG", L"GRAB_CONFIG"}) {
        SetEnvironmentVariableW(name, nullptr);
    }
#else
    setenv("XDG_CONFIG_HOME", (home / "Roaming").c_str(), 1);
    setenv("XDG_CACHE_HOME", (home / "Local").c_str(), 1);
    setenv("HOME", (home / "Home").c_str(), 1);
    unsetenv("RCLONE_CONFIG");
    unsetenv("GRAB_CONFIG");
#endif
    doctest::Context context(argc, argv);
    const int result = context.run();
    std::filesystem::remove_all(home, ec);
    return result;
}
