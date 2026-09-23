#include "listing.hpp"

#include <charconv>
#include <cstdint>
#include <optional>

#include "match.hpp"
#include "rclone.hpp"
#include "remote.hpp"
#include "util.hpp"

namespace grab {

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
                                  "--format", "sp",
                                  req.mode == Mode::folder ? "--dirs-only" : "--files-only"};
    if (!path_target) {
        argv.insert(argv.end(), {"-R", "--max-depth", std::to_string(req.max_depth)});
        if (req.skip_hidden && !wants_hidden(make_query(req.target, req.exact))) {
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

std::vector<RemoteEntry> parse_lsf_output(const ListRequest& req, std::string_view out) {
    const bool path_target = is_path_target(req.target);
    const std::string want = path_target ? remote_basename(req.target) : req.target;
    const std::string base = path_target ? parent_of(req.target) : req.root;
    const Query query = make_query(req.target, req.exact);
    const bool filter_hidden = req.skip_hidden && !path_target && !wants_hidden(query);

    std::vector<RemoteEntry> found;
    std::size_t start = 0;
    while (start < out.size()) {
        auto end = out.find('\n', start);
        if (end == std::string_view::npos) end = out.size();
        std::string_view line = out.substr(start, end - start);
        start = end + 1;

        if (line.ends_with('\r')) line.remove_suffix(1);
        if (line.empty()) continue;

        // "<size>;<path>": the size comes first, so a ';' inside the path is harmless.
        std::optional<std::uint64_t> size;
        if (const auto semi = line.find(';'); semi != std::string_view::npos) {
            const auto digits = line.substr(0, semi);
            std::uint64_t n = 0;
            const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), n);
            if (ec == std::errc{} && ptr == digits.data() + digits.size() && !digits.empty()) size = n;
            if (digits == "-1" || size) line.remove_prefix(semi + 1);
        }
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
        const bool hit = path_target ? name == want : matches(query, name);
        if (hit) found.push_back(RemoteEntry{join_remote(base, line), size});
    }
    return found;
}

} // namespace grab
