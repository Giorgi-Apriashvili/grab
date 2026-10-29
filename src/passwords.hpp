#pragma once

// grab's own password store (RCLONE-REPLACEMENT.md, phase 7): 'passwords' in the configuration's
// state_dir (gui_state.hpp; %APPDATA%\grab for the default configuration), kept there because
// two configurations can have servers of the same name and 0.19 must still read its old
// 'secrets' (secrets.hpp), which grab no longer writes. Keyed by grab.conf section; grab.conf
// holds no secret and no pointer to one. Values are plain inside the encryption (DPAPI, for the
// current Windows user): a password, a key's passphrase, an imported inline key (key_pem, as
// rclone keeps it), an rclone setting that can carry a password ("rclone.<key>"). Each value is
// bound to the login it was saved for, user@host:port, and a lookup for another one finds
// nothing, so a hand edit of grab.conf's host can't send a saved password to another machine.
// Elsewhere than Windows the file is plain text, readable by its owner only (mode 0600).

#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace grab::passwords {

// The keys a server's values are saved under; an rclone setting is "rclone.<its key>".
inline constexpr std::string_view password = "password";
inline constexpr std::string_view passphrase = "passphrase";
inline constexpr std::string_view key_pem = "key_pem";
inline constexpr std::string_view rclone_prefix = "rclone.";

// The login a value was saved for.
struct Binding {
    std::string user;
    std::string host;
    int port = 22;
    bool operator==(const Binding&) const = default;
    // "user@host:port", for messages.
    [[nodiscard]] std::string text() const;
};

struct Secret {
    Binding binding;
    std::string value;
    bool operator==(const Secret&) const = default;
};

// server -> key -> its value and login.
using Store = std::map<std::string, std::map<std::string, Secret, std::less<>>, std::less<>>;

// Whether the file is encrypted (Windows: DPAPI); elsewhere it is only readable by its owner.
[[nodiscard]] bool encrypted();

// The store of the configuration whose state_dir is `state_dir`: "passwords" in it.
[[nodiscard]] std::filesystem::path path_in(const std::filesystem::path& state_dir);

// The text form (a header, then one escaped "server\tuser\thost\tport\tkey\tvalue" line each)
// and back; nullopt for anything else.
[[nodiscard]] std::string to_text(const Store& store);
[[nodiscard]] std::optional<Store> from_text(std::string_view text);

// A missing file is an empty store; one that can't be decrypted or read is an error.
[[nodiscard]] std::expected<Store, std::string> load(const std::filesystem::path& file);
// Encrypted, replaced in one step (util::write_file_atomic), then read back: an error unless the
// file then holds `store`.
[[nodiscard]] std::expected<void, std::string> save(const std::filesystem::path& file, const Store& store);

// `key` of `server` when it was saved for `login`; nullopt when there is none or it was saved for
// another login (find tells which).
[[nodiscard]] std::optional<std::string> value_for(const Store& store, std::string_view server, std::string_view key,
                                                   const Binding& login);
// `key` of `server` whatever login it was saved for, or nullptr.
[[nodiscard]] const Secret* find(const Store& store, std::string_view server, std::string_view key);

} // namespace grab::passwords
