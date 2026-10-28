#pragma once

// The window's previews of images, text and PDFs without Windows (RCLONE-REPLACEMENT.md, phase 6),
// so they can be tested: one preview read through grab's own connections (fetch), the answers kept
// in memory (Cache) and the previews being fetched, which every request for the same file joins and
// the viewer stops when it moves on (Previews). src/gui/app.cpp runs them on its preview workers
// with the viewer's seats.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace grab::ssh {
class Reader;
} // namespace grab::ssh

namespace grab::preview {

// An answer to the page: the status, the content type, extra response headers ("Name: value"
// lines) and the body, held once: the cache and the response's stream (body_stream.hpp) share it.
struct Answer {
    int status = 200;
    std::string content_type;
    std::string headers;
    std::shared_ptr<const std::string> body; // none: empty
};
// A plain-text answer saying why there's no preview.
[[nodiscard]] Answer failed(int status, std::string message);
// The answer to a request whose preview was stopped (the viewer moved on, the window closes).
[[nodiscard]] Answer cancelled();

// A file's size and modification time (Unix seconds): the row's, as the page sent them, or the
// server's, as STAT gave them.
struct Stamp {
    std::optional<std::uint64_t> size;
    std::optional<std::int64_t> modified;
    bool operator==(const Stamp&) const = default;
};

// What a fetch gave: the answer, and the file's stamp as STAT gave it. `keep`: an answer worth
// keeping; failures never are (review G36).
struct Fetched {
    Answer answer;
    Stamp stamp;
    bool keep = false;
};

// A preview of `path` (its kind by its name, preview::kind_of) through `reader`, which reads that
// path: STAT, whose size decides 413 before anything is read for an image or PDF over its cap; a
// text file's first MiB (X-Grab-Total: its size), converted to UTF-8; an image or PDF whole.
// Without a size from the server, up to the cap and one byte more, which says there's more.
// Failures in the window's words (`remote`, `name` for them): "Not found on X." (404), a login
// failure (with the Trust hint for a host key), not a regular file (415), a lost connection;
// stopped gives cancelled(). An SVG gets "Content-Security-Policy: sandbox".
[[nodiscard]] Fetched fetch(ssh::Reader& reader, std::string_view remote, std::string_view path);

// Previews kept in memory for ←/→ and the neighbours fetched ahead, the most recent first, up to
// `max_bytes` of bodies. A kept answer younger than `fresh` is given as it is; an older one only
// while the row's stamp equals what STAT gave when it was fetched, else it goes (review G36): no
// STAT for that, since once the pool closed its idle connections one costs a login (1-3.5 s on a
// Storage Box) where a kept answer is instant. Not thread-safe.
class Cache {
public:
    using Clock = std::chrono::steady_clock;
    explicit Cache(std::size_t max_bytes = std::size_t{256} << 20, Clock::duration fresh = std::chrono::seconds(60))
        : max_bytes_(max_bytes), fresh_(fresh) {}

    [[nodiscard]] std::optional<Answer> find(const std::string& key, const Stamp& row, Clock::time_point now = Clock::now());
    void put(const std::string& key, Answer answer, Stamp stamp, Clock::time_point now = Clock::now());
    // Drops a server's answers (keys start with "<server>\n"): it was saved or removed.
    void forget_server(std::string_view server);
    [[nodiscard]] std::size_t bytes() const;
    [[nodiscard]] std::size_t size() const { return kept_.size(); }

private:
    struct Kept {
        std::string key;
        Answer answer;
        Stamp stamp;
        Clock::time_point fetched;
    };
    std::size_t max_bytes_;
    Clock::duration fresh_;
    std::list<Kept> kept_;
};

// A file's key: its server, a line break, its path.
[[nodiscard]] std::string key_of(std::string_view server, std::string_view path);

// The previews being fetched (queued or running) and the cache, shared by the page's requests (UI
// thread) and the preview workers. Thread-safe; replies are never called under its lock.
class Previews {
public:
    using Reply = std::function<void(Answer)>;
    struct Job {
        std::string key;
        std::string server;
        std::string path;
        Stamp row;
        std::stop_source stop;
        // Read under the lock (Previews::ahead): a request that isn't ahead joining makes it the one
        // shown.
        bool ahead = false;
        bool running = false;
        std::vector<Reply> replies;
    };
    struct Request {
        std::string server;
        std::string path;
        Stamp row;
        bool ahead = false; // a neighbour fetched before it is shown
    };

    // A request: answered at once from the cache (the reply called before this returns), else it
    // joins the job fetching the same file (review G50), else a new job, returned to be queued.
    [[nodiscard]] std::shared_ptr<Job> ask(Request request, Reply reply);
    // A worker takes `job`: false when it needn't run, its replies answered here: cancelled while
    // queued (answered then), or kept meanwhile by a fetch of the same file that ended while this
    // one waited (review G50).
    [[nodiscard]] bool begin(const std::shared_ptr<Job>& job);
    // The fetch ended: kept when worth it, and every request that joined it answered.
    void end(const std::shared_ptr<Job>& job, const Fetched& fetched);
    // The viewer moved or closed: every job for a file not in `keep` (keys) stops, and no new
    // request joins it; a queued one answers cancelled() now, a running one once its reads stop.
    void cancel(const std::set<std::string>& keep);
    // All stop (the window closes), none answered: nothing will read the answers.
    void stop_all();
    [[nodiscard]] bool ahead(const Job& job) const;
    void forget_server(std::string_view server);
    [[nodiscard]] std::size_t jobs() const;
    [[nodiscard]] std::size_t cached_bytes() const;

private:
    mutable std::mutex mutex_;
    Cache cache_;
    std::map<std::string, std::shared_ptr<Job>> jobs_; // by key
};

// The page's https://grab.ui/preview/<server>/<path>[?size=N&modified=T][&ahead] (server and path
// percent-encoded; the row's size and time; ahead: a neighbour fetched before it is shown);
// nullopt for anything else.
[[nodiscard]] std::optional<Previews::Request> parse_url(std::string_view url);

} // namespace grab::preview
