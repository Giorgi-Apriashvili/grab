#pragma once

// Checking downloads against the server's copy (optional: Settings → Downloads, `grab --verify`).
// One `rclone check` per download compares checksums that the server computes (md5sum over SSH)
// with the local files'. Both sides read the files once more.

#include "engine.hpp"
#include "fetch.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace grab::verify {

enum class Verdict { same, differs, missing, error };
struct Line {
    Verdict verdict = Verdict::same;
    std::string path;
    bool operator==(const Line&) const = default;
};
// `rclone check --combined -` output: "= path" same, "* path" differs, "+ path" missing locally,
// "! path" could not be read or hashed. Other lines are skipped.
[[nodiscard]] std::vector<Line> parse_combined(std::string_view out);

// From rclone's log: the checksum it compared ("md5"), "" when the two sides have none in common
// (it then compared sizes only), and how many files it could not hash.
[[nodiscard]] std::string hash_used(std::string_view log);
[[nodiscard]] std::size_t unhashed(std::string_view log);

[[nodiscard]] std::string remote_parent(std::string_view remote_path); // "/a/b/c" -> "/a/b", "c" -> ""

struct Request {
    const engine::Context* ctx = nullptr;
    std::string remote_dir;           // the server folder the paths are relative to
    std::filesystem::path local_dir;  // the local copy of that folder
    std::vector<std::string> paths;   // relative, '/'-separated
    std::vector<std::string> extra;   // added to the rclone call (`grab ... -- extra`)
    // The connections the check takes from the server's pool: one for a file, up to the
    // download's share for a folder.
    fetch::Connections* connections = nullptr;
    std::string server;
    int id = 0;
    bool joined = false; // the caller is in the pool under `id` already
    bool many = false;
    engine::CommandHook on_command;
};
[[nodiscard]] std::vector<std::string> check_argv(const Request& r, const std::filesystem::path& list_file,
                                                  int connections);

struct Report {
    std::string hash;                // "md5"; "" when the server can't compute checksums
    std::vector<std::string> differ; // copies that differ from the server's, or are missing
    std::vector<std::string> same;   // copies that match, when rclone says which
    std::size_t unchecked = 0;       // files either side could not hash
    std::size_t matched = 0;
    [[nodiscard]] bool verified() const { return !hash.empty() && differ.empty() && unchecked == 0; }
};
// Runs the check. An error means it could not run at all (the server unreachable, say); a
// stop returns an error too.
[[nodiscard]] std::expected<Report, std::string> run(const Request& r, std::stop_token stop);
// One downloaded file, `target` being the local copy of `remote_path` (under the same name);
// `base` gives the context, the pool and the hooks.
[[nodiscard]] std::expected<Report, std::string> file(Request base, const std::string& remote_path,
                                                      const std::filesystem::path& target, std::stop_token stop);

// For the queue row and the command: "checksum verified (MD5)", or for a folder's files "3 files
// verified (MD5)", or why not ("not verified: the server can't compute checksums").
[[nodiscard]] std::string summary(const Report& r, std::size_t files, bool folder = false);

} // namespace grab::verify
