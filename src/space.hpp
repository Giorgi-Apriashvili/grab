#pragma once

// Free disk space before downloading: whether what a selection still needs fits on the drive it
// goes to. Partial downloads are sparse files that take space only as data arrives, so what a
// download still needs is what it has yet to fetch.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace grab::space {

// The drive `path` is on, as shown: "E:", "\\server\share", or a mounted folder. Found from the
// nearest folder that exists (a Save-to folder may not yet), so paths on one drive give the same
// text, case aside.
[[nodiscard]] std::string drive_of(const std::filesystem::path& path);

// Bytes free for this user on `path`'s drive, from its nearest existing folder; nullopt when no
// folder of it exists to ask. Debug builds: the environment variable GRAB_TEST_FREE_BYTES caps
// it, to try a full drive.
[[nodiscard]] std::optional<std::uint64_t> free_bytes(const std::filesystem::path& path);

// What fetching a file of `size` into `target` still writes: nothing when a copy of that size is
// there, the rest of a partial download of it, else all of it.
[[nodiscard]] std::uint64_t still_needed(const std::filesystem::path& target, std::uint64_t size);

// Whether `needed`, with what downloads already on the drive still need (`queued`), fits in
// `free`.
[[nodiscard]] constexpr bool fits(std::uint64_t needed, std::uint64_t queued, std::uint64_t free) {
    return needed <= free && queued <= free - needed;
}

// "not enough free space on E: (48.2 GiB needed, 31.0 GiB free)"
[[nodiscard]] std::string shortage(const std::string& drive, std::uint64_t needed, std::uint64_t free);

} // namespace grab::space
