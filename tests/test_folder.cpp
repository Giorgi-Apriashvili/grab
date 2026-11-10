#include "folder.hpp"
#include "folder_job.hpp"
#include "gui_state.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

using namespace grab;
using folder::State;

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
                     {"g.mp4", 500, "", "queued", false, false, "g (2).mp4"}, // a second copy
                     {"h.mp4", 600, "", "failed", false, false, "", "permission denied"}}}; // kept with its error
    d.expanded = false; // folded by the user
    const auto back = parse_queue(queue_to_json({d}));
    REQUIRE(back.size() == 1);
    CHECK(back[0] == d);
    // grab 0.21's queue.json: no error, no expanded; the default rule then.
    const auto old = parse_queue(R"([{"remote":"r","path":"/p","dest":"C:\\d","mode":"folder",)"
                                 R"("files":[{"path":"x","state":"queued"}]}])");
    REQUIRE(old.size() == 1);
    CHECK_FALSE(old[0].expanded.has_value());
    // An error goes with a failed file only.
    const auto mixed = parse_queue(R"([{"remote":"r","path":"/p","dest":"C:\\d","mode":"folder","expanded":true,)"
                                   R"("files":[{"path":"x","state":"done","error":"stale"},{"path":"y","state":"failed","error":"gone"}]}])");
    REQUIRE(mixed.size() == 1);
    CHECK(mixed[0].expanded == true);
    REQUIRE(mixed[0].files.size() == 2);
    CHECK(mixed[0].files[0].error.empty());
    CHECK(mixed[0].files[1].state == "failed");
    CHECK(mixed[0].files[1].error == "gone");
    // Unknown states read as queued; entries without a path are dropped.
    const auto odd = parse_queue(R"([{"remote":"r","path":"/p","dest":"C:\\d","mode":"folder",)"
                                 R"("files":[{"path":"x","state":"running"},{"state":"done"}]}])");
    REQUIRE(odd.size() == 1);
    REQUIRE(odd[0].files.size() == 1);
    CHECK(odd[0].files[0].state == "queued");
}

TEST_CASE("a folder's files keep their priorities in queue.json; an older one's are all normal") {
    SavedDownload d{"Main", "folder", "/tmp/f", "f", "D:\\x", 900, {{"a", 100, "", "queued"}, {"b", 200, "", "paused"}, {"c", 300, "", "failed"}}};
    d.files[0].priority = "high";
    d.files[2].priority = "low";
    d.files[2].error = "gone";
    const auto text = queue_to_json({d});
    CHECK(text.find(R"("path":"a","size":100,"modtime":"","state":"queued","priority":"high"})") != std::string::npos);
    CHECK(text.find(R"("path":"b","size":200,"modtime":"","state":"paused"})") != std::string::npos); // normal: not written
    const auto back = parse_queue(text);
    REQUIRE(back.size() == 1);
    CHECK(back[0] == d);
    // grab 0.21's queue.json has none: every file is normal. Anything but high or low is normal too.
    const auto old = parse_queue(R"([{"remote":"r","path":"/p","dest":"C:\\d","mode":"folder",)"
                                 R"("files":[{"path":"x","state":"queued"},{"path":"y","priority":"urgent"},)"
                                 R"({"path":"z","priority":4},{"path":"w","priority":"low"}]}])");
    REQUIRE(old.size() == 1);
    REQUIRE(old[0].files.size() == 4);
    CHECK(old[0].files[0].priority == "normal");
    CHECK(old[0].files[1].priority == "normal");
    CHECK(old[0].files[2].priority == "normal");
    CHECK(old[0].files[3].priority == "low");
}

namespace {

// The next file to start as the plan defines it (DOWNLOADS-UI.md 7): the queued one of the kind of
// the highest priority, the first in folder order among those.
std::optional<std::size_t> brute_next(const folder::Tree& t, bool big) {
    std::optional<std::size_t> best;
    for (std::size_t i = 0; i < t.files.size(); ++i) {
        const auto& f = t.files[i];
        if (f.status != State::queued || f.big != big) continue;
        if (!best || f.weight > t.files[*best].weight) best = i;
    }
    return best;
}

} // namespace

