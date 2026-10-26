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

#include <atomic>
#include <cstdint>
#include <filesystem>
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
    std::atomic<int> logins{0};

    struct Session : ssh::Session {
        explicit Session(Tree& t) : tree(t), client(server) {
            server.shared_files = &t.files;
            server.dirs = t.dirs;
            server.links = t.links;
            server.readdir_size = t.readdir_size;
            server.denied = t.denied;
            server.readdir_batch = 3;
            client.start();
        }
        ~Session() override {
            std::lock_guard lock(tree.mutex);
            tree.reads.insert(tree.reads.end(), server.reads.begin(), server.reads.end());
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
