#include "cli.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace grab;

namespace {
std::vector<std::string> args(std::initializer_list<const char*> list) {
    return std::vector<std::string>(list.begin(), list.end());
}
} // namespace

TEST_CASE("folder mode with positional target and dest") {
    auto r = parse_args(args({"-f", "releases", "E:\\Backup\\releases"}));
    REQUIRE(r.has_value());
    CHECK(r->action == CliAction::run);
    CHECK(r->opts.mode == Mode::folder);
    CHECK(r->opts.target == "releases");
    CHECK(util::path_to_utf8(r->opts.dest) == "E:\\Backup\\releases");
    CHECK_FALSE(r->opts.first);
    CHECK_FALSE(r->opts.dry_run);
    CHECK(r->opts.extra.empty());
}

TEST_CASE("file mode, long flags and options in any order") {
    auto r = parse_args(args({"movie.mkv", "--file", "--first", "-n", "-v", "E:\\Backup", "-r",
                              "hetzner", "--depth", "2", "--config=C:\\g.conf"}));
    REQUIRE(r.has_value());
    CHECK(r->opts.mode == Mode::file);
    CHECK(r->opts.target == "movie.mkv");
    CHECK(util::path_to_utf8(r->opts.dest) == "E:\\Backup");
    CHECK(r->opts.first);
    CHECK(r->opts.dry_run);
    CHECK(r->opts.verbose);
    CHECK(r->opts.remote == "hetzner");
    CHECK(r->opts.depth == 2);
    REQUIRE(r->opts.config.has_value());
    CHECK(util::path_to_utf8(*r->opts.config) == "C:\\g.conf");
}

TEST_CASE("everything after -- goes to rclone verbatim") {
    auto r = parse_args(args({"-s", "x", "d", "--", "--bwlimit", "10M", "-f", "--dry-run"}));
    REQUIRE(r.has_value());
    CHECK(r->opts.mode == Mode::file);
    CHECK_FALSE(r->opts.dry_run);
    CHECK(r->opts.extra == std::vector<std::string>{"--bwlimit", "10M", "-f", "--dry-run"});
}

TEST_CASE("mode is required and exclusive") {
    CHECK_FALSE(parse_args(args({"releases", "E:\\x"})).has_value());
    CHECK_FALSE(parse_args(args({"-s", "-f", "releases", "E:\\x"})).has_value());
    // Repeating the same mode flag is harmless.
    CHECK(parse_args(args({"-f", "--folder", "releases", "E:\\x"})).has_value());
}

TEST_CASE("positional count and option values are validated") {
    CHECK_FALSE(parse_args(args({"-f", "releases"})).has_value());
    CHECK_FALSE(parse_args(args({"-f", "a", "b", "c"})).has_value());
    CHECK_FALSE(parse_args(args({"-f", "a", "b", "--depth"})).has_value());
    CHECK_FALSE(parse_args(args({"-f", "a", "b", "--depth", "x"})).has_value());
    CHECK_FALSE(parse_args(args({"-f", "a", "b", "--bogus"})).has_value());
    CHECK_FALSE(parse_args(args({"-f", "a", "b", "-r"})).has_value());
}

TEST_CASE("help, version and init short-circuit") {
    auto h = parse_args(args({"--help"}));
    REQUIRE(h.has_value());
    CHECK(h->action == CliAction::help);

    auto v = parse_args(args({"--version"}));
    REQUIRE(v.has_value());
    CHECK(v->action == CliAction::version);

    auto i = parse_args(args({"--init", "-c", "C:\\g.conf"}));
    REQUIRE(i.has_value());
    CHECK(i->action == CliAction::init);
    REQUIRE(i->opts.config.has_value());

    CHECK_FALSE(parse_args(args({"--init", "stray"})).has_value());
}

TEST_CASE("absolute remote targets are accepted as names") {
    auto r = parse_args(args({"-f", "/home/alice/releases", "E:\\Backup\\releases"}));
    REQUIRE(r.has_value());
    CHECK(r->opts.target == "/home/alice/releases");
}

TEST_CASE("usage mentions both modes") {
    const auto u = usage();
    CHECK(u.find("--folder") != std::string::npos);
    CHECK(u.find("--file") != std::string::npos);
}
