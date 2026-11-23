// The window's uploads (UPLOADS.md 5.10, chunk 3): the pickers and the check before an upload,
// the answer about the files already there (chunk 5: For all, single files' own, the setting),
// uploads as rows of the queue beside the downloads (sharing their ids, order, At once and each
// server's connection budget), the question when files turn up there later, what a cancel leaves
// on a server and its removal, and Browse showing a folder again once an upload changed it.

#include "app.hpp"

#include "browse.hpp"
#include "engine.hpp"
#include "existing.hpp"
#include "fetch.hpp"
#include "folder.hpp"
#include "folder_job.hpp"
#include "gui_state.hpp"
#include "json.hpp"
#include "local_name.hpp"
#include "queue_row.hpp"
#include "queue_view.hpp"
#include "rclone_encoding.hpp"
#include "upload.hpp"
#include "upload_check.hpp"
#include "upload_existing.hpp"
#include "upload_folder.hpp"
#include "util.hpp"
#include "verify.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace grab::gui {

namespace {

using json::Value;

Value str(std::string s) { return Value::make_string(std::move(s)); }
Value num(double n) { return Value::make_number(n); }

// The check's picked items the dialog lists, at most.
constexpr std::size_t picked_shown = 50;
// The files already on the server the dialog lists, each with a choice of its own, at most; the
// others follow For all (UPLOADS.md 5.10). A list the page draws at once.
constexpr std::size_t clashes_shown = 200;
// An upload waiting with the question about files that turned up there: those its row carries, at
// most (the row goes to the page whenever it changes).
constexpr std::size_t late_shown = 50;
// The check's progress while it walks the picked folders, at most this often.
constexpr auto check_progress_gap = std::chrono::milliseconds(150);

std::int64_t unix_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// "2023-05-06T11:18:12.123456789Z": a local file's time, as verify compares it.
std::string rfc3339_ns(std::int64_t ns) {
    return std::format("{:%FT%TZ}", std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{ns}});
}

// A file already on the server, as the question lists it: the server's size and time, yours, and
// how they compare.
Value clash_json(const std::string& name, const upload::Clash& c) {
    Value f = Value::make_object();
    f.set("name", str(name));
    f.set("size", num(static_cast<double>(c.remote_size)));
    f.set("modified", c.remote_mtime ? num(static_cast<double>(*c.remote_mtime)) : Value{});
    f.set("mineSize", num(static_cast<double>(c.local_size)));
    f.set("mineModified", num(static_cast<double>(c.local_mtime)));
    f.set("kind", str(std::string(upload::kind_name(c))));
    return f;
}

// Tests only (UPLOADS.md chunk 3; to be removed before 0.24.0, step 7.5): with
// GRAB_TEST_PICKS=<file>, the upload's file dialog isn't shown; its answer is the file's lines (one
// path each, UTF-8; none: cancelled), so a window driven over DevTools can upload without a native
// dialog in the way. What was asked goes to <file>.asked ("files|folders <title>").
std::optional<std::vector<std::filesystem::path>> test_picks(bool folders, const std::string& title) {
    const auto file = util::getenv_utf8("GRAB_TEST_PICKS");
    if (!file || file->empty()) return std::nullopt;
    const auto path = util::path_from_utf8(*file);
    {
        std::ofstream asked(util::path_from_utf8(*file + ".asked"), std::ios::app | std::ios::binary);
        asked << (folders ? "folders " : "files ") << title << '\n';
    }
    std::vector<std::filesystem::path> picked;
    const auto text = util::read_file_if_exists(path);
    if (!text || !*text) return picked;
    for (auto line : util::split(**text, '\n')) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) picked.push_back(util::path_from_utf8(line));
    }
    return picked;
}

} // namespace

// ---- the check before an upload ------------------------------------------------------------------

void App::pick_upload(const std::string& server, const std::string& dir, bool folders, int token) {
    const std::string title = std::format("Upload {} to {}:{}", folders ? "folders" : "files", server, dir.empty() ? "~" : dir);
    auto picked = test_picks(folders, title);
    if (!picked) picked = pick_paths(util::to_wide(title), folders, true, last_upload_dir_);
    if (picked->empty()) {
        Value v = Value::make_object();
        v.set("type", str("uploadCheck"));
        v.set("token", num(token));
        v.set("phase", str("cancelled"));
        post(v);
        return;
    }
    last_upload_dir_ = picked->front().parent_path();
    start_check(token, server, dir, std::move(*picked));
}

