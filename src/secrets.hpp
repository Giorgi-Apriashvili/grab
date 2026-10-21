#pragma once

// Server secrets kept out of grab's rclone.conf. rclone stores passwords there only "obscured",
// which anyone who can read the file can reverse. grab keeps them instead in a file encrypted for
// the current Windows user (DPAPI, util::protect_for_user) and hands them to rclone through
// environment variables (RCLONE_CONFIG_<REMOTE>_<KEY>, which rclone reads like config values), so
// they are never on disk unencrypted nor on a command line.

#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace grab::secrets {

// rclone sftp settings that are secrets: the password, a key file's passphrase, an inline key.
[[nodiscard]] bool is_secret_key(std::string_view key);

// remote -> key -> value, the value as rclone keeps it (obscured).
using Store = std::map<std::string, std::map<std::string, std::string>>;

// The store of grab's own rclone.conf: "secrets" next to it.
[[nodiscard]] std::filesystem::path path_beside(const std::filesystem::path& rclone_conf);

// The text form (one escaped "remote\tkey\tvalue" line each, after a header) and back.
[[nodiscard]] std::string to_text(const Store& store);
[[nodiscard]] std::optional<Store> from_text(std::string_view text);

// A missing file is an empty store. An unreadable one (another Windows user's, damaged) is an
// error, and the store on disk is then left alone.
[[nodiscard]] std::expected<Store, std::string> load(const std::filesystem::path& file);
// Encrypted, and replaced in one step (util::write_file_atomic).
[[nodiscard]] std::expected<void, std::string> save(const std::filesystem::path& file, const Store& store);

// The environment variable rclone reads for `key` of `remote`, as rclone's ConfigToEnv names it:
// RCLONE_CONFIG_<REMOTE>_<KEY>, upper case, '-' as '_' in the key only (my-box's password is
// RCLONE_CONFIG_MY-BOX_PASS).
[[nodiscard]] std::string env_name(std::string_view remote, std::string_view key);

// Puts every secret of `store` into this process's environment, so the rclone processes grab
// starts inherit them, and removes the ones it put there before that `store` no longer has.
void export_to_env(const Store& store);

// rclone.conf text without its secret lines, and the secrets taken from it. Comments, other
// keys, order and line endings stay as they were.
struct Taken {
    std::string text;
    Store found;
};
[[nodiscard]] Taken take_secrets(std::string_view rclone_text);

// For grab's own rclone.conf, before running rclone: moves any secrets still in it into the store
// (saved and read back before the file loses them), then exports the store. Returns why the
// store could not be read, if it couldn't; servers with passwords then can't log in.
[[nodiscard]] std::expected<void, std::string> prepare(const std::filesystem::path& rclone_conf);

} // namespace grab::secrets
