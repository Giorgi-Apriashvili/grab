#include "config.hpp"
#include "engine.hpp"
#include "folder.hpp"
#include "folder_job.hpp"
#include "gui_state.hpp"
#include "server_conf.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

using namespace grab;
using namespace std::string_view_literals;
using folder::State;

TEST_CASE("folder listing: files only, sorted, sizes and times") {
    const auto files = folder::parse_listing(
        R"([{"Path":"b.mp4","Name":"b.mp4","Size":42642378,"ModTime":"2023-05-06T15:18:12+04:00","IsDir":false},)"
        R"({"Path":"sub","Name":"sub","Size":-1,"IsDir":true},)"
        R"({"Path":"sub/a #1 [x].mp4","Name":"a #1 [x].mp4","Size":5,"ModTime":"2023-01-01T00:00:00Z","IsDir":false},)"
        R"({"Path":"a.txt","Size":0,"IsDir":false}])");
    REQUIRE(files.has_value());
    REQUIRE(files->size() == 3);
    CHECK((*files)[0] == folder::FolderFile{"a.txt", 0, ""});
    CHECK((*files)[1] == folder::FolderFile{"b.mp4", 42642378, "2023-05-06T15:18:12+04:00"});
    CHECK((*files)[2].path == "sub/a #1 [x].mp4");
    CHECK_FALSE(folder::parse_listing("{}").has_value());
    CHECK(folder::parse_listing("[]")->empty());
}

TEST_CASE("big files take the resumable path") {
    CHECK_FALSE(folder::is_big(0));
    CHECK_FALSE(folder::is_big(folder::big_file_threshold - 1));
    CHECK(folder::is_big(folder::big_file_threshold));
    CHECK(folder::is_big(std::uint64_t{20} << 30));
}

TEST_CASE("rclone's file list and its per-file log lines") {
    // Taken literally with --files-from-raw, so filter characters need no escaping.
    CHECK(folder::files_from_text({"a #1 [x].mp4", "sub/b.mp4", "line\nbreak"}) ==
          "a #1 [x].mp4\0sub/b.mp4\0line\nbreak\0"sv); // --files-from0 (G18)
    CHECK(folder::files_from_text({}).empty());

    // (R"j(...)j": the text itself contains `)"`.)
    CHECK(folder::parse_copied_line(R"j({"time":"t","level":"info","msg":"Copied (new)","size":3771504,)j"
                                    R"j("object":"sub/winrar.exe","objectType":"*sftp.Object"})j") == "sub/winrar.exe");
    CHECK(folder::parse_copied_line(R"j({"level":"info","msg":"Copied (replaced existing)","object":"x"})j") == "x");
    CHECK_FALSE(folder::parse_copied_line(R"({"level":"notice","msg":"stats","stats":{}})").has_value());
    CHECK_FALSE(folder::parse_copied_line("Copied, but not JSON").has_value());
}

TEST_CASE("rclone stats list each file in flight") {
    const auto p = engine::parse_stats_line(
        R"({"level":"notice","stats":{"bytes":300,"totalBytes":1000,"speed":10,"transferring":[)"
        R"({"name":"a.mp4","bytes":100,"size":400,"speed":4},{"name":"sub/b.mp4","bytes":200,"size":600,"speed":6}]}})");
    REQUIRE(p.has_value());
    REQUIRE(p->transferring.size() == 2);
    CHECK(p->transferring[0].name == "a.mp4");
    CHECK(p->transferring[0].bytes == 100);
    CHECK(p->transferring[0].size == 400);
    CHECK(p->transferring[1].name == "sub/b.mp4");
    CHECK(p->transferring[1].speed == doctest::Approx(6));
}

TEST_CASE("a big folder shows its active files and the first few queued") {
    const std::vector<State> states{State::done,   State::queued, State::running, State::queued, State::skipped,
                                    State::queued, State::paused, State::failed,  State::queued};
    CHECK(folder::visible(states, 2) == std::vector<std::size_t>{1, 2, 3, 6, 7});
    CHECK(folder::visible(states, 10) == std::vector<std::size_t>{1, 2, 3, 5, 6, 7, 8});
    CHECK(folder::visible({State::done, State::skipped}, 5).empty());
}

