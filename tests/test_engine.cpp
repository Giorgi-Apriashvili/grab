#include "config.hpp"
#include "engine.hpp"
#include "fetch.hpp"
#include "gui_state.hpp"
#include "listing.hpp"
#include "passwords.hpp"
#include "rclone.hpp"
#include "remote.hpp"
#include "secrets.hpp"
#include "server_conf.hpp"
#include "servers.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (SetEnvironmentVariableW, GetEnvironmentStringsW)
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace grab;

namespace {
// Captured from rclone v1.75.1 --use-json-log --stats 500ms --stats-log-level NOTICE
// (paths shortened). Early line: overall speed 0 and eta null, per-transfer speed set.
constexpr const char* early_stats =
    R"({"time":"2026-09-23T19:25:29.2559984+04:00","level":"notice","msg":"\nTransferred:   \t    1.027 MiB / 6 MiB, 17%, 0 B/s, ETA -\n","stats":{"bytes":1077248,"checks":0,"deletedDirs":0,"deletes":0,"elapsedTime":0.4980637,"errors":0,"eta":null,"fatalError":false,"listed":0,"renames":0,"retryError":false,"serverSideCopies":0,"serverSideCopyBytes":0,"serverSideMoveBytes":0,"serverSideMoves":0,"speed":0,"totalBytes":6291456,"totalChecks":0,"totalTransfers":1,"transferTime":0.4980637,"transferring":[{"bytes":1077248,"dstFs":"//?/C:/out","eta":null,"group":"global_stats","name":"src.bin","percentage":17,"size":6291456,"speed":2175986.9171889247,"speedAvg":0,"srcFs":"//?/C:/src"}],"transfers":0},"source":"accounting/stats.go:549"})";

constexpr const char* final_stats =
    R"({"time":"2026-09-23T19:25:31.7480806+04:00","level":"notice","msg":"\nTransferred:   \t        6 MiB / 6 MiB, 100%, 2.014 MiB/s, ETA 0s\n","stats":{"bytes":6291456,"checks":0,"deletedDirs":0,"deletes":0,"elapsedTime":2.9916528,"errors":0,"eta":0,"fatalError":false,"listed":0,"renames":0,"retryError":false,"speed":2111488,"totalBytes":6291456,"totalChecks":0,"totalTransfers":1,"transferTime":2.9916528,"transfers":1},"source":"accounting/stats.go:549"})";

constexpr const char* error_line =
    R"({"time":"2026-09-23T19:25:31.8700388+04:00","level":"error","msg":"Attempt 1/3 failed with 1 errors and: directory not found","source":"cmd/cmd.go:283"})";
} // namespace

TEST_CASE("stats line early in a transfer: speed and eta derived from the running file") {
    auto p = engine::parse_stats_line(early_stats);
    REQUIRE(p.has_value());
    CHECK(p->bytes == 1077248);
    CHECK(p->total == 6291456);
    CHECK(p->speed == doctest::Approx(2175986.917));
    REQUIRE(p->eta.has_value());
    CHECK(*p->eta == doctest::Approx((6291456.0 - 1077248.0) / 2175986.917));
    CHECK(p->current == "src.bin");
}

TEST_CASE("final stats line: overall speed and eta as reported, nothing transferring") {
    auto p = engine::parse_stats_line(final_stats);
    REQUIRE(p.has_value());
    CHECK(p->bytes == p->total);
    CHECK(p->speed == doctest::Approx(2111488));
    REQUIRE(p->eta.has_value());
    CHECK(*p->eta == 0);
    CHECK(p->current.empty());
}

TEST_CASE("other lines are not stats; error lines yield their message") {
    CHECK_FALSE(engine::parse_stats_line(error_line).has_value());
    CHECK_FALSE(engine::parse_stats_line("plain text").has_value());
    CHECK_FALSE(engine::parse_stats_line(R"({"stats": 5})").has_value());
    CHECK(engine::parse_error_line(error_line) ==
          "Attempt 1/3 failed with 1 errors and: directory not found");
    CHECK_FALSE(engine::parse_error_line(final_stats).has_value());
    CHECK_FALSE(engine::parse_error_line("{\"level\": broken").has_value());
}

TEST_CASE("rclone's version, and the oldest grab works with (G190)") {
    CHECK(engine::parse_rclone_version("rclone v1.75.1\n- os/version: Microsoft Windows 11\n") ==
          engine::RcloneVersion{1, 75, 1});
    CHECK(engine::parse_rclone_version("rclone v1.66.0-DEV\n") == engine::RcloneVersion{1, 66, 0});
    CHECK(engine::parse_rclone_version("rclone v1.70") == engine::RcloneVersion{1, 70, 0});
    CHECK_FALSE(engine::parse_rclone_version("not rclone").has_value());
    CHECK(engine::RcloneVersion{1, 66, 9} < engine::min_rclone);
    CHECK_FALSE(engine::RcloneVersion{1, 67, 0} < engine::min_rclone);
    CHECK_FALSE(engine::RcloneVersion{2, 0, 0} < engine::min_rclone);
}

#ifdef _WIN32
TEST_CASE("an old rclone that isn't the bundled one is refused with a reason (G190)") {
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-old-rclone";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream(dir / "old-rclone.cmd") << "@echo rclone v1.66.0\r\n";
        std::ofstream(dir / "new-rclone.cmd") << "@echo rclone v1.75.1\r\n";
    }
    const auto old = engine::check_rclone(util::path_to_utf8(dir / "old-rclone.cmd"), dir / "bin", true);
    REQUIRE_FALSE(old.has_value());
    CHECK(old.error().find("rclone 1.66.0, too old for grab, which needs 1.67 or newer") != std::string::npos);
    CHECK(old.error().find("Remove `rclone =`") != std::string::npos);
    CHECK(engine::check_rclone(util::path_to_utf8(dir / "new-rclone.cmd"), dir / "bin", true).has_value());
    // The bundled one is never asked.
    CHECK(engine::check_rclone(util::path_to_utf8(dir / "rclone.exe"), dir, false).has_value());
    std::filesystem::remove_all(dir);
}
#endif

