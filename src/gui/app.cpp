#include "app.hpp"

#include "activity.hpp"
#include "browse.hpp"
#include "config.hpp"
#include "conn_budget.hpp"
#include "engine.hpp"
#include "file_list.hpp"
#include "folder.hpp"
#include "gui_state.hpp"
#include "ini.hpp"
#include "listing.hpp"
#include "local_name.hpp"
#include "notices.hpp"
#include "open_file.hpp"
#include "play_server.hpp"
#include "preview.hpp"
#include "process.hpp"
#include "queue_view.hpp"
#include "quote.hpp"
#include "rclone_encoding.hpp"
#include "rclone_import.hpp"
#include "remote.hpp"
#include "space.hpp"
#include "speed.hpp"
#include "ssh.hpp"
#include "ssh_read.hpp"
#include "update.hpp"
#include "util.hpp"
#include "verify.hpp"

#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>   // IWYU pragma: keep (SHGetKnownFolderPath via shlobj_core.h)
#include <shobjidl.h> // IWYU pragma: keep (IFileOpenDialog via shobjidl_core.h)
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <span>
#include <utility>

#ifndef GRAB_VERSION
#define GRAB_VERSION "dev"
#endif

using Microsoft::WRL::ComPtr;

namespace grab::gui {

namespace {

// Where a file download lands: dest and the server's name as a safe Windows one (local_name.hpp),
// from the server path (the page's "name" is only shown).
std::filesystem::path file_target(const std::filesystem::path& dest, const std::string& remote_path) {
    return dest / local::relative(remote_basename(remote_path));
}

std::optional<std::int64_t> seconds_to_ns(std::optional<std::int64_t> seconds) {
    if (!seconds) return std::nullopt;
    return *seconds * 1'000'000'000;
}

using json::Value;

// A folder's files go to the page in chunks of this many rows, one per turn of the message loop,
// so the window keeps answering while a 177,000-file folder's list goes (DOWNLOADS-UI.md 6.3).
constexpr std::size_t files_chunk = 5000;
// queue.json is saved at most this often while downloads change it (DOWNLOADS-UI.md 6.5).
constexpr auto queue_save_gap = std::chrono::seconds(2);

Value str(std::string s) { return Value::make_string(std::move(s)); }
Value num(double n) { return Value::make_number(n); }

const char* status_name(int s) {
    constexpr const char* names[] = {"queued", "running", "paused", "done", "failed", "cancelled", "skipped"}; // Status order
    return names[s];
}

// A running download's phase for the page; nullptr for none (it is fetching).
const char* phase_name(int p) {
    constexpr std::array<const char*, 4> names = {nullptr, "listing", "checking", "verifying"}; // Phase order
    return names[static_cast<std::size_t>(p)];
}

Mode mode_of(const Value& msg) { return msg.string_of("mode") == "folder" ? Mode::folder : Mode::file; }

int int_of(const Value& msg, std::string_view key, int fallback = 0) {
    const Value* v = msg.find(key);
    return v != nullptr && v->kind == Value::Kind::number ? static_cast<int>(v->number) : fallback;
}

bool bool_of(const Value& msg, std::string_view key) {
    const Value* v = msg.find(key);
    return v != nullptr && v->kind == Value::Kind::boolean && v->boolean;
}

// Bandwidth priority names and weights (4:2:1).
int weight_of(std::string_view priority) { return priority == "high" ? 4 : priority == "low" ? 1 : 2; }
const char* priority_name(int weight) { return weight >= 4 ? "high" : weight <= 1 ? "low" : "normal"; }

std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// A search's notes (engine::SearchResult::notes) on its server's status, for the status line.
void set_notes(Value& status, const std::vector<std::string>& notes) {
    if (notes.empty()) return;
    Value list = Value::make_array();
    for (const auto& n : notes) list.push(str(n));
    status.set("notes", std::move(list));
}

double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

std::string known_folder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) out = util::to_utf8(p);
    CoTaskMemFree(p);
    return out;
}

} // namespace

App::App(Host host, std::filesystem::path config_path, std::filesystem::path state_path)
    : host_(std::move(host)),
      config_path_(std::move(config_path)),
      state_path_(std::move(state_path)),
      queue_path_(state_path_.parent_path() / "queue.json"),
      recent_path_(state_path_.parent_path() / "recent"),
      limits_path_(state_path_.parent_path() / "limits.json"),
      connections_([this](const std::string& server, int budget) { on_budget_learned(server, budget); }) {
    if (auto saved = read_gui_state(state_path_); !saved) {
        set_aside("grab's settings", state_path_, saved.error(), "the defaults are used");
    } else if (*saved) {
        state_ = std::move(**saved);
    }
    parallel_ = state_.parallel;
    // Learned connection limits are in limits.json, shared with the command (G195); what gui.json
    // held before moves there once.
    learned_ = load_learned_limits(limits_path_, state_.server_limits);
    if (std::error_code ec; !state_.server_limits.empty() && !std::filesystem::exists(limits_path_, ec)) {
        const auto& [server, n] = *state_.server_limits.begin();
        report_save("the learned connection limits", save_learned_limit(limits_path_, server, n, state_.server_limits),
                    limits_unsaved_);
    }
    timings_ = state_.timings;
    verify_downloads_ = state_.verify_downloads;
    // The speed limit that holds now; the first tick() tells the page and drops a change that
    // no longer holds.
    if (const auto applied = current_limit(schedule::local_now()); applied.limit.on) {
        limiter_.set_rate(schedule::bytes_per_s(applied.limit.mibps));
    }
    // Output of an earlier update: an installer update outlives the window that followed it. One
    // still open stays: its `grab update` (or grab-gui) still runs and writes it, and what an
    // update did is kept apart from it (update::Result).
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(std::filesystem::temp_directory_path(ec), ec)) {
        const auto name = e.path().filename().wstring();
        if ((name.starts_with(L"grab-update-") && name.ends_with(L".log")) || name == L"grab-gui-restart.log") {
            util::remove_if_unused(e.path());
        }
    }
    ui_thread_ = GetCurrentThreadId();
    restore_queue();
#ifndef NDEBUG
    // Tests: a synthetic folder of N files that advance by themselves, without a server or disk.
    if (const auto fake = util::getenv_utf8("GRAB_TEST_FAKE_FOLDER")) {
        std::size_t n = 0;
        if (std::from_chars(fake->data(), fake->data() + fake->size(), n).ec == std::errc{} && n > 0) add_fake_folder(n);
    }
#endif
    lists_dir_ = file_list::dir_for(config_path_);
    prune_lists();
    // grab 0.19's servers move from rclone.conf into grab.conf once (rclone_import.hpp), here on
    // the settings thread before its first server list, so the page's list (send_init, UI thread)
    // never waits on the settings lock for it; the list is sent again once it's done. Then the
    // one-time notice naming the servers moved (by this window or by a command before it), with
    // what each still needs (server_ops::import_banner); imported.json forgets them as it's made.
    // The settings' notices not shown yet in this configuration follow it (notices.hpp).
    settings_task([this](std::stop_token) {
        const auto files = rclone_import::files_for(config_path_);
        auto applied = rclone_import::apply(files);
        std::vector<std::string> notes;
        if (applied) {
            for (const auto& note : applied->notes) notes.push_back(std::format("[{}] {}", note.server, note.text));
            // Recorded, the note on grab's own old files comes with the notices below.
            if (!applied->old_files.empty() && !applied->old_files_recorded) notes.push_back(applied->old_files);
        }
        // The names leave imported.json only once the notice can be made (a grab.conf load_env
        // refuses keeps them for a later window).
        std::string banner;
        if (!notes.empty() || rclone_import::banner_waiting(files.state_dir)) {
            if (auto env = load_env()) {
                const auto names = rclone_import::take_banner(files.state_dir).value_or(std::vector<std::string>{});
                if (!names.empty() || !notes.empty()) {
                    banner = server_ops::import_banner(names, server_ops::list_servers(*env), notes);
                }
            }
        }
        if (auto all = notices::for_config(config_path_)) {
            auto fresh = notices::take_new(notices::path_in(state_dir(config_path_)), *all);
            for (const auto& n : fresh ? *fresh : *all) banner += (banner.empty() ? "" : "\n") + n.text;
        }
        const bool changed = !applied || !applied->imported.empty() || !applied->failed.empty();
        if (!changed && banner.empty()) return;
        host_.defer([this, changed, banner = std::move(banner), why = applied ? std::string() : applied.error()] {
            if (!why.empty()) notices_.push_back(why);
            if (!banner.empty()) import_banner_ = banner;
            if (!page_ready_) return; // its init takes the list, the notices and the banner
            if (changed) send_init();
            post_import_banner();
            if (!notices_.empty()) {
                Value v = Value::make_object();
                v.set("type", str("error"));
                v.set("message", str(util::join(std::exchange(notices_, {}), " ")));
                post(v);
            }
        });
    });
    discarder_ = std::jthread([this](const std::stop_token& stop) { run_discards(stop); });
    saver_ = std::jthread([this](const std::stop_token& stop) { run_saves(stop); });
    // Up to max_parallel downloads at once; parallel_ decides how many actually run.
    for (int i = 0; i < max_parallel; ++i) {
        workers_.emplace_back([this](std::stop_token stop) { worker_loop(std::move(stop)); });
    }
}

App::~App() {
    try {
        close();
    } catch (...) {
        // A save that threw (no memory left): the threads still stop as their members go, and
        // nothing waits on a server meanwhile.
        ssh::close_all_connections();
    }
}

void App::close() {
    if (closed_) return;
    closed_ = true;
    // First what outlives the program: running downloads are saved as unfinished (marked to go
    // on by themselves when an update restarts grab-gui), however the threads end below.
    save_state();
    save_queue();
    // Previews and Play first (review G42): nothing queued starts any more, and the running ones
    // stop at their next READs; their workers are joined before the members they use.
    {
        std::scoped_lock lock(play_mutex_);
        play_next_.reset();
        play_current_.request_stop(); // its wait for the warm part ends, and no player starts
    }
    play_thread_.request_stop();
    play_wake_.notify_all();
    preview_jobs_.stop_all();
    {
        std::scoped_lock lock(preview_mutex_);
        preview_queue_.clear();
        for (auto& worker : preview_workers_) worker.request_stop();
    }
    preview_wake_.notify_all();
    // Every other thread is told now, not only when its member is destroyed: a Browse or a list
    // refresh left for its turn could log in again meanwhile.
    if (search_thread_.joinable()) search_thread_.request_stop();
    if (browse_thread_.joinable()) browse_thread_.request_stop();
    {
        std::scoped_lock lock(lists_mutex_);
        for (auto& [server, refresh] : refresh_threads_) refresh.request_stop();
    }
    if (settings_thread_.joinable()) settings_thread_.request_stop();
    if (update_check_thread_.joinable()) update_check_thread_.request_stop();
    if (update_thread_.joinable()) update_thread_.request_stop(); // `grab update` carries on by itself
    cancel_pending();
    // Running downloads stop where they are: file downloads keep their partial file and come
    // back paused on the next start (saved above as unfinished).
    {
        std::lock_guard lock(mutex_);
        for (auto& item : items_) {
            if (item->status == Status::running) item->stop.request_stop();
        }
    }
    for (auto& w : workers_) w.request_stop();
    wake_.notify_all();
    // A login, read or command waiting on a server ends now; whatever is still connecting fails
    // at once. Without this a login stuck in the handshake held the exit for io_timeout, twice.
    ssh::close_all_connections();
}

void App::save_state() const { report_save("the window's settings", save_gui_state(state_path_, state_), state_unsaved_); }

// The one-time notice after an import, once: a reloaded page doesn't get it again.
void App::post_import_banner() {
    if (import_banner_.empty()) return;
    Value v = Value::make_object();
    v.set("type", str("importBanner"));
    v.set("text", str(std::exchange(import_banner_, {})));
    post(v);
}

// A saved file that exists but can't be read: moved aside, so nothing this run saves replaces
// it, with a notice for the page once it's open (reviews G212, G213).
void App::set_aside(std::string_view what, const std::filesystem::path& p, const std::string& why,
                    std::string_view instead) {
    auto moved = util::move_aside(p, "bad");
    notices_.push_back(moved ? std::format("{} couldn't be read ({}), so {}; the old file is kept as {}.", what, why,
                                           instead, util::path_to_utf8(*moved))
                             : std::format("{} couldn't be read ({}), so {}.", what, why, instead));
}

// gui.json and queue.json are saved often: a failure shows once, until a save works again (G214).
void App::report_save(std::string_view what, const std::expected<void, std::string>& saved,
                      std::atomic<bool>& failing) const {
    if (saved) {
        failing = false;
        return;
    }
    if (failing.exchange(true)) return;
    Value v = Value::make_object();
    v.set("type", str("error"));
    v.set("message", str(std::format("Couldn't save {}: {}", what, saved.error())));
    post(v);
}

// ---- the status bar's activities ------------------------------------------------------------

namespace {

// 4925 -> "4,925"
std::string thousands(std::uint64_t n) {
    std::string digits = std::to_string(n);
    for (auto at = digits.size(); at > 3; at -= 3) digits.insert(at - 3, ",");
    return digits;
}

// "1 entry", "4,925 entries"
std::string count_of(std::uint64_t n, std::string_view one, std::string_view many) {
    return std::format("{} {}", thousands(n), n == 1 ? one : many);
}

} // namespace

App::Activity::Activity(App& app, std::string text, std::string timing, bool background)
    : app_(app), id_(app.next_activity_++), timing_(std::move(timing)), started_(std::chrono::steady_clock::now()),
      text_(std::move(text)) {
    Value v = Value::make_object();
    v.set("type", str("activity"));
    v.set("id", num(id_));
    v.set("text", str(text_));
    if (background) v.set("bg", Value::make_bool(true));
    if (!timing_.empty()) {
        std::lock_guard lock(app_.timings_mutex_);
        const auto it = app_.timings_.find(timing_);
        v.set("estimate", num(it != app_.timings_.end() ? it->second : activity::guess(timing_)));
    }
    app_.post(v);
    sent_ = started_;
}

App::Activity::~Activity() {
    Value v = Value::make_object();
    v.set("type", str("activity"));
    v.set("id", num(id_));
    v.set("end", Value::make_bool(true));
    app_.post(v);
    if (failed_ || timing_.empty()) return;
    const auto ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_).count());
    int remembered = 0;
    {
        std::lock_guard lock(app_.timings_mutex_);
        const auto it = app_.timings_.find(timing_);
        remembered = activity::blend(it != app_.timings_.end() ? std::optional(it->second) : std::nullopt, ms);
        app_.timings_[timing_] = remembered;
    }
    app_.host_.defer([&app = app_, key = timing_, remembered] {
        app.state_.timings[key] = remembered;
        app.save_state();
    });
}

void App::Activity::text(std::string text) {
    std::lock_guard lock(mutex_);
    text_ = std::move(text);
    send(false);
}

void App::Activity::progress(std::uint64_t done, std::uint64_t total, std::string text) {
    std::lock_guard lock(mutex_);
    const bool first = !total_;
    done_ = done;
    total_ = total;
    if (!text.empty()) text_ = std::move(text);
    send(first); // the bar switches from the estimate to real numbers at once
}

void App::Activity::detail(std::string detail) {
    std::lock_guard lock(mutex_);
    detail_ = std::move(detail);
    send(true);
}

void App::Activity::timing(std::string key) {
    std::lock_guard lock(mutex_);
    timing_ = std::move(key);
    Value v = Value::make_object();
    v.set("type", str("activity"));
    v.set("id", num(id_));
    {
        std::lock_guard times(app_.timings_mutex_);
        const auto it = app_.timings_.find(timing_);
        v.set("estimate", num(it != app_.timings_.end() ? it->second : activity::guess(timing_)));
    }
    app_.post(v);
}

void App::Activity::send(bool force) {
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - sent_ < std::chrono::milliseconds(100)) return;
    sent_ = now;
    Value v = Value::make_object();
    v.set("type", str("activity"));
    v.set("id", num(id_));
    v.set("text", str(text_));
    if (!detail_.empty()) v.set("detail", str(detail_));
    if (done_ && total_ && *total_ > 0) {
        v.set("done", num(static_cast<double>(*done_)));
        v.set("total", num(static_cast<double>(*total_)));
    }
    app_.post(v);
}

// ---- previews ------------------------------------------------------------------------------

namespace {

// How long the viewer keeps its seat on a server after its last preview.
constexpr auto preview_seat_idle = std::chrono::seconds(60);
// Extra connections for a big image or PDF (ssh::ReadOptions::extra_from), on free seats only: a
// Storage Box connection gives 0.7-2.5 MiB/s, one of Main's reaches its link.
constexpr int preview_extra_box = 4;
constexpr int preview_extra_other = 2;

// Play's connections, for the whole film (decision 2, "burst all the time"): 8 on a Storage Box,
// whose connections give 0.7-2.5 MiB/s each, 2 elsewhere (one of Main's reaches its link, the
// other stays idle for a seek); fewer beside downloads from that server, as Play's High share
// allows. Its logins 8 at once on a Storage Box (box part 1: accepted, 0.7 s sooner than 4), the
// pool's 4 elsewhere (Main's fail2ban). Read-ahead in front of each of the player's requests. READs
// in flight per connection: 4 on a Storage Box (as fast there as 8, and half the wait behind them),
// 8 elsewhere (Main's link fills at 8); connections kept idle for a seek once the warm part is in.
constexpr int play_burst_box = 8;
constexpr int play_burst_other = 2;
constexpr int play_logins_box = 8;
constexpr std::uint64_t play_ahead_box = std::uint64_t{32} << 20;
constexpr std::uint64_t play_ahead_other = std::uint64_t{16} << 20;
constexpr int play_depth_box = 4;
constexpr int play_depth_other = 8;
constexpr int play_spares_box = 2;
constexpr int play_spares_other = 1;

} // namespace

void App::queue_preview(std::function<void()> job, bool first) {
    {
        std::scoped_lock lock(preview_mutex_);
        if (first) preview_queue_.push_front(std::move(job));
        else preview_queue_.push_back(std::move(job));
        while (preview_workers_.size() < 2) {
            preview_workers_.emplace_back([this](const std::stop_token& stop) { run_preview_jobs(stop); });
        }
    }
    preview_wake_.notify_all();
}

void App::run_preview_jobs(const std::stop_token& stop) {
    while (true) {
        std::function<void()> job;
        {
            std::unique_lock lock(preview_mutex_);
            if (!preview_wake_.wait(lock, stop, [this] { return !preview_queue_.empty(); })) return;
            if (stop.stop_requested()) return; // the window is closing: nothing more is taken (review G42)
            job = std::move(preview_queue_.front());
            preview_queue_.pop_front();
        }
        job();
    }
}

void App::preview_request(const std::string& url, std::function<void(WebResource)> reply) {
    auto request = preview::parse_url(url);
    if (!request) return reply(preview::failed(400, "not a preview"));
    const bool shown = !request->ahead;
    // Kept, or joining the fetch of the same file (review G50), or a new one: the one shown goes
    // first, neighbours fetched ahead wait their turn.
    if (auto job = preview_jobs_.ask(std::move(*request), std::move(reply))) {
        queue_preview([this, job] { run_preview(job); }, shown);
    }
}

void App::run_preview(const std::shared_ptr<preview::Previews::Job>& job) {
    if (!preview_jobs_.begin(job)) return;
    PreviewHold hold;
    const auto fetched = preview_answer(*job, hold);
    preview_jobs_.end(job, fetched);
    // After the answer: the connections go back to the pool (the extra ones' seats with them),
    // then the viewer's seat.
    hold.reader.reset();
    if (hold.seated) return_viewer_seat(job->server);
}

bool App::take_viewer_seat(const std::string& server, const preview::Previews::Job& job, const std::stop_token& stop) {
    std::unique_lock lock(preview_mutex_);
    ViewerSeat& seat = viewer_seats_[server];
    if (seat.id == 0) {
        seat.id = connections_.new_id();
        seat.idle_since = std::chrono::steady_clock::now();
    }
    connections_.join(server, seat.id, 4); // High; nothing when it has joined already
    // Counted for the whole wait, so release_viewer_seats leaves the seat's id alone meanwhile.
    ++seat.taking;
    const bool got = [&] {
        for (;;) {
            if (stop.stop_requested()) return false;
            if (seat.held > seat.in_use) return true; // the one it kept
            if (connections_.try_acquire(server, seat.id)) {
                ++seat.held;
                return true;
            }
            if (!preview_jobs_.ahead(job)) {
                // Shown now: it waits as a download does, and downloads above their share step aside.
                const int id = seat.id;
                lock.unlock();
                const bool acquired = connections_.acquire(server, id, stop, false);
                lock.lock();
                if (!acquired) return false;
                ++seat.held;
                return true;
            }
            // Fetched ahead: the kept seat once it's back, or one that comes free.
            preview_wake_.wait_for(lock, stop, std::chrono::milliseconds(250), [] { return false; });
        }
    }();
    --seat.taking;
    if (got) ++seat.in_use;
    return got;
}

void App::return_viewer_seat(const std::string& server) {
    {
        std::scoped_lock lock(preview_mutex_);
        auto& seat = viewer_seats_[server];
        seat.in_use = std::max(seat.in_use - 1, 0);
        // One is kept while the viewer is idle; any more go back now.
        if (const int keep = std::max(seat.in_use, 1); seat.held > keep) {
            connections_.release(server, seat.id, seat.held - keep);
            seat.held = keep;
        }
        seat.idle_since = std::chrono::steady_clock::now();
    }
    preview_wake_.notify_all();
}

