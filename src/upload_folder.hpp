#pragma once

// A folder upload (UPLOADS.md 5.3, 5.6, 5.8): a local folder into a server folder of its name,
// merged into one that is there already. Its files are scheduled as a folder download's are
// (folder_sched.hpp): files from small_limit up through upload::run, resumable, as many at once as
// "At once" says, their ranges spread over the folder's connections; the smaller ones beside them
// on every connection the server's budget gives the folder, up to 32 files and 8 MiB in flight on
// each, each into a temp file of its own that takes its name only when complete (OPEN, WRITEs,
// FSETSTAT, CLOSE, then RENAME, overlapped across the files). The server's folders are made first,
// top-down; a folder that can't be made fails the files below it with the reason, and the rest
// goes on. Nothing on the server is ever replaced but what the existing-file choice says, server
// folders are never removed but those this upload made (a cancel, once empty), and no local file
// is ever changed or deleted.
//
// The caller owns the upload's state (Tree) and the mutex guarding it, so it can show the files
// and act on single ones while the upload runs: pause, resume, retry and priority as folder_job.hpp
// has them, skip through skip_file here (a big file's partial upload is on the server).

#include "engine.hpp"
#include "fetch.hpp"
#include "folder_job.hpp"
#include "rate.hpp"
#include "upload_existing.hpp"
#include "verify.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace grab::ssh {
class Pool;
} // namespace grab::ssh

