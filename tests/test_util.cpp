#include "util.hpp"

#include <doctest/doctest.h>

using namespace grab;

TEST_CASE("trim strips spaces, tabs and CR") {
    CHECK(util::trim("  abc \t\r\n") == "abc");
    CHECK(util::trim("") == "");
    CHECK(util::trim("   ") == "");
    CHECK(util::trim("x") == "x");
}

TEST_CASE("split trims items and drops empties") {
    const auto v = util::split(" /home/alice , /srv,, /opt ", ',');
    REQUIRE(v.size() == 3);
    CHECK(v[0] == "/home/alice");
    CHECK(v[1] == "/srv");
    CHECK(v[2] == "/opt");
    CHECK(util::split("", ',').empty());
    CHECK(util::split("   ", ',').empty());
}

TEST_CASE("split_args behaves like a shell for flag strings") {
    const auto v = util::split_args("-P  --sftp-chunk-size 255Ki   --exclude \"a b\" --x='c d'");
    REQUIRE(v.size() == 6);
    CHECK(v[0] == "-P");
    CHECK(v[1] == "--sftp-chunk-size");
    CHECK(v[2] == "255Ki");
    CHECK(v[3] == "--exclude");
    CHECK(v[4] == "a b");
    CHECK(v[5] == "--x=c d");
    CHECK(util::split_args("").empty());
    CHECK(util::split_args("   ").empty());
    CHECK(util::split_args("''").size() == 1);
}

TEST_CASE("join") {
    CHECK(util::join({}, ", ").empty());
    CHECK(util::join({"a"}, ", ") == "a");
    CHECK(util::join({"a", "b", "c"}, ", ") == "a, b, c");
}

TEST_CASE("path round-trips through UTF-8") {
    const std::string s = "E:/Backup/Ünïcödé";
    CHECK(util::path_to_utf8(util::path_from_utf8(s)) == s);
}
