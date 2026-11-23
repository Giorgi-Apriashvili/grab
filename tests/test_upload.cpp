#include <doctest/doctest.h>

#include "conn_budget.hpp"
#include "fake_pool.hpp"
#include "fake_sftp.hpp"
#include "fetch.hpp"
#include "rate.hpp"
#include "rclone_encoding.hpp"
#include "sftp.hpp"
#include "ssh_pool.hpp"
#include "upload.hpp"
#include "upload_existing.hpp"
#include "util.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace grab;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

struct Dir {
    fs::path path;
    explicit Dir(std::string_view name) : path(fs::temp_directory_path() / "grab-tests-upload" / name) {
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~Dir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    Dir(const Dir&) = delete;
    Dir& operator=(const Dir&) = delete;
    [[nodiscard]] fs::path records() const { return path / "records"; }
};

// A server whose logins all write one store (FakePool::writable), with the folder /up in it.
struct Server {
    test::FakePool fake;
    // `folders`: absolute, each after its own folder.
    explicit Server(const std::vector<std::string>& folders = {"/up"}) {
        fake.writable = true;
        fake.store.dirs["/"] = {".", ".."};
        for (const auto& d : folders) {
            fake.store.dirs[d] = {".", ".."};
            const auto slash = d.rfind('/');
            fake.store.dirs[slash == 0 ? "/" : d.substr(0, slash)].push_back(d.substr(slash + 1));
        }
    }
    // A file of the server's own, listed in its folder.
    void put(const std::string& path, std::string content, std::uint32_t mtime = 1700000000) {
        std::scoped_lock lock(fake.store.mutex);
        fake.store.files[path] = std::move(content);
        fake.store.mtimes[path] = mtime;
        const auto slash = path.rfind('/');
        fake.store.dirs[slash == 0 ? "/" : path.substr(0, slash)].push_back(path.substr(slash + 1));
    }
    // Every file and folder there, and what each holds: what nothing but the upload may change.
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
    [[nodiscard]] std::optional<std::string> file(const std::string& path) {
        std::scoped_lock lock(fake.store.mutex);
        const auto it = fake.store.files.find(path);
        if (it == fake.store.files.end()) return std::nullopt;
        return it->second;
    }
    [[nodiscard]] std::uint32_t mtime(const std::string& path) {
        std::scoped_lock lock(fake.store.mutex);
        const auto it = fake.store.mtimes.find(path);
        return it == fake.store.mtimes.end() ? 1700000000 : it->second;
    }
    // Names in /up that are temps of grab's.
    [[nodiscard]] std::vector<std::string> temps(const std::string& dir = "/up") {
        std::scoped_lock lock(fake.store.mutex);
        std::vector<std::string> out;
        for (const auto& n : fake.store.dirs[dir])
            if (upload::is_temp_name(n)) out.push_back(n);
        return out;
    }
    // Bytes of every WRITE the sessions closed so far sent.
    [[nodiscard]] std::uint64_t written() {
        std::scoped_lock lock(fake.mutex);
        std::uint64_t n = 0;
        for (const auto& w : fake.writes) n += w.length;
        return n;
    }
};

// The local file's modification time set to `unix_seconds`.
void set_mtime(const fs::path& p, std::int64_t unix_seconds) {
    fs::last_write_time(p, std::chrono::clock_cast<std::chrono::file_clock>(std::chrono::sys_seconds{std::chrono::seconds{unix_seconds}}));
}

upload::Upload upload_of(const fs::path& local, std::shared_ptr<ssh::Pool> pool, const Dir& dir,
                         std::string remote = "/up/f.bin", upload::Existing existing = upload::Existing::ask) {
    upload::Upload u;
    u.local = local;
    u.server = "Fake";
    u.id = 1;
    u.remote_path = std::move(remote);
    u.existing = existing;
    u.pool = std::move(pool);
    u.records_dir = dir.records();
    return u;
}

// A local file of `content`.
fs::path local_file(const Dir& dir, std::string_view content, std::string_view name = "f.bin") {
    const fs::path p = dir.path / name;
    REQUIRE(util::write_file_atomic(p, content));
    return p;
}

} // namespace

TEST_CASE("upload: server names of local files, temp names and which names are grab's") {
    CHECK(upload::remote_child("/srv", "a.txt") == "/srv/a.txt");
    CHECK(upload::remote_child("/", "a.txt") == "/a.txt");
    CHECK(upload::remote_child("", "a.txt") == "a.txt");
    CHECK(upload::remote_child("tv/", "a.txt") == "tv/a.txt");
    // A local name goes up as it is: whatever grab's form makes of it, its bytes come back.
    for (const std::string name : {"plain", "with \u201B quote", "\u2400 symbol", "fullwidth \uFF1F", "tab\there"}) {
        const std::string remote = upload::remote_child("/srv/dir", name);
        CHECK(rclone_enc::raw_path(remote) == "/srv/dir/" + name);
        CHECK(upload::split_remote(remote).first == "/srv/dir");
    }
    CHECK(upload::split_remote("/x") == std::pair<std::string, std::string>{"/", "x"});
    CHECK(upload::split_remote("a/b") == std::pair<std::string, std::string>{"a", "b"});
    CHECK(upload::split_remote("b") == std::pair<std::string, std::string>{"", "b"});

    CHECK(upload::temp_name("f.bin", "0a1b2c") == ".f.bin.0a1b2c.grabpart");
    const std::string longest(255 - 17, 'n');
    CHECK(upload::temp_name(longest, "0a1b2c").size() == 255);
    CHECK(upload::temp_name(longest + "n", "0a1b2c") == ".grab-0a1b2c.grabpart");
    const std::string token = upload::new_token();
    CHECK(token.size() == 6);
    CHECK(upload::is_temp_name(upload::temp_name("f.bin", token), "f.bin", token));
    CHECK(upload::is_temp_name(".f.bin.0a1b2c.grabpart"));
    CHECK(upload::is_temp_name(".grab-0a1b2c.grabpart", "anything", "0a1b2c"));
    CHECK_FALSE(upload::is_temp_name(".f.bin.0a1b2c.grabpart", "g.bin"));
    CHECK_FALSE(upload::is_temp_name(".f.bin.0a1b2c.grabpart", "f.bin", "ffffff"));
    CHECK_FALSE(upload::is_temp_name("f.bin.0a1b2c.grabpart"));
    CHECK_FALSE(upload::is_temp_name(".f.bin.0A1B2C.grabpart"));
    CHECK_FALSE(upload::is_temp_name(".f.bin.0a1b2.grabpart"));
    CHECK_FALSE(upload::is_temp_name("..0a1b2c.grabpart"));
    CHECK_FALSE(upload::is_temp_name(".f.bin.grabpart"));
    CHECK_FALSE(upload::is_temp_name("f.bin"));
}

