#pragma once

// A grab server as grab's own SSH client logs in to it: the connection details of its rclone.conf
// section and its secrets from grab's store (rclone-obscured there, made plain here, in memory).

#include "config.hpp"
#include "secrets.hpp"
#include "ssh.hpp"

#include <expected>
#include <filesystem>
#include <string>

namespace grab::ssh {

// The Target for `remote`: its known_hosts_file (else `default_known_hosts`, grab's own), and
// how it logs in, as rclone would: ssh-agent (key_use_agent), else its key_file (with
// key_file_pass), else its password (pass). An error when it has no way to log in grab's client
// knows yet (an inline key_pem) or its secrets can't be read.
[[nodiscard]] std::expected<Target, std::string> target_for(const RcloneRemote& remote, const secrets::Store& store,
                                                           const std::filesystem::path& default_known_hosts);

} // namespace grab::ssh
