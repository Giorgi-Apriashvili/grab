#include "local_name.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace grab;

TEST_CASE("remote names as rclone writes them on Windows") {
    // Every pair is what rclone 1.75.1 wrote for the same Linux file name (2026-10-19; over SFTP
    // from Main 2026-10-22, review G24: rclone's own replacements in a server name), but the
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
        {"AC／DC.txt", "AC／DC.txt"},
        {"del␡.txt", "del\x7F.txt"},
        {"．", "．"},
        {"．．", "．．"},
        {"‛．", "‛．"},
        {"‛．．", "‛‛．‛．"},
        {"q‛／.txt", "q‛／.txt"},
        {"q‛‛.txt", "q‛‛.txt"},
        {"x‛．", "x‛‛‛．"},
        {"a‛＼b.txt", "a‛‛‛＼b.txt"},
        {"ends‛", "ends‛‛"},
        {"‛plain.txt", "‛‛plain.txt"},
        {"lt＜gt＞.txt", "lt‛＜gt‛＞.txt"},
        {"␀zero.txt", "␀zero.txt"},
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
    CHECK(local::differs_from_rclone("season 1/CON/e01.mkv"));
    CHECK(local::differs_from_rclone("aux.srt"));
    CHECK_FALSE(local::differs_from_rclone("season 1/e01.mkv"));
    CHECK_FALSE(local::differs_from_rclone("AC／DC/a:b.txt")); // rclone names those the same
}

TEST_CASE("names shaped like 8.3 short names get an underscore (G167)") {
    // On a volume that makes short names, "REPORT~1.DOC" also names "Report for the board.docx".
    CHECK(local::name("REPORT~1.DOC") == "REPORT~1_.DOC");
    CHECK(local::name("repor~12.doc") == "repor~12_.doc");
    CHECK(local::name("report~12.doc") == "report~12.doc"); // 9 before the period: never short
    CHECK(local::name("PROGRA~1") == "PROGRA~1_");
    CHECK(local::name("AB12C3~1.TXT") == "AB12C3~1_.TXT");
    CHECK(local::name("a~1") == "a~1_");
    CHECK(local::name("REPORT~1.DOCX") == "REPORT~1.DOCX"); // a short name's extension has 3
    CHECK(local::name("LONGNAME~1.TXT") == "LONGNAME~1.TXT"); // and its name 8
    CHECK(local::name("~1.txt") == "~1.txt");
    CHECK(local::name("a~b.txt") == "a~b.txt");
    CHECK(local::name("a~1.tar.gz") == "a~1.tar.gz");
    CHECK(local::name("Ä~1.txt") == "Ä~1.txt"); // Windows makes them of ASCII
    CHECK(local::is_short_name("x~9"));
    CHECK(local::differs_from_rclone("docs/PROGRA~1/a.txt"));
}

TEST_CASE("bidi controls are dropped from a saved name and shown as marks (G51)") {
    CHECK(local::name("invoice\u202Efdp.exe") == "invoicefdp.exe");
    CHECK(local::name("a\u2066b\u2069\u200F\u061C.txt") == "ab.txt");
    CHECK(local::name("\u202E") == "_");
    CHECK(local::name("end.\u202E") == "end．"); // what's left is encoded as a whole
    CHECK(local::name("\u202E..") == "．．");
    CHECK(local::name("bad\xFF\u202E") == "bad‛FF");
    CHECK(local::differs_from_rclone("x/invoice\u202Efdp.exe"));
    CHECK(local::bidi_visible("invoice\u202Efdp.exe") == "invoice⟨U+202E⟩fdp.exe");
    CHECK(local::bidi_visible("a\u200Eb\xFF") == "a⟨U+200E⟩b\xFF");
    CHECK(local::bidi_visible("plain ✓") == "plain ✓");
}

TEST_CASE("local paths compared as Windows does") {
    CHECK(local::key(util::path_from_utf8("C:\\dl\\a.bin.GRABPART")) == local::key(util::path_from_utf8("c:\\DL\\A.bin.grabpart")));
    CHECK(local::key(util::path_from_utf8("C:\\dl\\Ünï.txt")) == local::key(util::path_from_utf8("C:\\dl\\üNÏ.TXT")));
    CHECK(local::key(util::path_from_utf8("C:\\dl\\a.bin")) != local::key(util::path_from_utf8("C:\\dl\\a.bin.grabpart")));
}

TEST_CASE("one folder spelled two ways is one place for a download") {
    // The window takes the Save-to folder as typed, normalized (review G44).
    const auto typed = util::path_from_utf8("d:/Media/").lexically_normal();
    const auto picked = util::path_from_utf8("D:\\media");
    CHECK(local::key(typed / "big.bin") == local::key(picked / "BIG.bin"));
    CHECK(local::key(typed / local::relative("Season 1")) == local::key(picked / "season 1"));
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

TEST_CASE("names rclone takes for one on Windows (G25)") {
    const std::vector<std::string> paths{"Docs/a.txt", "docs/b.txt", "README", "readme", "caf\u00e9.txt", "cafe\u0301.txt",
                                         "plain.txt", "sub/x.txt", "sub/y.txt"};
    const auto clash = local::clashing(paths);
    CHECK(clash == std::unordered_set<std::string>{"Docs/a.txt", "docs/b.txt", "README", "readme", "caf\u00e9.txt",
                                                    "cafe\u0301.txt"});
    CHECK(local::clashing(std::vector<std::string>{"a/b", "a/c", "b"}).empty());
    CHECK(local::fold(util::path_from_utf8("Caf\u00e9")) == local::fold(util::path_from_utf8("CAFE\u0301")));
    CHECK(local::key(util::path_from_utf8("Caf\u00e9")) != local::key(util::path_from_utf8("CAFE\u0301"))); // two to NTFS
}
