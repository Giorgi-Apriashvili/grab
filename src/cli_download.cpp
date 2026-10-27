#include "cli_download.hpp"

#include "conn_budget.hpp"
#include "existing.hpp"
#include "fetch.hpp"
#include "folder_job.hpp"
#include "gui_state.hpp"
#include "local_name.hpp"
#include "process.hpp"
#include "progress_line.hpp"
#include "quote.hpp"
#include "rate.hpp"
#include "rclone.hpp"
#include "rclone_flags.hpp"
#include "schedule.hpp"
#include "space.hpp"
#include "ssh_pool.hpp"
#include "util.hpp"
#include "verify.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <format>
#include <iostream>
#include <map>
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
    bool verify = false;                  // check downloads against the server's checksum
    int files_at_once = 4;                // a folder's files at once: the window's "At once"
    // "A different file is already there" (review G105): the answers given in the question before
    // fetching, for the files listed in it (remote path -> answer); one for all the rest
    // (--existing, or a capital letter); anything else is asked when it comes up.
    const std::map<std::string, existing::Choice>* answered = nullptr;
    std::optional<existing::Choice>* for_rest = nullptr;
    bool several = false; // more than one pick: the question offers "the same for the rest"
    // engine = grab: files and folders through grab's own SSH client.
    std::shared_ptr<ssh::Pool> pool = nullptr;
    // With `verify`: grab's own connections to the server, which every server verifies through
    // (`pool` on engine = grab), or why there are none.
    std::shared_ptr<ssh::Pool> verify_pool = nullptr;
    std::string no_verify_pool = {};
};

// The live line while a verification runs: its text, a folder's files so far ("12 of 40"), and
// after 5 s the time taken, redrawn every second, as md5sum says nothing until it ends.
class VerifyLine {
public:
    VerifyLine(LiveLine& line, std::string text) : line_(line), text_(std::move(text)), start_(Clock::now()) {
        draw(true);
        thread_ = std::jthread([this](const std::stop_token& stop) {
            std::mutex sleep_m;
            std::condition_variable_any sleep;
            for (;;) {
                {
                    std::unique_lock lock(sleep_m);
                    sleep.wait_for(lock, stop, std::chrono::seconds(1), [] { return false; });
                }
                if (stop.stop_requested()) return;
                draw(true);
            }
        });
    }
    VerifyLine(const VerifyLine&) = delete;
    VerifyLine& operator=(const VerifyLine&) = delete;
    ~VerifyLine() = default;

    void files(std::size_t done, std::size_t total) {
        {
            std::scoped_lock lock(m_);
            files_ = std::format(" {} of {}", done, total);
        }
        draw(false);
    }

private:
    void draw(bool now) {
        std::string text;
        {
            std::scoped_lock lock(m_);
            text = text_ + files_;
        }
        if (const double secs = seconds_since(start_); secs >= 5) text += std::format(" ({})", progress::duration(secs));
        line_.show(text, now);
    }

    LiveLine& line_;
    const std::string text_;
    const Clock::time_point start_;
    std::mutex m_; // files_
    std::string files_;
    std::jthread thread_; // last: stops and joins first
};

// What a verification of download `id` needs from the session.
verify::Request verify_request(const Session& s, int id) {
    verify::Request r;
    r.ctx = &s.ctx;
    r.pool = s.verify_pool;
    r.no_pool = s.no_verify_pool;
    r.close_idle_after = !s.pool; // engine = rclone: a pool only to verify, closed between files
    r.connections = &s.connections;
    r.server = s.ctx.settings.name;
    r.id = id;
    r.on_command = s.trace;
    return r;
}