TEST_CASE("upload: the existing-file choices, how two files compare and what each choice does") {
    using upload::Action;
    using upload::Existing;
    for (const auto e : {Existing::ask, Existing::replace, Existing::different, Existing::newer, Existing::skip, Existing::keep_both}) {
        CHECK(upload::parse_existing(upload::name(e)) == e);
    }
    CHECK(upload::name(Existing::keep_both) == "both");
    CHECK_FALSE(upload::parse_existing("Replace"));
    CHECK_FALSE(upload::parse_existing(""));

    const auto clash = [](std::uint64_t mine, std::int64_t mine_t, std::uint64_t theirs, std::optional<std::int64_t> theirs_t,
                          bool dir = false) {
        return upload::Clash{.file = "f",
                             .local_size = mine,
                             .local_mtime = mine_t,
                             .remote_size = theirs,
                             .remote_mtime = theirs_t,
                             .remote_is_dir = dir};
    };
    using upload::Kind;
    CHECK(upload::kind(clash(10, 1000, 10, 1000)) == Kind::same);
    CHECK(upload::kind(clash(10, 1002, 10, 1000)) == Kind::same); // FAT's 2 s
    CHECK(upload::kind(clash(10, 1003, 10, 1000)) == Kind::newer_here);
    CHECK(upload::kind(clash(10, 997, 10, 1000)) == Kind::older_here);
    CHECK(upload::kind(clash(11, 1000, 10, 1000)) == Kind::different_size);
    CHECK(upload::kind(clash(11, 5000, 10, 1000)) == Kind::newer_here);
    CHECK(upload::kind(clash(10, 5000, 10, std::nullopt)) == Kind::same); // no time: the size decides
    CHECK(upload::kind(clash(11, 5000, 10, std::nullopt)) == Kind::different_size);

    const auto action = [](Existing e, const upload::Clash& c) { return upload::decide(e, c).action; };
    const auto same = clash(10, 1000, 10, 1000);
    const auto newer = clash(10, 9000, 10, 1000);
    const auto older = clash(10, 100, 10, 1000);
    const auto bigger = clash(12, 1000, 10, 1000);
    const auto unknown = clash(12, 9000, 10, std::nullopt);
    CHECK(action(Existing::ask, same) == Action::ask);
    CHECK(action(Existing::replace, same) == Action::replace);
    CHECK(action(Existing::skip, newer) == Action::skip);
    CHECK(action(Existing::keep_both, same) == Action::numbered);
    CHECK(action(Existing::different, same) == Action::skip);
    CHECK(action(Existing::different, newer) == Action::replace);
    CHECK(action(Existing::different, older) == Action::replace);
    CHECK(action(Existing::different, bigger) == Action::replace);
    CHECK(action(Existing::newer, newer) == Action::replace);
    CHECK(action(Existing::newer, older) == Action::skip);
    CHECK(action(Existing::newer, same) == Action::skip);
    CHECK(action(Existing::newer, bigger) == Action::skip);
    CHECK(action(Existing::newer, unknown) == Action::skip); // a server that gives no time isn't older
    // A folder of that name is never replaced.
    const auto folder = clash(10, 9000, 0, 1000, true);
    for (const auto e : {Existing::replace, Existing::different, Existing::newer}) {
        const auto d = upload::decide(e, folder);
        CHECK(d.action == Action::skip);
        CHECK(d.note == "a folder of that name is there");
    }
    CHECK(action(Existing::keep_both, folder) == Action::numbered);
    CHECK(action(Existing::skip, folder) == Action::skip);
    CHECK(action(Existing::ask, folder) == Action::ask);
}

TEST_CASE("upload: a record goes back as it was written; a malformed one, or one naming another file's temp, is refused") {
    upload::Record r;
    r.state = fetch::new_state("Main", "/srv/tv/ep 1.mkv", 10'000'000, "2026-11-21T10:00:00.123456789Z", 4'000'000);
    r.state.ranges[1].done = 1234;
    r.local = R"(C:\Users\x\ep 1.mkv)";
    r.local_mtime_ns = 1'763'719'200'123'456'789; // more than a double holds exactly
    r.token = "0a1b2c";
    r.temp = rclone_enc::from_raw_name(upload::temp_name("ep 1.mkv", r.token));
    const std::string text = upload::record_to_json(r);
    const auto back = upload::record_from_json(text);
    REQUIRE(back);
    CHECK(*back == r);
    CHECK(text.find("\"version\":1") != std::string::npos);

    const auto changed = [&](std::string_view from, std::string_view to) {
        std::string t = text;
        const auto at = t.find(from);
        REQUIRE(at != std::string::npos);
        t.replace(at, from.size(), to);
        return upload::record_from_json(t);
    };
    CHECK_FALSE(changed("\"version\":1", "\"version\":2"));
    CHECK_FALSE(changed("\"token\":\"0a1b2c\"", "\"token\":\"0a1b2d\"")); // the temp doesn't carry it
    CHECK_FALSE(changed(".ep 1.mkv.0a1b2c", ".ep 2.mkv.0a1b2c"));       // another file's
    CHECK_FALSE(changed("\"localMtimeNs\":\"", "\"localMtimeNs\":\"x"));
    CHECK_FALSE(changed("[4000000,4000000,1234]", "[4000000,4000001,1234]")); // ranges past the file
    CHECK_FALSE(upload::record_from_json("[]"));
    CHECK_FALSE(upload::record_from_json("{"));

    // One record per server and path, in the folder given.
    const auto a = upload::record_path("C:/r", "Main", "/srv/a");
    CHECK(a == upload::record_path("C:/r", "Main", "/srv/a"));
    CHECK(a != upload::record_path("C:/r", "Box", "/srv/a"));
    CHECK(a != upload::record_path("C:/r", "Main", "/srv/b"));
    CHECK(a.parent_path() == fs::path("C:/r"));
    CHECK(util::path_to_utf8(a.filename()).size() == 16 + 5);
}

