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
#include "existing.hpp"
#include "fetch.hpp"
#include "folder.hpp"
#include "rate.hpp"
#include "verify.hpp"

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
    bool fetched = false;  // done by this download, not found on disk already
    bool verified = false; // checked against the server's checksum (verify_files)
    // Where it goes instead, relative to the folder ("e01 (2).mkv"): Keep both, or a name another
    // file of the folder has on Windows (settle_names); fetched by grab.
    std::filesystem::path local = {};
};
[[nodiscard]] File make_file(std::string path, std::uint64_t size, std::string modtime);
// A folder's files (sorted by path) that are one name on Windows ("README" and "readme", folders
// "Docs" and "docs") or to rclone's batch, which also takes "café" in its two Unicode spellings
// for one (review G25). rclone keeps one of them and drops the others, a folder with all it holds,
// so each of them is fetched by grab; and a file whose name an earlier file or any folder of the
// listing has on Windows gets a numbered one, as Keep both names them: "readme (2)".
void settle_names(std::vector<File>& files);

// A folder download's state. Every access holds the caller's mutex.
struct Tree {
    bool listed = false;
    std::vector<File> files;
    // The running small-file batch. Stopping it restarts it without paused or skipped files.
    std::stop_source batch_stop;
    bool batch_running = false;
    int connections = 0;   // open right now
    int files_at_once = 0; // files it downloads right now: the batch's at once, or its big files running
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
// A big file's partial download to delete once the caller's mutex is released: fetch::discard
// with these.
struct Discard {
    std::filesystem::path target;
    std::string remote_path;
    fetch::Reserved reserved;
};
// Where the folder's files land (dir: its local directory), which no partial download may take.
// Under the caller's mutex.
[[nodiscard]] fetch::Reserved reserved_names(const std::filesystem::path& dir, const Tree& tree);
// `remote_path`: the folder's. Returns the partial download to delete after unlocking, if any.
std::optional<Discard> skip_file(Tree& tree, const std::filesystem::path& dir, const std::string& remote_path,
                                 const std::string& path);
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

// Where a file of the folder goes (dir: the folder's local directory): its server name made
// safe, or its Keep-both name.
[[nodiscard]] std::filesystem::path target_of(const std::filesystem::path& dir, const File& f);

// The folder's own local directory: DEST\<folder name>, as rclone copy names it.
[[nodiscard]] std::filesystem::path local_dir(const std::filesystem::path& dest, const std::string& remote_path);
[[nodiscard]] std::string child_remote_path(const std::string& folder, const std::string& rel);
struct Job;
// A job's local directory: local_dir, or DEST\<job.local_name> when it has one.
[[nodiscard]] std::filesystem::path job_dir(const Job& job);

// Deletes what a cancelled folder (`remote_path`) leaves behind: its big files' partial
// downloads, grab's by their records. Finished files stay; rclone's temp files went when its
// batch stopped.
void discard_partials(const std::string& remote_path, const std::filesystem::path& dir, const Tree& tree,
                      std::string_view rclone_remote = {});

enum class Event {
    progress,   // bytes moved; frequent, so throttle what it triggers
    changed,    // a file started or ended, or the batch started
    checkpoint, // listed, or a pass over the queued files ended: worth saving
};

struct Job {
    const engine::Context* ctx = nullptr;
    fetch::Connections* connections = nullptr; // nullptr: list() walks with rclone's own (--rclone)
    // How many of its files it may download at the same time now: "At once", less what other
    // downloads hold; at least 1. Called without the caller's mutex. Unset: 4.
    std::function<int()> files_at_once;
    rate::RateLimiter* limiter = nullptr;      // nullptr: unlimited
    std::string server;                   // grab.conf section: the connection pool
    int id = 0;                           // the folder's id in the pool
    const std::atomic<int>* weight = nullptr; // bandwidth priority; nullptr = normal
    std::string remote_path;
    std::filesystem::path dest; // DEST: files land in DEST\<folder name>\...
    // The local folder's name instead of the server's, e.g. "Season 1 (2)" when another pick of
    // the same command has that name too; empty: the server's (local_dir).
    std::filesystem::path local_name;
    // Replace: different files already there may be replaced. Otherwise none is: conflicts were
    // answered before run() (skipped, or a second copy), and one that appears meanwhile fails.
    existing::Choice existing = existing::Choice::ask;
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

// A queued file whose place holds a different file (another size, or older than the server's).
struct Conflict {
    std::string path;            // relative to the folder
    std::filesystem::path local; // the file there
    std::uint64_t size = 0;      // the server's
    std::string modtime;
};
// After mark_existing: the queued files that would replace a different file.
[[nodiscard]] std::vector<Conflict> conflicts(const Job& job, Tree& tree, std::mutex& m);
// The answer, under the caller's mutex: Skip leaves those files out, Keep both gives each a
// second-copy name (fetched by grab itself: rclone can't rename), Replace changes nothing.
void apply(existing::Choice choice, const std::vector<Conflict>& found, const std::filesystem::path& dir, Tree& tree);
// Fetches the queued files until none is left or `stop`. Files still running at a stop are
// queued again, ready for the next run.
void run(const Job& job, Tree& tree, std::mutex& m, std::stop_token stop);

// After run(): checks the files this download fetched, and has not checked yet, against the
// server's checksums in one rclone check. Copies that differ are deleted and marked failed, so a
// retry fetches them again.
struct Checked {
    std::size_t files = 0; // checked; 0 when there was nothing to check
    verify::Report report;
    std::string error;     // the check could not run
    [[nodiscard]] std::string summary() const; // for the row and the command
};
[[nodiscard]] Checked verify_files(const Job& job, Tree& tree, std::mutex& m, std::stop_token stop);

} // namespace grab::folder
