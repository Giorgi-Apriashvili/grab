#include "listing.hpp"

#include "rclone.hpp"
#include "remote.hpp"
#include "util.hpp"

namespace grab {

bool glob_match(std::string_view pattern, std::string_view name) {
    constexpr auto npos = std::string_view::npos;
    std::size_t p = 0;
    std::size_t n = 0;
    std::size_t star_p = npos; // position after the most recent '*'
    std::size_t star_n = 0;    // name position that '*' currently covers up to

    while (n < name.size()) {
        if (p < pattern.size()) {
            const char c = pattern[p];
            if (c == '*') {
                star_p = ++p;
                star_n = n;
                continue;
            }
            if (c == '?') {
                ++p;
                ++n;
                continue;
            }
            if (c == '[') {
                std::size_t scan = p + 1;
                const bool negate =
                    scan < pattern.size() && (pattern[scan] == '!' || pattern[scan] == '^');
                if (negate) ++scan;
                if (scan < pattern.size() && pattern[scan] == ']') ++scan; // literal ']' first
                const std::size_t close = pattern.find(']', scan);
                if (close != npos) {
                    bool matched = false;
                    for (std::size_t k = p + 1 + (negate ? 1 : 0); k < close;) {
                        if (k + 2 < close && pattern[k + 1] == '-') {
                            if (pattern[k] <= name[n] && name[n] <= pattern[k + 2]) matched = true;
                            k += 3;
                        } else {
                            if (pattern[k] == name[n]) matched = true;
                            ++k;
                        }
                    }
                    if (matched != negate) {
                        p = close + 1;
                        ++n;
                        continue;
                    }
                    // no match: fall through to backtracking
                } else if (c == name[n]) { // unterminated '[' is literal
                    ++p;
                    ++n;
                    continue;
                }
            } else if (c == name[n]) {
                ++p;
                ++n;
                continue;
            }
        }
        if (star_p != npos) {
            p = star_p;
            n = ++star_n;
            continue;
        }
        return false;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

std::string join_remote(std::string_view root, std::string_view rel) {
    while (rel.starts_with('/')) rel.remove_prefix(1);
    if (root.empty()) return std::string(rel);
    std::string out(root);
    if (!out.ends_with('/')) out += '/';
    out += rel;
    return out;
}

std::string parent_of(std::string_view path) {
    while (path.size() > 1 && path.ends_with('/')) path.remove_suffix(1);
    const auto slash = path.rfind('/');
    if (slash == std::string_view::npos) return {};
    if (slash == 0) return "/";
    return std::string(path.substr(0, slash));
}

std::vector<std::string> build_lsf_argv(const ListRequest& req) {
    const bool path_target = is_path_target(req.target);
    const std::string base = path_target ? parent_of(req.target) : req.root;

    std::vector<std::string> argv{req.rclone_exe, "lsf", remote_spec(req.rclone_remote, base),
                                  req.mode == Mode::folder ? "--dirs-only" : "--files-only"};
    if (!path_target) {
        argv.insert(argv.end(), {"-R", "--max-depth", std::to_string(req.max_depth)});
        if (req.skip_hidden && !req.target.starts_with('.')) {
            // Skip dot-entries and, thanks to the /** rule, never descend into dot-dirs.
            argv.insert(argv.end(), {"--exclude", ".*/**", "--exclude", ".*"});
        }
    }
    if (req.rclone_config) {
        argv.push_back("--config");
        argv.push_back(util::path_to_utf8(*req.rclone_config));
    }
    return argv;
}

std::vector<std::string> parse_lsf_output(const ListRequest& req, std::string_view out) {
    const bool path_target = is_path_target(req.target);
    const std::string want = path_target ? remote_basename(req.target) : req.target;
    const std::string base = path_target ? parent_of(req.target) : req.root;
    const bool filter_hidden = req.skip_hidden && !path_target && !want.starts_with('.');

    std::vector<std::string> matches;
    std::size_t start = 0;
    while (start < out.size()) {
        auto end = out.find('\n', start);
        if (end == std::string_view::npos) end = out.size();
        std::string_view line = out.substr(start, end - start);
        start = end + 1;

        if (line.ends_with('\r')) line.remove_suffix(1);
        if (line.empty()) continue;
        const bool is_dir = line.ends_with('/');
        if (is_dir) line.remove_suffix(1);
        if (line.empty() || is_dir != (req.mode == Mode::folder)) continue;

        if (filter_hidden) {
            bool hidden = false;
            for (const auto& part : util::split(line, '/')) {
                if (part.starts_with('.')) {
                    hidden = true;
                    break;
                }
            }
            if (hidden) continue;
        }

        const std::string name = remote_basename(line);
        const bool hit = path_target ? name == want : glob_match(want, name);
        if (hit) matches.push_back(join_remote(base, line));
    }
    return matches;
}

} // namespace grab
