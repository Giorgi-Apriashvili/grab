#include "remote.hpp"

#include "quote.hpp"
#include "util.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <istream>
#include <ostream>

namespace grab {

bool is_absolute_target(std::string_view target) { return target.starts_with('/'); }

std::string build_find_command(const FindRequest& req) {
    const char* type = req.mode == Mode::folder ? "d" : "f";
    std::string cmd = "find";

    if (is_absolute_target(req.target)) {
        cmd += ' ';
        cmd += quote::sh_single(req.target);
        cmd += std::format(" -maxdepth 0 -type {} -print0", type);
    } else {
        for (const auto& root : req.roots) {
            cmd += ' ';
            cmd += quote::sh_single(root);
        }
        cmd += std::format(" -mindepth 1 -maxdepth {}", req.max_depth);
        // Prune dot-directories unless the user is explicitly looking for a dot-name.
        if (req.skip_hidden && !req.target.starts_with('.')) {
            cmd += " \\( -name '.*' -prune \\) -o";
        }
        cmd += " -name ";
        cmd += quote::sh_single(req.target);
        cmd += std::format(" -type {} -print0", type);
    }
    cmd += " 2>/dev/null";
    return cmd;
}

std::vector<std::string> build_ssh_argv(const std::string& ssh_exe, const RcloneRemote& remote,
                                        std::span<const std::string> ssh_options,
                                        const std::string& remote_command) {
    std::vector<std::string> argv{ssh_exe, "-T", "-p", std::to_string(remote.port)};
    if (remote.key_file) {
        argv.insert(argv.end(), {"-i", *remote.key_file, "-o", "IdentitiesOnly=yes"});
    }
    if (remote.known_hosts_file) {
        argv.insert(argv.end(), {"-o", "UserKnownHostsFile=" + *remote.known_hosts_file});
    }
    argv.insert(argv.end(), ssh_options.begin(), ssh_options.end());
    argv.push_back(remote.user + "@" + remote.host);
    argv.push_back(remote_command);
    return argv;
}

std::vector<std::string> parse_find_output(std::string_view out) {
    std::vector<std::string> paths;
    std::size_t start = 0;
    while (start < out.size()) {
        auto end = out.find('\0', start);
        if (end == std::string_view::npos) end = out.size();
        if (end > start) paths.emplace_back(out.substr(start, end - start));
        start = end + 1;
    }
    return paths;
}

void rank_matches(std::vector<std::string>& matches) {
    auto depth = [](const std::string& p) { return std::ranges::count(p, '/'); };
    std::ranges::stable_sort(matches, [&](const std::string& a, const std::string& b) {
        const auto da = depth(a);
        const auto db = depth(b);
        if (da != db) return da < db;
        return a < b;
    });
}

std::expected<std::string, std::string> choose_match(std::vector<std::string> matches, bool first,
                                                     bool interactive, std::istream& in,
                                                     std::ostream& err) {
    if (matches.empty()) return util::fail("no match to choose from");
    rank_matches(matches);
    if (matches.size() == 1 || first) return matches.front();

    err << std::format("{} matches:\n", matches.size());
    for (std::size_t i = 0; i < matches.size(); ++i) {
        err << std::format("  [{}] {}\n", i + 1, matches[i]);
    }
    if (!interactive) {
        return util::fail("ambiguous target; rerun with --first or pass the absolute remote path");
    }

    for (int attempt = 0; attempt < 3; ++attempt) {
        err << std::format("Pick [1-{}] or q to quit: ", matches.size());
        err.flush();
        std::string line;
        if (!std::getline(in, line)) return util::fail("aborted (no input)");
        const auto t = util::trim(line);
        if (t == "q" || t == "Q") return util::fail("aborted");
        int n = 0;
        const auto [ptr, ec] = std::from_chars(t.data(), t.data() + t.size(), n);
        if (ec == std::errc{} && ptr == t.data() + t.size() && n >= 1 &&
            static_cast<std::size_t>(n) <= matches.size()) {
            return matches[static_cast<std::size_t>(n) - 1];
        }
        err << "invalid choice\n";
    }
    return util::fail("aborted (too many invalid choices)");
}

} // namespace grab
