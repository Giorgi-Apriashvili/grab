#pragma once

#include "ini.hpp"

#include <chrono>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grab {

// How TARGET is located on the server: grab.conf's `find = auto | ssh | walk` (`rclone`, the name
// grab 0.19 and before know, the same as walk), which GrabMethod resolves.
enum class FindMethod {
    auto_detect, // auto: find where grab's check finds GNU find on the server, else the walk
    ssh,         // find only
    rclone       // walk (or rclone): grab's SFTP walk, no shell needed
};

// grab.conf's `engine = grab | rclone`, which 0.20 read (rclone was its fallback until this grab
// removed it, RCLONE-REPLACEMENT.md phase 9): read only for a notice that `engine = rclone` is
// ignored now; every server goes through grab's own SSH/SFTP client.
enum class Engine { rclone, grab };

// How searches and file lists run: GNU find over an SSH command on a pooled connection, or grab's
// walk of the folders over SFTP.
enum class GrabMethod {
    probe, // find = auto: a check (its answer kept a week) picks find where the server has it
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
    std::optional<Engine> engine; // `engine = grab | rclone`, for a notice only
    // How every connection to the server is made (grab's own client and Trust's host-key fetch;
    // RCLONE-REPLACEMENT.md, phase 8): the local address to connect from, the time to reach the
    // server (0: off, as long as TCP tries) and any single wait after that (1 s to 24 h, never off:
    // a stalled read couldn't be cancelled). nullopt when not set: grab's 30 s.
    std::optional<std::string> bind;
    std::optional<std::chrono::milliseconds> connect_timeout;
    std::optional<std::chrono::milliseconds> io_timeout;
    // The keys grab 0.20 and before gave rclone and ssh.exe (phase 9): read only for the notices
    // that they are ignored now (notices.hpp), the guard (guard.hpp) and a bind left in them
    // (old_bind).
    std::vector<std::string> ssh_options;
    std::vector<std::string> common_flags;
    std::vector<std::string> folder_flags;
    std::vector<std::string> file_flags;
};

// A bind an old key still holds: common_flags' or folder_flags' --bind (the last one, as rclone
// read it), else ssh_options' -o BindAddress or -b (the first, as ssh.exe read it). A blank one is
// none. file_flags' reached only `--rclone -s`'s plain copy, gone since 0.20. With no `bind` key,
// grab doesn't connect to the server rather than connect from another address
// (engine::connect_settings).
struct OldBind {
    std::string key;   // "common_flags", "folder_flags", "ssh_options"
    std::string flag;  // "--bind", "-o BindAddress", "-b"
    std::string value; // "" when the flag has none
};
[[nodiscard]] std::optional<OldBind> old_bind(const RemoteSettings& settings);

struct GrabConfig {
    // [grab] rclone and ssh, the programs grab 0.20 and before ran: blank when not set; read only
    // for a notice that they are ignored now.
    std::string rclone;
    std::optional<std::filesystem::path> rclone_config; // only when set explicitly
    std::string ssh;
    std::optional<std::string> editor; // [grab] editor, used by `grab config`
    std::optional<std::string> default_remote;
    std::vector<RemoteSettings> remotes;
    // Keys of [grab] and the server sections that grab doesn't read ("[Main]", "connect_timout"),
    // in file order: ignored, never an error, and named once in a notice (notices.hpp, N7).
    std::vector<std::pair<std::string, std::string>> unknown_keys;

    [[nodiscard]] std::expected<const RemoteSettings*, std::string>
    select(const std::optional<std::string>& name) const;
};

// A server's connection details: from its grab.conf section (server_conf::to_remote), or an sftp
// remote of rclone.conf for `grab --init`. Renamed in phase 9.
struct RcloneRemote {
    std::string name;
    std::string host;
    std::string user;
    int port = 22;
    std::optional<std::string> key_file;
    std::optional<std::string> known_hosts_file; // rclone's "none" becomes nullopt
    bool key_use_agent = false;                   // authenticates through ssh-agent
    // How rclone runs commands there, as it saved what it found (read only; grab's verification
    // keeps to what it can do the same way, ssh::hash_blocked): the shell ("unix", "cmd",
    // "powershell", "none"), the MD5 command ("md5sum", "none"...), a path prefix for commands.
    std::optional<std::string> shell_type;
    std::optional<std::string> md5sum_command;
    std::optional<std::string> path_override;
};

