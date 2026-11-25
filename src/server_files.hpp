#pragma once

// Changes the user makes to a server's own files from Browse (SERVER-OPS.md): rename, a new
// folder, move and copy within one server, delete, and what Properties shows. Each runs on one
// connection of the server's pool, with a seat of its budget as a listing takes one
// (fetch::Listing), so running transfers make room; paths are in grab's form throughout.
//
// This is the first code that changes files grab didn't make, so each step is the user's own
// explicit act (the window confirms deletes) and nothing more: a rename never replaces anything,
// a name taken on a move or copy follows the uploads' existing-file choice (upload_existing.hpp),
// a folder is never replaced nor merged by a move, and a walk never goes through a link (a link is
// removed, moved or copied as itself, never what it points to). Copies are the server's own
// (copy-data: no byte comes through grab), each file into a temp name beside its place
// (upload::temp_name) that takes the name only once complete; a stop or a failure removes the
// temps and the folders the copy made that are still empty. Refusals are the uploads' plain words
// (upload::refusal).

#include "config.hpp"
#include "fetch.hpp"
#include "sftp.hpp"
#include "ssh_hash.hpp"
#include "ssh_pool.hpp"
#include "upload_existing.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace grab::server_files {

// What a copy has made on the server that isn't its result yet, as it comes (`made`) and goes
// (Where::on_undo): a temp (upload::temp_name, so its name proves it is grab's), from just before it
// is asked for until it is placed or removed; a folder, from its MKDIR's answer until the copy ends
// (then it is the copy's, or removed again as empty). What is still made when a copy is over is what
// it couldn't undo (its connection lost twice, grab quitting).
struct Undo {
    enum class Kind { temp, folder };
    Kind kind = Kind::temp;
    std::string path; // grab's form
    bool made = true;
};

