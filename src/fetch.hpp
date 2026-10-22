#pragma once

// grab's own resumable file download. rclone cannot resume a stopped transfer, so a file is
// fetched in byte ranges: several `rclone cat --offset N --count M` streams write into
// "<name>.grabpart" at the right offsets, and "<name>.grabpart.json" records how much of each
// chunk is on disk. Stopping keeps both; the next run continues from there, even after grab
// restarts. The streams of all downloads share each server's connection budget (conn_budget).
//
// Those files are grab's only when the record parses and names the same server file: grab never
// replaces, reuses or deletes anything it can't prove it made. When the names are taken by
// something else (the user's files, or files of a folder being downloaded), a download uses
// "<name>.2.grabpart" and so on.

#include "conn_budget.hpp"
#include "rate.hpp"

#include <atomic>
#include <chrono>
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
// A server that refuses every connection this long, with no data meanwhile, fails the download:
// it is full with something else's connections (review G9).
inline constexpr std::chrono::minutes refused_give_up{5};

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

// Whether a local copy already holds the server's file: the same size, and modified no earlier
// than the server's copy (2 s slack for coarse file systems). grab and rclone give a finished
// file the server's time; a later one is a copy made after the server's last change, e.g. by an
// older grab. Times in nanoseconds since 1970 UTC; without the server's time, size decides.
[[nodiscard]] bool up_to_date(std::uint64_t local_size, std::int64_t local_mtime, std::uint64_t size,
                              std::optional<std::int64_t> modtime);
// The same for a file on disk (false when there is none); `modtime` as rclone reports it.
[[nodiscard]] bool up_to_date(const std::filesystem::path& local, std::uint64_t size, std::string_view modtime);

// Where a file comes from and how rclone reaches it.
struct Source {
    std::string rclone; // executable
    std::optional<std::filesystem::path> config;
    std::string rclone_remote;
    std::string path;
    std::vector<std::string> flags; // the remote's common flags, without console progress
};
[[nodiscard]] std::vector<std::string> stat_argv(const Source& src);
// `low_read_ahead`: under a speed limit, keep rclone from downloading far ahead of what grab
// takes (--buffer-size 0, few SFTP requests in flight), so the network follows the limit.
[[nodiscard]] std::vector<std::string> cat_argv(const Source& src, std::uint64_t offset, std::uint64_t count,
                                                bool low_read_ahead = false);

// One partial download's files: the data, and the record of its State beside it.
struct PartialFiles {
    std::filesystem::path part;   // <target>.grabpart, or <target>.<slot>.grabpart
    std::filesystem::path record; // the same name plus ".json"
    bool operator==(const PartialFiles&) const = default;
};
inline constexpr int max_slot = 9;
// Slot 1 is "<target>.grabpart"; slots 2 to max_slot are "<target>.<slot>.grabpart".
[[nodiscard]] PartialFiles partial_files(const std::filesystem::path& target, int slot);

// Names a partial download must not take, in a folder download where the folder's own files
// land; empty: none.
using Reserved = std::function<bool(const std::filesystem::path&)>;

struct Partial {
    PartialFiles files;
    State state;
};
// grab's partial download of `target` from the server file `remote_path`: the first slot, not
// reserved, whose record parses and names that file, from the rclone remote `rclone_remote` when
// one is given. The same path on another server is another file: its partial is left alone
// (review G20).
[[nodiscard]] std::optional<Partial> find_partial(const std::filesystem::path& target, std::string_view remote_path,
                                                  const Reserved& reserved = {}, std::string_view rclone_remote = {});
// The first slot for a new partial download of `target`: not reserved, and nothing at either name.
[[nodiscard]] std::optional<PartialFiles> free_partial(const std::filesystem::path& target, const Reserved& reserved = {});
// Its bytes done and total size.
[[nodiscard]] std::optional<std::pair<std::uint64_t, std::uint64_t>>
saved_progress(const std::filesystem::path& target, std::string_view remote_path, const Reserved& reserved = {},
               std::string_view rclone_remote = {});
// Deletes it: the record, and the data file while it has the size grab gave it. Nothing else.
void discard(const std::filesystem::path& target, std::string_view remote_path, const Reserved& reserved = {},
             std::string_view rclone_remote = {});
// Moves it to `to`'s first partial names, so a download to `to` goes on from it (Keep both after
// the file there turned out different; review G74). False when there is none, or `to`'s names
// are taken.
bool move_partial(const std::filesystem::path& from, const std::filesystem::path& to, std::string_view remote_path,
                  std::string_view rclone_remote);

// The connection pools of all servers, shared by every running download. Thread-safe.
class Connections {
public:
    // Called (from a download thread) when a server's budget was lowered after a refusal.
    using OnLearned = std::function<void(const std::string& server, int budget)>;
    explicit Connections(OnLearned on_learned = {}) : on_learned_(std::move(on_learned)) {}

