#include "app.hpp"

#include "activity.hpp"
#include "browse.hpp"
#include "config.hpp"
#include "engine.hpp"
#include "file_list.hpp"
#include "folder.hpp"
#include "gui_state.hpp"
#include "ini.hpp"
#include "listing.hpp"
#include "local_name.hpp"
#include "preview.hpp"
#include "process.hpp"
#include "quote.hpp"
#include "rclone.hpp"
#include "rclone_flags.hpp"
#include "space.hpp"
#include "util.hpp"
#include "verify.hpp"

#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>   // IWYU pragma: keep (SHGetKnownFolderPath via shlobj_core.h)
#include <shobjidl.h> // IWYU pragma: keep (IFileOpenDialog via shobjidl_core.h)
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <future>
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

constexpr auto progress_interval = std::chrono::milliseconds(250); // at most 4 updates a second

Value str(std::string s) { return Value::make_string(std::move(s)); }
Value num(double n) { return Value::make_number(n); }

const char* status_name(int s) {
    constexpr const char* names[] = {"queued", "running", "paused", "done", "failed", "cancelled", "skipped"}; // Status order
    return names[s];
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

const char* auth_name(servers::Auth a) {
    switch (a) {
    case servers::Auth::key_file: return "key";
    case servers::Auth::agent: return "agent";
    case servers::Auth::password: break;
    }
    return "password";
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

const char* file_state_name(folder::State s) {
    constexpr const char* names[] = {"queued", "running", "paused", "done", "failed", "skipped"}; // folder::State order
    return names[static_cast<int>(s)];
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
    // Output of an earlier update: an installer update outlives the window that followed it.
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(std::filesystem::temp_directory_path(ec), ec)) {
        const auto name = e.path().filename().wstring();
        if ((name.starts_with(L"grab-update-") && name.ends_with(L".log")) || name == L"grab-gui-restart.log") {
            std::filesystem::remove(e.path(), ec);
        }
    }
    restore_queue();
    lists_dir_ = file_list::dir_for(config_path_);
    prune_lists();
    // Up to max_parallel downloads at once; parallel_ decides how many actually run.
    for (int i = 0; i < max_parallel; ++i) {
        workers_.emplace_back([this](std::stop_token stop) { worker_loop(std::move(stop)); });
    }
}

App::~App() {
    if (search_thread_.joinable()) search_thread_.request_stop();
    if (settings_thread_.joinable()) settings_thread_.request_stop();
    cancel_pending();
    // Running downloads stop where they are: file downloads keep their partial file and come
    // back paused on the next start (save_queue below records them as unfinished).
    {
        std::lock_guard lock(mutex_);
        for (auto& item : items_) {
            if (item->status == Status::running) item->stop.request_stop();
        }
    }
    save_queue();
    for (auto& w : workers_) w.request_stop();
    wake_.notify_all();
}

void App::save_state() const { report_save("the window's settings", save_gui_state(state_path_, state_), state_unsaved_); }

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

constexpr std::size_t preview_cache_bytes = std::size_t{256} << 20;

WebResource preview_failed(int status, std::string message) {
    return WebResource{status, "text/plain; charset=utf-8", {}, std::move(message)};
}

} // namespace

void App::queue_preview(std::function<void()> job) {
    {
        std::lock_guard lock(preview_mutex_);
        preview_jobs_.push_back(std::move(job));
        while (preview_workers_.size() < 2) {
            preview_workers_.emplace_back([this](const std::stop_token& stop) { run_preview_jobs(stop); });
        }
    }
    preview_wake_.notify_one();
}

void App::run_preview_jobs(const std::stop_token& stop) {
    while (true) {
        std::function<void()> job;
        {
            std::unique_lock lock(preview_mutex_);
            if (!preview_wake_.wait(lock, stop, [this] { return !preview_jobs_.empty(); })) return;
            job = std::move(preview_jobs_.front());
            preview_jobs_.pop_front();
        }
        job();
    }
}

void App::preview_request(const std::string& url, std::function<void(WebResource)> reply) {
    constexpr std::string_view prefix = "https://grab.ui/preview/";
    std::string_view rest = url;
    if (!rest.starts_with(prefix)) return reply(preview_failed(400, "not a preview"));
    rest.remove_prefix(prefix.size());
    rest = rest.substr(0, rest.find_first_of("?#"));
    const auto slash = rest.find('/');
    const auto remote = preview::url_decode(rest.substr(0, slash));
    const auto path = slash == std::string_view::npos ? std::nullopt : preview::url_decode(rest.substr(slash + 1));
    if (!remote || !path || remote->empty() || path->empty()) return reply(preview_failed(400, "not a preview"));

    std::string key = *remote + '\n' + *path;
    {
        std::lock_guard lock(preview_mutex_);
        const auto it = std::ranges::find(preview_cache_, key, &CachedPreview::key);
        if (it != preview_cache_.end()) {
            preview_cache_.splice(preview_cache_.begin(), preview_cache_, it);
            return reply(it->answer);
        }
    }
    const bool ahead = url.find("?ahead") != std::string::npos; // the viewer fetching the next one
    queue_preview([this, remote = *remote, path = *path, key = std::move(key), reply = std::move(reply), ahead] {
        auto answer = preview_answer(remote, path, ahead);
        {
            // Kept for ←/→ and the neighbours fetched ahead; failures too, briefly (they come
            // back after the others push them out).
            std::lock_guard lock(preview_mutex_);
            preview_cache_.push_front(CachedPreview{key, answer});
            std::size_t total = 0;
            for (auto it = preview_cache_.begin(); it != preview_cache_.end();) {
                total += it->answer.body.size();
                it = total > preview_cache_bytes && it != preview_cache_.begin() ? preview_cache_.erase(it) : std::next(it);
            }
        }
        reply(std::move(answer));
    });
}

