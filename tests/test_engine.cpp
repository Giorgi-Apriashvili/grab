#include "browse.hpp"
#include "config.hpp"
#include "engine.hpp"
#include "file_list.hpp"
#include "gui_state.hpp"
#include "passwords.hpp"
#include "scoped_env.hpp"
#include "server_conf.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using namespace grab;

TEST_CASE("a search's result keeps each hit's size and date through ranking and duplicates") {
    engine::Context ctx;
    ctx.settings.max_depth = 4;
    engine::SearchRequest req;
    req.target = "lioness";
    req.mode = Mode::file;
    const auto r = engine::make_result(ctx, req,
                                       {{"tv/Lioness.S03E08.mkv", 300, 1697354553},
                                        {"tv/Lioness.S03E02.mkv", 200, std::nullopt},
                                        {"tv/Lioness.S03E08.mkv", 1, 5}}); // a duplicate: the first one counts
    REQUIRE(r.hits.size() == 2);
    CHECK(r.hits[0].path == "tv/Lioness.S03E02.mkv");
    CHECK(r.hits[0].size == 200ULL);
    CHECK_FALSE(r.hits[0].modified.has_value());
    CHECK(r.hits[1].name == "Lioness.S03E08.mkv");
    CHECK(r.hits[1].size == 300ULL);
    CHECK(r.hits[1].modified == 1697354553);
}

TEST_CASE("format_size") {
    CHECK(util::format_size(0) == "0 B");
    CHECK(util::format_size(1023) == "1023 B");
    CHECK(util::format_size(1536) == "1.5 KiB");
    CHECK(util::format_size(13367823676ULL) == "12.4 GiB");
}

namespace {

// A server as grab.conf holds it, in the context load_context makes (engine::make_context), its
// store `passwords` (none: no file); made-up secrets only. Its host is pinned in grab's
// known_hosts (the test runner's scratch APPDATA) unless `pin` is false.
engine::Context grab_context(const std::string& name, server_conf::Connection c,
                             const std::filesystem::path& passwords = {}, bool pin = true) {
    if (pin) {
        std::filesystem::create_directories(grab_known_hosts_path().parent_path());
        std::ofstream(grab_known_hosts_path(), std::ios::binary | std::ios::app)
            << (c.port == 22 ? c.host : std::format("[{}]:{}", c.host, c.port)) << " ssh-ed25519 FAKEKEY\n";
    }
    RemoteSettings settings;
    settings.name = name;
    settings.rclone_remote = name;
    auto ctx = engine::make_context(GrabConfig{}, std::move(settings), std::move(c), "unused.conf");
    ctx.passwords = passwords;
    return ctx;
}

server_conf::Connection connection(std::string host, std::string user, server_conf::Auth auth, int port = 22) {
    server_conf::Connection c;
    c.host = std::move(host);
    c.user = std::move(user);
    c.auth = auth;
    c.port = port;
    return c;
}

passwords::Secret fake(std::string value, const server_conf::Connection& at) {
    return passwords::Secret{.binding = {.user = at.user, .host = at.host, .port = at.port}, .value = std::move(value)};
}

} // namespace

