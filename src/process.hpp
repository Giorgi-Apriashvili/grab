#pragma once

#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

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
    // run_capture only: each chunk of the child's stdout as it arrives (it is captured too), e.g.
    // to count a listing's entries while it runs, for a progress bar. On the reading thread.
    std::function<void(std::string_view)> on_output;
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

struct PipedResult {
    int exit_code = 0;
    std::string err; // the child's stderr
};

// Run argv[0] with no console and stdin on NUL, handing its stdout to `on_data` in binary
// chunks as they arrive (e.g. `rclone cat` of a byte range) and capturing stderr. `on_data`
// returning false stops the child, like RunOptions::stop; the result is then exit_stopped.
[[nodiscard]] std::expected<PipedResult, std::string>
run_piped(std::span<const std::string> argv, const std::function<bool(std::string_view)>& on_data,
          const RunOptions& opts = {});

// Run argv[0] with all three streams inherited (live progress output). Returns its exit code.
// On Windows the child is in a kill-on-close Job object, and by default so is everything it
// starts. `contain_descendants` false lets the child's own children leave the job, so they
// outlive it: an installer that restarts grab-gui after updating it (Restart Manager).
[[nodiscard]] std::expected<int, std::string> run_inherit(std::span<const std::string> argv,
                                                          bool contain_descendants = true);

// A program started by start_independent. Dropping this object leaves it running.
class Independent {
public:
    Independent() = default;
    ~Independent();
    Independent(Independent&& o) noexcept : handle_(std::exchange(o.handle_, nullptr)) {}
    Independent& operator=(Independent&& o) noexcept;
    Independent(const Independent&) = delete;
    Independent& operator=(const Independent&) = delete;
    // Its exit code once it has ended, waiting up to `timeout`; nullopt while it still runs.
    [[nodiscard]] std::optional<int> wait(std::chrono::milliseconds timeout);

private:
    friend std::expected<Independent, std::string> start_independent(std::span<const std::string> argv,
                                                                     const std::filesystem::path& output);
    void* handle_ = nullptr; // the process handle (Windows)
};

// Starts argv[0] on its own: no console, stdin on NUL, stdout and stderr written to `output`
// (created or emptied), and outside grab's job objects, so it keeps running when this process
// ends. For grab-gui starting `grab update`, whose installer closes grab-gui. Windows only.
[[nodiscard]] std::expected<Independent, std::string> start_independent(std::span<const std::string> argv,
                                                                       const std::filesystem::path& output);

} // namespace grab::proc