void App::release_viewer_seats() {
    std::scoped_lock lock(preview_mutex_);
    const auto now = std::chrono::steady_clock::now();
    for (auto& [server, seat] : viewer_seats_) {
        if (seat.id == 0 || seat.in_use > 0 || seat.taking > 0) continue;
        if (now - seat.idle_since < preview_seat_idle) continue;
        if (seat.held > 0) connections_.release(server, seat.id, seat.held);
        connections_.leave(server, seat.id);
        seat.held = 0;
        seat.id = 0;
    }
}

void App::forget_server_previews(const std::string& server) {
    preview_jobs_.forget_server(server);
    pools_.forget_login_failure(server);
}

void App::forget_server(const std::string& server) {
    forget_server_previews(server);
    pools_.forget(server);
    {
        std::lock_guard lock(mutex_);
        learned_.erase(server);
    }
    // "<step>:<server>"; a copy of the name, for the UI thread's turn below.
    const auto of_server = [server](const auto& kv) {
        const auto colon = kv.first.find(':');
        return colon != std::string::npos && std::string_view(kv.first).substr(colon + 1) == server;
    };
    {
        std::lock_guard lock(timings_mutex_);
        std::erase_if(timings_, of_server);
    }
    host_.defer([this, of_server] {
        if (std::erase_if(state_.timings, of_server) > 0) save_state();
    });
}

preview::Fetched App::preview_answer(const preview::Previews::Job& job, PreviewHold& hold) {
    auto fail = [](preview::Answer answer) {
        return preview::Fetched{.answer = std::move(answer), .stamp = {}, .keep = false};
    };
    const auto& remote = job.server;
    const auto& path = job.path;
    const auto stop = job.stop.get_token();
    const auto kind = preview::kind_of(path);
    if (preview::max_bytes(kind) == 0) return fail(preview::failed(415, "This kind of file has no preview."));
    auto ctx = engine::load_context(config_path_, remote);
    if (!ctx) return fail(preview::failed(500, ctx.error()));
    // Through grab's own connections (RCLONE-REPLACEMENT.md, phase 6): its pool, shared with the
    // downloads, Browse and verification, logs in once.
    auto target = engine::ssh_target(*ctx);
    if (!target) return fail(preview::failed(502, std::format("Can't preview from {}: {}", remote, target.error())));
    connections_.configure(remote, connection_budget(remote, ctx->remote.host, ctx->settings.max_connections));
    hold.pool = pools_.get(remote, *target, connections_.budget(remote));
    const auto name = remote_basename(path);
    Activity loading(*this, std::format("Opening {} on {}…", name, remote), std::string(), preview_jobs_.ahead(job));
    if (!take_viewer_seat(remote, job, stop)) return fail(preview::cancelled());
    hold.seated = true;
    int seat_id = 0;
    {
        std::scoped_lock lock(preview_mutex_);
        seat_id = viewer_seats_[remote].id;
    }

    ssh::ReadOptions o;
    o.stop = stop;
    // A big image or PDF also over free seats; text reads its first MiB only.
    o.extra = kind == preview::Kind::text ? 0
              : budget::is_storage_box(ctx->remote.host) ? preview_extra_box
                                                         : preview_extra_other;
    o.seat = [this, remote, seat_id] { return connections_.try_acquire(remote, seat_id); };
    o.seat_back = [this, remote, seat_id] { connections_.release(remote, seat_id); };
    o.yield = [this, remote, seat_id] { return connections_.yield_if_over_share(remote, seat_id); };
    // Never called once read() has returned, so `loading` may go first.
    o.progress = [&loading, &name](std::uint64_t done, std::uint64_t total) {
        loading.progress(done, total,
                         std::format("Loading {}: {} of {}", name, util::format_size(done), util::format_size(total)));
    };
    hold.reader.emplace(hold.pool, rclone_enc::raw_path(path), std::move(o));
    auto fetched = preview::fetch(*hold.reader, remote, path);
    if (fetched.answer.status != 200 && !stop.stop_requested()) loading.failed();
    return fetched;
}

void App::play(const Value& msg) {
    const std::string remote = msg.string_of("remote").value_or("");
    const std::string path = msg.string_of("path").value_or("");
    if (remote.empty() || path.empty()) return;
    {
        std::scoped_lock lock(play_mutex_);
        play_next_ = PlayJob{.remote = remote, .path = path};
        play_current_.request_stop(); // one still preparing gives way: no player opens for it
        if (!play_thread_.joinable()) {
            play_thread_ = std::jthread([this](const std::stop_token& stop) { run_plays(stop); });
        }
    }
    play_wake_.notify_all();
}

void App::run_plays(const std::stop_token& stop) {
    while (true) {
        PlayJob job;
        std::stop_source current;
        {
            std::unique_lock lock(play_mutex_);
            if (!play_wake_.wait(lock, stop, [this] { return play_next_.has_value(); })) return;
            job = std::move(*play_next_);
            play_next_.reset();
            play_current_ = current;
        }
        // The window closing stops it as well (review G42).
        const std::stop_callback closing(stop, [&current] { current.request_stop(); });
        run_play(job, current.get_token());
    }
}

void App::run_play(const PlayJob& job, const std::stop_token& stop) {
    const auto& remote = job.remote;
    const auto name = remote_basename(job.path);
    Value v = Value::make_object();
    auto fail = [&](const std::string& message) {
        v.set("type", str("error"));
        v.set("message", str(message));
        post(v);
    };
    // Only videos and audio go to a player (review G127).
    if (const auto kind = preview::kind_of(name); kind != preview::Kind::video && kind != preview::Kind::audio) {
        return fail(std::format("Can't play {}: only videos and audio play.", name));
    }
    auto ctx = engine::load_context(config_path_, remote);
    if (!ctx) return fail(ctx.error());
    auto target = engine::ssh_target(*ctx);
    if (!target) return fail(std::format("Can't play {}: {}", name, target.error()));
    connections_.configure(remote, connection_budget(remote, ctx->remote.host, ctx->settings.max_connections));
    const bool box = budget::is_storage_box(ctx->remote.host);
    play::Server::File file{.server = remote,
                            .pool = pools_.get(remote, *target, connections_.budget(remote)),
                            .path = rclone_enc::raw_path(job.path),
                            .name = name,
                            .connections = box ? play_burst_box : play_burst_other,
                            .logins_at_once = box ? play_logins_box : 0,
                            .ahead = box ? play_ahead_box : play_ahead_other,
                            .depth = box ? play_depth_box : play_depth_other,
                            .spares = box ? play_spares_box : play_spares_other};
    std::string address;
    {
        // The player starts once the warm part is in (decision 3): the status bar shows it coming.
        Activity preparing(*this, std::format("Preparing {}…", name), "warm:" + remote);
        auto got = play_server_.play(
            std::move(file),
            [&preparing, &name](std::uint64_t done, std::uint64_t total) {
                preparing.progress(done, total, std::format("Preparing {}: {} of {}", name, util::format_size(done),
                                                            util::format_size(total)));
            },
            stop);
        if (!got) {
            preparing.failed(); // a stopped run's time isn't a warm part's
            if (stop.stop_requested()) return; // another Play, or the window closing
            return fail(std::format("Can't play {}: {}.", name, got.error()));
        }
        address = std::move(*got);
    }
    // Nothing stopped it meanwhile: only then the player (review G42).
    if (stop.stop_requested()) return;
    const auto dot = name.rfind('.');
    auto program = open_with_player(dot == std::string::npos ? "" : util::to_lower(name.substr(dot + 1)), address);
    if (!program) return fail(program.error());
    v.set("type", str("played"));
    v.set("name", str(name));
    v.set("program", str(*program));
    post(v);
}

void App::post(const Value& message) const { host_.post(json::stringify(message)); }

bool App::downloads_active() const {
    std::lock_guard lock(mutex_);
    return std::ranges::any_of(items_, [](const auto& i) {
        return i->status == Status::queued || i->status == Status::running;
    });
}

App::QueueSummary App::summary() const {
    QueueSummary s;
    std::lock_guard lock(mutex_);
    for (const auto& i : items_) {
        if (i->status == Status::running) ++s.running;
        else if (i->status == Status::queued) ++s.queued;
        else if (i->status == Status::paused) ++s.paused;
        else continue;
        s.bytes += i->bytes;
        s.total += std::max(i->total, i->bytes);
    }
    return s;
}

// ---- messages -----------------------------------------------------------------------------

void App::on_message(const std::string& text) {
    auto msg = json::parse(text);
    if (!msg || msg->kind != Value::Kind::object) return;
    const std::string type = msg->string_of("type").value_or("");

    if (type == "init") {
        // A page load: it asks again for the files of the folders it shows, and gets the total
        // speed at the next flush.
        page_ready_ = true;
        watches_.clear();
        total_sent_.reset();
        send_init();
        post_update(); // the page may have been reloaded
        maybe_check_updates();
        post_import_banner();
        if (!notices_.empty()) {
            Value v = Value::make_object();
            v.set("type", str("error"));
            v.set("message", str(util::join(std::exchange(notices_, {}), " ")));
            post(v);
        }
    } else if (type == "updateCheckNow") {
        start_update_check(true);
    } else if (type == "setUpdateCheck") {
        state_.update_check = bool_of(*msg, "on");
        save_state();
    } else if (type == "updateStart") {
        start_update();
    } else if (type == "updateRestart") {
        restart_for_update();
    } else if (type == "updateDismiss") {
        dismiss_update();
    } else if (type == "openReleasePage") {
        open_release_page();
    } else if (type == "prefs") {
        if (auto remote = msg->string_of("remote")) state_.last_remote = *remote;
        // The results' sort: null for the default order; an invalid one changes nothing.
        if (const Value* sort = msg->find("sort")) {
            if (sort->kind == Value::Kind::null) state_.sort.reset();
            else if (auto order = sort_from_json(*sort)) state_.sort = std::move(order);
        }
        // The split between the list and Downloads, as the panel's share: null for the default;
        // anything else that isn't a number changes nothing.
        if (const Value* split = msg->find("split")) {
            if (split->kind == Value::Kind::null) state_.split.reset();
            else if (auto share = split_from_json(*split)) state_.split = share;
        }
        // The Downloads table's sort and column widths, as a whole.
        if (const Value* view = msg->find("downloads")) state_.downloads = downloads_view_from_json(*view);
        save_state();
    } else if (type == "search") {
        start_search(*msg);
    } else if (type == "previewCancel") {
        // The viewer moved or closed: {keep: [the preview URLs it still wants]}.
        std::set<std::string> keep;
        if (const Value* urls = msg->find("keep"); urls != nullptr && urls->kind == Value::Kind::array) {
            for (const Value& url : urls->items) {
                if (url.kind != Value::Kind::string) continue;
                if (auto at = preview::parse_url(url.str)) keep.insert(preview::key_of(at->server, at->path));
            }
        }
        preview_jobs_.cancel(keep); // they stop at their next READs (review G50)
        preview_wake_.notify_all(); // one waiting for a seat
    } else if (type == "play") {
        play(*msg);
    } else if (type == "browse") {
        run_browse(int_of(*msg, "id"), msg->string_of("remote").value_or(""), msg->string_of("path").value_or(""),
                   bool_of(*msg, "top"), bool_of(*msg, "live"), false, bool_of(*msg, "typed"));
    } else if (type == "suggest") {
        suggest(msg->string_of("remote").value_or(""), msg->string_of("path").value_or(""));
    } else if (type == "refreshLists") {
        const std::string remote = msg->string_of("remote").value_or("");
        if (remote == "*") {
            if (auto cfg = load_grab_config(config_path_)) {
                for (const auto& r : cfg->remotes) start_refresh(r.name, true);
            }
        } else if (!remote.empty()) {
            start_refresh(remote, true);
        }
    } else if (type == "cancelSearch") {
        if (search_thread_.joinable()) search_thread_.request_stop();
    } else if (type == "pickFolder") {
        // Not from inside WebView2's event handler: the dialog runs its own message loop.
        std::wstring current = util::to_wide(msg->string_of("current").value_or(""));
        std::string purpose = msg->string_of("purpose").value_or("dest");
        host_.defer([this, current = std::move(current), purpose = std::move(purpose)] { pick_folder(current, purpose); });
    } else if (type == "enqueue") {
        enqueue(*msg);
    } else if (type == "cancelItem") {
        cancel_items(int_of(*msg, "id", -1));
    } else if (type == "pauseItem") {
        pause_items(int_of(*msg, "id", -1));
    } else if (type == "resumeItem") {
        resume_items(int_of(*msg, "id", -1));
    } else if (type == "resolveExisting") {
        if (const auto choice = existing::parse(msg->string_of("choice").value_or(""))) {
            resolve_existing(int_of(*msg, "id", -1), *choice);
        }
    } else if (type == "cancelAll") {
        cancel_items(0);
    } else if (type == "pauseAll") {
        pause_items(0);
    } else if (type == "resumeAll") {
        resume_items(0);
    } else if (type == "watchFiles") {
        watch_files(int_of(*msg, "id", -1));
    } else if (type == "unwatchFiles") {
        watches_.erase(int_of(*msg, "id", -1));
    } else if (type == "fileAction") {
        file_action(*msg);
    } else if (type == "setFilePriority") {
        set_file_priority(*msg);
    } else if (type == "showFile") {
        const int id = int_of(*msg, "id", -1);
        const int gen = int_of(*msg, "gen", -1);
        const int index = int_of(*msg, "index", -1);
        if (index >= 0) host_.defer([this, id, gen, index] { show_file(id, gen, static_cast<std::size_t>(index)); });
    } else if (type == "openFile") {
        // Not from inside WebView2's event handler: Windows' own dialogs (Open with) run their
        // own message loop.
        const int id = int_of(*msg, "id", -1);
        const int gen = int_of(*msg, "gen", -1);
        const int index = int_of(*msg, "index", -1);
        const bool run = bool_of(*msg, "run");
        host_.defer([this, id, gen, index, run] {
            open_finished(id, gen, index >= 0 ? std::optional<std::size_t>(index) : std::nullopt, run);
        });
    } else if (type == "openDir") {
        const int id = int_of(*msg, "id", -1);
        host_.defer([this, id] { open_dir(id); });
    } else if (type == "setExpanded") {
        // The user's ▸/▾ on a folder, kept in queue.json.
        const int id = int_of(*msg, "id", -1);
        const bool open = bool_of(*msg, "open");
        {
            std::lock_guard lock(mutex_);
            for (auto& item : items_) {
                if (item->id == id) item->expanded = open;
            }
        }
        request_save();
        dirty_ = true;
    } else if (type == "moveItem") {
        move_item(int_of(*msg, "id", -1), int_of(*msg, "before", 0));
    } else if (type == "setPriority") {
        set_priority(int_of(*msg, "id", -1), weight_of(msg->string_of("priority").value_or("normal")));
    } else if (type == "setLimit") {
        const Value* mibps = msg->find("mibps");
        set_limit(bool_of(*msg, "on"),
                  mibps != nullptr && mibps->kind == Value::Kind::number ? mibps->number : state_.limit_mibps,
                  int_of(*msg, "token"));
    } else if (type == "setRecent") {
        if (const Value* items = msg->find("items")) save_recent(recent_path_, *items);
    } else if (type == "setVerify") {
        state_.verify_downloads = bool_of(*msg, "on");
        verify_downloads_ = state_.verify_downloads;
        save_state();
    } else if (type == "setSchedule") {
        if (const Value* rules = msg->find("rules")) set_schedule(*rules);
    } else if (type == "setParallel") {
        set_parallel(int_of(*msg, "value", state_.parallel));
    } else if (type == "retryItem") {
        retry_item(int_of(*msg, "id"));
    } else if (type == "clearFinished") {
        clear_finished();
    } else if (type == "openFolder") {
        const int id = int_of(*msg, "id");
        host_.defer([this, id] { open_item_folder(id); });
    } else if (type == "pickFile") {
        std::wstring current = util::to_wide(msg->string_of("current").value_or(""));
        host_.defer([this, current = std::move(current)] { pick_file(current); });
    } else if (type == "copyText") {
        copy_text(msg->string_of("text").value_or(""));
    } else if (type == "openConfig") {
        open_config();
    } else if (type == "servers") {
        settings_task([this](std::stop_token) {
            if (auto env = load_env()) post_servers(*env);
            else settings_error(env.error());
        });
    } else if (type == "serverSave") {
        save_server(*msg);
    } else if (type == "serverConfirm") {
        std::optional<Pending> pending;
        {
            std::lock_guard lock(settings_mutex_);
            pending.swap(pending_);
        }
        if (!pending || !bool_of(*msg, "trust")) return; // not trusted: the secret is wiped here
        settings_task([this, p = std::make_shared<Pending>(std::move(*pending))](std::stop_token stop) {
            apply_pending(std::move(*p), stop);
        });
    } else if (type == "serverCancel") {
        cancel_pending();
    } else if (type == "serverTrust") {
        const std::string name = msg->string_of("name").value_or("");
        settings_task([this, name](std::stop_token stop) {
            auto env = load_env();
            if (!env) return settings_error(env.error());
            const auto list = server_ops::list_servers(*env);
            auto it = std::ranges::find(list, name, &server_ops::ServerInfo::name);
            if (it == list.end()) return settings_error("no server called " + name);
            if (!it->error.empty()) return settings_error(it->error);
            // Refused for a server grab doesn't connect to, before anything dials.
            auto reach = server_ops::reach_for(*env, name);
            if (!reach) return settings_error(reach.error());
            auto keys = server_ops::scan_host_keys(it->host, it->port, stop, *reach);
            if (stop.stop_requested()) return;
            if (!keys) return settings_error(keys.error());
            Pending p;
            p.kind = Pending::Kind::trust;
            p.server.name = name;
            p.server.host = it->host;
            p.server.port = it->port;
            p.keys = std::move(*keys);
            post_host_keys(p);
            std::lock_guard lock(settings_mutex_);
            pending_ = std::move(p);
        });
    } else if (type == "serverTest") {
        const std::string name = msg->string_of("name").value_or("");
        settings_task([this, name](std::stop_token stop) { test_server(name, stop); });
    } else if (type == "serverRemove" || type == "serverDefault") {
        const bool remove = type == "serverRemove";
        const std::string name = msg->string_of("name").value_or("");
        settings_task([this, remove, name](std::stop_token) {
            auto env = load_env();
            if (!env) return settings_error(env.error());
            if (remove) {
                auto note = server_ops::remove_server(*env, name);
                if (!note) return settings_error(note.error());
                if (!note->empty()) settings_error(*note);
                forget_server(name);
            } else if (auto done = server_ops::set_default(*env, name); !done) {
                return settings_error(done.error());
            }
            servers_changed();
        });
    }
}

void App::send_init() {
    Value init = Value::make_object();
    init.set("type", str("init"));
    init.set("version", str(GRAB_VERSION));
    init.set("updateCheck", Value::make_bool(state_.update_check));
    init.set("verifyDownloads", Value::make_bool(state_.verify_downloads));
    init.set("configPath", str(util::path_to_utf8(config_path_)));
    init.set("recent", load_recent(recent_path_));

    Value remotes = Value::make_array();
    std::string selected;
    // No grab.conf yet is a first run, not an error: Settings creates it with the first server.
    std::error_code ec;
    auto cfg = std::filesystem::exists(config_path_, ec) ? load_grab_config(config_path_)
                                                          : std::expected<GrabConfig, std::string>(GrabConfig{});
    if (!cfg) {
        init.set("configError", str(cfg.error()));
    } else {
        for (const auto& r : cfg->remotes) {
            Value entry = Value::make_object();
            entry.set("name", str(r.name));
            // Without the import, which the settings thread runs: this is the UI thread.
            auto ctx = engine::load_context(config_path_, r.name, false);
            if (ctx) {
                entry.set("host", str(ctx->remote.host));
                entry.set("user", str(ctx->remote.user));
                // find, walk or probe (search_method).
                entry.set("method", str(std::string(search_method(ctx->settings, ctx->remote))));
            } else {
                entry.set("error", str(ctx.error()));
            }
            remotes.push(std::move(entry));
        }
        auto has = [&](const std::string& name) {
            return std::ranges::any_of(cfg->remotes, [&](const auto& r) { return r.name == name; });
        };
        if (has(state_.last_remote) || (state_.last_remote == "*" && cfg->remotes.size() > 1)) selected = state_.last_remote;
        else if (cfg->default_remote && has(*cfg->default_remote)) selected = *cfg->default_remote;
        else if (!cfg->remotes.empty()) selected = cfg->remotes.front().name;
    }
    init.set("remotes", std::move(remotes));
    init.set("remote", str(selected));
    if (state_.sort) init.set("sort", sort_to_json(*state_.sort));
    // The page takes these from its first init only: init comes again after every server change.
    if (state_.split) init.set("split", num(*state_.split));
    if (state_.downloads != DownloadsView{}) init.set("downloads", downloads_view_to_json(state_.downloads));
    Value dests = Value::make_object();
    for (const auto& [remote, path] : state_.destinations) dests.set(remote, str(path));
    init.set("destinations", std::move(dests));
    init.set("defaultDest", str(known_folder(FOLDERID_Downloads)));
    init.set("parallel", num(state_.parallel));
    Value limit = limit_message(schedule::local_now());
    limit_posted_ = json::stringify(limit);
    init.set("limit", std::move(limit));
    post(init);
    queue_changed_ = true;
    flush();
}

