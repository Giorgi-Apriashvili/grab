#include "gui_state.hpp"

#include "activity.hpp"
#include "json.hpp"
#include "util.hpp"

#include <algorithm>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace grab {

std::filesystem::path default_gui_state_path() { return util::config_home() / "grab" / "gui.json"; }

namespace {
constexpr std::string_view recent_header = "grab-recent-searches-sealed 1\n";
constexpr std::string_view recent_purpose = "grab recent searches";
} // namespace

json::Value load_recent(const std::filesystem::path& file) {
    auto data = util::read_file(file);
    if (!data || !data->starts_with(recent_header)) return json::Value::make_array();
    const auto text = util::unprotect_for_user(std::string_view(*data).substr(recent_header.size()), recent_purpose);
    if (!text) return json::Value::make_array(); // another user's, or damaged
    auto doc = json::parse(*text);
    if (!doc || doc->kind != json::Value::Kind::array) return json::Value::make_array();
    return *doc;
}

bool save_recent(const std::filesystem::path& file, const json::Value& items) {
    json::Value kept = json::Value::make_array();
    if (items.kind == json::Value::Kind::array) {
        for (const auto& it : items.items) {
            if (kept.items.size() == recent_max) break;
            if (it.kind == json::Value::Kind::object && json::stringify(it).size() <= 4096) kept.push(it);
        }
    }
    std::error_code ec;
    if (kept.items.empty()) {
        std::filesystem::remove(file, ec);
        return !ec;
    }
    const auto sealed = util::protect_for_user(json::stringify(kept), recent_purpose);
    return sealed && util::write_file_atomic(file, std::string(recent_header) + *sealed);
}

std::optional<SortOrder> sort_from_json(const json::Value& v) {
    auto key = v.string_of("key");
    auto dir = v.string_of("dir");
    if (key != "name" && key != "size" && key != "modified") return std::nullopt;
    if (dir != "asc" && dir != "desc") return std::nullopt;
    SortOrder order{std::move(*key), std::move(*dir)};
    if (const auto* then = v.find("then"); then != nullptr && order.key == "size") {
        if (auto before = sort_from_json(*then); before && before->key != "size") {
            order.then_key = std::move(before->key);
            order.then_dir = std::move(before->dir);
        }
    }
    return order;
}

json::Value sort_to_json(const SortOrder& s) {
    json::Value v = json::Value::make_object();
    v.set("key", json::Value::make_string(s.key));
    v.set("dir", json::Value::make_string(s.dir));
    if (!s.then_key.empty()) v.set("then", sort_to_json(SortOrder{s.then_key, s.then_dir}));
    return v;
}

GuiState parse_gui_state(std::string_view json_text) {
    GuiState s;
    auto doc = json::parse(json_text);
    if (!doc || doc->kind != json::Value::Kind::object) return s;

    auto number = [](const json::Value* v) -> std::optional<int> {
        if (v == nullptr || v->kind != json::Value::Kind::number) return std::nullopt;
        return static_cast<int>(v->number);
    };
    if (const auto* w = doc->find("window"); w != nullptr && w->kind == json::Value::Kind::object) {
        const auto x = number(w->find("x"));
        const auto y = number(w->find("y"));
        const auto width = number(w->find("width"));
        const auto height = number(w->find("height"));
        if (x && y && width && height && *width > 0 && *height > 0) {
            WindowPlacement p{*x, *y, *width, *height, false};
            if (const auto* m = w->find("maximized"); m && m->kind == json::Value::Kind::boolean) {
                p.maximized = m->boolean;
            }
            s.window = p;
        }
    }
    s.last_remote = doc->string_of("lastRemote").value_or("");
    if (const auto* sort = doc->find("sort")) s.sort = sort_from_json(*sort);
    if (const auto* d = doc->find("destinations"); d != nullptr && d->kind == json::Value::Kind::object) {
        for (std::size_t i = 0; i < d->keys.size(); ++i) {
            if (d->items[i].kind == json::Value::Kind::string && !d->items[i].str.empty()) {
                s.destinations[d->keys[i]] = d->items[i].str;
            }
        }
    }
    if (auto p = number(doc->find("parallel"))) s.parallel = std::clamp(*p, min_parallel, max_parallel);
    if (const auto* on = doc->find("verifyDownloads"); on != nullptr && on->kind == json::Value::Kind::boolean) {
        s.verify_downloads = on->boolean;
    }
    if (const auto* on = doc->find("limitOn"); on != nullptr && on->kind == json::Value::Kind::boolean) {
        s.limit_on = on->boolean;
    }
    if (const auto* v = doc->find("limitMiBps"); v != nullptr && v->kind == json::Value::Kind::number &&
                                                  v->number >= min_limit_mibps && v->number <= max_limit_mibps) {
        s.limit_mibps = v->number;
    }
    if (const auto* rules = doc->find("speedSchedule")) s.speed_schedule = schedule::from_json(*rules);
    if (const auto* c = doc->find("limitChange"); c != nullptr && c->kind == json::Value::Kind::object) {
        const auto* on = c->find("on");
        const auto* mibps = c->find("mibps");
        const auto* scheduled = c->find("scheduled");
        const auto* until = c->find("until");
        auto in_range = [](const json::Value* v) {
            return v != nullptr && v->kind == json::Value::Kind::number && v->number >= min_limit_mibps &&
                   v->number <= max_limit_mibps;
        };
        if (on != nullptr && on->kind == json::Value::Kind::boolean && in_range(mibps) && in_range(scheduled) &&
            until != nullptr && until->kind == json::Value::Kind::number && until->number > 0 && until->number < 1e15) {
            s.limit_change = schedule::Override{{on->boolean, mibps->number}, scheduled->number,
                                                static_cast<std::int64_t>(until->number)};
        }
    }
    if (const auto* l = doc->find("serverLimits"); l != nullptr && l->kind == json::Value::Kind::object) {
        for (std::size_t i = 0; i < l->keys.size(); ++i) {
            if (auto n = number(&l->items[i]); n && *n >= 1) s.server_limits[l->keys[i]] = *n;
        }
    }
    if (const auto* t = doc->find("timings"); t != nullptr && t->kind == json::Value::Kind::object) {
        for (std::size_t i = 0; i < t->keys.size(); ++i) {
            if (auto n = number(&t->items[i]); n && *n >= activity::min_ms && *n <= activity::max_ms) {
                s.timings[t->keys[i]] = *n;
            }
        }
    }
    if (const auto* on = doc->find("updateCheck"); on != nullptr && on->kind == json::Value::Kind::boolean) {
        s.update_check = on->boolean;
    }
    if (const auto* t = doc->find("lastUpdateCheck"); t != nullptr && t->kind == json::Value::Kind::number && t->number > 0) {
        s.last_update_check = static_cast<std::int64_t>(t->number);
    }
    s.latest_version = doc->string_of("latestVersion").value_or("");
    s.latest_page = doc->string_of("latestPage").value_or("");
    return s;
}

