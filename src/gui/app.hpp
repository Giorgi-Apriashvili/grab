#pragma once

#include "browse.hpp"
#include "cli.hpp"
#include "fetch.hpp"
#include "file_list.hpp"
#include "folder_job.hpp"
#include "gui_state.hpp"
#include "json.hpp"
#include "rate.hpp"
#include "schedule.hpp"
#include "server_ops.hpp"
#include "servers.hpp"
#include "update.hpp"

#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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
    ~App(); // cancels the search and running download, joins the threads
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // UI thread: one message from the page.
    void on_message(const std::string& json);

    [[nodiscard]] bool downloads_active() const;
    [[nodiscard]] QueueSummary summary() const;
    [[nodiscard]] GuiState& state() { return state_; }
    void save_state() const;
    // UI thread, every second: follows the speed schedule.
    void tick();

private:
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
    };
    enum class Status { queued, running, paused, done, failed, cancelled, skipped };
    // Why a running download is being stopped.
    enum class StopReason { none, pause, cancel };
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
        std::stop_source stop;
        StopReason stop_reason = StopReason::none;
        // Bandwidth priority as a weight: high 4, normal 2, low 1. Atomic: running streams
        // read it at every limiter request.
        std::atomic<int> weight{2};
        // Running or queued when an update decided to restart grab-gui: saved to continue by
        // itself afterwards (it may be paused by the shutdown before the last save).
        bool resume_mark = false;
        // Being checked against the server's checksum; a file whose check was stopped (paused)
        // is checked when it resumes, although it is complete on disk.
        bool verifying = false;
        bool verify_pending = false;
        folder::Tree tree; // folders: their files once listed (under mutex_)
    };

    void post(const json::Value& message) const;
    void send_init();
    void start_search(const json::Value& msg);
    // A search of one server, or of all ("*"). `update`: the same search again after a file list
    // was refreshed; it never starts another refresh. `under`: browse mode's search below a
    // folder, of one server, for folders and files at once.
    void run_search(int id, const std::string& remote, const engine::SearchRequest& req, bool update,
                    const std::optional<std::string>& under);
    // Folders and files below `under`: from the server's list when it holds that folder, else
    // live with the folder as the only root. Notes where the answer came from in `status`.
    [[nodiscard]] std::expected<std::vector<engine::Hit>, std::string> search_below(
        const engine::Context& ctx, const std::string& server, const engine::SearchRequest& req,
        const std::string& under, bool update, const proc::RunOptions& run, json::Value& status);

    // Browse mode: a folder of one server, from its file list when the list holds the folder's
    // contents, else listed live (kept for the session per server and folder). One request at a
    // time on browse_thread_; a new one stops the last. `top`: the server's top; `live`: list it
    // live now (Refresh); `update`: the same folder again after its list was refreshed.
    void run_browse(int id, const std::string& remote, const std::string& path, bool top, bool live, bool update);
    struct LastBrowse {
        int id = 0;
        std::string remote;
        std::string path;
        bool top = false;
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
    };
    ListSlot& list_slot(const std::string& server); // caller holds lists_mutex_
    // The server's list, if it fits `ctx`, with its browse index; any thread.
    struct ListView {
        std::shared_ptr<const file_list::List> list;
        std::shared_ptr<const browse::Index> index;
    };
    [[nodiscard]] ListView list_view(const std::string& server, const engine::Context& ctx);
    void start_refresh(const std::string& server);   // fetches a new list in the background; any thread
    void on_list_refreshed(const std::string& server); // UI thread: shows the last search again
    void prune_lists();                                // drops lists of servers no longer configured
    void enqueue(const json::Value& msg);
    // Queue actions; `id` 0 means every item the action applies to.
    void pause_items(int id);
    void resume_items(int id);
    void cancel_items(int id);
    void retry_item(int id);
    // A file of a folder download.
    void pause_child(int id, const std::string& path);
    void resume_child(int id, const std::string& path);
    void skip_child(int id, const std::string& path);
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
    void pick_folder(std::wstring current);
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
    void test_server(const std::string& name, std::stop_token stop) const;
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
    void restart_for_update();  // starts the new grab-gui, which waits for this one, and exits
    void mark_for_restart(bool on); // marks (or unmarks) running and queued downloads, then saves
    void post_update() const;
    void open_release_page() const;

    void worker_loop(std::stop_token stop);
    void run_item(const std::shared_ptr<Item>& item, std::stop_token app_stop);
    void run_file(const std::shared_ptr<Item>& item, const std::stop_token& item_stop);
    void run_folder(const std::shared_ptr<Item>& item, const std::stop_token& item_stop);
    void folder_event(Item& item, folder::Event e); // a running folder's progress, for the page
    void folder_totals(Item& item) const;          // caller holds mutex_
    void discard_folder_partials(const Item& item) const; // cancelled folder: big files' .grabpart
    // The connection budget for a server, from grab.conf, its host and what grab learned.
    [[nodiscard]] int connection_budget(const std::string& remote, const std::string& host,
                                        std::optional<int> configured) const;
    void on_budget_learned(const std::string& server, int budget); // any thread
    [[nodiscard]] json::Value queue_snapshot() const; // caller holds mutex_
    void post_queue();
    void restore_queue();
    void save_queue() const; // any thread; unfinished items, for the next start

    Host host_;
    std::filesystem::path config_path_;
    std::filesystem::path state_path_;
    std::filesystem::path queue_path_;
    GuiState state_;
    fetch::Connections connections_;
    rate::RateLimiter limiter_; // shared by every download's streams
    std::atomic<bool> verify_downloads_{false}; // GuiState::verify_downloads, for the workers
    std::string limit_posted_;  // the last "limit" message, so only changes are sent

    struct LastSearch {
        int id = 0;
        std::string remote;
        engine::SearchRequest req;
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
    std::mutex browse_mutex_; // guards last_browse_ and live_folders_
    LastBrowse last_browse_;
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

    // Guards items_, next_id_, last_progress_post_, parallel_, learned_. Worker threads read the
    // last two here rather than state_, which belongs to the UI thread.
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::vector<std::shared_ptr<Item>> items_;
    int next_id_ = 1;
    int parallel_ = 4;
    std::map<std::string, int> learned_;
    std::chrono::steady_clock::time_point last_progress_post_{};
    mutable std::mutex queue_file_mutex_; // one writer of queue.json at a time
    std::vector<std::jthread> workers_; // last member: stopped and joined first
};

} // namespace grab::gui
