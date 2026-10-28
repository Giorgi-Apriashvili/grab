#pragma once

// How many SSH/SFTP connections grab may open to one server, and how the downloads running on
// that server share them. A server cannot be asked for its limit, so grab starts from a known
// value (see default_budget), staggers connection starts so logins don't pile up (OpenSSH's
// MaxStartups), and lowers the budget when the server refuses one.

#include <chrono>
#include <optional>
#include <string_view>
#include <vector>

namespace grab::budget {

// Hetzner Storage Box hosts (<user>.your-storagebox.de).
[[nodiscard]] bool is_storage_box(std::string_view host);

// 14 for Storage Boxes, 12 elsewhere. Hetzner documents 10 connections per Storage Box account,
// but boxes accepted 16 on one account and 28 across a main account and its sub-account at
// once (2026-09 and 2026-10). Walks of the server (searches, lists) take theirs from the same
// budget (fetch::Listing). A refusal still lowers it.
[[nodiscard]] int default_budget(std::string_view host);

// [server] max_connections when set, else the default lowered to what grab learned.
[[nodiscard]] int effective_budget(std::optional<int> configured, int default_value, std::optional<int> learned);
// A learned limit after a download from its server finished without a refusal: one higher, so a
// drop that wasn't the server's real limit (a network blip, another program using the account for
// a while) wears off; nullopt once it's back at the default, which drops it (review G10).
[[nodiscard]] std::optional<int> raised(int learned, int default_value);

enum class Failure {
    refused, // the server turned the connection away: too many, reset during the handshake
    fatal,   // trying again can't help: the login was refused, the host key changed, no permission
    other    // anything else: network, missing file, ...
};

// Classifies a failed `rclone cat`/`rclone lsjson --stat` by its stderr.
[[nodiscard]] Failure classify_failure(std::string_view stderr_text);

// One server's connections, shared fairly by the downloads using it. Not thread-safe; the
// owner locks. Users are download ids, oldest first.
class ServerPool {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds stagger{150};

    explicit ServerPool(int budget) : budget_(budget < 1 ? 1 : budget) {}

    [[nodiscard]] int budget() const { return budget_; }
    void set_budget(int budget) { budget_ = budget < 1 ? 1 : budget; }

    // `weight`: the download's bandwidth priority (high 4, normal 2, low 1).
    void add_user(int id, int weight = 2);
    void set_weight(int id, int weight);
    void remove_user(int id);
    [[nodiscard]] int users() const { return static_cast<int>(users_.size()); }

    // A user's weighted share of the budget: budget × weight / total weight, rounded down,
    // the remainder going one each to the heaviest users (then the oldest), and at least 1.
    [[nodiscard]] int share(int id) const;
    [[nodiscard]] int active(int id) const;
    [[nodiscard]] int total_active() const;

    // Whether `id` may open another connection now: within the budget (a hard cap, also with more
    // users than connections), at least `stagger` after the previous start on this server, not
    // ahead of a waiting user that has none, and either below its share or borrowing capacity no
    // other download is waiting for (a small file that can only use one connection leaves the
    // rest of its share to the others).
    // `staggered`: this start may be a login (rclone's are); grab's engine logs in through its
    // pool, which paces logins itself, so its starts aren't kept `stagger` apart.
    [[nodiscard]] bool may_start(int id, Clock::time_point now, bool staggered = true) const;
    // Whether `id` (a user) may take a connection that is free now without anyone handing one
    // back or waiting longer for it: below the budget, and no other user waiting for one.
    [[nodiscard]] bool free_for(int id) const;
    // Downloads waiting for a connection, so borrowed ones are handed back.
    void waiting(int id, int delta);
    // Whether `id` should hand a connection back: it holds more than its share while another
    // download waits below its own.
    [[nodiscard]] bool should_yield(int id) const;
    // When the next start could happen, for waiting (now if nothing is in the way but the share).
    [[nodiscard]] Clock::time_point next_start(Clock::time_point now) const;
    void started(int id, Clock::time_point now);
    void finished(int id);
    // Connections of `id` that got data in their current run (`delta` +1 or -1).
    void working(int id, int delta);

    // The server refused a connection. While another of grab's connections to it gets data, that
    // means the budget is too high: it drops by one (not below 1). Returns whether it was lowered.
    bool refused();

private:
    struct User {
        int id;
        int active;
        int waiting = 0;
        int weight = 2;
        int working = 0;
    };
    [[nodiscard]] const User* find(int id) const;
    [[nodiscard]] bool other_starved(int id) const;
    int budget_;
    std::vector<User> users_;
    std::optional<Clock::time_point> last_start_;
};

} // namespace grab::budget
