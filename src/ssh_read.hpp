#pragma once

// One file's bytes through a server's pooled connections (RCLONE-REPLACEMENT.md, phase 6), for the
// window's previews in place of rclone serve. STAT comes first, so only a regular file is opened
// (an OPEN of a named pipe blocks OpenSSH's sftp-server, which answers one request at a time, and
// the connection and its seat with it) and its size decides before anything is read; then the
// bytes asked for, over one connection or, for a big range, also over extra ones while seats are
// free.
//
// Each connection keeps a few READs in flight: 8 x 255 KiB. On a Storage Box one connection gave
// 1.3-2.5 MiB/s at any depth from 4 to 64 and Main reaches its link at 8, while a deeper queue
// only makes a stop wait longer behind it. A connection's first READs are small and grow: one that
// sat idle starts TCP again from a small window (255 KiB took 0.5 s on Main and on a box, 32 KiB
// 0.2 s). A range over several connections is cut into pieces, each connection taking the next one
// no connection took yet, the pieces shrinking near the end and each connection's bytes in flight
// capped at its share of what is left, so the slowest connection doesn't set the time (a box's
// connections ran at 0.7-1.7 MiB/s side by side).

#include "conn_budget.hpp"
#include "sftp.hpp"
#include "ssh_pool.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace grab::ssh {

// What STAT said of a regular file.
struct FileStat {
    std::optional<std::uint64_t> size; // none: the server didn't say
    std::optional<std::uint32_t> mtime; // seconds since 1970
};

enum class ReadFailure {
    stopped,  // the stop token
    login,    // no login to the server (Pool::acquire_remembered: maybe one that failed before)
    status,   // the server answered with an error: no such file, permission denied
    not_file, // a folder, a named pipe, a device: never opened
    lost,     // the connection broke, or the server sent less than the file holds
};
struct ReadError {
    ReadFailure kind = ReadFailure::lost;
    std::string message;                            // the login's error, sftp::describe's words, ...
    sftp::Code code = sftp::Code::failure;          // ReadFailure::status: the server's
    budget::Failure login = budget::Failure::other; // ReadFailure::login: what kind of failure
};

inline constexpr std::uint64_t read_piece = std::uint64_t{2} << 20;

struct ReadOptions {
    std::stop_token stop;
    Retry retry_refused = interactive_retry; // for the first connection's login: a person waits
    int depth = 8;                           // READs in flight per connection
    std::uint32_t first_read = 32 * 1024;    // a connection's first READ, doubling up to sftp::max_read
    std::uint64_t piece = read_piece;        // what a connection takes at a time; less near the end
    std::uint64_t min_piece = std::uint64_t{256} << 10;
    // A stop with more than this still on its way on a connection drops the connection (SFTP READs
    // can't be called back, and waiting for them on a box at ~1.2 MiB/s takes seconds); with less,
    // the replies are waited for and the connection goes back to the pool.
    std::uint64_t drop_over = std::uint64_t{2} << 20;
    // Extra connections for a range of at least extra_from bytes of a file whose size STAT gave
    // (without one, a range is only a cap): up to `extra`, each only on a seat
    // free now (`seat`, as fetch::Connections::try_acquire, never waiting or making a download step
    // aside) and given back with `seat_back`. `yield` (may be empty), asked as an extra connection
    // takes its next piece: true when its seat was handed back for a download waiting for its share
    // (fetch::Connections::yield_if_over_share); the connection stops and doesn't give it back again.
    int extra = 0;
    std::uint64_t extra_from = std::uint64_t{8} << 20;
    std::function<bool()> seat;
    std::function<void()> seat_back;
    std::function<bool()> yield;
    // Bytes in so far and the range's length, from the connections' threads, one call at a time.
    std::function<void(std::uint64_t done, std::uint64_t total)> progress;
};

// One file on a server, read through its pool. Not thread-safe: one caller.
class Reader {
public:
    // `path`: raw bytes (rclone_enc::raw_path), from the login folder or absolute.
    Reader(std::shared_ptr<Pool> pool, std::string path, ReadOptions options);
    // Stops the extra connections still logging in or reading and waits for them.
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    // STAT on a connection of the pool (Pool::acquire_remembered); one reused that turns out broken
    // (the server closed it while it sat idle) is dropped and the next one tried, once. Anything
    // but a regular file is ReadFailure::not_file. The connection stays for read().
    [[nodiscard]] std::expected<FileStat, ReadError> stat();
    // [offset, offset + length) of the file (stat() first, which read() calls when it wasn't),
    // opened on stat's connection, with extra connections as the options allow. A file that ends
    // first gives fewer bytes; that is no error. That connection goes back to the pool at the end,
    // so the next preview finds it there; extra connections still logging in aren't waited for
    // (the destructor waits for them; one that gets in after the read is done goes back unused).
    [[nodiscard]] std::expected<std::string, ReadError> read(std::uint64_t offset, std::uint64_t length);

private:
    struct Shared;
    class Lane;
    void extra_lane(const std::shared_ptr<Shared>& shared, std::stop_token halt);

    std::shared_ptr<Pool> pool_;
    std::string path_;
    ReadOptions options_;
    std::optional<Pool::Lease> lease_;
    bool stated_ = false;
    bool sized_ = false; // STAT gave the size
    std::shared_ptr<Shared> shared_; // the last read's, while extra connections may use it
    std::vector<std::jthread> extras_;
};

} // namespace grab::ssh
