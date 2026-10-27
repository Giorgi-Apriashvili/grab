#include <doctest/doctest.h>

// Searches, file lists and Browse on engine = grab (RCLONE-REPLACEMENT.md, phase 4): the callers
// wired to grab's own SSH client, on in-memory servers with SFTP and a shell.

#include "browse.hpp"
#include "cli.hpp"
#include "config.hpp"
#include "conn_budget.hpp"
#include "engine.hpp"
#include "fake_command.hpp"
#include "fake_sftp.hpp"
#include "file_list.hpp"
#include "process.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_find.hpp"
#include "ssh_pool.hpp"

#include <atomic>
#include <cstddef>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace grab;
using namespace std::string_literals;

namespace {

// A server with SFTP over a tree in memory and a shell answering commands as `answer` sets them
// up (by default an SFTP-only account: nothing, exit 0, so the check walks). It records the
// commands it ran.
struct Server {
    std::map<std::string, std::string> files;
    std::map<std::string, std::vector<std::string>> dirs;
    std::map<std::string, std::string> links;
    std::set<std::string> denied;
    std::function<void(test::FakeCommand&, std::string_view command)> answer = [](test::FakeCommand&, std::string_view) {};
    bool never_eof = false; // READDIR past the last name gets empty NAMEs, not EOF
    std::mutex mutex;
    std::vector<std::string> commands;
    std::atomic<int> logins{0};
    std::atomic<int> dropped{0}; // the server has closed the connections of logins up to this one

    struct Session : ssh::Session {
        explicit Session(Server& s) : server(s), login(s.logins.load()), client(fake) {
            fake.shared_files = &s.files;
            fake.dirs = s.dirs;
            fake.links = s.links;
            fake.denied = s.denied;
            fake.never_eof = s.never_eof;
            client.start();
        }
        sftp::Client& sftp() override {
            fake.dropped = login <= server.dropped;
            return client;
        }
        std::expected<ssh::ExecResult, ssh::ExecError> exec(std::string_view command,
                                                             const ssh::ExecOptions& options) override {
            if (login <= server.dropped) throw ssh::Error(budget::Failure::other, "the server closed the connection");
            test::FakeCommand c;
            server.answer(c, command);
            auto r = ssh::run_command(c, command, options);
            std::scoped_lock lock(server.mutex);
            server.commands.emplace_back(command);
            return r;
        }
        bool reusable() override { return client.pending() == 0; }
        Server& server;
        int login;
        test::FakeServer fake;
        sftp::Client client;
    };

    std::shared_ptr<ssh::Pool> pool(int limit = 4) {
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
        if (slash != std::string::npos && slash > 0) dirs[path.substr(0, slash)].push_back(path.substr(slash + 1));
        dirs[path];
    }
    // find works: the check's answer, and `records` (NUL-ended) with `err` and `status` for a find.
    void with_find(std::vector<std::string> records, std::string err = {}, int status = 0) {
        answer = [records = std::move(records), err = std::move(err), status](test::FakeCommand& c, std::string_view command) {
            if (command == ssh::find_probe) {
                c.out = {"grab-find-ok\n"};
                return;
            }
            c.out = records;
            c.err = err;
            c.status = status;
        };
    }
    std::vector<std::string> ran() {
        std::scoped_lock lock(mutex);
        return commands;
    }
};

// The tree the tests search: /data with a show's folder and episode, a dot-folder, a folder the
// server won't list, and a link to a file elsewhere.
void fill(Server& s) {
    s.dir("/data");
    s.dir("/data/tv");
    s.dir("/data/tv/Lioness");
    s.file("/data/tv/Lioness/Lioness.S03E08.mkv", std::string(300, 'x'));
    s.file("/data/tv/lioness-notes.txt", "n");
    s.dir("/data/.hidden");
    s.file("/data/.hidden/lioness.secret", "s");
    s.dir("/data/locked");
    s.denied.insert("/data/locked");
    s.dir("/elsewhere");
    s.file("/elsewhere/real.mkv", "12345");
    s.dirs["/data"].emplace_back("lioness-link.mkv");
    s.links["/data/lioness-link.mkv"] = "/elsewhere/real.mkv";
}

engine::Context grab_context() {
    engine::Context ctx;
    ctx.settings.name = "Fake";
    ctx.settings.rclone_remote = "Fake";
    ctx.settings.search_roots = {"/data"};
    ctx.settings.engine = Engine::grab;
    ctx.engine = Engine::grab;
    ctx.grab_method = GrabMethod::probe;
    return ctx;
}

std::vector<std::string> paths_of(const std::vector<engine::Hit>& hits) {
    std::vector<std::string> out;
    for (const auto& h : hits) out.push_back(h.path);
    return out;
}

} // namespace

