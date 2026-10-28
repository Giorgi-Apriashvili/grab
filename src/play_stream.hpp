#pragma once

// Play (RCLONE-REPLACEMENT.md, phase 6): a video or audio file reaches the player through a local
// HTTP server (127.0.0.1, a random port, a 128-bit token in the path) whose every byte comes from
// grab's own pooled SFTP connections to the server, through a cache per played file (a stream).
//
// A stream's connections ("lanes", each with its own handle on the file) fetch, in this order: the
// bytes a player waits for and no connection is fetching, the warm part (what a player reads while
// it opens a file: its start up to the first video and the end, fetched before the player starts,
// as MPC-HC gives up after ~10 s), then read-ahead in front of each request. A Storage Box
// connection gives 0.7-2.5 MiB/s whatever its depth, Main's reaches its link at 8 READs of 255 KiB,
// and SFTP READs can't be called back: so each connection keeps at most 8 x 255 KiB in flight (a
// seek on it would wait behind them), its first READ after it sat idle is small and grows (TCP
// starts again from a small window: 255 KiB took 0.5 s on an idle connection, 32 KiB 0.2 s), and
// one connection stays idle for a seek while the others read ahead. Pieces of the warm part are
// dealt out among the connections and shrink near its end, each connection's bytes in flight
// capped at its share of what's left, so the slowest connection doesn't set the time (a box's ran
// at 0.7-1.7 MiB/s side by side).
//
// The scheduler (Plan), the request parsing and the answers are here without sockets, for tests;
// Server runs them. src/gui/app.cpp starts the player with the address play() gives.

#include "fetch.hpp"
#include "ssh_pool.hpp"

