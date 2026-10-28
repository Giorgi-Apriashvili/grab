#include "ssh_read.hpp"

#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_pool.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <span>
#include <string_view>
#include <utility>

namespace grab::ssh {

namespace {

constexpr std::uint64_t no_end = std::numeric_limits<std::uint64_t>::max();

struct Span {
    std::uint64_t at = 0;
    std::uint64_t n = 0;
};

ReadError stopped() { return ReadError{.kind = ReadFailure::stopped, .message = "stopped"}; }
ReadError lost(std::string message) { return ReadError{.kind = ReadFailure::lost, .message = std::move(message)}; }
ReadError status_error(const sftp::Status& status) {
    return ReadError{.kind = ReadFailure::status, .message = sftp::describe(status), .code = status.code};
}

} // namespace

// What the connections of one read share.
struct Reader::Shared {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    std::uint64_t next = 0;     // the first byte no connection took yet
    std::deque<Span> returned;  // pieces of a connection that broke, for the others
    std::uint64_t eof = no_end; // where the file turned out to end, when before `end`
    int lanes = 1;              // connections reading, the first one too
    int busy = 0;               // extra connections reading, which may still give pieces back
    std::optional<ReadError> failed;
    std::stop_source halt;      // the caller's stop, a failure, or the read done
    std::string buffer;         // the range's bytes, each connection writing its own pieces
    std::atomic<std::uint64_t> received{0};
    std::mutex progress_mutex;
    bool quiet = false; // under progress_mutex: read() returned, so no more progress
    const ReadOptions* options = nullptr;

    [[nodiscard]] std::uint64_t last() const { return std::min(end, eof); } // under the mutex
    void fail(ReadError error) {
        {
            std::scoped_lock lock(mutex);
            if (!failed) failed = std::move(error);
        }
        halt.request_stop();
        changed.notify_all();
    }
    void ended_at(std::uint64_t at) {
        {
            std::scoped_lock lock(mutex);
            eof = std::min(eof, at);
        }
        changed.notify_all();
    }
    void got(std::uint64_t n) {
        const std::uint64_t now = received.fetch_add(n) + n;
        if (!options->progress) return;
        std::scoped_lock lock(progress_mutex);
        if (!quiet) options->progress(now, end - start);
    }
    // No progress from here on: the caller's callback may go once read() returns, while a
    // connection that was halted may still be running a reply's callback.
    void silence() {
        std::scoped_lock lock(progress_mutex);
        quiet = true;
    }
};

// One connection's part of a read: pieces taken in turn, READs pipelined within its depth and
// its share of the bytes in flight, the data put in place.
class Reader::Lane {
public:
    Lane(Shared& shared, sftp::Client& client, std::string handle, bool first)
        : s_(shared), c_(client), handle_(std::move(handle)), o_(*shared.options), first_(first),
          size_(std::clamp<std::uint32_t>(o_.first_read, 1, sftp::max_read)) {}

    // Reads until nothing is left for it (the first connection also waits for the others, whose
    // pieces come back to it if they break), or until the halt. True: the connection may be used
    // again; false: the halt left more than drop_over on its way, so it is dropped unread, or it
    // broke once the read was done. Throws sftp::Broken while reading.
    bool run() {
        for (;;) {
            if (halted()) break;
            fill();
            if (reads_ == 0) {
                if (first_ && wait_for_work()) continue;
                break;
            }
            c_.pump();
        }
        if (!first_) leave(); // nothing more to give back: the first connection needn't wait for it
        // The read is done or halted here, so a connection that breaks now only goes itself: the
        // bytes are in (read() checks that they all are), or weren't wanted any more.
        try {
            if (reads_ > 0) {
                if (in_flight_ > o_.drop_over) return false;
                while (c_.pending() > 0) c_.pump(); // drained: the replies come in and are thrown away
            }
            // A handle that won't close leaves nothing behind that matters here.
            [[maybe_unused]] auto closed = sftp::close_file(c_, handle_);
        } catch (const sftp::Broken&) {
            return false;
        }
        return true;
    }
    // After sftp::Broken: its bytes not read yet, back to the others.
    void give_back() {
        {
            std::scoped_lock lock(s_.mutex);
            if (pos_ < piece_end_) s_.returned.push_back({.at = pos_, .n = piece_end_ - pos_});
            for (const auto& [at, n] : asked_) s_.returned.push_back({.at = at, .n = n});
        }
        pos_ = piece_end_;
        asked_.clear();
        s_.changed.notify_all();
    }
    // Its seat went back for a download waiting for its share (ReadOptions::yield).
    [[nodiscard]] bool yielded() const { return yielded_; }
    // An extra connection stops counting among those reading (once).
    void leave() {
        if (std::exchange(left_, true)) return;
        {
            std::scoped_lock lock(s_.mutex);
            --s_.lanes;
            --s_.busy;
        }
        s_.changed.notify_all();
    }

private:
    [[nodiscard]] bool halted() const { return s_.halt.stop_requested(); }