TEST_CASE("rclone's temp files: only those of the run's files, made by the run") {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "grab_test_partials";
    fs::remove_all(dir);
    fs::create_directories(dir / "sub");
    auto touch = [](const fs::path& p) { std::ofstream(p) << "x"; };
    const std::string long_name = std::string(120, 'L') + ".bin"; // cut to its length, as rclone 1.75.1 does
    const std::vector<fs::path> files{dir / "movie.mkv", dir / "sub" / "e01.mkv", dir / long_name};
    touch(dir / "movie.mkv.0badf00d.partial"); // there before the run: not this run's
    touch(dir / "notes.txt.partial");          // the user's
    touch(dir / "movie.mkv.backup.partial");   // the user's: not rclone's pattern
    touch(dir / "other.mkv.90c84c01.partial"); // not a file of this run
    const auto before = engine::rclone_temp_files(files);
    CHECK(before == std::vector<fs::path>{dir / "movie.mkv.0badf00d.partial"});

    touch(dir / "movie.mkv.37814e9b.partial");
    touch(dir / "sub" / "e01.mkv.90C84C01.partial");
    touch(dir / (std::string(107, 'L') + ".35105d76.partial"));
    touch(dir / "movie.mkv");
    CHECK(engine::rclone_temp_files(files).size() == 4);
    CHECK(engine::remove_rclone_temp_files(files, before, {}) == 3);
    CHECK(fs::exists(dir / "movie.mkv.0badf00d.partial"));
    CHECK(fs::exists(dir / "notes.txt.partial"));
    CHECK(fs::exists(dir / "movie.mkv.backup.partial"));
    CHECK(fs::exists(dir / "other.mkv.90c84c01.partial"));
    CHECK(fs::exists(dir / "movie.mkv"));
    CHECK_FALSE(fs::exists(dir / "movie.mkv.37814e9b.partial"));
    CHECK_FALSE(fs::exists(dir / "sub" / "e01.mkv.90C84C01.partial"));

    const std::vector<fs::path> missing{dir / "missing" / "x.bin"};
    CHECK(engine::rclone_temp_files(missing).empty()); // no folder: no throw
    fs::remove_all(dir);
}

TEST_CASE("rclone's temp files: never a file of the folder named like one") {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "grab_test_partials_own";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto touch = [](const fs::path& p) { std::ofstream(p) << "x"; };
    // The server folder holds both; the batch downloads both.
    const std::vector<fs::path> files{dir / "notes.txt", dir / "notes.txt.12345678.partial"};
    const auto before = engine::rclone_temp_files(files);
    CHECK(before.empty());
    touch(dir / "notes.txt");
    touch(dir / "notes.txt.12345678.partial");                  // downloaded: the folder's own file
    touch(dir / "notes.txt.90c84c01.partial");                  // rclone's, left by a stop
    touch(dir / "notes.txt.12345678.partial.0badf00d.partial"); // rclone's for the second file
    const auto own = [&](const fs::path& p) { return std::ranges::find(files, p) != files.end(); };
    CHECK(engine::remove_rclone_temp_files(files, before, own) == 2);
    CHECK(fs::exists(dir / "notes.txt.12345678.partial"));
    CHECK_FALSE(fs::exists(dir / "notes.txt.90c84c01.partial"));
    CHECK_FALSE(fs::exists(dir / "notes.txt.12345678.partial.0badf00d.partial"));
    fs::remove_all(dir);
}

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

TEST_CASE("live searches return dates: find over ssh and rclone lsf") {
    engine::Context ctx;
    engine::SearchRequest req;
    req.target = "lioness";
    req.mode = Mode::file;
    const std::string find_out = std::string("200\t1697354553.25\t./tv/Lioness.S03E02.mkv") + '\0';
    const auto by_find = engine::make_result(ctx, req, parse_find_output(find_out, true));
    REQUIRE(by_find.hits.size() == 1);
    CHECK(by_find.hits[0].path == "tv/Lioness.S03E02.mkv");
    CHECK(by_find.hits[0].size == 200ULL);
    CHECK(by_find.hits[0].modified == 1697354553);

    ListRequest list;
    list.mode = Mode::file;
    list.target = "lioness";
    list.root = "/home";
    const auto by_lsf = engine::make_result(ctx, req, parse_lsf_output(list, "1697354554,300,tv/Lioness.S03E08.mkv\n"));
    REQUIRE(by_lsf.hits.size() == 1);
    CHECK(by_lsf.hits[0].path == "/home/tv/Lioness.S03E08.mkv");
    CHECK(by_lsf.hits[0].size == 300ULL);
    CHECK(by_lsf.hits[0].modified == 1697354554);

    req.mode = Mode::folder;
    list.mode = Mode::folder;
    const auto folder = engine::make_result(ctx, req, parse_lsf_output(list, "1697000000,-1,tv/Lioness/\n"));
    REQUIRE(folder.hits.size() == 1);
    CHECK_FALSE(folder.hits[0].size.has_value());
    CHECK(folder.hits[0].modified == 1697000000);
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

using Var = std::pair<std::string, std::string>;
using Vars = std::vector<Var>;

std::optional<std::string> var(const proc::Environment& env, std::string_view name) {
    for (const auto& [n, v] : env.set) {
        if (n == name) return v;
    }
    return std::nullopt;
}

} // namespace

