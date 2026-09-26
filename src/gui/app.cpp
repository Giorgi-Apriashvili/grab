#include "app.hpp"

#include "config.hpp"
#include "engine.hpp"
#include "folder.hpp"
#include "ini.hpp"
#include "process.hpp"
#include "quote.hpp"
#include "rclone.hpp"
#include "util.hpp"

#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>   // IWYU pragma: keep (SHGetKnownFolderPath via shlobj_core.h)
#include <shobjidl.h> // IWYU pragma: keep (IFileOpenDialog via shobjidl_core.h)
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <memory>
#include <span>
#include <unordered_map>
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

// A folder download's own local directory: DEST\<folder name>, as rclone copy names it.
std::filesystem::path folder_dir(const std::filesystem::path& dest, const std::string& remote_path) {
    return dest / util::path_from_utf8(remote_basename(remote_path));
}

std::string child_remote_path(const std::string& folder, const std::string& rel) {
    return folder.empty() || folder.ends_with('/') ? folder + rel : folder + "/" + rel;
}

// For folder::visible; `status` in App::Status order: queued, running, paused, done, failed,
// cancelled, skipped.
folder::State folder_state(int status) {
    constexpr folder::State map[] = {folder::State::queued, folder::State::running, folder::State::paused,
                                     folder::State::done,   folder::State::failed,  folder::State::skipped,
                                     folder::State::skipped};
    return map[status];
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
    if (state_.limit_on) limiter_.set_rate(static_cast<std::uint64_t>(state_.limit_mibps * 1024 * 1024));
    restore_queue();
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
    init.set("parallel", num(state_.parallel));
    init.set("limitOn", Value::make_bool(state_.limit_on));
    init.set("limitMiBps", num(state_.limit_mibps));
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
                // A folder's Resume covers its files paused one by one too.
                for (auto& c : item->children) {
                    if (c.status == Status::paused) c.status = Status::queued;
                }
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
                    // Failed files are tried again; after a cancel, everything not finished.
                    for (auto& c : item->children) {
                        if (c.status == Status::failed || (cancelled && c.status != Status::done && c.status != Status::skipped)) {
                            c.status = Status::queued;
                            c.error.clear();
                        }
                    }
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
            for (auto& c : item->children) {
                if (c.path != path) continue;
                if (c.status == Status::queued) {
                    c.status = Status::paused;
                    if (c.in_batch) item->batch_stop.request_stop(); // rclone would still fetch it
                } else if (c.status == Status::running && c.big) {
                    c.stop_reason = StopReason::pause; // run_big_files marks it paused once stopped
                    c.stop.request_stop();
                } else if (c.status == Status::running) {
                    c.status = Status::paused; // in the small batch: restart it without this file
                    c.speed = 0;
                    c.bytes = 0; // rclone keeps no partial small file; it starts over on resume
                    item->batch_stop.request_stop();
                }
            }
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
            for (auto& c : item->children) {
                if (c.path == path && c.status == Status::paused) c.status = Status::queued;
            }
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
            for (auto& c : item->children) {
                if (c.path != path) continue;
                if (c.status == Status::queued || c.status == Status::paused || c.status == Status::failed) {
                    if (c.status == Status::queued && c.in_batch) item->batch_stop.request_stop(); // rclone would still fetch it
                    c.status = Status::skipped;
                    if (c.big) discard = folder_dir(item->dest, item->path) / util::path_from_utf8(c.path);
                } else if (c.status == Status::running && c.big) {
                    c.stop_reason = StopReason::skip; // run_big_files discards its partial file
                    c.stop.request_stop();
                } else if (c.status == Status::running) {
                    c.status = Status::skipped; // restart the batch without it
                    c.speed = 0;
                    item->batch_stop.request_stop();
                }
            }
            folder_totals(*item);
        }
    }
    if (discard) fetch::discard(*discard);
    save_queue();
    post_queue();
}

