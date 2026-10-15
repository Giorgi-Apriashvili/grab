#pragma once

// The grab command's downloads with grab's own engine: files with the resumable ranged download
// (fetch::run), folders with folder::run, one after another, each with the server's whole
// connection budget. Progress is one line redrawn on the console; Ctrl+C stops cleanly and
// keeps what is on disk for the next run.

#include "cli.hpp"
#include "engine.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace grab {

inline constexpr int exit_download_failed = 6;

struct DownloadTally {
    std::size_t done = 0;
    std::vector<std::string> failed; // remote paths
    bool stopped = false;            // Ctrl+C
    int code = 0;                    // the first failure's exit code, 0 if none
};

// Downloads `paths` (picked search hits, files or folders by opts.mode) into `dest`. `sizes` are
// the hits' sizes, for --dry-run.
[[nodiscard]] DownloadTally download_with_grab(const engine::Context& ctx, const Options& opts,
                                               const std::vector<std::string>& paths,
                                               const std::unordered_map<std::string, std::uint64_t>& sizes,
                                               const std::filesystem::path& dest);

} // namespace grab