WebResource App::preview_answer(const std::string& remote, const std::string& path, bool ahead) {
    const auto kind = preview::kind_of(path);
    const auto max = preview::max_bytes(kind);
    if (max == 0) return preview_failed(415, "This kind of file has no preview.");
    auto ctx = engine::load_context(config_path_, remote);
    if (!ctx) return preview_failed(500, ctx.error());
    const auto name = remote_basename(path);
    // Until the first bytes, rclone opens the file (the first one in a folder reads the folder).
    std::optional<Activity> loading;
    const auto url = stream_url(*ctx, remote, path, true, ahead, [&] {
        if (!loading) loading.emplace(*this, std::format("Opening {} on {}…", name, remote), std::string(), ahead);
    });
    if (!url) {
        if (loading) loading->failed();
        return preview_failed(502, url.error());
    }
    auto got = http_get(*url, max, [&](std::uint64_t done, std::uint64_t total) {
        loading->progress(done, total,
                          std::format("Loading {}: {} of {}", name, util::format_size(done), util::format_size(total)));
    });
    if (!got) return preview_failed(502, got.error());
    if (got->status == 404) return preview_failed(404, std::format("Not found on {}.", remote));
    if (got->status != 200 && got->status != 206) {
        return preview_failed(502, std::format("{} answered {}.", remote, got->status));
    }
    const bool cut = got->total && *got->total > got->body.size();
    if (cut && kind != preview::Kind::text) {
        return preview_failed(413, std::format("Too big to preview ({}); download it instead.", util::format_size(*got->total)));
    }
    WebResource answer{200, preview::content_type(path), {}, {}};
    if (kind == preview::Kind::text) {
        answer.body = preview::text_utf8(got->body, util::to_lower(path).ends_with(".nfo"));
        if (cut) answer.headers = std::format("X-Grab-Total: {}", *got->total);
    } else {
        answer.body = std::move(got->body);
    }
    // An SVG is shown as an image, where its scripts never run; if it were opened as a page
    // anyway, it gets no rights at all.
    if (util::to_lower(path).ends_with(".svg")) answer.headers = "Content-Security-Policy: sandbox";
    return answer;
}

std::expected<std::string, std::string> App::stream_url(const engine::Context& ctx, const std::string& remote,
                                                         const std::string& path, bool announce, bool bg,
                                                         const std::function<void()>& on_ready) {
    auto at = preview::where(path, engine::search_roots(ctx));
    const auto folder = preview::in_folder(path);
    while (true) {
        std::optional<Activity> starting;
        if (announce && !previews_.running(remote, at.root)) {
            starting.emplace(*this, std::format("Starting a stream from {} (rclone serve http)…", remote), "serve:" + remote, bg);
        }
        auto base = previews_.base(ctx, remote, at.root);
        if (!base) {
            if (starting) starting->failed();
            return util::fail(base.error());
        }
        starting.reset();
        if (on_ready) on_ready();
        auto url = *base + preview::url_path(at.path);
        const auto status = http_status(url);
        if (!status) return util::fail(status.error());
        if (*status == 200 || *status == 206) return url;
        if (*status == 404) return util::failf("Not found on {}.", remote);
        if (*status == 500 && at != folder) {
            at = folder;
            continue;
        }
        return util::fail(previews_.failure(remote, at.root, at.path).value_or(std::format("{} answered {}.", remote, *status)));
    }
}

