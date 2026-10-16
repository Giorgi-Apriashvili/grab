#include "app.hpp"

#include "config.hpp"
#include "engine.hpp"
#include "folder.hpp"
#include "ini.hpp"
#include "process.hpp"
#include "quote.hpp"
#include "util.hpp"

#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>   // IWYU pragma: keep (SHGetKnownFolderPath via shlobj_core.h)
#include <shobjidl.h> // IWYU pragma: keep (IFileOpenDialog via shobjidl_core.h)
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
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
      state_(load_gui_state(state_path_)),
      connections_([this](const std::string& server, int budget) { on_budget_learned(server, budget); }) {
    parallel_ = state_.parallel;
    learned_ = state_.server_limits;
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
    lists_dir_ = file_list::default_dir();
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

void App::save_state() const { save_gui_state(state_path_, state_); }

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
        if (auto mode = msg->string_of("mode"); mode == "file" || mode == "folder") state_.mode = *mode;
        save_state();
    } else if (type == "search") {
        start_search(*msg);
    } else if (type == "refreshLists") {
        const std::string remote = msg->string_of("remote").value_or("");
        if (remote == "*") {
            if (auto cfg = load_grab_config(config_path_)) {
                for (const auto& r : cfg->remotes) start_refresh(r.name);
            }
        } else if (!remote.empty()) {
            start_refresh(remote);
        }
    } else if (type == "cancelSearch") {
        if (search_thread_.joinable()) search_thread_.request_stop();
    } else if (type == "pickFolder") {
        // Not from inside WebView2's event handler: the dialog runs its own message loop.
        std::wstring current = util::to_wide(msg->string_of("current").value_or(""));
        host_.defer([this, current = std::move(current)] { pick_folder(current); });
    } else if (type == "enqueue") {
        enqueue(*msg);
    } else if (type == "cancelItem") {
        cancel_items(int_of(*msg, "id", -1));
    } else if (type == "pauseItem") {
        pause_items(int_of(*msg, "id", -1));
    } else if (type == "resumeItem") {
        resume_items(int_of(*msg, "id", -1));
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
    init.set("configPath", str(util::path_to_utf8(config_path_)));

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
    init.set("mode", str(state_.mode));
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
    run_search(int_of(msg, "id"), msg.string_of("remote").value_or(""), req, false);
}

