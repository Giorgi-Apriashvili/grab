#include "ssh_pool.hpp"

#include "conn_budget.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace grab::ssh {

namespace {

// A Connection whose SFTP session starts when it's first used: a run that only needs commands (a
// search or a list by find) saves its 3 round trips, ~0.3 s on Main. Members go in reverse:
// client, channel, login.
class SshSession : public Session {
public:
    explicit SshSession(const Target& target) : connection_(target) {}
    sftp::Client& sftp() override {
        if (!client_) start_sftp();
        return *client_;
    }
    std::expected<ExecResult, ExecError> exec(std::string_view command, const ExecOptions& options) override {
        return connection_.exec(command, options);
    }
    Connection* connection() override { return &connection_; }
    bool reusable() override {
        return !connection_.dropped() && !sftp_failed_ && (!client_ || client_->pending() == 0);
    }

private:
    void start_sftp() {
        auto reset = [this] {
            client_.reset();
            channel_.reset();
            sftp_failed_ = true;
        };
        try {
            channel_ = connection_.open_sftp();
            client_.emplace(*channel_);
            client_->start();
        } catch (const Error& e) {
            reset();
            // A server that refuses SFTP refuses it on every connection: no retries.
            if (e.kind == budget::Failure::fatal) throw sftp::Unavailable(e.what());
            throw sftp::Broken(e.what());
        } catch (const sftp::Broken& e) {
            reset();
            throw sftp::Broken(std::string("SFTP didn't start: ") + e.what());
        }
    }

    Connection connection_;
    std::unique_ptr<sftp::Channel> channel_;
    std::optional<sftp::Client> client_;
    bool sftp_failed_ = false; // it couldn't start: the session isn't reused
};

} // namespace

std::expected<ExecResult, ExecError> Session::exec(std::string_view /*command*/, const ExecOptions& /*options*/) {
    return std::unexpected(ExecError{.kind = ExecFailure::refused, .message = "no commands here", .err = {}});
}

std::unique_ptr<Session> open_session(const Target& target) { return std::make_unique<SshSession>(target); }

int hashes_at_once_for(std::string_view host) { return budget::is_storage_box(host) ? 2 : 1; }

Pool::Pool(Connect connect, Options options) : connect_(std::move(connect)), options_(std::move(options)) {
    options_.limit = std::max(options_.limit, 1);
    options_.logins_at_once = std::max(options_.logins_at_once, 1);
    options_.hashes_at_once = std::max(options_.hashes_at_once, 1);
    if (options_.idle_timer) {
        timer_ = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                {
                    std::unique_lock lock(mutex_);
                    changed_.wait_for(lock, stop, std::chrono::seconds(1), [] { return false; });
                }
                if (!stop.stop_requested()) close_idle(Clock::now());
            }
        });
    }
}

Pool::~Pool() {
    timer_.request_stop();
    if (timer_.joinable()) timer_.join();
}

std::optional<Pool::Lease> Pool::acquire(std::stop_token stop, Retry retry_refused, int logins_at_once) {
    const std::span<const std::chrono::milliseconds> pauses = retry_refused.value_or(options_.retry_refused);
    const int at_once = logins_at_once > 0 ? logins_at_once : options_.logins_at_once;
    std::unique_lock lock(mutex_);
    auto room = [this, at_once] { return open_locked() < options_.limit && logging_in_ < at_once; };
    for (;;) {
        if (stop.stop_requested()) return std::nullopt;
        if (!idle_.empty()) {
            std::unique_ptr<Session> session = std::move(idle_.back().session);
            idle_.pop_back();
            ++leased_;
            return Lease(this, std::move(session), true);
        }
        if (room()) break;
        changed_.wait(lock, stop, [&] { return !idle_.empty() || room(); });
    }
    return log_in(lock, stop, pauses);
}

