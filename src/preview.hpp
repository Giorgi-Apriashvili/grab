#pragma once

// Previews in grab's window: videos and audio play in the program Windows uses for them,
// streamed from a local `rclone serve http` (on 127.0.0.1 only, behind a random path); images,
// text files and PDFs show in the window's viewer. The parts here are the ones without Windows:
// what kind of file a name is, the server's command line, its address from its log, and text
// decoding.

#include "engine.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace grab::preview {

enum class Kind { none, video, audio, image, text, pdf };

// From the name's extension, ignoring case.
[[nodiscard]] Kind kind_of(std::string_view name);
// The content type the window gets for a previewed file ("image/png"; text is converted to UTF-8).
[[nodiscard]] std::string content_type(std::string_view name);

// How much of a file the viewer takes: text shows its first MiB; bigger images and PDFs are not
// previewed.
inline constexpr std::size_t max_text = std::size_t{1} << 20;
inline constexpr std::size_t max_image = std::size_t{64} << 20;
inline constexpr std::size_t max_pdf = std::size_t{128} << 20;
[[nodiscard]] std::size_t max_bytes(Kind kind);

// `rclone serve http <remote:root>` on a free port of 127.0.0.1, read-only, under `/<token>/`.
// Measured 2026-10-19 on a seek far into a film: one connection per file (rclone's parallel chunk
// reads took 15-27 s to start on a Storage Box), and few SFTP reads in flight, since with the
// configured 255Ki chunks rclone's default 64 made a seek wait 18-26 s on a Storage Box (2.5 MB/s
// per connection); 4 there start in about 3 s, and 8 elsewhere keep Main's 17 MiB/s.
[[nodiscard]] std::vector<std::string> serve_argv(const engine::Context& ctx, std::string_view root,
                                                  std::string_view token);
// The port from rclone's "HTTP Server started on [http://127.0.0.1:PORT/...]" log line.
[[nodiscard]] std::optional<int> served_port(std::string_view log);

// The server root and the path under it for a remote path: "" (the login home) for a relative
// one, "/" for an absolute one.
struct Where {
    std::string root;
    std::string path;
    bool operator==(const Where&) const = default;
};
[[nodiscard]] Where where(std::string_view remote_path);

// `path` for a URL: each segment percent-encoded, the slashes kept.
[[nodiscard]] std::string url_path(std::string_view path);
// The other way, for the window's requests; nullopt for a broken escape.
[[nodiscard]] std::optional<std::string> url_decode(std::string_view text);

// A text file's bytes as UTF-8 for the viewer: .nfo files as the DOS code page (CP437, whose
// box-drawing characters make their ASCII art); others as UTF-8 when they are, else
// Windows-1252.
[[nodiscard]] std::string text_utf8(std::string_view bytes, bool nfo);

// ---- Play's warm start (src/gui/play_server.*) ----
// What a player reads while it opens a file (its start, up to the first video, and its end) is
// fetched over several connections before the player starts: one stream from a Storage Box gives
// ~2.3 MiB/s with 2.5-5 s per seek, and an MKV can hold tens of MB of fonts before its first
// frame (MPC-HC gives up after ~10 s).
inline constexpr std::uint64_t warm_margin = std::uint64_t{8} << 20; // after the first video (a player probes a few seconds of it), and the least
inline constexpr std::uint64_t warm_first = std::uint64_t{16} << 20; // the first read, from the start
inline constexpr std::uint64_t warm_max = std::uint64_t{128} << 20;  // the start, at most
inline constexpr std::uint64_t warm_tail = std::uint64_t{4} << 20;   // the end: an MKV's or MP4's index
// How much of the start the player needs before it starts, from the file's first bytes (`head`)
// and its size: an MKV up to its first video cluster and warm_margin more; other files
// warm_margin. At most warm_max and the size.
[[nodiscard]] std::uint64_t warm_start(std::string_view head, std::uint64_t size);
// Where an MKV's first video cluster starts (or the first element its `head` doesn't reach,
// before which the cluster can't be); nullopt for anything else.
[[nodiscard]] std::optional<std::uint64_t> mkv_first_cluster(std::string_view head);

// A player's "Range: bytes=..." for a file of `size`, inclusive. whole: none, or one this
// doesn't understand (several ranges); unsatisfiable: it starts past the end.
struct Requested {
    enum class Kind { whole, part, unsatisfiable } kind = Kind::whole;
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    bool operator==(const Requested&) const = default;
};
[[nodiscard]] Requested requested_range(std::string_view value, std::uint64_t size);

} // namespace grab::preview
