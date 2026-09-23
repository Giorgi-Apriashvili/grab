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
    bool key_use_agent = false;                   // authenticates through ssh-agent
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

// Commented example with a placeholder server; `grab --init` falls back to it when rclone.conf
// is missing, encrypted or has no usable sftp remote.
extern const std::string_view example_config;

[[nodiscard]] std::filesystem::path default_grab_config_path(); // $GRAB_CONFIG or <config>/grab/grab.conf
// grab's own rclone config, holding the remotes grab manages: <config>/grab/rclone.conf.
[[nodiscard]] std::filesystem::path grab_rclone_config_path();
// Host keys pinned by `grab server add|trust`: <config>/grab/known_hosts.
[[nodiscard]] std::filesystem::path grab_known_hosts_path();
// The rclone.conf other tools use ($RCLONE_CONFIG, else <config>/rclone/rclone.conf); grab
// only reads it, to import remotes.
[[nodiscard]] std::filesystem::path standard_rclone_config_path();

// The rclone.conf grab passes to rclone: [grab] rclone_config if set, else grab's own file.
[[nodiscard]] std::filesystem::path effective_rclone_config(const GrabConfig& cfg);

// The rclone executable to run. [grab] rclone blank or the bare name "rclone": the bundled
// rclone.exe in `exe_dir` (the folder of the running grab) when present, else "rclone" from
// PATH. Anything else is used as given.
[[nodiscard]] std::string resolve_rclone_exe(const GrabConfig& cfg, const std::filesystem::path& exe_dir);

// ---- importing remotes into grab's own rclone.conf ----------------------------------------

// A section as rclone.conf text: "[name]\nkey = value\n...". Values are copied verbatim, so
// obscured passwords stay obscured.
[[nodiscard]] std::string section_text(const ini::Section& section);

struct ImportResult {
    std::vector<std::string> imported;  // copied into the target file
    std::vector<std::string> not_found; // referenced but in neither file
    std::string error;                  // non-empty when a file could not be read or written
};

// Copies each remote in `names` that `to` lacks and `from` has, from `from` into `to`
// (created if missing). `from` is never modified.
ImportResult import_rclone_remotes(const std::vector<std::string>& names,
                                   const std::filesystem::path& from, const std::filesystem::path& to);

// The one-time move to grab's own rclone.conf: when grab.conf has no explicit rclone_config,
// imports the remotes it references from the standard rclone.conf. Idempotent.
ImportResult migrate_rclone_remotes(const GrabConfig& cfg);

[[nodiscard]] std::expected<GrabConfig, std::string> parse_grab_config(const ini::Document& doc);
[[nodiscard]] std::expected<GrabConfig, std::string> load_grab_config(const std::filesystem::path& p);

[[nodiscard]] std::expected<RcloneRemote, std::string> parse_rclone_remote(const ini::Document& doc,
                                                                            std::string_view name);
[[nodiscard]] std::expected<RcloneRemote, std::string>
load_rclone_remote(const std::filesystem::path& p, std::string_view name);

// ---- generating grab.conf from rclone.conf (grab --init) ------------------------------------

// True for a config encrypted with `rclone config` -> "Set configuration password".
[[nodiscard]] bool is_encrypted_rclone_config(std::string_view text);

// sftp remotes in rclone.conf that grab can use (host and user present), in file order.
[[nodiscard]] std::vector<RcloneRemote> usable_sftp_remotes(const ini::Document& rclone);

// Usable sftp remotes that no grab.conf section references through rclone_remote.
[[nodiscard]] std::vector<RcloneRemote> missing_remotes(const GrabConfig& cfg,
                                                        const ini::Document& rclone);

// "alice@host:22, key file, lookup via ssh + find"
[[nodiscard]] std::string describe_remote(const RcloneRemote& remote);

// A commented grab.conf section for one rclone remote. search_roots is left blank (the login
// home, correct for both lookup methods) and the rclone flag keys are written commented out,
// so later changes to the built-in defaults still apply.
[[nodiscard]] std::string remote_section(const RcloneRemote& remote,
                                         const std::vector<std::string>& search_roots = {},
                                         int max_depth = 4);

// A complete grab.conf: the [grab] block plus one section per usable sftp remote, with
// default_remote set to the first. With a null `rclone` or no usable sftp remote it is the
// [grab] block alone, ready for `grab server add`.
[[nodiscard]] std::string generate_grab_config(const ini::Document* rclone,
                                               std::string_view rclone_path);

// Command that opens `file` for editing: the first non-blank candidate (in practice
// [grab] editor, $VISUAL, $EDITOR) split shell-style, else notepad on Windows and vi
// elsewhere, with `file` appended as the last argument.
[[nodiscard]] std::vector<std::string> editor_command(const std::vector<std::string>& candidates,
                                                      const std::filesystem::path& file);

} // namespace grab
