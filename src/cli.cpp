#include "cli.hpp"

#include "rate.hpp"
#include "rclone_flags.hpp"
#include "util.hpp"

#include <algorithm>
#include <charconv>
#include <string_view>

#ifndef GRAB_VERSION
#define GRAB_VERSION "dev"
#endif

namespace grab {

std::string usage() {
    return R"(grab )" GRAB_VERSION R"( - fetch a file or folder from a Linux server by name

Usage:
  grab [-s|--file | -f|--folder] TARGET [DEST] [options] [-- extra rclone args]
  grab config [-c PATH]
  grab server [list | add [NAME] | trust NAME | remove NAME]
  grab update [--check]
  grab --init | --help | --version

TARGET is searched for under the remote's search_roots. By default every word must appear
in the name, ignoring case, in any order: `grab "lioness s03e08" E:\TV`. A TARGET with
* ? or [ is a glob (case-insensitive); --exact matches the whole name exactly. A TARGET
containing '/' is a remote path (absolute, or relative to the login home), checked as-is.
Without -s or -f, grab looks for files. With several matches you pick from a numbered
list: 3, 1-5,8, a = all, Enter = 1.
DEST is the local parent directory: each item is written as DEST\<name>, created if needed.
Without DEST, grab asks for one after you have picked (Enter = the current directory).
`grab config` and `grab update` are commands; to search for a file named that, use -s.

Commands:
  config               open grab.conf in your editor: [grab] editor, then $VISUAL, then
                       $EDITOR, else notepad. The file is created from the example if missing.
  server list          the configured servers: login, auth, lookup method, host key
  server add [NAME]    set up a server step by step (host, login, host key, search roots);
                       writes grab.conf and grab's rclone.conf, then tests the connection
  server trust NAME    fetch, confirm and pin a server's host key
  server remove NAME   delete a server from grab.conf and grab's rclone.conf
  update               install the latest GitHub release over this copy (Windows), after
                       verifying its SHA-256; --check only reports whether one is available.

Options:
  -s, --file           TARGET is a file (default)
  -f, --folder         TARGET is a folder
  -r, --remote NAME    grab.conf section to use (default: [grab] default_remote)
  -c, --config PATH    grab.conf path (default: %APPDATA%\grab\grab.conf or $GRAB_CONFIG)
      --depth N        override max_depth for this run
      --limit RATE     cap the download speed: 5M, 800K, 2.5 (a bare number is MiB/s), or
                       off. Without --limit, the speed schedule set in grab's window applies
      --first          take the best match instead of asking
      --all            take every match instead of asking
      --exact          match TARGET as a whole name or glob, case-sensitive
      --type KINDS     only files of these kinds, comma-separated: video, audio, image, text,
                       document, archive, program, disc, other (e.g. --type video,audio)
      --min-size SIZE  only files at least this big: 700M, 1.5G, 20GiB (a bare number is MiB)
      --max-size SIZE  only files at most this big
      --fresh          fetch the server's file list now instead of using a saved one
      --verify         check each download against the server's checksum (MD5) afterwards;
      --no-verify      don't. Without either, the setting in grab's window applies (off at first)
      --existing WHAT  a different file already where a download goes (another size, or
                       older than the server's): replace it, both (keep yours and save the
                       new one as "name (2).ext") or skip it. Without it grab asks first, or
                       stops when there is no console to ask on
      --rclone         download with plain rclone copyto / copy and rclone's own progress
                       instead of grab's resumable download over several connections
  -- FLAGS             rclone flags: connection ones (--sftp-*, --bind, --timeout...) for
                       grab's downloads; with --rclone, any rclone flags for its copy
  -n, --dry-run        resolve the target and show what would be fetched, transfer nothing
  -v, --verbose        echo the ssh and rclone command lines before running them
      --init           write a commented example grab.conf to the config path if absent
  -h, --help           show this help
      --version        show version

Searches use a saved list of the server's files when it is under 15 minutes old (instant);
otherwise grab fetches a new list first. Path targets and searches for dot-names go live.
Downloads run over several connections at once and survive interruptions: Ctrl+C keeps
what is on disk, and the same command later continues from there. A file or folder file
already on disk (same size, not older than the server's) is not fetched again.

Exit codes: 0 ok, 1 usage, 2 config, 3 target not found / pick aborted, 4 remote lookup
(ssh or rclone listing) failed, 5 update failed, 6 download failed, 130 stopped by Ctrl+C.
With --rclone, a failed download exits with rclone's own code.
)";
}

namespace {

[[nodiscard]] std::expected<int, std::string> parse_positive_int(std::string_view text,
                                                                  std::string_view what) {
    int value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size() || value < 0) {
        return util::failf("{} must be a non-negative integer, got '{}'", what, text);
    }
    return value;
}

} // namespace

std::expected<CliResult, std::string> parse_args(std::span<const std::string> args) {
    CliResult result;
    std::optional<Mode> mode;
    std::vector<std::string> positionals;
    bool passthrough = false;

    // `grab config` and `grab update` are subcommands only as the first argument, so a folder
    // literally named "config" or "update" is still reachable with a mode flag first.
    std::size_t start = 0;
    if (!args.empty() && args[0] == "config") {
        result.action = CliAction::config;
        start = 1;
    } else if (!args.empty() && args[0] == "update") {
        result.action = CliAction::update;
        start = 1;
    } else if (!args.empty() && args[0] == "server") {
        result.action = CliAction::server;
        start = 1;
    }

    for (std::size_t i = start; i < args.size(); ++i) {
        const std::string& a = args[i];

        if (passthrough) {
            result.opts.extra.push_back(a);
            continue;
        }
        if (a == "--") {
            passthrough = true;
            continue;
        }

        // Support --opt=value as well as --opt value.
        std::string_view name = a;
        std::optional<std::string> inline_value;
        if (a.starts_with("--")) {
            if (const auto eq = a.find('='); eq != std::string::npos) {
                name = std::string_view(a).substr(0, eq);
                inline_value = a.substr(eq + 1);
            }
        }

        auto take_value = [&]() -> std::expected<std::string, std::string> {
            if (inline_value) return *inline_value;
            if (i + 1 >= args.size()) return util::failf("option {} requires a value", name);
            return args[++i];
        };

        if (name == "-s" || name == "--file" || name == "-f" || name == "--folder") {
            const Mode m = (name == "-s" || name == "--file") ? Mode::file : Mode::folder;
            if (mode && *mode != m) return util::fail("-s/--file and -f/--folder are mutually exclusive");
            mode = m;
        } else if (name == "-r" || name == "--remote") {
            auto v = take_value();
            if (!v) return util::fail(v.error());
            result.opts.remote = *v;
        } else if (name == "-c" || name == "--config") {
            auto v = take_value();
            if (!v) return util::fail(v.error());
            result.opts.config = util::path_from_utf8(*v);
        } else if (name == "--depth") {
            auto v = take_value();
            if (!v) return util::fail(v.error());
            auto n = parse_positive_int(*v, "--depth");
            if (!n) return util::fail(n.error());
            result.opts.depth = *n;
        } else if (name == "--limit") {
            auto v = take_value();
            if (!v) return util::fail(v.error());
            auto bytes = *v == "off" || *v == "0" ? std::optional<std::uint64_t>(0) : rate::parse_rate(*v);
            if (!bytes) return util::failf("--limit takes a rate like 5M, 800K or 2.5 (MiB/s), or off; got '{}'", *v);
            result.opts.limit = *bytes;
        } else if (name == "--verify" || name == "--no-verify") {
            result.opts.verify = name == "--verify";
        } else if (name == "--type") {
            auto v = take_value();
            if (!v) return util::fail(v.error());
            std::string_view list = *v;
            while (!list.empty()) {
                const auto comma = list.find(',');
                const auto part = util::trim(list.substr(0, comma));
                list.remove_prefix(comma == std::string_view::npos ? list.size() : comma + 1);
                if (part.empty()) continue;
                const auto kind = file_type::parse(part);
                if (!kind) return util::failf("--type takes {}; got '{}'", file_type::names(), part);
                auto& kinds = result.opts.filter.kinds;
                if (std::ranges::find(kinds, *kind) == kinds.end()) kinds.push_back(*kind);
            }
            if (result.opts.filter.kinds.empty()) return util::failf("--type takes {}", file_type::names());
        } else if (name == "--min-size" || name == "--max-size") {
            auto v = take_value();
            if (!v) return util::fail(v.error());
            const auto bytes = file_type::parse_size(*v);
            if (!bytes) {
                return util::failf("{} takes a size like 700M, 1.5G or 20GiB (a bare number is MiB); got '{}'", name, *v);
            }
            (name == "--min-size" ? result.opts.filter.min_size : result.opts.filter.max_size) = *bytes;
        } else if (name == "--existing") {
            auto v = take_value();
            if (!v) return util::fail(v.error());
            const auto choice = existing::parse(*v);
            if (!choice || *choice == existing::Choice::ask) {
                return util::failf("--existing takes replace, both or skip; got '{}'", *v);
            }
            result.opts.existing = *choice;
        } else if (name == "--first") {
            result.opts.first = true;
        } else if (name == "--all") {
            result.opts.all = true;
        } else if (name == "--exact") {
            result.opts.exact = true;
        } else if (name == "--rclone") {
            result.opts.rclone = true;
        } else if (name == "--fresh") {
            result.opts.fresh = true;
        } else if (name == "-n" || name == "--dry-run") {
            result.opts.dry_run = true;
        } else if (name == "-v" || name == "--verbose") {
            result.opts.verbose = true;
        } else if (name == "--check") {
            result.opts.check = true;
        } else if (name == "--init") {
            result.action = CliAction::init;
        } else if (name == "-h" || name == "--help") {
            result.action = CliAction::help;
            return result;
        } else if (name == "--version") {
            result.action = CliAction::version;
            return result;
        } else if (a.size() > 1 && a.front() == '-') {
            return util::failf("unknown option '{}'", a);
        } else {
            positionals.push_back(a);
        }
    }

    if (result.opts.check && result.action != CliAction::update) {
        return util::fail("--check only applies to `grab update`");
    }
    if (result.action == CliAction::server) {
        if (mode) return util::fail("`grab server` takes no -s/-f");
        const std::string sub = positionals.empty() ? "list" : positionals[0];
        const bool needs_name = sub == "trust" || sub == "remove";
        if (sub != "list" && sub != "add" && !needs_name) {
            return util::failf("unknown `grab server` command '{}' (list, add, trust, remove)", sub);
        }
        if (needs_name && positionals.size() != 2) return util::failf("usage: grab server {} NAME", sub);
        if (positionals.size() > 2 || (sub == "list" && positionals.size() > 1)) {
            return util::fail("too many arguments for `grab server`");
        }
        result.opts.server_args = positionals.empty() ? std::vector<std::string>{"list"} : positionals;
        return result;
    }
    if (result.action == CliAction::init || result.action == CliAction::config ||
        result.action == CliAction::update) {
        if (!positionals.empty() || mode) {
            const char* cmd = result.action == CliAction::init     ? "--init"
                              : result.action == CliAction::config ? "config"
                                                                   : "update";
            return util::failf("`grab {}` takes no TARGET/DEST arguments", cmd);
        }
        return result;
    }

    if (positionals.empty()) return util::fail("expected TARGET: what to search for");
    if (positionals.size() > 2) {
        return util::failf("expected TARGET [DEST], got {} arguments; quote a TARGET that has "
                           "spaces, e.g. \"lioness s03e08\"",
                           positionals.size());
    }
    if (util::trim(positionals[0]).empty()) return util::fail("TARGET must not be empty");

    result.opts.mode = mode.value_or(Mode::file); // a bare `grab NAME` looks for files
    result.opts.target = positionals[0];
    const auto& filter = result.opts.filter;
    if (filter.active() && result.opts.mode == Mode::folder) {
        return util::fail("--type, --min-size and --max-size pick files; they don't apply with -f");
    }
    if (filter.min_size && filter.max_size && *filter.min_size > *filter.max_size) {
        return util::fail("--min-size is bigger than --max-size");
    }
    if (positionals.size() == 2) {
        if (util::trim(positionals[1]).empty()) return util::fail("DEST must not be empty");
        result.opts.dest = util::path_from_utf8(positionals[1]);
    }
    // grab's engine uses only connection flags (rclone_flags.hpp): anything else after `--` would
    // be ignored, or worse ("-- --dry-run" still downloaded the big files, G186).
    if (!result.opts.rclone) {
        // Nor one that would replace the server grab gives rclone (its address, login, host keys).
        if (const auto bad = rclone_flags::first_refused(result.opts.extra)) {
            return util::failf("{} after -- would replace the server's {}, which grab gives rclone itself; change "
                               "the server with Edit in grab's Settings, or add --rclone for a plain rclone copy that "
                               "takes it",
                               bad->flag, bad->what);
        }
        if (const auto bad = rclone_flags::first_unusable(result.opts.extra)) {
            return util::failf("{} after -- isn't used by grab's downloads, which take only connection flags "
                               "(--sftp-*, --bind, --timeout...): {}",
                               bad->flag, bad->hint);
        }
    }
    return result;
}

} // namespace grab
