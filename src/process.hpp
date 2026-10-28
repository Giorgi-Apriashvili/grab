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

// The program argv[0] names, as every run_* and start_independent start it (review G3). A name
// with a folder in it is taken as given. A bare name ("ssh", "curl") comes from System32, the
// Windows folder or an absolute PATH entry, in that order, with ".exe" added when it has no
// extension; never from the current folder or grab's own (where a download could have put a
// program of that name), nor from a relative PATH entry. `pathext`: also try PATHEXT's other
// extensions (.cmd, .bat...), as ShellExecute would. On POSIX, argv[0] as given (posix_spawnp
// searches PATH, which doesn't include the current folder unless PATH says so).
[[nodiscard]] std::expected<std::filesystem::path, std::string> find_program(std::string_view argv0,
                                                                           bool pathext = false);

#ifdef _WIN32
// This process's environment as a CreateProcess block (UTF-16), without the RCLONE_* variables
// grab's internal rclone calls must not see (rclone_flags::kept_variable; review G8). run_capture,
// run_streaming and run_piped give their children this one; run_inherit (the user's own --rclone
// copy, an editor, an installer) and start_independent everything.
[[nodiscard]] std::wstring internal_environment();
// The same without any RCLONE_* variable at all, the RCLONE_CONFIG_* secrets grab exports too: for
// a long-lived program of someone else's (the media player Play starts; review G7, partly).
[[nodiscard]] std::wstring environment_without_rclone();
#endif

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
