#include "secrets.hpp"

#include "util.hpp"

#include <format>
#include <utility>
#include <vector>
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

std::expected<void, std::string> save(const fs::path& file, const Store& store) {
    const auto sealed = util::protect_for_user(to_text(store), purpose);
    if (!sealed) return util::failf("cannot write {}: the passwords could not be encrypted", util::path_to_utf8(file));
    return util::write_file_atomic(file, std::string(sealed_header) + *sealed);
}

} // namespace grab::secrets
