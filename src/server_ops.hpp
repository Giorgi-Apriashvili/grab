#pragma once

// The work behind `grab server ...` and grab-gui's Settings, without any prompting: load the
// config files, scan and pin host keys, and add, edit, trust, remove or test a server. The
// CLI (server_cli.cpp) and the GUI (gui/app.cpp) ask their questions their own way and call
// these.

#include "config.hpp"
#include "ini.hpp"
#include "secrets.hpp"
#include "servers.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grab::server_ops {

// Where everything lives, resolved once per operation.
struct Env {
    std::filesystem::path grab_conf;
    std::filesystem::path rclone_conf;
    std::filesystem::path known_hosts;
    std::string rclone;    // the rclone executable to run
    GrabConfig cfg;
    std::string grab_text; // grab.conf as it is on disk (or a fresh [grab] block)
    std::vector<std::string> imported; // remotes this load imported from the standard rclone.conf
    std::string secrets_error;         // why the encrypted secret store could not be read, if so
};

// Reads grab.conf (a missing file is an empty config), running the one-time import of rclone
// remotes. `exe_dir` is where a bundled rclone.exe would be.
[[nodiscard]] std::expected<Env, std::string> load_env(const std::filesystem::path& grab_conf,
                                                       const std::filesystem::path& exe_dir);

// A server's secrets in the store (secrets.hpp) after choosing `auth`: a password server keeps
// only its password, a key-file server only its passphrase, an ssh-agent server none. A new
// `obscured` secret replaces the old one; none keeps the old one of the same kind.
void apply_auth(secrets::Store& store, const std::string& remote, servers::Auth auth,
                const std::optional<std::string>& obscured);

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
    std::string error;         // non-empty: the rclone remote is missing or unusable
    // From the rclone remote (when error is empty):
    std::string host;
    std::string user;
    int port = 22;
    servers::Auth auth = servers::Auth::password;
    std::string key_file;
    Engine engine = Engine::rclone; // effective_engine(): what downloads and searches it
    std::string search;             // search_method(): "ssh", "rclone"; on grab "find", "walk", "probe"
    bool host_key_pinned = false;
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

// The rclone.conf servers are added to, as a document (empty when there is none). One that
// exists but can't be read or parsed is an error, never "no remotes": a new remote's name is
// checked against it, and `rclone config create` would replace one this couldn't see (G217).
[[nodiscard]] std::expected<ini::Document, std::string> read_rclone_conf(const Env& env);
// Why `name` can't be a new server's (grab.conf has that server, or `rclone` that remote), or
// nullopt when it's free.
[[nodiscard]] std::optional<std::string> name_taken(const Env& env, const ini::Document& rclone,
                                                    const std::string& name);

// `rclone obscure -` with the secret on stdin, so it never appears on a command line.
[[nodiscard]] std::expected<std::string, std::string> obscure(const Env& env, const std::string& secret);

// ---- changes ------------------------------------------------------------------------------

// Each change holds the settings lock (config.hpp) and starts from grab.conf and rclone.conf as
// they are then, not as they were when `env` was loaded: another grab may have changed them
// in between.

// Pins `keys` (in place of any for the server's host and port), creates the rclone remote,
// appends the grab.conf section and makes it the default when there is none. `obscured_secret`: the password or key passphrase, obscured. If
// a later step fails, the remote and its secret are removed again, so the name stays free.
// Returns a note when the saved passwords had to be set aside (secrets::load_for_change), else "".
[[nodiscard]] std::expected<std::string, std::string> add_server(const Env& env, const servers::NewServer& s,
                                                          const std::optional<std::string>& obscured_secret,
                                                          const HostKeys& keys);

// The rclone.conf keys to change so that the remote `current` matches `wanted`, as key/value
// pairs for `rclone config update`. Only what differs is listed, so keys grab doesn't manage
// (e.g. an imported remote's shell_type) survive. Switching the login method blanks the old
// method's keys. `obscured_secret`: a new password or passphrase; nullopt keeps the stored one.
[[nodiscard]] std::vector<std::pair<std::string, std::string>>
rclone_edit_updates(const ini::Section& current, const servers::NewServer& wanted,
                    const std::optional<std::string>& obscured_secret);

// Whether an edit from `current` to `wanted` must fetch and confirm the host key again: the
// address changed, or no key is pinned yet.
[[nodiscard]] bool edit_needs_host_key(const ServerInfo& current, const servers::NewServer& wanted);
// Whether an edit to password login must be given the password: the server didn't use one, or
// its host or user changed, so the saved password never goes to another server or account
// without being typed (G40: a page that took over the window could otherwise send it anywhere).
[[nodiscard]] bool edit_needs_password(const ServerInfo& current, const servers::NewServer& wanted);

// Applies an edit of server `wanted.name` (the name itself cannot change): rclone keys via
// `rclone config update`, search_roots/max_depth and the lead-in comment in grab.conf.
// `keys`, when given, are pinned (in place of any for the new host and port) and grab's
// known_hosts becomes the remote's known_hosts_file. A new host or port leaves the old one's keys
// out of grab's known_hosts, unless a remote still needs them (another server's, or one whose
// known_hosts_file is grab's; review G87). A note as add_server's, and when the old keys couldn't
// be taken out.
[[nodiscard]] std::expected<std::string, std::string> update_server(const Env& env, const servers::NewServer& wanted,
                                                             const std::optional<std::string>& obscured_secret,
                                                             const std::optional<HostKeys>& keys);

// Pins `keys` for an existing server, in place of those it had (review G87), and points its
// rclone remote at grab's known_hosts.
[[nodiscard]] std::expected<void, std::string> trust_server(const Env& env, std::string_view name,
                                                            const HostKeys& keys);

// Removes the grab.conf section, unless rclone_config is set explicitly the rclone remote, and its
// host and port's keys from grab's known_hosts unless a remote still needs them: another server's,
// or one whose known_hosts_file is grab's, such as its own left in an explicit rclone_config
// (review G87). A default_remote pointing at it moves to the next server. Returns a note when its
// host keys, saved password or rclone remote could not be removed (the server is gone from grab
// either way).
[[nodiscard]] std::expected<std::string, std::string> remove_server(const Env& env, std::string_view name);

[[nodiscard]] std::expected<void, std::string> set_default(const Env& env, std::string_view name);

struct TestResult {
    bool rclone_ok = false;
    std::string rclone_error;
    std::optional<bool> ssh_ok; // only for engine = rclone servers searched with ssh + find
    std::string ssh_error;
    std::optional<bool> grab_ok; // only for engine = grab servers: grab's own client
    std::string grab_error;
    std::string grab_search; // grab_ok: how searches go there ("with find (GNU find)", "by listing folders (...)")
    [[nodiscard]] bool ok() const { return rclone_ok && ssh_ok.value_or(true) && grab_ok.value_or(true); }
};

// The paths searches and downloads use: an rclone listing (on engine = grab still the way to
// verify and preview), plus an ssh login (BatchMode) for engine = rclone servers searched with ssh
// + find. On engine = grab: a login through grab's own client, the home folder listed over SFTP,
// and the find check, which says how its searches go.
[[nodiscard]] std::expected<TestResult, std::string> test_connection(const Env& env, std::string_view name,
                                                                     std::stop_token stop = {});

} // namespace grab::server_ops