TEST_CASE("upload: a file over several connections, each byte at its offset, under its own name with the local time") {
    Server srv;
    srv.put("/up/other.txt", "keep me");
    const auto before = srv.tree();
    Dir dir("several");
    const std::string content = test::pattern(40u << 20);
    const fs::path local = local_file(dir, content);
    set_mtime(local, 1'750'000'000);
    srv.fake.latency = 2ms; // long enough for the other connections to join and split
    auto pool = srv.fake.pool(4);
    fetch::Connections connections;
    connections.configure("Fake", 4);
    auto u = upload_of(local, pool, dir);
    std::atomic<std::uint64_t> sent{0};
    u.on_sent = [&](std::uint64_t n) { sent += n; };
    const auto r = upload::run(u, connections, {}, {});
    REQUIRE_MESSAGE(r.outcome == upload::Outcome::done, r.error);
    CHECK(r.placed == "/up/f.bin");
    CHECK(r.size == content.size());
    CHECK(srv.file("/up/f.bin") == content);
    CHECK(srv.mtime("/up/f.bin") == 1'750'000'000u);
    CHECK(srv.temps().empty());
    CHECK_FALSE(fs::exists(upload::record_path(dir.records(), "Fake", "/up/f.bin")));
    CHECK(fs::is_empty(dir.records())); // its lock's file went with it
    u.pool.reset();
    pool.reset(); // the sessions close and report their WRITEs
    // Each byte sent once, none past the end, and the server acknowledged them all.
    std::vector<std::pair<std::uint64_t, std::uint32_t>> writes;
    for (const auto& w : srv.fake.writes) writes.emplace_back(w.offset, w.length);
    std::ranges::sort(writes);
    std::uint64_t at = 0;
    for (const auto& [offset, length] : writes) {
        CHECK(offset == at);
        CHECK(length <= upload::fallback_write); // the server says nothing of its limits
        at = offset + length;
    }
    CHECK(at == content.size());
    CHECK(sent == content.size());
    CHECK(srv.fake.logins >= 2);
    CHECK(srv.fake.logins <= 4);
    // Nothing else changed: the other file, and the folder lists the new one.
    auto after = srv.tree();
    CHECK(after.at("/up/other.txt") == "keep me");
    after.erase("/up/f.bin");
    CHECK(after.at("dir /up") == "./../f.bin/other.txt");
    after["dir /up"] = before.at("dir /up");
    CHECK(after == before);
}

TEST_CASE("upload: a small file, an empty one and the WRITE size the server's limits give") {
    for (const std::uint32_t limit : {0u, 64u * 1024, 1024u * 1024}) {
        CAPTURE(limit);
        Server srv;
        srv.fake.max_write = limit;
        Dir dir("small");
        const std::string content = test::pattern(700'000);
        const fs::path local = local_file(dir, content);
        auto pool = srv.fake.pool(2);
        fetch::Connections connections;
        connections.configure("Fake", 2);
        const auto r = upload::run(upload_of(local, pool, dir), connections, {}, {});
        REQUIRE_MESSAGE(r.outcome == upload::Outcome::done, r.error);
        CHECK(srv.file("/up/f.bin") == content);
        CHECK_FALSE(fs::exists(dir.records())); // a small file has no record
        pool.reset();
        const std::uint32_t biggest = std::ranges::max(srv.fake.writes, {}, &test::FakeServer::Write::length).length;
        CHECK(biggest == (limit == 0 ? upload::fallback_write : std::min<std::uint32_t>(limit, sftp::max_write)));
    }
    Server srv;
    Dir dir("empty");
    const fs::path local = local_file(dir, "");
    auto pool = srv.fake.pool(2);
    fetch::Connections connections;
    connections.configure("Fake", 2);
    const auto r = upload::run(upload_of(local, pool, dir, "/up/empty"), connections, {}, {});
    REQUIRE_MESSAGE(r.outcome == upload::Outcome::done, r.error);
    CHECK(srv.file("/up/empty") == "");
    CHECK(srv.temps().empty());
}

TEST_CASE("upload: each connection keeps at most 64 WRITEs and 8 MiB unacknowledged, Play's depth while it is urgent") {
    for (const bool urgent : {false, true}) {
        CAPTURE(urgent);
        Server srv;
        srv.fake.latency = 20ms; // replies a round trip later: the WRITEs in flight pile up
        Dir dir(urgent ? "urgent" : "depth");
        const std::string content = test::pattern(24u << 20);
        const fs::path local = local_file(dir, content);
        srv.fake.max_write = 1u << 20; // 255 KiB WRITEs: 8 MiB is about 32 of them
        auto pool = srv.fake.pool(2);
        fetch::Connections connections;
        connections.configure("Fake", 2);
        if (urgent) connections.urgent("Fake", -100, 4); // a Play stream warming up, for good
        const auto r = upload::run(upload_of(local, pool, dir), connections, {}, {});
        REQUIRE_MESSAGE(r.outcome == upload::Outcome::done, r.error);
        CHECK(srv.file("/up/f.bin") == content);
        pool.reset();
        std::size_t most = 0;
        std::uint64_t most_bytes = 0;
        for (const auto& w : srv.fake.writes) {
            most = std::max(most, w.out);
            most_bytes = std::max(most_bytes, w.out_bytes);
        }
        MESSAGE("most WRITEs a connection had out: ", most, ", bytes: ", most_bytes);
        if (urgent) CHECK(most <= 4);
        else CHECK(most <= std::size_t{upload::write_depth});
        CHECK(most_bytes <= sftp::max_unacked);
        if (!urgent) CHECK(most > 8); // the pipeline does fill
    }
}

TEST_CASE("upload: stopped mid-file it keeps the temp and the record; the next run sends only what wasn't acknowledged") {
    Server srv;
    Dir dir("resume");
    const std::string content = test::pattern(3u << 20);
    const fs::path local = local_file(dir, content);
    fetch::Connections connections;
    connections.configure("Fake", 2);
    rate::RateLimiter limiter;
    limiter.set_rate(1u << 20);
    std::uint64_t first_sent = 0;
    {
        auto pool = srv.fake.pool(2);
        auto u = upload_of(local, pool, dir);
        u.limiter = &limiter;
        std::stop_source stop;
        std::atomic<std::uint64_t> sent{0};
        u.on_sent = [&](std::uint64_t n) {
            sent += n;
            if (sent >= (1u << 20)) stop.request_stop();
        };
        const auto first = upload::run(u, connections, {}, stop.get_token());
        REQUIRE(first.outcome == upload::Outcome::stopped);
        CHECK(first.temp.empty()); // the record keeps it
        first_sent = sent;
    }
    const auto saved = upload::saved_progress(dir.records(), "Fake", "/up/f.bin");
    REQUIRE(saved);
    const auto [saved_done, saved_size] = saved.value_or(std::pair<std::uint64_t, std::uint64_t>{});
    CHECK(saved_done >= (1u << 20));
    CHECK(saved_done < content.size());
    CHECK(saved_done <= first_sent);
    CHECK(saved_size == content.size());
    CHECK(srv.temps().size() == 1);
    CHECK_FALSE(srv.file("/up/f.bin"));
    {
        std::scoped_lock lock(srv.fake.mutex);
        srv.fake.writes.clear();
    }

    limiter.set_rate(0);
    auto pool = srv.fake.pool(2);
    const auto second = upload::run(upload_of(local, pool, dir), connections, {}, {});
    REQUIRE_MESSAGE(second.outcome == upload::Outcome::done, second.error);
    CHECK(second.note.empty()); // went on, didn't start over
    CHECK(srv.file("/up/f.bin") == content);
    CHECK(srv.temps().empty());
    CHECK_FALSE(upload::saved_progress(dir.records(), "Fake", "/up/f.bin"));
    pool.reset();
    CHECK(srv.written() == content.size() - saved_done); // nothing acknowledged was sent again
}

TEST_CASE("upload: stopped before every range began, the next run still goes on from the record") {
    // Ranges not begun (done 0) sit past the temp's end; they mustn't make the temp look short.
    Server srv;
    Dir dir("resume-unbegun");
    const std::string content = test::pattern(6u << 20);
    const fs::path local = local_file(dir, content);
    fetch::Connections connections;
    connections.configure("Fake", 4);
    rate::RateLimiter limiter;
    limiter.set_rate(1u << 20);
    {
        auto pool = srv.fake.pool(4);
        auto u = upload_of(local, pool, dir);
        u.limiter = &limiter;
        // 4 ranges of 1.5 MiB (the first call sets the chunk size), then one stream only: range 0.
        std::atomic<int> calls{0};
        u.streams_cap = [&] { return calls++ == 0 ? 4 : 1; };
        std::stop_source stop;
        std::atomic<std::uint64_t> sent{0};
        u.on_sent = [&](std::uint64_t n) {
            sent += n;
            if (sent >= (1u << 20)) stop.request_stop();
        };
        REQUIRE(upload::run(u, connections, {}, stop.get_token()).outcome == upload::Outcome::stopped);
    }
    const auto record = upload::record_path(dir.records(), "Fake", "/up/f.bin");
    const auto text = util::read_file(record);
    REQUIRE(text);
    const auto rec = upload::record_from_json(text.value_or(std::string()));
    REQUIRE(rec);
    const auto& ranges = rec.value_or(upload::Record{}).state.ranges;
    CHECK(ranges.size() == 4);
    CHECK(std::ranges::count_if(ranges, [](const fetch::Range& r) { return r.done == 0; }) >= 1); // the case at hand
    const auto saved = upload::saved_progress(dir.records(), "Fake", "/up/f.bin");
    REQUIRE(saved);
    const std::uint64_t saved_done = saved.value_or(std::pair<std::uint64_t, std::uint64_t>{}).first;
    CHECK(saved_done >= (1u << 20));
    {
        std::scoped_lock lock(srv.fake.mutex);
        srv.fake.writes.clear();
    }
    limiter.set_rate(0);
    auto pool = srv.fake.pool(4);
    const auto second = upload::run(upload_of(local, pool, dir), connections, {}, {});
    REQUIRE_MESSAGE(second.outcome == upload::Outcome::done, second.error);
    CHECK(second.note.empty()); // went on, didn't start over
    CHECK(srv.file("/up/f.bin") == content);
    CHECK(srv.temps().empty());
    pool.reset();
    CHECK(srv.written() == content.size() - saved_done);
}

TEST_CASE("upload: a partial upload starts over when the local file changed or its temp is gone") {
    for (const bool gone : {false, true}) {
        CAPTURE(gone);
        Server srv;
        Dir dir(gone ? "temp-gone" : "local-changed");
        const fs::path local = local_file(dir, test::pattern(3u << 20));
        fetch::Connections connections;
        connections.configure("Fake", 2);
        rate::RateLimiter limiter;
        limiter.set_rate(1u << 20);
        {
            auto pool = srv.fake.pool(2);
            auto u = upload_of(local, pool, dir);
            u.limiter = &limiter;
            std::stop_source stop;
            std::atomic<std::uint64_t> sent{0};
            u.on_sent = [&](std::uint64_t n) {
                sent += n;
                if (sent >= (512u << 10)) stop.request_stop();
            };
            REQUIRE(upload::run(u, connections, {}, stop.get_token()).outcome == upload::Outcome::stopped);
        }
        const auto old_temps = srv.temps();
        REQUIRE(old_temps.size() == 1);
        std::string content = test::pattern(3u << 20);
        if (gone) {
            std::scoped_lock lock(srv.fake.store.mutex);
            srv.fake.store.unlink("/up/" + old_temps[0]);
            std::erase(srv.fake.store.dirs["/up"], old_temps[0]);
        } else {
            content[100] = 'x';
            REQUIRE(util::write_file_atomic(local, content));
            set_mtime(local, 1'760'000'000);
        }
        auto pool = srv.fake.pool(2);
        const auto r = upload::run(upload_of(local, pool, dir), connections, {}, {});
        REQUIRE_MESSAGE(r.outcome == upload::Outcome::done, r.error);
        CHECK(r.note.find("started over") != std::string::npos);
        CHECK(srv.file("/up/f.bin") == content);
        CHECK(srv.temps().empty()); // the old temp removed too
    }
}

TEST_CASE("upload: a file that changes while it is sent starts over once, then fails, its temp removed") {
    for (const int changes : {1, 2}) {
        CAPTURE(changes);
        Server srv;
        Dir dir(changes == 1 ? "changes-once" : "changes-twice");
        std::string content = test::pattern(3u << 20);
        const fs::path local = local_file(dir, content);
        auto pool = srv.fake.pool(2);
        fetch::Connections connections;
        connections.configure("Fake", 2);
        auto u = upload_of(local, pool, dir);
        std::mutex m;
        std::uint64_t sent = 0;
        int changed = 0;
        bool written = true;
        std::uint64_t next_change = content.size() / 2;
        // Halfway through each round, the file grows by a byte (another size, another time),
        // saved as programs save: a new file takes its name.
        u.on_sent = [&](std::uint64_t n) {
            std::scoped_lock lock(m);
            sent += n;
            if (changed >= changes || sent < next_change) return;
            content += 'z';
            written = written && util::write_file_atomic(local, content).has_value();
            ++changed;
            next_change = sent + content.size(); // in the next round
        };
        const auto r = upload::run(u, connections, {}, {});
        REQUIRE(written);
        CHECK(changed == changes);
        if (changes == 1) {
            REQUIRE_MESSAGE(r.outcome == upload::Outcome::done, r.error);
            CHECK(r.note.find("changed while uploading; started over") != std::string::npos);
            CHECK(srv.file("/up/f.bin") == content);
        } else {
            CHECK(r.outcome == upload::Outcome::failed);
            CHECK(r.error == util::path_to_utf8(local) + " changed while uploading");
            CHECK_FALSE(srv.file("/up/f.bin"));
            CHECK_FALSE(fs::exists(upload::record_path(dir.records(), "Fake", "/up/f.bin")));
        }
        CHECK(srv.temps().empty());
    }
}

TEST_CASE("upload: the speed limit is taken before each WRITE, in small ones") {
    Server srv;
    Dir dir("limit");
    const std::string content = test::pattern(1536u << 10);
    const fs::path local = local_file(dir, content);
    auto pool = srv.fake.pool(2);
    fetch::Connections connections;
    connections.configure("Fake", 2);
    rate::RateLimiter limiter;
    limiter.set_rate(1u << 20); // 1 MiB/s
    auto u = upload_of(local, pool, dir);
    u.limiter = &limiter;
    const auto started = std::chrono::steady_clock::now();
    const auto r = upload::run(u, connections, {}, {});
    const auto took = std::chrono::steady_clock::now() - started;
    REQUIRE_MESSAGE(r.outcome == upload::Outcome::done, r.error);
    CHECK(srv.file("/up/f.bin") == content);
    CHECK(took >= 900ms); // 1.5 MiB at 1 MiB/s, less the bucket's first quarter second
    u.pool.reset();
    pool.reset();
    REQUIRE_FALSE(srv.fake.writes.empty());
    for (const auto& w : srv.fake.writes) CHECK(w.length <= 128u * 1024); // an eighth of a second's worth
}

TEST_CASE("upload: a limit switched on with more on its way than it lets a connection keep drops the connection; it goes on over a new login") {
    Server srv;
    // A connection's 8 MiB stay on their way: each reply comes only as the stream waits for one,
    // so it looks at the new limit with megabytes out (with replies taken as they come, it looks
    // right after them, with little out: charged, the connection kept; seen so since chunk 6).
    srv.fake.held = true;
    srv.fake.release_one = true;
    srv.fake.max_write = 1u << 20;
    Dir dir("limit-on");
    const std::string content = test::pattern(40u << 20);
    const fs::path local = local_file(dir, content);
    auto pool = srv.fake.pool(2);
    fetch::Connections connections;
    connections.configure("Fake", 2);
    rate::RateLimiter limiter; // unlimited at the start
    auto u = upload_of(local, pool, dir);
    u.limiter = &limiter;
    std::atomic<std::uint64_t> sent{0};
    std::atomic<bool> switched{false};
    // From the stream that got the acknowledgement: it has megabytes on their way as it looks.
    u.on_sent = [&](std::uint64_t n) {
        sent += n;
        if (sent >= (4u << 20) && !switched.exchange(true)) limiter.set_rate(2u << 20);
    };
    upload::Result r;
    {
        std::jthread running([&] { r = upload::run(u, connections, {}, {}); });
        for (int i = 0; i < 2000 && srv.fake.logins.load() <= 2; ++i) std::this_thread::sleep_for(10ms);
        limiter.set_rate(0); // the rest at full speed
    }
    REQUIRE_MESSAGE(r.outcome == upload::Outcome::done, r.error);
    CHECK(switched);
    CHECK(srv.fake.logins.load() > 2); // dropped, and logged in again: not a failure
    CHECK(srv.file("/up/f.bin") == content);
}

TEST_CASE("upload: a limit switched on while each WRITE's reply comes before the next goes takes hold at once") {
    // Live on Main (chunk 6, L6a): a flush that waits for the SSH window takes the replies that
    // came meanwhile, so an unlimited stream never reached its caps and saw a limit switched on only
    // once its whole range had gone (90 MiB at full speed). Here each WRITE's send takes a while
    // and its reply is there at once.
    Server srv;
    srv.fake.write_pace = 2ms;
    srv.fake.max_write = 1u << 20; // a megabyte a send: far over the limit if it isn't looked at
    Dir dir("limit-on-paced");
    const std::string content = test::pattern(24u << 20);
    const fs::path local = local_file(dir, content);
    auto pool = srv.fake.pool(1);
    fetch::Connections connections;
    connections.configure("Fake", 1);
    rate::RateLimiter limiter; // unlimited at the start
    auto u = upload_of(local, pool, dir);
    u.limiter = &limiter;
    constexpr std::uint64_t rate = 512u << 10;
    std::atomic<std::uint64_t> sent{0};
    std::uint64_t sent_at_switch = 0;
    std::chrono::steady_clock::time_point switched_at;
    std::atomic<bool> switched{false};
    // Switched on from the stream's own thread, between two WRITEs, as soon as 2 MiB came back.
    u.on_sent = [&](std::uint64_t n) {
        sent += n;
        if (switched.load() || sent.load() < (2u << 20)) return;
        sent_at_switch = sent.load();
        switched_at = std::chrono::steady_clock::now();
        limiter.set_rate(rate);
        switched = true;
    };
    upload::Result r;
    std::uint64_t after = 0;
    double seconds = 0;
    {
        std::jthread running([&] { r = upload::run(u, connections, {}, {}); });
        for (int i = 0; i < 4000 && !switched.load(); ++i) std::this_thread::sleep_for(5ms);
        REQUIRE(switched.load());
        std::this_thread::sleep_until(switched_at + 1s);
        after = sent.load() - sent_at_switch;
        seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - switched_at).count();
        limiter.set_rate(0); // the rest at full speed
    }
    // At the limit from the switch on: its rate, the bucket's first quarter second and what the
    // stream had on its way, a WRITE or two (kept and charged, or dropped and sent again under the
    // limit). Not looked at, the rest of the file went: about 20 MiB.
    MESSAGE("acknowledged in ", seconds, " s after the switch: ", after, " bytes");
    CHECK(static_cast<double>(after) <= static_cast<double>(rate) * seconds + static_cast<double>(2u << 20));
    REQUIRE_MESSAGE(r.outcome == upload::Outcome::done, r.error);
    CHECK(srv.file("/up/f.bin") == content);
}

TEST_CASE("upload: a limit lowered with less on its way than the new one lets a connection keep charges it; the connection stays") {
    constexpr std::uint64_t before = std::uint64_t{8} * sftp::max_read;
    constexpr std::uint64_t after = std::uint64_t{1} << 20;
    REQUIRE(fetch::limited_in_flight(before, 1) <= fetch::limited_keep(after, 1));
    Server srv;
    srv.fake.latency = 60ms;
    Dir dir("limit-lower");
    const std::string content = test::pattern(2560u << 10);
    const fs::path local = local_file(dir, content);
    auto pool = srv.fake.pool(1);
    fetch::Connections connections;
    connections.configure("Fake", 1);
    rate::RateLimiter limiter;
    limiter.set_rate(before);
    auto u = upload_of(local, pool, dir);
    u.limiter = &limiter;
    std::atomic<std::uint64_t> sent{0};
    std::atomic<bool> lowered{false};
    u.on_sent = [&](std::uint64_t n) {
        sent += n;
        if (sent >= (1u << 20) && !lowered.exchange(true)) limiter.set_rate(after);
    };
    const auto started = std::chrono::steady_clock::now();
    const auto r = upload::run(u, connections, {}, {});
    const auto took = std::chrono::steady_clock::now() - started;
    REQUIRE_MESSAGE(r.outcome == upload::Outcome::done, r.error);
    CHECK(lowered);
    CHECK(srv.fake.logins.load() == 1);
    CHECK(srv.file("/up/f.bin") == content);
    // 1 MiB at about 2 MiB/s, then 1.5 MiB at 1 MiB/s, less the buckets' first quarter seconds.
    CHECK(took >= 1500ms);
}

TEST_CASE("upload: a file already there: ask, skip, replace (in one step, or remove then rename), if different, if newer, keep both") {
    using upload::Existing;
    using upload::Outcome;
    struct Case {
        Existing choice;
        bool posix_rename;
        std::uint32_t theirs_time; // the server's file's time; the local file's is 1'750'000'000
        std::string theirs;        // its bytes
        Outcome outcome;
        std::string placed;        // where the upload went ("": nowhere)
    };
    const std::string mine = test::pattern(100'000);
    const std::string same_size(mine.size(), 's');
    const auto old = [](Existing choice, bool posix_rename, std::uint32_t theirs_time, Outcome outcome, std::string placed) {
        return Case{.choice = choice, .posix_rename = posix_rename, .theirs_time = theirs_time, .theirs = "old", .outcome = outcome,
                    .placed = std::move(placed)};
    };
    for (const auto& c : {old(Existing::ask, true, 1'700'000'000, Outcome::exists, ""),
                          old(Existing::skip, true, 1'700'000'000, Outcome::skipped, ""),
                          old(Existing::replace, true, 1'700'000'000, Outcome::done, "/up/f.bin"),
                          old(Existing::replace, false, 1'700'000'000, Outcome::done, "/up/f.bin"),
                          Case{.choice = Existing::different, .posix_rename = true, .theirs_time = 1'750'000'001,
                               .theirs = same_size, .outcome = Outcome::skipped, .placed = ""},
                          old(Existing::different, true, 1'750'000'001, Outcome::done, "/up/f.bin"),
                          old(Existing::newer, true, 1'800'000'000, Outcome::skipped, ""),
                          old(Existing::newer, true, 1'700'000'000, Outcome::done, "/up/f.bin"),
                          old(Existing::keep_both, true, 1'700'000'000, Outcome::done, "/up/f (3).bin")}) {
        CAPTURE(upload::name(c.choice));
        CAPTURE(c.posix_rename);
        CAPTURE(c.theirs_time);
        Server srv;
        srv.fake.posix_rename = c.posix_rename;
        srv.put("/up/f.bin", c.theirs, c.theirs_time);
        srv.put("/up/f (2).bin", "an earlier copy");
        Dir dir("existing");
        const fs::path local = local_file(dir, mine);
        set_mtime(local, 1'750'000'000);
        auto pool = srv.fake.pool(2);
        fetch::Connections connections;
        connections.configure("Fake", 2);
        const auto before = srv.tree();
        const auto r = upload::run(upload_of(local, pool, dir, "/up/f.bin", c.choice), connections, {}, {});
        REQUIRE_MESSAGE(r.outcome == c.outcome, r.error);
        CHECK(r.placed == c.placed);
        CHECK(srv.temps().empty());
        pool.reset();
        if (c.placed.empty()) {
            REQUIRE(r.clash);
            const upload::Clash clash = r.clash.value_or(upload::Clash{});
            CHECK(clash.remote_size == c.theirs.size());
            CHECK(clash.remote_mtime == c.theirs_time);
            CHECK(clash.local_size == mine.size());
            CHECK(clash.local_mtime == 1'750'000'000);
            CHECK(srv.tree() == before); // nothing sent, nothing touched
            CHECK(srv.fake.writes.empty());
            continue;
        }
        CHECK(srv.file(c.placed) == mine);
        CHECK(srv.mtime(c.placed) == 1'750'000'000u);
        if (c.choice == Existing::keep_both) {
            CHECK(srv.file("/up/f.bin") == c.theirs);
            CHECK(srv.file("/up/f (2).bin") == "an earlier copy");
        }
        const auto& asked = srv.fake.requests;
        const bool removed = std::ranges::count(asked, "REMOVE") > 0;
        CHECK(removed == !c.posix_rename); // the old file removed first only without posix-rename
    }
}

TEST_CASE("upload: a folder of that name: Replace skips it with the reason, Keep both goes beside it") {
    Server srv({"/up", "/up/f.bin"});
    Dir dir("folder-there");
    const fs::path local = local_file(dir, "abc");
    auto pool = srv.fake.pool(2);
    fetch::Connections connections;
    connections.configure("Fake", 2);
    const auto skipped = upload::run(upload_of(local, pool, dir, "/up/f.bin", upload::Existing::replace), connections, {}, {});
    CHECK(skipped.outcome == upload::Outcome::skipped);
    CHECK(skipped.note == "a folder of that name is there");
    const auto beside = upload::run(upload_of(local, pool, dir, "/up/f.bin", upload::Existing::keep_both), connections, {}, {});
    REQUIRE_MESSAGE(beside.outcome == upload::Outcome::done, beside.error);
    CHECK(beside.placed == "/up/f (2).bin");
    CHECK(srv.file("/up/f (2).bin") == "abc");
    std::scoped_lock lock(srv.fake.store.mutex);
    CHECK(srv.fake.store.dirs.contains("/up/f.bin"));
}

TEST_CASE("upload: Keep both passes over the names other uploads will take, which their files then get") {
    Server srv;
    srv.put("/up/a.txt", "theirs");
    Dir dir("both-reserved");
    const fs::path a = local_file(dir, "mine a", "a.txt");
    const fs::path a2 = local_file(dir, "mine a2", "a (2).txt");
    auto pool = srv.fake.pool(2);
    fetch::Connections connections;
    connections.configure("Fake", 2);
    // Picked together: a.txt (Keep both) goes first, "a (2).txt" (new, Skip for all) after it.
    auto first = upload_of(a, pool, dir, "/up/a.txt", upload::Existing::keep_both);
    first.reserved = [](std::string_view path) { return path == "/up/a (2).txt"; };
    const auto beside = upload::run(first, connections, {}, {});
    REQUIRE_MESSAGE(beside.outcome == upload::Outcome::done, beside.error);
    CHECK(beside.placed == "/up/a (3).txt");
    const auto own = upload::run(upload_of(a2, pool, dir, "/up/a (2).txt", upload::Existing::skip), connections, {}, {});
    REQUIRE_MESSAGE(own.outcome == upload::Outcome::done, own.error);
    CHECK(srv.file("/up/a.txt") == "theirs");
    CHECK(srv.file("/up/a (2).txt") == "mine a2");
    CHECK(srv.file("/up/a (3).txt") == "mine a");
}

TEST_CASE("upload: a file that appears at the name while uploading gets the upload's choice; asked, the finished temp waits for the answer") {
    // The file appears with the first acknowledgement; the rename looks right before it (LSTAT),
    // so a server whose RENAME replaces (rename_overwrites) doesn't replace it either.
    using upload::Existing;
    for (const bool overwrites : {false, true}) {
        CAPTURE(overwrites);
        Server srv;
        srv.fake.rename_overwrites = overwrites;
        Dir dir(overwrites ? "late-overwrites" : "late");
        const std::string content = test::pattern(3u << 20);
        const fs::path local = local_file(dir, content);
        fetch::Connections connections;
        connections.configure("Fake", 2);
        std::atomic<bool> appeared{false};
        const auto appear = [&](std::uint64_t) {
            if (!appeared.exchange(true)) srv.put("/up/f.bin", "theirs");
        };
        for (const auto choice : {Existing::skip, Existing::ask}) {
            CAPTURE(upload::name(choice));
            appeared = false;
            auto pool = srv.fake.pool(2);
            auto u = upload_of(local, pool, dir, "/up/f.bin", choice);
            u.on_sent = appear;
            const auto r = upload::run(u, connections, {}, {});
            CHECK(srv.file("/up/f.bin") == "theirs");
            REQUIRE(r.clash);
            CHECK(r.clash.value().remote_size == 6u);
            if (choice == Existing::skip) {
                CHECK(r.outcome == upload::Outcome::skipped);
                CHECK(srv.temps().empty());
                CHECK_FALSE(upload::saved_progress(dir.records(), "Fake", "/up/f.bin"));
                std::scoped_lock lock(srv.fake.store.mutex);
                srv.fake.store.unlink("/up/f.bin");
                std::erase(srv.fake.store.dirs["/up"], "f.bin");
                continue;
            }
            // Asked: the complete temp and its record stay, and an answer goes on from them at once.
            CHECK(r.outcome == upload::Outcome::exists);
            CHECK(srv.temps().size() == 1);
            const auto saved = upload::saved_progress(dir.records(), "Fake", "/up/f.bin");
            REQUIRE(saved);
            CHECK(saved.value().first == content.size());
            pool.reset();
            {
                std::scoped_lock lock(srv.fake.mutex);
                srv.fake.writes.clear();
            }
            pool = srv.fake.pool(2);
            const auto replaced = upload::run(upload_of(local, pool, dir, "/up/f.bin", Existing::replace), connections, {}, {});
            REQUIRE_MESSAGE(replaced.outcome == upload::Outcome::done, replaced.error);
            CHECK(srv.file("/up/f.bin") == content);
            CHECK(srv.temps().empty());
            pool.reset();
            CHECK(srv.written() == 0);
        }
    }
}

TEST_CASE("upload: a server that refuses writes says why plainly, and nothing is left behind") {
    struct Case {
        std::string label;
        bool read_only;
        sftp::Code as;
        std::uint64_t quota;
        std::string remote;
        std::string error; // "" : checked by its start
    };
    for (const auto& c : {Case{.label = "Box.Guest: Failure, statvfs read-only",
                               .read_only = true,
                               .as = sftp::Code::failure,
                               .quota = 0,
                               .remote = "/up/f.bin",
                               .error = "Fake is read-only for this account: nothing can be uploaded to /up"},
                          Case{.label = "a folder this account can't write",
                               .read_only = true,
                               .as = sftp::Code::permission_denied,
                               .quota = 0,
                               .remote = "/up/f.bin",
                               .error = "Fake doesn't let this account write in /up"},
                          Case{.label = "no such folder",
                               .read_only = false,
                               .as = sftp::Code::ok,
                               .quota = 0,
                               .remote = "/nope/f.bin",
                               .error = "there is no folder /nope on Fake"},
                          Case{.label = "full",
                               .read_only = false,
                               .as = sftp::Code::ok,
                               .quota = std::uint64_t{1} << 20,
                               .remote = "/up/f.bin",
                               .error = ""}}) {
        CAPTURE(c.label);
        Server srv;
        srv.fake.statvfs = true;
        srv.fake.read_only = c.read_only;
        srv.fake.read_only_as = c.as;
        srv.fake.quota_bytes = c.quota;
        srv.put("/up/other", "x");
        const auto before = srv.tree();
        Dir dir("refused");
        const std::string content = test::pattern(3u << 20);
        const fs::path local = local_file(dir, content);
        auto pool = srv.fake.pool(2);
        fetch::Connections connections;
        connections.configure("Fake", 2);
        const auto r = upload::run(upload_of(local, pool, dir, c.remote), connections, {}, {});
        CHECK(r.outcome == upload::Outcome::failed);
        if (!c.error.empty()) {
            CHECK(r.error == c.error);
            CHECK(srv.tree() == before);
            CHECK_FALSE(upload::saved_progress(dir.records(), "Fake", c.remote));
        } else {
            // The disk filled: what was free and what it needed, as the server's statvfs says.
            CHECK(r.error.starts_with("Fake:/up is full: "));
            CHECK(r.error.find(" free, ") != std::string::npos);
            CHECK(r.error.ends_with(" needed"));
            CHECK_FALSE(srv.file("/up/f.bin"));
            // A failure keeps the partial upload for a retry, which a cancel removes (discard).
            REQUIRE(srv.temps().size() == 1);
            REQUIRE(upload::discard(*pool, dir.records(), "Fake", "/up/f.bin"));
            CHECK(srv.temps().empty());
            CHECK(srv.tree() == before);
            CHECK_FALSE(upload::saved_progress(dir.records(), "Fake", "/up/f.bin"));
        }
    }
}

TEST_CASE("upload: the write check says whether the folder takes files, what is free and what the server offers") {
    {
        Server srv;
        srv.fake.statvfs = true;
        srv.fake.quota_bytes = std::uint64_t{64} << 20;
        srv.fake.max_write = 1u << 20; // more than grab ever sends
        srv.put("/up/other", "x");
        const auto before = srv.tree();
        auto pool = srv.fake.pool(2);
        const auto p = upload::probe(*pool, "Fake", "/up");
        REQUIRE(p);
        CHECK(p->writable);
        CHECK(p->why.empty());
        CHECK(p->free == (std::uint64_t{64} << 20) - 4096); // whole blocks: the 1 byte there takes one
        CHECK(p->posix_rename);
        CHECK(p->max_write == sftp::max_write);
        CHECK(p->left.empty());
        CHECK(srv.tree() == before); // the probe file went again
    }
    {
        Server srv;
        srv.fake.posix_rename = false;
        auto pool = srv.fake.pool(2);
        const auto p = upload::probe(*pool, "Fake", "/up");
        REQUIRE(p);
        CHECK(p->writable);
        CHECK_FALSE(p->free);
        CHECK_FALSE(p->posix_rename);
        CHECK(p->max_write == upload::fallback_write);
    }
    struct Case {
        bool read_only;
        sftp::Code as;
        bool statvfs;
        std::string dir;
        std::string why;
    };
    const std::string read_only = "Fake is read-only for this account: nothing can be uploaded to /up";
    const std::string denied = "Fake doesn't let this account write in /up";
    for (const auto& c : {Case{.read_only = true, .as = sftp::Code::failure, .statvfs = true, .dir = "/up", .why = read_only},
                          Case{.read_only = true, .as = sftp::Code::permission_denied, .statvfs = true, .dir = "/up", .why = denied},
                          Case{.read_only = true, .as = sftp::Code::permission_denied, .statvfs = false, .dir = "/up", .why = denied},
                          // Nothing tells a read-only file system apart.
                          Case{.read_only = true, .as = sftp::Code::failure, .statvfs = false, .dir = "/up", .why = "Failure"},
                          Case{.read_only = false, .as = sftp::Code::ok, .statvfs = true, .dir = "/nope",
                               .why = "there is no folder /nope on Fake"}}) {
        CAPTURE(c.why);
        CAPTURE(c.statvfs);
        Server srv;
        srv.fake.read_only = c.read_only;
        srv.fake.read_only_as = c.as;
        srv.fake.statvfs = c.statvfs;
        const auto before = srv.tree();
        auto pool = srv.fake.pool(2);
        const auto p = upload::probe(*pool, "Fake", c.dir);
        REQUIRE(p);
        CHECK_FALSE(p->writable);
        CHECK(p->why == c.why);
        CHECK(srv.tree() == before);
    }
    // No login: not known.
    Server srv;
    srv.fake.login_fails = budget::Failure::fatal;
    auto pool = srv.fake.pool(2);
    const auto p = upload::probe(*pool, "Fake", "/up");
    REQUIRE_FALSE(p);
    CHECK(p.error() == "Fake refused the login: the key was refused");
}

TEST_CASE("upload: cancel removes the temp and the record; a small file's temp comes back to the caller, removed only when it is grab's") {
    Server srv;
    srv.put("/up/keep.grabpart", "not grab's");
    const auto before = srv.tree();
    Dir dir("cancel");
    fetch::Connections connections;
    connections.configure("Fake", 2);
    rate::RateLimiter limiter;
    limiter.set_rate(512u << 10);
    for (const std::size_t size : {std::size_t{3} << 20, std::size_t{1} << 20}) {
        CAPTURE(size);
        const fs::path local = local_file(dir, test::pattern(size));
        auto pool = srv.fake.pool(2);
        auto u = upload_of(local, pool, dir);
        u.limiter = &limiter;
        std::stop_source stop;
        u.on_sent = [&](std::uint64_t) { stop.request_stop(); };
        const auto r = upload::run(u, connections, {}, stop.get_token());
        REQUIRE(r.outcome == upload::Outcome::stopped);
        REQUIRE(srv.temps().size() == 1);
        if (size >= upload::small_limit) {
            CHECK(r.temp.empty());
            REQUIRE(upload::saved_progress(dir.records(), "Fake", "/up/f.bin"));
            REQUIRE(upload::discard(*pool, dir.records(), "Fake", "/up/f.bin"));
            CHECK_FALSE(upload::saved_progress(dir.records(), "Fake", "/up/f.bin"));
            CHECK(fs::is_empty(dir.records()));
        } else {
            CHECK(r.temp == "/up/" + srv.temps()[0]);
            CHECK_FALSE(upload::remove_temp(*pool, "/up/keep.grabpart"));
            REQUIRE(upload::remove_temp(*pool, r.temp));
            REQUIRE(upload::remove_temp(*pool, r.temp)); // gone already: no error
        }
        CHECK(srv.temps().empty());
        CHECK(srv.tree() == before);
    }
    // Nothing recorded: nothing to do.
    auto pool = srv.fake.pool(2);
    CHECK(upload::discard(*pool, dir.records(), "Fake", "/up/none"));
}

TEST_CASE("upload: discarding a partial upload waits for a connection without holding its record") {
    Server srv;
    Dir dir("discard-wait");
    const fs::path local = local_file(dir, test::pattern(3u << 20));
    auto pool = srv.fake.pool(1);
    fetch::Connections connections;
    connections.configure("Fake", 1);
    {
        auto u = upload_of(local, pool, dir);
        std::stop_source stop;
        u.on_sent = [&](std::uint64_t) { stop.request_stop(); };
        REQUIRE(upload::run(u, connections, {}, stop.get_token()).outcome == upload::Outcome::stopped);
    }
    REQUIRE(upload::saved_progress(dir.records(), "Fake", "/up/f.bin"));
    fs::path lock_file = upload::record_path(dir.records(), "Fake", "/up/f.bin");
    lock_file.replace_extension(".lock");

    // The record held (a Retry's run has it): the connection goes back unbroken, the record stays.
    {
        auto held = util::lock_file(lock_file, 0ms);
        REQUIRE(held);
        const auto busy = upload::discard(*pool, dir.records(), "Fake", "/up/f.bin");
        CHECK_FALSE(busy);
        CHECK(busy.error_or("") == "Fake:/up/f.bin is still being uploaded");
    }
    CHECK(srv.temps().size() == 1);
    REQUIRE(upload::saved_progress(dir.records(), "Fake", "/up/f.bin"));
    const int logins = srv.fake.logins;

    // The pool's one connection in use (a transfer's): while discard waits for it, the record is
    // free for a Retry's run (review, chunk 3: it was locked for that whole wait).
    auto in_use = pool->acquire();
    REQUIRE(in_use);
    std::atomic<bool> over{false};
    std::expected<void, std::string> gone;
    std::jthread discarding([&] {
        gone = upload::discard(*pool, dir.records(), "Fake", "/up/f.bin");
        over = true;
    });
    for (int i = 0; i < 20; ++i) {
        CHECK_FALSE(over.load());
        CHECK(util::lock_file(lock_file, 0ms)); // taken and given back at once
        std::this_thread::sleep_for(10ms);
    }
    in_use.reset();
    discarding.join();
    CHECK_MESSAGE(gone, gone.error_or(""));
    CHECK_FALSE(upload::saved_progress(dir.records(), "Fake", "/up/f.bin"));
    CHECK(srv.temps().empty());
    CHECK(srv.fake.logins == logins); // the one connection, never broken
}

TEST_CASE("upload: a small file stopped as its temp is made hands the temp back") {
    Server srv;
    srv.put("/up/other", "x");
    const auto before = srv.tree();
    Dir dir("stop-made");
    const fs::path local = local_file(dir, test::pattern(1u << 20));
    auto pool = srv.fake.pool(2);
    fetch::Connections connections;
    connections.configure("Fake", 2);
    auto u = upload_of(local, pool, dir);
    std::stop_source stop;
    // The stop comes as the OPEN that makes the temp goes: the temp is made all the same.
    u.on_command = [&](const std::vector<std::string>& c) {
        if (c.size() >= 2 && c[1] == "create") stop.request_stop();
    };
    const auto r = upload::run(u, connections, {}, stop.get_token());
    REQUIRE(r.outcome == upload::Outcome::stopped);
    REQUIRE(srv.temps().size() == 1);
    CHECK(r.temp == "/up/" + srv.temps()[0]);
    REQUIRE(upload::remove_temp(*pool, r.temp));
    CHECK(srv.tree() == before);
}

TEST_CASE("upload: a server that won't set the file's time still gets the file, with a note") {
    for (const std::size_t size : {std::size_t{1} << 20, std::size_t{3} << 20}) {
        CAPTURE(size);
        Server srv;
        srv.fake.no_setstat = true;
        Dir dir("no-setstat");
        const std::string content = test::pattern(size);
        const fs::path local = local_file(dir, content);
        auto pool = srv.fake.pool(2);
        fetch::Connections connections;
        connections.configure("Fake", 2);
        const auto r = upload::run(upload_of(local, pool, dir), connections, {}, {});
        REQUIRE_MESSAGE(r.outcome == upload::Outcome::done, r.error);
        CHECK(r.placed == "/up/f.bin");
        CHECK(r.note == "the server kept its own time for it; Replace if newer / if different will see it as changed");
        CHECK(srv.file("/up/f.bin") == content);
        CHECK(srv.temps().empty());
        CHECK_FALSE(upload::saved_progress(dir.records(), "Fake", "/up/f.bin"));
    }
}

TEST_CASE("upload: a link that breaks right after the rename went through: the file is found in place, done once") {
    for (const bool replace : {false, true}) {
        CAPTURE(replace);
        Server srv;
        if (replace) srv.put("/up/f.bin", "old");
        Dir dir(replace ? "rename-broke-replace" : "rename-broke");
        const std::string content = test::pattern(3u << 20);
        const fs::path local = local_file(dir, content);
        set_mtime(local, 1'750'000'000);
        auto pool = srv.fake.pool(2);
        fetch::Connections connections;
        connections.configure("Fake", 2);
        srv.fake.break_after_rename = true; // posix-rename for Replace, RENAME for a new name
        const auto r = upload::run(upload_of(local, pool, dir, "/up/f.bin", replace ? upload::Existing::replace : upload::Existing::ask),
                                   connections, {}, {});
        REQUIRE_MESSAGE(r.outcome == upload::Outcome::done, r.error);
        CHECK_FALSE(srv.fake.break_after_rename.load()); // it did break
        CHECK(r.placed == "/up/f.bin");
        CHECK(r.note.empty());
        CHECK(srv.file("/up/f.bin") == content);
        CHECK(srv.mtime("/up/f.bin") == 1'750'000'000u);
        CHECK(srv.temps().empty());
        CHECK_FALSE(upload::saved_progress(dir.records(), "Fake", "/up/f.bin"));
        pool.reset();
        CHECK(srv.written() == content.size()); // sent once
    }
}

TEST_CASE("upload: a local file that is a link to another uploads once, not seen as changed") {
    Dir dir("link");
    const std::string content = test::pattern(3u << 20);
    const fs::path target = local_file(dir, content, "target.bin");
    set_mtime(target, 1'750'000'000);
    const fs::path link = dir.path / "f.bin";
    std::error_code ec;
    fs::create_symlink(target, link, ec);
    if (ec) {
        MESSAGE("no file link could be made here (", ec.message(), "): skipped");
        return;
    }
    Server srv;
    auto pool = srv.fake.pool(2);
    fetch::Connections connections;
    connections.configure("Fake", 2);
    const auto r = upload::run(upload_of(link, pool, dir), connections, {}, {});
    REQUIRE_MESSAGE(r.outcome == upload::Outcome::done, r.error);
    CHECK(r.note.empty());
    CHECK(srv.file("/up/f.bin") == content);
    CHECK(srv.mtime("/up/f.bin") == 1'750'000'000u); // the target's time
    pool.reset();
    CHECK(srv.written() == content.size()); // once
}

TEST_CASE("upload: one place is uploaded to by one run at a time") {
    Server srv;
    Dir dir("locked");
    const fs::path local = local_file(dir, test::pattern(3u << 20));
    auto pool = srv.fake.pool(2);
    fetch::Connections connections;
    connections.configure("Fake", 2);
    const fs::path record = upload::record_path(dir.records(), "Fake", "/up/f.bin");
    fs::path lock_file = record;
    lock_file.replace_extension(".lock");
    {
        auto held = util::lock_file(lock_file, 0ms);
        REQUIRE(held);
        const auto r = upload::run(upload_of(local, pool, dir), connections, {}, {});
        CHECK(r.outcome == upload::Outcome::failed);
        CHECK(r.error == "Fake:/up/f.bin is being uploaded already, by another grab");
        CHECK(srv.temps().empty());
    }
    const auto r = upload::run(upload_of(local, pool, dir), connections, {}, {});
    CHECK(r.outcome == upload::Outcome::done);
}

TEST_CASE("upload: the fake's store: sessions share it, and an open handle keeps a file whose name went") {
    test::FakeFiles store;
    store.dirs["/up"] = {".", ".."};
    test::FakeServer a;
    test::FakeServer b;
    a.shared = &store;
    b.shared = &store;
    sftp::Client ca(a);
    sftp::Client cb(b);
    ca.start();
    cb.start();
    auto h = sftp::open_file(ca, "/up/f", sftp::OpenFlags::write | sftp::OpenFlags::creat | sftp::OpenFlags::excl);
    REQUIRE(h);
    auto hb = sftp::open_file(cb, "/up/f", sftp::OpenFlags::write);
    REQUIRE(hb);
    REQUIRE(sftp::write_range(cb, *hb, 3, 3, [](std::uint64_t, std::uint32_t) { return std::string_view("def"); }));
    REQUIRE(sftp::write_range(ca, *h, 0, 3, [](std::uint64_t, std::uint32_t) { return std::string_view("abc"); }));
    CHECK(store.files["/up/f"] == "abcdef");
    // Renamed by one session, still written by the other's handle.
    REQUIRE(sftp::rename(cb, "/up/f", "/up/g"));
    REQUIRE(sftp::write_range(ca, *h, 6, 1, [](std::uint64_t, std::uint32_t) { return std::string_view("!"); }));
    CHECK(store.files["/up/g"] == "abcdef!");
    // Removed: the handles keep its bytes, the name doesn't come back.
    REQUIRE(sftp::remove(cb, "/up/g"));
    REQUIRE(sftp::write_range(ca, *h, 7, 1, [](std::uint64_t, std::uint32_t) { return std::string_view("?"); }));
    const auto attrs = sftp::fstat(ca, *h);
    REQUIRE(attrs);
    CHECK(attrs->size == 8u);
    std::string got;
    REQUIRE(sftp::read_range(cb, *hb, 0, 8, [&](std::uint64_t, std::string_view bytes) { got += bytes; }));
    CHECK(got == "abcdef!?");
    CHECK(store.files.empty());
    CHECK(store.dirs["/up"] == std::vector<std::string>{".", ".."});
    REQUIRE(sftp::close_file(ca, *h));
    REQUIRE(sftp::close_file(cb, *hb));
    CHECK(store.used() == 0);
}
