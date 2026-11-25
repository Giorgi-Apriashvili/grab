// The window's changes to a server's own files from Browse (SERVER-OPS.md, chunk 2): each server's
// account for the menus (read-only or not), rename, a new folder, paste (a move or copy on the
// server, after a look at the names taken there), delete (after its folders are counted), and
// Properties with its folder size and sums, each on a thread of its own over server_files. Before
// a change the paths a download, an upload or Play uses are refused; after it Browse, the file
// list and the previews forget what it changed.

#include "app.hpp"

#include "browse.hpp"
#include "engine.hpp"
#include "file_list.hpp"
#include "gui_state.hpp"
#include "json.hpp"
#include "rclone_encoding.hpp"
#include "server_files.hpp"
#include "sftp.hpp"
#include "ssh_hash.hpp"
#include "upload.hpp"
#include "upload_existing.hpp"
#include "util.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace grab::gui {

namespace {

using json::Value;
using server_files::Entry;

Value str(std::string s) { return Value::make_string(std::move(s)); }
Value num(double n) { return Value::make_number(n); }

// A copy's, delete's or count's progress goes to the page at most this often.
constexpr auto files_progress_gap = std::chrono::milliseconds(100);
// The entries (skipped, failed) a result carries, at most; it says how many there were.
constexpr std::size_t entries_shown = 50;
// The folders that couldn't be read a count carries, at most.
constexpr std::size_t unreadable_shown = 5;

int int_of(const Value& msg, std::string_view key) {
    const Value* v = msg.find(key);
    return v != nullptr && v->kind == Value::Kind::number ? static_cast<int>(v->number) : 0;
}

bool bool_of(const Value& msg, std::string_view key) {
    const Value* v = msg.find(key);
    return v != nullptr && v->kind == Value::Kind::boolean && v->boolean;
}

// The strings of the array `key` (the page's paths); anything else in it left out.
std::vector<std::string> strings_of(const Value& msg, std::string_view key) {
    std::vector<std::string> out;
    const Value* v = msg.find(key);
    if (v == nullptr || v->kind != Value::Kind::array) return out;
    for (const Value& s : v->items) {
        if (s.kind == Value::Kind::string) out.push_back(s.str);
    }
    return out;
}

std::int64_t unix_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// `name` (grab's form) in the folder `dir` (grab's form; "" the login folder), as server_files
// names it.
std::string child_of(std::string_view dir, std::string_view name) {
    if (dir.empty()) return std::string(name);
    return std::format("{}{}{}", dir, dir.ends_with('/') ? "" : "/", name);
}

// "holiday", or "3 items": what a change of `paths` is called while it runs.
std::string what_of(std::string_view doing, const std::vector<std::string>& paths) {
    if (paths.size() == 1) return std::format("{} {}", doing, upload::split_remote(paths.front()).second);
    return std::format("{} {} items", doing, paths.size());
}

// A task's answer: {type: filesDone, token, op}.
Value done_of(int token, std::string_view op) {
    Value v = Value::make_object();
    v.set("type", str("filesDone"));
    v.set("token", num(token));
    v.set("op", str(std::string(op)));
    return v;
}

Value progress_of(int token, const server_files::Progress& p) {
    Value v = Value::make_object();
    v.set("type", str("filesProgress"));
    v.set("token", num(token));
    v.set("bytes", num(static_cast<double>(p.bytes)));
    v.set("totalBytes", num(static_cast<double>(p.total_bytes)));
    v.set("files", num(static_cast<double>(p.files)));
    v.set("totalFiles", num(static_cast<double>(p.total_files)));
    return v;
}

std::string_view state_name(Entry::State s) {
    switch (s) {
    case Entry::State::skipped: return "skipped";
    case Entry::State::failed: return "failed";
    case Entry::State::asking: return "asking";
    }
    return "failed";
}

// What a move, copy or delete did: counts, its new paths, the first entries, and why it ended
// early (`stopped`, `error`).
void set_report(Value& v, const server_files::Report& r) {
    v.set("ok", Value::make_bool(!r.stopped && r.error.empty()));
    v.set("files", num(static_cast<double>(r.files)));
    v.set("folders", num(static_cast<double>(r.folders)));
    v.set("bytes", num(static_cast<double>(r.bytes)));
    v.set("left", num(static_cast<double>(r.left)));
    v.set("stopped", Value::make_bool(r.stopped));
    if (!r.error.empty()) v.set("error", str(r.error));
    Value placed = Value::make_array();
    for (const auto& p : r.placed) placed.push(str(p));
    v.set("placed", std::move(placed));
    Value entries = Value::make_array();
    std::size_t skipped = 0;
    std::size_t failed = 0;
    for (const Entry& e : r.entries) {
        (e.state == Entry::State::failed ? failed : skipped) += 1;
        if (entries.items.size() == entries_shown) continue;
        Value it = Value::make_object();
        it.set("state", str(std::string(state_name(e.state))));
        it.set("from", str(e.from));
        it.set("to", str(e.to));
        it.set("why", str(e.why));
        entries.push(std::move(it));
    }
    v.set("entries", std::move(entries));
    v.set("skipped", num(static_cast<double>(skipped)));
    v.set("failed", num(static_cast<double>(failed)));
}

// The names taken where a paste goes, counted by how they compare.
Value taken_of(const upload::Taken& t) {
    Value v = Value::make_object();
    v.set("count", num(static_cast<double>(t.count)));
    v.set("same", num(static_cast<double>(t.same)));
    v.set("newer", num(static_cast<double>(t.newer)));
    v.set("older", num(static_cast<double>(t.older)));
    v.set("differs", num(static_cast<double>(t.differs)));
    v.set("folders", num(static_cast<double>(t.folders)));
    return v;
}

// A name taken where a paste goes, as the question lists it (the uploads' clash, "mine" the one
// moved or copied): its name from the folder pasted into, its paths, both sizes and times, and how
// they compare.
Value clash_of(const Entry& e, std::string_view dir) {
    const upload::Clash& c = *e.clash;
    const std::string base = dir.empty() ? std::string() : std::format("{}{}", dir, dir.ends_with('/') ? "" : "/");
    Value f = Value::make_object();
    f.set("name", str(!base.empty() && e.to.starts_with(base) ? e.to.substr(base.size()) : e.to));
    f.set("from", str(e.from));
    f.set("to", str(e.to));
    f.set("size", num(static_cast<double>(c.remote_size)));
    f.set("modified", c.remote_mtime ? num(static_cast<double>(*c.remote_mtime)) : Value{});
    f.set("mineSize", num(static_cast<double>(c.local_size)));
    f.set("mineModified", num(static_cast<double>(c.local_mtime)));
    f.set("kind", str(std::string(upload::kind_name(c))));
    return f;
}

// A file's or folder's attributes for Properties.
std::string_view kind_name(const sftp::Attrs& a) {
    return a.is_dir() ? "folder" : a.is_file() ? "file" : a.is_link() ? "link" : "other";
}

void set_attrs(Value& v, const sftp::Attrs& a) {
    v.set("kind", str(std::string(kind_name(a))));
    if (a.size && !a.is_dir()) v.set("size", num(static_cast<double>(*a.size)));
    if (a.mtime) v.set("modified", num(static_cast<double>(*a.mtime)));
    if (a.atime) v.set("accessed", num(static_cast<double>(*a.atime)));
    if (a.permissions) v.set("mode", num(static_cast<double>(*a.permissions & 07777)));
}

} // namespace