void App::drop_upload(int token, const std::string& server, const std::string& dir, bool url, std::vector<std::filesystem::path> paths) {
    std::error_code ec;
    const auto temp = std::filesystem::temp_directory_path(ec); // WebView2's too: it inherits grab-gui's
    if (auto why = upload::drop_refusal(paths, ec ? std::filesystem::path() : temp, url); !why.empty()) {
        return post_refused(token, server, dir, why);
    }
    start_check(token, server, dir, std::move(paths));
}

void App::post_refused(int token, const std::string& server, const std::string& dir, const std::string& why) const {
    UploadCheck c;
    c.token = token;
    c.check.server = server;
    c.check.dir = dir;
    c.check.why = why;
    post_check(c, "refused");
}

void App::start_check(int token, const std::string& server, const std::string& dir, std::vector<std::filesystem::path> paths) {
    auto c = std::make_shared<UploadCheck>();
    c->token = token;
    c->check.server = server;
    c->check.dir = dir;
    // The thread holds no share of its check: checks_ keeps it until the thread is joined.
    UploadCheck* const raw = c.get();
    std::scoped_lock lock(checks_mutex_);
    for (const auto& other : checks_) other->thread.request_stop(); // one dialog at a time
    std::erase_if(checks_, [](const auto& other) { return other->over.load(); });
    checks_.push_back(c);
    c->thread = std::jthread([this, raw, paths = std::move(paths)](const std::stop_token& stop) {
        run_check(*raw, paths, stop);
        raw->over = true;
    });
}

void App::run_check(UploadCheck& c, const std::vector<std::filesystem::path>& paths, const std::stop_token& stop) {
    upload::Check& k = c.check;
    post_check(c, "local");
    auto shown = std::chrono::steady_clock::now();
    upload::look_local(k, paths, [&] {
        const auto now = std::chrono::steady_clock::now();
        if (now - shown < check_progress_gap) return;
        shown = now;
        post_check(c, "local");
    }, stop);
    if (stop.stop_requested()) return; // cancelled: the dialog is gone
    if (auto why = upload::nothing_goes(k); !why.empty()) {
        k.why = std::move(why);
        return post_check(c, "refused");
    }
    post_check(c, "server");
    auto ctx = engine::load_context(config_path_, k.server);
    if (!ctx) {
        k.why = ctx.error();
        return post_check(c, "refused");
    }
    auto target = engine::ssh_target(*ctx);
    if (!target) {
        k.why = target.error(); // it names the server
        return post_check(c, "refused");
    }
    connections_.configure(k.server, connection_budget(k.server, ctx->remote.host, ctx->settings.max_connections));
    const auto pool = pools_.get(k.server, *target, connections_.budget(k.server));
    upload::look_server(k, pool, connections_, stop);
    if (stop.stop_requested()) return;
    // Writable, but the server's side failed every item (a file where a folder goes, a listing
    // that failed): nothing to upload, and the dialog says why.
    if (auto why = upload::nothing_goes(k); k.writable.value_or(false) && !why.empty()) {
        k.why = std::move(why);
        return post_check(c, "refused");
    }
    const bool writable = k.writable.value_or(false);
    c.ready = writable;
    post_check(c, writable ? "ready" : "refused");
}