TEST_CASE("grab engine search: where find works, one find for both kinds, its notes, the check asked once") {
    Server s;
    s.with_find({"d\t1700000000.0000000000\t/data/tv/Lioness\0"s,
                 "f\t300\t1697354554.5000000000\t/data/tv/Lioness/Lioness.S03E08.mkv\0"s,
                 "f\t1\t1700000000.0\t/data/tv/lioness-notes.txt\0"s},
                "find: '/data/locked': Permission denied\n", 1);
    auto pool = s.pool();
    const auto ctx = grab_context();
    engine::SearchRequest req;
    req.target = "lioness";
    req.grab.pool = pool;
    std::vector<ssh::Lookup> ways;
    req.grab.on_lookup = [&](const ssh::Lookup& way) { ways.push_back(way); };
    std::size_t progress = 0;
    req.grab.progress = [&](std::size_t n) { progress = n; };
    std::vector<std::string> shown;
    const auto hook = [&](const std::vector<std::string>& argv) { shown.push_back(argv.front()); };

    const auto both = engine::search_both(ctx, req, {}, hook);
    REQUIRE_MESSAGE(both, both.error_or(""));
    CHECK(paths_of(both->first.hits) == std::vector<std::string>{"/data/tv/Lioness"});
    // Ranked as ever: both start with the word; the shallower first.
    CHECK(paths_of(both->second.hits) ==
          std::vector<std::string>{"/data/tv/lioness-notes.txt", "/data/tv/Lioness/Lioness.S03E08.mkv"});
    CHECK(both->second.hits[1].size == 300U);
    CHECK(both->second.hits[1].modified == 1697354554);
    CHECK_FALSE(both->first.hits[0].size);
    CHECK(both->first.notes == std::vector<std::string>{"/data/locked: permission denied"});
    CHECK(both->second.notes == both->first.notes);
    CHECK(progress == 3);
    REQUIRE(ways.size() == 1);
    CHECK(ways[0].find);
    CHECK(shown == std::vector<std::string>{"exec"});
    auto ran = s.ran();
    REQUIRE(ran.size() == 2);
    CHECK(ran[0] == ssh::find_probe);
    // One find printing both kinds, the name filtered on the server.
    CHECK(ran[1].find("-type d") != std::string::npos);
    CHECK(ran[1].find("-type f") != std::string::npos);
    CHECK(ran[1].find("-iname '*lioness*'") != std::string::npos);

    // A second search asks the check no more; one kind prints only that kind.
    req.mode = Mode::file;
    const auto files = engine::search(ctx, req);
    REQUIRE_MESSAGE(files, files.error_or(""));
    CHECK(files->hits.size() == 2);
    ran = s.ran();
    REQUIRE(ran.size() == 3);
    CHECK(ran[2].find("-type d") == std::string::npos);
    CHECK(s.logins == 1); // the check's connection, kept for the finds
}

