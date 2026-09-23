#include "remote.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <format>
#include <sstream>
#include <string>
#include <vector>

using namespace grab;

TEST_CASE("find command: words become one -iname per word, over several roots") {
    FindRequest req;
    req.mode = Mode::file;
    req.target = "Lioness  S03E08";
    req.roots = {"/home/alice", "/srv/my data"};
    req.max_depth = 4;
    req.skip_hidden = true;
    CHECK(build_find_command(req) ==
          "find '/home/alice' '/srv/my data' -mindepth 1 -maxdepth 4 "
          "\\( -name '.*' -prune \\) -o -iname '*lioness*' -iname '*s03e08*' -type f -print0 "
          "2>/dev/null");
}

TEST_CASE("find command: words are glob-escaped and shell-quoted") {
    FindRequest req;
    req.mode = Mode::file;
    req.target = "movie's 50%]";
    req.roots = {"/home/alice"};
    req.max_depth = 2;
    req.skip_hidden = false;
    CHECK(build_find_command(req) ==
          "find '/home/alice' -mindepth 1 -maxdepth 2 -iname '*movie'\\''s*' -iname '*50%\\]*' "
          "-type f -print0 2>/dev/null");
}

TEST_CASE("find command: glob gets -iname, --exact gets -name") {
    FindRequest req;
    req.mode = Mode::folder;
    req.target = "Release*";
    req.roots = {"/home/alice"};
    req.max_depth = 3;
    req.skip_hidden = false;
    CHECK(build_find_command(req) ==
          "find '/home/alice' -mindepth 1 -maxdepth 3 -iname 'Release*' -type d -print0 2>/dev/null");

    req.target = "releases";
    req.exact = true;
    CHECK(build_find_command(req) ==
          "find '/home/alice' -mindepth 1 -maxdepth 3 -name 'releases' -type d -print0 2>/dev/null");
}

TEST_CASE("looking for a dot-name disables pruning") {
    FindRequest req;
    req.mode = Mode::folder;
    req.target = ".config";
    req.roots = {"/home/alice"};
    req.skip_hidden = true;
    const auto cmd = build_find_command(req);
    CHECK(cmd.find("-prune") == std::string::npos);
    CHECK(cmd.find("-iname '*.config*'") != std::string::npos);
}

TEST_CASE("a path target only validates existence and type") {
    FindRequest req;
    req.mode = Mode::folder;
    req.target = "/home/alice/releases";
    req.roots = {"/ignored"};
    CHECK(build_find_command(req) ==
          "find '/home/alice/releases' -maxdepth 0 -type d -print0 2>/dev/null");

    req.target = "learning/Course - Intro to Testing - Jane Doe";
    CHECK(build_find_command(req) ==
          "find 'learning/Course - Intro to Testing - Jane Doe' -maxdepth 0 "
          "-type d -print0 2>/dev/null");

    CHECK(is_path_target("/x"));
    CHECK(is_path_target("a/b"));
    CHECK_FALSE(is_path_target("x"));
}

TEST_CASE("a blank root searches the login home") {
    FindRequest req;
    req.mode = Mode::folder;
    req.target = "releases";
    req.roots = {"", "projects"};
    req.max_depth = 2;
    req.skip_hidden = false;
    CHECK(build_find_command(req) ==
          "find . 'projects' -mindepth 1 -maxdepth 2 -iname '*releases*' -type d -print0 2>/dev/null");
}

TEST_CASE("ssh argv: options, identity, known_hosts, extras, host, command") {
    RcloneRemote rc;
    rc.host = "203.0.113.10";
    rc.user = "alice";
    rc.port = 2222;
    rc.key_file = "E:\\keys\\server.pem";
    rc.known_hosts_file = "C:\\Users\\alice\\.ssh\\known_hosts";
    const std::vector<std::string> extra{"-o", "ServerAliveInterval=30"};

    const auto argv = build_ssh_argv("ssh", rc, extra, "find / -print0");
    CHECK(argv == std::vector<std::string>{
                      "ssh", "-T", "-p", "2222", "-i", "E:\\keys\\server.pem", "-o",
                      "IdentitiesOnly=yes", "-o", "UserKnownHostsFile=C:\\Users\\alice\\.ssh\\known_hosts",
                      "-o", "ServerAliveInterval=30", "alice@203.0.113.10", "find / -print0"});
}

TEST_CASE("ssh argv without key or known_hosts") {
    RcloneRemote rc;
    rc.host = "h";
    rc.user = "u";
    const auto argv = build_ssh_argv("C:\\ssh.exe", rc, {}, "cmd");
    CHECK(argv == std::vector<std::string>{"C:\\ssh.exe", "-T", "-p", "22", "u@h", "cmd"});
}

TEST_CASE("find output is NUL separated, trailing NUL tolerated") {
    const std::string out("/a/b\0/a/c d\0", 12);
    CHECK(parse_find_output(out) == std::vector<std::string>{"/a/b", "/a/c d"});
    CHECK(parse_find_output("").empty());
    CHECK(parse_find_output(std::string("\0\0", 2)).empty());
    CHECK(parse_find_output("/no/terminator") == std::vector<std::string>{"/no/terminator"});
    // Home-relative searches come back as ./x; the prefix is dropped.
    CHECK(parse_find_output(std::string("./learning/x\0./y\0", 17)) ==
          std::vector<std::string>{"learning/x", "y"});
}

