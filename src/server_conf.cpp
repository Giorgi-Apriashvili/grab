#include "server_conf.hpp"

#include "rclone_encoding.hpp"
#include "servers.hpp"
#include "util.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <format>
#include <system_error>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (CompareStringOrdinal)
#endif

namespace grab::server_conf {

namespace {

// rclone 1.75.1's sftp settings that hold no secret and that grab passes on as they are
// (RCLONE-REPLACEMENT.md, phase 7 engineering defaults). md5sum_command, sha1sum_command,
// shell_type and path_override are grab.conf keys of their own.
constexpr auto allowed_rclone_keys = std::to_array<std::string_view>(
    {"use_insecure_cipher", "disable_hashcheck", "set_modtime", "hashes", "crc32sum_command", "sha256sum_command",
     "blake3sum_command", "xxh3sum_command", "xxh128sum_command", "skip_links", "subsystem", "server_command",
     "use_fstat", "disable_concurrent_reads", "disable_concurrent_writes", "idle_timeout", "chunk_size",
     "concurrency", "connections", "ciphers", "key_exchange", "macs", "host_key_algorithms", "pubkey_file",
     "copy_is_hardlink", "description"});

std::optional<std::string> nonblank(const ini::Section& s, std::string_view key) {
    auto v = s.get(key);
    if (!v) return std::nullopt;
    const auto t = util::trim(*v);
    if (t.empty()) return std::nullopt;
    return std::string(t);
}

std::size_t characters(std::string_view s) {
    return static_cast<std::size_t>(std::ranges::count_if(s, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

bool host_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_';
}

bool hex_char(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

} // namespace

std::string_view auth_name(Auth auth) {
    switch (auth) {
    case Auth::agent: return "agent";
    case Auth::key: return "key";
    case Auth::password: return "password";
    case Auth::stored_key: return "stored_key";
    }
    return "agent";
}

std::optional<Auth> parse_auth(std::string_view text) {
    for (const Auth a : {Auth::agent, Auth::key, Auth::password, Auth::stored_key}) {
        if (util::to_lower(util::trim(text)) == auth_name(a)) return a;
    }
    return std::nullopt;
}

bool rclone_key_allowed(std::string_view key) { return std::ranges::find(allowed_rclone_keys, key) != allowed_rclone_keys.end(); }

std::expected<std::optional<Connection>, std::string> parse(const ini::Section& section) {
    const auto& name = section.name;
    auto host = nonblank(section, "host");
    if (!host) return std::nullopt;
    Connection c;
    c.host = std::move(*host);
    c.user = nonblank(section, "user").value_or("");
    if (auto port = nonblank(section, "port")) {
        int n = 0;
        const auto [end, ec] = std::from_chars(port->data(), port->data() + port->size(), n);
        if (ec != std::errc{} || end != port->data() + port->size()) {
            return util::failf("[{}] port must be a number from 1 to 65535, got '{}'", name, *port);
        }
        c.port = n;
    }
    const auto auth = nonblank(section, "auth");
    if (!auth) {
        return util::failf("[{}] has a host but no login method: add auth = agent, key, password or stored_key (or "
                           "change the server with Edit in grab's Settings)",
                           name);
    }
    const auto method = parse_auth(*auth);
    if (!method) return util::failf("[{}] auth must be agent, key, password or stored_key, got '{}'", name, *auth);
    c.auth = *method;
    c.key_file = nonblank(section, "key_file");
    if (c.auth == Auth::key && !c.key_file) return util::failf("[{}] auth = key needs a key_file", name);
    if (c.key_file && c.auth != Auth::key && c.auth != Auth::agent) {
        return util::failf("[{}] key_file goes with auth = key or agent, not {}", name, auth_name(c.auth));
    }
    c.shell_type = nonblank(section, "shell_type");
    c.md5sum_command = nonblank(section, "md5sum_command");
    c.sha1sum_command = nonblank(section, "sha1sum_command");
    c.path_override = nonblank(section, "path_override");
    for (const auto& [key, value] : section.values) {
        if (!key.starts_with(rclone_prefix)) continue;
        const auto option = std::string_view(key).substr(rclone_prefix.size());
        if (!rclone_key_allowed(option)) {
            return util::failf("[{}] {} can't be set in grab.conf (grab gives rclone only the settings that hold no "
                               "password; change the server with Edit in grab's Settings)",
                               name, key);
        }
        const auto v = util::trim(value);
        if (!v.empty()) c.rclone.emplace_back(option, v);
    }
    if (const auto problems = check(name, c); !problems.empty()) return util::fail(problems.front());
    return c;
}

std::vector<std::pair<std::string, std::string>> lines(const Connection& c) {
    std::vector<std::pair<std::string, std::string>> out{
        {"host", c.host}, {"port", std::to_string(c.port)}, {"user", c.user}, {"auth", std::string(auth_name(c.auth))}};
    const auto add = [&](std::string_view key, const std::optional<std::string>& value) {
        if (value && !value->empty()) out.emplace_back(key, *value);
    };
    add("key_file", c.key_file);
    add("shell_type", c.shell_type);
    add("md5sum_command", c.md5sum_command);
    add("sha1sum_command", c.sha1sum_command);
    add("path_override", c.path_override);
    for (const auto& [key, value] : c.rclone) {
        if (!value.empty()) out.emplace_back(std::string(rclone_prefix) + key, value);
    }
    return out;
}

RcloneRemote to_remote(std::string_view name, const Connection& c, const std::filesystem::path& known_hosts) {
    RcloneRemote r;
    r.name = std::string(name);
    r.host = c.host;
    r.user = c.user;
    r.port = c.port;
    if (c.auth == Auth::key || c.auth == Auth::agent) r.key_file = c.key_file;
    r.key_use_agent = c.auth == Auth::agent;
    r.known_hosts_file = util::path_to_utf8(known_hosts);
    r.shell_type = c.shell_type;
    r.md5sum_command = c.md5sum_command;
    r.path_override = c.path_override;
    return r;
}

// ---- checks ---------------------------------------------------------------------------------

std::optional<std::string> check_value(std::string_view value) {
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto c = static_cast<unsigned char>(value[i]);
        // C0 controls and DEL, and C1 controls (U+0080..U+009F, "\xC2\x80".."\xC2\x9F").
        const bool c1 = c == 0xC2 && i + 1 < value.size() && static_cast<unsigned char>(value[i + 1]) >= 0x80 &&
                        static_cast<unsigned char>(value[i + 1]) <= 0x9F;
        if (c < 0x20 || c == 0x7F || c1) return "it can't hold control characters or line breaks";
    }
    return std::nullopt;
}

std::optional<std::string> check_host(std::string_view host) {
    if (host.empty()) return "a host is required";
    if (auto bad = check_value(host)) return bad;
    if (host.front() == '-') return "the host can't start with -";
    if (host.size() > 253) return "use at most 253 characters";
    // An IPv6 address (without brackets; "%zone" allowed), else a host name or an IPv4 address.
    if (host.contains(':')) {
        const auto zone = host.find('%');
        const auto address = host.substr(0, zone);
        const bool ok = std::ranges::all_of(address, [](char c) { return hex_char(c) || c == ':' || c == '.'; }) &&
                        (zone == std::string_view::npos ||
                         (zone + 1 < host.size() && std::ranges::all_of(host.substr(zone + 1), host_char)));
        if (ok) return std::nullopt;
        return "use an IPv6 address without brackets, or a host name";
    }
    if (host.front() == '.' || !std::ranges::all_of(host, host_char)) {
        return "use a host name (letters, digits, - and .) or an IP address";
    }
    return std::nullopt;
}

std::optional<std::string> check_user(std::string_view user) {
    if (user.empty()) return "a user is required";
    if (auto bad = check_value(user)) return bad;
    if (user.front() == '-') return "the user can't start with -";
    return std::nullopt;
}

std::optional<std::string> check_port(int port) {
    if (port < 1 || port > 65535) return "the port must be from 1 to 65535";
    return std::nullopt;
}

std::optional<std::string> check_key_file(std::string_view path, bool exists) {
    if (path.empty()) return "a key file is required";
    if (auto bad = check_value(path)) return bad;
    if (!exists) return std::format("there is no file {}", path);
    return std::nullopt;
}

std::optional<std::string> check_search_root(std::string_view root) {
    if (auto bad = check_value(root)) return bad;
    if (root.contains(',')) return "a folder with ',' in its name can't be a search folder (',' separates them)";
    return std::nullopt;
}

std::vector<std::string> check(std::string_view name, const Connection& c) {
    std::vector<std::string> out;
    const auto add = [&](std::string_view key, const std::optional<std::string>& problem) {
        if (problem) out.push_back(std::format("[{}] {}: {}", name, key, *problem));
    };
    add("host", check_host(c.host));
    add("port", check_port(c.port));
    add("user", check_user(c.user));
    if (c.key_file) add("key_file", check_value(*c.key_file));
    add("shell_type", c.shell_type ? check_value(*c.shell_type) : std::nullopt);
    add("md5sum_command", c.md5sum_command ? check_value(*c.md5sum_command) : std::nullopt);
    add("sha1sum_command", c.sha1sum_command ? check_value(*c.sha1sum_command) : std::nullopt);
    add("path_override", c.path_override ? check_value(*c.path_override) : std::nullopt);
    for (const auto& [key, value] : c.rclone) {
        const auto line = std::string(rclone_prefix) + key;
        if (!rclone_key_allowed(key)) add(line, "not a setting grab.conf can hold");
        else add(line, check_value(value));
    }
    return out;
}

bool same_ignoring_case(std::string_view a, std::string_view b) {
    if (rclone_enc::go_upper(a) == rclone_enc::go_upper(b)) return true;
#ifdef _WIN32
    const auto wa = util::to_wide(a);
    const auto wb = util::to_wide(b);
    return CompareStringOrdinal(wa.data(), static_cast<int>(wa.size()), wb.data(), static_cast<int>(wb.size()), TRUE) ==
           CSTR_EQUAL;
#else
    return false;
#endif
}

std::optional<std::string> check_new_name(std::string_view name, const std::vector<std::string>& sections) {
    if (auto bad = servers::validate_name(name)) return bad;
    if (characters(name) < 2) return "use at least 2 characters (rclone takes one letter for a drive)";
    for (const auto& other : sections) {
        if (other == name) return std::format("there is a server called {} already", name);
        if (same_ignoring_case(other, name)) {
            return std::format("{} differs from the server {} only in case, which Windows doesn't tell apart", name,
                               other);
        }
    }
    return std::nullopt;
}

std::vector<std::string> name_problems(std::string_view name, const std::vector<std::string>& sections) {
    std::vector<std::string> out;
    if (characters(name) == 1) out.emplace_back("its name is one character, which rclone takes for a drive letter");
    if (name.starts_with('-')) out.emplace_back("its name starts with -, which programs take for an option");
    for (const auto& other : sections) {
        if (other != name && same_ignoring_case(other, name)) {
            out.push_back(std::format("its name differs from [{}] only in case, so Windows gives both one file list",
                                      other));
        }
    }
    return out;
}

bool pinned(std::string_view known_hosts, std::string_view host, int port) {
    std::size_t start = 0;
    while (start < known_hosts.size()) {
        auto end = known_hosts.find('\n', start);
        if (end == std::string_view::npos) end = known_hosts.size();
        const auto line = util::trim(known_hosts.substr(start, end - start));
        start = end + 1;
        if (line.empty() || line.front() == '#' || line.front() == '@') continue;
        // The host list, then the key's type and the key.
        const auto names_end = line.find_first_of(" \t");
        if (names_end == std::string_view::npos) continue;
        const auto rest = util::trim(line.substr(names_end));
        if (rest.find_first_of(" \t") == std::string_view::npos) continue;
        for (const auto& name : util::split(line.substr(0, names_end), ',')) {
            if (servers::names_host(name, host, port)) return true;
        }
    }
    return false;
}

} // namespace grab::server_conf
