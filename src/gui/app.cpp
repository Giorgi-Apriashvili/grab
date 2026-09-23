#include "app.hpp"

#include "config.hpp"
#include "engine.hpp"
#include "process.hpp"
#include "util.hpp"

#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>   // IWYU pragma: keep (SHGetKnownFolderPath via shlobj_core.h)
#include <shobjidl.h> // IWYU pragma: keep (IFileOpenDialog via shobjidl_core.h)
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <utility>

#ifndef GRAB_VERSION
#define GRAB_VERSION "dev"
#endif

using Microsoft::WRL::ComPtr;

namespace grab::gui {

namespace {

using json::Value;

constexpr auto progress_interval = std::chrono::milliseconds(250); // at most 4 updates a second

Value str(std::string s) { return Value::make_string(std::move(s)); }
Value num(double n) { return Value::make_number(n); }

const char* status_name(int s) {
    constexpr const char* names[] = {"queued", "running", "done", "failed", "cancelled"};
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
      state_(load_gui_state(state_path_)),
      worker_([this](std::stop_token stop) { worker_loop(std::move(stop)); }) {}

App::~App() {
    if (search_thread_.joinable()) search_thread_.request_stop();
    {
        std::lock_guard lock(mutex_);
        for (auto& item : items_) {
            if (item->status == Status::running) item->stop.request_stop();
        }
    }
    worker_.request_stop();
    wake_.notify_all();
}

void App::save_state() const { save_gui_state(state_path_, state_); }

void App::post(const Value& message) const { host_.post(json::stringify(message)); }

bool App::downloads_active() const {
    std::lock_guard lock(mutex_);
    return std::ranges::any_of(items_, [](const auto& i) {
        return i->status == Status::queued || i->status == Status::running;
    });
}

// ---- messages -----------------------------------------------------------------------------

void App::on_message(const std::string& text) {
    auto msg = json::parse(text);
    if (!msg || msg->kind != Value::Kind::object) return;
    const std::string type = msg->string_of("type").value_or("");

    if (type == "init") {
        send_init();
    } else if (type == "prefs") {
        if (auto remote = msg->string_of("remote")) state_.last_remote = *remote;
        if (auto mode = msg->string_of("mode"); mode == "file" || mode == "folder") state_.mode = *mode;
        save_state();
    } else if (type == "search") {
        start_search(*msg);
    } else if (type == "cancelSearch") {
        if (search_thread_.joinable()) search_thread_.request_stop();
    } else if (type == "pickFolder") {
        // Not from inside WebView2's event handler: the dialog runs its own message loop.
        std::wstring current = util::to_wide(msg->string_of("current").value_or(""));
        host_.defer([this, current = std::move(current)] { pick_folder(current); });
    } else if (type == "enqueue") {
        enqueue(*msg);
    } else if (type == "cancelItem") {
        cancel_item(int_of(*msg, "id"));
    } else if (type == "retryItem") {
        retry_item(int_of(*msg, "id"));
    } else if (type == "clearFinished") {
        clear_finished();
    } else if (type == "openFolder") {
        const int id = int_of(*msg, "id");
        host_.defer([this, id] { open_item_folder(id); });
    }
}

void App::send_init() {
    Value init = Value::make_object();
    init.set("type", str("init"));
    init.set("version", str(GRAB_VERSION));
    init.set("configPath", str(util::path_to_utf8(config_path_)));

    Value remotes = Value::make_array();
    std::string selected;
    auto cfg = load_grab_config(config_path_);
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
        if (has(state_.last_remote)) selected = state_.last_remote;
        else if (cfg->default_remote && has(*cfg->default_remote)) selected = *cfg->default_remote;
        else if (!cfg->remotes.empty()) selected = cfg->remotes.front().name;
    }
    init.set("remotes", std::move(remotes));
    init.set("remote", str(selected));
    init.set("mode", str(state_.mode));
    Value dests = Value::make_object();
    for (const auto& [remote, path] : state_.destinations) dests.set(remote, str(path));
    init.set("destinations", std::move(dests));
    init.set("defaultDest", str(known_folder(FOLDERID_Downloads)));
    post(init);
    post_queue();
}

void App::start_search(const Value& msg) {
    // A new search replaces the previous one; stopping kills its ssh/rclone child at once.
    if (search_thread_.joinable()) {
        search_thread_.request_stop();
        search_thread_.join();
    }
    const int id = int_of(msg, "id");
    const std::string remote = msg.string_of("remote").value_or("");
    engine::SearchRequest req;
    req.target = msg.string_of("query").value_or("");
    req.mode = mode_of(msg);
    req.exact = bool_of(msg, "exact");
    req.batch_ssh = true; // no console to answer a prompt: fail fast instead
    if (int depth = int_of(msg, "depth"); depth > 0) req.depth = depth;

    search_thread_ = std::jthread([this, id, remote, req](std::stop_token stop) {
        const auto t0 = std::chrono::steady_clock::now();
        auto fail = [&](const std::string& message, bool cancelled) {
            Value v = Value::make_object();
            v.set("type", str("searchError"));
            v.set("id", num(id));
            v.set("message", str(message));
            v.set("cancelled", Value::make_bool(cancelled));
            post(v);
        };
        if (util::trim(req.target).empty()) return fail("type something to search for", false);

        auto ctx = engine::load_context(config_path_, remote);
        if (!ctx) return fail(ctx.error(), false);
        proc::RunOptions run;
        run.stop = stop;
        run.detached = true;
        auto found = engine::search(*ctx, req, run);
        if (stop.stop_requested()) return fail("search cancelled", true);
        if (!found) return fail(found.error(), false);

        Value v = Value::make_object();
        v.set("type", str("searchResult"));
        v.set("id", num(id));
        v.set("method", str(ctx->method == FindMethod::ssh ? "ssh" : "rclone"));
        v.set("maxDepth", num(found->max_depth));
        Value roots = Value::make_array();
        for (const auto& r : found->roots) roots.push(str(r.empty() ? "~" : r));
        v.set("roots", std::move(roots));
        Value hits = Value::make_array();
        for (const auto& h : found->hits) {
            Value hit = Value::make_object();
            hit.set("path", str(h.path));
            hit.set("name", str(h.name));
            hit.set("size", h.size ? num(static_cast<double>(*h.size)) : Value{});
            hits.push(std::move(hit));
        }
        v.set("hits", std::move(hits));
        v.set("elapsedMs", num(static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                       std::chrono::steady_clock::now() - t0)
                                                       .count())));
        post(v);
    });
}

