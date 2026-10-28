#include <doctest/doctest.h>

#include "conn_budget.hpp"
#include "fake_sftp.hpp"
#include "fetch.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_pool.hpp"
#include "ssh_read.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace grab;
using namespace std::chrono_literals;

namespace {

// Pooled sessions on in-memory SFTP servers sharing one set of files; what each was asked for is
// collected as it closes.
struct Fake {
    std::map<std::string, std::string> files;
    std::set<std::string> fifos, no_size, dirs, denied;
    std::map<std::string, std::uint64_t> claimed; // sizes STAT claims instead
    bool break_on_close = false;
    std::uint32_t max_data = sftp::max_read;
    bool reverse = false;
    std::size_t slice = 0;
    std::map<int, std::chrono::milliseconds> slow; // login -> a pause before each of its reads
    std::map<int, int> breaks;                     // login -> the read on which its stream breaks
    std::optional<budget::Failure> login_fails;    // every login fails so
    std::atomic<int> closed{0};                    // logins up to this one the server has closed
    std::mutex mutex;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> reads;
    std::map<int, std::uint64_t> asked_by; // login -> bytes its READs asked for
    std::vector<std::string> opened;
    std::atomic<int> logins{0};

    struct Session : ssh::Session {
        Session(Fake& f, int n) : fake(f), login(n), client(server) {
            {
                std::scoped_lock lock(f.mutex);
                server.files = f.files;
                for (const auto& d : f.dirs) server.dirs[d] = {".", ".."};
                server.fifos = f.fifos;
                server.no_size = f.no_size;
                server.denied = f.denied;
                server.listed_size = f.claimed;
                server.break_on_close = f.break_on_close;
                if (const auto it = f.slow.find(n); it != f.slow.end()) server.delay = it->second;
                if (const auto it = f.breaks.find(n); it != f.breaks.end()) server.broken_after = it->second;
            }
            server.max_data = f.max_data;
            server.reverse = f.reverse;
            server.slice = f.slice;
            client.start();
        }
        ~Session() override {
            std::scoped_lock lock(fake.mutex);
            fake.reads.insert(fake.reads.end(), server.reads.begin(), server.reads.end());
            for (const auto& [off, len] : server.reads) fake.asked_by[login] += len;
            fake.opened.insert(fake.opened.end(), server.opened.begin(), server.opened.end());
        }
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
        sftp::Client& sftp() override {
            if (login <= fake.closed) server.dropped = true; // found out at the next read
            return client;
        }
        bool reusable() override { return client.pending() == 0; }
        Fake& fake;
        int login;
        test::FakeServer server;
        sftp::Client client;
    };

    std::shared_ptr<ssh::Pool> pool(int limit = 8, std::chrono::milliseconds failure_holds = 30s) {
        auto o = ssh::Pools::download_options(limit);
        o.idle_timer = false;
        o.login_failure_holds = failure_holds;
        return std::make_shared<ssh::Pool>([this]() -> std::unique_ptr<ssh::Session> {
            const int login = ++logins;
            if (login_fails) throw ssh::Error(*login_fails, "Fake refused the login: the key was refused");
            return std::make_unique<Session>(*this, login);
        }, o);
    }
    std::uint64_t asked() {
        std::scoped_lock lock(mutex);
        std::uint64_t n = 0;
        for (const auto& [off, len] : reads) n += len;
        return n;
    }
};

// Extra connections' seats, counted.
struct Seats {
    std::atomic<int> free{0};
    std::atomic<int> taken{0};
    std::atomic<int> back{0};
    void into(ssh::ReadOptions& o) {
        o.seat = [this] {
            for (int f = free.load(); f > 0;) {
                if (free.compare_exchange_weak(f, f - 1)) {
                    ++taken;
                    return true;
                }
            }
            return false;
        };
        o.seat_back = [this] {
            ++back;
            ++free;
        };
    }
};

} // namespace