TEST_CASE("grab's own target: common_flags, then the command's flags after --, which win") {
    auto ctx = grab_context("Main", connection("main.example", "me", server_conf::Auth::agent));
    ctx.settings.common_flags = {"-P", "--contimeout", "10s", "--bind", "10.0.0.1"};
    auto plain = engine::ssh_target(ctx);
    REQUIRE_MESSAGE(plain, plain.error_or(""));
    CHECK(plain->agent);
    CHECK(plain->known_hosts == grab_known_hosts_path());
    CHECK(plain->connect_timeout == std::chrono::seconds(10));
    CHECK(plain->bind == "10.0.0.1");
    const std::vector<std::string> extra{"--bind", "10.8.0.2", "--timeout=1m"};
    auto given = engine::ssh_target(ctx, extra);
    REQUIRE_MESSAGE(given, given.error_or(""));
    CHECK(given->connect_timeout == std::chrono::seconds(10));
    CHECK(given->bind == "10.8.0.2");
    CHECK(given->timeout == std::chrono::minutes(1));
    const std::vector<std::string> bad{"--timeout", "soon"};
    CHECK(engine::ssh_target(ctx, bad).error_or("").starts_with("the flags after --: --timeout soon"));
    // A blank --bind after `--` clears the bind for the run; in common_flags it is no bind.
    const std::vector<std::string> unbind{"--bind="};
    auto unbound = engine::ssh_target(ctx, unbind);
    REQUIRE_MESSAGE(unbound, unbound.error_or(""));
    CHECK(unbound->bind.empty());
    ctx.settings.common_flags = {"--bind="};
    auto blank = engine::ssh_target(ctx);
    REQUIRE_MESSAGE(blank, blank.error_or(""));
    CHECK(blank->bind.empty());
    std::filesystem::remove(grab_known_hosts_path());
}

