#include "process.hpp"

#include "quote.hpp"
#include "util.hpp"

#include <format>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace grab::proc {

#ifdef _WIN32

namespace {

BOOL WINAPI ignore_ctrl(DWORD type) {
    return (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) ? TRUE : FALSE;
}

// While a child runs, grab itself ignores Ctrl+C; the child (rclone/ssh) receives it,
// cleans up, and grab reports the child's exit code.
struct CtrlGuard {
    CtrlGuard() { SetConsoleCtrlHandler(ignore_ctrl, TRUE); }
    ~CtrlGuard() { SetConsoleCtrlHandler(ignore_ctrl, FALSE); }
    CtrlGuard(const CtrlGuard&) = delete;
    CtrlGuard& operator=(const CtrlGuard&) = delete;
};

struct Handle {
    HANDLE h = nullptr;
    ~Handle() { close(); }
    void close() {
        if (h != nullptr && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = nullptr;
    }
    Handle() = default;
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

std::expected<PROCESS_INFORMATION, std::string> spawn(std::span<const std::string> argv,
                                                      STARTUPINFOW& si) {
    if (argv.empty()) return util::fail("empty command");
    std::wstring cmdline = util::to_wide(quote::windows_cmdline(argv));
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si,
                        &pi)) {
        return util::failf("cannot start '{}': {}", argv[0], last_error_message(GetLastError()));
    }
    return pi;
}

int wait_exit(PROCESS_INFORMATION& pi) {
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code == 0xC000013AU) return 130; // STATUS_CONTROL_C_EXIT, mirror the POSIX convention
    return static_cast<int>(code);
}

} // namespace

std::expected<CaptureResult, std::string> run_capture(std::span<const std::string> argv) {
    Handle read_end;
    Handle write_end;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&read_end.h, &write_end.h, &sa, 0)) {
        return util::failf("cannot create pipe: {}", last_error_message(GetLastError()));
    }
    SetHandleInformation(read_end.h, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = write_end.h;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    CtrlGuard guard;
    auto pi = spawn(argv, si);
    if (!pi) return util::fail(pi.error());
    write_end.close(); // only the child holds the write end now

    std::string out;
    char buf[8192];
    DWORD n = 0;
    while (ReadFile(read_end.h, buf, sizeof(buf), &n, nullptr) && n > 0) {
        out.append(buf, n);
    }
    const int code = wait_exit(*pi);
    return CaptureResult{code, std::move(out)};
}

std::expected<int, std::string> run_inherit(std::span<const std::string> argv) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    CtrlGuard guard;
    auto pi = spawn(argv, si);
    if (!pi) return util::fail(pi.error());
    return wait_exit(*pi);
}

#else // POSIX

namespace {

struct SigintGuard {
    void (*previous)(int);
    SigintGuard() : previous(std::signal(SIGINT, SIG_IGN)) {}
    ~SigintGuard() { std::signal(SIGINT, previous); }
    SigintGuard(const SigintGuard&) = delete;
    SigintGuard& operator=(const SigintGuard&) = delete;
};

int decode_status(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return status;
}

std::expected<pid_t, std::string> spawn(std::span<const std::string> argv,
                                        posix_spawn_file_actions_t* actions) {
    if (argv.empty()) return util::fail("empty command");
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, cargv[0], actions, nullptr, cargv.data(), environ);
    if (rc != 0) return util::failf("cannot start '{}': {}", argv[0], std::strerror(rc));
    return pid;
}

} // namespace

std::expected<CaptureResult, std::string> run_capture(std::span<const std::string> argv) {
    int fds[2];
    if (pipe(fds) != 0) return util::failf("cannot create pipe: {}", std::strerror(errno));

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);

    SigintGuard guard;
    auto pid = spawn(argv, &actions);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (!pid) {
        close(fds[0]);
        return util::fail(pid.error());
    }

    std::string out;
    char buf[8192];
    for (;;) {
        const ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n));
        } else if (n == 0 || errno != EINTR) {
            break;
        }
    }
    close(fds[0]);

    int status = 0;
    while (waitpid(*pid, &status, 0) < 0 && errno == EINTR) {
    }
    return CaptureResult{decode_status(status), std::move(out)};
}

std::expected<int, std::string> run_inherit(std::span<const std::string> argv) {
    SigintGuard guard;
    auto pid = spawn(argv, nullptr);
    if (!pid) return util::fail(pid.error());
    int status = 0;
    while (waitpid(*pid, &status, 0) < 0 && errno == EINTR) {
    }
    return decode_status(status);
}

#endif

} // namespace grab::proc
