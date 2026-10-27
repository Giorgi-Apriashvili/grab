#include "listing.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace grab;

TEST_CASE("remote path helpers") {
    CHECK(join_remote("", "learning/x") == "learning/x");
    CHECK(join_remote("/", "x") == "/x");
    CHECK(join_remote("/home", "x") == "/home/x");
    CHECK(join_remote("learning/", "x") == "learning/x");
    CHECK(parent_of("learning/x") == "learning");
    CHECK(parent_of("/x") == "/");
    CHECK(parent_of("/a/b/") == "/a");
    CHECK(parent_of("x") == "");
}

namespace {
std::vector<std::string> paths_of(const std::vector<RemoteEntry>& entries) {
    std::vector<std::string> out;
    for (const auto& e : entries) out.push_back(e.path);
    return out;
}

ListRequest base_request() {
    ListRequest r;
    r.mode = Mode::folder;
    r.target = "Course*";
    r.root = "";
    r.max_depth = 3;
    r.skip_hidden = true;
    r.rclone_exe = "rclone";
    r.rclone_remote = "arch_guest";
    return r;
}
} // namespace

TEST_CASE("lsf argv for a name search under the home root prunes dot-dirs") {
    CHECK(build_lsf_argv(base_request()) ==
          std::vector<std::string>{"rclone", "lsf", "arch_guest:", "--format", "tsp", "--time-format", "unix",
                                   "--csv", "--dirs-only", "-R", "--max-depth", "3", "--exclude", ".*/**", "--exclude", ".*"});
}

TEST_CASE("lsf argv for a file search with hidden allowed and --config") {
    auto r = base_request();
    r.mode = Mode::file;
    r.root = "/home";
    r.skip_hidden = false;
    r.rclone_config = util::path_from_utf8("C:\\cfg\\rclone.conf");
    CHECK(build_lsf_argv(r) ==
          std::vector<std::string>{"rclone", "lsf", "arch_guest:/home", "--format", "tsp", "--time-format", "unix",
                                   "--csv", "--files-only", "-R", "--max-depth", "3", "--config",
                                   util::path_to_utf8(*r.rclone_config)});
}

TEST_CASE("lsf argv for a path target lists only the parent, one level") {
    auto r = base_request();
    r.target = "learning/Course - Intro to Testing - Jane Doe";
    CHECK(build_lsf_argv(r) ==
          std::vector<std::string>{"rclone", "lsf", "arch_guest:learning", "--format", "tsp", "--time-format",
                                   "unix", "--csv", "--dirs-only"});
    r.target = "/x";
    CHECK(build_lsf_argv(r)[2] == "arch_guest:/");
}

TEST_CASE("lsf output: dirs end with '/', CRLF tolerated, hidden pruned, glob on the leaf") {
    const std::string out = "0,-1,games/\r\n"
                            "0,-1,learning/\n"
                            "0,-1,learning/Course - Intro to Testing - Jane Doe/\n"
                            "0,-1,learning/Course - Intro to Testing - Jane Doe/Resources/\n"
                            "0,-1,learning/.cache/Course old/\n"
                            "0,-1,misc/Course - Other/\n"
                            "\n";
    CHECK(paths_of(parse_lsf_output(base_request(), out)) ==
          std::vector<std::string>{"learning/Course - Intro to Testing - Jane Doe",
                                   "misc/Course - Other"});
}

TEST_CASE("lsf output joins onto an absolute root and respects the mode") {
    auto r = base_request();
    r.root = "/home";
    r.target = "*.mkv";
    r.mode = Mode::file;
    const std::string out = "0,1,movies/a.mkv\n0,-1,movies/b.mkv/\n0,1,movies/c.txt\n"; // b.mkv/ is a dir
    CHECK(paths_of(parse_lsf_output(r, out)) == std::vector<std::string>{"/home/movies/a.mkv"});
}

TEST_CASE("lsf output for a path target matches the leaf exactly in the parent listing") {
    auto r = base_request();
    r.target = "learning/Course - Intro to Testing - Jane Doe/";
    const std::string out =
        "0,-1,Course - Intro to Testing - Jane Doe/\n0,-1,Course - Other/\n";
    CHECK(paths_of(parse_lsf_output(r, out)) ==
          std::vector<std::string>{"learning/Course - Intro to Testing - Jane Doe"});
    r.target = "learning/missing";
    CHECK(parse_lsf_output(r, out).empty());
}

TEST_CASE("lsf output with a word query: case-insensitive, any order, hidden still pruned") {
    auto r = base_request();
    r.mode = Mode::file;
    r.target = "LIONESS e08";
    const std::string out =
        "0,1,tv/Lioness.2023.S03E08.The.Unravelling.2160p.ATV.WEB-DL.DDP5.1.DV.HDR.H.265-NTb.mkv\n"
        "0,1,tv/Lioness.2023.S03E07.Something.2160p.mkv\n"
        "0,1,tv/.trash/Lioness.2023.S03E08.old.mkv\n"
        "0,1,tv/lioness-e08-sample.MKV\n";
    CHECK(paths_of(parse_lsf_output(r, out)) ==
          std::vector<std::string>{
              "tv/Lioness.2023.S03E08.The.Unravelling.2160p.ATV.WEB-DL.DDP5.1.DV.HDR.H.265-NTb.mkv",
              "tv/lioness-e08-sample.MKV"});

    r.exact = true; // --exact: the whole name, case-sensitive
    CHECK(parse_lsf_output(r, out).empty());
}

TEST_CASE("a dot target disables hidden pruning") {
    auto r = base_request();
    r.target = ".config";
    const auto argv = build_lsf_argv(r);
    CHECK(std::ranges::find(argv, "--exclude") == argv.end());
    CHECK(paths_of(parse_lsf_output(r, "0,-1,.config/\n0,-1,x/.config/\n")) ==
          std::vector<std::string>{".config", "x/.config"});
}

