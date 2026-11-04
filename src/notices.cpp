#include "notices.hpp"

#include "gui_state.hpp"
#include "guard.hpp"
#include "json.hpp"
#include "passwords.hpp"
#include "rclone_import.hpp"
#include "server_conf.hpp"
#include "ssh_target.hpp"
#include "util.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <set>
#include <span>
#include <string_view>
#include <tuple>

namespace grab::notices {

namespace {

// One flag of a flag key and the value after it ("--bind=10.0.0.2", "--bind 10.0.0.2").
struct Item {
    std::string name;
    std::string value;
    bool operator==(const Item&) const = default;
};

std::vector<Item> items_of(const std::vector<std::string>& flags) {
    std::vector<Item> out;
    for (const auto& f : flags) {
        if (!f.starts_with('-') && !out.empty()) {
            // A value of the flag before it, as rclone took it.
            auto& last = out.back();
            last.value += (last.value.empty() ? "" : " ") + f;
            continue;
        }
        const auto eq = f.find('=');
        out.push_back(eq == std::string::npos ? Item{.name = f, .value = {}}
                                              : Item{.name = f.substr(0, eq), .value = f.substr(eq + 1)});
    }
    return out;
}

std::vector<Item> default_items(std::string_view flags) { return items_of(util::split_args(flags)); }

// Values a notice may quote: binds, times, sizes and counts, nothing that could be a proxy, a
// command or a path.
constexpr auto quotable = std::to_array<std::string_view>({"--bind", "--contimeout", "--timeout", "--sftp-connections"});

bool plain(std::string_view value) {
    return !value.empty() && std::ranges::all_of(value, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == ':' ||
               c == '%' || c == '_' || c == '-';
    });
}

// "--timeout 2m", or the flag alone when its value isn't one to quote.
std::string shown(const Item& item) {
    if (std::ranges::contains(quotable, item.name) && plain(item.value)) return item.name + " " + item.value;
    return item.name;
}

// A grab.conf line a notice may suggest: `key = value`.
struct Line {
    std::string_view key;
    std::string_view value;
};

// Why grab.conf wouldn't take `line` as config.cpp reads it, nullopt when it would: a notice
// never suggests a line that stops grab.conf from loading.
std::optional<std::string> refused(const Line& line) {
    const std::string_view key = line.key;
    const std::string_view value = util::trim(line.value);
    if (key == "io_timeout") {
        const auto t = ssh::parse_duration(value);
        if (t && *t >= min_io_timeout && *t <= max_io_timeout) return std::nullopt;
        return "io_timeout takes 1s to 24h";
    }
    if (key == "connect_timeout") {
        if (ssh::parse_duration(value)) return std::nullopt;
        return "connect_timeout takes a time such as 30s, or off";
    }
    if (key == "max_connections") {
        int n = 0;
        const bool count = !value.empty() && value.size() <= 2 && std::ranges::all_of(value, [&n](char c) {
            n = n * 10 + (c - '0');
            return c >= '0' && c <= '9';
        });
        if (count && n >= 1 && n <= 64) return std::nullopt;
        return "max_connections takes 1 to 64";
    }
    return std::nullopt;
}

// " = <value>" when grab.conf would take `line`, else "".
std::string suggested(const Line& line) {
    if (!plain(line.value) || refused(line)) return {};
    return std::format(" = {}", line.value);
}

// The guard's settings that keep grab off the server in `flags` alone, read as the guard reads
// common_flags (or folder_flags), by key ("socks_proxy"): what such a flag means comes with the
// guard's notice. One that doesn't block (a cipher list that shares one with grab's) is just
// ignored, like any other rclone flag.
std::set<std::string, std::less<>> blocking(const std::vector<std::string>& flags, bool folder) {
    guard::Sources sources;
    (folder ? sources.folder_flags : sources.common_flags) = flags;
    std::set<std::string, std::less<>> out;
    for (const auto& [key, found] : guard::evaluate(sources)) {
        if (found.blocks) out.insert(key);
    }
    return out;
}

// Whether `flag` is one of `blocking`'s: "--sftp-socks-proxy" for "socks_proxy".
bool blocked_by(std::string_view flag, const std::set<std::string, std::less<>>& blocking) {
    constexpr std::string_view sftp = "--sftp-";
    if (!flag.starts_with(sftp)) return false;
    std::string key(flag.substr(sftp.size()));
    std::ranges::replace(key, '-', '_');
    return blocking.contains(key);
}

// This grab's notices are keyed on their text, under key names grab 0.20 never used ("ignored:..."),
// so what 0.20's notices.json holds hides none of them (RCLONE-REPLACEMENT.md, phase 9 step 7).
struct Builder {
    std::vector<Notice>& out;
    const std::string& server;
    void add(std::string key, std::string flag, std::string text) const {
        out.push_back(Notice{.server = server,
                             .key = std::move(key),
                             .flag = std::move(flag),
                             .digest = util::sha256_hex(text),
                             .text = std::move(text)});
    }
};

// N11: common_flags, with what to write instead of its --timeout, --contimeout and
// --sftp-connections (never a line grab.conf would refuse, nor a key already set). A bind there
// with no `bind` key is engine::connect_settings' refusal, and the guard's flags that block have
// their own notice: neither counts here.
void common_notice(const Builder& b, const RemoteSettings& r, bool bind_refused) {
    const std::string& s = r.name;
    const auto defaults = default_items(default_common_flags);
    const auto guarded = blocking(r.common_flags, false);
    bool ignored = false;
    std::vector<std::string> instead;
    bool written = false; // the first suggestion says "write"
    for (const auto& item : items_of(r.common_flags)) {
        if (std::ranges::contains(defaults, item) || blocked_by(item.name, guarded)) continue;
        if (item.name == "--bind" && bind_refused) continue;
        ignored = true;
        const std::string_view key = item.name == "--timeout"            ? "io_timeout"
                                     : item.name == "--contimeout"       ? "connect_timeout"
                                     : item.name == "--sftp-connections" ? "max_connections"
                                                                         : "";
        const bool set = (key == "io_timeout" && r.io_timeout) || (key == "connect_timeout" && r.connect_timeout) ||
                         (key == "max_connections" && r.max_connections);
        if (key.empty() || set) continue;
        if (const auto bad = refused({.key = key, .value = item.value})) {
            instead.push_back(std::format("{}: {}", shown(item), *bad));
        } else if (const auto line = suggested({.key = key, .value = item.value}); !line.empty()) {
            instead.push_back(std::format("{}: {}{}{}", shown(item), written ? "" : "write ", key, line));
            written = true;
        }
    }
    if (!ignored) return;
    b.add("ignored:common_flags", "",
          std::format("[{}] common_flags is ignored: its flags were for rclone, which this grab no longer has{}.", s,
                      instead.empty() ? std::string() : " (" + util::join(instead, "; ") + ")"));
}

// Whether `flags` hold anything but `defaults`' flags (and a bind that is the refusal, and the
// guard's flags that block, `guarded`).
bool differs(const std::vector<std::string>& flags, std::string_view defaults, bool bind_refused,
             const std::set<std::string, std::less<>>& guarded) {
    const auto known = default_items(defaults);
    return std::ranges::any_of(items_of(flags), [&](const Item& item) {
        return !std::ranges::contains(known, item) && !(item.name == "--bind" && bind_refused) &&
               !blocked_by(item.name, guarded);
    });
}

// N12: folder_flags and file_flags (file_flags' --bind reached no connection since 0.20: only this;
// folder_flags' guard flags that block have the guard's notice).
void batch_notice(const Builder& b, const RemoteSettings& r, bool bind_refused) {
    const bool folder = differs(r.folder_flags, default_folder_flags, bind_refused, blocking(r.folder_flags, true));
    const bool file = differs(r.file_flags, default_file_flags, false, {});
    if (!folder && !file) return;
    const std::string_view hint = "(files at once: At once in grab's window; speed: --limit)";
    b.add("ignored:folder_flags,file_flags", "",
          folder && file ? std::format("[{}] folder_flags and file_flags are ignored: they were rclone flags {}.", r.name, hint)
                         : std::format("[{}] {} is ignored: its flags were for rclone {}.", r.name,
                                       folder ? "folder_flags" : "file_flags", hint));
}

// ssh.exe's options that take an argument.
bool takes_argument(char option) { return std::string_view("bcDEeFIiJLlmOopQRSWw").contains(option); }

bool digits(std::string_view v) {
    return !v.empty() && std::ranges::all_of(v, [](char c) { return c >= '0' && c <= '9'; });
}

// N13: ssh_options, with what to write instead of its ConnectTimeout, and a jump route it holds
// (decision 4: grab connects directly, as the server's downloads always did). Its bind with no
// `bind` key is the refusal, not this.
void ssh_notice(const Builder& b, const RemoteSettings& r, bool bind_refused) {
    const std::string& s = r.name;
    const auto& args = r.ssh_options;
    bool ignored = false;
    std::vector<std::string> instead;
    bool keepalive = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        const char option = arg.size() >= 2 && arg[0] == '-' ? arg[1] : '\0';
        std::string value;
        if (option != '\0' && takes_argument(option)) {
            if (arg.size() > 2) value = arg.substr(2);
            else if (i + 1 < args.size()) value = args[++i];
        }
        // -o "Keyword=value" or "Keyword value"; keywords ignore case.
        const auto sep = value.find_first_of("= \t");
        const std::string keyword = option == 'o' ? std::string(util::trim(std::string_view(value).substr(0, sep))) : "";
        std::string_view v = sep == std::string::npos ? std::string_view() : util::trim(std::string_view(value).substr(sep + 1));
        if (v.starts_with('=')) v = util::trim(v.substr(1));
        const std::string lower = util::to_lower(keyword);
        if (bind_refused && (option == 'b' || lower == "bindaddress")) continue;
        ignored = true;
        if (lower == "connecttimeout" && digits(v) && !r.connect_timeout) {
            instead.push_back(std::format("-o {}={}: write connect_timeout = {}s", keyword, v, v));
        } else if ((lower == "serveraliveinterval" || lower == "serveralivecountmax") && !keepalive) {
            instead.push_back(std::format("{}: grab sends its own keepalives", keyword));
            keepalive = true;
        }
    }
    if (!ignored) return;
    std::string text = std::format("[{}] ssh_options is ignored: grab no longer runs ssh.exe{}.", s,
                                   instead.empty() ? std::string() : " (" + util::join(instead, "; ") + ")");
    // The route as the guard reads it (OpenSSH's first value; "none" is none), never quoted.
    guard::Sources sources;
    sources.ssh_options = args;
    const auto routes = guard::evaluate(sources);
    for (const auto* key : {"ProxyCommand", "ProxyJump"}) {
        const auto it = routes.find(key);
        if (it == routes.end() || !it->second.blocks) continue;
        text += std::format(" {}'s searches went through its {}; grab now connects to {} directly for them too, as {}'s "
                            "downloads always did. Deleting the ssh_options line silences this note.",
                            s, it->second.setting, s, s);
        break;
    }
    b.add("ignored:ssh_options", "", std::move(text));
}

