#pragma once

// A server's connection details as grab.conf holds them (RCLONE-REPLACEMENT.md, phase 7: grab.conf
// becomes the one place servers are defined), and the one set of checks that `grab server`, the
// window's Settings and the import of rclone.conf (rclone_import.hpp) all use (review G28, G5,
// G169, names that differ only in case). Pure functions.
//
//   host = box.example            a host name or IP address
//   port = 23                     blank = 22
//   user = alice
//   auth = agent | key | password | stored_key
//   key_file = C:\Users\a\.ssh\id_ed25519   the key for `key`; beside `agent`, which agent key
//   shell_type, md5sum_command, sha1sum_command, path_override   what rclone found on the server
//   rclone.<key> = value          a non-secret rclone sftp setting (rclone_key_allowed), only for
//                                 engine = rclone's rclone
//
// Passwords, passphrases and an imported inline key (stored_key) are in grab's store
// (passwords.hpp), never here: grab.conf is plain text that editors open. Host keys are in grab's
// known_hosts.

#include "config.hpp"
#include "ini.hpp"

#include <array>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grab::server_conf {

enum class Auth {
    agent,      // ssh-agent, only its key_file's key when one is given (as rclone's key_use_agent)
    key,        // key_file, with its passphrase from the store when it has one
    password,   // the store's password
    stored_key, // an inline key imported from rclone.conf (key_pem), in the store; rclone only
};
[[nodiscard]] std::string_view auth_name(Auth auth);
[[nodiscard]] std::optional<Auth> parse_auth(std::string_view text);

struct Connection {
    std::string host;
    int port = 22;
    std::string user;
    Auth auth = Auth::agent;
    std::optional<std::string> key_file;
    std::optional<std::string> shell_type;
    std::optional<std::string> md5sum_command;
    std::optional<std::string> sha1sum_command;
    std::optional<std::string> path_override;
    std::vector<std::pair<std::string, std::string>> rclone; // rclone.<key> lines: key (without "rclone."), value
    bool operator==(const Connection&) const = default;
};

// The keys above, in the order grab writes them.
inline constexpr std::array<std::string_view, 9> keys = {
    "host", "port", "user", "auth", "key_file", "shell_type", "md5sum_command", "sha1sum_command", "path_override"};

// What an rclone setting's key starts with in a grab.conf section.
inline constexpr std::string_view rclone_prefix = "rclone.";

// rclone sftp settings a grab.conf section may hold as `rclone.<key>`: known ones that hold no
// secret. Any other (ssh, set_env, socks_proxy, http_proxy, pubkey, and anything unknown, which
// can carry a password) only ever goes into the store.
[[nodiscard]] bool rclone_key_allowed(std::string_view key);

// A section's connection details, checked (check below). nullopt when it has no `host`: a section
// of grab 0.19 and before, whose server is still in rclone.conf until the import moves it.
[[nodiscard]] std::expected<std::optional<Connection>, std::string> parse(const ini::Section& section);

// The grab.conf lines of `c`, keys in the order above, then its rclone.<key> lines; blank ones
// left out.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> lines(const Connection& c);

// The RcloneRemote grab's code reads a server as (until Phase 9 renames it): grab's known_hosts
// for every server.
[[nodiscard]] RcloneRemote to_remote(std::string_view name, const Connection& c,
                                     const std::filesystem::path& known_hosts);

// ---- checks: nullopt when fine, else why not ------------------------------------------------

// A value that goes into grab.conf: no control characters, line breaks included (G28).
[[nodiscard]] std::optional<std::string> check_value(std::string_view value);
// A host name or IP address, not starting with '-' (G5: ssh would take it for an option).
[[nodiscard]] std::optional<std::string> check_host(std::string_view host);
// A user name, not starting with '-' (G5).
[[nodiscard]] std::optional<std::string> check_user(std::string_view user);
[[nodiscard]] std::optional<std::string> check_port(int port);
// A key file: `exists` says whether there is a file at that path (the caller looks).
[[nodiscard]] std::optional<std::string> check_key_file(std::string_view path, bool exists);
// One search folder: no ',' (search_roots is a list of them).
[[nodiscard]] std::optional<std::string> check_search_root(std::string_view root);
// The whole of `c` (all of the above but the key file's existence), "[name] host: ..." for each
// problem; empty when fine.
[[nodiscard]] std::vector<std::string> check(std::string_view name, const Connection& c);

// Whether two server names are one to Windows: equal ignoring case as rclone names their
// variables (Go's upper case) or as Windows compares file names (their file lists).
[[nodiscard]] bool same_ignoring_case(std::string_view a, std::string_view b);
// A new server's name: servers::validate_name's rules, at least 2 characters (G169: rclone takes
// one letter for a drive), and no other section's name ignoring case (`sections`: grab.conf's).
[[nodiscard]] std::optional<std::string> check_new_name(std::string_view name, const std::vector<std::string>& sections);
// What is wrong with a server's existing name, for a note (an imported name keeps working): one
// character, a leading '-', another section of the same name ignoring case.
[[nodiscard]] std::vector<std::string> name_problems(std::string_view name, const std::vector<std::string>& sections);

// Whether grab's known_hosts (its text) pins host:port: a line naming [host]:port (for port 22
// also the plain host, hashed names too; servers::names_host), not a comment or a marker line
// (G209: pinned is what the file holds, read when asked).
[[nodiscard]] bool pinned(std::string_view known_hosts, std::string_view host, int port);

} // namespace grab::server_conf
