#include <doctest/doctest.h>

#include "conn_budget.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_pool.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <vector>

using namespace grab;
using namespace std::chrono_literals;

namespace {

// Logins without a server: counts them, can be slow, can be turned away or fail for good.
struct Logins {
    std::atomic<int> made{0}, now{0}, most_at_once{0}, alive{0};
    std::atomic<int> refuse_first{0};
    bool fatal = false;
    std::chrono::milliseconds takes{0};

    struct Fake : ssh::Session {
        explicit Fake(Logins& l, int id) : logins(l), id(id) { ++logins.alive; }
        ~Fake() override { --logins.alive; }
        Fake(const Fake&) = delete;
        Fake& operator=(const Fake&) = delete;
        sftp::Client& sftp() override { throw std::logic_error("no SFTP in this test"); }
        Logins& logins;
        int id;
    };

    ssh::Pool::Connect connect() {
        return [this]() -> std::unique_ptr<ssh::Session> {
            int id = ++made;
            int at_once = ++now;
            int seen = most_at_once.load();
            while (at_once > seen && !most_at_once.compare_exchange_weak(seen, at_once)) {
            }
            std::this_thread::sleep_for(takes);
            --now;
            if (refuse_first-- > 0) throw ssh::Error(budget::Failure::refused, "Failed getting banner");
            if (fatal) throw ssh::Error(budget::Failure::fatal, "the password was refused");
            return std::make_unique<Fake>(*this, id);
        };
    }
};

ssh::Pool::Options options(int limit, int logins_at_once = 4) {
    ssh::Pool::Options o;
    o.limit = limit;
    o.logins_at_once = logins_at_once;
    o.retry_refused = {1ms, 1ms, 1ms};
    o.idle_timer = false;
    return o;
}

int id_of(const ssh::Pool::Lease& lease) { return dynamic_cast<const Logins::Fake&>(*lease).id; }

} // namespace

TEST_CASE("ssh pool: a returned connection is used again, not logged in anew") {
    Logins logins;
    ssh::Pool pool(logins.connect(), options(4));
    int first = 0;
    {
        auto lease = pool.acquire();
        REQUIRE(lease);
        first = id_of(*lease);
        CHECK(pool.open() == 1);
    }
    CHECK(pool.idle() == 1);
    auto again = pool.acquire();
    REQUIRE(again);
    CHECK(id_of(*again) == first);
    CHECK(logins.made == 1);
}

TEST_CASE("ssh pool: no more than the limit open; a waiter gets the next one back") {
    Logins logins;
    ssh::Pool pool(logins.connect(), options(2));
    auto a = pool.acquire();
    auto b = pool.acquire();
    REQUIRE(a);
    REQUIRE(b);
    std::atomic<bool> got = false;
    std::jthread waiter([&] {
        auto c = pool.acquire();
        got = c.has_value();
    });
    std::this_thread::sleep_for(50ms);
    CHECK_FALSE(got);
    CHECK(pool.open() == 2);
    a->release();
    waiter.join();
    CHECK(got);
    CHECK(logins.made == 2); // the waiter took the returned one
}

TEST_CASE("ssh pool: logins at once are capped, whatever the limit") {
    Logins logins;
    logins.takes = 40ms;
    ssh::Pool pool(logins.connect(), options(8, 3));
    std::vector<std::optional<ssh::Pool::Lease>> leases(8);
    {
        std::vector<std::jthread> threads;
        for (auto& l : leases) threads.emplace_back([&pool, &l] { l = pool.acquire(); });
    }
    CHECK(std::ranges::all_of(leases, [](const auto& l) { return l.has_value(); }));
    CHECK(logins.made == 8);
    CHECK(logins.most_at_once <= 3);
    CHECK(logins.most_at_once >= 2);
}

TEST_CASE("ssh pool: a turned-away login is tried again; a refused one isn't") {
    SUBCASE("refused twice, then in") {
        Logins logins;
        logins.refuse_first = 2;
        ssh::Pool pool(logins.connect(), options(2));
        auto lease = pool.acquire();
        CHECK(lease);
        CHECK(logins.made == 3);
    }
    SUBCASE("refused every time: the refusal, after the retries") {
        Logins logins;
        logins.refuse_first = 100;
        ssh::Pool pool(logins.connect(), options(2));
        try {
            (void)pool.acquire();
            FAIL("no error");
        } catch (const ssh::Error& e) {
            CHECK(e.kind == budget::Failure::refused);
        }
        CHECK(logins.made == 4); // first try + 3 retries
        CHECK(pool.open() == 0);
    }
    SUBCASE("a fatal error at once") {
        Logins logins;
        logins.fatal = true;
        ssh::Pool pool(logins.connect(), options(2));
        CHECK_THROWS_AS((void)pool.acquire(), ssh::Error);
        CHECK(logins.made == 1);
    }
}

TEST_CASE("ssh pool: broken sessions and idle ones are closed") {
    Logins logins;
    ssh::Pool pool(logins.connect(), options(4));
    {
        auto lease = pool.acquire();
        REQUIRE(lease);
        lease->broken();
    }
    CHECK(pool.open() == 0);
    CHECK(logins.alive == 0);

    { auto lease = pool.acquire(); }
    CHECK(pool.idle() == 1);
    pool.close_idle(ssh::Pool::Clock::now()); // not idle a minute yet
    CHECK(pool.idle() == 1);
    pool.close_idle(ssh::Pool::Clock::now() + 61s);
    CHECK(pool.idle() == 0);
    CHECK(logins.alive == 0);
}

TEST_CASE("ssh pool: lowering the limit closes idle connections over it") {
    Logins logins;
    ssh::Pool pool(logins.connect(), options(4));
    {
        auto a = pool.acquire();
        auto b = pool.acquire();
        auto c = pool.acquire();
    }
    CHECK(pool.idle() == 3);
    pool.set_limit(1);
    CHECK(pool.idle() == 1);
    CHECK(logins.alive == 1);
    auto a = pool.acquire(); // the one left
    CHECK(a);
    CHECK(logins.made == 3);
    std::stop_source stop;
    stop.request_stop();
    CHECK_FALSE(pool.acquire(stop.get_token())); // no room for another
}

TEST_CASE("ssh pool: a waiting borrower stops when asked") {
    Logins logins;
    ssh::Pool pool(logins.connect(), options(1));
    auto held = pool.acquire();
    REQUIRE(held);
    std::stop_source stop;
    std::optional<ssh::Pool::Lease> got;
    std::atomic<bool> returned = false;
    std::jthread waiter([&] {
        got = pool.acquire(stop.get_token());
        returned = true;
    });
    std::this_thread::sleep_for(30ms);
    CHECK_FALSE(returned);
    stop.request_stop();
    waiter.join();
    CHECK_FALSE(got);
}
