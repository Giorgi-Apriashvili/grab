#pragma once

#include "cli.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace grab {

struct RcloneRequest {
    Mode mode = Mode::folder;
    std::string rclone_exe;
    std::optional<std::filesystem::path> rclone_config;
    std::string rclone_remote;          // remote name in rclone.conf
    std::string remote_path;            // absolute path on the server
    std::filesystem::path dest_dir;     // absolute local directory
    std::span<const std::string> common_flags;
    std::span<const std::string> mode_flags;
    std::span<const std::string> extra;
    std::optional<std::filesystem::path> local_name; // a file's name instead of the server's (Keep both)
};

// Last path component, ignoring trailing slashes.
[[nodiscard]] std::string remote_basename(std::string_view remote_path);
// Whether a remote path ends in a name to save it under: not "/", "." or ".." (whose local
// target would be the download folder itself, review G15).
[[nodiscard]] bool has_name(std::string_view remote_path);

// Whether a server path came without its real bytes: not UTF-8 (ssh + find prints them as they
// are), or with U+FFFD where rclone's JSON put it for bytes that weren't. rclone can't be given
// such a name on its command line or in a list, so it can't fetch it (review G17).
[[nodiscard]] bool name_not_utf8(std::string_view remote_path);
inline constexpr std::string_view not_utf8_error =
    "its name on the server isn't UTF-8 (shown with �), which rclone can't be given; rename it there to fetch it";

// "remote:/abs/path"
[[nodiscard]] std::string remote_spec(std::string_view rclone_remote, std::string_view remote_path);

// Where the transfer lands locally: DEST\<basename of remote_path>, in both modes.
[[nodiscard]] std::filesystem::path local_target(const RcloneRequest& req);

// Folder: rclone copy   remote:/path DEST\<name>   (the folder is recreated inside DEST)
// File:   rclone copyto remote:/path DEST\<name>
// The local side as rclone must be told it to use exactly local_target() (rclone_encoding.hpp);
// fails for a local name rclone can't be made to use.
[[nodiscard]] std::expected<std::vector<std::string>, std::string> build_rclone_argv(const RcloneRequest& req);

} // namespace grab
