#include "secrets.hpp"

#include "util.hpp"

#include <format>
#include <mutex>
#include <set>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)
#else
#include <cstdlib>
#endif

namespace grab::secrets {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view header = "grab-secrets 1";
constexpr std::string_view sealed_header = "grab-secrets-sealed 1\n";
constexpr std::string_view purpose = "grab secrets";

// Calls `each` for every line of `text`, the line ending included in the piece.
template <class Each>
void each_line(std::string_view text, Each&& each) {
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        end = end == std::string_view::npos ? text.size() : end + 1;
        each(text.substr(start, end - start));
        start = end;
    }
}

std::string_view without_eol(std::string_view line) {
    while (line.ends_with('\n') || line.ends_with('\r')) line.remove_suffix(1);
    return line;
}

} // namespace

bool is_secret_key(std::string_view key) { return key == "pass" || key == "key_file_pass" || key == "key_pem"; }

fs::path path_beside(const fs::path& rclone_conf) { return rclone_conf.parent_path() / "secrets"; }

std::string to_text(const Store& store) {
    std::string out = std::string(header) + "\n";
    for (const auto& [remote, values] : store) {
        for (const auto& [key, value] : values) {
            out += std::format("{}\t{}\t{}\n", util::escape_field(remote), util::escape_field(key),
                               util::escape_field(value));
        }
    }
    return out;
}

std::optional<Store> from_text(std::string_view text) {
    Store store;
    bool first = true;
    bool ok = true;
    each_line(text, [&](std::string_view raw) {
        const auto line = without_eol(raw);
        if (!ok) return;
        if (first) {
            first = false;
            ok = line == header;
            return;
        }
        if (line.empty()) return;
        const auto fields = util::split(line, '\t');
        if (fields.size() != 3) {
            ok = false;
            return;
        }
        auto remote = util::unescape_field(fields[0]);
        auto key = util::unescape_field(fields[1]);
        auto value = util::unescape_field(fields[2]);
        if (!remote || !key || !value || remote->empty() || key->empty()) {
            ok = false;
            return;
        }
        store[*remote][*key] = std::move(*value);
    });
    if (!ok || first) return std::nullopt;
    return store;
}

std::expected<Store, std::string> load(const fs::path& file) {
    std::error_code ec;
    if (!fs::exists(file, ec)) return Store{};
    auto data = util::read_file(file);
    if (!data) return util::fail(data.error());
    std::optional<Store> store;
    if (data->starts_with(sealed_header)) {
        if (auto text = util::unprotect_for_user(std::string_view(*data).substr(sealed_header.size()), purpose)) {
            store = from_text(*text);
        }
    }
    if (!store) {
        return util::failf("the saved passwords in {} can't be read (they belong to another Windows account, or the "
                           "file is damaged)",
                           util::path_to_utf8(file));
    }
    return std::move(*store);
}

bool save(const fs::path& file, const Store& store) {
    const auto sealed = util::protect_for_user(to_text(store), purpose);
    return sealed && util::write_file_atomic(file, std::string(sealed_header) + *sealed);
}

std::string env_name(std::string_view remote, std::string_view key) {
    std::string name = "RCLONE_CONFIG_";
    for (const char c : std::string(remote) + "_" + std::string(key)) {
        name += c == '-' ? '_' : static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    }
    return name;
}

void export_to_env(const Store& store) {
    static std::mutex m;
    static std::set<std::string> exported; // names this process set, to drop the stale ones
    std::lock_guard lock(m);
    std::set<std::string> now;
    auto set = [](const std::string& name, const std::string* value) {
#ifdef _WIN32
        SetEnvironmentVariableW(util::to_wide(name).c_str(), value != nullptr ? util::to_wide(*value).c_str() : nullptr);
#else
        if (value != nullptr) setenv(name.c_str(), value->c_str(), 1);
        else unsetenv(name.c_str());
#endif
    };
    for (const auto& [remote, values] : store) {
        for (const auto& [key, value] : values) {
            const auto name = env_name(remote, key);
            set(name, &value);
            now.insert(name);
        }
    }
    for (const auto& name : exported) {
        if (!now.contains(name)) set(name, nullptr);
    }
    exported = std::move(now);
}

Taken take_secrets(std::string_view rclone_text) {
    Taken t;
    std::string section;
    each_line(rclone_text, [&](std::string_view raw) {
        const auto line = util::trim(without_eol(raw));
        if (line.starts_with('[') && line.ends_with(']')) {
            section = std::string(util::trim(line.substr(1, line.size() - 2)));
        } else if (!section.empty() && !line.starts_with('#') && !line.starts_with(';')) {
            if (const auto eq = line.find('='); eq != std::string_view::npos) {
                const auto key = util::trim(line.substr(0, eq));
                if (is_secret_key(key)) {
                    const auto value = util::trim(line.substr(eq + 1));
                    if (!value.empty()) t.found[section][std::string(key)] = std::string(value);
                    return; // the line goes, with its line ending
                }
            }
        }
        t.text += raw;
    });
    return t;
}

std::expected<void, std::string> prepare(const fs::path& rclone_conf) {
    const auto file = path_beside(rclone_conf);
    auto store = load(file);
    if (auto text = util::read_file(rclone_conf)) {
        auto taken = take_secrets(*text);
        if (!taken.found.empty()) {
            // An unreadable store can't be used anyway: the secrets just found replace it.
            Store merged = store ? std::move(*store) : Store{};
            for (auto& [remote, values] : taken.found) {
                for (auto& [key, value] : values) merged[remote][key] = std::move(value);
            }
            // The file keeps its secrets until the store holds them for sure. If anything fails,
            // they stay where rclone reads them anyway, and the next run tries again.
            if (!save(file, merged) || load(file) != merged) {
                if (store) export_to_env(*store);
                return {};
            }
            (void)util::write_file_atomic(rclone_conf, taken.text);
            store = std::move(merged);
        }
    }
    if (!store) return util::fail(store.error());
    export_to_env(*store);
    return {};
}

} // namespace grab::secrets
