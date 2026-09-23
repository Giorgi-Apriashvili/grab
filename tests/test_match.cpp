#include "match.hpp"

#include <doctest/doctest.h>

using namespace grab;

namespace {
constexpr const char* lioness =
    "Lioness.2023.S03E08.The.Unravelling.2160p.ATV.WEB-DL.DDP5.1.DV.HDR.H.265-NTb.mkv";
}

TEST_CASE("make_query picks the kind") {
    CHECK(make_query("lioness", false).kind == Query::Kind::words);
    CHECK(make_query("Lioness S03E08", false).terms == std::vector<std::string>{"lioness", "s03e08"});
    CHECK(make_query("  a \t b  ", false).terms == std::vector<std::string>{"a", "b"});
    CHECK(make_query("Lioness*", false).kind == Query::Kind::glob);
    CHECK(make_query("S0?E08", false).kind == Query::Kind::glob);
    CHECK(make_query("[Ll]ioness", false).kind == Query::Kind::glob);
    CHECK(make_query("lioness", true).kind == Query::Kind::exact);
    CHECK(make_query("   ", false).kind == Query::Kind::exact); // nothing to search for
}

TEST_CASE("words: every word, any order, ignoring case") {
    CHECK(matches(make_query("lioness", false), lioness));
    CHECK(matches(make_query("LIONESS s03e08", false), lioness));
    CHECK(matches(make_query("unravelling 2160p", false), lioness));
    CHECK(matches(make_query("2160p lioness e08", false), lioness));
    CHECK(matches(make_query("the unravelling", false), lioness));
    CHECK_FALSE(matches(make_query("lioness s03e09", false), lioness));
    CHECK_FALSE(matches(make_query("lioness 1080p", false), lioness));
    // Only the last path component is matched.
    CHECK_FALSE(matches(make_query("tv", false), "tv/Lioness.mkv"));
    CHECK(matches(make_query("lioness", false), "tv/Lioness.mkv"));
}

TEST_CASE("glob is case-insensitive, exact is not") {
    CHECK(matches(make_query("lioness*", false), lioness));
    CHECK(matches(make_query("*S03E0[1-8]*", false), lioness));
    CHECK(matches(make_query("*s03e0[1-8]*", false), lioness));
    CHECK_FALSE(matches(make_query("lioness*", true), lioness));
    CHECK(matches(make_query("Lioness*", true), lioness));
    CHECK(matches(make_query(lioness, true), lioness));
    CHECK_FALSE(matches(make_query("Lioness", true), lioness)); // exact means the whole name
}

TEST_CASE("glob_match follows find -name rules") {
    CHECK(glob_match("releases", "releases"));
    CHECK_FALSE(glob_match("releases", "Releases"));
    CHECK(glob_match("releases", "Releases", true));
    CHECK(glob_match("Some.Movie*", "Some.Movie.2013.1080p"));
    CHECK(glob_match("*.mkv", "movie.mkv"));
    CHECK_FALSE(glob_match("*.mkv", "movie.mkv.part"));
    CHECK(glob_match("*.mkv", "MOVIE.MKV", true));
    CHECK(glob_match("S0?E01", "S01E01"));
    CHECK_FALSE(glob_match("S0?E01", "S011E01"));
    CHECK(glob_match("[abc]x", "bx"));
    CHECK_FALSE(glob_match("[abc]x", "Bx"));
    CHECK(glob_match("[abc]x", "BX", true));
    CHECK_FALSE(glob_match("[abc]x", "dx"));
    CHECK(glob_match("[!abc]x", "dx"));
    CHECK(glob_match("[a-c]x", "cx"));
    CHECK(glob_match("[a-c]x", "Cx", true));
    CHECK_FALSE(glob_match("[a-c]x", "Cx"));
    CHECK(glob_match("[]]", "]"));
    CHECK(glob_match("a[", "a[")); // unterminated bracket is literal
    CHECK(glob_match("*", ""));
    CHECK(glob_match("**", "anything"));
    CHECK_FALSE(glob_match("", "x"));
    CHECK(glob_match("", ""));
}

TEST_CASE("match_tier ranks exact names, then prefixes") {
    const auto q = make_query("lioness", false);
    CHECK(match_tier(q, "tv/Lioness") == 0);
    CHECK(match_tier(q, "tv/Lioness.2023.S03E08.mkv") == 1);
    CHECK(match_tier(q, "tv/The.Lioness.Returns.mkv") == 2);
    CHECK(match_tier(make_query("*.mkv", false), "a.mkv") == 2);
}

TEST_CASE("wants_hidden and escape_glob") {
    CHECK(wants_hidden(make_query(".config", false)));
    CHECK(wants_hidden(make_query("nvim .config", false)));
    CHECK_FALSE(wants_hidden(make_query("config", false)));
    CHECK(wants_hidden(make_query(".c*", false)));
    CHECK(escape_glob("50%[1]*?\\x") == "50%\\[1\\]\\*\\?\\\\x");
    CHECK(escape_glob("plain") == "plain");
}
