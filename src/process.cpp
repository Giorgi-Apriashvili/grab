#include "process.hpp"

#include "quote.hpp"
#include "util.hpp"

#include <format>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace grab::proc {

namespace {

// Splits a byte stream into lines for run_streaming, tolerating CRLF and a final line
// without a terminator.
class LineSplitter {
public:
    explicit LineSplitter(const std::function<void(std::string_view)>& sink) : sink_(sink) {}

    void feed(std::string_view chunk) {
        buffer_ += chunk;
        std::size_t start = 0;
        for (;;) {
            const auto nl = buffer_.find('\n', start);
            if (nl == std::string::npos) break;
            emit(std::string_view(buffer_).substr(start, nl - start));
            start = nl + 1;
        }
        buffer_.erase(0, start);
    }
    void finish() {
        if (!buffer_.empty()) emit(buffer_);
        buffer_.clear();
    }

private:
    void emit(std::string_view line) {
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (sink_) sink_(line);
    }
    const std::function<void(std::string_view)>& sink_;
    std::string buffer_;
};

} // namespace

#ifdef _WIN32

namespace {

BOOL WINAPI ignore_ctrl(DWORD type) {
    return (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) ? TRUE : FALSE;
}

// While a console child runs, grab itself ignores Ctrl+C; the child (rclone/ssh) receives it,
// cleans up, and grab reports the child's exit code.
struct CtrlGuard {
    bool active;
    explicit CtrlGuard(bool on = true) : active(on) {
        if (active) SetConsoleCtrlHandler(ignore_ctrl, TRUE);
    }
    ~CtrlGuard() {
        if (active) SetConsoleCtrlHandler(ignore_ctrl, FALSE);
    }
    CtrlGuard(const CtrlGuard&) = delete;
    CtrlGuard& operator=(const CtrlGuard&) = delete;
};

struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE v) : h(v) {}
    ~Handle() { close(); }
    void close() {
        if (h != nullptr && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = nullptr;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

std::string last_error_message(DWORD err) {
    LPWSTR buf = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::string msg = n != 0 ? util::to_utf8(std::wstring_view(buf, n)) : std::string{};
    if (buf != nullptr) LocalFree(buf);
    msg = std::string(util::trim(msg));
    if (msg.empty()) msg = std::format("Win32 error {}", err);
    return msg;
}

// An inheritable anonymous pipe; the parent's end is made non-inheritable.
struct Pipe {
    Handle read;
    Handle write;
};

std::expected<void, std::string> make_pipe(Pipe& p, bool parent_reads) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&p.read.h, &p.write.h, &sa, 0)) {
        return util::failf("cannot create pipe: {}", last_error_message(GetLastError()));
    }
    SetHandleInformation(parent_reads ? p.read.h : p.write.h, HANDLE_FLAG_INHERIT, 0);
    return {};
}

std::expected<Handle*, std::string> open_null(Handle& h) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    h.h = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                      OPEN_EXISTING, 0, nullptr);
    if (h.h == INVALID_HANDLE_VALUE) {
        return util::failf("cannot open NUL: {}", last_error_message(GetLastError()));
    }
    return &h;
}

// A running child inside its own Job object: terminating the job ends the child and
// everything it spawned; closing the job handle (kill-on-close) does the same if grab dies.
struct Child {
    Handle job;
    Handle process;
    Handle thread;
};

struct StdHandles {
    HANDLE in = nullptr;
    HANDLE out = nullptr;
    HANDLE err = nullptr;
    // True when all three are handles we created: then only they are inherited, which
    // keeps concurrently started children (GUI search + download) from holding each
    // other's pipe ends open.
    bool exclusive = false;
};

