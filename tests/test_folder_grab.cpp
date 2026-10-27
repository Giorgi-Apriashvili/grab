#include <doctest/doctest.h>

#include "config.hpp"
#include "engine.hpp"
#include "fake_sftp.hpp"
#include "fetch.hpp"
#include "folder.hpp"
#include "folder_job.hpp"
#include "local_name.hpp"
#include "rate.hpp"
#include "rclone_encoding.hpp"
#include "sftp.hpp"
#include "ssh_pool.hpp"
#include "ssh_walk.hpp"
#include "util.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace grab;
namespace fs = std::filesystem;

namespace {

// Pooled sessions on in-memory SFTP servers sharing one tree (as test_fetch_grab.cpp).
struct Tree {
    std::map<std::string, std::string> files;
    std::map<std::string, std::vector<std::string>> dirs;
    std::map<std::string, std::string> links;
    std::map<std::string, std::uint64_t> readdir_size;
    std::set<std::string> denied;
    std::mutex mutex;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> reads;
    std::size_t most_waiting = 0; // most requests a session's server answered at once
    std::atomic<int> logins{0};
    std::atomic<int> break_first_after{0}; // the next login's stream breaks on that read
    std::size_t readdir_batch = 3;          // names per READDIR answer

    struct Session : ssh::Session {
        explicit Session(Tree& t) : tree(t), client(server) {
            server.shared_files = &t.files;
            server.dirs = t.dirs;
            server.links = t.links;
            server.readdir_size = t.readdir_size;
            server.denied = t.denied;
            server.readdir_batch = t.readdir_batch;
            client.start();
            server.broken_after = t.break_first_after.exchange(0);
        }
        ~Session() override {
            std::lock_guard lock(tree.mutex);
            tree.reads.insert(tree.reads.end(), server.reads.begin(), server.reads.end());
            tree.most_waiting = std::max(tree.most_waiting, server.most_waiting);
        }
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
        sftp::Client& sftp() override { return client; }
        bool reusable() override { return client.pending() == 0; }
        Tree& tree;
        test::FakeServer server;
        sftp::Client client;
    };

    std::shared_ptr<ssh::Pool> pool(int limit) {
        auto o = ssh::Pools::download_options(limit);
        o.idle_timer = false;
        return std::make_shared<ssh::Pool>([this] {
            ++logins;
            return std::make_unique<Session>(*this);
        }, o);
    }
    void file(const std::string& path, std::string content) {
        const auto slash = path.rfind('/');
        dirs[path.substr(0, slash)].push_back(path.substr(slash + 1));
        files[path] = std::move(content);
    }
    void dir(const std::string& path) {
        const auto slash = path.rfind('/');
        dirs[path.substr(0, slash)].push_back(path.substr(slash + 1));
        dirs[path];
    }
    std::uint64_t asked() {
        std::lock_guard lock(mutex);
        std::uint64_t n = 0;
        for (auto [off, len] : reads) n += len;
        return n;
    }
};

struct Run {
    engine::Context ctx;
    fetch::Connections connections;
    folder::Job job;
    folder::Tree tree;
    std::mutex m;
    fs::path dest;

    Run(std::shared_ptr<ssh::Pool> pool, std::string_view name, int budget = 4, int at_once = 2) {
        dest = fs::temp_directory_path() / "grab-tests-folder-grab" / name;
        fs::remove_all(dest);
        fs::create_directories(dest);
        ctx.settings.name = "Fake";
        ctx.settings.rclone_remote = "Fake";
        connections.configure("Fake", budget);
        job.ctx = &ctx;
        job.connections = &connections;
        job.server = "Fake";
        job.id = 7;
        job.remote_path = "/data";
        job.dest = dest;
        job.files_at_once = [at_once] { return at_once; };
        job.pool = std::move(pool);
    }
    ~Run() {
        std::error_code ec;
        fs::remove_all(dest, ec);
    }
    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;
    fs::path local(const std::string& rel) const { return dest / "data" / local::relative(rel); }
};

} // namespace

