#include "config.hpp"

#include "conn_budget.hpp"
#include "server_conf.hpp"
#include "ssh_target.hpp"
#include "util.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <format>
#include <optional>
#include <utility>

namespace grab {

const std::string_view example_config = R"(# grab.conf - settings for the `grab` CLI
#
# Location: %APPDATA%\grab\grab.conf  (override with --config PATH or the GRAB_CONFIG env var)
# Same INI dialect as rclone.conf: [section], key = value, comment lines start with ; or #.
# Comments must be on their own line; a ; or # after a value is part of the value.
#
# Each server's section holds its connection details (host, port, user, how it logs in).
# Passwords and key passphrases are NOT stored here: grab keeps them in its own encrypted
# store (%APPDATA%\grab\passwords), and host keys in %APPDATA%\grab\known_hosts. Add servers
# with `grab server add`, change them with `grab server edit NAME` (or grab's Settings).

[grab]
# rclone executable. Blank = the rclone.exe bundled with grab (else rclone on PATH);
# a full path uses that one instead.
rclone =
# The rclone.conf grab 0.19 and before kept servers in: a section without `host` takes its
# details from the remote there once (grab only reads it, never writes it). Blank = grab's
# own (%APPDATA%\grab\rclone.conf), then rclone's standard one.
rclone_config =
# ssh executable used for the remote `find`. Blank = `ssh` on PATH (Windows OpenSSH).
ssh = ssh
# Editor for `grab config`, e.g.  code --wait   Blank = $VISUAL, then $EDITOR, then notepad.
editor =
# Remote section to use when -r/--remote is not given. Optional when only one remote exists.
default_remote = hetzner

# One section per server. The section name is what you pass to -r/--remote.
[hetzner]
# The server's address, SSH port (blank = 22) and login.
host = files.example.com
port = 22
user = alice
# How to log in: agent (the keys in Windows' ssh-agent; with key_file, only that one), key
# (key_file; its passphrase, if any, in grab's store), password (in grab's store: enter it
# with `grab server edit hetzner`) or stored_key (a key imported from rclone.conf, kept in
# grab's store with its passphrase).
auth = key
key_file = C:\Users\alice\.ssh\id_ed25519
# What the server told grab's Test, so rclone needn't ask it again on every run: its shell
# (unix, cmd, powershell, none) and its checksum commands. Left out = not known yet.
# shell_type = unix
# md5sum_command = md5sum
# A non-secret rclone sftp setting for this server's rclone runs (engine = rclone), e.g.
# rclone.ciphers = aes256-ctr
# The rclone remote grab 0.19 and before used for this server (default: this section's name).
# Kept in sections imported from rclone.conf: their partial downloads and 0.19 go by it.
# rclone_remote = hetzner
# How TARGET is located: auto = ssh + find when the server logs in with a key file or
# ssh-agent, otherwise an rclone listing (password servers and storage boxes have no shell).
# Force with ssh | rclone.
# With engine = grab: auto = find where grab's check finds GNU find on the server, else by
# listing folders (Storage Boxes always); ssh = find only; rclone or walk = listing folders.
find = auto
# Comma-separated directories to search: absolute (/srv) or relative to the login home
# (learning). Blank or . = the home directory itself.
search_roots = /home/alice
# How deep below each root `find` may look (find -maxdepth).
max_depth = 4
# Connections grab-gui's downloads may open to this server at once. Blank = automatic
# (14 on Hetzner Storage Boxes, 12 elsewhere, lowered when the server refuses connections).
max_connections =
# Skip dot-directories such as .cache and .config while searching.
skip_hidden = true
# Extra raw ssh arguments, e.g.  -o ServerAliveInterval=30
ssh_options =
# rclone flags for --rclone copies; grab's own downloads, listings, previews, Play and checks
# use only the connection ones (--sftp-*, --bind, --timeout...). 255Ki is the largest SFTP packet OpenSSH accepts and
# cuts round trips roughly 8x compared with the 32Ki default. --sftp-disable-hashcheck skips
# the md5sum rclone would otherwise run on the server after each of its own copies (minutes
# for a 30 GB file); SSH already guarantees integrity in flight. To check downloads against
# the server's copy, turn on Verify downloads (or grab --verify): grab's own check, which no
# rclone flag changes.
common_flags = -P --sftp-chunk-size 255Ki --sftp-disable-hashcheck
# rclone flags for -f/--folder (rclone copy). More transfers hide per-file round trips on
# trees of small files; 8 keeps connections (transfers x multi-thread streams) modest.
folder_flags = --transfers 8 --checkers 8
# rclone flags for -s/--file (rclone copyto). One big object: parallel range reads. Keep the
# chunk size large: 8Mi chunks measured ~2x slower than 64Mi (each chunk re-opens the file).
file_flags = --multi-thread-streams 8 --multi-thread-cutoff 64Mi --multi-thread-chunk-size 64Mi
)";

