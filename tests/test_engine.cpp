#include "engine.hpp"
#include "listing.hpp"
#include "rclone.hpp"
#include "remote.hpp"
#include "secrets.hpp"
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
    std::ofstream(dir / "rclone.conf", std::ios::binary) << "[Box]\ntype = sftp\nhost = box.example\nuser = u\n";
    engine::Context ctx;
    ctx.config.rclone = util::path_to_utf8(dir / "rclone.cmd");
    ctx.config.rclone_config = dir / "rclone.conf"; // the server rclone_env gives rclone
    ctx.method = FindMethod::rclone;
    ctx.settings.rclone_remote = "Box";
    engine::SearchRequest req;
    req.target = "lioness";
    req.walk_flags = {"--checkers", "3", "--sftp-connections", "3"};
    const auto both = engine::search_both(ctx, req);
    REQUIRE(both.has_value());
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
}
#endif
TEST_CASE("grab's own target: common_flags, then the command's flags after --, which win") {
    const auto dir = std::filesystem::temp_directory_path() / "grab-tests-engine-target";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    engine::Context ctx;
    ctx.config.rclone_config = dir / "rclone.conf"; // none there: no secrets either
    ctx.settings.name = "Main";
    ctx.settings.common_flags = {"-P", "--contimeout", "10s", "--bind", "10.0.0.1"};
    ctx.remote.name = "Main";
    ctx.remote.host = "main.example";
    ctx.remote.user = "me";
    ctx.remote.key_use_agent = true;
    auto plain = engine::ssh_target(ctx);
    REQUIRE_MESSAGE(plain, plain.error_or(""));
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
    std::filesystem::remove_all(dir);
}

namespace {

// Servers of a made-up rclone.conf, as engine::rclone_env reads them; made-up secrets only.
constexpr std::string_view env_conf = "[Main]\n"
                                      "type = sftp\n"
                                      "host = main.example\n"
                                      "port = 2222\n"
                                      "user = me\n"
                                      "key_use_agent = true\n"
                                      "key_file = `${RCLONE_CONFIG_DIR}/id_main`\n"
                                      "known_hosts_file = $rclone_config_dir/kh\n"
                                      "shell_type = unix\n"
                                      "md5sum_command = md5sum\n"
                                      "sha1sum_command = none\n"
                                      "Host = not-read.example\n"
                                      "key-file = not-read\n"
                                      "blank =\n"
                                      "\n"
                                      "[Main.eu]\n"
                                      "type = sftp\n"
                                      "host = eu.example\n"
                                      "user = me\n"
                                      "\n"
                                      "[pw]\n"
                                      "type = sftp\n"
                                      "host = pw.example\n"
                                      "user = u\n"
                                      "\n"
                                      "[inline]\n"
                                      "type = sftp\n"
                                      "host = in.example\n"
                                      "user = u\n"
                                      "pass = INLINE-FAKE-OBSCURED\n"
                                      "\n"
                                      "[\u10E1\u10D4\u10E0\u10D5\u10D4\u10E0\u10D8]\n"
                                      "type = sftp\n"
                                      "host = ge.example\n"
                                      "user = u\n"
                                      "shell_type = unix\n";

engine::Context env_context(const std::filesystem::path& conf, const std::string& name, bool agent) {
    engine::Context ctx;
    ctx.config.rclone_config = conf;
    ctx.settings.name = name;
    ctx.settings.rclone_remote = name;
    ctx.remote.name = name;
    ctx.remote.key_use_agent = agent;
    return ctx;
}

using Vars = std::vector<std::pair<std::string, std::string>>;

} // namespace

