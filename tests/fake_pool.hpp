#pragma once

// A server's pool of logins on in-memory SFTP servers (fake_sftp.hpp), for the tests of what reads
// or writes through ssh::Pool (test_ssh_read.cpp, test_play_stream.cpp, test_upload.cpp).

#include "conn_budget.hpp"
#include "fake_sftp.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_pool.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
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
    // > 0: no session answers a READ until this many logins happened (a Play stream's connections
    // all find work as they log in, however late their threads run on a busy machine); 20 s at most.
    int reads_wait_for_logins = 0;
    bool shared = false; // the sessions read `files` itself, not a copy each (big files; not changed meanwhile)
    // Every session reads and writes `store` instead (FakeServer::shared), one server's file system
    // for all its logins: uploads, several connections writing one file. `files` and `dirs` unused.
    bool writable = false;
    FakeFiles store;
    // Each session's server as these say (fake_sftp.hpp).
    std::chrono::milliseconds latency{0};
    std::chrono::milliseconds write_pace{0};
    bool held = false;
    bool release_one = false;
    bool read_only = false;
    sftp::Code read_only_as = sftp::Code::permission_denied;
    std::uint64_t quota_bytes = 0;
    bool posix_rename = true;
    bool rename_overwrites = false;
    std::uint32_t max_write = 0;
    bool statvfs = false;
    bool no_setstat = false;
    std::set<std::string> locked_dirs;           // folders no name may be added to or removed from (chmod 555)
    sftp::Code mkdir_existing_as = sftp::Code::failure; // what MKDIR of a name there answers
    std::set<std::string> rename_refused;        // plain RENAMEs to these answer "Permission denied"
    std::atomic<bool> break_after_rename{false}; // the next RENAME/posix-rename, on any session
    // The server's own copies, owners' names and links (fake_sftp.hpp, SERVER-OPS.md).
    bool copy_data = false;
    bool users_groups = false;
    std::map<std::uint32_t, std::string> user_names;
    std::map<std::uint32_t, std::string> group_names;
    std::uint32_t uid = 1000;
    std::uint32_t gid = 1000;
    bool symlink_refused = false;
    std::set<std::string> mounts;                // file systems of their own: no RENAME between them
    std::function<void(std::uint64_t)> on_copy;  // each copy-data's length, on its session's thread (a pace)
    bool readdir_follows_links = false;          // READDIR lists a link as what it leads to
    bool readdir_no_types = false;               // READDIR lists names without a type
    std::string home;                            // the login folder relative paths are below
    // An OPEN, STAT or LSTAT of a path it says yes to (on any session, its thread) breaks that
    // session's connection after it is done, before its reply.
    std::function<bool(const std::string&)> break_at;
    // With `writable`: told each OPEN's path on every session as it comes, with `store` (its lock
    // held: change it directly), so a test can put a file where an upload is about to go.
    std::function<void(FakeFiles&, const std::string&)> on_open;
    std::mutex mutex;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> reads;
    std::vector<FakeServer::Write> writes;   // every session's WRITEs, as each closed
    std::vector<std::string> requests;       // and their requests, by name
    std::map<int, std::vector<std::string>> requests_by; // login -> its requests, in order
    std::map<int, std::uint64_t> asked_by; // login -> bytes its READs asked for
    std::map<int, std::vector<std::pair<std::uint64_t, std::uint32_t>>> reads_by; // login -> its READs
    std::vector<std::string> opened;
    std::atomic<int> logins{0};
    std::atomic<int> pings{0}; // keepalives sent; one on a login the server closed fails

    struct Session : ssh::Session {
        Session(FakePool& f, int n) : fake(f), login(n), client(server) {
            {
                std::scoped_lock lock(f.mutex);
                if (f.writable) server.shared = &f.store;
                else if (f.shared) server.shared_files = &f.files;
                else server.files = f.files;
                if (!f.writable)
                    for (const auto& d : f.dirs) server.dirs[d] = {".", ".."};
                server.fifos = f.fifos;
                server.no_size = f.no_size;
                server.denied = f.denied;
                server.locked_dirs = f.locked_dirs;
                server.rename_refused = f.rename_refused;
                server.user_names = f.user_names;
                server.group_names = f.group_names;
                server.mounts = f.mounts;
                server.listed_size = f.claimed;
                server.break_on_close = f.break_on_close;
                if (const auto it = f.slow.find(n); it != f.slow.end()) server.delay = it->second;
                else server.delay = f.slow_all;
                if (const auto it = f.breaks.find(n); it != f.breaks.end()) server.broken_after = it->second;
            }
            server.max_data = f.max_data;
            server.reverse = f.reverse;
            server.slice = f.slice;
            server.latency = f.latency;
            server.write_pace = f.write_pace;
            server.held = f.held;
            server.release_one = f.release_one;
            server.read_only = f.read_only;
            server.read_only_as = f.read_only_as;
            server.quota_bytes = f.quota_bytes;
            server.posix_rename = f.posix_rename;
            server.rename_overwrites = f.rename_overwrites;
            server.max_write = f.max_write;
            server.statvfs = f.statvfs;
            server.no_setstat = f.no_setstat;
            server.mkdir_existing_as = f.mkdir_existing_as;
            server.break_after_rename = &f.break_after_rename;
            server.copy_data = f.copy_data;
            server.users_groups = f.users_groups;
            server.uid = f.uid;
            server.gid = f.gid;
            server.symlink_refused = f.symlink_refused;
            server.readdir_follows_links = f.readdir_follows_links;
            server.readdir_no_types = f.readdir_no_types;
            server.home = f.home;
            if (f.on_copy) server.on_copy = [&f](std::uint64_t length) { f.on_copy(length); };
            if (f.break_at) server.break_at = [&f](const std::string& path) { return f.break_at(path); };
            if (f.on_open && f.writable) server.on_open = [&f](const std::string& path) { f.on_open(f.store, path); };
            if (f.reads_wait_for_logins > 0) {
                server.on_read = [&f](const std::string&, std::uint32_t) {
                    const auto until = std::chrono::steady_clock::now() + 20s;
                    while (f.logins < f.reads_wait_for_logins && std::chrono::steady_clock::now() < until) {
                        std::this_thread::sleep_for(1ms);
                    }
                };
            }
            client.start();
        }
        ~Session() override {
            std::scoped_lock lock(fake.mutex);
            fake.reads.insert(fake.reads.end(), server.reads.begin(), server.reads.end());
            auto& mine = fake.reads_by[login];
            mine.insert(mine.end(), server.reads.begin(), server.reads.end());
            for (const auto& [off, len] : server.reads) fake.asked_by[login] += len;
            fake.opened.insert(fake.opened.end(), server.opened.begin(), server.opened.end());
            fake.writes.insert(fake.writes.end(), server.writes.begin(), server.writes.end());
            fake.requests.insert(fake.requests.end(), server.requests.begin(), server.requests.end());
            fake.requests_by[login] = server.requests;
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
