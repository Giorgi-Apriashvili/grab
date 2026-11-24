#pragma once

// `grab upload LOCAL... [-r SERVER] REMOTE_DIR` (UPLOADS.md 5.11): files and folders from this PC
// into a folder on the server, with the window's upload engine (upload.hpp, upload_folder.hpp) and
// its check first (upload_check.hpp): what goes, whether this account may write there, and the
// files already there, decided by --existing or the window's setting, or asked on the console.
// The picked items go one after another, each with the server's whole connection budget, on one
// progress line (cli_console.hpp); picked files of one folder that follow each other (a pattern's)
// go together, as a folder's files do. Ctrl+C keeps what the server acknowledged of a big file,
// and the same command again goes on from there.

#include "cli.hpp"
#include "cli_console.hpp"
#include "engine.hpp"
#include "fetch.hpp"
#include "rate.hpp"
#include "upload_existing.hpp"

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace grab::ssh {
class Pool;
} // namespace grab::ssh

namespace grab {

// The command's exit codes beside 0, 1 (usage), 2 (config) and 130 (Ctrl+C), as a download's:
inline constexpr int exit_upload_not_found = 3; // a LOCAL or REMOTE_DIR not there, or [q]uit at the question
inline constexpr int exit_upload_failed = 6;    // a file failed; a server that refuses writing; no console to ask on

// Whether `name` matches `pattern`, whose '*' stands for any characters and '?' for one, ignoring
// case as Windows names do (both UTF-8).
[[nodiscard]] bool local_glob_match(std::string_view pattern, std::string_view name);

// The LOCAL arguments as paths, in their order, absolute: one whose last part has '*' or '?'
// becomes the names in its folder that match, sorted by name; a folder's last separator is
// dropped ("E:\photos\" is E:\photos). A pattern leaves out what an upload of its folder would
// (local_walk::left_out: links and junctions, grab's own partial downloads), each such pattern
// said in `notes` ("E:\dl\*: 2 links and 1 partial download left out (name them to send them)");
// a name typed in full is taken as it is. The error: a LOCAL that isn't there, or a pattern that
// matches nothing (or only what is left out).
[[nodiscard]] std::expected<std::vector<std::filesystem::path>, std::string>
expand_locals(const std::vector<std::string>& typed, std::vector<std::string>* notes = nullptr);

// An answer typed at the question about files already on the server: a choice (r replace,
// d different, n newer, s skip, k or b both, or the words), for this file or, typed as a capital
// letter, the rest too; o (one by one, where `one_by_one`) or q (quit). nullopt for anything else.
struct Typed {
    enum class Kind { choice, one_by_one, quit };
    Kind kind = Kind::quit;
    upload::Existing choice = upload::Existing::skip;
    bool rest = false;
};
[[nodiscard]] std::optional<Typed> read_typed(std::string_view line, bool one_by_one);

// What a `grab upload` run works with besides its command line: the server, its pool and budget,
// the window's settings it follows, and the console (the tests' fakes, or upload_with_grab's).
struct UploadSetup {
    std::string server;                   // grab.conf section: the pool, and its name in messages
    const engine::Context* ctx = nullptr; // verify's
    std::shared_ptr<ssh::Pool> pool;
    std::shared_ptr<ssh::Pool> verify_pool;    // verify's checksums (the tests' md5sum server); nullptr: `pool`
    fetch::Connections* connections = nullptr; // configured for `server` with `budget`
    int budget = 0;
    std::filesystem::path records_dir;    // partial uploads' records (upload::Upload::records_dir)
    upload::Existing setting = upload::Existing::ask; // the window's: a file already on the server
    bool verify = false;                  // each upload against the server's checksum afterwards
    int files_at_once = 4;                // a folder's files at once: the window's "At once"
    rate::RateLimiter* limiter = nullptr; // the upload speed limit
    LiveLine* line = nullptr;             // where it says what it does
    // Whether there is a console to ask on, and its answer to a prompt (nullopt: input ended).
    bool console = false;
    std::function<std::optional<std::string>(const std::string& prompt)> ask;
    engine::CommandHook trace; // -v
    std::stop_token stop;      // Ctrl+C
};

// The run: LOCAL looked at (`locals`, expanded), REMOTE_DIR and the server's side, then each
// item uploaded in turn, or with opts.dry_run only said what would happen (nothing written on the
// server or here). Returns the exit code.
[[nodiscard]] int run_upload(const Options& opts, const std::vector<std::filesystem::path>& locals, UploadSetup& s);

// The command: LOCAL expanded, the window's settings it follows (the speed schedule's ↑ speeds
// without --limit, Verify, At once, what to do with a file already there, limits.json's learned
// connection limits), Ctrl+C, the console, and the server's pool; then run_upload.
[[nodiscard]] int upload_with_grab(const engine::Context& ctx, const Options& opts);

} // namespace grab