    void fill() {
        while (!halted() && reads_ < std::max(o_.depth, 1)) {
            if (pos_ >= piece_end_ && !take()) return;
            const std::uint64_t n = std::min<std::uint64_t>(size_, piece_end_ - pos_);
            if (reads_ > 0 && in_flight_ + n > cap()) return;
            ask(pos_, n);
            pos_ += n;
            size_ = std::min(size_ * 2, sftp::max_read);
        }
    }

    // The next piece: one a broken connection gave back, else the next bytes no connection took,
    // a part of what's left for each connection reading, so the pieces shrink near the end.
    bool take() {
        if (yielded_) return false;
        if (!first_ && o_.yield && o_.yield()) {
            yielded_ = true;
            return false;
        }
        std::scoped_lock lock(s_.mutex);
        const std::uint64_t last = s_.last();
        while (!s_.returned.empty()) {
            const Span p = s_.returned.front();
            s_.returned.pop_front();
            if (p.at >= last) continue;
            pos_ = p.at;
            piece_end_ = std::min(p.at + p.n, last);
            return true;
        }
        if (s_.next >= last) return false;
        const std::uint64_t untaken = last - s_.next;
        const auto lanes = static_cast<std::uint64_t>(std::max(s_.lanes, 1));
        const std::uint64_t most = std::max(o_.piece, o_.min_piece);
        const std::uint64_t n = std::min(std::clamp(untaken / (2 * lanes), o_.min_piece, most), untaken);
        pos_ = s_.next;
        piece_end_ = s_.next + n;
        s_.next += n;
        return true;
    }

    // The bytes this connection may have on their way: its share of what's left, within its depth.
    std::uint64_t cap() {
        std::uint64_t left = 0;
        std::uint64_t lanes = 1;
        {
            std::scoped_lock lock(s_.mutex);
            const std::uint64_t whole = s_.last() - s_.start;
            const std::uint64_t in = s_.received.load();
            left = whole > in ? whole - in : 0;
            lanes = static_cast<std::uint64_t>(std::max(s_.lanes, 1));
        }
        const std::uint64_t deepest = std::uint64_t{sftp::max_read} * static_cast<std::uint64_t>(std::max(o_.depth, 1));
        return std::clamp<std::uint64_t>(left / lanes, sftp::max_read, deepest);
    }

    void ask(std::uint64_t at, std::uint64_t n) {
        ++reads_;
        in_flight_ += n;
        asked_[at] = n;
        c_.read(handle_, at, static_cast<std::uint32_t>(n), [this, at, n](std::expected<std::string_view, sftp::Status> r) {
            --reads_;
            in_flight_ -= n;
            asked_.erase(at);
            if (!r) {
                // EOF: the file ends at `at` (shorter than STAT said, or a size it didn't say).
                if (r.error().code == sftp::Code::eof) s_.ended_at(at);
                else s_.fail(status_error(r.error()));
                return;
            }
            if (halted()) return;
            const std::uint64_t got = std::min<std::uint64_t>(r->size(), n);
            std::memcpy(s_.buffer.data() + (at - s_.start), r->data(), got);
            s_.got(got);
            if (got < n) ask(at + got, n - got); // a short read: the rest, exactly
        });
    }

    // The first connection, out of pieces: waits while others read, for pieces one gives back.
    bool wait_for_work() {
        std::unique_lock lock(s_.mutex);
        s_.changed.wait(lock, s_.halt.get_token(), [this] { return !s_.returned.empty() || s_.busy == 0; });
        return !s_.returned.empty() && !s_.halt.stop_requested();
    }

