#pragma once

// File names from the server as Windows file names. A Linux name can hold characters that mean
// something else on Windows: `..\x` climbs out of the download folder, `C:\x` and `\\host\share\x`
// are absolute, `a:b` writes an NTFS stream, `CON` is a device, a trailing dot or space is dropped.
// Every remote name becomes a local one here, the way rclone's local backend encodes it on
// Windows (its default --local-encoding), so grab's files match the ones rclone writes for a
// folder's small files byte for byte; device names get "_" (rclone writes them through \\?\ paths
// that few programs can open). Checked against rclone 1.75.1's real output on 2026-10-19.

#include <filesystem>
#include <string>
#include <string_view>

namespace grab::local {

// One name (no '/'), safe on Windows:
//   \ : * ? " < > |  ->  ＼ ： ＊ ？ ＂ ＜ ＞ ｜ (fullwidth); control characters -> ␀..␟;
//   the last character, when a space or a period -> ␠ or ．; "." and ".." -> ． and ．．;
//   characters that already are one of these replacements -> ‛ before them (‛ itself: ‛‛);
//   bytes that aren't UTF-8 -> ‛ and their hex; a device name (CON, PRN, AUX, NUL, COM0-9,
//   LPT0-9, before any extension) -> "_" after it: CON_, nul_.txt.
[[nodiscard]] std::string name(std::string_view remote_name);

// Whether a name (as name() makes it) is a Windows device name.
[[nodiscard]] bool is_device(std::string_view local_name);
// Whether any part of a relative remote path (a/b/c) is one: rclone can't write such a file
// properly, so grab fetches it itself.
[[nodiscard]] bool has_device(std::string_view remote_rel);

// A local path as Windows compares names (ignoring case), for sets of paths.
[[nodiscard]] std::filesystem::path::string_type key(const std::filesystem::path& p);

// A relative remote path (a/b/c) as a local one, each part through name(); empty parts dropped.
// Every part is a plain name (no separator, drive, "." or ".."), so `dest / relative(x)` stays
// below dest for any x.
[[nodiscard]] std::filesystem::path relative(std::string_view remote_rel);

} // namespace grab::local
