#pragma once

#include "cli.hpp"
#include "engine.hpp"
#include "fetch.hpp"
#include "gui_state.hpp"
#include "json.hpp"
#include "rate.hpp"
#include "server_ops.hpp"
#include "servers.hpp"

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
    // Why a running download (or a file of a folder) is being stopped.
    enum class StopReason { none, pause, cancel, skip };
    // One file of a folder download. Files of folder::big_file_threshold and up are fetched
    // like single files (resumable); smaller ones go in the folder's rclone batch.
    struct Child {
        std::string path; // relative to the folder, '/'-separated
        std::uint64_t size = 0;
        std::string modtime;
        bool big = false;
        Status status = Status::queued; // queued, running, paused, done, failed or skipped
        std::uint64_t bytes = 0;
        double speed = 0;
        std::optional<double> eta;
        std::string error;
        std::stop_source stop; // a running big file's own stop
        StopReason stop_reason = StopReason::none;
        // In the running rclone batch: queued until rclone starts it, then running.
        bool in_batch = false;
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
        std::stop_source stop;
        StopReason stop_reason = StopReason::none;
        // Folders: their files once listed, and the running small-file batch's stop (a file
        // paused or skipped in it restarts the batch without that file).
        bool listed = false;
        std::vector<Child> children;
        std::stop_source batch_stop;
        bool batch_running = false;
    };

    void post(const json::Value& message) const;
    void send_init();
    void start_search(const json::Value& msg);
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
    // The global download speed limit (UI thread).
    void set_limit(bool on, double mibps);
    void pick_folder(std::wstring current);
    void pick_file(std::wstring current);
    void open_item_folder(int id);
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

    void worker_loop(std::stop_token stop);
    void run_item(const std::shared_ptr<Item>& item, std::stop_token app_stop);
    void run_file(const std::shared_ptr<Item>& item, const std::stop_token& item_stop);
    void run_folder(const std::shared_ptr<Item>& item, const std::stop_token& item_stop);
    void run_small_batch(const std::shared_ptr<Item>& item, const engine::Context& ctx,
                         const std::vector<std::string>& paths, const std::stop_token& item_stop);
    void run_big_files(const std::shared_ptr<Item>& item, const engine::Context& ctx,
                       const std::stop_token& item_stop);
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

    std::jthread search_thread_;

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
