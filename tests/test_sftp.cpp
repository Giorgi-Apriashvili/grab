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
    auto h = sftp::open_file(c, "/up/a", creat_excl, sftp::Attrs{.size = {}, .permissions = 0600, .mtime = {}});
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
    REQUIRE(sftp::fsetstat(c, *h, sftp::Attrs{.size = {}, .permissions = {}, .mtime = 1600000000}));
    REQUIRE(sftp::close_file(c, *h));
    REQUIRE(sftp::setstat(c, "/up/f", sftp::Attrs{.size = 4, .permissions = 0640, .mtime = {}}));
    auto a = sftp::stat(c, "/up/f");
    REQUIRE(a);
    CHECK(a->mtime == 1600000000u);
    CHECK(a->size == 4u);
    CHECK(a->permissions == 0100640u);
    auto gone = sftp::setstat(c, "/up/none", sftp::Attrs{.size = {}, .permissions = {}, .mtime = 1});
    REQUIRE_FALSE(gone);
    CHECK(gone.error().code == sftp::Code::no_such_file);

    // Folders: an existing name, a missing parent, one with something in it.
    REQUIRE(sftp::mkdir(c, "/up/d", sftp::Attrs{.size = {}, .permissions = 0700, .mtime = {}}));
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
    CHECK(refused(sftp::setstat(c, "/ro/f", sftp::Attrs{.size = {}, .permissions = {}, .mtime = 1}), denied));
    CHECK(refused(sftp::rmdir(c, "/ro"), denied));
    auto got = read_all(c, "/ro/f", 0, 4, 2, 1024);
    REQUIRE(got);
    CHECK(*got == "data");
    auto fs = sftp::statvfs(c, "/ro");
    REQUIRE(fs);
    CHECK(fs->read_only());

    // A read-only file system, as OpenSSH answers EROFS: "Failure".
    srv.read_only_as = sftp::Code::failure;
    auto erofs = sftp::open_file(c, "/ro/x", creat_excl);
    REQUIRE_FALSE(erofs);
    CHECK(erofs.error().code == sftp::Code::failure);
    CHECK(sftp::describe(erofs.error()) == "Failure");
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