// ---- the tasks ------------------------------------------------------------------------------------------

void App::start_files_task(int token, bool change, std::string what, std::function<void(const std::stop_token&)> run) {
    auto t = std::make_shared<FilesTask>();
    t->token = token;
    t->change = change;
    t->what = std::move(what);
    std::string running;
    {
        std::scoped_lock lock(files_mutex_);
        std::erase_if(files_tasks_, [](const auto& x) { return x->over.load(); });
        if (change) {
            const auto it = std::ranges::find_if(files_tasks_, [](const auto& x) { return x->change && !x->answered; });
            if (it != files_tasks_.end()) running = (*it)->what;
        }
        if (running.empty()) {
            // The thread holds no share of its task: files_tasks_ keeps it until the thread is joined.
            FilesTask* const raw = t.get();
            files_tasks_.push_back(t);
            t->thread = std::jthread([this, raw, run = std::move(run)](const std::stop_token& stop) {
                run(stop);
                // A change's end, for the tray: read before `over`, after which the task may go (its
                // erase joins this thread under files_mutex_).
                std::optional<std::pair<bool, std::string>> ended;
                if (raw->change) {
                    std::scoped_lock done_lock(files_mutex_);
                    ended.emplace(raw->ok, raw->said);
                }
                const std::string what = raw->what;
                raw->over = true;
                if (ended && !files_closing_ && host_.on_change_ended) host_.on_change_ended(what, ended->first, ended->second);
            });
            return;
        }
    }
    // One change at a time in the window (SERVER-OPS.md, chunk 2): the second is refused, plainly.
    Value v = done_of(token, "refused");
    v.set("busy", Value::make_bool(true));
    v.set("error", str(std::format("{} is still going: wait until it ends, or cancel it first", running)));
    post(v);
}

