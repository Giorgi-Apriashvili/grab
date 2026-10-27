#include <doctest/doctest.h>

#include "conn_budget.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_pool.hpp"

#include <algorithm>
#include <array>
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

TEST_CASE("ssh pool: the most recently used idle connection stays longer than the others") {
    Logins logins;
    auto o = options(4);
    o.idle_close = 15s;
    o.keep_newest = 60s;
    ssh::Pool pool(logins.connect(), o);
    {
        auto a = pool.acquire();
        auto b = pool.acquire();
        REQUIRE(a);
        REQUIRE(b);
        a->release();
        std::this_thread::sleep_for(5ms);
        b->release(); // the most recently used
    }
    CHECK(pool.idle() == 2);
    pool.close_idle(ssh::Pool::Clock::now() + 16s);
    CHECK(pool.idle() == 1);
    pool.close_idle(ssh::Pool::Clock::now() + 59s);
    CHECK(pool.idle() == 1);
    auto again = pool.acquire();
    REQUIRE(again);
    CHECK(id_of(*again) == 2);
    again->release();
    pool.close_idle(ssh::Pool::Clock::now() + 61s);
    CHECK(pool.idle() == 0);
    // As the program's pools are made.
    CHECK(ssh::Pools::download_options(4).keep_newest == 60s);
    CHECK(ssh::Pools::download_options(4).idle_close == 15s);
    CHECK(ssh::Pools::download_options(4).retry_refused.empty());
}

TEST_CASE("ssh pool: a caller may bring its own pauses for turned-away logins") {
    Logins logins;
    logins.refuse_first = 2;
    auto o = options(2);
    o.retry_refused = {}; // the pool's own: none, as downloads have it
    ssh::Pool pool(logins.connect(), o);
    CHECK_THROWS_AS((void)pool.acquire(), ssh::Error);
    CHECK(logins.made == 1);
    const std::array pauses{1ms, 1ms};
    auto lease = pool.acquire({}, pauses);
    CHECK(lease);
    CHECK(logins.made == 3); // the refusal left, then in
    CHECK(ssh::interactive_retry.size() == 4);
    CHECK(ssh::interactive_retry.front() == 500ms);
    CHECK(ssh::interactive_retry.back() == 4s);
}

TEST_CASE("ssh pool: a new login on request, an idle connection closed to make room") {
    Logins logins;
    ssh::Pool pool(logins.connect(), options(2));
    {
        auto a = pool.acquire();
        auto b = pool.acquire();
        REQUIRE((a && b));
    }
    CHECK(pool.idle() == 2);
    // Full: the oldest idle connection goes, never one handed out.
    auto fresh = pool.acquire_new();
    REQUIRE(fresh);
    CHECK(id_of(*fresh) == 3);
    CHECK_FALSE(fresh->reused());
    CHECK(logins.alive == 2);
    CHECK(pool.open() == 2);
    // Room under the limit: no connection closed.
    fresh.reset();
    auto held = pool.acquire();
    REQUIRE(held);
    pool.set_limit(3);
    auto another = pool.acquire_new();
    REQUIRE(another);
    CHECK(id_of(*another) == 4);
    CHECK(logins.alive == 3);
    // Nothing idle and no room: it waits, and stops when asked.
    std::stop_source stop;
    std::jthread stopper([&] {
        std::this_thread::sleep_for(20ms);
        stop.request_stop();
    });
    auto third = pool.acquire();
    REQUIRE(third);
    CHECK_FALSE(pool.acquire_new(stop.get_token()));
}