void App::play(const Value& msg) {
    const std::string remote = msg.string_of("remote").value_or("");
    const std::string path = msg.string_of("path").value_or("");
    if (remote.empty() || path.empty()) return;
    std::optional<std::uint64_t> size; // the row's: no need to ask the server
    if (const Value* n = msg.find("size"); n != nullptr && n->kind == Value::Kind::number && n->number >= 0) {
        size = static_cast<std::uint64_t>(n->number);
    }
    queue_preview([this, remote, path, size] {
        Value v = Value::make_object();
        auto fail = [&](const std::string& message) {
            v.set("type", str("error"));
            v.set("message", str(message));
            post(v);
        };
        auto ctx = engine::load_context(config_path_, remote);
        if (!ctx) return fail(ctx.error());
        const auto name = remote_basename(path);
        const auto dot = name.rfind('.');
        // The player gets grab's own stream: what it reads while opening, fetched over several
        // connections first; the rest from rclone serve, which starts meanwhile. If grab's
        // stream can't start, rclone serve's.
        std::shared_future<std::expected<std::string, std::string>> upstream =
            std::async(std::launch::async, [this, ctx = *ctx, remote, path] {
                return stream_url(ctx, remote, path, false, true);
            }).share();
        connections_.configure(remote, connection_budget(remote, ctx->remote.host, ctx->settings.max_connections));
        PlayServer::File file{fetch::Source{ctx->config.rclone, ctx->config.rclone_config, ctx->settings.rclone_remote, path,
                                            rclone_flags::connection(ctx->settings.common_flags)},
                              remote, upstream, name, size};
        auto preparing = std::make_shared<Activity>(*this, std::format("Preparing {}…", name), "warm:" + remote);
        auto address = play_server_.play(std::move(file), [preparing, name](std::uint64_t done, std::uint64_t total) {
            preparing->progress(done, total, std::format("Preparing {}: {} of {}", name, util::format_size(done),
                                                         util::format_size(total)));
        });
        if (!address) { // rclone serve's own stream, as before
            const auto& direct = upstream.get();
            if (!direct) {
                preparing->failed();
                return fail(std::format("Can't play {}: {}", name, direct.error()));
            }
            address = *direct;
        }
        preparing.reset(); // it ends once the start is in, with the warm start's own copy
        auto program = open_with_player(dot == std::string::npos ? "" : util::to_lower(name.substr(dot + 1)), *address);
        if (!program) return fail(program.error());
        v.set("type", str("played"));
        v.set("name", str(name));
        v.set("program", str(*program));
        post(v);
    });
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
        send_init();
        post_update(); // the page may have been reloaded
        maybe_check_updates();
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
    } else if (type == "openReleasePage") {
        open_release_page();
    } else if (type == "prefs") {
        if (auto remote = msg->string_of("remote")) state_.last_remote = *remote;
        // The results' sort: null for the default order; an invalid one changes nothing.
        if (const Value* sort = msg->find("sort")) {
            if (sort->kind == Value::Kind::null) state_.sort.reset();
            else if (auto order = sort_from_json(*sort)) state_.sort = std::move(order);
        }
        save_state();
    } else if (type == "search") {
        start_search(*msg);
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
    } else if (type == "pauseChild" || type == "resumeChild" || type == "skipChild") {
        const int id = int_of(*msg, "id", -1);
        const std::string path = msg->string_of("path").value_or("");
        if (type == "pauseChild") pause_child(id, path);
        else if (type == "resumeChild") resume_child(id, path);
        else skip_child(id, path);
    } else if (type == "moveItem") {
        move_item(int_of(*msg, "id", -1), int_of(*msg, "before", 0));
    } else if (type == "setPriority") {
        set_priority(int_of(*msg, "id", -1), weight_of(msg->string_of("priority").value_or("normal")));
    } else if (type == "setLimit") {
        const Value* mibps = msg->find("mibps");
        set_limit(bool_of(*msg, "on"),
                  mibps != nullptr && mibps->kind == Value::Kind::number ? mibps->number : state_.limit_mibps);
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
            auto keys = server_ops::scan_host_keys(env->cfg.ssh, it->host, it->port, stop);
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
            auto ctx = engine::load_context(config_path_, r.name);
            if (ctx) {
                entry.set("host", str(ctx->remote.host));
                entry.set("user", str(ctx->remote.user));
                entry.set("method", str(ctx->method == FindMethod::ssh ? "ssh" : "rclone"));
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
    Value dests = Value::make_object();
    for (const auto& [remote, path] : state_.destinations) dests.set(remote, str(path));
    init.set("destinations", std::move(dests));
    init.set("defaultDest", str(known_folder(FOLDERID_Downloads)));
    init.set("parallel", num(state_.parallel));
    Value limit = limit_message(schedule::local_now());
    limit_posted_ = json::stringify(limit);
    init.set("limit", std::move(limit));
    post(init);
    post_queue();
}

void App::start_search(const Value& msg) {
    engine::SearchRequest req;
    req.target = msg.string_of("query").value_or("");
    req.mode = mode_of(msg);
    req.exact = bool_of(msg, "exact");
    req.batch_ssh = true; // no console to answer a prompt: fail fast instead
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
    // A new search replaces the previous one; stopping kills its ssh/rclone child at once.
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
                const bool over_ssh = ctx->method == FindMethod::ssh;
                const std::string doing = std::format("Searching {} with {}", server, over_ssh ? "ssh + find" : "rclone lsf");
                Activity act(*this, doing + "…", "search:" + server);
                activity::Records records; // ssh + find prints the matches; rclone lsf everything
                auto counted = run;
                counted.on_output = [&](std::string_view chunk) {
                    records.feed(chunk);
                    act.text(over_ssh ? std::format("{}… {} found so far", doing, thousands(records.count()))
                                      : std::format("{}… {} looked at", doing, count_of(records.count(), "entry", "entries")));
                };
                // Over rclone the walk's connections come from the pool (review G191), and one walk
                // lists both kinds (G197).
                auto r = req;
                std::optional<fetch::Listing> walk;
                if (!over_ssh) {
                    walk.emplace(connections_, server, stop);
                    r.walk_flags = walk->flags();
                }
                const auto hook = [&](const std::vector<std::string>& argv) { act.detail(quote::display_cmdline(argv)); };
                std::vector<std::pair<Mode, std::expected<engine::SearchResult, std::string>>> results;
                if (kinds.size() == 2) {
                    if (auto walked = engine::search_both(*ctx, r, counted, hook)) {
                        results.emplace_back(Mode::folder, std::move(walked->first));
                        results.emplace_back(Mode::file, std::move(walked->second));
                    } else {
                        results.emplace_back(Mode::folder, std::unexpected(walked.error()));
                    }
                } else {
                    r.mode = kinds.front();
                    results.emplace_back(kinds.front(), engine::search(*ctx, r, counted, hook));
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
        const std::string doing = std::format("{} {}'s file list", before > 0 ? "Refreshing" : "Building", server);
        Activity act(*this, doing + "…", "list:" + server, true);
        if (before > 0) act.progress(0, before); // against the last list's count, from the start
        activity::Records records;
        run.on_output = [&](std::string_view chunk) {
            records.feed(chunk);
            const auto n = records.count();
            if (before > 0) {
                act.progress(std::min<std::uint64_t>(n, before), before,
                             std::format("{}: {} of ~{} entries", doing, thousands(n), thousands(before)));
            } else {
                act.text(std::format("{}: {}", doing, count_of(n, "entry", "entries")));
            }
        };
        auto ctx = engine::load_context(config_path_, server);
        // Over rclone the walk's connections come from the pool (review G191).
        std::optional<fetch::Listing> walk;
        std::vector<std::string> walk_flags;
        if (ctx && ctx->method != FindMethod::ssh) {
            walk.emplace(connections_, server, stop);
            walk_flags = walk->flags();
        }
        auto fetched = ctx ? file_list::fetch(*ctx, unix_now(), true, run, [&](const std::vector<std::string>& argv) {
            act.detail(quote::display_cmdline(argv));
        }, walk_flags)
                           : std::expected<file_list::List, std::string>(std::unexpected(ctx.error()));
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
    const bool over_ssh = ctx.method == FindMethod::ssh;
    const std::string doing = under.empty() ? std::format("Searching all of {} with {}", server,
                                                          over_ssh ? "ssh + find" : "rclone lsf")
                                            : std::format("Searching below {} on {}", under, server);
    Activity act(*this, doing + "…", "search:" + server);
    activity::Records records;
    auto counted = run;
    counted.on_output = [&](std::string_view chunk) {
        records.feed(chunk);
        act.text(over_ssh ? std::format("{}… {} found so far", doing, thousands(records.count()))
                          : std::format("{}… {} looked at", doing, count_of(records.count(), "entry", "entries")));
    };
    // Over rclone the walk's connections come from the pool (review G191), and one walk lists both
    // kinds (G197).
    auto r = req;
    std::optional<fetch::Listing> walk;
    if (!over_ssh) {
        walk.emplace(connections_, server, run.stop);
        r.walk_flags = walk->flags();
    }
    auto found = engine::search_both(below, r, counted, [&](const std::vector<std::string>& argv) {
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
        auto rows_of = [&](const std::string& at, const std::string& what = {}) -> std::expected<Rows, std::string> {
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
            const std::string doing = what.empty() ? std::format("Listing {} on {}", at.empty() ? "~" : at, remote) : what;
            Activity act(*this, doing + "…", "lsjson:" + remote);
            activity::Records records('{'); // lsjson: "[", an item a line, "]"
            run.on_output = [&](std::string_view chunk) {
                records.feed(chunk);
                if (records.count() > 0) act.text(std::format("{}… {} so far", doing, count_of(records.count(), "item", "items")));
            };
            auto got = browse::list_live(*ctx, at, run, [&](const std::vector<std::string>& argv) {
                act.detail(quote::display_cmdline(argv));
            });
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
            if (rows && typed && select.empty() && rows->new_listing && browse::maybe_file(rows->items, folder)) {
                // lsjson lists a file as that one file: its folder opens instead, when the file is
                // there (rather than a folder of the same name holding just that file).
                const std::string parent = parent_of(folder);
                if (auto up = rows_of(parent, std::format("Checking whether {} is a file", remote_basename(folder)))) {
                    const auto name = remote_basename(folder);
                    const auto it = std::ranges::find(up->items, name, &browse::Item::name);
                    if (it != up->items.end() && !it->dir) {
                        select = folder;
                        folder = parent;
                        rows = std::move(up);
                    }
                }
                if (stop.stop_requested()) return;
            }
            if (!rows) {
                if (typed && rows.error().find("directory not found") != std::string::npos) {
                    return fail(std::format("Not found on {}: {}", remote, folder.empty() ? "~" : folder));
                }
                if (typed && rows.error().find("permission denied") != std::string::npos) {
                    return fail(std::format("{} doesn't let you list {}", remote, folder.empty() ? "~" : folder));
                }
                return fail(rows.error());
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
    if (auto ctx = engine::load_context(config_path_, remote)) {
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
    return server_ops::load_env(config_path_, util::self_exe_path().parent_path());
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
        v.set("auth", str(auth_name(s.auth)));
        v.set("keyFile", str(s.key_file));
        v.set("search", str(s.ssh_search ? "ssh" : "rclone"));
        v.set("pinned", Value::make_bool(s.host_key_pinned));
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
    s.auth = auth == "key" ? servers::Auth::key_file : auth == "agent" ? servers::Auth::agent : servers::Auth::password;
    if (s.auth == servers::Auth::key_file) s.key_file = std::string(util::trim(msg.string_of("keyFile").value_or("")));
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
    if (s.host.empty()) return invalid("host", "required");
    if (s.port < 1 || s.port > 65535) return invalid("port", "a number from 1 to 65535");
    if (s.user.empty()) return invalid("user", "required");
    std::error_code ec;
    if (s.auth == servers::Auth::key_file && !std::filesystem::is_regular_file(util::path_from_utf8(s.key_file), ec)) {
        return invalid("keyFile", "no such file");
    }
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
                return invalid("secret", it->auth == servers::Auth::password ? "enter the password again for the new host or user"
                                                                              : "enter the password");
            }
            need_keys = it->error.empty() ? server_ops::edit_needs_host_key(*it, s) : true;
        } else {
            p.kind = Pending::Kind::add;
            auto rclone_doc = server_ops::read_rclone_conf(*env);
            if (!rclone_doc) return settings_error(rclone_doc.error());
            if (auto why = server_ops::name_taken(*env, *rclone_doc, s.name)) return invalid("name", *why);
            if (s.auth == servers::Auth::password && p.secret.empty()) return invalid("secret", "enter the password");
        }
        if (!need_keys) return apply_pending(std::move(p), stop);

        auto keys = server_ops::scan_host_keys(env->cfg.ssh, s.host, s.port, stop);
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
    std::optional<std::string> obscured;
    if (!p.secret.empty()) {
        auto ob = server_ops::obscure(*env, p.secret.value());
        p.secret.wipe();
        if (!ob) return settings_error(ob.error());
        obscured = std::move(*ob);
    }
    std::expected<std::string, std::string> done; // a note to show, when it worked
    switch (p.kind) {
    case Pending::Kind::add:
        done = server_ops::add_server(*env, p.server, obscured, p.keys);
        break;
    case Pending::Kind::edit:
        done = server_ops::update_server(*env, p.server, obscured,
                                         p.keys.lines.empty() ? std::nullopt : std::optional(p.keys));
        break;
    case Pending::Kind::trust:
        done = server_ops::trust_server(*env, p.server.name, p.keys).transform([] { return std::string{}; });
        break;
    }
    if (!done) return settings_error(done.error());
    if (!done->empty()) settings_error(*done);
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

void App::test_server(const std::string& name, std::stop_token stop) const {
    auto env = load_env();
    if (!env) return settings_error(env.error());
    Value v = Value::make_object();
    v.set("type", str("serverTested"));
    v.set("name", str(name));
    auto result = server_ops::test_connection(*env, name, stop);
    if (stop.stop_requested()) return;
    if (!result) {
        v.set("ok", Value::make_bool(false));
        v.set("rcloneError", str(result.error()));
    } else {
        v.set("ok", Value::make_bool(result->ok()));
        v.set("rcloneOk", Value::make_bool(result->rclone_ok));
        v.set("rcloneError", str(result->rclone_error));
        v.set("sshOk", result->ssh_ok ? Value::make_bool(*result->ssh_ok) : Value{});
        v.set("sshError", str(result->ssh_error));
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
    save_queue();
    post_queue();
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
                auto ctx = engine::load_context(config_path_, remote);
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
    save_queue();
    post_queue();
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
                auto ctx = engine::load_context(config_path_, remote);
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
            item->status = Status::queued;
        }
    }
    wake_.notify_all();
    save_queue();
    post_queue();
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
                folder::resume_paused(item->tree); // a folder's Resume covers its files paused one by one too
            }
        }
    }
    wake_.notify_all();
    save_queue();
    post_queue();
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
    save_queue();
    post_queue();
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
    save_queue();
    post_queue();
}

void App::requeue(Item& item) const {
    // A failed file download continues from its partial file; a cancelled one's was deleted, so
    // it starts over.
    const bool cancelled = item.status == Status::cancelled;
    item.status = Status::queued;
    item.error.clear();
    if (item.mode == Mode::folder) {
        folder::retry_files(item.tree, cancelled); // after a cancel, everything not finished
    } else if (!fetch::saved_progress(item_target(item), item.path, {}, rclone_remote_of(item.remote))) {
        item.bytes = 0;
    }
    item.speed = 0;
    item.eta.reset();
}

void App::pause_child(int id, const std::string& path) {
    {
        std::lock_guard lock(mutex_);
        for (auto& item : items_) {
            if (item->id != id) continue;
            folder::pause_file(item->tree, path);
            folder_totals(*item);
        }
    }
    save_queue();
    post_queue();
}

void App::resume_child(int id, const std::string& path) {
    {
        std::lock_guard lock(mutex_);
        for (auto& item : items_) {
            if (item->id != id) continue;
            folder::resume_file(item->tree, path);
            // A folder that stopped waiting for this file runs again.
            if (item->status != Status::running && item->status != Status::queued) {
                item->status = Status::queued;
                item->error.clear();
            }
        }
    }
    wake_.notify_all();
    save_queue();
    post_queue();
}

void App::skip_child(int id, const std::string& path) {
    std::optional<folder::Discard> discard;
    std::string server;
    {
        std::lock_guard lock(mutex_);
        for (auto& item : items_) {
            if (item->id != id) continue;
            discard = folder::skip_file(item->tree, folder::local_dir(item->dest, item->path), item->path, path);
            server = item->remote;
            folder_totals(*item);
        }
    }
    if (discard) fetch::discard(discard->target, discard->remote_path, discard->reserved, rclone_remote_of(server));
    save_queue();
    post_queue();
}

void App::set_limit(bool on, double mibps) {
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
    update_limit();
}

void App::set_schedule(const Value& rules) {
    state_.speed_schedule = schedule::from_json(rules);
    state_.limit_change.reset(); // the new rules apply at once
    save_state();
    update_limit();
}

void App::tick() { update_limit(); }

schedule::Applied App::current_limit(const schedule::Now& now) const {
    return schedule::apply({state_.limit_on, state_.limit_mibps}, schedule::limit_at(state_.speed_schedule, now.minute),
                           state_.limit_change, now.unix);
}

void App::update_limit() {
    const auto now = schedule::local_now();
    if (state_.limit_change &&
        !schedule::holds(*state_.limit_change, schedule::limit_at(state_.speed_schedule, now.minute), now.unix)) {
        state_.limit_change.reset();
        save_state();
    }
    const auto applied = current_limit(now);
    set_rate(applied.limit.on ? schedule::bytes_per_s(applied.limit.mibps) : 0);
    Value message = limit_message(now);
    if (auto text = json::stringify(message); text != limit_posted_) {
        limit_posted_ = std::move(text);
        post(message);
    }
}

void App::set_rate(std::uint64_t bytes_per_s) {
    if (bytes_per_s == limiter_.rate()) return;
    // Streams through grab follow the new rate at once. rclone batches (a folder's small
    // files) have theirs fixed at start, so running ones restart with the new cap; only their
    // in-flight small files start over.
    limiter_.set_rate(bytes_per_s);
    std::lock_guard lock(mutex_);
    for (auto& item : items_) {
        if (item->tree.batch_running) item->tree.batch_stop.request_stop();
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
    save_queue();
    post_queue();
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
            // A folder's rclone batch has its --bwlimit share fixed at start: restart it.
            if (item->tree.batch_running && limiter_.rate() > 0) item->tree.batch_stop.request_stop();
        }
    }
    if (running) connections_.set_weight(server, id, weight); // shares rebalance at the next reads
    save_queue();
    post_queue();
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
}

void App::clear_finished() {
    {
        std::lock_guard lock(mutex_);
        std::erase_if(items_, [](const auto& i) {
            return i->status == Status::done || i->status == Status::failed ||
                   i->status == Status::cancelled;
        });
    }
    post_queue();
}

void App::open_item_folder(int id) {
    std::filesystem::path target;
    {
        std::lock_guard lock(mutex_);
        for (const auto& item : items_) {
            if (item->id == id) target = item_target(*item);
        }
    }
    if (target.empty()) return;
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
        }
        save_queue();
        post_queue();
        run_item(item, stop);
        wake_.notify_all(); // a slot is free for the next queued download
        save_queue();
        post_queue();

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

// Progress for the page, at most every progress_interval across all downloads.
namespace {
template <class Update>
bool update_item(std::mutex& m, std::chrono::steady_clock::time_point& last_post, Update&& update) {
    std::lock_guard lock(m);
    update();
    const auto now = std::chrono::steady_clock::now();
    if (now - last_post < progress_interval) return false;
    last_post = now;
    return true;
}
} // namespace

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
    d.source = fetch::Source{ctx->config.rclone, ctx->config.rclone_config, ctx->settings.rclone_remote, item->path,
                             rclone_flags::connection(ctx->settings.common_flags)};
    d.server = item->remote;
    d.id = item->id;
    existing::Choice choice{};
    {
        std::lock_guard lock(mutex_);
        d.target = item_target(*item);
        choice = item->existing;
    }
    d.replace = choice == existing::Choice::replace;
    d.limiter = &limiter_;
    d.weight = &item->weight;
    // engine = grab: through grab's own SSH client, its connections kept open between files.
    if (effective_engine(ctx->settings) == Engine::grab) {
        auto target = engine::ssh_target(*ctx);
        if (!target) return set(Status::failed, target.error());
        d.pool = pools_.get(item->remote, *target, connections_.budget(item->remote));
    }
    auto fetch_once = [&] {
        return fetch::run(d, connections_, [&](const fetch::Progress& p) {
            if (update_item(mutex_, last_progress_post_, [&] {
                    item->bytes = p.bytes;
                    if (p.total > 0) item->total = p.total;
                    item->speed = p.speed;
                    item->eta = p.eta;
                    item->connections = p.connections;
                })) {
                post_queue();
            }
        }, item_stop);
    };
    auto result = fetch_once();
    if (result.outcome == fetch::Outcome::exists && choice == existing::Choice::keep_both) {
        // A copy an earlier Keep both kept that already is the server's file (review G112).
        if (const auto kept = existing::kept_copy(d.target, result.size, util::parse_rfc3339(result.modtime))) {
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
        item->note = "A different file of that name is already there. Resume to choose: replace it, keep both, or skip.";
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
                item->verifying = true;
                item->speed = 0;
                item->eta.reset();
                item->connections = 0;
            }
        }
        if (!check) return set(Status::done, {}, result.present ? "already there, nothing fetched" : result.note);
        post_queue();
        verify::Request base;
        base.ctx = &*ctx;
        base.connections = &connections_;
        base.server = item->remote;
        base.id = item->id;
        const auto checked = verify::file(base, item->path, d.target, item_stop);
        {
            std::lock_guard lock(mutex_);
            item->verifying = false;
            if (!item_stop.stop_requested()) item->verify_pending = false;
        }
        if (!item_stop.stop_requested()) {
            if (!checked) return set(Status::done, {}, "not verified: " + checked.error());
            if (!checked->differ.empty()) {
                std::error_code ec;
                std::filesystem::remove(d.target, ec); // not the server's file
                {
                    std::lock_guard lock(mutex_);
                    item->bytes = 0; // starts over: the row offers Retry, not Resume
                }
                return set(Status::failed, std::format("The downloaded copy {}, so it was deleted. Retry downloads it again.",
                                                       verify::summary(*checked, 1)));
            }
            return set(Status::done, {}, verify::summary(*checked, 1));
        }
    }
    StopReason reason{};
    {
        std::lock_guard lock(mutex_);
        reason = item->stop_reason;
    }
    set(reason == StopReason::cancel ? Status::cancelled : Status::paused);
}