TEST_CASE("grab engine search: without find, one walk matching as lsf does, dot-names and depth as configured") {
    Server s;
    fill(s);
    auto pool = s.pool();
    auto ctx = grab_context();
    engine::SearchRequest req;
    req.target = "lioness";
    req.grab.pool = pool;
    int seats = 0;
    req.grab.walk_seat = [&] {
        ++seats;
        return 2;
    };
    std::vector<ssh::Lookup> ways;
    req.grab.on_lookup = [&](const ssh::Lookup& way) { ways.push_back(way); };
    std::atomic<std::size_t> looked{0};
    req.grab.progress = [&](std::size_t n) { looked = n; };
    std::vector<std::vector<std::string>> shown;
    const auto hook = [&](const std::vector<std::string>& argv) { shown.push_back(argv); };

    const auto both = engine::search_both(ctx, req, {}, hook);
    REQUIRE_MESSAGE(both, both.error_or(""));
    CHECK(paths_of(both->first.hits) == std::vector<std::string>{"/data/tv/Lioness"});
    // The link to a file lists as that file; the dot-folder's file is left out.
    CHECK(paths_of(both->second.hits) == std::vector<std::string>{"/data/lioness-link.mkv",
                                                                  "/data/tv/lioness-notes.txt",
                                                                  "/data/tv/Lioness/Lioness.S03E08.mkv"});
    CHECK(both->second.hits[0].size == 5U);
    CHECK(both->second.hits[2].size == 300U);
    CHECK(both->second.hits[2].modified == 1700000000);
    CHECK(both->first.notes == std::vector<std::string>{"/data/locked: permission denied"});
    CHECK(seats == 1);
    CHECK(looked > 0);
    REQUIRE(ways.size() == 1);
    CHECK_FALSE(ways[0].find);
    CHECK(ways[0].answer == "nothing, exit 0");
    REQUIRE(shown.size() == 1);
    CHECK(shown[0] == std::vector<std::string>{"sftp", "walk", "/data", "--max-depth", "4"});
    CHECK(s.ran() == std::vector<std::string>{std::string(ssh::find_probe)}); // asked once; no find

    // Looking for a dot-name: hidden ones are walked too.
    req.target = ".hidden";
    req.mode = Mode::folder;
    auto hidden = engine::search(ctx, req);
    REQUIRE_MESSAGE(hidden, hidden.error_or(""));
    CHECK(paths_of(hidden->hits) == std::vector<std::string>{"/data/.hidden"});
    // The depth: one level shows only /data's own entries.
    req.target = "lioness";
    req.mode = Mode::file;
    req.depth = 1;
    auto shallow = engine::search(ctx, req);
    REQUIRE_MESSAGE(shallow, shallow.error_or(""));
    CHECK(paths_of(shallow->hits) == std::vector<std::string>{"/data/lioness-link.mkv"});
    CHECK(shallow->max_depth == 1);
    // Every root failing is an error.
    ctx.settings.search_roots = {"/data/locked", "/missing"};
    req.depth.reset();
    auto none = engine::search(ctx, req);
    REQUIRE_FALSE(none);
    CHECK(none.error() == "cannot list /data/locked: permission denied; /missing: no such file or folder");
    // find = rclone or walk asks nothing.
    Server quiet;
    fill(quiet);
    ctx = grab_context();
    ctx.grab_method = GrabMethod::walk;
    req.grab.pool = quiet.pool();
    auto walked = engine::search(ctx, req);
    REQUIRE_MESSAGE(walked, walked.error_or(""));
    CHECK(walked->hits.size() == 3);
    CHECK(quiet.ran().empty());
}

