#pragma once

#include "cli.hpp"
#include "gui_state.hpp"
#include "json.hpp"

#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
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
    };

    App(Host host, std::filesystem::path config_path, std::filesystem::path state_path);
    ~App(); // cancels the search and running download, joins the threads
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // UI thread: one message from the page.
    void on_message(const std::string& json);

    [[nodiscard]] bool downloads_active() const;
    [[nodiscard]] GuiState& state() { return state_; }
    void save_state() const;

private:
    enum class Status { queued, running, done, failed, cancelled };
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
        std::string current;
        std::string error;
        std::stop_source stop;
    };

    void post(const json::Value& message) const;
    void send_init();
    void start_search(const json::Value& msg);
    void enqueue(const json::Value& msg);
    void cancel_item(int id);
    void retry_item(int id);
    void clear_finished();
    void pick_folder(std::wstring current);
    void open_item_folder(int id);

    void worker_loop(std::stop_token stop);
    void run_item(const std::shared_ptr<Item>& item, std::stop_token app_stop);
    [[nodiscard]] json::Value queue_snapshot() const; // caller holds mutex_
    void post_queue();

    Host host_;
    std::filesystem::path config_path_;
    std::filesystem::path state_path_;
    GuiState state_;

    std::jthread search_thread_;

    mutable std::mutex mutex_; // guards items_, next_id_, last_progress_post_
    std::condition_variable_any wake_;
    std::vector<std::shared_ptr<Item>> items_;
    int next_id_ = 1;
    std::chrono::steady_clock::time_point last_progress_post_{};
    std::jthread worker_; // last member: stopped and joined first
};

} // namespace grab::gui