void App::post_check(const UploadCheck& c, std::string_view phase) const {
    const upload::Check& k = c.check;
    Value v = Value::make_object();
    v.set("type", str("uploadCheck"));
    v.set("token", num(c.token));
    v.set("phase", str(std::string(phase)));
    v.set("server", str(k.server));
    v.set("dir", str(k.dir));
    Value items = Value::make_array();
    std::size_t errors = 0;
    for (const auto& p : k.items) {
        if (!p.error.empty()) ++errors;
        if (items.items.size() == picked_shown) continue;
        Value it = Value::make_object();
        it.set("name", str(p.name));
        it.set("dir", Value::make_bool(p.folder));
        it.set("files", num(static_cast<double>(p.files)));
        it.set("bytes", num(static_cast<double>(p.bytes)));
        if (p.merging) it.set("merging", Value::make_bool(true));
        if (p.clash) it.set("there", Value::make_bool(true));
        if (!p.error.empty()) it.set("error", str(p.error));
        items.push(std::move(it));
    }
    v.set("items", std::move(items));
    v.set("count", num(static_cast<double>(k.items.size())));
    v.set("errors", num(static_cast<double>(errors)));
    v.set("files", num(static_cast<double>(k.files)));
    v.set("bytes", num(static_cast<double>(k.bytes)));
    v.set("skippedLinks", num(static_cast<double>(k.links)));
    v.set("longNames", num(static_cast<double>(k.long_names)));
    v.set("unreadable", num(static_cast<double>(k.unreadable)));
    v.set("writable", k.writable ? Value::make_bool(*k.writable) : Value{});
    if (!k.why.empty()) v.set("why", str(k.why));
    if (k.free) v.set("free", num(static_cast<double>(*k.free)));
    if (upload::short_of_space(k)) v.set("short", Value::make_bool(true));
    Value taken = Value::make_object();
    taken.set("count", num(static_cast<double>(k.taken.count)));
    taken.set("same", num(static_cast<double>(k.taken.same)));
    taken.set("newer", num(static_cast<double>(k.taken.newer)));
    taken.set("older", num(static_cast<double>(k.taken.older)));
    taken.set("differs", num(static_cast<double>(k.taken.differs)));
    taken.set("folders", num(static_cast<double>(k.taken.folders)));
    // The first of them, each with yours, for a choice of its own (upload::taken_list's order,
    // which uploadStart's overrides name).
    if (k.taken.count > 0) {
        Value files = Value::make_array();
        for (const auto& l : upload::taken_list(k, clashes_shown)) files.push(clash_json(l.name, l.clash));
        taken.set("files", std::move(files));
    }
    v.set("existing", std::move(taken));
    // Settings → Uploads: Ask (the dialog asks, starting on Skip) or the choice saved.
    v.set("setting", str(std::string(upload::name(upload_existing_.load()))));
    post(v);
}

void App::cancel_check(int token) {
    std::scoped_lock lock(checks_mutex_);
    for (const auto& c : checks_) {
        if (c->token == token) c->thread.request_stop();
    }
    std::erase_if(checks_, [](const auto& c) { return c->over.load(); });
}

void App::start_upload(int token, const upload::Answer& answer, bool always) {
    std::shared_ptr<UploadCheck> c;
    {
        std::scoped_lock lock(checks_mutex_);
        const auto it = std::ranges::find_if(checks_, [&](const auto& x) { return x->token == token && x->ready.load(); });
        if (it == checks_.end()) return;
        c = *it;
        checks_.erase(it);
    }
    // Ready: its thread has only its last message to post.
    if (c->thread.joinable()) c->thread.join();
    // "Always do this": the choice for all is the setting from now on (Ask is none: the dialog
    // never offers it for all).
    if (always && answer.all != upload::Existing::ask) set_upload_existing(answer.all);
    // The answer about the files already there: each item's choice, a folder's files' own. Still
    // under Ask, the items the dialog listed nothing of ask about a file that turns up there.
    upload::Answer given = answer;
    if (upload_existing_.load() == upload::Existing::ask) given.unlisted = upload::Existing::ask;
    const auto choices = upload::apply_answer(c->check, given);
    std::vector<std::string> not_queued; // picks that would write where another row writes
    {
        std::lock_guard lock(mutex_);
        for (std::size_t k = 0; k < c->check.items.size(); ++k) {
            auto& p = c->check.items[k];
            if (!p.error.empty()) continue;
            auto item = std::make_shared<Item>();
            item->id = next_id_++;
            item->direction = queue_row::Direction::up;
            item->remote = c->check.server;
            item->mode = p.folder ? Mode::folder : Mode::file;
            item->path = p.target;
            item->name = p.name;
            item->source = p.source;
            item->into = c->check.dir;
            item->total = p.bytes;
            item->up_existing = choices[k];
            item->space_ok = true;
            // The same upload queued already (another pick of the same file into the same folder): no
            // second row; a failed one starts again. Another file of that name into that folder
            // isn't queued: the user is told which row has that place (review, chunk 3).
            if (const auto twin = twin_of(*item)) {
                if (local::key(twin->source) != local::key(item->source)) {
                    not_queued.push_back(std::format("{} is already queued for {}:{} from {}: cancel or finish that one first.",
                                                     item->name, item->remote, item->into.empty() ? "~" : item->into,
                                                     util::path_to_utf8(twin->source.parent_path())));
                } else if (twin->status == Status::failed) {
                    // This dialog's answer is the latest, its files' own choices too: the old
                    // answer's own ones go, and the server is looked at again (review, chunk 5).
                    twin->up_existing = choices[k];
                    if (twin->mode == Mode::folder && p.tree) upload::answer_again(twin->tree, *p.tree);
                    requeue(*twin);
                } else if (twin->status == Status::paused && !twin->asked.empty()) {
                    // It waits with the question already: that row's answer decides.
                    not_queued.push_back(std::format("{} is already in the list for {}:{}, waiting for your answer about the "
                                                     "files there: Resume that row to answer.",
                                                     item->name, item->remote, item->into.empty() ? "~" : item->into));
                }
                continue;
            }
            if (p.tree) {
                // The dialog's walk and look: each name taken there gets its choice now (its own,
                // in the tree's choices, else the upload's).
                item->tree = std::move(*p.tree);
                upload::apply(upload_job(*item), item->tree);
                refresh_folder(*item);
                item->names = std::make_shared<const queue_view::Names>(queue_view::names_of(item->tree.files));
                item->gen = next_gen_++;
            }
            adopt_leftovers(*item);
            items_.push_back(std::move(item));
        }
    }
    if (!not_queued.empty()) {
        std::string note = not_queued.front();
        if (not_queued.size() > 1) note += std::format(" ({} more like it)", not_queued.size() - 1);
        post_downloads_note(note);
    }
    wake_.notify_all();
    request_save();
    queue_changed_ = true;
    flush();
}