TEST_CASE("a file's priority in its folder: unfinished files only, the start order follows it") {
    folder::Tree t;
    for (const char* p : {"a", "b", "c", "d", "e"}) t.files.push_back(folder::make_file(p, 10, ""));
    t.files.push_back(folder::make_file("big1", folder::small_limit, ""));
    t.files.push_back(folder::make_file("big2", folder::small_limit, ""));
    t.files[3].status = State::done;
    t.files[4].status = State::skipped;
    CHECK(folder::next_queued(t, false) == 0); // all normal: folder order
    CHECK(folder::next_queued(t, true) == 5);
    const auto before = t.requeued;
    CHECK(folder::set_priority(t, 2, 4));
    CHECK(t.requeued != before); // the cursors start over
    CHECK(t.files[2].weight == 4);
    CHECK(folder::next_queued(t, false) == 2);
    CHECK_FALSE(folder::set_priority(t, 2, 4)); // no change
    CHECK_FALSE(folder::set_priority(t, 3, 4)); // done: keeps its own
    CHECK_FALSE(folder::set_priority(t, 4, 1)); // skipped
    CHECK_FALSE(folder::set_priority(t, 9, 1)); // no such file
    CHECK(folder::set_priority(t, 0, 1));
    CHECK(folder::set_priority(t, 6, 7)); // the nearest priority: high
    CHECK(t.files[6].weight == 4);
    CHECK(folder::next_queued(t, true) == 6);
    // Taken: the next look steps over it; the Low one comes last.
    t.files[2].status = State::running;
    CHECK(folder::next_queued(t, false) == 1);
    t.files[1].status = State::running;
    CHECK(folder::next_queued(t, false) == 0);
    t.files[0].status = State::running;
    CHECK_FALSE(folder::next_queued(t, false).has_value());
    // Put back by the folder's stop: found again.
    t.files[2].status = State::queued;
    ++t.requeued;
    CHECK(folder::next_queued(t, false) == 2);
    // A running file's priority changes too (its share at its next reads).
    CHECK(folder::set_priority(t, 1, 1));
    CHECK(folder::priority_weight(0) == 1);
    CHECK(folder::priority_weight(3) == 2);
    CHECK(folder::priority_weight(5) == 4);
}

TEST_CASE("the next file's cursors pick what a look at every file picks, through every change") {
    // Random actions on a folder of small and big files of all priorities, each followed by a
    // pick of both kinds, compared with brute_next. Requeues are the scheduler's (a folder's stop
    // puts running files back, bumping Tree::requeued); the rest are folder's own functions.
    const auto seed = std::random_device{}(); // other actions each run; a failure says its seed
    std::mt19937 rng(seed);
    for (int round = 0; round < 20; ++round) {
        folder::Tree t;
        const std::size_t n = 30 + rng() % 40;
        for (std::size_t i = 0; i < n; ++i) {
            t.files.push_back(folder::make_file("f" + std::to_string(i), rng() % 3 == 0 ? folder::small_limit : 100, ""));
            t.files.back().weight = std::array{1, 2, 4}[rng() % 3];
        }
        const auto dir = std::filesystem::path("D") / "f";
        for (int step = 0; step < 400; ++step) {
            const std::size_t i = rng() % n;
            auto& f = t.files[i];
            switch (rng() % 10) {
            case 0: // the scheduler starts the next one of a kind
            case 1:
                if (const auto k = folder::next_queued(t, rng() % 2 == 0)) t.files[*k].status = State::running;
                break;
            case 2: // a running one ends: done or failed
                if (f.status == State::running) f.status = rng() % 4 == 0 ? State::failed : State::done;
                break;
            case 3: // the folder stopped: running ones are queued again
                for (auto& g : t.files) {
                    if (g.status == State::running) g.status = State::queued;
                }
                ++t.requeued;
                break;
            case 4: folder::pause_file(t, i); break;
            case 5: folder::resume_file(t, i); break;
            case 6: folder::retry_file(t, i); break;
            case 7: folder::skip_file(t, dir, "/f", i); break;
            case 8: folder::set_priority(t, i, std::array{1, 2, 4}[rng() % 3]); break;
            default:
                if (rng() % 2 == 0) folder::resume_paused(t);
                else folder::retry_files(t, false);
                break;
            }
            for (const bool big : {false, true}) {
                REQUIRE_MESSAGE(folder::next_queued(t, big) == brute_next(t, big), "seed ", seed, " round ", round, " step ", step,
                                " big ", big);
            }
        }
    }
}

