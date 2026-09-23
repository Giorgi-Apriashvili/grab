#pragma once

// The work behind `grab server ...` and grab-gui's Settings, without any prompting: load the
// config files, scan and pin host keys, and add, edit, trust, remove or test a server. The
// CLI (server_cli.cpp) and the GUI (gui/app.cpp) ask their questions their own way and call
// these.

#include "config.hpp"
#include "ini.hpp"
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
};

// Reads grab.conf (a missing file is an empty config), running the one-time import of rclone
// remotes. `exe_dir` is where a bundled rclone.exe would be.
[[nodiscard]] std::expected<Env, std::string> load_env(const std::filesystem::path& grab_conf,
                                                       const std::filesystem::path& exe_dir);

// Writes through a temporary file and a rename, so a reader never sees half a file.
[[nodiscard]] bool write_text(const std::filesystem::path& p, const std::string& text);

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
    std::string error;         // non-empty: the rclone remote is missing or unusable
    // From the rclone remote (when error is empty):
    std::string host;
    std::string user;
    int port = 22;
    servers::Auth auth = servers::Auth::password;
    std::string key_file;
    bool ssh_search = false;   // searched with ssh + find (else rclone lsf)
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

// ssh-keyscan, falling back to one ssh handshake per key type when keyscan cannot negotiate
// (see servers::handshake_argv), then `ssh-keygen -lf -` for the fingerprints.
[[nodiscard]] std::expected<HostKeys, std::string> scan_host_keys(const std::string& ssh,
                                                                  const std::string& host, int port,
                                                                  std::stop_token stop = {});

[[nodiscard]] std::expected<void, std::string> pin_host_keys(const std::filesystem::path& known_hosts,
                                                             const std::vector<std::string>& lines);

// `rclone obscure -` with the secret on stdin, so it never appears on a command line.
[[nodiscard]] std::expected<std::string, std::string> obscure(const Env& env, const std::string& secret);

// ---- changes ------------------------------------------------------------------------------

// Pins `keys`, creates the rclone remote, appends the grab.conf section and makes it the
// default when there is none. `obscured_secret`: the password or key passphrase, obscured.
[[nodiscard]] std::expected<void, std::string> add_server(const Env& env, const servers::NewServer& s,
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

// Applies an edit of server `wanted.name` (the name itself cannot change): rclone keys via
// `rclone config update`, search_roots/max_depth and the lead-in comment in grab.conf.
// `keys`, when given, are pinned and become the remote's known_hosts_file.
[[nodiscard]] std::expected<void, std::string> update_server(const Env& env, const servers::NewServer& wanted,
                                                             const std::optional<std::string>& obscured_secret,
                                                             const std::optional<HostKeys>& keys);

// Pins `keys` for an existing server and points its rclone remote at grab's known_hosts.
[[nodiscard]] std::expected<void, std::string> trust_server(const Env& env, std::string_view name,
                                                            const HostKeys& keys);

// Removes the grab.conf section and, unless rclone_config is set explicitly, the rclone
// remote. A default_remote pointing at it moves to the next server. Returns a note when the
// rclone side could not be removed (the server is gone from grab either way).
[[nodiscard]] std::expected<std::string, std::string> remove_server(const Env& env, std::string_view name);

[[nodiscard]] std::expected<void, std::string> set_default(const Env& env, std::string_view name);

struct TestResult {
    bool rclone_ok = false;
    std::string rclone_error;
    std::optional<bool> ssh_ok; // only for servers searched with ssh + find
    std::string ssh_error;
    [[nodiscard]] bool ok() const { return rclone_ok && ssh_ok.value_or(true); }
};

// The paths searches and downloads use: an rclone listing, plus an ssh login (BatchMode) for
// servers searched with ssh + find.
[[nodiscard]] std::expected<TestResult, std::string> test_connection(const Env& env, std::string_view name,
                                                                     std::stop_token stop = {});

} // namespace grab::server_ops
