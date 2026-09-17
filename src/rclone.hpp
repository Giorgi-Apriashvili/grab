#pragma once

#include "cli.hpp"

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
};

// Last path component, ignoring trailing slashes.
[[nodiscard]] std::string remote_basename(std::string_view remote_path);

// "remote:/abs/path"
[[nodiscard]] std::string remote_spec(std::string_view rclone_remote, std::string_view remote_path);

// Where the transfer lands locally: DEST\<basename of remote_path>, in both modes.
[[nodiscard]] std::filesystem::path local_target(const RcloneRequest& req);

// Folder: rclone copy   remote:/path DEST\<name>   (the folder is recreated inside DEST)
// File:   rclone copyto remote:/path DEST\<name>
[[nodiscard]] std::vector<std::string> build_rclone_argv(const RcloneRequest& req);

} // namespace grab