    // Sets a server's budget (creating its pool); running streams above it finish normally.
    void configure(const std::string& server, int budget);
    // `weight`: bandwidth priority (high 4, normal 2, low 1); shares follow it.
    void join(const std::string& server, int id, int weight = 2);
    void set_weight(const std::string& server, int id, int weight);
    void leave(const std::string& server, int id);
    // Waits until `id` may open one more connection; false when `stop` was requested first.
    bool acquire(const std::string& server, int id, std::stop_token stop);
    // Takes up to `id`'s whole share at once (for an rclone copy or listing that manages its own
    // connections), at most `at_most`; at least 1 unless stopped (0). With `settle`, it waits that
    // long for downloads above their shares to hand connections back first.
    int reserve(const std::string& server, int id, std::stop_token stop, int at_most = 1 << 20,
                std::chrono::milliseconds settle = std::chrono::milliseconds(0));
    // `worked`: the connection got data (see working), and stops counting as working too.
    void release(const std::string& server, int id, int count = 1, bool worked = false);
    // `count` connections of `id` got their first data: the server takes grab's connections.
    void working(const std::string& server, int id, int count = 1);
    // For an rclone copy holding `held` connections (reserve): whether it should start again
    // with another number, because another download waits for its share while this holds more
    // than its own, or because at least two more are free within its share (review G192), up to
    // `at_most`.
    [[nodiscard]] bool unbalanced(const std::string& server, int id, int held, int at_most = 1 << 20);
    // When `id` holds more connections than its share while another download waits for its own
    // (e.g. one just joined), gives one up and returns true; the caller then stops that stream
    // and must not release it again.
    bool yield_if_over_share(const std::string& server, int id);
    // A connection of this server was refused; call while it still counts as open.
    void refused(const std::string& server);
    // How many refusals this server has had so far: a download that ends with the same number it
    // started with had none.
    [[nodiscard]] std::uint64_t refusals(const std::string& server);
    [[nodiscard]] int active(const std::string& server, int id);
    [[nodiscard]] int budget(const std::string& server);
    // `id`'s fair part of the server's budget now.
    [[nodiscard]] int share(const std::string& server, int id);
    // A pool id of its own for a short user (a listing): negative, so never a download's.
    [[nodiscard]] int new_id();

private:
    std::atomic<int> next_id_{0};
    budget::ServerPool& pool(const std::string& server); // caller holds mutex_
    std::mutex mutex_;
    std::condition_variable_any changed_;
    std::map<std::string, budget::ServerPool> pools_;
    std::map<std::string, std::uint64_t> refusals_;
    OnLearned on_learned_;
};

// Connections from a server's pool for one rclone walk of a tree (lsf -R, lsjson -R), which
// opens as many as its --checkers (rclone's default 8; each a new SSH login): up to `at_most`,
// its share at High priority while downloads run, given back when destroyed (review G191). Pass
// connections() to rclone as --checkers N --sftp-connections N.
class Listing {
public:
    Listing(Connections& connections, std::string server, std::stop_token stop, int at_most = 8);
    ~Listing();
    Listing(const Listing&) = delete;
    Listing& operator=(const Listing&) = delete;
    [[nodiscard]] int connections() const { return held_; } // 0: stopped while waiting
    // The rclone flags for them.
    [[nodiscard]] std::vector<std::string> flags() const;

private:
    Connections& connections_;
    std::string server_;
    int id_;
    int held_ = 0;
};

struct Progress {
    std::uint64_t bytes = 0; // on disk, including what earlier runs fetched
    std::uint64_t total = 0;
    double speed = 0;        // bytes per second, recent average
    std::optional<double> eta;
    int connections = 0;     // streams open right now
};

// exists: a different file is at the target (another size, or older than the server's) and the
// download may not replace it; nothing was replaced, and a partial download is kept.
enum class Outcome { done, stopped, failed, exists };
struct Result {
    Outcome outcome = Outcome::failed;
    std::string error;
    std::string note; // e.g. "the file changed on the server; started over"
    bool present = false; // done because the target already held the file (up_to_date); nothing fetched
    std::uint64_t size = 0; // the server's file, once looked at
    std::string modtime = {};
};

struct Download {
    Source source;
    std::string server; // grab.conf section: the connection pool
    int id = 0;         // the download's id in the pools
    std::filesystem::path target; // DEST\<name>
    Reserved reserved;            // names the partial files must not take (a folder's own files)
    // A different file at the target may be replaced (the user chose Replace); otherwise run()
    // ends with Outcome::exists, checked before fetching and again before the final rename.
    bool replace = false;
    // The caller already joined the pool under `id` and leaves it itself: a folder whose files
    // are fetched one after another (or several at once) as one pool user.
    bool joined = false;
    // A speed limit shared with other downloads; nullptr or rate 0 = unlimited.
    rate::RateLimiter* limiter = nullptr;
    // Bandwidth priority (high 4, normal 2, low 1), read at each stream start and limiter
    // request, so a change applies while running. nullptr = normal.
    const std::atomic<int>* weight = nullptr;
    // At most this many streams at once, asked as each starts and at each read: a folder's file
    // gets an even part of the folder's connections beside its other files downloading. A stream
    // over it gives its connection up and goes on later from its bytes. Unset: no cap.
    std::function<int()> streams_cap;
    // Sees every rclone command line before it runs (`grab -v`); called from several threads.
    std::function<void(const std::vector<std::string>&)> on_command;
};

// Fetches `d.source` into `d.target`, continuing a partial download. A target that is already
// up_to_date with no partial download in progress is kept (Result::present); a different one is
// replaced only with `d.replace` (else Outcome::exists). Stopping keeps the partial file and its
// state (Outcome::stopped); a failure keeps them too, so a retry continues.
[[nodiscard]] Result run(const Download& d, Connections& connections,
                         const std::function<void(const Progress&)>& on_progress, std::stop_token stop);

} // namespace grab::fetch
