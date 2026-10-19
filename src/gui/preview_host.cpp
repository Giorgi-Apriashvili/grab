#include "preview_host.hpp"

#include "preview.hpp"
#include "quote.hpp"
#include "server_ops.hpp"
#include "util.hpp"

#include <shlwapi.h> // AssocQueryStringW
#include <winhttp.h>

#include <charconv>
#include <chrono>
#include <format>
#include <random>
#include <system_error>
#include <vector>

namespace grab::gui {

namespace {

// 128 random bits as hex: the servers' path, so other programs on this PC can't guess it.
std::string random_token() {
    std::random_device rd;
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 4; ++i) {
        auto v = rd();
        for (int k = 0; k < 8; ++k) {
            out += hex[v & 0xF];
            v >>= 4;
        }
    }
    return out;
}

// A file another process is still writing.
std::string read_shared(const std::filesystem::path& p) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::string out;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n > 0) out.append(buf, n);
    CloseHandle(h);
    return out;
}

void end(HANDLE process, const std::filesystem::path& log) {
    TerminateProcess(process, 0);
    WaitForSingleObject(process, 2000);
    CloseHandle(process);
    std::error_code ec;
    std::filesystem::remove(log, ec);
}

struct Internet {
    HINTERNET h = nullptr;
    Internet(HINTERNET handle) : h(handle) {}
    ~Internet() {
        if (h != nullptr) WinHttpCloseHandle(h);
    }
    Internet(const Internet&) = delete;
    Internet& operator=(const Internet&) = delete;
};

std::wstring header(HINTERNET request, DWORD which) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, which, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
    if (size == 0) return {};
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, which, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &size, WINHTTP_NO_HEADER_INDEX)) return {};
    value.resize(size / sizeof(wchar_t));
    return value;
}

std::optional<std::uint64_t> number(std::wstring_view text) {
    const auto s = util::to_utf8(text);
    std::uint64_t n = 0;
    const auto [end, err] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (err != std::errc{} || end == s.data()) return std::nullopt;
    return n;
}

} // namespace

PreviewServers::PreviewServers() {
    job_ = CreateJobObjectW(nullptr, nullptr);
    if (job_ != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &info, sizeof(info));
    }
}

PreviewServers::~PreviewServers() {
    std::lock_guard lock(mutex_);
    for (auto& [key, s] : servers_) end(s.process, s.log);
    servers_.clear();
    if (job_ != nullptr) CloseHandle(job_);
}

bool PreviewServers::running(const std::string& remote, const std::string& root) {
    std::lock_guard lock(mutex_);
    const auto it = servers_.find(std::pair{remote, root});
    return it != servers_.end() && WaitForSingleObject(it->second.process, 0) == WAIT_TIMEOUT;
}

std::expected<std::string, std::string> PreviewServers::base(const engine::Context& ctx, const std::string& remote,
                                                            const std::string& root) {
    std::lock_guard lock(mutex_); // one start at a time; another caller waits for it and reuses it
    const auto key = std::pair{remote, root};
    if (const auto it = servers_.find(key); it != servers_.end()) {
        if (WaitForSingleObject(it->second.process, 0) == WAIT_TIMEOUT) return it->second.base;
        end(it->second.process, it->second.log); // it ended: a new one
        servers_.erase(it);
    }

    const auto token = random_token();
    const auto argv = preview::serve_argv(ctx, root, token);
    const auto log = std::filesystem::temp_directory_path() /
                     std::format("grab-preview-{}-{}.log", GetCurrentProcessId(), token.substr(0, 8));
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    if (out == INVALID_HANDLE_VALUE || in == INVALID_HANDLE_VALUE) {
        if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
        if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
        return util::fail("cannot create the preview server's log");
    }
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in;
    si.StartupInfo.hStdOutput = out;
    si.StartupInfo.hStdError = out;
    // Only these two are inherited.
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> storage(size);
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    HANDLE inherit[2] = {in, out};
    const bool listed = InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size) &&
                        UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                                  sizeof(inherit), nullptr, nullptr);
    std::wstring cmdline = util::to_wide(quote::windows_cmdline(argv));
    PROCESS_INFORMATION pi{};
    const BOOL ok = listed && CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                                             CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED |
                                                 EXTENDED_STARTUPINFO_PRESENT,
                                             nullptr, nullptr, &si.StartupInfo, &pi);
    const DWORD error = GetLastError();
    if (listed) DeleteProcThreadAttributeList(si.lpAttributeList);
    CloseHandle(in);
    CloseHandle(out);
    if (!ok) return util::failf("cannot start rclone: {}", std::system_category().message(static_cast<int>(error)));
    if (job_ != nullptr) AssignProcessToJobObject(job_, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    // Its address, once its log says it listens.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (true) {
        const auto text = read_shared(log);
        if (const auto port = preview::served_port(text)) {
            auto address = std::format("http://127.0.0.1:{}/{}/", *port, token);
            servers_[key] = Server{pi.hProcess, address, log};
            return address;
        }
        if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) {
            end(pi.hProcess, log);
            const auto why = server_ops::clean_rclone_error(text);
            return util::fail(why.empty() ? std::string("the preview server ended") : why);
        }
        if (std::chrono::steady_clock::now() > deadline) {
            end(pi.hProcess, log);
            return util::fail("the preview server didn't start within 20 s");
        }
    }
}