void App::folder_totals(Item& item) const {
    const auto t = folder::totals(item.tree);
    item.bytes = t.bytes;
    item.total = t.total;
    item.speed = t.speed;
    item.eta = t.eta;
    item.connections = item.tree.connections;
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

void App::folder_event(Item& item, folder::Event e) {
    bool post = e != folder::Event::progress;
    {
        std::lock_guard lock(mutex_);
        folder_totals(item);
        const auto now = std::chrono::steady_clock::now();
        if (!post && now - last_progress_post_ >= progress_interval) {
            post = true;
            last_progress_post_ = now;
        }
    }
    if (e == folder::Event::checkpoint) save_queue();
    if (post) post_queue();
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
    // The batch's part of the speed limit: this download's weight over all running downloads'.
    job.batch_limit = [this, item]() -> std::uint64_t {
        const auto limit = limiter_.rate();
        if (limit == 0) return 0;
        int total_weight = 0;
        {
            std::lock_guard lock(mutex_);
            for (const auto& other : items_) {
                if (other->status == Status::running) total_weight += other->weight.load();
            }
        }
        const int weight = item->weight.load();
        return limit * static_cast<std::uint64_t>(weight) / static_cast<std::uint64_t>(std::max(total_weight, weight));
    };
    job.on_event = [this, item](folder::Event e) { folder_event(*item, e); };
    // Its files at once: what "At once" leaves of the files other downloads hold.
    job.files_at_once = [this, item] {
        std::lock_guard lock(mutex_);
        return std::max(1, parallel_ - files_in_use(item.get()));
    };
    // engine = grab: walked and fetched through grab's own SSH client.
    if (effective_engine(ctx->settings) == Engine::grab) {
        auto target = engine::ssh_target(*ctx);
        if (!target) return fail(target.error());
        job.pool = pools_.get(item->remote, *target, connections_.budget(item->remote));
    }

    if (auto listed = folder::list(job, item->tree, mutex_, item_stop); !listed) return fail(listed.error());
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
            folder_totals(*item);
            item->asked = std::move(asked);
            item->asked_count = found.size();
            item->status = Status::paused;
            item->note = std::format("{} already here differ{} from the server's. Resume to choose: replace, keep both, or skip.",
                                     found.size() == 1 ? "1 file" : std::format("{} files", found.size()),
                                     found.size() == 1 ? "s" : "");
            return;
        }
        if (!found.empty()) {
            std::lock_guard lock(mutex_);
            folder::apply(choice, found, folder::local_dir(item->dest, item->path), item->tree);
            folder_totals(*item);
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
            folder_totals(*item);
            item->status = Status::paused;
            item->space_ok = true;
            item->note = text + ". Free some space and Resume, or Resume to download anyway.";
            return;
        }
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
            item->verifying = any;
        }
        if (any) {
            post_queue();
            const auto checked = folder::verify_files(job, item->tree, mutex_, item_stop);
            {
                std::lock_guard lock(mutex_);
                item->verifying = false;
            }
            if (checked.files > 0 && !item_stop.stop_requested()) checked_note = checked.summary();
        }
    }

    bool cancelled = false;
    {
        std::lock_guard lock(mutex_);
        folder_totals(*item);
        if (item_stop.stop_requested()) {
            cancelled = item->stop_reason == StopReason::cancel;
            item->status = cancelled ? Status::cancelled : Status::paused;
        } else if (const auto o = folder::outcome(item->tree); o.failed > 0) {
            item->status = Status::failed;
            item->error = std::format("{} file{} failed. {}", o.failed, o.failed == 1 ? "" : "s", o.first_error);
        } else {
            // Files paused one by one keep the folder paused until they are resumed or skipped.
            item->status = o.waiting > 0 ? Status::paused : Status::done;
            if (!checked_note.empty()) item->note = checked_note;
        }
        // Folders inside it the server wouldn't list: the rest came, and the row says what's missing.
        if (const auto& u = item->tree.unreadable; !u.empty() && !item_stop.stop_requested()) {
            const auto missing = std::format("{} couldn't be read: {}{}", u.size() == 1 ? "1 folder" : std::format("{} folders", u.size()),
                                             u.front(), u.size() > 1 ? ", ..." : "");
            item->note = item->note.empty() ? missing : item->note + ". " + missing;
        }
    }
    if (cancelled) {
        std::lock_guard lock(mutex_);
        discard_folder_partials(*item);
    }
}

