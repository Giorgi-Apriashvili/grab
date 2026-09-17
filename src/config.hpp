#pragma once

#include "ini.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace grab {

// How TARGET is located on the server.
enum class FindMethod {
    auto_detect, // ssh when the rclone remote has a key_file, otherwise rclone
    ssh,         // ssh + find: one round trip, needs a shell and a key or password prompt
    rclone       // rclone lsf: SFTP walk, no shell and no prompt needed
};

// One [section] of grab.conf describing how to search and download from a server.
struct RemoteSettings {
    std::string name;                      // grab.conf section name, used with -r
    std::string rclone_remote;             // remote name in rclone.conf (default: name)
    std::vector<std::string> search_roots; // normalized; "" = login home; may be empty
    FindMethod find = FindMethod::auto_detect;
    int max_depth = 4;
    bool skip_hidden = true;
    std::vector<std::string> ssh_options;
    std::vector<std::string> common_flags;
    std::vector<std::string> folder_flags;
    std::vector<std::string> file_flags;
};

struct GrabConfig {
    std::string rclone = "rclone";
    std::optional<std::filesystem::path> rclone_config; // only when set explicitly
    std::string ssh = "ssh";
    std::optional<std::string> editor; // [grab] editor, used by `grab config`
    std::optional<std::string> default_remote;
    std::vector<RemoteSettings> remotes;

    [[nodiscard]] std::expected<const RemoteSettings*, std::string>
    select(const std::optional<std::string>& name) const;
};

// Connection details read from an sftp remote in rclone.conf.
struct RcloneRemote {
    std::string name;
    std::string host;
    std::string user;
    int port = 22;
    std::optional<std::string> key_file;
    std::optional<std::string> known_hosts_file; // rclone's "none" becomes nullopt
};

// Resolve auto_detect: ssh when the remote authenticates with a key file (a shell is very
// likely), otherwise rclone (password remotes and storage boxes usually have no shell).
[[nodiscard]] FindMethod resolve_find_method(const RemoteSettings& settings,
                                             const RcloneRemote& remote);

// "." and "~" mean the login home and become ""; "/" stays; other roots lose trailing '/'.
[[nodiscard]] std::string normalize_root(std::string_view root);

// Built-in flag defaults, used when the key is absent from grab.conf.
inline constexpr std::string_view default_common_flags =
    "-P --sftp-chunk-size 255Ki --sftp-disable-hashcheck";
inline constexpr std::string_view default_folder_flags = "--transfers 8 --checkers 8";
inline constexpr std::string_view default_file_flags =
    "--multi-thread-streams 8 --multi-thread-cutoff 64Mi --multi-thread-chunk-size 64Mi";

// Commented example written by `grab --init`.
extern const std::string_view example_config;

[[nodiscard]] std::filesystem::path default_grab_config_path();   // $GRAB_CONFIG or <config>/grab/grab.conf
[[nodiscard]] std::filesystem::path default_rclone_config_path(); // <config>/rclone/rclone.conf

[[nodiscard]] std::expected<GrabConfig, std::string> parse_grab_config(const ini::Document& doc);
[[nodiscard]] std::expected<GrabConfig, std::string> load_grab_config(const std::filesystem::path& p);

[[nodiscard]] std::expected<RcloneRemote, std::string> parse_rclone_remote(const ini::Document& doc,
                                                                            std::string_view name);
[[nodiscard]] std::expected<RcloneRemote, std::string>
load_rclone_remote(const std::filesystem::path& p, std::string_view name);

// Command that opens `file` for editing: the first non-blank candidate (in practice
// [grab] editor, $VISUAL, $EDITOR) split shell-style, else notepad on Windows and vi
// elsewhere, with `file` appended as the last argument.
[[nodiscard]] std::vector<std::string> editor_command(const std::vector<std::string>& candidates,
                                                      const std::filesystem::path& file);

} // namespace grab
