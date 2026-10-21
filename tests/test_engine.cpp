#include "engine.hpp"
#include "listing.hpp"
#include "remote.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
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

TEST_CASE("console-only progress flags are dropped for streaming") {
    const std::vector<std::string> flags{"-P", "--sftp-chunk-size", "255Ki", "--progress",
                                         "--stats-one-line", "-q", "--transfers", "8"};
    CHECK(engine::without_console_progress(flags) ==
          std::vector<std::string>{"--sftp-chunk-size", "255Ki", "--transfers", "8"});
}

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
                                   util::path_to_utf8(dest / "Lioness.S03E08.mkv"), "-P",
                                   "--multi-thread-streams", "8"});
    const std::vector<std::string> extra{"--bwlimit", "10M"};
    CHECK(engine::download_argv(ctx, Mode::folder, "/srv/tv/show", dest, extra).value() ==
          std::vector<std::string>{"rclone", "copy", "hetzner:/srv/tv/show",
                                   util::path_to_utf8(dest / "show"), "-P", "--transfers", "8",
                                   "--bwlimit", "10M"});
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
    const auto by_lsf = engine::make_result(ctx, req, parse_lsf_output(list, "1697354554;300;tv/Lioness.S03E08.mkv\n"));
    REQUIRE(by_lsf.hits.size() == 1);
    CHECK(by_lsf.hits[0].path == "/home/tv/Lioness.S03E08.mkv");
    CHECK(by_lsf.hits[0].size == 300ULL);
    CHECK(by_lsf.hits[0].modified == 1697354554);

    req.mode = Mode::folder;
    list.mode = Mode::folder;
    const auto folder = engine::make_result(ctx, req, parse_lsf_output(list, "1697000000;-1;tv/Lioness/\n"));
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