namespace {

[[nodiscard]] std::optional<std::string> nonblank(const ini::Section& s, std::string_view key) {
    auto v = s.get(key);
    if (!v) return std::nullopt;
    const auto t = util::trim(*v);
    if (t.empty()) return std::nullopt;
    return std::string(t);
}

[[nodiscard]] std::vector<std::string> flag_list(const ini::Section& s, std::string_view key,
                                                 std::string_view fallback) {
    // Present-but-empty means "no flags"; absent means "use the built-in default".
    if (auto v = s.get(key)) return util::split_args(*v);
    return util::split_args(fallback);
}

[[nodiscard]] std::expected<int, std::string> int_value(const ini::Section& s, std::string_view key,
                                                        int fallback) {
    auto v = nonblank(s, key);
    if (!v) return fallback;
    int out = 0;
    const auto [ptr, ec] = std::from_chars(v->data(), v->data() + v->size(), out);
    if (ec != std::errc{} || ptr != v->data() + v->size()) {
        return util::failf("[{}] {} must be an integer, got '{}'", s.name, key, *v);
    }
    return out;
}

[[nodiscard]] std::expected<bool, std::string> bool_value(const ini::Section& s, std::string_view key,
                                                          bool fallback) {
    auto v = nonblank(s, key);
    if (!v) return fallback;
    const std::string lower = util::to_lower(*v);
    if (lower == "true" || lower == "yes" || lower == "on" || lower == "1") return true;
    if (lower == "false" || lower == "no" || lower == "off" || lower == "0") return false;
    return util::failf("[{}] {} must be true/false, got '{}'", s.name, key, *v);
}

// The keys grab reads in [grab] and in a server's section (server_conf::keys and its rclone.<key>
// lines aside); any other is named in a notice (N7), never an error.
constexpr std::array grab_keys{std::string_view("rclone"), std::string_view("rclone_config"), std::string_view("ssh"),
                               std::string_view("editor"), std::string_view("default_remote")};
constexpr std::array server_keys{
    std::string_view("rclone_remote"),   std::string_view("search_roots"), std::string_view("find"),
    std::string_view("max_depth"),       std::string_view("max_connections"), std::string_view("skip_hidden"),
    std::string_view("engine"),          std::string_view("bind"),         std::string_view("connect_timeout"),
    std::string_view("io_timeout"),      std::string_view("ssh_options"),  std::string_view("common_flags"),
    std::string_view("folder_flags"),    std::string_view("file_flags")};

bool known_server_key(std::string_view key) {
    return std::ranges::contains(server_keys, key) || std::ranges::contains(server_conf::keys, key) ||
           key.starts_with(server_conf::rclone_prefix);
}

// A time key (connect_timeout, io_timeout) as ssh::parse_duration reads it; nullopt when absent.
std::expected<std::optional<std::chrono::milliseconds>, std::string> time_value(const ini::Section& s,
                                                                                 std::string_view key, bool off) {
    auto v = nonblank(s, key);
    if (!v) return std::nullopt;
    const auto t = ssh::parse_duration(*v);
    if (off) {
        if (t) return t; // 0 (off) included
        return util::failf("[{}] {}: not a time grab can read (such as 30s, 1m30s or off)", s.name, key);
    }
    if (t && *t >= min_io_timeout && *t <= max_io_timeout) return t;
    return util::failf("[{}] {}: not a time grab can read (such as 30s or 1m30s; 1s to 24h)", s.name, key);
}

} // namespace