// Go's strconv.ParseBool.
std::optional<bool> go_bool(std::string_view v) {
    for (std::string_view t : {"1", "t", "T", "TRUE", "true", "True"}) {
        if (v == t) return true;
    }
    for (std::string_view f : {"0", "f", "F", "FALSE", "false", "False"}) {
        if (v == f) return false;
    }
    return std::nullopt;
}

// Whether rclone's `key` set to `value` keeps grab off a server, as the guard reads it.
bool blocks(std::string_view key, std::string_view value) {
    guard::Sources sources;
    sources.lines.emplace_back(key, value);
    const auto settings = guard::evaluate(sources);
    const auto found = settings.find(key);
    return found != settings.end() && found->second.blocks.has_value();
}

// N15: the rclone settings (grab.conf's rclone.<key> lines, the store's extras) that changed what
// rclone did, as grab's own client does it.
void rclone_notices(const Builder& b, const RemoteSettings& r, const std::vector<std::pair<std::string, std::string>>& settings) {
    const std::string& s = r.name;
    for (const auto& [key, raw] : settings) {
        const std::string_view value = util::trim(raw);
        const std::string name = "rclone." + key;
        std::string text;
        if (key == "skip_links" && go_bool(value) == true) {
            text = std::format("[{}] {} = true is ignored: rclone is gone, and grab's own client follows links.", s, name);
        } else if (key == "set_env" && !value.empty()) {
            text = std::format("[{}] {} is ignored: rclone is gone, and grab's own client sets no environment on the "
                               "server.",
                               s, name);
        } else if (key == "set_modtime" && go_bool(value) == false) {
            text = std::format("[{}] {} = false is ignored: rclone is gone, and grab's own client never changes the "
                               "server's files.",
                               s, name);
        } else if (key == "connections" && !value.empty() && value != "0") {
            const std::string cap = suggested({.key = "max_connections", .value = value});
            text = std::format("[{}] {}{} is ignored: rclone is gone{}.", s, name,
                               digits(value) ? std::format(" = {}", value) : "",
                               r.max_connections ? std::string()
                                                 : std::format("; to cap grab's connections to {}, set max_connections{} "
                                                               "in [{}]",
                                                               s, cap.empty() ? std::string(" (1 to 64)") : cap, s));
        } else if ((key == "chunk_size" && !value.empty() && value != "32Ki") ||
                   (key == "concurrency" && !value.empty() && value != "64")) {
            text = std::format("[{}] {}{} is ignored: rclone is gone, and grab's own client reads 255 KiB at a time, 64 "
                               "in flight.",
                               s, name, plain(value) ? std::format(" = {}", value) : "");
        } else if (key == "idle_timeout" && !value.empty() && ssh::parse_duration(value) != std::chrono::seconds(60)) {
            text = std::format("[{}] {}{} is ignored: rclone is gone, and grab's own client closes idle connections "
                               "itself.",
                               s, name, plain(value) ? std::format(" = {}", value) : "");
        } else if ((key == "ciphers" || key == "key_exchange" || key == "macs" || key == "host_key_algorithms") &&
                   !value.empty() && !blocks(key, value)) {
            // One that shares nothing with grab's offer is the guard's.
            text = std::format("[{}] {} is ignored: grab's own client offers its own list.", s, name);
        } else {
            continue;
        }
        // Keyed on the text, never the value: set_env and the store's extras can hold a secret, and
        // notices.json must be no guessing oracle for it.
        b.add("ignored:" + name, "", std::move(text));
    }
}

