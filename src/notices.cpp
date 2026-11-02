#include "notices.hpp"

#include "gui_state.hpp"
#include "guard.hpp"
#include "json.hpp"
#include "passwords.hpp"
#include "rclone_flags.hpp"
#include "server_conf.hpp"
#include "ssh_target.hpp"
#include "util.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <set>
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
            // A value of the flag before it (rclone_flags::connection reads them so).
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
constexpr auto quotable = std::to_array<std::string_view>(
    {"--bind", "--contimeout", "--timeout", "--sftp-chunk-size", "--sftp-concurrency", "--sftp-connections",
     "--sftp-idle-timeout", "--low-level-retries", "--tpslimit", "--tpslimit-burst", "--transfers", "--checkers",
     "--multi-thread-streams", "--multi-thread-cutoff", "--multi-thread-chunk-size", "--multi-thread-write-buffer-size",
     "--buffer-size", "--bwlimit", "--retries", "--max-depth"});

bool plain(std::string_view value) {
    return !value.empty() && std::ranges::all_of(value, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == ':' ||
               c == '%' || c == '_' || c == '-';
    });
}

// "--bind 10.0.0.2", or the flag alone when its value isn't one to quote.
std::string shown(const Item& item) {
    if (std::ranges::contains(quotable, item.name) && plain(item.value)) return item.name + " " + item.value;
    return item.name;
}

