#pragma once

// The 'secrets' store of grab 0.19 and before: server secrets kept out of grab's rclone.conf, in
// rclone's obscured form (which anyone who can read it can reverse) inside a file encrypted for
// the current Windows user (DPAPI, util::protect_for_user). Since phase 7 (RCLONE-REPLACEMENT.md)
// grab only reads it, to import its servers (rclone_import.hpp) into grab's own store
// (passwords.hpp); 0.19 still uses it. Also rclone's naming of a remote's variables and its
// obscuring, for each rclone run's environment (engine::rclone_env).

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

// Encrypted, and replaced in one step (util::write_file_atomic). grab itself never writes the
// store any more (RCLONE-REPLACEMENT.md, phase 7): its passwords are in passwords.hpp's.
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
// The reverse, as rclone's `obscure` makes it: a new random IV each time, so the same text is
// obscured differently each time and `reveal` (grab's or rclone's) gives it back. Only for a
// secret that goes into one rclone run's environment (engine::rclone_env), which takes passwords
// and passphrases in this form alone; never through another program's stdin or command line.
// nullopt when no random IV can be had.
[[nodiscard]] std::optional<std::string> obscure(std::string_view plain);

// rclone.conf text without its secret lines, and the secrets taken from it. Comments, other
// keys, order and line endings stay as they were.
struct Taken {
    std::string text;
    Store found;
};
[[nodiscard]] Taken take_secrets(std::string_view rclone_text);

} // namespace grab::secrets
