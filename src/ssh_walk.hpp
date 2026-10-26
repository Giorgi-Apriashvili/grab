#pragma once

// Walking a folder tree on a server through grab's engine: folders taken from a shared queue and
// read on several of the server's pooled connections at once (each folder: OPENDIR, READDIR until
// the end, CLOSE). Replaces `rclone lsjson -R` for folder downloads; searches and file lists
// follow (RCLONE-REPLACEMENT.md, phase 4).

#include "ssh_pool.hpp"

#include <cstdint>
#include <expected>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace grab::ssh {

struct Walked {
    std::string path; // relative to the root, '/'-separated, each name in grab's form (rclone_enc::grab_path)
    std::uint64_t size = 0;
    std::string modtime; // RFC 3339, UTC; empty when the server didn't say
    bool dir = false;
    bool operator==(const Walked&) const = default;
};

struct Walk {
    std::vector<Walked> entries;         // sorted by path
    std::vector<std::string> unreadable; // folders below the root that couldn't be read: "a/b: permission denied"
};

struct WalkOptions {
    int connections = 4;  // the pool's connections used at once
    bool folders = false; // list folders as entries too (a folder download wants only files)
};

// Every file below `root` (a path in grab's form; "" or "." for the login folder). Links are
// followed as rclone follows them: one to a file lists as that file; one to a folder is walked
// under the link's name, unless it leads into the tree itself or to a folder walked already (no
// loops, nothing twice). A folder below the root that can't be read is noted in `unreadable`
// rather than ending the walk (review G55); the root's own failure is the error, as is a login
// that fails for good. Empty with no error when `stop` was requested.
[[nodiscard]] std::expected<Walk, std::string> walk(Pool& pool, std::string_view root, const WalkOptions& options,
                                                    std::stop_token stop);

} // namespace grab::ssh