TEST_CASE("rclone_env: a server's section and its secrets as its own run's RCLONE_CONFIG_<NAME>_* (phase 7)") {
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-rclone-env-block";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto conf = dir / "rclone.conf";
    std::ofstream(conf, std::ios::binary) << env_conf;
    REQUIRE(secrets::save(secrets::path_beside(conf), {{"pw", {{"pass", "STORED-FAKE-OBSCURED"}}},
                                                       {"inline", {{"pass", "OLDER-FAKE-OBSCURED"}}}}));
    const std::string d = util::path_to_utf8(dir);

    // Every key rclone reads in the file, as it reads it: backticks off, ${RCLONE_CONFIG_DIR} the
    // file's folder through GRAB_RCLONE_CONFIG_DIR (rclone's own is "." with its config in memory),
    // "none" kept. Not "Host" or "key-file", which rclone skips in a file but would take from the
    // environment; not a blank.
    auto ctx = env_context(conf, "Main", true);
    const auto main = engine::rclone_env(ctx);
    REQUIRE_MESSAGE(main, main.error_or(""));
    CHECK(main->set == Vars{{"RCLONE_CONFIG_MAIN_HOST", "main.example"},
                            {"RCLONE_CONFIG_MAIN_KEY_FILE", "${GRAB_RCLONE_CONFIG_DIR}/id_main"},
                            {"RCLONE_CONFIG_MAIN_KEY_USE_AGENT", "true"},
                            {"RCLONE_CONFIG_MAIN_KNOWN_HOSTS_FILE", "${GRAB_RCLONE_CONFIG_DIR}/kh"},
                            {"RCLONE_CONFIG_MAIN_MD5SUM_COMMAND", "md5sum"},
                            {"RCLONE_CONFIG_MAIN_PORT", "2222"},
                            {"RCLONE_CONFIG_MAIN_SHA1SUM_COMMAND", "none"},
                            {"RCLONE_CONFIG_MAIN_SHELL_TYPE", "unix"},
                            {"RCLONE_CONFIG_MAIN_TYPE", "sftp"},
                            {"RCLONE_CONFIG_MAIN_USER", "me"},
                            {"GRAB_RCLONE_CONFIG_DIR", d}});
    // The user's own RCLONE_CONFIG_MAIN_* can't add to it (--rclone, whose environment is whole).
    CHECK(main->without_prefix == std::vector<std::string>{"RCLONE_CONFIG_MAIN_"});

    // A dotted name gets its own keys only, as grab reads it: nothing from [Main] (G86).
    const auto eu = engine::rclone_env(env_context(conf, "Main.eu", false));
    REQUIRE(eu);
    CHECK(eu->set == Vars{{"RCLONE_CONFIG_MAIN.EU_HOST", "eu.example"},
                          {"RCLONE_CONFIG_MAIN.EU_TYPE", "sftp"},
                          {"RCLONE_CONFIG_MAIN.EU_USER", "me"}});
    // A password from the store beside the file, as rclone keeps it; one in the section wins, as
    // for grab's own client (ssh_target).
    using Var = std::pair<std::string, std::string>;
    const auto pw = engine::rclone_env(env_context(conf, "pw", false));
    REQUIRE(pw);
    CHECK(std::ranges::find(pw->set, Var{"RCLONE_CONFIG_PW_PASS", "STORED-FAKE-OBSCURED"}) != pw->set.end());
    const auto in = engine::rclone_env(env_context(conf, "inline", false));
    REQUIRE(in);
    CHECK(std::ranges::find(in->set, Var{"RCLONE_CONFIG_INLINE_PASS", "INLINE-FAKE-OBSCURED"}) != in->set.end());
    // A Georgian name, upper-cased as Go does it (Mtavruli), which rclone looks for.
    const auto ge = engine::rclone_env(env_context(conf, "\u10E1\u10D4\u10E0\u10D5\u10D4\u10E0\u10D8", false));
    REQUIRE(ge);
    CHECK(ge->set.front().first == "RCLONE_CONFIG_\u1CA1\u1C94\u1CA0\u1C95\u1C94\u1CA0\u1C98_HOST");
    CHECK(ge->without_prefix == std::vector<std::string>{"RCLONE_CONFIG_\u1CA1\u1C94\u1CA0\u1C95\u1C94\u1CA0\u1C98_"});

    // A flag that would replace the server is refused for grab's own runs, not for --rclone's.
    ctx.settings.common_flags = {"-P", "--sftp-chunk-size", "255Ki", "--sftp-known-hosts-file", "C:\\other"};
    const auto refused = engine::rclone_env(ctx);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().starts_with(
        "[Main] common_flags: --sftp-known-hosts-file would replace the server's trusted host keys"));
    CHECK(engine::rclone_env(ctx, true).has_value());

    CHECK(engine::rclone_env(env_context(conf, "gone", true)).error_or("").find("rclone remote 'gone' not found") !=
          std::string::npos);

    // A '$' in the folder's own name isn't expanded a second time: rclone expands the value once,
    // and the folder only arrives through GRAB_RCLONE_CONFIG_DIR.
    const auto dollar = dir / "$sync";
    std::filesystem::create_directories(dollar);
    std::ofstream(dollar / "rclone.conf", std::ios::binary)
        << "[d]\ntype = sftp\nhost = d.example\nkey_file = ${RCLONE_CONFIG_DIR}/id\n";
    const auto in_dollar = engine::rclone_env(env_context(dollar / "rclone.conf", "d", true));
    REQUIRE(in_dollar);
    CHECK(std::ranges::find(in_dollar->set, Var{"RCLONE_CONFIG_D_KEY_FILE", "${GRAB_RCLONE_CONFIG_DIR}/id"}) !=
          in_dollar->set.end());
    CHECK(in_dollar->set.back() == Var{"GRAB_RCLONE_CONFIG_DIR", util::path_to_utf8(dollar)});
    // No path naming the folder, no variable.
    CHECK(std::ranges::none_of(eu->set, [](const Var& v) { return v.first == "GRAB_RCLONE_CONFIG_DIR"; }));
