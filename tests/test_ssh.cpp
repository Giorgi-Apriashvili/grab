#include <doctest/doctest.h>

#include "config.hpp"
#include "conn_budget.hpp"
#include "fake_command.hpp"
#include "secrets.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_pool.hpp"
#include "ssh_target.hpp"
#include "util.hpp"

#include <libssh2.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
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

TEST_CASE("ssh: a server's target from its rclone.conf section and its stored secrets") {
    RcloneRemote r;
    r.name = "Box";
    r.host = "box.example";
    r.user = "u1";
    r.port = 23;
    secrets::Store store;
    const std::filesystem::path grab_known = "C:/grab/known_hosts";

    SUBCASE("a password: revealed, grab's known_hosts by default") {
        store["Box"]["pass"] = "gLZBxgITy_HxlUNmOsLeEByAksnrlA"; // "potato", obscured by rclone
        auto t = ssh::target_for(r, store, grab_known);
        REQUIRE(t);
        CHECK(t->host == "box.example");
        CHECK(t->port == 23);
        CHECK(t->user == "u1");
        CHECK(t->password == "potato");
        CHECK_FALSE(t->agent);
        CHECK(t->known_hosts == grab_known);
    }
    SUBCASE("ssh-agent, the remote's own known_hosts") {
        r.key_use_agent = true;
        r.known_hosts_file = "D:/keys/known_hosts";
        auto t = ssh::target_for(r, store, grab_known);
        REQUIRE(t);
        CHECK(t->agent);
        CHECK(t->password.empty());
        CHECK(t->known_hosts == std::filesystem::path("D:/keys/known_hosts"));
    }
    SUBCASE("a key file and its passphrase") {
        r.key_file = "C:/keys/id_ed25519";
        store["Box"]["key_file_pass"] = "gLZBxgITy_HxlUNmOsLeEByAksnrlA";
        auto t = ssh::target_for(r, store, grab_known);
        REQUIRE(t);
        CHECK(t->key_file == std::filesystem::path("C:/keys/id_ed25519"));
        CHECK(t->key_passphrase == "potato");
    }
    SUBCASE("what it can't use is said, not guessed") {
        CHECK_FALSE(ssh::target_for(r, store, grab_known)); // no way to log in
        store["Box"]["key_pem"] = "x";
        CHECK_FALSE(ssh::target_for(r, store, grab_known)); // an inline key: not yet
        store["Box"].erase("key_pem");
        store["Box"]["pass"] = "not obscured!";
        CHECK_FALSE(ssh::target_for(r, store, grab_known));
        r.user.clear();
        store["Box"]["pass"] = "gLZBxgITy_HxlUNmOsLeEByAksnrlA";
        CHECK_FALSE(ssh::target_for(r, store, grab_known));
    }
}

TEST_CASE("ssh exec: stdout streams in pieces, stdin ends at once, stderr is kept up to its limit") {
    test::FakeCommand cmd;
    cmd.out = {"a\n", "bb\n", "ccc\n"};
    cmd.err = "warning\n";
    cmd.status = 1;
    cmd.start_waits = 2;
    std::vector<std::string> pieces;
    ssh::ExecOptions o;
    o.out = [&](std::string_view piece) {
        pieces.emplace_back(piece);
        return true;
    };
    o.err_limit = 4;
    auto r = ssh::run_command(cmd, "find .", o);
    REQUIRE(r);
    CHECK(cmd.command == "find .");
    CHECK(pieces == cmd.out);
    CHECK(r->out_bytes == 9);
    CHECK(r->err == "warn");
    CHECK(r->err_cut);
    CHECK(r->status == 1);
    CHECK(r->signal.empty());
    CHECK(cmd.log == std::vector<std::string>{"start", "eof", "close"});
    CHECK_FALSE(cmd.dropped);
}

TEST_CASE("ssh exec: a command reading its stdin ends, as EOF goes right after the exec request") {
    test::FakeCommand cmd; // cat, or the sftp-server of an internal-sftp account
    cmd.needs_eof = true;
    ssh::ExecOptions o;
    o.deadline = std::chrono::seconds(5); // a failure here instead of a hang
    auto r = ssh::run_command(cmd, "cat", o);
    REQUIRE(r);
    CHECK(r->status == 0);
    CHECK(r->out_bytes == 0);
    REQUIRE(cmd.log.size() >= 2);
    CHECK(cmd.log[0] == "start");
    CHECK(cmd.log[1] == "eof");
}

TEST_CASE("ssh exec: the end by a signal comes back without an exit status") {
    test::FakeCommand cmd;
    cmd.signal = "TERM";
    auto r = ssh::run_command(cmd, "sleep 60", {});
    REQUIRE(r);
    CHECK(r->status == -1);
    CHECK(r->signal == "TERM");
}

