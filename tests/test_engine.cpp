#include "config.hpp"
#include "engine.hpp"
#include "listing.hpp"
#include "passwords.hpp"
#include "rclone.hpp"
#include "remote.hpp"
#include "secrets.hpp"
#include "server_conf.hpp"
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

TEST_CASE("download_argv uses the remote's flags and DEST\\<name>") {
    engine::Context ctx;
    ctx.config.rclone = "rclone";
    ctx.settings.rclone_remote = "hetzner";
    ctx.settings.common_flags = {"-P"};
    ctx.settings.file_flags = {"--multi-thread-streams", "8"};
    ctx.settings.folder_flags = {"--transfers", "8"};
    const auto dest = util::path_from_utf8("E:\\TV");

    CHECK(engine::download_argv(ctx, Mode::file, "/srv/tv/Lioness.S03E08.mkv", dest).value() ==
          std::vector<std::string>{"rclone", "copyto", "hetzner:/srv/tv/Lioness.S03E08.mkv",
                                   util::path_to_utf8(dest / "Lioness.S03E08.mkv"), "--config",
                                   std::string(config_in_memory), "-P", "--multi-thread-streams", "8"});
    const std::vector<std::string> extra{"--bwlimit", "10M"};
    CHECK(engine::download_argv(ctx, Mode::folder, "/srv/tv/show", dest, extra).value() ==
          std::vector<std::string>{"rclone", "copy", "hetzner:/srv/tv/show", util::path_to_utf8(dest / "show"),
                                   "--config", std::string(config_in_memory), "-P", "--transfers", "8", "--bwlimit",
                                   "10M"});
}

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
    std::filesystem::remove(grab_known_hosts_path());
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
                                    {"pem", {{"key_pem", fake("-----BEGIN FAKE KEY-----", pem_c)}}},
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
    // The user's own RCLONE_CONFIG_MAIN_* can't add to it (--rclone, whose environment is whole).
    CHECK(main->without_prefix == std::vector<std::string>{"RCLONE_CONFIG_MAIN_"});

    // What grab.conf doesn't know yet is "none", so rclone asks nothing on every run (chunk B's
    // review note); a password obscured as rclone takes it, which rclone's reveal gives back.
    const auto pw = engine::rclone_env(grab_context("pw", pw_c, store));
    REQUIRE_MESSAGE(pw, pw.error_or(""));
    for (const char* key : {"SHELL_TYPE", "MD5SUM_COMMAND", "SHA1SUM_COMMAND"}) {
        CHECK(var(*pw, std::string("RCLONE_CONFIG_PW_") + key) == "none");
    }
    // The --rclone copy leaves them to rclone, whose --verify needs a sum command; known ones
    // still go.
    const auto pw_copy = engine::rclone_env(grab_context("pw", pw_c, store), true);
    REQUIRE_MESSAGE(pw_copy, pw_copy.error_or(""));
    for (const char* key : {"SHELL_TYPE", "MD5SUM_COMMAND", "SHA1SUM_COMMAND"}) {
        CHECK_FALSE(var(*pw_copy, std::string("RCLONE_CONFIG_PW_") + key).has_value());
    }
    const auto main_copy = engine::rclone_env(ctx, true);
    REQUIRE_MESSAGE(main_copy, main_copy.error_or(""));
    CHECK(var(*main_copy, "RCLONE_CONFIG_MAIN_MD5SUM_COMMAND") == "md5sum");
    CHECK(var(*main_copy, "RCLONE_CONFIG_MAIN_SHA1SUM_COMMAND") == "none");
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

    // A flag that would replace the server is refused for grab's own runs, not for --rclone's.
    ctx.settings.common_flags = {"-P", "--sftp-chunk-size", "255Ki", "--sftp-known-hosts-file", "C:\\other"};
    const auto refused = engine::rclone_env(ctx);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().starts_with(
        "[Main] common_flags: --sftp-known-hosts-file would replace the server's trusted host keys"));
    CHECK(engine::rclone_env(ctx, true).has_value());
#ifdef _WIN32
    // A store that can't be read: a password server can't log in; the others still can.
    std::ofstream(store, std::ios::binary) << "grab-passwords-sealed 1\nanother account's";
    CHECK(engine::rclone_env(grab_context("pw", pw_c, store)).error_or("").find("Enter the password of pw again") !=
          std::string::npos);
    ctx.settings.common_flags.clear();
    CHECK(engine::rclone_env(ctx).has_value());
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