// "name  (yours: 1.2 GiB, 2024-01-02; the server's: 1.4 GiB, 2025-03-04)"
std::string existing_line(const ExistingFile& f) {
    auto when = [](std::optional<std::int64_t> t) { return t ? ", " + util::local_date(*t) : std::string(); };
    const auto mine = existing::local_file(f.local).value_or(existing::Local{});
    if (!f.uncompared.empty()) {
        std::error_code ec;
        if (std::filesystem::is_directory(f.local, ec)) {
            return std::format("{}/  (your folder; couldn't compare with the server's: {})", f.name, f.uncompared);
        }
        return std::format("{}  (yours: {}{}; couldn't compare with the server's: {})", f.name,
                           util::format_size(mine.size), when(mine.modified), f.uncompared);
    }
    return std::format("{}  (yours: {}{}; the server's: {}{})", f.name, util::format_size(mine.size), when(mine.modified),
                       util::format_size(f.size), when(f.modified));
}

// `target`: where it lands (DEST\<name>, or a numbered name when another pick has the same).
int fetch_file(const Session& s, const std::string& path, int id, std::filesystem::path target) {
    const std::string& rclone_remote = s.ctx.settings.rclone_remote;
    std::uint64_t before = 0; // fetched by an earlier run
    if (auto p = fetch::saved_progress(target, path, {}, rclone_remote)) before = p->first;

    fetch::Download d;
    d.source = fetch::Source{s.ctx.config.rclone, s.ctx.config.rclone_config, s.ctx.settings.rclone_remote, path, s.flags};
    d.server = s.ctx.settings.name;
    d.id = id;
    d.limiter = &s.limiter;
    d.on_command = s.trace;
    d.pool = s.pool;
    const auto start = Clock::now();
    auto choice = [&] {
        if (const auto it = s.answered->find(path); it != s.answered->end()) return it->second;
        return s.for_rest->value_or(existing::Choice::ask);
    }();
    auto fetch_once = [&] {
        d.target = target;
        d.replace = choice == existing::Choice::replace;
        return fetch::run(d, s.connections, [&](const fetch::Progress& p) {
            s.line.show(progress::line({p.bytes, p.total, p.speed, p.eta, p.connections, {}, {}}));
        }, s.stop);
    };
    auto result = fetch_once();
    // A different file already there that the check before fetching didn't see, or one asked
    // about: the answer decides.
    while (result.outcome == fetch::Outcome::exists) {
        if (choice == existing::Choice::ask) {
            s.line.clear();
            const ExistingFile f{util::path_to_utf8(target.filename()), target, result.size,
                                 util::parse_rfc3339_seconds(result.modtime)};
            const auto answer = choose_existing(s.opts, {f}, 1, true, s.several);
            if (!answer) return answer.error();
            choice = answer->choice;
            if (answer->for_all) *s.for_rest = choice;
        }
        if (choice == existing::Choice::skip) {
            fetch::discard(target, path, {}, rclone_remote); // what was fetched for it won't be used (G74)
            s.line.finish("skipped: your file is kept");
            return 0;
        }
        if (choice == existing::Choice::keep_both) {
            // A copy an earlier run kept that already is the server's file (review G112).
            if (const auto kept = existing::kept_copy(target, result.size, util::parse_rfc3339(result.modtime))) {
                s.line.finish(std::format("already there as {}, nothing fetched", util::path_to_utf8(kept->filename())));
                return 0;
            }
            const auto second = existing::second_copy(target);
            if (!second) {
                s.line.clear();
                std::println(stderr, "grab: error: no free name for a second copy next to {}", util::path_to_utf8(target));
                return exit_download_failed;
            }
            // What was fetched goes on under the new name, done already if it was complete (G74).
            fetch::move_partial(target, *second, path, rclone_remote);
            target = *second;
            s.line.message("grab: saving it as " + util::path_to_utf8(target.filename()));
            before = 0;
            if (auto p = fetch::saved_progress(target, path, {}, rclone_remote)) before = p->first;
        }
        result = fetch_once();
    }
    const double secs = seconds_since(start);
    if (!result.note.empty()) {
        s.line.message("grab: note: " + result.note);
        before = 0; // it started over
    }

    switch (result.outcome) {
    case fetch::Outcome::done: {
        if (result.present) {
            s.line.finish("already there, nothing fetched");
            return 0;
        }
        std::error_code ec;
        const auto size = std::filesystem::file_size(target, ec);
        s.line.finish(progress::done(size, size - std::min(before, size), secs));
        if (!s.verify) return 0;
        const auto check_start = Clock::now();
        verify::Report checked;
        {
            const VerifyLine shown(s.line, "verifying against the server's checksum...");
            // Against the server's file as this download fetched it: its size and time.
            checked = verify::file(verify_request(s, id),
                                   verify::File{.path = path, .local = target, .size = result.size, .modtime = result.modtime},
                                   s.stop);
        }
        if (s.stop.stop_requested()) {
            s.line.finish("stopped while verifying: the file is complete, but not verified");
            return proc::exit_stopped;
        }
        switch (checked.files.front().verdict) {
        case verify::Verdict::differs:
        case verify::Verdict::missing:
            if (checked.files.front().verdict == verify::Verdict::differs) {
                std::filesystem::remove(target, ec); // not the server's file
                s.line.clear();
                std::println(stderr, "grab: error: the download {}; it was deleted", verify::summary(checked));
            } else {
                s.line.clear();
                std::println(stderr, "grab: error: {}", verify::summary(checked));
            }
            std::println(stderr, "grab: run the same command again to download it again");
            return exit_download_failed;
        case verify::Verdict::unchecked: s.line.finish(verify::summary(checked)); return 0;
        case verify::Verdict::same: break;
        }
        s.line.finish(std::format("{} in {}", verify::summary(checked), progress::duration(seconds_since(check_start))));
        return 0;
    }
    case fetch::Outcome::stopped: {
        const auto p = fetch::saved_progress(target, path, {}, rclone_remote);
        s.line.finish(p && p->second > 0
                          ? std::format("stopped at {}%; run the same command again to continue", p->first * 100 / p->second)
                          : "stopped");
        return proc::exit_stopped;
    }
    case fetch::Outcome::failed:
    case fetch::Outcome::exists:
        break;
    }
    s.line.clear();
    std::println(stderr, "grab: error: {}", result.error);
    if (fetch::saved_progress(target, path, {}, rclone_remote)) {
        std::println(stderr, "grab: what was fetched is kept; run the same command again to continue");
    }
    return exit_download_failed;
}

