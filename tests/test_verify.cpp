#include "engine.hpp"
#include "util.hpp"
#include "verify.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace grab;
using verify::Verdict;

TEST_CASE("rclone check's combined report, line by line") {
    const auto lines = verify::parse_combined("= a.mkv\r\n* sub/b c.mkv\n+ missing.txt\n! unreadable.bin\n"
                                              "- only-local.txt\nnoise\n\n=x\n");
    CHECK(lines == std::vector<verify::Line>{{Verdict::same, "a.mkv"},
                                             {Verdict::differs, "sub/b c.mkv"},
                                             {Verdict::missing, "missing.txt"},
                                             {Verdict::error, "unreadable.bin"}});
    CHECK(verify::parse_combined("").empty());
}

TEST_CASE("which checksum rclone compared, from its log") {
    const std::string log = "2026/10/16 07:01:02 INFO  : Local file system at //?/C:/dl: Using md5 for hash comparisons\n"
                            "2026/10/16 07:01:09 NOTICE: Local file system at //?/C:/dl: 0 differences found\n";
    CHECK(verify::hash_used(log) == "md5");
    CHECK(verify::hash_used("NOTICE: Local file system at C:/dl: No common hash found - not using a hash for checks\n")
              .empty());
    CHECK(verify::hash_used("").empty());
    CHECK(verify::unhashed("NOTICE: Local file system at C:/dl: 12 hashes could not be checked\n") == 12);
    CHECK(verify::unhashed(log) == 0);
}

TEST_CASE("the check asks the server for checksums, whatever the common flags say") {
    engine::Context ctx;
    ctx.config.rclone = "rclone";
    ctx.config.rclone_config = util::path_from_utf8("C:\\grab\\rclone.conf");
    ctx.settings.rclone_remote = "Box";
    // A --size-only made check compare sizes only while grab reported an MD5 check (G184), a
    // --log-file hid which checksum was used (G187): only connection flags reach it.
    ctx.settings.common_flags = {"-P", "--sftp-chunk-size", "255Ki", "--sftp-disable-hashcheck", "--size-only",
                                 "--log-file", "C:\\logs\\rclone.log"};
    verify::Request r;
    r.ctx = &ctx;
    r.remote_dir = "movies";
    r.local_dir = util::path_from_utf8("E:\\dl\\movies");
    r.extra = {"--bind", "10.8.0.2", "--fast-list"};
    const auto list = util::path_from_utf8("C:\\tmp\\list.txt");
    CHECK(verify::check_argv(r, list, 4).value() ==
          std::vector<std::string>{"rclone", "check", "Box:movies", util::path_to_utf8(r.local_dir), "--config",
                                   util::path_to_utf8(*ctx.config.rclone_config), "--sftp-chunk-size", "255Ki",
                                   "--sftp-disable-hashcheck", "--bind", "10.8.0.2", "--one-way", "--files-from-raw",
                                   util::path_to_utf8(list), "--no-traverse", "--combined", "-", "--checkers", "4",
                                   "--sftp-connections", "4", "--sftp-disable-hashcheck=false", "-v"});
}

TEST_CASE("rclone hashsum's sums, by path") {
    const auto sums = verify::parse_sums("9DD4E461268C8034F5C8564E155C67A6  CON\r\n"
                                         "d41d8cd98f00b204e9800998ecf8427e  season 1/nul.txt\n"
                                         "                     UNSUPPORTED  aux.srt\n"
                                         "2026/10/19 21:27:00 NOTICE: noise\n\nshort  x\n");
    CHECK(sums.size() == 2);
    CHECK(sums.at("CON") == "9dd4e461268c8034f5c8564e155c67a6");
    CHECK(sums.at("season 1/nul.txt") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(verify::parse_sums("").empty());
}

TEST_CASE("what the row and the command say about a check") {
    verify::Report ok;
    ok.hash = "md5";
    ok.matched = 1;
    CHECK(ok.verified());
    CHECK(verify::summary(ok, 1) == "checksum verified (MD5)");
    CHECK(verify::summary(ok, 1, true) == "1 file verified (MD5)"); // a folder's files
    ok.matched = 13;
    CHECK(verify::summary(ok, 13) == "13 files verified (MD5)");

    verify::Report bad = ok;
    bad.matched = 11;
    bad.differ = {"a", "b"};
    CHECK_FALSE(bad.verified());
    CHECK(verify::summary(bad, 1) == "differs from the server's copy (MD5)");
    CHECK(verify::summary(bad, 13) == "2 of 13 files differ from the server's copy (MD5)");
    CHECK(verify::summary(bad, 1, true) == "1 file differs from the server's copy (MD5)");

    verify::Report partly = ok;
    partly.matched = 10;
    partly.unchecked = 3;
    CHECK_FALSE(partly.verified());
    CHECK(verify::summary(partly, 13) == "10 files verified (MD5); 3 could not be checked");
    CHECK(verify::summary(partly, 1) == "not verified: the checksum could not be computed");

    verify::Report sizes_only;
    sizes_only.matched = 1;
    CHECK_FALSE(sizes_only.verified());
    CHECK(verify::summary(sizes_only, 1) == "not verified: the server can't compute checksums");
}
