#pragma once

// The grab command's downloads with grab's own engine: files with the resumable ranged download
// (fetch::run), folders with folder::run, one after another, each with the server's whole
// connection budget. Progress is one line redrawn on the console; Ctrl+C stops cleanly and
// keeps what is on disk for the next run.

#include "cli.hpp"
#include "engine.hpp"
#include "schedule.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace grab {

inline constexpr int exit_download_failed = 6;
inline constexpr int exit_aborted = 3; // as a pick that was aborted

// A file already there that a download would replace although it differs from the server's.
struct ExistingFile {
    std::string name;                     // as shown
    std::filesystem::path local;          // the file there
    std::uint64_t size = 0;               // the server's
    std::optional<std::int64_t> modified; // the server's, Unix seconds
};
// What to do with them (`count` of them in all, `files` some): --existing, else asked once on
// the console ([r]eplace, [k]eep both, [s]kip, [q]uit; no Keep both unless `keep_both`). An exit
// code instead when the user quits, or there is no console to ask on (with a message).
[[nodiscard]] std::expected<existing::Choice, int> choose_existing(const Options& opts, const std::vector<ExistingFile>& files,
                                                                 std::size_t count, bool keep_both);

struct DownloadTally {
    std::size_t done = 0;
    std::vector<std::string> failed; // remote paths
    bool stopped = false;            // Ctrl+C
    int code = 0;                    // the first failure's exit code, 0 if none
};

// "speed limited to 5 MiB/s until 24:00 by the speed schedule (...)" when the speed schedule set
// in grab's window limits the speed now.
[[nodiscard]] std::optional<std::string> schedule_note(const std::vector<schedule::Rule>& rules,
                                                       const schedule::Now& now);

// Downloads `paths` (picked search hits, files or folders by opts.mode) into `dest`. `sizes` and
// `modified` (Unix seconds) are the hits', for the checks before fetching and --dry-run.
[[nodiscard]] DownloadTally download_with_grab(const engine::Context& ctx, const Options& opts,
                                               const std::vector<std::string>& paths,
                                               const std::unordered_map<std::string, std::uint64_t>& sizes,
                                               const std::unordered_map<std::string, std::int64_t>& modified,
                                               const std::filesystem::path& dest);

} // namespace grab
