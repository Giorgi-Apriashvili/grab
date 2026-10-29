#pragma once

// The one-time import of servers from rclone.conf into grab.conf (RCLONE-REPLACEMENT.md, phase 7
// step 7), as a pure function: what grab 0.19 and before kept in rclone.conf and the old 'secrets'
// store (secrets.hpp) becomes each grab.conf section's connection details (server_conf.hpp), its
// passwords in grab's own store (passwords.hpp, plain inside the encryption) and its host keys in
// grab's known_hosts. The apply step reads the files, runs `plan` under the settings lock and
// writes what it says, in this order: the store (saved and read back), then known_hosts
// (servers::merge_known_hosts, which skips lines already there), then grab.conf, then
// imported.json. A section with `host` is never imported again, so grab.conf goes after what it
// needs. A 'passwords' file that is there but can't be loaded stops the import (no plan, a note):
// passing it as an empty Inputs.store would replace it. Nothing the import reads is ever written:
// rclone.conf and the old store stay as they are, so 0.19 still works with them.
//
// For each grab.conf section without `host` that this configuration hasn't imported before
// (imported.json in its state_dir): its rclone remote (`rclone_remote`, else its name) from the
// sources in order, read as rclone reads them (rclone_conf.hpp: a dotted name takes non-secret
// keys from its parent section, never a secret). Secrets from the store beside that rclone.conf,
// then its own lines; only `pass` and `key_file_pass` are revealed, `key_pem` is kept as rclone
// keeps it. The login method in rclone's order: key_use_agent -> agent (with its key_file), else
// key_pem -> stored_key, else key_file -> key (key_file_pass its passphrase), else pass ->
// password, else agent, unless ask_password or a parent's own `pass` line (password, none saved).
// A parent's secret rclone read from the file gets a note to enter it with Edit. A password
// beside a key is kept. A value grab's store already holds for that server and the same login
// stays (it is newer: a `grab --init` after grab.conf was deleted imports again), with a note
// when it differs; one bound to another login is replaced. key_file and known_hosts_file expanded as rclone does (~, $VAR,
// ${RCLONE_CONFIG_DIR} as the file's folder) and made absolute. Host keys: the remote's own
// known_hosts lines for its [host]:port are copied when grab's known_hosts has none for it; a
// network or device path, a file over 1 MiB, @cert-authority and wildcard lines and lines with a
// key the file revokes are skipped with a note to use Trust. shell_type, md5sum_command,
// sha1sum_command and path_override become grab.conf keys, the allowlisted sftp settings
// `rclone.<key>`, every other setting an rclone extra in the store. A section that fails (no such
// remote, not sftp, encrypted, an old store that can't be read for a server that needs its
// password, details grab refuses) keeps no details and gets a note; it is tried again only when a
// source file or its rclone_remote changes. A name once imported is never imported again, so a
// removed server doesn't come back from the old files. A second run changes nothing.

#include "config.hpp"
#include "passwords.hpp"
#include "rclone_conf.hpp"
#include "secrets.hpp"
#include "server_conf.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace grab::rclone_import {

// A file's size and last write time as the import saw them, to tell whether it changed since.
struct Stamp {
    std::string file;       // UTF-8
    std::int64_t size = -1; // -1: no such file
    std::int64_t time = 0;  // the file system's last write time, in its own ticks
    bool operator==(const Stamp&) const = default;
};

// A section the import couldn't import: tried again when its remote or a stamp changes.
struct Failure {
    std::string remote; // its rclone_remote, else its name
    std::vector<Stamp> stamps;
    bool operator==(const Failure&) const = default;
};

// imported.json in the configuration's state_dir: what the import did, so it does it once.
struct Record {
    std::set<std::string, std::less<>> imported;        // names never imported again
    std::map<std::string, Failure, std::less<>> failed; // by section name
    std::vector<std::string> banner;                               // imported servers the window still has to name once
    bool operator==(const Record&) const = default;
};
[[nodiscard]] std::filesystem::path record_path(const std::filesystem::path& state_dir);
[[nodiscard]] std::string to_json(const Record& record);
// nullopt when `text` isn't a record.
[[nodiscard]] std::optional<Record> from_json(std::string_view text);

// The rclone.conf files the import reads, in order: [grab] rclone_config alone when it is set,
// else grab's own rclone.conf, then the standard one.
[[nodiscard]] std::vector<std::filesystem::path> source_files(const GrabConfig& cfg);

// One of those files, as the apply step read it, with the old store beside it
// (secrets::path_beside).
struct Source {
    std::filesystem::path file;
    std::optional<std::string> text; // nullopt: no such file
    std::string error;               // non-empty: it is there but couldn't be read
    std::expected<secrets::Store, std::string> store = secrets::Store{};
};

