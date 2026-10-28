#pragma once

// Server secrets kept out of grab's rclone.conf. rclone stores passwords there only "obscured",
// which anyone who can read the file can reverse. grab keeps them instead in a file encrypted for
// the current Windows user (DPAPI, util::protect_for_user) and hands a server's to the rclone
// process for that server alone, in that process's environment (RCLONE_CONFIG_<REMOTE>_<KEY>,
// which rclone reads like config values; engine::rclone_env), so they are never on disk
// unencrypted, on a command line or in grab's own environment.

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

// The store about to be changed. One that was read but can't be decrypted or parsed (another
// Windows user's, damaged, or DPAPI failing for now, as after a domain password reset) is moved
// aside to "<file>.unreadable-<time>" and the change starts from an empty store, its passwords
// kept should they become readable again; `set_aside` then names that file (review G211). A file
// that can't be read at all is an error: the change has to wait rather than replace it.
struct ForChange {
    Store store;
    std::optional<std::filesystem::path> set_aside;
};
[[nodiscard]] std::expected<ForChange, std::string> load_for_change(const std::filesystem::path& file);
// Encrypted, and replaced in one step (util::write_file_atomic).
[[nodiscard]] std::expected<void, std::string> save(const std::filesystem::path& file, const Store& store);

// The environment variable rclone reads for `key` of `remote`, as rclone's ConfigToEnv names it:
// RCLONE_CONFIG_<REMOTE>_<KEY> in upper case as Go makes it (rclone_enc::go_upper: Georgian too),
// '-' as '_' in the key only (my-box's password is RCLONE_CONFIG_MY-BOX_PASS). Only the rclone
// process for that remote gets it (engine::rclone_env), never grab's own environment (review G7).
[[nodiscard]] std::string env_name(std::string_view remote, std::string_view key);
// What every one of `remote`'s variables starts with: RCLONE_CONFIG_<REMOTE>_.
[[nodiscard]] std::string env_prefix(std::string_view remote);

// A value as rclone obscures it, made plain again, for grab's own SSH client (which takes
// secrets in memory, never through the environment). rclone's lib/obscure: URL-safe base64
// without padding of a 16-byte IV and the text encrypted with AES-256-CTR under a key rclone
// publishes (obscuring hides a value from a glance, nothing more). nullopt when it isn't one.
[[nodiscard]] std::optional<std::string> reveal(std::string_view obscured);

// rclone.conf text without its secret lines, and the secrets taken from it. Comments, other
// keys, order and line endings stay as they were.
struct Taken {
    std::string text;
    Store found;
};
[[nodiscard]] Taken take_secrets(std::string_view rclone_text);

// For grab's own rclone.conf, before running rclone: moves any secrets still in it into the store
// (saved and read back before the file loses them). Returns why the store could not be read, if
// it couldn't; servers with passwords then can't log in.
[[nodiscard]] std::expected<void, std::string> prepare(const std::filesystem::path& rclone_conf);

} // namespace grab::secrets