std::expected<Fetched, std::string> http_get(const std::string& url, std::size_t max,
                                            const std::function<void(std::uint64_t, std::uint64_t)>& on_progress) {
    constexpr std::string_view local = "http://127.0.0.1:";
    if (!url.starts_with(local) || max == 0) return util::fail("not a preview address");
    const std::string_view rest = std::string_view(url).substr(local.size());
    const auto slash = rest.find('/');
    unsigned port = 0;
    const auto [end_port, err] = std::from_chars(rest.data(), rest.data() + std::min(slash, rest.size()), port);
    if (err != std::errc{} || slash == std::string_view::npos || port == 0 || port > 65535) {
        return util::fail("not a preview address");
    }
    const std::wstring path = util::to_wide(rest.substr(slash));

    const Internet session(WinHttpOpen(L"grab", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                       WINHTTP_NO_PROXY_BYPASS, 0));
    if (session.h == nullptr) return util::fail("cannot use WinHTTP");
    WinHttpSetTimeouts(session.h, 5000, 5000, 30000, 60000);
    const Internet connection(WinHttpConnect(session.h, L"127.0.0.1", static_cast<INTERNET_PORT>(port), 0));
    const Internet request(connection.h == nullptr ? nullptr
                                                   : WinHttpOpenRequest(connection.h, L"GET", path.c_str(), nullptr,
                                                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                                        WINHTTP_FLAG_ESCAPE_DISABLE));
    if (request.h == nullptr) return util::fail("cannot reach the preview server");
    const std::wstring range = std::format(L"Range: bytes=0-{}", max - 1);
    if (!WinHttpSendRequest(request.h, range.c_str(), static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
        return util::failf("the preview server didn't answer: {}", std::system_category().message(static_cast<int>(GetLastError())));
    }
    Fetched got;
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    got.status = static_cast<int>(status);
    // "bytes 0-1048575/52428800" for a part; the whole file's length otherwise.
    if (const auto content_range = header(request.h, WINHTTP_QUERY_CONTENT_RANGE); !content_range.empty()) {
        if (const auto slash_at = content_range.rfind(L'/'); slash_at != std::wstring::npos) {
            got.total = number(std::wstring_view(content_range).substr(slash_at + 1));
        }
    } else if (status == 200) {
        got.total = number(header(request.h, WINHTTP_QUERY_CONTENT_LENGTH));
    }
    while (got.body.size() < max) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available) || available == 0) break;
        const std::size_t want = std::min<std::size_t>(available, max - got.body.size());
        const std::size_t at = got.body.size();
        got.body.resize(at + want);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, got.body.data() + at, static_cast<DWORD>(want), &read)) {
            return util::fail("the preview server stopped answering");
        }
        got.body.resize(at + read);
        if (read == 0) break;
        if (on_progress) on_progress(got.body.size(), std::min<std::uint64_t>(got.total.value_or(max), max));
    }
    return got;
}

std::expected<std::string, std::string> open_with_player(std::string_view ext, const std::string& url) {
    std::wstring command;
#ifndef NDEBUG
    // Tests: a stand-in for the player, so none opens on the screen.
    if (const auto test = util::getenv_utf8("GRAB_TEST_PLAYER"); test && !test->empty()) {
        command = L"\"" + util::to_wide(*test) + L"\" \"%1\"";
    }
#endif
    const std::wstring dot = L"." + util::to_wide(ext);
    if (command.empty()) {
        // What Windows runs to open such a file: "C:\...\mpc-hc64.exe" "%1", say.
        DWORD length = 0;
        AssocQueryStringW(ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_COMMAND, dot.c_str(), L"open", nullptr, &length);
        if (length > 1) {
            command.resize(length);
            if (AssocQueryStringW(ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_COMMAND, dot.c_str(), L"open", command.data(),
                                  &length) == S_OK) {
                command.resize(length > 0 ? length - 1 : 0);
            } else {
                command.clear();
            }
        }
    }
    // A Store app opens files through a handler, not a command line that takes an address.
    const bool store_app = command.find(L"\\WindowsApps\\") != std::wstring::npos ||
                           command.find(L"DelegateExecute") != std::wstring::npos;
    if (command.empty() || store_app) {
        return util::failf("Windows opens .{} files with {}, which can't play a stream. Make a desktop player such as "
                           "VLC or MPC-HC the default for .{} files, or download it.",
                           ext, command.empty() ? "no program" : "a Store app", ext);
    }
    const std::wstring wide_url = util::to_wide(url);
    std::wstring line = command;
    bool placed = false;
    for (const std::wstring_view mark : {L"%1", L"%L", L"%l"}) {
        for (auto at = line.find(mark); at != std::wstring::npos; at = line.find(mark, at + wide_url.size())) {
            line.replace(at, mark.size(), wide_url);
            placed = true;
        }
    }
    if (!placed) line += L" \"" + wide_url + L"\"";
    for (auto at = line.find(L"%*"); at != std::wstring::npos; at = line.find(L"%*")) line.erase(at, 2);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_DEFAULT_ERROR_MODE, nullptr, nullptr, &si, &pi)) {
        return util::failf("cannot start the player: {}", std::system_category().message(static_cast<int>(GetLastError())));
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    // Its name, for the window: the program part of the command.
    std::wstring program = command;
    if (program.starts_with(L'"')) program = program.substr(1, program.find(L'"', 1) - 1);
    else program = program.substr(0, program.find(L' '));
    return util::path_to_utf8(std::filesystem::path(program).filename());
}

} // namespace grab::gui
