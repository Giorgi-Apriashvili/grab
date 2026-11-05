#pragma once

#include "browse.hpp"
#include "cli.hpp"
#include "existing.hpp"
#include "fetch.hpp"
#include "file_list.hpp"
#include "folder_job.hpp"
#include "gui_state.hpp"
#include "json.hpp"
#include "play_stream.hpp"
#include "preview_jobs.hpp"
#include "queue_view.hpp"
#include "rate.hpp"
#include "schedule.hpp"
#include "server_ops.hpp"
#include "servers.hpp"
#include "ssh_pool.hpp"
#include "ssh_read.hpp"
#include "update.hpp"
#include "verify.hpp"
#include "webview.hpp"

#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <expected>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace grab::gui {

// The GUI's backend: answers the page's messages (see ui/app.js for the protocol), runs
// searches and the download queue on worker threads, and remembers GuiState.
class App {
public:
    struct Host {
        HWND hwnd = nullptr;
        std::function<void(std::string)> post;             // any thread: JSON to the page
        std::function<void(std::function<void()>)> defer;  // any thread: run on the UI thread
        // Worker thread: a download finished (ok) or failed (error says why); not called for
        // cancelled ones.
        std::function<void(const std::string& name, bool ok, const std::string& error)> on_finished;
        std::function<void()> quit; // UI thread: close the window and exit, even with downloads running
    };

    struct QueueSummary {
        int running = 0;
        int queued = 0;
        int paused = 0;
        std::uint64_t bytes = 0; // of the running and queued items with a known size
        std::uint64_t total = 0;
    };

    App(Host host, std::filesystem::path config_path, std::filesystem::path state_path);
    ~App(); // close(), then joins the threads
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // UI thread, as the window closes: what must outlive the program is saved first (the window's
    // settings, the queue: running downloads come back paused, or go on after an update's
    // restart), then every thread is told to stop and every server connection is cut
    // (ssh::close_all_connections), so the destructor's joins end within moments, not after a
    // login stuck on a server that doesn't answer. Once; the destructor calls it too.
    void close();
    // UI thread: `grab update` closes the window to replace grab-gui and starts it again: the
    // running and queued downloads go on there by themselves.
    void will_restart() { mark_for_restart(true); }

    // UI thread: one message from the page.
    void on_message(const std::string& json);

    [[nodiscard]] bool downloads_active() const;
    [[nodiscard]] QueueSummary summary() const;
    [[nodiscard]] GuiState& state() { return state_; }
    void save_state() const;
    void post_import_banner();
    // UI thread, every second: follows the speed schedule.
    void tick();
    // UI thread, every 250 ms, and at once after the user's actions: what changed in the queue
    // goes to the page (DOWNLOADS-UI.md 6.2). The downloads whole ("queue") when they came, went or
    // moved, else those whose row changed ("queueDelta"); the files of the folders the page shows
    // that changed ("fileDelta"), found in the same pass over a folder's files that counts its
    // totals; and the files of a shown folder that was just listed ("files"). Workers only mark
    // the queue as changed.
    void flush();
    // The page's https://grab.ui/preview/<remote>/<path>[?size=N&modified=T][&ahead] requests (the
    // viewer's images, text and PDFs; each part percent-encoded; the row's size and time, for the
    // cache; ahead: a neighbour fetched before it's shown): read through grab's own connections to
    // the server on a preview worker, or from the recent ones kept in memory. A request for a file
    // already being fetched joins that fetch. `reply` answers, from any thread.
    void preview_request(const std::string& url, std::function<void(WebResource)> reply);

    // Something the window may be waiting on, shown in the status bar while it runs: what grab is
    // doing (and the command running, on hover), with real progress where it can be counted, else
    // filled over the time the same step (`timing`, e.g. "lsjson:Box.Guest") took before. Any
    // thread; it ends when it goes out of scope, and its time is remembered unless it failed.
    class Activity {
    public:
        Activity(App& app, std::string text, std::string timing, bool background = false);
        ~Activity();
        Activity(const Activity&) = delete;
        Activity& operator=(const Activity&) = delete;
        void text(std::string text);
        // Counted progress with its message, in one update; the first one is sent at once.
        void progress(std::uint64_t done, std::uint64_t total, std::string text = {});
        void detail(std::string detail);
        // The step it times turned out to be another ("search:Main" became "find:Main" once the
        // way was chosen): its remembered time is that one's, and the bar's estimate follows.
        void timing(std::string key);
        void failed() { failed_ = true; }