#ifdef _WIN32
    // A store that can't be read: a password server can't log in; the others still can.
    std::ofstream(secrets::path_beside(conf), std::ios::binary) << "grab-secrets-sealed 1\nanother account's";
    CHECK(engine::rclone_env(env_context(conf, "pw", false)).error_or("").find("Enter the password of pw again") !=
          std::string::npos);
    CHECK(engine::rclone_env(env_context(conf, "Main", true)).has_value());
#endif
    std::filesystem::remove_all(dir);
}

#ifdef _WIN32
TEST_CASE("each rclone run gets its own server's variables and no other's; grab keeps none (G7, G84)") {
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-rclone-env-runs";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto conf = dir / "rclone.conf";
    std::ofstream(conf, std::ios::binary) << env_conf;
    REQUIRE(secrets::save(secrets::path_beside(conf), {{"pw", {{"pass", "STORED-FAKE-OBSCURED"}}}}));
    // A stand-in rclone: logs its arguments and the RCLONE_* variables it gets (names and made-up
    // values; cmd /u writes them in UTF-16), lists one file.
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
    auto env_of = [&](const std::string& name, bool agent) {
        std::filesystem::remove(dir / "env.log");
        auto ctx = env_context(conf, name, agent);
        ctx.config.rclone = util::path_to_utf8(dir / "rclone.cmd");
        ctx.method = FindMethod::rclone;
        engine::SearchRequest req;
        req.target = "x";
        const auto found = engine::search(ctx, req);
        CHECK_MESSAGE(found.has_value(), found.error_or(""));
        const auto raw = util::read_file(dir / "env.log").value_or("");
        std::wstring wide(raw.size() / 2, L'\0');
        std::memcpy(wide.data(), raw.data(), wide.size() * sizeof(wchar_t));
        std::vector<std::string> lines;
        for (const auto& line : util::split(util::to_utf8(wide), '\n')) {
            if (const auto t = util::trim(line); !t.empty()) lines.emplace_back(t);
        }
        return lines;
    };
    const auto main = env_of("Main", true);
    const auto pw = env_of("pw", false);
    const auto ge = env_of("\u10E1\u10D4\u10E0\u10D5\u10D4\u10E0\u10D8", false);
    SetEnvironmentVariableW(L"RCLONE_CONFIG_PW_PASS", nullptr);
    SetEnvironmentVariableW(L"rclone_config_main_host", nullptr);
    SetEnvironmentVariableW(L"RCLONE_SFTP_HOST", nullptr);
    SetEnvironmentVariableW(L"RCLONE_SFTP_CHUNK_SIZE", nullptr);

    CHECK(main == std::vector<std::string>{"RCLONE_CONFIG_MAIN_HOST=main.example",
                                           "RCLONE_CONFIG_MAIN_KEY_FILE=${GRAB_RCLONE_CONFIG_DIR}/id_main",
                                           "RCLONE_CONFIG_MAIN_KEY_USE_AGENT=true",
                                           "RCLONE_CONFIG_MAIN_KNOWN_HOSTS_FILE=${GRAB_RCLONE_CONFIG_DIR}/kh",
                                           "RCLONE_CONFIG_MAIN_MD5SUM_COMMAND=md5sum",
                                           "RCLONE_CONFIG_MAIN_PORT=2222",
                                           "RCLONE_CONFIG_MAIN_SHA1SUM_COMMAND=none",
                                           "RCLONE_CONFIG_MAIN_SHELL_TYPE=unix",
                                           "RCLONE_CONFIG_MAIN_TYPE=sftp",
                                           "RCLONE_CONFIG_MAIN_USER=me",
                                           "RCLONE_SFTP_CHUNK_SIZE=64Ki"});
    CHECK(pw == std::vector<std::string>{"RCLONE_CONFIG_PW_HOST=pw.example",
                                         "RCLONE_CONFIG_PW_PASS=STORED-FAKE-OBSCURED", "RCLONE_CONFIG_PW_TYPE=sftp",
                                         "RCLONE_CONFIG_PW_USER=u", "RCLONE_SFTP_CHUNK_SIZE=64Ki"});
    const std::string g = "RCLONE_CONFIG_\u1CA1\u1C94\u1CA0\u1C95\u1C94\u1CA0\u1C98_";
    CHECK(ge == std::vector<std::string>{g + "HOST=ge.example", g + "SHELL_TYPE=unix", g + "TYPE=sftp", g + "USER=u",
                                         "RCLONE_SFTP_CHUNK_SIZE=64Ki"});
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
}
#endif
