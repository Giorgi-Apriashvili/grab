#include <doctest/doctest.h>

#include "fake_sftp.hpp"
#include "sftp.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stop_token>
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

// Writes `data` at `offset` of an open file through write_range.
std::expected<void, sftp::Status> put(sftp::Client& c, std::string_view handle, std::uint64_t offset, std::string_view data,
                                      std::uint32_t chunk = 64 * 1024, int depth = 4) {
    return sftp::write_range(
        c, handle, offset, data.size(), [&](std::uint64_t at, std::uint32_t n) { return data.substr(at - offset, n); }, {},
        depth, sftp::max_unacked, chunk);
}

// A file created and written whole, then closed.
std::expected<void, sftp::Status> create(sftp::Client& c, const std::string& path, std::string_view data) {
    auto h = sftp::open_file(c, path, sftp::OpenFlags::write | sftp::OpenFlags::creat | sftp::OpenFlags::excl);
    if (!h) return std::unexpected(h.error());
    auto written = put(c, *h, 0, data);
    auto closed = sftp::close_file(c, *h);
    if (!written) return written;
    return closed;
}

constexpr std::uint32_t wr = sftp::OpenFlags::write;
constexpr std::uint32_t creat_excl = sftp::OpenFlags::write | sftp::OpenFlags::creat | sftp::OpenFlags::excl;

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

TEST_CASE("sftp: a reply that breaks the protocol is a protocol error with its own text; a broken link isn't") {
    auto message = [](auto&& call) -> std::string {
        try {
            call();
        } catch (const sftp::Broken& e) {
            return std::string(dynamic_cast<const sftp::ProtocolError*>(&e) != nullptr ? "protocol: " : "link: ") + e.what();
        }
        return "no error";
    };
    FakeServer srv;
    srv.files["/f"] = "abc";
    sftp::Client c(srv);
    c.start();
    srv.corrupt = true;
    CHECK(message([&] { std::ignore = sftp::stat(c, "/f"); }) == "protocol: the server sent an SFTP packet of 1073741824 bytes");
    // A link that drops, or grab waiting with nothing asked, isn't the server's protocol.
    FakeServer gone;
    sftp::Client d(gone);
    d.start();
    CHECK(message([&] { d.pump(); }) == "link: waiting for an SFTP reply with no request pending");
    gone.dropped = true;
    const auto dropped = message([&] { std::ignore = sftp::stat(d, "/f"); });
    CHECK(dropped.starts_with("link: "));
    CHECK(dropped.find("engine = rclone") == std::string::npos);
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

// ---- writes (UPLOADS.md 5.1) ---------------------------------------------------------------

TEST_CASE("sftp: OPEN's flags for writing, as OpenSSH's open(2) takes them") {
    FakeServer srv;
    srv.dirs["/up"] = {".", ".."};
    sftp::Client c(srv);
    c.start();

    // A new file: creat | excl, its permissions given; a second time the name is there.
    auto h = sftp::open_file(c, "/up/a", creat_excl,
                             sftp::Attrs{.size = {}, .permissions = 0600, .mtime = {}, .uid = {}, .gid = {}, .atime = {}});
    REQUIRE(h);
    REQUIRE(put(c, *h, 0, "hello"));
    auto attrs = sftp::fstat(c, *h);
    REQUIRE(attrs);
    CHECK(attrs->size == 5u);
    REQUIRE(sftp::close_file(c, *h));
    CHECK(srv.files["/up/a"] == "hello");
    CHECK(srv.modes["/up/a"] == 0600u);
    CHECK(std::ranges::find(srv.dirs["/up"], "a") != srv.dirs["/up"].end()); // listed in its folder
    auto again = sftp::open_file(c, "/up/a", creat_excl);
    REQUIRE_FALSE(again);
    CHECK(again.error().code == sftp::Code::failure);
    CHECK(sftp::describe(again.error()) == "Failure");

    // No creat: the name must be there; creat: its folder must be.
    auto missing = sftp::open_file(c, "/up/b", wr);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == sftp::Code::no_such_file);
    auto nowhere = sftp::open_file(c, "/gone/b", creat_excl);
    REQUIRE_FALSE(nowhere);
    CHECK(nowhere.error().code == sftp::Code::no_such_file);
    auto folder = sftp::open_file(c, "/up", wr | sftp::OpenFlags::creat);
    REQUIRE_FALSE(folder);
    CHECK(folder.error().code == sftp::Code::failure);

    // Without trunc the rest stays; a write past the end leaves zeros between; trunc empties;
    // append writes at the end whatever the offset.
    h = sftp::open_file(c, "/up/a", wr);
    REQUIRE(h);
    REQUIRE(put(c, *h, 1, "EL"));
    REQUIRE(put(c, *h, 7, "!"));
    REQUIRE(sftp::close_file(c, *h));
    CHECK(srv.files["/up/a"] == std::string("hELlo\0\0!", 8));
    h = sftp::open_file(c, "/up/a", wr | sftp::OpenFlags::append);
    REQUIRE(h);
    REQUIRE(put(c, *h, 0, "+"));
    REQUIRE(sftp::close_file(c, *h));
    CHECK(srv.files["/up/a"] == std::string("hELlo\0\0!+", 9));
    h = sftp::open_file(c, "/up/a", wr | sftp::OpenFlags::trunc);
    REQUIRE(h);
    REQUIRE(sftp::close_file(c, *h));
    CHECK(srv.files["/up/a"].empty());

    // A handle opened for reading takes no WRITE (EBADF: OpenSSH says "no such file").
    h = sftp::open_file(c, "/up/a");
    REQUIRE(h);
    auto refused = put(c, *h, 0, "x");
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == sftp::Code::no_such_file);
    REQUIRE(sftp::close_file(c, *h));
    CHECK(c.pending() == 0);
}

TEST_CASE("sftp: SETSTAT, FSETSTAT, REMOVE, MKDIR and RMDIR as OpenSSH answers them") {
    FakeServer srv;
    srv.dirs["/up"] = {".", ".."};
    sftp::Client c(srv);
    c.start();
    REQUIRE(create(c, "/up/f", "0123456789"));

    // The time (access and modification), a size that cuts, permissions.
    auto h = sftp::open_file(c, "/up/f", wr);
    REQUIRE(h);
    REQUIRE(sftp::fsetstat(c, *h,
                           sftp::Attrs{.size = {}, .permissions = {}, .mtime = 1600000000, .uid = {}, .gid = {}, .atime = {}}));
    REQUIRE(sftp::close_file(c, *h));
    REQUIRE(sftp::setstat(c, "/up/f",
                          sftp::Attrs{.size = 4, .permissions = 0640, .mtime = {}, .uid = {}, .gid = {}, .atime = {}}));
    auto a = sftp::stat(c, "/up/f");
    REQUIRE(a);
    CHECK(a->mtime == 1600000000u);
    CHECK(a->size == 4u);
    CHECK(a->permissions == 0100640u);
    auto gone =
        sftp::setstat(c, "/up/none", sftp::Attrs{.size = {}, .permissions = {}, .mtime = 1, .uid = {}, .gid = {}, .atime = {}});
    REQUIRE_FALSE(gone);
    CHECK(gone.error().code == sftp::Code::no_such_file);

    // Folders: an existing name, a missing parent, one with something in it.
    REQUIRE(sftp::mkdir(c, "/up/d",
                        sftp::Attrs{.size = {}, .permissions = 0700, .mtime = {}, .uid = {}, .gid = {}, .atime = {}}));
    auto d = sftp::stat(c, "/up/d");
    REQUIRE(d);
    CHECK(d->is_dir());
    CHECK(d->permissions == 040700u);
    auto twice = sftp::mkdir(c, "/up/d");
    REQUIRE_FALSE(twice);
    CHECK(twice.error().code == sftp::Code::failure);
    auto onfile = sftp::mkdir(c, "/up/f");
    REQUIRE_FALSE(onfile);
    CHECK(onfile.error().code == sftp::Code::failure);
    auto deep = sftp::mkdir(c, "/up/x/y");
    REQUIRE_FALSE(deep);
    CHECK(deep.error().code == sftp::Code::no_such_file);
    REQUIRE(create(c, "/up/d/in", "z"));
    auto full = sftp::rmdir(c, "/up/d");
    REQUIRE_FALSE(full);
    CHECK(full.error().code == sftp::Code::failure);
    auto dir_removed = sftp::remove(c, "/up/d");
    REQUIRE_FALSE(dir_removed);
    CHECK(dir_removed.error().code == sftp::Code::failure);
    auto file_rmdir = sftp::rmdir(c, "/up/f");
    REQUIRE_FALSE(file_rmdir);
    CHECK(file_rmdir.error().code == sftp::Code::no_such_file);
    REQUIRE(sftp::remove(c, "/up/d/in"));
    REQUIRE(sftp::rmdir(c, "/up/d"));
    REQUIRE(sftp::remove(c, "/up/f"));
    auto twice_removed = sftp::remove(c, "/up/f");
    REQUIRE_FALSE(twice_removed);
    CHECK(twice_removed.error().code == sftp::Code::no_such_file);
    CHECK(srv.files.empty());
    CHECK(srv.dirs["/up"] == std::vector<std::string>{".", ".."});
    CHECK_FALSE(srv.dirs.contains("/up/d"));
    CHECK(c.pending() == 0);
}