    private:
        void send(bool force); // caller holds mutex_; at most ten updates a second

        App& app_;
        int id_ = 0;
        std::string timing_;
        std::chrono::steady_clock::time_point started_;
        std::mutex mutex_;
        std::string text_;
        std::string detail_;
        std::optional<std::uint64_t> done_;
        std::optional<std::uint64_t> total_;
        std::chrono::steady_clock::time_point sent_{};
        bool failed_ = false;
    };

private:
    // A video or audio file, in the program Windows plays it with: {remote, path}. On the Play
    // thread, where a new Play stops one still preparing (review G50).
    void play(const json::Value& msg);
    struct PlayJob {
        std::string remote;
        std::string path;
    };
    void run_plays(const std::stop_token& stop);
    // Through grab's own connections on every server (RCLONE-REPLACEMENT.md, phase 6): the warm
    // part first (the status bar shows it coming in), then the player.
    void run_play(const PlayJob& job, const std::stop_token& stop);

    // What a preview fetch holds while it runs and gives back once its answer went out: the reader
    // (its connections) and the viewer's seat on the server.
    struct PreviewHold {
        std::optional<ssh::Reader> reader;
        std::shared_ptr<ssh::Pool> pool;
        bool seated = false;
    };
    // A job from the queue, on a preview worker (preview::Previews: the cache again, the fetch,
    // every request that joined it answered), then its connections and seat given back.
    void run_preview(const std::shared_ptr<preview::Previews::Job>& job);
    // The fetch on grab's own connections, every server (RCLONE-REPLACEMENT.md, phase 6).
    [[nodiscard]] preview::Fetched preview_answer(const preview::Previews::Job& job, PreviewHold& hold);
    // The viewer's seat on `server`, which it keeps up to preview_seat_idle after a preview, so
    // flipping through images makes downloads step aside once, not for each image. A preview shown
    // takes it, else a free one, else waits for one as a download does (they step aside); a
    // neighbour fetched ahead takes it, else only a free one, waiting for either. False: stopped.
    bool take_viewer_seat(const std::string& server, const preview::Previews::Job& job, const std::stop_token& stop);
    void return_viewer_seat(const std::string& server);
    // Gives back the viewer's seats idle longer than preview_seat_idle. UI thread (tick).
    void release_viewer_seats();
    // A server saved, trusted or removed: its previews kept go (review G36) and the login the
    // previews remembered as failed is tried again (Pool::acquire_remembered).
    void forget_server_previews(const std::string& server);
    // A server removed, or moved to another user, host or port: also its pool (Pools::forget) and
    // what the window learned for it, its connection limit and timings (G122; server_ops drops
    // the saved ones).
    void forget_server(const std::string& server);
    // `first`: ahead of the jobs queued already (the preview shown now).
    void queue_preview(std::function<void()> job, bool first = false);
    void run_preview_jobs(const std::stop_token& stop);
    // A plaintext password or passphrase, overwritten when it is dropped.
    class Secret {
    public:
        Secret() = default;
        explicit Secret(std::string s) : s_(std::move(s)) {}
        ~Secret() { wipe(); }
        Secret(Secret&& o) noexcept : s_(o.s_) { o.wipe(); } // copy, then wipe: no stray bytes
        Secret& operator=(Secret&& o) noexcept {
            if (this != &o) {
                wipe();
                s_ = o.s_;
                o.wipe();
            }
            return *this;
        }
        Secret(const Secret&) = delete;
        Secret& operator=(const Secret&) = delete;
        [[nodiscard]] const std::string& value() const { return s_; }
        [[nodiscard]] bool empty() const { return s_.empty(); }
        void wipe() {
            SecureZeroMemory(s_.data(), s_.size());
            s_.clear();
        }

