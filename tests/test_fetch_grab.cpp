#include <doctest/doctest.h>

#include "conn_budget.hpp"
#include "fake_sftp.hpp"
#include "fetch.hpp"
#include "rate.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_pool.hpp"
#include "util.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace grab;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

// Pooled sessions on in-memory SFTP servers sharing one set of files; what they were asked for
// is collected as each closes.
struct Fake {
    std::map<std::string, std::string> files;
    std::uint32_t max_data = sftp::max_read;
    bool reverse = false;
    std::size_t slice = 0;
    std::chrono::milliseconds delay{0};
    std::mutex mutex;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> reads;
    std::atomic<int> logins{0};
    std::atomic<int> refuse_logins{0}; // the next this many logins are turned away (a full server)
    bool fatal_login = false;          // every login refused for good (a wrong password)
    std::set<int> sftp_fails;          // the logins (from 1) that can't start SFTP
    int no_sftp_from = 0;              // from this login on, the server refuses SFTP (0: never)
    std::atomic<int> closed{0};        // logins up to this one the server has closed: their reads break

    struct Session : ssh::Session {
        Session(Fake& f, int n, bool fails, bool refuses)
            : fake(f), login(n), no_sftp(fails), refuses_sftp(refuses), client(server) {
            server.shared_files = &f.files;
            server.max_data = f.max_data;
            server.reverse = f.reverse;
            server.slice = f.slice;
            server.delay = f.delay;
            client.start();
        }
        ~Session() override {
            std::lock_guard lock(fake.mutex);
            fake.reads.insert(fake.reads.end(), server.reads.begin(), server.reads.end());
        }
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
        // As the real session's SFTP, started on first use, fails to start on a broken link.
        sftp::Client& sftp() override {
            if (refuses_sftp) throw sftp::Unavailable("the server doesn't offer SFTP");
            if (no_sftp) throw sftp::Broken("SFTP didn't start: the server closed the SFTP channel");
            if (login <= fake.closed) server.dropped = true; // found out at the next read
            return client;
        }
        bool reusable() override { return !no_sftp && !refuses_sftp && client.pending() == 0; }
        Fake& fake;
        int login;
        bool no_sftp;
        bool refuses_sftp;
        test::FakeServer server;
        sftp::Client client;
    };

    std::shared_ptr<ssh::Pool> pool(int limit) {
        auto o = ssh::Pools::download_options(limit);
        o.idle_timer = false;
        return std::make_shared<ssh::Pool>([this]() -> std::unique_ptr<ssh::Session> {
            const int login = ++logins;
            if (fatal_login) throw ssh::Error(budget::Failure::fatal, "Fake refused the login: it refused the password");
            if (refuse_logins-- > 0) throw ssh::Error(budget::Failure::refused, "SSH handshake with Fake failed");
            return std::make_unique<Session>(*this, login, sftp_fails.contains(login),
                                             no_sftp_from > 0 && login >= no_sftp_from);
        }, o);
    }
    std::uint64_t asked() {
        std::lock_guard lock(mutex);
        std::uint64_t n = 0;
        for (auto [off, len] : reads) n += len;
        return n;
    }
};