void App::files_answered(int token, const Value& v) {
    // In a few words: "Done", "Done; 2 skipped, 1 failed", the error, the name taken, "Cancelled".
    const auto error = v.string_of("error");
    const auto taken = v.string_of("taken");
    const bool stopped = bool_of(v, "stopped");
    const int skipped = int_of(v, "skipped");
    const int failed = int_of(v, "failed");
    std::string said;
    if (error) said = *error;
    else if (taken) said = *taken;
    else if (stopped) said = "Cancelled";
    else {
        said = "Done";
        std::vector<std::string> counts;
        if (skipped > 0) counts.push_back(std::format("{} skipped", skipped));
        if (failed > 0) counts.push_back(std::format("{} failed", failed));
        if (!counts.empty()) said += "; " + util::join(counts, ", ");
    }
    std::scoped_lock lock(files_mutex_);
    for (const auto& t : files_tasks_) {
        if (!t->change || t->token != token) continue;
        t->answered = true;
        t->ok = !error && !taken && !stopped && failed == 0;
        t->said = said;
    }
}

void App::files_copying(int token, std::chrono::milliseconds timeout) {
    std::scoped_lock lock(files_mutex_);
    for (const auto& t : files_tasks_) {
        if (t->token != token) continue;
        t->copying = true;
        t->timeout = timeout;
    }
}

std::string App::running_change() const {
    std::scoped_lock lock(files_mutex_);
    for (const auto& t : files_tasks_) {
        if (t->change && !t->over) return t->what;
    }
    return {};
}

void App::files_undo(int token, const std::string& server, const server_files::Undo& u) {
    {
        std::lock_guard lock(mutex_);
        auto& r = copy_undo_[token];
        r.server = server;
        auto& paths = u.kind == server_files::Undo::Kind::temp ? r.temps : r.folders;
        if (u.made) paths.insert(u.path);
        else paths.erase(u.path);
    }
    // Saved soon, so even a grab killed meanwhile leaves the next start what to undo.
    if (u.made) request_save();
}

void App::files_undo_over(int token) {
    std::string server;
    bool left = false;
    {
        std::lock_guard lock(mutex_);
        const auto it = copy_undo_.find(token);
        if (it == copy_undo_.end()) return;
        server = it->second.server;
        const auto add = [&](std::string_view kind, const std::set<std::string>& paths) {
            for (const auto& p : paths) {
                SavedLeftover l{.server = server, .kind = std::string(kind), .path = p};
                if (std::ranges::find(leftovers_, l) == leftovers_.end()) leftovers_.push_back(std::move(l));
                left = true;
            }
        };
        add("temp", it->second.temps);
        add("folder", it->second.folders);
        copy_undo_.erase(it);
    }
    // At once: an empty folder a finished copy made is its own, and mustn't look like a leftover.
    request_save(true);
    // The cleaner tries them now; not while the window closes (its members may be going: they
    // wait in uploads.json for the next start).
    if (left && !files_closing_) wake_cleaner(server);
}

std::expected<App::FilesServer, std::string> App::files_server(const std::string& server) {
    auto ctx = engine::load_context(config_path_, server);
    if (!ctx) return util::fail(ctx.error());
    auto target = engine::ssh_target(*ctx);
    if (!target) return util::fail(target.error()); // it names the server
    connections_.configure(server, connection_budget(server, ctx->remote.host, ctx->settings.max_connections));
    return FilesServer{.where = server_files::Where{.server = server,
                                                    .pool = pools_.get(server, *target, connections_.budget(server)),
                                                    .connections = &connections_,
                                                    .timeout = target->timeout,
                                                    .on_command = {},
                                                    .home = {},
                                                    .on_undo = {}},
                       .remote = ctx->remote};
}

std::expected<server_files::Account, std::string> App::files_account(const std::string& server, const FilesServer& fs,
                                                                    const std::stop_token& stop, bool again) {
    if (!again) {
        std::scoped_lock lock(files_mutex_);
        if (const auto it = accounts_.find(server); it != accounts_.end()) return it->second;
    }
    auto a = server_files::account(fs.where, stop);
    if (stop.stop_requested()) return a;
    if (a) {
        std::scoped_lock lock(files_mutex_);
        accounts_[server] = *a;
    }
    post_account(server, a);
    return a;
}