    private:
        std::string s_;
    };

    // A server change waiting for the user to trust the host key.
    struct Pending {
        enum class Kind { add, edit, trust } kind = Kind::add;
        servers::NewServer server;
        Secret secret;
        server_ops::HostKeys keys;
        bool moved = false; // an edit to another user, host or port
    };
    enum class Status { queued, running, paused, done, failed, cancelled, skipped };
    // What a running download does besides fetching, for its Status (DOWNLOADS-UI.md 5.3): a
    // folder is listed, then checked (files already here, the question about different ones, the
    // free space); a finished download is checked against the server's checksums.
    enum class Phase { none, listing, checking, verifying };
    // Why a running download is being stopped.
    enum class StopReason { none, pause, cancel };
    // A file already there that a download would replace, for the question.
    struct Asked {
        std::string name; // as shown: the file's name, or its path in the folder
        std::uint64_t size = 0;
        std::optional<std::int64_t> modified; // the server's, Unix seconds
        existing::Local mine;
    };

    struct Item {
        int id = 0;
        std::string remote;
        Mode mode = Mode::file;
        std::string path;
        std::string name;
        std::filesystem::path dest;
        Status status = Status::queued;
        std::uint64_t bytes = 0;
        std::uint64_t total = 0;
        double speed = 0;
        std::optional<double> eta;
        int connections = 0;
        std::string current;
        std::string error;
        std::string note;
        // Why it waits paused for the user (the question about files already there, or the free
        // space): its own message row. The note is plain information for the Status tooltip.
        std::string waiting;
        std::stop_source stop;
        StopReason stop_reason = StopReason::none;
        // Bandwidth priority as a weight: high 4, normal 2, low 1. Atomic: running streams
        // read it at every limiter request.
        std::atomic<int> weight{2};
        // Running or queued when an update decided to restart grab-gui: saved to continue by
        // itself afterwards (it may be paused by the shutdown before the last save).
        bool resume_mark = false;
        // While running: listing or checking (folders), or verifying, which is checking against
        // the server's checksum; a file whose check was stopped (paused) is checked when it
        // resumes, although it is complete on disk.
        Phase phase = Phase::none;
        bool verify_pending = false;
        // A folder being checked: its files with their result so far, of how many.
        std::size_t verify_done = 0;
        std::size_t verify_total = 0;
        // What the last check found, for the row's ✓ or ⚠ (verify::mark).
        verify::Mark check = verify::Mark::none;
        // The free-space check is done: "Download anyway" was chosen, or Resume after the check
        // paused it (a folder too big for the drive once listed).
        bool space_ok = false;
        // A different file already at a target: what to do. Answered before queuing, or when a
        // folder was listed; ask = not answered (it then waits paused with the question).
        existing::Choice existing = existing::Choice::ask;
        std::filesystem::path local_name; // a file's Keep-both name, instead of the server's
        // Waiting for that answer: some of the files concerned, and how many there are.
        std::vector<Asked> asked;
        std::size_t asked_count = 0;
        folder::Tree tree; // folders: their files once listed (under mutex_)
        // A listed folder's file names, which the page's rows, queue.json and the actions by index
        // read without the lock, and their generation, which the page's "files" and actions carry:
        // new when the folder is listed or restored, or its files' local names change (Keep both).
        std::shared_ptr<const queue_view::Names> names;
        int gen = 0;
        std::array<std::size_t, 6> counts{}; // its files by folder::State, as the last pass counted
        // Its files changed since the last flush (a pass over them is due); set without mutex_.
        std::atomic<bool> touched{false};
        // The user's ▸/▾ in the Downloads table, kept in queue.json; unset: the default rule.
        std::optional<bool> expanded;
        // Restored with failed files: they are queued again when it runs, as a restart did.
        bool retry_failed = false;
        bool fake = false; // debug builds: GRAB_TEST_FAKE_FOLDER's folder, whose files advance by themselves
    };

