#pragma once

#include "existing.hpp"
#include "file_type.hpp"
#include "upload_existing.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace grab {

enum class Mode { file, folder };

// A run's own connection settings (--bind, --contimeout, --timeout), which win over the server's
// bind, connect_timeout and io_timeout (engine::connect_settings).
struct ConnectOptions {
    std::optional<std::string> bind; // "": no bind for this run, whatever the server's
    std::optional<std::chrono::milliseconds> connect_timeout; // 0: off
    std::optional<std::chrono::milliseconds> io_timeout;      // 1 s to 24 h
};

struct Options {
    Mode mode = Mode::file;
    std::string target;              // name (or absolute remote path) to look for
    std::optional<std::filesystem::path> dest; // local directory; asked for when absent
    std::optional<std::string> remote;
    std::optional<std::filesystem::path> config;
    std::optional<int> depth;
    // --limit: download speed cap in bytes per second (`grab upload`: the upload speed's); 0 = off.
    // Without it, the speed schedule.
    std::optional<std::uint64_t> limit;
    std::optional<bool> verify;      // --verify / --no-verify; without either, the window's setting
    // --existing: a different file already where a download goes; without it, grab asks.
    std::optional<existing::Choice> existing;
    // `grab upload LOCAL... REMOTE_DIR`: the local files and folders as typed ('*' and '?' in a
    // last part are expanded when it runs), the server folder they go into in grab's form ("" the
    // login folder), and --existing for a file already on the server (without it, the window's
    // setting).
    std::vector<std::string> locals;
    std::string remote_dir;
    std::optional<upload::Existing> upload_existing;
    file_type::Filter filter;        // --type, --min-size, --max-size: which file hits count
    bool first = false;
    bool all = false;                // --all: take every match without asking
    bool exact = false;              // --exact: whole-name, case-sensitive matching
    bool fresh = false;              // --fresh: fetch the server's file list before searching
    bool dry_run = false;
    bool verbose = false;
    bool check = false;              // `grab update --check`
    ConnectOptions connect;          // --bind, --contimeout, --timeout
    std::vector<std::string> server_args; // `grab server SUBCOMMAND [NAME]`
};

enum class CliAction { run, help, version, init, config, update, server, upload };

struct CliResult {
    CliAction action = CliAction::run;
    Options opts;
};

// `args` excludes the program name. `--` ends the options: what follows is TARGET and DEST even
// when it starts with '-' (G117), except a word there that would become DEST, or one of the flags
// grab 0.20 passed on to rclone after `--`, which is refused with what to write instead.
// `config`, `update`, `server` and `upload` are commands only as the first word.
[[nodiscard]] std::expected<CliResult, std::string> parse_args(std::span<const std::string> args);

// REMOTE_DIR as typed into grab's form of a server folder: "" for the login folder (also ".",
// "~" and "~/"), "~/x" and "./x" relative to it, the slashes at its end and doubled ones dropped.
[[nodiscard]] std::string remote_dir_of(std::string_view typed);

[[nodiscard]] std::string usage();

[[nodiscard]] constexpr const char* mode_noun(Mode m) {
    return m == Mode::folder ? "folder" : "file";
}

} // namespace grab
