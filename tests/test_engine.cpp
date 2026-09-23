#include "engine.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
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

    CHECK(engine::download_argv(ctx, Mode::file, "/srv/tv/Lioness.S03E08.mkv", dest) ==
          std::vector<std::string>{"rclone", "copyto", "hetzner:/srv/tv/Lioness.S03E08.mkv",
                                   util::path_to_utf8(dest / "Lioness.S03E08.mkv"), "-P",
                                   "--multi-thread-streams", "8"});
    const std::vector<std::string> extra{"--bwlimit", "10M"};
    CHECK(engine::download_argv(ctx, Mode::folder, "/srv/tv/show", dest, extra) ==
          std::vector<std::string>{"rclone", "copy", "hetzner:/srv/tv/show",
                                   util::path_to_utf8(dest / "show"), "-P", "--transfers", "8",
                                   "--bwlimit", "10M"});
}

TEST_CASE("remove_partials deletes only the interrupted item's temp files") {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "grab_test_partials";
    fs::remove_all(dir);
    fs::create_directories(dir / "show" / "sub");
    auto touch = [](const fs::path& p) { std::ofstream(p) << "x"; };
    touch(dir / "movie.mkv.37814e9b.partial");
    touch(dir / "movie.mkv.aaaa.partial");
    touch(dir / "movie.mkv");                    // a finished file: kept
    touch(dir / "other.mkv.1234.partial");        // another item: kept
    touch(dir / "show" / "e01.mkv.ff.partial");
    touch(dir / "show" / "sub" / "e02.mkv.ee.partial");
    touch(dir / "show" / "e03.mkv");

    CHECK(engine::remove_partials(dir / "movie.mkv", Mode::file) == 2);
    CHECK(fs::exists(dir / "movie.mkv"));
    CHECK(fs::exists(dir / "other.mkv.1234.partial"));

    CHECK(engine::remove_partials(dir / "show", Mode::folder) == 2);
    CHECK(fs::exists(dir / "show" / "e03.mkv"));
    CHECK_FALSE(fs::exists(dir / "show" / "sub" / "e02.mkv.ee.partial"));

    CHECK(engine::remove_partials(dir / "missing", Mode::folder) == 0); // no throw
    fs::remove_all(dir);
}

TEST_CASE("format_size") {
    CHECK(util::format_size(0) == "0 B");
    CHECK(util::format_size(1023) == "1023 B");
    CHECK(util::format_size(1536) == "1.5 KiB");
    CHECK(util::format_size(13367823676ULL) == "12.4 GiB");
}
