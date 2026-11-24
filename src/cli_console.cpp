#include "cli_console.hpp"

#include "folder.hpp"
#include "folder_job.hpp"
#include "queue_view.hpp"
#include "util.hpp"

#include <algorithm>
#include <condition_variable>
#include <format>
#include <print>
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

// StopOnCtrlC's: the presses so far, and the live one's stop.
std::atomic<int> presses{0};
std::stop_source* stopping = nullptr;

#ifdef _WIN32
BOOL WINAPI on_ctrl_c(DWORD type) {
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;
    if (presses++ > 0 || stopping == nullptr) return FALSE; // the default: end grab
    stopping->request_stop(); // the transfer prints how far it got
    return TRUE;
}
#else
void on_ctrl_c(int) {
    if (presses++ > 0) {
        std::signal(SIGINT, SIG_DFL);
        std::raise(SIGINT);
    }
}
#endif

} // namespace

// ---- LiveLine ------------------------------------------------------------------------------------

LiveLine::LiveLine(std::FILE* out) : out_(out) {
    if (out_ != stderr) return; // a file: lines only
#ifdef _WIN32
    handle_ = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    console_ = handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE && GetConsoleMode(handle_, &mode) != 0;
#else
    console_ = isatty(STDERR_FILENO) != 0;
#endif
}

void LiveLine::show(const std::string& text, bool now) {
    if (!console_) return;
    std::lock_guard lock(m_);
    const auto t = Clock::now();
    if (!now && t - last_ < std::chrono::milliseconds(250)) return;
    last_ = t;
    draw(progress::fit("  " + text, width() - 1)); // the last column would wrap
}

void LiveLine::finish(const std::string& text) {
    std::lock_guard lock(m_);
    erase();
    std::println(out_, "  {}", text);
    std::fflush(out_);
}

void LiveLine::clear() {
    std::lock_guard lock(m_);
    erase();
    std::fflush(out_);
}

void LiveLine::message(const std::string& text) {
    std::lock_guard lock(m_);
    erase();
    std::println(out_, "{}", text);
    std::fflush(out_);
    last_ = {};
}

void LiveLine::prompt(const std::string& text) {
    std::lock_guard lock(m_);
    erase();
    std::print(out_, "{}", text);
    std::fflush(out_);
    last_ = {};
}

