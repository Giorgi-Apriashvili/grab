#include "verify.hpp"

#include "listing.hpp"
#include "local_name.hpp"
#include "process.hpp"
#include "rclone.hpp"
#include "rclone_encoding.hpp"
#include "rclone_flags.hpp"
#include "server_ops.hpp"
#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <span>
#include <system_error>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)
#else
#include <unistd.h>
#endif

namespace grab::verify {

namespace fs = std::filesystem;

namespace {

unsigned long process_id() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(getpid());
#endif
}

std::string upper(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

// A --files-from0 list: a name can hold a line break, never a NUL (review G18).
std::expected<void, std::string> write_list(const fs::path& file, std::span<const std::string> paths) {
    std::string text;
    for (const auto& p : paths) {
        text += p;
        text += '\0';
    }
    return util::write_file_atomic(file, text);
}

proc::RunOptions quiet(std::stop_token stop) {
    proc::RunOptions o;
    o.detached = true;
    o.stop = std::move(stop);
    return o;
}

// The plain names: one `rclone check` over all of them.
std::expected<Report, std::string> check_names(const Request& r, std::span<const std::string> paths, int held,
                                               const std::stop_token& stop) {
    const auto list = fs::temp_directory_path() / std::format("grab-verify-{}-{}.txt", process_id(), r.id);
    if (auto written = write_list(list, paths); !written) return util::fail(written.error());
    const auto built = check_argv(r, list, held);
    if (!built) {
        std::error_code ec;
        fs::remove(list, ec);
        return util::fail(built.error());
    }
    const auto& argv = *built;
    if (r.on_command) r.on_command(argv);
    auto ran = proc::run_capture(argv, quiet(stop));
    std::error_code ec;
    fs::remove(list, ec);
    if (stop.stop_requested()) return util::fail("stopped");
    if (!ran) return util::fail(ran.error());

    const auto lines = parse_combined(ran->out);
    if (lines.empty()) {
        // A clean exit with nothing compared: the files were gone on both sides, say.
        if (ran->exit_code == 0) return util::fail("rclone check compared nothing");
        return util::fail(server_ops::clean_rclone_error(ran->err));
    }
    Report report;
    report.hash = hash_used(ran->err);
    for (const auto& line : lines) {
        switch (line.verdict) {
        case Verdict::same:
            ++report.matched;
            report.same.push_back(line.path);
            break;
        case Verdict::differs:
        case Verdict::missing: report.differ.push_back(line.path); break;
        case Verdict::error: ++report.unchecked; break;
        }
    }
    // Files the server no longer has are not reported at all.
    if (lines.size() < paths.size()) report.unchecked += paths.size() - lines.size();
    const std::size_t no_hash = unhashed(ran->err);
    if (no_hash > 0) { // counted as matches by rclone, without saying which
        report.unchecked += std::min(no_hash, report.matched);
        report.matched -= std::min(no_hash, report.matched);
        report.same.clear();
    }
    if (report.hash.empty()) report.same.clear(); // sizes only
    return report;
}

// Files rclone check can't pair by name (device names, renamed copies, or all of them when the
// folder's name can't be given to rclone): the server's MD5 sums from one `rclone hashsum`, each
// copy's from grab, added to `report`.
std::expected<void, std::string> compare_sums(const Request& r, std::span<const std::string> paths, int held,
                                              const std::stop_token& stop, Report& report) {
    const auto& ctx = *r.ctx;
    auto hashsum_argv = [&](const std::string& where) {
        std::vector<std::string> argv{ctx.config.rclone, "hashsum", "md5", remote_spec(ctx.settings.rclone_remote, where)};
        if (ctx.config.rclone_config) argv.insert(argv.end(), {"--config", util::path_to_utf8(*ctx.config.rclone_config)});
        const auto flags = rclone_flags::connection(ctx.settings.common_flags);
        argv.insert(argv.end(), flags.begin(), flags.end());
        const auto extra = rclone_flags::connection(r.extra);
        argv.insert(argv.end(), extra.begin(), extra.end());
        return argv;
    };
    // hashsum prints a line per file, so a name with a line break is asked about alone and its
    // sum read off the start of the answer (review G18).
    std::vector<std::string> by_list;
    std::vector<std::string> alone;
    for (const auto& p : paths) (p.find_first_of("\r\n") == std::string::npos ? by_list : alone).push_back(p);
    std::unordered_map<std::string, std::string> server;
    if (!by_list.empty()) {
        const auto list = fs::temp_directory_path() / std::format("grab-verify-{}-{}-sums.txt", process_id(), r.id);
        if (auto written = write_list(list, by_list); !written) return util::fail(written.error());
        auto argv = hashsum_argv(r.remote_dir);
        const auto n = std::to_string(std::max(held, 1));
        argv.insert(argv.end(), {"--files-from0", util::path_to_utf8(list), "--checkers", n, "--sftp-connections", n,
                                 "--sftp-disable-hashcheck=false"});
        if (r.on_command) r.on_command(argv);
        auto ran = proc::run_capture(argv, quiet(stop));
        std::error_code ec;
        fs::remove(list, ec);
        if (stop.stop_requested()) return util::fail("stopped");
        if (!ran) return util::fail(ran.error());
        server = parse_sums(ran->out);
    }
    for (const auto& p : alone) {
        auto argv = hashsum_argv(join_remote(r.remote_dir, p));
        argv.emplace_back("--sftp-disable-hashcheck=false");
        if (r.on_command) r.on_command(argv);
        auto ran = proc::run_capture(argv, quiet(stop));
        if (stop.stop_requested()) return util::fail("stopped");
        if (!ran) return util::fail(ran.error());
        const std::string_view out = ran->out;
        if (out.size() >= 34 && out.substr(32, 2) == "  " &&
            std::ranges::all_of(out.substr(0, 32), [](unsigned char c) { return std::isxdigit(c) != 0; })) {
            std::string lower(out.substr(0, 32));
            std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            server.insert_or_assign(p, std::move(lower));
        }
    }
    std::error_code ec;
    bool hashed = false;
    for (const auto& p : paths) {
        const auto theirs = server.find(p);
        if (theirs == server.end()) { // gone from the server, or it could not hash it
            ++report.unchecked;
            continue;
        }
        hashed = true;
        const auto renamed = r.renamed.find(p);
        const auto copy = r.local_dir / (renamed != r.renamed.end() ? renamed->second : local::relative(p));
        if (!fs::is_regular_file(copy, ec)) {
            report.differ.push_back(p);
            continue;
        }
        // grab sums its own copy: no local path for rclone to read under another name (G80).
        if (stop.stop_requested()) return util::fail("stopped");
        const auto mine = util::md5_file(copy);
        if (!mine) {
            ++report.unchecked;
        } else if (*mine == theirs->second) {
            ++report.matched;
            report.same.push_back(p);
        } else {
            report.differ.push_back(p);
        }
    }
    if (hashed && report.hash.empty()) report.hash = "md5";
    return {};
}

} // namespace

std::vector<Line> parse_combined(std::string_view out) {
    std::vector<Line> lines;
    while (!out.empty()) {
        const auto end = out.find('\n');
        std::string_view line = out.substr(0, end);
        out.remove_prefix(end == std::string_view::npos ? out.size() : end + 1);
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (line.size() < 3 || line[1] != ' ') continue;
        Verdict v{};
        switch (line[0]) {
        case '=': v = Verdict::same; break;
        case '*': v = Verdict::differs; break;
        case '+': v = Verdict::missing; break;
        case '!': v = Verdict::error; break;
        default: continue; // "-": only local, which a one-way check doesn't report
        }
        lines.push_back(Line{v, std::string(line.substr(2))});
    }
    return lines;
}

std::string hash_used(std::string_view log) {
    if (log.find("No common hash found") != std::string_view::npos) return "";
    const auto at = log.find(" for hash comparisons");
    if (at == std::string_view::npos) return "";
    const auto start = log.rfind("Using ", at);
    if (start == std::string_view::npos) return "";
    return std::string(log.substr(start + 6, at - start - 6));
}

std::size_t unhashed(std::string_view log) {
    const auto at = log.find(" hashes could not be checked");
    if (at == std::string_view::npos) return 0;
    std::size_t start = at;
    while (start > 0 && std::isdigit(static_cast<unsigned char>(log[start - 1])) != 0) --start;
    std::size_t n = 0;
    for (std::size_t i = start; i < at; ++i) n = n * 10 + static_cast<std::size_t>(log[i] - '0');
    return n;
}

std::unordered_map<std::string, std::string> parse_sums(std::string_view out) {
    std::unordered_map<std::string, std::string> sums;
    while (!out.empty()) {
        const auto end = out.find('\n');
        std::string_view line = out.substr(0, end);
        out.remove_prefix(end == std::string_view::npos ? out.size() : end + 1);
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (line.size() < 35 || line.substr(32, 2) != "  ") continue;
        const auto hex = line.substr(0, 32);
        if (!std::ranges::all_of(hex, [](unsigned char c) { return std::isxdigit(c) != 0; })) continue;
        std::string lower(hex);
        std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        sums.insert_or_assign(std::string(line.substr(34)), std::move(lower));
    }
    return sums;
}

std::expected<std::vector<std::string>, std::string> check_argv(const Request& r, const fs::path& list_file,
                                                                int connections) {
    const auto& ctx = *r.ctx;
    // As the folder batch: the folder as rclone must be told it to look in grab's (G80).
    auto local = rclone_enc::local_path(r.local_dir);
    if (!local) return util::fail(local.error());
    std::vector<std::string> argv{ctx.config.rclone, "check", remote_spec(ctx.settings.rclone_remote, r.remote_dir),
                                  std::move(*local)};
    if (ctx.config.rclone_config) argv.insert(argv.end(), {"--config", util::path_to_utf8(*ctx.config.rclone_config)});
    // Only connection flags (rclone_flags.hpp): a --size-only made check compare sizes alone while
    // grab reported an MD5 check (G184), a --log-file hid which checksum was used (G187).
    const auto flags = rclone_flags::connection(ctx.settings.common_flags);
    argv.insert(argv.end(), flags.begin(), flags.end());
    const auto extra = rclone_flags::connection(r.extra);
    argv.insert(argv.end(), extra.begin(), extra.end());
    // Last, so they win over the common flags: grab's default --sftp-disable-hashcheck would
    // leave the server without checksums. -v logs which checksum was compared.
    const auto n = std::to_string(std::max(connections, 1));
    // --no-traverse: each file looked up by name on both sides. Listing the folders instead,
    // rclone 1.75.1 paired none of the names with a tab, a control character, DEL or ‛ in them.
    argv.insert(argv.end(), {"--one-way", "--files-from0", util::path_to_utf8(list_file), "--no-traverse",
                             "--combined", "-", "--checkers", n, "--sftp-connections", n,
                             "--sftp-disable-hashcheck=false", "-v"});
    return argv;
}

std::expected<Report, std::string> run(const Request& r, std::stop_token stop) {
    int held = 1;
    if (r.connections != nullptr) {
        if (!r.joined) r.connections->join(r.server, r.id);
        held = r.many ? r.connections->reserve(r.server, r.id, stop)
                      : (r.connections->acquire(r.server, r.id, stop) ? 1 : 0);
    }
    auto done_with_pool = [&] {
        if (r.connections == nullptr) return;
        if (held > 0) r.connections->release(r.server, r.id, held);
        if (!r.joined) r.connections->leave(r.server, r.id);
    };
    if (held == 0) {
        done_with_pool();
        return util::fail("stopped");
    }

    std::vector<std::string> plain;
    std::vector<std::string> devices;
    // A folder rclone can't be told the name of (rclone_encoding.hpp): every file by its sum.
    const bool checkable = rclone_enc::local_path(r.local_dir).has_value();
    // check would compare one of names it takes for one and drop the others (local::clashing), and
    // a line break in a name splits its report line: those by their sums (review G25, G18).
    const auto clash = local::clashing(r.paths);
    for (const auto& p : r.paths) {
        const bool breaks = p.find_first_of("\r\n") != std::string::npos;
        const bool by_sum = !checkable || breaks || clash.contains(p) || local::differs_from_rclone(p) || r.renamed.contains(p);
        (by_sum ? devices : plain).push_back(p);
    }
    Report report;
    if (!plain.empty()) {
        auto checked = check_names(r, plain, held, stop);
        if (!checked) {
            done_with_pool();
            return checked;
        }
        report = std::move(*checked);
    }
    if (!devices.empty()) {
        if (auto summed = compare_sums(r, devices, held, stop, report); !summed) {
            done_with_pool();
            return util::fail(summed.error());
        }
    }
    done_with_pool();
    return report;
}

std::expected<Report, std::string> file(Request base, const std::string& remote_path, const fs::path& target,
                                        std::stop_token stop) {
    // rclone check pairs files by name: a copy under another name is compared by its sum.
    const std::string name = remote_basename(remote_path);
    if (local::relative(name) != target.filename()) base.renamed[name] = target.filename();
    base.remote_dir = parent_of(remote_path);
    base.local_dir = target.parent_path();
    base.paths = {name};
    base.many = false;
    return run(base, std::move(stop));
}

std::string summary(const Report& r, std::size_t files, bool folder) {
    if (r.hash.empty()) return "not verified: the server can't compute checksums";
    const std::string hash = upper(r.hash);
    auto count = [](std::size_t n) { return std::format("{} file{}", n, n == 1 ? "" : "s"); };
    if (!folder && files == 1) {
        if (!r.differ.empty()) return std::format("differs from the server's copy ({})", hash);
        if (r.unchecked > 0) return "not verified: the checksum could not be computed";
        return std::format("checksum verified ({})", hash);
    }
    if (!r.differ.empty()) {
        return files == 1 ? std::format("1 file differs from the server's copy ({})", hash)
                          : std::format("{} of {} files differ from the server's copy ({})", r.differ.size(), files, hash);
    }
    if (r.unchecked > 0) return std::format("{} verified ({}); {} could not be checked", count(r.matched), hash, r.unchecked);
    return std::format("{} verified ({})", count(r.matched), hash);
}

} // namespace grab::verify