std::string normalize_root(std::string_view root) {
    auto r = util::trim(root);
    if (r == "." || r == "~" || r == "~/") return {};
    if (r.starts_with("~/")) r.remove_prefix(2);
    if (r.starts_with("./")) r.remove_prefix(2);
    while (r.size() > 1 && r.ends_with('/')) r.remove_suffix(1);
    return std::string(r);
}

std::string search_roots_value(const std::vector<std::string>& roots) {
    std::vector<std::string> shown;
    for (const auto& r : roots) shown.push_back(r.empty() ? "~" : r);
    return util::join(shown, ", ");
}

FindMethod resolve_find_method(const RemoteSettings& settings, const RcloneRemote& remote) {
    if (settings.find != FindMethod::auto_detect) return settings.find;
    return remote.key_file || remote.key_use_agent ? FindMethod::ssh : FindMethod::rclone;
}

GrabMethod resolve_grab_method(const RemoteSettings& settings, const RcloneRemote& remote) {
    switch (settings.find) {
    case FindMethod::ssh: return GrabMethod::find;
    case FindMethod::rclone: return GrabMethod::walk;
    case FindMethod::auto_detect: break;
    }
    return budget::is_storage_box(remote.host) ? GrabMethod::walk : GrabMethod::probe;
}

Engine effective_engine(const RemoteSettings& settings) {
    if (auto env = util::getenv_utf8("GRAB_ENGINE")) {
        const std::string e = util::to_lower(util::trim(*env));
        if (e == "grab") return Engine::grab;
        if (e == "rclone") return Engine::rclone;
    }
    return settings.engine.value_or(Engine::rclone);
}

std::filesystem::path default_grab_config_path() {
    if (auto env = util::getenv_utf8("GRAB_CONFIG"); env && !util::trim(*env).empty()) {
        return util::path_from_utf8(util::trim(*env));
    }
    return util::config_home() / "grab" / "grab.conf";
}

std::string config_key(const std::filesystem::path& config_dir) {
    std::uint64_t h = 1469598103934665603ULL;
    for (const char16_t ch : util::path_from_utf8(util::to_lower(util::path_to_utf8(config_dir))).u16string()) {
        h = (h ^ static_cast<std::uint64_t>(ch)) * 1099511628211ULL;
    }
    return std::format("{:016x}", h);
}

std::optional<std::string> config_folder_name(const std::filesystem::path& grab_conf) {
    std::error_code ec;
    const auto dir = std::filesystem::absolute(grab_conf, ec).parent_path();
    const std::string key = config_key(dir);
    if (key == config_key(util::config_home() / "grab")) return std::nullopt;
    std::string name = util::path_to_utf8(dir.filename());
    return std::format("{}-{}", name.empty() ? "config" : name, key.substr(0, 8));
}

std::filesystem::path grab_rclone_config_path() { return util::config_home() / "grab" / "rclone.conf"; }

std::filesystem::path grab_known_hosts_path() { return util::config_home() / "grab" / "known_hosts"; }

std::filesystem::path standard_rclone_config_path() {
    // Same precedence as rclone itself.
    if (auto env = util::getenv_utf8("RCLONE_CONFIG"); env && !util::trim(*env).empty()) {
        return util::path_from_utf8(util::trim(*env));
    }
    return util::config_home() / "rclone" / "rclone.conf";
}

namespace {
// This thread's hold on the settings lock: how many SettingsLocks, and the file lock they share.
thread_local int settings_depth = 0;
thread_local util::FileLock settings_file;
} // namespace