    void post(const json::Value& message) const;
    void send_init();
    void start_search(const json::Value& msg);
    // A search of one server, or of all ("*"). `update`: the same search again after a file list
    // was refreshed; it never starts another refresh. `under`: a search below a folder, of one
    // server, for folders and files at once. `both`: folders and files at once (req.mode unused),
    // from a server's top or All servers'.
    void run_search(int id, const std::string& remote, const engine::SearchRequest& req, bool update, bool both,
                    const std::optional<std::string>& under);
    // Folders and files below `under`: from the server's list when it holds that folder, else
    // live with the folder as the only root. Notes where the answer came from in `status`.
    [[nodiscard]] std::expected<std::vector<engine::Hit>, std::string> search_below(
        const engine::Context& ctx, const std::string& server, const engine::SearchRequest& req,
        const std::string& under, bool update, const proc::RunOptions& run, json::Value& status);
    // A search, list or Browse through grab's own SSH client on `server`: its pool
    // (the budget configured first), shared with the downloads, and a seat of one connection
    // taken from the budget into `seat` now (fetch::Listing, review G191), which a walk widens to
    // up to 8 (GrabSide::walk_seat). Any thread.
    [[nodiscard]] std::expected<engine::GrabSide, std::string> grab_side(const engine::Context& ctx,
                                                                         const std::string& server,
                                                                         std::optional<fetch::Listing>& seat,
                                                                         std::stop_token stop);

    // Browse mode: a folder of one server, from its file list when the list holds the folder's
    // contents, else listed live (kept for the session per server and folder). One request at a
    // time on browse_thread_; a new one stops the last. `top`: the server's top; `live`: list it
    // live now (Refresh); `update`: the same folder again after its list was refreshed; `typed`:
    // `path` was typed in the address bar (a file's path opens its folder; an error leaves the
    // folder on screen as it was).
    void run_browse(int id, const std::string& remote, const std::string& path, bool top, bool live, bool update,
                    bool typed);
    // Folder names for the address bar's suggestions, from the list and this session's live
    // listings; never from the server. Without a list it starts fetching one ("later": ask again).
    void suggest(const std::string& remote, const std::string& path);
    struct LastBrowse {
        int id = 0;
        std::string remote;
        std::string path;
        bool top = false;
        bool typed = false;
    };
    struct LiveFolder {
        std::vector<browse::Item> items;
        std::int64_t listed = 0; // Unix seconds
    };

