#pragma once

// A grab server as grab's own SSH client logs in to it: the connection details of its grab.conf
// section, its secrets from grab's store (passwords.hpp), and how rclone's connection flags
// (--bind, --contimeout, --timeout) read for it (engine::connect_settings).

#include "passwords.hpp"
#include "server_conf.hpp"
#include "ssh.hpp"

#include <chrono>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace grab::ssh {

// The Target for server `name`, whose grab.conf section holds `c`, checked against `known_hosts`
// (grab's), logging in as `auth` says: ssh-agent, the key file with its saved passphrase, the
// key kept in the store (stored_key, as rclone reads key_pem: unquote_key_pem) with its saved
// passphrase, or the saved password; a password saved beside a key too, which rclone also tries.
// Only what `store` keeps for exactly c's user@host:port is used (passwords::value_for): a
// password saved for another login is refused, so a hand edit of grab.conf's host can't send it
// elsewhere. An error when its password or key isn't saved for that login, or the key isn't
// written as rclone reads it.
[[nodiscard]] std::expected<Target, std::string> target_for(std::string_view name, const server_conf::Connection& c,
                                                           const passwords::Store& store,
                                                           const std::filesystem::path& known_hosts);

// An rclone key_pem as rclone 1.75.1 reads it (backend/sftp: strconv.Unquote of the value in
// double quotes): the key's text on one line, its line breaks written \n, Go's escapes (\t, \\,
// \", \x41, \101, A...) taken; nullopt where rclone says "pem key not formatted properly"
// (a bare " or line break, an unknown escape).
[[nodiscard]] std::optional<std::string> unquote_key_pem(std::string_view value);

// An rclone time: Go's ("30s", "1m30s", "1.5h", "500ms"), rclone's days, weeks, months and years
// ("2d"), a bare number of seconds, or "off". 0 and "off" mean no limit, and so does a time over
// 2^31-1 ms (about 24.8 days), more than the 32-bit times of Winsock's select and libssh2 hold;
// nullopt for anything else.
[[nodiscard]] std::optional<std::chrono::milliseconds> parse_duration(std::string_view text);

// rclone's connection flags (rclone_flags::connection) as grab's client takes them (review G189):
// --bind the address to connect from, --contimeout the time to reach the server, --timeout any
// single wait after that, each as "--flag=VALUE" or "--flag VALUE" (the last one given counts).
// `unused` holds the rest as it came. An error when one of those three has no value or a time
// can't be read.
struct ConnectionFlags {
    std::optional<std::string> bind;
    std::optional<std::chrono::milliseconds> connect_timeout;
    std::optional<std::chrono::milliseconds> timeout;
    std::string timeout_text; // --timeout as given, for messages
    std::vector<std::string> unused;
};
[[nodiscard]] std::expected<ConnectionFlags, std::string> read_flags(std::span<const std::string> flags);
// read_flags' values set in `target`; the rest returned.
[[nodiscard]] std::expected<std::vector<std::string>, std::string> apply_flags(Target& target,
                                                                             std::span<const std::string> flags);

} // namespace grab::ssh