void App::set_upload_existing(upload::Existing choice) {
    state_.upload_existing = std::string(upload::name(choice));
    upload_existing_ = choice;
    save_state();
    Value v = Value::make_object();
    v.set("type", str("uploadExisting"));
    v.set("choice", str(state_.upload_existing));
    post(v);
}

// ---- running an upload -------------------------------------------------------------------------

upload::FolderJob App::upload_job(Item& item) {
    upload::FolderJob job;
    job.connections = &connections_;
    job.limiter = &up_limiter_;
    job.server = item.remote;
    job.id = item.id;
    job.weight = &item.weight;
    job.source = item.source;
    job.remote_path = item.path;
    job.existing = item.up_existing;
    job.records_dir = records_dir_;
    return job;
}

void App::run_upload_file(const std::shared_ptr<Item>& item, const std::stop_token& item_stop) {
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
    auto target = engine::ssh_target(*ctx);
    if (!target) return set(Status::failed, target.error());
    // The cleaner removing what an earlier run left there (its partial upload too): after it.
    wait_for_cleaner(item->remote, item_stop);
    if (item_stop.stop_requested()) {
        std::lock_guard lock(mutex_);
        item->status = item->stop_reason == StopReason::cancel ? Status::cancelled : Status::paused;
        return;
    }

    upload::Upload u;
    u.local = item->source;
    u.server = item->remote;
    u.id = item->id;
    // The places of the other uploads to that server not done yet (this dialog's other picks among
    // them), which a Keep-both copy must not take before their files get there (review, chunk 5).
    auto places = std::make_shared<std::unordered_set<std::string>>();
    {
        std::lock_guard lock(mutex_);
        for (const auto& other : items_) {
            if (other == item || !is_up(*other) || other->remote != item->remote) continue;
            if (other->status == Status::done || other->status == Status::cancelled) continue;
            places->insert(other->path);
        }
    }
    u.reserved = [places](std::string_view path) { return places->contains(std::string(path)); };
    {
        std::lock_guard lock(mutex_);
        u.remote_path = item->path;
        // Its copy there differed (verify): the retry replaces it, grab's own.
        u.existing = item->resend ? upload::Existing::replace : item->up_existing;
        item->note.clear();
    }
    u.limiter = &up_limiter_;
    u.weight = &item->weight;
    // Its speed and time left count the bytes the server acknowledged (flush).
    u.on_sent = [item](std::uint64_t n) { item->received += n; };
    u.pool = pools_.get(item->remote, *target, connections_.budget(item->remote));
    u.records_dir = records_dir_;
    const auto r = upload::run(u, connections_, [&](const fetch::Progress& p) {
        {
            std::lock_guard lock(mutex_);
            item->bytes = p.bytes;
            if (p.total > 0) item->total = p.total;
            item->connections = p.connections;
        }
        dirty_ = true;
    }, item_stop);
    // A small file's temp without a record: never of use again (it starts over), so it goes.
    if (!r.temp.empty()) add_leftovers(item->remote, upload::Leftovers{.temps = {r.temp}, .partials = {}, .folders = {}, .kept = {}});

    switch (r.outcome) {
    case upload::Outcome::done: break;
    case upload::Outcome::skipped:
        return set(Status::done, {}, r.note.empty() ? std::format("skipped: it is on {} already", item->remote) : r.note);
    case upload::Outcome::exists: {
        // A file turned up at its name after the dialog, and the choice is to ask (UPLOADS.md
        // 5.5): it waits paused with the question; nothing of the file there was touched.
        std::lock_guard lock(mutex_);
        if (!r.clash) {
            item->status = Status::failed;
            item->error = std::format("a file of that name is already on {}", item->remote);
            return;
        }
        upload::Taken taken;
        taken.add(*r.clash);
        ask_upload(*item,
                   {Asked{.name = item->name,
                          .size = r.clash->remote_size,
                          .modified = r.clash->remote_mtime,
                          .mine = existing::Local{.size = r.clash->local_size, .modified = r.clash->local_mtime},
                          .kind = std::string(upload::kind_name(*r.clash)),
                          .index = 0}},
                   1, taken);
        return;
    }
    case upload::Outcome::failed: return set(Status::failed, r.error, r.note);
    case upload::Outcome::stopped: {
        StopReason reason{};
        {
            std::lock_guard lock(mutex_);
            reason = item->stop_reason;
        }
        return set(reason == StopReason::cancel ? Status::cancelled : Status::paused);
    }
    }
    {
        std::lock_guard lock(mutex_);
        item->resend = false;
        item->bytes = r.size;
        item->total = r.size;
    }
    // Settings → Verify: the server's copy against the local file (UPLOADS.md 5.8); nothing
    // local is ever touched. A stop now leaves it done, unchecked: it is in place already.
    if (!verify_downloads_) return set(Status::done, {}, r.note);
    {
        std::lock_guard lock(mutex_);
        item->phase = Phase::verifying;
        item->speed = 0;
        item->eta.reset();
        item->connections = 0;
    }
    dirty_ = true;
    const auto checked = upload::verify_file(verify_request(*ctx, item, u.pool), item->source, r.placed, r.size,
                                             rfc3339_ns(r.mtime_ns), item_stop);
    {
        std::lock_guard lock(mutex_);
        item->phase = Phase::none;
        item->check = verify::mark(checked);
    }
    if (!checked.files.empty() && checked.files.front().verdict == verify::Verdict::differs) {
        {
            std::lock_guard lock(mutex_);
            item->resend = true;
            item->bytes = 0; // the row offers Retry, which sends it again over that copy
        }
        return set(Status::failed, std::format("{}. Retry uploads it again.", verify::summary(checked)));
    }
    set(Status::done, {}, verify::summary(checked));
}