std::optional<Pool::Lease> Pool::acquire_new(std::stop_token stop, Retry retry_refused) {
    const std::span<const std::chrono::milliseconds> pauses = retry_refused.value_or(options_.retry_refused);
    std::unique_lock lock(mutex_);
    auto room = [this] { return open_locked() < options_.limit && logging_in_ < options_.logins_at_once; };
    // Closing an idle connection makes room under the limit, not among the logins at once.
    auto closable = [this] {
        return !idle_.empty() && open_locked() >= options_.limit && logging_in_ < options_.logins_at_once;
    };
    for (;;) {
        if (stop.stop_requested()) return std::nullopt;
        if (room()) break;
        if (closable()) {
            std::unique_ptr<Session> drop = std::move(idle_.front().session);
            idle_.pop_front();
            lock.unlock();
            drop.reset(); // outside the lock: a disconnect talks to the server
            lock.lock();
            continue;
        }
        changed_.wait(lock, stop, [&] { return room() || closable(); });
    }
    return log_in(lock, stop, pauses);
}

std::optional<Pool::Lease> Pool::acquire_remembered(std::stop_token stop, Retry retry_refused, int logins_at_once) {
    bool trying = false; // this caller logs in while none of these logins has worked yet
    {
        std::unique_lock lock(mutex_);
        // The failure that still holds, if any.
        auto failed = [this]() -> const LoginFailure* {
            if (!login_failure_) return nullptr;
            const LoginFailure& f = *login_failure_;
            const bool holds = f.kind == budget::Failure::fatal || Clock::now() - f.since < options_.login_failure_holds;
            return holds ? &f : nullptr;
        };
        if (!changed_.wait(lock, stop, [&] { return !idle_.empty() || failed() != nullptr || !login_trying_; })) {
            return std::nullopt;
        }
        // An idle connection costs no login, whatever failed before (another caller's login worked).
        if (!idle_.empty()) {
            std::unique_ptr<Session> session = std::move(idle_.back().session);
            idle_.pop_back();
            ++leased_;
            return Lease(this, std::move(session), true);
        }
        if (const LoginFailure* f = failed()) throw Error(f->kind, f->what);
        if (!login_works_) trying = login_trying_ = true;
    }
    // Its outcome, for the next ones.
    auto outcome = [&](std::optional<LoginFailure> failure, bool worked) {
        {
            std::scoped_lock lock(mutex_);
            if (trying) login_trying_ = false;
            if (failure) {
                login_failure_ = std::move(failure);
                login_works_ = false;
            } else if (worked) {
                login_failure_.reset();
                login_works_ = true;
            }
        }
        changed_.notify_all();
    };
    std::optional<Lease> lease;
    try {
        lease = acquire(std::move(stop), retry_refused, logins_at_once);
    } catch (const Error& e) {
        outcome(LoginFailure{.kind = e.kind, .what = e.what(), .since = Clock::now()}, false);
        throw;
    } catch (...) {
        outcome(std::nullopt, false);
        throw;
    }
    outcome(std::nullopt, lease.has_value());
    return lease;
}

void Pool::forget_login_failure() {
    {
        std::scoped_lock lock(mutex_);
        login_failure_.reset();
    }
    changed_.notify_all();
}

std::optional<Pool::Lease> Pool::log_in(std::unique_lock<std::mutex>& lock, const std::stop_token& stop,
                                        std::span<const std::chrono::milliseconds> pauses) {
    // A new login, outside the lock: it takes a second or more.
    ++logging_in_;
    lock.unlock();
    std::unique_ptr<Session> session;
    std::exception_ptr failed;
    for (std::size_t attempt = 0;; ++attempt) {
        try {
            session = connect_();
            break;
        } catch (const Error& e) {
            if (e.kind != budget::Failure::refused || attempt >= pauses.size()) {
                failed = std::current_exception();
                break;
            }
        } catch (...) {
            failed = std::current_exception();
            break;
        }
        std::unique_lock pause(mutex_);
        changed_.wait_for(pause, stop, pauses[attempt], [] { return false; });
        if (stop.stop_requested()) break;
    }
    lock.lock();
    --logging_in_;
    if (session) {
        ++leased_;
        // Any caller's login that works (a download's, Browse's) is acquire_remembered's too: same
        // pool, same target.
        login_failure_.reset();
        login_works_ = true;
    }
    changed_.notify_all();
    if (failed) std::rethrow_exception(failed);
    if (!session) return std::nullopt;
    return Lease(this, std::move(session), false);
}