    Shared& s_;
    sftp::Client& c_;
    std::string handle_;
    const ReadOptions& o_;
    bool first_;
    std::uint32_t size_;            // the next READ's
    std::uint64_t pos_ = 0;         // the piece's next byte to ask for
    std::uint64_t piece_end_ = 0;
    std::map<std::uint64_t, std::uint64_t> asked_; // READs on their way: offset -> length
    std::uint64_t in_flight_ = 0;
    int reads_ = 0;
    bool yielded_ = false;
    bool left_ = false;
};

Reader::Reader(std::shared_ptr<Pool> pool, std::string path, ReadOptions options)
    : pool_(std::move(pool)), path_(std::move(path)), options_(std::move(options)) {}

Reader::~Reader() {
    if (shared_) shared_->halt.request_stop();
    lease_.reset();
    extras_.clear(); // joined: they use the pool and the options
}

std::expected<FileStat, ReadError> Reader::stat() {
    bool retried = false; // a reused connection broke and the next one was tried
    for (;;) {
        if (options_.stop.stop_requested()) return std::unexpected(stopped());
        if (!lease_) {
            try {
                lease_ = pool_->acquire_remembered(options_.stop, options_.retry_refused);
            } catch (const Error& e) {
                return std::unexpected(ReadError{.kind = ReadFailure::login, .message = e.what(), .login = e.kind});
            }
            if (!lease_) return std::unexpected(stopped());
        }
        std::expected<sftp::Attrs, sftp::Status> attrs;
        try {
            attrs = sftp::stat((*lease_)->sftp(), path_);
        } catch (const sftp::Unavailable& e) {
            lease_->broken();
            lease_.reset();
            return std::unexpected(lost(e.what()));
        } catch (const sftp::Broken& e) {
            // A reused connection the server closed while it sat idle: the next one, once.
            lease_->broken();
            const bool again = lease_->reused() && !std::exchange(retried, true);
            lease_.reset();
            if (again) continue;
            return std::unexpected(lost(e.what()));
        }
        if (!attrs) return std::unexpected(status_error(attrs.error()));
        if (!attrs->is_file()) return std::unexpected(ReadError{.kind = ReadFailure::not_file, .message = "not a regular file"});
        stated_ = true;
        sized_ = attrs->size.has_value();
        return FileStat{.size = attrs->size, .mtime = attrs->mtime};
    }
}

std::expected<std::string, ReadError> Reader::read(std::uint64_t offset, std::uint64_t length) {
    if (!stated_ || !lease_) {
        if (auto st = stat(); !st) return std::unexpected(st.error());
    }
    if (shared_) { // a read before: its extra connections end first
        shared_->halt.request_stop();
        extras_.clear();
        shared_.reset();
    }
    if (options_.stop.stop_requested()) return std::unexpected(stopped());
    length = std::min(length, no_end - offset);
    if (length == 0) return std::string();

    auto s = std::make_shared<Shared>();
    s->start = offset;
    s->end = offset + length;
    s->next = offset;
    s->options = &options_;
    s->buffer.resize(length);
    shared_ = s;
    std::stop_callback on_stop(options_.stop, [&s] { s->halt.request_stop(); });

    // The connection closed rather than used again.
    auto drop = [this] {
        if (!lease_) return;
        lease_->broken();
        lease_.reset();
    };
    sftp::Client* client = nullptr;
    std::string handle;
    try {
        if (!lease_) return std::unexpected(lost("no connection"));
        client = &(*lease_)->sftp();
        auto opened = sftp::open_file(*client, path_);
        if (!opened) return std::unexpected(status_error(opened.error()));
        handle = std::move(*opened);
    } catch (const sftp::Broken& e) {
        drop();
        return std::unexpected(lost(e.what()));
    }
    // Extra connections only where the size is known: without it, the range is a cap that a small
    // file ends long before.
    if (options_.extra > 0 && sized_ && length >= options_.extra_from && options_.seat) {
        for (int i = 0; i < options_.extra; ++i) extras_.emplace_back([this, s] { extra_lane(s, s->halt.get_token()); });
    }

    Lane lane(*s, *client, std::move(handle), true);
    try {
        if (!lane.run()) drop(); // its replies still on the way: closed, not waited for
    } catch (const sftp::Broken& e) {
        drop();
        s->fail(lost(e.what()));
    }
    // Back to the pool now, not when the reader goes: the next preview, asked for as soon as this
    // one's answer is in, finds it there instead of logging in again.
    lease_.reset();
    stated_ = false;
    // Extra connections still logging in stop there.
    s->halt.request_stop();
    s->changed.notify_all();
    s->silence();

    std::scoped_lock lock(s->mutex);
    if (s->failed) return std::unexpected(*s->failed);
    if (options_.stop.stop_requested()) return std::unexpected(stopped());
    const std::uint64_t got = s->last() - s->start;
    if (s->received.load() < got) return std::unexpected(lost("the server sent less than the file holds"));
    std::string out = std::move(s->buffer);
    out.resize(got);
    // A file that ended early (no size from the server, or one cut while it was read) would keep
    // the whole range's allocation in the cache, which counts only size().
    if (out.capacity() - got > (std::size_t{1} << 20)) out.shrink_to_fit();
    return out;
}

void Reader::extra_lane(const std::shared_ptr<Shared>& shared, std::stop_token halt) {
    if (halt.stop_requested() || !options_.seat()) return;
    bool yielded = false;
    {
        std::optional<Pool::Lease> lease;
        try {
            // No pauses for a login turned away: an extra connection isn't worth waiting for.
            lease = pool_->acquire(halt, std::span<const std::chrono::milliseconds>{});
        } catch (const Error&) {
            lease.reset();
        }
        // Logged in after the read was done (or stopped): back at once, its SFTP never started.
        if (lease && !halt.stop_requested()) {
            try {
                sftp::Client& c = (*lease)->sftp();
                if (auto handle = sftp::open_file(c, path_)) {
                    {
                        std::scoped_lock lock(shared->mutex);
                        ++shared->lanes;
                        ++shared->busy;
                    }
                    Lane lane(*shared, c, std::move(*handle), false);
                    bool reusable = false;
                    try {
                        reusable = lane.run();
                    } catch (const sftp::Broken&) {
                        lane.give_back();
                    }
                    lane.leave();
                    yielded = lane.yielded();
                    if (!reusable) lease->broken();
                }
            } catch (const sftp::Broken&) {
                lease->broken();
            }
        }
    } // the connection goes back to the pool before its seat, for whoever takes the seat next
    if (!yielded && options_.seat_back) options_.seat_back();
}

} // namespace grab::ssh
