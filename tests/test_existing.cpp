#include "existing.hpp"
#include "folder_job.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace grab;
namespace fs = std::filesystem;
using existing::Choice;

namespace {

constexpr std::int64_t ns_per_s = 1'000'000'000;

// A file of `size` bytes modified at `unix_seconds`.
void make(const fs::path& p, std::uint64_t size, std::int64_t unix_seconds) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << std::string(size, 'x');
    const auto when = std::chrono::clock_cast<fs::file_time_type::clock>(
        std::chrono::system_clock::time_point{std::chrono::seconds{unix_seconds}});
    fs::last_write_time(p, when);
}

} // namespace

TEST_CASE("the answers, as the window and --existing name them") {
    CHECK(existing::name(Choice::replace) == "replace");
    CHECK(existing::name(Choice::keep_both) == "both");
    CHECK(existing::parse("skip") == Choice::skip);
    CHECK(existing::parse("both") == Choice::keep_both);
    CHECK(existing::parse("ask") == Choice::ask);
    CHECK_FALSE(existing::parse("Replace").has_value());
    CHECK_FALSE(existing::parse("").has_value());
}

TEST_CASE("a second copy's name, as Explorer gives it") {
    CHECK(existing::numbered("movie.mkv", 2) == "movie (2).mkv");
    CHECK(existing::numbered("archive.tar.gz", 3) == "archive.tar (3).gz");
    CHECK(existing::numbered("README", 2) == "README (2)");
    CHECK(existing::numbered(".bashrc", 2) == ".bashrc (2)");
    CHECK(existing::numbered("Ünï.txt", 2) == "Ünï (2).txt");

    const auto dir = fs::temp_directory_path() / "grab-test-second-copy";
    fs::remove_all(dir);
    fs::create_directories(dir);
    make(dir / "movie (2).mkv", 1, 0);
    CHECK(*existing::second_copy(dir / "movie.mkv") == dir / "movie (3).mkv"); // (2) is taken
    const fetch::Reserved reserved = [&](const fs::path& p) { return p == dir / "movie (3).mkv"; };
    CHECK(*existing::second_copy(dir / "movie.mkv", reserved) == dir / "movie (4).mkv");
    fs::remove_all(dir);
}

TEST_CASE("a file there differs unless it is up to date with the server's") {
    const auto dir = fs::temp_directory_path() / "grab-test-differs";
    fs::remove_all(dir);
    const auto f = dir / "a.bin";
    const std::int64_t server = 1'700'000'000;
    CHECK_FALSE(existing::differs(f, 10, server * ns_per_s)); // nothing there
    make(f, 10, server);
    CHECK_FALSE(existing::differs(f, 10, server * ns_per_s));      // the same
    CHECK_FALSE(existing::differs(f, 10, std::nullopt));           // no time: the size decides
    CHECK(existing::differs(f, 11, server * ns_per_s));            // another size
    CHECK(existing::differs(f, 10, (server + 3600) * ns_per_s));   // older than the server's
    CHECK_FALSE(existing::differs(f, 10, (server - 3600) * ns_per_s)); // newer: a later copy
    // rclone copy also replaces one that is newer.
    CHECK(existing::rclone_replaces(f, 10, (server - 3600) * ns_per_s));
    CHECK_FALSE(existing::rclone_replaces(f, 10, server * ns_per_s));
    CHECK(existing::rclone_replaces(f, 9, server * ns_per_s));
    CHECK_FALSE(existing::rclone_replaces(dir / "none", 10, std::nullopt));
    const auto mine = existing::local_file(f);
    REQUIRE(mine.has_value());
    CHECK(mine->size == 10);
    CHECK(mine->modified == server);
    fs::create_directories(dir / "sub");
    CHECK_FALSE(existing::differs(dir / "sub", 10, std::nullopt)); // a folder is no file to replace
    fs::remove_all(dir);
}

