#include "folder.hpp"
#include "folder_job.hpp"
#include "gui_state.hpp"
#include "json.hpp"
#include "queue_view.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <map>
#include <string>
#include <vector>

using namespace grab;
using folder::State;

namespace {

folder::Tree sample() {
    folder::Tree t;
    t.files.push_back(folder::make_file("sub/a.bin", 100, "2026-01-01T00:00:00Z"));
    t.files.push_back(folder::make_file("a:b.txt", 200, ""));               // a name Windows can't take
    t.files.push_back(folder::make_file("e01.mkv", folder::small_limit, "")); // given a second copy's name
    t.files.push_back(folder::make_file("CON.txt", 400, ""));
    t.files[2].local = std::filesystem::path("e01 (2).mkv");
    t.listed = true;
    return t;
}

json::Value parsed(const std::string& text) {
    auto v = json::parse(text);
    REQUIRE(v.has_value());
    return *v;
}

} // namespace

TEST_CASE("a folder's names for the page: paths, sizes, and the local name where it differs") {
    const auto t = sample();
    const auto n = queue_view::names_of(t.files);
    CHECK(n.path == std::vector<std::string>{"sub/a.bin", "a:b.txt", "e01.mkv", "CON.txt"});
    CHECK(n.size == std::vector<std::uint64_t>{100, 200, folder::small_limit, 400});
    CHECK_FALSE(n.shown.contains(0)); // sub\a.bin, as the path says
    CHECK(n.shown.at(1) == "a\xEF\xBC\x9A" "b.txt"); // a：b.txt
    CHECK(n.shown.at(2) == "e01 (2).mkv");
    CHECK(n.shown.at(3) == "CON_.txt");
    REQUIRE(n.local.size() == 1);
    CHECK(n.local.at(2) == std::filesystem::path("e01 (2).mkv"));

    // Where they land, and what no partial download may take: as the tree says it.
    const auto dir = std::filesystem::path("D") / "f";
    for (std::size_t i = 0; i < t.files.size(); ++i) CHECK(queue_view::target_of(dir, n, i) == folder::target_of(dir, t.files[i]));
    const auto from_names = queue_view::reserved_names(dir, n);
    const auto from_tree = folder::reserved_names(dir, t);
    for (const auto* probe : {"sub\\A.BIN", "e01.mkv", "E01 (2).MKV", "CON_.txt", "a.bin", "e01.mkv.grabpart", "x"}) {
        const auto p = dir / util::path_from_utf8(probe);
        CHECK_MESSAGE(from_names(p) == from_tree(p), probe);
    }
    CHECK(from_names(dir / "sub" / "A.BIN"));
    CHECK(from_names(dir / "E01 (2).MKV"));
    CHECK_FALSE(from_names(dir / "e01.mkv.grabpart"));
}

TEST_CASE("one pass over a folder's files counts its totals and finds the rows that changed") {
    auto t = sample();
    t.files[0].status = State::done;
    t.files[0].bytes = 100;
    t.files[1].status = State::running;
    t.files[1].bytes = 50;
    t.files[1].speed = 25;
    t.files[1].eta = 6;
    t.files[2].status = State::failed;
    t.files[2].error = "permission denied";
    t.files[3].status = State::skipped;

    auto snap = queue_view::snapshot(t.files);
    REQUIRE(snap.rows.size() == 4);
    CHECK(snap.rows[1].speed == 0); // the running file's speed comes with the first delta
    CHECK(snap.errors.size() == 1);
    CHECK(snap.errors.at(2) == "permission denied");

    auto shadow = snap.rows;
    auto p = queue_view::pass(t.files, &shadow);
    const auto tot = folder::totals(t);
    CHECK(p.totals.bytes == tot.bytes);
    CHECK(p.totals.total == tot.total);
    CHECK(p.totals.speed == tot.speed);
    CHECK(p.totals.eta == tot.eta);
    CHECK(p.totals.files == tot.files);
    CHECK(p.totals.files_done == tot.files_done);
    CHECK(p.counts == std::array<std::size_t, 6>{0, 1, 0, 1, 1, 1});
    REQUIRE(p.changed.size() == 1); // the running one: its speed
    CHECK(p.changed[0].index == 1);
    CHECK(p.changed[0].now.speed == 25);
    CHECK(p.changed[0].eta == 6);

    CHECK(queue_view::pass(t.files, &shadow).changed.empty()); // nothing new
    t.files[1].bytes = 80;
    t.files[2].status = State::queued; // retried
    t.files[2].error.clear();
    t.files[0].verified = true;
    p = queue_view::pass(t.files, &shadow);
    std::vector<std::size_t> rows;
    for (const auto& r : p.changed) rows.push_back(r.index);
    CHECK(rows == std::vector<std::size_t>{0, 1, 2});
    CHECK(p.changed[0].now.verified);
    CHECK(p.changed[2].now.error == 0);

    t.files[2].status = State::failed;
    t.files[2].error = "no such file";
    p = queue_view::pass(t.files, &shadow);
    REQUIRE(p.changed.size() == 1);
    CHECK(p.changed[0].error == "no such file");
    CHECK(p.changed[0].now.error != 0);
    // Queued again and failed with another error between two passes, at the same bytes: it goes.
    t.files[2].error = "permission denied";
    p = queue_view::pass(t.files, &shadow);
    REQUIRE(p.changed.size() == 1);
    CHECK(p.changed[0].index == 2);
    CHECK(p.changed[0].error == "permission denied");
    t.files[1].status = State::paused; // stopped: its speed goes
    p = queue_view::pass(t.files, &shadow);
    REQUIRE(p.changed.size() == 1);
    CHECK(p.changed[0].now.speed == 0);
    CHECK_FALSE(p.changed[0].eta.has_value());

    // Another generation's shadow (another count of files) is not compared; the totals still come.
    std::vector<queue_view::Shadow> other(2);
    p = queue_view::pass(t.files, &other);
    CHECK(p.changed.empty());
    CHECK(p.totals.total == tot.total);
    CHECK(queue_view::pass(t.files, nullptr).changed.empty());
}

