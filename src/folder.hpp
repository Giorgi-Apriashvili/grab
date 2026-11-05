#pragma once

// Pieces of folder downloads that need no process or window: a file of the folder's listing and
// the states of its files. The download itself is in folder_job.hpp; what grab-gui's page is shown
// of a folder's files, in queue_view.hpp.

#include <cstdint>
#include <string>

namespace grab::folder {

struct FolderFile {
    std::string path; // relative to the folder, '/'-separated, in grab's form (rclone_enc::grab_path)
    std::uint64_t size = 0;
    std::string modtime;
    bool operator==(const FolderFile&) const = default;
};

// The order is the page's too: "files" and "fileDelta" send a state as its number (queue_view.hpp).
enum class State { queued, running, paused, done, failed, skipped };

} // namespace grab::folder
