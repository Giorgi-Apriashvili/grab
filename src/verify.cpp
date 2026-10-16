#include "verify.hpp"

#include "process.hpp"
#include "rclone.hpp"
#include "server_ops.hpp"
#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <system_error>
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

std::string remote_parent(std::string_view remote_path) {
    while (remote_path.size() > 1 && remote_path.back() == '/') remote_path.remove_suffix(1);
    const auto slash = remote_path.rfind('/');
    if (slash == std::string_view::npos) return "";
    if (slash == 0) return "/";
    return std::string(remote_path.substr(0, slash));
}

std::vector<std::string> check_argv(const Request& r, const fs::path& list_file, int connections) {
    const auto& ctx = *r.ctx;
    std::vector<std::string> argv{ctx.config.rclone, "check", remote_spec(ctx.settings.rclone_remote, r.remote_dir),
                                  util::path_to_utf8(r.local_dir)};
    if (ctx.config.rclone_config) argv.insert(argv.end(), {"--config", util::path_to_utf8(*ctx.config.rclone_config)});
    const auto flags = engine::without_console_progress(ctx.settings.common_flags);
    argv.insert(argv.end(), flags.begin(), flags.end());
    argv.insert(argv.end(), r.extra.begin(), r.extra.end());
    // Last, so they win over the common flags: grab's default --sftp-disable-hashcheck would
    // leave the server without checksums. -v logs which checksum was compared.
    const auto n = std::to_string(std::max(connections, 1));
    argv.insert(argv.end(), {"--one-way", "--files-from-raw", util::path_to_utf8(list_file), "--combined", "-",
                             "--checkers", n, "--sftp-connections", n, "--sftp-disable-hashcheck=false", "-v"});
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

    const auto list = fs::temp_directory_path() / std::format("grab-verify-{}-{}.txt", process_id(), r.id);
    if (!server_ops::write_text(list, [&] {
            std::string text;
            for (const auto& p : r.paths) text += p + '\n';
            return text;
        }())) {
        done_with_pool();
        return util::fail("cannot write " + util::path_to_utf8(list));
    }
    const auto argv = check_argv(r, list, held);
    if (r.on_command) r.on_command(argv);
    proc::RunOptions quiet;
    quiet.detached = true;
    quiet.stop = stop;
    auto ran = proc::run_capture(argv, quiet);
    std::error_code ec;
    fs::remove(list, ec);
    done_with_pool();
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
    if (lines.size() < r.paths.size()) report.unchecked += r.paths.size() - lines.size();
    const std::size_t no_hash = unhashed(ran->err);
    if (no_hash > 0) { // counted as matches by rclone, without saying which
        report.unchecked += std::min(no_hash, report.matched);
        report.matched -= std::min(no_hash, report.matched);
        report.same.clear();
    }
    if (report.hash.empty()) report.same.clear(); // sizes only
    return report;
}

std::expected<Report, std::string> file(Request base, const std::string& remote_path, const fs::path& target,
                                        std::stop_token stop) {
    // rclone check pairs files by name, so the copy must carry the server's.
    const std::string name = remote_basename(remote_path);
    if (util::path_from_utf8(name) != target.filename()) return util::fail("the copy has another name than the server's file");
    base.remote_dir = remote_parent(remote_path);
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
