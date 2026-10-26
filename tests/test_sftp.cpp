#include <doctest/doctest.h>

#include "sftp.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
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

namespace {

// An SFTP server in memory, answering the client's packets as they are written. Its switches
// reorder the replies, shorten READs, cut the stream into small pieces, refuse paths or corrupt
// the stream; it records the READs it was asked for.
class FakeServer : public sftp::Channel {
public:
    std::map<std::string, std::string> files;               // path -> content
    std::map<std::string, std::vector<std::string>> dirs;   // path -> entry names (". and .." too)
    std::map<std::string, std::uint64_t> listed_size;       // sizes to claim in ATTRS instead
    std::set<std::string> denied;                           // answered with permission denied
    std::uint32_t max_data = sftp::max_read;                // a DATA carries at most this
    bool reverse = false;                                   // answer each batch last first
    std::size_t slice = 0;                                  // hand out at most this many bytes per read
    std::size_t readdir_batch = 2;                          // entries per NAME
    bool corrupt = false;                                   // send a packet claiming 1 GiB

    std::vector<std::pair<std::uint64_t, std::uint32_t>> reads; // (offset, length) of each READ
    std::size_t most_waiting = 0;                               // most requests answered in one batch

    void write(std::string_view bytes) override {
        in_ += bytes;
        while (in_.size() >= 4) {
            std::uint32_t len = be32(in_.data());
            if (in_.size() < 4 + len) break;
            handle(std::string_view(in_).substr(4, len));
            in_.erase(0, 4 + len);
        }
    }

    std::size_t read(std::span<char> buffer) override {
        if (out_.empty()) {
            most_waiting = std::max(most_waiting, queue_.size());
            if (reverse) std::ranges::reverse(queue_);
            for (const std::string& r : queue_) out_ += r;
            queue_.clear();
        }
        if (corrupt) out_ = std::string("\x40\x00\x00\x00", 4) + "x";
        if (out_.empty()) throw sftp::Broken("fake server: nothing to send");
        std::size_t n = std::min(buffer.size(), out_.size());
        if (slice) n = std::min(n, slice);
        std::memcpy(buffer.data(), out_.data(), n);
        out_.erase(0, n);
        return n;
    }

private:
    static std::uint32_t be32(const char* p) {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v = (v << 8) | static_cast<std::uint8_t>(p[i]);
        return v;
    }
    struct In {
        std::string_view s;
        std::uint32_t u32() {
            std::uint32_t v = be32(s.data());
            s.remove_prefix(4);
            return v;
        }
        std::uint64_t u64() {
            std::uint64_t h = u32();
            return (h << 32) | u32();
        }
        std::string str() {
            std::uint32_t n = u32();
            std::string v(s.substr(0, n));
            s.remove_prefix(n);
            return v;
        }
    };
    static void u8(std::string& o, std::uint8_t v) { o.push_back(static_cast<char>(v)); }
    static void u32(std::string& o, std::uint32_t v) {
        for (int sh = 24; sh >= 0; sh -= 8) o.push_back(static_cast<char>((v >> sh) & 0xFF));
    }
    static void u64(std::string& o, std::uint64_t v) {
        u32(o, static_cast<std::uint32_t>(v >> 32));
        u32(o, static_cast<std::uint32_t>(v));
    }
    static void str(std::string& o, std::string_view v) {
        u32(o, static_cast<std::uint32_t>(v.size()));
        o.append(v);
    }
    void reply(std::uint8_t type, std::uint32_t id, const std::string& body) {
        std::string r;
        u32(r, static_cast<std::uint32_t>(1 + 4 + body.size()));
        u8(r, type);
        u32(r, id);
        r += body;
        queue_.push_back(std::move(r));
    }
    void status(std::uint32_t id, std::uint32_t code, std::string_view msg = "") {
        std::string b;
        u32(b, code);
        str(b, msg);
        str(b, "en");
        reply(101, id, b);
    }
    void attrs(std::string& b, const std::string& path) {
        if (dirs.contains(path)) {
            u32(b, 0x4 | 0x8);
            u32(b, 040755);
            u32(b, 0);
            u32(b, 1700000000);
            return;
        }
        u32(b, 0x1 | 0x4 | 0x8);
        u64(b, listed_size.contains(path) ? listed_size[path] : files[path].size());
        u32(b, 0100644);
        u32(b, 0);
        u32(b, 1700000000);
    }
    bool known(const std::string& path) const { return files.contains(path) || dirs.contains(path); }
    std::string new_handle(std::string path) {
        std::string h = "h" + std::to_string(next_handle_++);
        handles_[h] = {std::move(path), 0};
        return h;
    }