// `local_name`: the folder's name in DEST when another pick has the server's (else empty).
int fetch_folder(const Session& s, const std::string& path, int id, const std::filesystem::path& local_name) {
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
    job.local_name = local_name;
    job.batch_limit = [&s] { return s.limiter.rate(); }; // one download at a time: all of it
    job.files_at_once = [&s] { return s.files_at_once; }; // the window's "At once"
    job.extra = s.opts.extra;
    job.on_command = s.trace;
    job.pool = s.pool;
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
    {
        // Folders inside it the server wouldn't list: the rest is fetched (review G55).
        std::lock_guard lock(m);
        for (const auto& u : tree.unreadable) s.line.message("grab: couldn't read the folder " + u);
        // Files that have another file's name on Windows ("README" and "readme"), numbered (G25).
        for (const auto& f : tree.files) {
            if (f.local.empty()) continue;
            s.line.message(std::format("grab: {} is saved as \"{}\": another file of the folder has its name on Windows",
                                       f.path, util::path_to_utf8(f.local.filename())));
        }
    }
    folder::mark_existing(job, tree, m);
    // Different files already where some of the folder's go: the answer first, for this folder's
    // files as listed here, not one given for another pick (review G105).
    job.existing = s.for_rest->value_or(existing::Choice::ask);
    if (const auto found = folder::conflicts(job, tree, m); !found.empty()) {
        if (job.existing == existing::Choice::ask) {
            s.line.clear();
            const std::string folder_name = util::path_to_utf8(folder::job_dir(job).filename());
            std::vector<ExistingFile> files;
            for (const auto& c : found) {
                if (files.size() == 10) break;
                files.push_back({folder_name + "/" + c.path, c.local, c.size, util::parse_rfc3339_seconds(c.modtime)});
            }
            const auto answer = choose_existing(s.opts, files, found.size(), true, s.several);
            if (!answer) return answer.error();
            job.existing = answer->choice;
            if (answer->for_all) *s.for_rest = answer->choice;
        }
        std::lock_guard lock(m);
        folder::apply(job.existing, found, folder::job_dir(job), tree);
    }
    std::uint64_t before = 0;    // on disk already: finished files, partial big ones
    std::size_t already = 0;     // files that were complete
    std::uint64_t needed = 0;    // what is left to fetch
    {
        std::lock_guard lock(m);
        const auto t = folder::totals(tree);
        before = t.bytes;
        already = t.files_done;
        needed = t.total > t.bytes ? t.total - t.bytes : 0;
    }
    // Free space, now that the folder's size is known: before fetching anything.
    if (const auto free = space::free_bytes(s.dest); free && needed > *free) {
        s.line.clear();
        std::println(stderr, "grab: error: {}", space::shortage(space::drive_of(s.dest), needed, *free));
        return exit_download_failed;
    }
    folder::run(job, tree, m, s.stop);
    // --verify: what this run fetched, against the server's checksums. Copies that differ are
    // deleted and count as failed below.
    std::string checked_text;
    if (s.verify && !s.stop.stop_requested()) {
        const auto check_start = Clock::now();
        folder::Checked checked;
        {
            VerifyLine shown(s.line, "verifying against the server's checksums...");
            auto base = verify_request(s, id);
            base.on_progress = [&shown](std::size_t done, std::size_t total) { shown.files(done, total); };
            checked = folder::verify_files(job, tree, m, std::move(base), s.stop);
        }
        if (checked.files > 0 && !s.stop.stop_requested()) {
            checked_text = std::format("{} in {}", checked.summary(), progress::duration(seconds_since(check_start)));
        }
    }

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
    if (!checked_text.empty()) s.line.finish(checked_text);
    return 0;
}

