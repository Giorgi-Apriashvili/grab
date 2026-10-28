#include <doctest/doctest.h>

#include "conn_budget.hpp"
#include "fake_pool.hpp"
#include "fake_sftp.hpp"
#include "fetch.hpp"
#include "play_stream.hpp"
#include "sftp.hpp"
#include "ssh_pool.hpp"

#include <winsock2.h> // IWYU pragma: keep (the socket API)

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace grab;
using namespace std::chrono_literals;
using play::mib;

namespace {

using Piece = play::Plan::Piece;
using Kind = play::Plan::Kind;

play::Plan::Lanes lanes(bool busy, int idle_others, int reading) {
    return play::Plan::Lanes{.busy = busy, .idle_others = idle_others, .reading = reading};
}

// "http://127.0.0.1:PORT/TOKEN/ID/name", taken apart.
struct Url {
    int port = 0;
    std::string token;
    std::string path; // "/TOKEN/ID/name"
};
std::optional<Url> url_of(std::string_view url) {
    constexpr std::string_view at = "http://127.0.0.1:";
    if (!url.starts_with(at)) return std::nullopt;
    url.remove_prefix(at.size());
    Url u;
    const auto [end, ec] = std::from_chars(url.data(), url.data() + url.size(), u.port);
    if (ec != std::errc{} || end == url.data() + url.size() || *end != '/') return std::nullopt;
    u.path = std::string(end, url.data() + url.size());
    const auto slash = u.path.find('/', 1);
    if (slash == std::string::npos) return std::nullopt;
    u.token = u.path.substr(1, slash - 1);
    return u;
}

// One connection to the server on this PC.
class Socket {
public:
    // `receive_buffer` > 0: a small one, so a client that stops reading holds the server's sends.
    explicit Socket(int port, int receive_buffer = 0) {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
        s_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        to.sin_port = htons(static_cast<u_short>(port));
        const DWORD limit = 20000; // a hang fails the test instead of stopping it
        setsockopt(s_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&limit), sizeof(limit));
        if (receive_buffer > 0) {
            setsockopt(s_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&receive_buffer), sizeof(receive_buffer));
        }
        ok_ = ::connect(s_, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == 0;
    }
    ~Socket() {
        closesocket(s_);
        WSACleanup();
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    [[nodiscard]] bool ok() const { return ok_; }
    void close_now() { shutdown(s_, SD_BOTH); } // the server's sends and waits on it end
    bool send(std::string_view data) {
        while (!data.empty()) {
            const int n = ::send(s_, data.data(), static_cast<int>(data.size()), 0);
            if (n <= 0) return false;
            data.remove_prefix(static_cast<std::size_t>(n));
        }
        return true;
    }
    // Everything until the server closes (or the 20 s limit).
    std::string rest() {
        std::string out;
        std::array<char, 65536> buf{};
        for (;;) {
            const int n = ::recv(s_, buf.data(), static_cast<int>(buf.size()), 0);
            if (n <= 0) break;
            out.append(buf.data(), static_cast<std::size_t>(n));
        }
        return out;
    }

private:
    SOCKET s_ = INVALID_SOCKET;
    bool ok_ = false;
};

struct Response {
    int status = 0;
    std::map<std::string, std::string> headers; // names in lower case
    std::string body;
};
Response response_of(const std::string& raw) {
    Response r;
    const auto end = raw.find("\r\n\r\n");
    if (end == std::string::npos || !raw.starts_with("HTTP/1.1 ")) return r;
    std::from_chars(raw.data() + 9, raw.data() + 12, r.status);
    for (std::size_t at = raw.find("\r\n") + 2; at < end;) {
        const auto next = raw.find("\r\n", at);
        const std::string line = raw.substr(at, next - at);
        const auto colon = line.find(':');
        std::string name = line.substr(0, colon);
        std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        r.headers[name] = line.substr(line.find_first_not_of(' ', colon + 1));
        at = next + 2;
    }
    r.body = raw.substr(end + 4);
    return r;
}

// One request to the stream at `u`; `head`: extra header lines, each ending in CRLF.
Response ask(const Url& u, const std::string& head = {}, std::string_view method = "GET", const std::string& path = {}) {
    Socket s(u.port);
    REQUIRE(s.ok());
    s.send(std::format("{} {} HTTP/1.1\r\nHost: 127.0.0.1:{}\r\n{}\r\n", method, path.empty() ? u.path : path, u.port, head));
    return response_of(s.rest());
}

struct Played {
    test::FakePool fake;
    std::string content;
    fetch::Connections connections;
    std::shared_ptr<ssh::Pool> pool;
    std::optional<play::Server> server;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> progress;