std::string gui_state_to_json(const GuiState& state) {
    using json::Value;
    Value root = Value::make_object();
    if (state.window) {
        Value w = Value::make_object();
        w.set("x", Value::make_number(state.window->x));
        w.set("y", Value::make_number(state.window->y));
        w.set("width", Value::make_number(state.window->width));
        w.set("height", Value::make_number(state.window->height));
        w.set("maximized", Value::make_bool(state.window->maximized));
        root.set("window", std::move(w));
    }
    root.set("lastRemote", Value::make_string(state.last_remote));
    Value dest = Value::make_object();
    for (const auto& [remote, path] : state.destinations) dest.set(remote, Value::make_string(path));
    root.set("destinations", std::move(dest));
    if (state.sort) root.set("sort", sort_to_json(*state.sort));
    root.set("parallel", Value::make_number(state.parallel));
    root.set("verifyDownloads", Value::make_bool(state.verify_downloads));
    root.set("limitOn", Value::make_bool(state.limit_on));
    root.set("limitMiBps", Value::make_number(state.limit_mibps));
    root.set("speedSchedule", schedule::to_json(state.speed_schedule));
    if (state.limit_change) {
        Value c = Value::make_object();
        c.set("on", Value::make_bool(state.limit_change->limit.on));
        c.set("mibps", Value::make_number(state.limit_change->limit.mibps));
        c.set("scheduled", Value::make_number(state.limit_change->scheduled));
        c.set("until", Value::make_number(static_cast<double>(state.limit_change->until)));
        root.set("limitChange", std::move(c));
    }
    Value limits = Value::make_object();
    for (const auto& [remote, n] : state.server_limits) limits.set(remote, Value::make_number(n));
    root.set("serverLimits", std::move(limits));
    Value timings = Value::make_object();
    for (const auto& [step, ms] : state.timings) timings.set(step, Value::make_number(ms));
    root.set("timings", std::move(timings));
    root.set("updateCheck", Value::make_bool(state.update_check));
    root.set("lastUpdateCheck", Value::make_number(static_cast<double>(state.last_update_check)));
    if (!state.latest_version.empty()) {
        root.set("latestVersion", Value::make_string(state.latest_version));
        root.set("latestPage", Value::make_string(state.latest_page));
    }
    return json::stringify(root);
}

bool move_before(std::vector<int>& ids, int id, int before) {
    if (id == before) return false;
    const auto from = std::ranges::find(ids, id);
    if (from == ids.end()) return false;
    if (before != 0 && std::ranges::find(ids, before) == ids.end()) return false;
    const std::vector<int> old = ids;
    ids.erase(from);
    const auto to = before == 0 ? ids.end() : std::ranges::find(ids, before);
    ids.insert(to, id);
    return ids != old;
}

