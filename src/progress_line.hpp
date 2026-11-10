#pragma once

// The text of the grab command's download progress: one line redrawn in place while a file or
// folder downloads, and the line that replaces it at the end, with a folder's speed and time left
// on it (FolderMeter). Pure; the console handling is in cli_download.cpp.

#include "speed.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace grab::progress {

// "0:26", "12:03", "1:02:03"; negative or non-finite times read as 0.
[[nodiscard]] std::string duration(double seconds);

struct Status {
    std::uint64_t bytes = 0; // on disk, including earlier runs
    std::uint64_t total = 0;
    double speed = 0;        // bytes per second; 0 = not known yet
    std::optional<double> eta;
    int connections = 0;
    // Folders: finished files and all files (skipped ones excluded).
    std::optional<std::size_t> files_done;
    std::optional<std::size_t> files;
};

// "4/13 files  212.0 MiB / 597.0 MiB  36%  11.2 MiB/s  12 conns  ETA 0:34"; the file count only
// for folders, speed and ETA once known.
[[nodiscard]] std::string line(const Status& s);

// A folder's speed and time left on its line, measured as the window measures a download's
// (App::meter, speed.hpp): the speed is the recent average of the bytes that came, its small
// files' and big files' alike (the small ones have no speed of their own), and the time left comes
// from its pace in files and bytes, counted down. Fed at each redraw, every 250 ms; with the time
// passed in.
class FolderMeter {
public:
    struct Counts {
        std::uint64_t received = 0;   // bytes come so far (folder::Job::on_received)
        std::uint64_t finished = 0;   // files fetched so far, done or failed
        std::uint64_t left_files = 0; // files still to come: queued or running
        std::uint64_t left_bytes = 0; // and their bytes not here yet
        // How many times the speed limit changed (rate::RateLimiter::generation): a change starts
        // the speed again from it, so a new limit shows within about a second rather than fading
        // in over five (speed::Meter::restart); the time left goes on, its pace started again from
        // the new speed (speed::Pace::restart, speed::Countdown::hold).
        std::uint64_t limit_changes = 0;
        // The limit then, bytes per second (0: none): one below the speed is the speed from the
        // change on (speed::Meter::foreseen).
        std::uint64_t limit = 0;
    };
    void sample(speed::Clock::time_point now, const Counts& c);
    // Bytes per second; 0 until the bytes have come for about half a second.
    [[nodiscard]] double speed() const { return speed_; }
    // Seconds; nullopt until the bytes have come for 5 s, or while none come.
    [[nodiscard]] std::optional<double> eta() const { return eta_; }

private:
    speed::Meter meter_;
    speed::Pace pace_;
    speed::Countdown countdown_;
    std::optional<std::uint64_t> limit_changes_;
    double speed_ = 0;
    std::optional<double> eta_;
};

// The end of a finished download. `fetched` is what this run downloaded, which is less than
// `total` when it continued an earlier run or a folder's files were already there.
// "done: 898.0 MiB in 0:48 (18.7 MiB/s)"
// "done: 898.0 MiB in 0:20 (412.0 MiB of it now, 20.6 MiB/s)"
// "done: 13 files, 597.0 MiB in 0:53 (11.2 MiB/s)"
[[nodiscard]] std::string done(std::uint64_t total, std::uint64_t fetched, double seconds,
                               std::optional<std::size_t> files = std::nullopt);

// The end of a folder download in which files failed, each line ending in '\n': what verification
// found of the files that came first, as a finished line (`checked`; none when it didn't run), then
// `failed` (folder::failed_text) and what to do.
// "  13 files verified (MD5) in 0:02"
// "grab: error: 2 files failed. a.bin: connection lost"
// "grab: the other files are on disk; run the same command again to retry"
[[nodiscard]] std::string folder_failed(std::string_view failed, std::string_view checked);

// `text` cut to `width` columns (UTF-8 aware, one column per code point), ending in "…" when
// cut; for redrawing in place, a line must not wrap.
[[nodiscard]] std::string fit(std::string_view text, std::size_t width);

} // namespace grab::progress
