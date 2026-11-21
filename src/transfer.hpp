#pragma once

// What a download (fetch.cpp) and an upload (upload.cpp) share inside grab: the pauses between a
// range's tries, and a transfer's place in its server's pool.

#include "fetch.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <string>

namespace grab::transfer {

// The pause before a range's next try after `attempt` failures in a row (fetch::max_retries).
[[nodiscard]] inline std::chrono::seconds backoff(int attempt) {
    constexpr std::array<int, 6> steps{2, 4, 8, 16, 30, 30};
    return std::chrono::seconds(steps[static_cast<std::size_t>(std::clamp(attempt - 1, 0, 5))]);
}

// A transfer's place in its server's pool, from its first look at the server on, left on every
// way out. `joined`: the caller joined under `id` and leaves itself (a folder's files).
class Seat {
public:
    Seat(fetch::Connections& connections, const std::string& server, int id, bool joined, int weight)
        : connections_(connections), server_(server), id_(id), joined_(joined) {
        if (!joined_) connections_.join(server_, id_, weight);
    }
    Seat(const Seat&) = delete;
    Seat& operator=(const Seat&) = delete;
    ~Seat() { leave(); }
    void leave() {
        if (!joined_ && !left_) connections_.leave(server_, id_);
        left_ = true;
    }

private:
    fetch::Connections& connections_;
    const std::string& server_;
    int id_;
    bool joined_;
    bool left_ = false;
};

} // namespace grab::transfer
