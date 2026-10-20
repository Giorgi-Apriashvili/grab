#include "file_type.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>

using namespace grab;
using file_type::Kind;

namespace {
constexpr std::uint64_t MiB = std::uint64_t{1} << 20;
constexpr std::uint64_t GiB = std::uint64_t{1} << 30;
} // namespace

TEST_CASE("a file's kind, by its extension") {
    CHECK(file_type::kind_of("The.Movie.2020.1080p.MKV") == Kind::video);
    CHECK(file_type::kind_of("Album - 01 - Track.flac") == Kind::audio);
    CHECK(file_type::kind_of("scan.tiff") == Kind::image);
    CHECK(file_type::kind_of("movie.nfo") == Kind::text);
    CHECK(file_type::kind_of("movie.srt") == Kind::text);
    CHECK(file_type::kind_of("Effective Modern C++.pdf") == Kind::document);
    CHECK(file_type::kind_of("novel.epub") == Kind::document);
    CHECK(file_type::kind_of("release.part2.rar") == Kind::archive);
    CHECK(file_type::kind_of("release.r00") == Kind::archive);
    CHECK(file_type::kind_of("release.z01") == Kind::archive);
    CHECK(file_type::kind_of("split.7z.001") == Kind::archive);
    CHECK(file_type::kind_of("backup.tar.gz") == Kind::archive);
    CHECK(file_type::kind_of("winrar-x64-720.exe") == Kind::program);
    CHECK(file_type::kind_of("ubuntu-24.04.iso") == Kind::disc);
    CHECK(file_type::kind_of("data.bin") == Kind::other);
    CHECK(file_type::kind_of("LICENSE") == Kind::other);
    CHECK(file_type::kind_of(".bashrc") == Kind::other);
    CHECK(file_type::kind_of("movie.r0") == Kind::other);
    CHECK(file_type::kind_of("downloads/tv/show.s01e01.mkv") == Kind::video); // a path: its name
    CHECK(file_type::kind_of("folder.mkv/notes") == Kind::other);
}

TEST_CASE("kinds by name, for --type") {
    CHECK(file_type::parse("video") == Kind::video);
    CHECK(file_type::parse(" Disc ") == Kind::disc);
    CHECK(file_type::parse("other") == Kind::other);
    CHECK_FALSE(file_type::parse("videos").has_value());
    CHECK_FALSE(file_type::parse("").has_value());
    CHECK(file_type::names() == "video, audio, image, text, document, archive, program, disc and other");
}

TEST_CASE("sizes as typed") {
    CHECK(file_type::parse_size("700M") == 700 * MiB);
    CHECK(file_type::parse_size("700 MB") == 700 * MiB);
    CHECK(file_type::parse_size("700mib") == 700 * MiB);
    CHECK(file_type::parse_size("1.5G") == GiB + GiB / 2);
    CHECK(file_type::parse_size("20GiB") == 20 * GiB);
    CHECK(file_type::parse_size("2t") == 2048 * GiB);
    CHECK(file_type::parse_size("64k") == 64 * 1024);
    CHECK(file_type::parse_size("500B") == 500);
    CHECK(file_type::parse_size("100") == 100 * MiB); // a bare number is MiB
    CHECK(file_type::parse_size("0") == 0);
    CHECK_FALSE(file_type::parse_size("").has_value());
    CHECK_FALSE(file_type::parse_size("-1G").has_value());
    CHECK_FALSE(file_type::parse_size("big").has_value());
    CHECK_FALSE(file_type::parse_size("1x").has_value());
    CHECK_FALSE(file_type::parse_size("ib").has_value());
    CHECK_FALSE(file_type::parse_size("1 iB").has_value());
}

TEST_CASE("a filter by kind and size") {
    file_type::Filter f;
    CHECK_FALSE(f.active());
    CHECK(f.matches("anything", std::nullopt));
    f.kinds = {Kind::video, Kind::audio};
    CHECK(f.active());
    CHECK(f.matches("a.mkv", std::nullopt));
    CHECK(f.matches("a.flac", 10));
    CHECK_FALSE(f.matches("a.nfo", 10));
    f.min_size = GiB;
    CHECK_FALSE(f.matches("a.mkv", std::nullopt)); // unknown size, with a bound set
    CHECK_FALSE(f.matches("a.mkv", GiB - 1));
    CHECK(f.matches("a.mkv", GiB));
    f.max_size = 20 * GiB;
    CHECK(f.matches("a.mkv", 20 * GiB));
    CHECK_FALSE(f.matches("a.mkv", 20 * GiB + 1));
    CHECK(f.describe() == "video or audio, at least 1.0 GiB, at most 20.0 GiB");
    file_type::Filter size_only;
    size_only.max_size = 100 * MiB;
    CHECK(size_only.describe() == "at most 100.0 MiB");
    CHECK(size_only.matches("x.bin", 5));
}