    explicit Played(std::uint64_t size, play::Server::Options o = {}, int budget = 8) : content(test::pattern(size)) {
        fake.files["/v/a.mp4"] = content;
        pool = fake.pool(budget);
        connections.configure("Main", budget);
        server.emplace(connections, o);
    }
    play::Server::File file(int connections_count = 2, std::string path = "/v/a.mp4", std::string name = "a.mp4") {
        return play::Server::File{.server = "Main", .pool = pool, .path = std::move(path), .name = std::move(name),
                                  .connections = connections_count, .logins_at_once = 0, .ahead = 16 * mib,
                                  .close_on_release = false};
    }
    std::expected<std::string, std::string> start(play::Server::File f, std::stop_token stop = {}) {
        return server->play(std::move(f), [this](std::uint64_t done, std::uint64_t total) { progress.emplace_back(done, total); },
                            std::move(stop));
    }
};

// Waits up to `limit` for `done`; whether it came.
template <class F>
bool within(std::chrono::milliseconds limit, F done) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= until) return false;
        std::this_thread::sleep_for(20ms);
    }
    return true;
}

struct Bare : ssh::Session {
    sftp::Client& sftp() override { throw sftp::Broken("no SFTP here"); }
};

} // namespace

TEST_CASE("play: byte ranges kept merged, and the first gap two sets leave") {
    play::Ranges r;
    r.add(10, 20);
    r.add(30, 40);
    r.add(20, 25); // touching: one range
    CHECK(r.until(10) == 25);
    CHECK(r.until(24) == 25);
    CHECK(r.until(25) == 25);
    CHECK(r.until(5) == 5);
    CHECK(r.next_after(25, 100) == 30);
    CHECK(r.next_after(35, 100) == 100);
    CHECK(r.bytes_in(0, 100) == 25);
    CHECK(r.bytes_in(15, 35) == 15);
    CHECK(r.covers(30, 40));
    CHECK_FALSE(r.covers(24, 31));
    r.remove(12, 14);
    CHECK(r.bytes_in(0, 100) == 23);
    CHECK(r.until(10) == 12);
    CHECK(r.until(14) == 25);
    r.remove(0, 1000);
    CHECK(r.empty());

    play::Ranges a;
    play::Ranges b;
    a.add(0, 10);
    b.add(10, 15);
    a.add(20, 30);
    CHECK(play::gap(a, b, 0, 100) == std::pair<std::uint64_t, std::uint64_t>{15, 20});
    CHECK(play::gap(a, b, 21, 100) == std::pair<std::uint64_t, std::uint64_t>{30, 100});
    CHECK_FALSE(play::gap(a, b, 0, 15).has_value());
    CHECK_FALSE(play::gap(a, b, 5, 5).has_value());
}

TEST_CASE("play: a request's head as the server reads it") {
    auto r = play::parse_request("GET /t/1/a.mkv HTTP/1.1\r\nHost: 127.0.0.1:5000\r\nRange:  bytes=0-\r\nUser-Agent: x\r\n\r\n");
    REQUIRE(r);
    CHECK(r->method == "GET");
    CHECK(r->target == "/t/1/a.mkv");
    CHECK(r->host == "127.0.0.1:5000");
    CHECK(r->range == "bytes=0-");
    r = play::parse_request("HEAD /x HTTP/1.0\r\n\r\n");
    REQUIRE(r);
    CHECK_FALSE(r->host.has_value());
    CHECK_FALSE(r->range.has_value());
    CHECK_FALSE(play::parse_request("GET /x\r\n\r\n"));                                    // no version
    CHECK_FALSE(play::parse_request("GET x HTTP/1.1\r\n\r\n"));                            // not a path
    CHECK_FALSE(play::parse_request("G(T /x HTTP/1.1\r\n\r\n"));                           // not a method
    CHECK_FALSE(play::parse_request("GET /x HTTP/1.1\r\nHost 1\r\n\r\n"));                 // no colon
    CHECK_FALSE(play::parse_request("GET /x HTTP/1.1\r\nA: b\r\n  folded\r\n\r\n"));       // a folded line
    CHECK_FALSE(play::parse_request("GET /x HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n"));    // two hosts
    CHECK_FALSE(play::parse_request("GET /x HTTP/1.1\r\nHo st: a\r\n\r\n"));               // a space in a name
    CHECK_FALSE(play::parse_request("GET  /x HTTP/1.1\r\n\r\n"));                          // two spaces
}

