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
#include <vector>

namespace grab::proc {

// Exit code reported for a child that was stopped through RunOptions::stop (and, on Windows,
// for STATUS_CONTROL_C_EXIT), mirroring the POSIX 128 + SIGINT convention.
inline constexpr int exit_stopped = 130;

// What a child's environment gets beyond the one it inherits (the tests' rclone runs, which
// compare grab with rclone 1.75.1, give it a server's RCLONE_CONFIG_<NAME>_* this way, and leave
// out the user's own RCLONE_*). Names are compared as the system compares them: ignoring case on
// Windows (CompareStringOrdinal), exactly elsewhere.
struct Environment {
    // UTF-8 names and values, each replacing an inherited variable of the same name.
    std::vector<std::pair<std::string, std::string>> set;
    // Inherited variables whose names start with one of these are left out ("RCLONE_": the user's
    // own rclone settings, which an rclone run of the tests mustn't see).
    std::vector<std::string> without_prefix;
};

struct RunOptions {
    // request_stop() terminates the child and everything it started (a Job object on
    // Windows, the process group on POSIX); the run then returns exit_stopped.
    std::stop_token stop;
    // No console: no console window for the child (CREATE_NO_WINDOW), stdin is NUL, stderr
    // is captured instead of shared. For the GUI, which has no console to share.
    bool detached = false;
    // Bytes written to the child's stdin (then closed), so secrets never appear on a command line.
    std::optional<std::string> input;
    // Each chunk of the child's stdout as it arrives (it is captured too). On the reading thread.
    std::function<void(std::string_view)> on_output;
    // Added to the child's environment (this process's).
    Environment environment;
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
// This process's environment as a CreateProcess block (UTF-16) without any RCLONE_* variable: for
// a long-lived program of someone else's (the media player Play starts; review G7). Every other
// child (curl, tar, an editor, an installer) inherits the environment as it is, which holds no
// server's secrets since grab stopped running rclone (RCLONE-REPLACEMENT.md, phases 7 and 9).
[[nodiscard]] std::wstring environment_without_rclone();
// Remove every RCLONE_CONFIG_* (any case) from this process's own environment; RCLONE_CONFIG
// itself stays. grab-gui calls it first thing: a 0.19 grab-gui kept every server's secrets in its
// environment, and an in-app update hands that environment to the next window, whose editors,
// Explorer, browser and installer would pass them on in turn (review G7).
void drop_inherited_server_variables();
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

// Run argv[0] with all three streams and the whole environment inherited (live progress output).
// Returns its exit code. On Windows the child is in a kill-on-close Job object, and by default so
// is everything it starts. `contain_descendants` false lets the child's own children leave the
// job, so they outlive it: an installer that starts grab-gui again (the Restart Manager's restart,
// or its own after an install that failed).
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
// ends. For grab-gui starting `grab update`, which closes grab-gui for an installer, and for
// `grab update` starting grab-gui again afterwards. Windows only.
[[nodiscard]] std::expected<Independent, std::string> start_independent(std::span<const std::string> argv,
                                                                       const std::filesystem::path& output);

} // namespace grab::proc