std::expected<SettingsLock, std::string> lock_settings() {
    if (settings_depth == 0) {
        auto lock = util::lock_file(util::config_home() / "grab" / "settings.lock", std::chrono::seconds(30));
        if (!lock) return util::failf("another grab is still changing its settings ({})", lock.error());
        settings_file = std::move(*lock);
    }
    ++settings_depth;
    SettingsLock held;
    held.held_ = true;
    return held;
}

SettingsLock::SettingsLock(SettingsLock&& other) noexcept : held_(std::exchange(other.held_, false)) {}

SettingsLock::~SettingsLock() {
    if (held_ && --settings_depth == 0) settings_file = util::FileLock{};
}

std::string resolve_rclone_exe(const GrabConfig& cfg, const std::filesystem::path& exe_dir) {
    const std::string configured(util::trim(cfg.rclone));
    const std::string lower = util::to_lower(configured);
    if (!configured.empty() && lower != "rclone" && lower != "rclone.exe") return configured;
#ifdef _WIN32
    const auto bundled = exe_dir / "rclone.exe";
#else
    const auto bundled = exe_dir / "rclone";
#endif
    std::error_code ec;
    if (!exe_dir.empty() && std::filesystem::is_regular_file(bundled, ec)) return util::path_to_utf8(bundled);
    return "rclone";
}

std::expected<const RemoteSettings*, std::string>
GrabConfig::select(const std::optional<std::string>& name) const {
    auto find = [&](std::string_view n) -> const RemoteSettings* {
        for (const auto& r : remotes) {
            if (r.name == n) return &r;
        }
        return nullptr;
    };
    auto available = [&]() {
        std::vector<std::string> names;
        for (const auto& r : remotes) names.push_back(r.name);
        return util::join(names, ", ");
    };

    if (remotes.empty()) return util::fail("no servers configured yet; add one with `grab server add`");
    if (name) {
        if (const auto* r = find(*name)) return r;
        return util::failf("remote '{}' is not defined in grab.conf (available: {})", *name,
                           available());
    }
    if (default_remote) {
        if (const auto* r = find(*default_remote)) return r;
        return util::failf("default_remote '{}' is not defined in grab.conf (available: {})",
                           *default_remote, available());
    }
    if (remotes.size() == 1) return &remotes.front();
    return util::failf("several remotes are defined ({}); pass -r NAME or set default_remote",
                       available());
}

