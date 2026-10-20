#pragma once

// What kind of file a name is, by its extension, and filters by kind and size. The window's
// icons and filters use the same kinds and lists (src/gui/ui/app.js, ICON_KIND); keep them in
// sync.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace grab::file_type {

enum class Kind { video, audio, image, text, document, archive, program, disc, other };

// By the extension, ignoring case: "Movie.MKV" is video, "release.r00" an archive (split parts
// .r00, .z01, .001 too); no extension, or a name that only starts with a dot, is other.
[[nodiscard]] Kind kind_of(std::string_view name);
[[nodiscard]] std::string_view name(Kind k); // "video", "audio", ... "other"
[[nodiscard]] std::optional<Kind> parse(std::string_view text);
[[nodiscard]] std::string names(); // "video, audio, ... and other", for messages

// A size as typed: 700M, 700 MB, 1.5G, 20GiB, 2T, 500B (K/M/G/T are 1024-based); a bare number
// is MiB. nullopt for anything else.
[[nodiscard]] std::optional<std::uint64_t> parse_size(std::string_view text);

// Files of any of `kinds` (none: any kind) within the size bounds. A file of unknown size
// doesn't pass while a bound is set.
struct Filter {
    std::vector<Kind> kinds;
    std::optional<std::uint64_t> min_size;
    std::optional<std::uint64_t> max_size;

    [[nodiscard]] bool active() const { return !kinds.empty() || min_size || max_size; }
    [[nodiscard]] bool matches(std::string_view file_name, std::optional<std::uint64_t> size) const;
    // "video or audio, at least 1.0 GiB, at most 20.0 GiB"
    [[nodiscard]] std::string describe() const;
    bool operator==(const Filter&) const = default;
};

} // namespace grab::file_type
