#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace grab {

enum class Mode { file, folder };

struct Options {
    Mode mode = Mode::folder;
    std::string target;              // name (or absolute remote path) to look for
    std::filesystem::path dest;      // local directory to download into
    std::optional<std::string> remote;
    std::optional<std::filesystem::path> config;
    std::optional<int> depth;
    bool first = false;
    bool all = false;                // --all: take every match without asking
    bool exact = false;              // --exact: whole-name, case-sensitive matching
    bool dry_run = false;
    bool verbose = false;
    bool check = false;              // `grab update --check`
    std::vector<std::string> extra;  // everything after "--", appended to the rclone argv
};

enum class CliAction { run, help, version, init, config, update };

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
