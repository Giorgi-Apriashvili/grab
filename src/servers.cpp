#include "servers.hpp"

#include "util.hpp"

#include <cctype>
#include <format>
#include <set>

namespace grab::servers {

std::optional<std::string> validate_name(std::string_view name) {
    if (name.empty()) return "a name is required";
    if (name.size() > 64) return "use at most 64 characters";
    if (util::to_lower(name) == "grab") return "\"grab\" is reserved for grab.conf's own section";
    if (name.front() == '-' || name.front() == '.') return "the name cannot start with - or .";
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) || c == '_' || c == '-' || c == '.')) {
            return "use letters, digits, _ - and . only";
        }
    }
    return std::nullopt;
}

RcloneRemote to_remote(const NewServer& s, const std::filesystem::path& known_hosts) {
    RcloneRemote r;
    r.name = s.name;
    r.host = s.host;
    r.user = s.user;
    r.port = s.port;
    if (s.auth == Auth::key_file && !s.key_file.empty()) r.key_file = s.key_file;
    r.key_use_agent = s.auth == Auth::agent;
    if (!known_hosts.empty()) r.known_hosts_file = util::path_to_utf8(known_hosts);
    return r;
}

// ---- host keys -------------------------------------------------------------------------------

std::vector<std::string> keyscan_argv(const std::string& host, int port) {
    return {"ssh-keyscan", "-T", "10", "-p", std::to_string(port), host};
}

std::vector<std::string> handshake_argv(const std::string& ssh, const std::string& host, int port,
                                        const std::filesystem::path& record_to,
                                        const std::string& host_key_algorithm) {
#ifdef _WIN32
    const char* no_file = "NUL";
#else
    const char* no_file = "/dev/null";
#endif
    std::vector<std::string> argv{ssh,
                                  "-T",
                                  "-o", "BatchMode=yes",
                                  "-o", "StrictHostKeyChecking=accept-new",
                                  "-o", "UserKnownHostsFile=" + util::path_to_utf8(record_to),
                                  "-o", std::string("GlobalKnownHostsFile=") + no_file,
                                  "-o", "PreferredAuthentications=none",
                                  "-o", "ConnectTimeout=10"};
    if (!host_key_algorithm.empty()) argv.insert(argv.end(), {"-o", "HostKeyAlgorithms=" + host_key_algorithm});
    argv.insert(argv.end(), {"-p", std::to_string(port), "grab-hostkey@" + host, "exit"});
    return argv;
}

const std::vector<std::string>& handshake_key_algorithms() {
    static const std::vector<std::string> algorithms{
        "ssh-ed25519", "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384", "ecdsa-sha2-nistp521", "rsa-sha2-512"};
    return algorithms;
}

std::vector<std::string> parse_keyscan(std::string_view out) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < out.size()) {
        auto end = out.find('\n', start);
        if (end == std::string_view::npos) end = out.size();
        const auto line = util::trim(out.substr(start, end - start));
        start = end + 1;
        if (!line.empty() && !line.starts_with('#')) lines.emplace_back(line);
    }
    return lines;
}

std::vector<Fingerprint> parse_fingerprints(std::string_view out) {
    std::vector<Fingerprint> result;
    std::size_t start = 0;
    while (start < out.size()) {
        auto end = out.find('\n', start);
        if (end == std::string_view::npos) end = out.size();
        const auto line = util::trim(out.substr(start, end - start));
        start = end + 1;
        const auto parts = util::split_args(line);
        if (parts.size() < 3 || !parts[1].starts_with("SHA256:")) continue;
        std::string type = parts.back();
        if (type.size() >= 2 && type.front() == '(' && type.back() == ')') type = type.substr(1, type.size() - 2);
        result.push_back(Fingerprint{parts[1], type});
    }
    return result;
}

