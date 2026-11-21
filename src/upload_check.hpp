#pragma once

// The window's look before an upload (UPLOADS.md 5.10, the confirm dialog): what was picked (its
// files and bytes, links left out, names too long for a server), then the server's side: whether
// this account may write in the folder (upload::probe, which also gives the free space) and which
// of the names are taken there already, each compared with the file being uploaded
// (upload_existing.hpp). A folder's walk and the server's look are kept for its upload, which
// then needn't make them again.

#include "fetch.hpp"
#include "upload_existing.hpp"
#include "upload_folder.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace grab::ssh {
class Pool;
} // namespace grab::ssh

namespace grab::upload {

// The longest name a server takes, in bytes (Linux file systems).
inline constexpr std::size_t name_max_bytes = 255;

// One picked file or folder.
struct Picked {
    std::filesystem::path source;
    std::string name;          // its local name, UTF-8: the name it gets on the server
    bool folder = false;
    std::string target;        // where it goes on the server, grab's form (remote_child)
    std::uint64_t files = 0;   // a folder's files (a file: 1)
    std::uint64_t bytes = 0;
    std::int64_t mtime_ns = 0; // a file's
    std::string error;         // why it can't go (gone, unreadable, a file of the folder's name there)
    // A folder's walk, and once looked at the server's side (Tree::looked, merging, clashes).
    std::shared_ptr<Tree> tree;
    bool merging = false;      // a folder of its name is there: merged into
    std::optional<Clash> clash; // a file whose name is taken there
};

// The names already taken on the server, by how they compare (upload_existing.hpp's Kind), and
// those a folder has (never replaced).
struct Taken {
    std::size_t count = 0;
    std::size_t same = 0;
    std::size_t newer = 0;   // newer here
    std::size_t older = 0;   // older here
    std::size_t differs = 0; // another size, the times saying nothing
    std::size_t folders = 0;
};

struct Check {
    std::string server;
    std::string dir; // the server folder, grab's form ("" the login folder)
    std::vector<Picked> items;
    std::uint64_t files = 0;
    std::uint64_t bytes = 0;
    std::size_t links = 0;      // links and junctions below the folders, left out
    std::size_t long_names = 0; // names longer than name_max_bytes: those files would fail
    std::size_t unreadable = 0; // folders and names below the folders that couldn't be read, left out
    // The server: whether this account may write in `dir` (nullopt: not looked at, or the server
    // couldn't be reached, `why` saying so, naming the server), why not, and the bytes it may still
    // write there.
    std::optional<bool> writable;
    std::string why;
    std::optional<std::uint64_t> free;
    Taken taken;
};

// The picked paths into check.items, in their order: a file looked at, a folder walked (a link
// picked is followed, links below it never). `progress` after each one. A stop ends early.
void look_local(Check& check, const std::vector<std::filesystem::path>& picked, const std::function<void()>& progress,
                std::stop_token stop = {});

// The server's side through `pool`, each step on seats from `connections` (one for the probe and
// the files' names, a listing's for a folder's look), so transfers using the server's whole budget
// make room for it: the probe, then, where this account may write, the names taken there.
void look_server(Check& check, const std::shared_ptr<ssh::Pool>& pool, fetch::Connections& connections,
                 std::stop_token stop = {});

// Why nothing of what was picked can go: nothing was picked, or every item has an error (the
// first one's). "" when some of it can.
[[nodiscard]] std::string nothing_goes(const Check& check);

// Whether the server said it has less room than the bytes to send.
[[nodiscard]] bool short_of_space(const Check& check);

} // namespace grab::upload
