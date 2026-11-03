#pragma once

#include "cli.hpp"
#include "match.hpp"

#include <string>
#include <string_view>

namespace grab {

// ("", "a/b") -> "a/b"   ("/", "a") -> "/a"   ("x/", "a") -> "x/a"
[[nodiscard]] std::string join_remote(std::string_view root, std::string_view rel);

// "learning/x" -> "learning"   "/x" -> "/"   "a/b/" -> "a"   "x" -> ""
[[nodiscard]] std::string parent_of(std::string_view path);

// Which entries of a walk a lookup keeps: of the kind asked for, with no dot-name below where the
// walk started when those are skipped (unless the query wants one), and named as the query asks
// (see match.hpp; a path target: exactly its name). Searches over grab's walk use it
// (RCLONE-REPLACEMENT.md, phase 4).
class HitFilter {
public:
    HitFilter(Mode mode, std::string_view target, bool exact, bool skip_hidden);
    // `path`: the entry's path below the walk's start, without a trailing '/' (only its name counts
    // when dot-names aren't skipped here).
    [[nodiscard]] bool keeps(std::string_view path, bool dir) const;

private:
    Mode mode_;
    bool path_target_;
    std::string want_; // a path target's name
    Query query_;
    bool filter_hidden_;
};

} // namespace grab