std::string merge_known_hosts(std::string_view existing, const std::vector<std::string>& lines) {
    std::set<std::string, std::less<>> present;
    std::size_t start = 0;
    while (start < existing.size()) {
        auto end = existing.find('\n', start);
        if (end == std::string_view::npos) end = existing.size();
        present.emplace(util::trim(existing.substr(start, end - start)));
        start = end + 1;
    }
    std::string out(existing);
    if (!out.empty() && !out.ends_with('\n')) out += '\n';
    for (const auto& l : lines) {
        const std::string line(util::trim(l));
        if (line.empty() || present.contains(line)) continue;
        out += line;
        out += '\n';
        present.insert(line);
    }
    return out;
}

// ---- rclone command lines --------------------------------------------------------------------

std::vector<std::string> obscure_argv(const std::string& rclone) { return {rclone, "obscure", "-"}; }

std::vector<std::string> create_argv(const std::string& rclone, const std::filesystem::path& config,
                                     const NewServer& s, const std::optional<std::string>& obscured_secret,
                                     const std::filesystem::path& known_hosts) {
    std::vector<std::string> argv{rclone, "config", "create", s.name, "sftp",
                                  "host=" + s.host, std::format("port={}", s.port), "user=" + s.user};
    switch (s.auth) {
    case Auth::password:
        if (obscured_secret) argv.push_back("pass=" + *obscured_secret);
        break;
    case Auth::key_file:
        argv.push_back("key_file=" + s.key_file);
        if (obscured_secret) argv.push_back("key_file_pass=" + *obscured_secret);
        break;
    case Auth::agent:
        argv.emplace_back("key_use_agent=true");
        break;
    }
    if (!known_hosts.empty()) argv.push_back("known_hosts_file=" + util::path_to_utf8(known_hosts));
    argv.insert(argv.end(), {"--no-obscure", "--non-interactive", "--config", util::path_to_utf8(config)});
    return argv;
}

std::vector<std::string> update_argv(const std::string& rclone, const std::filesystem::path& config,
                                     const std::string& name, const std::string& key,
                                     const std::string& value) {
    return {rclone, "config", "update", name, key + "=" + value, "--no-obscure", "--non-interactive",
            "--config", util::path_to_utf8(config)};
}

std::vector<std::string> delete_argv(const std::string& rclone, const std::filesystem::path& config,
                                     const std::string& name) {
    return {rclone, "config", "delete", name, "--config", util::path_to_utf8(config)};
}

std::vector<std::string> probe_argv(const std::string& rclone, const std::filesystem::path& config,
                                    const std::string& name) {
    return {rclone, "lsf", name + ":", "--dirs-only", "--max-depth", "1", "--contimeout", "15s",
            "--timeout", "30s", "--low-level-retries", "1", "--retries", "1", "--config",
            util::path_to_utf8(config)};
}

// ---- grab.conf text edits --------------------------------------------------------------------

namespace {

struct Lines {
    std::vector<std::string> lines; // without line breaks
    std::string eol = "\n";
    bool final_newline = true;
};

Lines split_lines(std::string_view text) {
    Lines l;
    if (text.find("\r\n") != std::string_view::npos) l.eol = "\r\n";
    l.final_newline = text.empty() || text.ends_with('\n');
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(start, end - start);
        if (line.ends_with('\r')) line.remove_suffix(1);
        l.lines.emplace_back(line);
        start = end + 1;
    }
    return l;
}

std::string join_lines(const Lines& l) {
    std::string out;
    for (std::size_t i = 0; i < l.lines.size(); ++i) {
        out += l.lines[i];
        if (i + 1 < l.lines.size() || l.final_newline) out += l.eol;
    }
    return out;
}

bool is_header(std::string_view line) {
    const auto t = util::trim(line);
    return t.size() >= 2 && t.front() == '[' && t.back() == ']';
}

bool is_header_of(std::string_view line, std::string_view name) {
    const auto t = util::trim(line);
    return is_header(t) && util::trim(t.substr(1, t.size() - 2)) == name;
}

bool blank(std::string_view line) { return util::trim(line).empty(); }