#ifdef _WIN32
TEST_CASE("a search for folders and files walks the server once over rclone (G197)") {
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-search-both";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    // A stand-in rclone: logs each call and its arguments, prints a folder and a file below it.
    std::ofstream(dir / "rclone.cmd", std::ios::binary)
        << "@echo off\r\n"
           ">> \"%~dp0calls.log\" echo %*\r\n" // (redirect first: "...connections 3>>" would be handle 3)
           "echo 1697000000,-1,tv/Lioness/\r\n"
           "echo 1697354554,300,tv/Lioness/Lioness.S03E08.mkv\r\n";
    auto ctx = grab_context("Box", connection("box.example", "u", server_conf::Auth::agent));
    ctx.engine = Engine::rclone; // the fallback: grab's own client is the default
    ctx.config.rclone = util::path_to_utf8(dir / "rclone.cmd");
    ctx.method = FindMethod::rclone;
    engine::SearchRequest req;
    req.target = "lioness";
    req.walk_flags = {"--checkers", "3", "--sftp-connections", "3"};
    const auto both = engine::search_both(ctx, req);
    REQUIRE_MESSAGE(both.has_value(), both.error_or(""));
    REQUIRE(both->first.hits.size() == 1);
    CHECK(both->first.hits[0].path == "tv/Lioness");
    REQUIRE(both->second.hits.size() == 1);
    CHECK(both->second.hits[0].path == "tv/Lioness/Lioness.S03E08.mkv");
    const auto calls = util::split(util::read_file(dir / "calls.log").value_or(""), '\n');
    REQUIRE(calls.size() == 1); // one walk for both kinds
    CHECK(calls[0].find("--dirs-only") == std::string::npos);
    CHECK(calls[0].find("--files-only") == std::string::npos);
    CHECK(calls[0].find("--checkers 3 --sftp-connections 3") != std::string::npos);
    std::filesystem::remove_all(dir);
    std::filesystem::remove(grab_known_hosts_path());
}
#endif
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
    // A --bind that isn't an address never reaches a program (ssh.exe would take "-o..." for an option).
    s.bind.reset();
    s.common_flags = {"--bind=-oProxyCommand=x"};
    CHECK(engine::connect_settings(s).error_or("") == "[Main] common_flags: --bind -oProxyCommand=x: the address can't start with -");
    s.common_flags = {"--contimeout", "soon"};
    CHECK(engine::connect_settings(s).error_or("").starts_with("[Main] common_flags: --contimeout soon: not a time"));
    s.common_flags.clear();
    const std::vector<std::string> bad{"--bind=10.0.0.2;x"};
    CHECK(engine::connect_settings(s, bad).error_or("").starts_with("the flags after --: --bind 10.0.0.2;x: "));
    // A blank --bind is no bind, as rclone reads it: in common_flags nothing, after `--` it
    // clears the server's for the run.
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

