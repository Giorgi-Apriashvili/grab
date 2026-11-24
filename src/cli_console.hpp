#pragma once

// The grab command's console, shared by its downloads (cli_download.hpp) and uploads
// (cli_upload.hpp): the progress line redrawn in place, Ctrl+C that stops a transfer cleanly, the
// speed schedule followed while it runs, the lines while a verification or a folder runs, and the
// window's settings the command follows too.

#include "gui_state.hpp"
#include "progress_line.hpp"
#include "rate.hpp"
#include "schedule.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace grab {

namespace folder {
struct Tree;
} // namespace folder

// The transfer's progress on `out` (stderr): one line redrawn in place on a console. When it is a
// file or a pipe there is no live line, only the lines that end a transfer. Thread-safe.
class LiveLine {
public:
    explicit LiveLine(std::FILE* out = stderr);
    LiveLine(const LiveLine&) = delete;
    LiveLine& operator=(const LiveLine&) = delete;

    // Redraws the line, at most every 250 ms unless `now`.
    void show(const std::string& text, bool now = false);
    // Replaces the live line with `text`, kept as a normal line.
    void finish(const std::string& text);
    // Removes the live line, e.g. before an error message.
    void clear();
    // A line above the live one (a command line with -v, a note); the next show() redraws.
    void message(const std::string& text);
    // A question's prompt, the answer typed after it on the same line.
    void prompt(const std::string& text);

private:
    [[nodiscard]] std::size_t width() const;
    static std::size_t columns(std::string_view s);
    void draw(const std::string& text);
    void erase();

    std::mutex m_;
    std::FILE* out_;
    bool console_ = false;
    void* handle_ = nullptr; // the console's (Windows)
    std::size_t shown_ = 0;  // columns of the live line on screen
    std::chrono::steady_clock::time_point last_{};
};

// While alive, Ctrl+C stops the transfers (its token) instead of ending grab, so partial files
// and their progress stay for the next run. A second Ctrl+C ends grab at once.
class StopOnCtrlC {
public:
    StopOnCtrlC();
    ~StopOnCtrlC();
    StopOnCtrlC(const StopOnCtrlC&) = delete;
    StopOnCtrlC& operator=(const StopOnCtrlC&) = delete;

    [[nodiscard]] std::stop_token token() const { return stop_.get_token(); }

private:
    std::stop_source stop_;
#ifndef _WIN32
    std::jthread watcher_;
#endif
};

// Follows the speed schedule's limits for `dir` (downloads ↓, or uploads ↑) while they run: when
// the scheduled limit changes, the limiter switches, and grab's streams follow at once.
class ScheduleFollower {
public:
    ScheduleFollower(std::vector<schedule::Rule> rules, schedule::Dir dir, rate::RateLimiter& limiter, LiveLine& line);
    ScheduleFollower(const ScheduleFollower&) = delete;
    ScheduleFollower& operator=(const ScheduleFollower&) = delete;

private:
    void run(const std::stop_token& stop);

    const std::vector<schedule::Rule> rules_;
    const schedule::Dir dir_;
    rate::RateLimiter& limiter_;
    LiveLine& line_;
    std::optional<double> current_; // the follower thread's
    std::jthread thread_; // last: stops and joins first
};

// The live line while a verification runs: its text, a folder's files so far ("12 of 40"), and
// after 5 s the time taken, redrawn every second, as md5sum says nothing until it ends.
class VerifyLine {
public:
    VerifyLine(LiveLine& line, std::string text);
    VerifyLine(const VerifyLine&) = delete;
    VerifyLine& operator=(const VerifyLine&) = delete;
    ~VerifyLine() = default;

    void files(std::size_t done, std::size_t total);

private:
    void draw(bool now);

    LiveLine& line_;
    const std::string text_;
    const std::chrono::steady_clock::time_point start_;
    std::mutex m_; // files_
    std::string files_;
    std::jthread thread_; // last: stops and joins first
};

// The live line while a folder's files go: its files, bytes and connections, and the speed and
// time left of the bytes that move (`moved`: those that came, or those the server acknowledged;
// progress::FolderMeter), redrawn every 250 ms, also while none move. `prefix` before it: "↑ "
// for an upload.
class FolderLine {
public:
    FolderLine(LiveLine& line, const folder::Tree& tree, std::mutex& m, rate::RateLimiter& limiter,
               const std::atomic<std::uint64_t>& moved, std::string prefix = {});
    FolderLine(const FolderLine&) = delete;
    FolderLine& operator=(const FolderLine&) = delete;
    ~FolderLine() = default;

private:
    void draw();

    LiveLine& line_;
    const folder::Tree& tree_;
    std::mutex& m_; // tree_
    rate::RateLimiter& limiter_;
    const std::atomic<std::uint64_t>& moved_;
    const std::string prefix_;
    progress::FolderMeter meter_; // draw()'s: the constructor's, then the thread's
    std::jthread thread_;         // last: stops and joins first
};

// The window's settings at `gui_json` that the command follows too (Verify downloads, the speed
// schedule, learned connection limits, what an upload does with a file already there): the
// defaults when there are none, and a warning on stderr when the file can't be read; the file is
// the window's and is left as it is (G213).
[[nodiscard]] GuiState window_settings(const std::filesystem::path& gui_json);

// "speed limited to 5 MiB/s until 24:00 by the speed schedule (...)" when the speed schedule set
// in grab's window limits `dir`'s speed now ("upload speed limited to ..." for uploads).
[[nodiscard]] std::optional<std::string> schedule_note(const std::vector<schedule::Rule>& rules,
                                                       const schedule::Now& now, schedule::Dir dir);

} // namespace grab