// --dry-run: what would be fetched, without writing anything. `target`: where it would land.
void describe(const Session& s, const std::string& path, std::optional<std::uint64_t> size, int id,
              const std::string& position, int budget, const std::filesystem::path& target) {
    const auto local = util::path_to_utf8(target);
    std::string what;
    if (s.opts.mode == Mode::file) {
        if (size) {
            what = std::format(" ({}{})", util::format_size(*size),
                               existing::differs(target, *size, std::nullopt) ? "; a different file is there: asked first" : "");
        }
    } else {
        folder::Tree tree;
        std::mutex m;
        folder::Job job;
        job.ctx = &s.ctx;
        job.connections = &s.connections; // its walk takes them from the pool, as a download's
        job.server = s.ctx.settings.name;
        job.remote_path = path;
        job.dest = s.dest;
        if (target != folder::local_dir(s.dest, path)) job.local_name = target.filename();
        job.extra = s.opts.extra;
        job.on_command = s.trace;
        job.id = id;
        job.pool = s.pool;
        if (auto listed = folder::list(job, tree, m, s.stop); !listed) {
            what = std::format(" (cannot list it: {})", listed.error());
        } else {
            folder::mark_existing(job, tree, m);
            const auto t = folder::totals(tree);
            what = std::format(" ({} file{}, {}", t.files, t.files == 1 ? "" : "s", util::format_size(t.total));
            if (t.files_done > 0) what += std::format("; {} already there", t.files_done);
            if (const auto n = std::ranges::count_if(tree.files, [](const folder::File& f) { return !f.local.empty(); }); n > 0) {
                what += std::format("; {} numbered: another file has its name on Windows", n);
            }
            if (const auto n = folder::conflicts(job, tree, m).size(); n > 0) {
                what += std::format("; {} differ{} from yours: asked first", n, n == 1 ? "s" : "");
            }
            what += ")";
        }
    }
    std::println(stderr, "grab: would fetch {}{} {}{}  ->  {}  (up to {} connections)", mode_noun(s.opts.mode), position,
                 remote_spec(s.ctx.settings.rclone_remote, path), what, local, budget);
}

} // namespace

GuiState window_settings(const std::filesystem::path& gui_json) {
    auto saved = read_gui_state(gui_json);
    if (!saved) {
        std::println(stderr, "grab: {} can't be used ({}); going on without the window's settings (Verify downloads, "
                             "speed schedule, connection limits)", util::path_to_utf8(gui_json), saved.error());
        return {};
    }
    return saved->value_or(GuiState{});
}

