// grab's own client logging in with keys, against a real SSH server on this PC: the test rclone's
// `serve sftp` (skipped without build\<preset>\test-tools\rclone.exe or Windows' ssh-keygen),
// with throwaway keys made for the run and a made-up login. Nothing leaves 127.0.0.1.

#include "passwords.hpp"
#include "process.hpp"
#include "quote.hpp"
#include "server_conf.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_pool.hpp"
#include "ssh_target.hpp"
#include "test_tools.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h> // IWYU pragma: keep (the socket API)

#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)

using namespace grab;
namespace fs = std::filesystem;

namespace {

std::optional<fs::path> ssh_keygen() {
    const fs::path keygen = R"(C:\Windows\System32\OpenSSH\ssh-keygen.exe)";
    std::error_code ec;
    if (!fs::is_regular_file(keygen, ec)) return std::nullopt;
    return keygen;
}

// A port on 127.0.0.1 nothing listens on now.
int free_port() {
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int length = sizeof(at);
    int port = 0;
    if (::bind(s, reinterpret_cast<const sockaddr*>(&at), sizeof(at)) == 0 &&
        ::getsockname(s, reinterpret_cast<sockaddr*>(&at), &length) == 0) {
        port = ntohs(at.sin_port);
    }
    closesocket(s);
    return port;
}

bool answers(int port) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    at.sin_port = htons(static_cast<u_short>(port));
    const bool ok = ::connect(s, reinterpret_cast<const sockaddr*>(&at), sizeof(at)) == 0;
    closesocket(s);
    return ok;
}

// `rclone serve sftp`, ended with the object.
class Server {
public:
    Server(const fs::path& rclone, const std::vector<std::string>& args, const fs::path& scratch) {
        std::vector<std::string> argv{util::path_to_utf8(rclone)};
        argv.insert(argv.end(), args.begin(), args.end());
        std::wstring cmdline = util::to_wide(quote::windows_cmdline(argv));
        // grab's environment without RCLONE_*, and rclone's cache in the scratch folder.
        std::wstring env = proc::environment_without_rclone();
        while (!env.empty() && env.back() == L'\0') env.pop_back();
        if (!env.empty()) env.push_back(L'\0');
        env += util::to_wide("RCLONE_CACHE_DIR=" + util::path_to_utf8(scratch / "cache"));
        env.push_back(L'\0');
        env.push_back(L'\0');
        SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        log_ = CreateFileW((scratch / "serve.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = log_;
        si.hStdError = log_;
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(rclone.c_str(), cmdline.data(), nullptr, nullptr, TRUE,
                           CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, env.data(), nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            process_ = pi.hProcess;
        }
    }
    ~Server() {
        if (process_ != nullptr) {
            TerminateProcess(process_, 1);
            WaitForSingleObject(process_, 10000);
            CloseHandle(process_);
        }
        if (log_ != INVALID_HANDLE_VALUE) CloseHandle(log_);
    }
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    [[nodiscard]] bool started() const { return process_ != nullptr; }

private:
    HANDLE process_ = nullptr;
    HANDLE log_ = INVALID_HANDLE_VALUE;
};

// A key made by ssh-keygen: `extra` (a type, -m PEM) and its passphrase ("" for none).
void make_key(const fs::path& keygen, const fs::path& file, const std::vector<std::string>& extra,
              const std::string& passphrase) {
    std::vector<std::string> argv{util::path_to_utf8(keygen), "-q"};
    argv.insert(argv.end(), extra.begin(), extra.end());
    argv.insert(argv.end(), {"-N", passphrase, "-C", "grab-test", "-f", util::path_to_utf8(file)});
    proc::RunOptions quiet;
    quiet.detached = true;
    const auto ran = proc::run_capture(argv, quiet);
    REQUIRE_MESSAGE(ran, ran.error_or(""));
    REQUIRE_MESSAGE(ran->exit_code == 0, ran->err);
}

// A key file's text as an rclone.conf holds key_pem: one line, its line breaks written \n.
std::string as_key_pem(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == '\r') continue;
        if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

} // namespace

TEST_CASE("ssh: a key kept in grab's store logs in from memory, with and without a passphrase (rclone serve sftp)") {
    const auto rclone = test::test_rclone();
    const auto keygen = ssh_keygen();
    if (!rclone || !keygen) {
        MESSAGE(test::no_test_rclone << ", or no ssh-keygen.exe; skipped");
        return;
    }
    const auto dir = fs::temp_directory_path() / "grab-test-ssh-keys";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "served");
    REQUIRE(util::write_file_atomic(dir / "served" / "hello.txt", "hello\n"));

