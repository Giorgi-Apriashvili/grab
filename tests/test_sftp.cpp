#include <doctest/doctest.h>

#include "fake_sftp.hpp"
#include "sftp.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using namespace grab;
using grab::test::FakeServer;
using grab::test::pattern;

namespace {

// Reads [offset, offset + length) of `path` into a string the size of the range.
std::expected<std::string, sftp::Status> read_all(sftp::Client& c, const std::string& path, std::uint64_t offset,
                                                  std::uint64_t length, int depth, std::uint32_t chunk) {
    std::optional<std::expected<std::string, sftp::Status>> handle;
    c.open(path, [&](std::expected<std::string, sftp::Status> h) { handle = std::move(h); });
    while (!handle) c.pump();
    if (!*handle) return std::unexpected(handle->error());
    std::string got(length, '\0');
    auto r = sftp::read_range(c, **handle, offset, length,
                              [&](std::uint64_t at, std::string_view bytes) {
                                  std::memcpy(got.data() + (at - offset), bytes.data(), bytes.size());
                              },
                              depth, chunk);
    c.close(**handle);
    c.pump();
    if (!r) return std::unexpected(r.error());
    return got;
}

} // namespace

TEST_CASE("sftp: version, stat, realpath, missing and refused paths") {
    FakeServer srv;
    srv.files["/data/a.bin"] = pattern(1234);
    srv.dirs["/data"] = {".", "..", "a.bin"};
    srv.denied.insert("/data/secret");
    sftp::Client c(srv);
    c.start();
    CHECK(c.version() == 3);
    CHECK(c.has_extension("posix-rename@openssh.com"));
    CHECK_FALSE(c.has_extension("statvfs@openssh.com"));

    auto a = sftp::stat(c, "/data/a.bin");
    REQUIRE(a);
    CHECK(a->size == 1234u);
    CHECK(a->is_file());
    CHECK(a->mtime == 1700000000u);
    auto d = sftp::stat(c, "/data");
    REQUIRE(d);
    CHECK(d->is_dir());
    CHECK_FALSE(d->size);

    auto missing = sftp::stat(c, "/data/nope");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == sftp::Code::no_such_file);
    CHECK(sftp::describe(missing.error()) == "no such file or folder");

    auto refused = read_all(c, "/data/secret", 0, 1, 4, 1024);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == sftp::Code::permission_denied);
    CHECK(sftp::describe(refused.error()) == "permission denied");

    auto home = sftp::realpath(c, ".");
    REQUIRE(home);
    CHECK(*home == "/home/test");
    CHECK(c.pending() == 0);
}

TEST_CASE("sftp: a server's own status text is shown as one short, safe line") {
    const std::string said = "\x1b[31mred\nsecond" + std::string(std::size_t{1024} * 1024, 'x');
    CHECK(sftp::describe(sftp::Status{.code = sftp::Code::failure, .message = said}) == "\u241B[31mred");
    const std::string longer = "\n  \n" + std::string(300, 'y') + "\xFF";
    CHECK(sftp::describe(sftp::Status{.code = sftp::Code::failure, .message = longer}) ==
          std::string(100, 'y') + "\u2026");
    CHECK(sftp::describe(sftp::Status{.code = sftp::Code::failure, .message = "bad\xFF name"}) == "bad\u201BFF name");
    CHECK(sftp::describe(sftp::Status{.code = sftp::Code::failure, .message = " \n"}) ==
          "the server refused (SFTP status 4)");
}

TEST_CASE("sftp: listing a folder over several replies, without . and ..") {
    FakeServer srv;
    srv.dirs["/d"] = {".", "..", "one", "sub", "three", "four", "five"};
    srv.dirs["/d/sub"] = {".", ".."};
    for (const char* n : {"/d/one", "/d/three", "/d/four", "/d/five"}) srv.files[n] = "x";
    srv.readdir_batch = 3;
    sftp::Client c(srv);
    c.start();
    auto entries = sftp::list(c, "/d");
    REQUIRE(entries);
    std::vector<std::string> names;
    for (const auto& e : *entries) names.push_back(e.name);
    CHECK(names == std::vector<std::string>{"one", "sub", "three", "four", "five"});
    CHECK((*entries)[1].attrs.is_dir());
    CHECK((*entries)[0].attrs.is_file());
    CHECK((*entries)[0].attrs.size == 1u);
    CHECK(c.pending() == 0);

    auto missing = sftp::list(c, "/nowhere");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == sftp::Code::no_such_file);
}

