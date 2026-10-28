#pragma once

// A server's pool of logins on in-memory SFTP servers (fake_sftp.hpp), for the tests of what reads
// through ssh::Pool (test_ssh_read.cpp, test_play_stream.cpp).

#include "conn_budget.hpp"
#include "fake_sftp.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_pool.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace grab::test {

using namespace std::chrono_literals;

// Pooled sessions on in-memory SFTP servers sharing one set of files; what each was asked for is
// collected as it closes.
struct FakePool {
    std::map<std::string, std::string> files;
    std::set<std::string> fifos, no_size, dirs, denied;
    std::map<std::string, std::uint64_t> claimed; // sizes STAT claims instead
    bool break_on_close = false;
    std::uint32_t max_data = sftp::max_read;
    bool reverse = false;
    std::size_t slice = 0;
    std::map<int, std::chrono::milliseconds> slow; // login -> a pause before each of its reads
    std::chrono::milliseconds slow_all{0};         // the same for every login without its own
    std::atomic<int> slow_from_now{-1};            // >= 0: every session's pause (ms) from its next use on
    std::atomic<std::size_t> slice_from_now{0};    // > 0: every session's `slice` from its next use on
    std::map<int, int> breaks;                     // login -> the read on which its stream breaks
    std::optional<budget::Failure> login_fails;    // every login fails so
    std::atomic<int> closed{0};                    // logins up to this one the server has closed
    bool shared = false; // the sessions read `files` itself, not a copy each (big files; not changed meanwhile)
    std::mutex mutex;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> reads;
    std::map<int, std::uint64_t> asked_by; // login -> bytes its READs asked for
    std::map<int, std::vector<std::pair<std::uint64_t, std::uint32_t>>> reads_by; // login -> its READs
    std::vector<std::string> opened;
    std::atomic<int> logins{0};
    std::atomic<int> pings{0}; // keepalives sent; one on a login the server closed fails

    struct Session : ssh::Session {
        Session(FakePool& f, int n) : fake(f), login(n), client(server) {
            {
                std::scoped_lock lock(f.mutex);
                if (f.shared) server.shared_files = &f.files;
                else server.files = f.files;
                for (const auto& d : f.dirs) server.dirs[d] = {".", ".."};
                server.fifos = f.fifos;
                server.no_size = f.no_size;
                server.denied = f.denied;
                server.listed_size = f.claimed;
                server.break_on_close = f.break_on_close;
                if (const auto it = f.slow.find(n); it != f.slow.end()) server.delay = it->second;
                else server.delay = f.slow_all;
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
            auto& mine = fake.reads_by[login];
            mine.insert(mine.end(), server.reads.begin(), server.reads.end());
            for (const auto& [off, len] : server.reads) fake.asked_by[login] += len;
            fake.opened.insert(fake.opened.end(), server.opened.begin(), server.opened.end());
        }
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
        sftp::Client& sftp() override {
            if (login <= fake.closed) server.dropped = true; // found out at the next read
            if (const int ms = fake.slow_from_now; ms >= 0) server.delay = std::chrono::milliseconds(ms);
            if (const std::size_t n = fake.slice_from_now; n > 0) server.slice = n;
            return client;
        }
        bool keepalive() override {
            ++fake.pings;
            return login > fake.closed;
        }
        bool reusable() override { return client.pending() == 0; }
        FakePool& fake;
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

} // namespace grab::test
