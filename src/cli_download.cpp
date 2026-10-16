#include "cli_download.hpp"

#include "conn_budget.hpp"
#include "fetch.hpp"
#include "folder_job.hpp"
#include "gui_state.hpp"
#include "process.hpp"
#include "progress_line.hpp"
#include "quote.hpp"
#include "rate.hpp"
#include "rclone.hpp"
#include "schedule.hpp"
#include "util.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <format>
#include <mutex>
#include <optional>
#include <print>
#include <stop_token>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)
#else
#include <csignal>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace grab {

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

// The download's progress on stderr: one line redrawn in place on a console. When stderr is a
// file or a pipe there is no live line, only the lines that end a download. Thread-safe.
class LiveLine {
public:
    LiveLine() {
#ifdef _WIN32
        handle_ = GetStdHandle(STD_ERROR_HANDLE);
        DWORD mode = 0;
        console_ = handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE && GetConsoleMode(handle_, &mode) != 0;
#else
        console_ = isatty(STDERR_FILENO) != 0;
#endif
    }

    // Redraws the line, at most every 250 ms unless `now`.
    void show(const std::string& text, bool now = false) {
        if (!console_) return;
        std::lock_guard lock(m_);
        const auto t = Clock::now();
        if (!now && t - last_ < std::chrono::milliseconds(250)) return;
        last_ = t;
        draw(progress::fit("  " + text, width() - 1)); // the last column would wrap
    }
    // Replaces the live line with `text`, kept as a normal line.
    void finish(const std::string& text) {
        std::lock_guard lock(m_);
        erase();
        std::println(stderr, "  {}", text);
        std::fflush(stderr);
    }
    // Removes the live line, e.g. before an error message.
    void clear() {
        std::lock_guard lock(m_);
        erase();
        std::fflush(stderr);
    }
    // A line above the live one (a command line with -v, a note); the next show() redraws.
    void message(const std::string& text) {
        std::lock_guard lock(m_);
        erase();
        std::println(stderr, "{}", text);
        std::fflush(stderr);
        last_ = {};
    }

private:
    [[nodiscard]] std::size_t width() const {
#ifdef _WIN32
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (GetConsoleScreenBufferInfo(handle_, &info)) {
            return static_cast<std::size_t>(std::max(info.srWindow.Right - info.srWindow.Left + 1, 20));
        }
#else
        winsize ws{};
        if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return std::max<std::size_t>(ws.ws_col, 20);
#endif
        return 80;
    }
    static std::size_t columns(std::string_view s) {
        return static_cast<std::size_t>(std::ranges::count_if(s, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
    }
    void draw(const std::string& text) {
        const std::size_t cols = columns(text);
        std::string out = "\r" + text;
        if (cols < shown_) out.append(shown_ - cols, ' '); // over the rest of the previous line
        std::fputs(out.c_str(), stderr);
        std::fflush(stderr);
        shown_ = std::max(cols, shown_);
    }
    void erase() {
        if (!console_ || shown_ == 0) return;
        std::fputs(("\r" + std::string(shown_, ' ') + "\r").c_str(), stderr);
        shown_ = 0;
    }

    std::mutex m_;
    bool console_ = false;
#ifdef _WIN32
    HANDLE handle_ = nullptr;
#endif
    std::size_t shown_ = 0; // columns of the live line on screen
    Clock::time_point last_{};
};

// While alive, Ctrl+C stops the downloads (its token) instead of ending grab, so partial files
// and their progress stay for the next run. A second Ctrl+C ends grab at once; the rclone
// processes are in kill-on-close job objects and go with it.
class StopOnCtrlC {
public:
    StopOnCtrlC() {
        source_ = &stop_;
        presses_ = 0;
#ifdef _WIN32
        // A parent can pass on "ignore Ctrl+C" (SetConsoleCtrlHandler(nullptr, TRUE) is
        // inherited), which would keep Ctrl+C from reaching the handler at all.
        SetConsoleCtrlHandler(nullptr, FALSE);
        SetConsoleCtrlHandler(&handler, TRUE);
#else
        std::signal(SIGINT, &handler);
        watcher_ = std::jthread([this](std::stop_token done) {
            while (!done.stop_requested()) {
                if (presses_.load() > 0) stop_.request_stop();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
#endif
    }
    ~StopOnCtrlC() {
#ifdef _WIN32
        SetConsoleCtrlHandler(&handler, FALSE);
#else
        std::signal(SIGINT, SIG_DFL);
#endif
        source_ = nullptr;
    }
    StopOnCtrlC(const StopOnCtrlC&) = delete;
    StopOnCtrlC& operator=(const StopOnCtrlC&) = delete;

    [[nodiscard]] std::stop_token token() const { return stop_.get_token(); }

private:
#ifdef _WIN32
    static BOOL WINAPI handler(DWORD type) {
        if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;
        if (presses_++ > 0 || source_ == nullptr) return FALSE; // the default: end grab
        source_->request_stop(); // the download prints how far it got
        return TRUE;
    }
#else
    static void handler(int) {
        if (presses_++ > 0) {
            std::signal(SIGINT, SIG_DFL);
            std::raise(SIGINT);
        }
    }
#endif
    static inline std::atomic<int> presses_{0};
    static inline std::stop_source* source_ = nullptr;
    std::stop_source stop_;
#ifndef _WIN32
    std::jthread watcher_;
#endif
};

// Follows the speed schedule while downloads run: when the scheduled limit changes, the
// limiter switches (grab's streams follow at once) and a folder's rclone batch, whose cap is
// fixed when it starts, starts again with the new one.
class ScheduleFollower {
public:
    ScheduleFollower(std::vector<schedule::Rule> rules, rate::RateLimiter& limiter, LiveLine& line)
        : rules_(std::move(rules)), limiter_(limiter), line_(line),
          current_(schedule::limit_at(rules_, schedule::local_now().minute)) {
        if (current_) limiter_.set_rate(schedule::bytes_per_s(*current_));
        thread_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
    }
    ScheduleFollower(const ScheduleFollower&) = delete;
    ScheduleFollower& operator=(const ScheduleFollower&) = delete;

    // While alive, the folder being downloaded: its batch restarts on a change.
    class Watch {
    public:
        Watch(ScheduleFollower* f, folder::Tree& tree, std::mutex& m) : f_(f) {
            if (f_ != nullptr) f_->watch(&tree, &m);
        }
        ~Watch() {
            if (f_ != nullptr) f_->watch(nullptr, nullptr);
        }
        Watch(const Watch&) = delete;
        Watch& operator=(const Watch&) = delete;

    private:
        ScheduleFollower* f_;
    };

private:
    void watch(folder::Tree* tree, std::mutex* m) {
        std::lock_guard lock(m_);
        tree_ = tree;
        tree_mutex_ = m;
    }

    void run(const std::stop_token& stop) {
        std::mutex sleep_m;
        std::condition_variable_any sleep;
        for (;;) {
            {
                std::unique_lock lock(sleep_m);
                sleep.wait_for(lock, stop, std::chrono::seconds(1), [] { return false; });
            }
            if (stop.stop_requested()) return;
            const auto now = schedule::local_now();
            const auto limit = schedule::limit_at(rules_, now.minute);
            if (limit == current_) continue;
            current_ = limit;
            limiter_.set_rate(limit ? schedule::bytes_per_s(*limit) : 0);
            {
                std::lock_guard lock(m_);
                if (tree_ != nullptr) {
                    std::lock_guard tree_lock(*tree_mutex_);
                    if (tree_->batch_running) tree_->batch_stop.request_stop();
                }
            }
            const auto next = schedule::next_change(rules_, now.minute);
            const std::string until = next ? " until " + schedule::when_text(now.minute, next->minutes) : "";
            line_.message(limit ? std::format("grab: speed schedule: limited to {}{}", schedule::speed_text(*limit), until)
                                : std::format("grab: speed schedule: no limit{}", until));
        }
    }

    const std::vector<schedule::Rule> rules_;
    rate::RateLimiter& limiter_;
    LiveLine& line_;
    std::optional<double> current_; // the follower thread's
    std::mutex m_;                  // guards tree_ and tree_mutex_
    folder::Tree* tree_ = nullptr;
    std::mutex* tree_mutex_ = nullptr;
    std::jthread thread_; // last: stops and joins first
};

struct Session {
    const engine::Context& ctx;
    const Options& opts;
    const std::filesystem::path& dest;
    LiveLine& line;
    fetch::Connections& connections;
    rate::RateLimiter& limiter;
    std::vector<std::string> flags; // common flags without console progress, plus `-- extra`
    engine::CommandHook trace;      // -v
    std::stop_token stop;
    ScheduleFollower* follower = nullptr; // when the speed schedule applies
};

int fetch_file(const Session& s, const std::string& path, int id) {
    const auto target = s.dest / util::path_from_utf8(remote_basename(path));
    std::uint64_t before = 0; // fetched by an earlier run
    if (auto p = fetch::saved_progress(target)) before = p->first;

    fetch::Download d;
    d.source = fetch::Source{s.ctx.config.rclone, s.ctx.config.rclone_config, s.ctx.settings.rclone_remote, path, s.flags};
    d.server = s.ctx.settings.name;
    d.id = id;
    d.target = target;
    d.limiter = &s.limiter;
    d.on_command = s.trace;
    const auto start = Clock::now();
    const auto result = fetch::run(d, s.connections, [&](const fetch::Progress& p) {
        s.line.show(progress::line({p.bytes, p.total, p.speed, p.eta, p.connections, {}, {}}));
    }, s.stop);
    const double secs = seconds_since(start);
    if (!result.note.empty()) {
        s.line.message("grab: note: " + result.note);
        before = 0; // it started over
    }

    switch (result.outcome) {
    case fetch::Outcome::done: {
        if (result.present) {
            s.line.finish("already there, nothing fetched");
        } else {
            std::error_code ec;
            const auto size = std::filesystem::file_size(target, ec);
            s.line.finish(progress::done(size, size - std::min(before, size), secs));
        }
        return 0;
    }
    case fetch::Outcome::stopped: {
        const auto p = fetch::saved_progress(target);
        s.line.finish(p && p->second > 0
                          ? std::format("stopped at {}%; run the same command again to continue", p->first * 100 / p->second)
                          : "stopped");
        return proc::exit_stopped;
    }
    case fetch::Outcome::failed:
        break;
    }
    s.line.clear();
    std::println(stderr, "grab: error: {}", result.error);
    if (fetch::saved_progress(target)) {
        std::println(stderr, "grab: what was fetched is kept; run the same command again to continue");
    }
    return exit_download_failed;
}

int fetch_folder(const Session& s, const std::string& path, int id) {
    folder::Tree tree;
    std::mutex m;
    const ScheduleFollower::Watch watch(s.follower, tree, m);
    folder::Job job;
    job.ctx = &s.ctx;
    job.connections = &s.connections;
    job.limiter = &s.limiter;
    job.server = s.ctx.settings.name;
    job.id = id;
    job.remote_path = path;
    job.dest = s.dest;
    job.batch_limit = [&s] { return s.limiter.rate(); }; // one download at a time: all of it
    job.extra = s.opts.extra;
    job.on_command = s.trace;
    job.on_event = [&](folder::Event) {
        folder::Totals t;
        int conns = 0;
        {
            std::lock_guard lock(m);
            t = folder::totals(tree);
            conns = tree.connections;
        }
        s.line.show(progress::line({t.bytes, t.total, t.speed, t.eta, conns, t.files_done, t.files}));
    };

    const auto start = Clock::now();
    s.line.show("listing the folder...", true);
    if (auto listed = folder::list(job, tree, m, s.stop); !listed) {
        s.line.clear();
        std::println(stderr, "grab: error: {}", listed.error());
        return exit_download_failed;
    }
    if (s.stop.stop_requested()) {
        s.line.finish("stopped");
        return proc::exit_stopped;
    }
    folder::mark_existing(job, tree, m);
    std::uint64_t before = 0;    // on disk already: finished files, partial big ones
    std::size_t already = 0;     // files that were complete
    {
        std::lock_guard lock(m);
        const auto t = folder::totals(tree);
        before = t.bytes;
        already = t.files_done;
    }
    folder::run(job, tree, m, s.stop);

    folder::Totals t;
    folder::Outcome o;
    {
        std::lock_guard lock(m);
        t = folder::totals(tree);
        o = folder::outcome(tree);
    }
    if (s.stop.stop_requested()) {
        s.line.finish(std::format("stopped with {} of {} files done; run the same command again to continue",
                                  t.files_done, t.files));
        return proc::exit_stopped;
    }
    if (o.failed > 0) {
        s.line.clear();
        std::println(stderr, "grab: error: {} file{} failed. {}", o.failed, o.failed == 1 ? "" : "s", o.first_error);
        std::println(stderr, "grab: the other files are on disk; run the same command again to retry");
        return exit_download_failed;
    }
    if (already == t.files && t.files > 0) {
        s.line.finish(std::format("already there, nothing fetched ({} file{}, {})", t.files, t.files == 1 ? "" : "s",
                                  util::format_size(t.total)));
        return 0;
    }
    std::string text = progress::done(t.total, t.bytes - std::min(before, t.bytes), seconds_since(start), t.files);
    if (already > 0) text += std::format("; {} {} already there", already, already == 1 ? "was" : "were");
    s.line.finish(text);
    return 0;
}

// --dry-run: what would be fetched, without writing anything.
void describe(const Session& s, const std::string& path, std::optional<std::uint64_t> size, int id,
              const std::string& position, int budget) {
    const auto local = util::path_to_utf8(s.dest / util::path_from_utf8(remote_basename(path)));
    std::string what;
    if (s.opts.mode == Mode::file) {
        if (size) what = std::format(" ({})", util::format_size(*size));
    } else {
        folder::Tree tree;
        std::mutex m;
        folder::Job job;
        job.ctx = &s.ctx;
        job.remote_path = path;
        job.dest = s.dest;
        job.extra = s.opts.extra;
        job.on_command = s.trace;
        job.id = id;
        if (auto listed = folder::list(job, tree, m, s.stop); !listed) {
            what = std::format(" (cannot list it: {})", listed.error());
        } else {
            folder::mark_existing(job, tree, m);
            const auto t = folder::totals(tree);
            what = std::format(" ({} file{}, {}", t.files, t.files == 1 ? "" : "s", util::format_size(t.total));
            if (t.files_done > 0) what += std::format("; {} already there", t.files_done);
            what += ")";
        }
    }
    std::println(stderr, "grab: would fetch {}{} {}{}  ->  {}  (up to {} connections)", mode_noun(s.opts.mode), position,
                 remote_spec(s.ctx.settings.rclone_remote, path), what, local, budget);
}

} // namespace

std::optional<std::string> schedule_note(const std::vector<schedule::Rule>& rules, const schedule::Now& now) {
    const auto limit = schedule::limit_at(rules, now.minute);
    if (!limit) return std::nullopt;
    const auto next = schedule::next_change(rules, now.minute);
    return std::format("speed limited to {}{} by the speed schedule (--limit off ignores it)",
                       schedule::speed_text(*limit), next ? " until " + schedule::when_text(now.minute, next->minutes) : "");
}

DownloadTally download_with_grab(const engine::Context& ctx, const Options& opts, const std::vector<std::string>& paths,
                                 const std::unordered_map<std::string, std::uint64_t>& sizes,
                                 const std::filesystem::path& dest) {
    LiveLine line;
    const std::string& server = ctx.settings.name;
    fetch::Connections connections([&line](const std::string& name, int budget) {
        line.message(std::format("grab: {} refused a connection; using {} from now on", name, budget));
    });
    // The budget grab-gui uses, including what it learned from refusals (read only: gui.json
    // belongs to the window).
    std::optional<int> learned;
    const auto gui = load_gui_state(default_gui_state_path());
    if (const auto it = gui.server_limits.find(server); it != gui.server_limits.end()) learned = it->second;
    const int budget = budget::effective_budget(ctx.settings.max_connections, budget::default_budget(ctx.remote.host), learned);
    connections.configure(server, budget);
    rate::RateLimiter limiter;
    if (opts.limit) limiter.set_rate(*opts.limit);
    // Without --limit, the speed schedule set in grab's window decides, also mid-download.
    const auto rules = opts.limit ? std::vector<schedule::Rule>{} : gui.speed_schedule;
    if (auto note = schedule_note(rules, schedule::local_now())) std::println(stderr, "grab: {}", *note);
    std::optional<ScheduleFollower> follower;
    if (!opts.dry_run && !rules.empty()) follower.emplace(rules, limiter, line);

    StopOnCtrlC ctrl_c;
    Session s{ctx, opts, dest, line, connections, limiter, engine::without_console_progress(ctx.settings.common_flags), {},
              ctrl_c.token()};
    if (follower) s.follower = &*follower;
    s.flags.insert(s.flags.end(), opts.extra.begin(), opts.extra.end());
    if (opts.verbose) {
        s.trace = [&line](const std::vector<std::string>& argv) { line.message("+ " + quote::display_cmdline(argv)); };
        std::println(stderr, "grab: up to {} connections to {}", budget, server);
    }

    DownloadTally tally;
    const std::size_t total = paths.size();
    for (std::size_t i = 0; i < total; ++i) {
        const std::string& path = paths[i];
        const int id = static_cast<int>(i) + 1;
        const std::string position = total > 1 ? std::format(" {}/{}", i + 1, total) : "";
        if (opts.dry_run) {
            const auto it = sizes.find(path);
            describe(s, path, it == sizes.end() ? std::nullopt : std::optional(it->second), id, position, budget);
            continue;
        }
        std::println(stderr, "grab: {}{} {}  ->  {}", mode_noun(opts.mode), position,
                     remote_spec(ctx.settings.rclone_remote, path),
                     util::path_to_utf8(dest / util::path_from_utf8(remote_basename(path))));
        const int code = opts.mode == Mode::file ? fetch_file(s, path, id) : fetch_folder(s, path, id);
        if (code == 0) {
            ++tally.done;
            continue;
        }
        if (tally.code == 0) tally.code = code;
        tally.failed.push_back(path);
        if (code == proc::exit_stopped) {
            tally.stopped = i + 1 < total;
            break;
        }
    }
    return tally;
}

} // namespace grab
