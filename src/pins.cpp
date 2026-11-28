#include "pins.hpp"

#include "browse.hpp"
#include "config.hpp"
#include "gui_state.hpp"
#include "json.hpp"
#include "rclone_encoding.hpp"
#include "server_files.hpp"
#include "util.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace grab::pins {

namespace {

constexpr std::string_view header = "grab-pins-sealed 1\n";
constexpr std::string_view purpose = "grab pinned folders";
constexpr std::string_view full_text = "Quick access holds 100 folders per server: unpin one first.";

// The file as it is now.
struct Read {
    Pins pins;
    bool newer = false;
    std::string damaged; // why it can't be read; empty when it can (or there is none)
    bool io = false;     // not read at all (an I/O error): its content may be good
};

// A file whose content can't be read, and why.
Read damaged(std::string why) { return Read{.pins = {}, .newer = false, .damaged = std::move(why), .io = false}; }

Read read(const std::filesystem::path& file) {
    auto data = util::read_file_if_exists(file);
    if (!data) return Read{.pins = {}, .newer = false, .damaged = std::move(data.error()), .io = true};
    if (!*data) return Read{.pins = {}, .newer = false, .damaged = {}, .io = false}; // none yet
    const std::string& saved = **data;
    if (!saved.starts_with(header)) return damaged("it isn't a file of grab's pins");
    const auto text = util::unprotect_for_user(std::string_view(saved).substr(header.size()), purpose);
    if (!text) return damaged("it can't be decrypted: another Windows account's, or damaged");
    const auto doc = json::parse(*text);
    const json::Value* v = doc && doc->kind == json::Value::Kind::object ? doc->find("version") : nullptr;
    if (v == nullptr || v->kind != json::Value::Kind::number || v->number < 1) return damaged("it isn't what grab saves there");
    Pins pins;
    const json::Value* servers = doc->find("servers");
    for (std::size_t i = 0; servers != nullptr && servers->kind == json::Value::Kind::object && i < servers->keys.size(); ++i) {
        const json::Value& entries = servers->items[i];
        if (servers->keys[i].empty() || entries.kind != json::Value::Kind::array) continue;
        std::vector<Pin> list;
        for (const json::Value& e : entries.items) {
            if (list.size() == max_per_server) break;
            auto path = e.string_of("path");
            if (!path || bad_path(*path) || std::ranges::contains(list, *path, &Pin::path)) continue;
            // A name that can't be one is left out: the folder's own shows.
            auto name = e.string_of("name");
            if (name && bad_name(*name)) name.reset();
            list.push_back(Pin{.path = std::move(*path), .name = std::move(name).value_or("")});
        }
        if (!list.empty()) pins.emplace(servers->keys[i], std::move(list)); // a server twice: the first
    }
    return Read{.pins = std::move(pins), .newer = v->number > version, .damaged = {}, .io = false};
}

std::expected<void, std::string> write(const std::filesystem::path& file, const Pins& pins) {
    json::Value servers = json::Value::make_object();
    for (const auto& [server, list] : pins) {
        if (list.empty()) continue;
        json::Value entries = json::Value::make_array();
        for (const Pin& p : list) {
            json::Value e = json::Value::make_object();
            e.set("path", json::Value::make_string(p.path));
            if (!p.name.empty()) e.set("name", json::Value::make_string(p.name));
            entries.push(std::move(e));
        }
        servers.set(server, std::move(entries));
    }
    if (servers.keys.empty()) {
        std::error_code ec;
        std::filesystem::remove(file, ec);
        if (ec) return util::fail(ec.message());
        return {};
    }
    json::Value doc = json::Value::make_object();
    doc.set("version", json::Value::make_number(version));
    doc.set("servers", std::move(servers));
    const auto sealed = util::protect_for_user(json::stringify(doc), purpose);
    if (!sealed) return util::fail("Windows couldn't encrypt it for your account");
    return util::write_file_atomic(file, std::string(header) + *sealed);
}

// Moved aside, so that no change replaces it, and the pins start empty.
Loaded set_aside(const std::filesystem::path& file, std::string why) {
    auto moved = util::move_aside(file, "bad");
    return Loaded{.pins = {}, .newer = false, .damaged = std::move(why), .aside = moved ? *moved : std::filesystem::path{}};
}

// UTF-8 `s` (valid) by its code points.
std::size_t chars_of(std::string_view s) {
    return static_cast<std::size_t>(std::ranges::count_if(s, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

// The list of `server`, if it has one.
std::vector<Pin>* list_of(Pins& pins, std::string_view server) {
    const auto it = pins.find(server);
    return it == pins.end() ? nullptr : &it->second;
}

// The server's list gone once empty.
void drop_if_empty(Pins& pins, std::string_view server) {
    if (const auto it = pins.find(server); it != pins.end() && it->second.empty()) pins.erase(it);
}

} // namespace

std::filesystem::path path_for(const std::filesystem::path& grab_conf) { return state_dir(grab_conf) / "pins"; }

std::optional<std::string> bad_path(std::string_view path) {
    if (path.empty()) return "Open a folder of a server first.";
    if (path.size() > max_path_bytes) return "That folder's path is too long for Quick access (at most 4 KiB).";
    if (!rclone_enc::valid_utf8(path)) return "That folder's path isn't UTF-8, so it can't be pinned.";
    const auto control = [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; };
    if (std::ranges::any_of(path, control)) return "That folder's path holds a control character, so it can't be pinned.";
    return std::nullopt;
}

std::optional<std::string> bad_name(std::string_view name) {
    if (name.empty()) return "A pin's name can't be empty.";
    if (!rclone_enc::valid_utf8(name)) return "A pin's name has to be text (UTF-8).";
    if (chars_of(name) > max_name_chars) return "A pin's name can be at most 255 characters.";
    for (std::size_t i = 0; i < name.size(); ++i) {
        const auto c = static_cast<unsigned char>(name[i]);
        // C0, DEL, and C1 (U+0080 to U+009F: 0xC2 0x80 to 0xC2 0x9F).
        const bool c1 = c == 0xC2 && i + 1 < name.size() && static_cast<unsigned char>(name[i + 1]) <= 0x9F;
        if (c < 0x20 || c == 0x7F || c1) return "A pin's name can't hold control characters.";
    }
    return std::nullopt;
}

std::string shown_name(const Pin& pin) {
    if (!pin.name.empty()) return pin.name;
    std::string_view path = pin.path;
    while (path.size() > 1 && path.ends_with('/')) path.remove_suffix(1);
    const auto slash = path.rfind('/');
    const std::string_view last = slash == std::string_view::npos ? path : path.substr(slash + 1);
    return std::string(last.empty() ? path : last);
}

Loaded load(const std::filesystem::path& file) {
    // Left where it is: not read at all (its content may be good), or not even the lock.
    const auto left = [](std::string why) { return Loaded{.pins = {}, .newer = false, .damaged = std::move(why), .aside = {}}; };
    Read first = read(file);
    if (first.damaged.empty()) return Loaded{.pins = std::move(first.pins), .newer = first.newer, .damaged = {}, .aside = {}};
    if (first.io) return left(std::move(first.damaged));
    // Moved aside only while no other grab writes it, and only if it still can't be read then.
    auto lock = lock_settings();
    if (!lock) return left(std::move(first.damaged));
    Read now = read(file);
    if (now.damaged.empty()) return Loaded{.pins = std::move(now.pins), .newer = now.newer, .damaged = {}, .aside = {}};
    if (now.io) return left(std::move(now.damaged));
    return set_aside(file, std::move(now.damaged));
}

Updated update(const std::filesystem::path& file, const std::function<std::expected<bool, std::string>(Pins&)>& edit) {
    // Refused, the file left alone: what it holds isn't known.
    const auto unread = [](std::string_view why) {
        return Updated{.pins = {}, .changed = false, .error = std::format("{}{}", cant_save, why), .unread = true,
                       .damaged = {}, .aside = {}};
    };
    auto lock = lock_settings();
    if (!lock) return unread(lock.error());
    Read now = read(file);
    std::string damaged;
    std::filesystem::path aside;
    if (now.io) return unread(now.damaged);
    if (!now.damaged.empty()) {
        auto moved = util::move_aside(file, "bad");
        if (!moved) return unread(std::format("{}, and it couldn't be moved aside: {}", now.damaged, moved.error()));
        damaged = std::move(now.damaged);
        aside = std::move(*moved);
        now = Read{.pins = {}, .newer = false, .damaged = {}, .io = false}; // the change starts from none
    }
    const auto done = [&](Pins p, bool changed, std::string error) {
        return Updated{.pins = std::move(p), .changed = changed, .error = std::move(error), .unread = false,
                       .damaged = damaged, .aside = aside};
    };
    Pins pins = now.pins;
    auto changed = edit(pins);
    if (!changed) return done(std::move(now.pins), false, std::move(changed.error()));
    if (!*changed) return done(std::move(now.pins), false, {});
    if (now.newer) return done(std::move(now.pins), false, std::string(newer_text));
    if (auto saved = write(file, pins); !saved) return done(std::move(now.pins), false, std::format("{}{}", cant_save, saved.error()));
    return done(std::move(pins), true, {});
}

std::optional<std::size_t> find(const Pins& pins, std::string_view server, std::string_view path, std::string_view home) {
    const auto it = pins.find(server);
    if (it == pins.end()) return std::nullopt;
    const std::vector<Pin>& list = it->second;
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i].path == path) return i;
    }
    const std::string want = server_files::canonical(path, home);
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (server_files::canonical(list[i].path, home) == want) return i;
    }
    return std::nullopt;
}