namespace grab::upload {

// A folder upload's state, under the caller's mutex: its files as a folder download's (whose
// File::path is the file's path below the folder in grab's form, as local_walk gives it, File::
// modtime the local file's time, and File::local a Keep-both name's path in the same form), and
// what an upload adds.
struct Tree : folder::Tree {
    std::vector<std::string> folders; // below the folder, grab's form, each after its own (local_walk)
    std::size_t links = 0;            // links and junctions below the folder, left out
    std::size_t partials = 0;         // grab's own partial downloads in it, left out
    // What look() found on the server: whether the folder is there (to be merged into), and the
    // files whose names are taken there, by their index in `files`.
    bool looked = false;
    bool merging = false;
    std::map<std::size_t, Clash> clashes;
    std::set<std::string> on_server; // every path below the folder there (files and folders): Keep both's numbers
    std::set<std::string> server_dirs; // those of them that are folders: run() makes no MKDIR for them
    // A file's own existing-file choice, instead of the upload's (FolderJob::existing).
    std::map<std::size_t, Existing> choices;
    // Files whose copy on the server verify found different (verify_files): sent again over that
    // copy, grab's own, wherever it went (File::local for Keep both), whatever the choices say;
    // apply() leaves them alone. A file leaves the set once sent.
    std::set<std::size_t> resend;
    // Files whose name is taken on the server while the choice is to ask: failed until the
    // window asks (UPLOADS.md 5.5).
    std::set<std::size_t> asking;
    // The server folders this upload made, below the folder's place ("" the folder itself), in the
    // order made: a cancel removes those that are empty (discard).
    std::vector<std::string> made;
    // Temp files of grab's left on the server (grab's form): a stopped small file's, or one that
    // couldn't be removed. Removed at the next run's start and its end, or by a cancel.
    std::set<std::string> temps;
    // Small files whose temp was being put in place (its RENAME on its way) when the upload
    // stopped or the connection broke, and where to (grab's form): the server may have done it.
    // The next try looks there first, and a file of theirs (its size and time) is done.
    std::map<std::size_t, std::string> renaming;
    // Small files whose old copy Replace removed (a server without posix-rename) but whose new one
    // couldn't take the name: the temp holding it (grab's form), never removed while the file is
    // failed (its error names it); once the file is sent again it is a plain leftover temp.
    std::map<std::size_t, std::string> kept;
};

struct FolderJob {
    fetch::Connections* connections = nullptr; // the server's pool; look() and run() need it
    // How many of its files it may send at the same time now: "At once", less what other transfers
    // hold; at least 1. Called without the caller's mutex. Unset: 4.
    std::function<int()> files_at_once;
    rate::RateLimiter* limiter = nullptr; // uploads' own limit; nullptr: unlimited
    std::string server;                   // grab.conf section: the connection pool, and its name in messages
    int id = 0;                           // the upload's id in the pool and the limiter
    const std::atomic<int>* weight = nullptr; // its priority against other transfers; nullptr = normal
    std::filesystem::path source;         // the local folder
    // The server folder it becomes, in grab's form (upload::remote_child of the folder it goes into
    // and the local folder's name). The folder it is in must be there.
    std::string remote_path;
    Existing existing = Existing::ask; // what to do with a file already there (Tree::choices for one)
    engine::CommandHook on_command;    // sees each step before it is taken
    std::function<void(folder::Event)> on_event; // called without the mutex held
    // Told the bytes of each WRITE the server acknowledged, from the thread that saw it, without
    // the mutex: the folder's speed is what the server took.
    std::function<void(std::uint64_t)> on_sent;
    // Tests: called on the small files' thread before their workers start.
    std::function<void()> on_small_start;
    std::shared_ptr<ssh::Pool> pool;
    // Where the records of partial big files are kept (upload::Upload::records_dir); empty: none.
    std::filesystem::path records_dir;
};

// The local folder into the tree, unless it is listed already: its files and folders
// (local_walk.hpp), links and grab's own partial downloads left out and counted, the folders
// that couldn't be read noted (Tree::unreadable). A stop returns early with the tree unlisted.
[[nodiscard]] std::expected<void, std::string> walk(const FolderJob& job, Tree& tree, std::mutex& m, std::stop_token stop);

// The server's side, the dialog's look before an upload: whether the folder is there (merged into)
// or will be made, and the files whose names are taken there (Tree::clashes; a folder in a file's
// way too), walked through the seats a listing takes (fetch::Listing). An error when the folder it
// goes into isn't there, or a file has the folder's name.
[[nodiscard]] std::expected<void, std::string> look(const FolderJob& job, Tree& tree, std::mutex& m, std::stop_token stop);

// Each clash's choice (the file's own, else the upload's) applied to the queued files: Skip leaves
// a file out, Keep both gives it the first numbered name nothing on the server or in the folder
// has (File::local), Replace (if different, if newer: when it is) sends it, and Ask fails it until
// asked (Tree::asking). Under the caller's mutex.
void apply(const FolderJob& job, Tree& tree);

// The server folders, then the queued files, until none is left or `stop`. Looks at the server
// first (look, apply) when that wasn't done. Files still running at a stop are queued again; their
// temps on the server are kept (Tree::temps) and removed at the next run. A refusal that holds for
// the whole server (read-only, full) fails every queued file with it, one for a folder (no write
// access) the queued files that go there. At the end, unless stopped, the partial uploads of big
// files skipped meanwhile go (upload::discard).
void run(const FolderJob& job, Tree& tree, std::mutex& m, std::stop_token stop);

// Skip for a file of an upload (folder::skip_file), under the caller's mutex: a queued, paused or
// failed big file's target, whose partial upload the caller removes once the mutex is released
// (upload::discard with the job's pool, records_dir and server; a cancel's leftovers when it
// can't be reached). A running big file is stopped and its partial upload goes by itself.
// `skipped`: whether it changed anything.
std::optional<std::string> skip_file(const FolderJob& job, Tree& tree, std::size_t index, bool* skipped = nullptr);

// After run(): the files this upload sent, and has not checked yet, against the server's checksums
// (verify::run for uploads). A proven mismatch fails the file "the server's copy differs (MD5)",
// and its retry replaces that copy (Tree::resend); the local file is never touched. `base` gives
// the server's pool and the caller's hooks, as folder::verify_files.
[[nodiscard]] folder::Checked verify_files(const FolderJob& job, Tree& tree, std::mutex& m, verify::Request base,
                                           std::stop_token stop);

// What a cancel couldn't remove on the server, grab's form: kept for later (uploads.json).
struct Leftovers {
    std::vector<std::string> temps;    // temp files (upload::remove_temp)
    std::vector<std::string> partials; // big files' targets whose partial upload is still there (upload::discard)
    std::vector<std::string> folders;  // folders it made
    // Temps holding the only copy of a file whose old one Replace removed (Tree::kept): left for
    // the user, never removed.
    std::vector<std::string> kept;
    [[nodiscard]] bool empty() const { return temps.empty() && partials.empty() && folders.empty() && kept.empty(); }
};
// Cancel: the upload's temps on the server (its big files' by their records, the small files'
// Tree::temps), then the folders it made, deepest first, those that are empty (a folder with
// anything in it stays). Files sent stay, and so do the temps in Tree::kept (Leftovers::kept).
// What can't be reached or removed is returned.
[[nodiscard]] Leftovers discard(const FolderJob& job, Tree& tree, std::mutex& m, std::stop_token stop = {});

// What a cancel left to undo on `server`, tried again (the window's leftovers, at its start and
// after each cancel): the big files' partial uploads by their records (upload::discard), the temps
// (upload::remove_temp), then the folders, deepest first, each removed only when it is empty (one
// that isn't holds files that were sent, or someone else's: it stays, and is done with). Returns
// what is still to be undone: what the server couldn't be reached for, or refused. `kept` is
// never touched, and comes back as it was.
[[nodiscard]] Leftovers clean(ssh::Pool& pool, const std::filesystem::path& records_dir, std::string_view server,
                              Leftovers left, std::stop_token stop = {});

// Where a file of the folder goes on the server (grab's form): its path, or its Keep-both name.
[[nodiscard]] std::string target_of(const FolderJob& job, const folder::File& f);

// One uploaded file against the server's copy at `placed` (verify::file for uploads): the report,
// whose summary is "the server's copy differs (MD5)" for a proven mismatch. The local file is never
// touched. `size` and `modtime`: the local file's, as the upload set the server's copy's time.
[[nodiscard]] verify::Report verify_file(verify::Request base, const std::filesystem::path& local, const std::string& placed,
                                         std::uint64_t size, const std::string& modtime, std::stop_token stop);

} // namespace grab::upload