void App::set_limit(bool on, double mibps) {
    if (!(mibps >= min_limit_mibps && mibps <= max_limit_mibps)) mibps = state_.limit_mibps; // also NaN
    state_.limit_on = on;
    state_.limit_mibps = mibps;
    save_state();
    // Streams through grab follow the new rate at once. rclone batches (a folder's small
    // files) have theirs fixed at start, so running ones restart with the new cap; only their
    // in-flight small files start over.
    limiter_.set_rate(on ? static_cast<std::uint64_t>(mibps * 1024 * 1024) : 0);
    std::lock_guard lock(mutex_);
    for (auto& item : items_) {
        if (item->batch_running) item->batch_stop.request_stop();
    }
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
            if (item->batch_running && limiter_.rate() > 0) item->batch_stop.request_stop();
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
        return set(Status::done, {}, result.note);
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
    std::uint64_t bytes = 0;
    std::uint64_t total = 0;
    double speed = 0;
    for (const auto& c : item.children) {
        if (c.status == Status::skipped) continue;
        total += c.size;
        bytes += c.status == Status::done ? c.size : std::min(c.bytes, c.size);
        if (c.status == Status::running) speed += c.speed;
    }
    item.bytes = bytes;
    item.total = total;
    item.speed = speed;
    item.eta = speed > 0 && total > bytes ? std::optional(static_cast<double>(total - bytes) / speed) : std::nullopt;
}

void App::discard_folder_partials(const Item& item) const {
    const auto dir = folder_dir(item.dest, item.path);
    for (const auto& c : item.children) {
        if (c.big && c.status != Status::done) fetch::discard(dir / util::path_from_utf8(c.path));
    }
    engine::remove_partials(dir, Mode::folder); // rclone's own .partial files
}

// A folder is listed once, then its files are fetched: small ones in one rclone copy batch
// that reuses its connections, big ones with grab's resumable ranged download. The whole
// folder is one user of its server's connection budget.
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
    const auto dir = folder_dir(item->dest, item->path);

    bool listed = false;
    {
        std::lock_guard lock(mutex_);
        listed = item->listed;
    }
    if (!listed && !item_stop.stop_requested()) {
        std::vector<std::string> argv{ctx->config.rclone, "lsjson", "-R", "--files-only",
                                      remote_spec(ctx->settings.rclone_remote, item->path)};
        if (ctx->config.rclone_config) {
            argv.insert(argv.end(), {"--config", util::path_to_utf8(*ctx->config.rclone_config)});
        }
        const auto flags = engine::without_console_progress(ctx->settings.common_flags);
        argv.insert(argv.end(), flags.begin(), flags.end());
        proc::RunOptions quiet;
        quiet.detached = true;
        quiet.stop = item_stop;
        auto run = proc::run_capture(argv, quiet);
        if (!item_stop.stop_requested()) {
            if (!run) return fail(run.error());
            if (run->exit_code != 0) return fail("cannot list the folder: " + server_ops::clean_rclone_error(run->err));
            auto files = folder::parse_listing(run->out);
            if (!files) return fail(files.error());
            {
                std::lock_guard lock(mutex_);
                item->children.clear();
                for (auto& f : *files) {
                    Child c;
                    c.path = std::move(f.path);
                    c.size = f.size;
                    c.modtime = std::move(f.modtime);
                    c.big = folder::is_big(c.size);
                    item->children.push_back(std::move(c));
                }
                item->listed = true;
                folder_totals(*item);
            }
            save_queue();
            post_queue();
        }
    }

    // Files already complete on disk (an earlier run, or downloaded before) are not fetched
    // again; big ones with a partial file continue from it.
    {
        std::lock_guard lock(mutex_);
        for (auto& c : item->children) {
            if (c.status != Status::queued) continue;
            const auto local = dir / util::path_from_utf8(c.path);
            std::error_code ec;
            const bool partial = c.big && std::filesystem::exists(fetch::state_path(local), ec);
            if (!partial && std::filesystem::is_regular_file(local, ec) && std::filesystem::file_size(local, ec) == c.size) {
                c.status = Status::done;
                c.bytes = c.size;
            } else if (partial) {
                if (auto p = fetch::saved_progress(local)) c.bytes = p->first;
            }
        }
        folder_totals(*item);
    }
    post_queue();

    connections_.join(item->remote, item->id, item->weight.load());
    while (!item_stop.stop_requested()) {
        // (not "small": windows.h defines that as a macro)
        std::vector<std::string> small_files;
        bool big_waiting = false;
        {
            std::lock_guard lock(mutex_);
            for (const auto& c : item->children) {
                if (c.status != Status::queued) continue;
                if (c.big) big_waiting = true;
                else small_files.push_back(c.path);
            }
        }
        if (!small_files.empty()) run_small_batch(item, *ctx, small_files, item_stop);
        else if (big_waiting) run_big_files(item, *ctx, item_stop);
        else break;
        save_queue();
    }
    connections_.leave(item->remote, item->id);

    bool cancelled = false;
    {
        std::lock_guard lock(mutex_);
        item->connections = 0;
        for (auto& c : item->children) {
            if (c.status != Status::running) continue;
            c.status = Status::queued; // stopped with the folder: picked up again on resume
            c.speed = 0;
            c.eta.reset();
        }
        folder_totals(*item);
        if (item_stop.stop_requested()) {
            cancelled = item->stop_reason == StopReason::cancel;
            item->status = cancelled ? Status::cancelled : Status::paused;
        } else {
            std::size_t failed = 0;
            std::size_t waiting = 0;
            std::string first_error;
            for (const auto& c : item->children) {
                if (c.status == Status::failed) {
                    if (failed++ == 0) first_error = c.path + ": " + c.error;
                } else if (c.status == Status::paused || c.status == Status::queued) {
                    ++waiting;
                }
            }
            if (failed > 0) {
                item->status = Status::failed;
                item->error = std::format("{} file{} failed. {}", failed, failed == 1 ? "" : "s", first_error);
            } else {
                // Files paused one by one keep the folder paused until they are resumed or skipped.
                item->status = waiting > 0 ? Status::paused : Status::done;
            }
        }
    }
    if (cancelled) {
        std::lock_guard lock(mutex_);
        discard_folder_partials(*item);
    }
}