TEST_CASE("grab engine folders: small and big files over all connections, each byte asked for once") {
    Tree t;
    t.dir("/data");
    t.dir("/data/sub");
    t.dir("/data/sub/deeper");
    std::uint64_t total = 0;
    for (int i = 0; i < 40; ++i) {
        const auto content = test::pattern(static_cast<std::size_t>(i * 977 + (i % 3 == 0 ? 0 : 5000)));
        t.file(std::format("/data/{}/s{:02}.bin", i % 2 ? "sub" : "sub/deeper", i), content);
        total += content.size();
    }
    for (const auto& [name, size] : std::vector<std::pair<std::string, std::size_t>>{
             {"/data/big1.bin", 5u << 20}, {"/data/big2.bin", 3u << 20}, {"/data/sub/big3.bin", 7u << 20}}) {
        t.file(name, test::pattern(size));
        total += size;
    }
    t.file("/data/empty.txt", "");
    t.file("/data/caf\xE9.txt", "latin-1 name"); // not UTF-8
    t.file("/data/x\x01y.txt", "control character");
    total += 12 + 17;

    auto pool = t.pool(4);
    Run r(pool, "mixed");
    { auto listed = folder::list(r.job, r.tree, r.m, {}); REQUIRE_MESSAGE(listed, listed.error_or("")); }
    CHECK(r.tree.files.size() == 46);
    CHECK(r.tree.unreadable.empty());
    // Files from 2 MiB up take fetch::run (At once); the rest go beside them.
    for (const auto& f : r.tree.files) CHECK_MESSAGE(f.big == (f.size >= folder::small_limit), f.path);
    folder::run(r.job, r.tree, r.m, {});
    for (const auto& f : r.tree.files) {
        CHECK_MESSAGE(f.status == folder::State::done, f.path, ": ", f.error);
        const std::string server = "/data/" + rclone_enc::raw_path(f.path);
        CHECK_MESSAGE(util::read_file(r.local(f.path)) == t.files.at(server), f.path);
        CHECK_MESSAGE(fetch::up_to_date(r.local(f.path), f.size, f.modtime), f.path); // the server's time
    }
    // The byte quoted, as rclone names it on Windows.
    CHECK(fs::exists(r.dest / "data" / util::path_from_utf8("caf‛E9.txt")));
    r.job.pool.reset();
    pool.reset();
    CHECK(t.asked() == total); // no byte twice, none past an end
    CHECK(t.logins <= 4);
}

TEST_CASE("grab engine folders: a folder the server won't list is said, and the rest comes") {
    Tree t;
    t.dir("/data");
    t.dir("/data/open");
    t.dir("/data/closed");
    t.file("/data/open/a.txt", "a");
    t.file("/data/closed/b.txt", "b");
    t.file("/data/c.txt", "c");
    t.denied.insert("/data/closed");
    auto pool = t.pool(2);
    Run r(pool, "unreadable");
    { auto listed = folder::list(r.job, r.tree, r.m, {}); REQUIRE_MESSAGE(listed, listed.error_or("")); }
    REQUIRE(r.tree.unreadable.size() == 1);
    CHECK(r.tree.unreadable[0] == "closed: permission denied");
    CHECK(r.tree.files.size() == 2);
    folder::run(r.job, r.tree, r.m, {});
    CHECK(util::read_file(r.local("open/a.txt")) == "a");
    CHECK(util::read_file(r.local("c.txt")) == "c");

    SUBCASE("the folder itself unreadable is the error") {
        t.denied.insert("/data");
        auto pool2 = t.pool(2);
        Run r2(pool2, "unreadable-root");
        auto listed = folder::list(r2.job, r2.tree, r2.m, {});
        REQUIRE_FALSE(listed);
        CHECK(listed.error() == "cannot list the folder: cannot list /data: permission denied");
    }
}

TEST_CASE("grab engine folders: links are followed, but never into a loop or twice") {
    Tree t;
    t.dir("/data");
    t.dir("/elsewhere");
    t.file("/data/real.txt", "real");
    t.file("/elsewhere/far.txt", "far");
    t.dirs["/data"].push_back("to-file");
    t.links["/data/to-file"] = "/data/real.txt";
    t.dirs["/data"].push_back("to-elsewhere");
    t.links["/data/to-elsewhere"] = "/elsewhere";
    t.dirs["/data"].push_back("again");
    t.links["/data/again"] = "/elsewhere"; // a second link to the same folder: walked once
    t.dirs["/data"].push_back("loop");
    t.links["/data/loop"] = "/data"; // back to the root
    t.dirs["/data"].push_back("broken");
    t.links["/data/broken"] = "/nowhere";
    auto pool = t.pool(2);
    Run r(pool, "links");
    { auto listed = folder::list(r.job, r.tree, r.m, {}); REQUIRE_MESSAGE(listed, listed.error_or("")); }
    std::vector<std::string> paths;
    for (const auto& f : r.tree.files) paths.push_back(f.path);
    const bool via_first = paths == std::vector<std::string>{"again/far.txt", "real.txt", "to-file"} ||
                           paths == std::vector<std::string>{"real.txt", "to-elsewhere/far.txt", "to-file"};
    CHECK_MESSAGE(via_first, paths.size());
    folder::run(r.job, r.tree, r.m, {});
    CHECK(util::read_file(r.local("to-file")) == "real");
}