void Pool::give_back(std::unique_ptr<Session> session, bool broken) {
    std::unique_ptr<Session> drop; // closed outside the lock: a disconnect talks to the server
    {
        std::lock_guard lock(mutex_);
        --leased_;
        if (broken || !session->reusable() || open_locked() >= options_.limit) drop = std::move(session);
        else idle_.push_back({std::move(session), Clock::now()});
    }
    changed_.notify_all();
}

void Pool::set_limit(int limit) {
    std::vector<std::unique_ptr<Session>> drop;
    {
        std::lock_guard lock(mutex_);
        options_.limit = std::max(limit, 1);
        while (open_locked() > options_.limit && !idle_.empty()) {
            drop.push_back(std::move(idle_.front().session));
            idle_.pop_front();
        }
    }
    changed_.notify_all();
}

int Pool::limit() const {
    std::lock_guard lock(mutex_);
    return options_.limit;
}

int Pool::open() const {
    std::lock_guard lock(mutex_);
    return open_locked();
}

int Pool::idle() const {
    std::lock_guard lock(mutex_);
    return static_cast<int>(idle_.size());
}

void Pool::close_idle(Clock::time_point now) {
    std::vector<std::unique_ptr<Session>> drop;
    {
        std::lock_guard lock(mutex_);
        while (!idle_.empty()) {
            // The oldest first; the last one left is the most recently used.
            const auto after = idle_.size() == 1 ? std::max(options_.idle_close, options_.keep_newest) : options_.idle_close;
            if (now - idle_.front().since < after) break;
            drop.push_back(std::move(idle_.front().session));
            idle_.pop_front();
        }
    }
    if (!drop.empty()) changed_.notify_all();
}

void Pool::trim_idle(int keep) {
    std::vector<std::unique_ptr<Session>> drop;
    {
        std::scoped_lock lock(mutex_);
        while (std::cmp_greater(idle_.size(), std::max(keep, 0))) {
            drop.push_back(std::move(idle_.front().session));
            idle_.pop_front();
        }
    }
    if (!drop.empty()) changed_.notify_all();
}

template <class Check>
std::optional<Check> Pool::ask(Asked<Check>& asked, const std::stop_token& stop,
                               const std::function<std::optional<Check>()>& check) {
    std::unique_lock lock(mutex_);
    auto known = [&] { return asked.answer && Clock::now() - asked.since < holds(*asked.answer); };
    if (!changed_.wait(lock, stop, [&] { return known() || !asked.asking; })) return std::nullopt;
    if (known()) return asked.answer;
    // This caller asks; the others wait for its answer.
    asked.asking = true;
    lock.unlock();
    std::optional<Check> answer;
    try {
        answer = check();
    } catch (...) {
        lock.lock();
        asked.asking = false;
        lock.unlock();
        changed_.notify_all();
        throw;
    }
    lock.lock();
    asked.asking = false;
    if (answer) {
        asked.answer = answer;
        asked.since = Clock::now();
        // An answer an earlier run saved holds from when the server gave it.
        if constexpr (std::is_same_v<Check, FindCheck>) {
            if (answer->checked > 0) {
                const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::system_clock::now().time_since_epoch()).count();
                asked.since -= std::chrono::seconds(std::max<std::int64_t>(0, now - answer->checked));
            }
        }
    }
    lock.unlock();
    changed_.notify_all();
    return answer;
}

Pool::Clock::duration Pool::holds(const FindCheck& check) const {
    return check.lasting ? options_.find_check_trust : options_.find_check_expiry;
}

Pool::Clock::duration Pool::holds(const HashCheck& check) const {
    return check.lasting ? Clock::duration::max() : Clock::duration(options_.find_check_expiry);
}

std::optional<FindCheck> Pool::find_check(std::stop_token stop, const std::function<std::optional<FindCheck>()>& check) {
    return ask(find_asked_, stop, check);
}

