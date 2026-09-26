#pragma once

// How many SSH/SFTP connections grab may open to one server, and how the downloads running on
// that server share them. A server cannot be asked for its limit, so grab starts from a known
// value (Hetzner Storage Boxes allow 10 per account), staggers connection starts so logins don't
// pile up (OpenSSH's MaxStartups), and lowers the budget when the server refuses one.

#include <chrono>
#include <optional>
#include <string_view>
#include <vector>

namespace grab::budget {

// Hetzner Storage Box hosts (<user>.your-storagebox.de).
[[nodiscard]] bool is_storage_box(std::string_view host);

// 8 for Storage Boxes (their limit is 10; two stay free for searches), 12 elsewhere.
[[nodiscard]] int default_budget(std::string_view host);

// [server] max_connections when set, else the default lowered to what grab learned.
[[nodiscard]] int effective_budget(std::optional<int> configured, int default_value, std::optional<int> learned);

enum class Failure {
    refused, // the server turned the connection away: too many, reset during the handshake
    other    // anything else: network, missing file, ...
};

// Classifies a failed `rclone cat`/`rclone copy` by its stderr.
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

    // Whether `id` may open another connection now: within the budget, at least `stagger` after
    // the previous start on this server, and either below its share or borrowing capacity no
    // other download is waiting for (a small file that can only use one connection leaves the
    // rest of its share to the others).
    [[nodiscard]] bool may_start(int id, Clock::time_point now) const;
    // Downloads waiting for a connection, so borrowed ones are handed back.
    void waiting(int id, int delta);
    // Whether `id` should hand a connection back: it holds more than its share while another
    // download waits below its own.
    [[nodiscard]] bool should_yield(int id) const;
    // When the next start could happen, for waiting (now if nothing is in the way but the share).
    [[nodiscard]] Clock::time_point next_start(Clock::time_point now) const;
    void started(int id, Clock::time_point now);
    void finished(int id);

    // The server refused a connection. With two or more open that means the budget is too high:
    // it drops by one (not below 1). Returns whether it was lowered.
    bool refused();

private:
    struct User {
        int id;
        int active;
        int waiting = 0;
        int weight = 2;
    };
    [[nodiscard]] const User* find(int id) const;
    [[nodiscard]] bool other_starved(int id) const;
    int budget_;
    std::vector<User> users_;
    std::optional<Clock::time_point> last_start_;
};

} // namespace grab::budget