std::expected<GrabConfig, std::string> parse_grab_config(const ini::Document& doc) {
    GrabConfig cfg;

    if (const auto* g = doc.find("grab")) {
        if (auto v = nonblank(*g, "rclone")) cfg.rclone = *v;
        if (auto v = nonblank(*g, "rclone_config")) cfg.rclone_config = util::path_from_utf8(*v);
        if (auto v = nonblank(*g, "ssh")) cfg.ssh = *v;
        if (auto v = nonblank(*g, "editor")) cfg.editor = *v;
        if (auto v = nonblank(*g, "default_remote")) cfg.default_remote = *v;
    }

    for (const auto& s : doc.sections) {
        if (s.name.empty()) continue;
        for (const auto& [key, value] : s.values) {
            if (s.name == "grab" ? !std::ranges::contains(grab_keys, key) : !known_server_key(key)) {
                cfg.unknown_keys.emplace_back(s.name, key);
            }
        }
        if (s.name == "grab") continue;
        RemoteSettings r;
        r.name = s.name;
        r.rclone_remote = nonblank(s, "rclone_remote").value_or(s.name);
        if (auto v = s.get("search_roots")) {
            for (const auto& root : util::split(*v, ',')) {
                r.search_roots.push_back(normalize_root(root));
            }
        }
        if (auto v = nonblank(s, "find")) {
            const std::string method = util::to_lower(*v);
            if (method == "auto") {
                r.find = FindMethod::auto_detect;
            } else if (method == "ssh") {
                r.find = FindMethod::ssh;
            } else if (method == "rclone" || method == "walk") {
                r.find = FindMethod::rclone; // `walk` says it plainly on engine = grab
            } else {
                return util::failf("[{}] find must be auto, ssh, rclone or walk, got '{}'", s.name, *v);
            }
        }
        auto depth = int_value(s, "max_depth", 4);
        if (!depth) return util::fail(depth.error());
        if (*depth < 1) return util::failf("[{}] max_depth must be at least 1", s.name);
        r.max_depth = *depth;
        if (nonblank(s, "max_connections")) {
            auto conns = int_value(s, "max_connections", 0);
            if (!conns) return util::fail(conns.error());
            if (*conns < 1 || *conns > 64) {
                return util::failf("[{}] max_connections must be from 1 to 64", s.name);
            }
            r.max_connections = *conns;
        }
        auto hidden = bool_value(s, "skip_hidden", true);
        if (!hidden) return util::fail(hidden.error());
        r.skip_hidden = *hidden;
        if (auto v = nonblank(s, "engine")) {
            const std::string engine = util::to_lower(*v);
            if (engine == "rclone") r.engine = Engine::rclone;
            else if (engine == "grab") r.engine = Engine::grab;
            else return util::failf("[{}] engine must be rclone or grab, got '{}'", s.name, *v);
        }
        if (auto v = nonblank(s, "bind")) {
            if (auto bad = server_conf::check_host(*v)) {
                return util::failf("[{}] bind: {}", s.name,
                                   v->starts_with('-') ? std::string("the address can't start with -") : *bad);
            }
            r.bind = *v;
        }
        auto connect = time_value(s, "connect_timeout", true);
        if (!connect) return util::fail(connect.error());
        r.connect_timeout = *connect;
        auto io = time_value(s, "io_timeout", false);
        if (!io) return util::fail(io.error());
        r.io_timeout = *io;
        r.ssh_options = flag_list(s, "ssh_options", "");
        r.common_flags = flag_list(s, "common_flags", default_common_flags);
        r.folder_flags = flag_list(s, "folder_flags", default_folder_flags);
        r.file_flags = flag_list(s, "file_flags", default_file_flags);
        cfg.remotes.push_back(std::move(r));
    }

    return cfg; // no servers yet is valid: `grab server add` adds the first

}

std::expected<GrabConfig, std::string> load_grab_config(const std::filesystem::path& p) {
    auto doc = ini::parse_file(p);
    if (!doc) return util::fail(doc.error());
    auto cfg = parse_grab_config(*doc);
    if (!cfg) return util::failf("{}: {}", util::path_to_utf8(p), cfg.error());
    return cfg;
}

std::expected<RcloneRemote, std::string> parse_rclone_remote(const ini::Document& doc,
                                                             std::string_view name) {
    const auto* s = doc.find(name);
    if (s == nullptr) {
        return util::failf("rclone remote '{}' not found (available: {})", name,
                           util::join(doc.section_names(), ", "));
    }
    const auto type = nonblank(*s, "type").value_or("");
    if (type != "sftp") {
        return util::failf("rclone remote '{}' has type '{}', grab needs an sftp remote", name,
                           type);
    }
    RcloneRemote r;
    r.name = std::string(name);
    auto host = nonblank(*s, "host");
    if (!host) return util::failf("rclone remote '{}' has no host", name);
    r.host = *host;
    auto user = nonblank(*s, "user");
    if (!user) return util::failf("rclone remote '{}' has no user", name);
    r.user = *user;
    auto port = int_value(*s, "port", 22);
    if (!port) return util::fail(port.error());
    if (*port < 1 || *port > 65535) {
        return util::failf("rclone remote '{}' has an invalid port {}", name, *port);
    }
    r.port = *port;
    r.key_file = nonblank(*s, "key_file");
    r.known_hosts_file = nonblank(*s, "known_hosts_file");
    // rclone users write `known_hosts_file = none` to skip host key checks; passing that
    // literally to ssh (-o UserKnownHostsFile=none) disables its known_hosts handling.
    if (r.known_hosts_file && util::to_lower(*r.known_hosts_file) == "none") {
        r.known_hosts_file.reset();
    }
    auto agent = bool_value(*s, "key_use_agent", false);
    r.key_use_agent = agent && *agent;
    r.shell_type = nonblank(*s, "shell_type");
    r.md5sum_command = nonblank(*s, "md5sum_command");
    r.path_override = nonblank(*s, "path_override");
    return r;
}

