#include "rclone_import.hpp"

#include "ini.hpp"
#include "json.hpp"
#include "servers.hpp"
#include "util.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <format>
#include <span>
#include <system_error>
#include <utility>

namespace grab::rclone_import {

namespace fs = std::filesystem;

namespace {

constexpr int record_format = 1;

// rclone 1.75.1's sftp settings, and `description`, which every remote has.
constexpr auto sftp_options = std::to_array<std::string_view>(
    {"host",           "user",                     "port",
     "pass",           "key_pem",                  "key_file",
     "key_file_pass",  "pubkey",                   "pubkey_file",
     "known_hosts_file", "pin_host_key",           "host_keys",
     "key_use_agent",  "use_insecure_cipher",      "disable_hashcheck",
     "ask_password",   "path_override",            "set_modtime",
     "shell_type",     "hashes",                   "md5sum_command",
     "sha1sum_command", "crc32sum_command",        "sha256sum_command",
     "blake3sum_command", "xxh3sum_command",       "xxh128sum_command",
     "skip_links",     "subsystem",                "server_command",
     "use_fstat",      "disable_concurrent_reads", "disable_concurrent_writes",
     "idle_timeout",   "chunk_size",               "concurrency",
     "connections",    "set_env",                  "ciphers",
     "key_exchange",   "macs",                     "host_key_algorithms",
     "ssh",            "socks_proxy",              "http_proxy",
     "copy_is_hardlink", "encoding",               "description"});

// Settings a dotted name never takes from its parent (review G86): secrets, and those that can
// hold one.
constexpr auto own_only = std::to_array<std::string_view>(
    {"pass", "key_file_pass", "key_pem", "pubkey", "ssh", "set_env", "socks_proxy", "http_proxy"});

// Settings the import turns into grab.conf keys or the login method, not rclone extras.
constexpr auto mapped = std::to_array<std::string_view>(
    {"host", "user", "port", "pass", "key_pem", "key_file", "key_file_pass", "known_hosts_file", "key_use_agent",
     "ask_password", "shell_type", "md5sum_command", "sha1sum_command", "path_override"});

bool one_of(std::span<const std::string_view> list, std::string_view key) { return std::ranges::find(list, key) != list.end(); }

// A key rclone reads from a file: its options are named in lower case with '_'.
bool option_name(std::string_view key) {
    return !key.empty() &&
           std::ranges::all_of(key, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

// Go's strconv.ParseBool, as rclone reads a bool setting.
std::optional<bool> go_bool(std::string_view s) {
    for (const std::string_view t : {"1", "t", "T", "TRUE", "true", "True"}) {
        if (s == t) return true;
    }
    for (const std::string_view f : {"0", "f", "F", "FALSE", "false", "False"}) {
        if (s == f) return false;
    }
    return std::nullopt;
}

std::optional<std::string> nonblank(const ini::Section& s, std::string_view key) {
    auto v = s.get(key);
    if (!v) return std::nullopt;
    const auto t = util::trim(*v);
    if (t.empty()) return std::nullopt;
    return std::string(t);
}

// The same file, as Windows names files: ignoring case.
bool same_file(const fs::path& a, const fs::path& b) {
    const auto norm = [](const fs::path& p) { return util::path_to_utf8(p.lexically_normal()); };
#ifdef _WIN32
    return util::to_lower(norm(a)) == util::to_lower(norm(b));
#else
    return norm(a) == norm(b);
#endif
}

// OpenSSH's match_pattern for a host name: '*' any run, '?' one character, ignoring case.
bool glob(std::string_view pattern, std::string_view text) {
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t star = std::string_view::npos;
    std::size_t mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || std::tolower(static_cast<unsigned char>(pattern[p])) ==
                                                            std::tolower(static_cast<unsigned char>(text[t])))) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

// A known_hosts host-list name OpenSSH reads as a pattern: wildcards, or a "!" before it.
bool is_pattern(std::string_view name) { return name.starts_with('!') || name.contains('*') || name.contains('?'); }

// Whether such a pattern matches host:port ("[host]:port", for port 22 the host).
bool pattern_matches(std::string_view name, std::string_view host, int port) {
    if (name.starts_with('!')) name.remove_prefix(1);
    return port == 22 ? glob(name, host) : glob(name, std::format("[{}]:{}", host, port));
}

// Whether a host-list name stands for host:port, as a name or as a pattern.
bool for_host(std::string_view name, std::string_view host, int port) {
    return is_pattern(name) ? pattern_matches(name, host, port) : servers::names_host(name, host, port);
}

std::vector<std::string_view> fields_of(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t at = 0;
    while (at < line.size()) {
        const auto b = line.find_first_not_of(" \t", at);
        if (b == std::string_view::npos) break;
        auto e = line.find_first_of(" \t", b);
        if (e == std::string_view::npos) e = line.size();
        out.push_back(line.substr(b, e - b));
        at = e;
    }
    return out;
}

std::vector<std::string_view> names_of(std::string_view list) {
    std::vector<std::string_view> out;
    for (std::size_t b = 0; b <= list.size();) {
        auto e = list.find(',', b);
        if (e == std::string_view::npos) e = list.size();
        if (e > b) out.push_back(list.substr(b, e - b));
        b = e + 1;
    }
    return out;
}

Stamp stamp_from(const json::Value& v) {
    Stamp s;
    s.file = v.string_of("file").value_or("");
    if (const auto* size = v.find("size"); size != nullptr && size->kind == json::Value::Kind::number) {
        s.size = static_cast<std::int64_t>(size->number);
    }
    if (const auto time = v.string_of("time")) {
        std::int64_t t = 0;
        const auto [end, ec] = std::from_chars(time->data(), time->data() + time->size(), t);
        if (ec == std::errc{} && end == time->data() + time->size()) s.time = t;
    }
    return s;
}

// One section's import: what it becomes, or why it can't be imported.
struct Outcome {
    Imported server;
    std::map<std::string, passwords::Secret, std::less<>> secrets;
    std::vector<std::string> host_key_lines;
    std::vector<std::string> notes;
};

class Importer {
public:
    explicit Importer(const Inputs& in) : in_(in), parsed_(in.sources.size()) {}

    // `name`'s section; `pinned_text` is grab's known_hosts with the lines copied so far.
    std::expected<Outcome, std::string> run(const ini::Section& section, const std::string& pinned_text) {
        Outcome out;
        out.server.name = section.name;
        const std::string remote = nonblank(section, "rclone_remote").value_or(section.name);

        // The remote, from the first source that has it.
        const Source* source = nullptr;
        const rclone_conf::Document* doc = nullptr;
        std::vector<std::string> looked;
        for (std::size_t i = 0; i < in_.sources.size() && source == nullptr; ++i) {
            const auto& s = in_.sources[i];
            const auto file = util::path_to_utf8(s.file);
            if (!s.error.empty()) return util::failf("{} can't be read: {}", file, s.error);
            if (!s.text) {
                looked.push_back(file);
                continue;
            }
            if (is_encrypted_rclone_config(*s.text)) {
                return util::failf("{} is encrypted, and grab can't read an encrypted rclone.conf", file);
            }
            const auto& parsed = parsed_[i] ? *parsed_[i] : parsed_[i].emplace(rclone_conf::parse(*s.text));
            if (!parsed) return util::failf("rclone can't read {} either: {}", file, parsed.error());
            if (parsed->find(remote) != nullptr) {
                source = &s;
                doc = &*parsed;
            } else {
                looked.push_back(file);
            }
        }
        if (source == nullptr) {
            return util::failf("there is no rclone remote [{}]{}", remote,
                               looked.empty() ? std::string() : " in " + util::join(looked, " or "));
        }
        out.server.from = source->file;
        const auto file = util::path_to_utf8(source->file);

        // Every setting rclone would read, a dotted name's missing ones from its parents.
        std::map<std::string, std::string, std::less<>> values;
        std::map<std::string, std::string, std::less<>> parents; // key -> the section it came from
        const auto read = [&](std::string_view key, bool inherit) -> std::expected<void, std::string> {
            auto v = rclone_conf::get(*doc, remote, key, inherit);
            if (!v) return util::failf("{}: {}", file, v.error());
            if (!*v) return {};
            if ((*v)->section != remote) parents[std::string(key)] = (*v)->section;
            values[std::string(key)] = std::move((*v)->text);
            return {};
        };
        if (auto ok = read("type", true); !ok) return std::unexpected(std::move(ok.error()));
        const auto type = values.find("type");
        if (type == values.end()) return util::failf("[{}] in {} has no type", remote, file);
        if (type->second != "sftp") return util::failf("[{}] in {} is a {} remote; grab needs sftp", remote, file, type->second);
        for (const auto key : sftp_options) {
            if (auto ok = read(key, !one_of(own_only, key)); !ok) return std::unexpected(std::move(ok.error()));
        }
        // Keys rclone doesn't know (a newer rclone's, or a mistake): kept as rclone extras, since
        // one could hold a password.
        std::vector<std::string> unknown;
        for (const auto& key : doc->find(remote)->keys) {
            if (key == "type" || one_of(sftp_options, key) || !option_name(key)) continue;
            if (auto ok = read(key, false); !ok) return std::unexpected(std::move(ok.error()));
            if (values.contains(key)) unknown.push_back(key);
        }
        const auto value = [&](std::string_view key) -> std::optional<std::string> {
            const auto it = values.find(key);
            if (it == values.end()) return std::nullopt;
            return it->second;
        };

        // Where it logs in.
        auto& c = out.server.connection;
        const auto host = value("host");
        if (!host) return util::failf("[{}] in {} has no host", remote, file);
        const auto user = value("user");
        if (!user) return util::failf("[{}] in {} has no user", remote, file);
        c.host = *host;
        c.user = *user;
        if (const auto port = value("port")) {
            int n = 0;
            const auto [end, ec] = std::from_chars(port->data(), port->data() + port->size(), n);
            if (ec != std::errc{} || end != port->data() + port->size() || server_conf::check_port(n)) {
                return util::failf("[{}] in {} has port '{}', not a number from 1 to 65535", remote, file, *port);
            }
            c.port = n;
        }
        bool agent = false;
        bool ask_password = false;
        for (const auto& [key, flag] : {std::pair{"key_use_agent", &agent}, std::pair{"ask_password", &ask_password}}) {
            if (const auto v = value(key)) {
                const auto b = go_bool(*v);
                if (!b) return util::failf("[{}] in {} has {} = '{}', which rclone can't read either", remote, file, key, *v);
                *flag = *b;
            }
        }

        // Secrets: the store beside the file, then the section's own lines, as grab read them.
        std::map<std::string, std::string, std::less<>> obscured;
        if (source->store) {
            if (const auto it = source->store->find(remote); it != source->store->end()) {
                for (const auto& [key, v] : it->second) {
                    if (!util::trim(v).empty()) obscured[key] = v;
                }
            }
        }
        for (const auto key : {"pass", "key_file_pass", "key_pem"}) {
            if (auto v = value(key)) obscured[key] = std::move(*v);
        }
        const auto secret = [&](std::string_view key) -> std::optional<std::string> {
            const auto it = obscured.find(key);
            if (it == obscured.end()) return std::nullopt;
            return it->second;
        };
        // A parent's secret line that rclone, reading the file, took for this remote. It isn't
        // taken (G86), but the user is told; the parent's store entry never counted (rclone's
        // per-remote env vars didn't fall back to a parent). A passphrase only matters with a key.
        const auto key_file = value("key_file");
        std::set<std::string, std::less<>> parent_secrets;
        for (const std::string_view key : {"pass", "key_file_pass", "key_pem"}) {
            if (secret(key) || (key == "key_file_pass" && !key_file && !secret("key_pem"))) continue;
            auto v = rclone_conf::get(*doc, remote, key, true);
            if (!v) return util::failf("{}: {}", file, v.error());
            if (!*v || (*v)->section == remote || util::trim((*v)->text).empty()) continue;
            parent_secrets.emplace(key);
            const std::string_view what = key == "pass" ? "password" : key == "key_file_pass" ? "key passphrase" : "key (key_pem)";
            out.notes.push_back(std::format("[{}]'s {} isn't taken for it (secrets aren't inherited): enter it with Edit",
                                            (*v)->section, what));
        }

        // The login method, in rclone's order.
        c.auth = [&] {
            if (agent) return server_conf::Auth::agent;
            if (secret("key_pem")) return server_conf::Auth::stored_key;
            if (key_file) return server_conf::Auth::key;
            if (secret("pass") || parent_secrets.contains("pass")) return server_conf::Auth::password;
            if (!ask_password) return server_conf::Auth::agent; // rclone's fallback
            out.notes.emplace_back("rclone asked for its password each time (ask_password), which grab can't do in "
                                   "the background: enter it with Edit");
            return server_conf::Auth::password;
        }();
        // Without the old store, a server that doesn't say it uses ssh-agent may have its password
        // or passphrase there: it waits until the store can be read.
        if (!source->store && !agent) {
            return util::failf("{} (its password stays there until it can be read)", source->store.error());
        }

        // Paths as rclone expanded them, made absolute.
        const auto dir = source->file.parent_path().lexically_normal();
        const auto absolute = [&](std::string_view key, const std::string& raw) {
            auto p = util::path_from_utf8(rclone_conf::expand_path(raw, dir, in_.getenv));
            if (!p.is_absolute()) {
                out.notes.push_back(std::format("its {} '{}' doesn't say which folder it is in; taken as in {}", key, raw,
                                                util::path_to_utf8(dir)));
                p = dir / p;
            }
            return util::path_to_utf8(p.lexically_normal());
        };
        if (key_file && (c.auth == server_conf::Auth::agent || c.auth == server_conf::Auth::key)) {
            c.key_file = absolute("key_file", *key_file);
            if (network_or_device(*c.key_file)) {
                out.notes.push_back(std::format("its key file {} is on a network or device path, not checked", *c.key_file));
            } else if (in_.exists && !in_.exists(util::path_from_utf8(*c.key_file)) &&
                       !(c.auth == server_conf::Auth::agent && in_.exists(util::path_from_utf8(*c.key_file + ".pub")))) {
                out.notes.push_back(std::format("its key file {} isn't there", *c.key_file));
            }
        }
        c.shell_type = value("shell_type");
        c.md5sum_command = value("md5sum_command");
        c.sha1sum_command = value("sha1sum_command");
        c.path_override = value("path_override");

        // rclone settings: the allowlisted ones in grab.conf, the rest as extras in the store.
        const passwords::Binding login{.user = c.user, .host = c.host, .port = c.port};
        const auto keep = [&](std::string_view key, std::string v) {
            out.secrets[std::string(key)] = passwords::Secret{.binding = login, .value = std::move(v)};
        };
        for (const auto key : sftp_options) {
            const auto v = value(key);
            if (!v || one_of(mapped, key)) continue;
            if (server_conf::rclone_key_allowed(key)) {
                c.rclone.emplace_back(key, key == "pubkey_file" ? absolute(key, *v) : *v);
            } else {
                keep(std::string(passwords::rclone_prefix) + std::string(key), *v);
            }
        }
        for (const auto& key : unknown) keep(std::string(passwords::rclone_prefix) + key, values[key]);
        std::ranges::sort(c.rclone); // as grab reads them back (by key)

        if (const auto problems = server_conf::check(section.name, c); !problems.empty()) return util::fail(problems.front());

        // The secrets, plain: rclone keeps pass and key_file_pass obscured, key_pem as it is.
        for (const auto& [key, as] : {std::pair{"pass", passwords::password}, std::pair{"key_file_pass", passwords::passphrase}}) {
            const auto o = secret(key);
            if (!o) continue;
            auto plain = secrets::reveal(*o);
            if (!plain) {
                out.notes.push_back(std::format("its saved {} can't be read (it isn't in rclone's obscured form): enter "
                                                "it again with Edit",
                                                as == passwords::password ? "password" : "key passphrase"));
            } else if (!plain->empty()) {
                keep(as, std::move(*plain));
            }
        }
        if (c.auth == server_conf::Auth::stored_key) keep(passwords::key_pem, *secret("key_pem"));
        out.server.password_saved = out.secrets.contains(passwords::password);

        // Host keys (decision 2): the remote's own known_hosts lines for [host]:port.
        out.server.pinned = server_conf::pinned(pinned_text, c.host, c.port);
        const auto kh = value("known_hosts_file");
        if (!out.server.pinned && kh && *kh != "none") {
            const auto path = absolute("known_hosts_file", *kh);
            if (same_file(util::path_from_utf8(path), in_.known_hosts)) {
                // grab's own, which has none for it.
            } else if (network_or_device(path)) {
                out.notes.push_back(std::format("its host keys in {} weren't read (a network or device path): use Trust", path));
            } else {
                const auto read_keys = in_.read_host_keys ? in_.read_host_keys(util::path_from_utf8(path))
                                                          : HostKeysFile{.text = std::nullopt, .error = "not read"};
                if (!read_keys.text) {
                    out.notes.push_back(std::format("its host keys in {} weren't read ({}): use Trust", path, read_keys.error));
                } else {
                    auto found = host_key_lines(*read_keys.text, c.host, c.port);
                    for (const auto& skipped : found.skipped) {
                        out.notes.push_back(std::format("{} for it in {} isn't copied: use Trust", skipped, path));
                    }
                    out.host_key_lines = std::move(found.lines);
                    out.server.pinned = !out.host_key_lines.empty();
                }
            }
        }
        if (!out.server.pinned) {
            out.notes.push_back(std::format("its host key isn't trusted yet: use Trust (grab server trust {})", section.name));
        }

        // Which keys came from a parent section (G86), by section.
        std::map<std::string, std::vector<std::string>> by_parent;
        for (const auto& [key, parent] : parents) {
            if (key != "type") out.server.from_parent.push_back(key);
            by_parent[parent].push_back(key);
        }
        for (const auto& [parent, keys] : by_parent) {
            out.notes.push_back(std::format("{} came from [{}], as rclone reads [{}]", util::join(keys, ", "), parent, remote));
        }
        return out;
    }

private:
    const Inputs& in_;
    std::vector<std::optional<std::expected<rclone_conf::Document, std::string>>> parsed_;
};

} // namespace

fs::path record_path(const fs::path& state_dir) { return state_dir / "imported.json"; }

std::string to_json(const Record& record) {
    auto root = json::Value::make_object();
    root.set("format", json::Value::make_number(record_format));
    auto imported = json::Value::make_array();
    for (const auto& name : record.imported) imported.push(json::Value::make_string(name));
    root.set("imported", std::move(imported));
    auto failed = json::Value::make_object();
    for (const auto& [name, failure] : record.failed) {
        auto list = json::Value::make_array();
        for (const auto& s : failure.stamps) {
            auto stamp = json::Value::make_object();
            stamp.set("file", json::Value::make_string(s.file));
            stamp.set("size", json::Value::make_number(static_cast<double>(s.size)));
            stamp.set("time", json::Value::make_string(std::to_string(s.time))); // exact, beyond a double's 53 bits
            list.push(std::move(stamp));
        }
        auto entry = json::Value::make_object();
        entry.set("remote", json::Value::make_string(failure.remote));
        entry.set("stamps", std::move(list));
        failed.set(name, std::move(entry));
    }
    root.set("failed", std::move(failed));
    auto banner = json::Value::make_array();
    for (const auto& name : record.banner) banner.push(json::Value::make_string(name));
    root.set("banner", std::move(banner));
    return json::stringify(root) + "\n";
}

std::optional<Record> from_json(std::string_view text) {
    const auto doc = json::parse(text);
    if (!doc || doc->kind != json::Value::Kind::object) return std::nullopt;
    const auto* format = doc->find("format");
    if (format == nullptr || format->kind != json::Value::Kind::number || format->number != record_format) {
        return std::nullopt;
    }
    Record r;
    if (const auto* imported = doc->find("imported"); imported != nullptr && imported->kind == json::Value::Kind::array) {
        for (const auto& v : imported->items) {
            if (v.kind == json::Value::Kind::string) r.imported.insert(v.str);
        }
    }
    if (const auto* failed = doc->find("failed"); failed != nullptr && failed->kind == json::Value::Kind::object) {
        for (std::size_t i = 0; i < failed->keys.size(); ++i) {
            const auto& entry = failed->items[i];
            if (entry.kind != json::Value::Kind::object) continue;
            auto& failure = r.failed[failed->keys[i]];
            failure.remote = entry.string_of("remote").value_or("");
            if (const auto* stamps = entry.find("stamps"); stamps != nullptr) {
                for (const auto& s : stamps->items) {
                    if (s.kind == json::Value::Kind::object) failure.stamps.push_back(stamp_from(s));
                }
            }
        }
    }
    if (const auto* banner = doc->find("banner"); banner != nullptr && banner->kind == json::Value::Kind::array) {
        for (const auto& v : banner->items) {
            if (v.kind == json::Value::Kind::string) r.banner.push_back(v.str);
        }
    }
    return r;
}

std::vector<fs::path> source_files(const GrabConfig& cfg) {
    if (cfg.rclone_config) return {*cfg.rclone_config};
    return {grab_rclone_config_path(), standard_rclone_config_path()};
}

bool network_or_device(std::string_view path) {
    const auto sep = [](char c) { return c == '/' || c == '\\'; };
    if (path.size() >= 2 && sep(path[0]) && sep(path[1])) return true;
    // NT's own namespace: a separator, "??", a separator (UNC, GLOBALROOT, devices, drives), which
    // Win32 passes on as it is.
    if (path.size() >= 4 && sep(path[0]) && path[1] == '?' && path[2] == '?' && sep(path[3])) return true;
    // A device name as the last part ("NUL", "COM1.txt"), which Windows opens as the device.
    const auto last = path.find_last_of("/\\:");
    auto name = path.substr(last == std::string_view::npos ? 0 : last + 1);
    name = name.substr(0, name.find('.'));
    while (name.ends_with(' ')) name.remove_suffix(1);
    const auto lower = util::to_lower(name);
    for (const std::string_view device : {"con", "prn", "aux", "nul", "conin$", "conout$"}) {
        if (lower == device) return true;
    }
    return (lower.starts_with("com") || lower.starts_with("lpt")) && lower.size() == 4 && lower[3] >= '0' &&
           lower[3] <= '9';
}

HostKeyLines host_key_lines(std::string_view text, std::string_view host, int port) {
    HostKeyLines out;
    const auto each_line = [&](const auto& fn) {
        std::size_t start = 0;
        while (start < text.size()) {
            auto end = text.find('\n', start);
            if (end == std::string_view::npos) end = text.size();
            const auto line = util::trim(text.substr(start, end - start));
            start = end + 1;
            if (!line.empty() && line.front() != '#') fn(line, fields_of(line));
        }
    };
    // Keys a @revoked line revokes, whatever its host list: Go's knownhosts (rclone's) refuses
    // them for every host.
    std::set<std::string_view, std::less<>> revoked;
    each_line([&](std::string_view, const std::vector<std::string_view>& fields) {
        if (fields.size() >= 4 && fields[0] == "@revoked") revoked.insert(fields[3]);
    });
    each_line([&](std::string_view line, const std::vector<std::string_view>& fields) {
        if (line.front() == '@') {
            if (fields.size() >= 2 && fields[0] != "@revoked" &&
                std::ranges::any_of(names_of(fields[1]), [&](std::string_view n) { return for_host(n, host, port); })) {
                out.skipped.push_back(std::format("a {} line", fields[0]));
            }
            return;
        }
        if (fields.size() < 3) return;
        std::vector<std::string> own;
        bool wildcard = false;
        bool negated = false;
        for (const auto n : names_of(fields[0])) {
            if (!for_host(n, host, port)) continue;
            if (!is_pattern(n)) own.emplace_back(n);
            else if (n.starts_with('!')) negated = true;
            else wildcard = true;
        }
        if (negated) return; // OpenSSH: a matching "!pattern" means the line isn't for this host
        if (!own.empty() && revoked.contains(fields[2])) {
            out.skipped.emplace_back("a line with a key that file revokes");
        } else if (!own.empty()) {
            out.lines.push_back(util::join(own, ",") + std::string(line.substr(fields[0].size())));
        } else if (wildcard) {
            out.skipped.push_back(std::format("a wildcard line ({})", fields[0]));
        }
    });
    return out;
}

bool Plan::changes(const Inputs& in) const {
    return grab_text != in.grab_text || store != in.store || !host_key_lines.empty() || record != in.record;
}

std::expected<Plan, std::string> plan(const Inputs& in) {
    const auto doc = ini::parse(in.grab_text);
    if (!doc) return util::failf("grab.conf: {}", doc.error());
    Plan p{.grab_text = in.grab_text, .store = in.store, .host_key_lines = {}, .record = in.record, .imported = {},
           .failed = {}, .notes = {}};
    std::vector<std::string> names;
    for (const auto& s : doc->sections) {
        if (!s.name.empty() && s.name != "grab") names.push_back(s.name);
    }
    std::string pinned_text = in.known_hosts_text;
    Importer importer(in);
    for (const auto& s : doc->sections) {
        if (s.name.empty() || s.name == "grab" || nonblank(s, "host")) continue; // grab.conf's own details win
        if (p.record.imported.contains(s.name)) continue;
        // A failed one again only when grab.conf names another remote or a source file changed.
        const auto remote = nonblank(s, "rclone_remote").value_or(s.name);
        if (const auto f = p.record.failed.find(s.name);
            f != p.record.failed.end() && f->second.remote == remote && f->second.stamps == in.stamps) {
            continue;
        }
        auto outcome = importer.run(s, pinned_text);
        if (!outcome) {
            p.failed.push_back(s.name);
            p.record.failed[s.name] = Failure{.remote = remote, .stamps = in.stamps};
            p.notes.push_back(Note{.server = s.name, .text = "not imported: " + outcome.error()});
            continue;
        }
        // Each key set after the header in turn, so they end up in the order of `lines`.
        const auto lines = server_conf::lines(outcome->server.connection);
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
            p.grab_text = servers::set_value(p.grab_text, s.name, it->first, it->second);
        }
        auto& stored = p.store[s.name];
        stored.clear();
        for (auto& [key, secret] : outcome->secrets) stored[key] = std::move(secret);
        if (stored.empty()) p.store.erase(s.name);
        for (auto& line : outcome->host_key_lines) {
            pinned_text += "\n" + line;
            p.host_key_lines.push_back(std::move(line));
        }
        for (auto& problem : server_conf::name_problems(s.name, names)) outcome->notes.push_back(std::move(problem));
        for (auto& note : outcome->notes) p.notes.push_back(Note{.server = s.name, .text = std::move(note)});
        p.record.imported.insert(s.name);
        p.record.failed.erase(s.name);
        p.record.banner.push_back(s.name);
        p.imported.push_back(std::move(outcome->server));
    }
    return p;
}

} // namespace grab::rclone_import