TEST_CASE("grab engine search: a path target is one STAT, whatever the method") {
    Server s;
    fill(s);
    s.with_find({});
    auto pool = s.pool();
    const auto ctx = grab_context();
    engine::SearchRequest req;
    req.grab.pool = pool;
    std::vector<std::vector<std::string>> shown;
    const auto hook = [&](const std::vector<std::string>& argv) { shown.push_back(argv); };

    req.target = "/data/tv/Lioness/Lioness.S03E08.mkv";
    auto file = engine::search(ctx, req, {}, hook);
    REQUIRE_MESSAGE(file, file.error_or(""));
    REQUIRE(file->hits.size() == 1);
    CHECK(file->hits[0].path == "/data/tv/Lioness/Lioness.S03E08.mkv");
    CHECK(file->hits[0].size == 300U);
    CHECK(shown == std::vector<std::vector<std::string>>{{"sftp", "stat", "/data/tv/Lioness/Lioness.S03E08.mkv"}});
    // A link: what it leads to.
    req.target = "/data/lioness-link.mkv";
    file = engine::search(ctx, req);
    REQUIRE_MESSAGE(file, file.error_or(""));
    REQUIRE(file->hits.size() == 1);
    CHECK(file->hits[0].size == 5U);
    // A folder: a hit of the folder kind only; both kinds at once tell them apart.
    req.target = "/data/tv/Lioness/";
    CHECK(engine::search(ctx, req)->hits.empty());
    auto both = engine::search_both(ctx, req);
    REQUIRE_MESSAGE(both, both.error_or(""));
    CHECK(paths_of(both->first.hits) == std::vector<std::string>{"/data/tv/Lioness"});
    CHECK(both->second.hits.empty());
    // Nothing there is no hit rather than an error; "." and ".." never are.
    req.target = "/data/tv/missing.mkv";
    auto missing = engine::search(ctx, req);
    REQUIRE_MESSAGE(missing, missing.error_or(""));
    CHECK(missing->hits.empty());
    req.mode = Mode::folder;
    req.target = "/data/tv/..";
    auto dots = engine::search(ctx, req);
    REQUIRE_MESSAGE(dots, dots.error_or(""));
    CHECK(dots->hits.empty());
    CHECK(s.ran().empty()); // no check, no find
}

TEST_CASE("grab engine search: a stopped search says so; a login that fails is the error") {
    Server s;
    fill(s);
    auto pool = s.pool();
    const auto ctx = grab_context();
    engine::SearchRequest req;
    req.target = "lioness";
    req.grab.pool = pool;
    std::stop_source stop;
    stop.request_stop();
    proc::RunOptions run;
    run.stop = stop.get_token();
    auto stopped = engine::search(ctx, req, run);
    REQUIRE_FALSE(stopped);
    CHECK(stopped.error() == "search cancelled");

    ssh::Pool::Options o = ssh::Pools::download_options(2);
    o.idle_timer = false;
    req.grab.pool = std::make_shared<ssh::Pool>(
        []() -> std::unique_ptr<ssh::Session> { throw ssh::Error(budget::Failure::fatal, "the password was refused"); },
        o);
    auto refused = engine::search(ctx, req);
    REQUIRE_FALSE(refused);
    CHECK(refused.error() == "the password was refused");
}

TEST_CASE("grab engine: the way chosen in words, for -v and the window") {
    auto ctx = grab_context();
    CHECK(engine::lookup_text(ctx, ssh::Lookup{.find = true, .answer = "GNU find"}) == "with find (GNU find)");
    CHECK(engine::lookup_text(ctx, ssh::Lookup{.find = false, .answer = "\"Command not found\", exit 1"}) ==
          "by listing folders (its answer to grab's find check: \"Command not found\", exit 1)");
    ctx.remote.host = "u1.your-storagebox.de";
    CHECK(engine::lookup_text(ctx, ssh::Lookup{}) == "by listing folders (a Storage Box has no find, so grab doesn't ask)");
    ctx.settings.find = FindMethod::rclone; // find = walk
    CHECK(engine::lookup_text(ctx, ssh::Lookup{}) == "by listing folders, as [Fake]'s find setting says");
}

