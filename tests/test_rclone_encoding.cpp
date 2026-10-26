#include "local_name.hpp"
#include "process.hpp"
#include "rclone_encoding.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

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
    // Except the cases G24 is about ('／', '␡', names that are exactly '．'), and device names,
    // which grab handles itself.
    for (const std::string raw : {"Mission: Impossible (1996)", "a*b?c<d>e|f\"g\\h.txt", "trail.", "trail ",
                                  "x\x01y\x1Fz", "already\uFF1Acolon", "q\u201By", "caf\xE9.mp3", "\u2400nul",
                                  "plain", "ends\uFF0E", ".", ".."}) {
        CHECK_MESSAGE(enc::from_standard_name(raw) == local::name(raw), raw);
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

TEST_CASE("a local name goes to rclone in the form that comes back as itself (G80)") {
    for (const std::string raw : {"Mission: Impossible (1996)", "a*b?c", "trail.", "trail ", "x\x01y", "already\uFF1Acolon",
                                  "q\u201By", "\u2400nul"}) {
        const auto name = local::name(raw);
        CHECK(enc::from_standard_name(enc::to_standard_name(name)) == name);
    }
    CHECK(enc::to_standard_name("Mission\uFF1A Impossible (1996)") == "Mission: Impossible (1996)");
    CHECK(enc::to_standard_name("plain") == "plain");
#ifdef _WIN32
    CHECK(enc::local_path(util::path_from_utf8("D:\\Shows\uFF1A 2024\\Mission\uFF1A Impossible (1996)")).value() ==
          "D:\\Shows: 2024\\Mission: Impossible (1996)");
    CHECK(enc::local_path(util::path_from_utf8("C:\\Users\\me\\Downloads")).value() == "C:\\Users\\me\\Downloads");
    // A share's name is part of the volume, which rclone leaves alone.
    CHECK(enc::local_path(util::path_from_utf8("\\\\nas\\media\\a\uFF1Ab")).value() == "\\\\nas\\media\\a:b");
    // A ‛ before an ordinary character: rclone always writes "‛‛", so no form gives "a‛b".
    const auto refused = enc::local_path(util::path_from_utf8("D:\\a\u201Bb\\x"));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().find("a\u201Bb") != std::string::npos);
    // A byte that isn't UTF-8 can't be on a command line.
    CHECK_FALSE(enc::local_path(util::path_from_utf8("D:\\caf\u201BE9")).has_value());
    // A server's "a\b" is grab's "a＼b", whose Standard form "a\b" a Windows path would split.
    CHECK_FALSE(enc::local_path(util::path_from_utf8("D:\\x\\" + local::name("back\\slash"))).has_value());
#endif
}

#ifdef _WIN32
TEST_CASE("the bundled rclone creates exactly grab's folder names from what grab tells it (G80)") {
    namespace fs = std::filesystem;
    const auto rclone = util::self_exe_path().parent_path().parent_path() / "rclone.exe";
    std::error_code ec;
    if (!fs::is_regular_file(rclone, ec)) {
        MESSAGE("no bundled rclone.exe next to the build; skipped");
        return;
    }
    const auto base = fs::temp_directory_path() / "grab-test-rclone-names";
    fs::remove_all(base, ec);
    // The base itself has a name rclone would re-encode, as a Save-to folder grab created can.
    const auto dest = base / util::path_from_utf8(local::name("Shows: 2024"));
    fs::create_directories(dest);
    proc::RunOptions quiet;
    quiet.detached = true;
    for (const std::string raw : {"Mission: Impossible (1996)", "a*b?c<d>e|f\"g", "trail.", "trail ", "trail.\uFF0E",
                                  "already\uFF1Acolon", "q\u201By", "ctl\x01x"}) {
        const auto want = dest / util::path_from_utf8(local::name(raw));
        const auto told = enc::local_path(want);
        REQUIRE_MESSAGE(told.has_value(), raw);
        const auto ran = proc::run_capture(std::vector<std::string>{util::path_to_utf8(rclone), "mkdir", *told}, quiet);
        REQUIRE(ran.has_value());
        CHECK_MESSAGE(ran->exit_code == 0, ran->err);
        CHECK_MESSAGE(fs::is_directory(want, ec), raw);
    }
    // Nothing anywhere else: one folder per name, all inside grab's Save-to folder.
    std::size_t made = 0;
    for (const auto& e : fs::directory_iterator(dest, ec)) made += e.is_directory() ? 1 : 0;
    CHECK(made == 8);
    std::size_t top = 0;
    for (const auto& e : fs::directory_iterator(base, ec)) top += e.is_directory() ? 1 : 0;
    CHECK(top == 1);
    fs::remove_all(base, ec);
}
#endif
