#include <doctest/doctest.h>

// The Downloads table's rows as their actions see them (queue_row.hpp; UPLOADS.md 5.10, the
// audit): an upload's row never reaches what opens, shows or discards a download's local file, and
// a download's never what acts on an upload's server side.

#include "queue_row.hpp"
#include "util.hpp"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using namespace grab;
using queue_row::Act;
using queue_row::Direction;

namespace {

queue_row::Row down_row(std::string path, std::filesystem::path dest, std::filesystem::path local_name = {}) {
    return queue_row::Row{.direction = Direction::down,
                          .folder = false,
                          .server = "Main",
                          .path = std::move(path),
                          .dest = std::move(dest),
                          .local_name = std::move(local_name),
                          .source = {}};
}

queue_row::Row up_row(std::string server, std::string path, std::filesystem::path source, bool folder = false) {
    return queue_row::Row{.direction = Direction::up,
                          .folder = folder,
                          .server = std::move(server),
                          .path = std::move(path),
                          .dest = {},
                          .local_name = {},
                          .source = std::move(source)};
}

} // namespace

TEST_CASE("queue rows: every row action on an upload row acts on the upload, never on a download's local file") {
    // Each action, and whether it is an upload's (up) and a download's (down).
    const std::vector<std::pair<Act, std::pair<bool, bool>>> table = {
        // A download's local file: opened, its folder opened, its partial download deleted on a
        // cancel, the question about files already here.
        {Act::open, {false, true}},
        {Act::open_folder, {false, true}},
        {Act::discard_local, {false, true}},
        {Act::resolve_existing, {false, true}},
        // An upload's partial uploads removed on the server on a cancel.
        {Act::discard_server, {true, false}},
    };
    for (const auto& [act, applies] : table) {
        CAPTURE(static_cast<int>(act));
        CHECK(queue_row::allows(Direction::up, act) == applies.first);
        CHECK(queue_row::allows(Direction::down, act) == applies.second);
    }

    // Cancel, and Skip on a folder's file (App::cancel_items, App::file_action): an upload's
    // unfinished work is undone on its server, never by deleting anything on this PC.
    const auto up_file = up_row("Main", "/srv/x.mkv", "E:\\Videos\\x.mkv");
    const auto up_folder = up_row("Main", "/srv/holiday", "E:\\Pictures\\holiday", true);
    CHECK(queue_row::undo_of(up_file) == Act::discard_server);
    CHECK(queue_row::undo_of(up_folder) == Act::discard_server);
    CHECK(queue_row::undo_of(down_row("/srv/x.mkv", "C:\\D")) == Act::discard_local);
    CHECK(queue_row::allows(Direction::up, queue_row::undo_of(up_file)));
    CHECK(queue_row::allows(Direction::down, queue_row::undo_of(down_row("/srv/x.mkv", "C:\\D"))));

    // Show in Explorer on a folder's file (App::show_file): an upload's is the file it sends,
    // whatever name it got on the server; a download's where it landed.
    CHECK(queue_row::local_file(up_folder, "sub/a.jpg") == std::filesystem::path("E:\\Pictures\\holiday\\sub\\a.jpg"));
    CHECK(queue_row::local_file(up_folder, "sub/a.jpg", "sub/a (2).jpg") ==
          std::filesystem::path("E:\\Pictures\\holiday\\sub\\a.jpg"));
    auto down_folder = down_row("/srv/holiday", "C:\\D");
    down_folder.folder = true;
    CHECK(queue_row::local_file(down_folder, "sub/a:b.jpg") == std::filesystem::path(L"C:\\D\\holiday\\sub\\a\uFF1Ab.jpg"));
    CHECK(queue_row::local_file(down_folder, "sub/a.jpg", "sub\\a (2).jpg") ==
          std::filesystem::path("C:\\D\\holiday\\sub\\a (2).jpg"));
    // Show in Explorer and Open folder on the row (App::open_item_folder): an upload's source.
    CHECK(queue_row::local_of(up_folder) == std::filesystem::path("E:\\Pictures\\holiday"));
}

TEST_CASE("queue rows: a row's local side, and the key that keeps two rows from running into one place") {
    // A download lands under the server's name made safe for Windows, or its Keep-both name.
    CHECK(queue_row::local_of(down_row("/srv/a:b.txt", "C:\\D")) == std::filesystem::path(L"C:\\D\\a\uFF1Ab.txt"));
    CHECK(queue_row::local_of(down_row("/srv/a.txt", "C:\\D", "a (2).txt")) == std::filesystem::path("C:\\D\\a (2).txt"));
    // An upload's local side is what it sends, never anything grab writes.
    const auto up = up_row("Main", "/srv/tv/x.mkv", "E:\\Videos\\x.mkv");
    CHECK(queue_row::local_of(up) == std::filesystem::path("E:\\Videos\\x.mkv"));

    // Two downloads into one place, however it is spelled, have one key.
    CHECK(queue_row::key(down_row("/a/x.bin", "D:\\Media\\")) == queue_row::key(down_row("/b/x.bin", "d:\\media")));
    CHECK(queue_row::key(down_row("/a/x.bin", "D:\\Media")) != queue_row::key(down_row("/a/y.bin", "D:\\Media")));
    // An upload's is where it goes on its server: the same file from two places is one upload, the
    // same name on another server another.
    CHECK(queue_row::key(up) == queue_row::key(up_row("Main", "/srv/tv/x.mkv", "F:\\other\\x.mkv")));
    CHECK(queue_row::key(up) != queue_row::key(up_row("Box", "/srv/tv/x.mkv", "E:\\Videos\\x.mkv")));
    CHECK(queue_row::key(up) != queue_row::key(up_row("Main", "/srv/tv/y.mkv", "E:\\Videos\\y.mkv")));
    CHECK(util::path_to_utf8(std::filesystem::path(queue_row::key(up))) == "up|Main|/srv/tv/x.mkv");
    // An upload and a download never share one, whatever their paths.
    CHECK(queue_row::key(up_row("Main", "C:\\D\\x.mkv", "C:\\D\\x.mkv")) != queue_row::key(down_row("/x.mkv", "C:\\D")));
}