// Whether [grab]'s `key` names a program other than the one grab 0.20 ran by default.
bool not_default(std::string_view value, std::string_view program) {
    const auto v = util::to_lower(util::trim(value));
    return !v.empty() && v != program && v != std::string(program) + ".exe";
}

} // namespace

std::vector<Notice> settings_notices(const ini::Document& doc, const GrabConfig& cfg, const Extras& extras,
                                     const Environment& environment, const Moved& moved, bool store_unreadable) {
    std::vector<Notice> out;
    std::vector<std::string> switched; // N0: moved from rclone to grab's own client
    // N10 and N14, about grab itself.
    const std::string grab = "grab";
    const Builder g{.out = out, .server = grab};
    if (environment) {
        if (const auto engine = environment("GRAB_ENGINE"); engine && util::to_lower(util::trim(*engine)) == "rclone") {
            g.add("ignored:GRAB_ENGINE", "",
                  "GRAB_ENGINE=rclone is ignored: this grab has no rclone; every server uses grab's own client.");
        }
    }
    const bool rclone = not_default(cfg.rclone, "rclone");
    const bool ssh = not_default(cfg.ssh, "ssh");
    if (rclone || ssh) {
        g.add("ignored:rclone,ssh", "",
              rclone && ssh ? "[grab] rclone and ssh are ignored: this grab runs neither; it connects with its own SSH client."
              : rclone      ? "[grab] rclone is ignored: this grab doesn't run rclone; it connects with its own SSH client."
                            : "[grab] ssh is ignored: this grab doesn't run ssh.exe; it connects with its own SSH client.");
    }

    for (const auto& r : cfg.remotes) {
        const Builder b{.out = out, .server = r.name};
        // Its connection details, when it has them, and what keeps grab's client off it.
        std::optional<server_conf::Connection> details;
        if (const auto* section = doc.find(r.name)) {
            if (auto parsed = server_conf::parse(*section); parsed && *parsed) details = std::move(**parsed);
        }
        static const std::vector<std::pair<std::string, std::string>> none;
        const auto found = extras.find(r.name);
        const auto& kept = found != extras.end() ? found->second : none; // a reference, never a copy
        std::vector<guard::Reason> reasons;
        if (details) {
            auto sources = guard::sources(r, *details, kept);
            sources.environment = environment;
            auto guarded = guard::evaluate(sources);
            // Its copy of the extras (a proxy can hold a password), wiped as make_context does.
            for (auto& [key, value] : sources.extras) std::ranges::fill(value, '\0');
            if (store_unreadable) guard::add_unreadable_store(guarded);
            reasons = guard::reasons(guarded);
        }
        // A bind left in an old key with no `bind` key: every connection refuses the server
        // (engine::connect_settings), which says it each time; the notices leave it out.
        const auto old = r.bind ? std::nullopt : old_bind(r);
        const bool refused_bind = old.has_value();
        if (details && !r.engine && reasons.empty() && !refused_bind && moved.contains(r.name)) switched.push_back(r.name);
        if (r.engine == Engine::rclone) {
            b.add("ignored:engine", "",
                  std::format("[{}] engine = rclone is ignored: this grab has no rclone any more, so {} downloads, "
                              "searches and lists with grab's own client. If that fails where rclone worked, grab 0.20.1 "
                              "(GitHub releases) still has the fallback; please report it.",
                              r.name, r.name));
        }
        common_notice(b, r, refused_bind && old->key == "common_flags");
        batch_notice(b, r, refused_bind && old->key == "folder_flags");
        ssh_notice(b, r, refused_bind && old->key == "ssh_options");
        if (!details) continue; // no details yet: nothing to guard
        rclone_notices(b, r, kept);
        rclone_notices(b, r, details->rclone);
        // Keyed on the text (kind, where), never the value: a proxy can hold a password.
        for (const auto& reason : reasons) b.add("guard", reason.where, guard::text(r.name, std::span(&reason, 1)));
    }
    // N0 first, once per configuration whichever servers it names then (keyed on the default
    // itself, as 0.20 keyed it, so 0.20's users don't see it again): an imported server without an
    // `engine` line changed from rclone to grab's client, and a list fetched by rclone doesn't
    // match grab's (file_list::fingerprint).
    if (!switched.empty()) {
        out.insert(out.begin(),
                   Notice{.server = "grab",
                          .key = "engine",
                          .flag = "default",
                          .digest = util::sha256_hex("grab"),
                          .text = std::format("Downloads, searches and lists now use grab's own SSH client, no longer "
                                              "rclone ({}); their saved file lists are fetched again once.",
                                              util::join(switched, ", "))});
    }
    // N7, wherever the key is.
    for (const auto& [section, key] : cfg.unknown_keys) {
        const auto* s = doc.find(section);
        const std::string value = s != nullptr ? s->get(key).value_or("") : std::string();
        out.push_back(Notice{.server = section,
                             .key = key,
                             .flag = {},
                             .digest = util::sha256_hex(value),
                             .text = std::format("[{}] {} isn't a grab.conf setting (ignored).", section, key)});
    }
    return out;
}