// io_timeout's range, which grab's own client also holds `--timeout` to: a wait, never off.
inline constexpr std::chrono::milliseconds min_io_timeout{1'000};
inline constexpr std::chrono::milliseconds max_io_timeout{24 * 3'600'000};

// How searches and lists go: `find` as GrabMethod; auto asks the server (ssh::check_find), except a
// Storage Box (*.your-storagebox.de), whose restricted shell has no find, so it walks without asking.
[[nodiscard]] GrabMethod resolve_grab_method(const RemoteSettings& settings, const RcloneRemote& remote);

// "." and "~" mean the login home and become ""; "/" stays; other roots lose trailing '/'.
[[nodiscard]] std::string normalize_root(std::string_view root);

// Normalized roots as a grab.conf search_roots value: "~" for the login home, ", " between.
[[nodiscard]] std::string search_roots_value(const std::vector<std::string>& roots);

// The old flag keys' defaults, as grab 0.20 and before gave them to rclone when the key was absent:
// what the notices compare with (only what differs is named).
inline constexpr std::string_view default_common_flags =
    "-P --sftp-chunk-size 255Ki --sftp-disable-hashcheck";
inline constexpr std::string_view default_folder_flags = "--transfers 8 --checkers 8";
inline constexpr std::string_view default_file_flags =
    "--multi-thread-streams 8 --multi-thread-cutoff 64Mi --multi-thread-chunk-size 64Mi";

// Commented example with a placeholder server, as grab.conf.example has it.
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
// The rclone config where grab 0.19 and before kept its servers: <config>/grab/rclone.conf. Only
// the import (rclone_import.hpp) reads it now, and deletes it with the 'secrets' beside it once
// everything in them has moved (rclone_import::old_files).
[[nodiscard]] std::filesystem::path grab_rclone_config_path();
// Host keys pinned by `grab server add|trust`: <config>/grab/known_hosts.
[[nodiscard]] std::filesystem::path grab_known_hosts_path();
// The rclone.conf other tools use ($RCLONE_CONFIG, else <config>/rclone/rclone.conf); grab
// only reads it, to import remotes.
[[nodiscard]] std::filesystem::path standard_rclone_config_path();

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

[[nodiscard]] std::expected<GrabConfig, std::string> parse_grab_config(const ini::Document& doc);
[[nodiscard]] std::expected<GrabConfig, std::string> load_grab_config(const std::filesystem::path& p);

// An sftp remote of rclone.conf, for `grab --init`'s list of remotes (grab's servers are read
// from grab.conf; the import, rclone_import.hpp, reads rclone.conf as rclone does).
[[nodiscard]] std::expected<RcloneRemote, std::string> parse_rclone_remote(const ini::Document& doc,
                                                                            std::string_view name);

// ---- generating grab.conf from rclone.conf (grab --init) ------------------------------------

// True for a config encrypted with `rclone config` -> "Set configuration password".
[[nodiscard]] bool is_encrypted_rclone_config(std::string_view text);

// sftp remotes in rclone.conf that grab can use (host and user present), in file order.
[[nodiscard]] std::vector<RcloneRemote> usable_sftp_remotes(const ini::Document& rclone);

// Usable sftp remotes that no grab.conf section references through rclone_remote.
[[nodiscard]] std::vector<RcloneRemote> missing_remotes(const GrabConfig& cfg,
                                                        const ini::Document& rclone);

// How a server's searches and file lists go, as lists and texts name it, as resolve_grab_method
// says: "find" (find only), "walk" (by listing folders) or "probe" (find where grab's check finds
// GNU find, else by listing folders).
[[nodiscard]] std::string_view search_method(const RemoteSettings& settings, const RcloneRemote& remote);

// "alice@host:22, key file, lookup with find, else by listing folders": `method` is the lookup as
// search_method says it; without one, the default find setting's. `login`, when given, says how it
// logs in ("key in grab's store") instead of what `remote` says.
[[nodiscard]] std::string describe_remote(const RcloneRemote& remote, std::string_view method = {},
                                          std::string_view login = {});

// The grab.conf section of rclone remote `remote`; "grab" is taken by the global section.
[[nodiscard]] std::string section_name_for(std::string_view remote);

// grab's comment line right above a server's section: "# server [NAME]: DESCRIPTION". grab 0.19
// and before wrote "# rclone remote [NAME]: ...", which servers::remove_section and
// replace_lead_in still take for grab's.
[[nodiscard]] std::string lead_in(std::string_view name, std::string_view description);

// A commented grab.conf section for one rclone remote. search_roots is left blank (the login
// home, correct for both lookup methods); bind, connect_timeout and io_timeout are written
// commented out, so grab's defaults apply. With `connection` (server_conf::lines),
// those lines go after the header instead of `rclone_remote`, which a new server leaves to its
// name.
[[nodiscard]] std::string remote_section(const RcloneRemote& remote,
                                         const std::vector<std::string>& search_roots = {},
                                         int max_depth = 4,
                                         const std::vector<std::pair<std::string, std::string>>& connection = {});

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
