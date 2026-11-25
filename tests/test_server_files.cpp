#include <doctest/doctest.h>

// The changes the user makes to a server's files from Browse (server_files.hpp; SERVER-OPS.md,
// chunk 1) on an in-memory server every login shares (fake_pool.hpp): each one byte-exact, and a
// snapshot of the whole server showing that nothing else changed.

#include "conn_budget.hpp"
#include "fake_md5sum.hpp"
#include "fake_pool.hpp"
#include "fake_sftp.hpp"
#include "fetch.hpp"
#include "preview_jobs.hpp"
#include "rclone_encoding.hpp"
#include "server_files.hpp"
#include "sftp.hpp"
#include "ssh_hash.hpp"
#include "ssh_pool.hpp"
#include "upload.hpp"
#include "upload_existing.hpp"
#include "upload_folder.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace grab;
using namespace std::chrono_literals;
using namespace std::string_literals;
using server_files::Entry;
using upload::Existing;

namespace {

constexpr std::uint32_t old_time = 1'700'000'000;
constexpr std::uint32_t new_time = 1'700'500'000;

// The whole server as a user sees it: every file's bytes, mode and time, every folder's names and
// mode, every link's target.
struct Snap {
    std::map<std::string, std::string> files;
    std::map<std::string, std::set<std::string>> dirs;
    std::map<std::string, std::string> links;
    std::map<std::string, std::uint32_t> modes;
    std::map<std::string, std::uint32_t> mtimes;
    bool operator==(const Snap&) const = default;
};

std::string parent(const std::string& path) {
    const auto slash = path.rfind('/');
    return slash == 0 ? "/" : path.substr(0, slash);
}
std::string leaf(const std::string& path) { return path.substr(path.rfind('/') + 1); }

// A server whose logins all share one store, with the folder /srv in it.
struct Server {
    test::FakePool fake;
    fetch::Connections connections;
    std::shared_ptr<ssh::Pool> pool;
    Server() {
        fake.writable = true;
        fake.copy_data = true;
        fake.statvfs = true;
        fake.users_groups = true;
        fake.user_names = {{1000, "giorgi"}};
        fake.group_names = {{1000, "staff"}};
        fake.store.dirs["/"] = {".", ".."};
        connections.configure("Fake", 8);
        folder("/srv");
    }
    // The pool, made once the test has set the fake's switches (each login takes them as it starts).
    server_files::Where where() {
        if (!pool) pool = fake.pool();
        return server_files::Where{.server = "Fake", .pool = pool, .connections = &connections, .timeout = 30s, .on_command = {}, .home = {}, .on_undo = {}};
    }
    void folder(const std::string& path, std::uint32_t mode = 0755) {
        std::scoped_lock lock(fake.store.mutex);
        fake.store.dirs[path] = {".", ".."};
        fake.store.modes[path] = mode;
        if (path != "/") fake.store.dirs[parent(path)].push_back(leaf(path));
    }
    void put(const std::string& path, std::string content, std::uint32_t mtime = old_time, std::uint32_t mode = 0644) {
        std::scoped_lock lock(fake.store.mutex);
        fake.store.files[path] = std::move(content);
        fake.store.mtimes[path] = mtime;
        fake.store.modes[path] = mode;
        fake.store.dirs[parent(path)].push_back(leaf(path));
    }
    void link(const std::string& path, const std::string& target) {
        std::scoped_lock lock(fake.store.mutex);
        fake.store.links[path] = target;
        fake.store.dirs[parent(path)].push_back(leaf(path));
    }
    [[nodiscard]] Snap snap() {
        std::scoped_lock lock(fake.store.mutex);
        Snap s;
        s.files = fake.store.files;
        for (const auto& [path, names] : fake.store.dirs) {
            auto& listed = s.dirs[path];
            for (const auto& n : names)
                if (n != "." && n != "..") listed.insert(n);
            s.modes[path] = fake.store.modes.contains(path) ? fake.store.modes.at(path) : 0755;
        }
        s.links = fake.store.links;
        for (const auto& [path, content] : fake.store.files) {
            s.modes[path] = fake.store.modes.contains(path) ? fake.store.modes.at(path) : 0644;
            s.mtimes[path] = fake.store.mtimes.contains(path) ? fake.store.mtimes.at(path) : old_time;
        }
        return s;
    }
    // Every temp of grab's anywhere on the server.
    [[nodiscard]] std::vector<std::string> temps() {
        std::scoped_lock lock(fake.store.mutex);
        std::vector<std::string> out;
        for (const auto& [path, content] : fake.store.files)
            if (upload::is_temp_name(leaf(path))) out.push_back(path);
        for (const auto& [path, target] : fake.store.links)
            if (upload::is_temp_name(leaf(path))) out.push_back(path);
        return out;
    }
    // Requests of a type every login was sent so far (a session's are collected as it closes:
    // the idle ones are closed first).
    [[nodiscard]] std::size_t requests(std::string_view type) {
        if (pool) pool->trim_idle(0);
        std::scoped_lock lock(fake.mutex);
        return static_cast<std::size_t>(std::ranges::count(fake.requests, type));
    }
};

// What a snapshot becomes: a file, folder or link added, moved or gone.
void add_file(Snap& s, const std::string& path, std::string content, std::uint32_t mtime = old_time, std::uint32_t mode = 0644) {
    s.files[path] = std::move(content);
    s.mtimes[path] = mtime;
    s.modes[path] = mode;
    s.dirs[parent(path)].insert(leaf(path));
}
void drop_file(Snap& s, const std::string& path) {
    s.files.erase(path);
    s.mtimes.erase(path);
    s.modes.erase(path);
    s.dirs[parent(path)].erase(leaf(path));
}
void add_dir(Snap& s, const std::string& path, std::uint32_t mode = 0755) {
    s.dirs[path];
    s.modes[path] = mode;
    s.dirs[parent(path)].insert(leaf(path));
}
void drop_dir(Snap& s, const std::string& path) {
    s.dirs.erase(path);
    s.modes.erase(path);
    s.dirs[parent(path)].erase(leaf(path));
}
void add_link(Snap& s, const std::string& path, const std::string& target) {
    s.links[path] = target;
    s.dirs[parent(path)].insert(leaf(path));
}
void drop_link(Snap& s, const std::string& path) {
    s.links.erase(path);
    s.dirs[parent(path)].erase(leaf(path));
}
// Everything at and below `from` moved to `to` (a folder renamed).
void move_tree(Snap& s, const std::string& from, const std::string& to) {
    const auto moved = [&](const std::string& p) -> std::optional<std::string> {
        if (p == from) return to;
        if (p.starts_with(from + "/")) return to + p.substr(from.size());
        return std::nullopt;
    };
    const auto rekey = [&](auto& map) {
        auto copy = map;
        map.clear();
        for (auto& [k, v] : copy) map[moved(k).value_or(k)] = v;
    };
    rekey(s.files);
    rekey(s.dirs);
    rekey(s.links);
    rekey(s.modes);
    rekey(s.mtimes);
    s.dirs[parent(from)].erase(leaf(from));
    s.dirs[parent(to)].insert(leaf(to));
}

const Entry* find_entry(const server_files::Report& r, Entry::State state, std::string_view from) {
    const auto it = std::ranges::find_if(r.entries, [&](const Entry& e) { return e.state == state && e.from == from; });
    return it == r.entries.end() ? nullptr : &*it;
}

// How the server's READDIR answers (fake_sftp.hpp): as OpenSSH's, a link as what it leads to (a
// server that stat(2)s each name), or names without a type. A walk takes none of them through a link.
struct Listing {
    const char* name;
    bool follows_links;
    bool no_types;
};
const std::vector<Listing> listings{{.name = "openssh", .follows_links = false, .no_types = false},
                                    {.name = "follows links", .follows_links = true, .no_types = false},
                                    {.name = "no types", .follows_links = false, .no_types = true}};
void listing(Server& s, const Listing& l) {
    s.fake.readdir_follows_links = l.follows_links;
    s.fake.readdir_no_types = l.no_types;
}

// Folders below /src/many (d0..d<folders-1>) of `files` files each, of `bytes` bytes each (0: a
// few bytes naming it).
void put_many(Server& s, int folders, int files, std::size_t bytes = 0) {
    s.folder("/src");
    s.folder("/src/many");
    for (int d = 0; d < folders; ++d) {
        s.folder(std::format("/src/many/d{}", d));
        for (int f = 0; f < files; ++f) {
            s.put(std::format("/src/many/d{}/f{:02}", d, f), bytes == 0 ? std::format("file {} {}", d, f) : test::pattern(bytes));
        }
    }
}

// After a copy that ended early: every file new since `before` an exact copy of its source (its
// path with "/src" for `to`), every file and folder there before still there as it was (a folder
// with its mode), and no new folder empty.
void only_copies(const Snap& before, const Snap& after, const std::string& to) {
    for (const auto& [path, content] : after.files) {
        if (before.files.contains(path)) {
            CHECK(before.files.at(path) == content);
            continue;
        }
        CAPTURE(path);
        REQUIRE(path.starts_with(to + "/"));
        const std::string from = "/src" + path.substr(to.size());
        REQUIRE(before.files.contains(from));
        CHECK(before.files.at(from) == content);
    }
    for (const auto& [path, content] : before.files) CHECK(after.files.contains(path));
    for (const auto& [path, names] : after.dirs) {
        if (before.dirs.contains(path)) continue;
        CAPTURE(path);
        CHECK_FALSE(names.empty()); // an empty folder the copy made was removed
    }
    for (const auto& [path, names] : before.dirs) {
        CAPTURE(path);
        REQUIRE(after.dirs.contains(path));
        CHECK(after.modes.at(path) == before.modes.at(path));
    }
}

// A tree under /src/t: files at two depths, a hidden one, an empty folder, a link inside the tree,
// a link pointing outside it, and a link to a folder outside it (never walked into).
void put_tree(Server& s) {
    s.folder("/src");
    s.folder("/src/t");
    s.folder("/src/t/sub");
    s.folder("/src/t/sub/deep", 0700);
    s.folder("/src/t/empty");
    s.put("/src/t/a.txt", "alpha", old_time, 0600);
    s.put("/src/t/.hidden", "secret");
    s.put("/src/t/sub/b.bin", test::pattern(70'000), new_time, 0755);
    s.put("/src/t/sub/deep/c.txt", "");
    s.link("/src/t/to-a", "a.txt");
    s.folder("/elsewhere");
    s.put("/elsewhere/keep.txt", "keep me");
    s.link("/src/t/out", "/elsewhere/keep.txt");
    s.link("/src/t/dir-link", "/elsewhere");
}

// The same tree's files and links, as `root` holds them after a copy.
void add_tree(Snap& s, const std::string& root) {
    add_dir(s, root);
    add_dir(s, root + "/sub");
    add_dir(s, root + "/sub/deep", 0700);
    add_dir(s, root + "/empty");
    add_file(s, root + "/a.txt", "alpha", old_time, 0600);
    add_file(s, root + "/.hidden", "secret");
    add_file(s, root + "/sub/b.bin", test::pattern(70'000), new_time, 0755);
    add_file(s, root + "/sub/deep/c.txt", "");
    add_link(s, root + "/to-a", "a.txt");
    add_link(s, root + "/out", "/elsewhere/keep.txt");
    add_link(s, root + "/dir-link", "/elsewhere");
}

} // namespace

// ---- names, rename, new folder ----------------------------------------------------------------------------

TEST_CASE("server files: names a rename or new folder can't take") {
    CHECK(server_files::bad_name("ok.txt") == std::nullopt);
    CHECK(server_files::bad_name(".hidden") == std::nullopt);
    CHECK(server_files::bad_name("") == "a name can't be empty");
    CHECK(server_files::bad_name(".") == "\".\" can't be a name");
    CHECK(server_files::bad_name("..") == "\"..\" can't be a name");
    CHECK(server_files::bad_name("a/b") == "a name can't hold a /");
    CHECK(server_files::bad_name(std::string_view("a\0b", 3)) == "a name can't hold a NUL character");
    // In grab's form, as Browse shows names: what the server would get.
    CHECK(server_files::bad_name("a\xEF\xBC\x8F" "b") == "a name can't hold a /");              // '／'
    CHECK(server_files::bad_name("a\xE2\x90\x80" "b") == "a name can't hold a NUL character"); // '␀'
    CHECK(server_files::bad_name("\xEF\xBC\x8E") == "\".\" can't be a name");                // '．'
    CHECK(server_files::bad_name("\xEF\xBC\x8E\xEF\xBC\x8E") == "\"..\" can't be a name");
    CHECK(server_files::bad_name("\xE2\x80\x9B\xEF\xBC\x8F") == std::nullopt); // '‛／': a literal '／'
}

TEST_CASE("server files: a rename keeps bytes that aren't UTF-8: names are in grab's form, as Browse shows them") {
    Server s;
    s.put("/srv/caf\xE9.txt", "latin");
    const std::string shown = rclone_enc::from_raw_name("caf\xE9.txt");
    REQUIRE(shown == "caf\xE2\x80\x9B" "E9.txt");
    const auto w = s.where();
    Snap want = s.snap();
    // Its own name, as shown: nothing sent, nothing changed.
    auto r = server_files::rename(w, "/srv/" + shown, shown);
    REQUIRE(r);
    CHECK_FALSE(r->taken);
    CHECK(r->path == "/srv/" + shown);
    CHECK(s.requests("RENAME") == 0);
    CHECK(s.snap() == want);
    // Another extension: the byte 0xE9 kept.
    r = server_files::rename(w, "/srv/" + shown, "caf\xE2\x80\x9B" "E9.md");
    REQUIRE(r);
    CHECK(r->path == "/srv/caf\xE2\x80\x9B" "E9.md");
    drop_file(want, "/srv/caf\xE9.txt");
    add_file(want, "/srv/caf\xE9.md", "latin");
    CHECK(s.snap() == want);
    // A new folder so named; then its name taken.
    auto d = server_files::mkdir(w, "/srv", "d\xE2\x80\x9B" "E9");
    REQUIRE(d);
    CHECK(d->path == "/srv/d\xE2\x80\x9B" "E9");
    add_dir(want, "/srv/d\xE9");
    CHECK(s.snap() == want);
    d = server_files::mkdir(w, "/srv", "d\xE2\x80\x9B" "E9");
    REQUIRE(d);
    CHECK(d->taken);
    CHECK(s.snap() == want);
}

TEST_CASE("server files: rename a file and a folder; a name taken changes nothing") {
    Server s;
    s.put("/srv/a.txt", "alpha");
    s.put("/srv/b.txt", "bravo");
    s.folder("/srv/dir");
    s.put("/srv/dir/in.txt", "inside");
    const auto w = s.where();
    Snap want = s.snap();

    auto r = server_files::rename(w, "/srv/a.txt", "c.txt");
    REQUIRE(r);
    CHECK_FALSE(r->taken);
    CHECK(r->path == "/srv/c.txt");
    drop_file(want, "/srv/a.txt");
    add_file(want, "/srv/c.txt", "alpha");
    CHECK(s.snap() == want);

    r = server_files::rename(w, "/srv/dir", "folder");
    REQUIRE(r);
    CHECK(r->path == "/srv/folder");
    move_tree(want, "/srv/dir", "/srv/folder");
    CHECK(s.snap() == want);

    // Taken: by a file, by a folder; nothing sent but the looks.
    const std::size_t renames = s.requests("RENAME");
    CHECK(renames == 2);
    r = server_files::rename(w, "/srv/c.txt", "b.txt");
    REQUIRE(r);
    CHECK(r->taken);
    CHECK(r->why == "b.txt is already taken in /srv");
    r = server_files::rename(w, "/srv/b.txt", "folder");
    REQUIRE(r);
    CHECK(r->taken);
    CHECK(s.requests("RENAME") == renames);
    CHECK(s.snap() == want);

    // The same name: nothing sent at all.
    r = server_files::rename(w, "/srv/b.txt", "b.txt");
    REQUIRE(r);
    CHECK(r->path == "/srv/b.txt");
    // Gone meanwhile; a bad name; the login folder.
    auto gone = server_files::rename(w, "/srv/nothing", "x");
    REQUIRE_FALSE(gone);
    CHECK(gone.error() == "Fake:/srv/nothing is no longer there");
    CHECK(server_files::rename(w, "/srv/b.txt", "x/y").error() == "a name can't hold a /");
    CHECK(server_files::rename(w, "", "x").error() == "the login folder can't be renamed");
    CHECK(server_files::rename(w, "/", "x").error() == "the server's top folder can't be renamed");
    CHECK(s.snap() == want);
}

TEST_CASE("server files: the login folder, / and a folder holding the login folder are refused however spelled; one inside it isn't") {
    Server s;
    s.fake.home = "/home/u";
    s.folder("/home");
    s.folder("/home/u");
    s.folder("/home/u/d");
    s.put("/home/u/d/in.txt", "in");
    s.put("/home/u/x.txt", "x");
    s.folder("/dst");
    const Snap before = s.snap();
    // The login folder's real path given (the window's account), or asked by each change.
    for (const bool known : {true, false}) {
        CAPTURE(known);
        auto w = s.where();
        if (known) w.home = "/home/u";
        // Why a rename was refused ("" when it went).
        const auto rename_why = [&](std::string_view path, std::string_view name) {
            auto r = server_files::rename(w, path, name);
            return r ? std::string() : r.error();
        };
        const auto refused = [&](const std::string& path, const std::string& name) {
            CAPTURE(path);
            const std::vector<std::string> one{path};
            CHECK(rename_why(path, "v") == std::format("{} can't be renamed", name));
            CHECK(server_files::remove(w, one).error == std::format("{} can't be deleted", name));
            CHECK(server_files::move(w, server_files::into(one, "/dst"), Existing::replace).error == std::format("{} can't be moved", name));
            const auto look = server_files::look(w, server_files::into(one, "/dst"), false);
            REQUIRE_FALSE(look);
            CHECK(look.error() == std::format("{} can't be moved", name));
        };
        // By its full path from its parent, with "." and "..", from itself.
        for (const char* p : {"/home/u", "/home/./u", "//home/u", "/home/u/d/../../u", "/home/../home/u", ".", "d/.."}) {
            refused(p, "the login folder");
            const std::vector<std::string> one{p};
            CHECK(server_files::copy(w, server_files::into(one, "/dst"), Existing::ask).error == "the login folder can't be copied");
        }
        for (const char* p : {"/", "//", "/home/..", "/..", "/home/u/../.."}) refused(p, "the server's top folder");
        // A folder holding the login folder goes with it: refused too, with its own words; copied
        // elsewhere it may be, but never into the login folder (into itself).
        const std::vector<std::string> home_dir{"/home"};
        CHECK(rename_why("/home", "h") == "/home holds the login folder, so it can't be renamed");
        CHECK(server_files::remove(w, home_dir).error == "/home holds the login folder, so it can't be deleted");
        CHECK(server_files::move(w, server_files::into(home_dir, "/dst"), Existing::ask).error ==
              "/home holds the login folder, so it can't be moved");
        CHECK(server_files::copy(w, server_files::into(home_dir, "d"), Existing::ask).error == "home can't be copied into itself");
        // A folder copied into itself or onto itself under another spelling.
        const std::vector<std::string> d_abs{"/home/u/d"};
        CHECK(server_files::copy(w, server_files::into(d_abs, "d/sub"), Existing::ask).error == "d can't be copied into itself");
        CHECK(server_files::copy(w, server_files::into(d_abs, ""), Existing::keep_both).error == "d can't be copied onto itself");
        // A rename of the login folder spelled with a / at its end, or ".." at its end.
        for (const char* p : {"/home/u/", "/home/u/d/..", "./"}) {
            CAPTURE(p);
            CHECK(rename_why(p, "v") == "the login folder can't be renamed");
            CHECK(server_files::remove(w, std::vector<std::string>{p}).error == "the login folder can't be deleted");
        }
        CHECK(s.snap() == before);
    }
    CHECK(s.requests("RENAME") + s.requests("REMOVE") + s.requests("RMDIR") + s.requests("MKDIR") + s.requests("OPEN") == 0);

    // A folder inside the login folder is an ordinary one, by either spelling.
    const auto w = s.where();
    auto r = server_files::rename(w, "/home/u/d", "e");
    REQUIRE(r);
    CHECK(r->path == "/home/u/e");
    r = server_files::rename(w, "e", "d");
    REQUIRE(r);
    CHECK(r->path == "d");
    const std::vector<std::string> x{"/home/u/x.txt"};
    const auto moved = server_files::move(w, server_files::into(x, "/dst"), Existing::ask);
    CHECK(moved.error.empty());
    CHECK(moved.placed[0] == "/dst/x.txt");
}

TEST_CASE("server files: the login folder reached through a linked folder, or '..' after a link, is refused; a link to it stays a link") {
    Server s;
    s.fake.home = "/usr/home/u";
    s.folder("/usr");
    s.folder("/usr/home");
    s.folder("/usr/home/u");
    s.folder("/usr/home/u/sub");
    s.put("/usr/home/u/x.txt", "x");
    s.link("/home", "/usr/home");            // FreeBSD's /home, or one moved to a data disk
    s.folder("/a");
    s.link("/a/l", "/usr/home/u/sub");       // "/a/l/.." is the login folder on the server
    s.folder("/data");
    s.link("/data/me", "/usr/home/u");       // a link to the login folder
    s.folder("/dst");
    const Snap before = s.snap();
    for (const bool known : {true, false}) {
        CAPTURE(known);
        auto w = s.where();
        if (known) w.home = "/usr/home/u";
        const auto rename_why = [&](std::string_view path) {
            auto r = server_files::rename(w, path, "v");
            return r ? std::string() : r.error();
        };
        for (const char* p : {"/home/u", "/a/l/..", "/home/u/sub/.."}) {
            CAPTURE(p);
            const std::vector<std::string> one{p};
            CHECK(rename_why(p) == "the login folder can't be renamed");
            CHECK(server_files::remove(w, one).error == "the login folder can't be deleted");
        }
        // Moved or copied (a name ".." can't go anywhere: into() refuses it before).
        const std::vector<std::string> one{"/home/u"};
        CHECK(server_files::move(w, server_files::into(one, "/dst"), Existing::ask).error == "the login folder can't be moved");
        for (const bool copying : {false, true}) {
            const auto look = server_files::look(w, server_files::into(one, "/dst"), copying);
            REQUIRE_FALSE(look);
            CHECK(look.error() == std::format("the login folder can't be {}", copying ? "copied" : "moved"));
        }
        CHECK(server_files::copy(w, server_files::into(one, "/dst"), Existing::ask).error == "the login folder can't be copied");
        // Its parent reached through the link holds it.
        const std::vector<std::string> parent{"/home/u/.."};
        CHECK(server_files::remove(w, parent).error == "/home/u/.. holds the login folder, so it can't be deleted");
        // A path ending in '/' would act on what a link at its end points to.
        CHECK(rename_why("/data/me/") == "/data/me/ can't be renamed: name it without the '/' at its end");
        CHECK(server_files::remove(w, std::vector<std::string>{"/data/me/"}).error ==
              "/data/me/ can't be deleted: name it without the '/' at its end");
        CHECK(s.snap() == before);
    }
    CHECK(s.requests("RENAME") + s.requests("REMOVE") + s.requests("RMDIR") + s.requests("MKDIR") + s.requests("OPEN") == 0);

    // The link to the login folder is a link of its own: deleted, it goes alone.
    const auto w = s.where();
    const auto gone = server_files::remove(w, std::vector<std::string>{"/data/me"});
    CHECK(gone.error.empty());
    Snap want = before;
    drop_link(want, "/data/me");
    CHECK(s.snap() == want);
}

TEST_CASE("server files: a new folder; a name taken; a folder gone") {
    Server s;
    s.put("/srv/x", "file");
    const auto w = s.where();
    Snap want = s.snap();
    auto r = server_files::mkdir(w, "/srv", "new");
    REQUIRE(r);
    CHECK(r->path == "/srv/new");
    add_dir(want, "/srv/new");
    CHECK(s.snap() == want);
    r = server_files::mkdir(w, "/srv", "x");
    REQUIRE(r);
    CHECK(r->taken);
    CHECK(r->why == "x is already taken in /srv");
    CHECK(server_files::mkdir(w, "/srv/none", "y").error() == "Fake:/srv/none is no longer there");
    CHECK(server_files::mkdir(w, "/srv", "..").error() == "\"..\" can't be a name");
    CHECK(s.snap() == want);
}

// ---- move ------------------------------------------------------------------------------------------------

TEST_CASE("server files: move into a subfolder; links move as themselves; into itself refused before anything is sent") {
    Server s;
    s.put("/srv/a.txt", "alpha");
    s.folder("/srv/dir");
    s.put("/srv/dir/in.txt", "inside");
    s.folder("/srv/into");
    s.link("/srv/link", "a.txt");
    const auto w = s.where();
    Snap want = s.snap();

    const std::vector<std::string> picked{"/srv/a.txt", "/srv/dir", "/srv/link"};
    const auto items = server_files::into(picked, "/srv/into");
    REQUIRE(items.size() == 3);
    CHECK(items[1].to == "/srv/into/dir");
    const auto r = server_files::move(w, items, Existing::ask);
    CHECK(r.error.empty());
    CHECK(r.entries.empty());
    CHECK(r.files == 2);
    CHECK(r.folders == 1);
    CHECK(r.placed == std::vector<std::string>{"/srv/into/a.txt", "/srv/into/dir", "/srv/into/link"});
    drop_file(want, "/srv/a.txt");
    add_file(want, "/srv/into/a.txt", "alpha");
    move_tree(want, "/srv/dir", "/srv/into/dir");
    drop_link(want, "/srv/link");
    add_link(want, "/srv/into/link", "a.txt"); // the link itself, as stored
    CHECK(s.snap() == want);

    // Into itself, into its own subfolder: refused, nothing sent (no login even).
    test::FakePool fresh;
    fresh.writable = true;
    fetch::Connections c;
    const server_files::Where none{.server = "Fake", .pool = fresh.pool(), .connections = &c, .timeout = 30s, .on_command = {}, .home = {}, .on_undo = {}};
    for (const char* into : {"/srv/into/dir", "/srv/into/dir/x/y"}) {
        CAPTURE(into);
        const std::vector<std::string> one{"/srv/into/dir"};
        const auto refused = server_files::move(none, server_files::into(one, into), Existing::replace);
        CHECK(refused.error == "dir can't be moved into itself");
        CHECK(refused.files + refused.folders == 0);
        const auto copied = server_files::copy(none, server_files::into(one, into), Existing::replace);
        CHECK(copied.error == "dir can't be copied into itself");
        CHECK_FALSE(server_files::look(none, server_files::into(one, into), true));
    }
    CHECK(fresh.logins == 0);
    CHECK(s.snap() == want);
    // Where it is already: nothing to do.
    const std::vector<std::string> here{"/srv/into/a.txt"};
    const auto same = server_files::move(w, server_files::into(here, "/srv/into"), Existing::ask);
    CHECK(same.error.empty());
    CHECK(same.placed == std::vector<std::string>{"/srv/into/a.txt"});
    CHECK(s.snap() == want);
}

TEST_CASE("server files: move onto a name taken, by each existing-file choice") {
    struct Case {
        Existing choice;
        bool posix_rename;
        std::uint32_t there_time; // the file there; the moved one is new_time
        std::string there_bytes;
        enum { replaced, kept, numbered, asked } expect;
    };
    const std::vector<Case> cases{
        {.choice = Existing::replace, .posix_rename = true, .there_time = old_time, .there_bytes = "old", .expect = Case::replaced},
        {.choice = Existing::replace, .posix_rename = false, .there_time = old_time, .there_bytes = "old", .expect = Case::replaced},
        {.choice = Existing::different, .posix_rename = true, .there_time = new_time, .there_bytes = "moved", .expect = Case::kept}, // the same: kept
        {.choice = Existing::different, .posix_rename = true, .there_time = old_time, .there_bytes = "other", .expect = Case::replaced},
        {.choice = Existing::newer, .posix_rename = true, .there_time = old_time, .there_bytes = "old", .expect = Case::replaced},   // the moved one is newer
        {.choice = Existing::newer, .posix_rename = true, .there_time = new_time + 100, .there_bytes = "new", .expect = Case::kept}, // the one there is newer
        {.choice = Existing::skip, .posix_rename = true, .there_time = old_time, .there_bytes = "old", .expect = Case::kept},
        {.choice = Existing::keep_both, .posix_rename = true, .there_time = old_time, .there_bytes = "old", .expect = Case::numbered},
        {.choice = Existing::ask, .posix_rename = true, .there_time = old_time, .there_bytes = "old", .expect = Case::asked},
    };
    for (const Case& c : cases) {
        CAPTURE(upload::name(c.choice));
        CAPTURE(c.posix_rename);
        Server s;
        s.fake.posix_rename = c.posix_rename;
        s.folder("/srv/in");
        s.put("/srv/f.txt", "moved", new_time);
        s.put("/srv/in/f.txt", c.there_bytes, c.there_time);
        const auto w = s.where();
        Snap want = s.snap();
        const std::vector<std::string> picked{"/srv/f.txt"};
        const auto r = server_files::move(w, server_files::into(picked, "/srv/in"), c.choice);
        CHECK(r.error.empty());
        switch (c.expect) {
        case Case::replaced:
            drop_file(want, "/srv/f.txt");
            add_file(want, "/srv/in/f.txt", "moved", new_time);
            CHECK(r.placed[0] == "/srv/in/f.txt");
            CHECK(r.files == 1);
            CHECK(s.requests("REMOVE") == (c.posix_rename ? 0u : 1u));
            break;
        case Case::kept:
            CHECK(r.placed[0].empty());
            REQUIRE(find_entry(r, Entry::State::skipped, "/srv/f.txt") != nullptr);
            break;
        case Case::numbered:
            drop_file(want, "/srv/f.txt");
            add_file(want, "/srv/in/f (2).txt", "moved", new_time);
            CHECK(r.placed[0] == "/srv/in/f (2).txt");
            break;
        case Case::asked: {
            const Entry* e = find_entry(r, Entry::State::asking, "/srv/f.txt");
            REQUIRE(e != nullptr);
            REQUIRE(e->clash);
            CHECK(e->clash->remote_size == 3);
            CHECK(e->clash->local_size == 5);
            CHECK(e->to == "/srv/in/f.txt");
            break;
        }
        }
        CHECK(s.snap() == want);
    }
}

TEST_CASE("server files: move never replaces nor merges a folder; across file systems a plain refusal") {
    Server s;
    s.folder("/srv/in");
    s.folder("/srv/in/d");
    s.put("/srv/in/d/theirs.txt", "theirs");
    s.folder("/srv/d");
    s.put("/srv/d/mine.txt", "mine");
    s.put("/srv/in/f", "a file");
    s.folder("/srv/f");
    s.fake.mounts = {"/tmp"};
    s.folder("/tmp");
    const auto w = s.where();
    Snap want = s.snap();
    // A folder onto a folder, and a folder onto a file, under Replace: both skipped, saying why.
    std::vector<server_files::Item> items{{.from = "/srv/d", .to = "/srv/in/d", .choice = {}},
                                          {.from = "/srv/f", .to = "/srv/in/f", .choice = {}}};
    auto r = server_files::move(w, items, Existing::replace);
    CHECK(r.error.empty());
    REQUIRE(find_entry(r, Entry::State::skipped, "/srv/d") != nullptr);
    CHECK(find_entry(r, Entry::State::skipped, "/srv/d")->why == "a folder of that name is there");
    REQUIRE(find_entry(r, Entry::State::skipped, "/srv/f") != nullptr);
    CHECK(find_entry(r, Entry::State::skipped, "/srv/f")->why == "a file of that name is there, and a folder doesn't replace it");
    CHECK(s.snap() == want);
    // Keep both: the folder under a numbered name, "d (2)".
    r = server_files::move(w, std::span(items).first(1), Existing::keep_both);
    CHECK(r.placed[0] == "/srv/in/d (2)");
    move_tree(want, "/srv/d", "/srv/in/d (2)");
    CHECK(s.snap() == want);
    // Onto another file system: refused in plain words; nothing changed.
    const std::vector<std::string> one{"/srv/in/f"};
    r = server_files::move(w, server_files::into(one, "/tmp"), Existing::ask);
    REQUIRE(find_entry(r, Entry::State::failed, "/srv/in/f") != nullptr);
    CHECK(find_entry(r, Entry::State::failed, "/srv/in/f")->why ==
          "Fake can't move f into /tmp: that folder is on another file system of the server, and grab moves only within one");
    CHECK(s.snap() == want);
    // A folder this account can't write: the words name both folders (either may be the one).
    Server l;
    l.fake.locked_dirs = {"/srv/locked"};
    l.folder("/srv/locked");
    l.put("/srv/f.txt", "f");
    const auto lw = l.where();
    const Snap locked_before = l.snap();
    const std::vector<std::string> lf{"/srv/f.txt"};
    r = server_files::move(lw, server_files::into(lf, "/srv/locked"), Existing::ask);
    CHECK(r.error.empty());
    REQUIRE(find_entry(r, Entry::State::failed, "/srv/f.txt") != nullptr);
    CHECK(find_entry(r, Entry::State::failed, "/srv/f.txt")->why == "Fake doesn't let this account move f.txt from /srv into /srv/locked");
    CHECK(l.snap() == locked_before);
    // Two items for one name: the second left out.
    s.put("/srv/g.txt", "g1");
    s.put("/srv/in/d/g.txt", "g2");
    want = s.snap();
    const std::vector<std::string> both{"/srv/g.txt", "/srv/in/d/g.txt"};
    r = server_files::move(w, server_files::into(both, "/srv/f"), Existing::ask);
    CHECK(r.placed == std::vector<std::string>{"/srv/f/g.txt", ""});
    CHECK(find_entry(r, Entry::State::skipped, "/srv/in/d/g.txt")->why == "another item named g.txt goes there too");
    drop_file(want, "/srv/g.txt");
    add_file(want, "/srv/f/g.txt", "g1");
    CHECK(s.snap() == want);
}

TEST_CASE("server files: a move onto itself spelled another way changes nothing; a folder into itself so is refused") {
    for (const Existing choice : {Existing::replace, Existing::keep_both}) {
        CAPTURE(upload::name(choice));
        Server s;
        s.fake.posix_rename = false; // Replace would remove the one there first
        s.fake.home = "/home/u";
        s.folder("/home");
        s.folder("/home/u");
        s.put("/home/u/x.txt", "x");
        s.folder("/home/u/d");
        s.put("/home/u/d/in.txt", "in");
        const auto w = s.where();
        const Snap before = s.snap();
        // x.txt from the login folder, into "/home/u": the same folder by its real path.
        std::vector<server_files::Item> items{{.from = "x.txt", .to = "/home/u/x.txt", .choice = {}}};
        auto r = server_files::move(w, items, choice);
        CHECK(r.error.empty());
        CHECK(r.entries.empty());
        CHECK(r.placed == std::vector<std::string>{"/home/u/x.txt"});
        CHECK(s.requests("REMOVE") == 0);
        CHECK(s.requests("RENAME") == 0);
        CHECK(s.snap() == before);
        // The folder d into "/home/u/d": itself, seen with the login folder's real path before
        // anything is sent.
        items = {{.from = "d", .to = "/home/u/d/d", .choice = {}}};
        r = server_files::move(w, items, choice);
        CHECK(r.error == "d can't be moved into itself");
        CHECK(r.entries.empty());
        CHECK(s.requests("RENAME") == 0);
        CHECK(s.snap() == before);
    }
}

TEST_CASE("server files: a move into a folder gone meanwhile names it; a retry on another connection says nothing twice") {
    Server s;
    s.put("/srv/f.txt", "moved");
    s.put("/srv/g.txt", "g");
    s.folder("/srv/in");
    s.put("/srv/in/f.txt", "there");
    std::atomic<bool> armed{false};
    s.fake.break_at = [&](const std::string& path) { return path == "/srv/g.txt" && armed.exchange(false); };
    const auto w = s.where();
    Snap want = s.snap();
    const std::vector<std::string> f{"/srv/f.txt"};
    auto r = server_files::move(w, server_files::into(f, "/srv/none"), Existing::ask);
    CHECK(r.error.empty());
    REQUIRE(find_entry(r, Entry::State::failed, "/srv/f.txt") != nullptr);
    CHECK(find_entry(r, Entry::State::failed, "/srv/f.txt")->why == "Fake:/srv/none is no longer there");
    CHECK(s.snap() == want);

    // The connection that move used waits in the pool; it breaks as the second item is looked at,
    // before anything changed: tried again on a new one, the first item's question asked once.
    CHECK(s.fake.logins == 1);
    armed = true;
    const std::vector<std::string> both{"/srv/f.txt", "/srv/g.txt"};
    r = server_files::move(w, server_files::into(both, "/srv/in"), Existing::ask);
    CHECK(r.error.empty());
    CHECK(s.fake.logins == 2);
    CHECK(std::ranges::count(r.entries, Entry::State::asking, &Entry::state) == 1);
    CHECK(r.entries.size() == 1);
    CHECK(r.placed == std::vector<std::string>{"", "/srv/in/g.txt"});
    CHECK(r.files == 1);
    drop_file(want, "/srv/g.txt");
    add_file(want, "/srv/in/g.txt", "g");
    CHECK(s.snap() == want);
}

// ---- copy ------------------------------------------------------------------------------------------------

TEST_CASE("server files: copy a file and a tree on the server: bytes, modes, times, hidden names, links as links") {
    for (const Listing& l : listings) {
        CAPTURE(l.name);
        Server s;
        listing(s, l);
        put_tree(s);
        s.put("/srv/one.txt", "just one", new_time, 0640);
        s.folder("/dst");
        const auto w = s.where();
        Snap want = s.snap();
        std::vector<server_files::Progress> seen;
        const std::vector<std::string> picked{"/src/t", "/srv/one.txt"};
        const auto r = server_files::copy(w, server_files::into(picked, "/dst"), Existing::ask,
                                          [&](const server_files::Progress& p) { seen.push_back(p); });
        CHECK(r.error.empty());
        CHECK(r.entries.empty());
        CHECK_FALSE(r.stopped);
        CHECK(r.placed == std::vector<std::string>{"/dst/t", "/dst/one.txt"});
        CHECK(r.files == 8); // 5 files, 3 links
        CHECK(r.folders == 4);
        CHECK(r.bytes == 5 + 6 + 70'000 + 8);
        add_tree(want, "/dst/t");
        add_file(want, "/dst/one.txt", "just one", new_time, 0640);
        CHECK(s.snap() == want); // /elsewhere byte for byte as it was
        CHECK(s.temps().empty());
        REQUIRE_FALSE(seen.empty());
        CHECK(seen.back().files == 8);
        CHECK(seen.back().bytes == r.bytes);
        CHECK(seen.back().total_bytes == r.bytes);
        CHECK(seen.back().total_files == 8);
        // The link to a folder was never walked into: nothing of /elsewhere was copied but the links.
        CHECK_FALSE(s.snap().files.contains("/dst/t/dir-link/keep.txt"));
        // The folders' times and modes came last (deep, mode 0700).
        CHECK(s.snap().modes.at("/dst/t/sub/deep") == 0700);
    }
}

TEST_CASE("server files: an upload's or copy's temp in a folder copied isn't copied") {
    Server s;
    s.folder("/src");
    s.folder("/src/d");
    s.put("/src/d/a.txt", "alpha");
    const std::string temp = "/src/d/" + upload::temp_name("b.txt", upload::new_token());
    s.put(temp, "half");
    s.folder("/dst");
    const auto w = s.where();
    Snap want = s.snap();
    const std::vector<std::string> picked{"/src/d"};
    const auto r = server_files::copy(w, server_files::into(picked, "/dst"), Existing::ask);
    CHECK(r.error.empty());
    CHECK(r.files == 1);
    const Entry* e = find_entry(r, Entry::State::skipped, temp);
    REQUIRE(e != nullptr);
    CHECK(e->why == "a file grab is still writing (an upload's or a copy's)");
    add_dir(want, "/dst/d");
    add_file(want, "/dst/d/a.txt", "alpha");
    CHECK(s.snap() == want);
}

TEST_CASE("server files: a big file copied in pieces, its time and mode kept; a stop between pieces leaves nothing") {
    Server s;
    const std::string big = test::pattern(std::size_t{20} << 20);
    s.put("/srv/big.bin", big, new_time, 0640);
    s.folder("/dst");
    const auto w = s.where();
    Snap want = s.snap();
    std::vector<std::uint64_t> pieces;
    const std::vector<std::string> picked{"/srv/big.bin"};
    auto r = server_files::copy(w, server_files::into(picked, "/dst"), Existing::ask,
                                [&](const server_files::Progress& p) { pieces.push_back(p.bytes); });
    CHECK(r.error.empty());
    CHECK(r.placed[0] == "/dst/big.bin");
    add_file(want, "/dst/big.bin", big, new_time, 0640);
    CHECK(s.snap() == want);
    CHECK(pieces.size() >= 3); // the start, a piece of 8 MiB first, then the rest
    CHECK(std::ranges::is_sorted(pieces));

    // Stopped after its first piece: the temp removed, nothing at its place.
    std::stop_source stop;
    Server t;
    t.put("/srv/big.bin", big);
    t.folder("/dst");
    t.fake.on_copy = [&](std::uint64_t) { stop.request_stop(); };
    const auto tw = t.where();
    const Snap before = t.snap();
    r = server_files::copy(tw, server_files::into(picked, "/dst"), Existing::ask, {}, stop.get_token());
    CHECK(r.stopped);
    CHECK(r.files == 0);
    CHECK(r.placed[0].empty());
    CHECK(t.temps().empty());
    CHECK(t.snap() == before);
}

TEST_CASE("server files: a copy stopped midway removes its temps and only the empty folders it made") {
    Server s;
    s.folder("/src");
    s.folder("/src/many");
    for (int d = 0; d < 4; ++d) {
        s.folder(std::format("/src/many/d{}", d));
        for (int f = 0; f < 30; ++f) s.put(std::format("/src/many/d{}/f{:02}", d, f), std::format("file {} {}", d, f));
    }
    s.folder("/dst");
    std::stop_source stop;
    std::atomic<int> copies{0};
    s.fake.on_copy = [&](std::uint64_t) {
        if (++copies == 40) stop.request_stop(); // the first 32 are placed by then
    };
    const auto w = s.where();
    const Snap before = s.snap();
    const std::vector<std::string> picked{"/src/many"};
    const auto r = server_files::copy(w, server_files::into(picked, "/dst"), Existing::ask, {}, stop.get_token());
    CHECK(r.stopped);
    CHECK(r.error.empty());
    CHECK(s.temps().empty());
    const Snap after = s.snap();
    // Only copies of the files, byte for byte, and folders that hold some.
    for (const auto& [path, content] : after.files) {
        if (before.files.contains(path)) {
            CHECK(before.files.at(path) == content);
            continue;
        }
        REQUIRE(path.starts_with("/dst/many/"));
        CHECK(before.files.at("/src" + path.substr(4)) == content);
    }
    CHECK(after.files.size() - before.files.size() == r.files);
    CHECK(r.files > 0);
    CHECK(r.files < 120);
    for (const auto& [path, names] : after.dirs) {
        if (before.dirs.contains(path)) continue;
        CHECK_FALSE(names.empty()); // an empty folder the copy made was removed
    }
    CHECK(r.folders == after.dirs.size() - before.dirs.size());
}

TEST_CASE("server files: folders a copy makes are never more open than their sources, also when it stops midway") {
    Server s;
    s.folder("/src");
    s.folder("/src/private", 0700);
    s.folder("/src/private/group", 0750);
    s.folder("/src/private/shut", 0500); // filled by the owner, then shut again
    for (int f = 0; f < 30; ++f) s.put(std::format("/src/private/f{:02}", f), std::format("notes {}", f));
    for (int f = 0; f < 30; ++f) s.put(std::format("/src/private/group/g{:02}", f), std::format("group {}", f));
    for (int f = 0; f < 30; ++f) s.put(std::format("/src/private/shut/s{:02}", f), std::format("shut {}", f));
    s.folder("/dst");
    std::stop_source stop;
    std::atomic<int> copies{0};
    s.fake.on_copy = [&](std::uint64_t) {
        if (++copies == 40) stop.request_stop(); // the first 32 are placed by then
    };
    const auto w = s.where();
    const Snap before = s.snap();
    const std::vector<std::string> picked{"/src/private"};
    const auto r = server_files::copy(w, server_files::into(picked, "/dst"), Existing::ask, {}, stop.get_token());
    CHECK(r.stopped);
    CHECK(r.files > 0);
    const Snap after = s.snap();
    only_copies(before, after, "/dst");
    REQUIRE(after.dirs.contains("/dst/private"));
    for (const auto& [path, names] : after.dirs) {
        if (before.dirs.contains(path)) continue;
        CAPTURE(path);
        const std::uint32_t source = before.modes.at("/src" + path.substr(4));
        CHECK((after.modes.at(path) & 077 & ~source) == 0); // nothing for others the source doesn't give
    }

    // Run to its end: each folder's own mode exactly, the shut one shut again.
    Server t;
    t.folder("/src");
    t.folder("/src/private", 0700);
    t.folder("/src/private/shut", 0500);
    t.put("/src/private/shut/s", "shut");
    t.folder("/dst");
    const auto tw = t.where();
    Snap want = t.snap();
    const auto done = server_files::copy(tw, server_files::into(picked, "/dst"), Existing::ask);
    CHECK(done.error.empty());
    add_dir(want, "/dst/private", 0700);
    add_dir(want, "/dst/private/shut", 0500);
    add_file(want, "/dst/private/shut/s", "shut");
    CHECK(t.snap() == want);
}

TEST_CASE("server files: a copy stopped in a folder merged into leaves every folder there before as it was") {
    Server s;
    put_many(s, 4, 30);
    s.folder("/src/many/e");
    s.folder("/src/many/p", 0700);
    for (int f = 0; f < 5; ++f) s.put(std::format("/src/many/p/f{}", f), std::format("p {}", f));
    s.folder("/dst");
    s.folder("/dst/many", 0750);    // merged into
    s.folder("/dst/many/e");        // empty, merged into: never removed
    s.folder("/dst/many/p", 0700);  // empty, merged into
    s.folder("/dst/many/q", 0711);  // not in the copy at all
    s.put("/dst/many/keep.txt", "kept");
    std::stop_source stop;
    std::atomic<int> copies{0};
    s.fake.on_copy = [&](std::uint64_t) {
        if (++copies == 40) stop.request_stop();
    };
    const auto w = s.where();
    const Snap before = s.snap();
    const std::vector<std::string> picked{"/src/many"};
    const auto r = server_files::copy(w, server_files::into(picked, "/dst"), Existing::ask, {}, stop.get_token());
    CHECK(r.stopped);
    CHECK(r.error.empty());
    CHECK(s.temps().empty());
    CHECK(r.files > 0);
    only_copies(before, s.snap(), "/dst");
}

TEST_CASE("server files: a copy whose connection is lost leaves no temp and no empty folder of its own; what it placed is exact") {
    for (const bool at_open : {true, false}) {
        CAPTURE(at_open);
        Server s;
        put_many(s, 4, 30);
        s.folder("/dst");
        std::atomic<int> temps_opened{0};
        if (at_open) {
            // While the first 32 files' OPENs are on their way: the server made their temps, and
            // no answer comes back.
            s.fake.break_at = [&](const std::string& path) { return upload::is_temp_name(leaf(path)) && ++temps_opened == 5; };
        } else {
            s.fake.break_after_rename = true; // the first file put in place, its answer lost
        }
        const auto w = s.where();
        const Snap before = s.snap();
        const std::vector<std::string> picked{"/src/many"};
        const auto r = server_files::copy(w, server_files::into(picked, "/dst"), Existing::ask);
        CHECK(r.error.starts_with("the connection to Fake was lost"));
        CHECK_FALSE(r.stopped);
        CHECK(s.temps().empty());
        const Snap after = s.snap();
        only_copies(before, after, "/dst");
        if (at_open) CHECK(after == before);
        else CHECK(after.files.size() > before.files.size());
        CHECK(s.fake.logins == 2); // the cleaning on a second connection
    }
}

TEST_CASE("server files: a copy tells its temps and folders as they come and go; one cut off leaves them told, and the cleaner removes them") {
    // What is still made, by the copy's own words: (kind, path).
    using Told = std::set<std::pair<server_files::Undo::Kind, std::string>>;
    const auto still = [](const std::vector<server_files::Undo>& told) {
        Told out;
        for (const auto& u : told) {
            if (u.made) out.emplace(u.kind, u.path);
            else out.erase({u.kind, u.path});
        }
        return out;
    };
    const auto made_count = [](const std::vector<server_files::Undo>& told, server_files::Undo::Kind kind) {
        return std::ranges::count_if(told, [&](const server_files::Undo& u) { return u.made && u.kind == kind; });
    };

    // Run to its end: each temp told as it comes and as it is placed; each folder made, an empty one
    // too, told and then left to the copy. Nothing still made.
    Server s;
    put_many(s, 2, 3);
    s.folder("/src/many/empty");
    s.folder("/dst");
    auto w = s.where();
    std::vector<server_files::Undo> told;
    w.on_undo = [&](const server_files::Undo& u) { told.push_back(u); };
    const std::vector<std::string> picked{"/src/many"};
    const auto r = server_files::copy(w, server_files::into(picked, "/dst"), Existing::ask);
    CHECK(r.error.empty());
    CHECK(r.files == 6);
    CHECK(made_count(told, server_files::Undo::Kind::temp) == 6);
    CHECK(made_count(told, server_files::Undo::Kind::folder) == 4);
    for (const auto& u : told) {
        if (u.kind == server_files::Undo::Kind::temp) CHECK(upload::is_temp_name(rclone_enc::raw_path(leaf(u.path))));
    }
    CHECK(still(told).empty());
    CHECK(s.snap().dirs.contains("/dst/many/empty")); // the copy's own: never a leftover

    // Cut off: the connection lost while the first files' temps are opened, and no new login for the
    // cleaning (as when grab quits). The temps and folders it made are still told made: the window
    // keeps them in uploads.json, and the uploads' cleaner removes them at the next start.
    Server t;
    put_many(t, 4, 30);
    t.folder("/dst");
    std::atomic<int> opened{0};
    t.fake.break_at = [&](const std::string& path) {
        if (!upload::is_temp_name(leaf(path)) || ++opened != 5) return false;
        t.fake.login_fails = budget::Failure::fatal;
        return true;
    };
    auto tw = t.where();
    told.clear();
    tw.on_undo = [&](const server_files::Undo& u) { told.push_back(u); };
    const Snap before = t.snap();
    const auto cut = server_files::copy(tw, server_files::into(picked, "/dst"), Existing::ask);
    CHECK(cut.error.starts_with("the connection to Fake was lost"));
    const Told left = still(told);
    REQUIRE_FALSE(t.temps().empty()); // made on the server, not removed
    for (const auto& temp : t.temps()) CHECK(left.contains({server_files::Undo::Kind::temp, temp}));
    const Snap after = t.snap();
    std::size_t new_folders = 0;
    for (const auto& [path, names] : after.dirs) {
        if (before.dirs.contains(path)) continue;
        ++new_folders;
        CHECK(left.contains({server_files::Undo::Kind::folder, path}));
    }
    CHECK(new_folders == 5);
    upload::Leftovers leftovers;
    for (const auto& [kind, path] : left) (kind == server_files::Undo::Kind::temp ? leftovers.temps : leftovers.folders).push_back(path);
    t.fake.login_fails.reset();
    const auto still_left = upload::clean(*tw.pool, {}, "Fake", leftovers);
    CHECK(still_left.empty());
    CHECK(t.temps().empty());
    CHECK(t.snap() == before);
}

TEST_CASE("server files: closing waits for a stopped change's copy-data piece, then for its cleanup") {
    using std::chrono::milliseconds;
    const std::vector<milliseconds> two{30'000ms, 10'000ms};
    CHECK(server_files::close_wait(two, false, 5'000ms) == 35'000ms);
    CHECK(server_files::close_wait(std::vector<milliseconds>{2'000ms}, false, 5'000ms) == 7'000ms);
    // A long io_timeout (up to 24 h) or none: a hidden grab-gui lingers at most close_piece_most.
    CHECK(server_files::close_wait(std::vector<milliseconds>{86'400'000ms}, false, 5'000ms) == server_files::close_piece_most + 5'000ms);
    CHECK(server_files::close_wait(std::vector<milliseconds>{0ms}, false, 5'000ms) == server_files::close_piece_most + 5'000ms);
    // No copy among the changes (a rename, move or delete stops between requests): the cleanup's time.
    CHECK(server_files::close_wait(std::vector<milliseconds>{}, false, 5'000ms) == 5'000ms);
    // An update closing grab-gui ends it soon after: the cleanup's time only.
    CHECK(server_files::close_wait(two, true, 5'000ms) == 5'000ms);
}

TEST_CASE("server files: a copy that fills the server ends, its temps and its empty folders removed") {
    Server s;
    put_many(s, 4, 30, 1000);
    s.folder("/dst");
    {
        std::scoped_lock lock(s.fake.store.mutex);
        s.fake.quota_bytes = s.fake.store.used() + std::uint64_t{40} * 1000 + 500; // 40 copies fit, not 41
    }
    const auto w = s.where();
    const Snap before = s.snap();
    const std::vector<std::string> picked{"/src/many"};
    const auto r = server_files::copy(w, server_files::into(picked, "/dst"), Existing::ask);
    CHECK(r.error.find("is full") != std::string::npos);
    CHECK(r.files == 40);
    CHECK(s.temps().empty());
    const Snap after = s.snap();
    only_copies(before, after, "/dst");
    CHECK(after.files.size() - before.files.size() == 40);
}

TEST_CASE("server files: a folder that refuses new files fails only its own files in a copy; the rest is copied whole") {
    Server s;
    s.folder("/src");
    s.folder("/src/A");
    s.folder("/src/A/x");
    s.put("/src/A/x/1", "one");
    s.put("/src/A/x/2", "two");
    s.folder("/src/A/x/sub");
    s.put("/src/A/x/sub/6", "six");
    s.folder("/src/A/y");
    s.put("/src/A/y/3", "three");
    s.put("/src/A/y/4", "four");
    s.folder("/src/A/y/z");
    s.put("/src/A/y/z/5", "five");
    s.folder("/dst");
    s.folder("/dst/A");
    s.folder("/dst/A/x"); // merged into, and this account can't write in it
    s.fake.locked_dirs = {"/dst/A/x"};
    const auto w = s.where();
    Snap want = s.snap();
    const std::vector<std::string> picked{"/src/A"};
    const auto r = server_files::copy(w, server_files::into(picked, "/dst"), Existing::ask);
    CHECK(r.error.empty());
    CHECK(r.files == 3);
    const std::string said = "Fake doesn't let this account write in /dst/A/x";
    for (const char* f : {"/src/A/x/1", "/src/A/x/2", "/src/A/x/sub"}) {
        CAPTURE(f);
        const Entry* e = find_entry(r, Entry::State::failed, f);
        REQUIRE(e != nullptr);
        CHECK(e->why == said);
    }
    CHECK(find_entry(r, Entry::State::failed, "/src/A/x/sub/6") == nullptr); // its folder's entry says it
    add_dir(want, "/dst/A/y");
    add_dir(want, "/dst/A/y/z");
    add_file(want, "/dst/A/y/3", "three");
    add_file(want, "/dst/A/y/4", "four");
    add_file(want, "/dst/A/y/z/5", "five");
    CHECK(s.snap() == want);
    CHECK(s.temps().empty());
}

TEST_CASE("server files: copy onto a name taken, by each existing-file choice, also in a folder merged into") {
    struct Case {
        Existing choice;
        std::uint32_t there_time;
        std::string there_bytes;
        enum { replaced, kept, numbered, asked } expect;
    };
    const std::vector<Case> cases{
        {.choice = Existing::replace, .there_time = old_time, .there_bytes = "old", .expect = Case::replaced},
        {.choice = Existing::different, .there_time = new_time, .there_bytes = "copy", .expect = Case::kept}, // the same: kept
        {.choice = Existing::different, .there_time = old_time, .there_bytes = "other", .expect = Case::replaced},
        {.choice = Existing::newer, .there_time = old_time, .there_bytes = "old", .expect = Case::replaced},
        {.choice = Existing::newer, .there_time = new_time + 100, .there_bytes = "new", .expect = Case::kept},
        {.choice = Existing::skip, .there_time = old_time, .there_bytes = "old", .expect = Case::kept},
        {.choice = Existing::keep_both, .there_time = old_time, .there_bytes = "old", .expect = Case::numbered},
        {.choice = Existing::ask, .there_time = old_time, .there_bytes = "old", .expect = Case::asked},
    };
    for (const bool posix : {true, false}) {
        for (const Case& c : cases) {
            CAPTURE(upload::name(c.choice));
            CAPTURE(posix);
            Server s;
            s.fake.posix_rename = posix;
            s.folder("/src");
            s.folder("/src/d");
            s.put("/src/d/f.txt", "copy", new_time);
            s.put("/src/d/new.txt", "new one");
            s.folder("/dst");
            s.folder("/dst/d"); // merged into
            s.put("/dst/d/f.txt", c.there_bytes, c.there_time);
            s.put("/dst/d/other.txt", "untouched");
            s.put("/src/top.txt", "copy", new_time); // a file item onto a file
            s.put("/dst/top.txt", c.there_bytes, c.there_time);
            const auto w = s.where();
            Snap want = s.snap();
            const std::vector<std::string> picked{"/src/d", "/src/top.txt"};
            // The question first (look): every name taken, whatever the choice; nothing changed.
            const auto l = server_files::look(w, server_files::into(picked, "/dst"), true);
            REQUIRE(l);
            CHECK(l->taken.count == 2);
            CHECK(l->clashes.size() == 2);
            CHECK(l->files == 3);
            CHECK(l->folders == 1);
            CHECK(s.snap() == want);
            const auto r = server_files::copy(w, server_files::into(picked, "/dst"), c.choice);
            CHECK(r.error.empty());
            CHECK(s.temps().empty());
            add_file(want, "/dst/d/new.txt", "new one");
            CHECK(r.placed[0] == "/dst/d");
            for (const std::string& from : {std::string("/src/d/f.txt"), std::string("/src/top.txt")}) {
                const std::string to = "/dst" + from.substr(4);
                switch (c.expect) {
                case Case::replaced: add_file(want, to, "copy", new_time); break;
                case Case::kept: CHECK(find_entry(r, Entry::State::skipped, from) != nullptr); break;
                case Case::numbered: {
                    const std::string numbered = to.substr(0, to.size() - 4) + " (2).txt";
                    add_file(want, numbered, "copy", new_time);
                    break;
                }
                case Case::asked: {
                    const Entry* e = find_entry(r, Entry::State::asking, from);
                    REQUIRE(e != nullptr);
                    CHECK(e->to == to);
                    REQUIRE(e->clash);
                    CHECK(e->clash->remote_size == c.there_bytes.size());
                    break;
                }
                }
            }
            CHECK(r.placed[1] == (c.expect == Case::replaced   ? "/dst/top.txt"
                                  : c.expect == Case::numbered ? "/dst/top (2).txt"
                                                               : ""));
            CHECK(s.snap() == want);
        }
    }
}

TEST_CASE("server files: a copy's place refused leaves no temp; a folder a file has the name of fails, the rest goes") {
    Server s;
    s.fake.rename_refused = {"/dst/d/a.txt"};
    s.folder("/src");
    s.folder("/src/d");
    s.folder("/src/d/sub");
    s.put("/src/d/a.txt", "refused");
    s.put("/src/d/b.txt", "bravo");
    s.put("/src/d/sub/c.txt", "below a file's name");
    s.folder("/dst");
    s.folder("/dst/d");
    s.put("/dst/d/sub", "a file in the way");
    const auto w = s.where();
    Snap want = s.snap();
    const std::vector<std::string> picked{"/src/d"};
    const auto r = server_files::copy(w, server_files::into(picked, "/dst"), Existing::ask);
    CHECK(r.error.empty());
    CHECK(r.files == 1);
    REQUIRE(find_entry(r, Entry::State::failed, "/src/d/a.txt") != nullptr);
    REQUIRE(find_entry(r, Entry::State::failed, "/src/d/sub") != nullptr);
    CHECK(find_entry(r, Entry::State::failed, "/src/d/sub")->why == "Fake:/dst/d/sub is a file, so the folder can't be made there");
    CHECK(find_entry(r, Entry::State::failed, "/src/d/sub/c.txt") == nullptr); // the folder's entry says it
    add_file(want, "/dst/d/b.txt", "bravo");
    CHECK(s.snap() == want);
    CHECK(s.temps().empty());
}

TEST_CASE("server files: a copy of a folder onto itself refused; a file onto itself by the choice") {
    Server s;
    s.folder("/srv/d");
    s.put("/srv/d/x.txt", "x");
    const auto w = s.where();
    Snap want = s.snap();
    const std::vector<std::string> folder{"/srv/d"};
    auto r = server_files::copy(w, server_files::into(folder, "/srv"), Existing::keep_both);
    CHECK(r.error == "d can't be copied onto itself");
    CHECK(s.requests("OPEN") == 0);
    CHECK(s.snap() == want);
    const std::vector<std::string> file{"/srv/d/x.txt"};
    r = server_files::copy(w, server_files::into(file, "/srv/d"), Existing::keep_both);
    CHECK(r.placed[0] == "/srv/d/x (2).txt");
    add_file(want, "/srv/d/x (2).txt", "x");
    CHECK(s.snap() == want);
    r = server_files::copy(w, server_files::into(file, "/srv/d"), Existing::different);
    CHECK(find_entry(r, Entry::State::skipped, "/srv/d/x.txt") != nullptr);
    CHECK(s.snap() == want);
}

TEST_CASE("server files: links on a server that makes none are skipped with the reason; a server without copy-data says so") {
    Server s;
    s.fake.symlink_refused = true;
    put_tree(s);
    s.folder("/dst");
    const auto w = s.where();
    Snap want = s.snap();
    const std::vector<std::string> picked{"/src/t"};
    const auto r = server_files::copy(w, server_files::into(picked, "/dst"), Existing::ask);
    CHECK(r.error.empty());
    CHECK(r.files == 4);
    add_tree(want, "/dst/t");
    for (const char* l : {"/dst/t/to-a", "/dst/t/out", "/dst/t/dir-link"}) {
        drop_link(want, l);
        const auto e = std::ranges::find(r.entries, std::string(l), &Entry::to);
        REQUIRE(e != r.entries.end());
        CHECK(e->state == Entry::State::skipped);
        CHECK(e->why == "the server didn't make the link: permission denied");
    }
    CHECK(s.snap() == want);
    CHECK(s.temps().empty());

    Server none;
    none.fake.copy_data = false;
    none.put("/srv/a", "a");
    none.folder("/srv/b");
    const auto nw = none.where();
    const std::vector<std::string> a{"/srv/a"};
    const std::string said = "Fake doesn't copy files itself (its SSH server is older than OpenSSH 9.0)";
    CHECK(server_files::no_copy_data("Fake") == said);
    const Snap before = none.snap();
    CHECK(server_files::copy(nw, server_files::into(a, "/srv/b"), Existing::ask).error == said);
    // The paste's look says it before anything is looked at, so no question comes first; a move
    // (a rename) is looked at and goes as anywhere.
    const auto look = server_files::look(nw, server_files::into(a, "/srv/b"), true);
    REQUIRE_FALSE(look);
    CHECK(look.error() == said);
    CHECK(none.requests("LSTAT") + none.requests("STAT") + none.requests("OPENDIR") == 0);
    CHECK(none.snap() == before);
    CHECK(server_files::look(nw, server_files::into(a, "/srv/b"), false));
    CHECK(server_files::move(nw, server_files::into(a, "/srv/b"), Existing::ask).placed[0] == "/srv/b/a");
    // The account says so for the window's menus.
    const auto account = server_files::account(nw);
    REQUIRE(account);
    CHECK_FALSE(account->copy_data);
    CHECK(server_files::account(s.where())->copy_data);
}

// ---- delete ----------------------------------------------------------------------------------------------

TEST_CASE("server files: delete a file, a link (never its target) and a tree with hidden names and links") {
    for (const Listing& l : listings) {
        CAPTURE(l.name);
        Server s;
        listing(s, l);
        put_tree(s);
        s.put("/srv/f.txt", "file");
        s.link("/srv/l", "/elsewhere/keep.txt");
        const auto w = s.where();
        Snap want = s.snap();
        std::vector<server_files::Progress> seen;
        const std::vector<std::string> paths{"/srv/f.txt", "/srv/l", "/src/t", "/srv/gone"};
        const auto r = server_files::remove(w, paths, [&](const server_files::Progress& p) { seen.push_back(p); });
        CHECK(r.error.empty());
        CHECK(r.files == 2 + 4 + 3);
        CHECK(r.folders == 4);
        CHECK(r.left == 0);
        REQUIRE(find_entry(r, Entry::State::skipped, "/srv/gone") != nullptr);
        CHECK(find_entry(r, Entry::State::skipped, "/srv/gone")->why == "Fake:/srv/gone is no longer there");
        drop_file(want, "/srv/f.txt");
        drop_link(want, "/srv/l");
        for (const char* f : {"/src/t/a.txt", "/src/t/.hidden", "/src/t/sub/b.bin", "/src/t/sub/deep/c.txt"}) drop_file(want, f);
        for (const char* link : {"/src/t/to-a", "/src/t/out", "/src/t/dir-link"}) drop_link(want, link);
        for (const char* d : {"/src/t/sub/deep", "/src/t/sub", "/src/t/empty", "/src/t"}) drop_dir(want, d);
        CHECK(s.snap() == want); // /elsewhere byte for byte as it was
        // What the links pointed to is all there.
        CHECK(s.snap().files.at("/elsewhere/keep.txt") == "keep me");
        REQUIRE_FALSE(seen.empty());
        CHECK(seen.back().files == 9);
        CHECK(seen.back().total_files == 9);
    }
}

TEST_CASE("server files: a delete refused partway says what went and what is left; an unreadable folder deletes nothing") {
    Server s;
    s.folder("/srv/t");
    s.folder("/srv/t/a");
    s.folder("/srv/t/b");
    for (const char* f : {"/srv/t/a/1", "/srv/t/a/2", "/srv/t/a/3", "/srv/t/b/4", "/srv/t/b/5"}) s.put(f, f);
    s.fake.locked_dirs = {"/srv/t/b"};
    const auto w = s.where();
    Snap want = s.snap();
    const std::vector<std::string> t{"/srv/t"};
    auto r = server_files::remove(w, t);
    CHECK(r.files == 3);
    CHECK(r.folders == 0);
    CHECK(r.left == 5);
    CHECK(r.error == "Fake doesn't let this account write in /srv/t/b; 3 of 5 files and 0 of 3 folders were deleted, 5 still there");
    for (const char* f : {"/srv/t/a/1", "/srv/t/a/2", "/srv/t/a/3"}) drop_file(want, f);
    CHECK(s.snap() == want);

    Server u;
    u.folder("/srv/t");
    u.folder("/srv/t/closed");
    u.put("/srv/t/x", "x");
    u.put("/srv/t/closed/y", "y");
    u.fake.denied = {"/srv/t/closed"};
    const auto uw = u.where();
    const Snap before = u.snap();
    r = server_files::remove(uw, t);
    CHECK(r.error == "Fake:/srv/t can't be deleted: /srv/t/closed can't be read (permission denied); nothing was deleted");
    CHECK(r.files + r.folders == 0);
    CHECK(u.snap() == before);
    CHECK(u.requests("REMOVE") == 0);
}

TEST_CASE("server files: a read-only server refuses every change in plain words, nothing changed") {
    Server s;
    put_tree(s);
    s.folder("/dst");
    s.put("/srv/f.txt", "f");
    s.fake.read_only = true;
    s.fake.read_only_as = sftp::Code::failure; // as Box.Guest: "Failure", statvfs ST_RDONLY
    const auto w = s.where();
    const Snap before = s.snap();
    const std::string said = "Fake is read-only for this account";
    CHECK(server_files::rename(w, "/srv/f.txt", "g.txt").error() == said);
    CHECK(server_files::mkdir(w, "/srv", "new").error() == said);
    const std::vector<std::string> f{"/srv/f.txt"};
    const std::vector<std::string> t{"/src/t"};
    CHECK(server_files::move(w, server_files::into(f, "/dst"), Existing::ask).error == said);
    CHECK(server_files::copy(w, server_files::into(f, "/dst"), Existing::ask).error == said);
    CHECK(server_files::copy(w, server_files::into(t, "/dst"), Existing::ask).error == said);
    CHECK(server_files::remove(w, f).error == said + "; nothing was deleted");
    CHECK(server_files::remove(w, t).error == said + "; nothing was deleted");
    CHECK(s.snap() == before);
    const auto p = server_files::properties(w, "/srv/f.txt");
    REQUIRE(p);
    CHECK(p->read_only);
}

// ---- properties, sizes, sums -------------------------------------------------------------------------------

TEST_CASE("server files: properties of a file, a folder and links; owners' names or numbers; free space") {
    Server s;
    s.put("/srv/f.txt", "hello", new_time, 0640);
    s.link("/srv/l", "f.txt");
    s.link("/srv/dangling", "nowhere");
    const auto w = s.where();
    auto p = server_files::properties(w, "/srv/f.txt");
    REQUIRE(p);
    CHECK(p->kind == server_files::Properties::Kind::file);
    CHECK(p->attrs.size == 5u);
    CHECK(p->attrs.mtime == new_time);
    CHECK(p->attrs.permissions == 0100640u);
    CHECK(p->owner == "giorgi");
    CHECK(p->group == "staff");
    CHECK(p->names);
    CHECK(p->free == (std::uint64_t{1} << 40) - 4096); // whole blocks of 4 KiB
    CHECK_FALSE(p->read_only);
    p = server_files::properties(w, "/srv");
    REQUIRE(p);
    CHECK(p->kind == server_files::Properties::Kind::folder);
    p = server_files::properties(w, "/srv/l");
    REQUIRE(p);
    CHECK(p->kind == server_files::Properties::Kind::link);
    CHECK(p->link == "f.txt");
    REQUIRE(p->target);
    CHECK(p->target->size == 5u);
    p = server_files::properties(w, "/srv/dangling");
    REQUIRE(p);
    CHECK(p->link == "nowhere");
    CHECK_FALSE(p->target);
    CHECK(server_files::properties(w, "/srv/none").error() == "Fake:/srv/none is no longer there");

    // Numbers where the server names nobody, or doesn't offer names.
    Server t;
    t.fake.users_groups = false;
    t.put("/srv/f.txt", "x");
    p = server_files::properties(t.where(), "/srv/f.txt");
    REQUIRE(p);
    CHECK(p->owner == "1000");
    CHECK(p->group == "1000");
    CHECK_FALSE(p->names);
}

TEST_CASE("server files: a folder's size, never through a link; MD5 and SHA-256 from the server") {
    for (const Listing& l : listings) {
        CAPTURE(l.name);
        Server s;
        listing(s, l);
        put_tree(s);
        const auto w = s.where();
        const std::vector<std::string> t{"/src/t", "/elsewhere/keep.txt"};
        std::size_t calls = 0;
        const auto size = server_files::folder_size(w, t, [&](const server_files::Progress&) { ++calls; });
        REQUIRE(size);
        CHECK(size->files == 5);
        CHECK(size->folders == 4);
        CHECK(size->links == 3);
        CHECK(size->bytes == 5 + 6 + 70'000 + 7);
        CHECK(size->unreadable.empty());
        CHECK(calls > 0);
    }

    test::Box box;
    box.files["/home/test/movie.mkv"] = "the film";
    box.files["/srv/abs.txt"] = "absolute";
    const server_files::Where bw{.server = "Box", .pool = box.pool(), .connections = nullptr, .timeout = 30s, .on_command = {}, .home = {}, .on_undo = {}};
    CHECK(server_files::hash(bw, {}, "movie.mkv", 8, ssh::Sum::md5) == test::md5_hex("the film"));
    CHECK(server_files::hash(bw, {}, "movie.mkv", 8, ssh::Sum::sha256) == test::sha256_hex("the film"));
    CHECK(server_files::hash(bw, {}, "/srv/abs.txt", 8, ssh::Sum::sha256) == test::sha256_hex("absolute"));
    CHECK(box.commands.back().starts_with("sha256sum -z '/srv/abs.txt'"));
    box.sha256 = false;
    const auto none = server_files::hash(bw, {}, "movie.mkv", 8, ssh::Sum::sha256);
    REQUIRE_FALSE(none);
    CHECK(none.error() == "no sum from the server (sh: 1: sha256sum: not found)");
}

// ---- around them -------------------------------------------------------------------------------------------

TEST_CASE("server files: no change to a path a transfer or Play is using, nor to a folder holding one") {
    using server_files::Busy;
    const std::vector<Busy> busy{{.kind = Busy::Kind::download, .path = "/srv/movies/a.mkv"},
                                 {.kind = Busy::Kind::upload, .path = "/srv/up"},
                                 {.kind = Busy::Kind::play, .path = "tv/s01e01.mkv"}};
    const auto check = [&](std::vector<std::string> paths) { return server_files::busy_refusal(busy, paths, "Main"); };
    CHECK(check({"/srv/movies/b.mkv", "/srv/upload", "tv/s01e02.mkv", "/srv/movies/a.mkv.txt"}) == std::nullopt);
    CHECK(check({"/srv/movies/a.mkv"}) == "Main:/srv/movies/a.mkv is being downloaded: wait until it ends, or cancel it first");
    CHECK(check({"/srv/movies"}) ==
          "Main:/srv/movies holds /srv/movies/a.mkv, which is being downloaded: wait until it ends, or cancel it first");
    CHECK(check({"/srv"}).has_value());
    CHECK(check({"/srv/up/x/y.txt"}) ==
          "Main:/srv/up/x/y.txt is inside /srv/up, which is being uploaded: wait until it ends, or cancel it first");
    CHECK(check({"tv"}) == "Main:tv holds tv/s01e01.mkv, which is playing: stop playing it first");
    CHECK(check({"tv/s01e01.mkv"}) == "Main:tv/s01e01.mkv is playing: stop playing it first");
    CHECK(server_files::busy_refusal({}, std::vector<std::string>{"/srv"}, "Main") == std::nullopt);

    // Other spellings of one place: from the login folder or not, "." and "..".
    CHECK(server_files::canonical("a/../x", "") == "x");
    CHECK(server_files::canonical("/a/./b/../c/", "") == "/a/c");
    CHECK(server_files::canonical("/../x", "") == "/x");
    CHECK(server_files::canonical("../x", "") == "../x");
    CHECK(server_files::canonical("", "/home/u") == "/home/u");
    CHECK(server_files::canonical("tv/./a", "/home/u") == "/home/u/tv/a");
    CHECK(server_files::canonical("/srv/x", "/home/u") == "/srv/x");
    const std::vector<Busy> relative{{.kind = Busy::Kind::download, .path = "Videos/a.mkv"}};
    const auto in_home = [&](std::vector<std::string> paths) { return server_files::busy_refusal(relative, paths, "Main", "/home/dragon"); };
    CHECK(in_home({"/home/dragon/Videos/a.mkv"}) ==
          "Main:/home/dragon/Videos/a.mkv is being downloaded: wait until it ends, or cancel it first");
    CHECK(in_home({"/home/dragon/Videos/x/../a.mkv"}).has_value());
    CHECK(in_home({"/home/dragon"}).has_value());
    CHECK(in_home({""}).has_value());
    CHECK(in_home({"/home/dragon/Videos/b.mkv", "Videos/b.mkv"}) == std::nullopt);
    // The login folder not known: only the spellings given compare.
    CHECK(server_files::busy_refusal(relative, std::vector<std::string>{"/home/dragon/Videos/a.mkv"}, "Main") == std::nullopt);
}

TEST_CASE("server files: the account the window keeps: the login folder's real path, read-only or not, free space") {
    Server s;
    s.fake.home = "/home/u";
    s.folder("/home");
    s.folder("/home/u");
    auto a = server_files::account(s.where());
    REQUIRE(a);
    CHECK(a->home == "/home/u");
    CHECK_FALSE(a->read_only);
    CHECK(a->free == std::uint64_t{1} << 40); // nothing stored yet
    // Box.Guest: statvfs says read-only; nothing is changed by asking.
    Server ro;
    ro.folder("/home");
    ro.folder("/home/test");
    ro.fake.read_only = true;
    ro.fake.read_only_as = sftp::Code::failure;
    const Snap before = ro.snap();
    a = server_files::account(ro.where());
    REQUIRE(a);
    CHECK(a->home == "/home/test");
    CHECK(a->read_only);
    CHECK(ro.snap() == before);
    // Without statvfs: taken as writable, free space unknown.
    Server plain;
    plain.fake.statvfs = false;
    a = server_files::account(plain.where());
    REQUIRE(a);
    CHECK_FALSE(a->read_only);
    CHECK_FALSE(a->free);
    // No connection: the error.
    const server_files::Where none{.server = "Gone", .pool = nullptr, .connections = nullptr, .timeout = 30s, .on_command = {}, .home = {}, .on_undo = {}};
    CHECK(server_files::account(none).error() == "no connection to Gone");
}

TEST_CASE("server files: each path with its folder's real path, a link kept as itself") {
    Server s;
    s.fake.home = "/home/u";
    s.folder("/home");
    s.folder("/home/u");
    s.folder("/data");
    s.folder("/data/media");
    s.put("/data/media/a.mkv", "film");
    s.link("/srv/media", "/data/media");
    const std::vector<std::string> paths{"/srv/media/a.mkv", "/srv/media", "x.txt", "", "/", "/data/media/a.mkv"};
    CHECK(server_files::resolved(s.where(), paths) ==
          std::vector<std::string>{"/data/media/a.mkv", "/srv/media", "/home/u/x.txt", "", "/", "/data/media/a.mkv"});
    // A download through the link and a delete of its real folder meet in their resolved spellings.
    using server_files::Busy;
    const auto w = s.where();
    const std::vector<std::string> busy_given{"/srv/media/a.mkv"};
    const auto busy_real = server_files::resolved(w, busy_given);
    const std::vector<Busy> busy{{.kind = Busy::Kind::download, .path = busy_given[0]},
                                 {.kind = Busy::Kind::download, .path = busy_real[0]}};
    const std::vector<std::string> deleting{"/data/media"};
    CHECK(server_files::busy_refusal(std::vector<Busy>{busy[0]}, deleting, "Fake", "/home/u") == std::nullopt);
    CHECK(server_files::busy_refusal(busy, deleting, "Fake", "/home/u") ==
          "Fake:/data/media holds /data/media/a.mkv, which is being downloaded: wait until it ends, or cancel it first");
    // No connection: each as given.
    const server_files::Where none{.server = "Gone", .pool = nullptr, .connections = nullptr, .timeout = 30s, .on_command = {}, .home = {}, .on_undo = {}};
    CHECK(server_files::resolved(none, paths) == paths);
}

TEST_CASE("server files: each path REALPATHed whole, a busy link's target covered") {
    Server s;
    s.fake.home = "/home/u";
    s.folder("/home");
    s.folder("/home/u");
    s.folder("/data");
    s.folder("/data/media");
    s.put("/data/media/a.mkv", "film");
    s.put("/data/f.txt", "text");
    s.link("/home/u/media", "/data/media");
    s.link("/home/u/f", "/data/f.txt");
    const auto w = s.where();
    const Snap before = s.snap();
    // A link at the end followed; a name not there yet (an upload's target), "" and "/" as given.
    const std::vector<std::string> paths{"/home/u/media", "f", "/home/u/new.txt", "", "/", "/data/media/a.mkv"};
    CHECK(server_files::followed(w, paths) ==
          std::vector<std::string>{"/data/media", "/data/f.txt", "/home/u/new.txt", "", "/", "/data/media/a.mkv"});
    // A folder download of the link: a delete of a file in its target meets it only through the
    // followed spelling (resolved keeps the link's own name).
    using server_files::Busy;
    const std::vector<std::string> link{"/home/u/media"};
    CHECK(server_files::resolved(w, link) == link);
    std::vector<Busy> busy{{.kind = Busy::Kind::download, .path = link[0]}};
    const std::vector<std::string> deleting{"/data/media/a.mkv"};
    CHECK(server_files::busy_refusal(busy, deleting, "Fake", "/home/u") == std::nullopt);
    busy.push_back(Busy{.kind = Busy::Kind::download, .path = server_files::followed(w, link)[0]});
    const auto refused = server_files::busy_refusal(busy, deleting, "Fake", "/home/u");
    REQUIRE(refused);
    CHECK(refused->contains("being downloaded"));
    // Play of a link to a file: deleting its target is refused the same way.
    const std::vector<Busy> playing{{.kind = Busy::Kind::play, .path = "f"},
                                    {.kind = Busy::Kind::play, .path = server_files::followed(w, std::vector<std::string>{"f"})[0]}};
    CHECK(server_files::busy_refusal(playing, std::vector<std::string>{"/data/f.txt"}, "Fake", "/home/u").has_value());
    CHECK(s.snap() == before);
    // No connection: each as given.
    const server_files::Where none{.server = "Gone", .pool = nullptr, .connections = nullptr, .timeout = 30s, .on_command = {}, .home = {}, .on_undo = {}};
    CHECK(server_files::followed(none, paths) == paths);
}

TEST_CASE("server files: a look counts a folder whose name a file has, apart") {
    Server s;
    s.folder("/src");
    s.folder("/src/sub");
    s.put("/src/sub/in.txt", "in");
    s.put("/src/a.txt", "abc");
    s.folder("/dst");
    s.put("/dst/sub", "file");
    const Snap before = s.snap();
    const std::vector<std::string> picked{"/src/sub", "/src/a.txt"};
    for (const bool copying : {true, false}) {
        CAPTURE(copying);
        const auto l = server_files::look(s.where(), server_files::into(picked, "/dst"), copying);
        REQUIRE(l);
        CHECK(l->taken.count == 1);
        CHECK(l->taken.folders == 0);
        CHECK(l->folders_onto_files.count == 1);
        CHECK(l->folders_onto_files.folders == 0);
        CHECK(l->clashes.size() == 1);
        // A copy counts what Keep both would copy of it too.
        CHECK(l->files == (copying ? 2U : 1U));
        CHECK(l->folders == 1);
        CHECK(l->bytes == (copying ? 5U : 3U));
    }
    CHECK(s.snap() == before);
}

TEST_CASE("server files: what the window marks after a change") {
    using server_files::Mark;
    // A folder moved: its old and new places, their folders, and both forgotten in the previews.
    auto inv = server_files::invalidation("Main", {.from = {"/srv/a/d"}, .to = {"/srv/b/d"}});
    CHECK(inv.list == "Main");
    CHECK(inv.folders == std::vector<Mark>{{.folder = "/srv/a", .below = false},
                                           {.folder = "/srv/a/d", .below = true},
                                           {.folder = "/srv/b", .below = false},
                                           {.folder = "/srv/b/d", .below = true}});
    CHECK(inv.forget == std::vector<std::string>{"/srv/a/d", "/srv/b/d"});
    // A rename in place: one folder, twice changed; a new folder in the login folder.
    inv = server_files::invalidation("Main", {.from = {"x.txt"}, .to = {"y.txt"}});
    CHECK(inv.folders == std::vector<Mark>{{.folder = "", .below = false},
                                           {.folder = "x.txt", .below = true},
                                           {.folder = "y.txt", .below = true}});
    // A file deleted, and a folder holding it.
    inv = server_files::invalidation("Main", {.from = {"/srv/d/f", "/srv/d"}, .to = {}});
    CHECK(inv.folders == std::vector<Mark>{{.folder = "/srv/d", .below = true},
                                           {.folder = "/srv/d/f", .below = true},
                                           {.folder = "/srv", .below = false}});
    // The login folder known: each spelling marked and forgotten.
    inv = server_files::invalidation("Main", {.from = {"x.txt"}, .to = {"/home/u/d/y"}}, "/home/u");
    CHECK(inv.folders == std::vector<Mark>{{.folder = "", .below = false},
                                           {.folder = "x.txt", .below = true},
                                           {.folder = "/home/u", .below = false},
                                           {.folder = "/home/u/x.txt", .below = true},
                                           {.folder = "/home/u/d", .below = false},
                                           {.folder = "/home/u/d/y", .below = true},
                                           {.folder = "d", .below = false},
                                           {.folder = "d/y", .below = true}});
    CHECK(inv.forget == std::vector<std::string>{"x.txt", "/home/u/x.txt", "/home/u/d/y", "d/y"});
}

TEST_CASE("previews: forgetting a path a fetch is running for keeps its old answer out of the cache") {
    preview::Previews previews;
    std::vector<std::string> got;
    auto reply = [&](std::string tag) {
        return [&got, tag](preview::Answer a) { got.push_back(tag + ":" + std::to_string(a.status)); };
    };
    const auto ok = [] {
        return preview::Answer{.status = 200,
                               .content_type = "image/jpeg",
                               .headers = {},
                               .body = std::make_shared<const std::string>("the old photo")};
    };
    auto old = previews.ask({.server = "Main", .path = "/srv/d/photo.jpg", .row = {}, .ahead = false}, reply("old"));
    REQUIRE(old);
    REQUIRE(previews.begin(old));
    auto other = previews.ask({.server = "Main", .path = "/srv/e.jpg", .row = {}, .ahead = false}, reply("other"));
    REQUIRE(other);
    previews.forget("Main", "/srv/d"); // replaced meanwhile
    CHECK(previews.jobs() == 1);       // the other one's job stays
    auto fresh = previews.ask({.server = "Main", .path = "/srv/d/photo.jpg", .row = {}, .ahead = false}, reply("new"));
    REQUIRE(fresh); // a new fetch, not the old one joined
    CHECK(fresh != old);
    previews.end(old, preview::Fetched{.answer = ok(), .stamp = {}, .keep = true});
    CHECK(got == std::vector<std::string>{"old:200"}); // its own request answered
    CHECK(previews.cached_bytes() == 0);
    CHECK(previews.begin(fresh)); // not answered from the old one's answer
    CHECK(previews.jobs() == 2);
}

TEST_CASE("preview cache: forgetting a path drops it and everything below it, nothing else") {
    preview::Cache cache;
    const preview::Stamp stamp{.size = 1, .modified = 1};
    const auto answer = [] { return preview::Answer{.status = 200, .content_type = "text/plain", .headers = {}, .body = {}}; };
    for (const char* p : {"/srv/d", "/srv/d/a.jpg", "/srv/d/sub/b.jpg", "/srv/dx.jpg", "/srv/e.jpg"}) {
        cache.put(preview::key_of("Main", p), answer(), stamp);
    }
    cache.put(preview::key_of("Box", "/srv/d/a.jpg"), answer(), stamp);
    cache.forget("Main", "/srv/d");
    CHECK_FALSE(cache.find(preview::key_of("Main", "/srv/d"), stamp));
    CHECK_FALSE(cache.find(preview::key_of("Main", "/srv/d/a.jpg"), stamp));
    CHECK_FALSE(cache.find(preview::key_of("Main", "/srv/d/sub/b.jpg"), stamp));
    CHECK(cache.find(preview::key_of("Main", "/srv/dx.jpg"), stamp));
    CHECK(cache.find(preview::key_of("Main", "/srv/e.jpg"), stamp));
    CHECK(cache.find(preview::key_of("Box", "/srv/d/a.jpg"), stamp));
    cache.forget("Main", "/");
    CHECK(cache.size() == 1);
}

TEST_CASE("ssh hash: sha256sum's commands and records") {
    const std::vector<std::string> paths{"/h/x", "/h/y"};
    CHECK(ssh::hash_command(paths, true, ssh::Sum::sha256) == "sha256sum -z '/h/x' '/h/y'");
    CHECK(ssh::hash_command(paths, false, ssh::Sum::sha256) == "sha256sum '/h/x' '/h/y'");
    CHECK(ssh::hash_out_limit(paths, true, ssh::Sum::sha256) == 2 * 68 + 8);
    const std::string a(64, 'a');
    const std::string m(32, 'b');
    using Sums = std::vector<std::optional<std::string>>;
    CHECK(ssh::parse_sums_in_order(a + "  /h/x\0"s + a + "  /h/y\0"s, paths, true, ssh::Sum::sha256) == Sums{a, a});
    // An MD5's record is no SHA-256's, and the other way round.
    CHECK(ssh::parse_sums_in_order(m + "  /h/x\0"s, paths, true, ssh::Sum::sha256) == Sums{{}, {}});
    CHECK(ssh::parse_sums_in_order(a + "  /h/x\0"s, paths, true) == Sums{{}, {}});
}