void App::post_account(const std::string& server, const std::expected<server_files::Account, std::string>& account) const {
    Value v = Value::make_object();
    v.set("type", str("filesAccount"));
    v.set("server", str(server));
    if (!account) {
        v.set("error", str(account.error()));
    } else {
        v.set("readOnly", Value::make_bool(account->read_only));
        if (account->free) v.set("free", num(static_cast<double>(*account->free)));
        // The login folder's real path (the menus grey it however it is spelled), and whether a
        // copy can be pasted there.
        v.set("home", str(account->home));
        v.set("copyData", Value::make_bool(account->copy_data));
    }
    post(v);
}

std::optional<std::string> App::files_busy(const FilesServer& fs, const std::string& home, const std::vector<std::string>& paths,
                                           std::vector<std::string>& resolved_paths, const std::stop_token& stop) {
    using server_files::Busy;
    const std::string& server = fs.where.server;
    std::vector<Busy> busy;
    {
        // Every download and upload of the server not over: running, queued, or paused (a resume
        // takes its path up again).
        std::lock_guard lock(mutex_);
        for (const auto& item : items_) {
            if (item->remote != server) continue;
            if (item->status != Status::queued && item->status != Status::running && item->status != Status::paused) continue;
            busy.push_back(Busy{.kind = is_up(*item) ? Busy::Kind::upload : Busy::Kind::download, .path = item->path});
        }
    }
    for (const auto& p : play_server_.playing()) {
        if (p.server == server) busy.push_back(Busy{.kind = Busy::Kind::play, .path = rclone_enc::from_raw_path(p.path)});
    }
    {
        std::scoped_lock lock(play_mutex_);
        if (play_job_ && play_job_->remote == server) busy.push_back(Busy{.kind = Busy::Kind::play, .path = play_job_->path});
    }
    // Their folders' real paths too, the change's and the busy ones' in one go: a path through a
    // link to a folder meets the same place spelled without it.
    std::vector<std::string> all = paths;
    for (const Busy& b : busy) all.push_back(b.path);
    const auto real = server_files::resolved(fs.where, all, stop);
    resolved_paths.assign(real.begin(), real.begin() + static_cast<std::ptrdiff_t>(paths.size()));
    if (busy.empty()) return std::nullopt;
    std::vector<std::string> mine = paths;
    for (const auto& r : resolved_paths) {
        if (std::ranges::find(mine, r) == mine.end()) mine.push_back(r);
    }
    // A busy path's own link followed as well (a folder download of a link to a folder, Play of a
    // link to a file): a change in the place it points to meets it. The change's paths keep their
    // last name, so a link is still removed or renamed as itself.
    const std::vector<std::string> busy_given(all.begin() + static_cast<std::ptrdiff_t>(paths.size()), all.end());
    const auto whole = server_files::followed(fs.where, busy_given, stop);
    const std::size_t given = busy.size();
    for (std::size_t i = 0; i < given; ++i) {
        for (const std::string& r : {real[paths.size() + i], whole[i]}) {
            const bool known = std::ranges::any_of(busy, [&](const Busy& b) { return b.kind == busy[i].kind && b.path == r; });
            if (!known) busy.push_back(Busy{.kind = busy[i].kind, .path = r});
        }
    }
    return server_files::busy_refusal(busy, mine, server, home);
}