std::vector<std::string> editor_command(const std::vector<std::string>& candidates,
                                        const std::filesystem::path& file) {
    std::vector<std::string> argv;
    for (const auto& candidate : candidates) {
        argv = util::split_args(candidate);
        if (!argv.empty()) break;
    }
    if (argv.empty()) {
#ifdef _WIN32
        argv = {"notepad"};
#else
        argv = {"vi"};
#endif
    }
    argv.push_back(util::path_to_utf8(file));
    return argv;
}

// ---- generating grab.conf from rclone.conf ---------------------------------------------------

namespace {

// Section name for a remote; "grab" is taken by the global section.
std::string section_name_for(std::string_view remote) {
    return remote == "grab" ? std::string("grab_remote") : std::string(remote);
}

// Splits rclone.conf into usable sftp remotes and "name (reason)" labels for the rest.
void classify_remotes(const ini::Document& rclone, std::vector<RcloneRemote>& usable,
                      std::vector<std::string>& skipped) {
    for (const auto& s : rclone.sections) {
        if (s.name.empty()) continue;
        const auto type = nonblank(s, "type").value_or("unknown type");
        if (type != "sftp") {
            skipped.push_back(std::format("{} ({})", s.name, type));
            continue;
        }
        if (auto r = parse_rclone_remote(rclone, s.name)) {
            usable.push_back(std::move(*r));
        } else {
            skipped.push_back(std::format("{} (sftp, incomplete: no host/user or bad port)", s.name));
        }
    }
}

} // namespace

bool is_encrypted_rclone_config(std::string_view text) {
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3); // UTF-8 BOM
    return text.starts_with("# Encrypted rclone configuration") ||
           util::trim(text).starts_with("RCLONE_ENCRYPT_V");
}

std::vector<RcloneRemote> usable_sftp_remotes(const ini::Document& rclone) {
    std::vector<RcloneRemote> usable;
    std::vector<std::string> skipped;
    classify_remotes(rclone, usable, skipped);
    return usable;
}

std::vector<RcloneRemote> missing_remotes(const GrabConfig& cfg, const ini::Document& rclone) {
    std::vector<RcloneRemote> out;
    for (auto& r : usable_sftp_remotes(rclone)) {
        const bool referenced = std::ranges::any_of(
            cfg.remotes, [&](const RemoteSettings& s) { return s.rclone_remote == r.name; });
        if (!referenced) out.push_back(std::move(r));
    }
    return out;
}

std::string_view search_method(const RemoteSettings& settings, const RcloneRemote& remote, Engine engine) {
    if (engine == Engine::rclone) return resolve_find_method(settings, remote) == FindMethod::ssh ? "ssh" : "rclone";
    switch (resolve_grab_method(settings, remote)) {
    case GrabMethod::find: return "find";
    case GrabMethod::walk: return "walk";
    case GrabMethod::probe: break;
    }
    return "probe";
}

std::string describe_remote(const RcloneRemote& remote, const RemoteSettings& settings, std::string_view login) {
    const auto method = search_method(settings, remote, settings.engine.value_or(Engine::rclone));
    const char* lookup = method == "ssh"    ? "via ssh + find"
                         : method == "rclone" ? "via rclone lsf"
                         : method == "find"   ? "with find"
                         : method == "walk"   ? "by listing folders"
                                              : "with find, else by listing folders";
    const std::string_view auth = !login.empty()          ? login
                                  : remote.key_use_agent ? "ssh-agent"
                                  : remote.key_file      ? "key file"
                                                         : "password";
    return std::format("{}@{}:{}, {}, lookup {}", remote.user, remote.host, remote.port, auth, lookup);
}

