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

TEST_CASE("grab's own target: the keys, then the command's --bind, --contimeout and --timeout, which win") {
    using std::chrono::milliseconds;
    auto ctx = grab_context("Main", connection("main.example", "me", server_conf::Auth::agent));
    ctx.settings.bind = "10.0.0.1";
    ctx.settings.connect_timeout = milliseconds(10'000);
    auto plain = engine::ssh_target(ctx);
    REQUIRE_MESSAGE(plain, plain.error_or(""));
    CHECK(plain->agent);
    CHECK(plain->known_hosts == grab_known_hosts_path());
    CHECK(plain->connect_timeout == milliseconds(10'000));
    CHECK(plain->bind == "10.0.0.1");
    CHECK(plain->bind_from == "bind in [Main]");
    const ConnectOptions given{.bind = "10.8.0.2", .connect_timeout = std::nullopt, .io_timeout = milliseconds(60'000)};
    auto run = engine::ssh_target(ctx, given);
    REQUIRE_MESSAGE(run, run.error_or(""));
    CHECK(run->connect_timeout == milliseconds(10'000));
    CHECK(run->bind == "10.8.0.2");
    CHECK(run->bind_from == "--bind");
    CHECK(run->timeout == milliseconds(60'000));
    // A blank --bind clears the server's bind for the run.
    auto unbound = engine::ssh_target(ctx, ConnectOptions{.bind = "", .connect_timeout = {}, .io_timeout = {}});
    REQUIRE_MESSAGE(unbound, unbound.error_or(""));
    CHECK(unbound->bind.empty());
    // common_flags' --bind beside a `bind` key: the key, which wins (a notice says the flag is
    // ignored); its --contimeout and --timeout aren't read at all (phase 9 step 7).
    ctx.settings.common_flags = {"-P", "--bind", "10.0.0.9", "--contimeout", "3s", "--timeout", "5m"};
    ctx.settings.connect_timeout.reset();
    auto keyed = engine::ssh_target(ctx);
    REQUIRE_MESSAGE(keyed, keyed.error_or(""));
    CHECK(keyed->bind == "10.0.0.1");
    CHECK(keyed->connect_timeout == ssh::Target{}.connect_timeout);
    CHECK(keyed->timeout == ssh::Target{}.timeout);
    std::filesystem::remove(grab_known_hosts_path());
}

TEST_CASE("connection settings: grab.conf's keys, then the command's; the old keys' times aren't read") {
    using std::chrono::milliseconds;
    RemoteSettings s;
    s.name = "Main";
    s.common_flags = {"-P", "--contimeout=10s", "--timeout", "2m", "--sftp-chunk-size", "255Ki"};
    s.folder_flags = {"--timeout", "off"}; // 0.20 refused that on grab's client; ignored now
    const auto none = engine::connect_settings(s);
    REQUIRE_MESSAGE(none, none.error_or(""));
    CHECK_FALSE(none->bind);
    CHECK_FALSE(none->connect_timeout);
    CHECK_FALSE(none->io_timeout);
    s.bind = "10.0.0.2";
    s.connect_timeout = milliseconds(0); // off is a setting too
    s.io_timeout = milliseconds(45'000);
    const auto keys = engine::connect_settings(s);
    REQUIRE(keys);
    CHECK(keys->bind == "10.0.0.2");
    CHECK(keys->bind_from == "bind in [Main]");
    CHECK(keys->connect_timeout == milliseconds(0));
    CHECK(keys->io_timeout == milliseconds(45'000));
    const ConnectOptions run{.bind = "10.8.0.2", .connect_timeout = milliseconds(3'000), .io_timeout = std::nullopt};
    const auto given = engine::connect_settings(s, run);
    REQUIRE(given);
    CHECK(given->bind == "10.8.0.2");
    CHECK(given->bind_from == "--bind");
    CHECK(given->connect_timeout == milliseconds(3'000));
    CHECK(given->io_timeout == milliseconds(45'000));
    const auto cleared = engine::connect_settings(s, ConnectOptions{.bind = "", .connect_timeout = {}, .io_timeout = {}});
    REQUIRE(cleared);
    CHECK_FALSE(cleared->bind);
    CHECK(cleared->bind_from.empty());
}

TEST_CASE("a bind left in an old key with no `bind` key: refused before anything connects, never dropped (phase 9)") {
    RemoteSettings s;
    s.name = "Main";
    const auto error = [&] { return engine::connect_settings(s).error_or(""); };
    s.common_flags = {"-P", "--bind", "10.0.0.2", "--sftp-chunk-size", "255Ki"};
    CHECK(error() == "[Main] common_flags' --bind 10.0.0.2 isn't read any more: write bind = 10.0.0.2 in [Main] (grab "
                     "config) and take it out of common_flags. Until then grab doesn't connect to Main rather than connect "
                     "from another address");
    s.common_flags = {"--bind=10.0.0.3"};
    CHECK(error().starts_with("[Main] common_flags' --bind 10.0.0.3 isn't read any more: write bind = 10.0.0.3 in"));
    // rclone took the last one; a blank one is no bind, as rclone read it.
    s.common_flags = {"--bind", "10.0.0.2", "--bind="};
    CHECK(engine::connect_settings(s).has_value());
    s.common_flags = {"--bind"}; // no value: what it meant is unknown, so it isn't guessed
    CHECK(error() == "[Main] common_flags' --bind isn't read any more: write a bind line in [Main] (grab config) and take "
                     "it out of common_flags. Until then grab doesn't connect to Main rather than connect from another "
                     "address");
    s.common_flags = {"--bind", "-oProxyCommand=x"}; // never quoted as a line to write
    CHECK(error().starts_with("[Main] common_flags' --bind isn't read any more: write a bind line in [Main]"));
    s.common_flags.clear();
    s.folder_flags = {"--transfers", "8", "--bind", "fe80::1%eth0"};
    CHECK(error().starts_with("[Main] folder_flags' --bind fe80::1%eth0 isn't read any more: write bind = fe80::1%eth0 "));
    s.folder_flags.clear();
    for (const auto& [options, shown] :
         {std::pair{std::vector<std::string>{"-o", "BindAddress=10.0.0.4"}, "-o BindAddress=10.0.0.4"},
          std::pair{std::vector<std::string>{"-oBindAddress 10.0.0.4"}, "-o BindAddress=10.0.0.4"},
          std::pair{std::vector<std::string>{"-C", "-b", "10.0.0.4", "-o", "BindAddress=10.0.0.5"}, "-b 10.0.0.4"}}) {
        s.ssh_options = options;
        CHECK_MESSAGE(error().starts_with(std::format("[Main] ssh_options' {} isn't read any more: write bind = 10.0.0.4 "
                                                      "in [Main] (grab config) and take it out of ssh_options.",
                                                      shown)),
                      shown);
    }
    s.ssh_options = {"-o", "BindAddress="};
    CHECK(engine::connect_settings(s).has_value());
    // file_flags' --bind reached no connection since 0.20: a notice only.
    s.ssh_options.clear();
    s.file_flags = {"--bind", "10.0.0.6"};
    CHECK(engine::connect_settings(s).has_value());
    // With a `bind` key the old ones are ignored (a notice says so).
    s.common_flags = {"--bind", "10.0.0.2"};
    s.bind = "10.0.0.7";
    const auto keyed = engine::connect_settings(s);
    REQUIRE(keyed);
    CHECK(keyed->bind == "10.0.0.7");
    s.bind.reset();
    // The command's own --bind doesn't lift it: the server's setting is what's wrong.
    CHECK_FALSE(engine::connect_settings(s, ConnectOptions{.bind = "10.0.0.8", .connect_timeout = {}, .io_timeout = {}}));

    // Wherever grab would connect: its target is refused before the host key is even looked at,
    // so a search, a file list and Browse fail with nothing dialled (Trust and Test: server_ops).
    std::filesystem::remove(grab_known_hosts_path());
    auto ctx = grab_context("Main", connection("127.0.0.1", "u", server_conf::Auth::agent, 9), {}, false);
    ctx.settings.common_flags = {"--bind", "10.0.0.2"};
    const std::string refusal = "[Main] common_flags' --bind 10.0.0.2 isn't read any more";
    CHECK(engine::ssh_target(ctx).error_or("").starts_with(refusal));
    engine::SearchRequest req;
    req.target = "x";
    CHECK(engine::search(ctx, req).error_or("").starts_with(refusal));
    CHECK(file_list::fetch(ctx, 1).error_or("").starts_with(refusal));
    const auto listed = browse::list_live(ctx, "", {});
    REQUIRE_FALSE(listed);
    CHECK(listed.error().message.starts_with(refusal));
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

TEST_CASE("the guard's other sources: a grab.conf line, common_flags, an inherited variable") {
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
