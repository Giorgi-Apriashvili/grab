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
    argv.insert(argv.end(), {"--config", std::string(config_in_memory)});
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

HitFilter::HitFilter(Mode mode, std::string_view target, bool exact, bool skip_hidden)
    : mode_(mode), path_target_(is_path_target(target)), want_(path_target_ ? remote_basename(target) : std::string()),
      query_(make_query(target, exact)), filter_hidden_(skip_hidden && !path_target_ && !wants_hidden(query_)) {}

bool HitFilter::keeps(std::string_view path, bool dir) const {
    if (path.empty() || dir != (mode_ == Mode::folder)) return false;
    if (filter_hidden_) {
        for (const auto& part : util::split(path, '/')) {
            if (part.starts_with('.')) return false;
        }
    }
    return path_target_ ? remote_basename(path) == want_ : matches(query_, path);
}

std::vector<RemoteEntry> parse_lsf_output(const ListRequest& req, std::string_view out) {
    const std::string base = is_path_target(req.target) ? parent_of(req.target) : req.root;
    const HitFilter filter(req.mode, req.target, req.exact, req.skip_hidden);

    std::vector<RemoteEntry> found;
    for (const auto& record : parse_csv(out)) {
        const auto fields = parse_lsf_record(record);
        if (!fields) continue; // never a path on its own: a split name would be one (G59)
        std::string_view line = fields->path;
        const bool is_dir = line.ends_with('/');
        if (is_dir) line.remove_suffix(1);
        if (filter.keeps(line, is_dir)) found.push_back(RemoteEntry{join_remote(base, line), fields->size, fields->modified});
    }
    return found;
}

} // namespace grab