#include <winsock2.h> // IWYU pragma: keep (SOCKET)

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace grab::play {

inline constexpr std::uint64_t mib = std::uint64_t{1} << 20;

// Byte ranges [start, end), kept merged.
class Ranges {
public:
    void add(std::uint64_t start, std::uint64_t end);
    void remove(std::uint64_t start, std::uint64_t end);
    // Where the range holding `pos` ends; `pos` itself when none holds it.
    [[nodiscard]] std::uint64_t until(std::uint64_t pos) const;
    // The start of the first range after `pos` (none: `limit`).
    [[nodiscard]] std::uint64_t next_after(std::uint64_t pos, std::uint64_t limit) const;
    [[nodiscard]] std::uint64_t bytes_in(std::uint64_t start, std::uint64_t end) const;
    [[nodiscard]] bool covers(std::uint64_t start, std::uint64_t end) const { return bytes_in(start, end) == end - start; }
    [[nodiscard]] bool empty() const { return r_.empty(); }
    void clear() { r_.clear(); }

private:
    std::map<std::uint64_t, std::uint64_t> r_; // start -> end
};
// The first part of [start, end) that neither `a` nor `b` holds; nullopt when they hold all of it.
[[nodiscard]] std::optional<std::pair<std::uint64_t, std::uint64_t>> gap(const Ranges& a, const Ranges& b,
                                                                       std::uint64_t start, std::uint64_t end);

// ---- HTTP -------------------------------------------------------------------------------------

// The head of a request (up to the blank line), as the server reads it.
struct Request {
    std::string method;
    std::string target;              // "/TOKEN/ID/name"
    std::optional<std::string> host; // its Host header
    std::optional<std::string> range;
};
// nullopt: not a request line and headers this server takes.
[[nodiscard]] std::optional<Request> parse_request(std::string_view head);

// What a GET or HEAD of a file of `size` gets: 200 with the whole file (also for several ranges,
// or one this doesn't understand), 206 with one range (a suffix too), 416 for one past the end.
struct Answer {
    int status = 200;
    std::uint64_t first = 0;  // the bytes that go out: [first, first + length)
    std::uint64_t length = 0; // 0 for HEAD and 416
    std::string head;         // the status line and headers, the blank line included
};
[[nodiscard]] Answer answer(const Request& request, std::uint64_t size, std::string_view content_type);
// A short answer without a body ("404 Not Found").
[[nodiscard]] std::string bare(std::string_view status);
// What a player is told a file is, from its name (it looks at the bytes anyway).
[[nodiscard]] std::string media_type(std::string_view name);
// Whether `a` and `b` are equal, in a time that doesn't depend on where they differ.
[[nodiscard]] bool same_token(std::string_view a, std::string_view b);

// ---- the scheduler ----------------------------------------------------------------------------

// What a stream's connections fetch next. Not thread-safe: the stream's lock guards it.
class Plan {
public:
    struct Settings {
        std::uint64_t size = 0;
        std::uint64_t ahead = 16 * mib;      // read-ahead in front of each request
        std::uint64_t piece = 2 * mib;       // what a connection takes at a time
        std::uint64_t seek_piece = 1 * mib;  // where a request waits for bytes nobody fetches
        std::uint64_t min_piece = mib / 4;   // the warm part's pieces near its end, at least
    };
    explicit Plan(Settings settings) : s_(settings) {}

    enum class Kind { waiting, warm, ahead };
    struct Piece {
        std::uint64_t start = 0;
        std::uint64_t end = 0;
        Kind kind = Kind::ahead;
        bool operator==(const Piece&) const = default;
    };
    struct Lanes {
        bool busy = false;   // the connection asking has READs on their way
        int idle_others = 0; // other connections ready with nothing on their way
        int reading = 1;     // connections ready or reading, the one asking too
    };
    // The next piece for a connection, marked as taken; nullopt: nothing for it. A request waiting
    // for bytes nobody fetches comes first (a busy connection leaves it to an idle one, whose READs
    // aren't queued behind others); then the warm part, dealt out (its very start first, as it
    // decides how far the start goes; then the end; pieces shrinking near its end); then
    // read-ahead, the nearest missing bytes first, which an idle connection takes only while
    // another one stays idle for a seek. `room`: false when the cache is full (no read-ahead).
    [[nodiscard]] std::optional<Piece> next(const Lanes& lanes, bool room = true);
    // Whether next() would give such a connection a piece.
    [[nodiscard]] bool would(const Lanes& lanes, bool room = true) const { return pick(lanes, room).has_value(); }
    // Whether a connection would get work (a lane not connected yet: worth a seat and a login).
    [[nodiscard]] bool any_work(bool room = true) const;
    // Whether the bytes at `pos` are still worth asking for: in the warm part while it comes in,
    // or ahead of a request within its read-ahead.
    [[nodiscard]] bool wanted(std::uint64_t pos) const;

    // The warm part: [0, head) and [tail_at, size). warming() until it is in.
    void warm(std::uint64_t head, std::uint64_t tail_at);
    void warm_done() { warming_ = false; }
    [[nodiscard]] bool warming() const { return warming_; }
    [[nodiscard]] std::uint64_t head() const { return head_; }
    [[nodiscard]] std::uint64_t tail_at() const { return tail_at_; }
    // Bytes of the warm part in, and in all.
    [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> warm_progress() const;
    [[nodiscard]] bool warm_in() const;
    [[nodiscard]] bool pinned(std::uint64_t start, std::uint64_t end) const; // overlaps the warm part
    // Of the cache's blocks starting at `starts` (each `length` long), those that may go when it is
    // full, in the order they go: never the warm part, bytes on their way or a request's
    // read-ahead; first those behind every request (the farthest first), then those far ahead.
    [[nodiscard]] std::vector<std::uint64_t> evictable(const std::vector<std::uint64_t>& starts,
                                                       std::uint64_t length) const;

    // Requests reading the file, each at its next byte up to `last` (inclusive).
    int add_reader(std::uint64_t pos, std::uint64_t last);
    void move_reader(int id, std::uint64_t pos);
    void remove_reader(int id);
    [[nodiscard]] int readers() const { return static_cast<int>(readers_.size()); }

    // Where the file turned out to end (an EOF before STAT's size).
    void ended_at(std::uint64_t at);
    [[nodiscard]] std::uint64_t end() const { return eof_ < s_.size ? eof_ : s_.size; }
    [[nodiscard]] std::uint64_t size() const { return s_.size; }
    [[nodiscard]] const Settings& settings() const { return s_; }

    Ranges have;  // in the cache
    Ranges taken; // asked for or about to be, by a connection

private:
    struct Reader {
        int id = 0;
        std::uint64_t pos = 0;
        std::uint64_t last = 0;
    };
    [[nodiscard]] std::optional<Piece> pick(const Lanes& lanes, bool room) const;
    [[nodiscard]] std::uint64_t window_end(const Reader& r) const;

    Settings s_;
    std::vector<Reader> readers_;
    int next_reader_ = 1;
    bool warming_ = false;
    std::uint64_t head_ = 0;
    std::uint64_t tail_at_ = 0;
    std::uint64_t eof_ = ~std::uint64_t{0};
};

// ---- the server -------------------------------------------------------------------------------

// How the server runs; tests shorten the times.
struct Options {
    int depth = 8;                          // READs in flight per connection
    std::uint32_t first_read = 32 * 1024;   // a connection's first READ after it sat idle, doubling
    std::chrono::milliseconds idle_restart{250}; // idle that long: its next READ starts small again
    std::uint64_t drop_over = 2 * mib;      // a stop with more on its way drops the connection
    std::uint64_t stream_cap = 192 * mib;   // a stream's cache, at most
    std::size_t kept = 2;                   // streams that keep their data while not in use
    std::size_t ids = 64;                   // streams remembered (id, path, size), at most
    std::size_t max_clients = 16;           // requests at once; more are closed at once
    std::size_t max_head = std::size_t{16} << 10;       // a request's head, at most
    std::chrono::milliseconds head_within{10'000}; // all of it within this
    std::chrono::milliseconds release_after{10'000}; // connections back this long after the last request
    std::chrono::milliseconds free_after{300'000};   // data freed this long after the last request
    std::chrono::milliseconds start_within{60'000};  // the player starts by then while bytes come
    std::chrono::milliseconds stalled_after{30'000}; // no bytes this long: the start (or a request) fails
    std::chrono::milliseconds housekeeping{1'000};
};

class Server {
public:
    using Options = play::Options;
    explicit Server(fetch::Connections& connections, Options options = {});
    ~Server(); // the requests and streams stop, then the server
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    struct File {
        std::string server;               // grab.conf section: whose seats it takes
        std::shared_ptr<ssh::Pool> pool;  // and connections
        std::string path;                 // raw bytes (rclone_enc::raw_path)
        std::string name;                 // ends the address; the content type follows it
        int connections = 2;              // the burst, for the whole film (decision 2)
        int logins_at_once = 0;           // for its logins; 0: the pool's own
        std::uint64_t ahead = 16 * mib;   // read-ahead in front of each request
        bool close_on_release = false;    // engine = rclone: rclone can't see idle pool connections
    };
    // Bytes of the warm part in so far, and in all.
    using Progress = std::function<void(std::uint64_t done, std::uint64_t total)>;
    // STAT (only a regular file; its size, not the row's), then the warm part over the file's
    // connections, at High priority (downloads from that server step aside); the player's address
    // once it is in, or after start_within while bytes still come. Fails with why (the login, an
    // SFTP error, no bytes for stalled_after), or "stopped" once `stop` is requested.
    [[nodiscard]] std::expected<std::string, std::string> play(File file, const Progress& on_progress,
                                                              std::stop_token stop);

    // For tests: what the streams hold now.
    struct Stats {
        int streams = 0;          // remembered
        int with_data = 0;
        std::uint64_t cached = 0; // bytes in all caches
        std::uint64_t ahead = 0;  // bytes of read-ahead handed out, ever
        int connected = 0;        // connections held
        int seats = 0;            // seats held
        int clients = 0;          // requests being answered
    };
    [[nodiscard]] Stats stats();
    [[nodiscard]] std::string base(); // "http://127.0.0.1:PORT/TOKEN/" once listening

    struct Stream;

private:
    std::expected<void, std::string> listen(); // under mutex_
    void accept_loop();
    void serve(SOCKET client);
    void housekeep(const std::stop_token& stop);
    void forget_old(); // under mutex_
    void kick();       // the keeper looks now

    fetch::Connections& connections_;
    Options options_;
    std::mutex mutex_;
    SOCKET listener_ = INVALID_SOCKET;
    int port_ = 0;
    std::string base_;
    std::string token_;
    std::map<int, std::shared_ptr<Stream>> streams_;
    int next_id_ = 1;
    struct Client {
        SOCKET socket = INVALID_SOCKET;
        bool done = false;
        std::jthread thread;
    };
    std::list<Client> clients_; // under mutex_
    std::condition_variable_any changed_;
    bool kicked_ = false;
    std::atomic<bool> closing_{false};
    bool winsock_ = false;
    std::jthread acceptor_;
    std::jthread keeper_; // last: stopped first
};

} // namespace grab::play