// The queued small files in one rclone copy: its connection pool is reused for every file,
// where a new SSH login per small file would dominate. Pausing or skipping one of them
// restarts the batch without it.
void App::run_small_batch(const std::shared_ptr<Item>& item, const engine::Context& ctx,
                          const std::vector<std::string>& paths, const std::stop_token& item_stop) {
    const int reserved = connections_.reserve(item->remote, item->id, item_stop);
    if (reserved == 0) return; // stopped while waiting
    const auto list = std::filesystem::temp_directory_path() /
                      std::format("grab-files-{}-{}.txt", GetCurrentProcessId(), item->id);
    (void)server_ops::write_text(list, folder::files_from_text(paths));

    std::unordered_map<std::string, std::size_t> index; // path -> child
    std::stop_source batch;
    // This download's part of the speed limit: its weight over all running downloads'.
    int total_weight = 0;
    {
        std::lock_guard lock(mutex_);
        for (const auto& other : items_) {
            if (other->status == Status::running) total_weight += other->weight.load();
        }
        for (std::size_t i = 0; i < item->children.size(); ++i) index.emplace(item->children[i].path, i);
        for (const auto& p : paths) {
            auto& c = item->children[index[p]];
            if (c.status != Status::queued) continue;
            c.in_batch = true; // stays queued until rclone starts it
            c.bytes = 0;
            c.error.clear();
        }
        item->batch_stop = batch;
        item->batch_running = true;
        item->connections = connections_.active(item->remote, item->id);
    }
    post_queue();
    std::stop_callback forward(item_stop, [&batch] { batch.request_stop(); });
    proc::RunOptions run;
    run.detached = true;
    run.stop = batch.get_token();
    std::vector<std::string> extra{"--files-from-raw", util::path_to_utf8(list), "--transfers",
                                   std::to_string(reserved), "--sftp-connections", std::to_string(reserved), "-v"};
    // rclone writes these files itself, so the speed limit reaches it as --bwlimit; its bytes
    // are also charged to the shared limiter, which slows grab's own streams to match.
    if (const auto limit = limiter_.rate(); limit > 0) {
        const auto weight = static_cast<std::uint64_t>(item->weight.load());
        const auto share = limit * weight / static_cast<std::uint64_t>(std::max<int>(total_weight, static_cast<int>(weight)));
        extra.insert(extra.end(), {"--bwlimit", rate::bwlimit_arg(share)});
    }
    std::uint64_t charged = 0;
    auto result = engine::download(
        ctx, Mode::folder, item->path, item->dest,
        [&](const engine::Progress& p) {
            if (p.bytes > charged) {
                limiter_.debit(p.bytes - charged);
                charged = p.bytes;
            }
            const int conns = connections_.active(item->remote, item->id);
            if (update_item(mutex_, last_progress_post_, [&] {
                    for (const auto& t : p.transferring) {
                        auto it = index.find(t.name);
                        if (it == index.end()) continue;
                        auto& c = item->children[it->second];
                        if (!c.in_batch || (c.status != Status::queued && c.status != Status::running)) continue;
                        c.status = Status::running;
                        c.bytes = t.bytes;
                        c.speed = t.speed;
                        c.eta = t.speed > 0 && t.size > t.bytes ? std::optional(static_cast<double>(t.size - t.bytes) / t.speed)
                                                                : std::nullopt;
                    }
                    item->connections = conns;
                    folder_totals(*item);
                })) {
                post_queue();
            }
        },
        run, extra, {},
        [&](const std::string& name) {
            std::lock_guard lock(mutex_);
            auto it = index.find(name);
            if (it == index.end()) return;
            auto& c = item->children[it->second];
            if (!c.in_batch || (c.status != Status::queued && c.status != Status::running)) return;
            c.status = Status::done;
            c.bytes = c.size;
            c.speed = 0;
            c.eta.reset();
            folder_totals(*item);
        });
    std::error_code ec;
    std::filesystem::remove(list, ec);
    connections_.release(item->remote, item->id, reserved);

    const auto dir = folder_dir(item->dest, item->path);
    std::lock_guard lock(mutex_);
    item->batch_running = false;
    const bool finished = result && result->exit_code == 0;
    for (const auto& p : paths) {
        auto& c = item->children[index[p]];
        const bool was_in_batch = c.in_batch;
        c.in_batch = false;
        if (!was_in_batch || (c.status != Status::running && c.status != Status::queued)) continue; // done, paused, skipped
        c.speed = 0;
        c.eta.reset();
        if (finished) {
            const auto local = dir / util::path_from_utf8(c.path);
            if (std::filesystem::file_size(local, ec) == c.size && !ec) {
                c.status = Status::done;
                c.bytes = c.size;
            } else {
                c.status = Status::failed;
                c.error = "missing after the copy";
            }
        } else if (batch.stop_requested()) {
            c.status = Status::queued; // restarted without a paused/skipped file, or the folder stopped
            c.bytes = 0;               // and rclone starts it over
        } else {
            c.status = Status::failed;
            c.error = !result ? result.error()
                      : result->last_error.empty() ? std::format("rclone exited with code {}", result->exit_code)
                                                   : result->last_error;
        }
    }
    folder_totals(*item);
}

