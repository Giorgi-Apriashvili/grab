#pragma once

// Which of the user's rclone settings grab's own rclone calls use (review G8, G182, G184-G187,
// 2026-10-21). grab.conf's common_flags / file_flags / folder_flags and the command's `--` flags
// are meant for rclone copies; grab's internal calls (the size check, `cat` ranges, listings, the
// small-file batch; rclone serve for previews and Play, and verification's check and sums, until
// phases 6 and 5 moved them to grab's own client) depend on rclone's exact output, so a progress,
// logging, filter or comparison flag there broke downloads, previews and verification ("-vP":
// every download failed; "--exclude": `cat` refused; "-- --dry-run": big files were downloaded
// for real; "--size-only": a corrupt copy was "verified"). They take only flags about reaching the
// server; everything else is for --rclone, the plain rclone copy.

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace grab::rclone_flags {

// Whether a flag (its name, "--sftp-chunk-size", "--bind=10.0.0.2" works too) is about reaching
// the server: --sftp-*, --bind, --contimeout, --timeout, --low-level-retries, --user-agent,
// --ca-cert, --client-cert, --client-key, --no-check-certificate, --tpslimit(-burst), --dscp,
// --password-command.
[[nodiscard]] bool is_connection(std::string_view flag);

// The connection flags of `flags`, each with the value that follows it, if any (a token not
// starting with '-' belongs to the flag before it).
[[nodiscard]] std::vector<std::string> connection(std::span<const std::string> flags);

// The first flag of a command's `-- ...` that grab's own engine doesn't use, with a hint what to
// use instead ("--dry-run": grab's -n); nullopt when they are all connection flags.
struct Unusable {
    std::string flag;
    std::string hint;
};
[[nodiscard]] std::optional<Unusable> first_unusable(std::span<const std::string> extra);

// The first --sftp-* flag of `flags` that would replace what grab gives each of its rclone runs
// about the server (engine::rclone_env, RCLONE-REPLACEMENT.md phase 7): its address, port, user,
// password, key, key file, passphrase, certificate, ssh-agent use, trusted host keys or ssh
// command (--sftp-host, --sftp-port, --sftp-user, --sftp-pass, --sftp-ask-password,
// --sftp-key-pem, --sftp-key-file, --sftp-key-file-pass, --sftp-pubkey, --sftp-pubkey-file,
// --sftp-key-use-agent, --sftp-known-hosts-file, --sftp-ssh), with what it is in words
// ("address"). grab's own runs refuse them in common_flags and after `--`; --rclone, the user's
// own copy, takes them.
struct Refused {
    std::string flag; // its name, without a value
    std::string what;
};
[[nodiscard]] std::optional<Refused> first_refused(std::span<const std::string> flags);

// Whether an inherited environment variable reaches grab's internal rclone calls: anything but
// RCLONE_*, and of those RCLONE_PASSWORD_COMMAND and the RCLONE_SFTP_* that aren't about the
// server first_refused guards (RCLONE_SFTP_HOST, say, would beat the server's own address). No
// RCLONE_CONFIG*: each run gets its server's RCLONE_CONFIG_<NAME>_* from grab alone, with its
// config in memory (`--config NUL`). RCLONE_PROGRESS=true, say, would put rclone's progress text
// into the bytes `rclone cat` hands grab (G8). Names ignore case, as Windows' do.
[[nodiscard]] bool kept_variable(std::string_view name);

} // namespace grab::rclone_flags