TEST_CASE("ssh read: a file whole over one connection, its first READs small and growing, each byte asked once") {
    Fake fake;
    const std::string content = test::pattern((std::size_t{5} << 20) + 123);
    fake.files["/d/a.bin"] = content;
    auto pool = fake.pool();
    {
        ssh::Reader reader(pool, "/d/a.bin", {});
        const auto st = reader.stat();
        REQUIRE(st);
        REQUIRE(st->size);
        CHECK(*st->size == content.size());
        CHECK(st->mtime == 1700000000u);
        const auto body = reader.read(0, *st->size);
        REQUIRE(body);
        CHECK(*body == content);
        // Back in the pool already, for the next preview, while the reader is still there.
        CHECK(pool->idle() == 1);
    }
    pool.reset();
    CHECK(fake.logins == 1);
    CHECK(fake.asked() == content.size());
    REQUIRE(fake.reads.size() > 4);
    CHECK(fake.reads[0] == std::pair<std::uint64_t, std::uint32_t>{0, 32 * 1024});
    CHECK(fake.reads[1] == std::pair<std::uint64_t, std::uint32_t>{32 * 1024, 64 * 1024});
    CHECK(fake.reads[2].second == 128 * 1024);
    CHECK(fake.reads[3].second == sftp::max_read);
    // One connection reads its pieces in order.
    CHECK(std::ranges::is_sorted(fake.reads));
    CHECK(fake.opened == std::vector<std::string>{"/d/a.bin"});
}

TEST_CASE("ssh read: short reads, replies out of order and a stream in small slices still give the file exactly") {
    Fake fake;
    const std::string content = test::pattern((std::size_t{1} << 20) + 17);
    fake.files["/f"] = content;
    fake.max_data = 10'000;
    fake.reverse = true;
    fake.slice = 4096;
    auto pool = fake.pool();
    ssh::Reader reader(pool, "/f", {});
    const auto body = reader.read(100, content.size()); // past the end: the file ends first
    REQUIRE(body);
    CHECK(*body == content.substr(100));
}

TEST_CASE("ssh read: a big range also goes over extra connections on free seats, each given back") {
    Fake fake;
    const std::string content = test::pattern(std::size_t{20} << 20);
    fake.files["/big.jpg"] = content;
    for (int login = 1; login <= 4; ++login) fake.slow[login] = 1ms;
    auto pool = fake.pool();
    Seats seats;
    seats.free = 3;
    ssh::ReadOptions o;
    o.extra = 3;
    seats.into(o);
    {
        ssh::Reader reader(pool, "/big.jpg", o);
        const auto body = reader.read(0, content.size());
        REQUIRE(body);
        CHECK(*body == content);
    }
    pool.reset();
    CHECK(seats.taken == seats.back);
    CHECK(seats.free == 3);
    CHECK(fake.logins >= 2);
    CHECK(fake.asked() == content.size()); // no byte asked twice
    std::size_t reading = 0;
    for (const auto& [login, bytes] : fake.asked_by) reading += bytes > 0 ? 1 : 0;
    CHECK(reading >= 2);
}

TEST_CASE("ssh read: a small range or no free seat: one connection") {
    Fake fake;
    fake.files["/a"] = test::pattern(std::size_t{3} << 20);
    fake.files["/b"] = test::pattern(std::size_t{12} << 20);
    auto pool = fake.pool();
    Seats seats;
    seats.free = 3;
    ssh::ReadOptions o;
    o.extra = 3;
    seats.into(o);
    {
        ssh::Reader reader(pool, "/a", o); // under extra_from
        REQUIRE(reader.read(0, std::uint64_t{3} << 20));
    }
    CHECK(seats.taken == 0);
    seats.free = 0;
    {
        ssh::Reader reader(pool, "/b", o); // big enough, but every seat taken
        REQUIRE(reader.read(0, std::uint64_t{12} << 20));
    }
    CHECK(seats.taken == 0);
    CHECK(fake.logins == 1);
}

TEST_CASE("ssh read: a slow connection doesn't hold the rest back; a broken one's pieces are read by the others") {
    SUBCASE("slow") {
        Fake fake;
        const std::string content = test::pattern(std::size_t{16} << 20);
        fake.files["/v"] = content;
        fake.slow[2] = 30ms; // the extra connection: about 8 MiB/s
        auto pool = fake.pool();
        Seats seats;
        seats.free = 1;
        ssh::ReadOptions o;
        o.extra = 1;
        seats.into(o);
        {
            ssh::Reader reader(pool, "/v", o);
            const auto body = reader.read(0, content.size());
            REQUIRE(body);
            CHECK(*body == content);
        }
        pool.reset();
        CHECK(fake.asked() == content.size());
        // Its share of what was left, within its depth: never the half it would get dealt out alone.
        CHECK(fake.asked_by[2] < content.size() / 2);
    }
    SUBCASE("broken") {
        Fake fake;
        const std::string content = test::pattern(std::size_t{16} << 20);
        fake.files["/v"] = content;
        for (int login = 1; login <= 3; ++login) fake.slow[login] = 1ms;
        fake.breaks[2] = 6; // an extra connection's stream breaks with READs on their way
        auto pool = fake.pool();
        Seats seats;
        seats.free = 2;
        ssh::ReadOptions o;
        o.extra = 2;
        seats.into(o);
        {
            ssh::Reader reader(pool, "/v", o);
            const auto body = reader.read(0, content.size());
            REQUIRE(body);
            CHECK(*body == content);
        }
        CHECK(seats.taken == seats.back);
    }
}

