#include "cli.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <utility>
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
    CHECK_FALSE(r->opts.connect.bind.has_value());
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

TEST_CASE("-- ends the options: what follows is TARGET and DEST, even when it starts with - (G117, phase 9)") {
    auto dash = parse_args(args({"-s", "--", "-notes.txt", "E:\\Docs"}));
    REQUIRE_MESSAGE(dash.has_value(), dash.error_or(""));
    CHECK(dash->opts.target == "-notes.txt");
    CHECK(util::path_to_utf8(dash->opts.dest.value_or("")) == "E:\\Docs");
    auto alone = parse_args(args({"-n", "--", "-grab-p9-none"}));
    REQUIRE_MESSAGE(alone.has_value(), alone.error_or(""));
    CHECK(alone->opts.target == "-grab-p9-none");
    CHECK(alone->opts.dry_run);
    // Options before it still count; after it, words are words.
    auto words = parse_args(args({"-f", "--", "-v"}));
    REQUIRE(words.has_value());
    CHECK(words->opts.mode == Mode::folder);
    CHECK(words->opts.target == "-v");
    CHECK_FALSE(words->opts.verbose);
    CHECK(parse_args(args({"--", "movie", "E:\\x"}))->opts.target == "movie");
    // A dash word that would become DEST was most likely meant for grab 0.20's rclone: never a
    // folder of that name (`grab movie -- --dry-run` would download for real).
    for (const auto& list : {args({"movie", "--", "-x"}), args({"--", "movie", "-P"}), args({"--", "-a", "-b"})}) {
        const auto refused = parse_args(list);
        REQUIRE_FALSE(refused.has_value());
        CHECK_MESSAGE(refused.error().find("after -- would be DEST, the folder to save into: write .\\") != std::string::npos,
                      refused.error());
    }
    CHECK(parse_args(args({"movie", "--", "--dry-run"})).error_or("").starts_with("--dry-run after -- is an option of grab's"));
    CHECK(parse_args(args({"movie", "--", "-P"})).error_or("") ==
          "-P after -- would be DEST, the folder to save into: write .\\-P for a folder of that name");
    // A DEST given before it, or one that doesn't start with '-', is a DEST.
    CHECK(parse_args(args({"movie", "--", "E:\\x"})).has_value());
    CHECK(parse_args(args({"movie", "E:\\x", "--"})).has_value());
    CHECK(parse_args(args({"movie", "--", ".\\-P"})).has_value());
}

TEST_CASE("the flags grab 0.20 passed on to rclone after -- are refused with what to write instead (phase 9)") {
    const auto error = [](std::initializer_list<const char*> list) { return parse_args(args(list)).error_or(""); };
    CHECK(error({"movie", "--", "--bind", "10.0.0.2"}) ==
          "--bind after -- is an option of grab's now: write --bind 10.0.0.2 before --");
    CHECK(error({"--", "--bind=10.0.0.2", "movie"}) == "--bind after -- is an option of grab's now: write --bind 10.0.0.2 before --");
    CHECK(error({"movie", "d", "--", "--timeout", "2m"}) == "--timeout after -- is an option of grab's now: write --timeout 2m before --");
    CHECK(error({"movie", "--", "--contimeout"}) ==
          "--contimeout after -- is an option of grab's now: write --contimeout TIME before --");
    CHECK(error({"movie", "--", "--bind="}) == "--bind after -- is an option of grab's now: write --bind \"\" before --");
    for (const char* flag : {"--sftp-chunk-size", "--sftp-socks-proxy", "--bwlimit", "--transfers", "--multi-thread-streams"}) {
        CAPTURE(flag);
        const auto refused = error({"movie", "d", "--", flag, "4"});
        CHECK(refused == std::string(flag) + " after -- was a flag for rclone, which this grab doesn't have; leave it "
                                             "out (after --, grab takes only TARGET and DEST)");
    }
    // Never quoted beyond its name: a proxy's password stays off the screen.
    CHECK(error({"x", "--", "--sftp-socks-proxy=fake-user:fake-secret@127.0.0.1:9"}).find("fake-") == std::string::npos);
    // Before `--`, rclone's flags are unknown options, as they always were.
    CHECK(error({"movie", "--sftp-chunk-size", "1M"}) == "unknown option '--sftp-chunk-size'");
    // `grab server`, config, update and --init too, which take none of them either way: their own
    // refusal, never "write it before --".
    CHECK(error({"server", "test", "Main", "--", "--bind", "10.0.0.2"}) ==
          error({"server", "test", "Main", "--bind", "10.0.0.2"}));
    CHECK(error({"server", "test", "Main", "--", "--bind", "10.0.0.2"}).starts_with("`grab server` takes no --bind"));
    CHECK(error({"update", "--", "--timeout=2m"}) == "`grab update` takes no --bind, --contimeout or --timeout");
    CHECK(error({"--init", "--", "--contimeout", "5s"}) == "`grab --init` takes no --bind, --contimeout or --timeout");
}

