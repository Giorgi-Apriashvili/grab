#pragma once

// A local folder's files and folders, for uploading it (UPLOADS.md 5.3, 5.6). Links below the
// folder (symbolic links to files or folders, and junctions) are never followed: they are left
// out and counted, so an upload never reaches outside what was picked or loops; the folder itself
// is taken as picked, a link too. grab's own partial downloads are left out: a "*.grabpart" whose
// record beside it parses, and a "*.grabpart.json" that parses. Long paths are read whether or not
// Windows' long paths are on (each file then says for itself whether it can be read).

#include "folder.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace grab::local_walk {

struct Walked {
    // Every file below the folder, sorted by path: '/'-separated, each name the local one in
    // grab's form (rclone_enc::from_raw_name of its UTF-8), so that rclone_enc::raw_path gives the
    // name back; its size; its modification time (RFC 3339 in UTC, to the nanosecond).
    std::vector<folder::FolderFile> files;
    // Every folder below it in the same form, sorted, so each comes after the folder it is in.
    std::vector<std::string> folders;
    std::size_t links = 0;    // links and junctions below it, left out
    std::size_t partials = 0; // grab's partial downloads and their records, left out
    // Folders that couldn't be read, names that can't be written in UTF-8, and (on Windows) names
    // ending in a dot or a space, which open as another file: "a/b: why". Left out.
    std::vector<std::string> unreadable;
};

// The folder `root`'s files and folders. An error when `root` itself can't be read or isn't a
// folder; empty with no error when `stop` was requested.
[[nodiscard]] std::expected<Walked, std::string> walk(const std::filesystem::path& root, std::stop_token stop = {});

// Whether walk() would leave `path` out of a folder: a link or a junction (never followed), or one
// of grab's own partial downloads (a "*.grabpart" whose record beside it parses, a
// "*.grabpart.json" that parses). For names a pattern picked (grab upload's `*`), which the user
// didn't name one by one.
enum class Left { no, link, partial };
[[nodiscard]] Left left_out(const std::filesystem::path& path);

// The local path of a file or folder `rel` (Walked's form) in `root`.
[[nodiscard]] std::filesystem::path local_path(const std::filesystem::path& root, std::string_view rel);

} // namespace grab::local_walk