std::string lead_in(std::string_view name, std::string_view description) {
    return std::format("# server [{}]: {}", name, description);
}

std::string remote_section(const RcloneRemote& remote, const std::vector<std::string>& search_roots,
                           int max_depth, const std::vector<std::pair<std::string, std::string>>& connection) {
    std::string s;
    s += lead_in(section_name_for(remote.name), describe_remote(remote)) + "\n";
    s += std::format("[{}]\n", section_name_for(remote.name));
    if (connection.empty()) s += std::format("rclone_remote = {}\n", remote.name);
    for (const auto& [key, value] : connection) s += std::format("{} = {}\n", key, value);
    s += "# auto = ssh + find when the server uses a key file or ssh-agent, otherwise rclone lsf.\n";
    s += "# With engine = grab: auto = find where grab's check finds GNU find on the server, else by\n";
    s += "# listing folders (Storage Boxes always); ssh = find only; rclone or walk = listing folders.\n";
    s += "find = auto\n";
    s += "# Where to search: comma-separated absolute (/srv) or home-relative (media) dirs.\n";
    s += "# Blank = the login home. Narrow it to speed up searches on big servers.\n";
    s += search_roots.empty() ? std::string("search_roots =\n")
                              : std::format("search_roots = {}\n", search_roots_value(search_roots));
    s += std::format("max_depth = {}\n", max_depth);
    s += "skip_hidden = true\n";
    s += "# Built-in defaults, shown for reference; uncomment a line to override it here.\n";
    s += "# ssh_options = -o ServerAliveInterval=30\n";
    s += std::format("# common_flags = {}\n", default_common_flags);
    s += std::format("# folder_flags = {}\n", default_folder_flags);
    s += std::format("# file_flags = {}\n", default_file_flags);
    return s;
}

std::string generate_grab_config(const ini::Document* rclone, std::string_view rclone_path) {
    std::vector<RcloneRemote> usable;
    std::vector<std::string> skipped;
    if (rclone != nullptr) classify_remotes(*rclone, usable, skipped);

    std::string out = "# grab.conf - settings for the `grab` CLI\n#\n";
    if (!usable.empty()) {
        out += "# Generated by `grab --init`; servers imported from\n";
        out += std::format("#   {}\n", rclone_path);
    } else {
        out += "# Generated by `grab --init`. No servers yet: add one with `grab server add`.\n";
    }
    out += "# Each server's section holds its connection details (host, port, user, auth); passwords\n"
           "# are kept in grab's own encrypted store, never here. Manage servers with\n"
           "# `grab server add | list | edit | test | trust | remove` or grab's Settings.\n"
           "# grab.conf.example explains every key.\n"
           "# Comments must be on their own line; a ; or # after a value is part of the value.\n";
    if (!skipped.empty()) {
        out += std::format("#\n# Skipped rclone remotes (grab needs sftp): {}\n",
                           util::join(skipped, ", "));
    }
    out += "\n[grab]\n";
    out += "# rclone executable. Blank = the rclone.exe bundled with grab (else rclone on PATH).\n";
    out += "rclone =\n";
    out += "# The rclone.conf whose remote a section without `host` takes its details from, once (grab\n"
           "# only reads it). Blank = grab's own (%APPDATA%\\grab\\rclone.conf), then rclone's.\n";
    out += "rclone_config =\n";
    out += "ssh = ssh\n";
    out += "# Editor for `grab config`, e.g.  code --wait   Blank = $VISUAL, then $EDITOR, then notepad.\n";
    out += "editor =\n";
    out += "# Remote used when -r/--remote is not given.\n";
    out += std::format("default_remote = {}\n",
                       usable.empty() ? std::string{} : section_name_for(usable.front().name));
    for (const auto& r : usable) {
        out += '\n';
        out += remote_section(r);
    }
    return out;
}

} // namespace grab
