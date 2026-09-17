#include "listing.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace grab;

TEST_CASE("glob_match follows find -name rules") {
    CHECK(glob_match("releases", "releases"));
    CHECK_FALSE(glob_match("releases", "Releases"));
    CHECK(glob_match("Some.Movie*", "Some.Movie.2013.1080p"));
    CHECK(glob_match("*.mkv", "movie.mkv"));
    CHECK_FALSE(glob_match("*.mkv", "movie.mkv.part"));
    CHECK(glob_match("*Intro*", "Course - Intro to Testing - Jane Doe"));
    CHECK(glob_match("S0?E01", "S01E01"));
    CHECK_FALSE(glob_match("S0?E01", "S011E01"));
    CHECK(glob_match("[abc]x", "bx"));
    CHECK_FALSE(glob_match("[abc]x", "dx"));
    CHECK(glob_match("[!abc]x", "dx"));
    CHECK(glob_match("[a-c]x", "cx"));
    CHECK(glob_match("[]]", "]"));
    CHECK(glob_match("a[", "a[")); // unterminated bracket is literal
    CHECK(glob_match("*", ""));
    CHECK(glob_match("**", "anything"));
    CHECK_FALSE(glob_match("", "x"));
    CHECK(glob_match("", ""));
}

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
          std::vector<std::string>{"rclone", "lsf", "arch_guest:", "--dirs-only", "-R",
                                   "--max-depth", "3", "--exclude", ".*/**", "--exclude", ".*"});
}

TEST_CASE("lsf argv for a file search with hidden allowed and --config") {
    auto r = base_request();
    r.mode = Mode::file;
    r.root = "/home";
    r.skip_hidden = false;
    r.rclone_config = util::path_from_utf8("C:\\cfg\\rclone.conf");
    CHECK(build_lsf_argv(r) ==
          std::vector<std::string>{"rclone", "lsf", "arch_guest:/home", "--files-only", "-R",
                                   "--max-depth", "3", "--config",
                                   util::path_to_utf8(*r.rclone_config)});
}

TEST_CASE("lsf argv for a path target lists only the parent, one level") {
    auto r = base_request();
    r.target = "learning/Course - Intro to Testing - Jane Doe";
    CHECK(build_lsf_argv(r) ==
          std::vector<std::string>{"rclone", "lsf", "arch_guest:learning", "--dirs-only"});
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
    CHECK(parse_lsf_output(base_request(), out) ==
          std::vector<std::string>{"learning/Course - Intro to Testing - Jane Doe",
                                   "misc/Course - Other"});
}

TEST_CASE("lsf output joins onto an absolute root and respects the mode") {
    auto r = base_request();
    r.root = "/home";
    r.target = "*.mkv";
    r.mode = Mode::file;
    const std::string out = "movies/a.mkv\nmovies/b.mkv/\nmovies/c.txt\n"; // b.mkv/ is a dir
    CHECK(parse_lsf_output(r, out) == std::vector<std::string>{"/home/movies/a.mkv"});
}

TEST_CASE("lsf output for a path target matches the leaf exactly in the parent listing") {
    auto r = base_request();
    r.target = "learning/Course - Intro to Testing - Jane Doe/";
    const std::string out =
        "Course - Intro to Testing - Jane Doe/\nCourse - Other/\n";
    CHECK(parse_lsf_output(r, out) ==
          std::vector<std::string>{"learning/Course - Intro to Testing - Jane Doe"});
    r.target = "learning/missing";
    CHECK(parse_lsf_output(r, out).empty());
}

TEST_CASE("a dot target disables hidden pruning") {
    auto r = base_request();
    r.target = ".config";
    const auto argv = build_lsf_argv(r);
    CHECK(std::ranges::find(argv, "--exclude") == argv.end());
    CHECK(parse_lsf_output(r, ".config/\nx/.config/\n") ==
          std::vector<std::string>{".config", "x/.config"});
}
