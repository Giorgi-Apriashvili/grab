#pragma once

// Quick access: the folders the user pinned in the window, per server (PINS.md). Kept in
// state_dir's "pins", sealed for the current Windows user (DPAPI) as recent searches are, and
// changed one at a time under the settings lock from the file as it is then, so the window and
// `grab server remove` (and add) both change it. Nothing here asks a server anything: pinning,
// unpinning, renaming and reordering are local, and grab's own renames, moves and deletes carry
// the pins along by the paths they changed (follow, drop_below).
//
// In code and messages this is "pins", never "pinned": a pinned host key is another thing.

#include "server_files.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace grab::pins {

// One pinned folder.
struct Pin {
    // grab's form exactly as Browse had it: relative to the login folder, or absolute from "/".
    std::string path;
    // Set only when the user renamed the pin; empty: the list shows the folder's own name, which
    // so follows the folder's renames too.
    std::string name = {};
    bool operator==(const Pin&) const = default;
};
// Each server's pins (by grab.conf section) in the user's order; a server with none has no entry.
using Pins = std::map<std::string, std::vector<Pin>, std::less<>>;

// The file's format: "grab-pins-sealed 1\n", then {"version":1,"servers":{"Main":[{"path":
// "downloads/tv"},{"path":"/srv/media","name":"Movies"}]}} sealed by util::protect_for_user.
inline constexpr int version = 1;
inline constexpr std::size_t max_per_server = 100;
inline constexpr std::size_t max_path_bytes = 4096;
inline constexpr std::size_t max_name_chars = 255; // characters (code points), not bytes

// The configuration at `grab_conf`'s pins: state_dir's "pins", beside "recent".
[[nodiscard]] std::filesystem::path path_for(const std::filesystem::path& grab_conf);

// What a change says when the file is of a newer version.
inline constexpr std::string_view newer_text = "These pins were saved by a newer grab: update grab to change them.";

// Why `path` can't be pinned, in the window's words: empty (a server's top), over max_path_bytes,
// a control character (NUL included: grab's form has none) or not UTF-8; nullopt when it can.
[[nodiscard]] std::optional<std::string> bad_path(std::string_view path);
// Why `name` can't be a pin's own name: empty, over max_name_chars, a control character (C0, DEL
// or C1) or not UTF-8; nullopt when it can.
[[nodiscard]] std::optional<std::string> bad_name(std::string_view name);
// The name the list shows for `pin`: its own, else its folder's last name ("/" for the root).
[[nodiscard]] std::string shown_name(const Pin& pin);

// ---- the file ---------------------------------------------------------------------------------

struct Loaded {
    Pins pins;
    // Saved by a newer grab: shown as far as it can be read, and never written over.
    bool newer = false;
    // A file that couldn't be read: why, and where it was moved aside ("pins.bad-<date>-<time>").
    // The pins then start empty. `aside` is empty when the file was left where it is: it couldn't
    // be read at all (an I/O error, such as another program holding it), or it couldn't be moved.
    std::string damaged = {};
    std::filesystem::path aside = {};
};
// The pins saved; none when there is no file. Entries that can't be a pin are left out, and so
// are a server's pins past max_per_server and a path twice. A file whose content can't be read
// (damaged, another Windows user's, DPAPI failing for now) is moved aside under the settings lock
// (if it still can't be read then), so the next change doesn't replace it; one that can't be read
// at all stays where it is.
[[nodiscard]] Loaded load(const std::filesystem::path& file);

// What a change says when it can't be saved: "grab couldn't save Quick access: <why>".
inline constexpr std::string_view cant_save = "grab couldn't save Quick access: ";

// A change made to the file: the pins after it (as they are when it changed nothing, was refused
// or couldn't be saved), whether it was written, and why not in the window's words ("" when it
// changed nothing or was saved): the edit's refusal, newer_text, or cant_save and why.
struct Updated {
    Pins pins;
    bool changed = false;
    std::string error = {};
    // The file couldn't be read at all, or locked, or its damaged content couldn't be moved aside:
    // the change was refused and the file left alone, and `pins` (none) aren't the file's.
    bool unread = false;
    // A file whose content couldn't be read, moved aside before the change (as load says it): the
    // change then started from none.
    std::string damaged = {};
    std::filesystem::path aside = {};
};
// `edit` (true: it changed the pins; an error: refused, nothing written) applied under the settings
// lock to the file as it is then, which is written in one step (util::write_file_atomic), or
// removed once no pin is left. A file whose content can't be read is moved aside first and the
// change starts from none; one that can't be read at all (or moved aside) refuses the change, so a
// good file held by another program is never replaced; a newer one is never written: a change to
// it is refused with newer_text.
[[nodiscard]] Updated update(const std::filesystem::path& file,
                             const std::function<std::expected<bool, std::string>(Pins&)>& edit);