json::Value App::queue_snapshot() const {
    Value list = Value::make_array();
    int waiting_position = 0;
    for (const auto& i : items_) {
        Value v = Value::make_object();
        v.set("id", num(i->id));
        v.set("remote", str(i->remote));
        v.set("mode", str(i->mode == Mode::folder ? "folder" : "file"));
        v.set("path", str(i->path));
        v.set("name", str(i->name));
        v.set("dest", str(util::path_to_utf8(i->dest)));
        v.set("local", str(util::path_to_utf8(item_target(*i)))); // where it lands
        v.set("status", str(status_name(static_cast<int>(i->status))));
        if (!i->asked.empty()) v.set("ask", asked_json(i->asked, i->asked_count));
        v.set("bytes", num(static_cast<double>(i->bytes)));
        v.set("total", num(static_cast<double>(i->total)));
        v.set("speed", num(i->speed));
        v.set("eta", i->eta ? num(*i->eta) : Value{});
        v.set("current", str(i->current));
        v.set("error", str(i->error));
        v.set("note", str(i->note));
        v.set("connections", num(i->connections));
        if (i->verifying) v.set("verifying", Value::make_bool(true));
        v.set("priority", str(priority_name(i->weight.load())));
        if (i->status == Status::queued) v.set("position", num(++waiting_position)); // "Queued · #2"
        if (i->mode == Mode::folder && i->tree.listed) {
            // Counts for the folder row, and a bounded list of files for the tree: every
            // active one plus the next few queued, so a 5000-file folder stays cheap to send.
            const auto& tree_files = i->tree.files;
            std::vector<folder::State> states;
            states.reserve(tree_files.size());
            int counts[6] = {}; // by folder::State
            for (const auto& c : tree_files) {
                states.push_back(c.status);
                ++counts[static_cast<int>(c.status)];
            }
            auto count = [&](folder::State s) { return num(counts[static_cast<int>(s)]); };
            Value files = Value::make_object();
            files.set("total", num(static_cast<double>(tree_files.size())));
            files.set("queued", count(folder::State::queued));
            files.set("running", count(folder::State::running));
            files.set("paused", count(folder::State::paused));
            files.set("done", count(folder::State::done));
            files.set("failed", count(folder::State::failed));
            files.set("skipped", count(folder::State::skipped));
            v.set("files", std::move(files));
            constexpr std::size_t queued_shown = 20;
            Value children = Value::make_array();
            for (const auto k : folder::visible(states, queued_shown)) {
                const auto& c = tree_files[k];
                Value cv = Value::make_object();
                cv.set("path", str(c.path));
                cv.set("local", str(util::path_to_utf8(c.local.empty() ? local::relative(c.path) : c.local)));
                // A name grab gave it: Keep both, or another file of the folder has its own (G25).
                if (!c.local.empty()) cv.set("savedAs", str(util::path_to_utf8(c.local.filename())));
                cv.set("status", str(file_state_name(c.status)));
                cv.set("bytes", num(static_cast<double>(c.bytes)));
                cv.set("size", num(static_cast<double>(c.size)));
                cv.set("speed", num(c.speed));
                cv.set("eta", c.eta ? num(*c.eta) : Value{});
                cv.set("error", str(c.error));
                children.push(std::move(cv));
            }
            v.set("children", std::move(children));
            const auto queued = static_cast<std::size_t>(counts[static_cast<int>(folder::State::queued)]);
            v.set("moreQueued", num(static_cast<double>(queued > queued_shown ? queued - queued_shown : 0)));
        }
        list.push(std::move(v));
    }
    Value msg = Value::make_object();
    msg.set("type", str("queue"));
    msg.set("items", std::move(list));
    msg.set("parallel", num(parallel_));
    return msg;
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
                                                  : folder::State::queued;
                c.fetched = f.fetched;
                c.verified = f.verified;
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
            folder_totals(*item);
        }
        items_.push_back(std::move(item));
    }
}