std::filesystem::path default_queue_path() { return util::config_home() / "grab" / "queue.json"; }

std::vector<SavedDownload> parse_queue(std::string_view json_text) {
    std::vector<SavedDownload> out;
    auto doc = json::parse(json_text);
    if (!doc || doc->kind != json::Value::Kind::array) return out;
    for (const auto& v : doc->items) {
        SavedDownload d;
        auto remote = v.string_of("remote");
        auto path = v.string_of("path");
        auto dest = v.string_of("dest");
        if (!remote || !path || !dest || remote->empty() || path->empty() || dest->empty()) continue;
        d.remote = *remote;
        d.path = *path;
        d.dest = *dest;
        d.mode = v.string_of("mode") == "folder" ? "folder" : "file";
        d.name = v.string_of("name").value_or(*path);
        if (auto p = v.string_of("priority"); p == "high" || p == "low") d.priority = *p;
        if (const auto* r = v.find("resume"); r != nullptr && r->kind == json::Value::Kind::boolean) d.resume = r->boolean;
        if (const auto* o = v.find("spaceOk"); o != nullptr && o->kind == json::Value::Kind::boolean) d.space_ok = o->boolean;
        if (auto e = v.string_of("existing"); e == "replace" || e == "both" || e == "skip") d.existing = *e;
        d.local_name = v.string_of("localName").value_or("");
        if (const auto* t = v.find("total"); t != nullptr && t->kind == json::Value::Kind::number && t->number > 0) {
            d.total = static_cast<std::uint64_t>(t->number);
        }
        if (const auto* files = v.find("files"); files != nullptr && files->kind == json::Value::Kind::array) {
            for (const auto& f : files->items) {
                auto file_path = f.string_of("path");
                if (!file_path || file_path->empty()) continue;
                SavedFile sf;
                sf.path = *file_path;
                if (const auto* s = f.find("size"); s != nullptr && s->kind == json::Value::Kind::number && s->number > 0) {
                    sf.size = static_cast<std::uint64_t>(s->number);
                }
                sf.modtime = f.string_of("modtime").value_or("");
                const auto state = f.string_of("state").value_or("queued");
                sf.state = state == "paused" || state == "done" || state == "skipped" ? state : "queued";
                for (auto [key, flag] : {std::pair{"fetched", &sf.fetched}, std::pair{"verified", &sf.verified}}) {
                    const auto* b = f.find(key);
                    *flag = b != nullptr && b->kind == json::Value::Kind::boolean && b->boolean;
                }
                sf.local = f.string_of("local").value_or("");
                d.files.push_back(std::move(sf));
            }
        }
        out.push_back(std::move(d));
    }
    return out;
}

std::string queue_to_json(const std::vector<SavedDownload>& items) {
    using json::Value;
    Value list = Value::make_array();
    for (const auto& d : items) {
        Value v = Value::make_object();
        v.set("remote", Value::make_string(d.remote));
        v.set("mode", Value::make_string(d.mode));
        v.set("path", Value::make_string(d.path));
        v.set("name", Value::make_string(d.name));
        v.set("dest", Value::make_string(d.dest));
        v.set("total", Value::make_number(static_cast<double>(d.total)));
        v.set("priority", Value::make_string(d.priority));
        if (d.resume) v.set("resume", Value::make_bool(true));
        if (d.space_ok) v.set("spaceOk", Value::make_bool(true));
        if (!d.existing.empty()) v.set("existing", Value::make_string(d.existing));
        if (!d.local_name.empty()) v.set("localName", Value::make_string(d.local_name));
        if (!d.files.empty()) {
            Value files = Value::make_array();
            for (const auto& f : d.files) {
                Value fv = Value::make_object();
                fv.set("path", Value::make_string(f.path));
                fv.set("size", Value::make_number(static_cast<double>(f.size)));
                fv.set("modtime", Value::make_string(f.modtime));
                fv.set("state", Value::make_string(f.state));
                if (f.fetched) fv.set("fetched", Value::make_bool(true));
                if (f.verified) fv.set("verified", Value::make_bool(true));
                if (!f.local.empty()) fv.set("local", Value::make_string(f.local));
                files.push(std::move(fv));
            }
            v.set("files", std::move(files));
        }
        list.push(std::move(v));
    }
    return json::stringify(list);
}

GuiState load_gui_state(const std::filesystem::path& p) {
    auto text = util::read_file(p);
    return text ? parse_gui_state(*text) : GuiState{};
}

bool save_gui_state(const std::filesystem::path& p, const GuiState& state) {
    std::error_code ec;
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    // Write a sibling and swap it in, so a crash mid-write never leaves a truncated file.
    auto tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << gui_state_to_json(state);
        if (!out) return false;
    }
    std::filesystem::rename(tmp, p, ec);
    return !ec;
}

} // namespace grab
