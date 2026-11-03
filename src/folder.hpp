#pragma once

// Pieces of folder downloads that need no process or window: a file of the folder's listing, the
// states of its files, and which of a big folder's files grab-gui's page is shown. The download
// itself is in folder_job.hpp.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace grab::folder {

struct FolderFile {
    std::string path; // relative to the folder, '/'-separated, in grab's form (rclone_enc::grab_path)
    std::uint64_t size = 0;
    std::string modtime;
    bool operator==(const FolderFile&) const = default;
};

enum class State { queued, running, paused, done, failed, skipped };

// Which files of a folder the page lists, in folder order: every running, paused and failed
// one, and the first `queued_limit` queued ones. Done and skipped files are only counted.
[[nodiscard]] std::vector<std::size_t> visible(const std::vector<State>& states, std::size_t queued_limit);

} // namespace grab::folder