    // Saved file lists (file_list.hpp), one per server, all under lists_mutex_.
    struct ListSlot {
        bool loaded = false; // read from disk once
        std::shared_ptr<const file_list::List> list;
        std::shared_ptr<const browse::Index> index; // the list's folders, built on the first browse
        bool refreshing = false;
        std::string error; // why the last refresh failed
        int failures = 0;  // refreshes failed in a row
        std::chrono::steady_clock::time_point retry_after{}; // no automatic refresh before
    };
    ListSlot& list_slot(const std::string& server); // caller holds lists_mutex_
    // The server's list, if it fits `ctx`, with its browse index; any thread.
    struct ListView {
        std::shared_ptr<const file_list::List> list;
        std::shared_ptr<const browse::Index> index;
    };
    [[nodiscard]] ListView list_view(const std::string& server, const engine::Context& ctx);
    // Fetches a new list in the background; any thread. `asked`: the user's Refresh, which goes
    // ahead even while a failed one waits to be tried again.
    void start_refresh(const std::string& server, bool asked = false);
    void on_list_refreshed(const std::string& server); // UI thread: shows the last search again
    void prune_lists();                                // drops lists of servers no longer configured
    // Download (or Download to…): {dest, groups: [{remote, mode, items}], force, remember}. Unless
    // `force`, a selection that won't fit on the drive, with what downloads already queued there
    // still need, comes back as "spaceShort" instead of being queued.
    void enqueue(const json::Value& msg);
    // What the selection in `groups` still needs on disk in `dest`: files less what is there
    // already; folders as their server's saved list holds them (`partial` when it doesn't hold
    // all of one).
    struct Needed {
        std::uint64_t bytes = 0;
        bool partial = false;
    };
    [[nodiscard]] Needed needed_for(const json::Value& groups, const std::filesystem::path& dest);
    // The files of the selection that would replace a different file in `dest` (folders as their
    // server's saved list holds them): up to `shown` of them, and how many in all. `covered` gets
    // the items (server, path) with such files whose all are known: files, and folders the list
    // holds completely.
    std::vector<Asked> conflicts_for(const json::Value& groups, const std::filesystem::path& dest, std::size_t shown,
                                     std::size_t& count,
                                     std::set<std::pair<std::string, std::string>>* covered = nullptr);
    // The answer for a download waiting with the question: it goes on.
    void resolve_existing(int id, existing::Choice choice);
    // The question's files for the page: {count, files: [{name, size, modified, mineSize, mineModified}]}.
    [[nodiscard]] static json::Value asked_json(const std::vector<Asked>& asked, std::size_t count);
    // Where a file download lands: its server name made safe, or its Keep-both name.
    [[nodiscard]] static std::filesystem::path item_target(const Item& item);
    // Queue actions; `id` 0 means every item the action applies to.
    void pause_items(int id);
    void resume_items(int id);
    void cancel_items(int id);
    void retry_item(int id);
    // Files of a folder download by their index (the page's rows): "fileAction" {id, gen, act:
    // pause|resume|retry|skip, ranges: [[from, to], ...]}, one lock, one save and one flush for
    // all of them. Rows of another generation change nothing.
    void file_action(const json::Value& msg);
    // Their priority inside the folder, "setFilePriority" {id, gen, ranges, priority: high|normal|
    // low}, the same way.
    void set_file_priority(const json::Value& msg);
    // A file of a folder download in Explorer, "showFile" {id, gen, index}.
    void show_file(int id, int gen, std::size_t index);
    void show_in_explorer(const std::filesystem::path& target) const; // selected, or its folder when it's gone
    void clear_finished();
    void set_parallel(int n);
    // Queue order (`before` = another item's id, 0 = the end) and bandwidth priority.
    void move_item(int id, int before);
    void set_priority(int id, int weight);
    // The global download speed limit (UI thread): the user's own setting, the speed schedule,
    // or a change made while the schedule limits the speed, which holds until its next change.
    void set_limit(bool on, double mibps);
    void set_schedule(const json::Value& rules);
    [[nodiscard]] schedule::Applied current_limit(const schedule::Now& now) const;
    void update_limit(); // applies what holds now and tells the page about any change
    void set_rate(std::uint64_t bytes_per_s);
    [[nodiscard]] json::Value limit_message(const schedule::Now& now) const;
    // The folder picker. Its answer ("folderPicked") carries `purpose` back: "dest" for Save to,
    // "downloadTo" for the menu's Download to....
    void pick_folder(std::wstring current, std::string purpose);
    void pick_file(std::wstring current);
    void open_item_folder(int id);
    void copy_text(const std::string& text) const; // to the Windows clipboard, lines ending in CR LF
    void open_config() const;

    // Settings: server operations run one at a time on settings_thread_.
    void settings_task(std::function<void(std::stop_token)> task);
    void settings_error(const std::string& message) const;
    [[nodiscard]] std::expected<server_ops::Env, std::string> load_env() const;
    void post_servers(const server_ops::Env& env) const;
    void post_host_keys(const Pending& p) const;
    void servers_changed(); // settings thread: refresh the settings list and the search view
    void save_server(const json::Value& msg);
    void apply_pending(Pending pending, std::stop_token stop);
    void test_server(const std::string& name, std::stop_token stop);
    void cancel_pending();

