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
    REQUIRE(r->opts.dest.has_value());
    CHECK(util::path_to_utf8(*r->opts.dest) == "E:\\Backup\\releases");
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
    REQUIRE(r->opts.dest.has_value());
    CHECK(util::path_to_utf8(*r->opts.dest) == "E:\\Backup");
    CHECK(r->opts.first);
    CHECK(r->opts.dry_run);
    CHECK(r->opts.verbose);
    CHECK(r->opts.remote == "hetzner");
    CHECK(r->opts.depth == 2);
    REQUIRE(r->opts.config.has_value());
    CHECK(util::path_to_utf8(*r->opts.config) == "C:\\g.conf");
}

TEST_CASE("everything after -- goes to rclone verbatim with --rclone") {
    auto r = parse_args(args({"-s", "x", "d", "--rclone", "--", "--bwlimit", "10M", "-f", "--dry-run"}));
    REQUIRE(r.has_value());
    CHECK(r->opts.mode == Mode::file);
    CHECK_FALSE(r->opts.dry_run);
    CHECK(r->opts.extra == std::vector<std::string>{"--bwlimit", "10M", "-f", "--dry-run"});
}

TEST_CASE("grab's own downloads take only connection flags after -- (G186)") {
    auto ok = parse_args(args({"x", "d", "--", "--sftp-chunk-size", "1Mi", "--bind=10.8.0.2", "--timeout", "2m"}));
    REQUIRE(ok.has_value());
    CHECK(ok->opts.extra.size() == 5);
    // "-- --dry-run" still downloaded the big files; now it says what to use instead.
    auto dry = parse_args(args({"-f", "x", "d", "--", "--dry-run"}));
    REQUIRE_FALSE(dry.has_value());
    CHECK(dry.error().find("--dry-run after -- isn't used by grab's downloads") != std::string::npos);
    CHECK(dry.error().find("grab's own -n") != std::string::npos);
    auto limit = parse_args(args({"x", "d", "--", "--bwlimit", "10M"}));
    REQUIRE_FALSE(limit.has_value());
    CHECK(limit.error().find("--limit") != std::string::npos);
    auto filter = parse_args(args({"x", "d", "--", "--exclude", "*.part"}));
    REQUIRE_FALSE(filter.has_value());
    CHECK(filter.error().find("--rclone") != std::string::npos);
}

TEST_CASE("mode defaults to file and the two mode flags are exclusive") {
    auto bare = parse_args(args({"lioness"}));
    REQUIRE(bare.has_value());
    CHECK(bare->action == CliAction::run);
    CHECK(bare->opts.mode == Mode::file);
    CHECK(bare->opts.target == "lioness");
    CHECK_FALSE(bare->opts.dest.has_value()); // asked for later

    auto with_dest = parse_args(args({"lioness s03e08", "E:\\TV"}));
    REQUIRE(with_dest.has_value());
    CHECK(with_dest->opts.mode == Mode::file);
    REQUIRE(with_dest->opts.dest.has_value());

    auto folder = parse_args(args({"-f", "releases"}));
    REQUIRE(folder.has_value());
    CHECK(folder->opts.mode == Mode::folder);
    CHECK_FALSE(folder->opts.dest.has_value());

    CHECK_FALSE(parse_args(args({"-s", "-f", "releases", "E:\\x"})).has_value());
    // Repeating the same mode flag is harmless.
    CHECK(parse_args(args({"-f", "--folder", "releases", "E:\\x"})).has_value());
}

TEST_CASE("positional count and option values are validated") {
    CHECK_FALSE(parse_args(args({})).has_value());
    CHECK_FALSE(parse_args(args({"-f"})).has_value());
    CHECK_FALSE(parse_args(args({"  "})).has_value());
    CHECK_FALSE(parse_args(args({"a", " "})).has_value());
    CHECK_FALSE(parse_args(args({"-f", "a", "b", "c"})).has_value());
    CHECK_FALSE(parse_args(args({"lioness", "s03e08", "E:\\TV"})).has_value()); // unquoted words
    CHECK_FALSE(parse_args(args({"-f", "a", "b", "--depth"})).has_value());
    CHECK_FALSE(parse_args(args({"-f", "a", "b", "--depth", "x"})).has_value());
    CHECK_FALSE(parse_args(args({"-f", "a", "b", "--bogus"})).has_value());
    CHECK_FALSE(parse_args(args({"-f", "a", "b", "-r"})).has_value());
}

