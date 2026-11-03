#include "cli.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
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

TEST_CASE("everything after -- is kept as it is, for run() to check once the server's engine is known (phase 8)") {
    // Not ours to judge here: engine::check_run_flags does, per engine, before anything connects.
    auto r = parse_args(args({"-s", "x", "d", "--", "--bwlimit", "10M", "-f", "--dry-run", "--sftp-host", "h"}));
    REQUIRE(r.has_value());
    CHECK(r->opts.mode == Mode::file);
    CHECK_FALSE(r->opts.dry_run);
    CHECK(r->opts.extra == std::vector<std::string>{"--bwlimit", "10M", "-f", "--dry-run", "--sftp-host", "h"});
    auto ok = parse_args(args({"x", "d", "--", "--bind=10.8.0.2", "--timeout", "2m"}));
    REQUIRE(ok.has_value());
    CHECK(ok->opts.extra == std::vector<std::string>{"--bind=10.8.0.2", "--timeout", "2m"});
}

TEST_CASE("--rclone is gone: a usage error naming engine = rclone (phase 8, decision 2)") {
    auto bare = parse_args(args({"movie.mkv", "d", "--rclone"}));
    REQUIRE_FALSE(bare.has_value());
    CHECK(bare.error() == "--rclone is gone: grab no longer runs a plain rclone copy. To download from a server through "
                          "rclone until a later grab removes it, set engine = rclone in its section (grab config)");
    // The server's section when -r names it, before or after.
    auto named = parse_args(args({"--rclone", "-f", "x", "d", "-r", "Box.Guest"}));
    REQUIRE_FALSE(named.has_value());
    CHECK(named.error().find("set engine = rclone in [Box.Guest] (grab config)") != std::string::npos);
    CHECK_FALSE(parse_args(args({"movie.mkv", "--rclone=1"})).has_value());
    // After --, it is one of the flags run() checks.
    auto after = parse_args(args({"movie.mkv", "d", "--", "--rclone"}));
    REQUIRE(after.has_value());
    CHECK(after->opts.extra == std::vector<std::string>{"--rclone"});
    CHECK(usage().find("--rclone") == std::string::npos);
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
    // Flags after -- reach only a download: elsewhere they are refused, not ignored.
    CHECK(parse_args(args({"server", "test", "Main", "--", "--bind", "10.0.0.2"})).error_or("") ==
          "`grab server` takes no flags after --; a server's bind, connect_timeout and io_timeout are set in its "
          "section (grab config)");
    CHECK(parse_args(args({"config", "--", "-x"})).error_or("") == "`grab config` takes no flags after --");
    CHECK(parse_args(args({"update", "--", "-x"})).error_or("") == "`grab update` takes no flags after --");
    CHECK(parse_args(args({"--init", "--", "-x"})).error_or("") == "`grab --init` takes no flags after --");
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

TEST_CASE("docs/doc.md's Usage block names the options the help names, --rclone no longer (phase 8 step 11)") {
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