TEST_CASE("--bind, --contimeout and --timeout: this run's connection settings, checked before anything connects") {
    using std::chrono::milliseconds;
    auto r = parse_args(args({"movie", "d", "--bind", "10.8.0.2", "--contimeout=5s", "--timeout", "2m"}));
    REQUIRE_MESSAGE(r.has_value(), r.error_or(""));
    CHECK(r->opts.connect.bind == "10.8.0.2");
    CHECK(r->opts.connect.connect_timeout == milliseconds(5'000));
    CHECK(r->opts.connect.io_timeout == milliseconds(120'000));
    const auto none = parse_args(args({"movie"}));
    REQUIRE(none.has_value());
    CHECK_FALSE(none->opts.connect.bind);
    CHECK_FALSE(none->opts.connect.connect_timeout);
    CHECK_FALSE(none->opts.connect.io_timeout);
    // A blank --bind: no bind for this run, whatever the server's; --contimeout off: as long as TCP tries.
    CHECK(parse_args(args({"movie", "--bind="}))->opts.connect.bind == "");
    CHECK(parse_args(args({"movie", "--bind", ""}))->opts.connect.bind == "");
    CHECK(parse_args(args({"movie", "--contimeout", "off"}))->opts.connect.connect_timeout == milliseconds(0));
    CHECK(parse_args(args({"movie", "--timeout", "24h"}))->opts.connect.io_timeout == milliseconds(86'400'000));
    const auto error = [](std::initializer_list<const char*> list) { return parse_args(args(list)).error_or(""); };
    CHECK(error({"movie", "--bind", "-oProxyCommand=x"}) == "--bind -oProxyCommand=x: the address can't start with -");
    CHECK(error({"movie", "--bind=10.0.0.2;x"}).starts_with("--bind 10.0.0.2;x: "));
    CHECK(error({"movie", "--bind"}) == "option --bind requires a value");
    CHECK(error({"movie", "--contimeout", "soon"}) == "--contimeout soon: not a time grab can read (such as 30s, 1m30s or off)");
    // No limit would leave a stalled read that Cancel and Ctrl+C can't end.
    for (const char* time : {"off", "0", "500ms", "25h", "abc"}) {
        CHECK_MESSAGE(error({"movie", "--timeout", time}) ==
                          std::format("--timeout {}: grab takes a time from 1s to 24h (such as 30s or 1m30s; with none, a "
                                      "stalled download couldn't be cancelled)",
                                      time),
                      time);
    }
    // Only a download connects with them: elsewhere they are refused, not ignored.
    CHECK(error({"server", "test", "Main", "--bind", "10.0.0.2"}) ==
          "`grab server` takes no --bind, --contimeout or --timeout; a server's bind, connect_timeout and io_timeout are "
          "set in its section (grab config)");
    CHECK(error({"config", "--timeout", "1m"}) == "`grab config` takes no --bind, --contimeout or --timeout");
    CHECK(error({"update", "--contimeout", "1m"}) == "`grab update` takes no --bind, --contimeout or --timeout");
    CHECK(error({"--init", "--bind", "10.0.0.2"}) == "`grab --init` takes no --bind, --contimeout or --timeout");
    CHECK(error({"config", "--", "x"}) == "`grab config` takes no TARGET/DEST arguments");
    CHECK(error({"update", "--", "-x"}) == "`grab update` takes no TARGET/DEST arguments");
}

TEST_CASE("--first and --all don't go together (G117)") {
    CHECK(parse_args(args({"movie", "--first", "--all"})).error_or("") ==
          "--first and --all don't go together: --first takes the best match, --all every match");
    CHECK(parse_args(args({"movie", "--first"})).has_value());
    CHECK(parse_args(args({"movie", "--all"})).has_value());
}

TEST_CASE("--rclone is gone, and so is rclone") {
    auto bare = parse_args(args({"movie.mkv", "d", "--rclone"}));
    REQUIRE_FALSE(bare.has_value());
    CHECK(bare.error() == "--rclone is gone: this grab has no rclone; it downloads with its own SSH client");
    CHECK(parse_args(args({"--rclone", "-f", "x", "d", "-r", "Box.Guest"})).error_or("") == bare.error());
    CHECK_FALSE(parse_args(args({"movie.mkv", "--rclone=1"})).has_value());
    CHECK(usage().find("--rclone") == std::string::npos);
    CHECK(usage().find("rclone (") == std::string::npos);
    CHECK(usage().find("engine") == std::string::npos);
    CHECK(usage().find("GRAB_ENGINE") == std::string::npos);
    CHECK(usage().find("FLAGS") == std::string::npos);
}

TEST_CASE("a switch refuses a value: --verify=false no longer turns verification on (G108)") {
    auto verify = parse_args(args({"movie.mkv", "--verify=false"}));
    REQUIRE_FALSE(verify.has_value());
    CHECK(verify.error() == "option --verify takes no value (--no-verify turns verification off)");
    for (const char* bad : {"--all=no", "--first=0", "--no-verify=1", "--exact=", "--fresh=yes", "--dry-run=false",
                            "--verbose=0", "--file=x", "--folder=x"}) {
        CAPTURE(bad);
        auto r = parse_args(args({"movie.mkv", bad}));
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().ends_with(" takes no value"));
    }
    CHECK(parse_args(args({"update", "--check=1"})).error_or("") == "option --check takes no value");
    CHECK(parse_args(args({"--init=1"})).error_or("") == "option --init takes no value");
    // Options that take a value still take it either way.
    CHECK(parse_args(args({"movie.mkv", "--depth=2"}))->opts.depth == 2);
    CHECK(parse_args(args({"movie.mkv", "--all"}))->opts.all);
}

TEST_CASE("grab server: list, add [NAME], and edit, test, trust and remove with a NAME (phase 7)") {
    auto bare = parse_args(args({"server"}));
    REQUIRE(bare.has_value());
    CHECK(bare->action == CliAction::server);
    CHECK(bare->opts.server_args == std::vector<std::string>{"list"});
    for (const char* sub : {"edit", "test", "trust", "remove"}) {
        CAPTURE(sub);
        auto named = parse_args(args({"server", sub, "Box.Guest"}));
        REQUIRE(named.has_value());
        CHECK(named->opts.server_args == std::vector<std::string>{sub, "Box.Guest"});
        auto unnamed = parse_args(args({"server", sub}));
        REQUIRE_FALSE(unnamed.has_value());
        CHECK(unnamed.error() == std::string("usage: grab server ") + sub + " NAME");
    }
    auto add = parse_args(args({"server", "add"}));
    REQUIRE(add.has_value());
    CHECK(add->opts.server_args == std::vector<std::string>{"add"});
    auto unknown = parse_args(args({"server", "rename", "a"}));
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().find("(list, add, edit, test, trust, remove)") != std::string::npos);
    CHECK(usage().find("grab server [list | add [NAME] | edit NAME | test NAME | trust NAME | remove NAME]") !=
          std::string::npos);
    CHECK(usage().find("rclone.conf with") == std::string::npos);
    // After `--`, a server's NAME too.
    CHECK(parse_args(args({"server", "test", "--", "-odd"}))->opts.server_args == std::vector<std::string>{"test", "-odd"});
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

namespace {
// The options an options list names, one a line ("  -r, --remote NAME   ...", "  -- FLAGS ..."):
// the long name, or "--" for the flags after it, and with `short_names` the short one too ("-r").
// Continuation lines are indented further (one may start with a flag: "--timeout TIME (1s to 24h
// ...").
std::set<std::string> option_names(std::string_view block, bool short_names = false) {
    std::set<std::string> out;
    while (!block.empty()) {
        const auto eol = std::min(block.find('\n'), block.size());
        const std::string_view line = block.substr(0, eol);
        block.remove_prefix(std::min(eol + 1, block.size()));
        const auto indent = line.find_first_not_of(' ');
        if (indent == std::string_view::npos || indent < 2 || indent > 6 || line[indent] != '-') continue;
        std::string_view l = util::trim(line);
        if (l.size() > 4 && l[1] != '-' && l.substr(2, 2) == ", ") { // "-r, "
            if (short_names) out.emplace(l.substr(0, 2));
            l.remove_prefix(4);
        }
        out.emplace(l.substr(0, l.find(' ')));
    }
    return out;
}

// Every option a text mentions, in an options list, prose or examples: "-n", "--first", "--type"
// in "--type video", "--" before the connection flags. One starts after a space, a line break, a
// backtick or one of ( [ |, so "1-5,8" and "case-sensitive" aren't options.
std::set<std::string> mentioned_options(std::string_view text) {
    constexpr std::string_view before = " \n`([|";
    const auto name_char = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; };
    std::set<std::string> out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '-' || (i > 0 && before.find(text[i - 1]) == std::string_view::npos)) continue;
        const std::size_t name = i + (i + 1 < text.size() && text[i + 1] == '-' ? 2 : 1);
        std::size_t j = name;
        while (j < text.size() && name_char(text[j])) ++j;
        if (j > name) {
            out.emplace(text.substr(i, j - i));
        } else if (name == i + 2 && (j == text.size() || text[j] == ' ')) {
            out.emplace("--");
        }
        i = j - 1;
    }
    return out;
}

// A text file of the source tree, without the CRs a checkout may have added.
std::string source_file(std::string_view relative) {
    auto text = util::read_file(util::path_from_utf8(GRAB_SOURCE_DIR) / util::path_from_utf8(relative));
    REQUIRE_MESSAGE(text, text.error_or(""));
    std::erase(*text, '\r');
    return std::move(*text);
}

// The help's options list: from "Options:" to the blank line after it.
std::string_view help_options(const std::string& help) {
    const auto options = help.find("Options:\n");
    REQUIRE(options != std::string::npos);
    return std::string_view(help).substr(options, help.find("\n\n", options) - options);
}
} // namespace