void App::start_search(const Value& msg) {
    engine::SearchRequest req;
    req.target = msg.string_of("query").value_or("");
    req.mode = mode_of(msg);
    req.exact = bool_of(msg, "exact");
    if (int depth = int_of(msg, "depth"); depth > 0) req.depth = depth;
    std::optional<std::string> under;
    if (const Value* u = msg.find("under"); u != nullptr && u->kind == Value::Kind::string) under = u->str;
    run_search(int_of(msg, "id"), msg.string_of("remote").value_or(""), req, false, msg.string_of("mode") == "any",
               under);
}

// A server's search answers from its saved file list when it has one that fits; a list older
// than file_list::fresh_for, or an older grab's without dates, is refreshed in the background,
// and the search then runs again. A server without a list is searched live (one server) while
// its list is fetched for next time.
void App::run_search(int id, const std::string& remote, const engine::SearchRequest& req, bool update, bool both,
                     const std::optional<std::string>& under) {
    // A new search replaces the previous one; stopping ends its find or walk at once.
    if (search_thread_.joinable()) {
        search_replaced_ = true;
        search_thread_.request_stop();
        search_thread_.join();
        search_replaced_ = false;
    }
    {
        std::lock_guard lock(lists_mutex_);
        last_search_ = LastSearch{id, remote, req, both, under};
    }
    search_thread_ = std::jthread([this, id, remote, req, update, both, under](std::stop_token stop) {
        const auto t0 = std::chrono::steady_clock::now();
        auto fail = [&](const std::string& message, bool cancelled) {
            if (cancelled && search_replaced_) return; // the next search reports instead
            Value v = Value::make_object();
            v.set("type", str("searchError"));
            v.set("id", num(id));
            v.set("message", str(message));
            v.set("cancelled", Value::make_bool(cancelled));
            post(v);
        };
        if (util::trim(req.target).empty()) return fail("type something to search for", false);

        const bool all = remote == "*" && !under;
        std::vector<std::string> servers;
        if (all) {
            auto cfg = load_grab_config(config_path_);
            if (!cfg) return fail(cfg.error(), false);
            for (const auto& r : cfg->remotes) servers.push_back(r.name);
        } else {
            servers.push_back(remote);
        }
        proc::RunOptions run;
        run.stop = stop;
        run.detached = true;
        const auto now = unix_now();
        std::vector<file_list::ServerHit> hits;
        Value lists = Value::make_array(); // per server: where the answer came from
        Value v = Value::make_object();
        // The kinds asked for: one, or folders and files (`both`; folder hits marked).
        const std::vector<Mode> kinds = both ? std::vector<Mode>{Mode::folder, Mode::file} : std::vector<Mode>{req.mode};
        auto add = [&](std::optional<engine::SearchResult>& into, engine::SearchResult got, Mode kind) {
            if (both) {
                for (auto& h : got.hits) h.dir = kind == Mode::folder;
            }
            if (!into) {
                into = std::move(got);
                return;
            }
            std::ranges::move(got.hits, std::back_inserter(into->hits));
        };
        std::optional<Activity> across; // All servers: by server
        if (servers.size() > 1) across.emplace(*this, "Searching all servers…", std::string());
        std::size_t index = 0;
        for (const auto& server : servers) {
            if (stop.stop_requested()) return fail("search cancelled", true);
            if (across) {
                across->progress(index, servers.size(),
                                 std::format("Searching {} ({} of {})…", server, index + 1, servers.size()));
            }
            ++index;
            Value status = Value::make_object();
            status.set("remote", str(server));
            auto ctx = engine::load_context(config_path_, server);
            if (!ctx) {
                if (!all) return fail(ctx.error(), false);
                status.set("error", str(ctx.error()));
                lists.push(std::move(status));
                continue;
            }
            if (under) {
                auto below = search_below(*ctx, server, req, *under, update, run, status);
                if (stop.stop_requested()) return fail("search cancelled", true);
                if (!below) return fail(below.error(), false);
                for (auto& h : *below) hits.push_back(file_list::ServerHit{server, std::move(h)});
                lists.push(std::move(status));
                continue;
            }
            std::optional<engine::SearchResult> found;
            const bool listable = file_list::can_answer(*ctx, req);
            if (listable) {
                std::shared_ptr<const file_list::List> list;
                {
                    std::lock_guard lock(lists_mutex_);
                    auto& s = list_slot(server);
                    if (s.list && s.list->fingerprint == file_list::fingerprint(*ctx)) list = s.list;
                }
                if (!update && (!list || !file_list::fresh(*list, now))) start_refresh(server);
                if (list) {
                    for (const Mode kind : kinds) {
                        auto r = req;
                        r.mode = kind;
                        add(found, file_list::search(*ctx, *list, r), kind);
                    }
                    status.set("age", num(static_cast<double>(now - list->fetched)));
                    set_notes(status, list->notes);
                }
                std::lock_guard lock(lists_mutex_);
                const auto& s = list_slot(server);
                status.set("refreshing", Value::make_bool(s.refreshing));
                if (!s.error.empty()) status.set("error", str(s.error));
            }
            // One server without a usable list, or a search lists can't answer: live, as before.
            if (!found && (!all || !listable)) {
                bool with_find = false; // once find is chosen: it counts matches, the walk entries
                std::string doing = std::format("Searching {}", server);
                Activity act(*this, doing + "…", "search:" + server);
                auto counted_text = [&](std::size_t n) {
                    act.text(with_find ? std::format("{}… {} found so far", doing, thousands(n))
                                       : std::format("{}… {} looked at", doing, count_of(n, "entry", "entries")));
                };
                // The seat is one connection, widened for a walk (review G191); one find or walk
                // lists both kinds (G197).
                auto r = req;
                std::optional<fetch::Listing> walk;
                std::optional<std::string> no_way; // grab's client couldn't be set up
                if (auto side = grab_side(*ctx, server, walk, stop)) {
                    r.grab = std::move(*side);
                    r.grab.on_lookup = [&](const ssh::Lookup& way) {
                        with_find = way.find;
                        doing = std::format("Searching {} {}", server, way.find ? "with find" : "by listing folders");
                        act.timing(std::format("{}:{}", way.find ? "find" : "walk", server));
                        act.text(doing + "…");
                    };
                    r.grab.progress = counted_text;
                } else {
                    no_way = side.error();
                }
                const auto hook = [&](const std::vector<std::string>& argv) { act.detail(quote::display_cmdline(argv)); };
                std::vector<std::pair<Mode, std::expected<engine::SearchResult, std::string>>> results;
                if (no_way) {
                    results.emplace_back(kinds.front(), std::unexpected(*no_way));
                } else if (kinds.size() == 2) {
                    if (auto walked = engine::search_both(*ctx, r, run, hook)) {
                        results.emplace_back(Mode::folder, std::move(walked->first));
                        results.emplace_back(Mode::file, std::move(walked->second));
                    } else {
                        results.emplace_back(Mode::folder, std::unexpected(walked.error()));
                    }
                } else {
                    r.mode = kinds.front();
                    results.emplace_back(kinds.front(), engine::search(*ctx, r, run, hook));
                }
                walk.reset();
                for (auto& [kind, live] : results) {
                    if (!live) act.failed();
                    if (stop.stop_requested()) return fail("search cancelled", true);
                    if (!live) {
                        if (!all) return fail(live.error(), false);
                        status.set("error", str(live.error()));
                        found.reset();
                        break;
                    }
                    if (kind == results.front().first) set_notes(status, live->notes); // one lookup's, once
                    add(found, std::move(*live), kind);
                    status.set("live", Value::make_bool(true));
                }
            }
            if (found) {
                if (!all) {
                    v.set("maxDepth", num(found->max_depth));
                    Value roots = Value::make_array();
                    for (const auto& r : found->roots) roots.push(str(r.empty() ? "~" : r));
                    v.set("roots", std::move(roots));
                }
                for (auto& h : found->hits) hits.push_back(file_list::ServerHit{server, std::move(h)});
            }
            lists.push(std::move(status));
        }
        if (all || both || under) file_list::rank_across(hits, req); // folders and files together

        v.set("type", str("searchResult"));
        v.set("id", num(id));
        v.set("update", Value::make_bool(update));
        if (under) v.set("under", str(*under));
        if (both) v.set("both", Value::make_bool(true));
        if (all || under) {
            v.set("maxDepth", num(0));
            v.set("roots", Value::make_array());
        }
        v.set("lists", std::move(lists));
        Value out = Value::make_array();
        for (const auto& [server, h] : hits) {
            Value hit = Value::make_object();
            hit.set("remote", str(server));
            hit.set("path", str(h.path));
            hit.set("name", str(h.name));
            hit.set("size", h.size ? num(static_cast<double>(*h.size)) : Value{});
            if (h.dir) hit.set("dir", Value::make_bool(true));
            if (h.modified) hit.set("modified", num(static_cast<double>(*h.modified)));
            out.push(std::move(hit));
        }
        v.set("hits", std::move(out));
        v.set("elapsedMs", num(static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                       std::chrono::steady_clock::now() - t0)
                                                       .count())));
        post(v);
    });
}

App::ListSlot& App::list_slot(const std::string& server) {
    auto& s = lists_[server];
    if (!s.loaded) {
        s.loaded = true;
        if (auto list = file_list::load(file_list::path_for(lists_dir_, server))) {
            s.list = std::make_shared<const file_list::List>(std::move(*list));
        }
    }
    return s;
}

void App::start_refresh(const std::string& server, bool asked) {
    std::lock_guard lock(lists_mutex_);
    auto& s = list_slot(server);
    if (s.refreshing) return;
    // After a failed one, not again by itself for a while (1, 2, 4... up to 15 minutes): each is
    // a whole walk of the server; Refresh still asks at once (review G196).
    if (!asked && std::chrono::steady_clock::now() < s.retry_after) return;
    s.refreshing = true;
    // The previous refresh of this server, if any, has finished: replacing it joins it at once.
    const std::size_t before = s.list ? s.list->entries.size() : 0; // the last list's entries
    refresh_threads_[server] = std::jthread([this, server, before](std::stop_token stop) {
        proc::RunOptions run;
        run.stop = stop;
        run.detached = true;
        const std::string base = std::format("{} {}'s file list", before > 0 ? "Refreshing" : "Building", server);
        std::string doing = base; // and how, once that's chosen
        Activity act(*this, doing + "…", "list:" + server, true);
        if (before > 0) act.progress(0, before); // against the last list's count, from the start
        auto counted_text = [&](std::uint64_t n) {
            if (before > 0) {
                act.progress(std::min<std::uint64_t>(n, before), before,
                             std::format("{}: {} of ~{} entries", doing, thousands(n), thousands(before)));
            } else {
                act.text(std::format("{}: {}", doing, count_of(n, "entry", "entries")));
            }
        };
        auto ctx = engine::load_context(config_path_, server);
        // The seat is one connection, widened for a walk (review G191).
        std::optional<fetch::Listing> walk;
        engine::GrabSide side;
        std::expected<void, std::string> ready;
        if (!ctx) {
            ready = std::unexpected(ctx.error());
        } else if (auto got = grab_side(*ctx, server, walk, stop)) {
            side = std::move(*got);
            side.on_lookup = [&](const ssh::Lookup& way) {
                doing = std::format("{} {}", base, way.find ? "with find" : "by listing folders");
                act.timing(std::format("{}:{}", way.find ? "list.find" : "list.walk", server));
                if (before > 0) act.progress(0, before, doing + "…");
                else act.text(doing + "…");
            };
            side.progress = counted_text;
        } else {
            ready = std::unexpected(got.error());
        }
        auto fetched = ready ? file_list::fetch(*ctx, unix_now(), run, [&](const std::vector<std::string>& argv) {
            act.detail(quote::display_cmdline(argv));
        }, side)
                             : std::expected<file_list::List, std::string>(std::unexpected(ready.error()));
        walk.reset();
        if (!fetched) act.failed();
        if (fetched) (void)file_list::save(file_list::path_for(lists_dir_, server), *fetched);
        {
            std::lock_guard inner(lists_mutex_);
            auto& slot = list_slot(server);
            slot.refreshing = false;
            if (stop.stop_requested()) return;
            if (fetched) {
                slot.list = std::make_shared<const file_list::List>(std::move(*fetched));
                slot.index.reset(); // built again from the new list on the next browse
                slot.error.clear();
                slot.failures = 0;
            } else {
                slot.error = fetched.error();
                slot.retry_after = std::chrono::steady_clock::now() +
                                   std::min(std::chrono::minutes(1 << std::min(slot.failures, 4)), std::chrono::minutes(15));
                ++slot.failures;
            }
        }
        host_.defer([this, server] { on_list_refreshed(server); });
    });
}

void App::on_list_refreshed(const std::string& server) {
    LastSearch last;
    {
        std::lock_guard lock(lists_mutex_);
        last = last_search_;
    }
    if (last.id != 0 && (last.remote == server || last.remote == "*")) {
        run_search(last.id, last.remote, last.req, true, last.both, last.under);
    }
    LastBrowse browsed;
    {
        std::lock_guard lock(browse_mutex_);
        browsed = last_browse_;
    }
    if (browsed.id != 0 && browsed.remote == server) {
        run_browse(browsed.id, browsed.remote, browsed.path, browsed.top, false, true, browsed.typed);
    }
}

App::ListView App::list_view(const std::string& server, const engine::Context& ctx) {
    ListView view;
    {
        std::lock_guard lock(lists_mutex_);
        auto& s = list_slot(server);
        if (!s.list || s.list->fingerprint != file_list::fingerprint(ctx)) return view;
        view.list = s.list;
        view.index = s.index;
    }
    if (view.index) return view;
    // Built outside the lock (a few ms for a large list), then kept unless the list changed.
    view.index = std::make_shared<const browse::Index>(view.list, engine::search_roots(ctx), ctx.settings.max_depth);
    std::lock_guard lock(lists_mutex_);
    auto& s = list_slot(server);
    if (s.list == view.list) s.index = view.index;
    return view;
}

std::expected<std::vector<engine::Hit>, std::string> App::search_below(const engine::Context& ctx, const std::string& server,
                                                                       const engine::SearchRequest& req,
                                                                       const std::string& under, bool update,
                                                                       const proc::RunOptions& run, Value& status) {
    std::vector<engine::Hit> hits;
    auto add = [&](engine::SearchResult found, bool dir) {
        for (auto& h : found.hits) {
            if (!browse::is_below(h.path, under)) continue;
            h.dir = dir;
            hits.push_back(std::move(h));
        }
    };
    const auto now = unix_now();
    const auto view = file_list::can_answer(ctx, req) ? list_view(server, ctx) : ListView{};
    if (view.index && view.index->folder(under)) {
        if (!update && !file_list::fresh(*view.list, now)) start_refresh(server);
        for (const Mode m : {Mode::folder, Mode::file}) {
            auto r = req;
            r.mode = m;
            add(file_list::search(ctx, *view.list, r), m == Mode::folder);
        }
        status.set("age", num(static_cast<double>(now - view.list->fetched)));
        set_notes(status, view.list->notes);
        return hits;
    }
    // Deeper than the list (or a search lists can't answer): live, the folder the only root.
    engine::Context below = ctx;
    below.settings.search_roots = {under};
    bool with_find = false; // once find is chosen: it counts matches, the walk entries
    const std::string base = !under.empty() ? std::format("Searching below {} on {}", under, server)
                                            : std::format("Searching all of {}", server);
    // How it goes is said once it's chosen.
    std::string doing = base;
    Activity act(*this, doing + "…", "search:" + server);
    auto counted_text = [&](std::size_t n) {
        act.text(with_find ? std::format("{}… {} found so far", doing, thousands(n))
                           : std::format("{}… {} looked at", doing, count_of(n, "entry", "entries")));
    };
    // The seat is one connection, widened for a walk (review G191); one find or walk lists both
    // kinds (G197).
    auto r = req;
    std::optional<fetch::Listing> walk;
    auto side = grab_side(ctx, server, walk, run.stop);
    if (!side) {
        act.failed();
        return util::fail(side.error());
    }
    r.grab = std::move(*side);
    r.grab.on_lookup = [&](const ssh::Lookup& way) {
        with_find = way.find;
        doing = std::format("{} {}", base, way.find ? "with find" : "by listing folders");
        act.timing(std::format("{}:{}", way.find ? "find" : "walk", server));
        act.text(doing + "…");
    };
    r.grab.progress = counted_text;
    auto found = engine::search_both(below, r, run, [&](const std::vector<std::string>& argv) {
        act.detail(quote::display_cmdline(argv));
    });
    if (!found) {
        act.failed();
        return util::fail(found.error());
    }
    set_notes(status, found->first.notes); // one walk's, the same for both kinds
    add(std::move(found->first), true);
    add(std::move(found->second), false);
    status.set("live", Value::make_bool(true));
    return hits;
}

std::expected<engine::GrabSide, std::string> App::grab_side(const engine::Context& ctx, const std::string& server,
                                                            std::optional<fetch::Listing>& seat, std::stop_token stop) {
    auto target = engine::ssh_target(ctx);
    if (!target) return util::fail(target.error());
    connections_.configure(server, connection_budget(server, ctx.remote.host, ctx.settings.max_connections));
    engine::GrabSide side;
    side.pool = pools_.get(server, *target, connections_.budget(server));
    // The find check as a command or an earlier run left it, beside the learned limits.
    side.memory = engine::find_memory(limits_path_, *target, [this](const std::expected<void, std::string>& saved) {
        report_save("the find checks", saved, limits_unsaved_);
    });
    seat.emplace(connections_, server, stop, 1);
    side.walk_seat = [this, &seat, server, stop] {
        seat.reset();
        seat.emplace(connections_, server, stop, 8);
        return seat->connections();
    };
    return side;
}

namespace {

Value item_value(const browse::Item& it) {
    Value v = Value::make_object();
    v.set("name", str(it.name));
    v.set("path", str(it.path));
    if (it.dir) v.set("dir", Value::make_bool(true));
    if (it.size) v.set("size", num(static_cast<double>(*it.size)));
    if (it.items) v.set("items", num(static_cast<double>(*it.items)));
    if (it.modified) v.set("modified", num(static_cast<double>(*it.modified)));
    return v;
}

} // namespace

