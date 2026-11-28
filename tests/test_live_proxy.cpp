#include <doctest/doctest.h>

// grab's own client through a proxy on a live server (PROXIES.md chunk 0; the live checker runs
// it): a login, a command, SFTP and a read of a few MiB through the proxy, timed beside a direct
// login when asked; a dead proxy and a wrong proxy password (both fail before anything reaches the
// server); the proxy's connection cap; Trust's host-key scan through the proxy. Lines start with
// "hook: "; no password is printed.
//
//   GRAB_TEST_PROXY           the proxy as grab.conf writes it: socks5://[user@]host:port or
//                             http://[user@]host:port
//   GRAB_TEST_PROXY_PASSWORD  its password (with a user)
//   GRAB_TEST_SSH             user@host:port: the server as the proxy is asked for it (main.invalid
//                             with test_proxy.py's name map, 127.0.0.1 through ssh -D to Main)
//   GRAB_TEST_KNOWN_HOSTS     a known_hosts pinning the server's key under that name
//   GRAB_TEST_PROXY_DIRECT    user@host:port: the same server directly, for the timings (pinned too)
//   GRAB_TEST_PROXY_MIB       the read's size in MiB (8)
//   GRAB_TEST_PROXY_CAP       the proxy's cap on tunnels (test_proxy.py --cap, 1-2): that many
//                             logins at once, then one more, which the proxy turns away
//   GRAB_TEST_SCAN            also Trust's scan through the proxy (its last handshake fails to
//                             agree, which fail2ban's aggressive mode counts)
//   GRAB_TEST_APPDATA, GRAB_TEST_SEED_SERVER   with GRAB_TEST_PROXY_PASSWORD: no connection; that
//                             server of the isolated settings in GRAB_TEST_APPDATA gets its proxy's
//                             password saved as an Edit saves it (chunk 1a's command-line checks)
//   GRAB_TEST_APPDATA, GRAB_TEST_STORE_KEYS   no connection: that store's keys and bindings, never
//                             a value
//
// Logs in through ssh-agent. Writes only /tmp/grab-proxy-<random> on the server, removed at the
// end. At most 2 logins at once (the cap case: the cap's and one more, at most 3).

#include "config.hpp"
#include "conn_budget.hpp"
#include "guard.hpp"
#include "gui_state.hpp"
#include "live_tools.hpp"
#include "passwords.hpp"
#include "proxy.hpp"
#include "server_ops.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "util.hpp"

#ifdef _WIN32
#include "live_settings.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace grab;
using namespace grab::test::live;

