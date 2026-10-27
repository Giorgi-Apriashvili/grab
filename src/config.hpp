#pragma once

#include "ini.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace grab {

// How TARGET is located on the server (`find = auto | ssh | rclone`, `walk` the same as rclone).
enum class FindMethod {
    auto_detect, // ssh when the rclone remote has a key_file, otherwise rclone
    ssh,         // ssh + find: one round trip, needs a shell and a key or password prompt
    rclone       // rclone lsf: SFTP walk, no shell and no prompt needed
};

// What moves a server's files: rclone, or grab's own SSH/SFTP client (the rclone replacement,
// taking over phase by phase while rclone stays the default).
enum class Engine { rclone, grab };

// How searches and file lists run on engine = grab: GNU find over an SSH command on a pooled
// connection, or grab's walk of the folders over SFTP.
enum class GrabMethod {
    probe, // find = auto: a check once per program run picks find where the server has it
    find,  // find = ssh: find only; an error naming the server's answer where it doesn't work
    walk,  // find = rclone or walk, and Storage Boxes on auto (no find in their shell)
};

// One [section] of grab.conf describing how to search and download from a server.
struct RemoteSettings {
    std::string name;                      // grab.conf section name, used with -r
    std::string rclone_remote;             // remote name in rclone.conf (default: name)
    std::vector<std::string> search_roots; // normalized; "" = login home; may be empty
    FindMethod find = FindMethod::auto_detect;
    int max_depth = 4;
    std::optional<int> max_connections; // grab-gui downloads; nullopt = automatic
    bool skip_hidden = true;
    Engine engine = Engine::rclone; // `engine = rclone | grab`
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

// The server's engine, unless GRAB_ENGINE (grab or rclone) overrides it for every server, as
// tests comparing both do.
[[nodiscard]] Engine effective_engine(const RemoteSettings& settings);

// Resolve auto_detect: ssh when the remote authenticates with a key file (a shell is very
// likely), otherwise rclone (password remotes and storage boxes usually have no shell).
[[nodiscard]] FindMethod resolve_find_method(const RemoteSettings& settings,
                                             const RcloneRemote& remote);
// The same for engine = grab: auto asks the server (ssh::check_find), except a Storage Box
// (*.your-storagebox.de), whose restricted shell has no find, so it walks without asking.
[[nodiscard]] GrabMethod resolve_grab_method(const RemoteSettings& settings, const RcloneRemote& remote);

// "." and "~" mean the login home and become ""; "/" stays; other roots lose trailing '/'.
[[nodiscard]] std::string normalize_root(std::string_view root);

// Normalized roots as a grab.conf search_roots value: "~" for the login home, ", " between.
[[nodiscard]] std::string search_roots_value(const std::vector<std::string>& roots);

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

// A short stable key for a configuration folder: 16 hex digits of the FNV-1a of its lower-cased
// path. grab-gui runs one window per key.
[[nodiscard]] std::string config_key(const std::filesystem::path& config_dir);
// The name of the folder that keeps what the configuration at `grab_conf` saves apart from the
// default configuration's (gui_state.hpp's state_dir, file_list.hpp's dir_for), so two never
// share a queue or delete each other's lists (review G35, G219): "<its folder's name>-<8 hex
// digits of config_key>". nullopt for the default configuration (<config>/grab), whose files
// stay where they always were.
[[nodiscard]] std::optional<std::string> config_folder_name(const std::filesystem::path& grab_conf);
// grab's own rclone config, holding the remotes grab manages: <config>/grab/rclone.conf.
[[nodiscard]] std::filesystem::path grab_rclone_config_path();
// Host keys pinned by `grab server add|trust`: <config>/grab/known_hosts.
[[nodiscard]] std::filesystem::path grab_known_hosts_path();
// The rclone.conf other tools use ($RCLONE_CONFIG, else <config>/rclone/rclone.conf); grab
// only reads it, to import remotes.
[[nodiscard]] std::filesystem::path standard_rclone_config_path();

// The rclone.conf grab passes to rclone: [grab] rclone_config if set, else grab's own file.
[[nodiscard]] std::filesystem::path effective_rclone_config(const GrabConfig& cfg);

// Held while grab changes grab.conf, an rclone.conf, the password store or known_hosts, so that
// grab.exe and grab-gui (and the window's threads) change them one at a time, each change
// starting from the files as they are then (review G175). A lock on <config>/grab/settings.lock;
// re-entrant within a thread. Released when the last one in the thread is destroyed.
class SettingsLock {
public:
    SettingsLock(SettingsLock&& other) noexcept;
    SettingsLock& operator=(SettingsLock&&) = delete;
    SettingsLock(const SettingsLock&) = delete;
    SettingsLock& operator=(const SettingsLock&) = delete;
    ~SettingsLock();

private:
    SettingsLock() = default;
    friend std::expected<SettingsLock, std::string> lock_settings();
    bool held_ = false;
};
// Waits for the lock up to 30 seconds (another grab's change).
[[nodiscard]] std::expected<SettingsLock, std::string> lock_settings();

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

// How a server's searches and file lists go, as lists and texts name it. engine = rclone, as
// resolve_find_method says: "ssh" (ssh + find) or "rclone" (rclone lsf). engine = grab, as
// resolve_grab_method says: "find" (find only), "walk" (by listing folders) or "probe" (find where
// grab's check finds GNU find, else by listing folders).
[[nodiscard]] std::string_view search_method(const RemoteSettings& settings, const RcloneRemote& remote,
                                             Engine engine);

// "alice@host:22, key file, lookup via ssh + find": the lookup as `settings` (its engine as
// grab.conf says it, and its find setting) has it.
[[nodiscard]] std::string describe_remote(const RcloneRemote& remote, const RemoteSettings& settings = {});

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