TEST_CASE("ssh pool: hash commands take turns, as many at once as the server allows") {
    Logins logins;
    auto o = options(4);
    o.hashes_at_once = 2;
    ssh::Pool pool(logins.connect(), o);
    CHECK(pool.hashes_at_once() == 2);
    auto first = pool.hash_turn();
    auto second = pool.hash_turn();
    REQUIRE((first && second));
    // A third waits for one to end.
    std::atomic<bool> got{false};
    std::jthread third([&] {
        auto turn = pool.hash_turn();
        got = turn.has_value();
    });
    std::this_thread::sleep_for(30ms);
    CHECK_FALSE(got);
    first.reset();
    third.join();
    CHECK(got);
    // A waiter stops when asked.
    auto again = pool.hash_turn();
    std::stop_source stop;
    stop.request_stop();
    CHECK_FALSE(pool.hash_turn(stop.get_token()));
    CHECK(ssh::hashes_at_once_for("u123-sub1.Your-StorageBox.de") == 2);
    CHECK(ssh::hashes_at_once_for("65.21.224.123") == 1);
}

TEST_CASE("ssh pool: the hash check is asked once; one that may pass is asked again after a while") {
    Logins logins;
    auto o = options(2);
    o.find_check_expiry = 0ms;
    ssh::Pool pool(logins.connect(), o);
    int asked = 0;
    auto check = [&](bool lasting) {
        return [&asked, lasting]() -> std::optional<ssh::HashCheck> {
            ++asked;
            return ssh::HashCheck{.works = lasting, .z = true, .home = "/home/u", .answer = "md5sum -z", .failure = {},
                                  .lasting = lasting};
        };
    };
    REQUIRE(pool.hash_check({}, check(true)));
    REQUIRE(pool.hash_check({}, check(true)));
    CHECK(asked == 1); // for the pool's life
    // Replaced by a login that failed later, then dropped.
    pool.set_hash_check(ssh::HashCheck{.works = false, .z = false, .home = {}, .answer = "refused the login",
                                       .failure = ssh::HashFailure::login, .lasting = true});
    const ssh::HashCheck refused{.works = false, .z = false, .home = {}, .answer = "refused the login",
                                 .failure = ssh::HashFailure::login, .lasting = true};
    CHECK(pool.hash_check({}, check(true)) == refused);
    CHECK(asked == 1);
    pool.set_hash_check(std::nullopt);
    REQUIRE(pool.hash_check({}, check(false)));
    CHECK(asked == 2);
    REQUIRE(pool.hash_check({}, check(false)));
    CHECK(asked == 3); // not lasting: held only find_check_expiry
    // A refusal (already asked again on a new login): held as one that may pass, so an account
    // that refuses every command costs no login per verification.
    auto o2 = options(2);
    ssh::Pool other(logins.connect(), o2);
    const auto refused_answer = [&asked]() -> std::optional<ssh::HashCheck> {
        ++asked;
        return ssh::HashCheck{.works = false, .z = false, .home = {}, .answer = "the server refused to run the command",
                              .failure = ssh::HashFailure::refused, .lasting = false};
    };
    REQUIRE(other.hash_check({}, refused_answer));
    REQUIRE(other.hash_check({}, refused_answer));
    CHECK(asked == 4);
    ssh::Pool brief(logins.connect(), o);
    REQUIRE(brief.hash_check({}, refused_answer));
    REQUIRE(brief.hash_check({}, refused_answer));
    CHECK(asked == 6);
}

TEST_CASE("ssh pools: a login that failed for good is kept until the server's Test drops it") {
    ssh::Pools pools;
    ssh::Target t;
    t.user = "u";
    t.host = "example.invalid";
    auto pool = pools.get("srv", t, 2); // no login until a lease is asked for
    int asked = 0;
    const auto check = [&asked]() -> std::optional<ssh::HashCheck> {
        ++asked;
        return ssh::HashCheck{.works = false, .z = false, .home = {}, .answer = "the host key isn't trusted yet",
                              .failure = ssh::HashFailure::login, .lasting = true};
    };
    REQUIRE(pool->hash_check({}, check));
    REQUIRE(pool->hash_check({}, check));
    CHECK(asked == 1);
    pools.forget_hash_check("other"); // another server's: nothing
    REQUIRE(pool->hash_check({}, check));
    CHECK(asked == 1);
    pools.forget_hash_check("srv");
    REQUIRE(pool->hash_check({}, check));
    CHECK(asked == 2);
}