void App::save_queue() const {
    // The snapshot is taken under the file's lock too, so two savers can't write in the opposite
    // order of their snapshots and leave an older queue on disk (review G53).
    std::lock_guard file_lock(queue_file_mutex_);
    std::vector<SavedDownload> unfinished;
    {
        std::lock_guard lock(mutex_);
        for (const auto& i : items_) {
            if (i->status == Status::done || i->status == Status::cancelled) continue;
            SavedDownload d{i->remote, i->mode == Mode::folder ? "folder" : "file", i->path, i->name,
                            util::path_to_utf8(i->dest), i->total, {}, priority_name(i->weight.load())};
            d.resume = resume_after_restart_ && i->resume_mark;
            d.space_ok = i->space_ok;
            if (i->existing != existing::Choice::ask) d.existing = std::string(existing::name(i->existing));
            d.local_name = util::path_to_utf8(i->local_name);
            for (const auto& c : i->tree.files) {
                const char* state = c.status == folder::State::done      ? "done"
                                    : c.status == folder::State::skipped ? "skipped"
                                    : c.status == folder::State::paused  ? "paused"
                                                                         : "queued"; // running and failed retry
                const bool done = c.status == folder::State::done;
                d.files.push_back(SavedFile{c.path, c.size, c.modtime, state, done && c.fetched, done && c.verified,
                                            util::path_to_utf8(c.local)});
            }
            unfinished.push_back(std::move(d));
        }
    }
    std::error_code ec;
    if (unfinished.empty()) {
        std::filesystem::remove(queue_path_, ec);
        return;
    }
    report_save("the download queue", util::write_file_atomic(queue_path_, queue_to_json(unfinished)), queue_unsaved_);
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
    post(v);
}

