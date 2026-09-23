#include "cli.hpp"

#include "util.hpp"

#include <charconv>
#include <string_view>

#ifndef GRAB_VERSION
#define GRAB_VERSION "dev"
#endif

namespace grab {

std::string usage() {
    return R"(grab )" GRAB_VERSION R"( - fetch a file or folder from a Linux server by name

Usage:
  grab (-s|--file | -f|--folder) TARGET DEST [options] [-- extra rclone args]
  grab config [-c PATH]
  grab update [--check]
  grab --init | --help | --version

TARGET is a name to search for under the remote's search_roots (find -name style, globs
allowed), or a remote path containing '/' (absolute, or relative to the login home) that
is checked as-is. DEST is the local parent directory: the file or folder is written as
DEST\<name>, created if needed.

Commands:
  config               open grab.conf in your editor: [grab] editor, then $VISUAL, then
                       $EDITOR, else notepad. The file is created from the example if missing.
  update               install the latest GitHub release over this copy (Windows), after
                       verifying its SHA-256; --check only reports whether one is available.

Options:
  -s, --file           TARGET is a file   (rclone copyto, single-file tuned flags)
  -f, --folder         TARGET is a folder (rclone copy, many-file tuned flags)
  -r, --remote NAME    grab.conf section to use (default: [grab] default_remote)
  -c, --config PATH    grab.conf path (default: %APPDATA%\grab\grab.conf or $GRAB_CONFIG)
      --depth N        override max_depth for this run
      --first          take the shallowest match instead of asking
  -n, --dry-run        resolve the target and print the rclone command, transfer nothing
  -v, --verbose        echo the ssh and rclone command lines before running them
      --init           write a commented example grab.conf to the config path if absent
  -h, --help           show this help
      --version        show version

Exit codes: 0 ok, 1 usage, 2 config, 3 target not found / pick aborted, 4 remote lookup
(ssh or rclone listing) failed, 5 update failed, otherwise rclone's own exit code.
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
        } else if (name == "--first") {
            result.opts.first = true;
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

    if (!mode) return util::fail("specify -s/--file or -f/--folder");
    if (positionals.size() != 2) {
        return util::failf("expected TARGET and DEST, got {} positional argument(s)",
                           positionals.size());
    }
    if (positionals[0].empty()) return util::fail("TARGET must not be empty");
    if (positionals[1].empty()) return util::fail("DEST must not be empty");

    result.opts.mode = *mode;
    result.opts.target = positionals[0];
    result.opts.dest = util::path_from_utf8(positionals[1]);
    return result;
}

} // namespace grab
