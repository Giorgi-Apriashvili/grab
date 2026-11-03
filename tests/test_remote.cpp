#include "remote.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <format>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace grab;

TEST_CASE("a target with a '/' is a path to check, not a name to search for") {
    CHECK(is_path_target("/x"));
    CHECK(is_path_target("a/b"));
    CHECK_FALSE(is_path_target("x"));
}

TEST_CASE("remote_basename ignores trailing slashes") {
    CHECK(remote_basename("/home/alice/releases/movie.mkv") == "movie.mkv");
    CHECK(remote_basename("/home/alice/releases/") == "releases");
    CHECK(remote_basename("plain") == "plain");
}

TEST_CASE("remote_spec joins remote and absolute path") {
    CHECK(remote_spec("hetzner", "/home/alice/releases") == "hetzner:/home/alice/releases");
}

TEST_CASE("a remote path with no name to save it under (G15)") {
    CHECK(has_name("/srv/x"));
    CHECK(has_name("x/"));
    CHECK(has_name("a"));
    CHECK_FALSE(has_name("/"));
    CHECK_FALSE(has_name("//"));
    CHECK_FALSE(has_name(""));
    CHECK_FALSE(has_name("."));
    CHECK_FALSE(has_name("/srv/.."));
}

TEST_CASE("the pick list shows sizes and local dates") {
    const std::int64_t t = 1697346000; // 2023-10-15 05:00 UTC
    const auto date = util::local_date(t);
    REQUIRE(date.size() == 10);
    CHECK(pick_note(941563904, t) == "  (897.9 MiB, " + date + ")");
    CHECK(pick_note(std::nullopt, t) == "  (" + date + ")"); // a folder
    CHECK(pick_note(12, std::nullopt) == "  (12 B)");
    CHECK(pick_note(std::nullopt, std::nullopt).empty());

    std::istringstream in;
    std::ostringstream err;
    const auto r = choose_matches({"/r/b-releases", "/r/a-releases"}, make_query("releases", false), PickOptions{}, in,
                                  err, [&](const std::string& path) { return pick_note(path == "/r/a-releases" ? 5 : 7, t); });
    CHECK_FALSE(r.has_value()); // no terminal to ask
    CHECK(err.str().find("[1] /r/a-releases  (5 B, " + date + ")\n") != std::string::npos);
    CHECK(err.str().find("[2] /r/b-releases  (7 B, " + date + ")\n") != std::string::npos);
}

TEST_CASE("ranking: exact name, then prefix, then depth, then case-insensitive path") {
    const auto q = make_query("releases", false);
    std::vector<std::string> m{"/home/alice/downloads/autoadd/releases", "/srv/old-releases",
                               "/home/alice/releases", "/srv/Releases.2024", "/srv/releases"};
    rank_matches(m, q);
    CHECK(m == std::vector<std::string>{"/srv/releases", "/home/alice/releases",
                                        "/home/alice/downloads/autoadd/releases",
                                        "/srv/Releases.2024", "/srv/old-releases"});

    // A season lists in episode order regardless of case.
    std::vector<std::string> eps{"tv/Lioness.S03E08.mkv", "tv/lioness.S03E02.mkv",
                                 "tv/Lioness.S03E10.mkv"};
    rank_matches(eps, make_query("lioness", false));
    CHECK(eps == std::vector<std::string>{"tv/lioness.S03E02.mkv", "tv/Lioness.S03E08.mkv",
                                          "tv/Lioness.S03E10.mkv"});
}

TEST_CASE("parse_selection: numbers, ranges, all, dedup, errors") {
    using V = std::vector<std::size_t>;
    CHECK(parse_selection("3", 5) == V{2});
    CHECK(parse_selection(" 1-3,5 ", 5) == V{0, 1, 2, 4});
    CHECK(parse_selection("2 4", 5) == V{1, 3});
    CHECK(parse_selection("5-3", 5) == V{4, 3, 2});
    CHECK(parse_selection("1,1,2,1-2", 5) == V{0, 1});
    CHECK(parse_selection("a", 3) == V{0, 1, 2});
    CHECK(parse_selection("all", 2) == V{0, 1});
    CHECK_FALSE(parse_selection("0", 5).has_value());
    CHECK_FALSE(parse_selection("6", 5).has_value());
    CHECK_FALSE(parse_selection("2-9", 5).has_value());
    CHECK_FALSE(parse_selection("x", 5).has_value());
    CHECK_FALSE(parse_selection("1-", 5).has_value());
    CHECK_FALSE(parse_selection(",,", 5).has_value());
}

