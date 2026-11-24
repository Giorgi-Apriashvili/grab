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
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
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
    Taken taken; // the names taken there, by how they compare (upload_existing.hpp)
};

// The files whose names are taken on the server as the dialog lists them (UPLOADS.md decision 3):
// in the order of the picked items, a folder's files in the folder's order. The user's choices
// for single files name them by their place in this list.
struct Listed {
    std::size_t item = 0;            // its picked item: Check::items' index
    std::optional<std::size_t> file; // a folder's file: Tree::files' index; nullopt: the picked file
    std::string name;                // as shown: a file's name, a folder's file as "folder/path/in/it"
    Clash clash;
};
// The first `at_most` of them (items with an error have none).
[[nodiscard]] std::vector<Listed> taken_list(const Check& check, std::size_t at_most);

// The user's answer about the files already there: one choice for all of them, and single files'
// own choices by their place in taken_list. `unlisted`: the choice of the items the dialog listed
// nothing of (nothing of theirs was there), nullopt: `all`. Under the setting Ask that is Ask, so a
// file that turns up there later is asked about (UPLOADS.md 5.5), never decided by an answer about
// other items' files (review, chunk 5).
struct Answer {
    Existing all = Existing::skip;
    std::map<std::size_t, Existing> own;
    std::optional<Existing> unlisted;
};
// The answer into the picked items, before they are queued: a folder's files with a choice of
// their own get it in their Tree::choices (Ask is no choice of a file's own: such entries are
// left out). Returns each item's choice, by Check::items' index: a picked file's own, else `all`
// (`unlisted` for an item with nothing there), which a folder's other files follow, and also a
// file that turns up there later.
[[nodiscard]] std::vector<Existing> apply_answer(Check& check, const Answer& answer);

// The picked paths into check.items, in their order: a file looked at, a folder walked (a link
// picked is followed, links below it never). `progress` after each one. A stop ends early.
void look_local(Check& check, const std::vector<std::filesystem::path>& picked, const std::function<void()>& progress,
                std::stop_token stop = {});

// The server's side through `pool`, each step on seats from `connections` (one for the probe and
// the files' names, a listing's for a folder's look), so transfers using the server's whole budget
// make room for it: the probe, then, where this account may write, the names taken there. Without
// `write` (grab upload -n) nothing is written on the server: the probe only looks (upload::probe),
// and `writable` says only that nothing seen refuses it.
void look_server(Check& check, const std::shared_ptr<ssh::Pool>& pool, fetch::Connections& connections,
                 std::stop_token stop = {}, bool write = true);

// Why nothing of what was picked can go: nothing was picked, or every item has an error (the
// first one's). "" when some of it can.
[[nodiscard]] std::string nothing_goes(const Check& check);

// Whether the server said it has less room than the bytes to send.
[[nodiscard]] bool short_of_space(const Check& check);

// A drop's refusals (UPLOADS.md decision 7): nothing grab could take came with it; files that
// aren't on this PC as files (from inside a .zip, a mail's attachments); a link from a browser. As
// the page says them (app.js EXTRACT_FIRST, DROP_FILES).
inline constexpr std::string_view drop_nothing = "Drop files or folders from Explorer: nothing grab can upload came with this drop.";
inline constexpr std::string_view drop_extract_first =
    "Extract files from a .zip first: a file inside a .zip (or a mail’s attachment) isn’t on this PC as a file "
    "until it is extracted or saved. Then drop it from there.";
inline constexpr std::string_view drop_link = "Drop files or folders from Explorer: grab uploads files, not links.";

// Whether `path` is WebView2's own copy of a dropped file that has no place on disk (a file from
// inside a .zip, a mail's attachment): WebView2 writes those straight into its temp folder `temp`
// (grab-gui's, which it inherits) and hands their paths over like the user's own. They aren't
// uploaded: WebView2 deletes them again, so an upload from one would fail later.
[[nodiscard]] bool dropped_copy(const std::filesystem::path& path, const std::filesystem::path& temp);

// Why a drop of `paths` isn't taken, or "" when its check may start: none came with it
// (drop_nothing); some are WebView2's copies in `temp`: of a browser's link shortcut when a URL
// came with the drop (`url`: drop_link), else of files from inside a .zip (drop_extract_first). An
// Internet shortcut (.url) of the user's own, dragged from Explorer, comes with its URL too and is
// taken like any file.
[[nodiscard]] std::string drop_refusal(const std::vector<std::filesystem::path>& paths, const std::filesystem::path& temp, bool url);

} // namespace grab::upload