void App::run_upload_folder(const std::shared_ptr<Item>& item, const std::stop_token& item_stop) {
    auto fail = [&](std::string error) {
        std::lock_guard lock(mutex_);
        item->status = Status::failed;
        item->error = std::move(error);
    };
    auto ctx = engine::load_context(config_path_, item->remote);
    if (!ctx) return fail(ctx.error());
    connections_.configure(item->remote,
                           connection_budget(item->remote, ctx->remote.host, ctx->settings.max_connections));
    auto target = engine::ssh_target(*ctx);
    if (!target) return fail(target.error());

    upload::FolderJob job;
    {
        std::lock_guard lock(mutex_);
        job = upload_job(*item);
        item->note.clear();
        // Failed files kept from before a restart are tried again, as a download's are.
        if (std::exchange(item->retry_failed, false)) {
            folder::retry_files(item->tree, false);
            item->touched = true;
        }
    }
    job.on_event = [this, item](folder::Event e) { folder_event(*item, e); };
    job.on_sent = [item](std::uint64_t n) { item->received += n; };
    // Its files at once: what "At once" leaves of the files other transfers hold.
    job.files_at_once = [this, item] {
        std::lock_guard lock(mutex_);
        return std::max(1, parallel_ - files_in_use(item.get()));
    };
    job.pool = pools_.get(item->remote, *target, connections_.budget(item->remote));
    // The cleaner removing what an earlier run left there: after it, and the server looked at
    // again, as the folders it removed are made again (review, chunk 3).
    if (wait_for_cleaner(item->remote, item_stop)) {
        std::lock_guard lock(mutex_);
        item->tree.looked = false;
    }
    if (item_stop.stop_requested()) {
        std::lock_guard lock(mutex_);
        item->status = item->stop_reason == StopReason::cancel ? Status::cancelled : Status::paused;
        return;
    }

    // The local folder, walked once (the dialog did it for a new upload).
    if (auto walked = upload::walk(job, item->tree, mutex_, item_stop); !walked) return fail(walked.error());
    bool named = false;
    bool looked = false;
    {
        std::lock_guard lock(mutex_);
        named = item->names && item->names->path.size() == item->tree.files.size();
        looked = item->tree.looked;
        item->phase = Phase::checking;
    }
    if (!named) new_names(*item); // just walked: the page's rows come
    dirty_ = true;
    // The server's side, looked at again after a restart or a cancel: the folder there or not, and
    // the names taken, each getting the upload's choice.
    bool seen_ok = true;
    if (!looked && !item_stop.stop_requested()) {
        auto seen = upload::look(job, item->tree, mutex_, item_stop);
        seen_ok = seen.has_value();
        if (!item_stop.stop_requested()) {
            std::lock_guard lock(mutex_);
            if (seen) {
                upload::apply(job, item->tree);
            } else {
                for (auto& f : item->tree.files) {
                    if (f.status != folder::State::queued) continue;
                    f.status = folder::State::failed;
                    f.error = seen.error();
                }
            }
            refresh_folder(*item);
            item->touched = true;
        }
    }
    {
        std::lock_guard lock(mutex_);
        item->phase = Phase::none; // sending: the first file's start shows it
    }
    if (seen_ok) upload::run(job, item->tree, mutex_, item_stop);

    // Settings → Verify: what it sent against the server's checksums; nothing local is touched.
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
            const auto checked = upload::verify_files(job, item->tree, mutex_, verify_request(*ctx, item, job.pool), item_stop);
            {
                std::lock_guard lock(mutex_);
                item->phase = Phase::none;
                if (checked.files > 0 && !item_stop.stop_requested()) item->check = verify::mark(checked.report);
            }
            if (checked.files > 0 && !item_stop.stop_requested()) checked_note = checked.summary();
        }
    }

    std::lock_guard lock(mutex_);
    refresh_folder(*item);
    item->touched = true;
    upload::Taken taken; // files that turned up at their names after the dialog, under Ask
    const auto waiting = upload::asking_list(item->tree, late_shown, taken);
    if (item_stop.stop_requested()) {
        item->status = item->stop_reason == StopReason::cancel ? Status::cancelled : Status::paused;
    } else if (taken.count > 0) {
        // The rest went (UPLOADS.md 5.5): it waits paused with the question, nothing of those
        // files there touched.
        std::vector<Asked> asked;
        asked.reserve(waiting.size());
        for (const auto& w : waiting) {
            const auto& c = w.clash;
            asked.push_back(Asked{.name = item->name + "/" + rclone_enc::raw_path(item->tree.files[w.index].path),
                                  .size = c.remote_size,
                                  .modified = c.remote_mtime,
                                  .mine = existing::Local{.size = c.local_size, .modified = c.local_mtime},
                                  .kind = std::string(upload::kind_name(c)),
                                  .index = w.index});
        }
        ask_upload(*item, std::move(asked), taken.count, taken);
    } else if (const auto o = folder::outcome(item->tree); o.failed > 0) {
        item->status = Status::failed;
        item->error = folder::failed_text(o, checked_note);
    } else {
        item->status = o.waiting > 0 ? Status::paused : Status::done;
        // Files already there that Skip left as they are, then what verify found.
        const auto skipped = item->counts[static_cast<std::size_t>(folder::State::skipped)];
        std::string note = skipped == 0 ? std::string()
                                        : std::format("{} already on {}: skipped", skipped == 1 ? std::string("1 file")
                                                                                                : std::format("{} files", skipped),
                                                      item->remote);
        if (!checked_note.empty()) note += (note.empty() ? "" : "; ") + checked_note;
        if (!note.empty()) item->note = std::move(note);
    }
}