// [begin, end) of a section's lines, header included; nullopt when absent.
std::optional<std::pair<std::size_t, std::size_t>> find_section(const std::vector<std::string>& lines,
                                                                std::string_view name) {
    auto comment = [](std::string_view line) {
        const auto t = util::trim(line);
        return t.starts_with('#') || t.starts_with(';');
    };
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (!is_header_of(lines[i], name)) continue;
        std::size_t end = i + 1;
        while (end < lines.size() && !is_header(lines[end])) ++end;
        if (end < lines.size()) {
            // A comment block directly above the next header, set off from this section by a
            // blank line, is that section's lead-in ("# rclone remote [next]: ..."), not ours.
            std::size_t j = end;
            while (j > i + 1 && comment(lines[j - 1])) --j;
            if (j < end && j > i + 1 && blank(lines[j - 1])) end = j;
        }
        while (end > i + 1 && blank(lines[end - 1])) --end;
        return std::pair{i, end};
    }
    return std::nullopt;
}

} // namespace

std::string remove_section(std::string_view text, std::string_view section) {
    Lines l = split_lines(text);
    auto range = find_section(l.lines, section);
    if (!range) return std::string(text);
    auto [begin, end] = *range;
    // grab's generated lead-in comment ("# rclone remote [name]: ...").
    if (begin > 0) {
        const auto prev = util::trim(l.lines[begin - 1]);
        if (prev.starts_with(std::format("# rclone remote [{}]", section)) ||
            prev.starts_with(std::format("# rclone.conf [{}]", section))) {
            --begin;
        }
    }
    l.lines.erase(l.lines.begin() + static_cast<std::ptrdiff_t>(begin),
                  l.lines.begin() + static_cast<std::ptrdiff_t>(end));
    // Don't leave two blank lines where the section was.
    if (begin > 0 && begin <= l.lines.size() && blank(l.lines[begin - 1]) &&
        (begin == l.lines.size() || blank(l.lines[begin]))) {
        l.lines.erase(l.lines.begin() + static_cast<std::ptrdiff_t>(begin - 1));
    }
    // Nor a blank first line when the removed section opened the file.
    while (begin == 0 && !l.lines.empty() && blank(l.lines.front())) l.lines.erase(l.lines.begin());
    while (!l.lines.empty() && blank(l.lines.back())) l.lines.pop_back();
    l.final_newline = true;
    return join_lines(l);
}

std::string append_section(std::string_view text, std::string_view section_text) {
    Lines l = split_lines(text);
    while (!l.lines.empty() && blank(l.lines.back())) l.lines.pop_back();
    if (!l.lines.empty()) l.lines.emplace_back();
    Lines added = split_lines(section_text);
    l.lines.insert(l.lines.end(), added.lines.begin(), added.lines.end());
    l.final_newline = true;
    return join_lines(l);
}

std::string set_value(std::string_view text, std::string_view section, std::string_view key,
                      std::string_view value) {
    Lines l = split_lines(text);
    const std::string assignment = value.empty() ? std::format("{} =", key) : std::format("{} = {}", key, value);
    auto range = find_section(l.lines, section);
    if (!range) {
        std::vector<std::string> head{std::format("[{}]", section), assignment};
        if (!l.lines.empty()) head.emplace_back();
        l.lines.insert(l.lines.begin(), head.begin(), head.end());
        l.final_newline = true;
        return join_lines(l);
    }
    const auto [begin, end] = *range;
    for (std::size_t i = begin + 1; i < end; ++i) {
        const auto t = util::trim(l.lines[i]);
        if (t.empty() || t.starts_with('#') || t.starts_with(';')) continue;
        const auto eq = t.find('=');
        if (eq != std::string_view::npos && util::trim(t.substr(0, eq)) == key) {
            l.lines[i] = assignment;
            return join_lines(l);
        }
    }
    l.lines.insert(l.lines.begin() + static_cast<std::ptrdiff_t>(begin + 1), assignment);
    return join_lines(l);
}

} // namespace grab::servers