void App::run_browse(int id, const std::string& remote, const std::string& path, bool top, bool live, bool update,
                     bool typed) {
    if (browse_thread_.joinable()) {
        browse_thread_.request_stop();
        browse_thread_.join();
    }
    {
        std::lock_guard lock(browse_mutex_);
        last_browse_ = LastBrowse{id, remote, path, top, typed};
    }
    browse_thread_ = std::jthread([this, id, remote, path, top, live, update, typed](std::stop_token stop) {
        const auto t0 = std::chrono::steady_clock::now();
        Value v = Value::make_object();
        v.set("type", str("browseResult"));
        v.set("id", num(id));
        v.set("remote", str(remote));
        v.set("update", Value::make_bool(update));
        if (typed) v.set("typed", Value::make_bool(true));
        auto send = [&] {
            v.set("elapsedMs", num(static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                           std::chrono::steady_clock::now() - t0)
                                                           .count())));
            post(v);
        };
        // A typed path that fails leaves the window where it was, and so does a list refresh
        // after it; any other failure is shown as the folder's.
        auto fail = [&](const std::string& error) {
            v.set("path", str(path));
            v.set("top", Value::make_bool(top));
            v.set("error", str(error));
            {
                std::lock_guard lock(browse_mutex_);
                if (!typed) {
                    shown_browse_ = LastBrowse{id, remote, path, top, false};
                } else if (last_browse_.id == id) {
                    last_browse_ = shown_browse_;
                    last_browse_.id = id;
                }
            }
            send();
        };
        auto ctx = engine::load_context(config_path_, remote);
        if (!ctx) return fail(ctx.error());
        const auto roots = engine::search_roots(*ctx);
        const auto now = unix_now();
        const auto view = list_view(remote, *ctx);
        if (!update && (!view.list || !file_list::fresh(*view.list, now))) start_refresh(remote);
        {
            std::lock_guard lock(lists_mutex_);
            const auto& s = list_slot(remote);
            v.set("refreshing", Value::make_bool(s.refreshing));
            if (!s.error.empty()) v.set("listError", str(s.error));
        }

        // A typed path as browse uses it; a server's one root is its top.
        std::string folder = typed ? browse::address_path(path) : path;
        const bool at_top = top || (typed && roots.size() == 1 && folder == roots.front());
        // Several roots make a top of their own; with one, the top is that root's folder.
        const bool roots_top = at_top && roots.size() > 1;
        if (at_top && !roots_top) folder = roots.front();
        std::string select; // a typed file's path: its folder opens with it picked

        // A folder's rows: this session's live listing or the list, whichever is newer, else
        // listed live now.
        struct Rows {
            std::vector<browse::Item> items;
            std::string source;
            std::int64_t listed = 0;
            bool new_listing = false; // listed live just now, not kept yet
        };
        auto rows_of = [&](const std::string& at) -> std::expected<Rows, browse::ListError> {
            std::optional<LiveFolder> cached;
            if (!live) {
                std::lock_guard lock(browse_mutex_);
                if (const auto it = live_folders_.find({remote, at}); it != live_folders_.end()) cached = it->second;
            }
            auto from_list = !live && view.index ? view.index->folder(at) : std::nullopt;
            // A live listing made after the list was fetched is newer; otherwise the list wins,
            // which also knows the sub-folders' item counts.
            if (cached && (!from_list || cached->listed > view.list->fetched)) {
                return Rows{std::move(cached->items), "live", cached->listed};
            }
            if (from_list) return Rows{std::move(*from_list), "list", view.list->fetched};
            proc::RunOptions run;
            run.stop = stop;
            run.detached = true;
            const std::string doing = std::format("Listing {} on {}", at.empty() ? "~" : at, remote);
            Activity act(*this, doing + "…", "readdir:" + remote);
            // One connection of the server's budget (fetch::Listing, review G191).
            std::optional<fetch::Listing> seat;
            auto side = grab_side(*ctx, remote, seat, stop);
            if (!side) {
                act.failed();
                return std::unexpected(browse::ListError{.kind = browse::ListError::Kind::other, .message = side.error()});
            }
            auto got = browse::list_live(*ctx, at, run, [&](const std::vector<std::string>& argv) {
                act.detail(quote::display_cmdline(argv));
            }, *side);
            if (!got) {
                act.failed();
                return std::unexpected(got.error());
            }
            return Rows{std::move(*got), "live", now, true};
        };

        std::vector<browse::Item> items;
        std::string source;
        std::int64_t listed = now;
        if (roots_top) {
            if (view.index) {
                items = view.index->top();
                source = "list";
                listed = view.list->fetched;
            } else {
                for (const auto& r : roots) items.push_back(browse::Item{r.empty() ? "~" : r, r, true, {}, {}});
                source = "roots";
            }
        } else {
            // A file the list holds: its folder.
            if (typed && view.index) {
                if (const auto e = view.index->entry(folder); e && !e->dir) {
                    select = folder;
                    folder = parent_of(folder);
                }
            }
            auto rows = rows_of(folder);
            if (stop.stop_requested()) return; // the next request answers instead
            if (!rows && typed && select.empty() && rows.error().kind == browse::ListError::Kind::file) {
                // One STAT said it's a file; its folder opens with it picked, when the folder shows
                // it (a dot-name may be left out).
                const std::string parent = parent_of(folder);
                if (auto up = rows_of(parent)) {
                    const auto name = remote_basename(folder);
                    if (std::ranges::find(up->items, name, &browse::Item::name) != up->items.end()) select = folder;
                    folder = parent;
                    rows = std::move(up);
                }
                if (stop.stop_requested()) return;
            }
            if (!rows) {
                if (typed && rows.error().kind == browse::ListError::Kind::not_found) {
                    return fail(std::format("Not found on {}: {}", remote, folder.empty() ? "~" : folder));
                }
                if (typed && rows.error().kind == browse::ListError::Kind::denied) {
                    return fail(std::format("{} doesn't let you list {}", remote, folder.empty() ? "~" : folder));
                }
                return fail(rows.error().message);
            }
            if (rows->new_listing) {
                std::lock_guard lock(browse_mutex_);
                live_folders_[{remote, folder}] = LiveFolder{rows->items, rows->listed};
            }
            items = std::move(rows->items);
            source = std::move(rows->source);
            listed = rows->listed;
            Value crumbs = Value::make_array();
            for (const auto& c : browse::crumbs(roots, folder)) {
                Value cv = Value::make_object();
                cv.set("name", str(c.name));
                cv.set("path", str(c.path));
                crumbs.push(std::move(cv));
            }
            v.set("crumbs", std::move(crumbs));
        }
        const std::string at = roots_top ? std::string() : folder;
        {
            // Where the window now is (a typed path resolved), also for the next list refresh.
            std::lock_guard lock(browse_mutex_);
            shown_browse_ = LastBrowse{id, remote, at, at_top, false};
            if (typed && last_browse_.id == id) last_browse_ = shown_browse_;
        }
        v.set("path", str(at));
        v.set("top", Value::make_bool(at_top));
        if (!select.empty()) v.set("select", str(select));
        v.set("source", str(source));
        v.set("age", num(static_cast<double>(now - listed)));
        v.set("severalRoots", Value::make_bool(roots.size() > 1));
        Value list = Value::make_array();
        for (const auto& it : items) list.push(item_value(it));
        v.set("items", std::move(list));
        send();
    });
}

void App::suggest(const std::string& remote, const std::string& path) {
    Value v = Value::make_object();
    v.set("type", str("suggestions"));
    v.set("remote", str(remote));
    v.set("path", str(path));
    Value names = Value::make_array();
    // On the UI thread, so without the import (the settings thread does it at start-up): it
    // would take the settings lock and decrypt the stores on every key typed.
    if (auto ctx = engine::load_context(config_path_, remote, false)) {
        const std::string folder = browse::address_path(path);
        const auto view = list_view(remote, *ctx);
        if (!view.list) {
            // No list yet (a server not used since grab started): fetched now, for the next ask.
            start_refresh(remote);
            v.set("later", Value::make_bool(true));
        }
        std::optional<LiveFolder> cached;
        {
            std::lock_guard lock(browse_mutex_);
            if (const auto it = live_folders_.find({remote, folder}); it != live_folders_.end()) cached = it->second;
        }
        const auto from_list = view.index ? view.index->folder(folder) : std::nullopt;
        // As browse picks: the newer of the two.
        const std::vector<browse::Item>* items = nullptr;
        if (cached && (!from_list || cached->listed > view.list->fetched)) items = &cached->items;
        else if (from_list) items = &*from_list;
        if (items != nullptr) {
            for (const auto& it : *items) {
                if (it.dir) names.push(str(it.name));
            }
        }
    }
    v.set("folders", std::move(names));
    post(v);
}

void App::prune_lists() {
    auto cfg = load_grab_config(config_path_);
    if (!cfg) return;
    std::set<std::filesystem::path> keep;
    for (const auto& r : cfg->remotes) keep.insert(file_list::path_for(lists_dir_, r.name).filename());
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(lists_dir_, ec)) {
        if (e.path().extension() == L".list" && !keep.contains(e.path().filename())) std::filesystem::remove(e.path(), ec);
    }
    std::lock_guard lock(lists_mutex_);
    std::erase_if(lists_, [&](const auto& kv) {
        return !kv.second.refreshing && !keep.contains(file_list::path_for(lists_dir_, kv.first).filename());
    });
}

void App::pick_folder(std::wstring current, std::string purpose) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(purpose == "downloadTo" ? L"Download to" : L"Save downloads to");
    // Start in the current folder, or its nearest existing parent.
    std::filesystem::path start(current);
    std::error_code ec;
    while (!start.empty() && !std::filesystem::is_directory(start, ec)) {
        if (start == start.parent_path()) break;
        start = start.parent_path();
    }
    ComPtr<IShellItem> folder;
    if (!start.empty() &&
        SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
        dialog->SetFolder(folder.Get());
    }
    if (FAILED(dialog->Show(host_.hwnd))) return; // cancelled
    ComPtr<IShellItem> result;
    PWSTR path = nullptr;
    if (SUCCEEDED(dialog->GetResult(&result)) &&
        SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        Value v = Value::make_object();
        v.set("type", str("folderPicked"));
        v.set("path", str(util::to_utf8(path)));
        v.set("purpose", str(purpose));
        post(v);
    }
    CoTaskMemFree(path);
}

void App::pick_file(std::wstring current) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    dialog->SetTitle(L"Private key file");
    // Start next to the current key, else in ~/.ssh when it exists.
    std::filesystem::path start = std::filesystem::path(current).parent_path();
    std::error_code ec;
    if (start.empty() || !std::filesystem::is_directory(start, ec)) {
        start = util::path_from_utf8(known_folder(FOLDERID_Profile)) / ".ssh";
    }
    ComPtr<IShellItem> folder;
    if (std::filesystem::is_directory(start, ec) &&
        SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
        dialog->SetFolder(folder.Get());
    }
    if (FAILED(dialog->Show(host_.hwnd))) return; // cancelled
    ComPtr<IShellItem> result;
    PWSTR path = nullptr;
    if (SUCCEEDED(dialog->GetResult(&result)) &&
        SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        Value v = Value::make_object();
        v.set("type", str("filePicked"));
        v.set("path", str(util::to_utf8(path)));
        post(v);
    }
    CoTaskMemFree(path);
}

// Like `grab config`: [grab] editor, $VISUAL, $EDITOR, else Notepad. Started, not waited for.
void App::open_config() const {
    std::error_code ec;
    if (!std::filesystem::exists(config_path_, ec)) {
        if (auto created = util::write_file_atomic(config_path_, generate_grab_config(nullptr, "")); !created) {
            return settings_error(created.error());
        }
    }
    std::vector<std::string> candidates;
    if (auto doc = ini::parse_file(config_path_)) {
        if (const auto* g = doc->find("grab")) candidates.push_back(g->get("editor").value_or(""));
    }
    candidates.push_back(util::getenv_utf8("VISUAL").value_or(""));
    candidates.push_back(util::getenv_utf8("EDITOR").value_or(""));
    const auto argv = editor_command(candidates, config_path_);
    // By its full path: ShellExecute would look in the current folder first (G3). Like
    // ShellExecute, a bare name may be a .cmd ("code" is code.cmd).
    const auto exe = proc::find_program(argv.front(), true);
    if (!exe) return settings_error(std::format("could not start the editor: {}", exe.error()));
    const std::wstring params =
        util::to_wide(quote::windows_cmdline(std::span<const std::string>(argv).subspan(1)));
    const auto rc = reinterpret_cast<INT_PTR>(
        ShellExecuteW(host_.hwnd, L"open", exe->c_str(), params.c_str(), nullptr, SW_SHOWNORMAL));
    if (rc <= 32) settings_error(std::format("could not start the editor \"{}\"", argv.front()));
}

// ---- settings: servers ----------------------------------------------------------------------

void App::settings_task(std::function<void(std::stop_token)> task) {
    if (settings_busy_.exchange(true)) {
        return settings_error("Another server change is still running; wait for it to finish.");
    }
    if (settings_thread_.joinable()) settings_thread_.join(); // finished: busy was false
    auto busy = [this](bool on) {
        Value v = Value::make_object();
        v.set("type", str("settingsBusy"));
        v.set("busy", Value::make_bool(on));
        post(v);
    };
    busy(true);
    settings_thread_ = std::jthread([this, task = std::move(task), busy](std::stop_token stop) {
        task(stop);
        settings_busy_ = false;
        busy(false);
    });
}

void App::settings_error(const std::string& message) const {
    Value v = Value::make_object();
    v.set("type", str("serverError"));
    v.set("message", str(message));
    post(v);
}

std::expected<server_ops::Env, std::string> App::load_env() const {
    auto env = server_ops::load_env(config_path_);
    // grab's own old rclone.conf and 'secrets', deleted by this load once nothing in them was
    // left to move (an Edit that saved the last password, say): said once, where the import's
    // notice goes, with the configuration's other notices not shown yet when it is recorded as one.
    // The start's import says it in its own notice.
    if (env && !env->old_files.empty()) {
        std::string text = env->old_files_recorded ? std::string() : env->old_files;
        if (env->old_files_recorded) {
            for (const auto& n : notices::take_fresh(config_path_)) text += (text.empty() ? "" : "\n") + n.text;
        }
        if (!text.empty()) {
            Value v = Value::make_object();
            v.set("type", str("importBanner"));
            v.set("text", str(text));
            post(v);
        }
    }
    return env;
}

void App::post_servers(const server_ops::Env& env) const {
    const auto learned = load_learned_limits(limits_path_);
    Value items = Value::make_array();
    for (const auto& s : server_ops::list_servers(env)) {
        Value v = Value::make_object();
        v.set("name", str(s.name));
        v.set("isDefault", Value::make_bool(s.is_default));
        Value roots = Value::make_array();
        for (const auto& r : s.search_roots) roots.push(str(r));
        v.set("roots", std::move(roots));
        v.set("depth", num(s.max_depth));
        v.set("maxConnections", s.max_connections ? num(*s.max_connections) : Value{});
        {
            // The automatic value, lowered to what grab (or the command) learned from refusals.
            const auto it = learned.find(s.name);
            v.set("autoConnections",
                  num(budget::effective_budget(std::nullopt, s.default_connections,
                                               it == learned.end() ? std::nullopt : std::optional(it->second))));
        }
        v.set("error", str(s.error));
        v.set("host", str(s.host));
        v.set("user", str(s.user));
        v.set("port", num(s.port));
        // "password", "key", "agent", or "stored_key" (an imported key_pem, kept as it is on Edit).
        v.set("auth", str(std::string(server_conf::auth_name(s.auth))));
        v.set("keyFile", str(s.key_file));
        v.set("search", str(s.search)); // search_method(): find, walk, probe
        v.set("pinned", Value::make_bool(s.host_key_pinned));
        // Why grab doesn't connect to it at all (Trust included), when it doesn't: the guard's
        // reason, or a bind left in an old key.
        v.set("guard", str(s.unusable));
        v.set("connectionKeys", str(s.connection_keys)); // grab.conf's bind and times, when set
        // Whether its password (passphrase, stored key) is saved for this login; null for ssh-agent
        // or a store that can't be read. Never the value.
        v.set("secretSaved", s.secret_saved ? Value::make_bool(*s.secret_saved) : Value{});
        items.push(std::move(v));
    }
    Value msg = Value::make_object();
    msg.set("type", str("servers"));
    msg.set("configPath", str(util::path_to_utf8(env.grab_conf)));
    msg.set("items", std::move(items));
    post(msg);
}

void App::post_host_keys(const Pending& p) const {
    Value msg = Value::make_object();
    msg.set("type", str("hostKeys"));
    msg.set("kind", str(p.kind == Pending::Kind::add ? "add" : p.kind == Pending::Kind::edit ? "edit" : "trust"));
    msg.set("name", str(p.server.name));
    msg.set("host", str(p.server.host));
    msg.set("port", num(p.server.port));
    Value prints = Value::make_array();
    for (const auto& f : p.keys.fingerprints) {
        Value v = Value::make_object();
        v.set("type", str(f.type));
        v.set("hash", str(f.hash));
        prints.push(std::move(v));
    }
    msg.set("fingerprints", std::move(prints));
    Value lines = Value::make_array();
    if (p.keys.fingerprints.empty()) {
        for (const auto& l : p.keys.lines) lines.push(str(l));
    }
    msg.set("lines", std::move(lines));
    post(msg);
}

void App::servers_changed() {
    prune_lists();
    if (auto env = load_env()) post_servers(*env);
    host_.defer([this] { send_init(); }); // the search view's server list; state_ is UI-thread only
}

void App::save_server(const Value& msg) {
    const bool editing = bool_of(msg, "editing");
    servers::NewServer s;
    s.name = std::string(util::trim(msg.string_of("name").value_or("")));
    s.host = std::string(util::trim(msg.string_of("host").value_or("")));
    s.user = std::string(util::trim(msg.string_of("user").value_or("")));
    s.port = int_of(msg, "port");
    s.max_depth = int_of(msg, "depth");
    const std::string auth = msg.string_of("auth").value_or("password");
    s.auth = server_conf::parse_auth(auth).value_or(servers::Auth::password);
    if (s.auth == servers::Auth::key) s.key_file = std::string(util::trim(msg.string_of("keyFile").value_or("")));
    if (const Value* roots = msg.find("roots"); roots != nullptr && roots->kind == Value::Kind::array) {
        for (const auto& r : roots->items) {
            if (r.kind == Value::Kind::string && !util::trim(r.str).empty()) {
                s.search_roots.push_back(normalize_root(r.str));
            }
        }
    }
    // Shared so the task below stays copyable; wiped whichever way this ends.
    auto secret = std::make_shared<Secret>(msg.string_of("secret").value_or(""));

    auto invalid = [this](const char* field, const std::string& message) {
        Value v = Value::make_object();
        v.set("type", str("serverError"));
        v.set("field", str(field));
        v.set("message", str(message));
        post(v);
    };
    if (!editing) {
        if (auto why = servers::validate_name(s.name)) return invalid("name", *why);
    }
    // The checks grab.conf's details get everywhere (G5, G28).
    if (auto problem = server_ops::check_server(s)) return invalid(problem->field.c_str(), problem->why);
    if (s.max_depth < 1 || s.max_depth > 64) return invalid("depth", "a number from 1 to 64");
    if (const int conns = int_of(msg, "connections"); conns != 0) { // 0 or missing: automatic
        if (conns < 1 || conns > 64) return invalid("connections", "a number from 1 to 64, or blank for automatic");
        s.max_connections = conns;
    }

    settings_task([this, editing, s, secret, invalid](std::stop_token stop) {
        auto env = load_env();
        if (!env) return settings_error(env.error());
        Pending p;
        p.server = s;
        p.secret = std::move(*secret);
        bool need_keys = true;
        if (editing) {
            p.kind = Pending::Kind::edit;
            const auto list = server_ops::list_servers(*env);
            auto it = std::ranges::find(list, s.name, &server_ops::ServerInfo::name);
            if (it == list.end()) return settings_error("no server called " + s.name);
            if (p.secret.empty() && server_ops::edit_needs_password(*it, s)) {
                const bool saved = it->error.empty() && it->auth == servers::Auth::password && it->secret_saved == true;
                return invalid("secret", saved ? "enter the password again for the new user, host or port"
                                               : "enter the password");
            }
            need_keys = server_ops::edit_needs_host_key(*it, s);
            p.moved = it->host != s.host || it->port != s.port || it->user != s.user;
        } else {
            p.kind = Pending::Kind::add;
            if (auto why = server_ops::name_taken(*env, s.name)) return invalid("name", *why);
            if (s.auth == servers::Auth::password && p.secret.empty()) return invalid("secret", "enter the password");
        }
        if (!need_keys) return apply_pending(std::move(p), stop);

        // The server's bind and connect time; refused for a server grab doesn't connect to (an
        // inherited RCLONE_SFTP_SOCKS_PROXY covers a new one too), before anything dials. An Edit
        // sets an unreadable store aside, so that doesn't stop its fetch.
        const auto purpose = editing ? server_ops::Purpose::edit : server_ops::Purpose::trust;
        auto reach = server_ops::reach_for(*env, s.name, purpose);
        if (!reach) return settings_error(reach.error());
        auto keys = server_ops::scan_host_keys(s.host, s.port, stop, *reach);
        if (stop.stop_requested()) return;
        if (!keys) return settings_error(keys.error());
        p.keys = std::move(*keys);
        post_host_keys(p);
        std::lock_guard lock(settings_mutex_);
        pending_ = std::move(p);
    });
}

void App::apply_pending(Pending p, std::stop_token stop) {
    auto env = load_env();
    if (!env) return settings_error(env.error());
    // Plain into grab's store (encrypted there), and wiped here once handed over.
    std::optional<std::string> secret;
    if (!p.secret.empty()) secret = p.secret.value();
    p.secret.wipe();
    std::expected<std::string, std::string> done; // a note to show, when it worked
    switch (p.kind) {
    case Pending::Kind::add:
        done = server_ops::add_server(*env, p.server, secret, p.keys);
        break;
    case Pending::Kind::edit:
        done = server_ops::update_server(*env, p.server, secret,
                                         p.keys.lines.empty() ? std::nullopt : std::optional(p.keys));
        break;
    case Pending::Kind::trust:
        done = server_ops::trust_server(*env, p.server.name, p.keys).transform([] { return std::string{}; });
        break;
    }
    if (secret) std::fill(secret->begin(), secret->end(), '\0');
    if (!done) return settings_error(done.error());
    if (!done->empty()) settings_error(*done);
    // Its previews kept may be another server's files now, and a login that failed may work; a
    // server at another login also loses what grab learned for the old one (G122).
    if (p.moved) forget_server(p.server.name);
    else forget_server_previews(p.server.name);
    // An explicit connection limit replaces whatever grab learned for this server.
    if (p.server.max_connections) {
        {
            std::lock_guard lock(mutex_);
            learned_.erase(p.server.name);
        }
        report_save("the learned connection limits", save_learned_limit(limits_path_, p.server.name, std::nullopt),
                    limits_unsaved_);
    }

    Value v = Value::make_object();
    v.set("type", str("serverSaved"));
    v.set("name", str(p.server.name));
    post(v);
    servers_changed();
    test_server(p.server.name, stop);
}

void App::test_server(const std::string& name, std::stop_token stop) {
    auto env = load_env();
    if (!env) return settings_error(env.error());
    Value v = Value::make_object();
    v.set("type", str("serverTested"));
    v.set("name", str(name));
    auto result = server_ops::test_connection(*env, name, stop);
    if (stop.stop_requested()) return;
    // The test asked the find check again and saved its answer: the window's searches go by that
    // one now, not the one their pool kept. A login that works now also ends a failed login the
    // pool's hash check kept (a host key trusted since, an agent started since).
    if (result && result->grab_ok.value_or(false)) {
        pools_.forget_find_check(name);
        pools_.forget_hash_check(name);
        pools_.forget_login_failure(name);
    }
    if (!result) {
        v.set("ok", Value::make_bool(false));
        v.set("error", str(result.error()));
    } else {
        v.set("ok", Value::make_bool(result->ok()));
        // grab's login, how searches go ("with find (GNU find)"), and whether downloads can be
        // verified ("checksums: md5sum"); or why grab doesn't connect to it at all.
        v.set("grabOk", result->grab_ok ? Value::make_bool(*result->grab_ok) : Value{});
        v.set("grabError", str(result->grab_error));
        v.set("unusable", str(result->unusable));
        v.set("grabSearch", str(result->grab_search));
        v.set("checksums", str(result->checksums));
    }
    post(v);
}

