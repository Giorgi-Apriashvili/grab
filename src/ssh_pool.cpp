#include "ssh_pool.hpp"

#include "conn_budget.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <string>
#include <utility>

namespace grab::ssh {

namespace {

// A Connection with its SFTP session started. Members go in reverse: client, channel, login.
class SshSession : public Session {
public:
    explicit SshSession(const Target& target) : connection_(target), channel_(connection_.open_sftp()), client_(*channel_) {
        try {
            client_.start();
        } catch (const sftp::Broken& e) {
            throw Error(budget::Failure::other, std::string("SFTP didn't start: ") + e.what());
        }
    }
    sftp::Client& sftp() override { return client_; }
    std::expected<ExecResult, ExecError> exec(std::string_view command, const ExecOptions& options) override {
        return connection_.exec(command, options);
    }
    Connection* connection() override { return &connection_; }
    bool reusable() override { return !connection_.dropped() && client_.pending() == 0; }

private:
    Connection connection_;
    std::unique_ptr<sftp::Channel> channel_;
    sftp::Client client_;
};

} // namespace

std::expected<ExecResult, ExecError> Session::exec(std::string_view /*command*/, const ExecOptions& /*options*/) {
    return std::unexpected(ExecError{.kind = ExecFailure::refused, .message = "no commands here", .err = {}});
}

std::unique_ptr<Session> open_session(const Target& target) { return std::make_unique<SshSession>(target); }

Pool::Pool(Connect connect, Options options) : connect_(std::move(connect)), options_(std::move(options)) {
    options_.limit = std::max(options_.limit, 1);
    options_.logins_at_once = std::max(options_.logins_at_once, 1);
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

std::optional<Pool::Lease> Pool::acquire(std::stop_token stop) {
    std::unique_lock lock(mutex_);
    auto room = [this] { return open_locked() < options_.limit && logging_in_ < options_.logins_at_once; };
    for (;;) {
        if (stop.stop_requested()) return std::nullopt;
        if (!idle_.empty()) {
            std::unique_ptr<Session> session = std::move(idle_.back().session);
            idle_.pop_back();
            ++leased_;
            return Lease(this, std::move(session));
        }
        if (room()) break;
        changed_.wait(lock, stop, [&] { return !idle_.empty() || room(); });
    }

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
            if (e.kind != budget::Failure::refused || attempt >= options_.retry_refused.size()) {
                failed = std::current_exception();
                break;
            }
        } catch (...) {
            failed = std::current_exception();
            break;
        }
        std::unique_lock pause(mutex_);
        changed_.wait_for(pause, stop, options_.retry_refused[attempt], [] { return false; });
        if (stop.stop_requested()) break;
    }
    lock.lock();
    --logging_in_;
    if (session) ++leased_;
    changed_.notify_all();
    if (failed) std::rethrow_exception(failed);
    if (!session) return std::nullopt;
    return Lease(this, std::move(session));
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
        while (!idle_.empty() && now - idle_.front().since >= options_.idle_close) {
            drop.push_back(std::move(idle_.front().session));
            idle_.pop_front();
        }
    }
    if (!drop.empty()) changed_.notify_all();
}

Pool::Lease::Lease(Lease&& other) noexcept
    : pool_(std::exchange(other.pool_, nullptr)), session_(std::move(other.session_)), broken_(other.broken_) {}

Pool::Lease& Pool::Lease::operator=(Lease&& other) noexcept {
    if (this != &other) {
        release();
        pool_ = std::exchange(other.pool_, nullptr);
        session_ = std::move(other.session_);
        broken_ = other.broken_;
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
    auto pool = std::make_shared<Pool>([target] { return open_session(target); }, download_options(limit));
    pools_[server] = Entry{target, pool};
    return pool;
}

} // namespace grab::ssh
