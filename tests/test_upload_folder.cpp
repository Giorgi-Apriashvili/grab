#include <doctest/doctest.h>

// Folder uploads (upload_folder.hpp; UPLOADS.md 5.3, 5.6, 5.8) on an in-memory server every login
// writes (fake_pool.hpp), and the walk of the local folder they start from (local_walk.hpp).

#include "conn_budget.hpp"
#include "engine.hpp"
#include "fake_md5sum.hpp"
#include "fake_pool.hpp"
#include "fake_sftp.hpp"
#include "fetch.hpp"
#include "folder.hpp"
#include "folder_job.hpp"
#include "local_walk.hpp"
#include "rate.hpp"
#include "rclone_encoding.hpp"
#include "ssh_pool.hpp"
#include "upload.hpp"
#include "upload_existing.hpp"
#include "upload_folder.hpp"
#include "util.hpp"
#include "verify.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
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

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)
#endif

using namespace grab;
namespace fs = std::filesystem;

namespace {

constexpr std::int64_t local_time = 1'700'001'000; // the local files' time, unless a test says

struct Dir {
    fs::path path;
    explicit Dir(std::string_view name) : path(fs::temp_directory_path() / "grab-tests-upload-folder" / name) {
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

void set_mtime(const fs::path& p, std::int64_t unix_seconds) {
    fs::last_write_time(p, std::chrono::clock_cast<std::chrono::file_clock>(std::chrono::sys_seconds{std::chrono::seconds{unix_seconds}}));
}

// A local file `rel` ('/'-separated, UTF-8) below `root` holding `content`.
fs::path put_local(const fs::path& root, std::string_view rel, std::string_view content, std::int64_t mtime = local_time) {
    const fs::path p = local_walk::local_path(root, rclone_enc::from_raw_path(std::string(rel)));
    REQUIRE(util::write_file_atomic(p, content));
    set_mtime(p, mtime);
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
    void put(const std::string& path, std::string content, std::uint32_t mtime = 1700000000) {
        std::scoped_lock lock(fake.store.mutex);
        fake.store.files[path] = std::move(content);
        fake.store.mtimes[path] = mtime;
        const auto slash = path.rfind('/');
        fake.store.dirs[slash == 0 ? "/" : path.substr(0, slash)].push_back(path.substr(slash + 1));
    }
    [[nodiscard]] std::optional<std::string> file(const std::string& path) {
        std::scoped_lock lock(fake.store.mutex);
        const auto it = fake.store.files.find(path);
        if (it == fake.store.files.end()) return std::nullopt;
        return it->second;
    }
    [[nodiscard]] bool is_dir(const std::string& path) {
        std::scoped_lock lock(fake.store.mutex);
        return fake.store.dirs.contains(path);
    }
    [[nodiscard]] std::uint32_t mtime(const std::string& path) {
        std::scoped_lock lock(fake.store.mutex);
        const auto it = fake.store.mtimes.find(path);
        return it == fake.store.mtimes.end() ? 1700000000 : it->second;
    }
    // Every temp of grab's anywhere on the server.
    [[nodiscard]] std::vector<std::string> temps() {
        std::scoped_lock lock(fake.store.mutex);
        std::vector<std::string> out;
        for (const auto& [path, content] : fake.store.files) {
            if (upload::is_temp_name(path.substr(path.rfind('/') + 1))) out.push_back(path);
        }
        return out;
    }
    // Every file and folder there, and what each holds.
    [[nodiscard]] std::map<std::string, std::string> tree() {
        std::scoped_lock lock(fake.store.mutex);
        std::map<std::string, std::string> all = fake.store.files;
        for (const auto& [d, names] : fake.store.dirs) {
            std::vector<std::string> sorted = names;
            std::ranges::sort(sorted);
            all["dir " + d] = util::join(sorted, "/");
        }
        return all;
    }
};

// A folder upload of the local folder `source` to /up/<name> (or `remote`).
struct Up {
    fetch::Connections connections;
    upload::FolderJob job;
    upload::Tree tree;
    std::mutex m;
    std::shared_ptr<ssh::Pool> pool;
    fs::path records;

    Up(Server& server, const fs::path& source, std::string remote = "/up/src", int budget = 4, int at_once = 2) {
        pool = server.fake.pool(budget);
        connections.configure("Fake", budget);
        job.connections = &connections;
        job.files_at_once = [at_once] { return at_once; };
        job.server = "Fake";
        job.id = 7;
        job.source = source;
        job.remote_path = std::move(remote);
        job.pool = pool;
        records = source.parent_path() / (util::path_to_utf8(source.filename()) + "-records");
        job.records_dir = records;
    }
    Up(const Up&) = delete;
    Up& operator=(const Up&) = delete;
    void walk() {
        auto walked = upload::walk(job, tree, m, {});
        REQUIRE_MESSAGE(walked, walked.error_or(""));
    }
    void run(std::stop_token stop = {}) { upload::run(job, tree, m, std::move(stop)); }
    [[nodiscard]] std::size_t index(std::string_view path) const {
        const auto it = std::ranges::find(tree.files, path, &folder::File::path);
        REQUIRE_MESSAGE(it != tree.files.end(), path);
        return static_cast<std::size_t>(it - tree.files.begin());
    }
    [[nodiscard]] const folder::File& file(std::string_view path) const { return tree.files[index(path)]; }
    // The sessions closed: what they were asked is in the fake's records.
    void close() {
        job.pool.reset();
        pool.reset();
    }
};

std::string content_of(std::size_t n, std::size_t seed) {
    std::string s = test::pattern(n + seed);
    return s.substr(seed);
}

#ifdef _WIN32
// A junction at `link` to the folder `target` (mklink /J), as the user makes one.
bool junction(const fs::path& link, const fs::path& target) {
    const std::wstring command = L"cmd /c mklink /J \"" + link.wstring() + L"\" \"" + target.wstring() + L"\" >nul 2>nul";
    return _wsystem(command.c_str()) == 0 && fs::exists(link);
}
#endif

} // namespace

TEST_CASE("local walk: files and folders, empty ones too; links, junctions and grab's partial downloads left out") {
    Dir d("walk");
    const fs::path root = d.path / "src";
    put_local(root, "a.txt", "aaa", local_time);
    put_local(root, "sub/b.txt", "bb");
    put_local(root, ".hidden", "h");
    fs::create_directories(root / "emptydir");
    // A partial download of grab's (its record parses) and something else with those endings.
    put_local(root, "x.mkv.grabpart", "part");
    put_local(root, "x.mkv.grabpart.json", fetch::state_to_json(fetch::new_state("Main", "/x.mkv", 100, "2023-11-14T22:13:20Z", 100)));
    put_local(root, "y.grabpart", "y");
    put_local(root, "y.grabpart.json", "{not grab's}");
    fs::create_directories(d.path / "outside");
    put_local(d.path / "outside", "o.txt", "o");
    std::size_t links = 0;
#ifdef _WIN32
    REQUIRE(junction(root / "into-sub", root / "sub"));
    REQUIRE(junction(root / "sub" / "out", d.path / "outside"));
    links += 2;
    std::error_code ec;
    fs::create_symlink(d.path / "outside" / "o.txt", root / "linked.txt", ec);
    if (!ec) ++links;
    else MESSAGE("no file symlink here (developer mode off): only the junctions are tried");
#endif

    const auto walked = local_walk::walk(root);
    REQUIRE_MESSAGE(walked, walked.error_or(""));
    std::vector<std::string> paths;
    for (const auto& f : walked->files) paths.push_back(f.path);
    CHECK(paths == std::vector<std::string>{".hidden", "a.txt", "sub/b.txt", "y.grabpart", "y.grabpart.json"});
    CHECK(walked->folders == std::vector<std::string>{"emptydir", "sub"});
    CHECK(walked->links == links);
    CHECK(walked->partials == 2);
    CHECK(walked->unreadable.empty());
    const auto& a = walked->files[1];
    CHECK(a.size == 3);
    CHECK(util::parse_rfc3339(a.modtime) == local_time * 1'000'000'000);

#ifdef _WIN32
    // The folder itself as picked: through a junction too.
    REQUIRE(junction(d.path / "picked", root));
    const auto through = local_walk::walk(d.path / "picked");
    REQUIRE(through);
    CHECK(through->files.size() == walked->files.size());
#endif
    // Not a folder, or none.
    const auto file = local_walk::walk(root / "a.txt");
    REQUIRE_FALSE(file);
    CHECK(file.error().ends_with("isn't a folder"));
    const auto none = local_walk::walk(root / "nothing");
    REQUIRE_FALSE(none);
    CHECK(none.error().starts_with("there is no folder"));
}

TEST_CASE("local walk: long and non-ASCII names come back as their bytes, and a name UTF-8 can't hold is said") {
    Dir d("names");
    const fs::path root = d.path / "src";
    const std::string unicode = "\xE1\x83\xA2\xE1\x83\x94\xE1\x83\xA1\xE1\x83\xA2\xE1\x83\x98 \xC3\xBC/\xE5\x90\x8D\xE5\x89\x8D.txt"; // Georgian, ü, 名前
    put_local(root, unicode, "unicode");
    // Past 260 characters: five folders of 60.
    std::string deep;
    for (int i = 0; i < 5; ++i) deep += std::string(60, static_cast<char>('a' + i)) + "/";
    deep += "file.bin";
#ifdef _WIN32
    // Made through "\\?\" whether or not long paths are on here.
    const fs::path extended = fs::path(LR"(\\?\)" + fs::absolute(root).lexically_normal().wstring());
    put_local(extended, deep, "deep");
    // A name Windows holds that isn't valid UTF-16 (a lone surrogate): it can't go to a server.
    const fs::path odd = root / std::wstring(L"bad\xD800.txt");
    const HANDLE h = CreateFileW(odd.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    CloseHandle(h);
#else
    put_local(root, deep, "deep");
#endif
    const auto walked = local_walk::walk(root);
    REQUIRE_MESSAGE(walked, walked.error_or(""));
    std::map<std::string, std::uint64_t> sizes;
    for (const auto& f : walked->files) sizes[rclone_enc::raw_path(f.path)] = f.size;
    CHECK(sizes == std::map<std::string, std::uint64_t>{{deep, 4}, {unicode, 7}});
    if (util::long_paths()) {
        for (const auto& f : walked->files) {
            auto opened = upload::LocalFile::open(local_walk::local_path(root, f.path));
            CHECK_MESSAGE(opened, opened.error_or(""));
        }
    }
#ifdef _WIN32
    REQUIRE(walked->unreadable.size() == 1);
    CHECK(walked->unreadable[0].ends_with(": its name can't be written in UTF-8"));
    std::error_code ec;
    fs::remove(odd, ec);
    fs::remove_all(extended, ec);
#endif
}

TEST_CASE("folder uploads: small files pipelined on each connection, big ones in ranges, every byte once, every folder made") {
    Dir d("mixed");
    const fs::path src = d.path / "src";
    std::map<std::string, std::string> want; // server path -> content
    auto local = [&](const std::string& rel, std::string content) {
        put_local(src, rel, content);
        want["/up/src/" + rel] = std::move(content);
    };
    for (int i = 0; i < 40; ++i) local(std::format("many/s{:02}.bin", i), content_of(static_cast<std::size_t>(i * 1311 + 7), static_cast<std::size_t>(i)));
    local("a.txt", "a small one");
    local("zero.txt", "");
    local("sub/deeper/c.bin", content_of(std::size_t{3} << 20, 3));
    local("big.bin", content_of(std::size_t{5} << 20, 5));
    fs::create_directories(src / "empty");
    std::uint64_t total = 0;
    for (const auto& [path, content] : want) total += content.size();

    Server server;
    Up up(server, src);
    up.walk();
    CHECK(up.tree.files.size() == want.size());
    std::atomic<std::uint64_t> sent{0};
    up.job.on_sent = [&](std::uint64_t n) { sent += n; };
    up.run();
    for (const auto& f : up.tree.files) {
        CHECK_MESSAGE(f.status == folder::State::done, f.path, ": ", f.error);
        CHECK(f.fetched);
        const std::string at = "/up/src/" + f.path;
        CHECK_MESSAGE(server.file(at) == want.at(at), f.path);
        CHECK_MESSAGE(server.mtime(at) == local_time, f.path); // the local file's time
    }
    CHECK(server.is_dir("/up/src/empty"));
    CHECK(server.temps().empty());
    CHECK(up.tree.temps.empty());
    std::vector<std::string> made = up.tree.made;
    std::ranges::sort(made);
    CHECK(made == std::vector<std::string>{"", "empty", "many", "sub", "sub/deeper"});
    CHECK(sent == total); // each acknowledged byte counted once
    up.close();
    std::uint64_t written = 0;
    for (const auto& w : server.fake.writes) written += w.length;
    CHECK(written == total); // and sent once
    CHECK(server.fake.logins <= 4);
    // Small files overlap on a connection: several OPENs before its first RENAME.
    bool overlapped = false;
    for (const auto& [login, requests] : server.fake.requests_by) {
        const auto first_rename = std::ranges::find(requests, "RENAME");
        overlapped = overlapped || std::count(requests.begin(), first_rename, "OPEN") >= 8;
    }
    CHECK(overlapped);
}

TEST_CASE("folder uploads: merged into the folder there, each existing-file choice per file") {
    Dir d("choices");
    const fs::path src = d.path / "src";
    put_local(src, "same.txt", "same");               // as on the server
    put_local(src, "newer.txt", "newer here");        // the server's is older
    put_local(src, "older.txt", "older here");        // the server's is newer
    put_local(src, "diff.txt", "another size");       // the same time, another size
    put_local(src, "dirname", "a file");              // the server has a folder of that name
    put_local(src, "same (2).txt", "a local name");   // Keep both must not take it
    put_local(src, "new.txt", "nothing there");
    auto prepare = [](Server& s) {
        s.folder("/up/src");
        s.put("/up/src/same.txt", "same", local_time);
        s.put("/up/src/newer.txt", "old", local_time - 100);
        s.put("/up/src/older.txt", "new", local_time + 100);
        s.put("/up/src/diff.txt", "short", local_time);
        s.folder("/up/src/dirname");
    };
    const std::string server_name = "Fake";
    struct Want {
        upload::Existing choice;
        std::map<std::string, std::optional<std::string>> files; // what is then there
        std::map<std::string, folder::State> states;
    };
    using S = folder::State;
    const std::vector<Want> cases{
        {upload::Existing::replace,
         {{"same.txt", "same"}, {"newer.txt", "newer here"}, {"older.txt", "older here"}, {"diff.txt", "another size"}},
         {{"same.txt", S::done}, {"newer.txt", S::done}, {"older.txt", S::done}, {"diff.txt", S::done}, {"dirname", S::skipped}}},
        {upload::Existing::different,
         {{"same.txt", "same"}, {"newer.txt", "newer here"}, {"older.txt", "older here"}, {"diff.txt", "another size"}},
         {{"same.txt", S::skipped}, {"newer.txt", S::done}, {"older.txt", S::done}, {"diff.txt", S::done}, {"dirname", S::skipped}}},
        {upload::Existing::newer,
         {{"same.txt", "same"}, {"newer.txt", "newer here"}, {"older.txt", "new"}, {"diff.txt", "short"}},
         {{"same.txt", S::skipped}, {"newer.txt", S::done}, {"older.txt", S::skipped}, {"diff.txt", S::skipped}, {"dirname", S::skipped}}},
        {upload::Existing::skip,
         {{"same.txt", "same"}, {"newer.txt", "old"}, {"older.txt", "new"}, {"diff.txt", "short"}},
         {{"same.txt", S::skipped}, {"newer.txt", S::skipped}, {"older.txt", S::skipped}, {"diff.txt", S::skipped}, {"dirname", S::skipped}}},
        {upload::Existing::keep_both,
         {{"same.txt", "same"}, {"same (3).txt", "same"}, {"newer.txt", "old"}, {"newer (2).txt", "newer here"}, {"dirname (2)", "a file"}},
         {{"same.txt", S::done}, {"newer.txt", S::done}, {"dirname", S::done}}},
        {upload::Existing::ask,
         {{"same.txt", "same"}, {"newer.txt", "old"}},
         {{"same.txt", S::failed}, {"newer.txt", S::failed}, {"older.txt", S::failed}, {"diff.txt", S::failed}, {"dirname", S::failed}}},
    };
    for (const auto& c : cases) {
        CAPTURE(upload::name(c.choice));
        Server server;
        prepare(server);
        Up up(server, src);
        up.job.existing = c.choice;
        up.walk();
        auto looked = upload::look(up.job, up.tree, up.m, {});
        REQUIRE_MESSAGE(looked, looked.error_or(""));
        CHECK(up.tree.merging);
        CHECK(up.tree.clashes.size() == 5);
        {
            std::lock_guard lock(up.m);
            upload::apply(up.job, up.tree);
        }
        up.run();
        for (const auto& [name, content] : c.files) CHECK_MESSAGE(server.file("/up/src/" + name) == content, name);
        for (const auto& [name, state] : c.states) CHECK_MESSAGE(up.file(name).status == state, name, ": ", up.file(name).error);
        CHECK(up.file("new.txt").status == S::done);
        CHECK(up.file("same (2).txt").status == S::done);
        CHECK(server.is_dir("/up/src/dirname")); // a folder is never replaced
        if (c.choice == upload::Existing::replace || c.choice == upload::Existing::newer) {
            CHECK(up.file("dirname").error == "a folder of that name is there");
        }
        if (c.choice == upload::Existing::ask) {
            CHECK(up.tree.asking.size() == 5);
            CHECK(up.file("same.txt").error == "a file of that name is already on " + server_name);
        }
        CHECK(server.temps().empty());
        CHECK(up.tree.made.empty()); // merged: nothing made
    }

    // One file's own choice over the upload's.
    Server server;
    prepare(server);
    Up up(server, src);
    up.job.existing = upload::Existing::skip;
    up.walk();
    up.tree.choices[up.index("diff.txt")] = upload::Existing::replace;
    up.run(); // looks first itself
    CHECK(server.file("/up/src/diff.txt") == "another size");
    CHECK(server.file("/up/src/newer.txt") == "old");
    CHECK(up.file("diff.txt").status == folder::State::done);
    CHECK(up.file("newer.txt").status == folder::State::skipped);
}

TEST_CASE("folder uploads: a file that appears on the server after the look gets the file's choice") {
    Dir d("late");
    const fs::path src = d.path / "src";
    put_local(src, "late.txt", "mine");
    put_local(src, "late-big.bin", content_of(std::size_t{3} << 20, 1));
    put_local(src, "other.txt", "other");
    for (const auto choice : {upload::Existing::skip, upload::Existing::ask, upload::Existing::keep_both, upload::Existing::replace}) {
        CAPTURE(upload::name(choice));
        Server server;
        server.folder("/up/src");
        Up up(server, src);
        up.job.existing = choice;
        up.walk();
        REQUIRE(upload::look(up.job, up.tree, up.m, {}));
        CHECK(up.tree.clashes.empty());
        server.put("/up/src/late.txt", "theirs");
        server.put("/up/src/late-big.bin", "theirs too");
        up.run();
        CHECK(up.file("other.txt").status == folder::State::done);
        switch (choice) {
        case upload::Existing::skip:
            CHECK(up.file("late.txt").status == folder::State::skipped);
            CHECK(up.file("late-big.bin").status == folder::State::skipped);
            CHECK(server.file("/up/src/late.txt") == "theirs");
            break;
        case upload::Existing::ask:
            CHECK(up.file("late.txt").status == folder::State::failed);
            CHECK(up.file("late-big.bin").status == folder::State::failed);
            CHECK(up.tree.asking == std::set<std::size_t>{up.index("late.txt"), up.index("late-big.bin")});
            CHECK(server.file("/up/src/late.txt") == "theirs");
            break;
        case upload::Existing::keep_both:
            CHECK(up.file("late.txt").status == folder::State::done);
            CHECK(server.file("/up/src/late (2).txt") == "mine");
            CHECK(server.file("/up/src/late.txt") == "theirs");
            CHECK(upload::target_of(up.job, up.file("late.txt")) == "/up/src/late (2).txt");
            CHECK(upload::target_of(up.job, up.file("late-big.bin")) == "/up/src/late-big (2).bin");
            break;
        default:
            CHECK(server.file("/up/src/late.txt") == "mine");
            CHECK(server.file("/up/src/late-big.bin") == content_of(std::size_t{3} << 20, 1));
            break;
        }
        CHECK(server.temps().empty());
        CHECK(up.tree.temps.empty());
    }
}

TEST_CASE("folder uploads: files that turn up after the look wait for the answer under Ask; then each goes by its own choice or the one for all") {
    Dir d("late-answer");
    const fs::path src = d.path / "src";
    put_local(src, "a.txt", "mine a");
    put_local(src, "b.txt", "mine b");
    put_local(src, "c.txt", "mine c");
    put_local(src, "big.bin", content_of(std::size_t{3} << 20, 2));
    put_local(src, "other.txt", "other");
    Server server;
    server.folder("/up/src");
    Up up(server, src);
    up.job.existing = upload::Existing::ask; // the setting Ask, and nothing there when the dialog looked
    up.walk();
    REQUIRE(upload::look(up.job, up.tree, up.m, {}));
    CHECK(up.tree.clashes.empty());
    for (const char* name : {"a.txt", "b.txt", "c.txt", "big.bin"}) server.put(std::string("/up/src/") + name, std::string("theirs ") + name);
    up.run();
    CHECK(up.file("other.txt").status == folder::State::done);
    // Nothing of theirs touched: each waits, failed for the question.
    for (const char* name : {"a.txt", "b.txt", "c.txt", "big.bin"}) {
        CAPTURE(name);
        CHECK(server.file(std::string("/up/src/") + name) == std::string("theirs ") + name);
        CHECK(up.file(name).status == folder::State::failed);
        CHECK(up.file(name).error == "a file of that name is already on Fake");
    }
    {
        std::lock_guard lock(up.m);
        upload::Taken taken;
        const auto waiting = upload::asking_list(up.tree, 2, taken);
        CHECK(taken.count == 4);
        CHECK(taken.newer == 4); // the files here are newer than those that turned up
        REQUIRE(waiting.size() == 2); // the first part, in the folder's order
        CHECK(waiting[0].index == up.index("a.txt"));
        CHECK(waiting[1].index == up.index("b.txt"));
        CHECK(waiting[0].clash.remote_size == std::string("theirs a.txt").size());
        CHECK(waiting[0].clash.local_size == 6);
        // One skipped by hand meanwhile no longer waits.
        CHECK(upload::skip_file(up.job, up.tree, up.index("b.txt")) == std::nullopt);
        upload::Taken left;
        CHECK(upload::asking_list(up.tree, 10, left).size() == 3);
        CHECK(left.count == 3);
        // The answer: Skip for all, a.txt replaced, c.txt beside theirs.
        upload::answer(up.job, up.tree, upload::Existing::skip,
                       {{up.index("a.txt"), upload::Existing::replace}, {up.index("c.txt"), upload::Existing::keep_both}});
        CHECK(up.tree.choices.at(up.index("a.txt")) == upload::Existing::replace);
        CHECK(up.tree.choices.at(up.index("c.txt")) == upload::Existing::keep_both);
        CHECK(up.tree.choices.at(up.index("big.bin")) == upload::Existing::skip);
        upload::Taken none;
        CHECK(upload::asking_list(up.tree, 10, none).empty());
        CHECK(up.file("a.txt").status == folder::State::queued);
        CHECK(up.file("big.bin").status == folder::State::skipped);
    }
    up.run();
    CHECK(server.file("/up/src/a.txt") == "mine a");
    CHECK(server.file("/up/src/b.txt") == "theirs b.txt");
    CHECK(server.file("/up/src/c.txt") == "theirs c.txt");
    CHECK(server.file("/up/src/c (2).txt") == "mine c");
    CHECK(server.file("/up/src/big.bin") == "theirs big.bin");
    CHECK(up.file("a.txt").status == folder::State::done);
    CHECK(up.file("b.txt").status == folder::State::skipped);
    CHECK(up.file("c.txt").status == folder::State::done);
    CHECK(up.file("big.bin").status == folder::State::skipped);
    CHECK(up.tree.asking.empty());
    CHECK(server.temps().empty()); // the big file's partial upload went with its Skip

    // Ask for all decides nothing: they keep waiting.
    Server again;
    again.folder("/up/src");
    Up up2(again, src);
    up2.job.existing = upload::Existing::ask;
    up2.walk();
    REQUIRE(upload::look(up2.job, up2.tree, up2.m, {}));
    again.put("/up/src/a.txt", "theirs");
    up2.run();
    std::lock_guard lock(up2.m);
    upload::answer(up2.job, up2.tree, upload::Existing::ask, {});
    CHECK(up2.tree.asking == std::set<std::size_t>{up2.index("a.txt")});
    CHECK(up2.file("a.txt").status == folder::State::failed);
    CHECK(up2.tree.choices.empty());
    // One waiting without what is there known: queued again with its choice, which meets the file
    // there as it is put in place.
    up2.tree.clashes.erase(up2.index("a.txt"));
    upload::answer(up2.job, up2.tree, upload::Existing::keep_both, {});
    CHECK(up2.tree.asking.empty());
    CHECK(up2.file("a.txt").status == folder::State::queued);
    CHECK(up2.tree.choices.at(up2.index("a.txt")) == upload::Existing::keep_both);
}

TEST_CASE("folder uploads: Keep both for a big file asked about at its placement goes on from its complete temp; nothing is left behind") {
    Dir d("late-big-both");
    const fs::path src = d.path / "src";
    const std::string mine = content_of(std::size_t{3} << 20, 4);
    put_local(src, "big.bin", mine);
    put_local(src, "other.txt", "other");
    // answer: the answer to the question, as the window gives it; else a restart, whose look
    // numbers the file before an answer could go on from its temp (run()'s end then removes it).
    for (const bool answered : {true, false}) {
        CAPTURE(answered);
        Server server;
        server.folder("/up/src");
        Up up(server, src);
        up.job.existing = upload::Existing::ask;
        up.walk();
        REQUIRE(upload::look(up.job, up.tree, up.m, {}));
        // Its name taken after every byte went, right before it is put in place.
        up.job.on_command = [&](const std::vector<std::string>& c) {
            if (c.size() == 3 && c[1] == "stat" && c[2].find("/.big.bin.") != std::string::npos) server.put("/up/src/big.bin", "theirs");
        };
        up.run();
        const std::size_t big = up.index("big.bin");
        REQUIRE(up.file("big.bin").status == folder::State::failed);
        CHECK(up.tree.asking.contains(big));
        REQUIRE(upload::saved_progress(up.records, "Fake", "/up/src/big.bin"));
        REQUIRE(server.temps().size() == 1); // complete, kept for the answer
        up.job.on_command = {};
        {
            std::lock_guard lock(up.m);
            if (answered) {
                upload::answer(up.job, up.tree, upload::Existing::keep_both, {});
                CHECK(up.file("big.bin").status == folder::State::queued);
                CHECK(up.file("big.bin").local.empty()); // numbered as it is put in place
                CHECK_FALSE(up.tree.clashes.contains(big));
            } else {
                up.tree.choices[big] = upload::Existing::keep_both;
                up.tree.looked = false;
            }
        }
        std::atomic<std::uint64_t> sent{0};
        up.job.on_sent = [&](std::uint64_t n) { sent += n; };
        up.run();
        CHECK_MESSAGE(up.file("big.bin").status == folder::State::done, up.file("big.bin").error);
        CHECK(server.file("/up/src/big.bin") == "theirs");
        CHECK(server.file("/up/src/big (2).bin") == mine);
        CHECK(upload::target_of(up.job, up.file("big.bin")) == "/up/src/big (2).bin");
        if (answered) CHECK(sent.load() == 0); // nothing sent again
        CHECK(server.temps().empty());
        CHECK_FALSE(upload::saved_progress(up.records, "Fake", "/up/src/big.bin"));
        std::error_code ec;
        CHECK(std::distance(fs::directory_iterator(up.records, ec), fs::directory_iterator()) == 0);
    }
}

TEST_CASE("folder uploads: Keep both at placement passes over the folder's own names, which its files then take") {
    Dir d("late-both-own");
    const fs::path src = d.path / "src";
    put_local(src, "a.txt", "mine a");
    put_local(src, "a (2).txt", "mine a2");
    Server server;
    server.folder("/up/src");
    Up up(server, src);
    up.job.existing = upload::Existing::keep_both;
    up.walk();
    REQUIRE(upload::look(up.job, up.tree, up.m, {}));
    server.put("/up/src/a.txt", "theirs"); // after the look: placement numbers it
    {
        std::lock_guard lock(up.m);
        folder::pause_file(up.tree, up.index("a (2).txt")); // so a.txt goes first
    }
    up.run();
    CHECK(up.file("a.txt").status == folder::State::done);
    CHECK_FALSE(server.file("/up/src/a (2).txt"));
    {
        std::lock_guard lock(up.m);
        folder::resume_paused(up.tree);
    }
    up.run();
    for (const auto& f : up.tree.files) CHECK_MESSAGE(f.status == folder::State::done, f.path, ": ", f.error);
    CHECK(server.file("/up/src/a.txt") == "theirs");
    CHECK(server.file("/up/src/a (2).txt") == "mine a2");
    CHECK(server.file("/up/src/a (3).txt") == "mine a");
}

TEST_CASE("folder uploads: a new dialog's answer for a failed upload of the same folder takes the place of the old one's") {
    Dir d("answer-again");
    const fs::path src = d.path / "src";
    put_local(src, "x.jpg", "mine x");
    put_local(src, "y.jpg", "mine y");
    put_local(src, "z.jpg", "mine z");
    Server server;
    server.folder("/up/src");
    server.put("/up/src/x.jpg", "theirs x");
    server.put("/up/src/y.jpg", "theirs y");
    // The first dialog: x.jpg replaced, Skip for all (y.jpg skipped); the link failed before x.jpg went.
    Up up(server, src);
    up.job.existing = upload::Existing::skip;
    up.walk();
    up.tree.choices[up.index("x.jpg")] = upload::Existing::replace;
    REQUIRE(upload::look(up.job, up.tree, up.m, {}));
    {
        std::lock_guard lock(up.m);
        upload::apply(up.job, up.tree);
        auto& x = up.tree.files[up.index("x.jpg")];
        REQUIRE(x.status == folder::State::queued);
        x.status = folder::State::failed;
        x.error = "the connection was lost";
        REQUIRE(up.file("y.jpg").status == folder::State::skipped);
    }
    // The same folder picked again: the new dialog says Skip for all, y.jpg replaced.
    upload::Tree fresh;
    std::mutex fm;
    REQUIRE(upload::walk(up.job, fresh, fm, {}));
    fresh.choices[up.index("y.jpg")] = upload::Existing::replace;
    {
        std::lock_guard lock(up.m);
        upload::answer_again(up.tree, fresh);
        CHECK(up.tree.choices == std::map<std::size_t, upload::Existing>{{up.index("y.jpg"), upload::Existing::replace}});
        CHECK_FALSE(up.tree.looked);
        folder::retry_files(up.tree, false); // the row's Retry (App::requeue)
    }
    up.job.existing = upload::Existing::skip;
    up.run();
    CHECK(server.file("/up/src/x.jpg") == "theirs x"); // the dialog said nothing is replaced but y.jpg
    CHECK(server.file("/up/src/y.jpg") == "mine y");
    CHECK(server.file("/up/src/z.jpg") == "mine z");
    CHECK(up.file("x.jpg").status == folder::State::skipped);
    CHECK(up.file("y.jpg").status == folder::State::done);
    CHECK(server.temps().empty());
}

TEST_CASE("folder uploads: files' own choices hold through a retry after a cancel, whose look decides again") {
    Dir d("own-retry");
    const fs::path src = d.path / "src";
    put_local(src, "mine.txt", "replace me");
    put_local(src, "theirs.txt", "keep theirs");
    put_local(src, "both.txt", "beside");
    Server server;
    server.folder("/up/src");
    server.put("/up/src/mine.txt", "old");
    server.put("/up/src/theirs.txt", "old");
    server.put("/up/src/both.txt", "old");
    Up up(server, src);
    up.job.existing = upload::Existing::skip; // the dialog's For all
    up.walk();
    up.tree.choices[up.index("mine.txt")] = upload::Existing::replace;
    up.tree.choices[up.index("both.txt")] = upload::Existing::keep_both;
    REQUIRE(upload::look(up.job, up.tree, up.m, {}));
    {
        std::lock_guard lock(up.m);
        upload::apply(up.job, up.tree);
    }
    // Cancelled before anything was sent; Retry queues the rest again and looks at the server again.
    std::stop_source stopped;
    stopped.request_stop();
    up.run(stopped.get_token());
    CHECK(server.file("/up/src/mine.txt") == "old");
    {
        std::lock_guard lock(up.m);
        folder::retry_files(up.tree, true);
        up.tree.looked = false;
    }
    up.run();
    CHECK(server.file("/up/src/mine.txt") == "replace me");
    CHECK(server.file("/up/src/theirs.txt") == "old");
    CHECK(server.file("/up/src/both.txt") == "old");
    CHECK(server.file("/up/src/both (2).txt") == "beside");
    CHECK(up.file("theirs.txt").status == folder::State::skipped);
    CHECK(server.temps().empty());
}

TEST_CASE("folder uploads: a folder this account can't write in fails only what goes below it, saying why") {
    Dir d("readonly");
    const fs::path src = d.path / "src";
    put_local(src, "ok.txt", "fine");
    put_local(src, "ro/a.txt", "a");
    put_local(src, "ro/deep/b.txt", "b");
    put_local(src, "ro/big.bin", content_of(std::size_t{3} << 20, 2));
    put_local(src, "rw/c.txt", "c");
    Server server;
    server.folder("/up/src");
    server.folder("/up/src/ro");
    server.fake.locked_dirs = {"/up/src/ro"};
    const auto before = server.tree();
    Up up(server, src);
    up.walk();
    up.run();
    const std::string why = "Fake doesn't let this account write in /up/src/ro";
    for (const auto* path : {"ro/a.txt", "ro/deep/b.txt", "ro/big.bin"}) {
        CHECK_MESSAGE(up.file(path).status == folder::State::failed, path);
        CHECK_MESSAGE(up.file(path).error == why, path);
    }
    CHECK(up.file("ok.txt").status == folder::State::done);
    CHECK(up.file("rw/c.txt").status == folder::State::done);
    CHECK(server.file("/up/src/rw/c.txt") == "c");
    auto after = server.tree();
    after.erase("/up/src/ok.txt");
    after.erase("/up/src/rw/c.txt");
    after.erase("dir /up/src/rw");
    after["dir /up/src"] = before.at("dir /up/src");
    CHECK(after == before); // nothing else touched, nothing left in ro
    const auto o = folder::outcome(up.tree);
    CHECK(o.failed == 3);
}

TEST_CASE("folder uploads: a full disk fails the files that don't fit, saying so, their temps removed") {
    Dir d("quota");
    const fs::path src = d.path / "src";
    std::map<std::string, std::string> local;
    for (int i = 0; i < 10; ++i) {
        const std::string rel = std::format("f{}.bin", i);
        local[rel] = content_of(50 * 1024, static_cast<std::size_t>(i));
        put_local(src, rel, local[rel]);
    }
    Server server;
    server.fake.quota_bytes = 300 * 1024;
    server.fake.statvfs = true;
    Up up(server, src);
    up.walk();
    up.run();
    std::size_t done = 0;
    std::size_t full = 0;
    for (const auto& f : up.tree.files) {
        if (f.status == folder::State::done) {
            ++done;
            CHECK(server.file("/up/src/" + f.path) == local.at(f.path));
        } else {
            CHECK(f.status == folder::State::failed);
            CHECK_MESSAGE(f.error.starts_with("Fake:/up/src is full: "), f.error);
            if (f.error.ends_with(" needed")) ++full;
        }
    }
    CHECK(done >= 1);
    CHECK(done < 10);
    CHECK(full == 10 - done);
    CHECK(server.temps().empty());
    CHECK(up.tree.temps.empty());
}

TEST_CASE("folder uploads: a stop keeps what was sent and every temp it left is known; the next run finishes") {
    Dir d("stop");
    const fs::path src = d.path / "src";
    std::map<std::string, std::string> local;
    for (int i = 0; i < 60; ++i) {
        const std::string rel = std::format("d{}/f{:02}.bin", i % 3, i);
        local[rel] = content_of(20 * 1024, static_cast<std::size_t>(i));
        put_local(src, rel, local[rel]);
    }
    local["big.bin"] = content_of(std::size_t{4} << 20, 9);
    put_local(src, "big.bin", local["big.bin"]);
    Server server;
    server.fake.latency = std::chrono::milliseconds(2); // replies come a round trip later, so a stop finds files on their way
    Up up(server, src);
    up.walk();
    std::stop_source stop;
    std::atomic<std::uint64_t> acked{0};
    up.job.on_sent = [&](std::uint64_t n) {
        if ((acked += n) >= 300 * 1024) stop.request_stop();
    };
    up.run(stop.get_token());
    std::size_t done = 0;
    for (const auto& f : up.tree.files) {
        CHECK_MESSAGE((f.status == folder::State::done || f.status == folder::State::queued), f.path);
        if (f.status == folder::State::done) ++done;
    }
    CHECK(done < up.tree.files.size());
    // Every temp on the server is one the tree knows of (small files'), or the big file's record's.
    for (const auto& t : server.temps()) {
        const bool known = up.tree.temps.contains(t) || t.starts_with("/up/src/.big.bin.");
        CHECK_MESSAGE(known, t);
    }
    up.job.on_sent = {};
    up.run();
    for (const auto& f : up.tree.files) {
        CHECK_MESSAGE(f.status == folder::State::done, f.path, ": ", f.error);
        CHECK(server.file("/up/src/" + f.path) == local.at(f.path));
    }
    CHECK(server.temps().empty());
    CHECK(up.tree.temps.empty());
    CHECK_FALSE(upload::saved_progress(up.records, "Fake", "/up/src/big.bin"));
}

TEST_CASE("folder uploads: a RENAME the server did as the link broke counts as done, not as a file in the way") {
    Dir d("renamed");
    const fs::path src = d.path / "src";
    for (int i = 0; i < 3; ++i) put_local(src, std::format("f{}.txt", i), std::format("file {}", i));
    Server server;
    server.fake.break_after_rename = true; // the first RENAME is done, then its connection breaks
    Up up(server, src);
    up.job.existing = upload::Existing::ask; // a file of its own taken for someone else's would wait to be asked
    up.walk();
    up.run();
    for (const auto& f : up.tree.files) {
        CHECK_MESSAGE(f.status == folder::State::done, f.path, ": ", f.error);
        CHECK(server.file("/up/src/" + f.path) == std::format("file {}", f.path[1] - '0'));
    }
    CHECK(up.tree.asking.empty());
    CHECK(up.tree.renaming.empty());
    CHECK(server.temps().empty());
    CHECK(server.fake.logins >= 2); // the broken one, and the next
}

TEST_CASE("folder uploads: under a speed limit, small files' WRITEs are as small and as few on their way as it says") {
    Dir d("limit");
    const fs::path src = d.path / "src";
    for (int i = 0; i < 4; ++i) put_local(src, std::format("f{}.bin", i), content_of(64 * 1024, static_cast<std::size_t>(i)));
    Server server;
    Up up(server, src, "/up/src", 1);
    constexpr std::uint64_t rate = 256 * 1024;
    rate::RateLimiter limiter;
    limiter.set_rate(rate);
    up.job.limiter = &limiter;
    up.walk();
    up.run();
    for (const auto& f : up.tree.files) CHECK_MESSAGE(f.status == folder::State::done, f.path, ": ", f.error);
    up.close();
    REQUIRE_FALSE(server.fake.writes.empty());
    for (const auto& w : server.fake.writes) {
        CHECK(w.length <= fetch::limited_read(rate, 1));
        CHECK(w.out_bytes <= fetch::limited_in_flight(rate, 1)); // the client had at least this much on its way
    }
}

TEST_CASE("folder uploads: cancel removes the temps and the empty folders this upload made, and nothing else") {
    Dir d("cancel");
    const fs::path src = d.path / "src";
    for (int i = 0; i < 30; ++i) put_local(src, std::format("d{}/f{:02}.bin", i % 3, i), content_of(30 * 1024, static_cast<std::size_t>(i)));
    put_local(src, "later/x.txt", "never sent");
    put_local(src, "big.bin", content_of(std::size_t{6} << 20, 4));
    fs::create_directories(src / "empty");
    Server server;
    server.folder("/up/src");
    server.folder("/up/src/old"); // the server's own, empty
    server.fake.latency = std::chrono::milliseconds(2);
    Up up(server, src);
    up.walk();
    {
        std::lock_guard lock(up.m);
        CHECK(folder::pause_file(up.tree, up.index("later/x.txt")));
    }
    std::stop_source stop;
    std::atomic<std::uint64_t> acked{0};
    up.job.on_sent = [&](std::uint64_t n) {
        if ((acked += n) >= 400 * 1024) stop.request_stop();
    };
    up.run(stop.get_token());
    std::set<std::string> made(up.tree.made.begin(), up.tree.made.end());
    CHECK(made.contains("later"));
    CHECK_FALSE(made.contains("")); // merged into
    const auto left = upload::discard(up.job, up.tree, up.m);
    CHECK(left.empty());
    CHECK(server.temps().empty());
    CHECK(up.tree.temps.empty());
    CHECK_FALSE(upload::saved_progress(up.records, "Fake", "/up/src/big.bin"));
    CHECK(server.is_dir("/up/src/old")); // not this upload's
    CHECK_FALSE(server.is_dir("/up/src/later"));
    CHECK_FALSE(server.is_dir("/up/src/empty"));
    // A folder it made stays while a file it sent is in it (a RENAME the stop cut short may have
    // got there too), and only then.
    const auto there = server.tree();
    for (const auto& rel : made) {
        if (rel.empty()) continue;
        const std::string prefix = "/up/src/" + rel + "/";
        const bool holds = std::ranges::any_of(there, [&](const auto& e) { return e.first.starts_with(prefix); });
        CHECK_MESSAGE(server.is_dir("/up/src/" + rel) == holds, rel);
    }
    for (const auto& f : up.tree.files) {
        if (f.status == folder::State::done) CHECK(server.file("/up/src/" + f.path).has_value()); // sent files stay
    }
}

TEST_CASE("folder uploads: a cancel's leftovers, cleaned later, go as a cancel's own would; out of reach they stay") {
    Dir d("leftovers");
    const fs::path src = d.path / "src";
    for (int i = 0; i < 30; ++i) {
        put_local(src, std::format("d{}/f{:02}.bin", i % 3, i), content_of(std::size_t{30} * 1024, static_cast<std::size_t>(i)));
    }
    put_local(src, "later/x.txt", "never sent");
    put_local(src, "big.bin", content_of(std::size_t{6} << 20, 4));
    Server server;
    server.fake.latency = std::chrono::milliseconds(2);
    Up up(server, src);
    up.walk();
    {
        std::lock_guard lock(up.m);
        CHECK(folder::pause_file(up.tree, up.index("later/x.txt")));
    }
    std::stop_source stop;
    std::atomic<std::uint64_t> acked{0};
    up.job.on_sent = [&](std::uint64_t n) {
        acked += n;
        if (acked >= std::uint64_t{400} * 1024) stop.request_stop();
    };
    up.run(stop.get_token());
    REQUIRE_FALSE(server.temps().empty());
    // The window's cancel: what is there, without asking the server (no pool), for the cleaner.
    upload::FolderJob offline = up.job;
    offline.pool.reset();
    const auto plan = upload::discard(offline, up.tree, up.m);
    CHECK(plan.temps.size() == up.tree.temps.size());
    CHECK(std::ranges::find(plan.partials, "/up/src/big.bin") != plan.partials.end());
    CHECK(std::ranges::find(plan.folders, "/up/src/later") != plan.folders.end());
    CHECK(std::ranges::find(plan.folders, "/up/src") != plan.folders.end());

    // Out of reach: everything stays for the next try.
    {
        test::FakePool down;
        down.login_fails = budget::Failure::fatal;
        auto pool = down.pool(2);
        const auto still = upload::clean(*pool, up.records, "Fake", plan);
        CHECK(still.temps == plan.temps);
        // A big file that began has a record, which only the server's side can undo.
        const bool recorded = upload::saved_progress(up.records, "Fake", "/up/src/big.bin").has_value();
        CHECK(still.partials == (recorded ? std::vector<std::string>{"/up/src/big.bin"} : std::vector<std::string>{}));
        CHECK(std::set<std::string>(still.folders.begin(), still.folders.end()) ==
              std::set<std::string>(plan.folders.begin(), plan.folders.end()));
    }
    // Someone else's file in a folder the upload made: that folder stays, and is done with.
    server.put("/up/src/d0/theirs.txt", "theirs");
    auto pool = server.fake.pool(2);
    const auto still = upload::clean(*pool, up.records, "Fake", plan);
    CHECK(still.empty());
    CHECK(server.temps().empty());
    CHECK_FALSE(upload::saved_progress(up.records, "Fake", "/up/src/big.bin"));
    CHECK_FALSE(server.is_dir("/up/src/later"));
    CHECK(server.is_dir("/up/src/d0"));
    CHECK(server.file("/up/src/d0/theirs.txt") == "theirs");
    for (const auto& f : up.tree.files) {
        if (f.status == folder::State::done) CHECK(server.file("/up/src/" + f.path).has_value()); // sent files stay
    }
    // Again: nothing left to do, nothing asked to fail.
    CHECK(upload::clean(*pool, up.records, "Fake", plan).empty());
}

TEST_CASE("folder uploads: paused and skipped files stay, a higher priority starts first, At once holds, a retry sends") {
    Dir d("actions");
    const fs::path src = d.path / "src";
    for (int i = 1; i <= 10; ++i) put_local(src, std::format("f{:02}.txt", i), std::format("file {}", i));
    for (int i = 1; i <= 3; ++i) put_local(src, std::format("big{}.bin", i), content_of(std::size_t{2} << 20, static_cast<std::size_t>(i)));
    Server server;
    Up up(server, src, "/up/src", 1, 1); // one connection: one small worker, files in their order
    up.walk();
    const fs::path missing = src / "f09.txt";
    fs::remove(missing); // fails: it can't be read
    std::atomic<int> most_at_once{0};
    up.job.on_event = [&](folder::Event) {
        std::lock_guard lock(up.m);
        most_at_once = std::max(most_at_once.load(), up.tree.files_at_once);
    };
    {
        std::lock_guard lock(up.m);
        CHECK(folder::set_priority(up.tree, up.index("f07.txt"), 4));
        CHECK(folder::pause_file(up.tree, up.index("f03.txt")));
        bool skipped = false;
        CHECK_FALSE(folder::skip_file(up.tree, src, "/up/src", up.index("f05.txt"), &skipped));
        CHECK(skipped);
    }
    up.run();
    CHECK(up.file("f03.txt").status == folder::State::paused);
    CHECK(up.file("f05.txt").status == folder::State::skipped);
    CHECK(up.file("f09.txt").status == folder::State::failed);
    CHECK(up.file("f09.txt").error.starts_with("cannot read "));
    CHECK_FALSE(server.file("/up/src/f03.txt"));
    CHECK_FALSE(server.file("/up/src/f05.txt"));
    CHECK(most_at_once == 1);
    for (const char* name : {"f01.txt", "f07.txt", "f10.txt", "big1.bin", "big3.bin"}) CHECK(up.file(name).status == folder::State::done);
    up.close();
    // The small files' OPENs in order: the high one first.
    std::vector<std::string> opened;
    for (const auto& p : server.fake.opened) {
        const std::string name = p.substr(p.rfind('/') + 1);
        if (upload::is_temp_name(name) && name.find(".txt.") != std::string::npos) opened.push_back(name.substr(1, 7));
    }
    REQUIRE_FALSE(opened.empty());
    CHECK(opened.front() == "f07.txt");

    // Resumed and retried, they go.
    up.pool = server.fake.pool(1);
    up.job.pool = up.pool;
    put_local(src, "f09.txt", "back");
    {
        std::lock_guard lock(up.m);
        CHECK(folder::resume_file(up.tree, up.index("f03.txt")));
        CHECK(folder::retry_file(up.tree, up.index("f09.txt")));
    }
    up.run();
    CHECK(up.file("f03.txt").status == folder::State::done);
    CHECK(server.file("/up/src/f09.txt") == "back");
    CHECK(up.file("f05.txt").status == folder::State::skipped);
}

TEST_CASE("folder uploads: a file where a folder goes, a file at the folder's place, no folder to go into") {
    Dir d("way");
    const fs::path src = d.path / "src";
    put_local(src, "d/inside.txt", "in");
    put_local(src, "d/deeper/more.txt", "more");
    put_local(src, "fine.txt", "fine");
    {
        Server server;
        server.folder("/up/src");
        server.put("/up/src/d", "a file of the server's");
        Up up(server, src);
        up.walk();
        up.run();
        const std::string why = "Fake:/up/src/d is a file, so the folder can't be made there";
        CHECK(up.file("d/inside.txt").error == why);
        CHECK(up.file("d/deeper/more.txt").error == why);
        CHECK(up.file("fine.txt").status == folder::State::done);
        CHECK(server.file("/up/src/d") == "a file of the server's");
    }
    {
        Server server;
        server.put("/up/src", "a file where the folder goes");
        Up up(server, src);
        up.walk();
        const auto looked = upload::look(up.job, up.tree, up.m, {});
        REQUIRE_FALSE(looked);
        CHECK(looked.error() == "Fake:/up/src is a file, so the folder can't go there");
        up.run();
        for (const auto& f : up.tree.files) CHECK(f.error == looked.error());
        CHECK(server.file("/up/src") == "a file where the folder goes");
    }
    {
        Server server;
        Up up(server, src, "/nowhere/src");
        up.walk();
        const auto looked = upload::look(up.job, up.tree, up.m, {});
        REQUIRE_FALSE(looked);
        CHECK(looked.error() == "there is no folder /nowhere on Fake");
    }
}

TEST_CASE("folder uploads: verify fails a file whose server copy differs, keeps the local file, and its retry replaces grab's copy") {
    Dir d("verify");
    const fs::path src = d.path / "src";
    put_local(src, "a.txt", "alpha");
    put_local(src, "b/b.txt", "bravo");
    put_local(src, "c.bin", content_of(std::size_t{3} << 20, 8));
    put_local(src, "gone.txt", "gone soon");
    Server server;
    Up up(server, src);
    up.walk();
    up.run();
    for (const auto& f : up.tree.files) REQUIRE(f.status == folder::State::done);
    // The server's md5sum sees the copies as they are on the server, b.txt's damaged since.
    test::Box box;
    {
        std::scoped_lock lock(server.fake.store.mutex);
        for (const auto& [path, content] : server.fake.store.files) {
            box.files[path] = content;
            box.mtimes[path] = server.fake.store.mtimes.contains(path) ? server.fake.store.mtimes.at(path) : 1700000000;
        }
    }
    box.files["/up/src/b/b.txt"] = "brAvo";
    fs::remove(src / "gone.txt");
    engine::Context ctx;
    ctx.settings.name = "Fake";
    ctx.remote.host = "fake.example";
    verify::Request base;
    base.ctx = &ctx;
    base.pool = box.pool(4, 1);
    const auto checked = upload::verify_files(up.job, up.tree, up.m, base, {});
    CHECK(checked.files == 4);
    CHECK(checked.summary() == "1 of 4 files: the server's copy differs (MD5)");
    const auto& b = up.file("b/b.txt");
    CHECK(b.status == folder::State::failed);
    CHECK(b.error == "the server's copy differs (MD5)");
    CHECK(util::read_file(src / "b" / "b.txt") == "bravo"); // nothing local is ever deleted
    CHECK(up.tree.resend == std::set<std::size_t>{up.index("b/b.txt")});
    CHECK(up.tree.choices.empty()); // the user's choices stay as they were
    CHECK(up.file("a.txt").verified);
    CHECK(up.file("c.bin").verified);
    CHECK_FALSE(up.file("gone.txt").verified);
    CHECK(up.file("gone.txt").status == folder::State::done);
    CHECK(checked.report.files[up.index("gone.txt")].why == "your file is gone");

    // The retry sends it again, over grab's own copy.
    server.put("/up/src/b/b.txt.extra", "x"); // something else beside it stays
    {
        std::lock_guard lock(up.m);
        CHECK(folder::retry_file(up.tree, up.index("b/b.txt")));
    }
    const std::uint64_t before = [&] {
        std::scoped_lock lock(server.fake.mutex);
        return server.fake.writes.size();
    }();
    up.close();
    up.pool = server.fake.pool(4);
    up.job.pool = up.pool;
    up.run();
    CHECK(up.file("b/b.txt").status == folder::State::done);
    CHECK(server.file("/up/src/b/b.txt") == "bravo");
    CHECK(server.file("/up/src/b/b.txt.extra") == "x");
    CHECK(up.tree.resend.empty());
    up.close();
    std::scoped_lock lock(server.fake.mutex);
    CHECK(server.fake.writes.size() > before);
}

TEST_CASE("upload verify: one file whose server copy differs keeps the local file") {
    Dir d("verify-one");
    const fs::path local = put_local(d.path, "one.bin", "the local bytes");
    test::Box box;
    box.files["/srv/one.bin"] = "the LOCAL bytes"; // the same size and time: only the sums tell
    box.mtimes["/srv/one.bin"] = static_cast<std::uint32_t>(local_time);
    box.files["/srv/same.bin"] = "the local bytes";
    box.mtimes["/srv/same.bin"] = static_cast<std::uint32_t>(local_time);
    engine::Context ctx;
    ctx.settings.name = "Fake";
    ctx.remote.host = "fake.example";
    fetch::Connections connections;
    connections.configure("Fake", 2);
    verify::Request base;
    base.ctx = &ctx;
    base.pool = box.pool(2, 1);
    base.connections = &connections;
    base.server = "Fake";
    base.id = 3;
    const std::string modtime = "2023-11-14T22:30:00Z"; // local_time
    const auto differs = upload::verify_file(base, local, "/srv/one.bin", 15, modtime, {});
    REQUIRE(differs.files.size() == 1);
    CHECK(differs.files[0].verdict == verify::Verdict::differs);
    CHECK(verify::summary(differs) == "the server's copy differs (MD5)");
    CHECK(util::read_file(local) == "the local bytes"); // nothing local is ever deleted
    const auto same = upload::verify_file(base, local, "/srv/same.bin", 15, modtime, {});
    CHECK(verify::summary(same) == "checksum verified (MD5)");
    // A file changed here since: not proven, kept.
    set_mtime(local, local_time + 60);
    const auto changed = upload::verify_file(base, local, "/srv/one.bin", 15, modtime, {});
    CHECK(changed.files[0].verdict == verify::Verdict::unchecked);
    CHECK(changed.files[0].why == "your file changed since the upload");
}

TEST_CASE("folder uploads: a copy verify found different goes again over grab's own Keep-both copy, never over the file kept") {
    Dir d("resend");
    const fs::path src = d.path / "src";
    put_local(src, "x.txt", "mine");
    Server server;
    server.folder("/up/src");
    server.put("/up/src/x.txt", "theirs");
    Up up(server, src);
    up.job.existing = upload::Existing::keep_both;
    up.walk();
    up.run();
    const std::size_t x = up.index("x.txt");
    REQUIRE(up.file("x.txt").status == folder::State::done);
    REQUIRE(server.file("/up/src/x (2).txt") == "mine");
    // The server's md5sum sees grab's copy damaged since.
    {
        std::scoped_lock lock(server.fake.store.mutex);
        server.fake.store.files["/up/src/x (2).txt"] = "MINE";
    }
    test::Box box;
    {
        std::scoped_lock lock(server.fake.store.mutex);
        for (const auto& [path, content] : server.fake.store.files) {
            box.files[path] = content;
            box.mtimes[path] = server.fake.store.mtimes.contains(path) ? server.fake.store.mtimes.at(path) : 1700000000;
        }
    }
    engine::Context ctx;
    ctx.settings.name = "Fake";
    ctx.remote.host = "fake.example";
    verify::Request base;
    base.ctx = &ctx;
    base.pool = box.pool(2, 1);
    [[maybe_unused]] const auto checked = upload::verify_files(up.job, up.tree, up.m, base, {});
    REQUIRE(up.file("x.txt").status == folder::State::failed);
    CHECK(up.tree.resend == std::set<std::size_t>{x});
    CHECK(up.tree.choices.empty());
    // Retried, and the choices applied again meanwhile (the window does so when it asks about
    // another file): it is still grab's own copy that goes.
    {
        std::lock_guard lock(up.m);
        CHECK(folder::retry_file(up.tree, x));
        upload::apply(up.job, up.tree);
        CHECK(upload::target_of(up.job, up.tree.files[x]) == "/up/src/x (2).txt");
    }
    up.run();
    CHECK_MESSAGE(up.file("x.txt").status == folder::State::done, up.file("x.txt").error);
    CHECK(server.file("/up/src/x.txt") == "theirs"); // the file the user kept
    CHECK(server.file("/up/src/x (2).txt") == "mine");
    CHECK_FALSE(server.file("/up/src/x (3).txt"));
    CHECK(up.tree.resend.empty());
    CHECK(server.temps().empty());
}

TEST_CASE("folder uploads: restored after a restart, a file whose server copy differed replaces it under Skip") {
    Dir d("resend-restored");
    const fs::path src = d.path / "src";
    put_local(src, "a.txt", "alpha");
    put_local(src, "b.txt", "bravo");
    Server server;
    {
        Up first(server, src);
        first.walk();
        first.run();
        REQUIRE(first.file("b.txt").status == folder::State::done);
    }
    // grab's copy damaged since: verify had failed it (the server's copy differs).
    {
        std::scoped_lock lock(server.fake.store.mutex);
        server.fake.store.files["/up/src/b.txt"] = "brAvo";
    }
    // As the window restores it from uploads.json (App::restored_upload): its files with their
    // states, the flag for b.txt, the server not looked at yet, the upload's choice Skip.
    Up up(server, src);
    up.job.existing = upload::Existing::skip;
    up.walk();
    const std::size_t b = up.index("b.txt");
    {
        std::lock_guard lock(up.m);
        up.tree.files[up.index("a.txt")].status = folder::State::done;
        up.tree.files[b].status = folder::State::failed;
        up.tree.files[b].error = "the server's copy differs (MD5)";
        up.tree.resend.insert(b);
        up.tree.looked = false;
        // Resume: the failed files queued again (App::run_upload_folder), then look and apply.
        folder::retry_files(up.tree, false);
    }
    auto seen = upload::look(up.job, up.tree, up.m, {});
    REQUIRE_MESSAGE(seen, seen.error_or(""));
    {
        std::lock_guard lock(up.m);
        upload::apply(up.job, up.tree);
    }
    up.run();
    CHECK_MESSAGE(up.file("b.txt").status == folder::State::done, up.file("b.txt").error);
    CHECK(server.file("/up/src/b.txt") == "bravo"); // replaced, not "skipped: already there"
    CHECK(up.tree.resend.empty());
    CHECK(server.temps().empty());
}

TEST_CASE("folder uploads: Keep both at placement, the link broken after its RENAME: found under its number, one copy only") {
    Dir d("late-both");
    const fs::path src = d.path / "src";
    put_local(src, "x.txt", "mine");
    Server server;
    server.folder("/up/src");
    Up up(server, src);
    up.job.existing = upload::Existing::keep_both;
    up.walk();
    REQUIRE(upload::look(up.job, up.tree, up.m, {}));
    server.put("/up/src/x.txt", "theirs");   // after the look: upload::place numbers it
    server.fake.break_after_rename = true;   // its RENAME to "x (2).txt" is done, then the link breaks
    up.run();
    CHECK_MESSAGE(up.file("x.txt").status == folder::State::done, up.file("x.txt").error);
    CHECK(server.file("/up/src/x.txt") == "theirs");
    CHECK(server.file("/up/src/x (2).txt") == "mine");
    CHECK_FALSE(server.file("/up/src/x (3).txt"));
    CHECK(upload::target_of(up.job, up.file("x.txt")) == "/up/src/x (2).txt");
    CHECK(up.tree.renaming.empty());
    CHECK(server.temps().empty());
    CHECK(server.fake.logins >= 2);
}

TEST_CASE("folder uploads: Replace without posix-rename whose RENAME then fails keeps the new copy under its temp name, and says so") {
    Dir d("kept");
    const fs::path src = d.path / "src";
    put_local(src, "x.txt", "mine");
    Server server;
    server.folder("/up/src");
    server.put("/up/src/x.txt", "theirs");
    server.fake.posix_rename = false;
    server.fake.rename_refused = {"/up/src/x.txt"};
    Up up(server, src);
    up.job.existing = upload::Existing::replace;
    up.walk();
    up.run();
    const std::size_t x = up.index("x.txt");
    CHECK(up.file("x.txt").status == folder::State::failed);
    REQUIRE(up.tree.kept.size() == 1);
    const std::string temp = up.tree.kept.at(x);
    CHECK(server.file(temp) == "mine"); // the only copy now: kept
    CHECK_MESSAGE(up.file("x.txt").error.find(temp) != std::string::npos, up.file("x.txt").error);
    CHECK_FALSE(server.file("/up/src/x.txt")); // the old one went, as Replace said
    CHECK(server.temps() == std::vector<std::string>{temp});
    CHECK(up.tree.temps.empty());
    // A cancel leaves it, and says so.
    const auto left = upload::discard(up.job, up.tree, up.m);
    CHECK(left.kept == std::vector<std::string>{temp});
    CHECK(server.file(temp) == "mine");
    // Retried where the name can be taken: the file goes, and the kept temp with it.
    up.close();
    server.fake.rename_refused.clear();
    up.pool = server.fake.pool(4);
    up.job.pool = up.pool;
    {
        std::lock_guard lock(up.m);
        CHECK(folder::retry_file(up.tree, x));
    }
    up.run();
    CHECK_MESSAGE(up.file("x.txt").status == folder::State::done, up.file("x.txt").error);
    CHECK(server.file("/up/src/x.txt") == "mine");
    CHECK(server.temps().empty());
    CHECK(up.tree.kept.empty());
}

TEST_CASE("folder uploads: a file skipped while its RENAME is on its way gets no existing-file choice applied") {
    Dir d("skip-renaming");
    const fs::path src = d.path / "src";
    put_local(src, "x.txt", "mine");
    Server server;
    server.folder("/up/src");
    Up up(server, src);
    up.job.existing = upload::Existing::replace;
    up.walk();
    bool once = false;
    bool skipped = false;
    up.job.on_command = [&](const std::vector<std::string>& c) {
        if (c.size() != 4 || c[1] != "rename" || c[3] != "/up/src/x.txt" || std::exchange(once, true)) return;
        server.put("/up/src/x.txt", "theirs"); // so the RENAME on its way fails
        std::lock_guard lock(up.m);
        [[maybe_unused]] const auto partial = upload::skip_file(up.job, up.tree, 0, &skipped);
    };
    up.run();
    CHECK(once);
    CHECK(skipped);
    CHECK(up.file("x.txt").status == folder::State::skipped);
    CHECK(server.file("/up/src/x.txt") == "theirs");
    CHECK(server.temps().empty());
}

TEST_CASE("folder uploads: a server that answers MKDIR of a folder there with permission denied is merged into all the same") {
    Dir d("mkdir-denied");
    const fs::path src = d.path / "src";
    put_local(src, "a.txt", "a");
    put_local(src, "sub/b.txt", "b");
    put_local(src, "late/c.txt", "c");
    put_local(src, "new/d.txt", "d");
    Server server;
    server.folder("/up/src");
    server.folder("/up/src/sub");
    server.fake.mkdir_existing_as = sftp::Code::permission_denied;
    Up up(server, src);
    up.walk();
    REQUIRE(upload::look(up.job, up.tree, up.m, {}));
    {
        std::lock_guard lock(up.m);
        upload::apply(up.job, up.tree);
    }
    server.folder("/up/src/late"); // after the look: its MKDIR is refused, and a STAT finds the folder
    up.run();
    for (const auto& f : up.tree.files) CHECK_MESSAGE(f.status == folder::State::done, f.path, ": ", f.error);
    CHECK(server.file("/up/src/sub/b.txt") == "b");
    CHECK(server.file("/up/src/late/c.txt") == "c");
    CHECK(up.tree.made == std::vector<std::string>{"new"});
    up.close();
    // No MKDIR for what the look saw: the folder itself and sub.
    CHECK(std::ranges::count(server.fake.requests, "MKDIR") == 2);
}

TEST_CASE("folder uploads: a full server fails the files still queued at once, so what is sent stays near the quota") {
    Dir d("full");
    const fs::path src = d.path / "src";
    constexpr std::size_t files = 300;
    constexpr std::size_t each = 32 * 1024;
    for (std::size_t i = 0; i < files; ++i) put_local(src, std::format("f{:03}.bin", i), content_of(each, i));
    Server server;
    constexpr std::uint64_t quota = 256 * 1024;
    server.fake.quota_bytes = quota;
    server.fake.statvfs = true;
    Up up(server, src, "/up/src", 1, 1);
    up.walk();
    up.run();
    std::size_t done = 0;
    for (const auto& f : up.tree.files) {
        if (f.status == folder::State::done) {
            ++done;
            continue;
        }
        CHECK(f.status == folder::State::failed);
        CHECK_MESSAGE(f.error.starts_with("Fake:/up/src is full: "), f.path, ": ", f.error);
    }
    CHECK(done >= 1);
    CHECK(done * each <= quota);
    up.close();
    std::uint64_t written = 0;
    for (const auto& w : server.fake.writes) written += w.length;
    // What one connection has on its way (32 files) at most past the quota, not the folder's 9 MiB.
    CHECK(written <= quota + 32 * each);
    CHECK(server.temps().empty());
}

TEST_CASE("folder uploads: a big file paused mid-way, then skipped: its partial upload and its record go") {
    Dir d("skip-big");
    const fs::path src = d.path / "src";
    put_local(src, "big.bin", content_of(std::size_t{6} << 20, 3));
    put_local(src, "a.txt", "a");
    Server server;
    Up up(server, src);
    rate::RateLimiter limiter;
    limiter.set_rate(std::uint64_t{2} << 20); // slow enough to pause it on its way
    up.job.limiter = &limiter;
    up.walk();
    const std::size_t big = up.index("big.bin");
    std::atomic<std::uint64_t> acked{0};
    std::atomic<bool> paused{false};
    up.job.on_sent = [&](std::uint64_t n) {
        if ((acked += n) < 256 * 1024 || paused.exchange(true)) return;
        std::lock_guard lock(up.m);
        folder::pause_file(up.tree, big);
    };
    up.run();
    REQUIRE(up.file("big.bin").status == folder::State::paused);
    REQUIRE(upload::saved_progress(up.records, "Fake", "/up/src/big.bin"));
    REQUIRE_FALSE(server.temps().empty());
    std::optional<std::string> partial;
    {
        std::lock_guard lock(up.m);
        bool skipped = false;
        partial = upload::skip_file(up.job, up.tree, big, &skipped);
        CHECK(skipped);
    }
    CHECK(partial == "/up/src/big.bin");
    // Not removed by the caller: the next run's end removes it all the same.
    up.job.on_sent = {};
    up.run();
    CHECK(up.file("big.bin").status == folder::State::skipped);
    CHECK(up.file("a.txt").status == folder::State::done);
    CHECK(server.temps().empty());
    CHECK_FALSE(server.file("/up/src/big.bin"));
    CHECK_FALSE(upload::saved_progress(up.records, "Fake", "/up/src/big.bin"));
}

#ifdef _WIN32
TEST_CASE("local walk: a name ending in a dot or a space is said and left out, never read as the name without it") {
    Dir d("dots");
    const fs::path root = d.path / "src";
    put_local(root, "a", "plain a");
    put_local(root, "d/other.txt", "other");
    // Made through "\\?\", as WSL or a Samba copy leaves them.
    const fs::path extended = fs::path(LR"(\\?\)" + fs::absolute(root).lexically_normal().wstring());
    auto make = [](const fs::path& p, std::string_view content) {
        const HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(h != INVALID_HANDLE_VALUE);
        DWORD wrote = 0;
        CHECK(WriteFile(h, content.data(), static_cast<DWORD>(content.size()), &wrote, nullptr));
        CloseHandle(h);
    };
    make(extended / L"a.", "dotted");
    make(extended / L"b ", "spaced");
    REQUIRE(CreateDirectoryW((extended / L"d.").c_str(), nullptr));
    make(extended / L"d." / L"inside.txt", "inside");
    const auto walked = local_walk::walk(root);
    REQUIRE_MESSAGE(walked, walked.error_or(""));
    std::vector<std::string> paths;
    for (const auto& f : walked->files) paths.push_back(f.path);
    CHECK(paths == std::vector<std::string>{"a", "d/other.txt"});
    CHECK(walked->folders == std::vector<std::string>{"d"});
    REQUIRE(walked->unreadable.size() == 3);
    for (const auto& u : walked->unreadable) {
        CHECK_MESSAGE(u.ends_with(": its name ends with a dot or a space, which Windows programs can't open"), u);
    }
    std::error_code ec;
    fs::remove_all(extended, ec);
}
#endif
