#include "engine.hpp"
#include "folder.hpp"
#include "gui_state.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace grab;
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
    CHECK(folder::files_from_text({"a #1 [x].mp4", "sub/b.mp4"}) == "a #1 [x].mp4\nsub/b.mp4\n");
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
                    {{"a.mp4", 100, "2023-05-06T15:18:12+04:00", "done"},
                     {"b.mp4", 200, "", "paused"},
                     {"sub/c.mp4", 300, "", "skipped"},
                     {"d.mp4", 400, "", "queued"}}};
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
