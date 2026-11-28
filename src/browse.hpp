#pragma once

// Browsing a server's folders in grab's window. A folder opens from the server's saved file list
// (file_list.hpp) when the list holds all of its contents, which is instant; deeper folders,
// beyond the list's max_depth, are listed live with one SFTP listing on a pooled connection.

#include "engine.hpp"
#include "file_list.hpp"
#include "process.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace grab::browse {

struct Item {
    std::string name;
    std::string path; // in grab's form (rclone_enc::grab_path)
    bool dir = false;
    std::optional<std::uint64_t> size;  // files only: a folder's total isn't known
    std::optional<std::size_t> items;   // folders whose contents are known: how many entries
    std::optional<std::int64_t> modified{}; // Unix seconds; a folder's own
    bool operator==(const Item&) const = default;
};

// Folders first, then files, each in natural order ("Episode 2" before "Episode 10"), ignoring
// case.
void sort_items(std::vector<Item>& items);
[[nodiscard]] bool natural_less(std::string_view a, std::string_view b);

// Whether `path` lies below `folder` ("" being the login home, which holds every relative path).
[[nodiscard]] bool is_below(std::string_view path, std::string_view folder);

// A path typed in the window's address bar, as browse uses it: spaces around it and repeated or
// trailing slashes go; "~" is the login home ("") and "~/x" a path in it.
[[nodiscard]] std::string address_path(std::string_view text);

// How a Browse request names its folder: a click (a row, a crumb, Back), an address typed in the
// address bar, or a pin opened from Quick access (PINS.md 3.3).
enum class Asked { browsed, typed, pin };
// Where a Browse request opens: the folder (a typed address through address_path; a pin's path
// exactly as saved, so a folder named "sp " or "~", or a path with "//", stays itself; anything else
// as given), and whether it is the server's top: asked for, or a typed or pinned path equal to its
// one root, whose folder it then is. Several roots make a top of their own (`roots_top`, the
// folder "").
struct Opening {
    std::string folder;
    bool top = false;
    bool roots_top = false;
    bool operator==(const Opening&) const = default;
};
[[nodiscard]] Opening opening(const std::vector<std::string>& roots, std::string_view path, bool top, Asked asked);

// A server's folders as its saved file list knows them: every folder under its search roots
// down to max_depth. A folder at max_depth is in the list, but its contents are not.
class Index {
public:
    Index(std::shared_ptr<const file_list::List> list, std::vector<std::string> roots, int max_depth);

    // The contents of `path`, sorted; nullopt when the list doesn't know them (outside the
    // roots, or below max_depth).
    [[nodiscard]] std::optional<std::vector<Item>> folder(std::string_view path) const;
    // The server's top: its one root's contents, or with several roots, the roots as folders.
    [[nodiscard]] std::vector<Item> top() const;
    // The list's file or folder at `path`, when the list holds its folder's contents.
    [[nodiscard]] std::optional<Item> entry(std::string_view path) const;
    // The files the list holds below `path`, at any depth; `complete` when it holds them all (no
    // folder below is deeper than the list). None, and not complete, when the list doesn't hold
    // the folder's own contents. The entries live as long as the index.
    struct Below {
        std::vector<const file_list::Entry*> files;
        bool complete = false;
    };
    [[nodiscard]] Below below(std::string_view path) const;
    [[nodiscard]] const std::vector<std::string>& roots() const { return roots_; }

private:
    struct Folder {
        std::vector<std::size_t> children; // indexes into the list's entries
        bool known = false;                // its children are all in the list
    };
    [[nodiscard]] Item item(const file_list::Entry& e) const;
    [[nodiscard]] Item root_item(const std::string& root) const;

    std::shared_ptr<const file_list::List> list_;
    std::vector<std::string> roots_;
    std::map<std::string, Folder, std::less<>> folders_;
};

// The root a path belongs to (the longest that holds it), if any.
[[nodiscard]] std::optional<std::string> root_of(const std::vector<std::string>& roots, std::string_view path);

// The path bar from a server's top to `path`: each step's name and path. With one root the
// root itself is the top and has no step; with several it is the first step ("~" for the
// login home). A path outside the roots starts from "/", or "~" when it is relative.
struct Crumb {
    std::string name;
    std::string path;
    bool operator==(const Crumb&) const = default;
};
[[nodiscard]] std::vector<Crumb> crumbs(const std::vector<std::string>& roots, std::string_view path);

// Why a folder couldn't be listed live, by kind, so the window tells a typed path that isn't there
// or isn't yours to list without reading the server's text.
struct ListError {
    enum class Kind {
        not_found,
        denied,
        file, // the path is a file
        other,
    };
    Kind kind = Kind::other;
    std::string message; // ready to show
    bool operator==(const ListError&) const = default;
};
// What a folder that can't be listed says, in the window's words, however it was opened (a typed
// or pinned path, a click): "Not found on Main: downloads/tv", "Main doesn't let you list
// downloads/tv" ("~" for the login folder); nullopt for any other failure, which shows as the
// server's.
[[nodiscard]] std::optional<std::string> asked_failure(std::string_view server, std::string_view folder, const ListError& e);

// The file list shows a folder empty, as it also shows one the account can't read (find lists the
// folder but can't go in), so Browse lists it live once (PINS.md, B6). Whether the list's empty
// rows still stand after that listing failed with `e`: they do for a failure that says nothing of
// the folder (no connection, a timeout), never for one that isn't there, can't be listed or is a
// file.
[[nodiscard]] bool empty_stands(const ListError& e);

// Lists one folder live: its SFTP listing on one of the server's pooled connections (`grab`;
// OPENDIR and READDIRs, a STAT for each link, whose target decides as the walk decides; a broken
// link, a socket or a device is left out). A path that can't be opened as a folder gets one STAT,
// which tells a file (ListError::Kind::file) from nothing there, and names that aren't UTF-8 are in
// grab's ‛XX form, so such folders open too (review G60). Only `run.stop` of `run` counts.
[[nodiscard]] std::expected<std::vector<Item>, ListError> list_live(const engine::Context& ctx, std::string_view path,
                                                                   const proc::RunOptions& run,
                                                                   const engine::CommandHook& on_command = {},
                                                                   const engine::GrabSide& grab = {});

} // namespace grab::browse
