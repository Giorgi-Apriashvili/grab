#pragma once

// rclone's name encoding (lib/encoder, v1.75.1), for the two encodings that matter to grab:
// Standard, the form rclone's own paths are in, and the Windows local backend's ("OS"). rclone
// wrote a local name as windows.Encode(standard.Decode(name)); grab's local names
// (local_name.hpp) keep that form, so files land where grab 0.19 and before put them.

#include <cstddef>
#include <cstdint>
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

// `s` in upper case as Go's strings.ToUpper makes it, which is how rclone named a remote's
// environment variables (RCLONE_CONFIG_<NAME>_<KEY>); server names that differ only by it count as
// the same (server_conf.cpp's same_ignoring_case): every letter by its simple upper-case mapping
// in the tables of the Go that rclone 1.75.1 is built with (Go 1.26, Unicode 15.0.0: 'ß' stays,
// 'ǅ' becomes 'Ǆ', Georgian Mkhedruli becomes Mtavruli), a byte that isn't UTF-8 U+FFFD. Windows
// matches variable names ignoring case by an older table of its own, without Georgian, 'ſ', 'ı',
// 'µ', 'ς' and others (RCLONE-REPLACEMENT.md, phase 7).
[[nodiscard]] std::string go_upper(std::string_view s);

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

} // namespace grab::rclone_enc