std::expected<void, std::string> add(Pins& pins, std::string_view server, std::string_view path, std::string_view name,
                                     std::optional<std::size_t> at, std::string_view home) {
    if (auto why = bad_path(path)) return util::fail(std::move(*why));
    if (!name.empty()) {
        if (auto why = bad_name(name)) return util::fail(std::move(*why));
    }
    if (const auto there = find(pins, server, path, home)) {
        return util::failf("{} is already in Quick access", shown_name(list_of(pins, server)->at(*there)));
    }
    if (const auto* list = list_of(pins, server); list != nullptr && list->size() >= max_per_server) {
        return util::fail(std::string(full_text));
    }
    auto& list = pins[std::string(server)];
    const std::size_t place = std::min(at.value_or(list.size()), list.size());
    list.insert(list.begin() + static_cast<std::ptrdiff_t>(place), Pin{.path = std::string(path), .name = std::string(name)});
    return {};
}

bool remove(Pins& pins, std::string_view server, std::string_view path, std::string_view home) {
    const auto i = find(pins, server, path, home);
    if (!i) return false;
    auto& list = *list_of(pins, server);
    list.erase(list.begin() + static_cast<std::ptrdiff_t>(*i));
    drop_if_empty(pins, server);
    return true;
}

std::expected<bool, std::string> rename(Pins& pins, std::string_view server, std::string_view path, std::string_view name,
                                        std::string_view home) {
    if (!name.empty()) {
        if (auto why = bad_name(name)) return util::fail(std::move(*why));
    }
    const auto i = find(pins, server, path, home);
    if (!i) return false;
    Pin& pin = list_of(pins, server)->at(*i);
    if (pin.name == name) return false;
    pin.name = std::string(name);
    return true;
}

