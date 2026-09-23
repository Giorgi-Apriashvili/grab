#pragma once

#include <compare>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace grab::update {

inline constexpr int exit_update_failed = 5;

inline constexpr std::string_view default_api_url =
    "https://api.github.com/repos/Giorgi-Apriashvili/grab/releases/latest";

// Registry key the Inno Setup installer writes (see AppId in installer/grab.iss.in).
inline constexpr std::string_view installer_uninstall_key =
    "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
    "{B7E9C1D4-5F2A-4C3E-9A8B-2D6F1E0C7A55}_is1";

inline constexpr std::string_view sums_asset_name = "SHA256SUMS.txt";

struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;
    auto operator<=>(const Version&) const = default;
};

// "1.2.3" or "v1.2.3"; anything else (pre-release suffixes included) is rejected.
[[nodiscard]] std::optional<Version> parse_version(std::string_view text);
[[nodiscard]] std::string to_string(const Version& v);

struct Asset {
    std::string name;
    std::string url;
};

struct Release {
    std::string tag;
    Version version;
    std::vector<Asset> assets;
};

// A GitHub `releases/latest` JSON payload.
[[nodiscard]] std::expected<Release, std::string> parse_release(std::string_view json_text);
[[nodiscard]] const Asset* find_asset(const Release& release, std::string_view name);

enum class InstallMode {
    installer, // installed by the Inno Setup installer: update by running the new installer
    portable   // zip or `cmake --install`: swap grab.exe in place
};

// grab-<v>-windows-x64-setup.exe or grab-<v>-windows-x64.zip
[[nodiscard]] std::string asset_name(const Version& v, InstallMode mode);

// Lowercase hex digest for `file_name` from sha256sum-style text ("<hex>  <name>" or
// "<hex> *<name>", LF or CRLF).
[[nodiscard]] std::optional<std::string> find_sha256(std::string_view sums_text,
                                                     std::string_view file_name);

// Windows directory equality: case-insensitive, '/' == '\', repeated and trailing separators
// ignored (Inno stores InstallLocation with a trailing '\', so "<loc>\bin" doubles it).
[[nodiscard]] bool same_dir(std::string_view a, std::string_view b);

// `grab update [--check]`. Prints progress and returns the process exit code.
[[nodiscard]] int run(bool check_only);

} // namespace grab::update