    // Updates (UI thread unless noted). The banner's state, sent to the page as "update".
    struct UpdateBanner {
        std::string state;   // "" (none), "available", "running", "installed", "failed"
        std::string version; // of the release ("0.7.0")
        std::string page;    // its release page, for "What's new"
        std::string phase;   // while running: "checking", "downloading", "verified", "installing"
        std::string error;
        bool completing = false; // the same version, adding programs a portable folder lacks
    };
    void maybe_check_updates(); // once, at the page's first init
    void start_update_check(bool manual);
    void on_update_checked(const std::expected<update::Check, std::string>& result, bool manual);
    void start_update();        // runs `grab.exe update` from this program's folder
    void on_update_line(const std::string& line);
    void on_update_finished(int exit_code, const std::string& last_error);
    // The failed banner closed: the failure `grab update` kept (update::Result) isn't shown again.
    void dismiss_update();
    void restart_for_update();  // starts the new grab-gui, which waits for this one, and exits
    void mark_for_restart(bool on); // marks (or unmarks) running and queued downloads, then saves
    void post_update() const;
    void open_release_page() const;

    void worker_loop(std::stop_token stop);
    void run_item(const std::shared_ptr<Item>& item, std::stop_token app_stop);
    void run_file(const std::shared_ptr<Item>& item, const std::stop_token& item_stop);
    void run_folder(const std::shared_ptr<Item>& item, const std::stop_token& item_stop);
    void folder_event(Item& item, folder::Event e); // a running folder's progress: marks it for the next flush
    // A listed folder's totals and file counts from one pass over its files; with `shadow`, also
    // its files changed since (queue_view::pass). Caller holds mutex_.
    static queue_view::Pass refresh_folder(Item& item, std::vector<queue_view::Shadow>* shadow = nullptr);
    // A new generation of a listed folder's names. Built without mutex_, on the worker that alone
    // writes its files' paths and local names (the listing, Keep both).
    void new_names(Item& item);
    // What a verification of `item`'s download needs: grab's own connections to its server (the
    // download's pool, else the server's from pools_), or why there are none; the budget and its
    // id; the row's "12 of 40" as files end.
    [[nodiscard]] verify::Request verify_request(const engine::Context& ctx, const std::shared_ptr<Item>& item,
                                                 const std::shared_ptr<ssh::Pool>& download_pool);
    void discard_folder_partials(const Item& item) const; // cancelled folder: big files' .grabpart
    // grab.conf's rclone remote for `server` (else the name): partial downloads are told apart by
    // it, so another server's of the same path is left alone (review G20).
    [[nodiscard]] std::string rclone_remote_of(const std::string& server) const;
    // Another row, not over, for the same server file into the same place (compared as Windows
    // names it): it shares `item`'s partial download. Caller holds mutex_.
    [[nodiscard]] std::shared_ptr<Item> twin_of(const Item& item) const;
    // Files the running downloads but `except` download right now, as "At once" counts them: a
    // file is one, a folder the files it runs (at least one). Caller holds mutex_.
    [[nodiscard]] int files_in_use(const Item* except = nullptr) const;
    // A failed or cancelled row queued again (Retry): a file goes on from its partial download
    // when one is left. Caller holds mutex_.
    void requeue(Item& item) const;
    // The connection budget for a server, from grab.conf, its host and what grab (or the command)
    // learned, read from limits.json each time.
    [[nodiscard]] int connection_budget(const std::string& remote, const std::string& host,
                                        std::optional<int> configured);
    void on_budget_learned(const std::string& server, int budget); // any thread
    // After a download from `server` ended done: with no refusal since `refusals_before`, a
    // learned limit goes up by one (budget::raised).
    void download_went_well(const std::string& server, std::uint64_t refusals_before);
    // A download's row for the page; `position`: its place among the waiting ones (0: none).
    // Caller holds mutex_.
    [[nodiscard]] json::Value queue_item(const Item& item, int position) const;
    // The queue's messages to the page, from the UI thread only (a debug assert checks it).
    void post_ui(std::string text) const;
    // The page shows a folder's files (DOWNLOADS-UI.md 6.3): "watchFiles" {id} for each open
    // folder, "unwatchFiles" {id} when it is folded; the page's init drops every watch. A watched
    // folder's files go in chunks of files_chunk rows, one per turn of the message loop, then the
    // rows that changed at each flush.
    struct FilesSend {
        std::shared_ptr<const queue_view::Names> names;
        queue_view::Snapshot snap;
        int gen = 0;
        std::size_t next = 0;
    };
    struct Watch {
        int gen = 0; // the generation sent or being sent; 0: none yet
        std::vector<queue_view::Shadow> shadow; // what the page has, once every chunk went
        std::shared_ptr<FilesSend> send;        // while the chunks go: no deltas meanwhile
        bool catch_up = false; // every chunk went: the next flush diffs it even if not running or touched
    };
    void watch_files(int id);
    void start_files(int id);
    void send_files_chunk(int id, const std::shared_ptr<FilesSend>& send);
    void restore_queue();
    // queue.json now, from any thread: unfinished items. Under mutex_ it copies only compact data
    // (each file's state in a few bytes, the names from Item::names); the text is made after.
    void save_queue() const;
    // queue.json soon, from any thread: at most once every queue_save_gap (longer after a slow
    // save), on saver_. now (a download ended): without the queue_save_gap floor, still at most
    // once every ten times the last save's duration. Closing and an update's restart save at once.
    void request_save(bool now = false);
    void run_saves(const std::stop_token& stop);
    // Debug builds, GRAB_TEST_FLUSH_LOG=<file>: a line per flush, first answer and save (the
    // lock held, rows, bytes), for the performance checks (DOWNLOADS-UI.md 14, C).
    [[nodiscard]] static bool test_logging();
    static void test_log(const std::string& line);
    void add_fake_folder(std::size_t files); // debug builds: GRAB_TEST_FAKE_FOLDER=N
    void run_fake(Item& item, const std::stop_token& stop);
    void report_save(std::string_view what, const std::expected<void, std::string>& saved,
                     std::atomic<bool>& failing) const;
    void set_aside(std::string_view what, const std::filesystem::path& p, const std::string& why,
                   std::string_view instead);