// ---- what a cancel leaves on a server ------------------------------------------------------------

void App::discard_upload(const std::shared_ptr<Item>& item) {
    upload::Leftovers left;
    if (item->mode == Mode::folder) {
        upload::FolderJob job;
        {
            std::lock_guard lock(mutex_);
            job = upload_job(*item);
        }
        // Without a pool: what it left there, without asking the server (the cleaner asks).
        left = upload::discard(job, item->tree, mutex_);
        std::lock_guard lock(mutex_);
        // Now the leftovers'; a retry looks at the server again (requeue).
        item->tree.made.clear();
        item->tree.temps.clear();
    } else {
        std::string path;
        {
            std::lock_guard lock(mutex_);
            path = item->path;
        }
        if (upload::saved_progress(records_dir_, item->remote, path)) left.partials.push_back(path);
    }
    add_leftovers(item->remote, left);
}

void App::add_leftovers(const std::string& server, const upload::Leftovers& left) {
    if (left.temps.empty() && left.partials.empty() && left.folders.empty()) return;
    {
        std::lock_guard lock(mutex_);
        auto add = [&](std::string_view kind, const std::vector<std::string>& paths) {
            for (const auto& p : paths) {
                SavedLeftover l{.server = server, .kind = std::string(kind), .path = p};
                if (std::ranges::find(leftovers_, l) == leftovers_.end()) leftovers_.push_back(std::move(l));
            }
        };
        add("temp", left.temps);
        add("partial", left.partials);
        add("folder", left.folders);
    }
    request_save();
    wake_cleaner(server);
}

