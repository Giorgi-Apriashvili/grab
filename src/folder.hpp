#pragma once

// Pieces of grab-gui's folder downloads that need no process or window: the folder's file
// listing, which files take the resumable path, the list handed to rclone, rclone's per-file
// log lines, and which of a big folder's files the page is shown.

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace grab::folder {

// Files this big are fetched by grab's resumable ranged download (fetch::run); smaller ones go
// in one `rclone copy` batch that reuses its connections.
inline constexpr std::uint64_t big_file_threshold = std::uint64_t{64} << 20;
[[nodiscard]] inline bool is_big(std::uint64_t size) { return size >= big_file_threshold; }

struct FolderFile {
    std::string path; // relative to the folder, '/'-separated, as rclone names it
    std::uint64_t size = 0;
    std::string modtime;
    bool operator==(const FolderFile&) const = default;
};

// `rclone lsjson -R --files-only` output, sorted by path.
[[nodiscard]] std::expected<std::vector<FolderFile>, std::string> parse_listing(std::string_view json_text);

// A `--files-from-raw` list: one relative path per line, taken literally (no filter syntax).
[[nodiscard]] std::string files_from_text(const std::vector<std::string>& paths);

// The file named by an info-level "Copied (new)" / "Copied (replaced existing)" JSON log line.
[[nodiscard]] std::optional<std::string> parse_copied_line(std::string_view line);

enum class State { queued, running, paused, done, failed, skipped };

// Which files of a folder the page lists, in folder order: every running, paused and failed
// one, and the first `queued_limit` queued ones. Done and skipped files are only counted.
[[nodiscard]] std::vector<std::size_t> visible(const std::vector<State>& states, std::size_t queued_limit);

} // namespace grab::folder