TEST_CASE("ranking: exact name, then prefix, then depth, then case-insensitive path") {
    const auto q = make_query("releases", false);
    std::vector<std::string> m{"/home/alice/downloads/autoadd/releases", "/srv/old-releases",
                               "/home/alice/releases", "/srv/Releases.2024", "/srv/releases"};
    rank_matches(m, q);
    CHECK(m == std::vector<std::string>{"/srv/releases", "/home/alice/releases",
                                        "/home/alice/downloads/autoadd/releases",
                                        "/srv/Releases.2024", "/srv/old-releases"});

    // A season lists in episode order regardless of case.
    std::vector<std::string> eps{"tv/Lioness.S03E08.mkv", "tv/lioness.S03E02.mkv",
                                 "tv/Lioness.S03E10.mkv"};
    rank_matches(eps, make_query("lioness", false));
    CHECK(eps == std::vector<std::string>{"tv/lioness.S03E02.mkv", "tv/Lioness.S03E08.mkv",
                                          "tv/Lioness.S03E10.mkv"});
}

TEST_CASE("parse_selection: numbers, ranges, all, dedup, errors") {
    using V = std::vector<std::size_t>;
    CHECK(parse_selection("3", 5) == V{2});
    CHECK(parse_selection(" 1-3,5 ", 5) == V{0, 1, 2, 4});
    CHECK(parse_selection("2 4", 5) == V{1, 3});
    CHECK(parse_selection("5-3", 5) == V{4, 3, 2});
    CHECK(parse_selection("1,1,2,1-2", 5) == V{0, 1});
    CHECK(parse_selection("a", 3) == V{0, 1, 2});
    CHECK(parse_selection("all", 2) == V{0, 1});
    CHECK_FALSE(parse_selection("0", 5).has_value());
    CHECK_FALSE(parse_selection("6", 5).has_value());
    CHECK_FALSE(parse_selection("2-9", 5).has_value());
    CHECK_FALSE(parse_selection("x", 5).has_value());
    CHECK_FALSE(parse_selection("1-", 5).has_value());
    CHECK_FALSE(parse_selection(",,", 5).has_value());
}

TEST_CASE("choose_matches: single, --first, --all, non-interactive, interactive") {
    const auto q = make_query("releases", false);
    const std::vector<std::string> two{"/home/alice/downloads/autoadd/releases",
                                       "/home/alice/releases"};
    using V = std::vector<std::string>;
    PickOptions ask;
    ask.interactive = true;

    SUBCASE("single match needs no input") {
        std::istringstream in;
        std::ostringstream err;
        auto r = choose_matches({"/only"}, q, ask, in, err);
        REQUIRE(r.has_value());
        CHECK(*r == V{"/only"});
        CHECK(err.str().empty());
    }
    SUBCASE("--first takes the best, --all takes everything") {
        std::istringstream in;
        std::ostringstream err;
        PickOptions first = ask;
        first.first = true;
        CHECK(choose_matches(two, q, first, in, err) == V{"/home/alice/releases"});
        PickOptions all = ask;
        all.all = true;
        CHECK(choose_matches(two, q, all, in, err) ==
              V{"/home/alice/releases", "/home/alice/downloads/autoadd/releases"});
    }
    SUBCASE("non-interactive lists and fails") {
        std::istringstream in;
        std::ostringstream err;
        PickOptions batch;
        auto r = choose_matches(two, q, batch, in, err);
        REQUIRE_FALSE(r.has_value());
        CHECK(err.str().find("[1] /home/alice/releases") != std::string::npos);
        CHECK(err.str().find("[2] /home/alice/downloads/autoadd/releases") != std::string::npos);
    }
    SUBCASE("Enter picks [1]") {
        std::istringstream in("\n");
        std::ostringstream err;
        CHECK(choose_matches(two, q, ask, in, err) == V{"/home/alice/releases"});
    }
    SUBCASE("a range, after one invalid answer") {
        std::istringstream in("nope\n2-1\n");
        std::ostringstream err;
        auto r = choose_matches(two, q, ask, in, err);
        REQUIRE(r.has_value());
        CHECK(*r == V{"/home/alice/downloads/autoadd/releases", "/home/alice/releases"});
        CHECK(err.str().find("invalid choice") != std::string::npos);
    }
    SUBCASE("q and EOF abort; three invalid answers abort") {
        std::ostringstream err;
        std::istringstream quit("q\n");
        CHECK_FALSE(choose_matches(two, q, ask, quit, err).has_value());
        std::istringstream eof;
        CHECK_FALSE(choose_matches(two, q, ask, eof, err).has_value());
        std::istringstream bad("0\n3\n99\n");
        CHECK_FALSE(choose_matches(two, q, ask, bad, err).has_value());
    }
    SUBCASE("long lists are capped at 100 shown entries") {
        std::vector<std::string> many;
        for (int i = 0; i < 120; ++i) many.push_back(std::format("/r/releases{:03}", i));
        std::istringstream in("a\n");
        std::ostringstream err;
        auto r = choose_matches(many, q, ask, in, err);
        REQUIRE(r.has_value());
        CHECK(r->size() == 120);
        CHECK(err.str().find("[100]") != std::string::npos);
        CHECK(err.str().find("[101]") == std::string::npos);
        CHECK(err.str().find("20 more not shown") != std::string::npos);
    }
}
