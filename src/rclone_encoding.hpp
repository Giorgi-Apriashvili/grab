#pragma once

// rclone's name encoding (lib/encoder, v1.75.1), for the two encodings that matter to grab:
// Standard, the form rclone's own paths are in, and the Windows local backend's ("OS"). rclone
// writes a local name as windows.Encode(standard.Decode(name)), and that applies to every part of
// a local path it is given on its command line too, not only to the names it copies (review G80,
// 2026-10-21): handed "D:\x\Mission： Impossible", it wrote into "Mission‛： Impossible". So a
// local path grab hands to rclone goes in the Standard form of each part (local_path).

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace grab::rclone_enc {

using Mask = std::uint32_t;
// The flags of rclone's MultiEncoder these two encodings use (NUL, ␀ and ‛ are always handled).
inline constexpr Mask slash = 1U << 0;        // /
inline constexpr Mask lt_gt = 1U << 1;        // < >
inline constexpr Mask double_quote = 1U << 2; // "
inline constexpr Mask colon = 1U << 3;        // :
inline constexpr Mask question = 1U << 4;     // ?
inline constexpr Mask asterisk = 1U << 5;     // *
inline constexpr Mask pipe = 1U << 6;         // |
inline constexpr Mask back_slash = 1U << 7;   // backslash
inline constexpr Mask del = 1U << 8;          // DEL (0x7F)
inline constexpr Mask ctl = 1U << 9;          // 0x01-0x1F
inline constexpr Mask right_space = 1U << 10; // a trailing space
inline constexpr Mask right_period = 1U << 11; // a trailing period
inline constexpr Mask invalid_utf8 = 1U << 12; // bytes that aren't UTF-8, as ‛XX
inline constexpr Mask dot = 1U << 13;          // names that are exactly "." or ".."

inline constexpr Mask standard = slash | ctl | del | dot;
inline constexpr Mask windows = slash | dot | colon | question | double_quote | asterisk | lt_gt | pipe | back_slash |
                                ctl | right_space | right_period | invalid_utf8;

// Whether `s` is UTF-8 throughout (as Go's utf8 package sees it).
[[nodiscard]] bool valid_utf8(std::string_view s);

// rclone's MultiEncoder.Encode and .Decode for `mask`.
[[nodiscard]] std::string encode(std::string_view in, Mask mask);
[[nodiscard]] std::string decode(std::string_view in, Mask mask);

// grab's form of a server path is rclone's Standard one (a literal ‛ quoted, control characters
// as symbols), with bytes that aren't UTF-8 as ‛XX as well: what rclone shows for every name it
// can handle, and valid UTF-8 always, for the window and queue.json (review G17, G60). grab's own
// SFTP client hands the server the raw bytes.
inline constexpr Mask grab_path = standard | invalid_utf8;
// A server's name (one part of a path, raw bytes) in grab's form.
[[nodiscard]] std::string from_raw_name(std::string_view raw);
// A path in grab's form as raw bytes for the server, part by part ('/' stays the separator).
[[nodiscard]] std::string raw_path(std::string_view path);
// The reverse: a server's path of raw bytes (as find prints it) in grab's form, part by part.
[[nodiscard]] std::string from_raw_path(std::string_view raw);

// A server's free text (an SFTP status message, a line find printed) safe to show: its first line
// that isn't blank, at most `max` bytes of it with "…" after a cut, with control characters, DEL
// and bytes that aren't UTF-8 as symbols and ‛XX. "" when there is none.
[[nodiscard]] std::string shown_line(std::string_view text, std::size_t max = 100);

// rclone's FromStandardName(windows, name): what rclone writes on Windows for `name`.
[[nodiscard]] std::string from_standard_name(std::string_view name);
// rclone's ToStandardName(windows, name): the Standard form of a Windows name.
[[nodiscard]] std::string to_standard_name(std::string_view name);

// `p` as rclone's command line must say it so that rclone uses exactly `p` (on Windows each part
// after the drive or share in its Standard form; elsewhere `p` as is). Fails for a part rclone
// can't be told as it is, rather than letting rclone write somewhere else: "a＼b" (its Standard
// form "a\b" is two parts), a ‛ before an ordinary character ("a‛b"), a byte that isn't UTF-8.
[[nodiscard]] std::expected<std::string, std::string> local_path(const std::filesystem::path& p);

} // namespace grab::rclone_enc