std::expected<void, std::string> launch(std::span<const std::string> argv, const StdHandles& std_h,
                                        bool no_window, Child& child) {
    if (argv.empty()) return util::fail("empty command");

    child.job.h = CreateJobObjectW(nullptr, nullptr);
    if (child.job.h == nullptr) {
        return util::failf("cannot create job object: {}", last_error_message(GetLastError()));
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(child.job.h, JobObjectExtendedLimitInformation, &limits,
                            sizeof(limits));

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = std_h.in;
    si.StartupInfo.hStdOutput = std_h.out;
    si.StartupInfo.hStdError = std_h.err;

    std::vector<char> attr_storage;
    HANDLE inherit_list[3] = {std_h.in, std_h.out, std_h.err};
    DWORD flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
    if (no_window) flags |= CREATE_NO_WINDOW;
    if (std_h.exclusive) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        attr_storage.resize(size);
        si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_storage.data());
        if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size)) {
            return util::failf("cannot set up handle inheritance: {}",
                               last_error_message(GetLastError()));
        }
        // NUL may be passed for two of the three streams; list each handle once.
        std::size_t count = 1;
        if (std_h.out != std_h.in) inherit_list[count++] = std_h.out;
        if (std_h.err != std_h.in && std_h.err != std_h.out) inherit_list[count++] = std_h.err;
        if (!UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       inherit_list, count * sizeof(HANDLE), nullptr, nullptr)) {
            DeleteProcThreadAttributeList(si.lpAttributeList);
            return util::failf("cannot set up handle inheritance: {}",
                               last_error_message(GetLastError()));
        }
        flags |= EXTENDED_STARTUPINFO_PRESENT;
    }

    std::wstring cmdline = util::to_wide(quote::windows_cmdline(argv));
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE, flags, nullptr,
                                   nullptr, &si.StartupInfo, &pi);
    const DWORD create_error = GetLastError();
    if (si.lpAttributeList != nullptr) DeleteProcThreadAttributeList(si.lpAttributeList);
    if (!ok) {
        return util::failf("cannot start '{}': {}", argv[0], last_error_message(create_error));
    }
    child.process.h = pi.hProcess;
    child.thread.h = pi.hThread;
    AssignProcessToJobObject(child.job.h, pi.hProcess); // before it can start grandchildren
    ResumeThread(pi.hThread);
    return {};
}

int wait_exit(Child& child) {
    WaitForSingleObject(child.process.h, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(child.process.h, &code);
    if (code == 0xC000013AU) return exit_stopped; // STATUS_CONTROL_C_EXIT
    return static_cast<int>(code);
}

// Reads a pipe to EOF, handing chunks to `sink`.
template <class Sink>
void drain(HANDLE pipe, Sink&& sink) {
    char buf[8192];
    DWORD n = 0;
    while (ReadFile(pipe, buf, sizeof(buf), &n, nullptr) && n > 0) sink(std::string_view(buf, n));
}

} // namespace

std::expected<CaptureResult, std::string> run_capture(std::span<const std::string> argv,
                                                      const RunOptions& opts) {
    Pipe out;
    if (auto r = make_pipe(out, true); !r) return util::fail(r.error());
    Pipe err;
    Handle null_in;

    StdHandles std_h;
    std_h.out = out.write.h;
    if (opts.detached) {
        if (auto r = make_pipe(err, true); !r) return util::fail(r.error());
        if (auto r = open_null(null_in); !r) return util::fail(r.error());
        std_h.in = null_in.h;
        std_h.err = err.write.h;
        std_h.exclusive = true;
    } else {
        std_h.in = GetStdHandle(STD_INPUT_HANDLE);
        std_h.err = GetStdHandle(STD_ERROR_HANDLE);
    }

    CtrlGuard guard(!opts.detached);
    Child child;
    if (auto r = launch(argv, std_h, opts.detached, child); !r) return util::fail(r.error());
    out.write.close(); // only the child holds the write ends now
    err.write.close();
    std::stop_callback on_stop(opts.stop, [&] { TerminateJobObject(child.job.h, exit_stopped); });

    CaptureResult result;
    std::jthread err_reader;
    if (opts.detached) {
        err_reader = std::jthread([&] { drain(err.read.h, [&](std::string_view s) { result.err += s; }); });
    }
    drain(out.read.h, [&](std::string_view s) { result.out += s; });
    if (err_reader.joinable()) err_reader.join();
    result.exit_code = wait_exit(child); // a stopped job exits with exit_stopped
    return result;
}

std::expected<int, std::string> run_streaming(std::span<const std::string> argv,
                                              const std::function<void(std::string_view)>& on_line,
                                              const RunOptions& opts) {
    Pipe err;
    if (auto r = make_pipe(err, true); !r) return util::fail(r.error());
    Handle null_io;
    if (auto r = open_null(null_io); !r) return util::fail(r.error());

    StdHandles std_h{null_io.h, null_io.h, err.write.h, true};
    Child child;
    if (auto r = launch(argv, std_h, opts.detached, child); !r) return util::fail(r.error());
    err.write.close();
    std::stop_callback on_stop(opts.stop, [&] { TerminateJobObject(child.job.h, exit_stopped); });

    LineSplitter lines(on_line);
    drain(err.read.h, [&](std::string_view s) { lines.feed(s); });
    lines.finish();
    return wait_exit(child); // a stopped job exits with exit_stopped
}

