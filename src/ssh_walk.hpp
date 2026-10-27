#pragma once

// Walking a folder tree on a server through grab's engine: folders taken from a shared queue and
// read on several of the server's pooled connections at once, several folders in flight on each
// (OPENDIR, READDIR until the end, a STAT for each link, all pipelined; CLOSE not waited for).
// Replaces `rclone lsjson -R` for folder downloads and `rclone lsf -R` for searches and file lists
// on servers without find (RCLONE-REPLACEMENT.md, phases 3 and 4).

#include "sftp.hpp"
#include "ssh_pool.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace grab::ssh {

struct Walked {
    // '/'-separated, each name in grab's form (rclone_enc::grab_path): relative to the root for
    // walk(), joined onto its root as a search gives it for walk_roots() ("" + "a" = "a",
    // "/data" + "a" = "/data/a").
    std::string path;
    std::uint64_t size = 0;
    std::string modtime; // RFC 3339, UTC; empty when the server didn't say
    bool dir = false;
    std::optional<std::int64_t> modified; // the same time in Unix seconds
    bool operator==(const Walked&) const = default;
};

struct Walk {
    std::vector<Walked> entries; // sorted by path
    // Folders that couldn't be read ("a/b: permission denied", paths as in `entries`); for
    // walk_roots() also a root that failed ("/data: no such file or folder").
    std::vector<std::string> unreadable;
};

struct WalkOptions {
    int connections = 4;  // the pool's connections used at once
    bool folders = false; // list folders as entries too (a folder download wants only files)
    int max_depth = 0;    // levels below the root listed (1: the root's own entries); 0: all
    // Dot-names neither listed nor entered, as searches leave them out (lsf's --exclude ".*" and
    // ".*/**"); a root itself is always read.
    bool skip_hidden = false;
    // Past this many names read the walk fails ("listing too large", review G168).
    std::size_t max_entries = sftp::max_listing;
    // ...and past this many bytes of the paths it keeps.
    std::uint64_t max_bytes = sftp::max_listing_bytes;
    // Folders read at once on each connection, each holding a handle on the server. Main's
    // `downloads` (11,000 folders, 177,000 entries, 4 connections): 1 took 142 s to depth 4,
    // 16 took 35 s for all of it, 32 18 s, 64 10 s (find: 2-4 s).
    int in_flight = 32;
    // The names read so far, now and then (from the walk's threads, one call at a time).
    std::function<void(std::size_t)> progress{};
    // For its logins (Pool::acquire): interactive_retry for a search or list a person waits on.
    Retry retry_refused = std::nullopt;
};

// Every file below `root` (a path in grab's form; "" or "." for the login folder). Links are
// followed as rclone follows them: one to a file lists as that file; one to a folder is walked
// under the link's name, unless it leads into the tree itself or to a folder walked already (no
// loops, nothing twice). A folder below the root that can't be read is noted in `unreadable`
// rather than ending the walk (review G55); the root's own failure is the error, as is a login
// that fails for good. Empty with no error when `stop` was requested.
[[nodiscard]] std::expected<Walk, std::string> walk(Pool& pool, std::string_view root, const WalkOptions& options,
                                                    std::stop_token stop);

// The same for several roots at once (a search's or a file list's), on the same connections:
// a root that can't be read is a note in `unreadable`, and only every root failing is an error.
// Each root's links are judged against that root alone, as separate listings would.
[[nodiscard]] std::expected<Walk, std::string> walk_roots(Pool& pool, std::span<const std::string> roots,
                                                          const WalkOptions& options, std::stop_token stop);

} // namespace grab::ssh