TEST_CASE("connection settings: the keys, else common_flags' --bind, --contimeout and --timeout, then the command's") {
    using std::chrono::milliseconds;
    RemoteSettings s;
    s.name = "Main";
    s.common_flags = {"-P", "--bind", "10.0.0.1", "--contimeout=10s", "--timeout", "2m", "--sftp-chunk-size", "255Ki"};
    const auto legacy = engine::connect_settings(s);
    REQUIRE_MESSAGE(legacy, legacy.error_or(""));
    CHECK(legacy->bind == "10.0.0.1");
    CHECK(legacy->bind_from == "common_flags' --bind");
    CHECK(legacy->connect_timeout == milliseconds(10'000));
    CHECK(legacy->io_timeout == milliseconds(120'000));
    CHECK(legacy->legacy == std::vector<std::string>{"bind", "connect_timeout", "io_timeout"});
    // A key set wins over common_flags'; connect_timeout = off is a setting too.
    s.bind = "10.0.0.2";
    s.connect_timeout = milliseconds(0);
    s.io_timeout = milliseconds(45'000);
    const auto keys = engine::connect_settings(s);
    REQUIRE(keys);
    CHECK(keys->bind == "10.0.0.2");
    CHECK(keys->bind_from == "bind in [Main]");
    CHECK(keys->connect_timeout == milliseconds(0));
    CHECK(keys->io_timeout == milliseconds(45'000));
    CHECK(keys->legacy.empty());
    // The command's flags after `--` win over both.
    const std::vector<std::string> run{"--bind=10.8.0.2", "--contimeout", "3s"};
    const auto given = engine::connect_settings(s, run);
    REQUIRE(given);
    CHECK(given->bind == "10.8.0.2");
    CHECK(given->bind_from == "--bind");
    CHECK(given->connect_timeout == milliseconds(3'000));
    CHECK(given->io_timeout == milliseconds(45'000));
    // A --bind that isn't an address is refused before anything connects.
    s.bind.reset();
    s.common_flags = {"--bind=-oProxyCommand=x"};
    CHECK(engine::connect_settings(s).error_or("") == "[Main] common_flags: --bind -oProxyCommand=x: the address can't start with -");
    s.common_flags = {"--contimeout", "soon"};
    CHECK(engine::connect_settings(s).error_or("").starts_with("[Main] common_flags: --contimeout soon: not a time"));
    s.common_flags.clear();
    const std::vector<std::string> bad{"--bind=10.0.0.2;x"};
    CHECK(engine::connect_settings(s, bad).error_or("").starts_with("the flags after --: --bind 10.0.0.2;x: "));
    // A blank --bind is no bind, as rclone read it: in common_flags nothing, after `--` it clears
    // the server's for the run.
    s.common_flags = {"--bind="};
    const auto blank = engine::connect_settings(s);
    REQUIRE_MESSAGE(blank, blank.error_or(""));
    CHECK_FALSE(blank->bind);
    CHECK(blank->legacy.empty());
    s.bind = "10.0.0.2";
    const std::vector<std::string> clear{"--bind", ""};
    const auto cleared = engine::connect_settings(s, clear);
    REQUIRE_MESSAGE(cleared, cleared.error_or(""));
    CHECK_FALSE(cleared->bind);
    CHECK(cleared->bind_from.empty());
}

TEST_CASE("grab's own target: the keys, common_flags' legacy flags and the command's; never without an I/O time") {
    using std::chrono::milliseconds;
    auto ctx = grab_context("Main", connection("main.example", "me", server_conf::Auth::agent));
    ctx.settings.bind = "10.0.0.5";
    ctx.settings.io_timeout = milliseconds(45'000);
    ctx.settings.common_flags = {"--bind", "10.0.0.1", "--contimeout", "10s"};
    const auto t = engine::ssh_target(ctx);
    REQUIRE_MESSAGE(t, t.error_or(""));
    CHECK(t->bind == "10.0.0.5");
    CHECK(t->bind_from == "bind in [Main]");
    CHECK(t->connect_timeout == milliseconds(10'000)); // common_flags', the key being blank
    CHECK(t->timeout == milliseconds(45'000));
    // No I/O limit (or more than 24 h) would leave a stalled read that Cancel can't end.
    ctx.settings.io_timeout.reset();
    ctx.settings.common_flags = {"--timeout", "off"};
    CHECK(engine::ssh_target(ctx).error_or("").starts_with(
        "[Main] common_flags: --timeout off: grab's own client takes a time from 1s to 24h there"));
    ctx.settings.common_flags.clear();
    for (const char* time : {"0", "25h", "500ms"}) {
        const std::vector<std::string> run{"--timeout", time};
        CHECK_MESSAGE(engine::ssh_target(ctx, run).error_or("").starts_with(std::format("the flags after --: --timeout {}:", time)),
                      time);
    }
    const std::vector<std::string> fine{"--timeout", "24h"};
    CHECK(engine::ssh_target(ctx, fine).has_value());
    std::filesystem::remove(grab_known_hosts_path());
}

TEST_CASE("the guard: a server grab can't serve as configured is refused everywhere, before anything connects (phase 9)") {
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-guard";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto conf = dir / "grab.conf";
    const auto store = passwords::path_in(state_dir(conf));
    std::filesystem::create_directories(store.parent_path());
    std::filesystem::remove(grab_known_hosts_path());
    // A closed local port: a connection the guard failed to stop would fail fast, not hang.
    const auto c = connection("127.0.0.1", "u", server_conf::Auth::agent, 9);
    REQUIRE(passwords::save(store, {{"Box", {{"rclone.socks_proxy", fake("fake-user:fake-pass@127.0.0.1:9", c)},
                                             {"rclone.set_env", fake("X=1", c)}}}}));
    RemoteSettings s;
    s.name = "Box";
    s.rclone_remote = "Box";
    // What the store's decryption costs load_context (the window calls it per server): timed.
    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) {
        const auto made = engine::make_context(GrabConfig{}, s, c, conf);
        CHECK(!made.guard.empty());
    }
    MESSAGE(std::format("make_context with the guard's store read: {:.2f} ms each",
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() / 20));

    // Not pinned either: the guard is said first, as Trust would be refused too.
    const auto ctx = engine::make_context(GrabConfig{}, s, c, conf);
    const std::string reason = "[Box] connects through a SOCKS proxy (rclone.socks_proxy in grab's store), which grab "
                               "can't, so it doesn't connect to Box rather than go around it: the import kept "
                               "rclone.socks_proxy in grab's store: remove Box and add it again";
    const auto refused = engine::ssh_target(ctx);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().starts_with(reason));
    CHECK(refused.error().find("fake-") == std::string::npos);
    CHECK(refused.error().find("engine = rclone") == std::string::npos);
    // Every way grab would reach it: a search, a file list, Browse (each makes its pool from
    // ssh_target), with nothing dialled.
    engine::SearchRequest req;
    req.target = "x";
    CHECK(engine::search(ctx, req).error_or("").starts_with(reason));
    CHECK(engine::search_both(ctx, req).error_or("").starts_with(reason));
    CHECK(file_list::fetch(ctx, 1).error_or("").starts_with(reason));
    const auto listed = browse::list_live(ctx, "", {});
    REQUIRE_FALSE(listed);
    CHECK(listed.error().message.starts_with(reason));
    // `engine = rclone` (or GRAB_ENGINE=rclone) changes nothing: there's no rclone to send it to.
    s.engine = Engine::rclone;
    CHECK(engine::ssh_target(engine::make_context(GrabConfig{}, s, c, conf)).error_or("").starts_with(reason));
    s.engine.reset();

    // A store that exists but can't be read may hold a route: every server of it is refused, an
    // agent login too, until an Edit sets the file aside.
#ifdef _WIN32
    std::ofstream(store, std::ios::binary | std::ios::trunc) << "grab-passwords-sealed 1\nanother account's";
    const auto unreadable = engine::make_context(GrabConfig{}, s, c, conf);
    CHECK(engine::ssh_target(unreadable).error_or("") ==
          "[Box] may connect through a proxy or an ssh command (grab's store, which can't be read), which grab can't, so "
          "it doesn't connect to Box rather than go around it: saving Box with Edit (no change needed) sets the "
          "unreadable store aside, and with it any proxy or ssh command kept there: Box then connects directly");
#endif
    std::filesystem::remove_all(store.parent_path());
    std::filesystem::remove_all(dir);
}

TEST_CASE("the guard's other sources: a grab.conf line, common_flags, an inherited variable, a flag after --") {
    std::filesystem::remove(grab_known_hosts_path());
    auto c = connection("127.0.0.1", "u", server_conf::Auth::agent, 9);
    auto ctx = grab_context("Main", c);
    REQUIRE(engine::ssh_target(ctx).has_value()); // nothing set: grab's own client

    // A quirk switch in the section: refused, no route to go around.
    c.rclone = {{"disable_concurrent_reads", "true"}};
    CHECK(engine::ssh_target(grab_context("Main", c, {}, false)).error_or("") ==
          "[Main] needs one read at a time (rclone.disable_concurrent_reads), which grab can't, so it doesn't connect to "
          "Main: delete the rclone.disable_concurrent_reads line in [Main] (grab config)");
    c.rclone.clear();

    // A proxy in common_flags.
    auto flagged = grab_context("Main", c, {}, false);
    flagged.settings.common_flags = {"-P", "--sftp-socks-proxy", "fake-user:fake-pass@127.0.0.1:1080"};
    flagged = engine::make_context(GrabConfig{}, flagged.settings, c, "unused.conf");
    const auto by_flag = engine::ssh_target(flagged).error_or("");
    CHECK(by_flag.ends_with("take --sftp-socks-proxy out of common_flags in [Main] (grab config)"));
    CHECK(by_flag.find("fake-") == std::string::npos);

    // ssh_options' ProxyCommand is no reason (decision 4): grab connects directly.
    auto jumped = grab_context("Main", c, {}, false);
    jumped.settings.ssh_options = {"-o", "ProxyCommand=fake-proxy %h %p", "-J", "fake-jump"};
    jumped = engine::make_context(GrabConfig{}, jumped.settings, c, "unused.conf");
    CHECK(jumped.guard.contains("ProxyCommand"));
    CHECK(engine::grab_blockers(jumped).empty());
    CHECK(engine::ssh_target(jumped).has_value());

    // A guard flag after `--` on a server that has none.
    const std::vector<std::string> run{"--sftp-http-proxy", "fake-proxy:3128"};
    CHECK(engine::grab_blockers(ctx, run).size() == 1);
    CHECK(engine::ssh_target(ctx, run).error_or("").ends_with(": leave --sftp-http-proxy out after --"));

    {
        // An inherited RCLONE_SFTP_SOCKS_PROXY covers every server; RCLONE_SFTP_SSH never reached
        // rclone and doesn't count.
        const test::ScopedEnv ssh("RCLONE_SFTP_SSH", "fake-ssh jump");
        CHECK(engine::ssh_target(grab_context("Main", c, {}, false)).has_value());
        const test::ScopedEnv proxy("RCLONE_SFTP_SOCKS_PROXY", "fake-user:fake-pass@127.0.0.1:1080");
        for (const char* name : {"Main", "Other"}) {
            const auto inherited = engine::ssh_target(grab_context(name, c, {}, false)).error_or("");
            CHECK(inherited.starts_with(
                std::format("[{}] connects through a SOCKS proxy (RCLONE_SFTP_SOCKS_PROXY)", name)));
            CHECK(inherited.ends_with("RCLONE_SFTP_SOCKS_PROXY is set in grab's environment and covers every "
                                      "server; clear it for grab alone (`set RCLONE_SFTP_SOCKS_PROXY=` in that "
                                      "window before starting grab) or for good"));
            CHECK(inherited.find("fake-") == std::string::npos);
        }
    }
    std::filesystem::remove(grab_known_hosts_path());
}

TEST_CASE("engine = rclone and GRAB_ENGINE=rclone change nothing: every server goes through grab's own client") {
    std::filesystem::remove(grab_known_hosts_path());
    auto ctx = grab_context("Main", connection("main.example", "u", server_conf::Auth::agent));
    ctx.settings.engine = Engine::rclone;
    const auto target = engine::ssh_target(ctx);
    REQUIRE_MESSAGE(target, target.error_or(""));
    CHECK(target->host == "main.example");
    {
        const test::ScopedEnv rclone("GRAB_ENGINE", "rclone");
        CHECK(engine::ssh_target(ctx).has_value());
    }
    std::filesystem::remove(grab_known_hosts_path());
}

TEST_CASE("the flags after --, before anything connects (phase 8 step 6)") {
    using V = std::vector<std::string>;
    engine::Context ctx;
    ctx.settings.name = "Main";
    const auto error = [&](V extra) { return engine::check_run_flags(ctx, extra).error_or(""); };
    // grab's own client takes --bind, --contimeout and --timeout for the run, either way written.
    CHECK(engine::check_run_flags(ctx, V{}).has_value());
    CHECK(engine::check_run_flags(ctx, V{"--bind", "10.0.0.2", "--contimeout=5s", "--timeout", "2m"}).has_value());
    CHECK(engine::check_run_flags(ctx, V{"--bind="}).has_value()); // no bind for this run
    CHECK(engine::check_run_flags(ctx, V{"--contimeout", "off", "--timeout=24h"}).has_value());
    // The rest refused, naming what to use instead (the 2026-10-21 rule: refused, never ignored).
    CHECK(error({"--timeout", "1m", "--sftp-connections", "4"}) ==
          "--sftp-connections after -- isn't used by grab's own client; set max_connections in [Main] (grab config)");
    CHECK(error({"--sftp-chunk-size", "1Mi"}) == "--sftp-chunk-size after -- isn't used by grab's own client; it reads "
                                                 "255 KiB at a time, 64 in flight, and splits big files over its "
                                                 "connections, with nothing to set");
    CHECK(error({"--multi-thread-streams=4"}).find("with nothing to set") != std::string::npos);
    CHECK(error({"--low-level-retries", "3"}).ends_with("; it retries a failed piece by itself"));
    CHECK(error({"--bwlimit", "5M"}).ends_with("; grab's own --limit sets the speed"));
    CHECK(error({"--tpslimit", "5"}).ends_with("; grab's own --limit sets the speed"));
    CHECK(error({"--transfers", "4"}).ends_with("; files at once: At once in grab's window"));
    CHECK(error({"--dry-run"}).ends_with("; grab's own -n shows what would be fetched"));
    CHECK(error({"-P"}).ends_with("; grab shows its own progress"));
    CHECK(error({"--dscp", "CS1"}).ends_with("; it isn't used over SFTP"));
    CHECK(error({"--size-only"}) ==
          "--size-only after -- isn't used by grab's own client; it takes only --bind, --contimeout and --timeout there");
    CHECK(error({"--bind", "10.0.0.2", "stray"}).starts_with("'stray' after -- isn't a flag"));
    // What grab can't follow is refused, and never quotes the value.
    const auto proxy = error({"--sftp-socks-proxy", "fake-user:fake-secret@127.0.0.1:9"});
    CHECK(proxy.starts_with("--sftp-socks-proxy after --: grab's own client can't use a proxy"));
    CHECK(proxy.find("fake-") == std::string::npos);
    CHECK(error({"--sftp-ciphers=aes128-ctr"}).find("offers only its own algorithms") != std::string::npos);
    CHECK_FALSE(error({"--sftp-disable-concurrent-reads"}).empty());
    // The server's address, login and host keys, as before.
    CHECK(error({"--sftp-host", "elsewhere.example"}) ==
          "--sftp-host after -- would replace the server's address, which grab's own client takes from its settings; "
          "change grab.conf's `host` with `grab server edit Main` or Edit in grab's Settings");
    CHECK(error({"--sftp-ssh", "ssh -i key"}).ends_with("which grab's own client can't use; an ssh command can't be "
                                                         "given there"));
    CHECK(error({"--sftp-pubkey-file", "C:\\id-cert.pub"})
              .starts_with("--sftp-pubkey-file after -- would replace the server's certificate, which grab's own client "
                           "can't use; "));
    // Values grab's own client can't use: refused now, not when it connects.
    CHECK(error({"--timeout", "off"}) == "the flags after --: --timeout off: grab's own client takes a time from 1s to "
                                         "24h there (with none, a stalled download couldn't be cancelled)");
    CHECK(error({"--timeout=0.5s"}).starts_with("the flags after --: --timeout 0.5s: grab's own client takes"));
    CHECK(error({"--timeout", "25h"}).starts_with("the flags after --: --timeout 25h:"));
    CHECK(error({"--timeout", "abc"}).starts_with("the flags after --: --timeout abc: not a time grab can read"));
    CHECK(error({"--bind"}) == "the flags after --: --bind needs a value");
    CHECK(error({"--bind=-x"}) == "the flags after --: --bind -x: the address can't start with -");
    // `engine = rclone` in the section changes none of it.
    ctx.settings.engine = Engine::rclone;
    CHECK(error({"--dry-run"}).ends_with("; grab's own -n shows what would be fetched"));
    CHECK(error({"--sftp-chunk-size", "1Mi"}).starts_with("--sftp-chunk-size after -- isn't used by grab's own client"));
}

TEST_CASE("an unpinned server is refused before grab's client dials: use Trust") {
    std::filesystem::remove(grab_known_hosts_path());
    const auto ctx = grab_context("Main", connection("main.example", "me", server_conf::Auth::agent, 2222), {}, false);
    for (const auto& error : {engine::check_pinned(ctx).error_or(""), engine::ssh_target(ctx).error_or("")}) {
        CHECK(error.find("host key not trusted: use Trust") != std::string::npos);
    }
    // A key for another port, or another host, doesn't count.
    std::filesystem::create_directories(grab_known_hosts_path().parent_path());
    std::ofstream(grab_known_hosts_path(), std::ios::binary) << "main.example ssh-ed25519 PORT22\n[other]:2222 ssh-ed25519 X\n";
    CHECK_FALSE(engine::check_pinned(ctx).has_value());
    std::ofstream(grab_known_hosts_path(), std::ios::binary | std::ios::app) << "[main.example]:2222 ssh-ed25519 K\n";
    CHECK(engine::check_pinned(ctx).has_value());
    std::filesystem::remove(grab_known_hosts_path());
}