void App::cancel_pending() {
    if (settings_busy_) settings_thread_.request_stop(); // e.g. a host key scan
    std::lock_guard lock(settings_mutex_);
    pending_.reset(); // wipes its secret
}

// ---- download queue -------------------------------------------------------------------------

void App::enqueue(const Value& msg) {
    const std::string dest = std::string(util::trim(msg.string_of("dest").value_or("")));
    const Value* groups = msg.find("groups");
    if (groups == nullptr || groups->kind != Value::Kind::array) return;
    // A GUI's working directory is arbitrary, so a relative folder would land somewhere
    // unexpected; insist on a full path. One spelling of it ("D:/x/" is D:\x\), for telling two
    // downloads into one place (G44).
    const auto dest_path = util::path_from_utf8(dest).lexically_normal();
    const auto answer = existing::parse(msg.string_of("existing").value_or(""));
    if (!dest_path.is_absolute()) {
        Value v = Value::make_object();
        v.set("type", str("error"));
        v.set("message", str(std::format("\"{}\" is not a full folder path. Use something like "
                                         "C:\\Users\\you\\Downloads, or pick one with Browse.",
                                         dest)));
        post(v);
        return;
    }
    // A different file already there for some of it: nothing is queued until the user chose.
    if (!answer) {
        std::size_t count = 0;
        const auto found = conflicts_for(*groups, dest_path, 5, count);
        if (count > 0) {
            Value v = Value::make_object();
            v.set("type", str("existing"));
            v.set("ask", asked_json(found, count));
            post(v);
            return;
        }
    }
    const bool force = bool_of(msg, "force");

    // Free space: what this selection still needs on the drive, with what the downloads already
    // queued there still need (partial files fill in as data arrives).
    if (!force) {
        const auto need = needed_for(*groups, dest_path);
        const auto drive = space::drive_of(dest_path);
        std::vector<std::pair<std::filesystem::path, std::uint64_t>> pending;
        {
            std::lock_guard lock(mutex_);
            for (const auto& i : items_) {
                if (i->status == Status::queued || i->status == Status::running || i->status == Status::paused) {
                    pending.emplace_back(i->dest, i->total > i->bytes ? i->total - i->bytes : 0);
                }
            }
        }
        std::uint64_t queued = 0;
        for (const auto& [where, left] : pending) {
            if (left > 0 && util::to_lower(space::drive_of(where)) == util::to_lower(drive)) queued += left;
        }
        if (const auto free = space::free_bytes(dest_path); free && need.bytes > 0 && !space::fits(need.bytes, queued, *free)) {
            Value v = Value::make_object();
            v.set("type", str("spaceShort"));
            v.set("drive", str(drive));
            v.set("need", num(static_cast<double>(need.bytes)));
            v.set("queued", num(static_cast<double>(queued)));
            v.set("free", num(static_cast<double>(*free)));
            if (need.partial) v.set("partial", Value::make_bool(true));
            post(v);
            return;
        }
    }

    // The answer goes for the items whose differing files the question showed in full; any other
    // asks once it finds some, after its folder is listed (review G26).
    std::set<std::pair<std::string, std::string>> covered;
    if (answer) {
        std::size_t count = 0;
        [[maybe_unused]] const auto none_shown = conflicts_for(*groups, dest_path, 0, count, &covered);
    }
    {
        std::lock_guard lock(mutex_);
        for (const auto& group : groups->items) {
            const std::string remote = group.string_of("remote").value_or("");
            const Value* items = group.find("items");
            if (remote.empty() || items == nullptr || items->kind != Value::Kind::array) continue;
            // Download to… leaves the Save-to folder remembered as it was.
            if (bool_of(msg, "remember")) state_.destinations[remote] = dest;
            state_.last_remote = remote;
            for (const auto& it : items->items) {
                auto path = it.string_of("path");
                if (!path || !has_name(*path)) continue; // "/": it would land in the Save-to folder itself (G15)
                auto item = std::make_shared<Item>();
                item->id = next_id_++;
                item->remote = remote;
                item->mode = mode_of(group);
                item->path = *path;
                item->name = it.string_of("name").value_or(*path);
                item->dest = dest_path;
                item->space_ok = force;
                item->existing = answer && covered.contains({remote, *path}) ? *answer : existing::Choice::ask;
                if (const Value* size = it.find("size"); size && size->kind == Value::Kind::number) {
                    item->total = static_cast<std::uint64_t>(size->number);
                }
                // Already in the queue for the same place (the page lets a failed row's file through,
                // and spells a folder its own way): no second row sharing its partial download; a
                // failed one starts again from it, with this answer (review G134).
                if (const auto twin = twin_of(*item)) {
                    if (twin->status == Status::failed) {
                        twin->existing = item->existing;
                        twin->space_ok = force;
                        requeue(*twin);
                    }
                    continue;
                }
                items_.push_back(std::move(item));
            }
        }
    }
    save_state();
    wake_.notify_all();
    request_save();
    queue_changed_ = true;
    flush();
}

App::Needed App::needed_for(const Value& groups, const std::filesystem::path& dest) {
    Needed need;
    for (const auto& group : groups.items) {
        const std::string remote = group.string_of("remote").value_or("");
        const Value* items = group.find("items");
        if (items == nullptr || items->kind != Value::Kind::array) continue;
        const bool folders = mode_of(group) == Mode::folder;
        std::optional<ListView> view; // the server's list, for folders' sizes
        for (const auto& it : items->items) {
            const auto path = it.string_of("path");
            if (!path) continue;
            if (!folders) {
                const Value* size = it.find("size");
                if (size == nullptr || size->kind != Value::Kind::number) {
                    need.partial = true;
                    continue;
                }
                need.bytes += space::still_needed(file_target(dest, *path), *path, static_cast<std::uint64_t>(size->number));
                continue;
            }
            if (!view) {
                auto ctx = engine::load_context(config_path_, remote, false);
                view = ctx ? list_view(remote, *ctx) : ListView{};
            }
            if (!view->index) {
                need.partial = true; // no list: the folder checks once it is listed
                continue;
            }
            const auto below = view->index->below(*path);
            if (!below.complete) need.partial = true;
            // Files already there count only when the folder was downloaded before.
            const auto folder_dir = folder::local_dir(dest, *path);
            std::error_code ec;
            const bool started = std::filesystem::exists(folder_dir, ec);
            for (const auto* e : below.files) {
                const auto size = e->size.value_or(0);
                if (!started) {
                    need.bytes += size;
                    continue;
                }
                std::string_view rel = e->path;
                rel.remove_prefix(std::min(rel.size(), path->size()));
                while (rel.starts_with('/')) rel.remove_prefix(1);
                need.bytes += space::still_needed(folder_dir / local::relative(rel), e->path, size);
            }
        }
    }
    return need;
}

void App::pause_items(int id) {
    {
        std::lock_guard lock(mutex_);
        for (auto& item : items_) {
            if (id != 0 && item->id != id) continue;
            if (item->status == Status::queued) {
                item->status = Status::paused;
            } else if (item->status == Status::running && item->stop_reason == StopReason::none) {
                item->stop_reason = StopReason::pause; // run_item marks it paused once stopped
                item->resume_mark = false;
                item->stop.request_stop();
            }
        }
    }
    request_save();
    dirty_ = true;
    flush();
}

std::vector<App::Asked> App::conflicts_for(const Value& groups, const std::filesystem::path& dest, std::size_t shown,
                                           std::size_t& count, std::set<std::pair<std::string, std::string>>* covered) {
    std::vector<Asked> found;
    count = 0;
    auto add = [&](const std::filesystem::path& local, std::string name, std::uint64_t size,
                   std::optional<std::int64_t> modified) {
        if (!existing::differs(local, size, seconds_to_ns(modified))) return;
        if (++count > shown) return;
        if (const auto mine = existing::local_file(local)) found.push_back(Asked{std::move(name), size, modified, *mine});
    };
    for (const auto& group : groups.items) {
        const std::string remote = group.string_of("remote").value_or("");
        const Value* items = group.find("items");
        if (items == nullptr || items->kind != Value::Kind::array) continue;
        const bool folders = mode_of(group) == Mode::folder;
        std::optional<ListView> view; // the server's list, for folders' files
        for (const auto& it : items->items) {
            const auto path = it.string_of("path");
            if (!path) continue;
            if (!folders) {
                const Value* size = it.find("size");
                if (size == nullptr || size->kind != Value::Kind::number) continue;
                const Value* modified = it.find("modified");
                const auto target = file_target(dest, *path);
                const auto before = count;
                add(target, util::path_to_utf8(target.filename()), static_cast<std::uint64_t>(size->number),
                    modified != nullptr && modified->kind == Value::Kind::number
                        ? std::optional(static_cast<std::int64_t>(modified->number))
                        : std::nullopt);
                if (covered != nullptr && count > before) covered->emplace(remote, *path);
                continue;
            }
            // A folder's files, when it was downloaded here before and the list holds them; a
            // deeper one asks once it is listed.
            const auto folder_dir = folder::local_dir(dest, *path);
            std::error_code ec;
            if (!std::filesystem::is_directory(folder_dir, ec)) continue;
            if (!view) {
                auto ctx = engine::load_context(config_path_, remote, false);
                view = ctx ? list_view(remote, *ctx) : ListView{};
            }
            if (!view->index) continue;
            const std::string folder_name = util::path_to_utf8(folder_dir.filename());
            const auto below = view->index->below(*path);
            const auto before = count;
            for (const auto* e : below.files) {
                std::string_view rel = e->path;
                rel.remove_prefix(std::min(rel.size(), path->size()));
                while (rel.starts_with('/')) rel.remove_prefix(1);
                add(folder_dir / local::relative(rel), folder_name + "/" + std::string(rel), e->size.value_or(0),
                    e->modified);
            }
            // Only a folder the list holds completely had all of its differing files counted.
            if (covered != nullptr && count > before && below.complete) covered->emplace(remote, *path);
        }
    }
    return found;
}

void App::resolve_existing(int id, existing::Choice choice) {
    {
        std::lock_guard lock(mutex_);
        for (auto& item : items_) {
            if (item->id != id || item->status != Status::paused) continue;
            item->existing = choice;
            item->asked.clear();
            item->asked_count = 0;
            item->note.clear();
            item->waiting.clear();
            item->status = Status::queued;
        }
    }
    wake_.notify_all();
    request_save();
    dirty_ = true;
    flush();
}

Value App::asked_json(const std::vector<Asked>& asked, std::size_t count) {
    auto time = [](std::optional<std::int64_t> t) { return t ? num(static_cast<double>(*t)) : Value{}; };
    Value files = Value::make_array();
    for (const auto& a : asked) {
        Value f = Value::make_object();
        f.set("name", str(a.name));
        f.set("size", num(static_cast<double>(a.size)));
        f.set("modified", time(a.modified));
        f.set("mineSize", num(static_cast<double>(a.mine.size)));
        f.set("mineModified", time(a.mine.modified));
        files.push(std::move(f));
    }
    Value v = Value::make_object();
    v.set("count", num(static_cast<double>(count)));
    v.set("files", std::move(files));
    return v;
}

std::filesystem::path App::item_target(const Item& item) {
    return item.local_name.empty() ? file_target(item.dest, item.path) : item.dest / item.local_name;
}

void App::resume_items(int id) {
    {
        std::lock_guard lock(mutex_);
        for (auto& item : items_) {
            if ((id == 0 || item->id == id) && item->status == Status::paused && (id != 0 || item->asked.empty())) {
                item->status = Status::queued;
                item->note.clear();
                item->waiting.clear();
                folder::resume_paused(item->tree); // a folder's Resume covers its files paused one by one too
                item->touched = true;
            }
        }
    }
    wake_.notify_all();
    request_save();
    dirty_ = true;
    flush();
}

void App::cancel_items(int id) {
    struct Discard {
        std::filesystem::path target;
        std::string path;
        std::string server;
    };
    std::vector<Discard> discard; // partial downloads not running now
    {
        std::lock_guard lock(mutex_);
        for (auto& item : items_) {
            if (id != 0 && item->id != id) continue;
            if (item->status == Status::queued || item->status == Status::paused || item->status == Status::failed) {
                item->status = Status::cancelled;
                item->speed = 0;
                item->eta.reset();
                // Not while another row for the same file into the same place goes on from it (G134).
                if (twin_of(*item)) continue;
                if (item->mode == Mode::file) {
                    discard.push_back({.target = item_target(*item), .path = item->path, .server = item->remote});
                } else {
                    discard_folder_partials(*item); // finished files stay
                }
            } else if (item->status == Status::running) {
                item->stop_reason = StopReason::cancel;
                item->stop.request_stop();
            }
        }
    }
    for (const auto& d : discard) fetch::discard(d.target, d.path, {}, rclone_remote_of(d.server));
    request_save();
    dirty_ = true;
    flush();
}

void App::retry_item(int id) {
    {
        std::lock_guard lock(mutex_);
        for (auto& item : items_) {
            if (item->id == id && (item->status == Status::failed || item->status == Status::cancelled)) {
                requeue(*item);
            }
        }
    }
    wake_.notify_all();
    request_save();
    dirty_ = true;
    flush();
}

void App::requeue(Item& item) const {
    // A failed file download continues from its partial file; a cancelled one's was deleted, so
    // it starts over.
    const bool cancelled = item.status == Status::cancelled;
    item.status = Status::queued;
    item.error.clear();
    item.note.clear(); // the last run's, which a retried, then paused row showed again
    item.waiting.clear();
    if (item.mode == Mode::folder) {
        folder::retry_files(item.tree, cancelled); // after a cancel, everything not finished
        item.touched = true;
    } else if (!fetch::saved_progress(item_target(item), item.path, {}, rclone_remote_of(item.remote))) {
        item.bytes = 0;
    }
    item.speed = 0;
    item.eta.reset();
}

void App::file_action(const Value& msg) {
    const int id = int_of(msg, "id", -1);
    const int gen = int_of(msg, "gen", -1);
    const std::string act = msg.string_of("act").value_or("");
    if (act != "pause" && act != "resume" && act != "retry" && act != "skip") return;
    std::vector<folder::Discard> discard; // skipped big files' partial downloads, deleted after unlocking
    std::shared_ptr<const queue_view::Names> names;
    std::filesystem::path dir;
    std::string server;
    bool wake = false;
    {
        std::lock_guard lock(mutex_);
        const auto it = std::ranges::find(items_, id, [](const auto& i) { return i->id; });
        if (it == items_.end()) return;
        Item& item = **it;
        // The rows of another generation: the page gets the new ones and asks again.
        if (item.gen != gen || !item.tree.listed || !item.names) return;
        dir = folder::local_dir(item.dest, item.path);
        bool changed = false;
        for (const auto k : queue_view::indices_of(msg.find("ranges"), item.tree.files.size())) {
            if (act == "pause") {
                changed = folder::pause_file(item.tree, k) || changed;
            } else if (act == "resume") {
                changed = folder::resume_file(item.tree, k) || changed;
            } else if (act == "retry") {
                changed = folder::retry_file(item.tree, k) || changed;
            } else {
                bool skipped = false;
                if (auto d = folder::skip_file(item.tree, dir, item.path, k, &skipped)) discard.push_back(std::move(*d));
                changed = skipped || changed;
            }
        }
        if (!changed) return;
        item.touched = true;
        // A folder that stopped (paused, failed) runs again for the files resumed or retried.
        if ((act == "resume" || act == "retry") && item.status != Status::running && item.status != Status::queued) {
            item.status = Status::queued;
            item.error.clear();
            wake = true;
        }
        names = item.names;
        server = item.remote;
    }
    if (!discard.empty()) {
        {
            std::lock_guard lock(discard_mutex_);
            discard_jobs_.push_back({names, std::move(dir), rclone_remote_of(server), std::move(discard)});
        }
        discard_wake_.notify_all();
    }
    if (wake) wake_.notify_all();
    request_save();
    dirty_ = true;
    flush();
}

// Files of a folder by index ranges, their priority inside it (DOWNLOADS-UI.md 7): queued ones
// move in the start order, running ones get their new share at their next reads. One lock, one
// save and one flush for the batch; finished files keep theirs.
void App::set_file_priority(const Value& msg) {
    const int id = int_of(msg, "id", -1);
    const int gen = int_of(msg, "gen", -1);
    const int weight = weight_of(msg.string_of("priority").value_or("normal"));
    {
        std::lock_guard lock(mutex_);
        const auto it = std::ranges::find(items_, id, [](const auto& i) { return i->id; });
        if (it == items_.end()) return;
        Item& item = **it;
        // The rows of another generation: the page gets the new ones and asks again.
        if (item.gen != gen || !item.tree.listed || !item.names) return;
        bool changed = false;
        for (const auto k : queue_view::indices_of(msg.find("ranges"), item.tree.files.size())) {
            changed = folder::set_priority(item.tree, k, weight) || changed;
        }
        if (!changed) return;
        item.touched = true;
    }
    request_save();
    dirty_ = true;
    flush();
}

void App::show_file(int id, int gen, std::size_t index) {
    std::filesystem::path target;
    {
        std::lock_guard lock(mutex_);
        for (const auto& item : items_) {
            if (item->id != id || item->gen != gen || !item->names || index >= item->names->path.size()) continue;
            target = queue_view::target_of(folder::local_dir(item->dest, item->path), *item->names, index);
        }
    }
    if (!target.empty()) show_in_explorer(target);
}

void App::open_finished(int id, int gen, std::optional<std::size_t> index, bool run) {
    std::optional<open_file::Finished> done;
    std::string server;
    std::filesystem::path kept_own; // a file download done by Skip: the user's own file, kept
    {
        std::lock_guard lock(mutex_);
        const auto it = std::ranges::find(items_, id, [](const auto& i) { return i->id; });
        if (it == items_.end()) return;
        const Item& item = **it;
        server = item.remote;
        if (index) {
            // A file of a folder download, of the generation the page shows.
            if (item.mode == Mode::folder && item.gen == gen && item.names) {
                done = queue_view::finished_file(folder::local_dir(item.dest, item.path), *item.names, item.tree.files,
                                                 *index);
            }
        } else if (item.mode == Mode::file && item.status == Status::done) {
            done = item.finished;
            if (!done) kept_own = item_target(item);
        }
    }
    if (!kept_own.empty()) {
        return post_downloads_note(std::format("{} wasn't downloaded: Skip kept your own file there.",
                                               util::path_to_utf8(kept_own.filename())));
    }
    if (!done) return; // not finished: the page offers Open on finished rows only
    const auto name = util::path_to_utf8(done->path.filename());
    switch (open_file::check(*done)) {
    case open_file::Found::ok:
        break;
    case open_file::Found::gone:
        return post_downloads_note(std::format("{} isn't there any more: it was moved or deleted.", name));
    case open_file::Found::changed:
        return post_downloads_note(std::format("{} changed since grab downloaded it: open it from its folder.", name));
    }
    // A program, script, installer or shortcut runs: the page asks first (Run / Cancel).
    if (!run && open_file::runs_program(done->path)) {
        Value v = Value::make_object();
        v.set("type", str("openAsk"));
        v.set("id", num(id));
        if (index) {
            v.set("gen", num(gen));
            v.set("index", num(static_cast<double>(*index)));
        }
        v.set("name", str(name));
        v.set("server", str(server));
        return post(v);
    }
    const std::wstring file = done->path.wstring();
    const std::wstring folder = done->path.parent_path().wstring();
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.hwnd = host_.hwnd; // Windows' own dialogs (Open with, its errors) belong to the window
    info.lpVerb = L"open";
    info.lpFile = file.c_str();
    info.lpDirectory = folder.c_str(); // a program starts in its own folder, as from Explorer
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        const DWORD code = GetLastError();
        // Cancelled: the user closed Windows' Open with.
        if (code != ERROR_CANCELLED) post_downloads_note(std::format("Couldn't open {}: {}", name, util::error_message(code)));
    }
}

void App::open_dir(int id) {
    std::filesystem::path dir;
    {
        std::lock_guard lock(mutex_);
        for (const auto& item : items_) {
            if (item->id == id && item->mode == Mode::folder && item->status == Status::done) {
                dir = folder::local_dir(item->dest, item->path);
            }
        }
    }
    if (dir.empty()) return;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return post_downloads_note(
            std::format("{} isn't there any more: it was moved or deleted.", util::path_to_utf8(dir.filename())));
    }
    // Explorer on the folder; the Windows folder's, never an explorer.exe in the current folder
    // (G3), and never anything the folder's name could make Windows run.
    const auto explorer = proc::find_program("explorer");
    if (!explorer) return;
    const std::wstring args = L"\"" + dir.wstring() + L"\"";
    ShellExecuteW(host_.hwnd, L"open", explorer->c_str(), args.c_str(), nullptr, SW_SHOWNORMAL);
}

void App::post_downloads_note(const std::string& text) const {
    Value v = Value::make_object();
    v.set("type", str("downloadsNote"));
    v.set("text", str(text));
    post(v);
}

void App::set_limit(bool on, double mibps, int token) {
    if (!(mibps >= min_limit_mibps && mibps <= max_limit_mibps)) mibps = state_.limit_mibps; // also NaN
    const auto now = schedule::local_now();
    const auto scheduled = schedule::limit_at(state_.speed_schedule, now.minute);
    const schedule::Limit wanted{on, mibps};
    if (!scheduled) {
        state_.limit_on = on;
        state_.limit_mibps = mibps;
        state_.limit_change.reset();
    } else if (wanted == schedule::Limit{true, *scheduled}) {
        state_.limit_change.reset(); // back to what the schedule says
    } else {
        // Without a next change (one limit all week) it holds for a year, i.e. until undone.
        const auto next = schedule::next_change(state_.speed_schedule, now.minute);
        const std::int64_t until = next ? now.unix + std::int64_t{next->minutes} * 60 - now.second
                                        : now.unix + std::int64_t{365} * 24 * 3600;
        state_.limit_change = schedule::Override{wanted, *scheduled, until};
    }
    save_state();
    update_limit(token);
}

