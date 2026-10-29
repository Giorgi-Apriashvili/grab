#pragma once

// The work behind `grab server ...` and grab-gui's Settings, without any prompting: load the
// config files, scan and pin host keys, and add, edit, trust, remove or test a server. The
// CLI (server_cli.cpp) and the GUI (gui/app.cpp) ask their questions their own way and call
// these. Servers live in grab.conf (server_conf.hpp), their passwords in grab's store
// (passwords.hpp) and their host keys in grab's known_hosts; nothing here runs `rclone config`
// or writes an rclone.conf (RCLONE-REPLACEMENT.md, phase 7).

#include "config.hpp"
#include "servers.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace grab::server_ops {

// Where everything lives, resolved once per operation.
struct Env {
    std::filesystem::path grab_conf;
    std::filesystem::path known_hosts; // grab's
    std::filesystem::path passwords;   // grab's store of this configuration (passwords::path_in)
    std::string rclone;    // the rclone executable to run
    GrabConfig cfg;
    std::string grab_text; // grab.conf as it is on disk (or a fresh [grab] block)
    std::vector<std::string> imported;     // servers this load moved from rclone.conf into grab.conf
    std::vector<std::string> import_notes; // that import's notes ("[server] ..."), or why it stopped
};

// Reads grab.conf (a missing file is an empty config), after the one-time move of servers from
// rclone.conf into it (rclone_import::apply). `exe_dir` is where a bundled rclone.exe would be.
[[nodiscard]] std::expected<Env, std::string> load_env(const std::filesystem::path& grab_conf,
                                                       const std::filesystem::path& exe_dir);

// rclone's first stderr line without its log prefix: "2026/09/24 00:38:10 CRITICAL: Failed to
// create file system ..." -> "Failed to create file system ...".
[[nodiscard]] std::string clean_rclone_error(std::string_view err);

// ---- servers as shown in lists --------------------------------------------------------------

struct ServerInfo {
    std::string name;          // grab.conf section
    std::string rclone_remote;
    bool is_default = false;
    std::vector<std::string> search_roots; // as written in grab.conf ("~" for the login home)
    int max_depth = 4;
    std::optional<int> max_connections; // set in grab.conf; nullopt = automatic
    int default_connections = 12;       // the automatic value for this host
    std::string error;         // non-empty: the section has no connection details, or bad ones
    // From the section (when error is empty):
    std::string host;
    std::string user;
    int port = 22;
    servers::Auth auth = servers::Auth::password;
    std::string key_file;
    Engine engine = Engine::rclone; // effective_engine(): what downloads and searches it
    std::string search;             // search_method(): "ssh", "rclone"; on grab "find", "walk", "probe"
    bool host_key_pinned = false;   // grab's known_hosts has a key for [host]:port, read now (G209)
};

[[nodiscard]] bool is_default_server(const GrabConfig& cfg, std::string_view name);
[[nodiscard]] std::vector<ServerInfo> list_servers(const Env& env);
[[nodiscard]] const RemoteSettings* find_server(const GrabConfig& cfg, std::string_view name);

// ---- host keys ----------------------------------------------------------------------------

struct HostKeys {
    std::vector<std::string> lines;                  // known_hosts lines to pin
    std::vector<servers::Fingerprint> fingerprints;  // for the user to compare; may be empty
};

// The server's host keys and their fingerprints, read by grab's own client (ssh::scan_host_keys:
// handshakes with no login, nothing written) for every server, whichever engine it uses: no
// ssh-keyscan, ssh.exe or ssh-keygen. 10 s for each handshake.
[[nodiscard]] std::expected<HostKeys, std::string> scan_host_keys(const std::string& host, int port,
                                                                  std::stop_token stop = {});

// Pins `lines` for host:port in known_hosts, in place of every key it held for host:port before
// (servers::forget_host; review G87). Each line has to be one for host:port. Under the settings
// lock, written in one step; a known_hosts that exists but can't be read is an error, never
// replaced by the new lines alone (review G89).
[[nodiscard]] std::expected<void, std::string> pin_host_keys(const std::filesystem::path& known_hosts,
                                                             std::string_view host, int port,
                                                             const std::vector<std::string>& lines);
// Takes host:port's keys out of known_hosts (servers::forget_host), the same way. A missing
// known_hosts has none.
[[nodiscard]] std::expected<void, std::string> forget_host_keys(const std::filesystem::path& known_hosts,
                                                                std::string_view host, int port);

// Why `name` can't be a new server's (server_conf::check_new_name against grab.conf's sections:
// one there already, also one differing only in case), or nullopt when it's free. A name only
// grab's old rclone.conf still has is free.
[[nodiscard]] std::optional<std::string> name_taken(const Env& env, const std::string& name);

// Why the details of `s` can't be saved (server_conf's checks: host, user, port, the key file
// there, search folders; G5, G28), "field: why" for the first problem; nullopt when fine.
struct Problem {
    std::string field; // "host", "port", "user", "keyFile", "roots"
    std::string why;
};
[[nodiscard]] std::optional<Problem> check_server(const servers::NewServer& s);

