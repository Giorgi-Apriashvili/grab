#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace grab::util {

// Strip leading/trailing whitespace (including CR from CRLF files).
[[nodiscard]] std::string_view trim(std::string_view s);

// Split on `sep`, trimming each item and dropping empty ones.
[[nodiscard]] std::vector<std::string> split(std::string_view s, char sep);

// Split a flag string into arguments the way a shell would, honouring "..." and '...'.
[[nodiscard]] std::vector<std::string> split_args(std::string_view s);

// Environment variable as UTF-8, or nullopt when unset.
[[nodiscard]] std::optional<std::string> getenv_utf8(const char* name);

// %APPDATA% on Windows, $XDG_CONFIG_HOME or ~/.config elsewhere.
[[nodiscard]] std::filesystem::path config_home();

[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& p);
[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view s);

[[nodiscard]] bool stdin_is_tty();

// One line from stdin with console echo off (passwords); nullopt at end of input. Without a
// console it reads normally.
[[nodiscard]] std::optional<std::string> read_secret_line();

// Absolute path of the running executable, or empty when it cannot be determined.
[[nodiscard]] std::filesystem::path self_exe_path();

// Lowercase hex SHA-256 of a file (Windows CNG; not available on other platforms).
[[nodiscard]] std::expected<std::string, std::string> sha256_file(const std::filesystem::path& p);

// Writes `text` to `p` through a sibling file and a rename, so a crash never leaves it half
// written. Creates the parent folder.
bool write_file_atomic(const std::filesystem::path& p, std::string_view text);
// Writes `text` to a new file `p`; false when anything exists at `p` already (it is left alone)
// or the write fails (the new file is removed again).
bool write_new_file(const std::filesystem::path& p, std::string_view text);

// One field of a tab-separated line: backslashes, tabs and line breaks escaped (\\ \t \n \r),
// and back; nullopt for a stray or unknown escape.
[[nodiscard]] std::string escape_field(std::string_view s);
[[nodiscard]] std::optional<std::string> unescape_field(std::string_view s);

// Encrypts `data` for the current Windows user with DPAPI: only this user, on this PC, can
// decrypt it, without being asked for anything. `purpose` is mixed in and must match when
// decrypting. nullopt when it fails. Without DPAPI (other platforms) `data` comes back as is.
[[nodiscard]] std::optional<std::string> protect_for_user(std::string_view data, std::string_view purpose);
// The reverse; nullopt when `data` was not protected by this user for `purpose`, or is damaged.
[[nodiscard]] std::optional<std::string> unprotect_for_user(std::string_view data, std::string_view purpose);

[[nodiscard]] std::expected<std::string, std::string> read_file(const std::filesystem::path& p);

[[nodiscard]] std::string join(const std::vector<std::string>& items, std::string_view sep);

// Human-readable byte count: "512 B", "1.5 KiB", "12.4 GiB".
[[nodiscard]] std::string format_size(std::uint64_t bytes);

// ASCII lower-casing, for case-insensitive keywords.
[[nodiscard]] std::string to_lower(std::string_view s);

// Unix seconds as find's %T@ or rclone's --time-format unix print them: an optional '-', digits
// and an optional fraction ("1697354553.1234567000"), which is dropped. nullopt for anything else.
[[nodiscard]] std::optional<std::int64_t> parse_unix_time(std::string_view text);
// A listed modification time: one of 0 or less is unknown.
[[nodiscard]] inline std::optional<std::int64_t> known_time(std::optional<std::int64_t> t) {
    return t && *t > 0 ? t : std::nullopt;
}
// RFC 3339 time ("2023-10-15T11:22:33.123456789+02:00", "...Z") as nanoseconds since 1970 UTC;
// nullopt outside the years 1678-2261, which 64 bits of nanoseconds cannot hold.
[[nodiscard]] std::optional<std::int64_t> parse_rfc3339(std::string_view text);
// The same in whole seconds (the fraction dropped), for any year.
[[nodiscard]] std::optional<std::int64_t> parse_rfc3339_seconds(std::string_view text);
// "2023-10-15" in local time; "" when the time cannot be shown.
[[nodiscard]] std::string local_date(std::int64_t unix_seconds);

#ifdef _WIN32
[[nodiscard]] std::wstring to_wide(std::string_view utf8);
[[nodiscard]] std::string to_utf8(std::wstring_view wide);
#endif

[[nodiscard]] inline std::unexpected<std::string> fail(std::string msg) {
    return std::unexpected(std::move(msg));
}

template <class... Args>
[[nodiscard]] std::unexpected<std::string> failf(std::format_string<Args...> fmt, Args&&... args) {
    return std::unexpected(std::format(fmt, std::forward<Args>(args)...));
}

} // namespace grab::util