TEST_CASE("play: answers to whole files, one range, a suffix, past the end, several ranges and HEAD") {
    play::Request get{.method = "GET", .target = "/t/1/a", .host = {}, .range = {}};
    auto a = play::answer(get, 1000, "video/mp4");
    CHECK(a.status == 200);
    CHECK(a.first == 0);
    CHECK(a.length == 1000);
    CHECK(a.head.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(a.head.find("Content-Length: 1000\r\n") != std::string::npos);
    CHECK(a.head.find("Accept-Ranges: bytes\r\n") != std::string::npos);
    CHECK(a.head.ends_with("\r\n\r\n"));

    get.range = "bytes=100-199";
    a = play::answer(get, 1000, "video/mp4");
    CHECK(a.status == 206);
    CHECK(a.first == 100);
    CHECK(a.length == 100);
    CHECK(a.head.find("Content-Range: bytes 100-199/1000\r\n") != std::string::npos);
    get.range = "bytes=-300"; // the last 300
    a = play::answer(get, 1000, "video/mp4");
    CHECK(a.first == 700);
    CHECK(a.length == 300);
    get.range = "bytes=1000-";
    a = play::answer(get, 1000, "video/mp4");
    CHECK(a.status == 416);
    CHECK(a.length == 0);
    CHECK(a.head.find("Content-Range: bytes */1000\r\n") != std::string::npos);
    get.range = "bytes=0-1,5-9"; // several: the whole file
    CHECK(play::answer(get, 1000, "video/mp4").status == 200);
    // Numbers too big for 64 bits aren't a range this understands: the whole file, nothing wraps round.
    get.range = "bytes=99999999999999999999999-";
    a = play::answer(get, 1000, "video/mp4");
    CHECK(a.status == 200);
    CHECK(a.length == 1000);
    get.range = "bytes=0-99999999999999999999999"; // to the end
    a = play::answer(get, 1000, "video/mp4");
    CHECK(a.status == 206);
    CHECK(a.length == 1000);
    get.range = "bytes=18446744073709551614-18446744073709551615";
    CHECK(play::answer(get, std::uint64_t{1} << 63, "video/mp4").status == 416);
    get.range = "bytes=10-18446744073709551615";
    a = play::answer(get, 18446744073709551615ULL, "video/mp4");
    CHECK(a.status == 206);
    CHECK(a.length == 18446744073709551615ULL - 10);
    // An empty file: the whole of it, nothing in a range.
    get.range.reset();
    CHECK(play::answer(get, 0, "video/mp4").length == 0);

    CHECK(play::media_type("A.MKV") == "video/x-matroska");
    CHECK(play::media_type("x.flac") == "audio/flac");
    CHECK(play::media_type("x") == "application/octet-stream");
    CHECK(play::same_token("abc", "abc"));
    CHECK_FALSE(play::same_token("abc", "abd"));
    CHECK_FALSE(play::same_token("abc", "abcd"));
    CHECK_FALSE(play::same_token("", "a"));
}

TEST_CASE("play: a request waiting for bytes comes first, from an idle connection, then read-ahead with one kept for a seek") {
    play::Plan p({.size = 100 * mib, .ahead = 16 * mib});
    const int r = p.add_reader(50 * mib, 100 * mib - 1);
    // A busy connection leaves the waiting request's bytes to an idle one (its READs would wait
    // behind the busy one's); it may read ahead further on.
    auto busy = p.next(lanes(true, 1, 3));
    REQUIRE(busy);
    CHECK(busy->kind == Kind::ahead);
    CHECK(busy->start > 50 * mib);
    p.taken.remove(busy->start, busy->end);
    // The idle one: a small piece where the request waits.
    CHECK(p.next(lanes(false, 1, 3)) == Piece{.start = 50 * mib, .end = 51 * mib, .kind = Kind::waiting});
    // Read-ahead: the nearest missing bytes, while another connection stays idle.
    CHECK(p.next(lanes(false, 1, 3)) == Piece{.start = 51 * mib, .end = 53 * mib, .kind = Kind::ahead});
    CHECK_FALSE(p.next(lanes(false, 0, 3)).has_value()); // the last idle one stays for a seek
    CHECK(p.next(lanes(true, 0, 3)) == Piece{.start = 53 * mib, .end = 55 * mib, .kind = Kind::ahead});
    CHECK(p.next(lanes(false, 0, 1)) == Piece{.start = 55 * mib, .end = 57 * mib, .kind = Kind::ahead}); // alone: no spare
    CHECK_FALSE(p.next(lanes(false, 1, 3), false).has_value()); // the cache is full
    // Up to 16 MiB ahead of the request, no further.
    while (p.next(lanes(false, 1, 3))) {
    }
    CHECK(p.taken.until(50 * mib) == 66 * mib);
    CHECK(p.wanted(60 * mib));
    CHECK_FALSE(p.wanted(40 * mib)); // behind it
    CHECK_FALSE(p.wanted(90 * mib)); // far ahead
    // A seek: the request goes elsewhere; what it no longer needs isn't wanted any more.
    p.move_reader(r, 10 * mib);
    CHECK_FALSE(p.wanted(60 * mib));
    CHECK(p.next(lanes(false, 1, 3)) == Piece{.start = 10 * mib, .end = 11 * mib, .kind = Kind::waiting});
    p.remove_reader(r);
    CHECK_FALSE(p.any_work());
    CHECK_FALSE(p.wanted(10 * mib));
}

TEST_CASE("play: the warm part dealt out, its start first, then the end, pieces shrinking near its end (review G37)") {
    const std::uint64_t size = 1000 * mib;
    play::Plan p({.size = size});
    // An MKV: its start taken as 32 MiB before its first bytes say how far it goes.
    p.warm(32 * mib, size - 4 * mib);
    CHECK(p.next(lanes(false, 7, 8)) == Piece{.start = 0, .end = 2 * mib, .kind = Kind::warm});
    CHECK(p.next(lanes(false, 6, 8)) == Piece{.start = size - 4 * mib, .end = size - 2 * mib, .kind = Kind::warm});
    std::vector<Piece> pieces;
    while (auto piece = p.next(lanes(false, 5, 8))) pieces.push_back(*piece);
    // All of it handed out at once, the MKV's 16-32 MiB too, none of it twice.
    CHECK(p.taken.covers(0, 32 * mib));
    CHECK(p.taken.covers(size - 4 * mib, size));
    CHECK(p.taken.bytes_in(0, size) == 36 * mib);
    CHECK(pieces.back().end - pieces.back().start < 2 * mib);
    CHECK(std::ranges::all_of(pieces, [](const Piece& x) { return x.end - x.start <= 2 * mib; }));
    // The first bytes say the start goes to 9 MiB: the rest of the 32 isn't wanted any more.
    p.warm(9 * mib, size - 4 * mib);
    CHECK(p.wanted(5 * mib));
    CHECK_FALSE(p.wanted(20 * mib));
    CHECK(p.wanted(size - mib));
    CHECK(p.warm_progress() == std::pair<std::uint64_t, std::uint64_t>{0, 13 * mib});
    p.have.add(0, 9 * mib);
    CHECK_FALSE(p.warm_in());
    p.have.add(size - 4 * mib, size);
    CHECK(p.warm_in());
    // A file shorter than STAT said: the end's part ends where the file does.
    play::Plan cut({.size = 100 * mib});
    cut.warm(8 * mib, 96 * mib);
    cut.ended_at(97 * mib);
    cut.have.add(0, 8 * mib);
    cut.have.add(96 * mib, 97 * mib);
    CHECK(cut.warm_in());
    // A small file is all start.
    play::Plan small_file({.size = 3 * mib});
    small_file.warm(8 * mib, 3 * mib);
    CHECK(small_file.warm_progress().second == 3 * mib);
}

TEST_CASE("play: what the cache lets go when full: behind the requests first, never the warm part or bytes on their way") {
    play::Plan p({.size = 100 * mib, .ahead = 4 * mib});
    p.warm(8 * mib, 96 * mib);
    p.warm_done();
    p.add_reader(50 * mib, 100 * mib - 1);
    p.taken.add(60 * mib, 61 * mib);
    const std::vector<std::uint64_t> blocks{0, 10 * mib, 20 * mib, 49 * mib, 51 * mib, 60 * mib, 90 * mib, 97 * mib};
    CHECK(p.evictable(blocks, mib) == std::vector<std::uint64_t>{10 * mib, 20 * mib, 49 * mib, 90 * mib});
}

TEST_CASE("play: over the local server, the warm part before the address, then any range from the file's bytes") {
    const std::uint64_t size = 40 * mib + 12345;
    Played t(size);
    const auto address = t.start(t.file());
    REQUIRE_MESSAGE(address, address.error());
    const auto u = url_of(*address);
    REQUIRE(u);
    CHECK(address->ends_with("/a.mp4"));
    CHECK(u->token.size() == 32);
    // The warm part (8 MiB of the start, the last 4 MiB) came in before the address.
    REQUIRE_FALSE(t.progress.empty());
    CHECK(t.progress.back().first == t.progress.back().second);
    CHECK(t.progress.back().second == 12 * mib);
    CHECK(t.server->stats().cached >= 12 * mib);

    auto r = ask(*u, "Range: bytes=0-99\r\n");
    CHECK(r.status == 206);
    CHECK(r.headers["content-range"] == std::format("bytes 0-99/{}", size));
    CHECK(r.body == t.content.substr(0, 100));
    r = ask(*u, "Range: bytes=-1000\r\n");
    CHECK(r.status == 206);
    CHECK(r.body == t.content.substr(size - 1000));
    // Outside the warm part: fetched as asked for.
    r = ask(*u, "Range: bytes=20000000-25999999\r\n");
    CHECK(r.status == 206);
    CHECK(r.body == t.content.substr(20000000, 6000000));
    r = ask(*u, std::format("Range: bytes={}-\r\n", size));
    CHECK(r.status == 416);
    CHECK(r.headers["content-range"] == std::format("bytes */{}", size));
    r = ask(*u, "", "HEAD");
    CHECK(r.status == 200);
    CHECK(r.headers["content-length"] == std::to_string(size));
    CHECK(r.headers["content-type"] == "video/mp4");
    CHECK(r.body.empty());
    r = ask(*u, "Range: bytes=0-1,5-9\r\n"); // several ranges: the whole file
    CHECK(r.status == 200);
    CHECK(r.body.size() == size);
    CHECK(r.body == t.content);
}

TEST_CASE("play: the local server refuses a wrong token, a wrong Host, other methods, a huge or slow head and too many requests") {
    play::Server::Options o;
    o.head_within = 400ms;
    o.max_clients = 3;
    Played t(3 * mib, o);
    const auto address = t.start(t.file());
    REQUIRE(address);
    const auto u = url_of(*address);
    REQUIRE(u);
    CHECK(ask(*u, "", "GET", "/" + std::string(32, '0') + u->path.substr(33)).status == 404); // another token
    CHECK(ask(*u, "", "GET", u->path.substr(0, 34) + "999/a.mp4").status == 404);                // another stream
    CHECK(ask(*u, "", "POST").status == 405);
    {
        Socket s(u->port);
        s.send(std::format("GET {} HTTP/1.1\r\nHost: evil.example:{}\r\n\r\n", u->path, u->port)); // DNS rebinding
        CHECK(response_of(s.rest()).status == 403);
    }
    {
        Socket s(u->port);
        s.send(std::format("GET {} HTTP/1.1\r\n\r\n", u->path)); // no Host
        CHECK(response_of(s.rest()).status == 403);
    }
    {
        Socket s(u->port);
        s.send(std::format("GET {} HTTP/1.1\r\nHost: 127.0.0.1:{}\r\nX: {}\r\n\r\n", u->path, u->port, std::string(17000, 'x')));
        CHECK(response_of(s.rest()).status == 431);
    }
    {
        // A head that never ends is closed after head_within, unanswered.
        Socket s(u->port);
        s.send(std::format("GET {} HTTP/1.1\r\n", u->path));
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(s.rest().empty());
        CHECK(std::chrono::steady_clock::now() - t0 < 5s);
    }
    {
        // Three requests taking their time hold every place: a fourth is closed at once.
        Socket a(u->port);
        Socket b(u->port);
        Socket c(u->port);
        std::this_thread::sleep_for(100ms);
        Socket d(u->port);
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(d.rest().empty());
        CHECK(std::chrono::steady_clock::now() - t0 < 300ms);
    }
    std::this_thread::sleep_for(300ms); // their requests end as they close
    CHECK(ask(*u, "Range: bytes=0-9\r\n").body == t.content.substr(0, 10)); // the places came back
}

TEST_CASE("play: two requests on one stream at once, one far from the other, each gets its own bytes") {
    const std::uint64_t size = 64 * mib;
    Played t(size);
    t.fake.slice = std::size_t{64} << 10; // the bytes come in small parts, as from a network
    const auto address = t.start(t.file(3));
    REQUIRE(address);
    const auto u = url_of(*address);
    REQUIRE(u);
    Response early;
    Response later;
    std::thread a([&] { early = ask(*u, "Range: bytes=0-33554431\r\n"); });
    std::thread b([&] { later = ask(*u, "Range: bytes=50000000-55999999\r\n"); });
    a.join();
    b.join();
    CHECK(early.status == 206);
    CHECK(early.body == t.content.substr(0, 32 * mib));
    CHECK(later.status == 206);
    CHECK(later.body == t.content.substr(50000000, 6000000));
}

TEST_CASE("play: a seek is served from the idle connection at once, not behind the reads ahead of another request") {
    const std::uint64_t size = 96 * mib;
    Played t(size);
    const auto address = t.start(t.file(3)); // two read ahead, one stays idle for a seek
    REQUIRE(address);
    const auto u = url_of(*address);
    REQUIRE(u);
    // From now on each connection is slow (4 KiB per pause): one with 8 READs of 255 KiB on their
    // way needs hundreds of pauses before a new READ of it is answered.
    t.fake.slice_from_now = std::size_t{4} << 10;
    t.fake.slow_from_now = 2;
    std::atomic<bool> done{false};
    Response early;
    std::thread reader([&] {
        early = ask(*u, "Range: bytes=20971520-62914559\r\n");
        done = true;
    });
    std::this_thread::sleep_for(500ms); // the reads ahead of it are on their way
    const auto t0 = std::chrono::steady_clock::now();
    const auto seek = ask(*u, "Range: bytes=80000000-80099999\r\n");
    const auto took = std::chrono::steady_clock::now() - t0;
    CHECK(seek.status == 206);
    CHECK(seek.body == t.content.substr(80000000, 100000));
    CHECK(took < 3s);
    t.fake.slow_from_now = 0; // the other request may finish quickly now
    reader.join();
    CHECK(done);
    CHECK(early.body == t.content.substr(20971520, 41943040));
}

TEST_CASE("play: an idle stream gives its connections and seats back, then its data, and is played again by its id") {
    play::Server::Options o;
    o.release_after = 200ms;
    o.free_after = 600ms;
    o.housekeeping = 50ms;
    const std::uint64_t size = 30 * mib;
    Played t(size, o);
    const auto address = t.start(t.file());
    REQUIRE(address);
    const auto u = url_of(*address);
    REQUIRE(u);
    CHECK(ask(*u, "Range: bytes=10000000-10999999\r\n").body == t.content.substr(10000000, 1000000));
    auto stats = t.server->stats();
    CHECK(stats.connected > 0);
    // Connections and seats back release_after after the last request.
    for (int i = 0; i < 100 && (stats.connected > 0 || stats.seats > 0); ++i) {
        std::this_thread::sleep_for(50ms);
        stats = t.server->stats();
    }
    CHECK(stats.connected == 0);
    CHECK(stats.seats == 0);
    CHECK(t.pool->idle() > 0); // back in the pool, open for the next user
    // The data free_after after it (review G141, G145); the id stays.
    for (int i = 0; i < 100 && stats.with_data > 0; ++i) {
        std::this_thread::sleep_for(50ms);
        stats = t.server->stats();
    }
    CHECK(stats.with_data == 0);
    CHECK(stats.cached == 0);
    CHECK(stats.streams == 1);
    const auto again = ask(*u, "Range: bytes=20000000-20999999\r\n");
    CHECK(again.status == 206);
    CHECK(again.body == t.content.substr(20000000, 1000000));
}

TEST_CASE("play: data kept for two streams not in use; a third Play lets the oldest go") {
    play::Server::Options o;
    o.housekeeping = 50ms;
    Played t(20 * mib, o);
    t.fake.files["/v/b.mp4"] = t.content;
    t.fake.files["/v/c.mp4"] = t.content;
    REQUIRE(t.start(t.file(2, "/v/a.mp4", "a.mp4")));
    std::this_thread::sleep_for(20ms); // `used` apart
    REQUIRE(t.start(t.file(2, "/v/b.mp4", "b.mp4")));
    std::this_thread::sleep_for(20ms);
    const auto third = t.start(t.file(2, "/v/c.mp4", "c.mp4"));
    REQUIRE(third);
    auto stats = t.server->stats();
    for (int i = 0; i < 100 && stats.with_data > 2; ++i) {
        std::this_thread::sleep_for(50ms);
        stats = t.server->stats();
    }
    CHECK(stats.with_data == 2);
    CHECK(stats.streams == 3);
}

TEST_CASE("play: a failed login is tried once for several Plays, and a stopped Play ends at once (review G42)") {
    {
        Played t(10 * mib);
        t.fake.login_fails = budget::Failure::fatal;
        const auto first = t.start(t.file(2));
        REQUIRE_FALSE(first);
        CHECK(first.error().starts_with("can't log in to Main: Fake refused the login"));
        CHECK(first.error().ends_with("Test the server in Settings to try again"));
        const auto second = t.start(t.file(2));
        REQUIRE_FALSE(second);
        CHECK(t.fake.logins.load() == 1);
        CHECK(t.server->stats().seats == 0);
    }
    {
        Played t(10 * mib);
        const auto missing = t.start(t.file(2, "/v/none.mp4", "none.mp4"));
        REQUIRE_FALSE(missing);
        CHECK(missing.error() == "not found on Main");
    }
    {
        Played t(64 * mib);
        t.fake.slice = std::size_t{16} << 10;
        t.fake.slow_all = 5ms; // ~3 MiB/s: the warm part takes seconds
        std::stop_source stop;
        std::thread stopper([&] {
            std::this_thread::sleep_for(300ms);
            stop.request_stop();
        });
        const auto t0 = std::chrono::steady_clock::now();
        const auto stopped = t.start(t.file(2), stop.get_token());
        stopper.join();
        REQUIRE_FALSE(stopped);
        CHECK(stopped.error() == "stopped");
        CHECK(std::chrono::steady_clock::now() - t0 < 2s);
    }
}

TEST_CASE("play: an MKV's start decided by its first bytes; the bytes past it aren't fetched for the warm part") {
    // An MKV whose first cluster is at 1 MiB: the warm part is 9 MiB of its start and its end.
    std::string mkv = test::pattern(50 * mib);
    const std::string ebml("\x1A\x45\xDF\xA3\x84" "abcd", 9);
    const std::string segment("\x18\x53\x80\x67\x01\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 12);
    std::string head = ebml + segment;
    const std::uint64_t void_size = mib - head.size() - 9;
    head += std::string("\xEC\x01", 2);
    for (int shift = 48; shift >= 0; shift -= 8) head += static_cast<char>((void_size >> shift) & 0xFF);
    mkv.replace(0, head.size(), head);
    mkv.replace(mib, 4, std::string("\x1F\x43\xB6\x75", 4)); // the first cluster
    Played t(1);
    t.fake.files["/v/a.mkv"] = mkv;
    const auto address = t.start(t.file(2, "/v/a.mkv", "a.mkv"));
    REQUIRE(address);
    CHECK(t.progress.back() == std::pair<std::uint64_t, std::uint64_t>{13 * mib, 13 * mib});
    const auto u = url_of(*address);
    REQUIRE(u);
    const auto r = ask(*u, "Range: bytes=0-1048579\r\n");
    CHECK(r.body == mkv.substr(0, mib + 4));
}

TEST_CASE("pool: a caller's own cap on logins at once (Play's burst on a Storage Box), the pool's for the others") {
    auto most_at_once = [](int logins_at_once) {
        std::atomic<int> now{0};
        std::atomic<int> most{0};
        auto o = ssh::Pools::download_options(16);
        o.idle_timer = false;
        ssh::Pool pool(
            [&]() -> std::unique_ptr<ssh::Session> {
                const int n = ++now;
                for (int m = most.load(); n > m && !most.compare_exchange_weak(m, n);) {
                }
                std::this_thread::sleep_for(150ms);
                --now;
                return std::make_unique<Bare>();
            },
            o);
        std::vector<std::optional<ssh::Pool::Lease>> leases(8);
        {
            std::vector<std::jthread> threads;
            for (auto& lease : leases) {
                threads.emplace_back([&pool, &lease, logins_at_once] { lease = pool.acquire({}, std::nullopt, logins_at_once); });
            }
        }
        CHECK(std::ranges::all_of(leases, [](const auto& l) { return l.has_value(); }));
        leases.clear();
        return most.load();
    };
    CHECK(most_at_once(0) <= 4); // Options::logins_at_once
    CHECK(most_at_once(8) > 4);
}

TEST_CASE("play: short DATA replies are asked for again, exactly; a file shorter than STAT says ends where it does") {
    {
        Played t(20 * mib + 777);
        t.fake.max_data = 100 << 10;
        const auto address = t.start(t.file(3));
        REQUIRE_MESSAGE(address, address.error());
        const auto u = url_of(*address);
        REQUIRE(u);
        const auto r = ask(*u, "Range: bytes=9000000-15999999\r\n");
        CHECK(r.body == t.content.substr(9000000, 7000000));
        CHECK(ask(*u).body == t.content);
    }
    {
        play::Server::Options o;
        o.stalled_after = 10s;
        Played t(20 * mib, o);
        t.fake.claimed["/v/a.mp4"] = 25 * mib; // STAT says 5 MiB more than there is
        const auto address = t.start(t.file(2));
        REQUIRE_MESSAGE(address, address.error());
        const auto u = url_of(*address);
        REQUIRE(u);
        const auto t0 = std::chrono::steady_clock::now();
        auto r = ask(*u);
        CHECK(r.headers["content-length"] == std::to_string(25 * mib));
        CHECK(r.body == t.content); // the real bytes, then the request ends without waiting
        CHECK(std::chrono::steady_clock::now() - t0 < 5s);
    }
}

TEST_CASE("play: connections breaking without bytes end the reads after 3; the next request tries again") {
    Played t(40 * mib);
    const auto address = t.start(t.file(2));
    REQUIRE_MESSAGE(address, address.error());
    const auto u = url_of(*address);
    REQUIRE(u);
    t.fake.closed = 1000; // every connection, open or new, breaks at its next read
    const auto t0 = std::chrono::steady_clock::now();
    const auto failed = ask(*u, "Range: bytes=20000000-20999999\r\n");
    CHECK(failed.status == 206);
    CHECK(failed.body.empty());
    CHECK(std::chrono::steady_clock::now() - t0 < 10s); // ended, not stalled for stalled_after
    t.fake.closed = 0;
    const auto again = ask(*u, "Range: bytes=20000000-20999999\r\n");
    CHECK(again.body == t.content.substr(20000000, 1000000));
}

TEST_CASE("play: a stream still warming gives its connections and seats back once its reads failed") {
    play::Server::Options o;
    o.start_within = 300ms;
    o.release_after = 300ms;
    o.housekeeping = 50ms;
    Played t(64 * mib, o);
    t.fake.slice = std::size_t{16} << 10;
    t.fake.slow_all = 5ms; // the warm part takes seconds: the address comes while it is still warming
    const auto address = t.start(t.file(2));
    REQUIRE_MESSAGE(address, address.error());
    CHECK(t.progress.back().first < t.progress.back().second);
    t.fake.closed = 1000; // no player comes, and every read breaks from now on
    CHECK(within(5s, [&] {
        const auto s = t.server->stats();
        return s.seats == 0 && s.connected == 0;
    }));
}

TEST_CASE("play: read-ahead goes on past the cache's cap; the blocks behind the request go") {
    play::Server::Options o;
    o.stream_cap = 40 * mib; // more than the 12 MiB warm part and the 16 MiB read-ahead
    const std::uint64_t size = 160 * mib;
    Played t(size, o);
    t.fake.slice = std::size_t{64} << 10;
    t.fake.slow_all = 1ms;
    const auto address = t.start(t.file(4));
    REQUIRE_MESSAGE(address, address.error());
    const auto u = url_of(*address);
    REQUIRE(u);
    const auto before = t.server->stats().ahead;
    const auto r = ask(*u);
    CHECK(r.body == t.content);
    const auto stats = t.server->stats();
    CHECK(stats.cached <= o.stream_cap);
    // Most of the 148 MiB outside the warm part came as read-ahead, not 1 MiB at a time once full.
    CHECK(stats.ahead - before >= 100 * mib);
}

TEST_CASE("play: a Play over its share hands a seat to a waiting download between read-ahead pieces") {
    const std::uint64_t size = 200 * mib;
    Played t(size, {}, 4);
    t.fake.slice = std::size_t{16} << 10;
    const auto address = t.start(t.file(4));
    REQUIRE_MESSAGE(address, address.error());
    const auto u = url_of(*address);
    REQUIRE(u);
    t.fake.slow_from_now = 2; // a long film, read slowly from now on
    Response r;
    std::thread player([&] { r = ask(*u, std::format("Range: bytes=20000000-{}\r\n", 20000000 + 40 * mib - 1)); });
    std::this_thread::sleep_for(500ms);
    CHECK(t.server->stats().seats == 4);
    // A Normal download joins: Play's High share of 4 is less than 4, so a seat comes back to it.
    const int download = t.connections.new_id();
    t.connections.join("Main", download, 2);
    std::stop_source give_up;
    std::jthread limit([&](const std::stop_token& stop) {
        for (int i = 0; i < 200 && !stop.stop_requested(); ++i) std::this_thread::sleep_for(50ms);
        give_up.request_stop();
    });
    const auto t0 = std::chrono::steady_clock::now();
    const bool seated = t.connections.acquire("Main", download, give_up.get_token(), false);
    const auto took = std::chrono::steady_clock::now() - t0;
    CHECK(seated);
    CHECK(took < 5s);
    limit.request_stop();
    limit.join();
    if (seated) t.connections.release("Main", download);
    t.connections.leave("Main", download);
    t.fake.slow_from_now = 0;
    player.join();
    CHECK(r.body == t.content.substr(20000000, 40 * mib));
}

TEST_CASE("play: engine = rclone closes Play's connections as they go back, leaving none idle in the pool") {
    play::Server::Options o;
    o.release_after = 200ms;
    Played t(30 * mib, o);
    auto f = t.file(2);
    f.close_on_release = true;
    const auto address = t.start(std::move(f));
    REQUIRE_MESSAGE(address, address.error());
    const auto u = url_of(*address);
    REQUIRE(u);
    CHECK(ask(*u, "Range: bytes=15000000-15999999\r\n").body == t.content.substr(15000000, 1000000));
    CHECK(within(5s, [&] {
        const auto s = t.server->stats();
        return s.connected == 0 && s.seats == 0;
    }));
    CHECK(t.pool->idle() == 0);
}

TEST_CASE("play: a seek while every connection is busy reading for other requests is still served") {
    const std::uint64_t size = 128 * mib;
    Played t(size);
    const auto address = t.start(t.file(2));
    REQUIRE_MESSAGE(address, address.error());
    const auto u = url_of(*address);
    REQUIRE(u);
    t.fake.slice_from_now = std::size_t{16} << 10;
    t.fake.slow_from_now = 1;
    Response a;
    Response b;
    std::thread first([&] { a = ask(*u, "Range: bytes=20000000-45000000\r\n"); });
    std::thread second([&] { b = ask(*u, "Range: bytes=60000000-85000000\r\n"); });
    std::this_thread::sleep_for(500ms); // both connections busy
    const auto t0 = std::chrono::steady_clock::now();
    const auto seek = ask(*u, "Range: bytes=110000000-110099999\r\n");
    CHECK(seek.body == t.content.substr(110000000, 100000));
    CHECK(std::chrono::steady_clock::now() - t0 < 10s);
    t.fake.slow_from_now = 0;
    first.join();
    second.join();
    CHECK(a.body == t.content.substr(20000000, 25000001));
    CHECK(b.body == t.content.substr(60000000, 25000001));
}

TEST_CASE("play: the server stops at once with a player that stopped reading and one that sent no head") {
    Played t(100 * mib);
    const auto address = t.start(t.file(2));
    REQUIRE_MESSAGE(address, address.error());
    const auto u = url_of(*address);
    REQUIRE(u);
    Socket paused(u->port, 4096);
    REQUIRE(paused.ok());
    paused.send(std::format("GET {} HTTP/1.1\r\nHost: 127.0.0.1:{}\r\n\r\n", u->path, u->port)); // never read
    Socket silent(u->port);
    REQUIRE(silent.ok());
    std::this_thread::sleep_for(1s); // the server's sends to the paused one are blocked by now
    CHECK(t.server->stats().clients == 2);
    // A server that waits for them fails the test instead of hanging it.
    std::jthread watchdog([&](const std::stop_token& stop) {
        for (int i = 0; i < 100 && !stop.stop_requested(); ++i) std::this_thread::sleep_for(50ms);
        paused.close_now();
        silent.close_now();
    });
    const auto t0 = std::chrono::steady_clock::now();
    t.server.reset();
    CHECK(std::chrono::steady_clock::now() - t0 < 1500ms);
}