TEST_CASE("--limit takes a speed like rclone's --bwlimit") {
    auto r = parse_args(args({"movie.mkv", "--limit", "5M"}));
    REQUIRE(r.has_value());
    CHECK(r->opts.limit == 5u * 1024 * 1024);
    auto bare = parse_args(args({"movie.mkv", "--limit=2.5"}));
    REQUIRE(bare.has_value());
    CHECK(bare->opts.limit == 5u * 1024 * 1024 / 2); // a bare number is MiB/s
    CHECK_FALSE(parse_args(args({"movie.mkv", "--limit"})).has_value());
    CHECK_FALSE(parse_args(args({"movie.mkv", "--limit", "fast"})).has_value());
    auto off = parse_args(args({"movie.mkv", "--limit", "off"})); // no limit, not even the speed schedule
    REQUIRE(off.has_value());
    CHECK(off->opts.limit == 0u);
    auto zero = parse_args(args({"movie.mkv", "--limit", "0"}));
    REQUIRE(zero.has_value());
    CHECK(zero->opts.limit == 0u);
    CHECK_FALSE(parse_args(args({"movie.mkv"}))->opts.limit.has_value()); // the speed schedule decides
}

TEST_CASE("--type, --min-size and --max-size narrow the file hits") {
    using file_type::Kind;
    const auto none = parse_args(args({"lioness"}));
    REQUIRE(none.has_value());
    CHECK_FALSE(none->opts.filter.active());
    const auto r = parse_args(args({"lioness", "--type", "video, audio,video", "--min-size", "1G", "--max-size", "20GiB"}));
    REQUIRE(r.has_value());
    CHECK(r->opts.filter.kinds == std::vector<Kind>{Kind::video, Kind::audio});
    CHECK(r->opts.filter.min_size == std::uint64_t{1} << 30);
    CHECK(r->opts.filter.max_size == std::uint64_t{20} << 30);
    CHECK_FALSE(parse_args(args({"lioness", "--type", "movies"})).has_value());
    CHECK_FALSE(parse_args(args({"lioness", "--type", ","})).has_value());
    CHECK_FALSE(parse_args(args({"lioness", "--min-size", "huge"})).has_value());
    CHECK_FALSE(parse_args(args({"lioness", "--min-size", "2G", "--max-size", "1G"})).has_value());
    CHECK_FALSE(parse_args(args({"-f", "Limits", "--type", "video"})).has_value()); // files only
    CHECK_FALSE(parse_args(args({"lioness", "--type"})).has_value());
}

TEST_CASE("--existing answers the question before it is asked") {
    CHECK_FALSE(parse_args(args({"movie.mkv"}))->opts.existing.has_value());
    CHECK(parse_args(args({"movie.mkv", "--existing", "replace"}))->opts.existing == existing::Choice::replace);
    CHECK(parse_args(args({"movie.mkv", "--existing", "both"}))->opts.existing == existing::Choice::keep_both);
    CHECK(parse_args(args({"movie.mkv", "--existing", "skip"}))->opts.existing == existing::Choice::skip);
    CHECK_FALSE(parse_args(args({"movie.mkv", "--existing", "ask"})).has_value());
    CHECK_FALSE(parse_args(args({"movie.mkv", "--existing", "keep"})).has_value());
    CHECK_FALSE(parse_args(args({"movie.mkv", "--existing"})).has_value());
}

TEST_CASE("--verify and --no-verify override the window's setting for one run") {
    CHECK_FALSE(parse_args(args({"movie.mkv"}))->opts.verify.has_value());
    CHECK(parse_args(args({"movie.mkv", "--verify"}))->opts.verify == true);
    CHECK(parse_args(args({"movie.mkv", "--no-verify"}))->opts.verify == false);
    CHECK(parse_args(args({"movie.mkv", "--no-verify", "--verify"}))->opts.verify == true); // the last one wins
}

TEST_CASE("--rclone downloads with plain rclone, as before") {
    auto r = parse_args(args({"movie.mkv", "--rclone"}));
    REQUIRE(r.has_value());
    CHECK(r->opts.rclone);
    CHECK_FALSE(parse_args(args({"movie.mkv"}))->opts.rclone);
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

TEST_CASE("config subcommand") {
    auto c = parse_args(args({"config"}));
    REQUIRE(c.has_value());
    CHECK(c->action == CliAction::config);
    CHECK_FALSE(c->opts.config.has_value());

    auto with_path = parse_args(args({"config", "-c", "C:\\g.conf"}));
    REQUIRE(with_path.has_value());
    CHECK(with_path->action == CliAction::config);
    REQUIRE(with_path->opts.config.has_value());
    CHECK(util::path_to_utf8(*with_path->opts.config) == "C:\\g.conf");

    CHECK_FALSE(parse_args(args({"config", "stray"})).has_value());
    CHECK_FALSE(parse_args(args({"config", "-f"})).has_value());

    // A folder literally named "config" is still grabbable when a mode flag comes first.
    auto r = parse_args(args({"-f", "config", "E:\\x"}));
    REQUIRE(r.has_value());
    CHECK(r->action == CliAction::run);
    CHECK(r->opts.target == "config");
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
