#include "folder.hpp"
#include "folder_job.hpp"
#include "gui_state.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

using namespace grab;
using folder::State;

TEST_CASE("a big folder shows its active files and the first few queued") {
    const std::vector<State> states{State::done,   State::queued, State::running, State::queued, State::skipped,
                                    State::queued, State::paused, State::failed,  State::queued};
    CHECK(folder::visible(states, 2) == std::vector<std::size_t>{1, 2, 3, 6, 7});
    CHECK(folder::visible(states, 10) == std::vector<std::size_t>{1, 2, 3, 5, 6, 7, 8});
    CHECK(folder::visible({State::done, State::skipped}, 5).empty());
}

TEST_CASE("a folder's row says which folders below it couldn't be read") {
    CHECK(folder::unreadable_text({}).empty());
    CHECK(folder::unreadable_text({"a/b: permission denied"}) == "1 folder couldn't be read: a/b: permission denied");
    CHECK(folder::unreadable_text({"a: permission denied", "b/c: no such file", "d: permission denied"}) ==
          "3 folders couldn't be read: a: permission denied, ...");
}

TEST_CASE("a folder's files survive a restart in queue.json") {
    SavedDownload d{"hetzner", "folder", "/home/x/Limits", "Limits", "E:\\Learn", 597330609,
                    {{"a.mp4", 100, "2023-05-06T15:18:12+04:00", "done", true, true}, // fetched and verified
                     {"e.mp4", 50, "", "done", true, false},                            // fetched, not checked yet
                     {"f.mp4", 60, "", "done"},                                         // was there already
                     {"b.mp4", 200, "", "paused"},
                     {"sub/c.mp4", 300, "", "skipped"},
                     {"d.mp4", 400, "", "queued"},
                     {"g.mp4", 500, "", "queued", false, false, "g (2).mp4"}}}; // a second copy
    const auto back = parse_queue(queue_to_json({d}));
    REQUIRE(back.size() == 1);
    CHECK(back[0] == d);
    // Unknown states read as queued; entries without a path are dropped.
    const auto odd = parse_queue(R"([{"remote":"r","path":"/p","dest":"C:\\d","mode":"folder",)"
                                 R"("files":[{"path":"x","state":"running"},{"state":"done"}]}])");
    REQUIRE(odd.size() == 1);
    REQUIRE(odd[0].files.size() == 1);
    CHECK(odd[0].files[0].state == "queued");
}

TEST_CASE("folder totals count finished and running files, not skipped ones") {
    folder::Tree t;
    t.files.push_back(folder::make_file("a", 100, ""));
    t.files.push_back(folder::make_file("b", 200, ""));
    t.files.push_back(folder::make_file("c", 300, ""));
    t.files.push_back(folder::make_file("d", 400, ""));
    t.files[0].status = State::done;
    t.files[1].status = State::running;
    t.files[1].bytes = 50;
    t.files[1].speed = 10;
    t.files[2].status = State::skipped;
    t.files[3].bytes = 40; // a big file's partial download from an earlier run
    const auto tot = folder::totals(t);
    CHECK(tot.total == 700);
    CHECK(tot.bytes == 190);
    CHECK(tot.files == 3);
    CHECK(tot.files_done == 1);
    CHECK(tot.speed == 10);
    REQUIRE(tot.eta.has_value());
    CHECK(*tot.eta == doctest::Approx(51.0));
    // From small_limit up a file takes the resumable path; a device name changes nothing (grab
    // writes every file itself).
    CHECK(folder::make_file("x", folder::small_limit, "").big);
    CHECK_FALSE(folder::make_file("x", folder::small_limit - 1, "").big);
    CHECK_FALSE(folder::make_file("CON", 10, "").big);
    CHECK_FALSE(folder::make_file("season 1/nul.txt", 10, "").big);
    std::vector<folder::File> files{folder::make_file("a", 10, ""), folder::make_file("b", folder::small_limit, "")};
    files[0].big = true; // as an older run or a Keep both left it
    files[1].big = false;
    folder::set_big(files);
    CHECK_FALSE(files[0].big);
    CHECK(files[1].big);
    // A folder lands under its own name, made safe: never outside the download folder.
    const std::filesystem::path dest = util::path_from_utf8("D:\\Downloads");
    CHECK(folder::local_dir(dest, "/srv/..\\up") == dest / util::path_from_utf8("..＼up"));
    CHECK(folder::local_dir(dest, "/srv/C:\\Windows") == dest / util::path_from_utf8("C：＼Windows"));
    CHECK(folder::local_dir(dest, "/srv/..") == dest / util::path_from_utf8("．．"));
}