TEST_CASE("grab engine folders: a walk with more workers than the pool has connections finishes") {
    // Once, a worker kept its connection while it waited for work, and another holding a folder
    // waited for a connection: the walk never ended.
    Tree t;
    t.dir("/data");
    for (int i = 0; i < 12; ++i) {
        t.dir(std::format("/data/d{}", i));
        t.dir(std::format("/data/d{}/e", i));
        t.file(std::format("/data/d{}/e/f.txt", i), "x");
    }
    auto pool = t.pool(1);
    auto walked = ssh::walk(*pool, "/data", ssh::WalkOptions{.connections = 6}, {});
    REQUIRE_MESSAGE(walked, walked.error_or(""));
    CHECK(walked->entries.size() == 12);
}

TEST_CASE("grab engine folders: a file grown since the listing comes whole") {
    Tree t;
    t.dir("/data");
    const std::string content = test::pattern(300'000);
    t.file("/data/grew.log", content);
    t.readdir_size["/data/grew.log"] = 100'000; // the listing still says 100 KB
    auto pool = t.pool(1);
    Run r(pool, "grew");
    { auto listed = folder::list(r.job, r.tree, r.m, {}); REQUIRE_MESSAGE(listed, listed.error_or("")); }
    REQUIRE(r.tree.files.size() == 1);
    CHECK(r.tree.files[0].size == 100'000);
    folder::run(r.job, r.tree, r.m, {});
    CHECK(r.tree.files[0].status == folder::State::done);
    CHECK(util::read_file(r.local("grew.log")) == content);
}

TEST_CASE("grab engine folders: under a speed limit too") {
    Tree t;
    t.dir("/data");
    for (int i = 0; i < 12; ++i) t.file(std::format("/data/f{:02}.bin", i), test::pattern(50'000 + i));
    t.file("/data/big.bin", test::pattern(3u << 20));
    auto pool = t.pool(3);
    Run r(pool, "limit", 3);
    rate::RateLimiter limiter;
    limiter.set_rate(8u << 20);
    r.job.limiter = &limiter;
    { auto listed = folder::list(r.job, r.tree, r.m, {}); REQUIRE_MESSAGE(listed, listed.error_or("")); }
    folder::run(r.job, r.tree, r.m, {});
    for (const auto& f : r.tree.files) {
        CHECK_MESSAGE(f.status == folder::State::done, f.path, ": ", f.error);
        CHECK(util::read_file(r.local(f.path)) == t.files.at("/data/" + f.path));
    }
}

namespace {

std::vector<std::string> paths_of(const ssh::Walk& w) {
    std::vector<std::string> paths;
    for (const auto& e : w.entries) paths.push_back(e.path);
    return paths;
}

} // namespace

TEST_CASE("ssh walk: depth, folders and Unix times, as a search's walk wants them") {
    Tree t;
    t.dir("/data");
    t.dir("/data/a");
    t.dir("/data/a/b");
    t.dir("/data/a/b/c");
    t.file("/data/top.txt", "1");
    t.file("/data/a/one.txt", "22");
    t.file("/data/a/b/two.txt", "333");
    t.file("/data/a/b/c/deep.txt", "4444");
    auto pool = t.pool(2);
    ssh::WalkOptions o{.connections = 2, .folders = true, .max_depth = 2};
    auto w = ssh::walk(*pool, "/data", o, {});
    REQUIRE_MESSAGE(w, w.error_or(""));
    CHECK(paths_of(*w) == std::vector<std::string>{"a", "a/b", "a/one.txt", "top.txt"});
    CHECK(w->entries[0].dir);
    CHECK(w->entries[2] == ssh::Walked{"a/one.txt", 2, "2023-11-14T22:13:20Z", false, 1700000000});
    o.max_depth = 1;
    w = ssh::walk(*pool, "/data", o, {});
    REQUIRE_MESSAGE(w, w.error_or(""));
    CHECK(paths_of(*w) == std::vector<std::string>{"a", "top.txt"});
    o.max_depth = 0; // all of it
    w = ssh::walk(*pool, "/data", o, {});
    REQUIRE_MESSAGE(w, w.error_or(""));
    CHECK(w->entries.size() == 7);
}

TEST_CASE("ssh walk: dot-names are neither listed nor entered when hidden ones are skipped") {
    Tree t;
    t.dir("/data");
    t.dir("/data/.git");
    t.file("/data/.git/config", "x");
    t.file("/data/.env", "x");
    t.file("/data/keep.txt", "x");
    auto pool = t.pool(1);
    std::vector<std::size_t> progress;
    ssh::WalkOptions o{.connections = 1, .folders = true, .skip_hidden = true};
    o.progress = [&](std::size_t n) { progress.push_back(n); };
    auto w = ssh::walk(*pool, "/data", o, {});
    REQUIRE_MESSAGE(w, w.error_or(""));
    CHECK(paths_of(*w) == std::vector<std::string>{"keep.txt"});
    REQUIRE_FALSE(progress.empty());
    CHECK(progress.back() == 3); // the names read: .git wasn't entered
    // A root that is a dot-name itself is read all the same.
    w = ssh::walk(*pool, "/data/.git", o, {});
    REQUIRE_MESSAGE(w, w.error_or(""));
    CHECK(paths_of(*w) == std::vector<std::string>{"config"});
    o.skip_hidden = false;
    progress.clear();
    w = ssh::walk(*pool, "/data", o, {});
    REQUIRE_MESSAGE(w, w.error_or(""));
    CHECK(paths_of(*w) == std::vector<std::string>{".env", ".git", ".git/config", "keep.txt"});
    CHECK(progress.back() == 4);
}

TEST_CASE("ssh walk: past its entry cap it stops with an error") {
    Tree t;
    t.dir("/data");
    t.dir("/data/sub");
    for (int i = 0; i < 20; ++i) t.file(std::format("/data/sub/f{:02}", i), "x");
    auto pool = t.pool(2);
    auto w = ssh::walk(*pool, "/data", ssh::WalkOptions{.connections = 2, .max_entries = 10}, {});
    REQUIRE_FALSE(w);
    CHECK(w.error() == "listing too large: more than 10 entries");
    w = ssh::walk(*pool, "/data", ssh::WalkOptions{.connections = 2, .max_entries = 21}, {});
    REQUIRE_MESSAGE(w, w.error_or(""));
    CHECK(w->entries.size() == 20);
    // Bytes too: "sub" and 20 of "sub/fNN" are 143 (review G168).
    w = ssh::walk(*pool, "/data", ssh::WalkOptions{.connections = 2, .max_bytes = 100}, {});
    REQUIRE_FALSE(w);
    CHECK(w.error() == "listing too large: more than 100 B of paths");
    w = ssh::walk(*pool, "/data", ssh::WalkOptions{.connections = 2, .max_bytes = 143}, {});
    REQUIRE_MESSAGE(w, w.error_or(""));
    CHECK(w->entries.size() == 20);
}

TEST_CASE("ssh walk: several roots; a failing one is a note, only all failing an error") {
    Tree t;
    t.dir("/data");
    t.file("/data/x.txt", "x");
    t.dir("/more");
    t.file("/more/y.txt", "y");
    t.dir("/locked");
    t.denied.insert("/locked");
    t.dirs["."] = {"home.txt"}; // the login folder
    t.files["./home.txt"] = "h";
    auto pool = t.pool(3);
    const std::vector<std::string> roots{"/data", "/missing", "/locked", "/more", ""};
    auto w = ssh::walk_roots(*pool, roots, ssh::WalkOptions{.connections = 3}, {});
    REQUIRE_MESSAGE(w, w.error_or(""));
    CHECK(paths_of(*w) == std::vector<std::string>{"/data/x.txt", "/more/y.txt", "home.txt"});
    CHECK(w->unreadable ==
          std::vector<std::string>{"/locked: permission denied", "/missing: no such file or folder"});
    const std::vector<std::string> bad{"/missing", "/locked"};
    w = ssh::walk_roots(*pool, bad, ssh::WalkOptions{.connections = 3}, {});
    REQUIRE_FALSE(w);
    CHECK(w.error() == "cannot list /locked: permission denied; /missing: no such file or folder");
    // One root alone (a folder download): its failure is the error.
    w = ssh::walk(*pool, "/missing", {}, {});
    REQUIRE_FALSE(w);
    CHECK(w.error() == "cannot list /missing: no such file or folder");
}

TEST_CASE("ssh walk: several folders in flight on a connection, CLOSE not waited for, the connection kept") {
    struct Seen {
        std::size_t most_waiting = 0; // requests the server answered at once
        int logins = 0;
    };
    auto walked = [](int in_flight) {
        Tree t;
        t.readdir_batch = 100; // every folder in one answer: no READDIRs ahead
        t.dir("/data");
        for (int i = 0; i < 24; ++i) {
            t.dir(std::format("/data/d{:02}", i));
            t.file(std::format("/data/d{:02}/f.txt", i), "x");
        }
        auto pool = t.pool(1);
        ssh::WalkOptions o{.connections = 1, .in_flight = in_flight};
        auto w = ssh::walk(*pool, "/data", o, {});
        REQUIRE_MESSAGE(w, w.error_or(""));
        CHECK(w->entries.size() == 24);
        CHECK(pool->idle() == 1); // its replies all in, so it's reused
        w = ssh::walk(*pool, "/data", o, {});
        REQUIRE_MESSAGE(w, w.error_or(""));
        pool.reset();
        return Seen{.most_waiting = t.most_waiting, .logins = t.logins};
    };
    const Seen one = walked(1);
    const Seen many = walked(16);
    CHECK(one.logins == 1);
    CHECK(many.logins == 1);
    CHECK(one.most_waiting <= 3);   // a folder at a time (with the last one's CLOSE, the root's REALPATH)
    CHECK(many.most_waiting >= 16); // OPENDIRs and READDIRs of 16 folders together
}

TEST_CASE("ssh walk: a big folder has READDIRs in flight ahead, one more for each answer, up to 8") {
    Tree t;
    t.dir("/data");
    for (int i = 0; i < 60; ++i) t.file(std::format("/data/f{:02}", i), "x"); // 20 answers of 3
    auto pool = t.pool(1);
    auto w = ssh::walk(*pool, "/data", ssh::WalkOptions{.connections = 1, .in_flight = 1}, {});
    REQUIRE_MESSAGE(w, w.error_or(""));
    CHECK(w->entries.size() == 60);
    CHECK(pool->idle() == 1); // the CLOSE went out after the last READDIR, and came back
    pool.reset();
    CHECK(t.most_waiting == 8);
}

TEST_CASE("ssh walk: a connection that breaks mid-walk loses nothing: its folders are read again") {
    Tree t;
    t.dir("/data");
    for (int i = 0; i < 30; ++i) {
        t.dir(std::format("/data/d{:02}", i));
        t.file(std::format("/data/d{:02}/f.txt", i), "x");
    }
    t.break_first_after = 3;
    auto pool = t.pool(2);
    auto w = ssh::walk(*pool, "/data", ssh::WalkOptions{.connections = 2}, {});
    REQUIRE_MESSAGE(w, w.error_or(""));
    CHECK(w->entries.size() == 30);
    CHECK(w->unreadable.empty());
    CHECK(t.logins >= 2);
}

TEST_CASE("ssh walk: no entry without a name to save it under (G15); odd names in grab's form") {
    Tree t;
    t.dir("/data");
    t.dirs["/data"].insert(t.dirs["/data"].end(), {".", "..", "", "a/b"});
    t.files["/data/"] = "";    // the empty name, a file as far as the server says
    t.files["/data/a/b"] = "x"; // a name with a '/' in it
    t.file("/data/caf\xE9.txt", "x");
    auto pool = t.pool(1);
    auto w = ssh::walk(*pool, "/data", ssh::WalkOptions{.connections = 1, .folders = true}, {});
    REQUIRE_MESSAGE(w, w.error_or(""));
    CHECK(paths_of(*w) == std::vector<std::string>{rclone_enc::from_raw_name("a/b"), "caf‛E9.txt"});
}