TEST_CASE("a folder's files survive a restart in queue.json") {
    SavedDownload d{"hetzner", "folder", "/home/x/Limits", "Limits", "E:\\Learn", 597330609,
                    {{"a.mp4", 100, "2023-05-06T15:18:12+04:00", "done", true, true}, // fetched and verified
                     {"e.mp4", 50, "", "done", true, false},                            // fetched, not checked yet
                     {"f.mp4", 60, "", "done"},                                         // was there already
                     {"b.mp4", 200, "", "paused"},
                     {"sub/c.mp4", 300, "", "skipped"},
                     {"d.mp4", 400, "", "queued"},
                     {"g.mp4", 500, "", "queued", false, false, "g (2).mp4"}}}; // a second copy
    const auto back = parse_queue(queue_to_json({d}));
    REQUIRE(back.size() == 1);
    CHECK(back[0] == d);
    // Unknown states read as queued; entries without a path are dropped.
    const auto odd = parse_queue(R"([{"remote":"r","path":"/p","dest":"C:\\d","mode":"folder",)"
                                 R"("files":[{"path":"x","state":"running"},{"state":"done"}]}])");
    REQUIRE(odd.size() == 1);
    REQUIRE(odd[0].files.size() == 1);
    CHECK(odd[0].files[0].state == "queued");
}

TEST_CASE("folder totals count finished and running files, not skipped ones") {
    folder::Tree t;
    t.files.push_back(folder::make_file("a", 100, ""));
    t.files.push_back(folder::make_file("b", 200, ""));
    t.files.push_back(folder::make_file("c", 300, ""));
    t.files.push_back(folder::make_file("d", 400, ""));
    t.files[0].status = State::done;
    t.files[1].status = State::running;
    t.files[1].bytes = 50;
    t.files[1].speed = 10;
    t.files[2].status = State::skipped;
    t.files[3].bytes = 40; // a big file's partial download from an earlier run
    const auto tot = folder::totals(t);
    CHECK(tot.total == 700);
    CHECK(tot.bytes == 190);
    CHECK(tot.files == 3);
    CHECK(tot.files_done == 1);
    CHECK(tot.speed == 10);
    REQUIRE(tot.eta.has_value());
    CHECK(*tot.eta == doctest::Approx(51.0));
    CHECK(folder::make_file("x", folder::big_file_threshold, "").big);
    CHECK_FALSE(folder::make_file("x", 10, "").big);
    // A small file with a device name anywhere in its path goes through grab's engine, not rclone.
    CHECK(folder::make_file("CON", 10, "").big);
    CHECK(folder::make_file("season 1/nul.txt", 10, "").big);
    CHECK(folder::make_file("aux/e01.srt", 10, "").big);
    CHECK_FALSE(folder::make_file("console.txt", 10, "").big);
    // A folder lands under its own name, made safe: never outside the download folder.
    const std::filesystem::path dest = util::path_from_utf8("D:\\Downloads");
    CHECK(folder::local_dir(dest, "/srv/..\\up") == dest / util::path_from_utf8("..＼up"));
    CHECK(folder::local_dir(dest, "/srv/C:\\Windows") == dest / util::path_from_utf8("C：＼Windows"));
    CHECK(folder::local_dir(dest, "/srv/..") == dest / util::path_from_utf8("．．"));
}