std::expected<std::vector<Notice>, std::string> for_config(const std::filesystem::path& grab_conf) {
    auto doc = ini::parse_file(grab_conf);
    if (!doc) return util::fail(doc.error());
    auto cfg = parse_grab_config(*doc);
    if (!cfg) return util::failf("{}: {}", util::path_to_utf8(grab_conf), cfg.error());
    Extras extras;
    const auto store = passwords::load(passwords::path_in(state_dir(grab_conf)));
    if (store && !store->empty()) {
        for (const auto& r : cfg->remotes) {
            const auto* section = doc->find(r.name);
            if (section == nullptr) continue;
            const auto c = server_conf::parse(*section);
            if (!c || !*c) continue;
            const passwords::Binding login{.user = (*c)->user, .host = (*c)->host, .port = (*c)->port};
            if (auto kept = guard::store_extras(*store, r.name, login); !kept.empty()) extras[r.name] = std::move(kept);
        }
    }
    // The servers that used rclone before the switch: those the import moved.
    Moved moved;
    std::string noted; // the note of the run that deleted grab's own old files, or couldn't
    if (const auto text = util::read_file_if_exists(rclone_import::record_path(state_dir(grab_conf))); text && *text) {
        if (auto record = rclone_import::from_json(**text)) {
            moved = std::move(record->imported);
            noted = std::move(record->old_files);
        }
    }
    auto out = settings_notices(
        *doc, *cfg, extras, [](const std::string& name) { return util::getenv_utf8(name.c_str()); }, moved, !store);
    for (auto& [server, kept] : extras) {
        for (auto& [key, value] : kept) std::ranges::fill(value, '\0');
    }
    // grab's own old rclone.conf and 'secrets' (the user's decision 2): why they stay, else what
    // the run that deleted them (or couldn't) recorded.
    const auto old = rclone_import::old_files_kept(grab_conf).value_or(std::move(noted));
    if (!old.empty()) {
        out.push_back(Notice{.server = "grab", .key = "old files", .flag = {}, .digest = util::sha256_hex(old), .text = old});
    }
    return out;
}

