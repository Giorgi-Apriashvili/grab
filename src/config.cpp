#include "config.hpp"

#include "util.hpp"

#include <algorithm>
#include <charconv>

namespace grab {

const std::string_view example_config = R"(# grab.conf - settings for the `grab` CLI
#
# Location: %APPDATA%\grab\grab.conf  (override with --config PATH or the GRAB_CONFIG env var)
# Same INI dialect as rclone.conf: [section], key = value, comment lines start with ; or #.
# Comments must be on their own line; a ; or # after a value is part of the value.
#
# Server connection details (host, user, port, key, passphrase) are NOT stored here.
# grab reads them from the rclone remote named by `rclone_remote`, so configure the
# server once with `rclone config` and reference it from here.

[grab]
# rclone executable: a name on PATH or a full path.
rclone = rclone
# rclone.conf to read connection details from. Blank = rclone's default location
# (%APPDATA%\rclone\rclone.conf). When set, it is also passed to rclone as --config.
rclone_config =
# ssh executable used for the remote `find`. Blank = `ssh` on PATH (Windows OpenSSH).
ssh = ssh
# Editor for `grab config`, e.g.  code --wait   Blank = $VISUAL, then $EDITOR, then notepad.
editor =
# Remote section to use when -r/--remote is not given. Optional when only one remote exists.
default_remote = hetzner

# One section per server. The section name is what you pass to -r/--remote.
[hetzner]
# Name of the remote in rclone.conf (defaults to this section's name).
rclone_remote = hetzner
# Comma-separated absolute directories to search on the server, all in one `find` call.
search_roots = /home/alice
# How deep below each root `find` may look (find -maxdepth).
max_depth = 4
# Skip dot-directories such as .cache and .config while searching.
skip_hidden = true
# Extra raw ssh arguments, e.g.  -o ServerAliveInterval=30
ssh_options =
# rclone flags used in both modes. 255Ki is the largest SFTP packet OpenSSH accepts and
# cuts round trips roughly 8x compared with the 32Ki default.
common_flags = -P --sftp-chunk-size 255Ki
# rclone flags for -f/--folder (rclone copy). Concurrent connections ~= transfers x streams;
# keep that under ~16 to stay clear of sshd's default MaxStartups/MaxSessions limits.
folder_flags = --transfers 4 --checkers 8 --multi-thread-streams 4
# rclone flags for -s/--file (rclone copyto). One big object: parallel range reads.
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
    std::string lower = *v;
    std::ranges::transform(lower, lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "true" || lower == "yes" || lower == "on" || lower == "1") return true;
    if (lower == "false" || lower == "no" || lower == "off" || lower == "0") return false;
    return util::failf("[{}] {} must be true/false, got '{}'", s.name, key, *v);
}

} // namespace

std::filesystem::path default_grab_config_path() {
    if (auto env = util::getenv_utf8("GRAB_CONFIG"); env && !util::trim(*env).empty()) {
        return util::path_from_utf8(util::trim(*env));
    }
    return util::config_home() / "grab" / "grab.conf";
}

std::filesystem::path default_rclone_config_path() {
    return util::config_home() / "rclone" / "rclone.conf";
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
        if (s.name.empty() || s.name == "grab") continue;
        RemoteSettings r;
        r.name = s.name;
        r.rclone_remote = nonblank(s, "rclone_remote").value_or(s.name);
        if (auto v = s.get("search_roots")) r.search_roots = util::split(*v, ',');
        for (const auto& root : r.search_roots) {
            if (!root.starts_with('/')) {
                return util::failf("[{}] search_roots entries must be absolute, got '{}'", s.name,
                                   root);
            }
        }
        auto depth = int_value(s, "max_depth", 4);
        if (!depth) return util::fail(depth.error());
        if (*depth < 1) return util::failf("[{}] max_depth must be at least 1", s.name);
        r.max_depth = *depth;
        auto hidden = bool_value(s, "skip_hidden", true);
        if (!hidden) return util::fail(hidden.error());
        r.skip_hidden = *hidden;
        r.ssh_options = flag_list(s, "ssh_options", "");
        r.common_flags = flag_list(s, "common_flags", default_common_flags);
        r.folder_flags = flag_list(s, "folder_flags", default_folder_flags);
        r.file_flags = flag_list(s, "file_flags", default_file_flags);
        cfg.remotes.push_back(std::move(r));
    }

    if (cfg.remotes.empty()) {
        return util::fail("grab.conf defines no remote sections (run `grab --init` for an example)");
    }
    return cfg;
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

std::expected<RcloneRemote, std::string> load_rclone_remote(const std::filesystem::path& p,
                                                            std::string_view name) {
    auto doc = ini::parse_file(p);
    if (!doc) {
        return util::failf("{} (set rclone_config in grab.conf if rclone.conf lives elsewhere)",
                           doc.error());
    }
    auto r = parse_rclone_remote(*doc, name);
    if (!r) return util::failf("{}: {}", util::path_to_utf8(p), r.error());
    return r;
}

} // namespace grab
