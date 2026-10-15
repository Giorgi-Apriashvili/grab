#pragma once

// A folder download, shared by grab-gui's queue and the grab command. The folder is listed once
// (`rclone lsjson -R`), and files already on disk are not fetched again. Small files go in one
// `rclone copy` batch that reuses its connections, since a new SSH login per small file would
// dominate. Big ones take grab's resumable ranged download (fetch::run), up to three at a time.
// The whole folder is one user of its server's connection budget.
//
// The caller owns the folder's state (Tree) and the mutex guarding it, so it can show the
// files and act on single ones (pause, resume, skip) while the download runs.

#include "engine.hpp"
#include "fetch.hpp"
#include "folder.hpp"
#include "rate.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace grab::folder {

// Why a running big file is being stopped; the folder's own stop ends every file.
enum class StopReason { none, pause, skip };

struct File {
    std::string path; // relative to the folder, '/'-separated
    std::uint64_t size = 0;
    std::string modtime;
    bool big = false; // fetched like a single file (resumable); otherwise in the batch
    State status = State::queued;
    std::uint64_t bytes = 0;
    double speed = 0;
    std::optional<double> eta;
    std::string error;
    std::stop_source stop; // a running big file's own stop
    StopReason stop_reason = StopReason::none;
    bool in_batch = false; // in the running batch: queued until rclone starts it, then running
};
[[nodiscard]] File make_file(std::string path, std::uint64_t size, std::string modtime);

// A folder download's state. Every access holds the caller's mutex.
struct Tree {
    bool listed = false;
    std::vector<File> files;
    // The running small-file batch. Stopping it restarts it without paused or skipped files.
    std::stop_source batch_stop;
    bool batch_running = false;
    int connections = 0; // open right now
};

struct Totals {
    std::uint64_t bytes = 0; // finished files plus the progress of the others
    std::uint64_t total = 0; // skipped files count in neither
    double speed = 0;
    std::optional<double> eta;
    std::size_t files = 0; // not skipped
    std::size_t files_done = 0;
};
[[nodiscard]] Totals totals(const Tree& tree);

// Actions on single files, under the caller's mutex.
void pause_file(Tree& tree, const std::string& path);
void resume_file(Tree& tree, const std::string& path);
void resume_paused(Tree& tree); // every paused file
// Returns the file whose partial download to delete (fetch::discard) after unlocking, if any.
std::optional<std::filesystem::path> skip_file(Tree& tree, const std::filesystem::path& dir, const std::string& path);
// Failed files are queued again; with `all_unfinished` (after a cancel) every file not done or
// skipped.
void retry_files(Tree& tree, bool all_unfinished);

// How the files ended after run(), under the caller's mutex.
struct Outcome {
    std::size_t failed = 0;
    std::size_t waiting = 0; // queued or paused
    std::string first_error; // "<path>: <error>"
};
[[nodiscard]] Outcome outcome(const Tree& tree);

// The folder's own local directory: DEST\<folder name>, as rclone copy names it.
[[nodiscard]] std::filesystem::path local_dir(const std::filesystem::path& dest, const std::string& remote_path);
[[nodiscard]] std::string child_remote_path(const std::string& folder, const std::string& rel);

// Deletes what a cancelled folder leaves behind: its big files' partial downloads and rclone's
// .partial files. Finished files stay.
void discard_partials(const std::filesystem::path& dir, const Tree& tree);

enum class Event {
    progress,   // bytes moved; frequent, so throttle what it triggers
    changed,    // a file started or ended, or the batch started
    checkpoint, // listed, or a pass over the queued files ended: worth saving
};

struct Job {
    const engine::Context* ctx = nullptr;
    fetch::Connections* connections = nullptr;
    rate::RateLimiter* limiter = nullptr; // nullptr: unlimited
    std::string server;                   // grab.conf section: the connection pool
    int id = 0;                           // the folder's id in the pool
    const std::atomic<int>* weight = nullptr; // bandwidth priority; nullptr = normal
    std::string remote_path;
    std::filesystem::path dest; // DEST: files land in DEST\<folder name>\...
    // The small-file batch's share of the speed limit in bytes per second (0: none), asked
    // when a batch starts, without the mutex held.
    std::function<std::uint64_t()> batch_limit;
    std::vector<std::string> extra; // added to every rclone call
    engine::CommandHook on_command; // sees every rclone command line before it runs
    std::function<void(Event)> on_event; // called without the mutex held
};

// Lists the folder into the tree, unless it is listed already. A stop returns early with the
// tree still unlisted.
[[nodiscard]] std::expected<void, std::string> list(const Job& job, Tree& tree, std::mutex& m, std::stop_token stop);
// Marks queued files already complete on disk (fetch::up_to_date) as done; big ones with a
// partial download get its progress.
void mark_existing(const Job& job, Tree& tree, std::mutex& m);
// Fetches the queued files until none is left or `stop`. Files still running at a stop are
// queued again, ready for the next run.
void run(const Job& job, Tree& tree, std::mutex& m, std::stop_token stop);

} // namespace grab::folder
