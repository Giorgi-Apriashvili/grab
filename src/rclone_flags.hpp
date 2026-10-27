#pragma once

// Which of the user's rclone settings grab's own rclone calls use (review G8, G182, G184-G187,
// 2026-10-21). grab.conf's common_flags / file_flags / folder_flags and the command's `--` flags
// are meant for rclone copies; grab's internal calls (the size check, `cat` ranges, listings, the
// small-file batch, rclone serve; verification's check and sums until phase 5 moved them to grab's
// own client) depend on rclone's exact output, so a progress, logging, filter or comparison flag
// there broke downloads, previews and verification ("-vP": every download failed; "--exclude":
// `cat` refused; "-- --dry-run": big files were downloaded for real; "--size-only": a corrupt copy
// was "verified"). They take only flags about reaching the server; everything else is for
// --rclone, the plain rclone copy.

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

// Whether an inherited environment variable reaches grab's internal rclone calls: anything but
// RCLONE_*, and of those RCLONE_CONFIG* (rclone.conf's path and password, the passwords grab hands
// over), RCLONE_PASSWORD_COMMAND and RCLONE_SFTP_*. RCLONE_PROGRESS=true, say, would put
// rclone's progress text into the bytes `rclone cat` hands grab (G8). Names ignore case, as
// Windows' do.
[[nodiscard]] bool kept_variable(std::string_view name);

} // namespace grab::rclone_flags
