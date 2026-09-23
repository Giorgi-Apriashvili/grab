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

// Absolute path of the running executable, or empty when it cannot be determined.
[[nodiscard]] std::filesystem::path self_exe_path();

// Lowercase hex SHA-256 of a file (Windows CNG; not available on other platforms).
[[nodiscard]] std::expected<std::string, std::string> sha256_file(const std::filesystem::path& p);

[[nodiscard]] std::expected<std::string, std::string> read_file(const std::filesystem::path& p);

[[nodiscard]] std::string join(const std::vector<std::string>& items, std::string_view sep);

// Human-readable byte count: "512 B", "1.5 KiB", "12.4 GiB".
[[nodiscard]] std::string format_size(std::uint64_t bytes);

// ASCII lower-casing, for case-insensitive keywords.
[[nodiscard]] std::string to_lower(std::string_view s);

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
