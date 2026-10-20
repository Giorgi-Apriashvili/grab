#pragma once

#include "existing.hpp"
#include "file_type.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace grab {

enum class Mode { file, folder };

struct Options {
    Mode mode = Mode::file;
    std::string target;              // name (or absolute remote path) to look for
    std::optional<std::filesystem::path> dest; // local directory; asked for when absent
    std::optional<std::string> remote;
    std::optional<std::filesystem::path> config;
    std::optional<int> depth;
    // --limit: download speed cap in bytes per second; 0 = off. Without it, the speed schedule.
    std::optional<std::uint64_t> limit;
    std::optional<bool> verify;      // --verify / --no-verify; without either, the window's setting
    // --existing: a different file already where a download goes; without it, grab asks.
    std::optional<existing::Choice> existing;
    file_type::Filter filter;        // --type, --min-size, --max-size: which file hits count
    bool first = false;
    bool all = false;                // --all: take every match without asking
    bool exact = false;              // --exact: whole-name, case-sensitive matching
    bool rclone = false;             // --rclone: plain rclone copyto / copy instead of grab's engine
    bool fresh = false;              // --fresh: fetch the server's file list before searching
    bool dry_run = false;
    bool verbose = false;
    bool check = false;              // `grab update --check`
    std::vector<std::string> extra;  // everything after "--", added to every rclone call
    std::vector<std::string> server_args; // `grab server SUBCOMMAND [NAME]`
};

enum class CliAction { run, help, version, init, config, update, server };

struct CliResult {
    CliAction action = CliAction::run;
    Options opts;
};

// `args` excludes the program name.
[[nodiscard]] std::expected<CliResult, std::string> parse_args(std::span<const std::string> args);

[[nodiscard]] std::string usage();

[[nodiscard]] constexpr const char* mode_noun(Mode m) {
    return m == Mode::folder ? "folder" : "file";
}

} // namespace grab