    void handle(std::string_view packet) {
        auto type = static_cast<std::uint8_t>(packet[0]);
        In in{packet.substr(1)};
        if (type == 1) { // INIT: no id
            std::string r;
            std::string b;
            u32(b, 3);
            str(b, "posix-rename@openssh.com");
            str(b, "1");
            u32(r, static_cast<std::uint32_t>(1 + b.size()));
            u8(r, 2);
            r += b;
            queue_.push_back(std::move(r));
            return;
        }
        std::uint32_t id = in.u32();
        switch (type) {
        case 3: // OPEN
        case 11: { // OPENDIR
            std::string path = in.str();
            if (denied.contains(path)) return status(id, 3, "Permission denied");
            if (type == 3 ? !files.contains(path) : !dirs.contains(path)) return status(id, 2, "No such file");
            std::string b;
            str(b, new_handle(path));
            return reply(102, id, b);
        }
        case 5: { // READ
            const std::string& content = files[handles_.at(in.str()).path];
            std::uint64_t off = in.u64();
            std::uint32_t len = in.u32();
            reads.emplace_back(off, len);
            if (off >= content.size()) return status(id, 1, "EOF");
            std::size_t n = std::min<std::uint64_t>({len, max_data, content.size() - off});
            std::string b;
            str(b, std::string_view(content).substr(off, n));
            return reply(103, id, b);
        }
        case 4: // CLOSE
            handles_.erase(in.str());
            return status(id, 0);
        case 12: { // READDIR
            auto& h = handles_.at(in.str());
            const auto& names = dirs[h.path];
            if (h.pos >= names.size()) return status(id, 1, "EOF");
            std::string b;
            std::size_t n = std::min(readdir_batch, names.size() - h.pos);
            u32(b, static_cast<std::uint32_t>(n));
            for (std::size_t i = 0; i < n; ++i) {
                const std::string& name = names[h.pos + i];
                str(b, name);
                str(b, "-rw-r--r-- 1 u g 0 Jan 1 00:00 " + name);
                std::string full = h.path == "/" ? "/" + name : h.path + "/" + name;
                attrs(b, name == "." || name == ".." ? h.path : full);
            }
            h.pos += n;
            return reply(104, id, b);
        }
        case 7: // LSTAT
        case 17: { // STAT
            std::string path = in.str();
            if (!known(path)) return status(id, 2, "No such file");
            std::string b;
            attrs(b, path);
            return reply(105, id, b);
        }
        case 8: { // FSTAT
            std::string b;
            attrs(b, handles_.at(in.str()).path);
            return reply(105, id, b);
        }
        case 16: { // REALPATH
            std::string path = in.str();
            std::string b;
            u32(b, 1);
            str(b, path == "." ? "/home/test" : path);
            str(b, "");
            u32(b, 0);
            return reply(104, id, b);
        }
        default: return status(id, 8, "Operation unsupported");
        }
    }

    struct OpenHandle {
        std::string path;
        std::size_t pos;
    };
    std::string in_, out_;
    std::vector<std::string> queue_;
    std::map<std::string, OpenHandle> handles_;
    int next_handle_ = 1;
};

std::string pattern(std::size_t n) {
    std::string s(n, '\0');
    for (std::size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 131 + i / 977) & 0xFF);
    return s;
}

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