TEST_CASE("ask_destination: Enter = fallback, quotes stripped, EOF aborts") {
    const auto fallback = std::filesystem::path("C:\\cwd");
    std::ostringstream err;

    std::istringstream enter("\n");
    auto a = ask_destination(enter, err, fallback);
    REQUIRE(a.has_value());
    CHECK(*a == fallback);
    CHECK(err.str().find("Enter = C:\\cwd") != std::string::npos);

    std::istringstream quoted("  \"E:\\My TV\\Lioness\"  \n");
    auto b = ask_destination(quoted, err, fallback);
    REQUIRE(b.has_value());
    CHECK(util::path_to_utf8(*b) == "E:\\My TV\\Lioness");

    std::istringstream plain("E:\\TV\n");
    CHECK(util::path_to_utf8(*ask_destination(plain, err, fallback)) == "E:\\TV");

    std::istringstream eof;
    CHECK_FALSE(ask_destination(eof, err, fallback).has_value());
}

TEST_CASE("choose_matches: single, --first, --all, non-interactive, interactive") {
    const auto q = make_query("releases", false);
    const std::vector<std::string> two{"/home/alice/downloads/autoadd/releases",
                                       "/home/alice/releases"};
    using V = std::vector<std::string>;
    PickOptions ask;
    ask.interactive = true;

    SUBCASE("single match needs no input") {
        std::istringstream in;
        std::ostringstream err;
        auto r = choose_matches({"/only"}, q, ask, in, err);
        REQUIRE(r.has_value());
        CHECK(*r == V{"/only"});
        CHECK(err.str().empty());
    }
    SUBCASE("--first takes the best, --all takes everything") {
        std::istringstream in;
        std::ostringstream err;
        PickOptions first = ask;
        first.first = true;
        CHECK(choose_matches(two, q, first, in, err) == V{"/home/alice/releases"});
        PickOptions all = ask;
        all.all = true;
        CHECK(choose_matches(two, q, all, in, err) ==
              V{"/home/alice/releases", "/home/alice/downloads/autoadd/releases"});
    }
    SUBCASE("non-interactive lists and fails") {
        std::istringstream in;
        std::ostringstream err;
        PickOptions batch;
        auto r = choose_matches(two, q, batch, in, err);
        REQUIRE_FALSE(r.has_value());
        CHECK(err.str().find("[1] /home/alice/releases") != std::string::npos);
        CHECK(err.str().find("[2] /home/alice/downloads/autoadd/releases") != std::string::npos);
    }
    SUBCASE("Enter picks [1]") {
        std::istringstream in("\n");
        std::ostringstream err;
        CHECK(choose_matches(two, q, ask, in, err) == V{"/home/alice/releases"});
    }
    SUBCASE("a range, after one invalid answer") {
        std::istringstream in("nope\n2-1\n");
        std::ostringstream err;
        auto r = choose_matches(two, q, ask, in, err);
        REQUIRE(r.has_value());
        CHECK(*r == V{"/home/alice/downloads/autoadd/releases", "/home/alice/releases"});
        CHECK(err.str().find("invalid choice") != std::string::npos);
    }
    SUBCASE("q and EOF abort; three invalid answers abort") {
        std::ostringstream err;
        std::istringstream quit("q\n");
        CHECK_FALSE(choose_matches(two, q, ask, quit, err).has_value());
        std::istringstream eof;
        CHECK_FALSE(choose_matches(two, q, ask, eof, err).has_value());
        std::istringstream bad("0\n3\n99\n");
        CHECK_FALSE(choose_matches(two, q, ask, bad, err).has_value());
    }
    SUBCASE("long lists are capped at 100 shown entries") {
        std::vector<std::string> many;
        for (int i = 0; i < 120; ++i) many.push_back(std::format("/r/releases{:03}", i));
        std::istringstream in("a\n");
        std::ostringstream err;
        auto r = choose_matches(many, q, ask, in, err);
        REQUIRE(r.has_value());
        CHECK(r->size() == 120);
        CHECK(err.str().find("[100]") != std::string::npos);
        CHECK(err.str().find("[101]") == std::string::npos);
        CHECK(err.str().find("20 more not shown") != std::string::npos);
    }
}
