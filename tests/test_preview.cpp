#include "preview.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

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

TEST_CASE("paths in the stream's addresses") {
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

namespace {

constexpr std::uint64_t MiB = std::uint64_t{1} << 20;

// An EBML element's id and a size field of 8 bytes, as mkvmerge writes them.
std::string element(std::uint32_t id, std::uint64_t size, std::string_view body = {}) {
    std::string out;
    for (int shift = 24; shift >= 0; shift -= 8) {
        const auto byte = static_cast<char>((id >> shift) & 0xFF);
        if (!out.empty() || byte != 0) out += byte;
    }
    out += static_cast<char>(0x01);
    for (int shift = 48; shift >= 0; shift -= 8) out += static_cast<char>((size >> shift) & 0xFF);
    out += body;
    return out;
}

std::string unknown_size(std::uint32_t id) {
    std::string out = element(id, 0);
    out[out.size() - 7] = static_cast<char>(0xFF);
    for (std::size_t i = out.size() - 6; i < out.size(); ++i) out[i] = static_cast<char>(0xFF);
    return out;
}

} // namespace

TEST_CASE("Play's warm start: an MKV up to its first video and 8 MiB more, other files 8 MiB") {
    const std::string ebml = element(0x1A45DFA3, 4, "abcd");
    const std::string seek = element(0x114D9B74, 10, std::string(10, 'x'));
    const std::string info = element(0x1549A966, 20, std::string(20, 'x'));
    // Tracks, then 28 MB of fonts (only their header is in the first bytes), then the first cluster.
    std::string segment_body = seek + info + element(0x1654AE6B, 100, std::string(100, 't'));
    const std::uint64_t fonts_at = 12 /*segment header*/ + segment_body.size();
    segment_body += element(0x1941A469, 27'931'626); // Attachments
    const std::string head = ebml + unknown_size(0x18538067) + segment_body;
    const std::uint64_t segment_at = ebml.size() + 12;
    const auto cluster = preview::mkv_first_cluster(head);
    REQUIRE(cluster.has_value());
    CHECK(*cluster == ebml.size() + fonts_at + 12 + 27'931'626); // right after the fonts
    CHECK(preview::warm_start(head, 1'520'776'651) == *cluster + 8 * MiB);
    (void)segment_at;

    // No fonts: the first cluster is in the first bytes, and 8 MiB of video after it.
    const std::string plain = ebml + unknown_size(0x18538067) + seek + info + element(0x1F43B675, 0x7FFFFF) + "frames";
    const auto plain_cluster = ebml.size() + 12 + seek.size() + info.size();
    CHECK(preview::mkv_first_cluster(plain) == plain_cluster);
    CHECK(preview::warm_start(plain, 2'000'000'000) == plain_cluster + 8 * MiB);
    // A live stream's cluster of unknown size counts as the first cluster too.
    CHECK(preview::mkv_first_cluster(ebml + unknown_size(0x18538067) + unknown_size(0x1F43B675)).has_value());

    // At most 128 MiB, never more than the file.
    const std::string huge = ebml + unknown_size(0x18538067) + element(0x1941A469, 900 * MiB);
    CHECK(preview::warm_start(huge, 5'000'000'000) == 128 * MiB);
    CHECK(preview::warm_start(head, 10 * MiB) == 10 * MiB);

    // Not an MKV.
    CHECK_FALSE(preview::mkv_first_cluster("RIFF....AVI LIST").has_value());
    CHECK_FALSE(preview::mkv_first_cluster("").has_value());
    // An MP4's first bytes, its leading NULs included (review G126: a plain literal stopped at the first).
    const std::string mp4("\x00\x00\x00\x20" "ftypisom", 12);
    REQUIRE(mp4.size() == 12);
    CHECK_FALSE(preview::mkv_first_cluster(mp4).has_value());
    CHECK(preview::warm_start(mp4, 700 * MiB) == 8 * MiB);
    CHECK(preview::warm_start("", 100) == 100);
}

TEST_CASE("a player's Range header") {
    using R = preview::Requested;
    CHECK(preview::requested_range("bytes=0-", 1000) == R{R::Kind::part, 0, 999});
    CHECK(preview::requested_range("bytes=100-199", 1000) == R{R::Kind::part, 100, 199});
    CHECK(preview::requested_range(" Bytes=900-5000 ", 1000) == R{R::Kind::part, 900, 999});
    CHECK(preview::requested_range("bytes=-300", 1000) == R{R::Kind::part, 700, 999});
    CHECK(preview::requested_range("bytes=-5000", 1000) == R{R::Kind::part, 0, 999});
    CHECK(preview::requested_range("bytes=1000-", 1000).kind == R::Kind::unsatisfiable);
    CHECK(preview::requested_range("bytes=-0", 1000).kind == R::Kind::unsatisfiable);
    CHECK(preview::requested_range("", 1000).kind == R::Kind::whole);
    CHECK(preview::requested_range("bytes=0-1,5-9", 1000).kind == R::Kind::whole);
    CHECK(preview::requested_range("bytes=50-10", 1000).kind == R::Kind::whole);
    CHECK(preview::requested_range("items=0-1", 1000).kind == R::Kind::whole);
    CHECK(preview::requested_range("bytes=x-", 1000).kind == R::Kind::whole);
}
