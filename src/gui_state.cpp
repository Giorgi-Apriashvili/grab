#include "gui_state.hpp"

#include "json.hpp"
#include "util.hpp"

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
    return json::stringify(root);
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
