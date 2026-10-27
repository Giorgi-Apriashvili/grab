#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <stop_token>
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
// %LOCALAPPDATA% (else config_home()) on Windows, $XDG_CACHE_HOME or ~/.cache elsewhere: file
// lists, the window's browser profile. Environment variables are UTF-8 here: a path built
// straight from one would read it in the ANSI code page (review G140).
[[nodiscard]] std::filesystem::path local_data_home();

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

// How md5_file reads a file.
struct FileHashOptions {
    std::stop_token stop;                        // looked at before each read: the hash fails with "stopped"
    std::function<void(std::uint64_t)> progress; // bytes hashed so far, after each read
};
// md5_file's at once in a program (a verification's files, rows verifying side by side); the
// others wait for a turn.
inline constexpr int local_hashes_at_once = 4;
// Lowercase hex MD5 of a file, as md5sum and `rclone hashsum md5` print it: OpenSSL's EVP in 1 MiB
// reads, 1.45x Windows CNG's speed on this PC (RCLONE-REPLACEMENT.md, phase 5).
[[nodiscard]] std::expected<std::string, std::string> md5_file(const std::filesystem::path& p,
                                                               const FileHashOptions& options = {});

// Replaces `p` with `text` in one step. The text goes to a new file of a random name beside it,
// which is flushed to the disk before it takes p's place, so `p` holds the old text or the new
// one, never a part of either, also after a full disk or a power cut, and two writers never mix
// (review G11). Replacing works while grab reads `p` elsewhere (read_file); while another program
// has it open, it is tried again for a moment. Creates the parent folder. On failure `p` is
// unchanged, the new file is gone, and the error says why.
[[nodiscard]] std::expected<void, std::string> write_file_atomic(const std::filesystem::path& p,
                                                                 std::string_view text);
// Writes `text` to a new file `p`; false when anything exists at `p` already (it is left alone)
// or the write fails (the new file is removed again).
bool write_new_file(const std::filesystem::path& p, std::string_view text);
// A downloaded file into place: `data` goes to a new file of a random name beside `p`, which gets
// the modification time `unix_ns` (nanoseconds since 1970, when given) and then takes p's place,
// replacing a file there. Not flushed to the disk first: a download lost to a power cut is fetched
// again, and flushing each of a folder's small files would slow it down. On failure `p` is
// unchanged and the new file is gone. Creates the parent folder.
[[nodiscard]] std::expected<void, std::string> place_file(const std::filesystem::path& p, std::string_view data,
                                                          std::optional<std::int64_t> unix_ns);

// Moves the file `p` aside as "<p>.<tag>-YYYYMMDD-HHMMSS" (local time), so what replaces it never
// overwrites it: a damaged file is kept for whoever wants to look at it. Returns the new path.
[[nodiscard]] std::expected<std::filesystem::path, std::string> move_aside(const std::filesystem::path& p,
                                                                          std::string_view tag);

// An exclusive lock on the file `p` (created if missing), held until the object is destroyed:
// one holder at a time across threads and processes. lock_file waits for it up to `wait`.
class FileLock {
public:
    FileLock() = default;
    FileLock(FileLock&& other) noexcept;
    FileLock& operator=(FileLock&& other) noexcept;
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    ~FileLock();

private:
    friend std::expected<FileLock, std::string> lock_file(const std::filesystem::path& p, std::chrono::milliseconds wait);
    void release();
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};
[[nodiscard]] std::expected<FileLock, std::string> lock_file(const std::filesystem::path& p,
                                                             std::chrono::milliseconds wait);

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

// The whole file. A read that fails part-way is an error, never a shorter text (review G216).
// The file can be replaced meanwhile (write_file_atomic).
[[nodiscard]] std::expected<std::string, std::string> read_file(const std::filesystem::path& p);
// The same, but nullopt when there is no such file: a missing file is not an unreadable one.
[[nodiscard]] std::expected<std::optional<std::string>, std::string> read_file_if_exists(const std::filesystem::path& p);

// The system's text for an error code (Win32 GetLastError, else errno), in UTF-8.
[[nodiscard]] std::string error_message(unsigned long code);

// Whether this process can use local paths past 260 characters: Windows' long paths are on
// (LongPathsEnabled) and the program says it can (longPathAware, src/grab.manifest). Elsewhere
// always.
[[nodiscard]] bool long_paths();
// A local file path too long to use here, with `extra` characters more for the longest name grab
// makes beside it (a partial download's ".2.grabpart.json"): the reason, else nothing (review G19).
[[nodiscard]] std::optional<std::string> too_long(const std::filesystem::path& p, std::size_t extra = 0);
[[nodiscard]] std::optional<std::string> too_long(const std::filesystem::path& p, std::size_t extra, bool long_paths_on);

[[nodiscard]] std::string join(const std::vector<std::string>& items, std::string_view sep);

// Human-readable byte count: "512 B", "1.5 KiB", "12.4 GiB".
[[nodiscard]] std::string format_size(std::uint64_t bytes);

// ASCII lower-casing, for case-insensitive keywords.
[[nodiscard]] std::string to_lower(std::string_view s);

// Standard base64 (RFC 4648, '+' and '/'), with '=' padding, as known_hosts holds keys.
[[nodiscard]] std::string base64_encode(std::string_view bytes);
// The bytes of standard base64, padded or not; nullopt for any other character.
[[nodiscard]] std::optional<std::string> base64_decode(std::string_view text);

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