void App::set_schedule(const Value& rules) {
    state_.speed_schedule = schedule::from_json(rules);
    state_.limit_change.reset(); // the new rules apply at once
    save_state();
    update_limit();
}

void App::tick() {
    update_limit();
    release_viewer_seats();
}

schedule::Applied App::current_limit(const schedule::Now& now) const {
    return schedule::apply({state_.limit_on, state_.limit_mibps}, schedule::limit_at(state_.speed_schedule, now.minute),
                           state_.limit_change, now.unix);
}

void App::update_limit(int token) {
    const auto now = schedule::local_now();
    if (state_.limit_change &&
        !schedule::holds(*state_.limit_change, schedule::limit_at(state_.speed_schedule, now.minute), now.unix)) {
        state_.limit_change.reset();
        save_state();
    }
    const auto applied = current_limit(now);
    set_rate(applied.limit.on ? schedule::bytes_per_s(applied.limit.mibps) : 0);
    Value message = limit_message(now);
    auto text = json::stringify(message);
    if (text == limit_posted_ && token == 0) return;
    limit_posted_ = std::move(text);
    // The answer to the page's set: sent even when nothing changed (the same value set again), so
    // the page confirms it only once it applies, and only with what applies.
    if (token != 0) message.set("token", num(token));
    post(message);
}

void App::set_rate(std::uint64_t bytes_per_s) {
    if (bytes_per_s == limiter_.rate()) return;
    limiter_.set_rate(bytes_per_s); // the streams follow the new rate at once
    // The speeds shown start again from the change, so the new limit shows within about a second
    // rather than fading in over five (speed::Meter::restart); the times left go on.
    const auto now = speed::Clock::now();
    total_meter_.restart(now, received_.load());
    std::lock_guard lock(mutex_);
    for (const auto& i : items_) {
        if (i->metered) i->speed_meter.restart(now, i->received.load());
    }
}

// What the status bar and Settings show: the limit that applies and why, when the schedule
// next changes ("until") and to what, and the rules.
Value App::limit_message(const schedule::Now& now) const {
    const auto applied = current_limit(now);
    Value m = Value::make_object();
    m.set("type", str("limit"));
    m.set("on", Value::make_bool(applied.limit.on));
    m.set("mibps", num(applied.limit.mibps));
    m.set("source", str(applied.source == schedule::Source::schedule ? "schedule"
                        : applied.source == schedule::Source::change ? "change"
                                                                     : "own"));
    if (const auto next = schedule::next_change(state_.speed_schedule, now.minute)) {
        m.set("until", str(schedule::when_text(now.minute, next->minutes)));
        if (next->limit) m.set("nextMiBps", num(*next->limit));
    }
    m.set("rules", schedule::to_json(state_.speed_schedule));
    return m;
}

void App::move_item(int id, int before) {
    {
        std::lock_guard lock(mutex_);
        std::vector<int> ids;
        ids.reserve(items_.size());
        for (const auto& i : items_) ids.push_back(i->id);
        if (!move_before(ids, id, before)) return;
        // The list order is the queue order: workers take the first waiting item.
        std::vector<std::shared_ptr<Item>> reordered;
        reordered.reserve(items_.size());
        for (const int k : ids) {
            reordered.push_back(*std::ranges::find(items_, k, [](const auto& i) { return i->id; }));
        }
        items_ = std::move(reordered);
    }
    request_save();
    queue_changed_ = true;
    flush();
}

void App::set_priority(int id, int weight) {
    std::string server;
    bool running = false;
    {
        std::lock_guard lock(mutex_);
        for (auto& item : items_) {
            if (item->id != id || item->weight.load() == weight) continue;
            item->weight = weight; // the limiter reads it at every request
            server = item->remote;
            running = item->status == Status::running;
        }
    }
    if (running) connections_.set_weight(server, id, weight); // shares rebalance at the next reads
    request_save();
    dirty_ = true;
    flush();
}

void App::set_parallel(int n) {
    n = std::clamp(n, min_parallel, max_parallel);
    state_.parallel = n;
    save_state();
    {
        std::lock_guard lock(mutex_);
        parallel_ = n; // more: waiting workers start now; fewer: running ones finish first
    }
    wake_.notify_all();
    dirty_ = true;
}

void App::clear_finished() {
    {
        std::lock_guard lock(mutex_);
        std::erase_if(items_, [](const auto& i) {
            return i->status == Status::done || i->status == Status::failed ||
                   i->status == Status::cancelled;
        });
        std::erase_if(watches_, [&](const auto& w) {
            return std::ranges::none_of(items_, [&](const auto& i) { return i->id == w.first; });
        });
    }
    request_save(); // failed ones were kept in queue.json
    queue_changed_ = true;
    flush();
}

void App::open_item_folder(int id) {
    std::filesystem::path target;
    {
        std::lock_guard lock(mutex_);
        for (const auto& item : items_) {
            if (item->id == id) target = item_target(*item);
        }
    }
    if (!target.empty()) show_in_explorer(target);
}

void App::show_in_explorer(const std::filesystem::path& target) const {
    std::error_code ec;
    if (std::filesystem::exists(target, ec)) {
        // Open Explorer with the downloaded file or folder selected; the Windows folder's, never
        // an explorer.exe in the current folder (G3).
        const std::wstring args = L"/select,\"" + target.wstring() + L"\"";
        const auto explorer = proc::find_program("explorer");
        if (!explorer) return;
        ShellExecuteW(host_.hwnd, L"open", explorer->c_str(), args.c_str(), nullptr, SW_SHOWNORMAL);
    } else {
        ShellExecuteW(host_.hwnd, L"open", target.parent_path().c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
    }
}

void App::copy_text(const std::string& text) const {
    std::wstring w;
    for (const wchar_t c : util::to_wide(text)) {
        if (c == L'\n') w += L'\r';
        w += c;
    }
    if (!OpenClipboard(host_.hwnd)) return;
    EmptyClipboard();
    const std::size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* p = GlobalLock(mem)) {
            std::memcpy(p, w.c_str(), bytes);
            GlobalUnlock(mem);
            if (SetClipboardData(CF_UNICODETEXT, mem) == nullptr) GlobalFree(mem); // else the clipboard owns it
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
}

void App::worker_loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::shared_ptr<Item> item;
        {
            std::unique_lock lock(mutex_);
            auto next_queued = [&]() -> std::shared_ptr<Item> {
                // "At once" counts files: a folder's take places too.
                if (files_in_use() >= parallel_) return nullptr;
                // Never two downloads into the same file or folder at once: they would share
                // one partial file.
                auto same_target = [](const Item& a, const Item& b) {
                    // Where they write, as Windows names it: "D:\x\" and "d:\X" are one folder (G44).
                    return local::key(item_target(a)) == local::key(item_target(b));
                };
                // Nor more downloads on one server than its connections: past that the extra ones
                // would only wait for one (review G194); a queued one for another server goes first.
                auto server_full = [&](const Item& i) {
                    const auto running_there = std::ranges::count_if(items_, [&](const auto& r) {
                        return r->status == Status::running && r->remote == i.remote;
                    });
                    return running_there >= connections_.budget(i.remote);
                };
                auto it = std::ranges::find_if(items_, [&](const auto& i) {
                    return i->status == Status::queued && !server_full(*i) && std::ranges::none_of(items_, [&](const auto& r) {
                               return r->status == Status::running && same_target(*r, *i);
                           });
                });
                return it == items_.end() ? nullptr : *it;
            };
            wake_.wait(lock, stop, [&] { return next_queued() != nullptr; });
            if (stop.stop_requested()) return;
            item = next_queued();
            item->status = Status::running;
            item->stop = std::stop_source{};
            item->stop_reason = StopReason::none;
            item->error.clear();
            item->check = verify::Mark::none;
            // A folder is listed first (once), then checked again on every run before its files.
            if (item->mode == Mode::folder) item->phase = item->tree.listed ? Phase::checking : Phase::listing;
        }
        request_save();
        dirty_ = true;
        run_item(item, stop);
        wake_.notify_all(); // a slot is free for the next queued download
        request_save(true); // soon: a download ended
        dirty_ = true;

        Status status{};
        std::string name;
        std::string error;
        {
            std::lock_guard lock(mutex_);
            status = item->status;
            name = item->name;
            error = item->error;
        }
        if ((status == Status::done || status == Status::failed) && host_.on_finished) {
            host_.on_finished(name, status == Status::done, error);
        }
    }
}

void App::run_item(const std::shared_ptr<Item>& item, std::stop_token app_stop) {
    std::stop_token item_stop;
    {
        std::lock_guard lock(mutex_);
        item_stop = item->stop.get_token();
    }
    // Closing the app stops the running download too; it is resumed on the next start.
    std::stop_callback on_app_stop(app_stop, [item] { item->stop.request_stop(); });
    if (item->fake) return run_fake(*item, item_stop);

    std::error_code ec;
    std::filesystem::create_directories(item->dest, ec);
    if (ec) {
        std::lock_guard lock(mutex_);
        item->status = Status::failed;
        item->error = std::format("cannot create {}: {}", util::path_to_utf8(item->dest), ec.message());
        return;
    }
    const auto refusals_before = connections_.refusals(item->remote);
    if (item->mode == Mode::file) run_file(item, item_stop);
    else run_folder(item, item_stop);
    bool done = false;
    {
        std::lock_guard lock(mutex_);
        done = item->status == Status::done;
    }
    if (done) download_went_well(item->remote, refusals_before);

    // A stopped download ends paused or cancelled, as asked; closing the app leaves it paused.
    std::lock_guard lock(mutex_);
    item->speed = 0;
    item->eta.reset();
    item->connections = 0;
    item->current.clear();
    item->phase = Phase::none;
    if (item->status == Status::cancelled && item->mode == Mode::file && !twin_of(*item)) {
        fetch::discard(item_target(*item), item->path, {}, rclone_remote_of(item->remote));
    }
}

int App::connection_budget(const std::string& remote, const std::string& host, std::optional<int> configured) {
    auto limits = load_learned_limits(limits_path_); // the command may have learned since
    std::optional<int> learned;
    const int default_value = budget::default_budget(host);
    {
        std::lock_guard lock(mutex_);
        learned_ = std::move(limits);
        defaults_[remote] = default_value;
        if (auto it = learned_.find(remote); it != learned_.end()) learned = it->second;
    }
    return budget::effective_budget(configured, default_value, learned);
}

void App::on_budget_learned(const std::string& server, int budget) {
    {
        std::lock_guard lock(mutex_);
        learned_[server] = budget;
    }
    report_save("the learned connection limits", save_learned_limit(limits_path_, server, budget), limits_unsaved_);
}

void App::download_went_well(const std::string& server, std::uint64_t refusals_before) {
    if (connections_.refusals(server) != refusals_before) return;
    std::optional<int> raised;
    {
        std::lock_guard lock(mutex_);
        const auto it = learned_.find(server);
        const auto def = defaults_.find(server);
        if (it == learned_.end() || def == defaults_.end()) return;
        raised = budget::raised(it->second, def->second);
        if (raised) it->second = *raised;
        else learned_.erase(it);
    }
    report_save("the learned connection limits", save_learned_limit(limits_path_, server, raised), limits_unsaved_);
}

void App::run_file(const std::shared_ptr<Item>& item, const std::stop_token& item_stop) {
    auto set = [&](Status status, std::string error = {}, std::string note = {}) {
        std::lock_guard lock(mutex_);
        item->status = status;
        item->error = std::move(error);
        if (!note.empty()) item->note = std::move(note);
    };
    auto ctx = engine::load_context(config_path_, item->remote);
    if (!ctx) return set(Status::failed, ctx.error());
    connections_.configure(item->remote,
                           connection_budget(item->remote, ctx->remote.host, ctx->settings.max_connections));

    fetch::Download d;
    d.source = fetch::Source{.rclone_remote = ctx->settings.rclone_remote, .path = item->path};
    d.server = item->remote;
    d.id = item->id;
    existing::Choice choice{};
    {
        std::lock_guard lock(mutex_);
        d.target = item_target(*item);
        choice = item->existing;
        item->finished.reset();
    }
    // Done with the server's copy at `at` (`size` bytes): Open opens it.
    auto done_at = [&](const std::filesystem::path& at, std::uint64_t size) {
        std::lock_guard lock(mutex_);
        item->finished = open_file::Finished{.path = at, .size = size};
    };
    d.replace = choice == existing::Choice::replace;
    d.limiter = &limiter_;
    d.weight = &item->weight;
    // Its speed and time left, and the status bar's total, count the bytes as they come (flush).
    d.on_received = [this, item](std::uint64_t n) {
        item->received += n;
        received_ += n;
    };
    // Through grab's own SSH client, its connections kept open between files.
    auto target = engine::ssh_target(*ctx);
    if (!target) return set(Status::failed, target.error());
    d.pool = pools_.get(item->remote, *target, connections_.budget(item->remote));
    auto fetch_once = [&] {
        return fetch::run(d, connections_, [&](const fetch::Progress& p) {
            {
                std::lock_guard lock(mutex_);
                item->bytes = p.bytes;
                if (p.total > 0) item->total = p.total;
                item->connections = p.connections;
            }
            dirty_ = true; // the next flush sends it, with its speed (meter)
        }, item_stop);
    };
    auto result = fetch_once();
    if (result.outcome == fetch::Outcome::exists && choice == existing::Choice::keep_both) {
        // A copy an earlier Keep both kept that already is the server's file (review G112).
        if (const auto kept = existing::kept_copy(d.target, result.size, util::parse_rfc3339(result.modtime))) {
            done_at(*kept, result.size);
            return set(Status::done, {}, std::format("already there as {}, nothing fetched", util::path_to_utf8(kept->filename())));
        }
        const auto second = existing::second_copy(d.target);
        if (!second) return set(Status::failed, "no free name for a second copy next to " + util::path_to_utf8(d.target));
        // What was fetched goes on under the new name, done already if it was complete (G74).
        fetch::move_partial(d.target, *second, item->path, d.source.rclone_remote);
        {
            std::lock_guard lock(mutex_);
            item->local_name = second->filename();
            d.target = *second;
        }
        save_queue();
        result = fetch_once();
    }
    if (result.outcome == fetch::Outcome::exists && choice == existing::Choice::skip) {
        fetch::discard(d.target, item->path, {}, d.source.rclone_remote); // what was fetched won't be used (G74)
        return set(Status::done, {}, "skipped: your file is kept");
    }
    if (result.outcome == fetch::Outcome::exists) { // not answered yet: wait for the answer
        Asked asked{util::path_to_utf8(d.target.filename()), result.size, std::nullopt,
                    existing::local_file(d.target).value_or(existing::Local{})};
        if (const auto t = util::parse_rfc3339_seconds(result.modtime)) asked.modified = *t;
        std::lock_guard lock(mutex_);
        item->asked = {std::move(asked)};
        item->asked_count = 1;
        item->status = Status::paused;
        item->speed = 0;
        item->eta.reset();
        item->connections = 0;
        item->waiting = "A different file of that name is already there. Resume to choose: replace it, keep both, or skip.";
        return;
    }
    if (result.outcome == fetch::Outcome::failed) return set(Status::failed, result.error, result.note);
    if (result.outcome == fetch::Outcome::done) {
        // Settings → Downloads: check the copy against the server's checksum.
        bool check = false;
        {
            std::lock_guard lock(mutex_);
            check = verify_downloads_ && (!result.present || item->verify_pending);
            if (check) {
                item->verify_pending = true;
                item->phase = Phase::verifying;
                item->speed = 0;
                item->eta.reset();
                item->connections = 0;
            }
        }
        if (!check) {
            done_at(d.target, result.size);
            return set(Status::done, {}, result.present ? "already there, nothing fetched" : result.note);
        }
        dirty_ = true;
        // Against the server's file as this download fetched it: its size and time.
        const auto checked = verify::file(verify_request(*ctx, item, d.pool),
                                          verify::File{.path = item->path, .local = d.target, .size = result.size,
                                                       .modtime = result.modtime},
                                          item_stop);
        {
            std::lock_guard lock(mutex_);
            item->phase = Phase::none;
            if (!item_stop.stop_requested()) item->verify_pending = false;
        }
        if (!item_stop.stop_requested()) {
            const auto verdict = checked.files.front().verdict;
            if (verdict == verify::Verdict::differs || verdict == verify::Verdict::missing) {
                std::error_code ec;
                if (verdict == verify::Verdict::differs) std::filesystem::remove(d.target, ec); // not the server's file
                {
                    std::lock_guard lock(mutex_);
                    item->bytes = 0; // starts over: the row offers Retry, not Resume
                }
                return set(Status::failed, verdict == verify::Verdict::differs
                                               ? std::format("The downloaded copy {}, so it was deleted. Retry downloads it again.",
                                                             verify::summary(checked))
                                               : "The downloaded copy is gone. Retry downloads it again.");
            }
            {
                std::lock_guard lock(mutex_);
                item->check = verify::mark(checked);
            }
            done_at(d.target, result.size);
            return set(Status::done, {}, verify::summary(checked));
        }
    }
    StopReason reason{};
    {
        std::lock_guard lock(mutex_);
        reason = item->stop_reason;
    }
    set(reason == StopReason::cancel ? Status::cancelled : Status::paused);
}

verify::Request App::verify_request(const engine::Context& ctx, const std::shared_ptr<Item>& item,
                                    const std::shared_ptr<ssh::Pool>& download_pool) {
    verify::Request r;
    r.ctx = &ctx;
    r.connections = &connections_;
    r.server = item->remote;
    r.id = item->id;
    // Through grab's own client (RCLONE-REPLACEMENT.md, phase 5): the download's pool, shared with
    // Browse and searches, logs in once.
    if (download_pool) {
        r.pool = download_pool;
    } else if (auto target = engine::ssh_target(ctx)) {
        r.pool = pools_.get(item->remote, *target, connections_.budget(item->remote));
    } else {
        r.no_pool = target.error();
    }
    // A folder's "12 of 40", and its verified files saved now and then, so a crash keeps them.
    auto saved = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
    r.on_progress = [this, item, saved](std::size_t done, std::size_t total) {
        {
            std::lock_guard lock(mutex_);
            item->verify_done = done;
            item->verify_total = total;
        }
        item->touched = true; // its files' ✓
        dirty_ = true;
        if (const auto now = std::chrono::steady_clock::now(); item->mode == Mode::folder && now - *saved >= std::chrono::seconds(2)) {
            *saved = now;
            request_save();
        }
    };
    return r;
}

queue_view::Pass App::refresh_folder(Item& item, std::vector<queue_view::Shadow>* shadow) {
    auto p = queue_view::pass(item.tree.files, shadow, item.tree.small_connections > 0);
    item.bytes = p.totals.bytes;
    item.total = p.totals.total;
    item.connections = item.tree.connections;
    item.counts = p.counts;
    item.held = p.held;
    item.left_files = p.left_files;
    item.left_bytes = p.left_bytes;
    return p;
}

bool App::meter(Item& item, speed::Clock::time_point now) {
    if (item.status != Status::running || item.phase != Phase::none) {
        if (!item.metered) return false;
        // Stopped, or listing, checking or verifying: it starts over when it fetches again.
        item.metered = false;
        item.speed_meter = {};
        item.pace = {};
        item.countdown = {};
        item.speed = 0;
        item.eta.reset();
        return true;
    }
    item.metered = true;
    const std::uint64_t received = item.received.load();
    item.speed_meter.sample(now, received);
    item.speed = item.speed_meter.rate();
    std::optional<double> left;
    if (item.mode == Mode::folder) {
        // Its files fetched (done or failed) and bytes come; its time left by both (speed::Pace).
        const auto finished = item.counts[static_cast<std::size_t>(folder::State::done)] +
                              item.counts[static_cast<std::size_t>(folder::State::failed)];
        // From its first bytes, as its speed: the time spent connecting isn't its pace.
        if (item.speed_meter.since()) item.pace.sample(now, finished, received);
        left = item.pace.time_left(item.left_files, item.left_bytes, item.speed);
    } else if (item.total > item.bytes) {
        // A file: its bytes at its speed, after 5 s of them.
        left = speed::file_left(item.speed_meter.since(), now, item.total - item.bytes, item.speed);
    }
    item.eta = item.countdown.next(now, left);
    return true;
}

void App::new_names(Item& item) {
    auto names = std::make_shared<const queue_view::Names>(queue_view::names_of(item.tree.files));
    std::lock_guard lock(mutex_);
    item.names = std::move(names);
    item.gen = next_gen_++;
    item.touched = true;
    dirty_ = true;
}

void App::discard_folder_partials(const Item& item) const {
    folder::discard_partials(item.path, folder::local_dir(item.dest, item.path), item.tree, rclone_remote_of(item.remote));
}

std::shared_ptr<App::Item> App::twin_of(const Item& item) const {
    const auto key = local::key(item_target(item));
    const auto it = std::ranges::find_if(items_, [&](const auto& o) {
        return o.get() != &item && o->remote == item.remote && o->path == item.path && o->mode == item.mode &&
               o->status != Status::done && o->status != Status::cancelled && local::key(item_target(*o)) == key;
    });
    return it == items_.end() ? nullptr : *it;
}

