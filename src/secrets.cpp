#include "secrets.hpp"

#include "config.hpp"
#include "util.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <mutex>
#include <set>
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

namespace {

// The store in a file's `data`; nullopt when it can't be decrypted or parsed.
std::optional<Store> unseal(std::string_view data) {
    if (!data.starts_with(sealed_header)) return std::nullopt;
    const auto text = util::unprotect_for_user(data.substr(sealed_header.size()), purpose);
    return text ? from_text(*text) : std::nullopt;
}

} // namespace

std::expected<Store, std::string> load(const fs::path& file) {
    auto data = util::read_file_if_exists(file);
    if (!data) return util::fail(data.error());
    if (!*data) return Store{};
    auto store = unseal(**data);
    if (!store) {
        return util::failf("the saved passwords in {} can't be read (they belong to another Windows account, or the "
                           "file is damaged)",
                           util::path_to_utf8(file));
    }
    return std::move(*store);
}

std::expected<ForChange, std::string> load_for_change(const fs::path& file) {
    auto data = util::read_file_if_exists(file);
    if (!data) return util::fail(data.error());
    if (!*data) return ForChange{};
    if (auto store = unseal(**data)) return ForChange{.store = std::move(*store), .set_aside = std::nullopt};
    auto moved = util::move_aside(file, "unreadable");
    if (!moved) return util::fail(moved.error());
    return ForChange{.store = {}, .set_aside = std::move(*moved)};
}

std::expected<void, std::string> save(const fs::path& file, const Store& store) {
    const auto sealed = util::protect_for_user(to_text(store), purpose);
    if (!sealed) return util::failf("cannot write {}: the passwords could not be encrypted", util::path_to_utf8(file));
    return util::write_file_atomic(file, std::string(sealed_header) + *sealed);
}

std::string env_name(std::string_view remote, std::string_view key) {
    // As rclone's fs.ConfigToEnv: '-' becomes '_' in the key only; the remote keeps its '-' and
    // '.', or rclone never finds the password of a server called "my-box" (review G6).
    const auto upper = [](char c) { return static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c); };
    std::string name = "RCLONE_CONFIG_";
    for (const char c : remote) name += upper(c);
    name += '_';
    for (const char c : key) name += c == '-' ? '_' : upper(c);
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
    auto text = util::read_file(rclone_conf);
    if (text && !take_secrets(*text).found.empty()) {
        // Moved under the settings lock, from both files as they are then: another grab may be
        // moving them at the same time (review G175). Without the lock they stay for now.
        if (auto lock = lock_settings()) {
            text = util::read_file(rclone_conf);
            auto taken = text ? take_secrets(*text) : Taken{};
            // A store that can't be decrypted is set aside first, as for any change (G211); the
            // secrets just found start a new one.
            auto base = taken.found.empty() ? util::fail("nothing to move") : load_for_change(file);
            if (base) {
                Store merged = std::move(base->store);
                for (auto& [remote, values] : taken.found) {
                    for (auto& [key, value] : values) merged[remote][key] = std::move(value);
                }
                // The file keeps its secrets until the store holds them for sure. If anything
                // fails, they stay where rclone reads them anyway, and the next run tries again.
                if (save(file, merged) && load(file) == merged) {
                    if (!util::write_file_atomic(rclone_conf, taken.text)) {
                        // They stay in the file as well, where rclone reads them anyway.
                    }
                }
            }
            store = load(file);
        }
    }
    if (!store) return util::fail(store.error());
    export_to_env(*store);
    return {};
}

std::optional<std::string> reveal(std::string_view obscured) {
    // rclone's fixed key (lib/obscure/obscure.go).
    static constexpr unsigned char key[32] = {0x9c, 0x93, 0x5b, 0x48, 0x73, 0x0a, 0x55, 0x4d, 0x6b, 0xfd, 0x7c,
                                              0x63, 0xc8, 0x86, 0xa9, 0x2b, 0xd3, 0x90, 0x19, 0x8e, 0xb8, 0x12,
                                              0x8a, 0xfb, 0xf4, 0xde, 0x16, 0x2b, 0x8b, 0x95, 0xf6, 0x38};
    std::string raw;
    std::uint32_t acc = 0;
    int bits = 0;
    for (char c : obscured) {
        int v = c >= 'A' && c <= 'Z'   ? c - 'A'
                : c >= 'a' && c <= 'z' ? c - 'a' + 26
                : c >= '0' && c <= '9' ? c - '0' + 52
                : c == '-'             ? 62
                : c == '_'             ? 63
                                       : -1;
        if (v < 0) return std::nullopt;
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            raw.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    if (raw.size() < 16) return std::nullopt;
    std::string plain(raw.size() - 16, '\0');
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int n = 0;
    bool ok = ctx && EVP_DecryptInit_ex(ctx, EVP_aes_256_ctr(), nullptr, key,
                                        reinterpret_cast<const unsigned char*>(raw.data())) == 1 &&
              EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(plain.data()), &n,
                                reinterpret_cast<const unsigned char*>(raw.data() + 16),
                                static_cast<int>(raw.size() - 16)) == 1;
    EVP_CIPHER_CTX_free(ctx);
    OPENSSL_cleanse(raw.data(), raw.size());
    if (!ok || static_cast<std::size_t>(n) != plain.size()) return std::nullopt;
    return plain;
}

} // namespace grab::secrets
