#pragma once

// File names from the server as Windows file names. A Linux name can hold characters that mean
// something else on Windows: `..\x` climbs out of the download folder, `C:\x` and `\\host\share\x`
// are absolute, `a:b` writes an NTFS stream, `CON` is a device, a trailing dot or space is dropped.
// Every remote name becomes a local one here, the way rclone writes it on Windows (the Windows
// encoding of the name's Standard decoding, rclone_encoding.hpp), so grab's files match the ones
// rclone writes for a folder's small files byte for byte. Checked against rclone 1.75.1's real
// output over SFTP (review G24, 2026-10-22: "AC／DC", "del␡", "．"). Except, where grab names a
// file itself and fetches it with its own engine (differs_from_rclone):
//   - device names get "_" (rclone writes them through \\?\ paths few programs can open);
//   - names shaped like 8.3 short names get "_": on a volume that makes short names, "REPORT~1.DOC"
//     is also the name of an existing "Report for the board.docx" (review G167);
//   - bidi controls are dropped: "invoice<U+202E>fdp.exe" shows as "invoiceexe.pdf" (review G51).

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>

namespace grab::local {

// One name (no '/'), safe on Windows:
//   \ : * ? " < > |  ->  ＼ ： ＊ ？ ＂ ＜ ＞ ｜ (fullwidth); control characters -> ␀..␟;
//   the last character, when a space or a period -> ␠ or ．; "." and ".." -> ． and ．．;
//   rclone's own replacements in the name (／ ␡ ␀..␟) as the characters they stand for, the
//   Windows ones with ‛ before them (‛ itself: ‛‛); bytes that aren't UTF-8 -> ‛ and their hex;
//   a device name (CON, PRN, AUX, NUL, COM0-9, LPT0-9, before any extension) or one shaped like
//   an 8.3 short name -> "_" after it: CON_, nul_.txt, REPORT~1_.DOC; bidi controls dropped.
[[nodiscard]] std::string name(std::string_view remote_name);

// Whether a name (as name() makes it) is a Windows device name.
[[nodiscard]] bool is_device(std::string_view local_name);
// Whether a name (as name() makes it) could be a file's 8.3 short name ("REPORT~1.DOC").
[[nodiscard]] bool is_short_name(std::string_view local_name);
// Whether grab names any part of a relative remote path (a/b/c) otherwise than rclone would (a
// device name, a short name, bidi controls): rclone would write it elsewhere, so grab fetches it
// itself and checks it by its sum.
[[nodiscard]] bool differs_from_rclone(std::string_view remote_rel);
// `text` with each bidi control shown as "⟨U+202E⟩", so a server's name can't read backwards on
// screen (the window does the same, app.js).
[[nodiscard]] std::string bidi_visible(std::string_view text);

// A local path as Windows compares names (ignoring case), for sets of paths.
[[nodiscard]] std::filesystem::path::string_type key(const std::filesystem::path& p);
// A local path as rclone matches names copying to or checking Windows: case aside, and the
// Unicode spellings of a character as one ("e" + U+0301 is "é"), which NTFS keeps apart.
[[nodiscard]] std::filesystem::path::string_type fold(const std::filesystem::path& p);
// Of these relative remote paths (a/b/c), the ones rclone takes for another of them on Windows,
// by the file's name or a folder on its way ("README" and "readme", "Docs/a" and "docs/b", "café"
// spelled two ways): rclone copies or checks one of each such group and drops the rest, a folder
// with all it holds (review G25).
[[nodiscard]] std::unordered_set<std::string> clashing(std::span<const std::string> remote_rels);

// A relative remote path (a/b/c) as a local one, each part through name(); empty parts dropped.
// Every part is a plain name (no separator, drive, "." or ".."), so `dest / relative(x)` stays
// below dest for any x.
[[nodiscard]] std::filesystem::path relative(std::string_view remote_rel);

} // namespace grab::local