void App::files_changed(const std::string& server, const std::string& home, const server_files::Changed& changed,
                        const std::vector<std::string>& went) {
    if (files_closing_) return; // the window is closing: no list refresh, no Browse
    const auto inv = server_files::invalidation(server, changed, home);
    const std::int64_t now = unix_seconds();
    bool shown = false;
    {
        // Browse: this session's live listings of them go, and the file list's are older now
        // (changed_at): each is listed live again when shown.
        std::lock_guard lock(browse_mutex_);
        for (const auto& m : inv.folders) {
            std::erase_if(live_folders_, [&](const auto& kv) {
                const auto& [s, f] = kv.first;
                return s == server && (f == m.folder || (m.below && browse::is_below(f, m.folder)));
            });
            auto& c = changed_folders_[{server, m.folder}];
            c.ended = std::max(c.ended, now);
            c.below = c.below || m.below;
            const std::string& at = shown_browse_.path;
            if (shown_browse_.remote == server && (at == m.folder || (m.below && browse::is_below(at, m.folder)))) shown = true;
        }
    }
    for (const auto& p : inv.forget) preview_jobs_.forget(server, p);
    // The file list: what surely went leaves it now (a search right after finds none of it), and
    // the list is fetched again for the rest (once more after a fetch already running).
    const auto gone = server_files::invalidation(server, {.from = went, .to = {}}, home).forget;
    std::shared_ptr<const file_list::List> list;
    {
        std::lock_guard lock(lists_mutex_);
        list = list_slot(server).list;
    }
    if (list && !gone.empty()) {
        const std::set<std::string, std::less<>> drop(gone.begin(), gone.end());
        const auto dropped = [&](std::string_view p) {
            for (;;) {
                if (drop.contains(p)) return true;
                const auto slash = p.rfind('/');
                if (slash == std::string_view::npos || slash == 0) return false;
                p = p.substr(0, slash);
            }
        };
        // Copied only when something in it goes: a big list is walked, not copied, for nothing.
        std::shared_ptr<file_list::List> kept;
        if (std::ranges::any_of(list->entries, [&](const file_list::Entry& e) { return dropped(e.path); })) {
            kept = std::make_shared<file_list::List>(*list);
            std::erase_if(kept->entries, [&](const file_list::Entry& e) { return dropped(e.path); });
        }
        if (kept) {
            std::lock_guard lock(lists_mutex_);
            auto& slot = list_slot(server);
            if (slot.list == list) {
                slot.list = std::move(kept);
                slot.index.reset(); // built again from it on the next browse
            }
        }
    }
    bool refreshing = false;
    {
        std::lock_guard lock(lists_mutex_);
        auto& slot = list_slot(server);
        refreshing = slot.refreshing;
        if (refreshing) slot.again = true;
    }
    if (!refreshing) start_refresh(server, true);
    request_save(); // the marks are kept in uploads.json
    if (shown) host_.defer([this] { refresh_browse(); });
}

// ---- the page's messages ----------------------------------------------------------------------------------

