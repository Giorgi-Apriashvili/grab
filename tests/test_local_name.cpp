#include "local_name.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using namespace grab;

TEST_CASE("remote names as rclone writes them on Windows") {
    // Every pair is what rclone 1.75.1 wrote for the same Linux file name (2026-10-19), but the
    // device names, which grab gives a "_".
    const std::vector<std::pair<std::string, std::string>> rclone{
        {"..\\escaped.bin", "..＼escaped.bin"},
        {"..\\..\\AppData\\Roaming\\x.bin", "..＼..＼AppData＼Roaming＼x.bin"},
        {"C:\\Windows\\Temp\\x.bin", "C：＼Windows＼Temp＼x.bin"},
        {"\\\\server\\share\\x.bin", "＼＼server＼share＼x.bin"},
        {"a:b.txt", "a：b.txt"},
        {"x?y*z\"<>|.txt", "x？y＊z＂＜＞｜.txt"},
        {"ctl\x01" "char.txt", "ctl␁char.txt"},
        {"tab\there.txt", "tab␉here.txt"},
        {"trailing.", "trailing．"},
        {"trailing ", "trailing␠"},
        {"two  ", "two ␠"},
        {"twodots..", "twodots.．"},
        {"dotspace. ", "dotspace.␠"},
        {"full＼width.txt", "full‛＼width.txt"},
        {"fullq＂.txt", "fullq‛＂.txt"},
        {"colon：ful.txt", "colon‛：ful.txt"},
        {"quote‛char.txt", "quote‛‛char.txt"},
        {"enddot．", "enddot‛．"},
        {"endspace␠", "endspace‛␠"},
        {"mid．dot.txt", "mid．dot.txt"},
        {"mid␠space.txt", "mid␠space.txt"},
        {"ctlpic␁.txt", "ctlpic␁.txt"},
        {"del\x7F" "char.txt", "del\x7F" "char.txt"},
        {" lead.txt", " lead.txt"},
        {"bad\xFF" "byte.txt", "bad‛FFbyte.txt"},
        {"normal.txt", "normal.txt"},
        {"console.txt", "console.txt"},
    };
    for (const auto& [remote, local] : rclone) {
        INFO(remote);
        CHECK(local::name(remote) == local);
    }
    CHECK(local::name(".") == "．");
    CHECK(local::name("..") == "．．");
    CHECK(local::name("\xED\xA0\x80") == "‛ED‛A0‛80"); // a surrogate isn't UTF-8: byte by byte
    CHECK(local::name("cut\xC3") == "cut‛C3");
    CHECK(local::name("Ünïcödé ✓.mkv") == "Ünïcödé ✓.mkv");
}

TEST_CASE("Windows device names get an underscore") {
    CHECK(local::name("CON") == "CON_");
    CHECK(local::name("nul.txt") == "nul_.txt");
    CHECK(local::name("COM1.log") == "COM1_.log");
    CHECK(local::name("lpt9") == "lpt9_");
    CHECK(local::name("Con.tar.gz") == "Con_.tar.gz");
    CHECK(local::name("AUX") == "AUX_");
    CHECK(local::name("prn.") == "prn．"); // its trailing dot is encoded: no device
    CHECK(local::name("COM¹") == "COM¹_");
    CHECK(local::name("CONIN$") == "CONIN$_");
    CHECK(local::name("COM10") == "COM10");
    CHECK(local::name("console.txt") == "console.txt");
    CHECK(local::is_device("con .txt")); // Windows ignores the spaces before the extension
    CHECK_FALSE(local::is_device("conn"));
    CHECK(local::has_device("season 1/CON/e01.mkv"));
    CHECK(local::has_device("aux.srt"));
    CHECK_FALSE(local::has_device("season 1/e01.mkv"));
}

TEST_CASE("local paths compared as Windows does") {
    CHECK(local::key(util::path_from_utf8("C:\\dl\\a.bin.GRABPART")) == local::key(util::path_from_utf8("c:\\DL\\A.bin.grabpart")));
    CHECK(local::key(util::path_from_utf8("C:\\dl\\Ünï.txt")) == local::key(util::path_from_utf8("C:\\dl\\üNÏ.TXT")));
    CHECK(local::key(util::path_from_utf8("C:\\dl\\a.bin")) != local::key(util::path_from_utf8("C:\\dl\\a.bin.grabpart")));
}

TEST_CASE("a remote path never leaves the download folder") {
    const std::filesystem::path dest = util::path_from_utf8("C:\\Users\\me\\Downloads");
    // Below dest, and every part a plain name that Windows reads as written.
    auto inside = [&](const std::string& remote) {
        INFO(remote);
        const auto rel = local::relative(remote);
        REQUIRE_FALSE(rel.empty());
        CHECK_FALSE(rel.has_root_name());
        CHECK_FALSE(rel.has_root_directory());
        for (const auto& part : rel) {
            CHECK(part != ".");
            CHECK(part != "..");
            CHECK(util::path_to_utf8(part).find_first_of("\\/:") == std::string::npos);
        }
        const auto r = (dest / rel).lexically_normal().lexically_relative(dest);
        CHECK(r == rel);
    };
    // The user's case, and the absolute, network, stream and drive-relative ones.
    for (const char* remote : {"..\\escaped.bin", "..\\..\\AppData\\Roaming\\x.bin", "C:\\Windows\\Temp\\x.bin",
                               "\\\\server\\share\\x.bin", "\\\\?\\C:\\x.bin", "\\x.bin", "C:x.bin", "notes.txt:hidden",
                               "../../x.bin", "./x", "..\\up/inner.txt", "a/../../b", "...", ". .", "CON", "trailing. "}) {
        inside(remote);
    }
    CHECK(dest / local::relative("..\\escaped.bin") == dest / util::path_from_utf8("..＼escaped.bin"));
    CHECK(local::relative("../x") == util::path_from_utf8("．．") / "x");
    CHECK(local::relative("season 1//e01.mkv") == std::filesystem::path("season 1") / "e01.mkv");
    CHECK(local::relative("/abs/path") == std::filesystem::path("abs") / "path");
    CHECK(local::relative("").empty());
    CHECK(local::relative("///").empty());
    CHECK(local::relative("a:b/CON/c?.txt") == util::path_from_utf8("a：b") / "CON_" / util::path_from_utf8("c？.txt"));
}