// The new key a legacy connection flag stands for.
std::string_view key_for(std::string_view flag) {
    if (flag == "--bind") return "bind";
    if (flag == "--contimeout") return "connect_timeout";
    if (flag == "--timeout") return "io_timeout";
    return {};
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
    if (key == "bind") {
        if (!value.starts_with('-') && !server_conf::check_host(value)) return std::nullopt;
        return "bind takes a host address";
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

// Why grab's own client doesn't use an rclone connection flag (N2).
std::string why_unused(std::string_view flag) {
    if (flag == "--sftp-chunk-size" || flag == "--sftp-concurrency") {
        return "grab's own client reads 255 KiB at a time, 64 in flight";
    }
    if (flag == "--sftp-idle-timeout") return "grab's own client closes idle connections itself";
    if (flag == "--low-level-retries") return "grab retries a failed piece by itself";
    if (flag == "--tpslimit" || flag == "--tpslimit-burst") return "grab's speed limit is --limit or the speed schedule";
    if (flag == "--sftp-disable-hashcheck") return "grab's own Verify (--verify, Verify downloads) checks downloads";
    if (flag == "--sftp-skip-links") return "grab's own client follows links";
    if (flag == "--sftp-set-modtime") return "grab's own client never changes the server's files";
    if (flag == "--sftp-set-env") return "grab's own client sets no environment on the server";
    for (std::string_view other : {"--dscp", "--user-agent", "--ca-cert", "--client-cert", "--client-key",
                                   "--no-check-certificate", "--password-command"}) {
        if (flag == other) return "it isn't used over SFTP";
    }
    return "grab's own client doesn't use it";
}

// What grab has instead of a flag only --rclone copies use (N8), after "grab doesn't use it".
std::string instead_of(std::string_view flag) {
    if (flag == "--transfers" || flag == "--checkers") return " (files at once: At once in grab's window)";
    if (flag == "--bwlimit") return " (speed: --limit or the speed schedule)";
    if (flag.starts_with("--multi-thread")) return " (grab splits big files over its connections itself)";
    if (flag == "-P" || flag == "--progress" || flag.starts_with("--stats")) return " (grab shows its own progress)";
    if (flag == "-v" || flag == "-vv" || flag == "--verbose") return " (grab's own -v shows what it runs)";
    if (flag == "-n" || flag == "--dry-run") return " (grab's own -n shows what would be fetched)";
    return {};
}

// The guard's flags: what they mean comes with the guard's notice.
bool guard_flag(std::string_view flag) {
    constexpr std::string_view sftp = "--sftp-";
    if (!flag.starts_with(sftp)) return false;
    std::string key(flag.substr(sftp.size()));
    std::ranges::replace(key, '-', '_');
    for (std::string_view k : {"socks_proxy", "http_proxy", "ssh", "subsystem", "server_command", "pubkey", "pubkey_file",
                               "use_insecure_cipher", "ciphers", "key_exchange", "macs", "host_key_algorithms",
                               "disable_concurrent_reads", "use_fstat"}) {
        if (key == k) return true;
    }
    return false;
}

struct Builder {
    std::vector<Notice>& out;
    const std::string& server;
    void add(std::string key, std::string flag, std::string_view value, std::string text) const {
        out.push_back(Notice{.server = server,
                             .key = std::move(key),
                             .flag = std::move(flag),
                             .digest = util::sha256_hex(value),
                             .text = std::move(text)});
    }
    void add_keyed_on_text(std::string key, std::string flag, std::string text) const {
        out.push_back(Notice{.server = server,
                             .key = std::move(key),
                             .flag = std::move(flag),
                             .digest = util::sha256_hex(text),
                             .text = std::move(text)});
    }
};

void flag_notices(const Builder& b, const RemoteSettings& r, bool on_grab) {
    const std::string& s = r.name;
    // common_flags
    const auto common_defaults = default_items(default_common_flags);
    for (const auto& item : items_of(r.common_flags)) {
        if (std::ranges::contains(common_defaults, item)) continue;
        const std::vector<std::string> one{item.name};
        if (rclone_flags::is_connection(item.name)) {
            if (!on_grab || guard_flag(item.name) || rclone_flags::first_refused(one)) continue;
            if (item.name == "--bind" && util::trim(item.value).empty()) continue; // no bind, as rclone reads it
            if (const auto key = key_for(item.name); !key.empty()) {
                const bool set = (key == "bind" && r.bind) || (key == "connect_timeout" && r.connect_timeout) ||
                                 (key == "io_timeout" && r.io_timeout);
                const auto bad = refused({.key = key, .value = item.value});
                const std::string line = std::format("`{}{}`", key, suggested({.key = key, .value = item.value}));
                b.add("common_flags", item.name, item.value,
                      set   ? std::format("[{}] common_flags: {} does nothing while [{}] has `{}`, which wins on every "
                                          "connection; take it out of common_flags.",
                                          s, shown(item), s, key)
                      : bad ? std::format("[{}] common_flags: grab's own client refuses {} ({}). Take it out of "
                                          "common_flags, or write an `{}` line in [{}] (grab config).",
                                          s, shown(item), *bad, key, s)
                            : std::format("[{}] common_flags: grab reads {} as {} for now. Write that line in [{}] (grab "
                                          "config); common_flags is only for rclone and goes away with it.",
                                          s, shown(item), line, s));
            } else if (item.name == "--sftp-connections") {
                const std::string cap = suggested({.key = "max_connections", .value = item.value});
                b.add("common_flags", item.name, item.value,
                      std::format("[{}] common_flags: {} is for rclone only; to cap grab's connections to {}, set "
                                  "max_connections{} in [{}].",
                                  s, shown(item), s, cap.empty() ? std::string(" (1 to 64)") : cap, s));
            } else {
                b.add("common_flags", item.name, item.value,
                      std::format("[{}] common_flags: {} is for rclone only; {}.", s, shown(item), why_unused(item.name)));
            }
            continue;
        }
        b.add("common_flags", item.name, item.value,
              std::format("[{}] common_flags: {} is only for --rclone copies, which go with rclone; grab doesn't use "
                          "it{}.",
                          s, shown(item), instead_of(item.name)));
    }
    // folder_flags and file_flags
    for (const auto& [key, flags, defaults, batch] :
         {std::tuple{std::string("folder_flags"), &r.folder_flags, default_items(default_folder_flags), true},
          std::tuple{std::string("file_flags"), &r.file_flags, default_items(default_file_flags), false}}) {
        for (const auto& item : items_of(*flags)) {
            if (std::ranges::contains(defaults, item)) continue;
            if (rclone_flags::is_connection(item.name)) {
                // folder_flags' reach the batch of engine = rclone; file_flags' only --rclone -s.
                if (batch && !on_grab) continue;
                if (item.name == "--bind" && util::trim(item.value).empty()) continue; // no bind
                const auto as = key_for(item.name);
                const std::string reaches = batch ? "rclone's folder batch" : "--rclone -s copies";
                std::string text = std::format("[{}] {}: {} reaches only {}", s, key, shown(item), reaches);
                if (const auto bad = as.empty() ? std::nullopt : refused({.key = as, .value = item.value})) {
                    text += std::format("; for every connection write an `{}` line ({}).", as, *bad);
                } else if (!as.empty()) {
                    text += std::format("; write `{}{}` for every connection.", as, suggested({.key = as, .value = item.value}));
                } else {
                    text += ".";
                }
                b.add(key, item.name, item.value, std::move(text));
                continue;
            }
            b.add(key, item.name, item.value,
                  std::format("[{}] {}: {} is only for --rclone copies, which go with rclone; grab doesn't use it{}.", s,
                              key, shown(item), instead_of(item.name)));
        }
    }
}

// ssh.exe's options that take an argument.
bool takes_argument(char option) { return std::string_view("bcDEeFIiJLlmOopQRSWw").contains(option); }

bool digits(std::string_view v) {
    return !v.empty() && std::ranges::all_of(v, [](char c) { return c >= '0' && c <= '9'; });
}

void ssh_notices(const Builder& b, const RemoteSettings& r) {
    const std::string& s = r.name;
    const auto& args = r.ssh_options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg.size() < 2 || arg[0] != '-') continue;
        const char option = arg[1];
        std::string value;
        if (takes_argument(option)) {
            if (arg.size() > 2) value = arg.substr(2);
            else if (i + 1 < args.size()) value = args[++i];
        }
        if (option == 'J') continue; // the guard's
        std::string what = std::string("-") + option;
        std::string flag = what;
        if (option == 'o') {
            const auto sep = value.find_first_of("= \t");
            const std::string keyword(util::trim(std::string_view(value).substr(0, sep)));
            std::string_view v = sep == std::string::npos ? std::string_view() : util::trim(std::string_view(value).substr(sep + 1));
            if (v.starts_with('=')) v = util::trim(v.substr(1));
            const std::string lower = util::to_lower(keyword);
            if (lower == "proxycommand" || lower == "proxyjump") continue; // the guard's
            flag = "-o " + keyword;
            if (lower == "connecttimeout" && digits(v)) {
                what = std::format("-o {}={}: use connect_timeout = {}s", keyword, v, v);
            } else if (lower == "bindaddress" && plain(v)) {
                what = std::format("-o {}={}: use bind = {}", keyword, v, v);
            } else if ((lower == "serveraliveinterval" || lower == "serveralivecountmax") && digits(v)) {
                what = std::format("-o {}={}: grab sends its own keepalives", keyword, v);
            } else {
                what = flag;
            }
        }
        b.add("ssh_options", flag, value,
              std::format("[{}] ssh_options is for ssh.exe, which grab's own client doesn't use ({}).", s, what));
    }
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

void rclone_notices(const Builder& b, const std::string& s, const std::vector<std::pair<std::string, std::string>>& settings) {
    for (const auto& [key, raw] : settings) {
        const std::string_view value = util::trim(raw);
        const std::string name = "rclone." + key;
        std::string text;
        if (key == "skip_links" && go_bool(value) == true) {
            text = std::format("[{}] {} = true is for rclone only: grab's own client follows links.", s, name);
        } else if (key == "set_env" && !value.empty()) {
            text = std::format("[{}] {} is for rclone only: grab's own client sets no environment on the server.", s, name);
        } else if (key == "set_modtime" && go_bool(value) == false) {
            text = std::format("[{}] {} = false is for rclone only: grab's own client never changes the server's files.",
                               s, name);
        } else if (key == "connections" && !value.empty() && value != "0") {
            const std::string cap = suggested({.key = "max_connections", .value = value});
            text = std::format("[{}] {}{} is for rclone only; to cap grab's connections to {}, set max_connections{} in [{}].",
                               s, name, digits(value) ? std::format(" = {}", value) : "", s,
                               cap.empty() ? std::string(" (1 to 64)") : cap, s);
        } else if ((key == "chunk_size" && !value.empty() && value != "32Ki") ||
                   (key == "concurrency" && !value.empty() && value != "64")) {
            text = std::format("[{}] {}{} is for rclone only: grab's own client reads 255 KiB at a time, 64 in flight.", s,
                               name, plain(value) ? std::format(" = {}", value) : "");
        } else if (key == "idle_timeout" && !value.empty() && ssh::parse_duration(value) != std::chrono::seconds(60)) {
            text = std::format("[{}] {}{} is for rclone only: grab's own client closes idle connections itself.", s, name,
                               plain(value) ? std::format(" = {}", value) : "");
        } else {
            continue;
        }
        // Keyed on the text, not the value: set_env and the store's extras can hold a secret, and
        // notices.json must be no guessing oracle for it.
        b.add_keyed_on_text(name, "", std::move(text));
    }
}

} // namespace

