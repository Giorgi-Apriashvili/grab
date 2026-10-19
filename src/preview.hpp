#pragma once

// Previews in grab's window: videos and audio play in the program Windows uses for them,
// streamed from a local `rclone serve http` (on 127.0.0.1 only, behind a random path); images,
// text files and PDFs show in the window's viewer. The parts here are the ones without Windows:
// what kind of file a name is, the server's command line, its address from its log, and text
// decoding.

#include "engine.hpp"

#include <cstddef>
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

} // namespace grab::preview
