#pragma once

#include "cli.hpp"
#include "remote.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace grab {

// Looking a target up with `rclone lsf` instead of ssh + find. Works on hosts without a
// shell (storage boxes), needs no password or passphrase prompt, and costs an SFTP walk of
// the tree bounded by max_depth.
struct ListRequest {
    Mode mode = Mode::folder;
    std::string target;   // search words / pattern, or a remote path containing '/'
    bool exact = false;   // --exact: whole-name, case-sensitive (see match.hpp)
    std::string root;     // "" = the login home, "/abs" or "relative/to/home"
    int max_depth = 4;
    bool skip_hidden = true;
    std::string rclone_exe;
    std::optional<std::filesystem::path> rclone_config;
    std::string rclone_remote;
    // Files and folders together (a server's whole file list), instead of `mode`'s kind only.
    bool all_entries = false;
    std::vector<std::string> flags; // the server's connection flags (rclone_flags.hpp)
};

// ("", "a/b") -> "a/b"   ("/", "a") -> "/a"   ("x/", "a") -> "x/a"
[[nodiscard]] std::string join_remote(std::string_view root, std::string_view rel);

// "learning/x" -> "learning"   "/x" -> "/"   "a/b/" -> "a"   "x" -> ""
[[nodiscard]] std::string parent_of(std::string_view path);

// Name search: `rclone lsf REMOTE:root --format tsp --time-format unix --csv --dirs-only|--files-only
// -R --max-depth N [--exclude ...]`.
// Path target:  `rclone lsf REMOTE:<parent> --format tsp --time-format unix --csv
// --dirs-only|--files-only` (one level).
[[nodiscard]] std::vector<std::string> build_lsf_argv(const ListRequest& req);

// --csv output (Go's encoding/csv): records of comma-separated fields; a field holding a comma, a
// quote, a line break or a leading space is in quotes, its quotes doubled. A name can hold a line
// break, so a record isn't a line (review G59).
[[nodiscard]] std::vector<std::vector<std::string>> parse_csv(std::string_view text);

// One record of that output: <time>,<size>,<path>, the time in Unix seconds, folders ending in '/'
// with size -1.
struct LsfLine {
    std::string path; // a folder's trailing '/' included
    std::optional<std::uint64_t> size;
    std::optional<std::int64_t> modified;
};
// nullopt unless it is the three fields with a size (or -1); a time that doesn't parse gives an
// unknown date.
[[nodiscard]] std::optional<LsfLine> parse_lsf_record(const std::vector<std::string>& fields);

// Entries from lsf output whose last component matches the query (see match.hpp); paths are
// joined onto the root / parent.
[[nodiscard]] std::vector<RemoteEntry> parse_lsf_output(const ListRequest& req,
                                                        std::string_view out);

} // namespace grab