std::optional<std::string> schedule_note(const std::vector<schedule::Rule>& rules, const schedule::Now& now) {
    const auto limit = schedule::limit_at(rules, now.minute);
    if (!limit) return std::nullopt;
    const auto next = schedule::next_change(rules, now.minute);
    return std::format("speed limited to {}{} by the speed schedule (--limit off ignores it)",
                       schedule::speed_text(*limit), next ? " until " + schedule::when_text(now.minute, next->minutes) : "");
}


std::expected<Answer, int> choose_existing(const Options& opts, const std::vector<ExistingFile>& files,
                                         std::size_t count, bool keep_both, bool offer_all) {
    if (opts.existing) {
        if (*opts.existing == existing::Choice::keep_both && !keep_both) {
            std::println(stderr, "grab: error: --existing both can't be used here: rclone copies a folder under the "
                                 "server's names; use replace or skip");
            return std::unexpected(exit_aborted);
        }
        return Answer{.choice = *opts.existing, .for_all = true};
    }
    const bool unsure = std::ranges::any_of(files, [](const ExistingFile& f) { return !f.uncompared.empty(); });
    std::println(stderr, "grab: {} already there {} from the server's:",
                 count == 1 ? std::string("1 file") : std::format("{} files", count),
                 unsure ? "may differ" : count == 1 ? "differs" : "differ");
    for (const auto& f : files) std::println(stderr, "  {}", existing_line(f));
    if (count > files.size()) std::println(stderr, "  ... and {} more", count - files.size());
    if (!util::stdin_is_tty()) {
        std::println(stderr, "grab: error: nothing was replaced; to say what to do without being asked, add --existing "
                             "replace, {}skip",
                     keep_both ? "both or " : "");
        return std::unexpected(exit_download_failed);
    }
    for (;;) {
        std::print(stderr, "{}{}", keep_both ? "[r]eplace, [k]eep both, [s]kip, [q]uit" : "[r]eplace, [s]kip, [q]uit",
                   !offer_all ? ": " : keep_both ? " (R, K, S: the same for the rest): " : " (R, S: the same for the rest): ");
        std::string line;
        if (!std::getline(std::cin, line)) return std::unexpected(exit_aborted);
        const std::string typed(util::trim(line));
        // A capital letter answers every later question of this run too.
        if (offer_all && typed == "R") return Answer{.choice = existing::Choice::replace, .for_all = true};
        if (offer_all && keep_both && typed == "K") return Answer{.choice = existing::Choice::keep_both, .for_all = true};
        if (offer_all && typed == "S") return Answer{.choice = existing::Choice::skip, .for_all = true};
        const auto answer = util::to_lower(typed);
        if (answer == "r" || answer == "replace") return Answer{.choice = existing::Choice::replace};
        if (keep_both && (answer == "k" || answer == "keep both" || answer == "both")) {
            return Answer{.choice = existing::Choice::keep_both};
        }
        if (answer == "s" || answer == "skip") return Answer{.choice = existing::Choice::skip};
        if (answer == "q" || answer == "quit") return std::unexpected(exit_aborted);
    }
}

