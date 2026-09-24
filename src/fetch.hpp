#pragma once

// grab's own resumable file download. rclone cannot resume a stopped transfer, so a file is
// fetched in byte ranges: several `rclone cat --offset N --count M` streams write into
// "<name>.grabpart" at the right offsets, and "<name>.grabpart.json" records how much of each
// chunk is on disk. Stopping keeps both; the next run continues from there, even after grab
// restarts. The streams of all downloads share each server's connection budget (conn_budget).

#include "conn_budget.hpp"

#include <condition_variable>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grab::fetch {

inline constexpr std::uint64_t min_chunk = std::uint64_t{16} << 20;
inline constexpr std::uint64_t max_chunk = std::uint64_t{128} << 20;
// A busy range is split for an idle stream only when both halves get at least this much.
inline constexpr std::uint64_t min_split = std::uint64_t{4} << 20;
inline constexpr int max_retries = 6; // per range, before the download fails

// Chunk size for a file of `size` bytes fetched over up to `streams` connections: enough chunks
// to keep every stream busy, within [min_chunk, max_chunk].
[[nodiscard]] std::uint64_t chunk_size_for(std::uint64_t size, int streams);

// A byte range of the file and how much of it, from its start, is on disk.
struct Range {
    std::uint64_t start = 0;
    std::uint64_t length = 0;
    std::uint64_t done = 0;
    [[nodiscard]] std::uint64_t remaining() const { return length - done; }
    bool operator==(const Range&) const = default;
};

// Progress of one partial file, saved as "<name>.grabpart.json". The ranges cover the file
// exactly; they start as equal chunks, and near the end a stream that ran out of work splits
// the busiest range so the last bytes don't trickle in over a single connection.
struct State {
    std::string rclone_remote;
    std::string path;    // remote path
    std::uint64_t size = 0;
    std::string modtime; // as rclone reports it; a changed file starts over
    std::vector<Range> ranges;

    [[nodiscard]] std::uint64_t bytes_done() const;
    [[nodiscard]] bool complete() const;
    bool operator==(const State&) const = default;
};

[[nodiscard]] State new_state(std::string rclone_remote, std::string path, std::uint64_t size, std::string modtime,
                              std::uint64_t chunk);
// Splits the unfinished tail of range `i`: its second half (rounded to 64 KiB) becomes a new range
// appended at the end, whose index is returned. nullopt when either half would be below
// min_split.
std::optional<std::size_t> split(State& s, std::size_t i);
[[nodiscard]] std::string state_to_json(const State& s);
// nullopt for malformed or inconsistent state (ranges not covering the file exactly, progress
// beyond a range).
[[nodiscard]] std::optional<State> state_from_json(std::string_view text);

struct RemoteStat {
    std::uint64_t size = 0;
    std::string modtime;
    bool is_dir = false;
};
// `rclone lsjson --stat` output.
[[nodiscard]] std::expected<RemoteStat, std::string> parse_stat(std::string_view json_text);

// RFC 3339 time ("2023-10-15T11:22:33.123456789+02:00", "...Z") as nanoseconds since 1970 UTC.
[[nodiscard]] std::optional<std::int64_t> parse_rfc3339(std::string_view text);

// Where a file comes from and how rclone reaches it.
struct Source {
    std::string rclone; // executable
    std::optional<std::filesystem::path> config;
    std::string rclone_remote;
    std::string path;
    std::vector<std::string> flags; // the remote's common flags, without console progress
};
[[nodiscard]] std::vector<std::string> stat_argv(const Source& src);
[[nodiscard]] std::vector<std::string> cat_argv(const Source& src, std::uint64_t offset, std::uint64_t count);

[[nodiscard]] std::filesystem::path part_path(const std::filesystem::path& target);  // <target>.grabpart
[[nodiscard]] std::filesystem::path state_path(const std::filesystem::path& target); // <target>.grabpart.json

// Bytes done and total size of a partial download of `target`, if there is one.
[[nodiscard]] std::optional<std::pair<std::uint64_t, std::uint64_t>> saved_progress(const std::filesystem::path& target);
// Deletes the partial file and its state.
void discard(const std::filesystem::path& target);

// The connection pools of all servers, shared by every running download. Thread-safe.
class Connections {
public:
    // Called (from a download thread) when a server's budget was lowered after a refusal.
    using OnLearned = std::function<void(const std::string& server, int budget)>;
    explicit Connections(OnLearned on_learned = {}) : on_learned_(std::move(on_learned)) {}

    // Sets a server's budget (creating its pool); running streams above it finish normally.
    void configure(const std::string& server, int budget);
    void join(const std::string& server, int id);
    void leave(const std::string& server, int id);
    // Waits until `id` may open one more connection; false when `stop` was requested first.
    bool acquire(const std::string& server, int id, std::stop_token stop);
    // Takes up to `id`'s whole share at once (for an rclone copy that manages its own
    // connections); at least 1 unless stopped (0).
    int reserve(const std::string& server, int id, std::stop_token stop);
    void release(const std::string& server, int id, int count = 1);
    // When `id` holds more connections than its share while another download waits for its own
    // (e.g. one just joined), gives one up and returns true; the caller then stops that stream
    // and must not release it again.
    bool yield_if_over_share(const std::string& server, int id);
    // A connection of this server was refused; call while it still counts as open.
    void refused(const std::string& server);
    [[nodiscard]] int active(const std::string& server, int id);
    [[nodiscard]] int budget(const std::string& server);

private:
    budget::ServerPool& pool(const std::string& server); // caller holds mutex_
    std::mutex mutex_;
    std::condition_variable_any changed_;
    std::map<std::string, budget::ServerPool> pools_;
    OnLearned on_learned_;
};

struct Progress {
    std::uint64_t bytes = 0; // on disk, including what earlier runs fetched
    std::uint64_t total = 0;
    double speed = 0;        // bytes per second, recent average
    std::optional<double> eta;
    int connections = 0;     // streams open right now
};

enum class Outcome { done, stopped, failed };
struct Result {
    Outcome outcome = Outcome::failed;
    std::string error;
    std::string note; // e.g. "the file changed on the server; started over"
};

struct Download {
    Source source;
    std::string server; // grab.conf section: the connection pool
    int id = 0;         // the download's id in the pools
    std::filesystem::path target; // DEST\<name>
};

// Fetches `d.source` into `d.target`, continuing a partial download. Stopping keeps the partial
// file and its state (Outcome::stopped); a failure keeps them too, so a retry continues.
[[nodiscard]] Result run(const Download& d, Connections& connections,
                         const std::function<void(const Progress&)>& on_progress, std::stop_token stop);

} // namespace grab::fetch