TEST_CASE("the page's file rows: \"files\" in chunks, \"fileDelta\" with only what is set") {
    auto t = sample();
    t.files[0].status = State::done;
    t.files[0].bytes = 100;
    t.files[0].verified = true;
    t.files[2].status = State::failed;
    t.files[2].error = "gone \"away\"";
    const auto names = queue_view::names_of(t.files);
    const auto snap = queue_view::snapshot(t.files);

    const auto all = parsed(queue_view::files_message(7, 3, 0, 5000, names, snap));
    CHECK(all.string_of("type") == "files");
    CHECK(all.find("id")->number == 7);
    CHECK(all.find("gen")->number == 3);
    CHECK(all.find("from")->number == 0);
    CHECK(all.find("total")->number == 4);
    const auto& rows = all.find("rows")->items;
    REQUIRE(rows.size() == 4);
    // [path, size, status, bytes, priority, local, error, verified, named]: the tail as far as set.
    CHECK(json::stringify(rows[0]) == R"(["sub/a.bin",100,3,100,2,null,null,1])");
    CHECK(json::stringify(rows[1]) == "[\"a:b.txt\",200,0,0,2,\"a\xEF\xBC\x9A" "b.txt\"]");
    CHECK(json::stringify(rows[2]) == R"(["e01.mkv",2097152,4,0,2,"e01 (2).mkv","gone \"away\"",0,1])");
    CHECK(json::stringify(rows[3]) == R"(["CON.txt",400,0,0,2,"CON_.txt"])");

    const auto chunk = parsed(queue_view::files_message(7, 3, 3, 2, names, snap)); // the last chunk
    CHECK(chunk.find("from")->number == 3);
    CHECK(chunk.find("total")->number == 4);
    CHECK(chunk.find("rows")->items.size() == 1);
    CHECK(parsed(queue_view::files_message(7, 3, 9, 2, names, snap)).find("rows")->items.empty());

    queue_view::Row running;
    running.index = 1;
    running.now = {.bytes = 50, .speed = 1048576.4F, .error = 0, .status = State::running, .weight = 2, .verified = false};
    running.eta = 6.4;
    queue_view::Row failed;
    failed.index = 2;
    failed.now = {.bytes = 0, .speed = 0, .error = 9, .status = State::failed, .weight = 2, .verified = false};
    failed.error = "x";
    queue_view::Row checked;
    checked.now = {.bytes = 100, .speed = 0, .error = 0, .status = State::done, .weight = 2, .verified = true};
    const auto delta = parsed(queue_view::file_delta_message(7, 3, {running, failed, checked}));
    CHECK(delta.string_of("type") == "fileDelta");
    CHECK(delta.find("gen")->number == 3);
    const auto& d = delta.find("rows")->items;
    REQUIRE(d.size() == 3);
    // [index, status, bytes, speed, eta, priority, error, verified]
    CHECK(json::stringify(d[0]) == "[1,1,50,1048576,6,2]");
    CHECK(json::stringify(d[1]) == R"([2,4,0,0,null,2,"x"])");
    CHECK(json::stringify(d[2]) == "[0,3,100,0,null,2,null,1]");
}

