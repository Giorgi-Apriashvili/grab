#include "listing.hpp"

#include "match.hpp"
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

} // namespace grab
