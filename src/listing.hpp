#pragma once

#include "cli.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace grab {

// Looking a target up with `rclone lsf` instead of ssh + find. Works on hosts without a
// shell (storage boxes), needs no password or passphrase prompt, and costs an SFTP walk of
// the tree bounded by max_depth.
struct ListRequest {
    Mode mode = Mode::folder;
    std::string target;   // name pattern, or a remote path containing '/'
    std::string root;     // "" = the login home, "/abs" or "relative/to/home"
    int max_depth = 4;
    bool skip_hidden = true;
    std::string rclone_exe;
    std::optional<std::filesystem::path> rclone_config;
    std::string rclone_remote;
};

// fnmatch-style matching as `find -name` does it: * ? [set] [!set]; no escapes.
[[nodiscard]] bool glob_match(std::string_view pattern, std::string_view name);

// ("", "a/b") -> "a/b"   ("/", "a") -> "/a"   ("x/", "a") -> "x/a"
[[nodiscard]] std::string join_remote(std::string_view root, std::string_view rel);

// "learning/x" -> "learning"   "/x" -> "/"   "a/b/" -> "a"   "x" -> ""
[[nodiscard]] std::string parent_of(std::string_view path);

// Name search: `rclone lsf REMOTE:root -R --max-depth N --dirs-only|--files-only [--exclude ...]`.
// Path target:  `rclone lsf REMOTE:<parent> --dirs-only|--files-only` (one level).
[[nodiscard]] std::vector<std::string> build_lsf_argv(const ListRequest& req);

// Remote paths from lsf output (one entry per line, directories end with '/') whose last
// component matches the target; results are joined onto the root / parent.
[[nodiscard]] std::vector<std::string> parse_lsf_output(const ListRequest& req,
                                                        std::string_view out);

} // namespace grab
