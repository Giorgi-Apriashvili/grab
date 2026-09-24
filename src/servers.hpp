#pragma once

// Building blocks for `grab server add | list | trust | remove` (and the GUI's settings):
// rclone command lines, host-key handling, and in-place edits of grab.conf text. Pure
// functions; the dialogue lives in main.cpp.

#include "config.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grab::servers {

enum class Auth {
    password, // rclone `pass`; searched with rclone lsf
    key_file, // rclone `key_file` (+ optional `key_file_pass`); searched with ssh + find
    agent     // rclone `key_use_agent`; searched with ssh + find
};

struct NewServer {
    std::string name;
    std::string host;
    int port = 22;
    std::string user;
    Auth auth = Auth::password;
    std::string key_file;                  // Auth::key_file
    std::vector<std::string> search_roots; // blank = login home
    int max_depth = 4;
    std::optional<int> max_connections; // grab-gui downloads; nullopt = automatic
};

// nullopt when usable as both a grab.conf section and an rclone remote name, else why not.
[[nodiscard]] std::optional<std::string> validate_name(std::string_view name);

// The RcloneRemote the new server will be, for describe_remote/remote_section.
[[nodiscard]] RcloneRemote to_remote(const NewServer& s, const std::filesystem::path& known_hosts);

// ---- host keys ---------------------------------------------------------------------------

[[nodiscard]] std::vector<std::string> keyscan_argv(const std::string& host, int port);

// Fallback when ssh-keyscan cannot negotiate (Windows OpenSSH 9.5's keyscan fails on servers
// that pick the sntrup761 key exchange): a real ssh handshake that records the server's key
// into `record_to` (StrictHostKeyChecking=accept-new) and offers no login method, so nothing
// secret is sent. The connection is expected to fail after the key is recorded.
// With `host_key_algorithm` set, only that key type is requested; running once per entry of
// handshake_key_algorithms() collects every key the server has, like ssh-keyscan would. That
// matters because rclone's SSH library prefers ECDSA/RSA where OpenSSH picks ED25519, and a
// key type missing from known_hosts is a "key mismatch" for it.
[[nodiscard]] std::vector<std::string> handshake_argv(const std::string& ssh, const std::string& host,
                                                      int port, const std::filesystem::path& record_to,
                                                      const std::string& host_key_algorithm = {});
[[nodiscard]] const std::vector<std::string>& handshake_key_algorithms();
// known_hosts lines from ssh-keyscan's stdout (comments and blanks dropped).
[[nodiscard]] std::vector<std::string> parse_keyscan(std::string_view out);

struct Fingerprint {
    std::string hash; // "SHA256:..."
    std::string type; // "ED25519", "RSA", ...
    bool operator==(const Fingerprint&) const = default;
};
// Lines of `ssh-keygen -lf -`: "256 SHA256:abc host (ED25519)".
[[nodiscard]] std::vector<Fingerprint> parse_fingerprints(std::string_view out);

// `existing` known_hosts text plus the lines it does not already contain.
[[nodiscard]] std::string merge_known_hosts(std::string_view existing, const std::vector<std::string>& lines);

// ---- rclone command lines ----------------------------------------------------------------

// `rclone obscure -`: reads the secret from stdin, prints the obscured form.
[[nodiscard]] std::vector<std::string> obscure_argv(const std::string& rclone);

// `rclone config create NAME sftp ... --no-obscure --non-interactive --config FILE`.
// `obscured_secret` is the password (Auth::password) or key passphrase (Auth::key_file),
// already obscured, so no plaintext secret is ever on a command line.
[[nodiscard]] std::vector<std::string> create_argv(const std::string& rclone,
                                                   const std::filesystem::path& config,
                                                   const NewServer& s,
                                                   const std::optional<std::string>& obscured_secret,
                                                   const std::filesystem::path& known_hosts);

[[nodiscard]] std::vector<std::string> update_argv(const std::string& rclone,
                                                   const std::filesystem::path& config,
                                                   const std::string& name, const std::string& key,
                                                   const std::string& value);
// Several keys in one run; an empty value blanks the key.
[[nodiscard]] std::vector<std::string> update_argv(const std::string& rclone,
                                                   const std::filesystem::path& config,
                                                   const std::string& name,
                                                   const std::vector<std::pair<std::string, std::string>>& values);

[[nodiscard]] std::vector<std::string> delete_argv(const std::string& rclone,
                                                   const std::filesystem::path& config,
                                                   const std::string& name);

// A quick connection test: list the login home's folders once, with short timeouts.
[[nodiscard]] std::vector<std::string> probe_argv(const std::string& rclone,
                                                  const std::filesystem::path& config,
                                                  const std::string& name);

// ---- grab.conf text edits (everything else, comments included, is kept) -------------------

// Removes [section] and its lines, plus grab's generated comment line right above it.
[[nodiscard]] std::string remove_section(std::string_view text, std::string_view section);
// Appends `section_text` after one blank line.
[[nodiscard]] std::string append_section(std::string_view text, std::string_view section_text);
// Sets key = value in [section], replacing an existing line or adding one after the header;
// adds the section at the top when missing.
[[nodiscard]] std::string set_value(std::string_view text, std::string_view section,
                                    std::string_view key, std::string_view value);
// Replaces grab's generated comment line right above [section] ("# rclone remote [...]: ...")
// with `comment`; text without that line is returned unchanged.
[[nodiscard]] std::string replace_lead_in(std::string_view text, std::string_view section,
                                          std::string_view comment);

} // namespace grab::servers
