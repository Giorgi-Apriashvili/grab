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
// Scanned by ssh::scan_host_keys (grab's own client, for every server); grab's known_hosts holds
// the lines ssh::known_host_line writes, which OpenSSH, rclone and grab's client all read.

struct Fingerprint {
    std::string hash; // "SHA256:..."
    std::string type; // "ED25519", "ECDSA", "RSA", as ssh-keygen -l names them
    bool operator==(const Fingerprint&) const = default;
};
// ssh-keygen's name for a key type: "ssh-ed25519" -> "ED25519", "ecdsa-sha2-nistp521" ->
// "ECDSA", "ssh-rsa" -> "RSA".
[[nodiscard]] std::string key_label(std::string_view type);

// Whether `name`, one name of a known_hosts line's host list, stands for host:port as OpenSSH
// reads it: "[host]:port", for port 22 also the plain "host"; a hashed name ("|1|salt|hash")
// when it is the hash of one of those. Plain names match as written, as libssh2 and rclone
// compare them (a hashed one also lower-cased, as OpenSSH hashes it); patterns
// ("*.example.com", "!host") never match.
[[nodiscard]] bool names_host(std::string_view name, std::string_view host, int port);
// `text` (a known_hosts file) without what pins host:port (review G87): such names leave a
// line's host list ("a,[host]:23,b" keeps a and b), and a line left with none goes. Comments,
// marker lines (@revoked, @cert-authority) and other hosts' lines stay as they are.
[[nodiscard]] std::string forget_host(std::string_view text, std::string_view host, int port);
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
