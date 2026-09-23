#include "config.hpp"

#include "util.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>

namespace grab {

const std::string_view example_config = R"(# grab.conf - settings for the `grab` CLI
#
# Location: %APPDATA%\grab\grab.conf  (override with --config PATH or the GRAB_CONFIG env var)
# Same INI dialect as rclone.conf: [section], key = value, comment lines start with ; or #.
# Comments must be on their own line; a ; or # after a value is part of the value.
#
# Server connection details (host, user, port, key, password) are NOT stored here: they live
# in grab's own rclone.conf (%APPDATA%\grab\rclone.conf), in the rclone remote named by
# `rclone_remote`. Add servers with `grab server add`; it writes both files.

[grab]
# rclone executable. Blank = the rclone.exe bundled with grab (else rclone on PATH);
# a full path uses that one instead.
rclone =
# rclone.conf with the server connections. Blank = grab's own (%APPDATA%\grab\rclone.conf).
# It is always passed to rclone as --config.
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
# How TARGET is located: auto = ssh + find when the rclone remote has a key_file, otherwise
# an rclone listing (password remotes and storage boxes have no shell). Force with ssh | rclone.
find = auto
# Comma-separated directories to search: absolute (/srv) or relative to the login home
# (learning). Blank or . = the home directory itself.
search_roots = /home/alice
# How deep below each root `find` may look (find -maxdepth).
max_depth = 4
# Skip dot-directories such as .cache and .config while searching.
skip_hidden = true
# Extra raw ssh arguments, e.g.  -o ServerAliveInterval=30
ssh_options =
# rclone flags used in both modes. 255Ki is the largest SFTP packet OpenSSH accepts and
# cuts round trips roughly 8x compared with the 32Ki default. --sftp-disable-hashcheck skips
# the post-transfer md5sum rclone would otherwise run on the server (minutes for a 30 GB
# file); SSH already guarantees integrity in flight.
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

std::filesystem::path default_grab_config_path() {
    if (auto env = util::getenv_utf8("GRAB_CONFIG"); env && !util::trim(*env).empty()) {
        return util::path_from_utf8(util::trim(*env));
    }
    return util::config_home() / "grab" / "grab.conf";
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

std::filesystem::path effective_rclone_config(const GrabConfig& cfg) {
    return cfg.rclone_config.value_or(grab_rclone_config_path());
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

std::string section_text(const ini::Section& section) {
    std::string out = std::format("[{}]\n", section.name);
    for (const auto& [key, value] : section.values) out += std::format("{} = {}\n", key, value);
    return out;
}

ImportResult import_rclone_remotes(const std::vector<std::string>& names,
                                   const std::filesystem::path& from, const std::filesystem::path& to) {
    ImportResult result;
    std::error_code ec;
    std::string target_text;
    if (std::filesystem::exists(to, ec)) {
        auto text = util::read_file(to);
        if (!text) {
            result.error = text.error();
            return result;
        }
        target_text = std::move(*text);
    }
    auto target = ini::parse(target_text);
    if (!target) {
        result.error = std::format("{}: {}", util::path_to_utf8(to), target.error());
        return result;
    }

    std::vector<std::string> wanted;
    for (const auto& name : names) {
        if (target->find(name) == nullptr && std::ranges::find(wanted, name) == wanted.end()) {
            wanted.push_back(name);
        }
    }
    if (wanted.empty()) return result;

    std::optional<ini::Document> source;
    if (auto text = util::read_file(from); text && !is_encrypted_rclone_config(*text)) {
        if (auto doc = ini::parse(*text)) source = std::move(*doc);
    }
    std::string appended;
    for (const auto& name : wanted) {
        const ini::Section* s = source ? source->find(name) : nullptr;
        if (s == nullptr) {
            result.not_found.push_back(name);
            continue;
        }
        appended += (target_text.empty() && appended.empty() ? "" : "\n") + section_text(*s);
        result.imported.push_back(name);
    }
    if (appended.empty()) return result;

    if (!target_text.empty() && !target_text.ends_with('\n')) target_text += '\n';
    target_text += appended;
    std::filesystem::create_directories(to.parent_path(), ec);
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!out || !(out << target_text)) {
        result.error = std::format("cannot write {}", util::path_to_utf8(to));
        result.imported.clear();
    }
    return result;
}

ImportResult migrate_rclone_remotes(const GrabConfig& cfg) {
    if (cfg.rclone_config) return {}; // an explicit rclone.conf is used as is
    std::vector<std::string> names;
    for (const auto& r : cfg.remotes) names.push_back(r.rclone_remote);
    return import_rclone_remotes(names, standard_rclone_config_path(), grab_rclone_config_path());
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
        if (s.name.empty() || s.name == "grab") continue;
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
            } else if (method == "rclone") {
                r.find = FindMethod::rclone;
            } else {
                return util::failf("[{}] find must be auto, ssh or rclone, got '{}'", s.name, *v);
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
    auto text = util::read_file(p);
    if (!text) {
        return util::failf("{} (set rclone_config in grab.conf if rclone.conf lives elsewhere)",
                           text.error());
    }
    if (is_encrypted_rclone_config(*text)) {
        return util::failf("{} is encrypted and grab cannot decrypt it to read the remote's host, "
                           "user and port; point rclone_config in grab.conf at an unencrypted "
                           "rclone.conf",
                           util::path_to_utf8(p));
    }
    auto doc = ini::parse(*text);
    if (!doc) return util::failf("{}: {}", util::path_to_utf8(p), doc.error());
    auto r = parse_rclone_remote(*doc, name);
    if (!r) return util::failf("{}: {}", util::path_to_utf8(p), r.error());
    return r;
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

std::string describe_remote(const RcloneRemote& remote) {
    const bool ssh = resolve_find_method(RemoteSettings{}, remote) == FindMethod::ssh;
    const char* auth = remote.key_use_agent ? "ssh-agent" : remote.key_file ? "key file" : "password";
    return std::format("{}@{}:{}, {}, lookup via {}", remote.user, remote.host, remote.port, auth,
                       ssh ? "ssh + find" : "rclone lsf");
}

std::string remote_section(const RcloneRemote& remote, const std::vector<std::string>& search_roots,
                           int max_depth) {
    std::string s;
    s += std::format("# rclone remote [{}]: {}\n", remote.name, describe_remote(remote));
    s += std::format("[{}]\n", section_name_for(remote.name));
    s += std::format("rclone_remote = {}\n", remote.name);
    s += "# auto = ssh + find when the server uses a key file or ssh-agent, otherwise rclone lsf.\n";
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
    out += "# Connection details (host, user, port, key, password) live in grab's own rclone.conf\n"
           "# next to this file; manage servers with `grab server add | list | trust | remove`.\n"
           "# grab.conf.example explains every key.\n"
           "# Comments must be on their own line; a ; or # after a value is part of the value.\n";
    if (!skipped.empty()) {
        out += std::format("#\n# Skipped rclone remotes (grab needs sftp): {}\n",
                           util::join(skipped, ", "));
    }
    out += "\n[grab]\n";
    out += "# rclone executable. Blank = the rclone.exe bundled with grab (else rclone on PATH).\n";
    out += "rclone =\n";
    out += "# rclone.conf with the server connections. Blank = grab's own (%APPDATA%\\grab\\rclone.conf).\n";
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