TEST_CASE("sftp: an empty NAME reply ends a listing, as OpenSSH's sftp takes it") {
    FakeServer srv;
    srv.dirs["/d"] = {".", "..", "one", "two", "three"};
    for (const char* n : {"/d/one", "/d/two", "/d/three"}) srv.files[n] = "x";
    srv.never_eof = true;
    sftp::Client c(srv);
    c.start();
    auto entries = sftp::list(c, "/d");
    REQUIRE(entries);
    CHECK(entries->size() == 3);
    CHECK(c.pending() == 0);
}

TEST_CASE("sftp: a listing stops past its entry cap, and its handle is closed") {
    FakeServer srv;
    for (int i = 0; i < 10; ++i) {
        const std::string name = "f" + std::to_string(i);
        srv.dirs["/d"].push_back(name);
        srv.files["/d/" + name] = "x";
    }
    srv.readdir_batch = 3;
    sftp::Client c(srv);
    c.start();
    auto capped = sftp::list(c, "/d", 5);
    REQUIRE_FALSE(capped);
    CHECK(capped.error().code == sftp::Code::failure);
    CHECK(sftp::describe(capped.error()) == "the folder holds more than 5 entries");
    CHECK(c.pending() == 0);
    auto whole = sftp::list(c, "/d", 10);
    REQUIRE(whole);
    CHECK(whole->size() == 10);
}

TEST_CASE("sftp: reading a range asks for exactly its bytes, pipelined") {
    FakeServer srv;
    const std::string content = pattern(1'000'000);
    srv.files["/f"] = content;
    sftp::Client c(srv);
    c.start();

    auto got = read_all(c, "/f", 1000, 600'000, 4, 65536);
    REQUIRE(got);
    CHECK(*got == content.substr(1000, 600'000));
    std::uint64_t asked = 0;
    for (auto [off, len] : srv.reads) {
        CHECK(off >= 1000);
        CHECK(off + len <= 601'000); // never past the range: no read-ahead
        asked += len;
    }
    CHECK(asked == 600'000); // each byte asked for once
    CHECK(srv.most_waiting <= 4 + 1); // the depth (+ the OPEN or CLOSE sent with them)
    CHECK(srv.most_waiting >= 4);     // and actually pipelined
}

TEST_CASE("sftp: short reads are asked again for exactly the rest") {
    FakeServer srv;
    const std::string content = pattern(300'000);
    srv.files["/f"] = content;
    srv.max_data = 10'000; // the server sends at most 10 KB per READ
    sftp::Client c(srv);
    c.start();
    auto got = read_all(c, "/f", 0, content.size(), 8, sftp::max_read);
    REQUIRE(got);
    CHECK(*got == content);
    for (auto [off, len] : srv.reads) CHECK(off + len <= content.size());
    CHECK(srv.reads.size() >= content.size() / 10'000);
}

TEST_CASE("sftp: replies in any order and the stream in small pieces") {
    FakeServer srv;
    const std::string content = pattern(500'000);
    srv.files["/f"] = content;
    srv.reverse = true; // each batch answered last request first
    srv.slice = 7;      // and handed over a few bytes at a time
    srv.max_data = 30'000;
    sftp::Client c(srv);
    c.start();
    auto got = read_all(c, "/f", 123, 400'000, 16, 50'000);
    REQUIRE(got);
    CHECK(*got == content.substr(123, 400'000));
}

TEST_CASE("sftp: a file shorter than listed ends the range with EOF") {
    FakeServer srv;
    srv.files["/f"] = pattern(100'000);
    sftp::Client c(srv);
    c.start();
    auto got = read_all(c, "/f", 0, 150'000, 4, 32'768);
    REQUIRE_FALSE(got);
    CHECK(got.error().code == sftp::Code::eof);
    CHECK(c.pending() == 0); // every reply was taken before giving up
}

TEST_CASE("sftp: a broken stream throws instead of being trusted") {
    SUBCASE("a packet claiming a gigabyte") {
        FakeServer srv;
        srv.files["/f"] = "abc";
        sftp::Client c(srv);
        c.start();
        srv.corrupt = true;
        CHECK_THROWS_AS(std::ignore = sftp::stat(c, "/f"), sftp::Broken);
    }
    SUBCASE("waiting with nothing asked") {
        FakeServer srv;
        sftp::Client c(srv);
        c.start();
        CHECK_THROWS_AS(c.pump(), sftp::Broken);
    }
    SUBCASE("a server that stops answering") {
        FakeServer srv;
        sftp::Client c(srv);
        c.start();
        srv.dirs["/d"] = {};
        c.stat("/d", [](std::expected<sftp::Attrs, sftp::Status>) {});
        c.flush();
        std::array<char, 64> lost{};
        (void)srv.read(lost); // the reply goes missing
        CHECK_THROWS_AS(c.pump(), sftp::Broken);
    }
}