std::vector<Notice> settings_notices(const ini::Document& doc, const GrabConfig& cfg, const Extras& extras,
                                     const Environment& environment, bool store_unreadable) {
    std::vector<Notice> out;
    for (const auto& r : cfg.remotes) {
        const Builder b{.out = out, .server = r.name};
        const Engine engine = effective_engine(r);
        const bool on_grab = engine == Engine::grab;
        flag_notices(b, r, on_grab);
        if (on_grab) ssh_notices(b, r);
        const auto* section = doc.find(r.name);
        if (section == nullptr) continue;
        const auto connection = server_conf::parse(*section);
        if (!connection || !*connection) continue; // no details yet: nothing to guard
        const auto& c = **connection;
        const auto found = extras.find(r.name);
        const auto& kept = found != extras.end() ? found->second : std::vector<std::pair<std::string, std::string>>{};
        if (on_grab) {
            rclone_notices(b, r.name, kept);
            rclone_notices(b, r.name, c.rclone);
        }
        auto sources = guard::sources(r, c, kept);
        sources.environment = environment;
        auto guarded = guard::evaluate(sources);
        if (store_unreadable) guard::add_unreadable_store(guarded);
        // Keyed on the text (kind, where, engine), never the value: a proxy can hold a password.
        for (const auto& reason : guard::reasons(guarded)) {
            b.add_keyed_on_text("guard", reason.where, guard::text(r.name, std::span(&reason, 1), engine));
        }
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
    auto out = settings_notices(
        *doc, *cfg, extras, [](const std::string& name) { return util::getenv_utf8(name.c_str()); }, !store);
    for (auto& [server, kept] : extras) {
        for (auto& [key, value] : kept) std::ranges::fill(value, '\0');
    }
    return out;
}

std::filesystem::path path_in(const std::filesystem::path& state_dir) { return state_dir / "notices.json"; }

namespace {

using Id = std::tuple<std::string, std::string, std::string, std::string>; // server, key, flag, digest

Id id_of(const Notice& n) { return {n.server, n.key, n.flag, n.digest}; }

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

} // namespace grab::notices