    Host host_;
    std::filesystem::path config_path_;
    std::filesystem::path state_path_;
    std::filesystem::path queue_path_;
    std::filesystem::path recent_path_; // recent searches, sealed (gui_state.hpp)
    std::filesystem::path limits_path_; // learned connection limits, shared with the command
    GuiState state_;
    std::vector<std::string> notices_; // UI thread: saved files set aside, told once the page is open
    std::string import_banner_;        // UI thread: the one-time notice after an import, until the page shows it
    bool page_ready_ = false;          // UI thread: the page has sent "init"
    bool closed_ = false;              // UI thread: close() ran
    fetch::Connections connections_;
    ssh::Pools pools_;          // grab's own SSH connections: engine = grab servers, every server's verification
    rate::RateLimiter limiter_; // shared by every download's streams
    std::atomic<bool> verify_downloads_{false}; // GuiState::verify_downloads, for the workers
    std::string limit_posted_;  // the last "limit" message, so only changes are sent

    struct LastSearch {
        int id = 0;
        std::string remote;
        engine::SearchRequest req;
        bool both = false;
        std::optional<std::string> under;
    };
    std::filesystem::path lists_dir_;
    std::mutex lists_mutex_; // guards lists_, refresh_threads_, last_search_
    std::map<std::string, ListSlot> lists_;
    std::map<std::string, std::jthread> refresh_threads_;
    LastSearch last_search_;
    // After the lists: the search thread uses them and starts refreshes, so it ends first.
    std::jthread search_thread_;
    std::atomic<bool> search_replaced_{false}; // a stopped search is being replaced: no "cancelled"
    std::mutex browse_mutex_; // guards last_browse_, shown_browse_ and live_folders_
    LastBrowse last_browse_;  // the last request, again after a list refresh
    LastBrowse shown_browse_; // the folder the window shows: where a failed typed path leaves it
    std::map<std::pair<std::string, std::string>, LiveFolder> live_folders_; // (server, folder)
    std::jthread browse_thread_; // after the lists, which it uses

    UpdateBanner update_;
    bool update_checks_started_ = false;
    std::atomic<bool> update_check_busy_{false};
    std::jthread update_check_thread_;
    std::jthread update_thread_; // follows a running `grab update`
    // Set while an update is about to restart grab-gui: save_queue() then saves the marked
    // downloads (Item::resume_mark) to continue by themselves after the restart.
    std::atomic<bool> resume_after_restart_{false};

