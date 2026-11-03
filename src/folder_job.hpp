#pragma once

// A folder download, shared by grab-gui's queue and the grab command. The folder is walked once
// over SFTP on the server's pooled connections (Job::pool), and files already on disk are not
// fetched again. Files from small_limit up take grab's resumable ranged download (fetch::run) as
// many at once as "At once" says, their pieces spread over the folder's connections; smaller ones
// go beside them on every free connection, many in flight on each (RCLONE-REPLACEMENT.md, phase
// 3). The whole folder is one user of its server's connection budget.
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
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace grab::folder {

// Why a running big file is being stopped; the folder's own stop ends every file.
enum class StopReason { none, pause, skip };

// Files under this size go beside the At once files on any free connection, many in flight on
// each (the user's choice, 2026-10-27). One can't be split over connections, so
// counting them against At once would leave most connections idle in a folder of small files.
// From here up a file is fetched as a single download is (fetch::run): resumable, its pieces
// spread over the folder's connections, and At once counts it.
inline constexpr std::uint64_t small_limit = std::uint64_t{2} << 20;

struct File {
    std::string path; // relative to the folder, '/'-separated
    std::uint64_t size = 0;
    std::string modtime;
    bool big = false; // fetched like a single file (resumable, At once counts it); otherwise beside them
    State status = State::queued;
    std::uint64_t bytes = 0;
    double speed = 0;
    std::optional<double> eta;
    std::string error;
    std::stop_source stop; // a running big file's own stop
    StopReason stop_reason = StopReason::none;
    bool fetched = false;  // done by this download, not found on disk already
    bool verified = false; // checked against the server's checksum (verify_files)
    // Where it goes instead, relative to the folder ("e01 (2).mkv"): Keep both, or a name another
    // file of the folder has on Windows (settle_names).
    std::filesystem::path local = {};
};
[[nodiscard]] File make_file(std::string path, std::uint64_t size, std::string modtime);
// A file of a folder's files (sorted by path) whose name an earlier file or any folder of the
// listing has on Windows ("README" and "readme", folders "Docs" and "docs"; review G25) gets a
// numbered one, as Keep both names them: "readme (2)".
void settle_names(std::vector<File>& files);
// Which files take fetch::run (File::big): every file from small_limit up.
void set_big(std::vector<File>& files);

// A folder download's state. Every access holds the caller's mutex.
struct Tree {
    bool listed = false;
    std::vector<File> files;
    int connections = 0;   // open right now
    int files_at_once = 0; // its big files running
    // Folders below the folder that the walk couldn't read ("a/b: permission denied"): the rest is
    // downloaded and these are said (review G55).
    std::vector<std::string> unreadable;
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
// A folder whose files failed, as its row and the command say it: "2 files failed. <first
// error>", then what verification found of the files that came (`checked`, Checked::summary;
// empty when none ran), since a failed row shows only this.
[[nodiscard]] std::string failed_text(const Outcome& o, std::string_view checked = {});

// Where a file of the folder goes (dir: the folder's local directory): its server name made
// safe, or its Keep-both name.
[[nodiscard]] std::filesystem::path target_of(const std::filesystem::path& dir, const File& f);

// The folder's own local directory: DEST\<folder name> (local::relative of its name).
[[nodiscard]] std::filesystem::path local_dir(const std::filesystem::path& dest, const std::string& remote_path);
[[nodiscard]] std::string child_remote_path(const std::string& folder, const std::string& rel);
struct Job;
// A job's local directory: local_dir, or DEST\<job.local_name> when it has one.
[[nodiscard]] std::filesystem::path job_dir(const Job& job);

// Deletes what a cancelled folder (`remote_path`) leaves behind: its big files' partial
// downloads, grab's by their records. Finished files stay; a small file is only written whole.
void discard_partials(const std::string& remote_path, const std::filesystem::path& dir, const Tree& tree,
                      std::string_view rclone_remote = {});

enum class Event {
    progress,   // bytes moved; frequent, so throttle what it triggers
    changed,    // a file started or ended
    checkpoint, // listed, or a pass over the queued files ended: worth saving
};

struct Job {
    const engine::Context* ctx = nullptr;
    fetch::Connections* connections = nullptr; // the server's pool; list() and run() need it
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
    engine::CommandHook on_command; // sees each step before it is taken (the walk, a file's reads)
    std::function<void(Event)> on_event; // called without the mutex held
    // The server's pool of grab's own SSH connections, which the folder is walked and fetched
    // through. Needed, and so is `connections`.
    std::shared_ptr<ssh::Pool> pool;
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
// second-copy name, Replace changes nothing.
void apply(existing::Choice choice, const std::vector<Conflict>& found, const std::filesystem::path& dir, Tree& tree);
// Fetches the queued files until none is left or `stop`. Files still running at a stop are
// queued again, ready for the next run.
void run(const Job& job, Tree& tree, std::mutex& m, std::stop_token stop);

// After run(): checks the files this download fetched, and has not checked yet, against the
// server's checksums (verify::run), each by its own path, whatever its local name. A file is
// marked verified as soon as its result is known, so a stop keeps what was done. Copies proven to
// differ are deleted and marked failed, as are copies that are gone, so a retry fetches them again;
// the others stay done. `base` gives the server's pool (or why there is none) and the caller's
// hooks (on_progress, on_command when the job has none); the rest comes from the job.
struct Checked {
    std::size_t files = 0; // checked; 0 when there was nothing to check
    verify::Report report;
    [[nodiscard]] std::string summary() const; // for the row and the command
};
[[nodiscard]] Checked verify_files(const Job& job, Tree& tree, std::mutex& m, verify::Request base,
                                   std::stop_token stop);

} // namespace grab::folder