// The server an operation runs on.
struct Where {
    std::string server;                     // grab.conf section: the budget, and its name in messages
    std::shared_ptr<ssh::Pool> pool;
    fetch::Connections* connections = nullptr; // the server's budget, shared with the transfers
    // The connection's timeout (ssh::Target::timeout): each copy-data piece is sized to be done well
    // within it (sftp::copy_range).
    std::chrono::milliseconds timeout{30'000};
    // Each change before it is sent ("sftp rename <from> <to>"; -v), from the operation's thread.
    std::function<void(const std::vector<std::string>&)> on_command;
    // The login folder's real path (Account::home, grab's form) when known; empty: a change asks the
    // server (REALPATH of ".") before it sends anything. The login folder, "/" and a folder holding
    // the login folder are refused however they are spelled (canonical() with it).
    std::string home;
    // A copy's temps and folders as they come and go (Undo), from the copy's thread: the window keeps
    // those still made in uploads.json's leftovers, so a copy cut off midway is undone at the next
    // start by the uploads' cleaner (a temp only by its name, a folder only when empty).
    std::function<void(const Undo&)> on_undo;
};

// Why a copy can't run on `server`: it doesn't copy files itself (no copy-data: OpenSSH before 9.0).
// A move (RENAME) works there.
[[nodiscard]] std::string no_copy_data(std::string_view server);

// The longest closing waits for a stopped copy's copy-data piece, however long its connection's
// timeout (io_timeout goes up to 24 h): a hidden grab-gui never lingers longer for one.
inline constexpr std::chrono::milliseconds close_piece_most{120'000};
// How long closing the window waits for the changes it stopped: for the copies among them, each on
// a connection of `copies` (Where::timeout), the copy-data piece each may be in, sized to end well
// within its connection's timeout (that timeout, at most close_piece_most; 0, no limit, counts as
// close_piece_most), then `cleanup` for removing its temps and the empty folders it made. Other
// changes (no `copies`: a rename, move or delete stops between requests answered at once) and
// `restarting` (an update closing grab-gui, which is ended soon after): `cleanup` only. What a cut
// copy leaves is in uploads.json for the next start's cleaner.
[[nodiscard]] std::chrono::milliseconds close_wait(std::span<const std::chrono::milliseconds> copies, bool restarting,
                                                   std::chrono::milliseconds cleanup);

// Why `name` (grab's form, as Browse shows names) can't be a file's or folder's new name: empty, "."
// or "..", or holding a '/' or NUL, as the server would get it ('／' is a '/' there, '．' a ".").
[[nodiscard]] std::optional<std::string> bad_name(std::string_view name);

// ---- rename, a new folder --------------------------------------------------------------------------

// A rename's or new folder's answer: done (the new path), or the name taken, nothing changed (the
// window says so and keeps the name box open).
struct Named {
    bool taken = false;
    std::string path; // done: the new path, grab's form
    std::string why;  // taken: "<name> is already taken in <folder>"
};

// `path` renamed to `new_name` (grab's form, as Browse shows names: "caf‛E9.txt" keeps its byte
// 0xE9) in its folder by RENAME, which never replaces anything: a name taken is Named::taken. The same name: done, nothing sent. Anything else refused is the error, in plain
// words ("<Server> is read-only for this account", "...doesn't let this account write in <dir>",
// "<path> is no longer there"); so are the login folder, "/" and a folder holding the login folder,
// however spelled, before anything is sent.
[[nodiscard]] std::expected<Named, std::string> rename(const Where& w, std::string_view path, std::string_view new_name,
                                                       std::stop_token stop = {});
// A new folder `name` (grab's form) in `dir` (MKDIR); a name taken is Named::taken.
[[nodiscard]] std::expected<Named, std::string> mkdir(const Where& w, std::string_view dir, std::string_view name,
                                                      std::stop_token stop = {});

// ---- move, copy, delete ------------------------------------------------------------------------------

// A file or folder to move or copy: from where, to which path (its new name included), and its own
// existing-file choice (the window's answer for it), else the operation's.
struct Item {
    std::string from;
    std::string to;
    std::optional<upload::Existing> choice;
};
// Each of `paths` into the folder `dir` under its own name.
[[nodiscard]] std::vector<Item> into(std::span<const std::string> paths, std::string_view dir);

// Something an operation left as it was, or couldn't do: skipped (the existing-file choice, a link
// the server makes none of, a folder of that name there), failed (refused, no longer there), or
// asking (a name taken under Ask: nothing done for it; the window asks and sends it again with the
// answer as its Item::choice).
struct Entry {
    enum class State { skipped, failed, asking };
    State state = State::failed;
    std::string from; // grab's form
    std::string to;   // where it was to go (move, copy)
    std::string why;
    std::optional<upload::Clash> clash; // the name taken: what is there, against the one moved or copied
};

// How far a copy or delete is: bytes and files (links included) done of all, as each file ends
// (and each piece of a big file), from the operation's thread.
struct Progress {
    std::uint64_t bytes = 0;
    std::uint64_t total_bytes = 0;
    std::size_t files = 0;
    std::size_t total_files = 0;
};
using OnProgress = std::function<void(const Progress&)>;

// What an operation did.
struct Report {
    std::size_t files = 0;   // files and links moved, copied or deleted
    std::size_t folders = 0; // folders moved, made (copy) or deleted
    std::uint64_t bytes = 0; // copied, or deleted
    // Move, copy: each item's new path, in the items' order (Keep both: the numbered one); empty
    // for one that didn't go.
    std::vector<std::string> placed;
    std::vector<Entry> entries;
    bool stopped = false; // the stop token ended it
    // What ended it early, plainly: a refusal that holds for everything left ("<Server> is
    // read-only for this account"), a lost connection; a delete's also says what was deleted and
    // what is left. Empty when it ran to its end.
    std::string error;
    std::size_t left = 0; // delete: files and folders still there below what was asked
};

// Why nothing of `items` may be sent, before anything is looked at on the server: one going into
// itself or into one of its own subfolders, a name that can't be, the login folder or "/" itself,
// or (a move) a folder holding the login folder; each compared as canonical() makes it with `home`
// (the login folder's real path; empty: only the spellings given, "." and ".." taken away). (A copy
// of a folder onto itself, where its own folder is pasted into, is refused once the copy sees it is
// a folder; a file pasted where it is gets the existing-file choice.) Move, copy and look check it
// again with the login folder known (Where::home, else asked).
[[nodiscard]] std::optional<std::string> refused_before(std::span<const Item> items, bool copying, std::string_view home = {});

// Moves each item by RENAME (instant, nothing copied). A name taken follows the item's choice
// (upload::decide on the one moved against the one there): Replace by posix-rename where offered,
// else REMOVE then RENAME, and only then; Keep both under the first numbered name free; Skip and
// Ask leave both (Ask: an asking entry). A folder there is never replaced nor merged into: the
// item is skipped with the reason. Into another file system of the server the server refuses
// (OpenSSH: "Failure"): a plain message naming it, no copy and delete instead. refused_before is
// checked first.
[[nodiscard]] Report move(const Where& w, std::span<const Item> items, upload::Existing existing, std::stop_token stop = {});

// Copies each item on the server (copy-data): a file into a temp name in its target folder,
// pieces sized to Where::timeout for big ones, its modification time and mode set, then put in
// place by the choice as uploads place theirs (upload::place); a folder walked (never through a
// link, hidden names too), its folders made top-down and merged into where one is there, its files
// copied as above, its links made again as links (READLINK + SYMLINK) or skipped with the reason
// where the server won't, and the folders' modes and times set last; a socket or device skipped.
// A stop ends it between files and pieces; then and on a failure that ends it, its temps are
// removed and the folders it made that are still empty. Ask: each name taken is an asking entry,
// the rest copied. Its temps and the folders it made are told to Where::on_undo as they come and go.
// A server without copy-data: no_copy_data(), nothing sent.
[[nodiscard]] Report copy(const Where& w, std::span<const Item> items, upload::Existing existing,
                          const OnProgress& progress = {}, std::stop_token stop = {});

// What a move or copy of `items` would meet, nothing changed: the files, folders and bytes (a copy
// walks its folders, a folder whose name a file has too: Keep both copies it) and the names taken
// (entries asking, with their clashes; the copy's files in folders merged into too), or what
// refused_before says, or (a copy) no_copy_data() before anything is walked. For the window's
// question before a paste.
struct Look {
    std::size_t files = 0;
    std::size_t folders = 0;
    std::uint64_t bytes = 0;
    upload::Taken taken;
    // Of `taken`: the folders moved or copied onto a file of their name, by how they compare. Only
    // Keep both puts them anywhere (beside it); every other choice skips them.
    upload::Taken folders_onto_files;
    std::vector<Entry> clashes; // the first max_clashes of them
};
inline constexpr std::size_t max_clashes = 200;
[[nodiscard]] std::expected<Look, std::string> look(const Where& w, std::span<const Item> items, bool copying,
                                                    std::stop_token stop = {});

// Deletes each path for good: a file or link by REMOVE (a link itself, never its target); a folder
// walked with LSTAT (never through a link), hidden names too, its files and links removed
// pipelined, then its folders RMDIR deepest first. A refusal ends it: Report::error says what was
// deleted, what is left and why. The login folder, "/" and a folder holding the login folder,
// however spelled, are refused before anything is sent.
[[nodiscard]] Report remove(const Where& w, std::span<const std::string> paths, const OnProgress& progress = {},
                            std::stop_token stop = {});

// ---- properties ------------------------------------------------------------------------------------

struct Properties {
    enum class Kind { file, folder, link, other };
    Kind kind = Kind::other;
    sftp::Attrs attrs;                   // LSTAT's: a link's own
    std::optional<sftp::Attrs> target;   // a link: what it leads to (STAT); none when it leads nowhere
    std::string link;                    // a link: where it points, as stored (grab's form)
    std::string owner;                   // the owner's name, else its number; empty when the server sent none
    std::string group;
    bool names = false;                  // the server named the owner and group (users-groups-by-id)
    std::optional<std::uint64_t> free;   // bytes this account may still write there (statvfs)
    bool read_only = false;              // statvfs says the file system is read-only for this account
};
[[nodiscard]] std::expected<Properties, std::string> properties(const Where& w, std::string_view path,
                                                                std::stop_token stop = {});

// The files, folders, links and bytes at and below `paths` (folders walked, never through a link):
// Properties' folder size, and the delete question's.
struct Size {
    std::size_t files = 0;
    std::size_t folders = 0;
    std::size_t links = 0;
    std::uint64_t bytes = 0;
    std::vector<std::string> unreadable; // folders that couldn't be listed ("<path>: <why>")
};
[[nodiscard]] std::expected<Size, std::string> folder_size(const Where& w, std::span<const std::string> paths,
                                                           const OnProgress& progress = {}, std::stop_token stop = {});

// The server's MD5 or SHA-256 of the file `path` of `size` bytes (md5sum or sha256sum, ssh_hash.hpp),
// `server` as grab.conf has it (its shell's keys), or why there is none.
[[nodiscard]] std::expected<std::string, std::string> hash(const Where& w, const RcloneRemote& server, std::string_view path,
                                                           std::uint64_t size, ssh::Sum sum, std::stop_token stop = {});

// ---- what the window does around them ----------------------------------------------------------------

// The account on a server as the window keeps it while Browse shows that server: the login folder's
// real path (REALPATH of "."), which canonical(), busy_refusal() and invalidation() take as `home`;
// and what statvfs says of the login folder's file system: read-only for this account (Box.Guest:
// the window greys every change, saying why) and the bytes it may still write. A server without
// statvfs is taken as writable: grab tries, and says the server's refusal. `copy_data`: the server
// copies files itself (copy-data); without it the window greys Paste of a copy there, saying why
// (no_copy_data), and a move still works.
struct Account {
    std::string home; // grab's form
    bool read_only = false;
    std::optional<std::uint64_t> free;
    bool copy_data = false;
};
[[nodiscard]] std::expected<Account, std::string> account(const Where& w, std::stop_token stop = {});

// Each of `paths` (grab's form) with its folder's real path (REALPATH of each folder, asked at
// once) and its own name kept, so a link is still itself: the spelling the window compares and
// marks beside the given one when a path goes through a link to a folder. One whose folder can't
// be resolved (gone, not readable, the connection failing) stays as given; so do the login folder
// and "/".
[[nodiscard]] std::vector<std::string> resolved(const Where& w, std::span<const std::string> paths,
                                                std::stop_token stop = {});
// Each of `paths` (grab's form) REALPATHed whole, a link at its end followed too: the place a
// transfer or Play through a link really reads or writes (a folder download of a link to a folder,
// Play of a link to a file). One that can't be resolved (not there yet, as an upload's target; not
// readable; the connection failing) stays as given; so do the login folder and "/".
[[nodiscard]] std::vector<std::string> followed(const Where& w, std::span<const std::string> paths,
                                                std::stop_token stop = {});

// A path a transfer or Play is using on the server: no change may touch it, nor a folder holding it.
struct Busy {
    enum class Kind { download, upload, play };
    Kind kind = Kind::download;
    std::string path; // grab's form
};
// `path` (grab's form) as one spelling for the same place: below `home` (the login folder's real
// path, ssh::Pool's REALPATH of ".") when it doesn't start with '/', its "." and ".." taken away by
// their names ("a/../x" is "x"). Links aren't followed: two paths through different links stay
// two.
[[nodiscard]] std::string canonical(std::string_view path, std::string_view home);
// Why an operation that changes `paths` (grab's form: what it renames, moves, deletes or writes, and
// what a copy reads, so it never copies an upload's half-written file) may not run while `busy` is
// going: one of them is a busy path, holds one, or lies inside one, each compared as canonical()
// makes them with `home` (empty when not known: then only the spellings given compare).
[[nodiscard]] std::optional<std::string> busy_refusal(std::span<const Busy> busy, std::span<const std::string> paths,
                                                      std::string_view server, std::string_view home = {});

// What changed on the server, for the window to show it: the paths gone or replaced (`from`: a
// rename's, a move's or a delete's) and those new or replaced (`to`: a rename's, a move's or a
// copy's new paths, a new folder).
struct Changed {
    std::vector<std::string> from;
    std::vector<std::string> to;
};
// What the window marks after it: the folders whose listings changed (Browse lists them live
// again), with `below` for a folder whose own contents came or went too, the paths whose previews
// to forget (each with all below it: preview::Previews::forget), and the server whose file list to
// refresh (search results otherwise keep the old names for up to 15 minutes). With `home` (the
// login folder's real path) each path is marked and forgotten in each of its spellings: as given,
// canonical(), and from the login folder for one below it.
struct Mark {
    std::string folder;
    bool below = false;
    bool operator==(const Mark&) const = default;
};
struct Invalidation {
    std::vector<Mark> folders;
    std::vector<std::string> forget;
    std::string list;
};
[[nodiscard]] Invalidation invalidation(std::string_view server, const Changed& changed, std::string_view home = {});

} // namespace grab::server_files