    make_key(*keygen, dir / "host", {"-t", "ed25519"}, "");
    struct Key {
        std::string name;
        std::vector<std::string> type;
        std::string passphrase;
    };
    const std::vector<Key> keys{{.name = "ed25519", .type = {"-t", "ed25519"}, .passphrase = "fake-phrase-1"},
                                {.name = "rsa-pem", .type = {"-t", "rsa", "-b", "2048", "-m", "PEM"}, .passphrase = ""},
                                {.name = "ecdsa-pem", .type = {"-t", "ecdsa", "-m", "PEM"}, .passphrase = "fake-phrase-3"}};
    std::string authorized;
    for (const auto& k : keys) {
        make_key(*keygen, dir / k.name, k.type, k.passphrase);
        authorized += util::read_file(dir / (k.name + ".pub")).value_or("");
    }
    REQUIRE(util::write_file_atomic(dir / "authorized_keys", authorized));

    const int port = free_port();
    REQUIRE(port > 0);
    // Stopped before the folder goes (it holds its log open).
    std::optional<Server> server;
    server.emplace(*rclone,
                   std::vector<std::string>{"serve", "sftp", util::path_to_utf8(dir / "served"), "--addr",
                                            std::format("127.0.0.1:{}", port), "--key", util::path_to_utf8(dir / "host"),
                                            "--authorized-keys", util::path_to_utf8(dir / "authorized_keys"), "--config",
                                            "NUL"},
                   dir);
    REQUIRE(server->started());
    for (int i = 0; i < 150 && !answers(port); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE_MESSAGE(answers(port), util::read_file(dir / "serve.log").value_or(""));
    // Its host key pinned, as Trust would.
    const auto host_pub = util::split(util::trim(util::read_file(dir / "host.pub").value_or("")), ' ');
    REQUIRE(host_pub.size() >= 2);
    const auto known_hosts = dir / "known_hosts";
    REQUIRE(util::write_file_atomic(known_hosts, std::format("[127.0.0.1]:{} {} {}\n", port, host_pub[0], host_pub[1])));

    server_conf::Connection c;
    c.host = "127.0.0.1";
    c.port = port;
    c.user = "tester";
    c.auth = server_conf::Auth::stored_key;
    const passwords::Binding login{.user = c.user, .host = c.host, .port = c.port};
    auto login_with = [&](const Key& k, const std::string& passphrase) -> std::expected<std::size_t, std::string> {
        passwords::Store store;
        store["Fake"]["key_pem"] =
            passwords::Secret{.binding = login, .value = as_key_pem(util::read_file(dir / k.name).value_or(""))};
        if (!passphrase.empty()) store["Fake"]["passphrase"] = passwords::Secret{.binding = login, .value = passphrase};
        auto target = ssh::target_for("Fake", c, store, known_hosts);
        if (!target) return util::fail(target.error());
        try {
            const auto session = ssh::open_session(*target);
            const auto listed = sftp::list(session->sftp(), ".");
            if (!listed) return util::fail(sftp::describe(listed.error()));
            return listed->size();
        } catch (const ssh::Error& e) {
            return util::fail(e.what());
        } catch (const sftp::Broken& e) {
            return util::fail(e.what());
        }
    };
    for (const auto& k : keys) {
        const auto listed = login_with(k, k.passphrase);
        CHECK_MESSAGE(listed == std::size_t{1}, k.name, ": ", listed.error_or("listed"));
    }
    // A wrong passphrase: refused, said as the key kept in grab's store, the phrase not shown.
    const auto wrong = login_with(keys[0], "fake-wrong");
    REQUIRE_FALSE(wrong);
    CHECK(wrong.error().find("refused the login: it refused the key kept in grab's store") != std::string::npos);
    CHECK(wrong.error().find("fake-") == std::string::npos);
    server.reset();
    fs::remove_all(dir, ec);
    CHECK_FALSE(fs::exists(dir));
}
#endif