std::expected<int, std::string> run_inherit(std::span<const std::string> argv) {
    const StdHandles std_h{GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE),
                           GetStdHandle(STD_ERROR_HANDLE), false};
    CtrlGuard guard;
    Child child;
    if (auto r = launch(argv, std_h, false, child); !r) return util::fail(r.error());
    return wait_exit(child);
}

#else // POSIX

namespace {

struct SigintGuard {
    bool active;
    void (*previous)(int) = SIG_DFL;
    explicit SigintGuard(bool on = true) : active(on) {
        if (active) previous = std::signal(SIGINT, SIG_IGN);
    }
    ~SigintGuard() {
        if (active) std::signal(SIGINT, previous);
    }
    SigintGuard(const SigintGuard&) = delete;
    SigintGuard& operator=(const SigintGuard&) = delete;
};

int decode_status(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return status;
}

// Spawns in a new process group so a stop can signal the child and its descendants.
std::expected<pid_t, std::string> spawn(std::span<const std::string> argv,
                                        posix_spawn_file_actions_t* actions, bool own_group) {
    if (argv.empty()) return util::fail("empty command");
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    if (own_group) {
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&attr, 0);
    }
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, cargv[0], actions, &attr, cargv.data(), environ);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) return util::failf("cannot start '{}': {}", argv[0], std::strerror(rc));
    return pid;
}

int wait_pid(pid_t pid) {
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return decode_status(status);
}

template <class Sink>
void drain(int fd, Sink&& sink) {
    char buf[8192];
    for (;;) {
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            sink(std::string_view(buf, static_cast<std::size_t>(n)));
        } else if (n == 0 || errno != EINTR) {
            break;
        }
    }
}

} // namespace

std::expected<CaptureResult, std::string> run_capture(std::span<const std::string> argv,
                                                      const RunOptions& opts) {
    int out[2];
    int err[2] = {-1, -1};
    if (pipe(out) != 0) return util::failf("cannot create pipe: {}", std::strerror(errno));
    if (opts.detached && pipe(err) != 0) {
        close(out[0]);
        close(out[1]);
        return util::failf("cannot create pipe: {}", std::strerror(errno));
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, out[0]);
    posix_spawn_file_actions_addclose(&actions, out[1]);
    if (opts.detached) {
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_adddup2(&actions, err[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, err[0]);
        posix_spawn_file_actions_addclose(&actions, err[1]);
    }

    SigintGuard guard(!opts.detached);
    auto pid = spawn(argv, &actions, true);
    posix_spawn_file_actions_destroy(&actions);
    close(out[1]);
    if (opts.detached) close(err[1]);
    if (!pid) {
        close(out[0]);
        if (opts.detached) close(err[0]);
        return util::fail(pid.error());
    }
    std::stop_callback on_stop(opts.stop, [&] { kill(-*pid, SIGTERM); });

    CaptureResult result;
    std::jthread err_reader;
    if (opts.detached) {
        err_reader = std::jthread([&] { drain(err[0], [&](std::string_view s) { result.err += s; }); });
    }
    drain(out[0], [&](std::string_view s) { result.out += s; });
    if (err_reader.joinable()) err_reader.join();
    close(out[0]);
    if (opts.detached) close(err[0]);
    const int code = wait_pid(*pid);
    result.exit_code = opts.stop.stop_requested() ? exit_stopped : code;
    return result;
}

std::expected<int, std::string> run_streaming(std::span<const std::string> argv,
                                              const std::function<void(std::string_view)>& on_line,
                                              const RunOptions& opts) {
    int err[2];
    if (pipe(err) != 0) return util::failf("cannot create pipe: {}", std::strerror(errno));

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, err[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, err[0]);
    posix_spawn_file_actions_addclose(&actions, err[1]);

    auto pid = spawn(argv, &actions, true);
    posix_spawn_file_actions_destroy(&actions);
    close(err[1]);
    if (!pid) {
        close(err[0]);
        return util::fail(pid.error());
    }
    std::stop_callback on_stop(opts.stop, [&] { kill(-*pid, SIGTERM); });

    LineSplitter lines(on_line);
    drain(err[0], [&](std::string_view s) { lines.feed(s); });
    lines.finish();
    close(err[0]);
    const int code = wait_pid(*pid);
    return opts.stop.stop_requested() ? exit_stopped : code;
}

std::expected<int, std::string> run_inherit(std::span<const std::string> argv) {
    SigintGuard guard;
    auto pid = spawn(argv, nullptr, false);
    if (!pid) return util::fail(pid.error());
    return wait_pid(*pid);
}

#endif

} // namespace grab::proc
