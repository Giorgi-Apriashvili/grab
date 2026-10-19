#include "engine.hpp"
#include "preview.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace grab;
using preview::Kind;

TEST_CASE("what kind of file a name is") {
    CHECK(preview::kind_of("movies/The.Movie.2023.1080p.mkv") == Kind::video);
    CHECK(preview::kind_of("x.MP4") == Kind::video);
    CHECK(preview::kind_of("album/01 - Song.flac") == Kind::audio);
    CHECK(preview::kind_of("cover.JPG") == Kind::image);
    CHECK(preview::kind_of("release.nfo") == Kind::text);
    CHECK(preview::kind_of("subs/en.srt") == Kind::text);
    CHECK(preview::kind_of("book.pdf") == Kind::pdf);
    CHECK(preview::kind_of("archive.rar") == Kind::none);
    CHECK(preview::kind_of("noextension") == Kind::none);
    CHECK(preview::kind_of("folder.mkv/inside") == Kind::none); // the dot is in a folder's name
    CHECK(preview::kind_of(".mkv") == Kind::video);
    CHECK(preview::content_type("a.jpeg") == "image/jpeg");
    CHECK(preview::content_type("a.png") == "image/png");
    CHECK(preview::content_type("a.svg") == "image/svg+xml");
    CHECK(preview::content_type("a.nfo") == "text/plain; charset=utf-8");
    CHECK(preview::content_type("a.pdf") == "application/pdf");
    CHECK(preview::max_bytes(Kind::text) == 1 << 20);
    CHECK(preview::max_bytes(Kind::video) == 0);
}

TEST_CASE("the local stream server's command line and address") {
    engine::Context ctx;
    ctx.config.rclone = "rclone.exe";
    ctx.settings.rclone_remote = "Box.Guest";
    ctx.settings.common_flags = {"-P", "--sftp-chunk-size", "255Ki"};
    ctx.remote.host = "u443987-sub1.your-storagebox.de";
    const auto argv = preview::serve_argv(ctx, "", "abc123");
    CHECK(std::vector<std::string>(argv.begin(), argv.begin() + 4) ==
          std::vector<std::string>{"rclone.exe", "serve", "http", "Box.Guest:"});
    auto has = [&](std::string a, std::string b) {
        const auto it = std::ranges::find(argv, a);
        return it != argv.end() && it + 1 != argv.end() && *(it + 1) == b;
    };
    CHECK(has("--addr", "127.0.0.1:0")); // this PC only, a free port
    CHECK(has("--baseurl", "/abc123"));
    CHECK(std::ranges::find(argv, "--read-only") != argv.end());
    CHECK(std::ranges::find(argv, "-P") == argv.end()); // no console progress
    CHECK(has("--sftp-chunk-size", "255Ki"));
    CHECK(has("--sftp-concurrency", "4")); // few reads in flight: a seek starts in seconds
    CHECK(argv.back() == "4");             // after the configured flags, so it wins
    CHECK(std::ranges::find(argv, "--vfs-read-chunk-streams") == argv.end()); // slow to start on a Storage Box
    CHECK(preview::serve_argv(ctx, "/", "t")[3] == "Box.Guest:/");
    ctx.remote.host = "65.21.224.123";
    CHECK(preview::serve_argv(ctx, "", "t").back() == "8");
    CHECK(preview::served_port(
              "2026/10/19 16:23:03 NOTICE: sftp://x/y: HTTP Server started on [http://127.0.0.1:53954/abc123/]") == 53954);
    CHECK_FALSE(preview::served_port("NOTICE: starting").has_value());
    CHECK_FALSE(preview::served_port("http://127.0.0.1:/").has_value());
}

TEST_CASE("paths in the stream's addresses") {
    CHECK(preview::where("movies/a.mkv") == preview::Where{"", "movies/a.mkv"});
    CHECK(preview::where("/etc/x.nfo") == preview::Where{"/", "etc/x.nfo"});
    CHECK(preview::url_path("movies/Sahe AKA The Precinct (2010) #1.avi") ==
          "movies/Sahe%20AKA%20The%20Precinct%20%282010%29%20%231.avi");
    CHECK(preview::url_path("ა.jpg") == "%E1%83%90.jpg"); // UTF-8 bytes
    CHECK(preview::url_decode("movies%2FSahe%20AKA.avi") == "movies/Sahe AKA.avi");
    CHECK(preview::url_decode(preview::url_path("a b/c+d%e")) == "a b/c+d%e");
    CHECK_FALSE(preview::url_decode("bad%2").has_value());
    CHECK_FALSE(preview::url_decode("bad%zz").has_value());
}

TEST_CASE("text files as UTF-8 for the viewer") {
    // An .nfo's box drawing (CP437): ╔═╗ and █.
    CHECK(preview::text_utf8("\xC9\xCD\xBB \xDB", true) == "╔═╗ █");
    CHECK(preview::text_utf8("plain\r\nascii", true) == "plain\r\nascii");
    // UTF-8 stays; a byte order mark goes; a sequence cut off at the end (the first MiB) is dropped.
    CHECK(preview::text_utf8("caf\xC3\xA9", false) == "café");
    CHECK(preview::text_utf8("\xEF\xBB\xBFhi", false) == "hi");
    CHECK(preview::text_utf8("ab\xE1\x83", false) == "ab");
    // Not UTF-8: Windows-1252.
    CHECK(preview::text_utf8("caf\xE9 \x93q\x94 \x80", false) == "café “q” €");
    // UTF-16 with its byte order mark (some subtitles).
    CHECK(preview::text_utf8(std::string("\xFF\xFEh\0i\0", 6), false) == "hi");
    CHECK(preview::text_utf8(std::string("\xFE\xFF\0h\0i", 6), false) == "hi");
}
