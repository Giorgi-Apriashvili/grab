#pragma once

// A row of the window's Downloads table as its actions see it (UPLOADS.md 5.10, the audit):
// downloads and uploads share the table, the queue and its actions, but an upload's local side is
// the file or folder it was sent from, never something grab wrote. So nothing that opens, shows,
// discards or replaces a download's local file reaches an upload, and an upload's server side is
// what its own actions act on. App (src/gui) asks here before each action.

#include <filesystem>
#include <string>
#include <string_view>

namespace grab::queue_row {

enum class Direction { down, up };

struct Row {
    Direction direction = Direction::down;
    bool folder = false;
    std::string server;
    // On the server, grab's form: what a download fetches, where an upload goes.
    std::string path;
    std::filesystem::path dest;       // a download's Save-to folder
    std::filesystem::path local_name; // a file download's Keep-both name, instead of the server's
    std::filesystem::path source;     // an upload's local file or folder
};

// The local file or folder of a row: where a download lands (the server's name made safe for
// Windows, or its Keep-both name), and what an upload sends.
[[nodiscard]] std::filesystem::path local_of(const Row& r);

// Two rows with the same key never run at once (they would share a partial file): a download's is
// where it lands, as Windows names it ("D:\x\" and "d:\X" are one folder); an upload's is
// "up|<server>|<path>", the file or folder it makes on the server.
[[nodiscard]] std::filesystem::path::string_type key(const Row& r);

// A folder's file `i` on this PC (`rel` its path in the folder, `local_name` its Keep-both name if
// it has one): what Show in Explorer selects. Where a download's landed, the file an upload sends.
[[nodiscard]] std::filesystem::path local_file(const Row& r, std::string_view rel,
                                               const std::filesystem::path& local_name = {});

// The row actions the window's host decides by direction (the page's menus, buttons, keys and
// double-click; pause, resume, retry, priority and moves are the same for both).
enum class Act {
    open,             // a finished file download in its app, or a file of a folder download
    open_folder,      // a finished folder download's folder in Explorer
    discard_local,    // a cancelled download's partial files deleted on this PC
    resolve_existing, // the answer to a download's question about files already on this PC
    discard_server,   // a cancelled upload's temps and partial uploads removed on the server
};
// Whether `act` applies to a row of `direction`.
[[nodiscard]] bool allows(Direction direction, Act act);

// What undoes a row's unfinished work when it is cancelled, or a folder's file is skipped:
// discard_local (a download's partial files on this PC) or discard_server (an upload's temps and
// partial uploads on its server, never a file on this PC).
[[nodiscard]] Act undo_of(const Row& r);

} // namespace grab::queue_row