struct Dir {
    fs::path path;
    explicit Dir(std::string_view name) : path(fs::temp_directory_path() / "grab-tests-fetch-grab" / name) {
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

fetch::Download download(const fs::path& target, std::shared_ptr<ssh::Pool> pool, const std::string& path = "/data/f.bin") {
    fetch::Download d;
    d.source = fetch::Source{"rclone-is-not-run", std::nullopt, "Fake", path, {}};
    d.server = "Fake";
    d.id = 1;
    d.target = target;
    d.pool = std::move(pool);
    return d;
}

} // namespace

TEST_CASE("grab engine: a file over several connections, each byte asked for once") {
    Fake fake;
    const std::string content = test::pattern(40u << 20);
    fake.files["/data/f.bin"] = content;
    fake.delay = 2ms; // long enough for the other connections to join and split
    Dir dir("several");
    auto pool = fake.pool(4);
    fetch::Connections connections;
    connections.configure("Fake", 4);
    auto d = download(dir.path / "f.bin", pool);
    const auto r = fetch::run(d, connections, {}, {});
    REQUIRE_MESSAGE(r.outcome == fetch::Outcome::done, r.error);
    CHECK(util::read_file(dir.path / "f.bin") == content);
    CHECK_FALSE(fs::exists(fetch::partial_files(dir.path / "f.bin", 1).record));
    d.pool.reset();
    pool.reset(); // the sessions close and report what they were asked for
    CHECK(fake.asked() == content.size()); // no byte twice, none past the end
    CHECK(fake.logins >= 2);
    CHECK(fake.logins <= 4);
    CHECK(r.modtime == "2023-11-14T22:13:20Z");
}

TEST_CASE("grab engine: a partial download rclone started goes on, its done bytes not fetched again") {
    Fake fake;
    const std::string content = test::pattern(3u << 20);
    fake.files["/data/f.bin"] = content;
    Dir dir("resume");
    const fs::path target = dir.path / "f.bin";
    // As rclone's engine left it: its time in this PC's zone (+04:00), the same moment as the
    // server's 1700000000.
    auto state = fetch::new_state("Fake", "/data/f.bin", content.size(), "2023-11-15T02:13:20+04:00", 1536u << 10);
    REQUIRE(state.ranges.size() == 2);
    state.ranges[0].done = 1u << 20;
    state.ranges[1].done = 512u << 10;
    const auto files = fetch::partial_files(target, 1);
    {
        std::string part(content.size(), '\0');
        for (const auto& r : state.ranges) part.replace(r.start, r.done, content.substr(r.start, r.done));
        REQUIRE(util::write_file_atomic(files.part, part));
        REQUIRE(util::write_file_atomic(files.record, fetch::state_to_json(state)));
    }
    auto pool = fake.pool(2);
    fetch::Connections connections;
    connections.configure("Fake", 2);
    const auto r = fetch::run(download(target, pool), connections, {}, {});
    REQUIRE_MESSAGE(r.outcome == fetch::Outcome::done, r.error);
    CHECK(r.note.empty()); // went on, didn't start over
    CHECK(util::read_file(target) == content);
    pool.reset();
    CHECK(fake.asked() == content.size() - state.bytes_done());
}

TEST_CASE("grab engine: short reads, replies out of order, the stream in pieces") {
    Fake fake;
    const std::string content = test::pattern((2u << 20) + 12345);
    fake.files["/data/f.bin"] = content;
    fake.max_data = 10'000;
    fake.reverse = true;
    fake.slice = 4096;
    Dir dir("short");
    auto pool = fake.pool(1);
    fetch::Connections connections;
    connections.configure("Fake", 1);
    const auto r = fetch::run(download(dir.path / "f.bin", pool), connections, {}, {});
    REQUIRE_MESSAGE(r.outcome == fetch::Outcome::done, r.error);
    CHECK(util::read_file(dir.path / "f.bin") == content);
}

TEST_CASE("grab engine: a missing file, and one the server won't open, fail with the reason") {
    Fake fake;
    fake.files["/data/f.bin"] = "x";
    Dir dir("refused");
    auto pool = fake.pool(2);
    fetch::Connections connections;
    connections.configure("Fake", 2);
    auto missing = fetch::run(download(dir.path / "a", pool, "/data/nope.bin"), connections, {}, {});
    CHECK(missing.outcome == fetch::Outcome::failed);
    CHECK(missing.error == "no such file or folder");
}

TEST_CASE("grab engine: turned-away logins are waited out and lower the budget; a refused login fails at once") {
    SUBCASE("a full server: the download waits, goes on, and learns") {
        Fake fake;
        const std::string content = test::pattern(3u << 20);
        fake.files["/data/f.bin"] = content;
        fake.refuse_logins = 2; // the size check's first login and the next
        Dir dir("full");
        auto pool = fake.pool(4);
        fetch::Connections connections;
        connections.configure("Fake", 4);
        const auto r = fetch::run(download(dir.path / "f.bin", pool), connections, {}, {});
        REQUIRE_MESSAGE(r.outcome == fetch::Outcome::done, r.error);
        CHECK(util::read_file(dir.path / "f.bin") == content);
        CHECK(connections.refusals("Fake") == 2);
    }
    SUBCASE("a refused password: no retries, the reason") {
        Fake fake;
        fake.files["/data/f.bin"] = "x";
        fake.fatal_login = true;
        Dir dir("fatal");
        auto pool = fake.pool(4);
        fetch::Connections connections;
        connections.configure("Fake", 4);
        const auto r = fetch::run(download(dir.path / "f.bin", pool), connections, {}, {});
        CHECK(r.outcome == fetch::Outcome::failed);
        CHECK(r.error == "Fake refused the login: it refused the password");
        CHECK(fake.logins == 1);
    }
}

TEST_CASE("grab engine: a connection whose SFTP doesn't start is dropped, and the download goes on with others") {
    Fake fake;
    const std::string content = test::pattern(8u << 20);
    fake.files["/data/f.bin"] = content;
    fake.delay = 1ms; // long enough for the other connections to join
    fake.sftp_fails = {2, 3}; // the size check's login works; the next two ranges' don't
    Dir dir("no-sftp");
    auto pool = fake.pool(3);
    fetch::Connections connections;
    connections.configure("Fake", 3);
    auto d = download(dir.path / "f.bin", pool);
    const auto r = fetch::run(d, connections, {}, {});
    REQUIRE_MESSAGE(r.outcome == fetch::Outcome::done, r.error);
    CHECK(util::read_file(dir.path / "f.bin") == content);
    CHECK(fake.logins >= 4);
}

TEST_CASE("grab engine: a server that refuses SFTP fails the download at once, without logging in again") {
    Fake fake;
    fake.files["/data/f.bin"] = test::pattern(8u << 20);
    fake.delay = 1ms;
    fake.no_sftp_from = 2; // the size check's login has it; the other ranges' logins don't
    Dir dir("sftp-refused");
    auto pool = fake.pool(3);
    fetch::Connections connections;
    connections.configure("Fake", 3);
    const auto r = fetch::run(download(dir.path / "f.bin", pool), connections, {}, {});
    CHECK(r.outcome == fetch::Outcome::failed);
    CHECK(r.error == "the server doesn't offer SFTP");
    CHECK(fake.logins <= 3);
}

TEST_CASE("grab engine: the size check on a reused connection the server closed while idle tries the next one, once") {
    Fake fake;
    fake.files["/data/f.bin"] = "first";
    const std::string content = test::pattern(3u << 10);
    fake.files["/data/g.bin"] = content;
    Dir dir("closed-idle");
    fetch::Connections connections;
    connections.configure("Fake", 2);

    SUBCASE("one connection: the size check's new login fetches the file") {
        auto pool = fake.pool(1);
        connections.configure("Fake", 1);
        REQUIRE(fetch::run(download(dir.path / "f.bin", pool), connections, {}, {}).outcome == fetch::Outcome::done);
        REQUIRE(fake.logins == 1);
        REQUIRE(pool->idle() == 1);
        fake.closed = 1;
        const auto r = fetch::run(download(dir.path / "g.bin", pool, "/data/g.bin"), connections, {}, {});
        REQUIRE_MESSAGE(r.outcome == fetch::Outcome::done, r.error);
        CHECK(util::read_file(dir.path / "g.bin") == content);
        CHECK(fake.logins == 2);
    }
    SUBCASE("two closed while idle: the second is the answer, no third try") {
        auto pool = fake.pool(2);
        {
            auto a = pool->acquire();
            auto b = pool->acquire();
            REQUIRE(a);
            REQUIRE(b);
        }
        REQUIRE(fake.logins == 2);
        REQUIRE(pool->idle() == 2);
        fake.closed = 2;
        const auto r = fetch::run(download(dir.path / "g.bin", pool, "/data/g.bin"), connections, {}, {});
        CHECK(r.outcome == fetch::Outcome::failed);
        CHECK(r.error == "fake server: the server closed the connection");
        CHECK(fake.logins == 2);
        CHECK(pool->idle() == 0); // neither went back
    }
    SUBCASE("a new login that breaks is the answer") {
        auto pool = fake.pool(2);
        fake.closed = 100;
        const auto r = fetch::run(download(dir.path / "g.bin", pool, "/data/g.bin"), connections, {}, {});
        CHECK(r.outcome == fetch::Outcome::failed);
        CHECK(r.error == "fake server: the server closed the connection");
        CHECK(fake.logins == 1);
    }
}

TEST_CASE("grab engine: a file changed on the server since the partial download starts over, and says so") {
    Fake fake;
    const std::string content = test::pattern(2u << 20);
    fake.files["/data/f.bin"] = content;
    Dir dir("changed");
    const fs::path target = dir.path / "f.bin";
    auto state = fetch::new_state("Fake", "/data/f.bin", content.size(), "2020-01-01T00:00:00Z", 1u << 20);
    state.ranges[0].done = 1u << 20;
    const auto files = fetch::partial_files(target, 1);
    REQUIRE(util::write_file_atomic(files.part, std::string(content.size(), 'z')));
    REQUIRE(util::write_file_atomic(files.record, fetch::state_to_json(state)));
    auto pool = fake.pool(2);
    fetch::Connections connections;
    connections.configure("Fake", 2);
    const auto r = fetch::run(download(target, pool), connections, {}, {});
    REQUIRE_MESSAGE(r.outcome == fetch::Outcome::done, r.error);
    CHECK(r.note.find("started over") != std::string::npos);
    CHECK(util::read_file(target) == content); // none of the old partial's bytes
}

TEST_CASE("grab engine: the speed limit is taken before each request, in small ones") {
    Fake fake;
    const std::string content = test::pattern(1536u << 10);
    fake.files["/data/f.bin"] = content;
    Dir dir("limit");
    auto pool = fake.pool(2);
    fetch::Connections connections;
    connections.configure("Fake", 2);
    rate::RateLimiter limiter;
    limiter.set_rate(1u << 20); // 1 MiB/s
    auto d = download(dir.path / "f.bin", pool);
    d.limiter = &limiter;
    const auto started = std::chrono::steady_clock::now();
    const auto r = fetch::run(d, connections, {}, {});
    const auto took = std::chrono::steady_clock::now() - started;
    REQUIRE_MESSAGE(r.outcome == fetch::Outcome::done, r.error);
    CHECK(util::read_file(dir.path / "f.bin") == content);
    CHECK(took >= 900ms); // 1.5 MiB at 1 MiB/s, less the bucket's first quarter second
    d.pool.reset();
    pool.reset();
    REQUIRE_FALSE(fake.reads.empty());
    for (auto [off, len] : fake.reads) CHECK(len <= 128u * 1024); // an eighth of a second's worth
}

TEST_CASE("grab engine: stopped mid-file it keeps what came, and goes on from there") {
    Fake fake;
    const std::string content = test::pattern(3u << 20);
    fake.files["/data/f.bin"] = content;
    Dir dir("stop");
    auto pool = fake.pool(2);
    fetch::Connections connections;
    connections.configure("Fake", 2);
    rate::RateLimiter limiter;
    limiter.set_rate(1u << 20);
    auto d = download(dir.path / "f.bin", pool);
    d.limiter = &limiter;
    std::stop_source stop;
    std::jthread stopper([&] {
        std::this_thread::sleep_for(700ms);
        stop.request_stop();
    });
    const auto first = fetch::run(d, connections, {}, stop.get_token());
    stopper.join();
    REQUIRE(first.outcome == fetch::Outcome::stopped);
    const auto saved = fetch::saved_progress(d.target, "/data/f.bin", {}, "Fake");
    REQUIRE(saved);
    CHECK(saved->first > 0);
    CHECK(saved->first < content.size());

    limiter.set_rate(0);
    const auto second = fetch::run(d, connections, {}, {});
    REQUIRE_MESSAGE(second.outcome == fetch::Outcome::done, second.error);
    CHECK(util::read_file(d.target) == content);
}