TEST_CASE("grab engine file lists: find or the walk, the same entries in list format v2") {
    Server walked;
    fill(walked);
    const auto ctx = grab_context();
    engine::GrabSide side;
    side.pool = walked.pool();
    auto list = file_list::fetch(ctx, 1234, false, {}, {}, {}, side);
    REQUIRE_MESSAGE(list, list.error_or(""));
    CHECK(list->fetched == 1234);
    CHECK(list->fingerprint == file_list::fingerprint(ctx));
    std::vector<std::string> paths;
    for (const auto& e : list->entries) paths.push_back(e.path);
    CHECK(paths == std::vector<std::string>{"/data/lioness-link.mkv", "/data/locked", "/data/tv", "/data/tv/Lioness",
                                            "/data/tv/Lioness/Lioness.S03E08.mkv", "/data/tv/lioness-notes.txt"});
    CHECK(list->entries[0] == file_list::Entry{"/data/lioness-link.mkv", false, 5, 1700000000});
    CHECK(list->entries[2] == file_list::Entry{"/data/tv", true, std::nullopt, 1700000000});
    CHECK(list->notes == std::vector<std::string>{"/data/locked: permission denied"});
    // Saved as before: the notes aren't.
    auto back = file_list::from_text(file_list::to_text(*list));
    REQUIRE(back);
    CHECK(back->entries == list->entries);
    CHECK(back->notes.empty());

    Server found;
    found.with_find({"d\t1700000000.0\t/data/tv\0"s, "f\t1\t1700000000.0\t/data/tv/caf\xE9.txt\0"s});
    side.pool = found.pool();
    list = file_list::fetch(ctx, 1234, false, {}, {}, {}, side);
    REQUIRE_MESSAGE(list, list.error_or(""));
    REQUIRE(list->entries.size() == 2);
    CHECK(list->entries[1] == file_list::Entry{"/data/tv/caf‛E9.txt", false, 1, 1700000000});
    const auto ran = found.ran();
    REQUIRE(ran.size() == 2);
    CHECK(ran[1].find("-iname") == std::string::npos); // everything, both kinds
    CHECK(ran[1].find("\\( -type d -printf") != std::string::npos);
}

TEST_CASE("grab engine file lists: the fingerprint names the engine and the find setting") {
    auto ctx = grab_context();
    const auto probe = file_list::fingerprint(ctx);
    CHECK(probe.find("|grab+probe|") != std::string::npos);
    auto other = ctx;
    other.grab_method = GrabMethod::walk;
    CHECK(file_list::fingerprint(other).find("|grab+walk|") != std::string::npos);
    other.grab_method = GrabMethod::find;
    CHECK(file_list::fingerprint(other).find("|grab+find|") != std::string::npos);
    // The rclone engine's lists keep theirs.
    other = ctx;
    other.engine = Engine::rclone;
    other.method = FindMethod::ssh;
    CHECK(file_list::fingerprint(other) == "1|Fake|ssh|@:22|depth=4|hidden=skip|roots=/data");
}

TEST_CASE("grab engine Browse: links followed, dot-names left out, odd names in grab's form, sorted") {
    Server s;
    fill(s);
    s.dir("/data/caf\xE9");
    s.file("/data/caf\xE9/x.txt", "x");
    s.dirs["/data"].emplace_back("to-tv");
    s.links["/data/to-tv"] = "/data/tv";
    s.dirs["/data"].emplace_back("broken");
    s.links["/data/broken"] = "/nowhere";
    auto pool = s.pool(1);
    const auto ctx = grab_context();
    engine::GrabSide side;
    side.pool = pool;
    std::vector<std::vector<std::string>> shown;
    const auto hook = [&](const std::vector<std::string>& argv) { shown.push_back(argv); };

    auto items = browse::list_live(ctx, "/data", {}, hook, side);
    REQUIRE(items);
    std::vector<std::string> names;
    for (const auto& it : *items) names.push_back(it.name + (it.dir ? "/" : ""));
    CHECK(names == std::vector<std::string>{"caf‛E9/", "locked/", "to-tv/", "tv/", "lioness-link.mkv"});
    CHECK(items->back().size == 5U);
    CHECK(items->back().path == "/data/lioness-link.mkv");
    CHECK(shown == std::vector<std::vector<std::string>>{{"sftp", "readdir", "/data"}});
    // A folder whose name isn't UTF-8 opens in grab's form (review G60).
    items = browse::list_live(ctx, "/data/caf‛E9", {}, {}, side);
    REQUIRE(items);
    REQUIRE(items->size() == 1);
    CHECK(items->front().path == "/data/caf‛E9/x.txt");
    CHECK(pool->idle() == 1); // every reply in: the connection is used again
    CHECK(s.logins == 1);
    CHECK(s.ran().empty());
}

