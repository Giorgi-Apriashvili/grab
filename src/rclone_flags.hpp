#pragma once

// Which of the user's rclone settings grab's own rclone calls use (review G8, G182, G184-G187,
// 2026-10-21). grab.conf's common_flags / file_flags / folder_flags and the command's `--` flags
// were meant for rclone copies; grab's internal calls (the size check, `cat` ranges, listings, the
// small-file batch; rclone serve for previews and Play, and verification's check and sums, until
// phases 6 and 5 moved them to grab's own client) depend on rclone's exact output, so a progress,
// logging, filter or comparison flag there broke downloads, previews and verification ("-vP":
// every download failed; "--exclude": `cat` refused; "-- --dry-run": big files were downloaded
// for real; "--size-only": a corrupt copy was "verified"). They take only flags about reaching the
// server; the rest went with --rclone, the plain rclone copy (phase 8), and is refused after `--`.

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

// The first flag of a command's `-- ...` that a server on engine = rclone doesn't use (its rclone
// runs take only connection flags), with a hint what to use instead ("--dry-run": grab's -n), ""
// when grab has nothing for it; nullopt when they are all connection flags.
struct Unusable {
    std::string flag;
    std::string hint;
};
[[nodiscard]] std::optional<Unusable> first_unusable(std::span<const std::string> extra);

// The same for grab's own client (engine = grab; RCLONE-REPLACEMENT.md phase 8 step 6), which
// takes only --bind, --contimeout and --timeout (each with its value) after `--`: the message for
// the first other flag (or a stray value), saying why and what to use instead, with `server`'s
// section for a setting ("--sftp-connections after -- isn't used by grab's own client; set
// max_connections in [Main] (grab config)"). A setting only rclone can follow (a proxy, another
// SFTP server, its algorithms, its quirk switches; guard.hpp) says to set engine = rclone.
// nullopt when it takes them all. first_refused's are for the caller to check first.
[[nodiscard]] std::optional<std::string> first_unused_by_grab(std::span<const std::string> extra,
                                                              std::string_view server);

// The first --sftp-* flag of `flags` that would replace what grab gives each of its rclone runs
// about the server (engine::rclone_env, RCLONE-REPLACEMENT.md phase 7): its address, port, user,
// password, key, key file, passphrase, certificate, ssh-agent use, trusted host keys or ssh
// command (--sftp-host, --sftp-port, --sftp-user, --sftp-pass, --sftp-ask-password,
// --sftp-key-pem, --sftp-key-file, --sftp-key-file-pass, --sftp-pubkey, --sftp-pubkey-file,
// --sftp-key-use-agent, --sftp-known-hosts-file, --sftp-ssh), with what it is in words
// ("address"). grab refuses them in common_flags and after `--`.
struct Refused {
    std::string flag; // its name, without a value
    std::string what;
    // Where grab keeps it instead: grab.conf's key ("host", "key_file", "auth",
    // "rclone.pubkey_file"), "" for a secret in grab's store, "trust" for grab's known_hosts,
    // "ssh" for nothing (a run takes no ssh command there).
    std::string in_grab;
};
[[nodiscard]] std::optional<Refused> first_refused(std::span<const std::string> flags);
// What to do instead, for the refusal's message: "change grab.conf's `host` with `grab server
// edit Main` or Edit in grab's Settings"; `server` is the server's name (or a placeholder).
[[nodiscard]] std::string change_instead(const Refused& refused, std::string_view server);

// Whether an inherited environment variable reaches grab's internal rclone calls: anything but
// RCLONE_*, and of those RCLONE_PASSWORD_COMMAND and the RCLONE_SFTP_* that aren't about the
// server first_refused guards (RCLONE_SFTP_HOST, say, would beat the server's own address). No
// RCLONE_CONFIG*: each run gets its server's RCLONE_CONFIG_<NAME>_* from grab alone, with its
// config in memory (`--config NUL`). RCLONE_PROGRESS=true, say, would put rclone's progress text
// into the bytes `rclone cat` hands grab (G8). Names ignore case, as Windows' do.
[[nodiscard]] bool kept_variable(std::string_view name);

} // namespace grab::rclone_flags
