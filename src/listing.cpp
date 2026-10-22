#include "listing.hpp"

#include <charconv>
#include <cstdint>
#include <optional>
#include <system_error>

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
                                  "--format", "tsp", "--time-format", "unix", "--csv"};
    if (!req.all_entries) argv.push_back(req.mode == Mode::folder ? "--dirs-only" : "--files-only");
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
    argv.insert(argv.end(), req.flags.begin(), req.flags.end());
    return argv;
}

std::vector<std::vector<std::string>> parse_csv(std::string_view text) {
    std::vector<std::vector<std::string>> records;
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;  // inside a quoted field
    bool started = false; // the record has a field begun
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (c != '"') {
                field += c;
            } else if (i + 1 < text.size() && text[i + 1] == '"') {
                field += '"';
                ++i;
            } else {
                quoted = false;
            }
        } else if (c == '"' && field.empty()) {
            quoted = true;
            started = true;
        } else if (c == ',') {
            fields.push_back(std::move(field));
            field.clear();
            started = true;
        } else if (c == '\n' || (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n')) {
            if (c == '\r') ++i;
            if (started || !field.empty()) {
                fields.push_back(std::move(field));
                records.push_back(std::move(fields));
            }
            fields.clear();
            field.clear();
            started = false;
        } else {
            field += c;
            started = true;
        }
    }
    if (started || !field.empty()) {
        fields.push_back(std::move(field));
        records.push_back(std::move(fields));
    }
    return records;
}

std::optional<LsfLine> parse_lsf_record(const std::vector<std::string>& fields) {
    if (fields.size() != 3) return std::nullopt;
    const auto time = util::parse_unix_time(fields[0]);
    const std::string_view digits = fields[1];
    std::optional<std::uint64_t> size;
    std::uint64_t n = 0;
    const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), n);
    if (ec == std::errc{} && ptr == digits.data() + digits.size() && !digits.empty()) size = n;
    // A time that doesn't parse only leaves the date unknown: an rclone without the "unix" format
    // prints the format's name there, and its paths and sizes are still right.
    if (!size && digits != "-1") return std::nullopt;
    return LsfLine{.path = fields[2], .size = size, .modified = util::known_time(time)};
}

std::vector<RemoteEntry> parse_lsf_output(const ListRequest& req, std::string_view out) {
    const bool path_target = is_path_target(req.target);
    const std::string want = path_target ? remote_basename(req.target) : req.target;
    const std::string base = path_target ? parent_of(req.target) : req.root;
    const Query query = make_query(req.target, req.exact);
    const bool filter_hidden = req.skip_hidden && !path_target && !wants_hidden(query);

    std::vector<RemoteEntry> found;
    for (const auto& record : parse_csv(out)) {
        const auto fields = parse_lsf_record(record);
        if (!fields) continue; // never a path on its own: a split name would be one (G59)
        std::string_view line = fields->path;
        const auto size = fields->size;
        const auto modified = fields->modified;
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
        if (hit) found.push_back(RemoteEntry{join_remote(base, line), size, modified});
    }
    return found;
}

} // namespace grab