TEST_CASE("a folder's conflicts: skipped, kept beside, or replaced") {
    const auto dest = fs::temp_directory_path() / "grab-test-conflicts";
    fs::remove_all(dest);
    folder::Job job;
    job.remote_path = "/srv/Show";
    job.dest = dest;
    const auto dir = folder::local_dir(dest, job.remote_path);
    const std::string when = "2024-01-01T00:00:00Z";
    const std::int64_t server = *util::parse_rfc3339_seconds(when);
    auto tree_of = [&] {
        folder::Tree t;
        t.files.push_back(folder::make_file("e01.mkv", 10, when)); // the same file: done by mark_existing
        t.files.push_back(folder::make_file("e02.mkv", 20, when)); // yours differs
        t.files.push_back(folder::make_file("sub/e03.mkv", 30, when)); // yours is older
        t.files.push_back(folder::make_file("e04.mkv", 40, when)); // not there
        t.listed = true;
        return t;
    };
    make(dir / "e01.mkv", 10, server);
    make(dir / "e02.mkv", 5, server);
    make(dir / "sub" / "e03.mkv", 30, server - 86400);
    make(dir / "e02 (2).mkv", 1, server); // a second copy's first name is taken

    std::mutex m;
    auto t = tree_of();
    folder::mark_existing(job, t, m);
    const auto found = folder::conflicts(job, t, m);
    REQUIRE(found.size() == 2);
    CHECK(found[0].path == "e02.mkv");
    CHECK(found[0].local == dir / "e02.mkv");
    CHECK(found[0].size == 20);
    CHECK(found[1].path == "sub/e03.mkv");

    folder::apply(Choice::skip, found, dir, t);
    CHECK(t.files[1].status == folder::State::skipped);
    CHECK(t.files[2].status == folder::State::skipped);
    CHECK(t.files[3].status == folder::State::queued);

    t = tree_of();
    folder::mark_existing(job, t, m);
    folder::apply(Choice::keep_both, found, dir, t);
    CHECK(t.files[1].local == fs::path("e02 (3).mkv"));
    CHECK(t.files[1].big); // grab writes it under that name
    CHECK(folder::target_of(dir, t.files[1]) == dir / "e02 (3).mkv");
    CHECK(t.files[2].local == fs::path("sub") / "e03 (2).mkv");
    CHECK(t.files[3].local.empty());
    CHECK(folder::target_of(dir, t.files[3]) == dir / "e04.mkv");
    CHECK(folder::conflicts(job, t, m).empty()); // the second copies aren't there yet

    t = tree_of();
    folder::mark_existing(job, t, m);
    folder::apply(Choice::replace, found, dir, t);
    CHECK(t.files[1].status == folder::State::queued);
    CHECK(t.files[1].local.empty());
    fs::remove_all(dest);
}

TEST_CASE("a numbered copy that already is the server's file counts as there (G112)") {
    const auto dir = fs::temp_directory_path() / "grab-test-kept-copy";
    fs::remove_all(dir);
    make(dir / "movie.mkv", 5, 100);        // the user's own, different
    make(dir / "movie (2).mkv", 9, 1000);   // an older Keep both: another version
    make(dir / "movie (3).mkv", 10, 2000);  // the server's file, kept by the last run
    CHECK(existing::kept_copy(dir / "movie.mkv", 10, 2000 * ns_per_s) == dir / "movie (3).mkv");
    CHECK_FALSE(existing::kept_copy(dir / "movie.mkv", 11, 2000 * ns_per_s).has_value()); // the server's changed
    make(dir / "movie (5).mkv", 11, 3000);  // past a gap: copies are numbered in a row
    CHECK_FALSE(existing::kept_copy(dir / "movie.mkv", 11, 3000 * ns_per_s).has_value());
    fs::remove_all(dir);
}
TEST_CASE("picks of the same name land under numbered names, in pick order (G103)") {
    const fs::path dest = "E:\\x";
    CHECK(existing::pick_targets(dest, {"/r1/sample.txt", "/r2/sample.txt", "/r3/SAMPLE.TXT", "/r4/other.txt"}, false) ==
          std::vector<fs::path>{dest / "sample.txt", dest / "sample (2).txt", dest / "SAMPLE (3).TXT", dest / "other.txt"});
    // A pick already named like a numbered one keeps its name; the next number is free.
    CHECK(existing::pick_targets(dest, {"/a/sample (2).txt", "/b/sample.txt", "/c/sample.txt"}, false) ==
          std::vector<fs::path>{dest / "sample (2).txt", dest / "sample.txt", dest / "sample (3).txt"});
    CHECK(existing::pick_targets(dest, {"/tv/A/Season 1", "/tv/B/Season 1"}, true) ==
          std::vector<fs::path>{dest / "Season 1", dest / "Season 1 (2)"});
    CHECK(existing::pick_targets(dest, {"/x/v1.2", "/y/v1.2"}, true) == std::vector<fs::path>{dest / "v1.2", dest / "v1.2 (2)"});
}