TEST_CASE("the keys on rclone's and ssh.exe's command lines: only when set, after grab's fixed values") {
    using std::chrono::milliseconds;
    RemoteSettings s;
    CHECK(engine::key_flags(s).empty()); // blank keys: rclone keeps its own times
    s.bind = "10.0.0.2";
    s.connect_timeout = milliseconds(1'500);
    s.io_timeout = milliseconds(30'000);
    CHECK(engine::key_flags(s) == std::vector<std::string>{"--bind", "10.0.0.2", "--contimeout", "1500ms", "--timeout", "30s"});
    s.connect_timeout = milliseconds(0);
    CHECK(engine::key_flags(s)[3] == "0s");

    // ssh.exe: whole seconds rounded up, nothing for off.
    engine::ConnectSettings reach;
    CHECK(engine::ssh_connect_options(reach).empty());
    reach.bind = "10.0.0.2";
    reach.connect_timeout = milliseconds(500);
    CHECK(engine::ssh_connect_options(reach) ==
          std::vector<std::string>{"-o", "BindAddress=10.0.0.2", "-o", "ConnectTimeout=1"});
    reach.connect_timeout = milliseconds(1'500);
    CHECK(engine::ssh_connect_options(reach).back() == "ConnectTimeout=2");
    reach.connect_timeout = milliseconds(0);
    CHECK(engine::ssh_connect_options(reach) == std::vector<std::string>{"-o", "BindAddress=10.0.0.2"});

    // fetch's stat and cat: common_flags', grab's fixed times, then the keys and the command's,
    // which win (rclone takes the last of a repeated flag).
    const fetch::Source src{.rclone = "rclone",
                            .environment = {},
                            .rclone_remote = "Main",
                            .path = "a/b",
                            .flags = {"--bind", "10.0.0.1", "--timeout", "9m"},
                            .after = {"--timeout", "5m", "--contimeout", "1s"}};
    for (const auto& argv : {fetch::stat_argv(src), fetch::cat_argv(src, 0, 10)}) {
        const auto at = [&](std::string_view v) { return std::ranges::find(argv, v) - argv.begin(); };
        CHECK(at("9m") < at("60s"));
        CHECK(at("60s") < at("5m"));
        CHECK(at("15s") < at("1s"));
    }
    // Test's probe: common_flags' connection flags before its short times, the keys after them.
    const std::vector<std::string> before{"--bind", "10.0.0.1"};
    const std::vector<std::string> after{"--contimeout", "2s"};
    const auto probe = servers::probe_argv("rclone", "box", before, after);
    const auto at = [&](std::string_view v) { return std::ranges::find(probe, v) - probe.begin(); };
    CHECK(at("10.0.0.1") < at("15s"));
    CHECK(at("15s") < at("2s"));
    CHECK(probe.back() == config_in_memory);
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

TEST_CASE("the guard: a server whose settings route it is refused by grab's client and searched by rclone's listing") {
    using std::chrono::milliseconds;
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-guard";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto conf = dir / "grab.conf";
    const auto store = passwords::path_in(state_dir(conf));
    std::filesystem::create_directories(store.parent_path());
    std::filesystem::remove(grab_known_hosts_path());
    std::filesystem::create_directories(grab_known_hosts_path().parent_path());
    std::ofstream(grab_known_hosts_path(), std::ios::binary) << "box.example ssh-ed25519 FAKEKEY\n";
    const auto c = connection("box.example", "u", server_conf::Auth::agent);
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

    // No `engine` line: the guard keeps it on rclone by itself (phase 8 step 8).
    const auto ctx = engine::make_context(GrabConfig{}, s, c, conf);
    CHECK(ctx.engine == Engine::rclone);
    CHECK(ctx.method == FindMethod::rclone); // an agent server: ssh + find, but ssh.exe would ignore the proxy
    const auto refused = engine::ssh_target(ctx);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().starts_with("[Box] connects through a SOCKS proxy (rclone.socks_proxy), which grab's own client "
                                      "can't: its downloads go through rclone, and its searches and lists by rclone's "
                                      "listing; previews, Play and Verify are off for it"));
    CHECK(refused.error().find("fake-") == std::string::npos);
    // rclone still gets it: its runs go through the proxy.
    const auto env = engine::rclone_env(ctx);
    REQUIRE_MESSAGE(env, env.error_or(""));
    CHECK(var(*env, "RCLONE_CONFIG_BOX_SOCKS_PROXY") == "fake-user:fake-pass@127.0.0.1:9");
    // engine = grab: refused where it's used, never at load, saying to set engine = rclone.
    s.engine = Engine::grab;
    const auto on_grab = engine::make_context(GrabConfig{}, s, c, conf);
    CHECK(engine::ssh_target(on_grab).error_or("").find("set engine = rclone in [Box] (grab config)") != std::string::npos);

    // ssh_options' ProxyCommand: grab's client refused, ssh.exe find kept (it goes through it).
    const auto plain_conf = dir / "plain" / "grab.conf";
    s.engine.reset();
    s.ssh_options = {"-o", "ProxyCommand=fake-proxy %h %p"};
    const auto proxied = engine::make_context(GrabConfig{}, s, c, plain_conf);
    CHECK(proxied.engine == Engine::rclone); // any reason keeps it on rclone, ssh.exe's own too
    CHECK(proxied.method == FindMethod::ssh);
    CHECK(engine::ssh_target(proxied).error_or("").find("connects through a ProxyCommand (ssh_options' -o ProxyCommand)") !=
          std::string::npos);
    // A guard flag after `--` on a server that has none.
    s.ssh_options.clear();
    const auto plain = engine::make_context(GrabConfig{}, s, c, plain_conf);
    CHECK(plain.guard.empty());
    CHECK(plain.engine == Engine::grab); // the default
    CHECK(engine::ssh_target(plain).has_value());
    const std::vector<std::string> run{"--sftp-http-proxy", "fake-proxy:3128"};
    CHECK(engine::grab_blockers(plain, run).size() == 1);
    CHECK(engine::ssh_target(plain, run).error_or("").find("(--sftp-http-proxy after --)") != std::string::npos);
#ifdef _WIN32
    // A store that exists but can't be read may hold a route: the guard fails closed, for an
    // agent login too (grab's client refuses, ssh.exe find isn't used), and so does rclone.
    std::ofstream(store, std::ios::binary | std::ios::trunc) << "grab-passwords-sealed 1\nanother account's";
    const auto unreadable = engine::make_context(GrabConfig{}, s, c, conf);
    CHECK(unreadable.method == FindMethod::rclone);
    const auto direct = engine::ssh_target(unreadable).error_or("");
    CHECK(direct.starts_with("[Box] may connect through a proxy or an ssh command (grab's store, which can't be read), "
                             "which grab's own client can't; grab doesn't start rclone for it either until the store "
                             "can be read; saving the server with Edit (no change needed) sets the unreadable file "
                             "aside"));
    CHECK(engine::rclone_env(unreadable).error_or("").find("so grab doesn't start rclone for it") != std::string::npos);
#endif
    std::filesystem::remove_all(store.parent_path());
    std::filesystem::remove_all(dir);
    std::filesystem::remove(grab_known_hosts_path());
}

TEST_CASE("grab's own client names the fallback where it downloads (phase 8, decision 3: grab.conf only)") {
    std::filesystem::remove(grab_known_hosts_path());
    auto ctx = grab_context("Main", connection("main.example", "u", server_conf::Auth::agent));
    CHECK(ctx.engine == Engine::grab); // no `engine` line, nothing guarded: the default
    const auto on_grab = engine::ssh_target(ctx);
    REQUIRE_MESSAGE(on_grab, on_grab.error_or(""));
    CHECK(on_grab->fallback == "; for Main's downloads, searches and lists to go through rclone until a later grab "
                               "removes it, set engine = rclone in [Main] (grab config, or Open grab.conf in grab's "
                               "Settings)");
    // On rclone, grab's client only previews, plays and verifies: there's nothing to fall back to.
    ctx.engine = Engine::rclone;
    const auto on_rclone = engine::ssh_target(ctx);
    REQUIRE_MESSAGE(on_rclone, on_rclone.error_or(""));
    CHECK(on_rclone->fallback.empty());
    std::filesystem::remove(grab_known_hosts_path());
}

#ifdef _WIN32
TEST_CASE("the keys and the command's flags reach rclone's listing and ssh.exe, after grab's own") {
    using std::chrono::milliseconds;
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-keys-argv";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "rclone.cmd", std::ios::binary) << "@echo off\r\n>> \"%~dp0calls.log\" echo %*\r\n";
    std::ofstream(dir / "ssh.cmd", std::ios::binary) << "@echo off\r\n";
    auto ctx = grab_context("Box", connection("box.example", "u", server_conf::Auth::agent));
    ctx.engine = Engine::rclone; // the fallback, whose runs these are
    ctx.config.rclone = util::path_to_utf8(dir / "rclone.cmd");
    ctx.config.ssh = util::path_to_utf8(dir / "ssh.cmd");
    ctx.settings.bind = "10.0.0.2";
    ctx.settings.connect_timeout = milliseconds(1'500);
    ctx.settings.common_flags = {"--contimeout", "9s"};
    ctx.settings.ssh_options = {"-o", "ConnectTimeout=99"};
    engine::SearchRequest req;
    req.target = "x";
    req.walk_flags = {"--sftp-connections", "3"};
    req.run_flags = {"--timeout", "1m"};
    ctx.method = FindMethod::rclone;
    REQUIRE(engine::search(ctx, req));
    const auto calls = util::read_file(dir / "calls.log").value_or("");
    CHECK(calls.find("--contimeout 9s --sftp-connections 3 --bind 10.0.0.2 --contimeout 1500ms --timeout 1m") !=
          std::string::npos);
    // ssh.exe: the key's (or the command's) bind and time first, then the server's ssh_options.
    ctx.method = FindMethod::ssh;
    std::vector<std::string> ran;
    req.run_flags = {"--bind", "10.0.0.9"};
    REQUIRE(engine::search(ctx, req, {}, [&](const std::vector<std::string>& argv) { ran = argv; }));
    const auto at = [&](std::string_view v) { return std::ranges::find(ran, v) - ran.begin(); };
    CHECK(at("BindAddress=10.0.0.9") < at("ConnectTimeout=2"));
    CHECK(at("ConnectTimeout=2") < at("ConnectTimeout=99"));
    CHECK(std::ranges::find(ran, "BindAddress=10.0.0.2") == ran.end());
    std::filesystem::remove_all(dir);
    std::filesystem::remove(grab_known_hosts_path());
}
#endif

TEST_CASE("the flags after --, per engine, before anything connects (phase 8 step 6)") {
    using V = std::vector<std::string>;
    engine::Context ctx;
    ctx.settings.name = "Main";
    ctx.engine = Engine::grab;
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
    // What only rclone can follow says to set engine = rclone, and never quotes the value.
    const auto proxy = error({"--sftp-socks-proxy", "fake-user:fake-secret@127.0.0.1:9"});
    CHECK(proxy == "--sftp-socks-proxy after --: grab's own client can't use a proxy; set engine = rclone in [Main] "
                   "(grab config) to use it through rclone");
    CHECK(error({"--sftp-ciphers=aes128-ctr"}).find("offers only its own algorithms; set engine = rclone in [Main]") !=
          std::string::npos);
    CHECK(error({"--sftp-disable-concurrent-reads"}).find("set engine = rclone") != std::string::npos);
    // The server's address, login and host keys, as before.
    CHECK(error({"--sftp-host", "elsewhere.example"}) ==
          "--sftp-host after -- would replace the server's address, which grab's own client takes from its settings; "
          "change grab.conf's `host` with `grab server edit Main` or Edit in grab's Settings");
    CHECK(error({"--sftp-ssh", "ssh -i key"}).ends_with("which grab's own client can't use; an ssh command can't be "
                                                         "given there"));
    CHECK(error({"--sftp-pubkey-file", "C:\\id-cert.pub"}) ==
          "--sftp-pubkey-file after -- would replace the server's certificate, which grab's own client can't use; "
          "write `rclone.pubkey_file = PATH` in [Main] (grab config); only rclone logs in with a certificate "
          "(engine = rclone)");
    // Values grab's own client can't use: refused now, not when it connects.
    CHECK(error({"--timeout", "off"}) == "the flags after --: --timeout off: grab's own client takes a time from 1s to "
                                         "24h there (with none, a stalled download couldn't be cancelled)");
    CHECK(error({"--timeout=0.5s"}).starts_with("the flags after --: --timeout 0.5s: grab's own client takes"));
    CHECK(error({"--timeout", "25h"}).starts_with("the flags after --: --timeout 25h:"));
    CHECK(error({"--timeout", "abc"}).starts_with("the flags after --: --timeout abc: not a time grab can read"));
    CHECK(error({"--bind"}) == "the flags after --: --bind needs a value");
    CHECK(error({"--bind=-x"}) == "the flags after --: --bind -x: the address can't start with -");

    // engine = rclone: connection flags, as before (a guard flag makes the run guarded instead).
    ctx.engine = Engine::rclone;
    CHECK(engine::check_run_flags(ctx, V{"--sftp-chunk-size", "1Mi", "--bind=10.8.0.2", "--timeout", "off",
                                         "--sftp-socks-proxy", "127.0.0.1:9", "--sftp-connections", "4"})
              .has_value());
    CHECK(error({"--dry-run"}) == "--dry-run after -- isn't used on [Main] (engine = rclone), whose rclone runs take "
                                  "only connection flags (--sftp-*, --bind, --timeout...): grab's own -n shows what "
                                  "would be fetched");
    CHECK(error({"--exclude", "*.part"}).ends_with("(--sftp-*, --bind, --timeout...)")); // nothing instead
    CHECK(error({"--sftp-user", "u"}) == "--sftp-user after -- would replace the server's user, which grab gives rclone "
                                         "itself; change grab.conf's `user` with `grab server edit Main` or Edit in "
                                         "grab's Settings");
    for (const auto& e : {error({"--exclude", "*.part"}), error({"-vP"})}) CHECK(e.find("--rclone") == std::string::npos);
    // Their values too, before DEST is made; "off" and a blank --bind stay rclone's to take.
    CHECK(error({"--timeout", "abc"}).starts_with("the flags after --: --timeout abc: not a time grab can read"));
    CHECK(error({"--bind=-x"}) == "the flags after --: --bind -x: the address can't start with -");
    CHECK(error({"--bind"}) == "the flags after --: --bind needs a value");
    CHECK(engine::check_run_flags(ctx, V{"--timeout", "off", "--bind="}).has_value());
}

TEST_CASE("an unpinned server is refused before any rclone run, grab's login or ssh: use Trust") {
    std::filesystem::remove(grab_known_hosts_path());
    const auto ctx = grab_context("Main", connection("main.example", "me", server_conf::Auth::agent, 2222), {}, false);
    for (const auto& error : {engine::check_pinned(ctx).error_or(""), engine::rclone_env(ctx).error_or(""),
                              engine::ssh_target(ctx).error_or(""),
                              engine::run_over_ssh(ctx, "echo x", true).error_or("")}) {
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

TEST_CASE("rclone_env: a server's grab.conf section and its saved secrets as its own run's RCLONE_CONFIG_<NAME>_* (phase 7)") {
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-rclone-env-block";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::filesystem::remove(grab_known_hosts_path());
    const auto store = dir / "passwords";
    const std::string kh = util::path_to_utf8(grab_known_hosts_path());

    // An agent server with its key file, what rclone found there, an allowlisted setting.
    auto main_c = connection("main.example", "me", server_conf::Auth::agent, 2222);
    main_c.key_file = "C:\\keys\\id_main";
    main_c.shell_type = "unix";
    main_c.md5sum_command = "md5sum";
    main_c.sha1sum_command = "none";
    main_c.rclone = {{"ciphers", "aes256-ctr"}};
    const auto pw_c = connection("pw.example", "u", server_conf::Auth::password);
    const auto key_c = [] {
        auto c = connection("k.example", "u", server_conf::Auth::key);
        c.key_file = "C:\\keys\\id_k";
        return c;
    }();
    const auto pem_c = connection("pem.example", "u", server_conf::Auth::stored_key);
    REQUIRE(passwords::save(store, {{"Main", {{"rclone.set_env", fake("FAKE_VAR=1", main_c)}}},
                                    {"pw", {{"password", fake("fake-pw", pw_c)}}},
                                    {"k", {{"passphrase", fake("fake-phrase", key_c)}, {"password", fake("fake-beside", key_c)}}},
                                    {"pem", {{"key_pem", fake("-----BEGIN FAKE KEY-----", pem_c)},
                                             {"passphrase", fake("fake-pem-phrase", pem_c)}}},
                                    {"moved", {{"password", fake("fake-pw", pw_c)}}}}));

    auto ctx = grab_context("Main", main_c, store);
    const auto main = engine::rclone_env(ctx);
    REQUIRE_MESSAGE(main, main.error_or(""));
    CHECK(main->set == Vars{{"RCLONE_CONFIG_MAIN_CIPHERS", "aes256-ctr"},
                            {"RCLONE_CONFIG_MAIN_HOST", "main.example"},
                            {"RCLONE_CONFIG_MAIN_KEY_FILE", "C:\\keys\\id_main"},
                            {"RCLONE_CONFIG_MAIN_KEY_USE_AGENT", "true"},
                            {"RCLONE_CONFIG_MAIN_KNOWN_HOSTS_FILE", kh},
                            {"RCLONE_CONFIG_MAIN_MD5SUM_COMMAND", "md5sum"},
                            {"RCLONE_CONFIG_MAIN_PORT", "2222"},
                            {"RCLONE_CONFIG_MAIN_SET_ENV", "FAKE_VAR=1"},
                            {"RCLONE_CONFIG_MAIN_SHA1SUM_COMMAND", "none"},
                            {"RCLONE_CONFIG_MAIN_SHELL_TYPE", "unix"},
                            {"RCLONE_CONFIG_MAIN_TYPE", "sftp"},
                            {"RCLONE_CONFIG_MAIN_USER", "me"}});
    // The user's own RCLONE_CONFIG_MAIN_* can't add to it.
    CHECK(main->without_prefix == std::vector<std::string>{"RCLONE_CONFIG_MAIN_"});

    // What grab.conf doesn't know yet is "none", so rclone asks nothing on every run (chunk B's
    // review note); a password obscured as rclone takes it, which rclone's reveal gives back.
    const auto pw = engine::rclone_env(grab_context("pw", pw_c, store));
    REQUIRE_MESSAGE(pw, pw.error_or(""));
    for (const char* key : {"SHELL_TYPE", "MD5SUM_COMMAND", "SHA1SUM_COMMAND"}) {
        CHECK(var(*pw, std::string("RCLONE_CONFIG_PW_") + key) == "none");
    }
    const auto pass = var(*pw, "RCLONE_CONFIG_PW_PASS");
    REQUIRE(pass.has_value());
    CHECK(*pass != "fake-pw");
    CHECK(secrets::reveal(*pass) == "fake-pw");
    CHECK_FALSE(var(*pw, "RCLONE_CONFIG_PW_KEY_USE_AGENT").has_value());
    // A key file: its passphrase, and the password beside it, which rclone tries after the key.
    const auto k = engine::rclone_env(grab_context("k", key_c, store));
    REQUIRE_MESSAGE(k, k.error_or(""));
    CHECK(var(*k, "RCLONE_CONFIG_K_KEY_FILE") == "C:\\keys\\id_k");
    CHECK(secrets::reveal(var(*k, "RCLONE_CONFIG_K_KEY_FILE_PASS").value_or("")) == "fake-phrase");
    CHECK(secrets::reveal(var(*k, "RCLONE_CONFIG_K_PASS").value_or("")) == "fake-beside");
    // An imported inline key, as it was stored (rclone keeps key_pem as it is).
    const auto pem = engine::rclone_env(grab_context("pem", pem_c, store));
    REQUIRE_MESSAGE(pem, pem.error_or(""));
    CHECK(var(*pem, "RCLONE_CONFIG_PEM_KEY_PEM") == "-----BEGIN FAKE KEY-----");
    // Its passphrase too: rclone reads key_file_pass for key_pem as for a key file.
    CHECK(secrets::reveal(var(*pem, "RCLONE_CONFIG_PEM_KEY_FILE_PASS").value_or("")) == "fake-pem-phrase");
    // A password saved for another login is never sent (G40), nor is one never saved.
    const auto moved = engine::rclone_env(grab_context("moved", connection("evil.example", "u", server_conf::Auth::password), store));
    REQUIRE_FALSE(moved);
    CHECK(moved.error().find("the saved password is for u@pw.example:22") != std::string::npos);
    CHECK(engine::rclone_env(grab_context("none", pw_c, store)).error_or("").find("no password is saved") != std::string::npos);
    // A Georgian name, upper-cased as Go does it (Mtavruli), which rclone looks for.
    const auto ge = engine::rclone_env(grab_context("\u10E1\u10D4\u10E0\u10D5\u10D4\u10E0\u10D8",
                                                    connection("ge.example", "u", server_conf::Auth::agent)));
    REQUIRE(ge);
    CHECK(ge->set.front().first == "RCLONE_CONFIG_\u1CA1\u1C94\u1CA0\u1C95\u1C94\u1CA0\u1C98_HOST");
    CHECK(ge->without_prefix == std::vector<std::string>{"RCLONE_CONFIG_\u1CA1\u1C94\u1CA0\u1C95\u1C94\u1CA0\u1C98_"});

    // A path with a '$' reaches rclone through its own variable: rclone expands the value once,
    // and Go doesn't expand what it substituted.
    auto dollar_c = connection("d.example", "u", server_conf::Auth::key);
    dollar_c.key_file = "C:\\$sync\\id";
    dollar_c.rclone = {{"pubkey_file", "C:\\$sync\\id.pub"}};
    const auto dollar = engine::rclone_env(grab_context("d", dollar_c));
    REQUIRE_MESSAGE(dollar, dollar.error_or(""));
    CHECK(var(*dollar, "RCLONE_CONFIG_D_KEY_FILE") == "${GRAB_RCLONE_KEY_FILE}");
    CHECK(var(*dollar, "GRAB_RCLONE_KEY_FILE") == "C:\\$sync\\id");
    CHECK(var(*dollar, "RCLONE_CONFIG_D_PUBKEY_FILE") == "${GRAB_RCLONE_PUBKEY_FILE}");
    CHECK(var(*dollar, "GRAB_RCLONE_PUBKEY_FILE") == "C:\\$sync\\id.pub");
    CHECK_FALSE(var(*main, "GRAB_RCLONE_KEY_FILE").has_value()); // no '$', no variable

    // A flag that would replace the server is refused.
    ctx.settings.common_flags = {"-P", "--sftp-chunk-size", "255Ki", "--sftp-known-hosts-file", "C:\\other"};
    const auto refused = engine::rclone_env(ctx);
    REQUIRE_FALSE(refused);
    CHECK(refused.error() == "[Main] common_flags: --sftp-known-hosts-file would replace the server's trusted host keys, "
                             "which grab gives rclone itself; take it out (pin its host keys with `grab server trust "
                             "Main` or Trust in grab's Settings)");
    // A common_flags --bind that isn't an address doesn't reach rclone either.
    ctx.settings.common_flags = {"--bind", "-x"};
    CHECK(engine::rclone_env(ctx).error_or("").starts_with("[Main] common_flags: --bind needs a value"));
    ctx.settings.common_flags = {"--bind=a b"};
    CHECK(engine::rclone_env(ctx).error_or("").starts_with("[Main] common_flags: --bind a b: "));
    // A blank one is no bind, as rclone reads it.
    ctx.settings.common_flags = {"--bind="};
    CHECK(engine::rclone_env(ctx).has_value());
#ifdef _WIN32
    // A store that can't be read: a password server can't log in, nor one with a key kept there.
    // Neither starts rclone, nor does any other: the store may keep the server's proxy or ssh
    // command, which rclone would go without (fail closed, as the guard does for grab's client).
    std::ofstream(store, std::ios::binary) << "grab-passwords-sealed 1\nanother account's";
    const auto why = std::format("the saved passwords in {} can't be read (they belong to another Windows account, or "
                                 "the file is damaged)",
                                 util::path_to_utf8(store));
    CHECK(engine::rclone_env(grab_context("pw", pw_c, store)).error_or("") ==
          why + ". Enter the password of pw again with Edit (Settings → Edit in grab's window, or `grab server edit "
                "pw`), which sets the unreadable file aside");
    CHECK(engine::rclone_env(grab_context("pem", pem_c, store)).error_or("") ==
          why + ". pem logs in with a key kept there: choose a key file or ssh-agent for it with Edit (Settings → Edit "
                "in grab's window, or `grab server edit pem`), which sets the unreadable file aside");
    ctx.settings.common_flags.clear();
    for (const auto& unreadable : {ctx, grab_context("k", key_c, store)}) {
        const auto& name = unreadable.settings.name;
        CHECK(engine::rclone_env(unreadable).error_or("") ==
              why + std::format("; it may keep rclone settings of {} (a proxy, an ssh command), so grab doesn't start "
                                "rclone for it. Saving it unchanged with Edit (Settings → Edit in grab's window, "
                                "or `grab server edit {}`) sets the unreadable file aside",
                                name, name));
    }
#endif
    std::filesystem::remove_all(dir);
    std::filesystem::remove(grab_known_hosts_path());
}

#ifdef _WIN32
TEST_CASE("each rclone run gets its own server's variables and no other's; grab keeps none (G7, G84)") {
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-rclone-env-runs";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::filesystem::remove(grab_known_hosts_path());
    const auto store = dir / "passwords";
    const auto pw_c = connection("pw.example", "u", server_conf::Auth::password);
    REQUIRE(passwords::save(store, {{"pw", {{"password", fake("fake-pw", pw_c)}}}}));
    // A stand-in rclone: logs its arguments and the names of the RCLONE_* variables it gets (cmd
    // /u writes them in UTF-16), lists one file.
    std::ofstream(dir / "rclone.cmd", std::ios::binary)
        << "@echo off\r\n"
           ">> \"%~dp0calls.log\" echo %*\r\n"
           "cmd /d /u /c set RCLONE_ > \"%~dp0env.log\"\r\n"
           "echo 1697354554,300,tv/x.mkv\r\n";
    // What the user's own environment may hold: another server's password, an address that would
    // beat the server's own, a tuning variable that stays.
    SetEnvironmentVariableW(L"RCLONE_CONFIG_PW_PASS", L"INHERITED-FAKE");
    SetEnvironmentVariableW(L"rclone_config_main_host", L"inherited.example");
    SetEnvironmentVariableW(L"RCLONE_SFTP_HOST", L"inherited.example");
    SetEnvironmentVariableW(L"RCLONE_SFTP_CHUNK_SIZE", L"64Ki");
    auto env_of = [&](engine::Context ctx) {
        std::filesystem::remove(dir / "env.log");
        ctx.engine = Engine::rclone; // the fallback, whose runs these are
        ctx.config.rclone = util::path_to_utf8(dir / "rclone.cmd");
        ctx.method = FindMethod::rclone;
        engine::SearchRequest req;
        req.target = "x";
        const auto found = engine::search(ctx, req);
        CHECK_MESSAGE(found.has_value(), found.error_or(""));
        const auto raw = util::read_file(dir / "env.log").value_or("");
        std::wstring wide(raw.size() / 2, L'\0');
        std::memcpy(wide.data(), raw.data(), wide.size() * sizeof(wchar_t));
        std::vector<std::string> names;
        for (const auto& line : util::split(util::to_utf8(wide), '\n')) {
            if (const auto t = util::trim(line); !t.empty()) names.emplace_back(t.substr(0, t.find('=')));
        }
        return names;
    };
    const auto main = env_of(grab_context("Main", connection("main.example", "me", server_conf::Auth::agent)));
    const auto pw = env_of(grab_context("pw", pw_c, store));
    const auto ge = env_of(grab_context("\u10E1\u10D4\u10E0\u10D5\u10D4\u10E0\u10D8",
                                        connection("ge.example", "u", server_conf::Auth::agent)));
    SetEnvironmentVariableW(L"RCLONE_CONFIG_PW_PASS", nullptr);
    SetEnvironmentVariableW(L"rclone_config_main_host", nullptr);
    SetEnvironmentVariableW(L"RCLONE_SFTP_HOST", nullptr);
    SetEnvironmentVariableW(L"RCLONE_SFTP_CHUNK_SIZE", nullptr);

    const auto names_of = [](const std::string& prefix, std::vector<std::string> keys) {
        std::vector<std::string> out;
        for (const auto& k : keys) out.push_back(prefix + k);
        out.emplace_back("RCLONE_SFTP_CHUNK_SIZE");
        return out;
    };
    CHECK(main == names_of("RCLONE_CONFIG_MAIN_", {"HOST", "KEY_USE_AGENT", "KNOWN_HOSTS_FILE", "MD5SUM_COMMAND", "PORT",
                                                   "SHA1SUM_COMMAND", "SHELL_TYPE", "TYPE", "USER"}));
    CHECK(pw == names_of("RCLONE_CONFIG_PW_", {"HOST", "KNOWN_HOSTS_FILE", "MD5SUM_COMMAND", "PASS", "PORT",
                                               "SHA1SUM_COMMAND", "SHELL_TYPE", "TYPE", "USER"}));
    CHECK(ge == names_of("RCLONE_CONFIG_\u1CA1\u1C94\u1CA0\u1C95\u1C94\u1CA0\u1C98_",
                         {"HOST", "KEY_USE_AGENT", "KNOWN_HOSTS_FILE", "MD5SUM_COMMAND", "PORT", "SHA1SUM_COMMAND",
                          "SHELL_TYPE", "TYPE", "USER"}));
    // Its config in memory, on every run.
    for (const auto& call : util::split(util::read_file(dir / "calls.log").value_or(""), '\n')) {
        if (!util::trim(call).empty()) CHECK(call.find("--config NUL") != std::string_view::npos);
    }
    // grab's own environment holds no server's variables, for any program it starts (G7).
    LPWCH all = GetEnvironmentStringsW();
    REQUIRE(all != nullptr);
    for (const wchar_t* p = all; *p != L'\0'; p += std::wcslen(p) + 1) {
        CHECK_FALSE(util::to_lower(util::to_utf8(p)).starts_with("rclone_config"));
    }
    FreeEnvironmentStringsW(all);
    std::filesystem::remove_all(dir);
    std::filesystem::remove(grab_known_hosts_path());
}
#endif