TEST_CASE("running big files' parts of the folder's connections add up to what it has free") {
    using V = std::vector<int>;
    auto parts = [](int free, V weights) { return folder::connection_parts(free, weights); };
    auto sum = [](const V& v) { return std::accumulate(v.begin(), v.end(), 0); };
    CHECK(parts(4, {2, 2}) == V{2, 2});
    CHECK(parts(4, {4, 2}) == V{3, 1}); // High + Normal: none left idle (16/6 and 8/6 round to 2 + 1)
    CHECK(parts(4, {2, 1}) == V{3, 1});
    CHECK(parts(4, {1, 2}) == V{1, 3});
    CHECK(parts(12, {4, 2, 1}) == V{7, 3, 2}); // 6.86, 3.43, 1.71
    CHECK(sum(parts(12, {4, 2, 1})) == 12);
    CHECK(parts(3, {2, 2}) == V{2, 1}); // equal remainders: the earlier file
    CHECK(parts(5, {4, 1}) == V{4, 1});
    CHECK(parts(6, {100, 100, 1}) == V{2, 3, 1}); // at least 1 each, taken from the biggest
    CHECK(parts(3, {4, 2, 1}) == V{1, 1, 1});
    // Fewer connections than files: the highest priorities read, the earlier file first; a Low
    // file doesn't read while a High one waits.
    CHECK(parts(2, {4, 2, 1}) == V{1, 1, 0});
    CHECK(parts(2, {1, 2, 4}) == V{0, 1, 1});
    CHECK(parts(1, {2, 4, 4}) == V{0, 1, 0});
    CHECK(parts(2, {2, 2, 2}) == V{1, 1, 0});
    // None free (the small files claim them all): 0 each, all wait.
    CHECK(parts(0, {2}) == V{0});
    CHECK(parts(-4, {4, 2}) == V{0, 0});
    // No weights: 1 each, the server's budget decides.
    CHECK(parts(4, {0, 0}) == V{1, 1});
    CHECK(parts(4, {}).empty());
    // Any mix of priorities, as many connections as the budget may give: all used, each file at
    // least 1 when there are enough, and never a lower priority's while a higher one has none.
    for (int free = 1; free <= 16; ++free) {
        for (int n = 1; n <= 8; ++n) {
            V w;
            for (int k = 0; k < n; ++k) w.push_back(std::array{4, 2, 1}[static_cast<std::size_t>((k * 7 + free) % 3)]);
            const auto p = parts(free, w);
            CHECK(sum(p) == free);
            const int least = std::ranges::min(p);
            CHECK((free >= n ? least >= 1 : least == 0));
            for (std::size_t x = 0; x < p.size(); ++x) {
                for (std::size_t y = 0; y < p.size(); ++y) {
                    if (w[x] > w[y] && p[y] > 0) CHECK(p[x] > 0);
                }
            }
        }
    }
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

TEST_CASE("single files of a folder by their index: pause, resume, skip and retry") {
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

    CHECK(folder::pause_file(t, 0)); // queued: not taken
    CHECK(t.files[0].status == State::paused);
    CHECK_FALSE(folder::pause_file(t, 0)); // paused already
    CHECK(folder::pause_file(t, 1)); // a small file in flight: dropped once its replies are in
    CHECK(t.files[1].status == State::paused);
    CHECK(t.files[1].bytes == 0);
    CHECK(folder::pause_file(t, 2)); // its own stop; it is marked paused once stopped
    CHECK(t.files[2].status == State::running);
    CHECK(t.files[2].stop_reason == folder::StopReason::pause);
    CHECK(t.files[2].stop.stop_requested());
    CHECK_FALSE(folder::pause_file(t, 2)); // asked already
    CHECK_FALSE(folder::pause_file(t, 3)); // failed
    CHECK_FALSE(folder::pause_file(t, 4)); // no such file

    CHECK(folder::resume_file(t, 0));
    CHECK(t.files[0].status == State::queued);
    CHECK_FALSE(folder::resume_file(t, 0));
    CHECK_FALSE(folder::resume_file(t, 7));
    folder::resume_paused(t);
    CHECK(t.files[1].status == State::queued);

    const auto dir = std::filesystem::path("D") / "Limits";
    bool skipped = false;
    const auto discard = folder::skip_file(t, dir, "/srv/Limits", 3, &skipped);
    CHECK(skipped);
    CHECK(t.files[3].status == State::skipped);
    REQUIRE(discard.has_value());
    if (discard) {
        CHECK(discard->target == dir / "broken");
        CHECK(discard->remote_path == "/srv/Limits/broken");
        CHECK_FALSE(discard->reserved); // a batch builds it once, outside the lock (queue_view::reserved_names)
    }
    CHECK_FALSE(folder::skip_file(t, dir, "/srv/Limits", 2, &skipped).has_value()); // running: stopped, then discarded
    CHECK(skipped);
    CHECK(t.files[2].stop_reason == folder::StopReason::skip);
    CHECK_FALSE(folder::skip_file(t, dir, "/srv/Limits", 2, &skipped).has_value()); // asked already
    CHECK_FALSE(skipped);
    CHECK_FALSE(folder::skip_file(t, dir, "/srv/Limits", 3, &skipped).has_value()); // skipped already
    CHECK_FALSE(skipped);
    CHECK_FALSE(folder::skip_file(t, dir, "/srv/Limits", 9).has_value());

    t.files[1].status = State::failed;
    t.files[1].error = "x";
    auto o = folder::outcome(t);
    CHECK(o.failed == 1);
    CHECK(o.first_error == "copying: x");
    CHECK(o.waiting == 1); // "waiting" is queued
    CHECK(folder::retry_file(t, 1)); // the row's Retry
    CHECK(t.files[1].status == State::queued);
    CHECK(t.files[1].error.empty());
    CHECK_FALSE(folder::retry_file(t, 1));
    CHECK_FALSE(folder::retry_file(t, 3)); // skipped files stay skipped
    t.files[1].status = State::failed;
    folder::retry_files(t, false);
    CHECK(t.files[1].status == State::queued);
    CHECK(t.files[3].status == State::skipped);
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
