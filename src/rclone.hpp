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