TEST_CASE("single files of a folder: pause, resume, skip and retry") {
    folder::Tree t;
    t.files.push_back(folder::make_file("waiting", 10, ""));
    t.files.push_back(folder::make_file("copying", 10, ""));
    t.files.push_back(folder::make_file("big", folder::big_file_threshold, ""));
    t.files.push_back(folder::make_file("broken", folder::big_file_threshold, ""));
    t.files[0].in_batch = true;
    t.files[1].in_batch = true;
    t.files[1].status = State::running;
    t.files[1].bytes = 5;
    t.files[2].status = State::running;
    t.files[3].status = State::failed;
    t.files[3].error = "oops";

    folder::pause_file(t, "waiting"); // still queued in the batch: rclone would fetch it
    CHECK(t.files[0].status == State::paused);
    CHECK(t.batch_stop.stop_requested());
    t.batch_stop = {};
    folder::pause_file(t, "copying"); // in the batch: restarts it without the file
    CHECK(t.files[1].status == State::paused);
    CHECK(t.files[1].bytes == 0);
    CHECK(t.batch_stop.stop_requested());
    folder::pause_file(t, "big"); // its own stop; it is marked paused once stopped
    CHECK(t.files[2].status == State::running);
    CHECK(t.files[2].stop_reason == folder::StopReason::pause);
    CHECK(t.files[2].stop.stop_requested());

    folder::resume_file(t, "waiting");
    CHECK(t.files[0].status == State::queued);
    folder::resume_paused(t);
    CHECK(t.files[1].status == State::queued);

    const auto dir = std::filesystem::path("D") / "Limits";
    const auto discard = folder::skip_file(t, dir, "/srv/Limits", "broken");
    CHECK(t.files[3].status == State::skipped);
    REQUIRE(discard.has_value());
    CHECK(discard->target == dir / "broken");
    CHECK(discard->remote_path == "/srv/Limits/broken");
    REQUIRE(discard->reserved);
    CHECK(discard->reserved(dir / "BIG")); // the folder's files, as Windows compares names
    CHECK_FALSE(discard->reserved(dir / "broken.grabpart"));
    // A folder file named like an rclone temp file is the folder's own: kept by the batch cleanup.
    folder::Tree named;
    named.files.push_back(folder::make_file("notes.txt", 1, ""));
    named.files.push_back(folder::make_file("notes.txt.12345678.partial", 1, ""));
    CHECK(folder::reserved_names(dir, named)(dir / "notes.txt.12345678.partial"));
    CHECK_FALSE(folder::reserved_names(dir, named)(dir / "notes.txt.90c84c01.partial"));
    CHECK_FALSE(folder::skip_file(t, dir, "/srv/Limits", "big").has_value()); // running: stopped, then discarded
    CHECK(t.files[2].stop_reason == folder::StopReason::skip);

    t.files[1].status = State::failed;
    t.files[1].error = "x";
    auto o = folder::outcome(t);
    CHECK(o.failed == 1);
    CHECK(o.first_error == "copying: x");
    CHECK(o.waiting == 1); // "waiting" is queued
    folder::retry_files(t, false);
    CHECK(t.files[1].status == State::queued);
    CHECK(t.files[1].error.empty());
    CHECK(t.files[3].status == State::skipped); // skipped files stay skipped
    t.files[0].status = State::paused;
    folder::retry_files(t, true); // after a cancel: everything not done or skipped
    CHECK(t.files[0].status == State::queued);
}

TEST_CASE("a folder's local directory and its files' remote paths") {
    CHECK(folder::local_dir("Backup", "/home/x/Limits") == std::filesystem::path("Backup") / "Limits");
    CHECK(folder::child_remote_path("/a", "b/c.mkv") == "/a/b/c.mkv");
    CHECK(folder::child_remote_path("/a/", "b") == "/a/b");
    CHECK(folder::child_remote_path("", "b") == "b");
}