void App::adopt_leftovers(Item& item) {
    if (!is_up(item)) return;
    std::erase_if(leftovers_, [&](const SavedLeftover& l) {
        if (l.server != item.remote || std::ranges::find(cleaning_, l) != cleaning_.end()) return false;
        const bool at = l.path == item.path;
        // A file: only its own partial upload. Anything else there (a folder a cancelled folder
        // upload of that name made) stays for the cleaner.
        if (item.mode == Mode::file) return l.kind == "partial" && at;
        if (!at && !browse::is_below(l.path, item.path)) return false;
        // A folder: its big files' partial uploads (never one at its own path: a file upload's).
        if (l.kind == "partial") return !at;
        if (l.kind != "folder") return false;
        // A folder its earlier run made, not removed yet: its own again, for a later cancel.
        const std::string rel = at ? std::string() : l.path.substr(item.path.size() + 1);
        if (std::ranges::find(item.tree.made, rel) == item.tree.made.end()) item.tree.made.push_back(rel);
        return true;
    });
}

bool App::wait_for_cleaner(const std::string& server, const std::stop_token& stop) {
    std::unique_lock lock(clean_mutex_);
    if (cleaning_server_ != server) return false;
    clean_wake_.wait(lock, stop, [&] { return cleaning_server_ != server; });
    return true;
}

void App::wake_cleaner(const std::string& server) {
    {
        std::lock_guard lock(clean_mutex_);
        clean_servers_.insert(server);
    }
    clean_wake_.notify_all();
}

// One server at a time: its leftovers as they are now, removed through its pool (upload::clean),
// and those done with taken off. What it can't reach stays for the next start. While a pass runs,
// no upload adopts what it took (cleaning_), and uploads to that server wait for its end
// (wait_for_cleaner), so none of them uses what it removes (review, chunk 3).
void App::run_cleanups(const std::stop_token& stop) {
    for (;;) {
        std::string server;
        {
            std::unique_lock lock(clean_mutex_);
            if (!clean_wake_.wait(lock, stop, [&] { return !clean_servers_.empty(); })) return;
            server = *clean_servers_.begin();
            clean_servers_.erase(clean_servers_.begin());
            cleaning_server_ = server;
        }
        clean_pass(server, stop);
        {
            std::lock_guard lock(clean_mutex_);
            cleaning_server_.clear();
        }
        clean_wake_.notify_all();
    }
}