TEST_CASE("single files of a folder: pause, resume, skip and retry") {
    folder::Tree t;
    t.files.push_back(folder::make_file("waiting", 10, ""));
    t.files.push_back(folder::make_file("copying", 10, ""));
    t.files.push_back(folder::make_file("big", folder::small_limit, ""));
    t.files.push_back(folder::make_file("broken", folder::small_limit, ""));
    t.files[1].status = State::running;
    t.files[1].bytes = 5;
    t.files[2].status = State::running;
    t.files[3].status = State::failed;
    t.files[3].error = "oops";

    folder::pause_file(t, "waiting"); // queued: not taken
    CHECK(t.files[0].status == State::paused);
    folder::pause_file(t, "copying"); // a small file in flight: dropped once its replies are in
    CHECK(t.files[1].status == State::paused);
    CHECK(t.files[1].bytes == 0);
    folder::pause_file(t, "big"); // its own stop; it is marked paused once stopped
    CHECK(t.files[2].status == State::running);
    CHECK(t.files[2].stop_reason == folder::StopReason::pause);
    CHECK(t.files[2].stop.stop_requested());

    folder::resume_file(t, "waiting");
    CHECK(t.files[0].status == State::queued);
    folder::resume_paused(t);
    CHECK(t.files[1].status == State::queued);

    const auto dir = std::filesystem::path("D") / "Limits";
    const auto discard = folder::skip_file(t, dir, "/srv/Limits", "broken");
    CHECK(t.files[3].status == State::skipped);
    REQUIRE(discard.has_value());
    CHECK(discard->target == dir / "broken");
    CHECK(discard->remote_path == "/srv/Limits/broken");
    REQUIRE(discard->reserved);
    CHECK(discard->reserved(dir / "BIG")); // the folder's files, as Windows compares names
    CHECK_FALSE(discard->reserved(dir / "broken.grabpart"));
    CHECK_FALSE(folder::skip_file(t, dir, "/srv/Limits", "big").has_value()); // running: stopped, then discarded
    CHECK(t.files[2].stop_reason == folder::StopReason::skip);

    t.files[1].status = State::failed;
    t.files[1].error = "x";
    auto o = folder::outcome(t);
    CHECK(o.failed == 1);
    CHECK(o.first_error == "copying: x");
    CHECK(o.waiting == 1); // "waiting" is queued
    folder::retry_files(t, false);
    CHECK(t.files[1].status == State::queued);
    CHECK(t.files[1].error.empty());
    CHECK(t.files[3].status == State::skipped); // skipped files stay skipped
    t.files[0].status = State::paused;
    folder::retry_files(t, true); // after a cancel: everything not done or skipped
    CHECK(t.files[0].status == State::queued);
}

TEST_CASE("a folder's local directory and its files' remote paths") {
    CHECK(folder::local_dir("Backup", "/home/x/Limits") == std::filesystem::path("Backup") / "Limits");
    CHECK(folder::child_remote_path("/a", "b/c.mkv") == "/a/b/c.mkv");
    CHECK(folder::child_remote_path("/a/", "b") == "/a/b");
    CHECK(folder::child_remote_path("", "b") == "b");
}

TEST_CASE("a folder without a connection pool isn't listed") {
    folder::Job job;
    job.server = "Main";
    job.remote_path = "/tmp/f";
    folder::Tree tree;
    std::mutex m;
    const auto listed = folder::list(job, tree, m, std::stop_token{});
    REQUIRE_FALSE(listed);
    CHECK(listed.error().find("no connection") != std::string::npos);
    CHECK_FALSE(tree.listed);
}

TEST_CASE("names that are one on Windows get numbered ones (G25)") {
    // As a listing comes, sorted: README before readme, Docs before docs.
    std::vector<folder::File> files;
    for (const char* p : {"Docs/a.txt", "Docs/only-upper.txt", "NOTES/x.txt", "README", "caf\u00e9.txt", "cafe\u0301.txt",
                          "docs/a.txt", "docs/only-lower.txt", "notes", "plain.txt", "readme", "readme (2)"}) {
        files.push_back(folder::make_file(p, 5, ""));
    }
    folder::settle_names(files);
    auto file = [&](std::string_view p) -> const folder::File& {
        return *std::ranges::find(files, p, &folder::File::path);
    };
    // Every file is grab's to write, small ones beside the others: none is made big for its name.
    for (const auto& f : files) CHECK_FALSE(f.big);
    // The first keeps its name; a later one with the same name on Windows gets the next free number.
    CHECK(file("README").local.empty());
    CHECK(file("readme").local == std::filesystem::path("readme (3)")); // "readme (2)" is the server's
    CHECK(file("Docs/a.txt").local.empty());
    CHECK(file("docs/a.txt").local == std::filesystem::path("docs") / "a (2).txt");
    CHECK(file("docs/only-lower.txt").local.empty()); // one folder on Windows, no clash in it
    CHECK(file("notes").local == std::filesystem::path("notes (2)")); // a folder has its name
    CHECK(file("caf\u00e9.txt").local.empty()); // two names to NTFS
    CHECK(file("cafe\u0301.txt").local.empty());
}