bool move(Pins& pins, std::string_view server, std::string_view path, std::ptrdiff_t to, std::string_view home) {
    const auto i = find(pins, server, path, home);
    if (!i) return false;
    auto& list = *list_of(pins, server);
    const auto last = static_cast<std::ptrdiff_t>(list.size()) - 1;
    const auto from = static_cast<std::ptrdiff_t>(*i);
    const std::ptrdiff_t place = std::clamp(to, std::ptrdiff_t{0}, last);
    if (place == from) return false;
    Pin pin = std::move(list[*i]);
    list.erase(list.begin() + from);
    list.insert(list.begin() + place, std::move(pin));
    return true;
}

std::vector<Moved> moved(std::span<const server_files::Item> items, std::span<const std::string> placed) {
    std::vector<Moved> out;
    for (std::size_t i = 0; i < items.size() && i < placed.size(); ++i) {
        if (placed[i].empty() || placed[i] == items[i].from) continue;
        out.push_back(Moved{.from = items[i].from, .to = placed[i]});
    }
    return out;
}

std::vector<Moved> moved_by_paste(bool copying, std::span<const server_files::Item> items, std::span<const std::string> placed,
                                  std::span<const server_files::Item> again, std::span<const std::string> placed_again) {
    if (copying) return {};
    std::vector<Moved> out = moved(items, placed);
    std::ranges::move(moved(again, placed_again), std::back_inserter(out));
    return out;
}