int App::files_in_use(const Item* except) const {
    int used = 0;
    for (const auto& i : items_) {
        if (i.get() == except || i->status != Status::running) continue;
        used += i->mode == Mode::folder ? std::max(1, i->tree.files_at_once) : 1;
    }
    return used;
}

std::string App::rclone_remote_of(const std::string& server) const {
    if (auto cfg = load_grab_config(config_path_)) {
        for (const auto& r : cfg->remotes) {
            if (r.name == server) return r.rclone_remote;
        }
    }
    return server;
}

// Without the lock: the next flush counts the folder's files, once for all the events since.
void App::folder_event(Item& item, folder::Event e) {
    item.touched = true;
    dirty_ = true;
    if (e == folder::Event::checkpoint) request_save();
    // A file of it ended: the place may go to another download.
    if (e == folder::Event::changed) wake_.notify_all();
}

// A folder is listed once, then its files are fetched by folder::run (shared with the grab
// command). The whole folder is one user of its server's connection budget.
void App::run_folder(const std::shared_ptr<Item>& item, const std::stop_token& item_stop) {
    auto fail = [&](std::string error) {
        std::lock_guard lock(mutex_);
        item->status = Status::failed;
        item->error = std::move(error);
    };
    auto ctx = engine::load_context(config_path_, item->remote);
    if (!ctx) return fail(ctx.error());
    connections_.configure(item->remote,
                           connection_budget(item->remote, ctx->remote.host, ctx->settings.max_connections));

    folder::Job job;
    job.ctx = &*ctx;
    job.connections = &connections_;
    job.limiter = &limiter_;
    job.server = item->remote;
    job.id = item->id;
    job.weight = &item->weight;
    job.remote_path = item->path;
    job.dest = item->dest;
    job.on_event = [this, item](folder::Event e) { folder_event(*item, e); };
    // Its speed and time left, and the status bar's total, count the bytes as they come (flush).
    job.on_received = [this, item](std::uint64_t n) {
        item->received += n;
        received_ += n;
    };
    // Its files at once: what "At once" leaves of the files other downloads hold.
    job.files_at_once = [this, item] {
        std::lock_guard lock(mutex_);
        return std::max(1, parallel_ - files_in_use(item.get()));
    };
    // Walked and fetched through grab's own SSH client.
    auto target = engine::ssh_target(*ctx);
    if (!target) return fail(target.error());
    job.pool = pools_.get(item->remote, *target, connections_.budget(item->remote));
    {
        // Failed files kept from before a restart are tried again, as a restart did up to 0.21.
        std::lock_guard lock(mutex_);
        if (std::exchange(item->retry_failed, false)) {
            folder::retry_files(item->tree, false);
            item->touched = true;
        }
    }

    if (auto listed = folder::list(job, item->tree, mutex_, item_stop); !listed) return fail(listed.error());
    bool named = false;
    {
        std::lock_guard lock(mutex_);
        item->phase = Phase::checking;
        named = item->names && item->names->path.size() == item->tree.files.size();
    }
    if (!named) new_names(*item); // just listed: the page's rows come
    dirty_ = true;
    // Files already complete on disk (an earlier run, or downloaded before) are not fetched
    // again; big ones with a partial file continue from it.
    folder::mark_existing(job, item->tree, mutex_);
    // Different files already where some of the folder's go: asked about before anything is
    // fetched (it waits, paused), or answered already.
    if (!item_stop.stop_requested()) {
        const auto found = folder::conflicts(job, item->tree, mutex_);
        existing::Choice choice{};
        {
            std::lock_guard lock(mutex_);
            choice = item->existing;
        }
        if (!found.empty() && choice == existing::Choice::ask) {
            const std::string folder_name = util::path_to_utf8(folder::local_dir(item->dest, item->path).filename());
            std::vector<Asked> asked;
            for (const auto& c : found) {
                if (asked.size() == 5) break;
                asked.push_back(Asked{folder_name + "/" + c.path, c.size, util::parse_rfc3339_seconds(c.modtime),
                                      existing::local_file(c.local).value_or(existing::Local{})});
            }
            std::lock_guard lock(mutex_);
            refresh_folder(*item);
            item->asked = std::move(asked);
            item->asked_count = found.size();
            item->status = Status::paused;
            item->waiting = std::format("{} already here differ{} from the server's. Resume to choose: replace, keep both, or skip.",
                                     found.size() == 1 ? "1 file" : std::format("{} files", found.size()),
                                     found.size() == 1 ? "s" : "");
            return;
        }
        if (!found.empty()) {
            {
                std::lock_guard lock(mutex_);
                folder::apply(choice, found, folder::local_dir(item->dest, item->path), item->tree);
                refresh_folder(*item);
                item->touched = true;
            }
            if (choice == existing::Choice::keep_both) new_names(*item); // second-copy names
        }
        job.existing = choice;
    }
    // Free space, now that the folder's size is known: one that can't fit waits, paused, before
    // anything is fetched. Resume goes ahead (after freeing space, or anyway).
    if (!item_stop.stop_requested()) {
        bool check = false;
        std::uint64_t needed = 0;
        {
            std::lock_guard lock(mutex_);
            check = !item->space_ok && item->tree.listed;
            const auto t = folder::totals(item->tree);
            needed = t.total > t.bytes ? t.total - t.bytes : 0;
        }
        if (const auto free = check ? space::free_bytes(item->dest) : std::nullopt; free && needed > *free) {
            auto text = space::shortage(space::drive_of(item->dest), needed, *free);
            text[0] = 'N';
            std::lock_guard lock(mutex_);
            refresh_folder(*item);
            item->status = Status::paused;
            item->space_ok = true;
            item->waiting = text + ". Free some space and Resume, or Resume to download anyway.";
            return;
        }
    }
    {
        std::lock_guard lock(mutex_);
        item->phase = Phase::none; // fetching: the first file's start shows it
    }
    folder::run(job, item->tree, mutex_, item_stop);

    // Settings → Downloads: check what this download fetched against the server's checksums.
    std::string checked_note;
    if (!item_stop.stop_requested() && verify_downloads_) {
        bool any = false;
        {
            std::lock_guard lock(mutex_);
            any = std::ranges::any_of(item->tree.files, [](const folder::File& f) {
                return f.status == folder::State::done && f.fetched && !f.verified;
            });
            if (any) item->phase = Phase::verifying;
            item->verify_done = 0;
            item->verify_total = 0;
        }
        if (any) {
            dirty_ = true;
            const auto checked = folder::verify_files(job, item->tree, mutex_, verify_request(*ctx, item, job.pool), item_stop);
            {
                std::lock_guard lock(mutex_);
                item->phase = Phase::none;
                if (checked.files > 0 && !item_stop.stop_requested()) item->check = verify::mark(checked.report);
            }
            if (checked.files > 0 && !item_stop.stop_requested()) checked_note = checked.summary();
        }
    }

    bool cancelled = false;
    {
        std::lock_guard lock(mutex_);
        refresh_folder(*item);
        item->touched = true;
        if (item_stop.stop_requested()) {
            cancelled = item->stop_reason == StopReason::cancel;
            item->status = cancelled ? Status::cancelled : Status::paused;
        } else if (const auto o = folder::outcome(item->tree); o.failed > 0) {
            item->status = Status::failed;
            item->error = folder::failed_text(o, checked_note); // a failed row shows only this
        } else {
            // Files paused one by one keep the folder paused until they are resumed or skipped.
            item->status = o.waiting > 0 ? Status::paused : Status::done;
            if (!checked_note.empty()) item->note = checked_note;
        }
        // Folders inside it the server wouldn't list: the rest comes, and the row's message says
        // what's missing (queue_item, "unreadable").
    }
    if (cancelled) {
        std::lock_guard lock(mutex_);
        discard_folder_partials(*item);
    }
}

json::Value App::queue_item(const Item& i, int position) const {
    Value v = Value::make_object();
    v.set("id", num(i.id));
    v.set("remote", str(i.remote));
    v.set("mode", str(i.mode == Mode::folder ? "folder" : "file"));
    v.set("path", str(i.path));
    v.set("name", str(i.name));
    v.set("dest", str(util::path_to_utf8(i.dest)));
    v.set("local", str(util::path_to_utf8(item_target(i)))); // where it lands
    v.set("status", str(status_name(static_cast<int>(i.status))));
    if (!i.asked.empty()) v.set("ask", asked_json(i.asked, i.asked_count));
    v.set("bytes", num(static_cast<double>(i.bytes)));
    v.set("total", num(static_cast<double>(i.total)));
    // As the row shows them (speed::shown, whole seconds): a change it wouldn't show isn't sent.
    v.set("speed", num(static_cast<double>(speed::shown(i.speed))));
    v.set("eta", i.eta ? num(std::round(*i.eta)) : Value{});
    v.set("current", str(i.current));
    v.set("error", str(i.error));
    v.set("note", str(i.note));
    // A file download Skip ended by keeping the user's own file: done, but nothing of grab's to open.
    if (i.mode == Mode::file && i.status == Status::done && !i.finished) v.set("kept", Value::make_bool(true));
    if (!i.waiting.empty()) v.set("waiting", str(i.waiting)); // why it waits paused: a message row
    v.set("connections", num(i.connections));
    // Listing, checking or verifying, while running (DOWNLOADS-UI.md 5.3), and the ✓ or ⚠ after a check.
    const char* phase = i.status == Status::running ? phase_name(static_cast<int>(i.phase)) : nullptr;
    if (phase != nullptr) v.set("phase", str(phase));
    if (i.check != verify::Mark::none) v.set("check", str(i.check == verify::Mark::ok ? "ok" : "warn"));
    // A folder's files checked so far: "Verifying 12/40".
    if (i.phase == Phase::verifying && i.mode == Mode::folder && i.verify_total > 0) {
        Value checked = Value::make_object();
        checked.set("done", num(static_cast<double>(i.verify_done)));
        checked.set("total", num(static_cast<double>(i.verify_total)));
        v.set("verified", std::move(checked));
    }
    v.set("priority", str(priority_name(i.weight.load())));
    if (position > 0) v.set("position", num(position)); // "Queued #2"
    if (i.mode == Mode::folder && i.tree.listed) {
        // Its files by state, for the folder's row, as the last pass over them counted; the files
        // themselves come by "watchFiles", of generation `gen` (DOWNLOADS-UI.md 6.3).
        auto count = [&](folder::State s) { return num(static_cast<double>(i.counts[static_cast<std::size_t>(s)])); };
        Value files = Value::make_object();
        files.set("total", num(static_cast<double>(i.tree.files.size())));
        files.set("queued", count(folder::State::queued));
        files.set("running", count(folder::State::running));
        files.set("paused", count(folder::State::paused));
        files.set("done", count(folder::State::done));
        files.set("failed", count(folder::State::failed));
        files.set("skipped", count(folder::State::skipped));
        // Big files waiting for its small files, which hold its connections ("Waiting").
        if (i.held > 0) files.set("held", num(static_cast<double>(i.held)));
        v.set("files", std::move(files));
        if (i.gen != 0) v.set("gen", num(i.gen));
        // Folders inside it the server wouldn't list, for a message row under it.
        if (!i.tree.unreadable.empty()) v.set("unreadable", str(folder::unreadable_text(i.tree.unreadable)));
    }
    if (i.expanded) v.set("expanded", Value::make_bool(*i.expanded)); // the user's ▸/▾
    return v;
}

// Unfinished downloads from the last run come back paused, with their progress.
void App::restore_queue() {
    auto queue = read_queue(queue_path_);
    if (!queue) {
        // This run didn't load it, so its saves (at the latest when the window closes) must not
        // delete or replace it (review G212).
        return set_aside("The saved download queue", queue_path_, queue.error(), "it starts empty");
    }
    if (!*queue) return;
    for (const auto& saved : **queue) {
        auto item = std::make_shared<Item>();
        item->id = next_id_++;
        item->remote = saved.remote;
        item->mode = saved.mode == "folder" ? Mode::folder : Mode::file;
        item->path = saved.path;
        item->name = saved.name;
        item->dest = util::path_from_utf8(saved.dest);
        item->total = saved.total;
        item->space_ok = saved.space_ok;
        item->existing = existing::parse(saved.existing).value_or(existing::Choice::ask);
        item->local_name = util::path_from_utf8(saved.local_name);
        item->expanded = saved.expanded;
        // Saved for an update's restart while running or queued: it goes on by itself.
        item->status = saved.resume ? Status::queued : Status::paused;
        item->weight = weight_of(saved.priority);
        if (item->mode == Mode::file) {
            if (auto p = fetch::saved_progress(item_target(*item), item->path, {}, rclone_remote_of(item->remote))) {
                item->bytes = p->first;
                item->total = p->second;
            }
        } else if (!saved.files.empty()) {
            const auto dir = folder::local_dir(item->dest, item->path);
            for (const auto& f : saved.files) {
                auto c = folder::make_file(f.path, f.size, f.modtime);
                c.status = f.state == "done"      ? folder::State::done
                           : f.state == "skipped" ? folder::State::skipped
                           : f.state == "paused"  ? folder::State::paused
                           : f.state == "failed"  ? folder::State::failed
                                                  : folder::State::queued;
                // A failed file shows with its error until the folder runs again (retry_failed).
                if (c.status == folder::State::failed) {
                    c.error = f.error;
                    item->retry_failed = true;
                }
                c.fetched = f.fetched;
                c.verified = f.verified;
                c.weight = weight_of(f.priority);
                if (!f.local.empty()) {
                    c.local = util::path_from_utf8(f.local);
                    c.big = true; // a second copy: grab writes it under that name
                }
                if (c.status == folder::State::done) c.bytes = c.size;
                else if (c.big) {
                    if (auto p = fetch::saved_progress(folder::target_of(dir, c), folder::child_remote_path(item->path, c.path),
                                                       {}, rclone_remote_of(item->remote))) {
                        c.bytes = p->first;
                    }
                }
                item->tree.files.push_back(std::move(c));
            }
            item->tree.listed = true;
            refresh_folder(*item);
            item->names = std::make_shared<const queue_view::Names>(queue_view::names_of(item->tree.files));
            item->gen = next_gen_++;
        }
        items_.push_back(std::move(item));
    }
}

namespace {

// A folder's files as queue.json keeps them, copied under the queue's lock in a compact form: each
// file's state and flags in two bytes and every time in one string; the names come from the
// immutable queue_view::Names, copied here only for a folder listed a moment ago without them.
struct SavedFiles {
    std::shared_ptr<const queue_view::Names> names;
    std::vector<std::string> paths;                                     // without names
    std::vector<std::pair<std::size_t, std::filesystem::path>> locals; // without names, by index
    std::vector<std::uint64_t> sizes;
    std::vector<folder::State> states;
    std::vector<std::uint8_t> flags; // 1: fetched, 2: verified (done files)
    std::vector<std::uint8_t> weights; // priorities
    std::string times;
    std::vector<std::size_t> time_ends;
    std::vector<std::pair<std::size_t, std::string>> errors; // by index
};

SavedFiles copy_files(const std::vector<folder::File>& files, const std::shared_ptr<const queue_view::Names>& names) {
    SavedFiles s;
    const auto n = files.size();
    if (names && names->path.size() == n) {
        s.names = names;
    } else {
        s.paths.reserve(n);
        for (std::size_t k = 0; k < n; ++k) {
            s.paths.push_back(files[k].path);
            if (!files[k].local.empty()) s.locals.emplace_back(k, files[k].local);
        }
    }
    s.sizes.reserve(n);
    s.states.reserve(n);
    s.flags.reserve(n);
    s.weights.reserve(n);
    s.time_ends.reserve(n);
    s.times.reserve(n * 25);
    for (std::size_t k = 0; k < n; ++k) {
        const auto& c = files[k];
        const bool done = c.status == folder::State::done;
        s.sizes.push_back(c.size);
        s.states.push_back(c.status);
        s.flags.push_back(static_cast<std::uint8_t>((done && c.fetched ? 1 : 0) | (done && c.verified ? 2 : 0)));
        s.weights.push_back(static_cast<std::uint8_t>(c.weight));
        s.times += c.modtime;
        s.time_ends.push_back(s.times.size());
        if (c.status == folder::State::failed) s.errors.emplace_back(k, c.error);
    }
    return s;
}

std::vector<SavedFile> saved_files(const SavedFiles& s) {
    std::vector<SavedFile> out;
    out.reserve(s.states.size());
    std::size_t time_at = 0;
    auto local = s.locals.begin(); // both by index
    auto error = s.errors.begin();
    for (std::size_t k = 0; k < s.states.size(); ++k) {
        SavedFile f;
        f.path = s.names ? s.names->path[k] : s.paths[k];
        f.size = s.sizes[k];
        f.modtime = s.times.substr(time_at, s.time_ends[k] - time_at);
        time_at = s.time_ends[k];
        switch (s.states[k]) {
        case folder::State::done: f.state = "done"; break;
        case folder::State::skipped: f.state = "skipped"; break;
        case folder::State::paused: f.state = "paused"; break;
        case folder::State::failed: f.state = "failed"; break;
        case folder::State::queued:
        case folder::State::running: f.state = "queued"; break; // a running one goes on from its partial file
        }
        f.fetched = (s.flags[k] & 1) != 0;
        f.verified = (s.flags[k] & 2) != 0;
        f.priority = priority_name(s.weights[k]);
        if (s.names) {
            if (const auto it = s.names->local.find(k); it != s.names->local.end()) f.local = util::path_to_utf8(it->second);
        } else if (local != s.locals.end() && local->first == k) {
            f.local = util::path_to_utf8((local++)->second);
        }
        if (error != s.errors.end() && error->first == k) f.error = (error++)->second;
        out.push_back(std::move(f));
    }
    return out;
}

} // namespace

void App::save_queue() const {
    // The snapshot is taken under the file's lock too, so two savers can't write in the opposite
    // order of their snapshots and leave an older queue on disk (review G53).
    std::lock_guard file_lock(queue_file_mutex_);
    const auto started = std::chrono::steady_clock::now();
    std::vector<SavedDownload> unfinished;
    std::vector<SavedFiles> files; // by unfinished's index
    double held = 0;
    {
        std::lock_guard lock(mutex_);
        const auto locked = std::chrono::steady_clock::now();
        for (const auto& i : items_) {
            if (i->status == Status::done || i->status == Status::cancelled) continue;
            SavedDownload d{i->remote, i->mode == Mode::folder ? "folder" : "file", i->path, i->name,
                            util::path_to_utf8(i->dest), i->total, {}, priority_name(i->weight.load())};
            d.resume = resume_after_restart_ && i->resume_mark;
            d.space_ok = i->space_ok;
            if (i->existing != existing::Choice::ask) d.existing = std::string(existing::name(i->existing));
            d.local_name = util::path_to_utf8(i->local_name);
            d.expanded = i->expanded;
            files.push_back(copy_files(i->tree.files, i->names));
            unfinished.push_back(std::move(d));
        }
        held = ms_since(locked);
    }
    for (std::size_t k = 0; k < unfinished.size(); ++k) unfinished[k].files = saved_files(files[k]);
    std::error_code ec;
    if (unfinished.empty()) {
        std::filesystem::remove(queue_path_, ec);
        return;
    }
    const auto text = queue_to_json(unfinished);
    report_save("the download queue", util::write_file_atomic(queue_path_, text), queue_unsaved_);
    if (test_logging()) test_log(std::format("save lock={:.2f}ms total={:.0f}ms bytes={}", held, ms_since(started), text.size()));
}

// ---- updates ------------------------------------------------------------------------------

void App::post_update() const {
    Value v = Value::make_object();
    v.set("type", str("update"));
    v.set("state", str(update_.state));
    v.set("version", str(update_.version));
    v.set("page", str(update_.page));
    v.set("phase", str(update_.phase));
    v.set("error", str(update_.error));
    v.set("completing", Value::make_bool(update_.completing));
    v.set("note", str(update_.note));
    post(v);
}

void App::maybe_check_updates() {
    if (update_checks_started_) return;
    update_checks_started_ = true;
    const auto current = update::parse_version(GRAB_VERSION).value_or(update::Version{});
    // An update that failed while no window followed it (an installer update closes the one that
    // started it) is shown until it is dismissed: "Update failed: <why>" with Try again.
    // This copy's (grab.exe and grab-gui.exe share a folder), not another copy's sharing %LOCALAPPDATA%.
    if (auto failed = update::pending_failure(update::result_path(), current, util::self_exe_path().parent_path())) {
        update_ = UpdateBanner{"failed", failed->to, {}, {}, failed->error, false, {}};
        if (update_.error.empty()) update_.error = std::format("grab update exited with code {}", failed->exit_code);
        post_update();
    } else if (auto seen = update::parse_version(state_.latest_version); seen && *seen > current) {
        // A newer release an earlier check found shows at once, without asking GitHub again.
        update_ = UpdateBanner{"available", update::to_string(*seen),
                               update::is_release_page(state_.latest_page) ? state_.latest_page : "", {}, {}, false, {}};
        post_update();
    }
    if (state_.update_check && update::update_due(state_.last_update_check, unix_now())) start_update_check(false);
}