    std::mutex settings_mutex_; // guards pending_
    std::optional<Pending> pending_;
    std::atomic<bool> settings_busy_{false};
    std::jthread settings_thread_;

    // Guards items_, next_id_, next_gen_, parallel_, learned_, defaults_. Worker threads read
    // parallel_ here rather than state_, which belongs to the UI thread.
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::vector<std::shared_ptr<Item>> items_;
    int next_id_ = 1;
    int next_gen_ = 1;
    int parallel_ = 4;
    std::map<std::string, int> learned_;  // as limits.json had it when last read
    std::map<std::string, int> defaults_; // each server's default budget, by its host
    // The page's view, UI thread (dirty_ any thread): what flush() sends next.
    DWORD ui_thread_ = 0;
    std::atomic<bool> dirty_{false}; // something changed: set by workers, actions, folder events
    bool queue_changed_ = true;      // downloads came, went or moved: the next flush sends "queue"
    std::map<int, std::string> item_sent_; // each download's row as last sent
    int parallel_sent_ = 0;
    std::map<int, Watch> watches_; // the folders whose files the page shows, by id
    mutable std::mutex queue_file_mutex_; // one writer of queue.json at a time
    std::mutex save_mutex_;               // guards save_wanted_, save_now_, next_save_, next_urgent_save_
    std::condition_variable_any save_wake_;
    bool save_wanted_ = false;
    bool save_now_ = false; // the one wanted is urgent (request_save(true))
    std::chrono::steady_clock::time_point next_save_{};
    std::chrono::steady_clock::time_point next_urgent_save_{};
    mutable std::atomic<bool> queue_unsaved_{false}; // the last save of queue.json failed (shown once)
    mutable std::atomic<bool> state_unsaved_{false}; // the same for gui.json
    mutable std::atomic<bool> limits_unsaved_{false}; // and for limits.json

    // The status bar's activities: their ids, and how long steps took (from and to gui.json).
    std::atomic<int> next_activity_{1};
    std::mutex timings_mutex_;
    std::map<std::string, int> timings_;

    // Play's local stream server; after connections_ and pools_, which its streams use.
    play::Server play_server_{connections_};
    std::mutex play_mutex_; // guards play_next_, play_current_
    std::condition_variable_any play_wake_;
    std::optional<PlayJob> play_next_;
    std::stop_source play_current_; // the Play preparing now
    std::jthread play_thread_;      // after play_server_: joined before it goes
    // The previews being fetched, by file, and the ones kept in memory (thread-safe).
    preview::Previews preview_jobs_;
    // Guards preview_queue_, viewer_seats_, preview_workers_.
    std::mutex preview_mutex_;
    std::condition_variable_any preview_wake_; // a job queued, a viewer seat back
    std::deque<std::function<void()>> preview_queue_;
    struct ViewerSeat {
        int id = 0;    // the viewer's own in connections_, joined at High priority while it has seats
        int held = 0;  // seats it has: in use, or kept
        int in_use = 0;
        int taking = 0; // in take_viewer_seat, waiting for a seat
        std::chrono::steady_clock::time_point idle_since;
    };
    std::map<std::string, ViewerSeat> viewer_seats_; // by server
    std::vector<std::jthread> preview_workers_; // after the members their jobs use

    // Skipped big files' partial downloads, deleted on discarder_: the names of where a folder's
    // files land (queue_view::reserved_names) take long for a big folder, so not on the UI thread.
    struct DiscardJob {
        std::shared_ptr<const queue_view::Names> names;
        std::filesystem::path dir;
        std::string rclone_remote;
        std::vector<folder::Discard> discard;
    };
    void run_discards(const std::stop_token& stop);
    std::mutex discard_mutex_; // guards discard_jobs_
    std::condition_variable_any discard_wake_;
    std::deque<DiscardJob> discard_jobs_;
    std::jthread discarder_; // what was asked for is done at its stop

    std::jthread saver_; // ends after workers_: the saves they asked for are made, the last at its stop
    std::vector<std::jthread> workers_; // last member: stopped and joined first
};

} // namespace grab::gui
