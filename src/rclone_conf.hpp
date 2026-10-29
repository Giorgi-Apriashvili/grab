#pragma once

// An rclone.conf as rclone 1.75.1 reads it (its config reader, goconfig v1.0.0), for what grab
// takes from one: the rclone run of a server still defined there (engine::rclone_env) and the
// one-time import of servers into grab.conf (rclone_import.hpp). grab's own ini reader (ini.hpp)
// stays for grab.conf. Checked against `rclone config dump` (tests/test_rclone_env.cpp;
// RCLONE-REPLACEMENT.md, progress log 2026-10-29):
// - A line is a comment when it starts with '#' or ';' after any indent; a '#' or ';' after a
//   value is part of it. A UTF-8 byte order mark and CRLF are fine.
// - A key ends at the first '=' or ':'; a key in "...", """...""" or `...` loses its quotes. Only
//   `...` and """...""" come off a value (from the first to the last one on the line); "..." and
//   '...' stay.
// - Keys before any section, and [DEFAULT]'s, are DEFAULT's, which no section falls back to: only
//   %(key)s reads it. The same key again replaces it; a section again adds to it; names are
//   case-sensitive; [ x ] is x.
// - A whole file is refused for one bad line: a header with something after it ("[s] # c"), keys
//   under "[]", a line without '=' or ':', a quote that isn't closed on its line.
// - %(key)s in a value is DEFAULT's key when DEFAULT has it (even blank), else the section's own
//   raw value, else nothing; up to 200 passes. grab refuses a value that grows past 64 KiB that
//   way, and DEFAULT keys that refer to themselves, which rclone would expand until it fails.
// - A dotted name ([box.eu]) takes a key it lacks from its parent section by exact name ([box]),
//   and so on up the chain; the chain stops at the first missing section, or at a blank value.

#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace grab::rclone_conf {

// The section that keys before any [section] go to, and that %(key)s reads first.
inline constexpr std::string_view default_section = "DEFAULT";

struct Section {
    std::string name;
    std::vector<std::string> keys;                          // in file order, each once
    std::map<std::string, std::string, std::less<>> values; // as on the line, quotes taken off
};

struct Document {
    std::vector<Section> sections; // in file order; DEFAULT among them when it has keys

    [[nodiscard]] const Section* find(std::string_view name) const;
};

// The file's text read as rclone reads it, or why rclone would refuse it ("line 3: ...").
// Not for an encrypted rclone.conf (is_encrypted_rclone_config): rclone decrypts that first.
[[nodiscard]] std::expected<Document, std::string> parse(std::string_view text);

// A value as rclone gets it.
struct Value {
    std::string text;    // %(key)s replaced
    std::string section; // where it was found: the section asked for, or a parent of a dotted name
};

// `key` of `section` as rclone reads it: the section's own value, else (`parents`) a dotted name's
// parent's. nullopt when the key isn't set or is blank (rclone takes a blank value as not set).
// An error when %(key)s makes it too big or a DEFAULT key refers to itself.
[[nodiscard]] std::expected<std::optional<Value>, std::string> get(const Document& doc, std::string_view section,
                                                                 std::string_view key, bool parents = true);

// Looks up an environment variable (as UTF-8; nullopt when unset).
using Getenv = std::function<std::optional<std::string>(std::string_view)>;

// A key_file or known_hosts_file as rclone expands it (lib/env ShellExpand): a leading "~" (alone
// or before '/' or '\') as the home folder (HOME, else USERPROFILE, else HOMEDRIVE and HOMEPATH),
// then $VAR and ${VAR} as the environment has them (Go's os.ExpandEnv), RCLONE_CONFIG_DIR as
// `config_dir`, the folder of the rclone.conf the value came from. What is substituted isn't
// expanded again. The result may still be relative.
[[nodiscard]] std::string expand_path(std::string_view value, const std::filesystem::path& config_dir,
                                      const Getenv& getenv);

} // namespace grab::rclone_conf
