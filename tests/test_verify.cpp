#include <doctest/doctest.h>

// Checking downloads through grab's own SSH client (verify.hpp; RCLONE-REPLACEMENT.md, phase 5):
// the server's md5sum and grab's MD5 of each copy, compared by path, on an in-memory server
// (fake_md5sum.hpp); what proves a mismatch, what keeps a copy, and what the row and the command
// say.

#include "config.hpp"
#include "engine.hpp"
#include "fake_command.hpp"
#include "fake_md5sum.hpp"
#include "fetch.hpp"
#include "folder.hpp"
#include "folder_job.hpp"
#include "local_name.hpp"
#include "progress_line.hpp"
#include "rclone_encoding.hpp"
#include "ssh.hpp"
#include "ssh_pool.hpp"
#include "util.hpp"
#include "verify.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace grab;
using namespace std::string_literals;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
using verify::Verdict;

namespace {

// The time the fake server gives its files (1700000000), as a download records it.
constexpr std::string_view server_time = "2023-11-14T22:13:20Z";
constexpr std::int64_t server_ns = std::int64_t{1'700'000'000} * 1'000'000'000;

// A server (test::Box) with files under /data, their downloaded copies in a folder of the test's
// own, and what a verification of them needs.
struct Check {
    test::Box box;
    engine::Context ctx;
    fetch::Connections connections;
    fs::path dir;
    std::shared_ptr<ssh::Pool> pool;

    explicit Check(std::string_view name, int hashes_at_once = 1) {
        dir = fs::temp_directory_path() / "grab-tests-verify" / name;
        fs::remove_all(dir);
        fs::create_directories(dir);
        ctx.settings.name = "Fake";
        ctx.remote.host = "fake.example";
        connections.configure("Fake", 4);
        pool = box.pool(4, hashes_at_once);
    }
    ~Check() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    Check(const Check&) = delete;
    Check& operator=(const Check&) = delete;

    // `content` on the server at /data/<path> (`path` in grab's form), and its copy at
    // dir/<local> (else the name grab gives it) holding `copy` (else the same) with the server's
    // time, as the download left it.
    verify::File file(const std::string& path, const std::string& content, fs::path local = {},
                      std::optional<std::string> copy = std::nullopt) {
        box.files["/data/" + rclone_enc::raw_path(path)] = content;
        const fs::path at = dir / (local.empty() ? local::relative(path) : local);
        REQUIRE(util::place_file(at, copy.value_or(content), server_ns));
        return verify::File{.path = path, .local = at, .size = content.size(), .modtime = std::string(server_time)};
    }
    [[nodiscard]] verify::Request request(std::vector<verify::File> files = {}) {
        verify::Request r;
        r.ctx = &ctx;
        r.pool = pool;
        r.remote_dir = "/data";
        r.files = std::move(files);
        r.connections = &connections;
        r.server = "Fake";
        r.id = 1;
        r.many = true;
        return r;
    }
};

std::vector<Verdict> verdicts(const verify::Report& r) {
    std::vector<Verdict> v;
    for (const auto& f : r.files) v.push_back(f.verdict);
    return v;
}

void touch(const fs::path& p) {
    std::error_code ec;
    fs::last_write_time(p, fs::file_time_type::clock::now(), ec);
    REQUIRE_FALSE(ec);
}

} // namespace

TEST_CASE("verify: each copy by its own path, whatever its local name") {
    Check c("paths");
    // Keep both, a device name, a case clash, both spellings of é, a name that isn't UTF-8, a line
    // break, a backslash, a quote, a leading '-'.
    std::vector<verify::File> files{
        c.file("plain.mkv", "plain"),
        c.file("kept.txt", "kept", "kept (2).txt"),
        c.file("CON.txt", "device"),
        c.file("README", "upper"),
        c.file("readme", "lower", "readme (2)"),
        c.file("caf\xC3\xA9.txt", "nfc"),
        c.file("cafe\xCC\x81.txt", "nfd", util::path_from_utf8("cafe\xCC\x81 (2).txt")),
        c.file("caf‛E9.txt", "latin1"),
        c.file("new␊line.txt", "line"),
        c.file("back\\slash.txt", "back"),
        c.file("it's.txt", "quote"),
        c.file("-dash.txt", "dash"),
    };
    auto r = c.request(files);
    std::vector<std::vector<std::string>> lines;
    std::vector<std::size_t> progress;
    r.on_command = [&](const std::vector<std::string>& argv) { lines.push_back(argv); };
    r.on_progress = [&](std::size_t done, std::size_t total) {
        CHECK(total == files.size());
        progress.push_back(done);
    };
    const auto report = verify::run(r, {});
    CHECK(verdicts(report) == std::vector<Verdict>(files.size(), Verdict::same));
    CHECK(verify::summary(report, true) == "12 files verified (MD5)");
    CHECK(progress.size() == files.size());
    CHECK(progress.back() == files.size());
    // Absolute raw paths in one command; -v names its folder, not every file.
    REQUIRE(c.box.hashes().size() == 1);
    CHECK(c.box.hashes()[0].find("'/data/caf\xE9.txt'") != std::string::npos);
    CHECK(c.box.hashes()[0].find("'/data/new\nline.txt'") != std::string::npos);
    CHECK(lines == std::vector<std::vector<std::string>>{{"exec", "md5sum", "-z", "/data", "(12 files)"}});
    // Nothing left any seat taken.
    CHECK(c.connections.active("Fake", 1) == 0);

    // One file: its own line, a control byte in grab's form so it can't rewrite the console.
    Check one("one");
    auto single = one.request();
    single.remote_dir.clear();
    auto f = one.file("esc␛[2J.mkv", "x", "copy (2).mkv");
    f.path = "/data/" + f.path;
    lines.clear();
    single.on_command = [&](const std::vector<std::string>& argv) { lines.push_back(argv); };
    const auto checked = verify::file(single, f, {});
    CHECK(verdicts(checked) == std::vector<Verdict>{Verdict::same});
    CHECK(verify::summary(checked) == "checksum verified (MD5)");
    CHECK(lines == std::vector<std::vector<std::string>>{{"exec", "md5sum", "-z", "/data/esc␛[2J.mkv"}});
}

TEST_CASE("verify: only a proven mismatch differs; a changed copy on either side is kept") {
    Check c("proof");
    // A byte flipped on the server after the download with its time put back: the sums differ,
    // the server's file looks as it did, the copy is as the download left it.
    auto flipped = c.file("flipped.bin", "hello world", {}, "hellO world");
    // Changed on the server since: another time (and size).
    auto server_changed = c.file("server.bin", "new content!", {}, "old content");
    server_changed.size = 11;
    c.box.mtimes["/data/server.bin"] = 1'700'000'100;
    // The copy changed since: a player wrote its tags, say.
    auto mine_changed = c.file("mine.bin", "abcdef", {}, "abcdeX");
    touch(mine_changed.local);
    // The copy is gone; the server's is gone.
    verify::File gone{.path = "gone.bin", .local = c.dir / "gone.bin", .size = 3, .modtime = std::string(server_time)};
    auto server_gone = c.file("away.bin", "away");
    c.box.files.erase("/data/away.bin");
    auto same = c.file("same.bin", "same");

    std::vector<std::vector<std::string>> lines;
    auto r = c.request({flipped, server_changed, mine_changed, gone, server_gone, same});
    r.on_command = [&](const std::vector<std::string>& argv) { lines.push_back(argv); };
    const auto report = verify::run(r, {});
    CHECK(verdicts(report) == std::vector<Verdict>{Verdict::differs, Verdict::unchecked, Verdict::unchecked,
                                                   Verdict::missing, Verdict::unchecked, Verdict::same});
    CHECK(report.files[1].why == "the server's copy changed since the download");
    CHECK(report.files[2].why == "your copy changed since the download");
    CHECK(report.files[4].why == "no sum from the server (md5sum: /data/away.bin: No such file or directory)");
    // One STAT for each differing sum.
    CHECK(std::ranges::count(lines, std::vector<std::string>{"sftp", "stat", "/data/flipped.bin"}) == 1);
    CHECK(std::ranges::count(lines, std::vector<std::string>{"sftp", "stat", "/data/same.bin"}) == 0);
    // The check deletes nothing itself: the caller does, for the proven ones.
    CHECK(fs::exists(flipped.local));
    CHECK(fs::exists(mine_changed.local));
    CHECK(verify::summary(report, true) == "1 of 6 files differ from the server's copy (MD5); 1 downloaded copy is gone");

    verify::Report one;
    one.files = {report.files[0]};
    CHECK(verify::summary(one) == "differs from the server's copy (MD5)");
    one.files = {report.files[1]};
    CHECK(verify::summary(one) == "not verified: the server's copy changed since the download");
    one.files = {report.files[3]};
    CHECK(verify::summary(one) == "the downloaded copy is gone");
}

TEST_CASE("verify: the copy's own look, as the download left it") {
    Check c("look");
    const auto f = c.file("a.bin", "12345");
    CHECK(verify::copy_unchanged(f.local, 5, server_time));
    CHECK_FALSE(verify::copy_unchanged(f.local, 6, server_time));
    CHECK_FALSE(verify::copy_unchanged(c.dir / "none", 5, server_time));
    // Within 2 s (FAT keeps times to 2 s), not further.
    CHECK(verify::copy_unchanged(f.local, 5, "2023-11-14T22:13:21Z"));
    CHECK(verify::copy_unchanged(f.local, 5, "2023-11-14T22:13:18Z"));
    CHECK_FALSE(verify::copy_unchanged(f.local, 5, "2023-11-14T22:13:25Z"));
    // A download that knew no time set none: the size alone.
    CHECK(verify::copy_unchanged(f.local, 5, ""));
    CHECK_FALSE(verify::copy_unchanged(f.local, 5, "not a time"));
    touch(f.local);
    CHECK_FALSE(verify::copy_unchanged(f.local, 5, server_time));
}

TEST_CASE("verify: a server that can't check keeps every copy, not verified") {
    struct Case {
        const char* name;
        std::function<void(Check&)> make;
        std::string says;
    };
    const std::vector<Case> cases{
        {.name = "no md5sum",
         .make = [](Check& c) { c.box.shell = test::Box::Shell::restricted; },
         .says = R"(the server can't compute checksums (its answer to `md5sum -z ''\-` was "Command not found.)"},
        {.name = "a Windows shell",
         .make = [](Check& c) { c.box.shell = test::Box::Shell::windows; },
         .says = R"(the server can't compute checksums (its answer to `md5sum -z ''\-` was "md5sum: ''\-: No such file)"},
        {.name = "refused (SFTP only)",
         .make = [](Check& c) { c.box.refuse = true; },
         .says = "the server refused to run the command"},
        {.name = "no login",
         .make = [](Check& c) { c.box.no_login = ssh::Error(budget::Failure::fatal, "h refused the login: it accepted none of ssh-agent's keys"); },
         .says = "h refused the login: it accepted none of ssh-agent's keys"},
        {.name = "rclone.conf's shell_type",
         .make = [](Check& c) { c.ctx.remote.shell_type = "powershell"; },
         .says = "the server can't compute checksums (shell_type = powershell in rclone.conf)"},
        {.name = "a server that takes too long",
         .make = [](Check& c) {
             c.box.also = [](test::FakeCommand& cmd, std::string_view command) {
                 if (!test::is_probe(command)) cmd.forever = true;
             };
         },
         .says = "the server took too long"},
    };
    for (const auto& k : cases) {
        CAPTURE(k.name);
        Check c("cant");
        k.make(c);
        const auto a = c.file("a.mkv", "aaa");
        const auto b = c.file("b.mkv", "bbb");
        auto r = c.request({a, b});
        r.deadline = 100ms;
        const auto report = verify::run(r, {});
        CHECK(verdicts(report) == std::vector<Verdict>{Verdict::unchecked, Verdict::unchecked});
        CHECK_MESSAGE(report.files[0].why.find(k.says) != std::string::npos, report.files[0].why);
        CHECK(fs::exists(a.local));
        CHECK(fs::exists(b.local));
        CHECK(verify::summary(report, true).starts_with("not verified: "));
    }

    // No login could be made at all (engine::ssh_target failed): every copy kept, saying why.
    Check none("none");
    auto r = none.request({none.file("a.mkv", "aaa")});
    r.pool = nullptr;
    r.no_pool = "Fake: its host and user are needed";
    const auto report = verify::run(r, {});
    CHECK(report.files == std::vector<verify::Result>{{Verdict::unchecked, "Fake: its host and user are needed"}});
    CHECK(none.box.logins == 0);
}

TEST_CASE("verify: a copy isn't read to the end once its server side failed") {
    // A big copy (512 MiB of zeros, no disk writes: NTFS reads them past the written length).
    Check c("no-login-big");
    const fs::path big = c.dir / "big.mkv";
    REQUIRE(util::place_file(big, "x", server_ns));
    fs::resize_file(big, std::uint64_t{512} << 20);
    const auto full_from = std::chrono::steady_clock::now();
    REQUIRE(util::md5_file(big));
    const auto full = std::chrono::steady_clock::now() - full_from;

    c.box.files["/data/big.mkv"] = "x";
    c.box.no_login = ssh::Error(budget::Failure::fatal, "h refused the login: it accepted none of ssh-agent's keys");
    const auto at = std::chrono::steady_clock::now();
    const auto report = verify::run(c.request({verify::File{.path = "big.mkv", .local = big, .size = 1, .modtime = std::string(server_time)}}), {});
    const auto took = std::chrono::steady_clock::now() - at;
    CHECK(report.files == std::vector<verify::Result>{{Verdict::unchecked, "h refused the login: it accepted none of ssh-agent's keys"}});
    CHECK_MESSAGE(took < full / 2, std::chrono::duration_cast<std::chrono::milliseconds>(took).count(), " ms vs ",
                  std::chrono::duration_cast<std::chrono::milliseconds>(full).count(), " ms");
    CHECK(fs::exists(big));
}

TEST_CASE("verify: a pool made only to verify closes its idle connections at the end") {
    for (const bool close : {false, true}) {
        CAPTURE(close);
        Check c(close ? "close-idle" : "keep-idle");
        auto r = c.request({c.file("a.mkv", "aaa")});
        r.close_idle_after = close;
        CHECK(verify::run(r, {}).files == std::vector<verify::Result>{{Verdict::same, ""}});
        CHECK((c.pool->open() == 0) == close);
    }
}

TEST_CASE("verify: a stop keeps the copies, unchecked") {
    Check before("stop-before");
    std::stop_source stopped;
    stopped.request_stop();
    const auto a = before.file("a.mkv", "aaa");
    const auto report = verify::run(before.request({a}), stopped.get_token());
    CHECK(report.files == std::vector<verify::Result>{{Verdict::unchecked, "stopped"}});
    CHECK(fs::exists(a.local));
    CHECK(before.connections.active("Fake", 1) == 0);

    Check during("stop-during");
    during.box.also = [](test::FakeCommand& cmd, std::string_view command) {
        if (!test::is_probe(command)) cmd.forever = true;
    };
    const auto b = during.file("b.mkv", "bbb");
    std::stop_source stop;
    std::jthread stopper([&] {
        std::this_thread::sleep_for(100ms);
        stop.request_stop();
    });
    const auto at = std::chrono::steady_clock::now();
    const auto cut = verify::run(during.request({b}), stop.get_token());
    CHECK(std::chrono::steady_clock::now() - at < 5s);
    CHECK(cut.files == std::vector<verify::Result>{{Verdict::unchecked, "stopped"}});
    CHECK(fs::exists(b.local));
}

TEST_CASE("verify: a folder's files are marked as their results come; a stop keeps the finished ones") {
    // Two hash commands at once: without -z a name holding a backslash goes alone, and that one
    // never ends here, while the others' command does.
    Check c("folder", 2);
    c.box.z = false;
    c.box.also = [](test::FakeCommand& cmd, std::string_view command) {
        if (command.find("slow") != std::string_view::npos) cmd.forever = true;
    };
    folder::Job job;
    job.ctx = &c.ctx;
    job.connections = &c.connections;
    job.server = "Fake";
    job.id = 3;
    job.remote_path = "/data";
    job.dest = c.dir.parent_path() / "folder-dest";
    fs::remove_all(job.dest);
    folder::Tree tree;
    std::mutex m;
    // The files in the job's folder (DEST\data) as the download left them.
    const fs::path dir = folder::job_dir(job);
    auto add = [&](const std::string& path, const std::string& content, std::optional<std::string> copy = std::nullopt,
                   bool fetched = true) {
        c.box.files["/data/" + rclone_enc::raw_path(path)] = content;
        auto f = folder::make_file(path, content.size(), std::string(server_time));
        f.status = folder::State::done;
        f.fetched = fetched;
        REQUIRE(util::place_file(folder::target_of(dir, f), copy.value_or(content), server_ns));
        tree.files.push_back(std::move(f));
    };
    add("a.mkv", "aaa");
    add("sub/b.mkv", "bbb");
    add("bad.mkv", "good", "baad");
    add("slow\\name.mkv", "slow");
    add("there.mkv", "there", std::nullopt, false); // already there, not fetched: never checked
    tree.files.push_back(folder::make_file("gone.mkv", 4, std::string(server_time)));
    tree.files.back().status = folder::State::done;
    tree.files.back().fetched = true;
    c.box.files["/data/gone.mkv"] = "gone";

    std::stop_source stop;
    std::atomic<std::size_t> done{0};
    std::jthread stopper([&] {
        // The gone one, a.mkv and sub/b.mkv have their results (bad.mkv's mismatch waits to be
        // proven at the end): stop then.
        while (done < 3) std::this_thread::sleep_for(5ms);
        stop.request_stop();
    });
    verify::Request base;
    base.pool = c.pool;
    base.on_progress = [&](std::size_t n, std::size_t total) {
        CHECK(total == 5);
        done = n;
    };
    const auto checked = folder::verify_files(job, tree, m, std::move(base), stop.get_token());
    stopper.join();
    CHECK(checked.files == 5);
    auto file = [&](std::string_view path) -> folder::File& {
        return *std::ranges::find(tree.files, path, &folder::File::path);
    };
    CHECK(file("a.mkv").verified);
    CHECK(file("sub/b.mkv").verified);
    // Gone: failed, nothing to delete; a retry fetches it.
    CHECK(file("gone.mkv").status == folder::State::failed);
    CHECK(file("gone.mkv").error == "the downloaded copy is gone");
    CHECK_FALSE(file("gone.mkv").fetched);
    // Stopped, the slow one and the mismatch not yet proven: kept, done, checked by the next run.
    for (const auto* kept : {"slow\\name.mkv", "bad.mkv"}) {
        CAPTURE(kept);
        CHECK(file(kept).status == folder::State::done);
        CHECK(file(kept).fetched);
        CHECK_FALSE(file(kept).verified);
        CHECK(fs::exists(folder::target_of(dir, file(kept))));
    }
    CHECK_FALSE(file("there.mkv").verified);

    // The next run checks only what is left.
    c.box.also = {};
    verify::Request again;
    again.pool = c.pool;
    const auto rest = folder::verify_files(job, tree, m, std::move(again), {});
    CHECK(rest.files == 2);
    CHECK(file("slow\\name.mkv").verified);
    // Proven to differ: deleted, failed, fetched again by a retry.
    CHECK(file("bad.mkv").status == folder::State::failed);
    CHECK(file("bad.mkv").error == "differs from the server's copy; deleted");
    CHECK_FALSE(file("bad.mkv").fetched);
    CHECK_FALSE(fs::exists(dir / "bad.mkv"));
    CHECK(rest.summary() == "1 of 2 files differ from the server's copy (MD5)");
    fs::remove_all(job.dest);
}

TEST_CASE("verify: what the row and the command say") {
    using R = verify::Result;
    verify::Report ok;
    ok.files = {R{Verdict::same, {}}};
    CHECK(verify::summary(ok) == "checksum verified (MD5)");
    CHECK(verify::summary(ok, true) == "1 file verified (MD5)"); // a folder's files
    ok.files.assign(13, R{Verdict::same, {}});
    CHECK(verify::summary(ok) == "13 files verified (MD5)");

    verify::Report bad = ok;
    bad.files[0].verdict = Verdict::differs;
    bad.files[1].verdict = Verdict::differs;
    CHECK(verify::summary(bad) == "2 of 13 files differ from the server's copy (MD5)");
    verify::Report one_bad;
    one_bad.files = {R{Verdict::differs, {}}};
    CHECK(verify::summary(one_bad) == "differs from the server's copy (MD5)");
    CHECK(verify::summary(one_bad, true) == "1 file differs from the server's copy (MD5)");

    verify::Report partly = ok;
    for (int i = 0; i < 3; ++i) partly.files[i] = R{Verdict::unchecked, "the server took too long"};
    CHECK(verify::summary(partly) == "10 files verified (MD5); 3 could not be checked (the server took too long)");
    partly.files[0].why = "your copy changed since the download";
    CHECK(verify::summary(partly) ==
          "10 files verified (MD5); 3 could not be checked (your copy changed since the download, and other reasons)");

    verify::Report none;
    none.files.assign(2, R{Verdict::unchecked, "the server can't compute checksums (shell_type = cmd in rclone.conf)"});
    CHECK(verify::summary(none, true) == "not verified: the server can't compute checksums (shell_type = cmd in rclone.conf)");
    none.files.resize(1);
    CHECK(verify::summary(none) == "not verified: the server can't compute checksums (shell_type = cmd in rclone.conf)");
}

TEST_CASE("verify: a folder whose files failed still says what was verified: the row after the failure, the command before") {
    Check c("failed-folder");
    folder::Job job;
    job.ctx = &c.ctx;
    job.connections = &c.connections;
    job.server = "Fake";
    job.id = 4;
    job.remote_path = "/data";
    job.dest = c.dir / "dest";
    folder::Tree tree;
    std::mutex m;
    const fs::path dir = folder::job_dir(job);
    for (const std::string name : {"a.mkv", "b.mkv"}) {
        c.box.files["/data/" + name] = name;
        auto f = folder::make_file(name, name.size(), std::string(server_time));
        f.status = folder::State::done;
        f.fetched = true;
        REQUIRE(util::place_file(folder::target_of(dir, f), name, server_ns));
        tree.files.push_back(std::move(f));
    }
    // One the download couldn't fetch (rclone's engine and a name that isn't UTF-8, a lost link).
    tree.files.push_back(folder::make_file("c.mkv", 5, std::string(server_time)));
    tree.files.back().status = folder::State::failed;
    tree.files.back().error = "connection lost";

    verify::Request base;
    base.pool = c.pool;
    const auto checked = folder::verify_files(job, tree, m, std::move(base), {});
    REQUIRE(checked.files == 2);
    const auto o = folder::outcome(tree);
    REQUIRE(o.failed == 1);
    // The window: a failed row shows only its error, so what was verified goes after it.
    CHECK(folder::failed_text(o) == "1 file failed. c.mkv: connection lost");
    CHECK(folder::failed_text(o, checked.summary()) == "1 file failed. c.mkv: connection lost · 2 files verified (MD5)");
    // The command: the summary first, as a finished line, then the failure.
    CHECK(progress::folder_failed(folder::failed_text(o), checked.summary() + " in 0:01") ==
          "  2 files verified (MD5) in 0:01\n"
          "grab: error: 1 file failed. c.mkv: connection lost\n"
          "grab: the other files are on disk; run the same command again to retry\n");
    // Nothing verified (Verify downloads off, or a stop): the failure alone, as before.
    CHECK(progress::folder_failed("2 files failed. c.mkv: connection lost", "") ==
          "grab: error: 2 files failed. c.mkv: connection lost\n"
          "grab: the other files are on disk; run the same command again to retry\n");
}