void App::start_update_check(bool manual) {
    if (update_check_busy_.exchange(true)) return;
    update_check_thread_ = std::jthread([this, manual](std::stop_token stop) {
        std::optional<Activity> act(std::in_place, *this, "Checking GitHub for a grab update…", "update", !manual);
        auto result = update::check(true, stop);
        if (!result) act->failed();
        act.reset();
        update_check_busy_ = false;
        if (stop.stop_requested()) return;
        host_.defer([this, manual, result = std::move(result)] { on_update_checked(result, manual); });
    });
}

void App::on_update_checked(const std::expected<update::Check, std::string>& result, bool manual) {
    Value msg = Value::make_object();
    msg.set("type", str("updateChecked"));
    if (!result) { // a failed automatic check stays silent; the next start tries again
        msg.set("ok", Value::make_bool(false));
        msg.set("text", str("Could not check: " + result.error()));
        if (manual) post(msg);
        return;
    }
    const auto& c = *result;
    const std::string latest = update::to_string(c.release.version);
    state_.last_update_check = unix_now();
    state_.latest_version = latest;
    state_.latest_page = c.release.page;
    save_state();
    // A failure stays until it is dismissed; its Try again installs the latest release anyway.
    if (update_.state != "running" && update_.state != "installed" && update_.state != "failed") {
        if (c.available()) {
            update_ = UpdateBanner{"available", latest, c.release.page, {}, {}, c.release.version == c.current, {}};
        } else if (update_.state == "available") {
            update_ = {};
        }
        post_update();
    }
    if (manual) {
        msg.set("ok", Value::make_bool(true));
        msg.set("available", Value::make_bool(c.available()));
        msg.set("text", str(!c.available()                  ? std::format("grab {} is up to date", update::to_string(c.current))
                            : c.release.version == c.current ? std::format("This folder lacks {}", util::join(c.missing, ", "))
                                                             : std::format("grab {} is available", latest)));
        post(msg);
    }
}

// `grab update` does the work (verify, then the installer or the portable swap). It runs on its
// own, outside grab-gui's job, because for the installer it closes grab-gui (and starts it again);
// its output, written to a file, tells the banner how far it got.
void App::start_update() {
    if (update_.state == "running") return;
    auto fail = [this](std::string why) {
        update_.state = "failed";
        update_.error = std::move(why);
        post_update();
    };
    std::error_code ec;
    const auto grab_exe = util::self_exe_path().parent_path() / L"grab.exe";
    if (!std::filesystem::is_regular_file(grab_exe, ec)) {
        return fail("grab.exe is missing next to grab-gui.exe; get grab from the release page");
    }
    const auto log = std::filesystem::temp_directory_path(ec) / std::format("grab-update-{}.log", GetCurrentProcessId());
    auto started = proc::start_independent(std::vector<std::string>{util::path_to_utf8(grab_exe), "update"}, log);
    if (!started) return fail(started.error());
    update_.state = "running";
    update_.phase = "checking";
    update_.error.clear();
    update_.note.clear();
    post_update();

    update_thread_ = std::jthread([this, child = std::move(*started), log](std::stop_token stop) mutable {
        std::uint64_t offset = 0;
        std::string pending; // output after the last line break
        std::string last_error;
        auto take = [&](std::string line) {
            if (line.ends_with('\r')) line.pop_back();
            if (const auto cr = line.rfind('\r'); cr != std::string::npos) line.erase(0, cr + 1); // curl's progress redraws
            if (update::phase_of(line) == update::Phase::failed) last_error = line.substr(update::text::error.size());
            host_.defer([this, line = std::move(line)] { on_update_line(line); });
        };
        auto read_new = [&] {
            std::ifstream in(log, std::ios::binary);
            if (!in) return;
            in.seekg(static_cast<std::streamoff>(offset));
            const std::string chunk((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            offset += chunk.size();
            pending += chunk;
            for (std::size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
                take(pending.substr(0, nl));
                pending.erase(0, nl + 1);
            }
        };
        std::optional<int> code;
        while (!(code = child.wait(std::chrono::milliseconds(250)))) {
            if (stop.stop_requested()) return; // grab-gui is closing; grab update carries on
            read_new();
        }
        read_new();
        if (!pending.empty()) take(pending);
        std::error_code rm;
        std::filesystem::remove(log, rm);
        host_.defer([this, exit_code = *code, last_error] { on_update_finished(exit_code, last_error); });
    });
}

void App::on_update_line(const std::string& line) {
    const auto phase = update::phase_of(line);
    if (phase == update::Phase::failed || phase == update::Phase::other) return;
    // A try GitHub failed shows ("GitHub answered 504 ...; trying again in 3 s (try 2 of 6)") until
    // one gets through or the next step starts.
    update_.note = phase == update::Phase::retrying ? line : std::string();
    switch (phase) {
    case update::Phase::checking: update_.phase = "checking"; break;
    case update::Phase::downloading: update_.phase = "downloading"; break;
    case update::Phase::verified: update_.phase = "verified"; break;
    case update::Phase::installing:
        // grab-gui is closed for the installer and started again afterwards (the close request
        // marks them too): running downloads stop there (partial files kept) and continue in the
        // new one.
        update_.phase = "installing";
        mark_for_restart(true);
        break;
    case update::Phase::done: update_.phase = "done"; break;
    case update::Phase::up_to_date: update_.phase = "up_to_date"; break;
    case update::Phase::retrying:
    case update::Phase::retried:
    case update::Phase::failed:
    case update::Phase::other: break;
    }
    if (update_.state == "running") post_update();
}

void App::on_update_finished(int exit_code, const std::string& last_error) {
    if (resume_after_restart_) mark_for_restart(false); // still here: nothing to continue after
    update_.note.clear();
    if (exit_code == 0 && update_.phase == "done") {
        update_.state = "installed"; // a portable swap, or an installer that left this window open
    } else if (exit_code == 0) {
        update_ = {}; // nothing newer after all
    } else {
        update_.state = "failed";
        update_.error = !last_error.empty() ? last_error : std::format("grab update exited with code {}", exit_code);
    }
    post_update();
}

void App::dismiss_update() {
    if (update_.state != "failed") return;
    update_ = {};
    std::error_code ec;
    std::filesystem::remove(update::result_path(), ec);
}

void App::restart_for_update() {
    std::error_code ec;
    const auto gui_exe = util::self_exe_path().parent_path() / L"grab-gui.exe";
    const auto log = std::filesystem::temp_directory_path(ec) / "grab-gui-restart.log";
    mark_for_restart(true);
    // "--restart PID": the new grab-gui waits for this one to exit instead of handing over to it.
    auto started = proc::start_independent(
        std::vector<std::string>{util::path_to_utf8(gui_exe), "--restart", std::to_string(GetCurrentProcessId())}, log);
    if (!started) {
        mark_for_restart(false);
        update_.state = "failed";
        update_.error = "cannot start grab again: " + started.error();
        post_update();
        return;
    }
    if (host_.quit) host_.quit(); // saves the queue again on the way out, still marked
}

void App::mark_for_restart(bool on) {
    {
        std::lock_guard lock(mutex_);
        for (auto& item : items_) {
            item->resume_mark = on && (item->status == Status::running || item->status == Status::queued);
        }
    }
    resume_after_restart_ = on;
    save_queue();
}

void App::open_release_page() const {
    if (!update::is_release_page(update_.page)) return;
    ShellExecuteW(host_.hwnd, L"open", util::to_wide(update_.page).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---- the page's view of the queue (DOWNLOADS-UI.md 6) -------------------------------------------

void App::post_ui(std::string text) const {
    assert(GetCurrentThreadId() == ui_thread_ && "the queue's messages go from the UI thread");
    host_.post(std::move(text));
}

void App::flush() {
    if (!page_ready_) return; // its init asks for everything
    // What the downloads receive, all together: the status bar's total, when it would show another
    // value (speed::shown).
    // Nothing fetching (as of the last flush): 0 at once rather than the fading of the last bytes.
    const auto now = speed::Clock::now();
    if (metering_) total_meter_.sample(now, received_.load());
    else total_meter_ = {};
    if (const auto total = speed::shown(total_meter_.rate()); total != total_sent_) {
        total_sent_ = total;
        post_ui(std::format(R"({{"type":"speed","total":{}}})", total));
    }
    const bool full = std::exchange(queue_changed_, false);
    // A download that fetches has a new speed and time left at every flush, events or not.
    if (!dirty_.exchange(false) && !full && !metering_) return;
    struct Delta {
        int id = 0;
        int gen = 0;
        std::vector<queue_view::Row> rows;
    };
    std::vector<std::pair<int, Value>> rows;
    std::vector<Delta> deltas;
    std::vector<int> fresh; // shown folders with new names: their files go again
    std::size_t passed = 0; // files gone over
    int parallel = 0;
    double held = 0;
    {
        std::lock_guard lock(mutex_);
        const auto locked = std::chrono::steady_clock::now();
        int waiting = 0;
        bool metering = false;
        for (const auto& i : items_) {
            const bool touched = i->touched.exchange(false);
            const auto w = watches_.find(i->id);
            Watch* watch = w == watches_.end() ? nullptr : &w->second;
            // Changes made while its chunks went had no shadow to diff against: they go now.
            const bool catch_up = watch != nullptr && !watch->send && std::exchange(watch->catch_up, false);
            if (i->mode == Mode::folder && i->tree.listed &&
                (full || touched || catch_up || i->status == Status::running)) {
                // The folder's totals and counts, and the shown files that changed, in one pass.
                const bool diff = watch != nullptr && !watch->send && watch->gen != 0 && watch->gen == i->gen;
                auto p = refresh_folder(*i, diff ? &watch->shadow : nullptr);
                passed += i->tree.files.size();
                if (!p.changed.empty()) deltas.push_back({i->id, i->gen, std::move(p.changed)});
            }
            if (watch != nullptr && !watch->send && i->names && i->gen != 0 && watch->gen != i->gen) fresh.push_back(i->id);
            metering = meter(*i, now) || metering; // after the pass: a folder's files left
            rows.emplace_back(i->id, queue_item(*i, i->status == Status::queued ? ++waiting : 0));
        }
        metering_ = metering;
        parallel = parallel_;
        held = ms_since(locked);
    }
    // The downloads: all of them when they came, went or moved, else the rows that changed.
    std::vector<std::pair<int, std::string>> texts;
    texts.reserve(rows.size());
    for (const auto& [id, v] : rows) texts.emplace_back(id, json::stringify(v));
    const auto changed = queue_view::changed_items(item_sent_, texts);
    std::size_t queue_bytes = 0;
    if (full || !changed.empty() || parallel != parallel_sent_) {
        std::string text = full ? R"({"type":"queue","items":[)" : R"({"type":"queueDelta","items":[)";
        bool first = true;
        auto add = [&](const std::string& t) {
            if (!first) text += ',';
            first = false;
            text += t;
        };
        if (full) {
            for (const auto& t : texts) add(t.second);
        } else {
            for (const auto k : changed) add(texts[k].second);
        }
        text += std::format(R"(],"parallel":{}}})", parallel);
        parallel_sent_ = parallel;
        queue_bytes = text.size();
        post_ui(std::move(text));
    }
    std::size_t delta_rows = 0;
    std::size_t delta_bytes = 0;
    for (const auto& d : deltas) {
        auto text = queue_view::file_delta_message(d.id, d.gen, d.rows);
        delta_rows += d.rows.size();
        delta_bytes += text.size();
        post_ui(std::move(text));
    }
    for (const int id : fresh) start_files(id);
    if (test_logging() && (queue_bytes > 0 || delta_bytes > 0)) {
        test_log(std::format("flush lock={:.2f}ms files={} {}={}B fileDelta={} rows {}B", held, passed,
                             full ? "queue" : "queueDelta", queue_bytes, delta_rows, delta_bytes));
    }
}

void App::watch_files(int id) {
    watches_[id] = Watch{}; // again after a fold or a reload: the whole list
    start_files(id);
}

// The first answer: every file as it is now, copied in a compact form under the lock, then sent
// in chunks. A folder not listed yet sends when it is (flush).
void App::start_files(int id) {
    const auto w = watches_.find(id);
    if (w == watches_.end()) return;
    auto send = std::make_shared<FilesSend>();
    double held = 0;
    {
        std::lock_guard lock(mutex_);
        const auto locked = std::chrono::steady_clock::now();
        const auto it = std::ranges::find(items_, id, [](const auto& i) { return i->id; });
        if (it == items_.end()) return;
        const Item& item = **it;
        if (!item.tree.listed || !item.names || item.gen == 0 || item.names->path.size() != item.tree.files.size()) return;
        send->names = item.names;
        send->gen = item.gen;
        send->snap = queue_view::snapshot(item.tree.files, item.tree.small_connections > 0);
        held = ms_since(locked);
    }
    if (test_logging()) test_log(std::format("first answer lock={:.2f}ms files={}", held, send->snap.rows.size()));
    w->second.gen = send->gen;
    w->second.shadow.clear();
    w->second.send = send;
    send_files_chunk(id, send);
}

// One chunk, the next on the next turn of the message loop, so input keeps flowing; once all went,
// the deltas go on from what was sent.
void App::send_files_chunk(int id, const std::shared_ptr<FilesSend>& send) {
    const auto w = watches_.find(id);
    if (w == watches_.end() || w->second.send != send) return; // folded, reloaded or sent again since
    post_ui(queue_view::files_message(id, send->gen, send->next, files_chunk, *send->names, send->snap));
    send->next += files_chunk;
    if (send->next < send->snap.rows.size()) {
        host_.defer([this, id, send] { send_files_chunk(id, send); });
        return;
    }
    w->second.shadow = std::move(send->snap.rows);
    w->second.send.reset();
    // The snapshot may be older than the files now (an action, a run's end, a new gen while the
    // chunks went): the next flush compares them.
    w->second.catch_up = true;
    dirty_ = true;
}

// One batch of skips at a time. Where the folder's files land is worked out once for the batch,
// so no partial download deleted takes one. A stop does what was asked for, then ends.
void App::run_discards(const std::stop_token& stop) {
    for (;;) {
        DiscardJob job;
        {
            std::unique_lock lock(discard_mutex_);
            discard_wake_.wait(lock, stop, [&] { return !discard_jobs_.empty(); });
            if (discard_jobs_.empty()) return;
            job = std::move(discard_jobs_.front());
            discard_jobs_.pop_front();
        }
        const auto reserved = queue_view::reserved_names(job.dir, *job.names);
        for (const auto& d : job.discard) fetch::discard(d.target, d.remote_path, reserved, job.rclone_remote);
    }
}

void App::request_save(bool now) {
    {
        std::lock_guard lock(save_mutex_);
        save_wanted_ = true;
        save_now_ = save_now_ || now;
    }
    save_wake_.notify_all();
}

// The saver: a save asked for waits until queue_save_gap after the last one ended, or ten times
// what that one took when that is longer (a 177,000-file folder's queue.json is about 20 MB). An
// urgent one (a download ended) skips the queue_save_gap floor but keeps the ten times, so many
// small downloads ending don't each write a big queue.json. A stop saves what was asked for at
// once, then ends.
void App::run_saves(const std::stop_token& stop) {
    for (;;) {
        {
            std::unique_lock lock(save_mutex_);
            save_wake_.wait(lock, stop, [&] { return save_wanted_; });
            if (!save_wanted_) return;
            // Woken again when an urgent request comes during the wait: its time is earlier.
            while (!stop.stop_requested()) {
                const auto at = save_now_ ? next_urgent_save_ : next_save_;
                const bool urgent = save_now_;
                if (save_wake_.wait_until(lock, stop, at, [&] { return save_now_ != urgent; })) continue;
                break;
            }
            save_wanted_ = false;
            save_now_ = false;
        }
        const auto started = std::chrono::steady_clock::now();
        save_queue();
        const auto ended = std::chrono::steady_clock::now();
        const auto took = ended - started;
        std::lock_guard lock(save_mutex_);
        next_save_ = ended + std::max<std::chrono::steady_clock::duration>(queue_save_gap, 10 * took);
        next_urgent_save_ = ended + 10 * took;
    }
}

bool App::test_logging() {
#ifndef NDEBUG
    static const bool on = !util::getenv_utf8("GRAB_TEST_FLUSH_LOG").value_or("").empty();
    return on;
#else
    return false;
#endif
}

void App::test_log([[maybe_unused]] const std::string& line) {
#ifndef NDEBUG
    static std::mutex m;
    static const auto path = util::path_from_utf8(util::getenv_utf8("GRAB_TEST_FLUSH_LOG").value_or(""));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    std::lock_guard lock(m);
    std::ofstream out(path, std::ios::app | std::ios::binary);
    out << ms << ' ' << line << '\n';
#endif
}

// Debug builds: a folder of `files` made-up files downloaded without a server or disk writes
// (run_fake), for the window's performance checks (DOWNLOADS-UI.md 14, C). One in 25 is a big
// file of 32 MiB, the others from 4 KiB to 1 MiB, a thousand to a folder. Saved in queue.json
// like any folder, so its size and saves can be measured; after a restart it is an ordinary
// paused folder of a server "test" that doesn't exist.
void App::add_fake_folder(std::size_t files) {
    auto item = std::make_shared<Item>();
    item->id = next_id_++;
    item->remote = "test";
    item->mode = Mode::folder;
    item->path = "/fake/synthetic";
    item->name = "synthetic";
    std::error_code ec;
    item->dest = std::filesystem::temp_directory_path(ec);
    item->fake = true;
    item->space_ok = true;
    item->tree.files.reserve(files);
    for (std::size_t k = 0; k < files; ++k) {
        const std::uint64_t size = k % 25 == 7 ? std::uint64_t{32} << 20 : 4096 + (k * 7919) % (std::uint64_t{1} << 20);
        item->tree.files.push_back(folder::make_file(std::format("dir{:03}/file{:06}.bin", k / 1000, k), size, "2026-01-01T00:00:00Z"));
    }
    item->tree.listed = true;
    refresh_folder(*item);
    item->names = std::make_shared<const queue_view::Names>(queue_view::names_of(item->tree.files));
    item->gen = next_gen_++;
    items_.push_back(std::move(item));
}

// GRAB_TEST_FAKE_FOLDER's download: every 100 ms 30 small files land (300 a second) and up to 8
// big files move on at 4 MiB/s each, until all are done or it is stopped, in the start order of a
// real one (folder::next_queued: priority, then folder order). Pause, Resume, Skip and a file's
// priority act as on a real one; the bytes count as received (its speed and the total).
void App::run_fake(Item& item, const std::stop_token& stop) {
    constexpr auto tick = std::chrono::milliseconds(100);
    constexpr double big_speed = 4.0 * 1024 * 1024;
    constexpr std::size_t landing = 30;
    constexpr std::size_t big_at_once = 8;
    std::vector<std::size_t> running; // big files
    std::mutex wait_m;
    std::condition_variable_any wait_cv;
    {
        std::lock_guard lock(mutex_);
        item.phase = Phase::none;
    }
    for (bool left = true; left && !stop.stop_requested();) {
        {
            std::lock_guard lock(mutex_);
            auto& files = item.tree.files;
            std::erase_if(running, [&](std::size_t k) {
                auto& f = files[k];
                if (f.status != folder::State::running) return true;
                if (f.stop_reason != folder::StopReason::none) { // paused or skipped
                    f.status = f.stop_reason == folder::StopReason::pause ? folder::State::paused : folder::State::skipped;
                    f.stop_reason = folder::StopReason::none;
                    f.speed = 0;
                    f.eta.reset();
                    return true;
                }
                const auto moved = std::min(f.size - std::min(f.bytes, f.size), static_cast<std::uint64_t>(big_speed / 10));
                f.bytes += moved;
                item.received += moved;
                received_ += moved;
                f.speed = big_speed;
                f.eta = static_cast<double>(f.size - f.bytes) / big_speed;
                if (f.bytes < f.size) return false;
                f.status = folder::State::done;
                f.fetched = true;
                f.speed = 0;
                f.eta.reset();
                return true;
            });
            while (running.size() < big_at_once) {
                const auto k = folder::next_queued(item.tree, true);
                if (!k) break;
                auto& f = files[*k];
                f.status = folder::State::running;
                f.stop = std::stop_source{};
                f.stop_reason = folder::StopReason::none;
                running.push_back(*k);
            }
            for (std::size_t landed = 0; landed < landing; ++landed) {
                const auto k = folder::next_queued(item.tree, false);
                if (!k) break;
                auto& f = files[*k];
                f.status = folder::State::done;
                item.received += f.size - std::min(f.bytes, f.size);
                received_ += f.size - std::min(f.bytes, f.size);
                f.bytes = f.size;
                f.fetched = true;
            }
            item.tree.files_at_once = static_cast<int>(running.size());
            left = !running.empty() || folder::next_queued(item.tree, false).has_value() ||
                   folder::next_queued(item.tree, true).has_value();
        }
        item.touched = true;
        dirty_ = true;
        std::unique_lock lock(wait_m);
        wait_cv.wait_for(lock, stop, tick, [] { return false; });
    }
    std::lock_guard lock(mutex_);
    for (auto& f : item.tree.files) {
        if (f.status != folder::State::running) continue;
        f.status = folder::State::queued;
        f.speed = 0;
        f.eta.reset();
    }
    ++item.tree.requeued;
    item.tree.files_at_once = 0;
    if (stop.stop_requested()) item.status = item.stop_reason == StopReason::cancel ? Status::cancelled : Status::paused;
    else item.status = folder::outcome(item.tree).waiting > 0 ? Status::paused : Status::done;
    refresh_folder(item);
    item.speed = 0;
    item.eta.reset();
    item.touched = true;
    dirty_ = true;
}

} // namespace grab::gui
