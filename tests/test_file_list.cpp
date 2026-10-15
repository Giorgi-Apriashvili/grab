#include "engine.hpp"
#include "file_list.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace grab;
using file_list::Entry;
using file_list::List;

namespace {

engine::Context box_context() {
    engine::Context ctx;
    ctx.settings.name = "Box";
    ctx.settings.rclone_remote = "Box";
    ctx.settings.max_depth = 4;
    ctx.settings.skip_hidden = true;
    ctx.remote.host = "u1.your-storagebox.de";
    ctx.remote.user = "u1";
    ctx.remote.port = 23;
    ctx.method = FindMethod::rclone;
    return ctx;
}

engine::SearchRequest request(std::string target, Mode mode = Mode::file, bool exact = false) {
    engine::SearchRequest r;
    r.target = std::move(target);
    r.mode = mode;
    r.exact = exact;
    return r;
}

List sample() {
    List l;
    l.fetched = 1'760'000'000;
    l.fingerprint = "fp";
    l.entries = {
        {"movies", true, std::nullopt},
        {"movies/Sahe AKA The Precinct 2010.avi", false, 941563904},
        {"movies/precinct", true, std::nullopt},
        {"movies/precinct/precinct.srt", false, 12},
        {"tv/Lioness.S03E01.mkv", false, 100},
        {"tv/Lioness.S03E02.mkv", false, 200},
        {"tv/lioness extras/Lioness Making Of.mkv", false, 300},
    };
    return l;
}

std::vector<std::string> paths(const engine::SearchResult& r) {
    std::vector<std::string> out;
    for (const auto& h : r.hits) out.push_back(h.path);
    return out;
}

} // namespace

TEST_CASE("a file list survives its saved form, odd names included") {
    List l = sample();
    l.fingerprint = "1|Box|rclone|u1@h:23|depth=4|hidden=skip|roots=\x1flearning";
    l.entries.push_back({"odd/tab\there", false, 1});
    l.entries.push_back({"odd/line\nbreak", false, std::nullopt});
    l.entries.push_back({"odd/back\\slash\\t", true, std::nullopt});
    l.entries.push_back({"odd/\xC3\xA4\xE2\x82\xAC unicode.mkv", false, 5});
    const auto back = file_list::from_text(file_list::to_text(l));
    REQUIRE(back.has_value());
    CHECK(*back == l);

    CHECK_FALSE(file_list::from_text("").has_value());
    CHECK_FALSE(file_list::from_text("something else\n").has_value());
    CHECK_FALSE(file_list::from_text("grab-file-list 1\nd\tbad\\q\n").has_value()); // unknown escape
    const auto later = file_list::from_text("grab-file-list 1\nfetched\t5\nnewfield\tx\nd\ta\n");
    REQUIRE(later.has_value()); // fields a later version adds are skipped
    CHECK(later->fetched == 5);
    CHECK(later->entries == std::vector<Entry>{{"a", true, std::nullopt}});

    const auto dir = std::filesystem::temp_directory_path() / "grab-test-lists";
    std::filesystem::remove_all(dir);
    const auto file = file_list::path_for(dir, "Box");
    CHECK(file_list::save(file, l)); // creates the folder
    CHECK(file_list::load(file) == l);
    std::filesystem::remove_all(dir);
}

TEST_CASE("list files are named after their server, safely") {
    const std::filesystem::path dir = "lists";
    CHECK(file_list::path_for(dir, "Box.Guest") == dir / "Box.Guest.list");
    const auto spaced = file_list::path_for(dir, "my box").filename().string();
    CHECK(spaced.starts_with("my_box-"));
    CHECK(spaced != file_list::path_for(dir, "my_box").filename().string()); // never the same file
    CHECK(file_list::path_for(dir, "../x").filename().string().starts_with(".._x-"));
}

TEST_CASE("a list is fresh for 15 minutes") {
    const std::int64_t t = 1'760'000'000;
    CHECK(file_list::fresh(t, t));
    CHECK(file_list::fresh(t, t + 14 * 60));
    CHECK_FALSE(file_list::fresh(t, t + 15 * 60));
    CHECK_FALSE(file_list::fresh(t, t - 1)); // the clock went back
    CHECK_FALSE(file_list::fresh(0, t));
}