void App::pick_folder(std::wstring current) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Save downloads to");
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
        post(v);
    }
    CoTaskMemFree(path);
}

// ---- download queue -------------------------------------------------------------------------

void App::enqueue(const Value& msg) {
    const std::string remote = msg.string_of("remote").value_or("");
    const std::string dest = std::string(util::trim(msg.string_of("dest").value_or("")));
    const Value* items = msg.find("items");
    if (remote.empty() || items == nullptr || items->kind != Value::Kind::array) return;
    // A GUI's working directory is arbitrary, so a relative folder would land somewhere
    // unexpected; insist on a full path.
    if (!util::path_from_utf8(dest).is_absolute()) {
        Value v = Value::make_object();
        v.set("type", str("error"));
        v.set("message", str(std::format("\"{}\" is not a full folder path. Use something like "
                                         "C:\\Users\\you\\Downloads, or pick one with Browse.",
                                         dest)));
        post(v);
        return;
    }

    state_.destinations[remote] = dest;
    state_.last_remote = remote;
    save_state();

    {
        std::lock_guard lock(mutex_);
        for (const auto& it : items->items) {
            auto path = it.string_of("path");
            if (!path) continue;
            auto item = std::make_shared<Item>();
            item->id = next_id_++;
            item->remote = remote;
            item->mode = mode_of(msg);
            item->path = *path;
            item->name = it.string_of("name").value_or(*path);
            item->dest = util::path_from_utf8(dest);
            if (const Value* size = it.find("size"); size && size->kind == Value::Kind::number) {
                item->total = static_cast<std::uint64_t>(size->number);
            }
            items_.push_back(std::move(item));
        }
    }
    wake_.notify_all();
    post_queue();
}

void App::cancel_item(int id) {
    {
        std::lock_guard lock(mutex_);
        for (auto& item : items_) {
            if (item->id != id) continue;
            if (item->status == Status::queued) item->status = Status::cancelled;
            else if (item->status == Status::running) item->stop.request_stop();
        }
    }
    post_queue();
}

