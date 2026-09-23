#include "app.hpp"

#include "config.hpp"
#include "engine.hpp"
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
#include <memory>
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

const char* auth_name(servers::Auth a) {
    switch (a) {
    case servers::Auth::key_file: return "key";
    case servers::Auth::agent: return "agent";
    case servers::Auth::password: break;
    }
    return "password";
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
    if (settings_thread_.joinable()) settings_thread_.request_stop();
    cancel_pending();
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

App::QueueSummary App::summary() const {
    QueueSummary s;
    std::lock_guard lock(mutex_);
    for (const auto& i : items_) {
        if (i->status == Status::running) ++s.running;
        else if (i->status == Status::queued) ++s.queued;
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
        cancel_item(int_of(*msg, "id"));
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
