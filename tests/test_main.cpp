#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <filesystem>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)
#else
#include <cstdlib>
#endif

// The tests never touch the real %APPDATA%\grab or %LOCALAPPDATA%\grab (servers, passwords, file
// lists): both point at a scratch folder for the whole run.
int main(int argc, char** argv) {
    const auto home = std::filesystem::temp_directory_path() / "grab-tests-home";
    std::error_code ec;
    std::filesystem::remove_all(home, ec);
    std::filesystem::create_directories(home / "Roaming", ec);
    std::filesystem::create_directories(home / "Local", ec);
#ifdef _WIN32
    SetEnvironmentVariableW(L"APPDATA", (home / "Roaming").c_str());
    SetEnvironmentVariableW(L"LOCALAPPDATA", (home / "Local").c_str());
#else
    setenv("XDG_CONFIG_HOME", (home / "Roaming").c_str(), 1);
    setenv("XDG_CACHE_HOME", (home / "Local").c_str(), 1);
#endif
    doctest::Context context(argc, argv);
    const int result = context.run();
    std::filesystem::remove_all(home, ec);
    return result;
}