TEST_CASE("listing commands and their output") {
    CHECK(file_list::find_command({"", "learning"}, 4, true) ==
          "find . 'learning' -mindepth 1 -maxdepth 4 \\( -name '.*' -prune \\) -o "
          "\\( -type d -printf 'd\\t%p\\0' -o -type f -printf 'f\\t%s\\t%p\\0' \\) 2>/dev/null");
    CHECK(file_list::find_command({"/srv"}, 2, false).find("-prune") == std::string::npos);

    using namespace std::string_literals;
    const auto found = file_list::parse_find("d\t./movies\0f\t12\t./movies/a b.mkv\0f\t7\tlearning/x\0junk\0"s);
    CHECK(found == std::vector<Entry>{{"movies", true, std::nullopt},
                                      {"movies/a b.mkv", false, 12},
                                      {"learning/x", false, 7}});

    const auto listed = file_list::parse_lsf("-1;movies/\r\n941563904;movies/a;b.avi\n-1;tv/\n", "/home");
    CHECK(listed == std::vector<Entry>{{"/home/movies", true, std::nullopt},
                                       {"/home/movies/a;b.avi", false, 941563904},
                                       {"/home/tv", true, std::nullopt}});
    CHECK(file_list::parse_lsf("3;x\n", "")[0].path == "x");
}

TEST_CASE("searching a list matches and ranks like a live search") {
    const auto ctx = box_context();
    const auto l = sample();
    CHECK(paths(file_list::search(ctx, l, request("precinct"))) ==
          std::vector<std::string>{"movies/precinct/precinct.srt" /* tier 1: starts with it */,
                                   "movies/Sahe AKA The Precinct 2010.avi"});
    CHECK(paths(file_list::search(ctx, l, request("precinct", Mode::folder))) ==
          std::vector<std::string>{"movies/precinct"});
    CHECK(paths(file_list::search(ctx, l, request("lioness s03"))) ==
          std::vector<std::string>{"tv/Lioness.S03E01.mkv", "tv/Lioness.S03E02.mkv"});
    CHECK(paths(file_list::search(ctx, l, request("*.MKV"))).size() == 3); // globs fold case
    CHECK(file_list::search(ctx, l, request("*.MKV", Mode::file, true)).hits.empty()); // --exact doesn't
    const auto sized = file_list::search(ctx, l, request("sahe"));
    REQUIRE(sized.hits.size() == 1);
    CHECK(sized.hits[0].size == 941563904);
    CHECK(sized.hits[0].name == "Sahe AKA The Precinct 2010.avi");
    CHECK_FALSE(file_list::search(ctx, l, request("movies", Mode::folder)).hits[0].size.has_value());
}

TEST_CASE("what a list can't answer goes live") {
    auto ctx = box_context();
    CHECK(file_list::can_answer(ctx, request("lioness")));
    CHECK_FALSE(file_list::can_answer(ctx, request("tv/Lioness.S03E01.mkv"))); // a path target
    CHECK_FALSE(file_list::can_answer(ctx, request(".bashrc")));             // lists skip dot-names
    auto deeper = request("lioness");
    deeper.depth = 6;
    CHECK_FALSE(file_list::can_answer(ctx, deeper));
    deeper.depth = 4;
    CHECK(file_list::can_answer(ctx, deeper));
    ctx.settings.skip_hidden = false; // then lists keep dot-names too
    CHECK(file_list::can_answer(ctx, request(".bashrc")));
}

TEST_CASE("a list's fingerprint follows what it covers") {
    const auto ctx = box_context();
    const auto fp = file_list::fingerprint(ctx);
    CHECK(file_list::fingerprint(ctx) == fp);
    auto changed = ctx;
    changed.settings.max_depth = 5;
    CHECK(file_list::fingerprint(changed) != fp);
    changed = ctx;
    changed.settings.search_roots = {"learning"};
    CHECK(file_list::fingerprint(changed) != fp);
    changed = ctx;
    changed.remote.host = "u2.your-storagebox.de";
    CHECK(file_list::fingerprint(changed) != fp);
    changed = ctx;
    changed.method = FindMethod::ssh;
    CHECK(file_list::fingerprint(changed) != fp);
}

TEST_CASE("hits of several servers rank together") {
    std::vector<file_list::ServerHit> hits{
        {"Main", {"downloads/tv/deep/precinct-2.avi", "precinct-2.avi", 1}},
        {"Box", {"movies/Sahe AKA The Precinct 2010.avi", "Sahe AKA The Precinct 2010.avi", 2}},
        {"Box.Guest", {"movies/precinct", "precinct", std::nullopt}},
        {"Box", {"movies/precinct", "precinct", std::nullopt}},
    };
    file_list::rank_across(hits, request("precinct"));
    REQUIRE(hits.size() == 4);
    CHECK(hits[0].server == "Box"); // the exact name first; equal paths by server
    CHECK(hits[0].hit.path == "movies/precinct");
    CHECK(hits[1].server == "Box.Guest");
    CHECK(hits[2].hit.path == "downloads/tv/deep/precinct-2.avi"); // starts with the word
    CHECK(hits[3].hit.name == "Sahe AKA The Precinct 2010.avi");
}