void App::maybe_check_updates() {
    if (update_checks_started_) return;
    update_checks_started_ = true;
    const auto current = update::parse_version(GRAB_VERSION).value_or(update::Version{});
    // A newer release an earlier check found shows at once, without asking GitHub again.
    if (auto seen = update::parse_version(state_.latest_version); seen && *seen > current) {
        update_ = UpdateBanner{"available", update::to_string(*seen),
                               update::is_release_page(state_.latest_page) ? state_.latest_page : "", {}, {}, false};
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
    if (update_.state != "running" && update_.state != "installed") {
        if (c.available()) {
            update_ = UpdateBanner{"available", latest, c.release.page, {}, {}, c.release.version == c.current};
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
// own, outside grab-gui's job, because the installer closes grab-gui; its output, written to a
// file, tells the banner how far it got.
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
    switch (update::phase_of(line)) {
    case update::Phase::checking: update_.phase = "checking"; break;
    case update::Phase::downloading: update_.phase = "downloading"; break;
    case update::Phase::verified: update_.phase = "verified"; break;
    case update::Phase::installing:
        // The installer closes grab-gui next and starts it again afterwards: running downloads
        // stop there (partial files kept) and continue in the new one.
        update_.phase = "installing";
        mark_for_restart(true);
        break;
    case update::Phase::done: update_.phase = "done"; break;
    case update::Phase::up_to_date: update_.phase = "up_to_date"; break;
    case update::Phase::failed:
    case update::Phase::other: return;
    }
    if (update_.state == "running") post_update();
}

void App::on_update_finished(int exit_code, const std::string& last_error) {
    if (resume_after_restart_) mark_for_restart(false); // still here: nothing to continue after
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

void App::post_queue() {
    std::string text;
    {
        std::lock_guard lock(mutex_);
        text = json::stringify(queue_snapshot());
    }
    host_.post(std::move(text));
}

} // namespace grab::gui
