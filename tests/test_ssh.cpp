#include <doctest/doctest.h>

#include "conn_budget.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_pool.hpp"
#include "util.hpp"

#include <libssh2.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace grab;

TEST_CASE("libssh2 is built in, on OpenSSL") {
    REQUIRE(libssh2_init(0) == 0);
    CHECK(std::string_view(libssh2_version(0)).starts_with("1.11"));
    CHECK(libssh2_crypto_engine() == libssh2_openssl);
    libssh2_exit();
}

TEST_CASE("ssh: host key algorithms come from the keys pinned for that host and port") {
    const std::string known = "# grab\n"
                              "[box.example]:23 ssh-ed25519 AAAAC3Nza\n"
                              "[box.example]:23 ecdsa-sha2-nistp521 AAAAE2Vj\r\n"
                              "[box.example]:23 ssh-rsa AAAAB3Nz\n"
                              "[other.example]:23 ssh-ed25519 AAAAC3Nzb\n"
                              "plain.example,10.0.0.5 ecdsa-sha2-nistp256 AAAAE2Vk\n"
                              "[box.example]:2222 ssh-ed25519 AAAAC3Nzc\n";
    CHECK(ssh::pinned_algorithms(known, "box.example", 23) == "ssh-ed25519,ecdsa-sha2-nistp521,rsa-sha2-512,rsa-sha2-256");
    CHECK(ssh::pinned_algorithms(known, "plain.example", 22) == "ecdsa-sha2-nistp256");
    CHECK(ssh::pinned_algorithms(known, "10.0.0.5", 22) == "ecdsa-sha2-nistp256");
    CHECK(ssh::pinned_algorithms(known, "box.example", 22).empty()); // a port 22 key only as "host"
    CHECK(ssh::pinned_algorithms(known, "nowhere.example", 23).empty());
}

TEST_CASE("ssh: the offered algorithms leave out the weak ones") {
    for (std::string_view weak : {"sha1", "cbc", "arcfour", "md5", "3des", "blowfish", "group1-"}) {
        CHECK(ssh::kex_algorithms.find(weak) == std::string_view::npos);
        CHECK(ssh::cipher_algorithms.find(weak) == std::string_view::npos);
        CHECK(ssh::mac_algorithms.find(weak) == std::string_view::npos);
    }
}

namespace {

// Live tests run against a real server only when GRAB_TEST_SSH=user@host:port and
// GRAB_TEST_KNOWN_HOSTS name one (logging in through ssh-agent). CI sets neither.
std::optional<ssh::Target> live_target() {
    auto where = util::getenv_utf8("GRAB_TEST_SSH");
    auto known = util::getenv_utf8("GRAB_TEST_KNOWN_HOSTS");
    if (!where || !known) return std::nullopt;
    const std::string& w = *where;
    ssh::Target t;
    t.user = w.substr(0, w.find('@'));
    t.host = w.substr(w.find('@') + 1, w.rfind(':') - w.find('@') - 1);
    t.port = std::stoi(w.substr(w.rfind(':') + 1));
    t.known_hosts = util::path_from_utf8(*known);
    t.agent = true;
    return t;
}

// Whether `target` is turned away as fatal with a known_hosts holding only `line` for it.
bool refused_with(const ssh::Target& target, const std::string& line) {
    ssh::Target t = target;
    t.known_hosts = std::filesystem::temp_directory_path() / "grab-test-known_hosts";
    std::ofstream(t.known_hosts) << std::format("[{}]:{} {}\n", t.host, t.port, line);
    bool fatal = false;
    try {
        ssh::Connection c(t);
    } catch (const ssh::Error& e) {
        fatal = e.kind == budget::Failure::fatal;
        MESSAGE(std::string(e.what()));
    }
    std::filesystem::remove(t.known_hosts);
    return fatal;
}

} // namespace

TEST_CASE("ssh: live login, SFTP and a command (GRAB_TEST_SSH)") {
    auto t = live_target();
    if (!t) {
        MESSAGE("skipped: GRAB_TEST_SSH / GRAB_TEST_KNOWN_HOSTS not set");
        return;
    }
    ssh::Connection c(*t);
    MESSAGE("algorithms: " << c.algorithms() << ", login " << c.login_time().count() << " ms");
    auto out = c.run("echo out; echo err >&2; exit 3");
    CHECK(out.out == "out\n");
    CHECK(out.err == "err\n");
    CHECK(out.status == 3);
    CHECK_FALSE(out.cut);
    auto big = c.run("head -c 300000 /dev/zero", 1000);
    CHECK(big.out.size() == 1000);
    CHECK(big.cut);
    CHECK(big.status == 0);

    auto channel = c.open_sftp();
    sftp::Client s(*channel);
    s.start();
    auto home = sftp::realpath(s, ".");
    REQUIRE(home);
    auto entries = sftp::list(s, *home);
    REQUIRE(entries);
    CHECK_FALSE(entries->empty());
    CHECK(c.received() > 0);
}

TEST_CASE("ssh: live pool, 4 threads twice over, 4 logins (GRAB_TEST_SSH)") {
    auto t = live_target();
    if (!t) {
        MESSAGE("skipped: GRAB_TEST_SSH / GRAB_TEST_KNOWN_HOSTS not set");
        return;
    }
    std::atomic<int> logins = 0;
    ssh::Pool::Options o;
    o.limit = 4;
    ssh::Pool pool([&] { ++logins; return ssh::open_session(*t); }, o);
    for (int round = 0; round < 2; ++round) {
        std::atomic<int> ok = 0;
        {
            std::vector<std::jthread> threads;
            for (int i = 0; i < 4; ++i) {
                threads.emplace_back([&] {
                    auto lease = pool.acquire();
                    if (lease && sftp::realpath((*lease)->sftp(), ".")) ++ok;
                    std::this_thread::sleep_for(std::chrono::milliseconds(200)); // all four held at once
                });
            }
        }
        CHECK(ok == 4);
    }
    CHECK(logins == 4);
    CHECK(pool.idle() == 4);
}

TEST_CASE("ssh: live, a changed or missing host key ends it before the login (GRAB_TEST_SSH)") {
    auto t = live_target();
    if (!t) {
        MESSAGE("skipped: GRAB_TEST_SSH / GRAB_TEST_KNOWN_HOSTS not set");
        return;
    }
    // Right type, other bytes: the key changed.
    CHECK(refused_with(*t, "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIEJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJC"));
    // Nothing for this host at all.
    ssh::Target none = *t;
    none.known_hosts = std::filesystem::temp_directory_path() / "grab-test-no-known_hosts";
    CHECK_THROWS_AS(ssh::Connection{none}, ssh::Error);
}
