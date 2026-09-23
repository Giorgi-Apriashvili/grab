#include "remote.hpp"

#include "quote.hpp"
#include "util.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <format>
#include <istream>
#include <ostream>
#include <utility>

namespace grab {

bool is_path_target(std::string_view target) {
    return target.find('/') != std::string_view::npos;
}

std::string build_find_command(const FindRequest& req) {
    const bool folder = req.mode == Mode::folder;
    const char* type = folder ? "d" : "f";
    // Folder sizes would need du; files report theirs cheaply.
    const char* print = folder ? "-print0" : "-printf '%s\\t%p\\0'";
    std::string cmd = "find";

    if (is_path_target(req.target)) {
        cmd += ' ';
        cmd += quote::sh_single(req.target);
        cmd += std::format(" -maxdepth 0 -type {} {}", type, print);
    } else {
        for (const auto& root : req.roots) {
            cmd += ' ';
            cmd += root.empty() ? std::string(".") : quote::sh_single(root);
        }
        cmd += std::format(" -mindepth 1 -maxdepth {}", req.max_depth);
        const Query query = make_query(req.target, req.exact);
        // Prune dot-directories unless the user is explicitly looking for a dot-name.
        if (req.skip_hidden && !wants_hidden(query)) {
            cmd += " \\( -name '.*' -prune \\) -o";
        }
        // Filter on the server so only hits cross the network. Consecutive predicates are
        // ANDed by find, and bind tighter than the -o above.
        switch (query.kind) {
        case Query::Kind::words:
            for (const auto& term : query.terms) {
                cmd += " -iname ";
                cmd += quote::sh_single("*" + escape_glob(term) + "*");
            }
            break;
        case Query::Kind::glob:
            cmd += " -iname ";
            cmd += quote::sh_single(query.text);
            break;
        case Query::Kind::exact:
            cmd += " -name ";
            cmd += quote::sh_single(query.text);
            break;
        }
        cmd += std::format(" -type {} {}", type, print);
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

std::vector<RemoteEntry> parse_find_output(std::string_view out, bool sized) {
    std::vector<RemoteEntry> entries;
    std::size_t start = 0;
    while (start < out.size()) {
        auto end = out.find('\0', start);
        if (end == std::string_view::npos) end = out.size();
        std::string_view item = out.substr(start, end - start);
        start = end + 1;

        RemoteEntry e;
        if (sized) {
            const auto tab = item.find('\t');
            if (tab != std::string_view::npos) {
                std::uint64_t size = 0;
                const auto digits = item.substr(0, tab);
                const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), size);
                if (ec == std::errc{} && ptr == digits.data() + digits.size() && !digits.empty()) {
                    e.size = size;
                    item.remove_prefix(tab + 1);
                }
            }
        }
        if (item.starts_with("./")) item.remove_prefix(2);
        if (item.empty()) continue;
        e.path = std::string(item);
        entries.push_back(std::move(e));
    }
    return entries;
}

void rank_matches(std::vector<std::string>& matches, const Query& query) {
    struct Key {
        int tier;
        std::ptrdiff_t depth;
        std::string folded;
    };
    std::vector<std::pair<Key, std::string>> keyed;
    keyed.reserve(matches.size());
    for (auto& m : matches) {
        Key k{match_tier(query, m), std::ranges::count(m, '/'), util::to_lower(m)};
        keyed.emplace_back(std::move(k), std::move(m));
    }
    std::ranges::stable_sort(keyed, [](const auto& a, const auto& b) {
        if (a.first.tier != b.first.tier) return a.first.tier < b.first.tier;
        if (a.first.depth != b.first.depth) return a.first.depth < b.first.depth;
        if (a.first.folded != b.first.folded) return a.first.folded < b.first.folded;
        return a.second < b.second;
    });
    for (std::size_t i = 0; i < keyed.size(); ++i) matches[i] = std::move(keyed[i].second);
}

std::expected<std::vector<std::size_t>, std::string> parse_selection(std::string_view answer,
                                                                     std::size_t count) {
    const auto text = util::trim(answer);
    std::vector<std::size_t> picked;
    if (text == "a" || text == "A" || text == "all") {
        for (std::size_t i = 0; i < count; ++i) picked.push_back(i);
        return picked;
    }

    auto number = [&](std::string_view s) -> std::expected<std::size_t, std::string> {
        std::size_t n = 0;
        const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
        if (s.empty() || ec != std::errc{} || ptr != s.data() + s.size()) {
            return util::failf("'{}' is not a number", s);
        }
        if (n < 1 || n > count) return util::failf("{} is outside 1-{}", n, count);
        return n - 1;
    };
    auto add = [&](std::size_t i) {
        if (std::ranges::find(picked, i) == picked.end()) picked.push_back(i);
    };

    std::string normalized(text);
    std::ranges::replace(normalized, ',', ' ');
    std::size_t pos = 0;
    while (pos < normalized.size()) {
        const auto start = normalized.find_first_not_of(" \t", pos);
        if (start == std::string::npos) break;
        auto end = normalized.find_first_of(" \t", start);
        if (end == std::string::npos) end = normalized.size();
        const std::string_view token = std::string_view(normalized).substr(start, end - start);
        pos = end;

        if (const auto dash = token.find('-'); dash != std::string_view::npos && dash > 0) {
            auto lo = number(token.substr(0, dash));
            if (!lo) return util::fail(lo.error());
            auto hi = number(token.substr(dash + 1));
            if (!hi) return util::fail(hi.error());
            if (*lo <= *hi) {
                for (auto i = *lo; i <= *hi; ++i) add(i);
            } else {
                for (auto i = *lo + 1; i-- > *hi;) add(i);
            }
        } else {
            auto n = number(token);
            if (!n) return util::fail(n.error());
            add(*n);
        }
    }
    if (picked.empty()) return util::fail("nothing selected");
    return picked;
}

std::expected<std::vector<std::string>, std::string>
choose_matches(std::vector<std::string> matches, const Query& query, const PickOptions& pick,
               std::istream& in, std::ostream& err,
               const std::function<std::string(const std::string&)>& describe) {
    if (matches.empty()) return util::fail("no match to choose from");
    rank_matches(matches, query);
    if (matches.size() == 1 || pick.first) return std::vector<std::string>{matches.front()};
    if (pick.all) return matches;

    constexpr std::size_t shown_max = 100;
    const std::size_t shown = std::min(matches.size(), shown_max);
    err << std::format("{} matches:\n", matches.size());
    for (std::size_t i = 0; i < shown; ++i) {
        err << std::format("  [{}] {}{}\n", i + 1, matches[i], describe ? describe(matches[i]) : "");
    }
    if (shown < matches.size()) {
        err << std::format("  ... {} more not shown; refine the search, or answer a for all\n",
                           matches.size() - shown);
    }
    if (!pick.interactive) {
        return util::fail("several matches; rerun with --first, --all or more search words");
    }

    for (int attempt = 0; attempt < 3; ++attempt) {
        err << "Pick: number, ranges (1-5,8), a = all, Enter = 1, q = quit: ";
        err.flush();
        std::string line;
        if (!std::getline(in, line)) return util::fail("aborted (no input)");
        const auto answer = util::trim(line);
        if (answer.empty()) return std::vector<std::string>{matches.front()};
        if (answer == "q" || answer == "Q") return util::fail("aborted");
        auto picked = parse_selection(answer, matches.size());
        if (!picked) {
            err << std::format("invalid choice: {}\n", picked.error());
            continue;
        }
        std::vector<std::string> out;
        out.reserve(picked->size());
        for (const auto i : *picked) out.push_back(matches[i]);
        return out;
    }
    return util::fail("aborted (too many invalid choices)");
}

std::expected<std::filesystem::path, std::string>
ask_destination(std::istream& in, std::ostream& err, const std::filesystem::path& fallback) {
    err << std::format("Save to folder (Enter = {}): ", util::path_to_utf8(fallback));
    err.flush();
    std::string line;
    if (!std::getline(in, line)) return util::fail("aborted (no destination given)");
    auto answer = util::trim(line);
    // Explorer's "Copy as path" and drag-and-drop add quotes.
    if (answer.size() >= 2 && (answer.front() == '"' || answer.front() == '\'') &&
        answer.back() == answer.front()) {
        answer = util::trim(answer.substr(1, answer.size() - 2));
    }
    if (answer.empty()) return fallback;
    return util::path_from_utf8(answer);
}

} // namespace grab
