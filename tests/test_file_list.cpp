#include "engine.hpp"
#include "file_list.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
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
        {"movies", true, std::nullopt, 1'700'000'000},
        {"movies/Sahe AKA The Precinct 2010.avi", false, 941563904, 1'697'354'553},
        {"movies/precinct", true, std::nullopt, 1'697'000'000},
        {"movies/precinct/precinct.srt", false, 12, std::nullopt},
        {"tv/Lioness.S03E01.mkv", false, 100, 1'750'000'001},
        {"tv/Lioness.S03E02.mkv", false, 200, 1'750'000'002},
        {"tv/lioness extras/Lioness Making Of.mkv", false, 300, 1'750'000'003},
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
    CHECK_FALSE(file_list::from_text("grab-file-list 3\n").has_value());
    CHECK_FALSE(file_list::from_text("grab-file-list 2\nd\t\tbad\\q\n").has_value()); // unknown escape
    CHECK_FALSE(file_list::from_text("grab-file-list 2\nd\tno-date-field\n").has_value());
    const auto later = file_list::from_text("grab-file-list 2\nfetched\t5\nnewfield\tx\nd\t\ta\n");
    REQUIRE(later.has_value()); // fields a later version adds are skipped
    CHECK(later->fetched == 5);
    CHECK(later->dated);
    CHECK(later->entries == std::vector<Entry>{{"a", true, std::nullopt}});

    const auto dir = std::filesystem::temp_directory_path() / "grab-test-lists";
    std::filesystem::remove_all(dir);
    const auto file = file_list::path_for(dir, "Box");
    CHECK(file_list::save(file, l)); // creates the folder
    CHECK(file_list::load(file) == l);
    std::filesystem::remove_all(dir);
}

TEST_CASE("the list's text, version 2: dates after sizes, empty when unknown") {
    List l;
    l.fetched = 5;
    l.fingerprint = "fp";
    l.entries = {{"movies", true, std::nullopt, 1697354553},
                 {"movies/a.mkv", false, 12, 1697354554},
                 {"movies/b.mkv", false, std::nullopt, std::nullopt},
                 {"tv", true, std::nullopt, std::nullopt}};
    const std::string text = "grab-file-list 2\nfetched\t5\nfingerprint\tfp\n"
                             "d\t1697354553\tmovies\nf\t12\t1697354554\tmovies/a.mkv\nf\t\t\tmovies/b.mkv\nd\t\ttv\n";
    CHECK(file_list::to_text(l) == text);
    CHECK(file_list::from_text(text) == l);
    // A date that doesn't parse, or is 0 or before, is unknown.
    const auto odd = file_list::from_text("grab-file-list 2\nf\t1\tx\ta\nf\t2\t0\tb\nd\t-5\tc\n");
    REQUIRE(odd.has_value());
    CHECK(odd->entries == std::vector<Entry>{{"a", false, 1}, {"b", false, 2}, {"c", true, std::nullopt}});
}

TEST_CASE("a list saved by an older grab answers at once, without dates, and is due for a refresh") {
    const auto old = file_list::from_text("grab-file-list 1\nfetched\t1760000000\nfingerprint\tfp\n"
                                          "d\tmovies\nf\t12\tmovies/a.mkv\nf\t\tmovies/b.mkv\n");
    REQUIRE(old.has_value());
    CHECK_FALSE(old->dated);
    CHECK(old->fetched == 1760000000); // kept: the window shows the list's age
    CHECK(old->entries == std::vector<Entry>{{"movies", true, std::nullopt},
                                             {"movies/a.mkv", false, 12},
                                             {"movies/b.mkv", false, std::nullopt}});
    CHECK_FALSE(file_list::fresh(*old, 1760000000));
    CHECK(file_list::fresh(old->fetched, 1760000000)); // only its missing dates make it stale
    const auto found = file_list::search(box_context(), *old, request("a.mkv"));
    REQUIRE(found.hits.size() == 1);
    CHECK(found.hits[0].size == 12);
    CHECK_FALSE(found.hits[0].modified.has_value());

    List current = sample();
    CHECK(current.dated);
    CHECK(file_list::fresh(current, current.fetched + 60));
    current.dated = false;
    CHECK_FALSE(file_list::fresh(current, current.fetched + 60));
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
          "\\( -type d -printf 'd\\t%T@\\t%p\\0' -o -type f -printf 'f\\t%s\\t%T@\\t%p\\0' \\) 2>/dev/null");
    CHECK(file_list::find_command({"/srv"}, 2, false).find("-prune") == std::string::npos);

    using namespace std::string_literals;
    const auto found = file_list::parse_find("d\t1697354553.1234567000\t./movies\0"
                                             "f\t12\t1697354554.0000000000\t./movies/a b.mkv\0"
                                             "f\t7\t0.0000000000\tlearning/x\0"
                                             "d\tnot-a-time\tlearning/tab\there\0"
                                             "d\t./no-time-field\0junk\0"s);
    CHECK(found == std::vector<Entry>{{"movies", true, std::nullopt, 1697354553},
                                      {"movies/a b.mkv", false, 12, 1697354554},
                                      {"learning/x", false, 7, std::nullopt},
                                      {"learning/tab\there", true, std::nullopt, std::nullopt}});

    const auto listed = file_list::parse_lsf("1697354553,-1,movies/\r\n1697354554,941563904,movies/a;b.avi\n"
                                             "-62135596800,-1,tv/\nmissing-fields\n5,x,y\n",
                                             "/home");
    CHECK(listed == std::vector<Entry>{{"/home/movies", true, std::nullopt, 1697354553},
                                       {"/home/movies/a;b.avi", false, 941563904, 1697354554},
                                       {"/home/tv", true, std::nullopt, std::nullopt}});
    CHECK(file_list::parse_lsf("5,3,x\n", "")[0] == Entry{"x", false, 3, 5});
    // An rclone without --time-format's "unix" prints its name: the entry stays, undated.
    CHECK(file_list::parse_lsf("unix,3,x\n", "") == std::vector<Entry>{{"x", false, 3, std::nullopt}});
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
    CHECK(sized.hits[0].modified == 1697354553);
    CHECK(sized.hits[0].name == "Sahe AKA The Precinct 2010.avi");
    const auto folder = file_list::search(ctx, l, request("movies", Mode::folder));
    REQUIRE(folder.hits.size() == 1);
    CHECK_FALSE(folder.hits[0].size.has_value());
    CHECK(folder.hits[0].modified == 1700000000); // a folder's own date
    CHECK_FALSE(file_list::search(ctx, l, request("precinct.srt")).hits[0].modified.has_value());
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

TEST_CASE("saved lists are encrypted for the current user") {
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-sealed";
    std::filesystem::remove_all(dir);
    const auto file = file_list::path_for(dir, "Box");
    const List l = sample();
    REQUIRE(file_list::save(file, l));
    const auto raw = util::read_file(file);
    REQUIRE(raw.has_value());
    CHECK(raw->starts_with("grab-file-list-sealed 1\n"));
#ifdef _WIN32
    CHECK(raw->find("Precinct") == std::string::npos); // no name readable on disk
    CHECK(raw->find("movies") == std::string::npos);
#endif
    CHECK(file_list::load(file) == l);

    // An unencrypted list, or a damaged one, is not used: grab fetches a new one.
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << file_list::to_text(l);
    }
    CHECK_FALSE(file_list::load(file).has_value());
#ifdef _WIN32
    REQUIRE(file_list::save(file, l));
    auto damaged = *util::read_file(file);
    damaged[damaged.size() / 2] ^= 0x55;
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << damaged;
    }
    CHECK_FALSE(file_list::load(file).has_value());
#endif
    std::filesystem::remove_all(dir);
}
