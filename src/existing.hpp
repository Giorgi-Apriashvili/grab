#pragma once

// A download that would replace a file already there which differs from the server's (another
// size, or older): grab asks first. Replace it, keep both (the new one as "name (2).ext", as
// Explorer names a second copy), or skip it and keep yours. A copy that matches the server's
// (fetch::up_to_date) is no question: it is simply not fetched again.

#include "fetch.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace grab::existing {

enum class Choice { ask, replace, keep_both, skip };
[[nodiscard]] std::string_view name(Choice c);                    // "ask", "replace", "both", "skip"
[[nodiscard]] std::optional<Choice> parse(std::string_view text); // those names

// A file at `local` that a download of the server's file (`size`; modified `modtime_ns`
// nanoseconds after 1970, when known) would replace although it differs: one that isn't
// fetch::up_to_date.
[[nodiscard]] bool differs(const std::filesystem::path& local, std::uint64_t size,
                           std::optional<std::int64_t> modtime_ns);
// The same as rclone copy decides it (--rclone): another size, or a time more than 1 s off.
[[nodiscard]] bool rclone_replaces(const std::filesystem::path& local, std::uint64_t size,
                                   std::optional<std::int64_t> modtime_ns);

// "name (n).ext": the extension is what follows the last '.', unless the name starts with it.
[[nodiscard]] std::string numbered(std::string_view name, int n);
// Where the second copy goes: "name (2).ext" next to `target`, or (3) and so on, the first with
// nothing there that isn't reserved.
[[nodiscard]] std::optional<std::filesystem::path> second_copy(const std::filesystem::path& target,
                                                               const fetch::Reserved& reserved = {});

// The file already there, for the question.
struct Local {
    std::uint64_t size = 0;
    std::optional<std::int64_t> modified; // Unix seconds
};
[[nodiscard]] std::optional<Local> local_file(const std::filesystem::path& p);

} // namespace grab::existing