#ifdef _WIN32
TEST_CASE("a folder is listed without a connection pool (--rclone, a dry run's listing)") {
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-list-no-pool";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    // A stand-in rclone: logs its arguments, lists one file.
    std::ofstream(dir / "rclone.cmd", std::ios::binary)
        << "@echo off\r\n"
           ">> \"%~dp0calls.log\" echo %*\r\n"
           "echo [{\"Path\":\"a.txt\",\"Name\":\"a.txt\",\"Size\":3,\"ModTime\":\"2026-10-21T10:00:00Z\",\"IsDir\":false}]\r\n";
    // The server as grab.conf has it, pinned in grab's known_hosts (the runner's scratch APPDATA).
    server_conf::Connection c;
    c.host = "main.example";
    c.user = "u";
    c.auth = server_conf::Auth::agent;
    RemoteSettings settings;
    settings.name = "Main";
    settings.rclone_remote = "Main";
    // --rclone's check before its copy takes the flags the copy does (rclone_flags::first_refused).
    settings.common_flags = {"--sftp-port", "2222"};
    auto ctx = engine::make_context(GrabConfig{}, settings, c, dir / "grab.conf");
    ctx.config.rclone = util::path_to_utf8(dir / "rclone.cmd");
    std::filesystem::create_directories(grab_known_hosts_path().parent_path());
    std::ofstream(grab_known_hosts_path(), std::ios::binary) << "main.example ssh-ed25519 FAKEKEY\n";
    folder::Job job;
    job.ctx = &ctx; // no job.connections
    job.remote_path = "/tmp/f";
    job.dest = dir / "out";
    folder::Tree tree;
    std::mutex m;
    REQUIRE(folder::list(job, tree, m, std::stop_token{}).has_value());
    REQUIRE(tree.files.size() == 1);
    CHECK(tree.files[0].path == "a.txt");
    const auto calls = util::read_file(dir / "calls.log").value_or("");
    CHECK(calls.find("--checkers") == std::string::npos); // rclone's own walk
    CHECK(calls.find("--config NUL --sftp-port 2222") != std::string::npos);
    std::filesystem::remove_all(dir);
    std::filesystem::remove(grab_known_hosts_path());
}
#endif

TEST_CASE("a path listed twice is one file (G81)") {
    const auto files = folder::parse_listing(R"([{"Path":"a.txt","Size":3},{"Path":"b.txt","Size":1},{"Path":"a.txt","Size":3}])");
    REQUIRE(files.has_value());
    REQUIRE(files->size() == 2);
    CHECK((*files)[0].path == "a.txt");
    CHECK((*files)[1].path == "b.txt");
}

TEST_CASE("names that are one on Windows or to rclone's batch (G25)") {
    // As rclone lists them, sorted: README before readme, Docs before docs.
    std::vector<folder::File> files;
    for (const char* p : {"Docs/a.txt", "Docs/only-upper.txt", "NOTES/x.txt", "README", "caf\u00e9.txt", "cafe\u0301.txt",
                          "docs/a.txt", "docs/only-lower.txt", "notes", "plain.txt", "readme", "readme (2)"}) {
        files.push_back(folder::make_file(p, 5, ""));
    }
    folder::settle_names(files);
    auto file = [&](std::string_view p) -> const folder::File& {
        return *std::ranges::find(files, p, &folder::File::path);
    };
    // rclone would copy one of each and drop the rest (a folder with all it holds): grab fetches them.
    for (const char* p : {"Docs/a.txt", "Docs/only-upper.txt", "README", "caf\u00e9.txt", "cafe\u0301.txt", "docs/a.txt",
                          "docs/only-lower.txt", "readme"}) {
        INFO(p);
        CHECK(file(p).big);
    }
    CHECK_FALSE(file("plain.txt").big);
    CHECK(file("NOTES/x.txt").big); // its folder and the file "notes" are one name to rclone
    CHECK_FALSE(file("readme (2)").big);
    // The first keeps its name; a later one with the same name on Windows gets the next free number.
    CHECK(file("README").local.empty());
    CHECK(file("readme").local == std::filesystem::path("readme (3)")); // "readme (2)" is the server's
    CHECK(file("Docs/a.txt").local.empty());
    CHECK(file("docs/a.txt").local == std::filesystem::path("docs") / "a (2).txt");
    CHECK(file("docs/only-lower.txt").local.empty()); // one folder on Windows, no clash in it
    CHECK(file("notes").local == std::filesystem::path("notes (2)")); // a folder has its name
    CHECK(file("caf\u00e9.txt").local.empty()); // two names to NTFS
    CHECK(file("cafe\u0301.txt").local.empty());
}