// A server's search answers from its saved file list when it has one that fits; a list older
// than file_list::fresh_for is refreshed in the background, and the search then runs again. A
// server without a list is searched live (one server) while its list is fetched for next time.
void App::run_search(int id, const std::string& remote, const engine::SearchRequest& req, bool update) {
    // A new search replaces the previous one; stopping kills its ssh/rclone child at once.
    if (search_thread_.joinable()) {
        search_replaced_ = true;
        search_thread_.request_stop();
        search_thread_.join();
        search_replaced_ = false;
    }
    {
        std::lock_guard lock(lists_mutex_);
        last_search_ = LastSearch{id, remote, req};
    }
    search_thread_ = std::jthread([this, id, remote, req, update](std::stop_token stop) {
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

        const bool all = remote == "*";
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
        for (const auto& server : servers) {
            if (stop.stop_requested()) return fail("search cancelled", true);
            Value status = Value::make_object();
            status.set("remote", str(server));
            auto ctx = engine::load_context(config_path_, server);
            if (!ctx) {
                if (!all) return fail(ctx.error(), false);
                status.set("error", str(ctx.error()));
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
                if (!update && (!list || !file_list::fresh(list->fetched, now))) start_refresh(server);
                if (list) {
                    found = file_list::search(*ctx, *list, req);
                    status.set("age", num(static_cast<double>(now - list->fetched)));
                }
                std::lock_guard lock(lists_mutex_);
                const auto& s = list_slot(server);
                status.set("refreshing", Value::make_bool(s.refreshing));
                if (!s.error.empty()) status.set("error", str(s.error));
            }
            // One server without a usable list, or a search lists can't answer: live, as before.
            if (!found && (!all || !listable)) {
                auto live = engine::search(*ctx, req, run);
                if (stop.stop_requested()) return fail("search cancelled", true);
                if (!live) {
                    if (!all) return fail(live.error(), false);
                    status.set("error", str(live.error()));
                } else {
                    found = std::move(*live);
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
        if (all) file_list::rank_across(hits, req);

        v.set("type", str("searchResult"));
        v.set("id", num(id));
        v.set("update", Value::make_bool(update));
        if (all) {
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

void App::start_refresh(const std::string& server) {
    std::lock_guard lock(lists_mutex_);
    auto& s = list_slot(server);
    if (s.refreshing) return;
    s.refreshing = true;
    // The previous refresh of this server, if any, has finished: replacing it joins it at once.
    refresh_threads_[server] = std::jthread([this, server](std::stop_token stop) {
        proc::RunOptions run;
        run.stop = stop;
        run.detached = true;
        auto ctx = engine::load_context(config_path_, server);
        auto fetched = ctx ? file_list::fetch(*ctx, unix_now(), true, run)
                           : std::expected<file_list::List, std::string>(std::unexpected(ctx.error()));
        if (fetched) (void)file_list::save(file_list::path_for(lists_dir_, server), *fetched);
        {
            std::lock_guard inner(lists_mutex_);
            auto& slot = list_slot(server);
            slot.refreshing = false;
            if (stop.stop_requested()) return;
            if (fetched) {
                slot.list = std::make_shared<const file_list::List>(std::move(*fetched));
                slot.error.clear();
            } else {
                slot.error = fetched.error();
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
    if (last.id != 0 && (last.remote == server || last.remote == "*")) run_search(last.id, last.remote, last.req, true);
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
    if (!std::filesystem::exists(config_path_, ec) &&
        !server_ops::write_text(config_path_, generate_grab_config(nullptr, ""))) {
        return settings_error("cannot create " + util::path_to_utf8(config_path_));
    }
    std::vector<std::string> candidates;
    if (auto doc = ini::parse_file(config_path_)) {
        if (const auto* g = doc->find("grab")) candidates.push_back(g->get("editor").value_or(""));
    }
    candidates.push_back(util::getenv_utf8("VISUAL").value_or(""));
    candidates.push_back(util::getenv_utf8("EDITOR").value_or(""));
    const auto argv = editor_command(candidates, config_path_);
    const std::wstring exe = util::to_wide(argv.front());
    const std::wstring params =
        util::to_wide(quote::windows_cmdline(std::span<const std::string>(argv).subspan(1)));
    const auto rc = reinterpret_cast<INT_PTR>(
        ShellExecuteW(host_.hwnd, L"open", exe.c_str(), params.c_str(), nullptr, SW_SHOWNORMAL));
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
            // The automatic value, lowered to what grab learned from refusals.
            std::lock_guard lock(mutex_);
            const auto it = learned_.find(s.name);
            v.set("autoConnections",
                  num(budget::effective_budget(std::nullopt, s.default_connections,
                                               it == learned_.end() ? std::nullopt : std::optional(it->second))));
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
            if (s.auth == servers::Auth::password && p.secret.empty() && it->auth != servers::Auth::password) {
                return invalid("secret", "enter the password");
            }
            need_keys = it->error.empty() ? server_ops::edit_needs_host_key(*it, s) : true;
        } else {
            p.kind = Pending::Kind::add;
            if (server_ops::find_server(env->cfg, s.name) != nullptr) {
                return invalid("name", "a server with this name already exists");
            }
            auto rclone_doc = ini::parse(util::read_file(env->rclone_conf).value_or(""));
            if (rclone_doc && rclone_doc->find(s.name) != nullptr) {
                return invalid("name", "grab's rclone.conf already has a remote with this name");
            }
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
    std::expected<void, std::string> done;
    switch (p.kind) {
    case Pending::Kind::add:
        done = server_ops::add_server(*env, p.server, obscured, p.keys);
        break;
    case Pending::Kind::edit:
        done = server_ops::update_server(*env, p.server, obscured,
                                         p.keys.lines.empty() ? std::nullopt : std::optional(p.keys));
        break;
    case Pending::Kind::trust:
        done = server_ops::trust_server(*env, p.server.name, p.keys);
        break;
    }
    if (!done) return settings_error(done.error());
    // An explicit connection limit replaces whatever grab learned for this server.
    if (p.server.max_connections) {
        const std::string name = p.server.name;
        {
            std::lock_guard lock(mutex_);
            learned_.erase(name);
        }
        host_.defer([this, name] {
            state_.server_limits.erase(name);
            save_state();
        });
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
    save_queue();
    post_queue();
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

void App::resume_items(int id) {
    {
        std::lock_guard lock(mutex_);
        for (auto& item : items_) {
            if ((id == 0 || item->id == id) && item->status == Status::paused) {
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
    std::vector<std::filesystem::path> discard; // partial files of downloads not running now
    {
        std::lock_guard lock(mutex_);
        for (auto& item : items_) {
            if (id != 0 && item->id != id) continue;
            if (item->status == Status::queued || item->status == Status::paused || item->status == Status::failed) {
                item->status = Status::cancelled;
                item->speed = 0;
                item->eta.reset();
                if (item->mode == Mode::file) discard.push_back(item->dest / util::path_from_utf8(item->name));
                else discard_folder_partials(*item); // finished files stay
            } else if (item->status == Status::running) {
                item->stop_reason = StopReason::cancel;
                item->stop.request_stop();
            }
        }
    }
    for (const auto& target : discard) fetch::discard(target);
    save_queue();
    post_queue();
}

void App::retry_item(int id) {
    {
        std::lock_guard lock(mutex_);
        for (auto& item : items_) {
            if (item->id == id && (item->status == Status::failed || item->status == Status::cancelled)) {
                // A failed file download continues from its partial file; a cancelled one's
                // was deleted, so it starts over.
                const bool cancelled = item->status == Status::cancelled;
                item->status = Status::queued;
                item->error.clear();
                if (item->mode == Mode::folder) {
                    folder::retry_files(item->tree, cancelled); // after a cancel, everything not finished
                } else if (!fetch::saved_progress(item->dest / util::path_from_utf8(item->name))) {
                    item->bytes = 0;
                }
                item->speed = 0;
                item->eta.reset();
            }
        }
    }
    wake_.notify_all();
    save_queue();
    post_queue();
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
    std::optional<std::filesystem::path> discard;
    {
        std::lock_guard lock(mutex_);
        for (auto& item : items_) {
            if (item->id != id) continue;
            discard = folder::skip_file(item->tree, folder::local_dir(item->dest, item->path), path);
            folder_totals(*item);
        }
    }
    if (discard) fetch::discard(*discard);
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
            auto next_queued = [&]() -> std::shared_ptr<Item> {
                const auto running = std::ranges::count_if(items_, [](const auto& i) { return i->status == Status::running; });
                if (running >= parallel_) return nullptr;
                // Never two downloads into the same file or folder at once: they would share
                // one partial file.
                auto same_target = [](const Item& a, const Item& b) {
                    return a.name == b.name && a.dest == b.dest;
                };
                auto it = std::ranges::find_if(items_, [&](const auto& i) {
                    return i->status == Status::queued && std::ranges::none_of(items_, [&](const auto& r) {
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
    if (item->mode == Mode::file) run_file(item, item_stop);
    else run_folder(item, item_stop);

    // A stopped download ends paused or cancelled, as asked; closing the app leaves it paused.
    std::lock_guard lock(mutex_);
    item->speed = 0;
    item->eta.reset();
    item->connections = 0;
    item->current.clear();
    if (item->status == Status::cancelled && item->mode == Mode::file) {
        fetch::discard(item->dest / util::path_from_utf8(item->name));
    }
}

int App::connection_budget(const std::string& remote, const std::string& host, std::optional<int> configured) const {
    std::optional<int> learned;
    {
        std::lock_guard lock(mutex_);
        if (auto it = learned_.find(remote); it != learned_.end()) learned = it->second;
    }
    return budget::effective_budget(configured, budget::default_budget(host), learned);
}

void App::on_budget_learned(const std::string& server, int budget) {
    {
        std::lock_guard lock(mutex_);
        learned_[server] = budget;
    }
    host_.defer([this, server, budget] {
        state_.server_limits[server] = budget;
        save_state();
    });
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
                             engine::without_console_progress(ctx->settings.common_flags)};
    d.server = item->remote;
    d.id = item->id;
    d.target = item->dest / util::path_from_utf8(item->name);
    d.limiter = &limiter_;
    d.weight = &item->weight;
    const auto result = fetch::run(d, connections_, [&](const fetch::Progress& p) {
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

    switch (result.outcome) {
    case fetch::Outcome::done:
        return set(Status::done, {}, result.present ? "already there, nothing fetched" : result.note);
    case fetch::Outcome::failed:
        return set(Status::failed, result.error, result.note);
    case fetch::Outcome::stopped:
        break;
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
    folder::discard_partials(folder::local_dir(item.dest, item.path), item.tree);
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

    if (auto listed = folder::list(job, item->tree, mutex_, item_stop); !listed) return fail(listed.error());
    // Files already complete on disk (an earlier run, or downloaded before) are not fetched
    // again; big ones with a partial file continue from it.
    folder::mark_existing(job, item->tree, mutex_);
    folder::run(job, item->tree, mutex_, item_stop);

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
        v.set("status", str(status_name(static_cast<int>(i->status))));
        v.set("bytes", num(static_cast<double>(i->bytes)));
        v.set("total", num(static_cast<double>(i->total)));
        v.set("speed", num(i->speed));
        v.set("eta", i->eta ? num(*i->eta) : Value{});
        v.set("current", str(i->current));
        v.set("error", str(i->error));
        v.set("note", str(i->note));
        v.set("connections", num(i->connections));
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
    auto text = util::read_file(queue_path_);
    if (!text) return;
    for (const auto& saved : parse_queue(*text)) {
        auto item = std::make_shared<Item>();
        item->id = next_id_++;
        item->remote = saved.remote;
        item->mode = saved.mode == "folder" ? Mode::folder : Mode::file;
        item->path = saved.path;
        item->name = saved.name;
        item->dest = util::path_from_utf8(saved.dest);
        item->total = saved.total;
        // Saved for an update's restart while running or queued: it goes on by itself.
        item->status = saved.resume ? Status::queued : Status::paused;
        item->weight = weight_of(saved.priority);
        if (item->mode == Mode::file) {
            if (auto p = fetch::saved_progress(item->dest / util::path_from_utf8(item->name))) {
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
                if (c.status == folder::State::done) c.bytes = c.size;
                else if (c.big) {
                    if (auto p = fetch::saved_progress(dir / util::path_from_utf8(c.path))) c.bytes = p->first;
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
    std::vector<SavedDownload> unfinished;
    {
        std::lock_guard lock(mutex_);
        for (const auto& i : items_) {
            if (i->status == Status::done || i->status == Status::cancelled) continue;
            SavedDownload d{i->remote, i->mode == Mode::folder ? "folder" : "file", i->path, i->name,
                            util::path_to_utf8(i->dest), i->total, {}, priority_name(i->weight.load())};
            d.resume = resume_after_restart_ && i->resume_mark;
            for (const auto& c : i->tree.files) {
                const char* state = c.status == folder::State::done      ? "done"
                                    : c.status == folder::State::skipped ? "skipped"
                                    : c.status == folder::State::paused  ? "paused"
                                                                         : "queued"; // running and failed retry
                d.files.push_back(SavedFile{c.path, c.size, c.modtime, state});
            }
            unfinished.push_back(std::move(d));
        }
    }
    std::lock_guard lock(queue_file_mutex_);
    std::error_code ec;
    if (unfinished.empty()) {
        std::filesystem::remove(queue_path_, ec);
        return;
    }
    (void)server_ops::write_text(queue_path_, queue_to_json(unfinished));
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
        auto result = update::check(true, stop);
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