// A remote's own known_hosts_file, read by the apply step: at most 1 MiB, else `error`.
struct HostKeysFile {
    std::optional<std::string> text;
    std::string error;
};
inline constexpr std::size_t max_host_keys_file = std::size_t{1024} * 1024;

struct Inputs {
    std::vector<Source> sources;
    std::vector<Stamp> stamps;         // each source's and its store's, now
    std::string grab_text;             // grab.conf
    std::filesystem::path known_hosts; // grab's
    std::string known_hosts_text;
    passwords::Store store; // grab's new store as it is
    Record record;          // imported.json as it is
    rclone_conf::Getenv getenv;
    std::function<bool(const std::filesystem::path&)> exists; // whether a key file is there
    std::function<HostKeysFile(const std::filesystem::path&)> read_host_keys;
};

struct Note {
    std::string server;
    std::string text;
    bool operator==(const Note&) const = default;
};

struct Imported {
    std::string name;
    std::filesystem::path from; // the rclone.conf
    server_conf::Connection connection;
    std::vector<std::string> from_parent; // keys a dotted name took from a parent section
    bool password_saved = false;
    bool pinned = false;
};

struct Plan {
    std::string grab_text;                   // grab.conf as it is to be (the input when nothing changes)
    passwords::Store store;                  // the store as it is to be, saved and read back first
    std::vector<std::string> host_key_lines; // for grab's known_hosts (servers::merge_known_hosts)
    Record record;                           // imported.json as it is to be
    std::vector<Imported> imported;
    std::vector<std::string> failed; // sections tried and not imported
    std::vector<Note> notes;
    // Whether anything is to be written.
    [[nodiscard]] bool changes(const Inputs& in) const;
};

// The import over `in`; an error only when grab.conf can't be read.
[[nodiscard]] std::expected<Plan, std::string> plan(const Inputs& in);

// A path the import never reads (a remote's known_hosts_file, a key file it would check): a
// network or device path ("\\server\share\...", "\\.\...", "//..."), anything in NT's own
// namespace (a separator, "??", a separator: UNC, GLOBALROOT, devices, drives too), or a Windows
// device name.
[[nodiscard]] bool network_or_device(std::string_view path);

// The lines of a known_hosts file (`text`) that pin host:port, each with its host list cut down to
// the names that do (servers::names_host), and what was skipped: @cert-authority lines, lines whose
// wildcard pattern matches and lines whose key a @revoked line revokes (for any host, as Go's
// knownhosts does), which only Trust replaces. The @revoked lines themselves aren't copied.
struct HostKeyLines {
    std::vector<std::string> lines;
    std::vector<std::string> skipped; // "a @cert-authority line", "a wildcard line (*.example)"
};
[[nodiscard]] HostKeyLines host_key_lines(std::string_view text, std::string_view host, int port);

// ---- the apply step ---------------------------------------------------------------------------

// What the import of one configuration reads and writes: its grab.conf, its state_dir
// (gui_state.hpp: the 'passwords' store and imported.json) and grab's known_hosts.
struct Files {
    std::filesystem::path grab_conf;
    std::filesystem::path state_dir;
    std::filesystem::path known_hosts;
};
[[nodiscard]] Files files_for(const std::filesystem::path& grab_conf);

// A file's Stamp now.
[[nodiscard]] Stamp stamp_of(const std::filesystem::path& file);
// An rclone.conf as the import reads it, with the old store beside it (read only when the file is
// there).
[[nodiscard]] Source read_source(const std::filesystem::path& file);

// `files` as `plan` takes them, read now; nothing is written. An error when grab.conf, grab's
// known_hosts, the 'passwords' store or imported.json is there but can't be read: the import then
// waits (a store taken as empty would be replaced).
[[nodiscard]] std::expected<Inputs, std::string> read_inputs(const Files& files);

struct Result {
    std::vector<Imported> imported;
    std::vector<std::string> failed;
    std::vector<Note> notes;
};

// The apply step, run by load_context, load_env, --init and the window's settings thread. First,
// without the lock, whether any section waits for the import (no `host`, not imported before,
// not failed with the same remote and source files): when none does, as on almost every run, it
// returns at once. Else under the settings lock: read_inputs, plan, then the writes in the order
// the header gives (store, known_hosts, grab.conf, imported.json), each only when it changes. A
// write that fails stops it with an error; what was written before is what the next run plans
// from, and it finishes then (the store and known_hosts lines come out the same again, and a
// section is done once grab.conf has its `host`). No grab.conf: nothing to do.
[[nodiscard]] std::expected<Result, std::string> apply(const Files& files);

} // namespace grab::rclone_import