// ---- changes to the list (nothing written) ----------------------------------------------------
// Paths compare as server_files::canonical makes them with `home` (the login folder's real path,
// when known; empty: only the spellings given, "." and ".." taken away), so one folder is pinned
// at most once per server however it is spelled.

// The index of `server`'s pin of `path`, if any: the same spelling first, else the same folder.
[[nodiscard]] std::optional<std::size_t> find(const Pins& pins, std::string_view server, std::string_view path,
                                              std::string_view home = {});
// Pins `path` on `server` with its own `name` ("" none), last or at `at` (Undo's place, past the
// end: last). Refused: a path or name bad_path/bad_name refuses, the folder pinned already ("tv
// is already in Quick access"), or the server's list full.
[[nodiscard]] std::expected<void, std::string> add(Pins& pins, std::string_view server, std::string_view path,
                                                   std::string_view name = {}, std::optional<std::size_t> at = {},
                                                   std::string_view home = {});
// Unpins `path` on `server`. Whether it was pinned.
bool remove(Pins& pins, std::string_view server, std::string_view path, std::string_view home = {});
// The pin's own name; "" the folder's again. Whether it changed; refused for a bad name.
[[nodiscard]] std::expected<bool, std::string> rename(Pins& pins, std::string_view server, std::string_view path,
                                                      std::string_view name, std::string_view home = {});
// Moves the pin of `path` to index `to` of the server's list (clamped to it). Whether it moved.
bool move(Pins& pins, std::string_view server, std::string_view path, std::ptrdiff_t to, std::string_view home = {});

// A folder grab renamed or moved: from where to where.
struct Moved {
    std::string from;
    std::string to;
    bool operator==(const Moved&) const = default;
};
// A move's pairs from one of its runs: each item's `from` and its place there (Report::placed, in
// the items' order), those that didn't go (no place) or stayed where they were left out.
[[nodiscard]] std::vector<Moved> moved(std::span<const server_files::Item> items, std::span<const std::string> placed);
// A paste's pairs (the window's filesPaste): a copy's none; a move's from both its runs, the first
// (`items` and its places) and the second (`again`, the names the question gave a choice of their
// own, and its places, empty when it didn't run). The second run's places reach only the backend,
// so Keep both's "tv (2)" is followed from here.
[[nodiscard]] std::vector<Moved> moved_by_paste(bool copying, std::span<const server_files::Item> items,
                                                std::span<const std::string> placed,
                                                std::span<const server_files::Item> again,
                                                std::span<const std::string> placed_again);
// grab's own renames and moves on `server`: each pin at or below a `from` follows to its `to`, with
// `to`'s spelling and the rest of its own path below it ("tv2" is not below "tv"; links aren't
// followed: compared by spelling only). Its own name stays. Two pins that end at one folder keep
// the earlier. Whether a pin changed.
bool follow(Pins& pins, std::string_view server, std::string_view home, std::span<const Moved> moves);
// The pins of `server` at or below `paths` go. Whether one went.
bool drop_below(Pins& pins, std::string_view server, std::string_view home, std::span<const std::string> paths);
// A delete of `paths` in grab on `server` (PINS.md decision 6), as `report` says it went: one that
// ran to its end (neither stopped nor ended by an error) unpins what it deleted (drop_below); one
// that ended early keeps every pin, since which of them went isn't known. Whether one went.
bool deleted(Pins& pins, std::string_view server, std::string_view home, std::span<const std::string> paths,
             const server_files::Report& report);
// A server removed, or a new one taking the name of pins left by an older grab: all its pins go.
bool forget_server(Pins& pins, std::string_view server);

} // namespace grab::pins