TEST_CASE("docs/doc.md's Usage block names the options the help names, `--` and the connection options (phase 9)") {
    const std::string doc = source_file("docs/doc.md");
    const auto start = doc.find("\n## Usage\n\n```\n");
    REQUIRE(start != std::string::npos);
    const auto body = start + std::string_view("\n## Usage\n\n```\n").size();
    const auto end = doc.find("```", body);
    REQUIRE(end != std::string::npos);
    const auto documented = option_names(std::string_view(doc).substr(body, end - body));

    const std::string help = usage();
    const auto helped = option_names(help_options(help));
    CHECK(helped.contains("--"));
    CHECK(helped.contains("--bind"));
    CHECK(helped.contains("--contimeout"));
    CHECK(helped.contains("--timeout"));
    CHECK_FALSE(helped.contains("--rclone"));
    // The doc's block leaves out what its first line and the commands show.
    for (const auto& name : helped) {
        if (name == "--file" || name == "--folder" || name == "--help" || name == "--version") continue;
        CHECK_MESSAGE(documented.contains(name), name);
    }
    for (const auto& name : documented) CHECK_MESSAGE(helped.contains(name), name);
}

TEST_CASE("every option the README's Basic usage mentions is one the help names") {
    const std::string readme = source_file("README.md");
    const auto start = readme.find("\n## Basic usage\n");
    REQUIRE(start != std::string::npos);
    const auto end = readme.find("\n## ", start + 1);
    REQUIRE(end != std::string::npos);
    const std::string_view section = std::string_view(readme).substr(start, end - start);
    const auto block = section.find("```\n");
    REQUIRE(block != std::string_view::npos);
    const auto block_end = section.find("```", block + 4);
    REQUIRE(block_end != std::string_view::npos);
    const auto listed = option_names(section.substr(block + 4, block_end - block - 4));

    const std::string help = usage();
    const auto helped = option_names(help_options(help));
    const auto spellings = option_names(help_options(help), true);
    CHECK(spellings.contains("-n"));
    CHECK(spellings.contains("--dry-run"));
    // Its options list, then every option its prose and examples use.
    CHECK(listed.size() >= 5);
    for (const auto& name : listed) CHECK_MESSAGE(helped.contains(name), name);
    const auto mentioned = mentioned_options(section);
    CHECK(mentioned.contains("--first"));
    CHECK(mentioned.contains("-f"));
    for (const auto& name : mentioned) CHECK_MESSAGE(spellings.contains(name), name);
}

TEST_CASE("mentioned_options finds options, not ranges or hyphenated words") {
    const auto found =
        mentioned_options("grab [-s|--file] x (`-n`) --type video,audio 1-5,8 case-sensitive\n- a\n[-- FLAGS]");
    CHECK(found == std::set<std::string>{"-s", "--file", "-n", "--type", "--"});
}
