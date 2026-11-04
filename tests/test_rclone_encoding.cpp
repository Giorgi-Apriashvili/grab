#include "local_name.hpp"
#include "rclone_encoding.hpp"

#include <doctest/doctest.h>

#include <string>

using namespace grab;
namespace enc = grab::rclone_enc;

TEST_CASE("rclone's Windows encoding, as lib/encoder does it") {
    CHECK(enc::encode("Mission: Impossible (1996)", enc::windows) == "Mission\uFF1A Impossible (1996)");
    CHECK(enc::encode("Mission\uFF1A Impossible (1996)", enc::windows) == "Mission\u201B\uFF1A Impossible (1996)");
    CHECK(enc::encode("a*b?c<d>e|f\"g\\h", enc::windows) == "a\uFF0Ab\uFF1Fc\uFF1Cd\uFF1Ee\uFF5Cf\uFF02g\uFF3Ch");
    CHECK(enc::encode("trail.", enc::windows) == "trail\uFF0E");
    CHECK(enc::encode("trail ", enc::windows) == "trail\u2420");
    CHECK(enc::encode("trail\uFF0E", enc::windows) == "trail\u201B\uFF0E");
    CHECK(enc::encode("x\x01y", enc::windows) == "x\u2401y");
    CHECK(enc::encode("q\u201By", enc::windows) == "q\u201B\u201By"); // the quote itself is always quoted
    CHECK(enc::encode("caf\xE9", enc::windows) == "caf\u201BE9");     // a byte that isn't UTF-8
    CHECK(enc::encode("..", enc::windows) == "\uFF0E\uFF0E");
    CHECK(enc::encode("plain name.txt", enc::windows) == "plain name.txt");
    // Standard: only NUL, '/', control characters, DEL and "." / "..".
    CHECK(enc::encode("a:b/c\x7F", enc::standard) == "a:b\uFF0Fc\u2421");
    CHECK(enc::encode("a\uFF0Fb", enc::standard) == "a\u201B\uFF0Fb");

    for (const std::string name :
         {"Mission: Impossible (1996)", "a*b?c", "trail.", "trail ", "x\x01y", "q\u201By", "caf\xE9", "..", "a\uFF0Fb",
          "x\u201B\uFF1Ay", "\u2420lead", "end\u201B"}) {
        CHECK(enc::decode(enc::encode(name, enc::windows), enc::windows) == name);
        CHECK(enc::decode(enc::encode(name, enc::standard), enc::standard) == name);
    }
}

TEST_CASE("rclone's mapping of a server name agrees with grab's local names") {
    // rclone's FromStandardName(windows, name), what 0.19 wrote. Except the cases G24 is about ('／',
    // '␡', names that are exactly '．'), and device names, which grab handles itself.
    for (const std::string raw : {"Mission: Impossible (1996)", "a*b?c<d>e|f\"g\\h.txt", "trail.", "trail ",
                                  "x\x01y\x1Fz", "already\uFF1Acolon", "q\u201By", "caf\xE9.mp3", "\u2400nul",
                                  "plain", "ends\uFF0E", ".", ".."}) {
        CHECK_MESSAGE(enc::encode(enc::decode(raw, enc::standard), enc::windows) == local::name(raw), raw);
    }
}

TEST_CASE("grab's form of server paths: rclone's Standard one, and bytes that aren't UTF-8 as ‛XX") {
    // As rclone shows them, for every name rclone can handle.
    for (const std::string raw : {"plain name.mkv", "x\x01y", "q‛y", "a:b*c", "␀nul", "a／b", ".."}) {
        CHECK_MESSAGE(enc::from_raw_name(raw) == enc::encode(raw, enc::standard), raw);
    }
    // Names rclone can't show: valid UTF-8 now, and back to their bytes for the server.
    CHECK(enc::from_raw_name("caf\xE9.mp3") == "caf‛E9.mp3");
    CHECK(enc::valid_utf8(enc::from_raw_name("\xFF\xFE bytes \x80")));
    for (const std::string raw : {"caf\xE9.mp3", "q‛y", "x\x01y", "\xFF\xFE", "a‛41b", "plain"}) {
        CHECK_MESSAGE(enc::raw_path(enc::from_raw_name(raw)) == raw, raw);
    }
    // A path part by part, '/' as the separator; absolute and relative.
    const std::string path = "/srv/" + enc::from_raw_name("caf\xE9") + "/" + enc::from_raw_name("x\x01y") + "/f.bin";
    CHECK(enc::raw_path(path) == "/srv/caf\xE9/x\x01y/f.bin");
    CHECK(enc::raw_path("movies/a b.mkv") == "movies/a b.mkv");
    CHECK(enc::raw_path("") == "");
    // The local name is the same from either form.
    CHECK(local::name(enc::from_raw_name("caf\xE9.mp3")) == local::name("caf\xE9.mp3"));
    CHECK(local::name("caf\xE9.mp3") == "caf‛E9.mp3");
}