std::size_t LiveLine::width() const {
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

std::size_t LiveLine::columns(std::string_view s) {
    return static_cast<std::size_t>(std::ranges::count_if(s, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

void LiveLine::draw(const std::string& text) {
    const std::size_t cols = columns(text);
    std::string out = "\r" + text;
    if (cols < shown_) out.append(shown_ - cols, ' '); // over the rest of the previous line
    std::fputs(out.c_str(), out_);
    std::fflush(out_);
    shown_ = std::max(cols, shown_);
}

void LiveLine::erase() {
    if (!console_ || shown_ == 0) return;
    std::fputs(("\r" + std::string(shown_, ' ') + "\r").c_str(), out_);
    shown_ = 0;
}

// ---- StopOnCtrlC ---------------------------------------------------------------------------------

StopOnCtrlC::StopOnCtrlC() {
    stopping = &stop_;
    presses = 0;
#ifdef _WIN32
    // A parent can pass on "ignore Ctrl+C" (SetConsoleCtrlHandler(nullptr, TRUE) is
    // inherited), which would keep Ctrl+C from reaching the handler at all.
    SetConsoleCtrlHandler(nullptr, FALSE);
    SetConsoleCtrlHandler(&on_ctrl_c, TRUE);
#else
    std::signal(SIGINT, &on_ctrl_c);
    watcher_ = std::jthread([this](std::stop_token done) {
        while (!done.stop_requested()) {
            if (presses.load() > 0) stop_.request_stop();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
#endif
}

StopOnCtrlC::~StopOnCtrlC() {
#ifdef _WIN32
    SetConsoleCtrlHandler(&on_ctrl_c, FALSE);
#else
    std::signal(SIGINT, SIG_DFL);
#endif
    stopping = nullptr;
}

// ---- ScheduleFollower ----------------------------------------------------------------------------

ScheduleFollower::ScheduleFollower(std::vector<schedule::Rule> rules, schedule::Dir dir, rate::RateLimiter& limiter,
                                   LiveLine& line)
    : rules_(std::move(rules)), dir_(dir), limiter_(limiter), line_(line),
      current_(schedule::limit_at(rules_, schedule::local_now().minute, dir_)) {
    if (current_) limiter_.set_rate(schedule::bytes_per_s(*current_));
    thread_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
}

void ScheduleFollower::run(const std::stop_token& stop) {
    std::mutex sleep_m;
    std::condition_variable_any sleep;
    for (;;) {
        {
            std::unique_lock lock(sleep_m);
            // Just past each whole second, so a rule's minute applies within milliseconds.
            sleep.wait_for(lock, stop, schedule::to_next_second(), [] { return false; });
        }
        if (stop.stop_requested()) return;
        const auto now = schedule::local_now();
        const auto limit = schedule::limit_at(rules_, now.minute, dir_);
        if (limit == current_) continue;
        current_ = limit;
        limiter_.set_rate(limit ? schedule::bytes_per_s(*limit) : 0);
        const auto next = schedule::next_change(rules_, now.minute, dir_);
        const std::string until = next ? " until " + schedule::when_text(now.minute, next->minutes) : "";
        const bool up = dir_ == schedule::Dir::up;
        line_.message(limit ? std::format("grab: speed schedule: {}limited to {}{}", up ? "uploads " : "",
                                          schedule::speed_text(*limit), until)
                            : std::format("grab: speed schedule: no {}limit{}", up ? "upload " : "", until));
    }
}

// ---- VerifyLine ----------------------------------------------------------------------------------

VerifyLine::VerifyLine(LiveLine& line, std::string text) : line_(line), text_(std::move(text)), start_(Clock::now()) {
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

void VerifyLine::files(std::size_t done, std::size_t total) {
    {
        std::scoped_lock lock(m_);
        files_ = std::format(" {} of {}", done, total);
    }
    draw(false);
}

void VerifyLine::draw(bool now) {
    std::string text;
    {
        std::scoped_lock lock(m_);
        text = text_ + files_;
    }
    if (const double secs = seconds_since(start_); secs >= 5) text += std::format(" ({})", progress::duration(secs));
    line_.show(text, now);
}

// ---- FolderLine ----------------------------------------------------------------------------------

FolderLine::FolderLine(LiveLine& line, const folder::Tree& tree, std::mutex& m, rate::RateLimiter& limiter,
                       const std::atomic<std::uint64_t>& moved, std::string prefix)
    : line_(line), tree_(tree), m_(m), limiter_(limiter), moved_(moved), prefix_(std::move(prefix)) {
    draw();
    thread_ = std::jthread([this](const std::stop_token& stop) {
        std::mutex sleep_m;
        std::condition_variable_any sleep;
        for (auto next = Clock::now();;) {
            // A draw held up (a console in QuickEdit selection blocks writes) isn't caught up on.
            next = std::max(next + std::chrono::milliseconds(250), Clock::now());
            {
                std::unique_lock lock(sleep_m);
                sleep.wait_until(lock, stop, next, [] { return false; });
            }
            if (stop.stop_requested()) return;
            draw();
        }
    });
}

void FolderLine::draw() {
    queue_view::Pass p;
    int conns = 0;
    {
        std::scoped_lock lock(m_);
        p = queue_view::pass(tree_.files, nullptr);
        conns = tree_.connections;
    }
    const auto finished = p.counts[static_cast<std::size_t>(folder::State::done)] +
                          p.counts[static_cast<std::size_t>(folder::State::failed)];
    const auto limit = limiter_.setting(); // its rate and generation together
    meter_.sample(Clock::now(), {.received = moved_.load(),
                                 .finished = finished,
                                 .left_files = p.left_files,
                                 .left_bytes = p.left_bytes,
                                 .limit_changes = limit.generation,
                                 .limit = limit.rate});
    const auto& t = p.totals;
    line_.show(prefix_ + progress::line({.bytes = t.bytes,
                                         .total = t.total,
                                         .speed = meter_.speed(),
                                         .eta = meter_.eta(),
                                         .connections = conns,
                                         .files_done = t.files_done,
                                         .files = t.files}),
               true);
}

// ---- the window's settings -----------------------------------------------------------------------

GuiState window_settings(const std::filesystem::path& gui_json) {
    auto saved = read_gui_state(gui_json);
    if (!saved) {
        std::println(stderr, "grab: {} can't be used ({}); going on without the window's settings (Verify downloads, "
                             "speed schedule, connection limits)", util::path_to_utf8(gui_json), saved.error());
        return {};
    }
    return saved->value_or(GuiState{});
}

std::optional<std::string> schedule_note(const std::vector<schedule::Rule>& rules, const schedule::Now& now,
                                         schedule::Dir dir) {
    const auto limit = schedule::limit_at(rules, now.minute, dir);
    if (!limit) return std::nullopt;
    const auto next = schedule::next_change(rules, now.minute, dir);
    return std::format("{}speed limited to {}{} by the speed schedule (--limit off ignores it)",
                       dir == schedule::Dir::up ? "upload " : "", schedule::speed_text(*limit),
                       next ? " until " + schedule::when_text(now.minute, next->minutes) : "");
}

} // namespace grab
