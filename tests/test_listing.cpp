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
          std::vector<std::string>{"rclone", "lsf", "arch_guest:", "--format", "sp", "--dirs-only", "-R",
                                   "--max-depth", "3", "--exclude", ".*/**", "--exclude", ".*"});
}

TEST_CASE("lsf argv for a file search with hidden allowed and --config") {
    auto r = base_request();
    r.mode = Mode::file;
    r.root = "/home";
    r.skip_hidden = false;
    r.rclone_config = util::path_from_utf8("C:\\cfg\\rclone.conf");
    CHECK(build_lsf_argv(r) ==
          std::vector<std::string>{"rclone", "lsf", "arch_guest:/home", "--format", "sp", "--files-only", "-R",
                                   "--max-depth", "3", "--config",
                                   util::path_to_utf8(*r.rclone_config)});
}

TEST_CASE("lsf argv for a path target lists only the parent, one level") {
    auto r = base_request();
    r.target = "learning/Course - Intro to Testing - Jane Doe";
    CHECK(build_lsf_argv(r) ==
          std::vector<std::string>{"rclone", "lsf", "arch_guest:learning", "--format", "sp", "--dirs-only"});
    r.target = "/x";
    CHECK(build_lsf_argv(r)[2] == "arch_guest:/");
}

TEST_CASE("lsf output: dirs end with '/', CRLF tolerated, hidden pruned, glob on the leaf") {
    const std::string out = "games/\r\n"
                            "learning/\n"
                            "learning/Course - Intro to Testing - Jane Doe/\n"
                            "learning/Course - Intro to Testing - Jane Doe/Resources/\n"
                            "learning/.cache/Course old/\n"
                            "misc/Course - Other/\n"
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
    const std::string out = "movies/a.mkv\nmovies/b.mkv/\nmovies/c.txt\n"; // b.mkv/ is a dir
    CHECK(paths_of(parse_lsf_output(r, out)) == std::vector<std::string>{"/home/movies/a.mkv"});
}

TEST_CASE("lsf output for a path target matches the leaf exactly in the parent listing") {
    auto r = base_request();
    r.target = "learning/Course - Intro to Testing - Jane Doe/";
    const std::string out =
        "Course - Intro to Testing - Jane Doe/\nCourse - Other/\n";
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
        "tv/Lioness.2023.S03E08.The.Unravelling.2160p.ATV.WEB-DL.DDP5.1.DV.HDR.H.265-NTb.mkv\n"
        "tv/Lioness.2023.S03E07.Something.2160p.mkv\n"
        "tv/.trash/Lioness.2023.S03E08.old.mkv\n"
        "tv/lioness-e08-sample.MKV\n";
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
    CHECK(paths_of(parse_lsf_output(r, ".config/\nx/.config/\n")) ==
          std::vector<std::string>{".config", "x/.config"});
}

TEST_CASE("lsf --format sp: sizes for files, none for directories, ';' inside names") {
    auto r = base_request();
    r.mode = Mode::file;
    r.target = "knight";
    r.root = "/home";
    const std::string out = "2859284292;tv/A.Knight.S01E01.mkv\n"
                            "-1;tv/knight/\n"
                            "17;tv/knight;notes.txt\n";
    CHECK(parse_lsf_output(r, out) ==
          std::vector<RemoteEntry>{{"/home/tv/A.Knight.S01E01.mkv", 2859284292ULL},
                                   {"/home/tv/knight;notes.txt", 17ULL}});

    r.mode = Mode::folder;
    CHECK(parse_lsf_output(r, out) ==
          std::vector<RemoteEntry>{{"/home/tv/knight", std::nullopt}});
}