void App::clean_pass(const std::string& server, const std::stop_token& stop) {
    std::vector<SavedLeftover> taken;
    upload::Leftovers left;
    {
        std::lock_guard lock(mutex_);
        for (const auto& l : leftovers_) {
            if (l.server != server) continue;
            taken.push_back(l);
            (l.kind == "temp" ? left.temps : l.kind == "partial" ? left.partials : left.folders).push_back(l.path);
        }
        cleaning_ = taken;
    }
    upload::Leftovers still;
    bool asked = false;
    if (!taken.empty()) {
        auto ctx = engine::load_context(config_path_, server);
        std::optional<ssh::Target> target;
        if (ctx) {
            if (auto t = engine::ssh_target(*ctx)) target = std::move(*t);
        }
        if (target) {
            connections_.configure(server, connection_budget(server, ctx->remote.host, ctx->settings.max_connections));
            const auto pool = pools_.get(server, *target, connections_.budget(server));
            // A seat of the server's budget, as a listing's: transfers using all of it make room
            // at once instead of when a range ends (review, chunk 3).
            const fetch::Listing seat(connections_, server, stop, 1);
            still = upload::clean(*pool, records_dir_, server, std::move(left), stop);
            asked = true;
        }
    }
    auto kept = [&](const SavedLeftover& l) {
        if (!asked) return true;
        const auto& list = l.kind == "temp" ? still.temps : l.kind == "partial" ? still.partials : still.folders;
        return std::ranges::find(list, l.path) != list.end();
    };
    {
        std::lock_guard lock(mutex_);
        std::erase_if(leftovers_, [&](const SavedLeftover& l) {
            return std::ranges::find(taken, l) != taken.end() && !kept(l);
        });
        cleaning_.clear();
    }
    if (asked) request_save();
}

// ---- Browse after an upload ----------------------------------------------------------------------

void App::upload_ended(const Item& item) {
    std::string server;
    std::string into;
    std::string path;
    bool folder = false;
    {
        std::lock_guard lock(mutex_);
        server = item.remote;
        into = item.into;
        path = item.path;
        folder = item.mode == Mode::folder;
    }
    const std::int64_t now = unix_seconds();
    bool shown = false;
    {
        std::lock_guard lock(browse_mutex_);
        // This session's live listings of them are older now; the file list's too (changed_at).
        std::erase_if(live_folders_, [&](const auto& kv) {
            const auto& [s, f] = kv.first;
            return s == server && (f == into || (folder && (f == path || browse::is_below(f, path))));
        });
        auto mark = [&](const std::string& f, bool below) {
            auto& c = changed_folders_[{server, f}];
            c.ended = std::max(c.ended, now);
            c.below = c.below || below;
        };
        mark(into, false);
        if (folder) mark(path, true);
        shown = shown_browse_.remote == server &&
                (shown_browse_.path == into || shown_browse_.path == path || browse::is_below(shown_browse_.path, path));
    }
    request_save(); // the marks are kept in uploads.json
    if (shown) host_.defer([this] { refresh_browse(); });
}

std::optional<std::int64_t> App::changed_at(const std::string& server, const std::string& folder, std::int64_t list_fetched,
                                            bool& dropped) {
    std::optional<std::int64_t> at;
    for (auto it = changed_folders_.begin(); it != changed_folders_.end();) {
        const auto& [s, f] = it->first;
        if (s != server) {
            ++it;
            continue;
        }
        // A list fetched after the upload ended shows what it did.
        if (list_fetched > it->second.ended) {
            it = changed_folders_.erase(it);
            dropped = true;
            continue;
        }
        if (f == folder || (it->second.below && browse::is_below(folder, f))) at = std::max(at.value_or(0), it->second.ended);
        ++it;
    }
    return at;
}

void App::refresh_browse() {
    LastBrowse last;
    LastBrowse shown;
    {
        std::lock_guard lock(browse_mutex_);
        last = last_browse_;
        shown = shown_browse_;
    }
    // Only while the page waits for nothing else: an answer with its last request's id is what it
    // shows next.
    if (last.id == 0 || last.typed || last.remote != shown.remote || last.top != shown.top ||
        (!last.top && last.path != shown.path)) {
        return;
    }
    run_browse(last.id, shown.remote, shown.path, shown.top, false, true, false);
}

} // namespace grab::gui