TEST_CASE("ssh read: a stop drops a connection with much still on its way, else waits for its replies") {
    for (const bool drop : {true, false}) {
        CAPTURE(drop);
        Fake fake;
        fake.files["/f"] = test::pattern(std::size_t{4} << 20);
        auto pool = fake.pool();
        std::stop_source stop;
        ssh::ReadOptions o;
        o.stop = stop.get_token();
        o.drop_over = drop ? 0 : std::uint64_t{1} << 30;
        o.progress = [&](std::uint64_t, std::uint64_t) { stop.request_stop(); };
        {
            ssh::Reader reader(pool, "/f", o);
            const auto body = reader.read(0, std::uint64_t{4} << 20);
            REQUIRE_FALSE(body);
            CHECK(body.error().kind == ssh::ReadFailure::stopped);
        }
        CHECK(pool->idle() == (drop ? 0 : 1));
        CHECK(pool->open() == pool->idle());
        CHECK(fake.asked() < (std::uint64_t{4} << 20)); // nothing more was asked after the stop
    }
}

TEST_CASE("ssh read: stopped before it starts, nothing logs in") {
    Fake fake;
    fake.files["/f"] = "x";
    auto pool = fake.pool();
    std::stop_source stop;
    stop.request_stop();
    ssh::ReadOptions o;
    o.stop = stop.get_token();
    ssh::Reader reader(pool, "/f", o);
    const auto st = reader.stat();
    REQUIRE_FALSE(st);
    CHECK(st.error().kind == ssh::ReadFailure::stopped);
    CHECK(fake.logins == 0);
}

TEST_CASE("ssh read: a reused connection the server closed meanwhile is dropped and the next one tried, once") {
    Fake fake;
    fake.files["/f"] = test::pattern(5000);
    auto pool = fake.pool();
    {
        ssh::Reader reader(pool, "/f", {});
        REQUIRE(reader.read(0, 5000));
    }
    REQUIRE(pool->idle() == 1);
    fake.closed = 1;
    {
        ssh::Reader reader(pool, "/f", {});
        const auto body = reader.read(0, 5000);
        REQUIRE(body);
        CHECK(*body == fake.files["/f"]);
    }
    CHECK(fake.logins == 2);

    // A new connection that breaks isn't tried again.
    fake.closed = 100;
    pool->trim_idle(0);
    ssh::Reader reader(pool, "/f", {});
    const auto st = reader.stat();
    REQUIRE_FALSE(st);
    CHECK(st.error().kind == ssh::ReadFailure::lost);
    CHECK(fake.logins == 3);
}

TEST_CASE("ssh read: only a regular file is opened; a missing one, one that can't be opened") {
    Fake fake;
    fake.files["/d/ok.txt"] = "fine";
    fake.files["/d/locked.txt"] = "no";
    fake.fifos.insert("/d/pipe.jpg");
    fake.dirs.insert("/d/folder.jpg");
    fake.denied.insert("/d/locked.txt");
    auto pool = fake.pool();
    for (const std::string path : {"/d/pipe.jpg", "/d/folder.jpg"}) {
        CAPTURE(path);
        ssh::Reader reader(pool, path, {});
        const auto st = reader.stat();
        REQUIRE_FALSE(st);
        CHECK(st.error().kind == ssh::ReadFailure::not_file);
        const auto body = reader.read(0, 10);
        REQUIRE_FALSE(body);
        CHECK(body.error().kind == ssh::ReadFailure::not_file);
    }
    {
        ssh::Reader reader(pool, "/d/missing.txt", {});
        const auto body = reader.read(0, 10);
        REQUIRE_FALSE(body);
        CHECK(body.error().kind == ssh::ReadFailure::status);
        CHECK(body.error().code == sftp::Code::no_such_file);
    }
    {
        ssh::Reader reader(pool, "/d/locked.txt", {});
        const auto body = reader.read(0, 10);
        REQUIRE_FALSE(body);
        CHECK(body.error().code == sftp::Code::permission_denied);
        CHECK(body.error().message == "permission denied");
    }
    {
        ssh::Reader reader(pool, "/d/ok.txt", {});
        const auto body = reader.read(0, 10);
        REQUIRE(body);
        CHECK(*body == "fine");
    }
    pool.reset();
    CHECK(fake.logins == 1);
    // Neither the pipe nor the folder was ever opened (nor the missing file: STAT said so).
    CHECK(fake.opened == std::vector<std::string>{"/d/locked.txt", "/d/ok.txt"});
}