void App::files_message(const std::string& type, const Value& msg) {
    const int token = int_of(msg, "token");
    const std::string server = msg.string_of("server").value_or("");
    if (type == "filesCancel") {
        std::scoped_lock lock(files_mutex_);
        for (const auto& t : files_tasks_) {
            if (t->token == token && token > 0) t->thread.request_stop();
        }
        return;
    }
    if (server.empty()) return;
    if (type == "filesAccount") {
        // Browse opened the server (or Refresh, `again`): read-only or not, for the menus.
        const bool again = bool_of(msg, "again");
        std::optional<server_files::Account> known;
        {
            std::scoped_lock lock(files_mutex_);
            if (const auto it = accounts_.find(server); it != accounts_.end() && !again) known = it->second;
            else if (!accounts_asking_.insert(server).second) return; // being asked: its answer comes
        }
        if (known) return post_account(server, *known);
        start_files_task(0, false, {}, [this, server](const std::stop_token& stop) {
            if (auto fs = files_server(server)) {
                [[maybe_unused]] auto asked = files_account(server, *fs, stop, true);
            } else {
                post_account(server, std::unexpected(fs.error()));
            }
            std::scoped_lock lock(files_mutex_);
            accounts_asking_.erase(server);
        });
        return;
    }
    if (token <= 0) return;

    // Each task below: the server's connections, its login folder (for the comparisons), and
    // its answer posted; a stop answers nothing more than "stopped".
    struct Begun {
        FilesServer fs;
        std::string home;
    };
    const auto begin = [this, server, token](Value& v, const std::stop_token& stop) -> std::optional<Begun> {
        auto fs = files_server(server);
        if (!fs) {
            v.set("error", str(fs.error()));
            files_answered(token, v);
            post(v);
            return std::nullopt;
        }
        // Not known (the account couldn't be asked): only the spellings given compare, and each
        // change asks the server for the login folder itself.
        auto account = files_account(server, *fs, stop, false);
        std::string home = account ? account->home : std::string();
        fs->where.home = home;
        return Begun{.fs = std::move(*fs), .home = std::move(home)};
    };
    const auto fail = [this, token](Value& v, const std::string& why) {
        v.set("error", str(why));
        files_answered(token, v);
        post(v);
    };

    if (type == "filesRename" || type == "filesMkdir") {
        // {path, name} renamed; {dir, name} a new folder: "taken" keeps the dialog's name box open.
        const bool rename = type == "filesRename";
        const std::string path = msg.string_of(rename ? "path" : "dir").value_or("");
        const std::string name = msg.string_of("name").value_or("");
        const std::string what = rename ? std::format("Renaming {}", upload::split_remote(path).second)
                                        : std::format("Making the folder {}", name);
        start_files_task(token, true, what, [=, this](const std::stop_token& stop) {
            Value v = done_of(token, rename ? "rename" : "mkdir");
            const auto b = begin(v, stop);
            if (!b) return;
            const std::string to = child_of(rename ? upload::split_remote(path).first : path, name);
            std::vector<std::string> touched = rename ? std::vector<std::string>{path, to} : std::vector<std::string>{to};
            std::vector<std::string> real;
            if (auto why = files_busy(b->fs, b->home, touched, real, stop)) return fail(v, *why);
            auto r = rename ? server_files::rename(b->fs.where, path, name, stop) : server_files::mkdir(b->fs.where, path, name, stop);
            if (!r) return fail(v, r.error());
            if (r->taken) {
                v.set("taken", str(r->why));
                files_answered(token, v);
                return post(v);
            }
            v.set("ok", Value::make_bool(true));
            v.set("path", str(r->path));
            files_answered(token, v);
            post(v);
            if (rename && r->path == path) return; // the same name: nothing changed
            server_files::Changed changed;
            if (rename) changed.from = {path, real[0]};
            changed.to = {r->path, real.back()};
            files_changed(server, b->home, changed, changed.from);
        });
    } else if (type == "filesLook" || type == "filesPaste") {
        // {op: copy | move, paths, dir}: each path into the folder `dir` under its own name. A look
        // answers what goes and the names taken there (the question's list); a paste runs it with
        // the answer: `choice` for all, `own` [{from, to, choice}] for the files the question
        // listed. Nothing is asked at the paste: a name taken since is skipped unless chosen so.
        const bool copying = msg.string_of("op") == "copy";
        const bool paste = type == "filesPaste";
        const auto paths = strings_of(msg, "paths");
        const std::string dir = msg.string_of("dir").value_or("");
        const auto existing = upload::parse_existing(msg.string_of("choice").value_or("")).value_or(upload::Existing::skip);
        std::map<std::pair<std::string, std::string>, upload::Existing> own;
        if (const Value* o = msg.find("own"); o != nullptr && o->kind == Value::Kind::array) {
            for (const Value& e : o->items) {
                if (e.kind != Value::Kind::object) continue;
                const auto c = upload::parse_existing(e.string_of("choice").value_or(""));
                if (!c || *c == upload::Existing::ask) continue;
                own[{e.string_of("from").value_or(""), e.string_of("to").value_or("")}] = *c;
            }
        }
        if (paths.empty()) return;
        const std::string what = what_of(paste ? (copying ? "Copying" : "Moving") : "Looking at", paths);
        start_files_task(token, paste, what, [=, this](const std::stop_token& stop) {
            Value v = done_of(token, paste ? (copying ? "copy" : "move") : "look");
            const auto b = begin(v, stop);
            if (!b) return;
            server_files::Where where = b->fs.where;
            // A copy's temps and folders kept as it goes, so one cut off midway is undone later.
            if (paste && copying) where.on_undo = [this, token, server](const server_files::Undo& u) { files_undo(token, server, u); };
            const auto items = server_files::into(paths, dir);
            std::vector<std::string> touched = paths;
            for (const auto& it : items) touched.push_back(it.to);
            std::vector<std::string> real;
            if (auto why = files_busy(b->fs, b->home, touched, real, stop)) return fail(v, *why);
            if (!paste) {
                auto l = server_files::look(where, items, copying, stop);
                if (stop.stop_requested()) { // Cancel: the footer's line waits for the word
                    v.set("stopped", Value::make_bool(true));
                    return post(v);
                }
                if (!l) return fail(v, l.error());
                v.set("files", num(static_cast<double>(l->files)));
                v.set("folders", num(static_cast<double>(l->folders)));
                v.set("bytes", num(static_cast<double>(l->bytes)));
                Value taken = taken_of(l->taken);
                // Of them, the folders onto a file of their name: only Keep both puts them anywhere.
                taken.set("foldersOntoFiles", taken_of(l->folders_onto_files));
                Value files = Value::make_array();
                for (const Entry& e : l->clashes) {
                    if (e.clash) files.push(clash_of(e, dir));
                }
                taken.set("files", std::move(files));
                v.set("existing", std::move(taken));
                return post(v);
            }
            auto shown = std::chrono::steady_clock::time_point{};
            const server_files::OnProgress progress = [&](const server_files::Progress& p) {
                const auto at = std::chrono::steady_clock::now();
                if (at - shown < files_progress_gap) return;
                shown = at;
                post(progress_of(token, p));
            };
            // Quitting from here on waits for the copy-data piece a copy is in (App::close).
            if (copying) files_copying(token, where.timeout);
            // What an earlier copy left there, being removed by the cleaner now: after it, as an
            // upload waits, so nothing pasted is what it removes.
            wait_for_cleaner(server, stop);
            const auto run = [&](std::span<const server_files::Item> some, upload::Existing choice) {
                return copying ? server_files::copy(where, some, choice, progress, stop) : server_files::move(where, some, choice, stop);
            };
            // The files the question listed with a choice of their own: the rest first, each name
            // taken left as it is (Ask); then those, each with its own choice or the one for all.
            auto r = run(items, own.empty() ? existing : upload::Existing::ask);
            std::vector<std::string> placed = r.placed;
            // The paths that left their places (a move's: search results drop them at once).
            std::vector<std::string> moved;
            const auto went = [&](std::span<const server_files::Item> some, const std::vector<std::string>& to) {
                for (std::size_t i = 0; i < some.size() && i < to.size(); ++i) {
                    if (!to[i].empty() && to[i] != some[i].from) moved.push_back(some[i].from);
                }
            };
            went(items, r.placed);
            if (!own.empty()) {
                std::vector<server_files::Item> again;
                std::vector<Entry> rest;
                for (auto& e : r.entries) {
                    if (e.state != Entry::State::asking) {
                        rest.push_back(std::move(e));
                        continue;
                    }
                    const auto it = own.find({e.from, e.to});
                    again.push_back(server_files::Item{.from = e.from, .to = e.to, .choice = it != own.end() ? it->second : existing});
                }
                r.entries = std::move(rest);
                if (!again.empty() && !r.stopped && r.error.empty()) {
                    auto more = run(again, existing);
                    r.files += more.files;
                    r.folders += more.folders;
                    r.bytes += more.bytes;
                    std::ranges::move(more.entries, std::back_inserter(r.entries));
                    std::ranges::copy(more.placed, std::back_inserter(placed));
                    went(again, more.placed);
                    r.stopped = more.stopped;
                    r.error = more.error;
                }
            }
            // What the copy couldn't undo (its connection lost twice) to the leftovers.
            if (copying) files_undo_over(token);
            set_report(v, r);
            if (!copying) {
                Value gone = Value::make_array();
                for (const auto& m : moved) gone.push(str(m));
                v.set("moved", std::move(gone));
            }
            files_answered(token, v);
            post(v);
            // What it changed: the moved ones' old places, every new one (a stopped copy's
            // folders too: Browse lists them live either way). Only those that moved leave the
            // file list at once, as given and resolved.
            server_files::Changed changed;
            std::vector<std::string> went_away;
            if (!copying) {
                changed.from = paths;
                std::ranges::copy(real.begin(), real.begin() + static_cast<std::ptrdiff_t>(paths.size()), std::back_inserter(changed.from));
                for (std::size_t i = 0; i < paths.size(); ++i) {
                    if (std::ranges::find(moved, paths[i]) == moved.end()) continue;
                    went_away.push_back(paths[i]);
                    went_away.push_back(real[i]);
                }
            }
            for (std::size_t i = 0; i < items.size(); ++i) {
                changed.to.push_back(items[i].to);
                changed.to.push_back(real[paths.size() + i]);
            }
            for (const auto& p : placed) {
                if (!p.empty()) changed.to.push_back(p);
            }
            files_changed(server, b->home, changed, went_away);
        });
    } else if (type == "filesSize") {
        // {paths, check}: their files, folders, links and bytes (the delete question's counts, with
        // `check` the busy paths refused first; Properties' Folder size).
        const auto paths = strings_of(msg, "paths");
        const bool check = bool_of(msg, "check");
        if (paths.empty()) return;
        start_files_task(token, false, what_of("Counting", paths), [=, this](const std::stop_token& stop) {
            Value v = done_of(token, "size");
            const auto b = begin(v, stop);
            if (!b) return;
            std::vector<std::string> real;
            if (check) {
                if (auto why = files_busy(b->fs, b->home, paths, real, stop)) return fail(v, *why);
            }
            auto shown = std::chrono::steady_clock::time_point{};
            auto s = server_files::folder_size(b->fs.where, paths, [&](const server_files::Progress& p) {
                const auto at = std::chrono::steady_clock::now();
                if (at - shown < files_progress_gap) return;
                shown = at;
                post(progress_of(token, p));
            }, stop);
            if (stop.stop_requested()) return;
            if (!s) return fail(v, s.error());
            v.set("files", num(static_cast<double>(s->files)));
            v.set("folders", num(static_cast<double>(s->folders)));
            v.set("links", num(static_cast<double>(s->links)));
            v.set("bytes", num(static_cast<double>(s->bytes)));
            Value unreadable = Value::make_array();
            for (std::size_t i = 0; i < s->unreadable.size() && i < unreadable_shown; ++i) unreadable.push(str(s->unreadable[i]));
            v.set("unreadable", std::move(unreadable));
            v.set("unreadableCount", num(static_cast<double>(s->unreadable.size())));
            post(v);
        });
    } else if (type == "filesDelete") {
        // {paths}: deleted for good, after the page's confirmation.
        const auto paths = strings_of(msg, "paths");
        if (paths.empty()) return;
        start_files_task(token, true, what_of("Deleting", paths), [=, this](const std::stop_token& stop) {
            Value v = done_of(token, "delete");
            const auto b = begin(v, stop);
            if (!b) return;
            std::vector<std::string> real;
            if (auto why = files_busy(b->fs, b->home, paths, real, stop)) return fail(v, *why);
            auto shown = std::chrono::steady_clock::time_point{};
            const auto r = server_files::remove(b->fs.where, paths, [&](const server_files::Progress& p) {
                const auto at = std::chrono::steady_clock::now();
                if (at - shown < files_progress_gap) return;
                shown = at;
                post(progress_of(token, p));
            }, stop);
            set_report(v, r);
            files_answered(token, v);
            post(v);
            server_files::Changed changed;
            changed.from = paths;
            std::ranges::copy(real, std::back_inserter(changed.from));
            // A delete that ended early left some of them: the file list waits for its fetch.
            const bool all_went = r.error.empty() && !r.stopped;
            files_changed(server, b->home, changed, all_went ? changed.from : std::vector<std::string>{});
        });
    } else if (type == "filesProps") {
        // {path}: what Properties shows.
        const std::string path = msg.string_of("path").value_or("");
        start_files_task(token, false, {}, [=, this](const std::stop_token& stop) {
            Value v = done_of(token, "props");
            const auto b = begin(v, stop);
            if (!b) return;
            auto p = server_files::properties(b->fs.where, path, stop);
            if (stop.stop_requested()) return;
            if (!p) return fail(v, p.error());
            set_attrs(v, p->attrs);
            if (p->kind == server_files::Properties::Kind::link) {
                v.set("link", str(p->link));
                if (p->target) {
                    Value t = Value::make_object();
                    set_attrs(t, *p->target);
                    v.set("target", std::move(t));
                }
            }
            if (!p->owner.empty()) v.set("owner", str(p->owner));
            if (!p->group.empty()) v.set("group", str(p->group));
            if (p->free) v.set("free", num(static_cast<double>(*p->free)));
            v.set("readOnly", Value::make_bool(p->read_only));
            post(v);
        });
    } else if (type == "filesHash") {
        // {path, size, sum: md5 | sha256}: the server's sum of the file (md5sum, sha256sum).
        const std::string path = msg.string_of("path").value_or("");
        const Value* size = msg.find("size");
        const std::uint64_t bytes =
            size != nullptr && size->kind == Value::Kind::number && size->number >= 0 ? static_cast<std::uint64_t>(size->number) : 0;
        const bool sha = msg.string_of("sum") == "sha256";
        start_files_task(token, false, {}, [=, this](const std::stop_token& stop) {
            Value v = done_of(token, "hash");
            v.set("sum", str(sha ? "sha256" : "md5"));
            const auto b = begin(v, stop);
            if (!b) return;
            auto h = server_files::hash(b->fs.where, b->fs.remote, path, bytes, sha ? ssh::Sum::sha256 : ssh::Sum::md5, stop);
            if (stop.stop_requested()) return;
            if (!h) return fail(v, h.error());
            v.set("hash", str(*h));
            post(v);
        });
    }
}

} // namespace grab::gui
