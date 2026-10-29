#include "passwords.hpp"

#include "util.hpp"

#include <charconv>
#include <format>
#include <system_error>
#include <utility>
#include <vector>

namespace grab::passwords {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view header = "grab-passwords 1";
constexpr std::string_view sealed_header = "grab-passwords-sealed 1\n";
constexpr std::string_view purpose = "grab passwords";

std::optional<int> port_of(std::string_view text) {
    int port = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), port);
    if (ec != std::errc{} || end != text.data() + text.size() || port < 1 || port > 65535) return std::nullopt;
    return port;
}

} // namespace

std::string Binding::text() const { return std::format("{}@{}:{}", user, host, port); }

bool encrypted() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

fs::path path_in(const fs::path& state_dir) { return state_dir / "passwords"; }

std::string to_text(const Store& store) {
    std::string out = std::string(header) + "\n";
    for (const auto& [server, values] : store) {
        for (const auto& [key, secret] : values) {
            out += std::format("{}\t{}\t{}\t{}\t{}\t{}\n", util::escape_field(server),
                               util::escape_field(secret.binding.user), util::escape_field(secret.binding.host),
                               secret.binding.port, util::escape_field(key), util::escape_field(secret.value));
        }
    }
    return out;
}

std::optional<Store> from_text(std::string_view text) {
    Store store;
    bool first = true;
    for (std::size_t start = 0; start < text.size();) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        auto line = text.substr(start, end - start);
        start = end + 1;
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (first) {
            if (line != header) return std::nullopt;
            first = false;
            continue;
        }
        if (line.empty()) continue;
        // Six fields, empty ones too (a blank value), each with its tabs escaped.
        std::vector<std::string_view> parts;
        for (std::size_t at = 0;;) {
            const auto tab = line.find('\t', at);
            parts.push_back(line.substr(at, tab == std::string_view::npos ? std::string_view::npos : tab - at));
            if (tab == std::string_view::npos) break;
            at = tab + 1;
        }
        if (parts.size() != 6) return std::nullopt;
        auto server = util::unescape_field(parts[0]);
        auto user = util::unescape_field(parts[1]);
        auto host = util::unescape_field(parts[2]);
        const auto port = port_of(parts[3]);
        auto key = util::unescape_field(parts[4]);
        auto value = util::unescape_field(parts[5]);
        if (!server || !user || !host || !port || !key || !value || server->empty() || key->empty()) {
            return std::nullopt;
        }
        store[*server][*key] = Secret{.binding = {.user = std::move(*user), .host = std::move(*host), .port = *port},
                                      .value = std::move(*value)};
    }
    if (first) return std::nullopt;
    return store;
}

namespace {

// The store a file's bytes hold; nullopt when they can't be decrypted or parsed.
std::optional<Store> unseal(std::string_view sealed) {
    if (!sealed.starts_with(sealed_header)) return std::nullopt;
    const auto text = util::unprotect_for_user(sealed.substr(sealed_header.size()), purpose);
    if (!text) return std::nullopt;
    return from_text(*text);
}

} // namespace

std::expected<ForChange, std::string> load_for_change(const fs::path& file) {
    auto data = util::read_file_if_exists(file);
    if (!data) return util::fail(data.error());
    if (!*data) return ForChange{};
    if (auto store = unseal(**data)) return ForChange{.store = std::move(*store), .set_aside = std::nullopt};
    auto moved = util::move_aside(file, "unreadable");
    if (!moved) return util::fail(moved.error());
    return ForChange{.store = {}, .set_aside = std::move(*moved)};
}

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

std::expected<void, std::string> save(const fs::path& file, const Store& store) {
    const auto sealed = util::protect_for_user(to_text(store), purpose);
    if (!sealed) return util::failf("cannot write {}: the passwords could not be encrypted", util::path_to_utf8(file));
    if (auto written = util::write_file_atomic(file, std::string(sealed_header) + *sealed); !written) return written;
    // Read back before anything relies on it: a password lost here can't be had again.
    const auto back = load(file);
    if (!back) return util::fail(back.error());
    if (*back != store) return util::failf("cannot write {}: it doesn't read back as written", util::path_to_utf8(file));
    return {};
}

const Secret* find(const Store& store, std::string_view server, std::string_view key) {
    const auto s = store.find(server);
    if (s == store.end()) return nullptr;
    const auto k = s->second.find(key);
    return k == s->second.end() ? nullptr : &k->second;
}

std::optional<std::string> value_for(const Store& store, std::string_view server, std::string_view key,
                                     const Binding& login) {
    const auto* secret = find(store, server, key);
    if (secret == nullptr || secret->binding != login) return std::nullopt;
    return secret->value;
}

std::string why_none(const Store& store, std::string_view server, std::string_view key, const Binding& login) {
    const std::string_view what = key == password ? "password" : key == passphrase ? "key passphrase" : key;
    if (const auto* secret = find(store, server, key); secret != nullptr && secret->binding != login) {
        return std::format("the saved {} is for {}, not {}: enter it again with Edit", what, secret->binding.text(),
                           login.text());
    }
    return std::format("no {} is saved: enter it with Edit", what);
}

} // namespace grab::passwords
