#pragma once

// Browsing a server's folders in grab's window. A folder opens from the server's saved file list
// (file_list.hpp) when the list holds all of its contents, which is instant; deeper folders,
// beyond the list's max_depth, are listed live with one `rclone lsjson`.

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
    std::string path; // ready for rclone
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

// Whether `rclone lsjson <path>`'s answer may be `path` itself as a file: lsjson lists a file as
// that one file. A folder holding just a file of its own name looks the same.
[[nodiscard]] bool maybe_file(const std::vector<Item>& items, std::string_view path);

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

// One folder live: `rclone lsjson <remote:path>`.
[[nodiscard]] std::vector<std::string> listing_argv(const engine::Context& ctx, std::string_view path);
// Its output, as items under `path`; dot-entries dropped when `skip_hidden`.
[[nodiscard]] std::expected<std::vector<Item>, std::string> parse_lsjson(std::string_view json_text,
                                                                        std::string_view path, bool skip_hidden);
// Runs it.
[[nodiscard]] std::expected<std::vector<Item>, std::string> list_live(const engine::Context& ctx, std::string_view path,
                                                                     const proc::RunOptions& run,
                                                                     const engine::CommandHook& on_command = {});

} // namespace grab::browse