// The queued big files, up to three at a time, each resumable like a single-file download.
// They share the folder's connections (same pool id) and split ranges to keep them busy.
void App::run_big_files(const std::shared_ptr<Item>& item, const engine::Context& ctx,
                        const std::stop_token& item_stop) {
    constexpr int at_once = 3;
    const auto dir = folder_dir(item->dest, item->path);
    auto worker = [&] {
        for (;;) {
            if (item_stop.stop_requested()) return;
            std::size_t i = 0;
            std::stop_source child_stop;
            std::string path;
            {
                std::lock_guard lock(mutex_);
                auto it = std::ranges::find_if(item->children, [](const Child& c) {
                    return c.big && c.status == Status::queued;
                });
                if (it == item->children.end()) return;
                i = static_cast<std::size_t>(it - item->children.begin());
                it->status = Status::running;
                it->error.clear();
                it->stop = child_stop;
                it->stop_reason = StopReason::none;
                path = it->path;
            }
            post_queue();
            std::stop_callback forward(item_stop, [&child_stop] { child_stop.request_stop(); });
            fetch::Download d;
            d.source = fetch::Source{ctx.config.rclone, ctx.config.rclone_config, ctx.settings.rclone_remote,
                                     child_remote_path(item->path, path),
                                     engine::without_console_progress(ctx.settings.common_flags)};
            d.server = item->remote;
            d.id = item->id;
            d.target = dir / util::path_from_utf8(path);
            d.joined = true;
            d.limiter = &limiter_;
            d.weight = &item->weight;
            std::error_code ec;
            std::filesystem::create_directories(d.target.parent_path(), ec);
            const auto result = fetch::run(d, connections_, [&](const fetch::Progress& p) {
                if (update_item(mutex_, last_progress_post_, [&] {
                        auto& c = item->children[i];
                        c.bytes = p.bytes;
                        c.speed = p.speed;
                        c.eta = p.eta;
                        item->connections = p.connections;
                        folder_totals(*item);
                    })) {
                    post_queue();
                }
            }, child_stop.get_token());

            bool discard = false;
            {
                std::lock_guard lock(mutex_);
                auto& c = item->children[i];
                c.speed = 0;
                c.eta.reset();
                switch (result.outcome) {
                case fetch::Outcome::done:
                    c.status = Status::done;
                    c.bytes = c.size;
                    break;
                case fetch::Outcome::failed:
                    c.status = Status::failed;
                    c.error = result.error;
                    break;
                case fetch::Outcome::stopped:
                    c.status = c.stop_reason == StopReason::pause  ? Status::paused
                               : c.stop_reason == StopReason::skip ? Status::skipped
                                                                   : Status::queued; // the folder stopped
                    discard = c.status == Status::skipped;
                    break;
                }
                folder_totals(*item);
            }
            if (discard) fetch::discard(d.target);
            post_queue();
        }
    };
    std::vector<std::jthread> workers;
    for (int k = 0; k < at_once; ++k) workers.emplace_back(worker);
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
        if (i->mode == Mode::folder && i->listed) {
            // Counts for the folder row, and a bounded list of files for the tree: every
            // active one plus the next few queued, so a 5000-file folder stays cheap to send.
            std::vector<folder::State> states;
            states.reserve(i->children.size());
            int counts[7] = {};
            for (const auto& c : i->children) {
                states.push_back(folder_state(static_cast<int>(c.status)));
                ++counts[static_cast<int>(c.status)];
            }
            Value files = Value::make_object();
            files.set("total", num(static_cast<double>(i->children.size())));
            files.set("queued", num(counts[static_cast<int>(Status::queued)]));
            files.set("running", num(counts[static_cast<int>(Status::running)]));
            files.set("paused", num(counts[static_cast<int>(Status::paused)]));
            files.set("done", num(counts[static_cast<int>(Status::done)]));
            files.set("failed", num(counts[static_cast<int>(Status::failed)]));
            files.set("skipped", num(counts[static_cast<int>(Status::skipped)]));
            v.set("files", std::move(files));
            constexpr std::size_t queued_shown = 20;
            Value children = Value::make_array();
            for (const auto k : folder::visible(states, queued_shown)) {
                const auto& c = i->children[k];
                Value cv = Value::make_object();
                cv.set("path", str(c.path));
                cv.set("status", str(status_name(static_cast<int>(c.status))));
                cv.set("bytes", num(static_cast<double>(c.bytes)));
                cv.set("size", num(static_cast<double>(c.size)));
                cv.set("speed", num(c.speed));
                cv.set("eta", c.eta ? num(*c.eta) : Value{});
                cv.set("error", str(c.error));
                children.push(std::move(cv));
            }
            v.set("children", std::move(children));
            const auto queued = static_cast<std::size_t>(counts[static_cast<int>(Status::queued)]);
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
        item->status = Status::paused;
        item->weight = weight_of(saved.priority);
        if (item->mode == Mode::file) {
            if (auto p = fetch::saved_progress(item->dest / util::path_from_utf8(item->name))) {
                item->bytes = p->first;
                item->total = p->second;
            }
        } else if (!saved.files.empty()) {
            const auto dir = folder_dir(item->dest, item->path);
            for (const auto& f : saved.files) {
                Child c;
                c.path = f.path;
                c.size = f.size;
                c.modtime = f.modtime;
                c.big = folder::is_big(f.size);
                c.status = f.state == "done"      ? Status::done
                           : f.state == "skipped" ? Status::skipped
                           : f.state == "paused"  ? Status::paused
                                                  : Status::queued;
                if (c.status == Status::done) c.bytes = c.size;
                else if (c.big) {
                    if (auto p = fetch::saved_progress(dir / util::path_from_utf8(c.path))) c.bytes = p->first;
                }
                item->children.push_back(std::move(c));
            }
            item->listed = true;
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
            for (const auto& c : i->children) {
                const char* state = c.status == Status::done      ? "done"
                                    : c.status == Status::skipped ? "skipped"
                                    : c.status == Status::paused  ? "paused"
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

void App::post_queue() {
    std::string text;
    {
        std::lock_guard lock(mutex_);
        text = json::stringify(queue_snapshot());
    }
    host_.post(std::move(text));
}

} // namespace grab::gui