void App::retry_item(int id) {
    {
        std::lock_guard lock(mutex_);
        for (auto& item : items_) {
            if (item->id == id && (item->status == Status::failed || item->status == Status::cancelled)) {
                item->status = Status::queued;
                item->error.clear();
                item->bytes = 0;
                item->speed = 0;
                item->eta.reset();
                item->stop = std::stop_source{};
            }
        }
    }
    wake_.notify_all();
    post_queue();
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
            if (item->id == id) target = item->dest / util::path_from_utf8(item->name);
        }
    }
    if (target.empty()) return;
    std::error_code ec;
    if (std::filesystem::exists(target, ec)) {
        // Open Explorer with the downloaded file or folder selected.
        const std::wstring args = L"/select,\"" + target.wstring() + L"\"";
        ShellExecuteW(host_.hwnd, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    } else {
        ShellExecuteW(host_.hwnd, L"open", target.parent_path().c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
    }
}

void App::worker_loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::shared_ptr<Item> item;
        {
            std::unique_lock lock(mutex_);
            auto next_queued = [&] {
                auto it = std::ranges::find_if(items_, [](const auto& i) { return i->status == Status::queued; });
                return it == items_.end() ? nullptr : *it;
            };
            wake_.wait(lock, stop, [&] { return next_queued() != nullptr; });
            if (stop.stop_requested()) return;
            item = next_queued();
            item->status = Status::running;
        }
        post_queue();
        run_item(item, stop);
        post_queue();
    }
}

void App::run_item(const std::shared_ptr<Item>& item, std::stop_token app_stop) {
    auto finish = [&](Status status, std::string error) {
        std::lock_guard lock(mutex_);
        item->status = status;
        item->error = std::move(error);
        item->speed = 0;
        item->eta.reset();
        item->current.clear();
        if (status == Status::done && item->total > 0) item->bytes = item->total;
    };

    auto ctx = engine::load_context(config_path_, item->remote);
    if (!ctx) return finish(Status::failed, ctx.error());
    std::error_code ec;
    std::filesystem::create_directories(item->dest, ec);
    if (ec) {
        return finish(Status::failed, std::format("cannot create {}: {}", util::path_to_utf8(item->dest),
                                                  ec.message()));
    }

    std::stop_token item_stop;
    {
        std::lock_guard lock(mutex_);
        item_stop = item->stop.get_token();
    }
    // Closing the app cancels the running download too.
    std::stop_callback on_app_stop(app_stop, [item] { item->stop.request_stop(); });

    proc::RunOptions run;
    run.stop = item_stop;
    run.detached = true;
    auto result = engine::download(
        *ctx, item->mode, item->path, item->dest,
        [&](const engine::Progress& p) {
            bool post_now = false;
            {
                std::lock_guard lock(mutex_);
                item->bytes = p.bytes;
                if (p.total > 0) item->total = p.total;
                item->speed = p.speed;
                item->eta = p.eta;
                item->current = p.current;
                const auto now = std::chrono::steady_clock::now();
                if (now - last_progress_post_ >= progress_interval) {
                    last_progress_post_ = now;
                    post_now = true;
                }
            }
            if (post_now) post_queue();
        },
        run);

    if (!result) return finish(Status::failed, result.error());
    if (result->exit_code == 0) return finish(Status::done, {});
    if (result->exit_code == proc::exit_stopped && item_stop.stop_requested()) {
        return finish(Status::cancelled, {});
    }
    finish(Status::failed, result->last_error.empty()
                               ? std::format("rclone exited with code {}", result->exit_code)
                               : result->last_error);
}

json::Value App::queue_snapshot() const {
    Value list = Value::make_array();
    for (const auto& i : items_) {
        Value v = Value::make_object();
        v.set("id", num(i->id));
        v.set("remote", str(i->remote));
        v.set("mode", str(i->mode == Mode::folder ? "folder" : "file"));
        v.set("path", str(i->path));
        v.set("name", str(i->name));
        v.set("dest", str(util::path_to_utf8(i->dest)));
        v.set("status", str(status_name(static_cast<int>(i->status))));
        v.set("bytes", num(static_cast<double>(i->bytes)));
        v.set("total", num(static_cast<double>(i->total)));
        v.set("speed", num(i->speed));
        v.set("eta", i->eta ? num(*i->eta) : Value{});
        v.set("current", str(i->current));
        v.set("error", str(i->error));
        list.push(std::move(v));
    }
    Value msg = Value::make_object();
    msg.set("type", str("queue"));
    msg.set("items", std::move(list));
    return msg;
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