namespace {

// user@host:port into a Target logging in through ssh-agent, with GRAB_TEST_KNOWN_HOSTS.
std::optional<ssh::Target> target_at(const std::string& where) {
    const auto known = util::getenv_utf8("GRAB_TEST_KNOWN_HOSTS");
    const auto at = where.find('@');
    const auto colon = where.rfind(':');
    if (!known || at == std::string::npos || colon == std::string::npos || colon < at) return std::nullopt;
    ssh::Target t;
    t.user = where.substr(0, at);
    t.host = where.substr(at + 1, colon - at - 1);
    t.port = std::stoi(where.substr(colon + 1));
    t.known_hosts = util::path_from_utf8(*known);
    t.agent = true;
    return t;
}

// GRAB_TEST_SSH through GRAB_TEST_PROXY.
std::optional<ssh::Target> proxied_target() {
    const auto where = util::getenv_utf8("GRAB_TEST_SSH");
    const auto written = util::getenv_utf8("GRAB_TEST_PROXY");
    if (!where || !written) return std::nullopt;
    auto t = target_at(*where);
    if (!t) return std::nullopt;
    auto p = proxy::parse(*written);
    REQUIRE_MESSAGE(p, p.error_or(""));
    p->password = util::getenv_utf8("GRAB_TEST_PROXY_PASSWORD").value_or("");
    t->proxy = std::move(*p);
    return t;
}

std::string route(const ssh::Target& t) {
    return std::format("{}:{} through {}", t.host, t.port, proxy::text(t.proxy));
}

// Removes the folder through `c` when it goes, whatever happened.
struct RemoveAfter {
    ssh::Connection& c;
    std::string dir;
    RemoveAfter(ssh::Connection& c, std::string dir) : c(c), dir(std::move(dir)) {}
    RemoveAfter(const RemoveAfter&) = delete;
    RemoveAfter& operator=(const RemoveAfter&) = delete;
    ~RemoveAfter() {
        try {
            const auto gone = c.run(std::format("rm -rf {}", dir));
            say(std::format("removed {}: {}", dir, yes(gone.status == 0)));
        } catch (const std::exception& e) {
            std::fputs("hook: removing the folder failed: ", stdout);
            std::fputs(e.what(), stdout);
            std::fputs("\n", stdout);
        } catch (...) {
            std::fputs("hook: removing the folder failed\n", stdout);
        }
    }
};

// The file read whole over SFTP on `c`: its time, its MD5 checked against the server's.
void read_back(ssh::Connection& c, const std::string& file, std::uint64_t size, const std::string& md5, std::string_view how) {
    auto channel = c.open_sftp();
    sftp::Client s(*channel);
    s.start();
    const auto h = sftp::open_file(s, file);
    REQUIRE_MESSAGE(h, code_of(h));
    std::string got(static_cast<std::size_t>(size), '\0');
    const auto started = Clock::now();
    const auto r = sftp::read_range(s, *h, 0, size, [&](std::uint64_t at, std::string_view bytes) {
        std::memcpy(got.data() + at, bytes.data(), bytes.size());
    });
    const double took = seconds(Clock::now() - started);
    if (auto closed = sftp::close_file(s, *h); !closed) {
        // A handle that fails to close doesn't spoil what was read.
    }
    REQUIRE_MESSAGE(r, code_of(r));
    const bool same = md5_of(got) == md5;
    CHECK(same);
    say(std::format("{}: read {} MiB in {:.2f} s, {:.1f} MiB/s, MD5 matches: {}", how, size / mib, took,
                    static_cast<double>(size) / static_cast<double>(mib) / took, yes(same)));
}

} // namespace

TEST_CASE("proxy: live, a login, a command and a read of a few MiB through the proxy (GRAB_TEST_PROXY)") {
    auto t = proxied_target();
    if (!t) {
        MESSAGE("skipped: GRAB_TEST_PROXY / GRAB_TEST_SSH / GRAB_TEST_KNOWN_HOSTS not set");
        return;
    }
    say("route: " + route(*t));
    auto started = Clock::now();
    ssh::Connection c(*t);
    say(std::format("through the proxy: login {} ms (connect {:.0f} ms in all), {}", c.login_time().count(),
                    seconds(Clock::now() - started) * 1000, c.algorithms()));
    CHECK(c.run("echo ok").out == "ok\n");

    const std::uint64_t size = number_from_env("GRAB_TEST_PROXY_MIB", 8) * mib;
    const std::string dir = "/tmp/grab-proxy-" + token();
    const RemoveAfter remove(c, dir);
    const auto made = c.run(std::format("umask 077 && mkdir {0} && head -c {1} /dev/urandom > {0}/data && md5sum {0}/data",
                                        dir, size));
    REQUIRE_MESSAGE(made.status == 0, made.err);
    REQUIRE(made.out.size() >= 32);
    const std::string md5 = made.out.substr(0, 32);
    read_back(c, dir + "/data", size, md5, "through the proxy");

    if (const auto direct_at = util::getenv_utf8("GRAB_TEST_PROXY_DIRECT")) {
        const auto direct = target_at(*direct_at);
        REQUIRE(direct);
        started = Clock::now();
        ssh::Connection d(*direct);
        say(std::format("directly: login {} ms (connect {:.0f} ms in all)", d.login_time().count(),
                        seconds(Clock::now() - started) * 1000));
        read_back(d, dir + "/data", size, md5, "directly");
    }
}