// ---- changes ------------------------------------------------------------------------------

// Each change holds the settings lock (config.hpp) and starts from grab.conf as it is then, not
// as it was when `env` was loaded: another grab may have changed it in between. A secret passed
// here is plain: grab's store keeps it encrypted (passwords.hpp), bound to the server's
// user@host:port.

// Pins `keys` (in place of any for the server's host and port), saves the password (Auth::password)
// or the key's passphrase (Auth::key) in the store, appends the grab.conf section and makes it the
// default when there is none. If writing grab.conf fails, the store goes back to what it was, so
// the name stays free. Returns a note, or "".
[[nodiscard]] std::expected<std::string, std::string> add_server(const Env& env, const servers::NewServer& s,
                                                                 const std::optional<std::string>& secret,
                                                                 const HostKeys& keys);

// Whether an edit from `current` to `wanted` must fetch and confirm the host key again: the
// address changed, or no key is pinned yet.
[[nodiscard]] bool edit_needs_host_key(const ServerInfo& current, const servers::NewServer& wanted);
// Whether an edit to password login must be given the password: the server didn't use one, or
// its user, host or port changed, so the saved password never goes to another server or account
// without being typed (G40: a page that took over the window could otherwise send it anywhere;
// the store's binding enforces it below the window too).
[[nodiscard]] bool edit_needs_password(const ServerInfo& current, const servers::NewServer& wanted);

// Applies an edit of server `wanted.name` (the name itself cannot change): the section's keys
// with set_value, each only when it changes, search_roots, max_depth and the lead-in comment.
// The store changes only when the login method or the secret changes (G85), or the login moves
// to another user, host or port: the key's passphrase, an imported key and rclone extras move
// with it, a password has to be typed again (G40). A stored_key server keeps its key; no other
// can get one here. `keys`, when given, are pinned (in place of any for the new host and port).
// A new user, host or port drops what the old one told grab (shell_type and the sum commands,
// asked again by the next Test), the learned connection limit and timings (G122), and the old
// address's keys unless another server is there (review G87). Returns a note, or "".
[[nodiscard]] std::expected<std::string, std::string> update_server(const Env& env, const servers::NewServer& wanted,
                                                                    const std::optional<std::string>& secret,
                                                                    const std::optional<HostKeys>& keys);

// Pins `keys` for an existing server, in place of those it had (review G87).
[[nodiscard]] std::expected<void, std::string> trust_server(const Env& env, std::string_view name,
                                                            const HostKeys& keys);

// Removes the grab.conf section (a default_remote pointing at it moves to the next server), then
// its saved passwords, its host and port's keys unless another grab.conf section, or a remote of
// an explicit rclone_config whose known_hosts_file is grab's, is at that address (review G87;
// grab's old rclone.conf doesn't count), its file list, learned connection limit and timings
// (G122). Returns a note naming what couldn't be cleaned up (the server is gone either way).
[[nodiscard]] std::expected<std::string, std::string> remove_server(const Env& env, std::string_view name);

[[nodiscard]] std::expected<void, std::string> set_default(const Env& env, std::string_view name);

struct TestResult {
    // rclone's listing, only on engine = rclone servers (their downloads, searches and lists go
    // through rclone); engine = grab ones use rclone for nothing a test can show.
    std::optional<bool> rclone_ok;
    std::string rclone_error;
    std::optional<bool> ssh_ok; // only for engine = rclone servers searched with ssh + find
    std::string ssh_error;
    // grab's own client: on engine = grab for downloads, searches, previews, Play and verification,
    // on the others for previews, Play and verification, which every server runs through it.
    std::optional<bool> grab_ok;
    std::string grab_error;
    bool grab_engine = false; // engine = grab
    std::string grab_search;  // engine = grab, grab_ok: how searches go there ("with find (GNU find)", "by listing folders (...)")
    std::string checksums;    // grab_ok: "checksums: md5sum", or why verification can't run there
    // grab's own login counts on engine = grab only: on engine = rclone the downloads go through
    // rclone, so a failure there is a warning (no previews, Play or verification), not a failed test.
    [[nodiscard]] bool ok() const {
        return rclone_ok.value_or(true) && ssh_ok.value_or(true) && (!grab_engine || grab_ok.value_or(false));
    }
};

// The paths searches, downloads, previews, Play and verification use. An unpinned server
// (engine::check_pinned) fails each part at once, nothing dialled. On engine = rclone an rclone
// listing (`--config NUL`, the server in its environment), plus an ssh login (BatchMode) for
// servers searched with ssh + find. On every server a login through grab's own client and its
// home folder listed over SFTP, what rclone would ask the server (ssh::learn_commands: shell_type
// and the sum commands, saved in grab.conf when grab.conf doesn't have them yet), and its hash
// check, which says whether downloads can be verified there (ssh_hash.hpp); on engine = grab also
// the find check, which says how its searches go.
[[nodiscard]] std::expected<TestResult, std::string> test_connection(const Env& env, std::string_view name,
                                                                     std::stop_token stop = {});

} // namespace grab::server_ops