std::filesystem::path path_in(const std::filesystem::path& state_dir) { return state_dir / "notices.json"; }

namespace {

using Id = std::tuple<std::string, std::string, std::string, std::string>; // server, key, flag, digest

Id id_of(const Notice& n) { return {n.server, n.key, n.flag, n.digest}; }

// N0 is about the switch itself: once shown it stays shown, also while no server is on grab's
// client for a time.
bool sticky(const Id& id) {
    return std::get<0>(id) == "grab" && std::get<1>(id) == "engine" && std::get<2>(id) == "default";
}

std::set<Id> read_shown(const std::filesystem::path& file) {
    std::set<Id> shown;
    auto text = util::read_file_if_exists(file);
    if (!text || !*text) return shown;
    auto doc = json::parse(**text);
    if (!doc) return shown;
    const auto* list = doc->find("shown");
    if (list == nullptr || list->kind != json::Value::Kind::array) return shown;
    for (const auto& v : list->items) {
        shown.emplace(v.string_of("server").value_or(""), v.string_of("key").value_or(""), v.string_of("flag").value_or(""),
                      v.string_of("sha256").value_or(""));
    }
    return shown;
}

} // namespace

std::expected<std::vector<Notice>, std::string> take_new(const std::filesystem::path& file,
                                                         const std::vector<Notice>& current) {
    auto lock = lock_settings();
    if (!lock) return util::fail(lock.error());
    const auto shown = read_shown(file);
    std::vector<Notice> fresh;
    std::set<Id> now;
    for (const auto& n : current) {
        if (!shown.contains(id_of(n))) fresh.push_back(n);
        now.insert(id_of(n));
    }
    for (const auto& id : shown) {
        if (sticky(id)) now.insert(id);
    }
    if (now == shown) return fresh;
    auto list = json::Value::make_array();
    for (const auto& [server, key, flag, digest] : now) {
        auto v = json::Value::make_object();
        v.set("server", json::Value::make_string(server));
        v.set("key", json::Value::make_string(key));
        v.set("flag", json::Value::make_string(flag));
        v.set("sha256", json::Value::make_string(digest));
        list.push(std::move(v));
    }
    auto doc = json::Value::make_object();
    doc.set("shown", std::move(list));
    if (auto written = util::write_file_atomic(file, json::stringify(doc) + "\n"); !written) return util::fail(written.error());
    return fresh;
}

std::vector<Notice> take_fresh(const std::filesystem::path& grab_conf) {
    auto all = for_config(grab_conf);
    if (!all) return {};
    auto fresh = take_new(path_in(state_dir(grab_conf)), *all);
    return fresh ? std::move(*fresh) : std::move(*all);
}

} // namespace grab::notices
