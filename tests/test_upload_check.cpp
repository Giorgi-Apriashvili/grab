#include <doctest/doctest.h>

// The window's check before an upload (upload_check.hpp; UPLOADS.md 5.10): what was picked here,
// then the server's side, on an in-memory server every login writes (fake_pool.hpp).

#include "conn_budget.hpp"
#include "fake_pool.hpp"
#include "fetch.hpp"
#include "local_walk.hpp"
#include "rclone_encoding.hpp"
#include "sftp.hpp"
#include "upload_check.hpp"
#include "upload_existing.hpp"
#include "util.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)
#endif

using namespace grab;
namespace fs = std::filesystem;

namespace {

constexpr std::int64_t local_time = 1'700'001'000;

struct Dir {
    fs::path path;
    explicit Dir(std::string_view name) : path(fs::temp_directory_path() / "grab-tests-upload-check" / name) {
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~Dir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    Dir(const Dir&) = delete;
    Dir& operator=(const Dir&) = delete;
};

fs::path put_local(const fs::path& root, std::string_view rel, std::string_view content, std::int64_t mtime = local_time) {
    const fs::path p = local_walk::local_path(root, rclone_enc::from_raw_path(std::string(rel)));
    REQUIRE(util::write_file_atomic(p, content));
    fs::last_write_time(p, std::chrono::clock_cast<std::chrono::file_clock>(std::chrono::sys_seconds{std::chrono::seconds{mtime}}));
    return p;
}

// A server whose logins all write one store, with the folder /up in it.
struct Server {
    test::FakePool fake;
    Server() {
        fake.writable = true;
        fake.store.dirs["/"] = {".", ".."};
        folder("/up");
    }
    void folder(const std::string& path) {
        std::scoped_lock lock(fake.store.mutex);
        fake.store.dirs[path] = {".", ".."};
        const auto slash = path.rfind('/');
        fake.store.dirs[slash == 0 ? "/" : path.substr(0, slash)].push_back(path.substr(slash + 1));
    }
    void put(const std::string& path, std::string content, std::uint32_t mtime) {
        std::scoped_lock lock(fake.store.mutex);
        fake.store.files[path] = std::move(content);
        fake.store.mtimes[path] = mtime;
        const auto slash = path.rfind('/');
        fake.store.dirs[slash == 0 ? "/" : path.substr(0, slash)].push_back(path.substr(slash + 1));
    }
    [[nodiscard]] std::map<std::string, std::string> tree() {
        std::scoped_lock lock(fake.store.mutex);
        std::map<std::string, std::string> all = fake.store.files;
        for (const auto& [d, names] : fake.store.dirs) all["dir " + d] = util::join(names, "/");
        return all;
    }
};

#ifdef _WIN32
bool junction(const fs::path& link, const fs::path& target) {
    const std::wstring command = L"cmd /c mklink /J \"" + link.wstring() + L"\" \"" + target.wstring() + L"\" >nul 2>nul";
    return _wsystem(command.c_str()) == 0 && fs::exists(link);
}
#endif

// A check of `picked` into /up (or `dir`) on Fake, the local side only.
upload::Check local_check(const std::vector<fs::path>& picked, std::string dir = "/up") {
    upload::Check c;
    c.server = "Fake";
    c.dir = std::move(dir);
    int progress = 0;
    upload::look_local(c, picked, [&] { ++progress; });
    CHECK(progress == static_cast<int>(picked.size()));
    return c;
}

} // namespace

TEST_CASE("upload check: what was picked here, its files and bytes, links left out, names too long for a server") {
    Dir d("local");
    const fs::path a = put_local(d.path, "a.txt", "aaa");
    const fs::path src = d.path / "holiday";
    put_local(src, "1.jpg", std::string(1000, 'x'));
    put_local(src, "sub/2.jpg", std::string(500, 'y'));
    fs::create_directories(src / "empty");
    // 100 Georgian letters: 300 bytes, which no Linux file system takes as a name.
    std::string georgian;
    for (int i = 0; i < 100; ++i) georgian += "\xE1\x83\x90";
    put_local(src, "sub/" + georgian + ".txt", "z");
    std::size_t links = 0;
#ifdef _WIN32
    fs::create_directories(d.path / "outside");
    put_local(d.path / "outside", "o.txt", "never sent");
    REQUIRE(junction(src / "linked", d.path / "outside"));
    ++links;
#endif
    const auto c = local_check({a, src, d.path / "gone.txt"});
    REQUIRE(c.items.size() == 3);
    const auto& file = c.items[0];
    CHECK(file.name == "a.txt");
    CHECK_FALSE(file.folder);
    CHECK(file.target == "/up/a.txt");
    CHECK(file.files == 1);
    CHECK(file.bytes == 3);
    CHECK(file.mtime_ns == local_time * 1'000'000'000);
    CHECK(file.error.empty());
    const auto& folder = c.items[1];
    CHECK(folder.name == "holiday");
    CHECK(folder.folder);
    CHECK(folder.target == "/up/holiday");
    CHECK(folder.files == 3);
    CHECK(folder.bytes == 1501);
    REQUIRE(folder.tree);
    CHECK(folder.tree->listed);
    CHECK_FALSE(folder.tree->looked);
    CHECK(folder.tree->folders == std::vector<std::string>{"empty", "sub"});
    const auto& gone = c.items[2];
    CHECK_FALSE(gone.error.empty());
    // The totals leave out what can't go.
    CHECK(c.files == 4);
    CHECK(c.bytes == 1504);
    CHECK(c.links == links);
    CHECK(c.long_names == 1);
    CHECK(c.unreadable == 0);
    CHECK_FALSE(c.writable);
    // The login folder: the name alone.
    const auto home = local_check({a}, "");
    CHECK(home.items.front().target == "a.txt");
    // A stop before anything ends it there.
    std::stop_source stop;
    stop.request_stop();
    upload::Check stopped;
    upload::look_local(stopped, {a}, {}, stop.get_token());
    CHECK(stopped.items.empty());
}

TEST_CASE("upload check: the server takes it: free space, the names there by how they compare, a folder merged into") {
    Dir d("server");
    const fs::path same = put_local(d.path, "same.txt", "12345");
    const fs::path newer = put_local(d.path, "newer.txt", "abc", local_time + 3600);
    const fs::path other = put_local(d.path, "other.txt", "a longer one");
    const fs::path fresh = put_local(d.path, "fresh.txt", "new");
    const fs::path dirname = put_local(d.path, "adir", "a file where the server has a folder");
    const fs::path src = d.path / "photos";
    put_local(src, "1.jpg", "one");
    put_local(src, "2.jpg", "two");
    Server srv;
    srv.fake.statvfs = true;
    srv.fake.quota_bytes = std::uint64_t{64} << 20;
    srv.put("/up/same.txt", "12345", local_time);
    srv.put("/up/newer.txt", "abc", local_time);
    srv.put("/up/other.txt", "short", local_time);
    srv.folder("/up/adir");
    srv.folder("/up/photos");
    srv.put("/up/photos/1.jpg", "one", local_time);
    const auto before = srv.tree();
    auto c = local_check({same, newer, other, fresh, dirname, src});
    auto pool = srv.fake.pool(4);
    fetch::Connections connections;
    connections.configure("Fake", 4);
    upload::look_server(c, pool, connections);
    REQUIRE(c.writable);
    CHECK(*c.writable);
    CHECK(c.why.empty());
    REQUIRE(c.free);
    CHECK(*c.free > 0);
    CHECK_FALSE(upload::short_of_space(c));
    CHECK(c.taken.count == 5);
    CHECK(c.taken.same == 2); // same.txt, and photos/1.jpg
    CHECK(c.taken.newer == 1);
    CHECK(c.taken.differs == 1);
    CHECK(c.taken.older == 0);
    CHECK(c.taken.folders == 1);
    CHECK_FALSE(c.items[3].clash); // fresh.txt
    REQUIRE(c.items[4].clash.has_value());
    CHECK(c.items[4].clash.value_or(upload::Clash{}).remote_is_dir);
    const auto& photos = c.items[5];
    CHECK(photos.merging);
    REQUIRE(photos.tree);
    CHECK(photos.tree->looked);
    CHECK(photos.tree->clashes.size() == 1);
    // Nothing was written: the probe's file went again.
    CHECK(srv.tree() == before);

    // Less room than the bytes: said, still writable.
    Server small_disk;
    small_disk.fake.statvfs = true;
    small_disk.fake.quota_bytes = 8192;
    auto big = local_check({put_local(d.path, "big.bin", std::string(std::size_t{64} * 1024, 'b'))});
    auto small_pool = small_disk.fake.pool(2);
    upload::look_server(big, small_pool, connections);
    CHECK(big.writable == std::optional<bool>(true));
    CHECK(upload::short_of_space(big));
}

TEST_CASE("upload check: a read-only server (Box.Guest) says so plainly, and nothing more is asked of it") {
    Dir d("refused");
    const fs::path a = put_local(d.path, "a.txt", "aaa");
    struct Case {
        bool read_only;
        sftp::Code as;
        std::string dir;
        std::string why;
    };
    for (const auto& k : {Case{.read_only = true, .as = sftp::Code::failure, .dir = "/up",
                               .why = "Fake is read-only for this account: nothing can be uploaded to /up"},
                          Case{.read_only = true, .as = sftp::Code::permission_denied, .dir = "/up",
                               .why = "Fake doesn't let this account write in /up"},
                          Case{.read_only = false, .as = sftp::Code::ok, .dir = "/nope", .why = "there is no folder /nope on Fake"}}) {
        CAPTURE(k.why);
        Server srv;
        srv.fake.statvfs = true;
        srv.fake.read_only = k.read_only;
        srv.fake.read_only_as = k.as;
        srv.put("/up/a.txt", "aaa", local_time); // would be a name taken, were it asked
        auto c = local_check({a}, k.dir);
        auto pool = srv.fake.pool(2);
        fetch::Connections connections;
        connections.configure("Fake", 2);
        upload::look_server(c, pool, connections);
        CHECK(c.writable == std::optional<bool>(false));
        CHECK(c.why == k.why);
        CHECK(c.taken.count == 0);
        CHECK_FALSE(c.items[0].clash);
    }
    // No login: not known, and why.
    Server srv;
    srv.fake.login_fails = budget::Failure::fatal;
    auto c = local_check({a});
    auto pool = srv.fake.pool(2);
    fetch::Connections connections;
    connections.configure("Fake", 2);
    upload::look_server(c, pool, connections);
    CHECK_FALSE(c.writable);
    CHECK(c.why == "Fake refused the login: the key was refused");
}

TEST_CASE("upload check: a picked folder whose name is a file on the server can't go, the rest can") {
    Dir d("in-the-way");
    const fs::path a = put_local(d.path, "a.txt", "aaa");
    const fs::path src = d.path / "photos";
    put_local(src, "1.jpg", "one");
    Server srv;
    srv.put("/up/photos", "a file", local_time);
    auto c = local_check({src, a});
    auto pool = srv.fake.pool(2);
    fetch::Connections connections;
    connections.configure("Fake", 2);
    upload::look_server(c, pool, connections);
    CHECK(c.writable == std::optional<bool>(true));
    CHECK(c.items[0].error == "Fake:/up/photos is a file, so the folder can't go there");
    CHECK(c.items[1].error.empty());
    CHECK(upload::nothing_goes(c).empty()); // a.txt can: Upload stays

    // The folder alone: nothing of it can go, so the dialog refuses with why (review, chunk 3).
    auto alone = local_check({src});
    upload::look_server(alone, pool, connections);
    CHECK(alone.writable == std::optional<bool>(true));
    CHECK(upload::nothing_goes(alone) == "Fake:/up/photos is a file, so the folder can't go there");
    CHECK(upload::nothing_goes(upload::Check{}) == "Nothing was picked");
}

TEST_CASE("upload check: many picked files are looked at on the server many at once, not a round trip each") {
    Dir d("many");
    constexpr int count = 300; // more than one batch
    std::vector<fs::path> picked;
    for (int i = 0; i < count; ++i) picked.push_back(put_local(d.path, std::format("f{:03}.txt", i), "abc"));
    Server srv;
    srv.put("/up/f007.txt", "abc", local_time);         // the same
    srv.put("/up/f299.txt", "longer one", local_time); // another size
    srv.folder("/up/f150.txt");                         // a folder of that name
    srv.fake.latency = std::chrono::milliseconds(20);
    auto c = local_check(picked);
    auto pool = srv.fake.pool(2);
    fetch::Connections connections;
    connections.configure("Fake", 2);
    const auto started = std::chrono::steady_clock::now();
    upload::look_server(c, pool, connections);
    const auto took = std::chrono::steady_clock::now() - started;
    REQUIRE(c.writable == std::optional<bool>(true));
    CHECK(c.taken.count == 3);
    CHECK(c.taken.same == 1);
    CHECK(c.taken.differs == 1);
    CHECK(c.taken.folders == 1);
    CHECK(c.items[7].clash.has_value());
    CHECK(c.items[150].clash.has_value());
    CHECK(c.items[299].clash.has_value());
    CHECK_FALSE(c.items[8].clash.has_value());
    // One LSTAT a round trip would take count x latency (6 s); a batch at a time takes a few.
    CHECK(took < count / 4 * srv.fake.latency);
}

#ifdef _WIN32
TEST_CASE("upload check: a picked file or folder whose name ends with a dot or a space can't go (Windows would open another)") {
    Dir d("dots");
    put_local(d.path, "a", "the plain one");
    fs::create_directories(d.path / "d");
    // Made through \\?\, beside their plain twins: Win32 paths would open "a" and "d" for them.
    const auto raw = [&](std::wstring_view name) { return fs::path(L"\\\\?\\" + (d.path / name).wstring()); };
    {
        std::ofstream out(raw(L"a."), std::ios::binary);
        out << "the dotted one";
    }
    REQUIRE(fs::exists(raw(L"a.")));
    REQUIRE(fs::create_directory(raw(L"d ")));
    const auto c = local_check({d.path / L"a.", d.path / L"d ", d.path / "a"});
    REQUIRE(c.items.size() == 3);
    CHECK(c.items[0].error == "a.: its name ends with a dot or a space, which Windows programs can't open");
    CHECK(c.items[1].error == "d : its name ends with a dot or a space, which Windows programs can't open");
    CHECK_FALSE(c.items[1].tree);
    CHECK(c.items[2].error.empty());
    CHECK(c.files == 1);
    CHECK(c.bytes == 13); // the plain one's only
    std::error_code ec;
    fs::remove(raw(L"a."), ec);
    fs::remove(raw(L"d "), ec);
}
#endif

TEST_CASE("upload check: a drop of WebView2's own copies (files from inside a .zip, a browser's link) or of nothing is refused, others go") {
    // WebView2 writes a dropped file that has no place on disk straight into its temp folder (the
    // window's): here a folder of the test's own stands for it.
    Dir temp("webview-temp");
    Dir mine("mine");
    const auto copy = put_local(temp.path, "inner.png", "from a zip");
    const auto deeper = put_local(temp.path, "sub/kept.txt", "the user's own, a level down");
    const auto own = put_local(mine.path, "a.txt", "the user's own");
    fs::create_directories(mine.path / "folder");

    CHECK(upload::dropped_copy(copy, temp.path));
    CHECK(upload::dropped_copy(copy, fs::path(temp.path.native() + fs::path::preferred_separator))); // "C:\T\" is "C:\T"
    CHECK_FALSE(upload::dropped_copy(deeper, temp.path));
    CHECK_FALSE(upload::dropped_copy(own, temp.path));
    CHECK_FALSE(upload::dropped_copy(mine.path / "folder", temp.path));
    CHECK_FALSE(upload::dropped_copy(copy, fs::path()));
    // A temp folder that can't be opened: by its name, as Windows compares names.
    CHECK(upload::dropped_copy(fs::path("C:/nowhere-grab/x.png"), fs::path("C:/nowhere-grab")));
#ifdef _WIN32
    CHECK(upload::dropped_copy(fs::path(LR"(C:\Nowhere-Grab\x.png)"), fs::path(LR"(c:\NOWHERE-GRAB\)")));
#endif

    CHECK(upload::drop_refusal({}, temp.path, false) == upload::drop_nothing);
    CHECK(upload::drop_refusal({copy}, temp.path, false) == upload::drop_extract_first);
    CHECK(upload::drop_refusal({own, copy}, temp.path, false) == upload::drop_extract_first); // one drag, one source
    CHECK(upload::drop_refusal({own, mine.path / "folder", deeper}, temp.path, false).empty());
    // Many from one folder (each folder is looked at once), then one copy: still refused.
    std::vector<fs::path> many;
    many.reserve(502);
    for (int i = 0; i < 500; ++i) many.push_back(mine.path / std::format("f{}.txt", i));
    CHECK(upload::drop_refusal(many, temp.path, false).empty());
    many.push_back(copy);
    CHECK(upload::drop_refusal(many, temp.path, false) == upload::drop_extract_first);
    many.insert(many.begin(), temp.path / "first.png"); // the copy first, the user's own after
    CHECK(upload::drop_refusal(many, temp.path, false) == upload::drop_extract_first);

    // A URL came with the drop (the page's `url`): WebView2's copy is a browser's link shortcut
    // (a virtual Example Domain.url it saved), refused as a link; the user's own Internet shortcut
    // dragged from Explorer (its URL comes along) goes like any file, beside others too.
    const auto shortcut = put_local(temp.path, "Example Domain.url", "[InternetShortcut]\r\nURL=https://example.com/\r\n");
    const auto kept = put_local(mine.path, "kept.url", "[InternetShortcut]\r\nURL=https://example.org/\r\n");
    CHECK(upload::drop_refusal({shortcut}, temp.path, true) == upload::drop_link);
    CHECK(upload::drop_refusal({kept, shortcut}, temp.path, true) == upload::drop_link);
    CHECK(upload::drop_refusal({kept}, temp.path, true).empty());
    CHECK(upload::drop_refusal({kept, own}, temp.path, true).empty());
    CHECK(upload::drop_refusal({}, temp.path, true) == upload::drop_nothing);
    // The dialog's check takes such a drop as the file dialog's pick: files and folders mixed.
    const auto c = local_check({own, mine.path / "folder"});
    REQUIRE(c.items.size() == 2);
    CHECK_FALSE(c.items[0].folder);
    CHECK(c.items[1].folder);
}