DownloadTally download_with_grab(const engine::Context& ctx, const Options& opts, const std::vector<std::string>& paths,
                                 const std::unordered_map<std::string, std::uint64_t>& sizes,
                                 const std::unordered_map<std::string, std::int64_t>& modified,
                                 const std::filesystem::path& dest, ssh::Pools& pools) {
    LiveLine line;
    const std::string& server = ctx.settings.name;
    const auto config = opts.config.value_or(default_grab_config_path());
    const auto gui = window_settings(state_dir(config) / "gui.json");
    // The connection limits learned from refusals, by the window or an earlier command; what this
    // run learns is saved there too (review G195).
    const auto limits_file = learned_limits_path(config);
    const auto limits = load_learned_limits(limits_file, gui.server_limits);
    fetch::Connections connections([&](const std::string& name, int budget) {
        line.message(std::format("grab: {} refused a connection; using {} from now on", name, budget));
        if (auto saved = save_learned_limit(limits_file, name, budget, gui.server_limits); !saved) {
            line.message("grab: " + saved.error());
        }
    });
    std::optional<int> learned;
    if (const auto it = limits.find(server); it != limits.end()) learned = it->second;
    const int default_budget = budget::default_budget(ctx.remote.host);
    const int budget = budget::effective_budget(ctx.settings.max_connections, default_budget, learned);
    connections.configure(server, budget);
    rate::RateLimiter limiter;
    if (opts.limit) limiter.set_rate(*opts.limit);
    // Without --limit, the speed schedule set in grab's window decides, also mid-download.
    const auto rules = opts.limit ? std::vector<schedule::Rule>{} : gui.speed_schedule;
    if (auto note = schedule_note(rules, schedule::local_now())) std::println(stderr, "grab: {}", *note);
    std::optional<ScheduleFollower> follower;
    if (!opts.dry_run && !rules.empty()) follower.emplace(rules, limiter, line);
    const bool verify = opts.verify.value_or(gui.verify_downloads);
    if (verify) {
        std::println(stderr, "grab: downloads are checked against the server's checksum afterwards{}",
                     opts.verify ? "" : " (the window's setting; --no-verify skips it)");
    }

    StopOnCtrlC ctrl_c;
    // grab's engine: only the connection flags of the configured ones and of `--` (rclone_flags.hpp;
    // cli.cpp refuses any other after `--`).
    Session s{ctx, opts, dest, line, connections, limiter, rclone_flags::connection(ctx.settings.common_flags), {},
              ctrl_c.token()};
    if (follower) s.follower = &*follower;
    s.verify = verify;
    s.files_at_once = std::clamp(gui.parallel, min_parallel, max_parallel);
    std::map<std::string, existing::Choice> answered;
    std::optional<existing::Choice> for_rest = opts.existing;
    s.answered = &answered;
    s.for_rest = &for_rest;
    s.several = paths.size() > 1;
    const auto given = rclone_flags::connection(opts.extra);
    s.flags.insert(s.flags.end(), given.begin(), given.end());
    if (opts.verbose) {
        s.trace = [&line](const std::vector<std::string>& argv) { line.message("+ " + quote::display_cmdline(argv)); };
        std::println(stderr, "grab: up to {} connections to {}", budget, server);
    }

    DownloadTally tally;
    if (effective_engine(ctx.settings) == Engine::grab) {
        // The connection flags after `--` apply to grab's own connections too, as they did to
        // rclone's (`--bind`, `--contimeout`, `--timeout`).
        auto target = engine::ssh_target(ctx, given);
        if (!target) {
            std::println(stderr, "grab: {}", target.error());
            tally.code = exit_download_failed;
            tally.failed = paths;
            return tally;
        }
        s.pool = pools.get(server, *target, budget);
        if (opts.verbose) std::println(stderr, "grab: {} through grab's own SSH client (engine = grab)", server);
    }
    // Every server verifies through grab's own client (RCLONE-REPLACEMENT.md, phase 5); on engine =
    // rclone it logs in only to verify. Without a login the downloads go on, "not verified".
    if (verify && !opts.dry_run) {
        if (s.pool) {
            s.verify_pool = s.pool;
        } else if (auto target = engine::ssh_target(ctx, given)) {
            s.verify_pool = pools.get(server, *target, budget);
        } else {
            s.no_verify_pool = target.error();
        }
    }
    const std::size_t total = paths.size();
    const auto targets = existing::pick_targets(dest, paths, opts.mode == Mode::folder);
    // A picked file's size and time (nanoseconds), when the pick list had them.
    auto known = [&](const std::string& path) -> std::optional<std::pair<std::uint64_t, std::optional<std::int64_t>>> {
        const auto size = sizes.find(path);
        if (size == sizes.end()) return std::nullopt;
        const auto when = modified.find(path);
        return std::pair{size->second, when == modified.end() ? std::nullopt : std::optional(when->second * 1'000'000'000)};
    };
    // Different files already where picked files go: one question for all of them, before
    // fetching any (a folder asks once it is listed). The answer goes for the files listed.
    if (!opts.dry_run && opts.mode == Mode::file && !for_rest) {
        std::vector<ExistingFile> found;
        std::vector<std::string> listed;
        for (std::size_t i = 0; i < total; ++i) {
            const auto k = known(paths[i]);
            if (!k || !existing::differs(targets[i], k->first, k->second)) continue;
            listed.push_back(paths[i]);
            if (listed.size() <= 10) {
                const auto seconds = k->second ? std::optional(*k->second / 1'000'000'000) : std::nullopt;
                found.push_back({.name = util::path_to_utf8(targets[i].filename()), .local = targets[i], .size = k->first,
                                 .modified = seconds});
            }
        }
        if (!listed.empty()) {
            const auto answer = choose_existing(opts, found, listed.size(), true, total > listed.size());
            if (!answer) {
                tally.code = answer.error();
                tally.failed = paths;
                return tally;
            }
            for (const auto& path : listed) answered[path] = answer->choice;
            if (answer->for_all) for_rest = answer->choice;
        }
    }
    // Free space for the files picked, all together, before fetching any (a folder checks once
    // it is listed). Files to be skipped don't count (review G113).
    if (!opts.dry_run && opts.mode == Mode::file) {
        std::uint64_t needed = 0;
        for (std::size_t i = 0; i < total; ++i) {
            const auto k = known(paths[i]);
            if (!k) continue;
            const auto it = answered.find(paths[i]);
            const auto choice = it != answered.end() ? it->second : for_rest.value_or(existing::Choice::ask);
            if (choice == existing::Choice::skip && existing::differs(targets[i], k->first, k->second)) continue;
            needed += space::still_needed(targets[i], paths[i], k->first);
        }
        if (const auto free = space::free_bytes(dest); free && needed > *free) {
            std::println(stderr, "grab: error: {}", space::shortage(space::drive_of(dest), needed, *free));
            tally.code = exit_download_failed;
            tally.failed = paths;
            return tally;
        }
    }
    for (std::size_t i = 0; i < total; ++i) {
        const std::string& path = paths[i];
        const int id = static_cast<int>(i) + 1;
        const std::string position = total > 1 ? std::format(" {}/{}", i + 1, total) : "";
        // Another pick has its name: a numbered one instead (pick_targets).
        const bool renamed = targets[i] != (opts.mode == Mode::folder ? folder::local_dir(dest, path)
                                                                      : dest / local::relative(remote_basename(path)));
        if (opts.dry_run) {
            const auto it = sizes.find(path);
            describe(s, path, it == sizes.end() ? std::nullopt : std::optional(it->second), id, position, budget,
                     targets[i]);
            continue;
        }
        std::println(stderr, "grab: {}{} {}  ->  {}{}", mode_noun(opts.mode), position,
                     remote_spec(ctx.settings.rclone_remote, path), util::path_to_utf8(targets[i]),
                     renamed ? "  (another pick has that name)" : "");
        const int code = opts.mode == Mode::file
                             ? fetch_file(s, path, id, targets[i])
                             : fetch_folder(s, path, id, renamed ? targets[i].filename() : std::filesystem::path());
        if (code == 0) {
            ++tally.done;
            continue;
        }
        if (tally.code == 0) tally.code = code;
        tally.failed.push_back(path);
        // Ctrl+C, or [q]uit at a question: the picks after it don't start (review G106).
        if (code == proc::exit_stopped || code == exit_aborted) {
            tally.stopped = code == proc::exit_stopped;
            tally.quit = code == exit_aborted;
            tally.not_started = total - i - 1;
            break;
        }
    }
    // Downloads that ended without a refusal: a learned limit goes up by one, so a drop that
    // wasn't the server's real limit wears off (review G10).
    if (learned && tally.done > 0 && connections.refusals(server) == 0) {
        if (auto saved = save_learned_limit(limits_file, server, budget::raised(*learned, default_budget), gui.server_limits);
            !saved) {
            std::println(stderr, "grab: {}", saved.error());
        }
    }
    return tally;
}

} // namespace grab
