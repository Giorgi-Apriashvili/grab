#pragma once

#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

namespace grab::proc {

// Exit code reported for a child that was stopped through RunOptions::stop (and, on Windows,
// for STATUS_CONTROL_C_EXIT), mirroring the POSIX 128 + SIGINT convention.
inline constexpr int exit_stopped = 130;

struct RunOptions {
    // request_stop() terminates the child and everything it started (a Job object on
    // Windows, the process group on POSIX); the run then returns exit_stopped.
    std::stop_token stop;
    // No console: no console window for the child (CREATE_NO_WINDOW), stdin is NUL, stderr
    // is captured instead of shared. For the GUI, which has no console to share.
    bool detached = false;
    // run_capture only: bytes written to the child's stdin (then closed), e.g. a password for
    // `rclone obscure -`, so secrets never appear on a command line.
    std::optional<std::string> input;
};

struct CaptureResult {
    int exit_code = 0;
    std::string out; // child's stdout, raw bytes
    std::string err; // child's stderr; only captured when RunOptions::detached
};

// Run argv[0] with stdout captured. Unless detached, stdin and stderr stay attached to the
// console so an ssh passphrase prompt still works, and Ctrl+C is left to the child.
[[nodiscard]] std::expected<CaptureResult, std::string>
run_capture(std::span<const std::string> argv, const RunOptions& opts = {});

// Run argv[0] with stdin and stdout on NUL, calling `on_line` for every stderr line (without
// the line break), e.g. rclone's --use-json-log output. Returns the exit code.
[[nodiscard]] std::expected<int, std::string>
run_streaming(std::span<const std::string> argv, const std::function<void(std::string_view)>& on_line,
              const RunOptions& opts = {});

// Run argv[0] with all three streams inherited (live progress output). Returns its exit code.
// On Windows the child is in a kill-on-close Job object, and by default so is everything it
// starts. `contain_descendants` false lets the child's own children leave the job, so they
// outlive it: an installer that restarts grab-gui after updating it (Restart Manager).
[[nodiscard]] std::expected<int, std::string> run_inherit(std::span<const std::string> argv,
                                                          bool contain_descendants = true);

} // namespace grab::proc
