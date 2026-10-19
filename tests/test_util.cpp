#include "util.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>

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

TEST_CASE("Unix times as find and rclone print them") {
    CHECK(util::parse_unix_time("1697354553") == 1697354553);
    CHECK(util::parse_unix_time("1697354553.1234567000") == 1697354553); // find's %T@
    CHECK(util::parse_unix_time("0") == 0);
    CHECK(util::parse_unix_time("0.5") == 0);
    CHECK(util::parse_unix_time("-62135596800") == -62135596800); // rclone's unknown time
    CHECK(util::parse_unix_time("-1.5") == -1);
    constexpr const char* bad[] = {"",   "-",  ".5",  "5.",  "1.2.3", "+5",    " 5",
                                   "5 ", "12a", "1e9", "0x10", "7.5x", "99999999999999999999"};
    for (const char* text : bad) {
        CAPTURE(text);
        CHECK_FALSE(util::parse_unix_time(text).has_value());
    }
    // A listed time of 0 or before is unknown.
    CHECK(util::known_time(1) == 1);
    CHECK_FALSE(util::known_time(0).has_value());
    CHECK_FALSE(util::known_time(-62135596800).has_value());
    CHECK_FALSE(util::known_time(std::nullopt).has_value());
}

TEST_CASE("RFC 3339 times") {
    constexpr std::int64_t sec = 1'000'000'000;
    CHECK(util::parse_rfc3339("1970-01-01T00:00:00Z") == 0);
    CHECK(util::parse_rfc3339("2000-01-01T00:00:00Z") == std::int64_t{946684800} * sec);
    CHECK(util::parse_rfc3339("2000-01-01T01:00:00+01:00") == std::int64_t{946684800} * sec);
    CHECK(util::parse_rfc3339("2000-01-01T00:00:00.5Z") == std::int64_t{946684800} * sec + 500'000'000);
    CHECK_FALSE(util::parse_rfc3339("2000-01-01 garbage").has_value());
    CHECK_FALSE(util::parse_rfc3339("2000-01-01T00:00:00").has_value()); // no zone
    CHECK_FALSE(util::parse_rfc3339("2000-13-01T00:00:00Z").has_value());

    // 64 bits of nanoseconds reach from 1677 to 2262: the whole years between, any offset.
    CHECK(util::parse_rfc3339("1678-01-01T00:00:00Z") == std::int64_t{-9214560000} * sec);
    CHECK(util::parse_rfc3339("1678-01-01T00:00:00+23:59").has_value());
    CHECK(util::parse_rfc3339("2261-12-31T23:59:59.999999999-23:59").has_value());
    CHECK_FALSE(util::parse_rfc3339("1677-12-31T23:59:59Z").has_value());
    CHECK_FALSE(util::parse_rfc3339("2262-01-01T00:00:00Z").has_value());
    CHECK_FALSE(util::parse_rfc3339("0001-01-01T00:00:00Z").has_value());
    CHECK_FALSE(util::parse_rfc3339("9999-12-31T23:59:59Z").has_value());

    // In seconds, any year; the fraction is dropped.
    CHECK(util::parse_rfc3339_seconds("2023-10-15T11:22:33.987654321+02:00") == 1697361753);
    CHECK(util::parse_rfc3339_seconds("2023-10-15T09:22:33Z") == 1697361753);
    CHECK(util::parse_rfc3339_seconds("0001-01-01T00:00:00Z") == -62135596800);
    CHECK(util::parse_rfc3339_seconds("9999-12-31T23:59:59Z") == 253402300799);
    CHECK_FALSE(util::parse_rfc3339_seconds("yesterday").has_value());
    CHECK_FALSE(util::parse_rfc3339_seconds("").has_value());
}

TEST_CASE("a date in local time") {
    // 2023-10-15 05:00 UTC is the 14th or the 15th in every time zone (UTC-12 to UTC+14).
    const auto date = util::local_date(1697346000);
    CAPTURE(date);
    CHECK((date == "2023-10-14" || date == "2023-10-15"));
}

TEST_CASE("path round-trips through UTF-8") {
    const std::string s = "E:/Backup/Ünïcödé";
    CHECK(util::path_to_utf8(util::path_from_utf8(s)) == s);
}

TEST_CASE("data protected for the current user comes back only with the same purpose") {
    const std::string secret = "names of private files";
    const auto sealed = util::protect_for_user(secret, "grab test");
    REQUIRE(sealed.has_value());
    CHECK(util::unprotect_for_user(*sealed, "grab test") == secret);
#ifdef _WIN32
    CHECK(sealed->find("private") == std::string::npos);
    CHECK_FALSE(util::unprotect_for_user(*sealed, "something else").has_value());
    CHECK_FALSE(util::unprotect_for_user("not protected at all", "grab test").has_value());
    CHECK(util::unprotect_for_user(*util::protect_for_user("", "p"), "p") == std::string{});
#endif
}

TEST_CASE("a new file is written only where nothing is") {
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-new-file";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "sub");
    CHECK(util::write_new_file(dir / "a.txt", "one"));
    CHECK(util::read_file(dir / "a.txt").value_or("") == "one");
    CHECK_FALSE(util::write_new_file(dir / "a.txt", "two")); // there already: left alone
    CHECK(util::read_file(dir / "a.txt").value_or("") == "one");
    CHECK_FALSE(util::write_new_file(dir / "sub", "x")); // a folder
    CHECK(util::write_new_file(dir / "empty.txt", ""));
    CHECK(std::filesystem::file_size(dir / "empty.txt") == 0);
    std::filesystem::remove_all(dir);
}