void Pool::forget_find_check() {
    std::scoped_lock lock(mutex_);
    find_asked_.answer.reset();
}

std::optional<HashCheck> Pool::hash_check(std::stop_token stop, const std::function<std::optional<HashCheck>()>& check) {
    return ask(hash_asked_, stop, check);
}

void Pool::set_hash_check(std::optional<HashCheck> answer) {
    std::scoped_lock lock(mutex_);
    hash_asked_.answer = std::move(answer);
    hash_asked_.since = Clock::now();
}

std::optional<Pool::Turn> Pool::hash_turn(std::stop_token stop) {
    std::unique_lock lock(mutex_);
    if (!changed_.wait(lock, stop, [this] { return hashing_ < options_.hashes_at_once; })) return std::nullopt;
    ++hashing_;
    return Turn(this);
}

int Pool::hashes_at_once() const {
    std::scoped_lock lock(mutex_);
    return options_.hashes_at_once;
}

Pool::Turn& Pool::Turn::operator=(Turn&& other) noexcept {
    if (this != &other) {
        release();
        pool_ = std::exchange(other.pool_, nullptr);
    }
    return *this;
}

void Pool::Turn::release() {
    if (!pool_) return;
    {
        std::scoped_lock lock(pool_->mutex_);
        --pool_->hashing_;
    }
    pool_->changed_.notify_all();
    pool_ = nullptr;
}

Pool::Lease::Lease(Lease&& other) noexcept
    : pool_(std::exchange(other.pool_, nullptr)), session_(std::move(other.session_)), broken_(other.broken_),
      reused_(other.reused_) {}

Pool::Lease& Pool::Lease::operator=(Lease&& other) noexcept {
    if (this != &other) {
        release();
        pool_ = std::exchange(other.pool_, nullptr);
        session_ = std::move(other.session_);
        broken_ = other.broken_;
        reused_ = other.reused_;
    }
    return *this;
}

Pool::Lease::~Lease() { release(); }

void Pool::Lease::release() {
    if (pool_ && session_) pool_->give_back(std::move(session_), broken_);
    pool_ = nullptr;
}

Pool::Options Pools::download_options(int limit) {
    Pool::Options o;
    o.limit = limit;
    o.idle_close = std::chrono::seconds(15);
    o.keep_newest = std::chrono::seconds(60);
    o.retry_refused = {};
    return o;
}

std::shared_ptr<Pool> Pools::get(const std::string& server, const Target& target, int limit) {
    std::lock_guard lock(mutex_);
    auto it = pools_.find(server);
    if (it != pools_.end() && it->second.target == target) {
        it->second.pool->set_limit(limit);
        return it->second.pool;
    }
    auto options = download_options(limit);
    options.hashes_at_once = hashes_at_once_for(target.host);
    auto pool = std::make_shared<Pool>([target] { return open_session(target); }, std::move(options));
    pools_[server] = Entry{target, pool};
    return pool;
}

void Pools::forget_find_check(const std::string& server) {
    std::lock_guard lock(mutex_);
    if (const auto it = pools_.find(server); it != pools_.end() && it->second.pool) it->second.pool->forget_find_check();
}

void Pools::forget_hash_check(const std::string& server) {
    std::lock_guard lock(mutex_);
    if (const auto it = pools_.find(server); it != pools_.end() && it->second.pool) it->second.pool->set_hash_check(std::nullopt);
}

void Pools::forget_login_failure(const std::string& server) {
    std::scoped_lock lock(mutex_);
    if (const auto it = pools_.find(server); it != pools_.end() && it->second.pool) it->second.pool->forget_login_failure();
}

void Pools::forget(const std::string& server) {
    std::shared_ptr<Pool> pool;
    {
        std::scoped_lock lock(mutex_);
        const auto it = pools_.find(server);
        if (it == pools_.end()) return;
        pool = std::move(it->second.pool);
        pools_.erase(it);
    }
    if (!pool) return;
    pool->trim_idle(0);
    pool->forget_find_check();
    pool->set_hash_check(std::nullopt);
    pool->forget_login_failure();
}

} // namespace grab::ssh