TEST_CASE("ssh exec: cancel sends TERM and drops the channel, looking at least once a second") {
    test::FakeCommand cmd;
    cmd.forever = true; // a silent sleep 60
    std::stop_source stop;
    ssh::ExecOptions o;
    o.stop = stop.get_token();
    std::jthread cancel([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        stop.request_stop();
    });
    auto started = std::chrono::steady_clock::now();
    auto r = ssh::run_command(cmd, "sleep 60", o);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
    REQUIRE_FALSE(r);
    CHECK(r.error().kind == ssh::ExecFailure::stopped);
    REQUIRE(cmd.log.size() >= 2);
    CHECK(cmd.log[cmd.log.size() - 2] == "TERM");
    CHECK(cmd.log.back() == "drop");
    CHECK(cmd.dropped);
    CHECK(cmd.longest_wait <= std::chrono::seconds(1)); // no wait longer than a second
    CHECK(cmd.longest_wait > std::chrono::milliseconds(500)); // (and the keepalive isn't due for 15 s)
}

TEST_CASE("ssh exec: endless output stops at the cap") {
    test::FakeCommand cmd;
    cmd.endless = true; // yes
    std::uint64_t got = 0;
    ssh::ExecOptions o;
    o.out = [&](std::string_view piece) {
        got += piece.size();
        return true;
    };
    SUBCASE("the byte cap") {
        o.out_limit = 100'000;
        auto r = ssh::run_command(cmd, "yes", o);
        REQUIRE_FALSE(r);
        CHECK(r.error().kind == ssh::ExecFailure::too_much);
        CHECK(got == 100'000); // every byte up to the cap, none past it
    }
    SUBCASE("the reader says stop (an entry cap)") {
        int pieces = 0;
        o.out = [&](std::string_view) { return ++pieces < 3; };
        auto r = ssh::run_command(cmd, "yes", o);
        REQUIRE_FALSE(r);
        CHECK(r.error().kind == ssh::ExecFailure::too_much);
        CHECK(pieces == 3);
    }
    CHECK(cmd.count("TERM") == 1);
    CHECK(cmd.log.back() == "drop");
}

TEST_CASE("ssh exec: past the deadline it's stopped, with what stderr said") {
    test::FakeCommand cmd;
    cmd.forever = true;
    cmd.err = "Welcome to the server\n"; // a banner, then nothing
    ssh::ExecOptions o;
    o.deadline = std::chrono::milliseconds(50);
    auto started = std::chrono::steady_clock::now();
    auto r = ssh::run_command(cmd, "env LC_ALL=C find -L . -maxdepth 0 -printf 'grab-find-ok\\n'", o);
    auto took = std::chrono::steady_clock::now() - started;
    REQUIRE_FALSE(r);
    CHECK(r.error().kind == ssh::ExecFailure::deadline);
    CHECK(r.error().err == "Welcome to the server\n");
    CHECK(took >= std::chrono::milliseconds(50));
    CHECK(took < std::chrono::milliseconds(500));
    CHECK(cmd.count("TERM") == 1);
    CHECK(cmd.log.back() == "drop");
}

TEST_CASE("ssh exec: a refused exec request is its own kind and leaves the connection as it was") {
    test::FakeCommand cmd;
    cmd.refuse = true;
    auto r = ssh::run_command(cmd, "find .", {});
    REQUIRE_FALSE(r);
    CHECK(r.error().kind == ssh::ExecFailure::refused);
    CHECK(cmd.log == std::vector<std::string>{"start", "close"}); // no EOF, TERM or drop
    CHECK_FALSE(cmd.dropped);
    CHECK(r.error().message == "the server refused to run the command");
}

TEST_CASE("ssh exec: a refused channel is refused too, with the server's reason") {
    test::FakeCommand cmd; // MaxSessions 1: the pooled session's SFTP channel is the one
    cmd.refuse_channel = "Channel open failure (connect failed)";
    auto r = ssh::run_command(cmd, "find .", {});
    REQUIRE_FALSE(r);
    CHECK(r.error().kind == ssh::ExecFailure::refused);
    CHECK(r.error().message == "the server refused to run the command: Channel open failure (connect failed)");
    CHECK(cmd.log == std::vector<std::string>{"start", "close"});
    CHECK_FALSE(cmd.dropped);
}

TEST_CASE("ssh exec: keepalives keep a silent command alive; a link that answers none is dead") {
    // Times well above Windows' 15.6 ms sleep granularity (FakeCommand::wait), so that a late
    // tick under load doesn't change the outcome.
    test::FakeCommand cmd;
    ssh::ExecOptions o;
    o.keepalive = std::chrono::milliseconds(10);
    o.dead_after = std::chrono::milliseconds(200);
    SUBCASE("silent for longer than dead_after, answering each keepalive") {
        cmd.silent = std::chrono::milliseconds(400);
        cmd.out = {"done\n"};
        auto r = ssh::run_command(cmd, "sleep 1; echo done", o);
        REQUIRE(r);
        CHECK(r->status == 0);
        CHECK(r->out_bytes == 5);
        CHECK(cmd.count("keepalive") >= 5);
        CHECK_FALSE(cmd.dropped);
    }
    SUBCASE("no answers: an error, the channel dropped without TERM") {
        cmd.forever = true;
        cmd.answers = false;
        auto started = std::chrono::steady_clock::now();
        std::string why;
        try {
            auto r = ssh::run_command(cmd, "sleep 60", o);
            FAIL("no error; it ended " << (r ? "normally" : r.error().message));
        } catch (const ssh::Error& e) {
            why = e.what();
        }
        CHECK(why.starts_with("the server stopped answering"));
        CHECK(std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(200));
        CHECK(cmd.count("keepalive") >= 3);
        CHECK(cmd.count("TERM") == 0);
        CHECK(cmd.log.back() == "drop dead");
    }
    SUBCASE("output keeps it alive without keepalives") {
        cmd.out = std::vector<std::string>(50, "line\n");
        auto r = ssh::run_command(cmd, "find .", o);
        REQUIRE(r);
        CHECK(cmd.count("keepalive") == 0);
    }
}

TEST_CASE("ssh exec: a session without a login refuses commands") {
    struct Plain : ssh::Session {
        sftp::Client& sftp() override { throw std::logic_error("no SFTP in this test"); }
    };
    Plain session;
    auto r = session.exec("find .", {});
    REQUIRE_FALSE(r);
    CHECK(r.error().kind == ssh::ExecFailure::refused);
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

// Phase 4's check of exec on a real server, on one login: EOF, keepalives, cancel and the cap.
// Nothing is left running there (TERM ends sleep and yes).
TEST_CASE("ssh: live exec, EOF, keepalives, cancel and the cap on one login (GRAB_TEST_SSH)") {
    auto t = live_target();
    if (!t) {
        MESSAGE("skipped: GRAB_TEST_SSH / GRAB_TEST_KNOWN_HOSTS not set");
        return;
    }
    using Clock = std::chrono::steady_clock;
    auto seconds = [](Clock::duration d) { return std::chrono::duration<double>(d).count(); };
    ssh::Connection c(*t);

    // stdin ends at once, so cat has nothing to copy and ends.
    auto started = Clock::now();
    auto cat = c.exec("cat");
    auto took = Clock::now() - started;
    REQUIRE_MESSAGE(cat.has_value(), (cat ? "" : cat.error().message));
    CHECK(cat->status == 0);
    CHECK(cat->out_bytes == 0);
    CHECK(took < std::chrono::seconds(3));
    MESSAGE("cat: " << seconds(took) << " s");

    // Silent for longer than dead_after: only the keepalives' answers keep it alive. It ends
    // normally and the connection works on.
    ssh::ExecOptions quiet;
    quiet.keepalive = std::chrono::seconds(3);
    quiet.dead_after = std::chrono::seconds(7);
    auto before = c.received();
    started = Clock::now();
    auto slept = c.exec("sleep 10", quiet);
    took = Clock::now() - started;
    REQUIRE_MESSAGE(slept.has_value(), (slept ? "" : slept.error().message));
    CHECK(slept->status == 0);
    CHECK(took >= std::chrono::seconds(10));
    CHECK_FALSE(c.dropped());
    MESSAGE("sleep 10: " << seconds(took) << " s, " << c.received() - before << " bytes received");
    CHECK(c.run("echo ok").out == "ok\n");

    // EOF at once, the exit status 3 s later: a keepalive's answer in between doesn't end the
    // wait for the server's close, which comes after the status.
    ssh::ExecOptions late;
    late.keepalive = std::chrono::seconds(1);
    started = Clock::now();
    auto closed = c.exec("exec >&- 2>&-; sleep 3; exit 3", late);
    took = Clock::now() - started;
    REQUIRE_MESSAGE(closed.has_value(), (closed ? "" : closed.error().message));
    CHECK(closed->status == 3);
    CHECK(took >= std::chrono::seconds(3));
    MESSAGE("output closed, exit 3 after 3 s: " << seconds(took) << " s");

    // A silent sleep 60 stops on cancel: TERM, then the channel is dropped.
    std::stop_source stop;
    ssh::ExecOptions cancellable;
    cancellable.stop = stop.get_token();
    std::jthread cancel([&] {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        stop.request_stop();
    });
    started = Clock::now();
    auto slow = c.exec("sleep 60", cancellable);
    took = Clock::now() - started;
    REQUIRE_FALSE(slow.has_value());
    CHECK(slow.error().kind == ssh::ExecFailure::stopped);
    CHECK(took < std::chrono::seconds(3));
    CHECK(c.dropped()); // a pooled session wouldn't be reused; this test goes on with its link
    MESSAGE("sleep 60, cancelled after 1 s: " << seconds(took) << " s");

    // Endless output stops at the cap.
    ssh::ExecOptions capped;
    capped.out_limit = std::uint64_t{1024} * 1024;
    std::uint64_t got = 0;
    capped.out = [&](std::string_view piece) {
        got += piece.size();
        return true;
    };
    started = Clock::now();
    auto yes = c.exec("yes", capped);
    took = Clock::now() - started;
    REQUIRE_FALSE(yes.has_value());
    CHECK(yes.error().kind == ssh::ExecFailure::too_much);
    CHECK(got == 1024 * 1024);
    CHECK(took < std::chrono::seconds(5));
    MESSAGE("yes, cut at 1 MiB: " << seconds(took) << " s");
}

// What a command leaves behind on its connection, on one login: a refused channel (the server's
// session limit, MaxSessions 10 on OpenSSH) leaves the link and its SFTP channels as they were, and
// SFTP waits after a command still last the whole timeout (libssh2's keepalives are off again).
TEST_CASE("ssh: live, a connection after commands: refused channel, SFTP waits (GRAB_TEST_SSH)") {
    auto t = live_target();
    if (!t) {
        MESSAGE("skipped: GRAB_TEST_SSH / GRAB_TEST_KNOWN_HOSTS not set");
        return;
    }
    using Clock = std::chrono::steady_clock;
    auto seconds = [](Clock::duration d) { return std::chrono::duration<double>(d).count(); };
    t->timeout = std::chrono::seconds(5);
    ssh::Connection c(*t);
    auto ran = c.exec("true");
    REQUIRE_MESSAGE(ran.has_value(), (ran ? "" : ran.error().message));

    // SFTP channels until the server refuses one.
    std::vector<std::unique_ptr<sftp::Channel>> channels;
    bool full = false;
    while (!full && channels.size() < 32) {
        try {
            channels.push_back(c.open_sftp());
        } catch (const ssh::Error&) {
            full = true;
        }
    }
    REQUIRE(channels.size() >= 2);
    if (full) {
        auto refused = c.exec("true");
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().kind == ssh::ExecFailure::refused);
        CHECK_FALSE(c.dropped());
        MESSAGE(channels.size() << " SFTP channels, then: " << refused.error().message);
    } else {
        MESSAGE("the server took " << channels.size() << " channels: no refused channel to try");
    }
    sftp::Client s(*channels[0]);
    s.start();
    CHECK(sftp::realpath(s, "."));

    // A channel the server says nothing on (sftp-server waits for its INIT): the read gives up
    // after the session's timeout, not after the 1-2 s of libssh2's keepalive interval.
    std::array<char, 16> buffer{};
    auto started = Clock::now();
    CHECK_THROWS_AS((void)channels[1]->read(buffer), sftp::Broken);
    auto took = Clock::now() - started;
    CHECK(took >= t->timeout - std::chrono::seconds(1));
    MESSAGE("a silent SFTP channel gave up after " << seconds(took) << " s");
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

// From a server's rclone.conf section to a login through the pool, as the engine will do it:
// GRAB_TEST_RCLONE_CONF names an rclone.conf (no passwords needed for an agent server) and
// GRAB_TEST_REMOTE its section.
TEST_CASE("ssh: live, a server from its rclone.conf section through the pool (GRAB_TEST_REMOTE)") {
    auto conf = util::getenv_utf8("GRAB_TEST_RCLONE_CONF");
    auto name = util::getenv_utf8("GRAB_TEST_REMOTE");
    if (!conf || !name) {
        MESSAGE("skipped: GRAB_TEST_RCLONE_CONF / GRAB_TEST_REMOTE not set");
        return;
    }
    auto remote = load_rclone_remote(util::path_from_utf8(*conf), *name);
    REQUIRE_MESSAGE(remote, remote.error_or(""));
    auto target = ssh::target_for(*remote, secrets::Store{}, grab_known_hosts_path());
    REQUIRE_MESSAGE(target, target.error_or(""));
    ssh::Pool::Options o;
    o.limit = 2;
    ssh::Pool pool([&] { return ssh::open_session(*target); }, o);
    auto lease = pool.acquire();
    REQUIRE(lease);
    auto home = sftp::realpath((*lease)->sftp(), ".");
    REQUIRE(home);
    MESSAGE("home: " << *home);
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
