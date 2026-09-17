#include "remote.hpp"

#include <doctest/doctest.h>

#include <sstream>
#include <string>
#include <vector>

using namespace grab;

TEST_CASE("find command for a folder name over several roots") {
    FindRequest req;
    req.mode = Mode::folder;
    req.target = "releases";
    req.roots = {"/home/alice", "/srv/my data"};
    req.max_depth = 4;
    req.skip_hidden = true;
    CHECK(build_find_command(req) ==
          "find '/home/alice' '/srv/my data' -mindepth 1 -maxdepth 4 "
          "\\( -name '.*' -prune \\) -o -name 'releases' -type d -print0 2>/dev/null");
}

TEST_CASE("find command for a file without hidden pruning") {
    FindRequest req;
    req.mode = Mode::file;
    req.target = "movie's.mkv";
    req.roots = {"/home/alice"};
    req.max_depth = 2;
    req.skip_hidden = false;
    CHECK(build_find_command(req) ==
          "find '/home/alice' -mindepth 1 -maxdepth 2 -name 'movie'\\''s.mkv' -type f -print0 "
          "2>/dev/null");
}

TEST_CASE("looking for a dot-name disables pruning") {
    FindRequest req;
    req.mode = Mode::folder;
    req.target = ".config";
    req.roots = {"/home/alice"};
    req.skip_hidden = true;
    const auto cmd = build_find_command(req);
    CHECK(cmd.find("-prune") == std::string::npos);
    CHECK(cmd.find("-name '.config'") != std::string::npos);
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
          "find . 'projects' -mindepth 1 -maxdepth 2 -name 'releases' -type d -print0 2>/dev/null");
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

TEST_CASE("matches rank shallowest first, then lexical") {
    std::vector<std::string> m{"/home/alice/downloads/autoadd/releases", "/srv/releases",
                               "/home/alice/releases"};
    rank_matches(m);
    CHECK(m == std::vector<std::string>{"/srv/releases", "/home/alice/releases",
                                        "/home/alice/downloads/autoadd/releases"});
}

TEST_CASE("choose_match: single, --first, non-interactive, interactive, quit") {
    std::vector<std::string> two{"/home/alice/downloads/autoadd/releases", "/home/alice/releases"};

    SUBCASE("single match needs no input") {
        std::istringstream in;
        std::ostringstream err;
        auto r = choose_match({"/only"}, false, true, in, err);
        REQUIRE(r.has_value());
        CHECK(*r == "/only");
        CHECK(err.str().empty());
    }
    SUBCASE("--first takes the shallowest") {
        std::istringstream in;
        std::ostringstream err;
        auto r = choose_match(two, true, true, in, err);
        REQUIRE(r.has_value());
        CHECK(*r == "/home/alice/releases");
    }
    SUBCASE("non-interactive lists and fails") {
        std::istringstream in;
        std::ostringstream err;
        auto r = choose_match(two, false, false, in, err);
        REQUIRE_FALSE(r.has_value());
        CHECK(err.str().find("[1] /home/alice/releases") != std::string::npos);
        CHECK(err.str().find("[2] /home/alice/downloads/autoadd/releases") != std::string::npos);
    }
    SUBCASE("interactive pick honours the number, retries once on garbage") {
        std::istringstream in("nope\n2\n");
        std::ostringstream err;
        auto r = choose_match(two, false, true, in, err);
        REQUIRE(r.has_value());
        CHECK(*r == "/home/alice/downloads/autoadd/releases");
        CHECK(err.str().find("invalid choice") != std::string::npos);
    }
    SUBCASE("q aborts") {
        std::istringstream in("q\n");
        std::ostringstream err;
        CHECK_FALSE(choose_match(two, false, true, in, err).has_value());
    }
    SUBCASE("EOF aborts") {
        std::istringstream in;
        std::ostringstream err;
        CHECK_FALSE(choose_match(two, false, true, in, err).has_value());
    }
    SUBCASE("out of range then out of range then out of range aborts") {
        std::istringstream in("0\n3\n99\n");
        std::ostringstream err;
        CHECK_FALSE(choose_match(two, false, true, in, err).has_value());
    }
}