TEST_CASE("grab engine Browse: errors by kind; a file path is told by one STAT") {
    Server s;
    fill(s);
    auto pool = s.pool(1);
    const auto ctx = grab_context();
    engine::GrabSide side;
    side.pool = pool;
    auto failed = browse::list_live(ctx, "/data/missing", {}, {}, side);
    REQUIRE_FALSE(failed);
    CHECK(failed.error() == browse::ListError{browse::ListError::Kind::not_found, "no such file or folder"});
    failed = browse::list_live(ctx, "/data/locked", {}, {}, side);
    REQUIRE_FALSE(failed);
    CHECK(failed.error().kind == browse::ListError::Kind::denied);
    failed = browse::list_live(ctx, "/data/tv/lioness-notes.txt", {}, {}, side);
    REQUIRE_FALSE(failed);
    CHECK(failed.error() == browse::ListError{browse::ListError::Kind::file,
                                              "/data/tv/lioness-notes.txt is a file, not a folder"});
    failed = browse::list_live(ctx, "/data/lioness-link.mkv", {}, {}, side); // a link to a file
    REQUIRE_FALSE(failed);
    CHECK(failed.error().kind == browse::ListError::Kind::file);
    // Stopped: no answer, and the connection isn't left with replies on their way.
    std::stop_source stop;
    stop.request_stop();
    proc::RunOptions run;
    run.stop = stop.get_token();
    failed = browse::list_live(ctx, "/data", run, {}, side);
    REQUIRE_FALSE(failed);
    CHECK(failed.error().message == "stopped");

    // The rclone engine's errors get their kinds from rclone's text.
    CHECK(browse::rclone_error_kind("error listing: directory not found") == browse::ListError::Kind::not_found);
    CHECK(browse::rclone_error_kind("couldn't list: permission denied") == browse::ListError::Kind::denied);
    CHECK(browse::rclone_error_kind("connection refused") == browse::ListError::Kind::other);
}

TEST_CASE("grab engine: a server that answers READDIR with empty NAMEs, never EOF, ends Browse and the walk") {
    Server s;
    fill(s);
    s.never_eof = true;
    auto pool = s.pool(1);
    const auto ctx = grab_context();
    engine::GrabSide side;
    side.pool = pool;
    auto items = browse::list_live(ctx, "/data/tv", {}, {}, side);
    REQUIRE(items);
    CHECK(items->size() == 2);
    auto list = file_list::fetch(ctx, 1234, false, {}, {}, {}, side);
    REQUIRE_MESSAGE(list, list.error_or(""));
    CHECK(list->entries.size() == 6);
    CHECK(pool->idle() == 1); // every reply in
}

TEST_CASE("grab engine: a reused connection the server closed while idle is dropped and the next one tried") {
    Server s;
    fill(s);
    s.with_find({"f\t1\t1700000000.0\t/data/tv/lioness-notes.txt\0"s});
    auto pool = s.pool(2);
    const auto ctx = grab_context();
    engine::GrabSide side;
    side.pool = pool;
    REQUIRE(browse::list_live(ctx, "/data", {}, {}, side));
    REQUIRE(s.logins == 1);

    // Browse: the idle one is dead; a new login reads the folder.
    s.dropped = 1;
    auto items = browse::list_live(ctx, "/data/tv", {}, {}, side);
    REQUIRE(items);
    CHECK(items->size() == 2);
    CHECK(s.logins == 2);

    // A path target's STAT.
    s.dropped = 2;
    engine::SearchRequest req;
    req.target = "/data/tv/lioness-notes.txt";
    req.grab = side;
    auto hits = engine::search(ctx, req);
    REQUIRE_MESSAGE(hits, hits.error_or(""));
    CHECK(paths_of(hits->hits) == std::vector<std::string>{"/data/tv/lioness-notes.txt"});
    CHECK(s.logins == 3);

    // The find check and find, each on a connection that died while idle.
    s.dropped = 3;
    req.target = "lioness";
    hits = engine::search(ctx, req);
    REQUIRE_MESSAGE(hits, hits.error_or(""));
    CHECK(paths_of(hits->hits) == std::vector<std::string>{"/data/tv/lioness-notes.txt"});
    CHECK(s.logins == 4);

    // A new login that breaks is the error: no second try.
    s.dropped = 100;
    auto failed = browse::list_live(ctx, "/data", {}, {}, side);
    REQUIRE_FALSE(failed);
    CHECK(failed.error().message == "fake server: the server closed the connection");
}
