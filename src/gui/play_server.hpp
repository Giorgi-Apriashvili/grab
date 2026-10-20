#pragma once

// Play: videos and audio reach the player through grab. A local HTTP server (127.0.0.1, a random
// port and path) answers what a player reads while it opens a file (the start up to the first
// video, and the end) from memory, fetched over several connections before the player starts,
// and everything else from the server's rclone serve (preview_host.hpp), as before. One stream
// from a Storage Box gives ~2.3 MiB/s with 2.5-5 s per seek, and an MKV can hold tens of MB of
// fonts before its first frame: players gave up (MPC-HC after ~10 s) before they had opened it.

#include "fetch.hpp"

#include <winsock2.h> // IWYU pragma: keep (SOCKET)

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

namespace grab::gui {

class PlayServer {
public:
    explicit PlayServer(fetch::Connections& connections);
    ~PlayServer(); // stops the warm starts and the requests, then the server
    PlayServer(const PlayServer&) = delete;
    PlayServer& operator=(const PlayServer&) = delete;

    struct File {
        fetch::Source source;  // rclone cat, for the warm start
        std::string server;    // grab.conf section: whose connections it takes
        // The file on rclone serve, for everything else: rclone serve starts meanwhile.
        std::shared_future<std::expected<std::string, std::string>> upstream;
        std::string name;      // its name, which ends the player's address
        std::optional<std::uint64_t> size; // when known (else rclone serve is asked)
    };
    // Bytes of what the player needs fetched so far, and how many in all (from the fetching
    // threads, until it is all in).
    using Progress = std::function<void(std::uint64_t done, std::uint64_t total)>;
    // Warms `file` and returns the player's address once what it reads while opening is in (a
    // read that fails is answered by rclone serve instead); at most 60 s.
    [[nodiscard]] std::expected<std::string, std::string> play(File file, Progress on_progress);

    // A part of the file fetched in one long read (one connection), served as it arrives.
    struct Segment {
        std::uint64_t start = 0;
        std::uint64_t length = 0;
        enum class State { waiting, fetching, done, failed } state = State::waiting;
    };
    struct Stream;

private:
    std::expected<void, std::string> listen(); // under mutex_
    void accept_loop();
    void serve(SOCKET client);
    void warm(Stream& stream); // a fetching thread: long reads until none is left
    void forget_old(); // under mutex_

    fetch::Connections& connections_;
    std::mutex mutex_;
    SOCKET listener_ = INVALID_SOCKET;
    std::string base_; // "http://127.0.0.1:PORT/TOKEN/"
    std::string token_;
    std::map<int, std::shared_ptr<Stream>> streams_;
    int next_id_ = 1;
    std::set<SOCKET> clients_;
    std::condition_variable clients_done_;
    std::jthread acceptor_;
    std::atomic<bool> closing_{false};
};

} // namespace grab::gui