TEST_CASE("sftp: RENAME never replaces a name; posix-rename does, when the server offers it") {
    FakeServer srv;
    srv.dirs["/up"] = {".", ".."};
    sftp::Client c(srv);
    c.start();
    REQUIRE(create(c, "/up/a", "AAA"));
    REQUIRE(create(c, "/up/b", "BB"));

    // OpenSSH's RENAME (link and unlink): an existing name fails it, both files as they were.
    auto clash = sftp::rename(c, "/up/a", "/up/b");
    REQUIRE_FALSE(clash);
    CHECK(clash.error().code == sftp::Code::failure);
    CHECK(srv.files["/up/a"] == "AAA");
    CHECK(srv.files["/up/b"] == "BB");
    auto missing = sftp::rename(c, "/up/none", "/up/c");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == sftp::Code::no_such_file);
    auto nowhere = sftp::rename(c, "/up/a", "/gone/a");
    REQUIRE_FALSE(nowhere);
    CHECK(nowhere.error().code == sftp::Code::no_such_file);

    // A new name: moved, listed under it; a handle still open writes on into it.
    auto h = sftp::open_file(c, "/up/a", wr);
    REQUIRE(h);
    REQUIRE(sftp::rename(c, "/up/a", "/up/c"));
    REQUIRE(put(c, *h, 3, "+"));
    REQUIRE(sftp::close_file(c, *h));
    CHECK_FALSE(srv.files.contains("/up/a"));
    CHECK(srv.files["/up/c"] == "AAA+");
    CHECK(srv.dirs["/up"] == std::vector<std::string>{".", "..", "b", "c"});

    // posix-rename: replaces the file in one step.
    REQUIRE(c.has_extension("posix-rename@openssh.com"));
    REQUIRE(sftp::posix_rename(c, "/up/c", "/up/b"));
    CHECK(srv.files["/up/b"] == "AAA+");
    CHECK_FALSE(srv.files.contains("/up/c"));
    CHECK(srv.dirs["/up"] == std::vector<std::string>{".", "..", "b"});

    // A folder renamed takes what is in it; never onto a folder with something in it.
    REQUIRE(sftp::mkdir(c, "/up/d"));
    REQUIRE(create(c, "/up/d/x", "x"));
    REQUIRE(sftp::mkdir(c, "/up/e"));
    REQUIRE(create(c, "/up/e/y", "y"));
    auto onto_full = sftp::posix_rename(c, "/up/d", "/up/e");
    REQUIRE_FALSE(onto_full);
    CHECK(onto_full.error().code == sftp::Code::failure);
    REQUIRE(sftp::rename(c, "/up/d", "/up/f"));
    CHECK(srv.files["/up/f/x"] == "x");
    CHECK(srv.dirs.contains("/up/f"));
    CHECK_FALSE(srv.dirs.contains("/up/d"));
    auto file_onto_dir = sftp::posix_rename(c, "/up/b", "/up/e");
    REQUIRE_FALSE(file_onto_dir);
    CHECK(file_onto_dir.error().code == sftp::Code::failure);

    // A server that doesn't offer it answers "unsupported"; one whose RENAME replaces (unlike
    // OpenSSH's) is what the knob gives.
    FakeServer plain;
    plain.dirs["/up"] = {".", ".."};
    plain.posix_rename = false;
    plain.rename_overwrites = true;
    sftp::Client p(plain);
    p.start();
    CHECK_FALSE(p.has_extension("posix-rename@openssh.com"));
    REQUIRE(create(p, "/up/a", "new"));
    REQUIRE(create(p, "/up/b", "old"));
    auto unsupported = sftp::posix_rename(p, "/up/a", "/up/b");
    REQUIRE_FALSE(unsupported);
    CHECK(unsupported.error().code == sftp::Code::op_unsupported);
    REQUIRE(sftp::rename(p, "/up/a", "/up/b"));
    CHECK(plain.files["/up/b"] == "new");
    CHECK(plain.files.size() == 1);
}

TEST_CASE("sftp: a read-only server refuses every change and still reads") {
    FakeServer srv;
    srv.dirs["/ro"] = {".", "..", "f"};
    srv.files["/ro/f"] = "data";
    srv.read_only = true;
    srv.statvfs = true;
    sftp::Client c(srv);
    c.start();
    const auto refused = [](const auto& r, sftp::Code code) { return !r && r.error().code == code; };
    const sftp::Code denied = sftp::Code::permission_denied;
    CHECK(refused(sftp::open_file(c, "/ro/.grab-probe-1", creat_excl), denied));
    CHECK(refused(sftp::open_file(c, "/ro/f", wr), denied));
    CHECK(refused(sftp::mkdir(c, "/ro/d"), denied));
    CHECK(refused(sftp::remove(c, "/ro/f"), denied));
    CHECK(refused(sftp::rename(c, "/ro/f", "/ro/g"), denied));
    CHECK(refused(sftp::posix_rename(c, "/ro/f", "/ro/g"), denied));
    CHECK(refused(
        sftp::setstat(c, "/ro/f", sftp::Attrs{.size = {}, .permissions = {}, .mtime = 1, .uid = {}, .gid = {}, .atime = {}}),
        denied));
    CHECK(refused(sftp::rmdir(c, "/ro"), denied));
    auto got = read_all(c, "/ro/f", 0, 4, 2, 1024);
    REQUIRE(got);
    CHECK(*got == "data");
    // A folder this account can't write is on a file system that can be written.
    auto fs = sftp::statvfs(c, "/ro");
    REQUIRE(fs);
    CHECK_FALSE(fs->read_only());

    // A read-only file system, as OpenSSH answers EROFS and Box.Guest an account that may only
    // read: "Failure", and statvfs says read-only.
    srv.read_only_as = sftp::Code::failure;
    auto erofs = sftp::open_file(c, "/ro/x", creat_excl);
    REQUIRE_FALSE(erofs);
    CHECK(erofs.error().code == sftp::Code::failure);
    CHECK(sftp::describe(erofs.error()) == "Failure");
    fs = sftp::statvfs(c, "/ro");
    REQUIRE(fs);
    CHECK(fs->read_only());
    CHECK(srv.files.size() == 1);
    CHECK(srv.dirs.size() == 1);
    CHECK(c.pending() == 0);
}

