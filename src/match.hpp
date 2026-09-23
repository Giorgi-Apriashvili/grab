#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace grab {

// How a TARGET name is matched against remote file and folder names.
struct Query {
    enum class Kind {
        words, // default: every whitespace-separated word appears, case-insensitive, any order
        glob,  // TARGET has * ? or [: fnmatch-style pattern, case-insensitive
        exact  // --exact: whole name or pattern, case-sensitive (the pre-0.3 behaviour)
    };
    Kind kind = Kind::words;
    std::string text;               // as typed
    std::vector<std::string> terms; // words only: lowercased, non-empty
};

[[nodiscard]] bool is_glob(std::string_view s);

// Builds the query for a name TARGET (path targets containing '/' are handled elsewhere).
[[nodiscard]] Query make_query(std::string_view target, bool exact);

// Does the last path component `name` match?
[[nodiscard]] bool matches(const Query& q, std::string_view name);

// Ranking tier for a matching name: 0 = the name equals what was typed (ignoring case),
// 1 = it starts with the first word, 2 = anything else. Lower sorts first.
[[nodiscard]] int match_tier(const Query& q, std::string_view name);

// True when the query itself targets a dot-name, so hidden entries must not be pruned.
[[nodiscard]] bool wants_hidden(const Query& q);

// fnmatch-style matching as `find -name` / `-iname` does it: * ? [set] [!set]; no escapes.
[[nodiscard]] bool glob_match(std::string_view pattern, std::string_view name,
                              bool casefold = false);

// Escape * ? [ ] and \ so a literal word can be embedded in a find -iname pattern.
[[nodiscard]] std::string escape_glob(std::string_view literal);

} // namespace grab