TEST_CASE("the page's ranges of rows become indices, each once, within the folder") {
    auto ranges = [](const char* text) {
        const auto v = json::parse(text);
        REQUIRE(v.has_value());
        return *v;
    };
    auto r = ranges("[[2,4],[3,6],[9,9],[0,0]]");
    CHECK(queue_view::indices_of(&r, 10) == std::vector<std::size_t>{0, 2, 3, 4, 5, 6, 9});
    r = ranges("[[8,20],[-5,1]]"); // clamped to the folder
    CHECK(queue_view::indices_of(&r, 10) == std::vector<std::size_t>{0, 1, 8, 9});
    r = ranges(R"([[4,2],["a",3],[1],[12,14],[1.5,2.5],7])"); // backwards, not numbers, beyond: left out
    CHECK(queue_view::indices_of(&r, 10) == std::vector<std::size_t>{2});
    CHECK(queue_view::indices_of(&r, 0).empty());
    CHECK(queue_view::indices_of(nullptr, 10).empty());
    r = ranges(R"({"from":1})");
    CHECK(queue_view::indices_of(&r, 10).empty());
}

TEST_CASE("only the downloads whose row changed go again") {
    std::map<int, std::string> sent;
    CHECK(queue_view::changed_items(sent, {{1, "a"}, {2, "b"}}) == std::vector<std::size_t>{0, 1});
    CHECK(queue_view::changed_items(sent, {{1, "a"}, {2, "b"}}).empty());
    CHECK(queue_view::changed_items(sent, {{2, "B"}, {1, "a"}, {3, "c"}}) == std::vector<std::size_t>{0, 2});
    CHECK(queue_view::changed_items(sent, {{3, "c"}}).empty()); // 1 and 2 gone
    CHECK(sent.size() == 1);
    CHECK(queue_view::changed_items(sent, {{1, "a"}, {3, "c"}}) == std::vector<std::size_t>{0}); // 1 back: new
}

// The flush's pass over a 177,191-file folder under the queue's lock: at most 3 ms in a Release
// build (DOWNLOADS-UI.md 6.3); timed here, the best of 5, and said.
TEST_CASE("the shadow diff of a 177,191-file folder stays within its time") {
    folder::Tree t;
    constexpr std::size_t n = 177191;
    t.files.reserve(n);
    for (std::size_t k = 0; k < n; ++k) {
        t.files.push_back(folder::make_file(std::format("dir{:03}/file{:06}.bin", k / 1000, k), 4096 + k, "2026-01-01T00:00:00Z"));
    }
    auto shadow = queue_view::snapshot(t.files).rows;
    double best = 1e9;
    std::size_t changed = 0;
    for (int round = 0; round < 5; ++round) {
        // 8 big files moving, 75 small ones landing: a tick of a busy folder.
        for (std::size_t k = 0; k < 8; ++k) {
            auto& f = t.files[1000 + k];
            f.status = State::running;
            f.bytes += 1000;
            f.speed = 1e6;
        }
        for (std::size_t k = 0; k < 75; ++k) {
            auto& f = t.files[static_cast<std::size_t>(round) * 75 + k];
            f.status = State::done;
            f.bytes = f.size;
        }
        const auto started = std::chrono::steady_clock::now();
        const auto p = queue_view::pass(t.files, &shadow);
        best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
        changed = p.changed.size();
    }
    CHECK(changed == 83);
    MESSAGE(std::format("shadow diff of {} files: {:.2f} ms (best of 5)", n, best));
#ifdef NDEBUG
    WARN(best <= 3.0); // the plan's gate; a busy machine may miss it
#endif
}

// queue.json for a 177,191-file folder: its size, and the time to make its text (the save does
// this outside the queue's lock; DOWNLOADS-UI.md 6.5 expected about 20 MB). Said, not gated.
TEST_CASE("queue.json of a 177,191-file folder: its size and the time to make it") {
    SavedDownload d{"Main", "folder", "/tmp/grab-big", "grab-big", "C:\\Downloads", 0, {}, "normal"};
    constexpr std::size_t n = 177191;
    d.files.reserve(n);
    for (std::size_t k = 0; k < n; ++k) {
        d.files.push_back(SavedFile{std::format("dir{:03}/file{:06}.bin", k / 1000, k), 4096 + k, "2026-01-01T00:00:00Z",
                                    k % 3 == 0 ? "done" : "queued", k % 3 == 0, false});
    }
    const auto started = std::chrono::steady_clock::now();
    const auto text = queue_to_json({d});
    const double took = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    MESSAGE(std::format("queue.json of {} files: {:.1f} MB, made in {:.0f} ms", n, static_cast<double>(text.size()) / 1e6, took));
    const auto back = parse_queue(text);
    REQUIRE(back.size() == 1);
    CHECK(back[0].files.size() == n);
    CHECK(back[0].files[3] == d.files[3]);
}