TEST_CASE("proxy: live, a dead proxy and a wrong proxy password fail, and nothing goes directly (GRAB_TEST_PROXY)") {
    auto t = proxied_target();
    if (!t) {
        MESSAGE("skipped: GRAB_TEST_PROXY / GRAB_TEST_SSH / GRAB_TEST_KNOWN_HOSTS not set");
        return;
    }
    // How a login ended: its error, or "logged in" (kind other).
    const auto ended = [](const ssh::Target& target) {
        try {
            ssh::Connection c(target);
        } catch (const ssh::Error& e) {
            return e;
        }
        return ssh::Error(budget::Failure::other, "logged in");
    };
    ssh::Target dead = *t;
    dead.proxy.host = "127.0.0.1";
    dead.proxy.port = 1; // nothing listens there
    const ssh::Error refused = ended(dead);
    CHECK(refused.kind == budget::Failure::other);
    CHECK(std::string(refused.what()).starts_with("can't reach the proxy 127.0.0.1:1: "));
    say(std::format("a dead proxy: {}", refused.what()));

    if (t->proxy.user.empty()) {
        say("the proxy has no user: no wrong password to try");
        return;
    }
    ssh::Target wrong = *t;
    wrong.proxy.password += "-wrong";
    const ssh::Error turned_down = ended(wrong);
    const std::string what = turned_down.what();
    CHECK(turned_down.kind == budget::Failure::fatal);
    CHECK(what == "the proxy turned down the user name and password");
    CHECK(what.find(wrong.proxy.password) == std::string::npos);
    say(std::format("a wrong proxy password: {} ({})", what,
                    turned_down.kind == budget::Failure::fatal ? "fatal" : "NOT fatal"));
}

TEST_CASE("proxy: live, the proxy's cap turns one more away while the others are open (GRAB_TEST_PROXY_CAP)") {
    auto t = proxied_target();
    const auto cap = number_from_env("GRAB_TEST_PROXY_CAP", 0);
    if (!t || cap == 0) {
        MESSAGE("skipped: GRAB_TEST_PROXY / GRAB_TEST_SSH / GRAB_TEST_KNOWN_HOSTS / GRAB_TEST_PROXY_CAP not set");
        return;
    }
    // Main takes at most 4 logins at once: the cap's, one more (should the proxy not enforce
    // its cap) and the ssh -D login behind a proxy.
    REQUIRE(cap <= 2);
    std::vector<std::unique_ptr<ssh::Connection>> open;
    for (std::uint64_t i = 0; i < cap; ++i) open.push_back(std::make_unique<ssh::Connection>(*t));
    std::string what = "connected";
    auto kind = budget::Failure::other;
    std::optional<budget::Failure> proxied;
    try {
        ssh::Connection more(*t);
    } catch (const ssh::Error& e) {
        what = e.what();
        kind = e.kind;
        proxied = e.proxied;
    }
    auto name = [](budget::Failure f) {
        return f == budget::Failure::refused ? "refused" : f == budget::Failure::fatal ? "fatal" : "other";
    };
    say(std::format("one more than the cap of {}: {} ({}, alone it would be {})", cap, what, name(kind),
                    proxied ? name(*proxied) : "the same"));
    CHECK(kind == budget::Failure::refused);
    CHECK(proxied.has_value());
    open.pop_back();
    ssh::Connection again(*t);
    say("one closed, one more: connected");
}