TEST_CASE("ssh read: attributes without a size: the read takes what is there") {
    Fake fake;
    const std::string content = test::pattern(1000);
    fake.files["/n.txt"] = content;
    fake.no_size.insert("/n.txt");
    auto pool = fake.pool();
    Seats seats;
    seats.free = 3;
    ssh::ReadOptions o;
    o.extra = 3;
    seats.into(o);
    {
        ssh::Reader reader(pool, "/n.txt", o);
        const auto st = reader.stat();
        REQUIRE(st);
        CHECK_FALSE(st->size);
        const auto body = reader.read(0, (std::uint64_t{64} << 20) + 1); // an image's cap and a byte
        REQUIRE(body);
        CHECK(*body == content);
    }
    CHECK(seats.taken == 0); // a cap isn't a size: no extra connection for it
    CHECK(fake.logins == 1);
}

TEST_CASE("ssh read: a file that ends early keeps no more memory than its bytes") {
    Fake fake;
    const std::string content = test::pattern(20'000);
    fake.files["/cut.png"] = content;
    fake.claimed["/cut.png"] = std::uint64_t{16} << 20; // STAT's size, before the file was cut
    auto pool = fake.pool();
    ssh::Reader reader(pool, "/cut.png", {});
    const auto body = reader.read(0, std::uint64_t{16} << 20);
    REQUIRE(body);
    CHECK(*body == content);
    CHECK(body->capacity() < (std::size_t{1} << 20));
}

TEST_CASE("ssh read: a connection that breaks once every byte is in gives the read, and goes") {
    Fake fake;
    const std::string content = test::pattern(100'000);
    fake.files["/f"] = content;
    fake.break_on_close = true;
    auto pool = fake.pool();
    {
        ssh::Reader reader(pool, "/f", {});
        const auto body = reader.read(0, content.size());
        REQUIRE(body);
        CHECK(*body == content);
    }
    CHECK(pool->idle() == 0); // dropped, not kept
}

TEST_CASE("ssh read: a login that fails is tried once for many reads, until it is forgotten or its time is up") {
    SUBCASE("for good, one after another and at once") {
        Fake fake;
        fake.login_fails = budget::Failure::fatal;
        auto pool = fake.pool();
        for (int i = 0; i < 10; ++i) {
            ssh::Reader reader(pool, "/f", {});
            const auto st = reader.stat();
            REQUIRE_FALSE(st);
            CHECK(st.error().kind == ssh::ReadFailure::login);
            CHECK(st.error().login == budget::Failure::fatal);
            CHECK(st.error().message == "Fake refused the login: the key was refused");
        }
        CHECK(fake.logins == 1);

        auto fresh = fake.pool();
        fake.logins = 0;
        std::atomic<int> failed{0};
        {
            std::vector<std::jthread> previews;
            for (int i = 0; i < 10; ++i) {
                previews.emplace_back([&] {
                    ssh::Reader reader(fresh, "/f", {});
                    if (const auto st = reader.stat(); !st && st.error().kind == ssh::ReadFailure::login) ++failed;
                });
            }
        }
        CHECK(failed == 10);
        CHECK(fake.logins == 1);

        fresh->forget_login_failure(); // a Test that worked, Trust, a server saved
        ssh::Reader reader(fresh, "/f", {});
        CHECK_FALSE(reader.stat());
        CHECK(fake.logins == 2);
    }
    SUBCASE("for a while") {
        Fake fake;
        fake.login_fails = budget::Failure::other;
        auto pool = fake.pool(8, 100ms);
        for (int i = 0; i < 5; ++i) {
            ssh::Reader reader(pool, "/f", {});
            CHECK_FALSE(reader.stat());
        }
        CHECK(fake.logins == 1);
        std::this_thread::sleep_for(150ms);
        {
            ssh::Reader reader(pool, "/f", {});
            CHECK_FALSE(reader.stat());
        }
        CHECK(fake.logins == 2);
        // It works again: the next ones log in as they need to.
        fake.login_fails.reset();
        fake.files["/f"] = "ok";
        std::this_thread::sleep_for(150ms);
        ssh::Reader reader(pool, "/f", {});
        CHECK(reader.read(0, 2) == "ok");
    }
    SUBCASE("another caller's login that works clears it; an idle connection needs no login") {
        Fake fake;
        fake.login_fails = budget::Failure::fatal;
        fake.files["/f"] = "ok";
        auto pool = fake.pool();
        {
            ssh::Reader reader(pool, "/f", {});
            CHECK_FALSE(reader.stat());
        }
        CHECK(fake.logins == 1);
        // A download logs in once the agent is back, and its connection goes idle.
        fake.login_fails.reset();
        {
            auto lease = pool->acquire();
            REQUIRE(lease);
        }
        CHECK(fake.logins == 2);
        CHECK(pool->idle() == 1);
        fake.login_fails = budget::Failure::fatal; // would fail again: the idle one is used
        {
            ssh::Reader reader(pool, "/f", {});
            CHECK(reader.read(0, 2) == "ok");
        }
        CHECK(fake.logins == 2);
        // The download's login cleared the failure: a new login is tried again.
        fake.login_fails.reset();
        {
            ssh::Reader a(pool, "/f", {});
            ssh::Reader b(pool, "/f", {});
            CHECK(a.stat());
            CHECK(b.read(0, 2) == "ok");
        }
        CHECK(fake.logins == 3);
    }
}

TEST_CASE("connections: a free seat only when one is free and nobody waits; a download never steps aside for it") {
    fetch::Connections c;
    c.configure("S", 4);
    c.join("S", 1, 2); // a download
    for (int i = 0; i < 3; ++i) REQUIRE(c.acquire("S", 1, {}, false));
    const int viewer = c.new_id();
    c.join("S", viewer, 4);
    // One free: taken, and the download, now over its share, isn't asked to give one back.
    CHECK(c.try_acquire("S", viewer));
    CHECK_FALSE(c.yield_if_over_share("S", 1));
    // None free.
    CHECK_FALSE(c.try_acquire("S", viewer));
    CHECK_FALSE(c.yield_if_over_share("S", 1));

    // A seat asked for the usual way waits, counted, and the download over its share steps aside.
    std::atomic<bool> got{false};
    std::jthread waiting([&] { got = c.acquire("S", viewer, {}, false); });
    bool yielded = false;
    for (int i = 0; i < 200 && !yielded; ++i) {
        yielded = c.yield_if_over_share("S", 1);
        if (!yielded) std::this_thread::sleep_for(5ms);
    }
    CHECK(yielded);
    waiting.join();
    CHECK(got);

    // While a download waits for its share, a free seat stays for it.
    c.release("S", viewer, 2);
    CHECK(c.active("S", viewer) == 0);
    budget::ServerPool p(4);
    p.add_user(1);
    p.add_user(2);
    p.waiting(1, +1);
    CHECK_FALSE(p.free_for(2));
    p.waiting(1, -1);
    CHECK(p.free_for(2));
    CHECK_FALSE(p.free_for(3)); // not a user
}

TEST_CASE("ssh read: extra connections on Connections' free seats only") {
    Fake fake;
    const std::string content = test::pattern(std::size_t{10} << 20);
    fake.files["/big"] = content;
    fetch::Connections c;
    c.configure("S", 2);
    c.join("S", 1, 2);
    REQUIRE(c.acquire("S", 1, {}, false)); // a download holds one
    const int viewer = c.new_id();
    c.join("S", viewer, 4);
    REQUIRE(c.acquire("S", viewer, {}, false)); // the preview's own
    auto pool = fake.pool();
    ssh::ReadOptions o;
    o.extra = 3;
    o.seat = [&] { return c.try_acquire("S", viewer); };
    o.seat_back = [&] { c.release("S", viewer); };
    o.yield = [&] { return c.yield_if_over_share("S", viewer); };
    {
        ssh::Reader reader(pool, "/big", o);
        const auto body = reader.read(0, content.size());
        REQUIRE(body);
        CHECK(*body == content);
    }
    CHECK(fake.logins == 1); // the budget was full
    CHECK(c.active("S", viewer) == 1);
    CHECK(c.active("S", 1) == 1);
}