TEST_CASE("sftp: statvfs, limits and fsync answers, and malformed ones") {
    FakeServer srv;
    srv.dirs["/up"] = {".", ".."};
    srv.statvfs = true;
    srv.fsync = true;
    srv.max_write = 128 * 1024;
    srv.quota_bytes = std::uint64_t{10} << 20;
    sftp::Client c(srv);
    c.start();
    REQUIRE(create(c, "/up/f", std::string(std::size_t{1} << 20, 'x')));

    std::vector<std::string> offered;
    for (const auto& e : c.extensions()) offered.push_back(e.name + "=" + e.data);
    CHECK(offered == std::vector<std::string>{"posix-rename@openssh.com=1", "statvfs@openssh.com=2", "fsync@openssh.com=1",
                                              "limits@openssh.com=1"});
    auto fs = sftp::statvfs(c, "/up");
    REQUIRE(fs);
    CHECK(fs->free_bytes() == std::uint64_t{9} * 1024 * 1024);
    CHECK(fs->frsize == 4096u);
    CHECK(fs->namemax == 255u);
    CHECK_FALSE(fs->read_only());
    auto nofs = sftp::statvfs(c, "/gone");
    REQUIRE_FALSE(nofs);
    CHECK(nofs.error().code == sftp::Code::no_such_file);
    auto lim = sftp::limits(c);
    REQUIRE(lim);
    CHECK(lim->packet == 256u * 1024);
    CHECK(lim->read == sftp::max_read);
    CHECK(lim->write == 128u * 1024);
    auto h = sftp::open_file(c, "/up/f", wr);
    REQUIRE(h);
    CHECK(sftp::fsync(c, *h));
    REQUIRE(sftp::close_file(c, *h));
    auto unknown = sftp::fsync(c, "no-such-handle");
    REQUIRE_FALSE(unknown);
    CHECK(unknown.error().code == sftp::Code::failure);

    // Any other extension: unsupported; the generic request gives a reply's raw bytes.
    std::optional<std::expected<std::string, sftp::Status>> other;
    c.extended("copy-data", "", [&](std::expected<std::string, sftp::Status> r) { other = std::move(r); });
    while (!other) c.pump();
    REQUIRE_FALSE(*other);
    CHECK(other->error().code == sftp::Code::op_unsupported);
    srv.bad_extended_reply = std::string("\0\0\0\1\0\0\0", 7);
    other.reset();
    c.extended("whatever@example.com", "", [&](std::expected<std::string, sftp::Status> r) { other = std::move(r); });
    while (!other) c.pump();
    REQUIRE(*other);
    CHECK(**other == std::string("\0\0\0\1\0\0\0", 7));

    // Too short to hold their figures: the server broke the protocol.
    CHECK_THROWS_AS(std::ignore = sftp::statvfs(c, "/up"), sftp::ProtocolError);
    FakeServer again;
    again.max_write = 64 * 1024;
    again.bad_extended_reply = std::string(31, '\0'); // 4 figures need 32 bytes
    sftp::Client d(again);
    d.start();
    CHECK_THROWS_AS(std::ignore = sftp::limits(d), sftp::ProtocolError);

    // Not offered: unsupported.
    FakeServer plain;
    sftp::Client p(plain);
    p.start();
    CHECK(p.extensions().size() == 1);
    auto none = sftp::statvfs(p, "/");
    REQUIRE_FALSE(none);
    CHECK(none.error().code == sftp::Code::op_unsupported);
    auto nolimits = sftp::limits(p);
    REQUIRE_FALSE(nolimits);
    CHECK(nolimits.error().code == sftp::Code::op_unsupported);

    // free_bytes: frsize, else bsize; more than a uint64 holds is the most it holds.
    sftp::Statvfs v;
    v.bsize = 512;
    v.bavail = 10;
    CHECK(v.free_bytes() == 5120u);
    v.frsize = 4096;
    CHECK(v.free_bytes() == 40960u);
    v.bavail = std::numeric_limits<std::uint64_t>::max() / 2;
    CHECK(v.free_bytes() == std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE("sftp: write_range sends each byte once and keeps its WRITEs within the caps") {
    FakeServer srv;
    srv.dirs["/up"] = {".", ".."};
    const std::string content = pattern(1'000'000);
    sftp::Client c(srv);
    c.start();
    auto h = sftp::open_file(c, "/up/f", creat_excl);
    REQUIRE(h);
    std::uint64_t acked = 0;
    std::uint64_t asked = 0;
    auto r = sftp::write_range(
        c, *h, 1000, 600'000,
        [&](std::uint64_t at, std::uint32_t n) {
            asked += n;
            return std::string_view(content).substr(at, n);
        },
        [&](std::uint64_t, std::uint32_t n) { acked += n; }, 4, sftp::max_unacked, 65536);
    REQUIRE(r);
    REQUIRE(sftp::close_file(c, *h));
    CHECK(srv.files["/up/f"].substr(1000) == content.substr(1000, 600'000));
    CHECK(asked == 600'000);
    CHECK(acked == 600'000);
    std::uint64_t at = 1000;
    for (const auto& w : srv.writes) {
        CHECK(w.offset == at); // in order, each byte once
        CHECK(w.length <= 65536);
        CHECK(w.out == 1); // each reply, here at once, taken (pump_ready) before the next WRITE
        at += w.length;
    }
    CHECK(at == 601'000);

    // A chunk past the largest WRITE is cut to it.
    srv.writes.clear();
    h = sftp::open_file(c, "/up/g", creat_excl);
    REQUIRE(h);
    REQUIRE(put(c, *h, 0, content, 4 * 1024 * 1024, 64));
    REQUIRE(sftp::close_file(c, *h));
    CHECK(srv.files["/up/g"] == content);
    CHECK(std::ranges::max(srv.writes, {}, &FakeServer::Write::length).length == sftp::max_write);
    for (const auto& w : srv.writes) CHECK(w.out == 1);
    CHECK(c.pending() == 0);
}

TEST_CASE("sftp: write_range refills as acknowledgements come, never past its WRITEs or bytes in flight") {
    // The server holds its replies until the writer waits for one, then hands over one: the writer
    // sends up to its cap without waiting, then refills each acknowledged WRITE at once, so every
    // WRITE after the first fill goes with the cap in flight (a writer that waited for all its
    // replies before sending more would reach the cap once per fill). With one reply at a time, the
    // server's count of WRITEs it hasn't answered is exactly what the writer has in flight.
    // Then the same with each reply 20 ms after its request, as a link's round trip: the caps hold.
    for (const bool timed : {false, true}) {
        for (const auto& [depth, unacked, cap] : {std::tuple{8, std::uint64_t{8} << 20, std::size_t{8}},
                                                  std::tuple{64, std::uint64_t{128} * 1024, std::size_t{4}}}) {
            CAPTURE(timed);
            CAPTURE(depth);
            CAPTURE(unacked);
            FakeServer srv;
            srv.dirs["/up"] = {".", ".."};
            srv.held = !timed;
            srv.release_one = !timed;
            if (timed) srv.latency = std::chrono::milliseconds(20);
            const std::string content = pattern(std::size_t{2} << 20);
            sftp::Client c(srv);
            c.start();
            auto h = sftp::open_file(c, "/up/f", creat_excl);
            REQUIRE(h);
            auto r = sftp::write_range(
                c, *h, 0, content.size(),
                [&](std::uint64_t at, std::uint32_t n) { return std::string_view(content).substr(at, n); }, {}, depth,
                unacked, 32 * 1024);
            REQUIRE(r);
            REQUIRE(sftp::close_file(c, *h));
            CHECK(srv.files["/up/f"] == content);
            REQUIRE(srv.writes.size() == 64);
            std::size_t most = 0;
            std::uint64_t most_bytes = 0;
            std::size_t at_cap = 0;
            for (std::size_t i = 0; i < srv.writes.size(); ++i) {
                const auto& w = srv.writes[i];
                most = std::max(most, w.out);
                most_bytes = std::max(most_bytes, w.out_bytes);
                if (w.out == cap) ++at_cap;
                // One reply at a time: 1..cap for the first fill, then the cap for every refill.
                if (!timed) CHECK(w.out == std::min(i + 1, cap));
            }
            CHECK(most <= cap);
            CHECK(most_bytes <= unacked);
            if (!timed) CHECK(at_cap == srv.writes.size() - cap + 1);
            CHECK(c.pending() == 0);
        }
    }
}

TEST_CASE("sftp: pump_ready takes the replies that are here and waits for none") {
    FakeServer srv;
    srv.files["/f"] = "abc";
    sftp::Client c(srv);
    c.start();
    int answered = 0;
    const auto count = [&](std::expected<sftp::Attrs, sftp::Status> a) { answered += a ? 1 : 0; };
    c.stat("/f", count);
    c.stat("/f", count);
    c.stat("/f", count);
    CHECK(c.pump_ready() == 0); // nothing sent yet: nothing can have come
    c.flush();
    CHECK(c.pump_ready() == 3);
    CHECK(answered == 3);
    CHECK(c.pending() == 0);
    CHECK(c.pump_ready() == 0);

    // Not here yet (the server holds it until the client waits): nothing taken; then waited for.
    srv.held = true;
    c.stat("/f", count);
    c.flush();
    CHECK(c.pump_ready() == 0);
    CHECK(c.pending() == 1);
    c.pump();
    CHECK(answered == 4);

    // A stream handed over a few bytes at a time: a reply that came in pieces is still whole.
    srv.held = false;
    srv.slice = 5;
    c.stat("/f", count);
    c.stat("/f", count);
    c.flush();
    CHECK(c.pump_ready() == 2);
    CHECK(answered == 6);
}

TEST_CASE("sftp: write_range stops at the first failure, and the server's limit is never passed") {
    SUBCASE("a full disk: the WRITEs past the quota fail, the ones before it are acknowledged") {
        FakeServer srv;
        srv.dirs["/up"] = {".", ".."};
        srv.quota_bytes = 100'000;
        sftp::Client c(srv);
        c.start();
        auto h = sftp::open_file(c, "/up/f", creat_excl);
        REQUIRE(h);
        const std::string content = pattern(200'000);
        std::uint64_t acked = 0;
        auto r = sftp::write_range(
            c, *h, 0, content.size(), [&](std::uint64_t at, std::uint32_t n) { return std::string_view(content).substr(at, n); },
            [&](std::uint64_t, std::uint32_t n) { acked += n; }, 4, sftp::max_unacked, 32768);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == sftp::Code::failure);
        CHECK(acked == 3 * 32768);
        CHECK(srv.files["/up/f"] == content.substr(0, std::size_t{3} * 32768));
        CHECK(srv.writes.size() <= 3 + 4); // stopped: what was in flight, no more
        CHECK(c.pending() == 0);
    }
    SUBCASE("a source that ends early") {
        FakeServer srv;
        srv.dirs["/up"] = {".", ".."};
        sftp::Client c(srv);
        c.start();
        auto h = sftp::open_file(c, "/up/f", creat_excl);
        REQUIRE(h);
        const std::string content = pattern(100'000);
        auto r = sftp::write_range(
            c, *h, 0, 300'000,
            [&](std::uint64_t at, std::uint32_t n) { return std::string_view(content).substr(std::min<std::uint64_t>(at, content.size()), n); },
            {}, 4, sftp::max_unacked, 32768);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == sftp::Code::eof);
        CHECK(srv.files["/up/f"].size() == 3 * 32768);
        CHECK(c.pending() == 0);
    }
    SUBCASE("a WRITE past limits@openssh.com ends the session; write_range keeps to its chunk") {
        FakeServer srv;
        srv.dirs["/up"] = {".", ".."};
        srv.max_write = 64 * 1024;
        sftp::Client c(srv);
        c.start();
        auto lim = sftp::limits(c);
        REQUIRE(lim);
        auto h = sftp::open_file(c, "/up/f", creat_excl);
        REQUIRE(h);
        const std::string content = pattern(300'000);
        REQUIRE(put(c, *h, 0, content, static_cast<std::uint32_t>(lim->write), 8));
        CHECK(srv.files["/up/f"] == content);
        c.write(*h, 0, std::string(std::size_t{64} * 1024 + 1, 'x'), {});
        CHECK_THROWS_AS(c.pump(), sftp::Broken);
    }
}

// ---- the server's own copies, owners' names and links (SERVER-OPS.md chunk 0) ----------------

namespace {

// A reply's fields as a server writes them, for the malformed answers below.
std::string be32(std::uint32_t v) {
    std::string out;
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<char>((v >> shift) & 0xFF));
    return out;
}
std::string sstr(std::string_view s) { return be32(static_cast<std::uint32_t>(s.size())) + std::string(s); }

// A folder's handle, for the requests that must refuse one.
std::expected<std::string, sftp::Status> open_dir(sftp::Client& c, std::string_view path) {
    std::optional<std::expected<std::string, sftp::Status>> handle;
    c.opendir(path, [&](std::expected<std::string, sftp::Status> h) { handle = std::move(h); });
    while (!handle) c.pump();
    return std::move(*handle);
}

// What a request answered: its code (0 for none).
template <typename T> std::uint32_t code(const std::expected<T, sftp::Status>& r) {
    return r ? 0 : static_cast<std::uint32_t>(r.error().code);
}

constexpr std::uint32_t eof = 1, no_such_file = 2, denied = 3, failure = 4, bad_message = 5, unsupported = 8;

} // namespace

TEST_CASE("sftp: ATTRS keep the owner, the group and the access time; SETSTAT sends them as SFTP pairs them") {
    FakeServer srv;
    srv.dirs["/d"] = {".", "..", "f"};
    srv.files["/d/f"] = "abc";
    srv.uid = 1001;
    srv.gid = 1002;
    sftp::Client c(srv);
    c.start();
    const auto attrs = [&] {
        auto a = sftp::stat(c, "/d/f");
        REQUIRE(a);
        return *a;
    };
    sftp::Attrs a = attrs();
    CHECK(a.uid == 1001u);
    CHECK(a.gid == 1002u);
    CHECK(a.atime == 1700000000u);
    CHECK(a.mtime == 1700000000u);
    CHECK(a.size == 3u); // the rest as before
    CHECK(a.is_file());

    // The times go together: an atime of its own, else the mtime's; an atime alone isn't sent.
    REQUIRE(sftp::setstat(
        c, "/d/f", sftp::Attrs{.size = {}, .permissions = {}, .mtime = 1600000000, .uid = {}, .gid = {}, .atime = 1650000000}));
    a = attrs();
    CHECK(a.atime == 1650000000u);
    CHECK(a.mtime == 1600000000u);
    REQUIRE(sftp::setstat(c, "/d/f",
                          sftp::Attrs{.size = {}, .permissions = {}, .mtime = 1500000000, .uid = {}, .gid = {}, .atime = {}}));
    a = attrs();
    CHECK(a.atime == 1500000000u);
    CHECK(a.mtime == 1500000000u);
    REQUIRE(sftp::setstat(c, "/d/f",
                          sftp::Attrs{.size = {}, .permissions = {}, .mtime = {}, .uid = {}, .gid = {}, .atime = 1400000000}));
    CHECK(attrs().atime == 1500000000u);

    // The owner goes with the group: the login's own is taken, another's refused (chown(2) for
    // all but root) with nothing changed; one number alone isn't sent.
    REQUIRE(sftp::setstat(c, "/d/f",
                          sftp::Attrs{.size = {}, .permissions = {}, .mtime = {}, .uid = 1001, .gid = 50, .atime = {}}));
    CHECK(attrs().gid == 50u);
    CHECK(code(sftp::setstat(c, "/d/f",
                             sftp::Attrs{.size = {}, .permissions = {}, .mtime = {}, .uid = 0, .gid = 0, .atime = {}})) == denied);
    REQUIRE(sftp::setstat(c, "/d/f", sftp::Attrs{.size = {}, .permissions = {}, .mtime = {}, .uid = 0, .gid = {}, .atime = {}}));
    REQUIRE(sftp::setstat(c, "/d/f", sftp::Attrs{.size = {}, .permissions = {}, .mtime = {}, .uid = {}, .gid = 0, .atime = {}}));
    a = attrs();
    CHECK(a.uid == 1001u);
    CHECK(a.gid == 50u);

    // A listing's entries, a folder's and a link's ATTRS carry them too.
    auto listed = sftp::list(c, "/d");
    REQUIRE(listed);
    REQUIRE(listed->size() == 1);
    CHECK((*listed)[0].attrs.uid == 1001u);
    CHECK((*listed)[0].attrs.gid == 50u);
    CHECK((*listed)[0].attrs.atime == 1500000000u);
    auto folder = sftp::stat(c, "/d");
    REQUIRE(folder);
    CHECK(folder->uid == 1001u);
    CHECK(folder->gid == 1002u);
    srv.links["/d/l"] = "/d/f";
    auto link = sftp::lstat(c, "/d/l");
    REQUIRE(link);
    CHECK(link->is_link());
    CHECK(link->uid == 1001u);
    CHECK(c.pending() == 0);
}

TEST_CASE("sftp: copy-data copies on the server, as OpenSSH's sftp-server answers it") {
    FakeServer srv;
    srv.copy_data = true;
    srv.dirs["/d"] = {".", "..", "src"};
    const std::string content = pattern(300'000);
    srv.files["/d/src"] = content;
    sftp::Client c(srv);
    c.start();
    REQUIRE(c.has_extension(sftp::copy_data_extension));
    auto from = sftp::open_file(c, "/d/src");
    REQUIRE(from);

    // Up to the end of the file (length 0) into a new one: the server's bytes, none through grab.
    auto to = sftp::open_file(c, "/d/a", creat_excl);
    REQUIRE(to);
    REQUIRE(sftp::copy_data(c, *from, 0, 0, *to, 0));
    CHECK(srv.files["/d/a"] == content);
    CHECK(srv.reads.empty());
    CHECK(srv.writes.empty());
    REQUIRE(srv.copies.size() == 1);
    CHECK(srv.copies[0].from == "/d/src");
    CHECK(srv.copies[0].from_offset == 0);
    CHECK(srv.copies[0].length == 0);
    CHECK(srv.copies[0].to == "/d/a");
    CHECK(srv.copies[0].to_offset == 0);

    // A range, over more than one of OpenSSH's 64 KiB pieces, at an offset of another file.
    auto b = sftp::open_file(c, "/d/b", creat_excl);
    REQUIRE(b);
    REQUIRE(sftp::copy_data(c, *from, 1000, 70'000, *b, 10));
    CHECK(srv.files["/d/b"] == std::string(10, '\0') + content.substr(1000, 70'000));
    CHECK(srv.copies.back().from_offset == 1000);
    CHECK(srv.copies.back().length == 70'000);
    CHECK(srv.copies.back().to_offset == 10);
    // A file opened to append takes the bytes at its end, whatever the offset.
    auto appended = sftp::open_file(c, "/d/b", wr | sftp::OpenFlags::append);
    REQUIRE(appended);
    REQUIRE(sftp::copy_data(c, *from, 0, 5, *appended, 0));
    REQUIRE(sftp::close_file(c, *appended));
    CHECK(srv.files["/d/b"].size() == 70'015u);
    CHECK(srv.files["/d/b"].ends_with(content.substr(0, 5)));

    // The file ending first: EOF, what came before it written. Within the last 64 KiB asked for
    // it is answered ok all the same: OpenSSH counts a piece as done when it asks for it.
    auto e = sftp::open_file(c, "/d/e", creat_excl);
    REQUIRE(e);
    CHECK(code(sftp::copy_data(c, *from, 200'000, 200'000, *e, 0)) == eof);
    CHECK(srv.files["/d/e"] == content.substr(200'000));
    auto q = sftp::open_file(c, "/d/q", creat_excl);
    REQUIRE(q);
    REQUIRE(sftp::copy_data(c, *from, 250'000, 60'000, *q, 0));
    CHECK(srv.files["/d/q"] == content.substr(250'000));

    // Refused: the same handle twice, the same file twice, a folder's handle, a handle that isn't
    // open; a target opened only for reading is EBADF, "no such file".
    CHECK(code(sftp::copy_data(c, *from, 0, 0, *from, 0)) == failure);
    auto again = sftp::open_file(c, "/d/src", wr);
    REQUIRE(again);
    CHECK(code(sftp::copy_data(c, *from, 0, 0, *again, 0)) == failure);
    REQUIRE(sftp::close_file(c, *again));
    auto dir = open_dir(c, "/d");
    REQUIRE(dir);
    CHECK(code(sftp::copy_data(c, *dir, 0, 0, *to, 0)) == failure);
    CHECK(code(sftp::copy_data(c, *from, 0, 0, *dir, 0)) == failure);
    CHECK(code(sftp::copy_data(c, "nope", 0, 0, *to, 0)) == failure);
    auto reading = sftp::open_file(c, "/d/a");
    REQUIRE(reading);
    CHECK(code(sftp::copy_data(c, *from, 0, 0, *reading, 0)) == no_such_file);
    CHECK(srv.files["/d/src"] == content);
    CHECK(srv.files["/d/a"] == content);

    // A full disk: "Failure" where the space ends, what fitted written; but within the last 64
    // KiB of a length asked for, ok with nothing written.
    srv.quota_bytes = srv.used() + 100'000;
    auto full = sftp::open_file(c, "/d/full", creat_excl);
    REQUIRE(full);
    CHECK(code(sftp::copy_data(c, *from, 0, 0, *full, 0)) == failure);
    CHECK(srv.files["/d/full"] == content.substr(0, 65'536));
    auto masked = sftp::open_file(c, "/d/m", creat_excl);
    REQUIRE(masked);
    REQUIRE(sftp::copy_data(c, *from, 0, 60'000, *masked, 0));
    CHECK(srv.files["/d/m"].empty());
    srv.quota_bytes = 0;
    // A server gone read-only under an open handle refuses as it refuses any write.
    srv.read_only = true;
    srv.read_only_as = sftp::Code::failure;
    CHECK(code(sftp::copy_data(c, *from, 0, 0, *b, 0)) == failure);
    CHECK(srv.files["/d/b"].size() == 70'015u);
    srv.read_only = false;
    for (const std::string& h : {*from, *to, *b, *e, *q, *dir, *reading, *full, *masked}) REQUIRE(sftp::close_file(c, h));
    CHECK(c.pending() == 0);

    // Not offered: the helper says so and sends nothing; the request itself gets the server's
    // "unsupported" (OpenSSH's answer to an extension it doesn't know).
    FakeServer plain;
    sftp::Client p(plain);
    p.start();
    CHECK_FALSE(p.has_extension(sftp::copy_data_extension));
    CHECK(code(sftp::copy_data(p, "h1", 0, 0, "h2", 0)) == unsupported);
    CHECK(plain.requests.empty());
    std::optional<std::expected<void, sftp::Status>> sent;
    p.copy_data("h1", 0, 0, "h2", 0, [&](std::expected<void, sftp::Status> r) { sent = std::move(r); });
    while (!sent) p.pump();
    CHECK(code(*sent) == unsupported);
}

TEST_CASE("sftp: copy_range copies in pieces, shows each, checks the copy's size and stops between pieces") {
    FakeServer srv;
    srv.copy_data = true;
    srv.dirs["/d"] = {".", "..", "src"};
    const std::string content = pattern(1'000'000);
    srv.files["/d/src"] = content;
    sftp::Client c(srv);
    c.start();
    auto from = sftp::open_file(c, "/d/src");
    REQUIRE(from);
    const auto target = [&](const std::string& path) {
        auto h = sftp::open_file(c, path, creat_excl);
        REQUIRE(h);
        return *h;
    };
    // No time budget: every piece the size given (pieces sized to the pace: the next case).
    constexpr std::chrono::milliseconds no_budget{0};

    // In pieces, never more than asked, each followed by a look at the target's size.
    std::vector<std::uint64_t> seen;
    const std::string to = target("/d/to");
    auto whole = sftp::copy_range(
        c, *from, 0, content.size(), to, 0, no_budget, [&](std::uint64_t n) { seen.push_back(n); }, {}, 300'000);
    REQUIRE(whole);
    CHECK(*whole == content.size());
    CHECK(srv.files["/d/to"] == content);
    CHECK(seen == std::vector<std::uint64_t>{300'000, 600'000, 900'000, 1'000'000});
    REQUIRE(srv.copies.size() == 4);
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(srv.copies[i].from_offset == i * 300'000);
        CHECK(srv.copies[i].to_offset == i * 300'000);
        CHECK(srv.copies[i].length == (i < 3 ? 300'000u : 100'000u)); // never 0, which would copy to the end
    }
    CHECK(std::ranges::count(srv.requests, "FSTAT") == 4);
    CHECK(srv.reads.empty());

    // At offsets of both files, in one piece.
    const std::string at = target("/d/at");
    auto shifted = sftp::copy_range(c, *from, 100, 500'000, at, 7, no_budget);
    REQUIRE(shifted);
    CHECK(*shifted == 500'000u);
    CHECK(srv.files["/d/at"] == std::string(7, '\0') + content.substr(100, 500'000));

    // Stopped after the second piece: what was copied so far, and no more requests; stopped
    // before the first: nothing.
    std::stop_source stop;
    const std::string part = target("/d/part");
    const std::size_t copies_before = srv.copies.size();
    const auto stop_at = [&](std::uint64_t n) {
        if (n >= 600'000) stop.request_stop();
    };
    auto partial = sftp::copy_range(c, *from, 0, content.size(), part, 0, no_budget, stop_at, stop.get_token(), 300'000);
    REQUIRE(partial);
    CHECK(*partial == 600'000u);
    CHECK(srv.copies.size() == copies_before + 2);
    CHECK(srv.files["/d/part"] == content.substr(0, 600'000));
    auto none = sftp::copy_range(c, *from, 0, content.size(), part, 0, no_budget, {}, stop.get_token());
    REQUIRE(none);
    CHECK(*none == 0u);
    CHECK(srv.copies.size() == copies_before + 2);
    // Nothing to copy: no request at all.
    auto empty = sftp::copy_range(c, *from, 0, 0, part, 0, no_budget);
    REQUIRE(empty);
    CHECK(*empty == 0u);
    CHECK(srv.copies.size() == copies_before + 2);

    // The source shorter than asked: EOF from the piece that met its end.
    const std::string longer = target("/d/longer");
    CHECK(code(sftp::copy_range(c, *from, 0, 2'000'000, longer, 0, no_budget, {}, {}, 300'000)) == eof);

    // A piece OpenSSH answers ok though its last 64 KiB failed (a full disk): the target's size
    // gives it away.
    const std::string cut = target("/d/cut");
    srv.quota_bytes = srv.used() + std::uint64_t{3} * 65'536 + 1000;
    auto masked = sftp::copy_range(c, *from, 0, 200'000, cut, 0, no_budget, {}, {}, 300'000);
    REQUIRE_FALSE(masked);
    CHECK(masked.error().code == sftp::Code::failure);
    CHECK(sftp::describe(masked.error()) == "the server's copy came out short: 196608 of 200000 bytes");
    srv.quota_bytes = 0;
    // ...also at an offset of the target: its size must reach where the piece ended there, not
    // just the bytes copied (which the 64 KiB already in it would pass).
    const std::string off = target("/d/off");
    REQUIRE(sftp::copy_range(c, *from, 0, 65'536, off, 0, no_budget));
    srv.quota_bytes = srv.used() + std::uint64_t{3} * 65'536 + 1000;
    auto masked_at = sftp::copy_range(c, *from, 0, 200'000, off, 65'536, no_budget, {}, {}, 300'000);
    REQUIRE_FALSE(masked_at);
    CHECK(masked_at.error().code == sftp::Code::failure);
    CHECK(sftp::describe(masked_at.error()) == "the server's copy came out short: 262144 of 265536 bytes");
    srv.quota_bytes = 0;
    // A server that doesn't tell the size: no way to know, so not taken as copied.
    const std::string untold = target("/d/untold");
    srv.no_size.insert("/d/untold");
    auto unsure = sftp::copy_range(c, *from, 0, 1000, untold, 0, no_budget);
    REQUIRE_FALSE(unsure);
    CHECK(sftp::describe(unsure.error()) == "the server didn't tell the copy's size");
    CHECK(c.pending() == 0);

    // Not offered: unsupported, nothing sent.
    FakeServer plain;
    sftp::Client p(plain);
    p.start();
    CHECK(code(sftp::copy_range(p, "h1", 0, 10, "h2", 0, no_budget)) == unsupported);
    CHECK(plain.requests.empty());
}

TEST_CASE("sftp: copy_range sizes its pieces to the server's pace, each within a third of the connection's timeout") {
    constexpr std::uint64_t mib = 1024 * 1024;
    constexpr std::chrono::milliseconds budget{3000};
    // The pace a piece showed: what fits in a third of the budget, between 1 MiB and the cap.
    using namespace std::chrono_literals;
    CHECK(sftp::next_copy_piece(8 * mib, 4s, budget, 16 * mib) == 2 * mib);
    CHECK(sftp::next_copy_piece(8 * mib, 1s, budget, 16 * mib) == 8 * mib);
    CHECK(sftp::next_copy_piece(8 * mib, 100s, budget, 16 * mib) == sftp::copy_least_piece);
    CHECK(sftp::next_copy_piece(8 * mib, 1ms, budget, 16 * mib) == 16 * mib);
    CHECK(sftp::next_copy_piece(8 * mib, 100s, budget, 300'000) == 300'000u); // the cap below the least piece
    // No budget (a Target made by hand), or no time measured: the cap.
    CHECK(sftp::next_copy_piece(8 * mib, 100s, 0ms, 16 * mib) == 16 * mib);
    CHECK(sftp::next_copy_piece(8 * mib, 100s, -5ms, 16 * mib) == 16 * mib);
    CHECK(sftp::next_copy_piece(8 * mib, 0s, budget, 16 * mib) == 16 * mib);
    CHECK(sftp::copy_first_piece == 8 * mib);
    CHECK(sftp::copy_least_piece == 1 * mib);

    FakeServer srv;
    srv.copy_data = true;
    srv.dirs["/d"] = {".", "..", "src"};
    const std::string content = pattern(46 * mib);
    srv.files["/d/src"] = content;
    // The server's time: each copy-data takes its bytes at `per_s`.
    std::chrono::steady_clock::time_point t{};
    std::uint64_t per_s = 2 * mib;
    srv.on_copy = [&](std::uint64_t length) { t += std::chrono::nanoseconds(length * 1'000'000'000 / per_s); };
    const sftp::Now now = [&] { return t; };
    sftp::Client c(srv);
    c.start();
    auto from = sftp::open_file(c, "/d/src");
    REQUIRE(from);
    auto to = sftp::open_file(c, "/d/to", creat_excl);
    REQUIRE(to);

    // 8 MiB first (the pace unknown); at 2 MiB/s 2 MiB fit a third of 3 s; slower, 1 MiB at the
    // least; fast again, up to the cap at once.
    const auto paced = [&](std::uint64_t done) {
        if (done >= 13 * mib) per_s = 1000 * mib;
        else if (done >= 10 * mib) per_s = mib / 4;
    };
    auto r = sftp::copy_range(c, *from, 0, content.size(), *to, 0, budget, paced, {}, 16 * mib, now);
    REQUIRE(r);
    CHECK(*r == content.size());
    CHECK(srv.files["/d/to"] == content);
    std::vector<std::uint64_t> pieces;
    for (const auto& copy : srv.copies) pieces.push_back(copy.length / mib);
    CHECK(pieces == std::vector<std::uint64_t>{8, 2, 2, 1, 1, 16, 16});
    std::uint64_t at = 0;
    for (const auto& copy : srv.copies) {
        CHECK(copy.from_offset == at);
        CHECK(copy.to_offset == at);
        at += copy.length;
    }

    // A cap below the first piece: the cap from the start.
    srv.copies.clear();
    auto capped = sftp::open_file(c, "/d/capped", creat_excl);
    REQUIRE(capped);
    REQUIRE(sftp::copy_range(c, *from, 0, 1'000'000, *capped, 0, budget, {}, {}, 300'000, now));
    REQUIRE(srv.copies.size() == 4);
    CHECK(srv.copies.front().length == 300'000u);
    CHECK(srv.files["/d/capped"] == content.substr(0, 1'000'000));
    CHECK(c.pending() == 0);
}

TEST_CASE("sftp: users-groups-by-id names numbers in OpenSSH's packed lists; malformed answers break the protocol") {
    FakeServer srv;
    srv.users_groups = true;
    srv.user_names = {{0, "root"}, {1000, "dragon"}};
    srv.group_names = {{33, "www-data"}, {1000, "dragon"}};
    sftp::Client c(srv);
    c.start();
    REQUIRE(c.has_extension(sftp::users_groups_extension));
    const std::array<std::uint32_t, 3> uids{1000, 4242, 0};
    const std::array<std::uint32_t, 2> gids{33, 1000};
    auto names = sftp::users_groups_by_id(c, uids, gids);
    REQUIRE(names);
    CHECK(names->users == std::vector<std::string>{"dragon", "", "root"}); // in the order asked, "" for no name
    CHECK(names->groups == std::vector<std::string>{"www-data", "dragon"});
    auto users_only = sftp::users_groups_by_id(c, std::span(uids).first(1), {});
    REQUIRE(users_only);
    CHECK(users_only->users == std::vector<std::string>{"dragon"});
    CHECK(users_only->groups.empty());
    CHECK(c.pending() == 0);

    // Not offered: unsupported, nothing sent.
    FakeServer plain;
    sftp::Client p(plain);
    p.start();
    CHECK(code(sftp::users_groups_by_id(p, uids, gids)) == unsupported);
    CHECK(plain.requests.empty());

    // Fewer names than numbers asked, a name running past its list, a list running past the
    // reply: the server broke the protocol.
    for (const std::string& reply : {sstr(sstr("dragon")) + sstr(""), sstr(be32(100) + "ab") + sstr(""), be32(50) + "x"}) {
        CAPTURE(reply.size());
        FakeServer bad;
        bad.users_groups = true;
        bad.bad_extended_reply = reply;
        sftp::Client b(bad);
        b.start();
        CHECK_THROWS_AS(std::ignore = sftp::users_groups_by_id(b, std::span(uids).first(2), {}), sftp::ProtocolError);
    }
}

TEST_CASE("sftp: SYMLINK sends OpenSSH's order, READLINK reads the target back, REMOVE takes the link itself") {
    FakeServer srv;
    srv.dirs["/d"] = {".", "..", "f"};
    srv.files["/d/f"] = "data";
    sftp::Client c(srv);
    c.start();

    // The server took the target first and the link second (OpenSSH's order, the draft's
    // reversed): the link is where it was asked for, pointing where asked.
    REQUIRE(sftp::symlink(c, "/d/l", "/d/f"));
    CHECK(srv.links == std::map<std::string, std::string>{{"/d/l", "/d/f"}});
    CHECK(std::ranges::find(srv.dirs["/d"], "l") != srv.dirs["/d"].end()); // listed in its folder
    auto target = sftp::readlink(c, "/d/l");
    REQUIRE(target);
    CHECK(*target == "/d/f");
    auto as_link = sftp::lstat(c, "/d/l");
    REQUIRE(as_link);
    CHECK(as_link->is_link());
    auto through = sftp::stat(c, "/d/l");
    REQUIRE(through);
    CHECK(through->is_file());
    CHECK(through->size == 4u);

    // A relative target is kept as given and followed from the link's folder; one pointing
    // nowhere is made too.
    REQUIRE(sftp::symlink(c, "/d/r", "f"));
    target = sftp::readlink(c, "/d/r");
    REQUIRE(target);
    CHECK(*target == "f");
    through = sftp::stat(c, "/d/r");
    REQUIRE(through);
    CHECK(through->size == 4u);
    REQUIRE(sftp::symlink(c, "/d/dangling", "/d/none"));
    CHECK(code(sftp::stat(c, "/d/dangling")) == no_such_file);
    // ...and an exclusive create there doesn't follow it (O_EXCL).
    CHECK(code(sftp::open_file(c, "/d/dangling", creat_excl)) == failure);
    CHECK_FALSE(srv.files.contains("/d/none"));

    // Refused: an existing name (a file, a link: nothing replaced), a missing folder; READLINK of
    // what isn't a link (OpenSSH: EINVAL) and of a missing name.
    CHECK(code(sftp::symlink(c, "/d/f", "/d/l")) == failure);
    CHECK(srv.files["/d/f"] == "data");
    CHECK(code(sftp::symlink(c, "/d/l", "/d/elsewhere")) == failure);
    CHECK(srv.links["/d/l"] == "/d/f");
    CHECK(code(sftp::symlink(c, "/gone/l", "/d/f")) == no_such_file);
    CHECK(code(sftp::readlink(c, "/d/f")) == bad_message);
    CHECK(code(sftp::readlink(c, "/d/none")) == no_such_file);

    // A link is the file system's: it moves with its folder (pointing where it did) and another
    // login sharing the store sees it.
    REQUIRE(sftp::mkdir(c, "/d/sub"));
    REQUIRE(sftp::symlink(c, "/d/sub/x", "/d/f"));
    REQUIRE(sftp::rename(c, "/d/sub", "/d/moved"));
    target = sftp::readlink(c, "/d/moved/x");
    REQUIRE(target);
    CHECK(*target == "/d/f");
    CHECK(code(sftp::readlink(c, "/d/sub/x")) == no_such_file);
    REQUIRE(sftp::remove(c, "/d/moved/x"));
    REQUIRE(sftp::rmdir(c, "/d/moved"));
    test::FakeFiles store;
    store.dirs["/s"] = {".", ".."};
    FakeServer one;
    one.shared = &store;
    FakeServer two;
    two.shared = &store;
    sftp::Client first(one);
    first.start();
    sftp::Client second(two);
    second.start();
    REQUIRE(sftp::symlink(first, "/s/l", "/s/t"));
    target = sftp::readlink(second, "/s/l");
    REQUIRE(target);
    CHECK(*target == "/s/t");
    CHECK(store.links.size() == 1);

    // REMOVE of a link: the link goes, never its target.
    REQUIRE(sftp::remove(c, "/d/l"));
    CHECK_FALSE(srv.links.contains("/d/l"));
    CHECK(std::ranges::find(srv.dirs["/d"], "l") == srv.dirs["/d"].end());
    CHECK(srv.files["/d/f"] == "data");

    // A server that makes no links; a read-only one: refused, nothing made.
    srv.symlink_refused = true;
    CHECK(code(sftp::symlink(c, "/d/no", "/d/f")) == denied);
    srv.symlink_refused = false;
    srv.read_only = true;
    srv.read_only_as = sftp::Code::failure;
    CHECK(code(sftp::symlink(c, "/d/no", "/d/f")) == failure);
    CHECK_FALSE(srv.links.contains("/d/no"));
    CHECK(srv.links.size() == 2);
    CHECK(c.pending() == 0);

    // READLINK answered with other than one name, or a name running past the reply: the server
    // broke the protocol.
    const std::string two_names = be32(2) + sstr("a") + sstr("a") + be32(0) + sstr("b") + sstr("b") + be32(0);
    for (const std::string& reply : {be32(0), two_names, be32(1) + be32(40) + "short"}) {
        CAPTURE(reply.size());
        FakeServer bad;
        bad.bad_readlink_reply = reply;
        sftp::Client b(bad);
        b.start();
        CHECK_THROWS_AS(std::ignore = sftp::readlink(b, "/l"), sftp::ProtocolError);
    }
}

TEST_CASE("sftp: RENAME takes a link itself; onto a link it answers as OpenSSH and rename(2) do") {
    FakeServer srv;
    srv.dirs["/d"] = {".", "..", "f", "g", "sub"};
    srv.dirs["/d/sub"] = {".", ".."};
    srv.files["/d/f"] = "data";
    srv.files["/d/g"] = "gg";
    sftp::Client c(srv);
    c.start();
    REQUIRE(sftp::symlink(c, "/d/l", "/d/f"));
    REQUIRE(sftp::symlink(c, "/d/dang", "/d/none"));
    // Never a file and a link under one name: LSTAT, READLINK and the folder's listing agree.
    const auto agree = [&] {
        for (const char* name : {"f", "g", "l", "l2", "dang", "m", "n", "sub"}) {
            CAPTURE(name);
            const std::string path = std::string("/d/") + name;
            const auto listed = std::ranges::count(srv.dirs["/d"], name);
            const bool link = srv.links.contains(path);
            const bool there = link || srv.files.contains(path) || srv.dirs.contains(path);
            CHECK(listed == (there ? 1 : 0));
            CHECK(static_cast<int>(link) + static_cast<int>(srv.files.contains(path)) + static_cast<int>(srv.dirs.contains(path)) ==
                  (there ? 1 : 0));
            auto l = sftp::lstat(c, path);
            CHECK(l.has_value() == there);
            if (!l) continue;
            CHECK(l->is_link() == link);
            auto target = sftp::readlink(c, path);
            CHECK(target.has_value() == link);
            if (link && target) CHECK(*target == srv.links[path]);
        }
    };
    agree();

    // The link renamed, pointing where it did; its target untouched.
    REQUIRE(sftp::rename(c, "/d/l", "/d/l2"));
    CHECK(srv.links == std::map<std::string, std::string>{{"/d/l2", "/d/f"}, {"/d/dang", "/d/none"}});
    CHECK(srv.files["/d/f"] == "data");
    agree();

    // RENAME onto a link: a file never (link(2): EEXIST), even onto one pointing nowhere; a folder
    // or a link only when the link points nowhere (else "Failure"), a folder then ENOTDIR.
    CHECK(code(sftp::rename(c, "/d/g", "/d/l2")) == failure);
    CHECK(code(sftp::rename(c, "/d/g", "/d/dang")) == failure);
    CHECK(code(sftp::rename(c, "/d/sub", "/d/l2")) == failure);
    CHECK(code(sftp::rename(c, "/d/sub", "/d/dang")) == no_such_file);
    REQUIRE(sftp::symlink(c, "/d/m", "/d/g"));
    CHECK(code(sftp::rename(c, "/d/m", "/d/l2")) == failure);
    CHECK(srv.links == std::map<std::string, std::string>{{"/d/l2", "/d/f"}, {"/d/dang", "/d/none"}, {"/d/m", "/d/g"}});
    CHECK(srv.files["/d/g"] == "gg");
    agree();
    REQUIRE(sftp::rename(c, "/d/m", "/d/dang"));
    CHECK(srv.links == std::map<std::string, std::string>{{"/d/l2", "/d/f"}, {"/d/dang", "/d/g"}});
    agree();

    // posix-rename (rename(2)) onto a link: a file or a link replaces it, a folder is ENOTDIR.
    CHECK(code(sftp::posix_rename(c, "/d/sub", "/d/dang")) == no_such_file);
    REQUIRE(sftp::symlink(c, "/d/n", "/d/none"));
    REQUIRE(sftp::posix_rename(c, "/d/n", "/d/dang"));
    CHECK(srv.links == std::map<std::string, std::string>{{"/d/l2", "/d/f"}, {"/d/dang", "/d/none"}});
    agree();
    REQUIRE(sftp::posix_rename(c, "/d/g", "/d/l2"));
    CHECK(srv.links == std::map<std::string, std::string>{{"/d/dang", "/d/none"}});
    CHECK(srv.files["/d/l2"] == "gg");
    CHECK_FALSE(srv.files.contains("/d/g"));
    CHECK(srv.files["/d/f"] == "data");
    agree();

    // A link moved onto a file: RENAME "Failure", posix-rename replaces it; onto a folder never.
    CHECK(code(sftp::rename(c, "/d/dang", "/d/l2")) == failure);
    CHECK(code(sftp::posix_rename(c, "/d/dang", "/d/sub")) == failure);
    agree();
    REQUIRE(sftp::posix_rename(c, "/d/dang", "/d/l2"));
    CHECK(srv.links == std::map<std::string, std::string>{{"/d/l2", "/d/none"}});
    CHECK_FALSE(srv.files.contains("/d/l2"));
    CHECK(srv.dirs.contains("/d/sub"));
    agree();
    CHECK(c.pending() == 0);
}