TEST_CASE("proxy: live, Trust's host-key scan through the proxy (GRAB_TEST_PROXY, GRAB_TEST_SCAN)") {
    auto t = proxied_target();
    if (!t || !util::getenv_utf8("GRAB_TEST_SCAN")) {
        MESSAGE("skipped: GRAB_TEST_PROXY / GRAB_TEST_SSH / GRAB_TEST_KNOWN_HOSTS / GRAB_TEST_SCAN not set");
        return;
    }
    const ssh::Reach reach{.bind = "", .bind_from = "--bind", .connect_timeout = std::nullopt, .proxy = t->proxy};
    const auto started = Clock::now();
    const auto keys = ssh::scan_host_keys(t->host, t->port, std::chrono::seconds(30), {}, reach);
    REQUIRE_MESSAGE(keys, keys.error_or(""));
    say(std::format("scan through the proxy: {:.2f} s", seconds(Clock::now() - started)));
    const std::string pinned = util::read_file(t->known_hosts).value_or("");
    CHECK_FALSE(keys->empty());
    for (const auto& k : *keys) {
        const bool known = ssh::check_known_host(pinned, t->host, t->port, util::base64_decode(k.key).value_or("")) ==
                           ssh::KnownHost::match;
        CHECK(known);
        say(std::format("{} {} pinned: {}", k.type, k.fingerprint, yes(known)));
    }
}

#ifdef _WIN32
TEST_CASE("proxy: live, a server's proxy password saved as an Edit saves it (GRAB_TEST_APPDATA, GRAB_TEST_SEED_SERVER)") {
    // For the live checker's isolated settings (PROXIES.md chunk 1a): server GRAB_TEST_SEED_SERVER
    // of the grab.conf in GRAB_TEST_APPDATA\grab keeps its `proxy` line, and an Edit that types
    // GRAB_TEST_PROXY_PASSWORD for it (server_ops::update_server) saves that password, bound to
    // the proxy, and the copy for older grabs. Never run on the user's own settings.
    const auto appdata = util::getenv_utf8("GRAB_TEST_APPDATA");
    const auto name = util::getenv_utf8("GRAB_TEST_SEED_SERVER");
    const auto password = util::getenv_utf8("GRAB_TEST_PROXY_PASSWORD");
    if (!appdata || !name || !password) {
        MESSAGE("skipped: GRAB_TEST_APPDATA / GRAB_TEST_SEED_SERVER / GRAB_TEST_PROXY_PASSWORD not set");
        return;
    }
    const test::AppData isolated(util::path_from_utf8(*appdata));
    const auto env = server_ops::load_env(default_grab_config_path());
    REQUIRE_MESSAGE(env, env.error_or(""));
    const auto list = server_ops::list_servers(*env);
    const auto current = std::ranges::find(list, *name, &server_ops::ServerInfo::name);
    REQUIRE(current != list.end());
    REQUIRE_MESSAGE(current->error.empty(), current->error);
    REQUIRE_MESSAGE(current->proxy_from == guard::ProxyFrom::line, "the server has no proxy line");
    auto wanted = server_ops::edit_defaults(*current);
    wanted.proxy = current->proxy;
    wanted.proxy->password = *password;
    const auto saved = server_ops::update_server(*env, wanted, std::nullopt, std::nullopt);
    REQUIRE_MESSAGE(saved, saved.error_or(""));
    say(std::format("{}: the password of {} saved in {}", *name, proxy::text(current->proxy),
                    util::path_to_utf8(env->passwords)));
}

TEST_CASE("proxy: live, what an isolated store keeps, never a value (GRAB_TEST_APPDATA, GRAB_TEST_STORE_KEYS)") {
    // Each server's keys in grab's store of the settings in GRAB_TEST_APPDATA, with the login each
    // value is bound to: the proxy password's to its proxy, the copy for older grabs' to the
    // server's login. Values are never printed.
    const auto appdata = util::getenv_utf8("GRAB_TEST_APPDATA");
    if (!appdata || !util::getenv_utf8("GRAB_TEST_STORE_KEYS")) {
        MESSAGE("skipped: GRAB_TEST_APPDATA / GRAB_TEST_STORE_KEYS not set");
        return;
    }
    const test::AppData isolated(util::path_from_utf8(*appdata));
    const auto store = passwords::load(passwords::path_in(state_dir(default_grab_config_path())));
    REQUIRE_MESSAGE(store, store.error_or(""));
    for (const auto& [server, values] : *store) {
        for (const auto& [key, secret] : values) {
            say(std::format("{} {} bound to {}", server, key, secret.binding.text()));
        }
    }
    say(std::format("{} servers in the store", store->size()));
}
#endif
