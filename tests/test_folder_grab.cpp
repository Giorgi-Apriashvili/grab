#include <doctest/doctest.h>

#include "config.hpp"
#include "engine.hpp"
#include "fake_sftp.hpp"
#include "fetch.hpp"
#include "folder.hpp"
#include "folder_job.hpp"
#include "local_name.hpp"
#include "queue_view.hpp"
#include "rate.hpp"
#include "rclone_encoding.hpp"
#include "sftp.hpp"
#include "ssh_pool.hpp"
#include "ssh_walk.hpp"
#include "util.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
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
#include <thread>
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
    std::uint64_t most_read_bytes = 0; // most bytes of READs a session's server answered at once
    std::atomic<int> logins{0};
    std::atomic<int> break_first_after{0}; // the next login's stream breaks on that read
    std::size_t readdir_batch = 3;          // names per READDIR answer
    std::atomic<int> sftp_fails{0};         // the next this many logins can't start SFTP
    std::atomic<bool> no_sftp_server{false}; // the server refuses SFTP on every login
    std::chrono::milliseconds delay{0};     // every session's pause before each read, as a network's
    std::size_t slice = 0;                  // every session's most bytes per read (0: all there are)
    std::chrono::milliseconds login_delay{0}; // each login takes this long
    // Every OPEN and every READ (its path and length) as the sessions got them, when they did.
    using Clock = std::chrono::steady_clock;
    std::vector<std::pair<std::string, Clock::time_point>> opens;
    struct Asked {
        std::string path;
        std::uint32_t length = 0;
        Clock::time_point at;
    };
    std::vector<Asked> asks;

    struct Session : ssh::Session {
        explicit Session(Tree& t) : tree(t), no_sftp(t.sftp_fails-- > 0), client(server) {
            server.shared_files = &t.files;
            server.dirs = t.dirs;
            server.links = t.links;
            server.readdir_size = t.readdir_size;
            server.denied = t.denied;
            server.readdir_batch = t.readdir_batch;
            server.delay = t.delay;
            server.slice = t.slice;
            server.on_open = [&t](const std::string& path) {
                std::lock_guard lock(t.mutex);
                t.opens.emplace_back(path, Clock::now());
            };
            server.on_read = [&t](const std::string& path, std::uint32_t length) {
                std::lock_guard lock(t.mutex);
                t.asks.push_back(Asked{path, length, Clock::now()});
            };
            client.start();
            server.broken_after = t.break_first_after.exchange(0);
        }
        ~Session() override {
            std::lock_guard lock(tree.mutex);
            tree.reads.insert(tree.reads.end(), server.reads.begin(), server.reads.end());
            tree.most_waiting = std::max(tree.most_waiting, server.most_waiting);
            tree.most_read_bytes = std::max(tree.most_read_bytes, server.most_read_bytes);
        }
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
        // As the real session's SFTP, started on first use, fails to start on a broken link.
        sftp::Client& sftp() override {
            if (tree.no_sftp_server) {
                no_sftp = true;
                throw sftp::Unavailable("the server doesn't offer SFTP");
            }
            if (no_sftp) throw sftp::Broken("SFTP didn't start: the server closed the SFTP channel");
            return client;
        }
        bool reusable() override { return !no_sftp && client.pending() == 0; }
        Tree& tree;
        bool no_sftp;
        test::FakeServer server;
        sftp::Client client;
    };

    std::shared_ptr<ssh::Pool> pool(int limit) {
        auto o = ssh::Pools::download_options(limit);
        o.idle_timer = false;
        return std::make_shared<ssh::Pool>([this] {
            ++logins;
            if (login_delay.count() > 0) std::this_thread::sleep_for(login_delay);
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
    // The files' names in the order they were first opened, of those under `prefix`.
    std::vector<std::string> started(std::string_view prefix) {
        std::lock_guard lock(mutex);
        std::vector<std::string> out;
        for (const auto& [path, at] : opens) {
            if (!path.starts_with(prefix)) continue;
            auto name = path.substr(path.rfind('/') + 1);
            if (std::ranges::find(out, name) == out.end()) out.push_back(std::move(name));
        }
        return out;
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
    // A listed file's index by its path in the folder, under the lock or before run().
    std::size_t index(std::string_view path) const {
        const auto it = std::ranges::find(tree.files, path, &folder::File::path);
        REQUIRE_MESSAGE(it != tree.files.end(), path);
        return static_cast<std::size_t>(it - tree.files.begin());
    }
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
    std::atomic<std::uint64_t> received{0}; // every READ's reply, small files' and big files'
    r.job.on_received = [&](std::uint64_t n) { received += n; };
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
    CHECK(received == total);  // and each counted once as it came, for the window's speeds
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
    // The tree keeps the size fetched, which verification proves against, not the listing's.
    CHECK(r.tree.files[0].size == 300'000);

    // A big file too (fetched alone, resumable).
    Tree big;
    big.dir("/data");
    big.file("/data/grew.log", content);
    big.readdir_size["/data/grew.log"] = 100'000;
    Run b(big.pool(1), "grew-big");
    { auto listed = folder::list(b.job, b.tree, b.m, {}); REQUIRE_MESSAGE(listed, listed.error_or("")); }
    REQUIRE(b.tree.files.size() == 1);
    b.tree.files[0].big = true;
    folder::run(b.job, b.tree, b.m, {});
    CHECK(b.tree.files[0].status == folder::State::done);
    CHECK(util::read_file(b.local("grew.log")) == content);
    CHECK(b.tree.files[0].size == 300'000);
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

TEST_CASE("grab engine folders: small files keep Play's depth while it is urgent on the server") {
    // Bytes of READs in flight on the one connection, at most, with Play urgent or not.
    auto peak = [](bool urgent) {
        Tree t;
        t.dir("/data");
        for (int i = 0; i < 6; ++i) t.file(std::format("/data/f{}.bin", i), test::pattern(6 * sftp::max_read + i));
        auto pool = t.pool(1);
        Run r(pool, urgent ? "urgent" : "not-urgent", 1, 1);
        if (urgent) r.connections.urgent("Fake", -100, 4);
        { auto listed = folder::list(r.job, r.tree, r.m, {}); REQUIRE_MESSAGE(listed, listed.error_or("")); }
        CHECK(std::ranges::none_of(r.tree.files, [](const folder::File& f) { return f.big; }));
        folder::run(r.job, r.tree, r.m, {});
        for (const auto& f : r.tree.files) {
            CHECK_MESSAGE(f.status == folder::State::done, f.path, ": ", f.error);
            CHECK(util::read_file(r.local(f.path)) == t.files.at("/data/" + f.path));
        }
        r.job.pool.reset();
        pool.reset(); // the sessions close and say what they answered
        return t.most_read_bytes;
    };
    CHECK(peak(true) <= 4 * std::uint64_t{sftp::max_read});
    CHECK(peak(false) > 8 * std::uint64_t{sftp::max_read});
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

TEST_CASE("ssh walk: a connection whose SFTP doesn't start takes no folders; the others read them") {
    Tree t;
    t.dir("/data");
    for (int i = 0; i < 30; ++i) {
        t.dir(std::format("/data/d{:02}", i));
        t.file(std::format("/data/d{:02}/f.txt", i), "x");
    }
    t.sftp_fails = 1;
    auto pool = t.pool(2);
    auto w = ssh::walk(*pool, "/data", ssh::WalkOptions{.connections = 2}, {});
    REQUIRE_MESSAGE(w, w.error_or(""));
    CHECK(w->entries.size() == 30);
    CHECK(w->unreadable.empty());
    CHECK(t.logins >= 2);
}

TEST_CASE("grab engine folders: a connection whose SFTP doesn't start is dropped; the small files come on others") {
    Tree t;
    t.dir("/data");
    for (int i = 0; i < 12; ++i) t.file(std::format("/data/s{:02}.txt", i), test::pattern(1000 + i));
    Run r(t.pool(4), "no-sftp");
    { auto listed = folder::list(r.job, r.tree, r.m, {}); REQUIRE_MESSAGE(listed, listed.error_or("")); }
    // New logins for the files, the first two of which can't start SFTP.
    t.sftp_fails = 2;
    r.job.pool = t.pool(4);
    folder::run(r.job, r.tree, r.m, {});
    for (const auto& f : r.tree.files) {
        CHECK_MESSAGE(f.status == folder::State::done, f.path, ": ", f.error);
        CHECK_MESSAGE(util::read_file(r.local(f.path)) == t.files.at("/data/" + f.path), f.path);
    }
    CHECK(t.sftp_fails <= 0); // both were tried
}

TEST_CASE("ssh walk: a server without SFTP ends the walk at once, one login per connection") {
    Tree t;
    t.dir("/data");
    for (int i = 0; i < 30; ++i) t.dir(std::format("/data/d{:02}", i));
    t.no_sftp_server = true;
    auto pool = t.pool(4);
    auto w = ssh::walk(*pool, "/data", ssh::WalkOptions{.connections = 4}, {});
    REQUIRE_FALSE(w);
    CHECK(w.error().find("doesn't offer SFTP") != std::string::npos);
    CHECK(t.logins <= 4);
}

TEST_CASE("grab engine folders: a server without SFTP fails the small files at once, one login per worker") {
    Tree t;
    t.dir("/data");
    for (int i = 0; i < 12; ++i) t.file(std::format("/data/s{:02}.txt", i), test::pattern(1000 + i));
    Run r(t.pool(4), "no-sftp-server");
    { auto listed = folder::list(r.job, r.tree, r.m, {}); REQUIRE_MESSAGE(listed, listed.error_or("")); }
    // As if the list came by find: the files' logins are the first to try SFTP.
    t.no_sftp_server = true;
    t.logins = 0;
    r.job.pool = t.pool(4);
    const auto started = std::chrono::steady_clock::now();
    folder::run(r.job, r.tree, r.m, {});
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2)); // no 2 s retry pauses
    for (const auto& f : r.tree.files) {
        CHECK_MESSAGE(f.status == folder::State::failed, f.path);
        CHECK_MESSAGE(f.error.find("doesn't offer SFTP") != std::string::npos, f.path, ": ", f.error);
    }
    CHECK(t.logins <= 4);
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

// ---- a file's priority in its folder (DOWNLOADS-UI.md 7 and 14, D) -------------------------------

TEST_CASE("grab engine folders: High files start first, then Normal, then Low, of the small and of the big") {
    Tree t;
    t.dir("/data");
    for (int i = 0; i < 12; ++i) t.file(std::format("/data/s{:02}.bin", i), test::pattern(1000 + i));
    for (int i = 0; i < 6; ++i) t.file(std::format("/data/t{:02}.bin", i), test::pattern((2u << 20) + i));
    // One connection and one big file at a time: each kind's files start one after another.
    Run r(t.pool(1), "priority-order", 1, 1);
    { auto listed = folder::list(r.job, r.tree, r.m, {}); REQUIRE_MESSAGE(listed, listed.error_or("")); }
    // High on late files, Low on early ones.
    for (const char* p : {"s00.bin", "s01.bin", "s02.bin", "t00.bin", "t01.bin"}) REQUIRE(folder::set_priority(r.tree, r.index(p), 1));
    for (const char* p : {"s09.bin", "s10.bin", "s11.bin", "t04.bin", "t05.bin"}) REQUIRE(folder::set_priority(r.tree, r.index(p), 4));
    folder::run(r.job, r.tree, r.m, {});
    for (const auto& f : r.tree.files) CHECK_MESSAGE(f.status == folder::State::done, f.path, ": ", f.error);
    CHECK(t.started("/data/s") == std::vector<std::string>{"s09.bin", "s10.bin", "s11.bin", "s03.bin", "s04.bin", "s05.bin",
                                                           "s06.bin", "s07.bin", "s08.bin", "s00.bin", "s01.bin", "s02.bin"});
    CHECK(t.started("/data/t") == std::vector<std::string>{"t04.bin", "t05.bin", "t02.bin", "t03.bin", "t00.bin", "t01.bin"});
}

TEST_CASE("grab engine folders: small files don't wait for High big files waiting for At once") {
    Tree t;
    t.dir("/data");
    for (int i = 0; i < 500; ++i) t.file(std::format("/data/s{:03}.bin", i), test::pattern(2000 + i % 7));
    for (int i = 0; i < 3; ++i) t.file(std::format("/data/t{}.bin", i), test::pattern((3u << 20) + i));
    Run r(t.pool(4), "small-beside-high", 4, 1);
    { auto listed = folder::list(r.job, r.tree, r.m, {}); REQUIRE_MESSAGE(listed, listed.error_or("")); }
    std::vector<std::size_t> big;
    for (int i = 0; i < 3; ++i) {
        big.push_back(r.index(std::format("t{}.bin", i)));
        REQUIRE(folder::set_priority(r.tree, big.back(), 4));
    }
    std::optional<Tree::Clock::time_point> first_big_done;
    r.job.on_event = [&](folder::Event) {
        std::lock_guard lock(r.m);
        if (!first_big_done &&
            std::ranges::any_of(big, [&](std::size_t k) { return r.tree.files[k].status == folder::State::done; })) {
            first_big_done = Tree::Clock::now();
        }
    };
    folder::run(r.job, r.tree, r.m, {});
    for (const auto& f : r.tree.files) CHECK_MESSAGE(f.status == folder::State::done, f.path, ": ", f.error);
    REQUIRE(first_big_done.has_value());
    std::optional<Tree::Clock::time_point> first_small;
    {
        std::lock_guard lock(t.mutex);
        const auto it = std::ranges::find_if(t.opens, [](const auto& o) { return o.first.starts_with("/data/s"); });
        if (it != t.opens.end()) first_small = it->second;
    }
    REQUIRE(first_small.has_value());
    // The small files started at once, not once the High ones had their places.
    CHECK(*first_small < *first_big_done);
}

TEST_CASE("grab engine folders: a priority changed mid-run moves the files still queued") {
    Tree t;
    t.dir("/data");
    for (int i = 0; i < 80; ++i) t.file(std::format("/data/s{:02}.bin", i), test::pattern(3000 + i));
    for (int i = 0; i < 4; ++i) t.file(std::format("/data/t{}.bin", i), test::pattern((2u << 20) + 4096 * i));
    // One connection and one big file at a time, as above.
    Run r(t.pool(1), "priority-mid-run", 1, 1);
    { auto listed = folder::list(r.job, r.tree, r.m, {}); REQUIRE_MESSAGE(listed, listed.error_or("")); }
    const auto s40 = r.index("s40.bin");
    const auto s79 = r.index("s79.bin");
    const auto t0 = r.index("t0.bin");
    const auto t3 = r.index("t3.bin");
    std::size_t opens_then = 0;
    bool small_moved = false;
    bool big_moved = false;
    r.job.on_event = [&](folder::Event) {
        std::lock_guard lock(r.m);
        // The first small file landed, 31 more on their way: the last one goes first now, s40 last.
        if (!small_moved && std::ranges::any_of(r.tree.files, [](const folder::File& f) {
                return !f.big && f.status == folder::State::done;
            })) {
            small_moved = true;
            folder::set_priority(r.tree, s79, 4);
            folder::set_priority(r.tree, s40, 1);
            std::lock_guard opens(t.mutex);
            opens_then = t.opens.size();
        }
        // The first big file started: the last one comes next.
        if (!big_moved && r.tree.files[t0].status == folder::State::running) {
            big_moved = true;
            folder::set_priority(r.tree, t3, 4);
        }
    };
    folder::run(r.job, r.tree, r.m, {});
    for (const auto& f : r.tree.files) CHECK_MESSAGE(f.status == folder::State::done, f.path, ": ", f.error);
    REQUIRE(small_moved);
    REQUIRE(big_moved);
    {
        std::lock_guard lock(t.mutex);
        REQUIRE(opens_then < t.opens.size());
        CHECK(t.opens[opens_then].first == "/data/s79.bin");
    }
    const auto small_order = t.started("/data/s");
    REQUIRE(small_order.size() == 80);
    CHECK(small_order.back() == "s40.bin");
    CHECK(t.started("/data/t") == std::vector<std::string>{"t0.bin", "t3.bin", "t1.bin", "t2.bin"});
}

TEST_CASE("grab engine folders: files paused and resumed, failed and retried, or stopped mid-run all come") {
    // Each queues a file again after the next file's cursors went past it (Tree::requeued).
    Tree t;
    t.dir("/data");
    for (int i = 0; i < 80; ++i) t.file(std::format("/data/s{:02}.bin", i), test::pattern(3000 + i));
    for (int i = 0; i < 4; ++i) t.file(std::format("/data/t{}.bin", i), test::pattern((2u << 20) + 4096 * i));
    Run r(t.pool(2), "lose-none", 2, 1);
    { auto listed = folder::list(r.job, r.tree, r.m, {}); REQUIRE_MESSAGE(listed, listed.error_or("")); }
    std::vector<std::size_t> held; // paused, then resumed
    for (int i = 75; i < 80; ++i) held.push_back(r.index(std::format("s{}.bin", i)));
    held.push_back(r.index("t3.bin"));
    const auto failing = r.index("s74.bin");
    std::stop_source stop;
    int stage = 0;
    r.job.on_event = [&](folder::Event) {
        std::lock_guard lock(r.m);
        const auto done = std::ranges::count_if(r.tree.files, [](const folder::File& f) {
            return !f.big && f.status == folder::State::done;
        });
        if (stage == 0 && done >= 3) {
            stage = 1;
            for (const auto k : held) folder::pause_file(r.tree, k);
            auto& f = r.tree.files[failing]; // as if it failed
            if (f.status == folder::State::queued) {
                f.status = folder::State::failed;
                f.error = "made up";
            }
        } else if (stage == 1 && done >= 40) {
            stage = 2;
            for (const auto k : held) folder::resume_file(r.tree, k);
            folder::retry_file(r.tree, failing);
        } else if (stage == 2 && done >= 60) {
            stage = 3;
            stop.request_stop(); // the folder's own stop
        }
    };
    folder::run(r.job, r.tree, r.m, stop.get_token());
    CHECK(stage == 3);
    {
        std::lock_guard lock(r.m);
        CHECK(std::ranges::any_of(r.tree.files, [](const folder::File& f) { return f.status == folder::State::queued; }));
        CHECK(std::ranges::none_of(r.tree.files, [](const folder::File& f) {
            return f.status == folder::State::running || f.status == folder::State::paused || f.status == folder::State::failed;
        }));
    }
    r.job.on_event = {};
    folder::run(r.job, r.tree, r.m, {});
    for (const auto& f : r.tree.files) {
        CHECK_MESSAGE(f.status == folder::State::done, f.path, ": ", f.error);
        CHECK_MESSAGE(util::read_file(r.local(f.path)) == t.files.at("/data/" + f.path), f.path);
    }
}

TEST_CASE("grab engine folders: running big files split the folder's connections by priority") {
    Tree t;
    t.dir("/data");
    t.file("/data/high.bin", test::pattern(16u << 20));
    t.file("/data/low.bin", test::pattern(16u << 20));
    // Every connection as fast as the others, and slower than the test's threads.
    t.delay = std::chrono::milliseconds(2);
    t.slice = std::size_t{64} * 1024;
    Run r(t.pool(5), "connection-part", 5, 2);
    // Play's depth of 1 for good (no speed limit, which keeps a quarter second of it on its way
    // since chunk J): each stream has one READ on its way at a time, so what the streams asked for
    // follows what came.
    r.connections.urgent("Fake", -100, 1);
    { auto listed = folder::list(r.job, r.tree, r.m, {}); REQUIRE_MESSAGE(listed, listed.error_or("")); }
    REQUIRE(folder::set_priority(r.tree, r.index("high.bin"), 4));
    REQUIRE(folder::set_priority(r.tree, r.index("low.bin"), 1));
    folder::run(r.job, r.tree, r.m, {});
    for (const auto& f : r.tree.files) {
        CHECK_MESSAGE(f.status == folder::State::done, f.path, ": ", f.error);
        CHECK_MESSAGE(util::read_file(r.local(f.path)) == t.files.at("/data/" + f.path), f.path);
    }
    // What Low had asked for when High asked for its last bytes: 4 of the 5 connections were High's.
    std::uint64_t high = 0;
    std::uint64_t low = 0;
    std::uint64_t low_then = 0;
    {
        std::lock_guard lock(t.mutex);
        for (const auto& a : t.asks) {
            if (a.path == "/data/high.bin") {
                high += a.length;
                low_then = low;
            } else if (a.path == "/data/low.bin") {
                low += a.length;
            }
        }
    }
    REQUIRE(low_then > 0);
    const double ratio = static_cast<double>(high) / static_cast<double>(low_then);
    MESSAGE("High : Low = ", ratio);
    CHECK(ratio > 4.0 * 0.85);
    CHECK(ratio < 4.0 * 1.15);
}

namespace {

// Bytes of READs asked for under `prefix` from 0.5 s to 3.5 s after `start`: each right after its
// tokens, past the start.
std::uint64_t asked_between(Tree& from, std::string_view prefix, Tree::Clock::time_point start) {
    std::lock_guard lock(from.mutex);
    std::uint64_t n = 0;
    for (const auto& a : from.asks) {
        const auto at = a.at - start;
        if (a.path.starts_with(prefix) && at >= std::chrono::milliseconds(500) && at < std::chrono::milliseconds(3500)) {
            n += a.length;
        }
    }
    return n;
}

// Runs `r`'s listed normal folder (on `t`) for 3.5 s under 4 MiB/s, beside a normal single
// download of `other`'s /x/single.bin from server `server` ("Other", budget 4, or the folder's
// own); says the folder's share of what both asked for.
double share_beside(Tree& t, Run& r, Tree& other, const std::string& server) {
    rate::RateLimiter limiter;
    limiter.set_rate(4u << 20);
    r.job.limiter = &limiter;
    if (server == "Other") r.connections.configure("Other", 4);
    fetch::Download d;
    d.source = fetch::Source{.rclone_remote = server, .path = "/x/single.bin"};
    d.server = server;
    d.id = 8;
    d.target = r.dest / "single.bin";
    d.limiter = &limiter;
    d.pool = other.pool(4);
    std::stop_source stop;
    auto single = fetch::Outcome::done;
    const auto start = Tree::Clock::now();
    {
        std::jthread folder_run([&] { folder::run(r.job, r.tree, r.m, stop.get_token()); });
        std::jthread single_run([&] { single = fetch::run(d, r.connections, {}, stop.get_token()).outcome; });
        std::this_thread::sleep_for(std::chrono::milliseconds(3500));
        stop.request_stop();
    }
    r.job.limiter = nullptr;
    CHECK(single == fetch::Outcome::stopped);
    const auto folder_bytes = asked_between(t, "/data/", start);
    const auto single_bytes = asked_between(other, "/x/", start);
    REQUIRE(folder_bytes + single_bytes > 0);
    const double share = static_cast<double>(folder_bytes) / static_cast<double>(folder_bytes + single_bytes);
    MESSAGE("folder ", folder_bytes, " B, single ", single_bytes, " B: the folder's share ", share);
    return share;
}

} // namespace

TEST_CASE("grab engine folders: under a speed limit a folder's files share its part, half beside another download") {
    // A normal folder with a High and a Low big file and small files running at once, beside a
    // normal single download from another server, under 4 MiB/s.
    Tree t;
    t.dir("/data");
    t.file("/data/high.bin", test::pattern(8u << 20));
    t.file("/data/low.bin", test::pattern(8u << 20));
    for (int i = 0; i < 4; ++i) t.file(std::format("/data/s{}.bin", i), test::pattern((2u << 20) - 4096 - i));
    Tree other;
    other.dir("/x");
    other.file("/x/single.bin", test::pattern(16u << 20));
    Run r(t.pool(8), "limit-share", 8, 2);
    { auto listed = folder::list(r.job, r.tree, r.m, {}); REQUIRE_MESSAGE(listed, listed.error_or("")); }
    REQUIRE(folder::set_priority(r.tree, r.index("high.bin"), 4));
    REQUIRE(folder::set_priority(r.tree, r.index("low.bin"), 1));
    const auto start = Tree::Clock::now();
    const double share = share_beside(t, r, other, "Other");
    CHECK(share > 0.45);
    CHECK(share < 0.55);
    // Its small and big files all came meanwhile, the High one faster than the Low one.
    const auto high = asked_between(t, "/data/high.bin", start);
    const auto low = asked_between(t, "/data/low.bin", start);
    CHECK(asked_between(t, "/data/s", start) > 0);
    CHECK(low > 0);
    CHECK(high >= 2 * low);
}

TEST_CASE("grab engine folders: files waiting for a connection don't lower the folder's share of a speed limit") {
    // Running big files whose streams wait for a connection ask the limiter nothing: the files
    // that read hold the folder's whole share, half beside a normal single download.
    Tree other;
    other.dir("/x");
    other.file("/x/single.bin", test::pattern(16u << 20));
    SUBCASE("3 big files, 2 connections: the single download from the same server takes the others") {
        Tree t;
        t.dir("/data");
        for (int i = 0; i < 3; ++i) t.file(std::format("/data/b{}.bin", i), test::pattern((8u << 20) + 4096 * i));
        Run r(t.pool(4), "limit-waiting-same", 4, 3);
        { auto listed = folder::list(r.job, r.tree, r.m, {}); REQUIRE_MESSAGE(listed, listed.error_or("")); }
        REQUIRE(folder::set_priority(r.tree, r.index("b0.bin"), 4));
        REQUIRE(folder::set_priority(r.tree, r.index("b2.bin"), 1));
        const auto start = Tree::Clock::now();
        const double share = share_beside(t, r, other, "Fake");
        CHECK(share > 0.45);
        CHECK(share < 0.55);
        // The two connections are High's and Normal's: Low waits rather than read while High does.
        const auto high = asked_between(t, "/data/b0.bin", start);
        const auto low = asked_between(t, "/data/b2.bin", start);
        MESSAGE("High ", high, " B, Low ", low, " B");
        CHECK(high > 4 * low);
    }
    SUBCASE("many small files hold the connections; the big files wait") {
        Tree t;
        t.dir("/data");
        for (int i = 0; i < 100; ++i) t.file(std::format("/data/s{:03}.bin", i), test::pattern((200u << 10) + i));
        for (int i = 0; i < 2; ++i) t.file(std::format("/data/b{}.bin", i), test::pattern((8u << 20) + 4096 * i));
        Run r(t.pool(4), "limit-waiting-small", 4, 2);
        { auto listed = folder::list(r.job, r.tree, r.m, {}); REQUIRE_MESSAGE(listed, listed.error_or("")); }
        const double share = share_beside(t, r, other, "Other");
        CHECK(share > 0.45);
        CHECK(share < 0.55);
    }
    SUBCASE("At once 8 on 4 connections") {
        Tree t;
        t.dir("/data");
        for (int i = 0; i < 8; ++i) t.file(std::format("/data/b{}.bin", i), test::pattern((4u << 20) + 4096 * i));
        Run r(t.pool(4), "limit-waiting-8", 4, 8);
        { auto listed = folder::list(r.job, r.tree, r.m, {}); REQUIRE_MESSAGE(listed, listed.error_or("")); }
        int waiting_seen = 0; // running big files shown waiting, at most at once
        r.job.on_event = [&](folder::Event) {
            std::lock_guard lock(r.m);
            const auto n = std::ranges::count_if(r.tree.files, [](const folder::File& f) {
                return f.status == folder::State::running && f.waiting;
            });
            waiting_seen = std::max(waiting_seen, static_cast<int>(n));
        };
        const double share = share_beside(t, r, other, "Other");
        CHECK(share > 0.45);
        CHECK(share < 0.55);
        CHECK(waiting_seen >= 4); // 8 running, 4 connections
    }
}

TEST_CASE("grab engine folders: a big file waits before its first read, never once all its bytes are in") {
    // Its last stream ending before the file was put in place left it waiting until the progress
    // loop's next tick (up to a quarter second): the window showed Queued just before Done.
    // Watched from beside: a wait after reading that lasts several looks is that.
    Tree t;
    t.dir("/data");
    for (int i = 0; i < 4; ++i) t.file(std::format("/data/b{}.bin", i), test::pattern((3u << 20) + 4096 * i));
    t.delay = std::chrono::milliseconds(1); // reading takes long enough to be seen
    t.slice = std::size_t{64} * 1024;
    Run r(t.pool(2), "no-flash", 2, 1);
    { auto listed = folder::list(r.job, r.tree, r.m, {}); REQUIRE_MESSAGE(listed, listed.error_or("")); }
    struct Seen {
        bool waited_first = false; // waiting as it started, before any read
        bool read = false;
        int waits = 0;   // looks in a row finding it waiting since it read
        int longest = 0; // the most of them
    };
    std::vector<Seen> seen(r.tree.files.size());
    r.job.on_event = [&](folder::Event) {
        std::lock_guard lock(r.m);
        for (std::size_t k = 0; k < r.tree.files.size(); ++k) {
            const auto& f = r.tree.files[k];
            if (f.status == folder::State::running && f.waiting && f.bytes == 0) seen[k].waited_first = true;
        }
    };
    {
        std::jthread watch([&](const std::stop_token& st) {
            while (!st.stop_requested()) {
                {
                    std::lock_guard lock(r.m);
                    for (std::size_t k = 0; k < r.tree.files.size(); ++k) {
                        const auto& f = r.tree.files[k];
                        auto& s = seen[k];
                        if (f.status != folder::State::running || !f.waiting) {
                            if (f.status == folder::State::running) s.read = true;
                            s.waits = 0;
                        } else if (s.read) {
                            s.longest = std::max(s.longest, ++s.waits);
                        }
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        folder::run(r.job, r.tree, r.m, {});
    }
    for (std::size_t k = 0; k < seen.size(); ++k) {
        const auto& f = r.tree.files[k];
        CHECK_MESSAGE(f.status == folder::State::done, f.path, ": ", f.error);
        CHECK_MESSAGE(seen[k].waited_first, f.path);
        CHECK_MESSAGE(seen[k].read, f.path);
        CHECK_MESSAGE(seen[k].longest < 3, f.path, ": waiting after its reads for ", seen[k].longest, " looks");
        CHECK_FALSE(f.waiting);
    }
}

TEST_CASE("grab engine folders: a big file whose connections the small files hold waits for them; one past At once is queued") {
    Tree t;
    t.dir("/data");
    for (int i = 0; i < 400; ++i) t.file(std::format("/data/s{:03}.bin", i), test::pattern(20000 + i));
    for (int i = 0; i < 3; ++i) t.file(std::format("/data/t{}.bin", i), test::pattern((3u << 20) + 4096 * i));
    t.delay = std::chrono::milliseconds(1); // the small files take a while
    Run r(t.pool(2), "held", 2, 1);
    { auto listed = folder::list(r.job, r.tree, r.m, {}); REQUIRE_MESSAGE(listed, listed.error_or("")); }
    std::size_t most_held = 0;
    bool queued_held = false; // a big file not started shown waiting for the small files
    bool held_after = false;  // one still held once no small file runs
    r.job.on_event = [&](folder::Event) {
        std::lock_guard lock(r.m);
        const bool small_hold = r.tree.small_connections > 0;
        const auto p = queue_view::pass(r.tree.files, nullptr, small_hold);
        most_held = std::max(most_held, p.held);
        const auto snap = queue_view::snapshot(r.tree.files, small_hold);
        for (std::size_t k = 0; k < r.tree.files.size(); ++k) {
            const auto& f = r.tree.files[k];
            if (f.big && f.status == folder::State::queued && snap.rows[k].held) queued_held = true;
        }
        if (!small_hold && p.held > 0) held_after = true;
    };
    folder::run(r.job, r.tree, r.m, {});
    for (const auto& f : r.tree.files) CHECK_MESSAGE(f.status == folder::State::done, f.path, ": ", f.error);
    CHECK(most_held == 1); // At once 1: the big file that has its place, Waiting
    CHECK_FALSE(queued_held); // the others, Queued
    CHECK_FALSE(held_after);
    CHECK(r.tree.small_connections == 0);
}

TEST_CASE("grab engine folders: small files reach a speed limit over the network's round trip") {
    // One file and one READ at a time per connection under a limit left most of each round trip
    // idle (half of 1 MiB/s on Main, chunk F's L4). Now many are on their way, each READ still sent
    // right after its tokens: the limit, and no more.
    Tree t;
    t.dir("/data");
    for (int i = 0; i < 300; ++i) t.file(std::format("/data/s{:03}.bin", i), test::pattern(16 * 1024 + i));
    t.delay = std::chrono::milliseconds(20); // a round trip
    Run r(t.pool(2), "small-limit", 2, 1);
    rate::RateLimiter limiter;
    limiter.set_rate(1u << 20);
    r.job.limiter = &limiter;
    { auto listed = folder::list(r.job, r.tree, r.m, {}); REQUIRE_MESSAGE(listed, listed.error_or("")); }
    std::stop_source stop;
    const auto start = Tree::Clock::now();
    {
        std::jthread run([&] { folder::run(r.job, r.tree, r.m, stop.get_token()); });
        std::this_thread::sleep_for(std::chrono::milliseconds(3000));
        stop.request_stop();
    }
    std::uint64_t asked = 0; // READs asked from 0.5 s to 2.5 s, past the bucket's first burst
    {
        std::lock_guard lock(t.mutex);
        for (const auto& a : t.asks) {
            const auto when = a.at - start;
            if (when >= std::chrono::milliseconds(500) && when < std::chrono::milliseconds(2500)) asked += a.length;
        }
    }
    const double mibps = static_cast<double>(asked) / 2.0 / (1 << 20);
    MESSAGE("small files under 1 MiB/s: ", mibps, " MiB/s");
    CHECK(mibps > 0.9);
    CHECK(mibps < 1.1);
}

TEST_CASE("grab engine folders: a speed limit switched on mid-run reaches the small files at once") {
    // The schedule can turn a limit on in the middle of a folder. The small-file workers read the
    // rate at every pass and always count as the limiter's members, so they take tokens from then
    // on rather than at their next turn (which can be the rest of the folder); what they had on
    // their way counts against it (or ends their turns: the next test). Here how much a worker has
    // on its way at the switch depends on the machine's timing, about fetch::limited_keep, so
    // either can happen: a worker kept goes on at once and pays for it, one cut logs in again.
    Tree t;
    t.dir("/data");
    for (int i = 0; i < 2000; ++i) t.file(std::format("/data/s{:04}.bin", i), test::pattern(16 * 1024 + i));
    t.delay = std::chrono::milliseconds(20); // a round trip
    Run r(t.pool(2), "small-limit-later", 2, 1);
    rate::RateLimiter limiter; // unlimited at the start
    r.job.limiter = &limiter;
    std::mutex came_m;
    std::vector<std::pair<Tree::Clock::time_point, std::uint64_t>> came; // every reply's bytes
    r.job.on_received = [&](std::uint64_t n) {
        std::lock_guard lock(came_m);
        came.emplace_back(Tree::Clock::now(), n);
    };
    { auto listed = folder::list(r.job, r.tree, r.m, {}); REQUIRE_MESSAGE(listed, listed.error_or("")); }
    auto came_total = [&] {
        std::lock_guard lock(came_m);
        std::uint64_t n = 0;
        for (const auto& c : came) n += c.second;
        return n;
    };
    constexpr std::uint64_t limit = 1u << 20;
    std::stop_source stop;
    const auto start = Tree::Clock::now();
    Tree::Clock::time_point switched;
    int logins_then = 0;
    {
        std::jthread run([&] { folder::run(r.job, r.tree, r.m, stop.get_token()); });
        // Both workers well under way (a busy machine's logins can take longer than half a second).
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        for (int i = 0; i < 2000 && came_total() < (1u << 20); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        logins_then = t.logins.load();
        switched = Tree::Clock::now();
        limiter.set_rate(limit);
        std::this_thread::sleep_for(std::chrono::milliseconds(3500));
        stop.request_stop();
    }
    const int cut = t.logins.load() - logins_then; // workers that logged in again
    std::uint64_t before = 0; // READs asked before the switch: it ran unlimited
    // From 1.25 s to 3.25 s after it: a kept worker's debt (at most limited_keep a worker, a second
    // at this limit) is paid by then, and a cut one has logged in again.
    std::uint64_t asked = 0;
    {
        std::lock_guard lock(t.mutex);
        for (const auto& a : t.asks) {
            if (a.at >= start && a.at < switched) before += a.length;
            const auto when = a.at - switched;
            if (when >= std::chrono::milliseconds(1250) && when < std::chrono::milliseconds(3250)) asked += a.length;
        }
    }
    std::uint64_t first = 0; // what came in the first half second after the switch
    {
        std::lock_guard lock(came_m);
        for (const auto& [at, n] : came) {
            if (at >= switched && at - switched < std::chrono::milliseconds(500)) first += n;
        }
    }
    const double mibps = static_cast<double>(asked) / 2.0 / (1 << 20);
    MESSAGE("before the switch ", before, " bytes; in its first half second ", first, " bytes (", cut,
            " cut); after it ", mibps, " MiB/s");
    CHECK(before > 0);
    CHECK(mibps > 0.9);
    CHECK(mibps < 1.1);
    // What the two workers may keep on their way and nothing asked past it: the bucket starts empty
    // and pays what they kept first. Unlimited, each has up to 8 MiB on its way.
    CHECK(first <= 2 * fetch::limited_keep(limit, 1) + (64u << 10));
}

TEST_CASE("grab engine folders: small files: a limit switched on below what a connection has on its way drops it; its files come whole over a new login") {
    // Unlimited, a small-file connection has up to 8 MiB of READs on its way, which nothing calls
    // back. The turn ends at once as a broken connection's does, the worker logs in again, and it
    // stays one of the folder's small workers meanwhile (its big files would otherwise stop showing
    // that they wait for them); its unfinished files stay running for its next turn, not Queued.
    Tree t;
    t.dir("/data");
    for (int i = 0; i < 40; ++i) t.file(std::format("/data/m{:02}.bin", i), test::pattern((1u << 20) - 4096 * i));
    t.slice = std::size_t{64} * 1024;
    t.delay = std::chrono::milliseconds(4); // about 16 MiB/s a connection
    t.login_delay = std::chrono::milliseconds(150);
    Run r(t.pool(2), "small-limit-cut", 2, 1);
    rate::RateLimiter limiter; // unlimited at the start
    r.job.limiter = &limiter;
    std::mutex came_m;
    std::vector<std::pair<Tree::Clock::time_point, std::uint64_t>> came; // every reply's bytes
    r.job.on_received = [&](std::uint64_t n) {
        std::lock_guard lock(came_m);
        came.emplace_back(Tree::Clock::now(), n);
    };
    { auto listed = folder::list(r.job, r.tree, r.m, {}); REQUIRE_MESSAGE(listed, listed.error_or("")); }
    auto came_total = [&] {
        std::lock_guard lock(came_m);
        std::uint64_t n = 0;
        for (const auto& c : came) n += c.second;
        return n;
    };
    Tree::Clock::time_point switched;
    int logins_then = 0;
    int fewest_workers = 2; // small workers, from the switch on for a second
    std::vector<std::size_t> running_then; // files running at the switch
    int queued_again = 0;                  // of them, seen Queued in that second
    {
        std::jthread run([&] { folder::run(r.job, r.tree, r.m, {}); });
        for (int i = 0; i < 1000 && came_total() < (2u << 20); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        logins_then = t.logins.load();
        {
            std::lock_guard lock(r.m);
            for (std::size_t i = 0; i < r.tree.files.size(); ++i) {
                if (r.tree.files[i].status == folder::State::running) running_then.push_back(i);
            }
        }
        switched = Tree::Clock::now();
        limiter.set_rate(1u << 20);
        while (Tree::Clock::now() - switched < std::chrono::seconds(1)) {
            {
                std::lock_guard lock(r.m);
                fewest_workers = std::min(fewest_workers, r.tree.small_connections);
                for (const std::size_t i : running_then) {
                    if (r.tree.files[i].status == folder::State::queued) ++queued_again;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        limiter.set_rate(0); // the rest at full speed
    }
    std::uint64_t first = 0; // what came in the first half second after the switch
    {
        std::lock_guard lock(came_m);
        for (const auto& [at, n] : came) {
            if (at >= switched && at - switched < std::chrono::milliseconds(500)) first += n;
        }
    }
    MESSAGE("in the first half second after the switch: ", first, " bytes");
    for (const auto& f : r.tree.files) {
        CHECK_MESSAGE(f.status == folder::State::done, f.path, ": ", f.error);
        CHECK_MESSAGE(util::read_file(r.local(f.path)) == t.files.at("/data/" + f.path), f.path);
    }
    CHECK(t.logins.load() > logins_then); // dropped, logged in again: not a failure
    CHECK(fewest_workers == 2);
    CHECK(!running_then.empty());
    CHECK(queued_again == 0);
    // The limit's half second, less the login, and a READ each connection had come as it looked;
    // not the 8 MiB each had on its way.
    CHECK(first <= (5u << 18));
}

TEST_CASE("grab engine folders: a big file whose connections a new limit dropped doesn't show waiting while they log in again") {
    // Its streams with more on their way than the limit allows end and log in again; meanwhile they
    // still count as reading, as its file would show Queued (waiting) for that moment.
    Tree t;
    t.dir("/data");
    t.file("/data/big.bin", test::pattern(48u << 20));
    t.slice = std::size_t{64} * 1024;
    t.delay = std::chrono::milliseconds(4); // about 16 MiB/s a connection
    t.login_delay = std::chrono::milliseconds(150);
    Run r(t.pool(2), "limit-cut-big", 2, 1);
    rate::RateLimiter limiter; // unlimited at the start
    r.job.limiter = &limiter;
    { auto listed = folder::list(r.job, r.tree, r.m, {}); REQUIRE_MESSAGE(listed, listed.error_or("")); }
    int logins_then = 0;
    bool waited = false;
    {
        std::jthread run([&] { folder::run(r.job, r.tree, r.m, {}); });
        for (int i = 0; i < 1000; ++i) {
            {
                std::lock_guard lock(r.m);
                if (r.tree.files[0].bytes >= (4u << 20)) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        logins_then = t.logins.load();
        const auto switched = Tree::Clock::now();
        limiter.set_rate(2u << 20);
        while (Tree::Clock::now() - switched < std::chrono::seconds(1)) {
            {
                std::lock_guard lock(r.m);
                const auto& f = r.tree.files[0];
                waited = waited || (f.status == folder::State::running && f.waiting);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        limiter.set_rate(0); // the rest at full speed
    }
    const auto& f = r.tree.files[0];
    CHECK_MESSAGE(f.status == folder::State::done, f.error);
    CHECK(util::read_file(r.local(f.path)) == t.files.at("/data/big.bin"));
    CHECK(t.logins.load() > logins_then);
    CHECK_FALSE(waited);
}