bool follow(Pins& pins, std::string_view server, std::string_view home, std::span<const Moved> moves) {
    auto* list = list_of(pins, server);
    if (list == nullptr) return false;
    // Each move by the folder it left; the login folder and "/" never move (grab refuses to).
    struct Move {
        std::string from; // canonical
        std::string to;   // as given
    };
    const std::string login = server_files::canonical("", home);
    std::vector<Move> canon;
    for (const Moved& m : moves) {
        std::string from = server_files::canonical(m.from, home);
        if (m.to.empty() || from.empty() || from == "/" || from == login || from == server_files::canonical(m.to, home)) continue;
        std::string to = m.to;
        while (to.size() > 1 && to.ends_with('/')) to.pop_back();
        canon.push_back(Move{.from = std::move(from), .to = std::move(to)});
    }
    std::vector<bool> followed(list->size(), false);
    for (std::size_t i = 0; i < list->size(); ++i) {
        Pin& pin = (*list)[i];
        const std::string at = server_files::canonical(pin.path, home);
        // The deepest folder it is at or below.
        const Move* by = nullptr;
        for (const Move& m : canon) {
            if ((at == m.from || browse::is_below(at, m.from)) && (by == nullptr || m.from.size() > by->from.size())) by = &m;
        }
        if (by == nullptr) continue;
        pin.path = by->to + at.substr(by->from.size());
        followed[i] = true;
    }
    if (!std::ranges::contains(followed, true)) return false;
    // Two at one folder now, one of them moved here: the earlier stays. Two this change didn't move
    // (spelled two ways before the login folder was known) both stay.
    std::map<std::string, bool, std::less<>> kept_at; // a kept pin's folder: whether one moved there
    std::vector<Pin> kept;
    for (std::size_t i = 0; i < list->size(); ++i) {
        const auto [it, fresh] = kept_at.try_emplace(server_files::canonical((*list)[i].path, home), followed[i]);
        if (!fresh && (it->second || followed[i])) continue;
        kept.push_back(std::move((*list)[i]));
    }
    *list = std::move(kept);
    return true;
}

bool drop_below(Pins& pins, std::string_view server, std::string_view home, std::span<const std::string> paths) {
    auto* list = list_of(pins, server);
    if (list == nullptr) return false;
    // Never the login folder or "/" (grab refuses to delete them), which would take every pin.
    const std::string login = server_files::canonical("", home);
    std::vector<std::string> gone;
    for (const auto& p : paths) {
        if (std::string c = server_files::canonical(p, home); !c.empty() && c != "/" && c != login) gone.push_back(std::move(c));
    }
    const auto dropped = std::erase_if(*list, [&](const Pin& pin) {
        const std::string at = server_files::canonical(pin.path, home);
        return std::ranges::any_of(gone, [&](const std::string& g) { return at == g || browse::is_below(at, g); });
    });
    drop_if_empty(pins, server);
    return dropped > 0;
}

bool deleted(Pins& pins, std::string_view server, std::string_view home, std::span<const std::string> paths,
             const server_files::Report& report) {
    if (report.stopped || !report.error.empty()) return false;
    return drop_below(pins, server, home, paths);
}

bool forget_server(Pins& pins, std::string_view server) {
    const auto it = pins.find(server);
    if (it == pins.end()) return false;
    pins.erase(it);
    return true;
}

} // namespace grab::pins