TEST_CASE("lsf --format tsp --csv: times, sizes for files, none for directories, quoted names") {
    auto r = base_request();
    r.mode = Mode::file;
    r.target = "knight";
    r.root = "/home";
    const std::string out = "1697354553,2859284292,tv/A.Knight.S01E01.mkv\n"
                            "1697000000,-1,tv/knight/\n"
                            "-62135596800,17,\"tv/knight,\"\"notes\"\".txt\"\n"
                            "1697000001,5,\"tv/knight\nmore.txt\"\n";
    CHECK(parse_lsf_output(r, out) ==
          std::vector<RemoteEntry>{{"/home/tv/A.Knight.S01E01.mkv", 2859284292ULL, 1697354553},
                                   {"/home/tv/knight,\"notes\".txt", 17ULL, std::nullopt}, // a time before 1970: unknown
                                   {"/home/tv/knight\nmore.txt", 5ULL, 1697000001}});  // one name with a line break (G59)

    r.mode = Mode::folder;
    CHECK(parse_lsf_output(r, out) ==
          std::vector<RemoteEntry>{{"/home/tv/knight", std::nullopt, 1697000000}});
}

TEST_CASE("one lsf record: the three fields; a time that doesn't parse is only unknown") {
    using Fields = std::vector<std::string>;
    const auto file = parse_lsf_record(Fields{"1697354553", "12", "a;b.mkv"});
    REQUIRE(file.has_value());
    CHECK(file->path == "a;b.mkv");
    CHECK(file->size == 12ULL);
    CHECK(file->modified == 1697354553);
    const auto dir = parse_lsf_record(Fields{"0", "-1", "tv/"});
    REQUIRE(dir.has_value());
    CHECK(dir->path == "tv/");
    CHECK_FALSE(dir->size.has_value());
    CHECK_FALSE(dir->modified.has_value());
    CHECK_FALSE(parse_lsf_record(Fields{"tv/"}).has_value());
    CHECK_FALSE(parse_lsf_record(Fields{"12", "tv/a.mkv"}).has_value());          // the old "sp" form
    CHECK_FALSE(parse_lsf_record(Fields{"5", "size", "tv/a.mkv"}).has_value());   // not a size
    CHECK_FALSE(parse_lsf_record(Fields{"5", "", "tv/a.mkv"}).has_value());
    // An rclone without --time-format's "unix" prints the format's name: right path and size,
    // no date.
    const auto no_time = parse_lsf_record(Fields{"unix", "3", "tv/a.mkv"});
    REQUIRE(no_time.has_value());
    CHECK(no_time->path == "tv/a.mkv");
    CHECK(no_time->size == 3ULL);
    CHECK_FALSE(no_time->modified.has_value());

    ListRequest r;
    r.root = "/home";
    r.target = "a";
    r.mode = Mode::file;
    CHECK(parse_lsf_output(r, "unix,3,tv/a.mkv\n") == std::vector<RemoteEntry>{{"/home/tv/a.mkv", 3ULL, std::nullopt}});
}

TEST_CASE("rclone's CSV: quoted fields, doubled quotes, line breaks inside (G59)") {
    using Records = std::vector<std::vector<std::string>>;
    // As rclone 1.75.1 printed them for Main's /tmp/grab-g6/nl (2026-10-22).
    CHECK(parse_csv("1792651228,5,\" lead.txt\"\n1792651228,6,\"a,b\"\"c.txt\"\n1792614386,13,\"endcr\r\"\n"
                    "1792614386,13,\"notes\n2023.txt\"\n1792614386,12,notes.txt\n") ==
          Records{{"1792651228", "5", " lead.txt"},
                  {"1792651228", "6", "a,b\"c.txt"},
                  {"1792614386", "13", "endcr\r"},
                  {"1792614386", "13", "notes\n2023.txt"},
                  {"1792614386", "12", "notes.txt"}});
    CHECK(parse_csv("a,b\r\nc,\n\n") == Records{{"a", "b"}, {"c", ""}});
    CHECK(parse_csv("x,y") == Records{{"x", "y"}});
    CHECK(parse_csv("").empty());
}

TEST_CASE("a listing's hits: kind, dot-names below the start, the name as the query asks") {
    const HitFilter words(Mode::file, "lioness s03", false, true);
    CHECK(words.keeps("tv/Lioness.S03E08.mkv", false));
    CHECK_FALSE(words.keeps("tv/Lioness.S03E08.mkv", true)); // a folder isn't a file hit
    CHECK_FALSE(words.keeps("Lioness S03/readme.txt", false)); // only the name counts
    CHECK_FALSE(words.keeps(".cache/Lioness.S03E08.mkv", false)); // below a dot-folder
    CHECK_FALSE(words.keeps("", false));
    // Not skipped: a dot-folder's files are hits too; so when the query wants a dot-name.
    CHECK(HitFilter(Mode::file, "lioness s03", false, false).keeps(".cache/Lioness.S03E08.mkv", false));
    CHECK(HitFilter(Mode::folder, ".git", false, true).keeps("src/.git", true));
    // A glob, --exact, and a path target (its name exactly, wherever the listing started).
    CHECK(HitFilter(Mode::file, "*.MKV", false, true).keeps("a/x.mkv", false));
    CHECK_FALSE(HitFilter(Mode::file, "X.mkv", true, true).keeps("a/x.mkv", false));
    const HitFilter path(Mode::folder, "/srv/.config/app", false, true);
    CHECK(path.keeps("app", true));
    CHECK_FALSE(path.keeps("apps", true));
}
