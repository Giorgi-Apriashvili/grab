#include "gui_state.hpp"

#include "json.hpp"
#include "util.hpp"

#include <algorithm>
#include <fstream>

namespace grab {

std::filesystem::path default_gui_state_path() { return util::config_home() / "grab" / "gui.json"; }

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
    if (auto mode = doc->string_of("mode"); mode == "file" || mode == "folder") s.mode = *mode;
    if (const auto* d = doc->find("destinations"); d != nullptr && d->kind == json::Value::Kind::object) {
        for (std::size_t i = 0; i < d->keys.size(); ++i) {
            if (d->items[i].kind == json::Value::Kind::string && !d->items[i].str.empty()) {
                s.destinations[d->keys[i]] = d->items[i].str;
            }
        }
    }
    if (auto p = number(doc->find("parallel"))) s.parallel = std::clamp(*p, min_parallel, max_parallel);
    if (const auto* l = doc->find("serverLimits"); l != nullptr && l->kind == json::Value::Kind::object) {
        for (std::size_t i = 0; i < l->keys.size(); ++i) {
            if (auto n = number(&l->items[i]); n && *n >= 1) s.server_limits[l->keys[i]] = *n;
        }
    }
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
    root.set("mode", Value::make_string(state.mode));
    Value dest = Value::make_object();
    for (const auto& [remote, path] : state.destinations) dest.set(remote, Value::make_string(path));
    root.set("destinations", std::move(dest));
    root.set("parallel", Value::make_number(state.parallel));
    Value limits = Value::make_object();
    for (const auto& [remote, n] : state.server_limits) limits.set(remote, Value::make_number(n));
    root.set("serverLimits", std::move(limits));
    return json::stringify(root);
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
        if (const auto* t = v.find("total"); t != nullptr && t->kind == json::Value::Kind::number && t->number > 0) {
            d.total = static_cast<std::uint64_t>(t->number);
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
