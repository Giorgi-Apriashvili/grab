#pragma once

// A grab server as grab's own SSH client logs in to it: the connection details of its rclone.conf
// section and its secrets from grab's store (rclone-obscured there, made plain here, in memory),
// and the connection flags of its common_flags.

#include "config.hpp"
#include "secrets.hpp"
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

// The Target for `remote`: its known_hosts_file (else `default_known_hosts`, grab's own), and
// how it logs in, as rclone would: ssh-agent (key_use_agent), else its key_file (with
// key_file_pass), else its password (pass). An error when it has no way to log in grab's client
// knows yet (an inline key_pem) or its secrets can't be read.
[[nodiscard]] std::expected<Target, std::string> target_for(const RcloneRemote& remote, const secrets::Store& store,
                                                           const std::filesystem::path& default_known_hosts);

// An rclone time: Go's ("30s", "1m30s", "1.5h", "500ms"), rclone's days, weeks, months and years
// ("2d"), a bare number of seconds, or "off". 0 and "off" mean no limit, and so does a time over
// 2^31-1 ms (about 24.8 days), more than the 32-bit times of Winsock's select and libssh2 hold;
// nullopt for anything else.
[[nodiscard]] std::optional<std::chrono::milliseconds> parse_duration(std::string_view text);

// rclone's connection flags (rclone_flags::connection) as grab's client takes them (review G189):
// --bind the address to connect from, --contimeout the time to reach the server, --timeout any
// single wait after that, each as "--flag=VALUE" or "--flag VALUE". The rest is returned as it
// came, for a -v line. An error when one of those three has no value or a time can't be read.
[[nodiscard]] std::expected<std::vector<std::string>, std::string> apply_flags(Target& target,
                                                                             std::span<const std::string> flags);

} // namespace grab::ssh
