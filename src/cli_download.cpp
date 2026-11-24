#include "cli_download.hpp"

#include "cli_console.hpp"
#include "conn_budget.hpp"
#include "existing.hpp"
#include "fetch.hpp"
#include "folder_job.hpp"
#include "gui_state.hpp"
#include "local_name.hpp"
#include "process.hpp"
#include "progress_line.hpp"
#include "quote.hpp"
#include "rate.hpp"
#include "remote.hpp"
#include "schedule.hpp"
#include "space.hpp"
#include "ssh_pool.hpp"
#include "util.hpp"
#include "verify.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <format>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <print>
#include <stop_token>
#include <system_error>
#include <utility>

namespace grab {

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

struct Session {
    const engine::Context& ctx;
    const Options& opts;
    const std::filesystem::path& dest;
    LiveLine& line;
    fetch::Connections& connections;
    rate::RateLimiter& limiter;
    engine::CommandHook trace; // -v
    std::stop_token stop;
    bool verify = false; // check downloads against the server's checksum
    int files_at_once = 4;                // a folder's files at once: the window's "At once"
    // "A different file is already there" (review G105): the answers given in the question before
    // fetching, for the files listed in it (remote path -> answer); one for all the rest
    // (--existing, or a capital letter); anything else is asked when it comes up.
    const std::map<std::string, existing::Choice>* answered = nullptr;
    std::optional<existing::Choice>* for_rest = nullptr;
    bool several = false; // more than one pick: the question offers "the same for the rest"
    // Files and folders through grab's own SSH client, which verifies them too.
    std::shared_ptr<ssh::Pool> pool = nullptr;
};

// What a verification of download `id` needs from the session.
verify::Request verify_request(const Session& s, int id) {
    verify::Request r;
    r.ctx = &s.ctx;
    r.pool = s.pool;
    r.connections = &s.connections;
    r.server = s.ctx.settings.name;
    r.id = id;
    r.on_command = s.trace;
    return r;
}

// "name  (yours: 1.2 GiB, 2024-01-02; the server's: 1.4 GiB, 2025-03-04)"
std::string existing_line(const ExistingFile& f) {
    auto when = [](std::optional<std::int64_t> t) { return t ? ", " + util::local_date(*t) : std::string(); };
    const auto mine = existing::local_file(f.local).value_or(existing::Local{});
    if (!f.uncompared.empty()) {
        std::error_code ec;
        if (std::filesystem::is_directory(f.local, ec)) {
            return std::format("{}/  (your folder; couldn't compare with the server's: {})", f.name, f.uncompared);
        }
        return std::format("{}  (yours: {}{}; couldn't compare with the server's: {})", f.name,
                           util::format_size(mine.size), when(mine.modified), f.uncompared);
    }
    return std::format("{}  (yours: {}{}; the server's: {}{})", f.name, util::format_size(mine.size), when(mine.modified),
                       util::format_size(f.size), when(f.modified));
}

// `target`: where it lands (DEST\<name>, or a numbered name when another pick has the same).
int fetch_file(const Session& s, const std::string& path, int id, std::filesystem::path target) {
    const std::string& rclone_remote = s.ctx.settings.rclone_remote;
    std::uint64_t before = 0; // fetched by an earlier run
    if (auto p = fetch::saved_progress(target, path, {}, rclone_remote)) before = p->first;

    fetch::Download d;
    d.source = fetch::Source{.rclone_remote = s.ctx.settings.rclone_remote, .path = path};
    d.server = s.ctx.settings.name;
    d.id = id;
    d.limiter = &s.limiter;
    d.on_command = s.trace;
    d.pool = s.pool;
    const auto start = Clock::now();
    auto choice = [&] {
        if (const auto it = s.answered->find(path); it != s.answered->end()) return it->second;
        return s.for_rest->value_or(existing::Choice::ask);
    }();
    auto fetch_once = [&] {
        d.target = target;
        d.replace = choice == existing::Choice::replace;
        return fetch::run(d, s.connections, [&](const fetch::Progress& p) {
            s.line.show(progress::line({p.bytes, p.total, p.speed, p.eta, p.connections, {}, {}}));
        }, s.stop);
    };
    auto result = fetch_once();
    // A different file already there that the check before fetching didn't see, or one asked
    // about: the answer decides.
    while (result.outcome == fetch::Outcome::exists) {
        if (choice == existing::Choice::ask) {
            s.line.clear();
            const ExistingFile f{util::path_to_utf8(target.filename()), target, result.size,
                                 util::parse_rfc3339_seconds(result.modtime)};
            const auto answer = choose_existing(s.opts, {f}, 1, s.several);
            if (!answer) return answer.error();
            choice = answer->choice;
            if (answer->for_all) *s.for_rest = choice;
        }
        if (choice == existing::Choice::skip) {
            fetch::discard(target, path, {}, rclone_remote); // what was fetched for it won't be used (G74)
            s.line.finish("skipped: your file is kept");
            return 0;
        }
        if (choice == existing::Choice::keep_both) {
            // A copy an earlier run kept that already is the server's file (review G112).
            if (const auto kept = existing::kept_copy(target, result.size, util::parse_rfc3339(result.modtime))) {
                s.line.finish(std::format("already there as {}, nothing fetched", util::path_to_utf8(kept->filename())));
                return 0;
            }
            const auto second = existing::second_copy(target);
            if (!second) {
                s.line.clear();
                std::println(stderr, "grab: error: no free name for a second copy next to {}", util::path_to_utf8(target));
                return exit_download_failed;
            }
            // What was fetched goes on under the new name, done already if it was complete (G74).
            fetch::move_partial(target, *second, path, rclone_remote);
            target = *second;
            s.line.message("grab: saving it as " + util::path_to_utf8(target.filename()));
            before = 0;
            if (auto p = fetch::saved_progress(target, path, {}, rclone_remote)) before = p->first;
        }
        result = fetch_once();
    }
    const double secs = seconds_since(start);
    if (!result.note.empty()) {
        s.line.message("grab: note: " + result.note);
        before = 0; // it started over
    }

    switch (result.outcome) {
    case fetch::Outcome::done: {
        if (result.present) {
            s.line.finish("already there, nothing fetched");
            return 0;
        }
        std::error_code ec;
        const auto size = std::filesystem::file_size(target, ec);
        s.line.finish(progress::done(size, size - std::min(before, size), secs));
        if (!s.verify) return 0;
        const auto check_start = Clock::now();
        verify::Report checked;
        {
            const VerifyLine shown(s.line, "verifying against the server's checksum...");
            // Against the server's file as this download fetched it: its size and time.
            checked = verify::file(verify_request(s, id),
                                   verify::File{.path = path, .local = target, .size = result.size, .modtime = result.modtime},
                                   s.stop);
        }
        if (s.stop.stop_requested()) {
            s.line.finish("stopped while verifying: the file is complete, but not verified");
            return proc::exit_stopped;
        }
        switch (checked.files.front().verdict) {
        case verify::Verdict::differs:
        case verify::Verdict::missing:
            if (checked.files.front().verdict == verify::Verdict::differs) {
                std::filesystem::remove(target, ec); // not the server's file
                s.line.clear();
                std::println(stderr, "grab: error: the download {}; it was deleted", verify::summary(checked));
            } else {
                s.line.clear();
                std::println(stderr, "grab: error: {}", verify::summary(checked));
            }
            std::println(stderr, "grab: run the same command again to download it again");
            return exit_download_failed;
        case verify::Verdict::unchecked: s.line.finish(verify::summary(checked)); return 0;
        case verify::Verdict::same: break;
        }
        s.line.finish(std::format("{} in {}", verify::summary(checked), progress::duration(seconds_since(check_start))));
        return 0;
    }
    case fetch::Outcome::stopped: {
        const auto p = fetch::saved_progress(target, path, {}, rclone_remote);
        s.line.finish(p && p->second > 0
                          ? std::format("stopped at {}%; run the same command again to continue", p->first * 100 / p->second)
                          : "stopped");
        return proc::exit_stopped;
    }
    case fetch::Outcome::failed:
    case fetch::Outcome::exists:
        break;
    }
    s.line.clear();
    std::println(stderr, "grab: error: {}", result.error);
    if (fetch::saved_progress(target, path, {}, rclone_remote)) {
        std::println(stderr, "grab: what was fetched is kept; run the same command again to continue");
    }
    return exit_download_failed;
}

// `local_name`: the folder's name in DEST when another pick has the server's (else empty).
int fetch_folder(const Session& s, const std::string& path, int id, const std::filesystem::path& local_name) {
    folder::Tree tree;
    std::mutex m;
    folder::Job job;
    job.ctx = &s.ctx;
    job.connections = &s.connections;
    job.limiter = &s.limiter;
    job.server = s.ctx.settings.name;
    job.id = id;
    job.remote_path = path;
    job.dest = s.dest;
    job.local_name = local_name;
    job.files_at_once = [&s] { return s.files_at_once; }; // the window's "At once"
    job.on_command = s.trace;
    job.pool = s.pool;
    // Its speed and time left count the bytes as they come (FolderLine).
    std::atomic<std::uint64_t> received{0};
    job.on_received = [&received](std::uint64_t n) { received += n; };

    // The folder's files and bytes between the listing and the fetching (the local checks can take
    // seconds on a big folder); the speed and time left come with FolderLine.
    const auto show_totals = [&] {
        folder::Totals t;
        int conns = 0;
        {
            std::lock_guard lock(m);
            t = folder::totals(tree);
            conns = tree.connections;
        }
        s.line.show(progress::line({.bytes = t.bytes,
                                    .total = t.total,
                                    .speed = 0,
                                    .eta = std::nullopt,
                                    .connections = conns,
                                    .files_done = t.files_done,
                                    .files = t.files}),
                    true);
    };

    const auto start = Clock::now();
    s.line.show("listing the folder...", true);
    if (auto listed = folder::list(job, tree, m, s.stop); !listed) {
        s.line.clear();
        std::println(stderr, "grab: error: {}", listed.error());
        return exit_download_failed;
    }
    if (s.stop.stop_requested()) {
        s.line.finish("stopped");
        return proc::exit_stopped;
    }
    {
        // Folders inside it the server wouldn't list: the rest is fetched (review G55).
        std::lock_guard lock(m);
        for (const auto& u : tree.unreadable) s.line.message("grab: couldn't read the folder " + u);
        // Files that have another file's name on Windows ("README" and "readme"), numbered (G25).
        for (const auto& f : tree.files) {
            if (f.local.empty()) continue;
            s.line.message(std::format("grab: {} is saved as \"{}\": another file of the folder has its name on Windows",
                                       f.path, util::path_to_utf8(f.local.filename())));
        }
    }
    show_totals(); // after the notes: message() erases the line
    folder::mark_existing(job, tree, m);
    show_totals(); // the files already here, while the conflicts and the free space are checked
    // Different files already where some of the folder's go: the answer first, for this folder's
    // files as listed here, not one given for another pick (review G105).
    job.existing = s.for_rest->value_or(existing::Choice::ask);
    if (const auto found = folder::conflicts(job, tree, m); !found.empty()) {
        if (job.existing == existing::Choice::ask) {
            s.line.clear();
            const std::string folder_name = util::path_to_utf8(folder::job_dir(job).filename());
            std::vector<ExistingFile> files;
            for (const auto& c : found) {
                if (files.size() == 10) break;
                files.push_back({folder_name + "/" + c.path, c.local, c.size, util::parse_rfc3339_seconds(c.modtime)});
            }
            const auto answer = choose_existing(s.opts, files, found.size(), s.several);
            if (!answer) return answer.error();
            job.existing = answer->choice;
            if (answer->for_all) *s.for_rest = answer->choice;
        }
        std::lock_guard lock(m);
        folder::apply(job.existing, found, folder::job_dir(job), tree);
    }
    std::uint64_t before = 0;    // on disk already: finished files, partial big ones
    std::size_t already = 0;     // files that were complete
    std::uint64_t needed = 0;    // what is left to fetch
    {
        std::lock_guard lock(m);
        const auto t = folder::totals(tree);
        before = t.bytes;
        already = t.files_done;
        needed = t.total > t.bytes ? t.total - t.bytes : 0;
    }
    // Free space, now that the folder's size is known: before fetching anything.
    if (const auto free = space::free_bytes(s.dest); free && needed > *free) {
        s.line.clear();
        std::println(stderr, "grab: error: {}", space::shortage(space::drive_of(s.dest), needed, *free));
        return exit_download_failed;
    }
    {
        const FolderLine shown(s.line, tree, m, s.limiter, received);
        folder::run(job, tree, m, s.stop);
    }
    // --verify: what this run fetched, against the server's checksums. Copies that differ are
    // deleted and count as failed below.
    std::string checked_text;
    if (s.verify && !s.stop.stop_requested()) {
        const auto check_start = Clock::now();
        folder::Checked checked;
        {
            VerifyLine shown(s.line, "verifying against the server's checksums...");
            auto base = verify_request(s, id);
            base.on_progress = [&shown](std::size_t done, std::size_t total) { shown.files(done, total); };
            checked = folder::verify_files(job, tree, m, std::move(base), s.stop);
        }
        if (checked.files > 0 && !s.stop.stop_requested()) {
            checked_text = std::format("{} in {}", checked.summary(), progress::duration(seconds_since(check_start)));
        }
    }

    folder::Totals t;
    folder::Outcome o;
    {
        std::lock_guard lock(m);
        t = folder::totals(tree);
        o = folder::outcome(tree);
    }
    if (s.stop.stop_requested()) {
        s.line.finish(std::format("stopped with {} of {} files done; run the same command again to continue",
                                  t.files_done, t.files));
        return proc::exit_stopped;
    }
    if (o.failed > 0) {
        // What verification found of the files that came, then the failure.
        s.line.clear();
        std::print(stderr, "{}", progress::folder_failed(folder::failed_text(o), checked_text));
        std::fflush(stderr);
        return exit_download_failed;
    }
    if (already == t.files && t.files > 0) {
        s.line.finish(std::format("already there, nothing fetched ({} file{}, {})", t.files, t.files == 1 ? "" : "s",
                                  util::format_size(t.total)));
        return 0;
    }
    std::string text = progress::done(t.total, t.bytes - std::min(before, t.bytes), seconds_since(start), t.files);
    if (already > 0) text += std::format("; {} {} already there", already, already == 1 ? "was" : "were");
    s.line.finish(text);
    if (!checked_text.empty()) s.line.finish(checked_text);
    return 0;
}

// --dry-run: what would be fetched, without writing anything. `target`: where it would land.
void describe(const Session& s, const std::string& path, std::optional<std::uint64_t> size, int id,
              const std::string& position, int budget, const std::filesystem::path& target) {
    const auto local = util::path_to_utf8(target);
    std::string what;
    if (s.opts.mode == Mode::file) {
        if (size) {
            what = std::format(" ({}{})", util::format_size(*size),
                               existing::differs(target, *size, std::nullopt) ? "; a different file is there: asked first" : "");
        }
    } else {
        folder::Tree tree;
        std::mutex m;
        folder::Job job;
        job.ctx = &s.ctx;
        job.connections = &s.connections; // its walk takes them from the pool, as a download's
        job.server = s.ctx.settings.name;
        job.remote_path = path;
        job.dest = s.dest;
        if (target != folder::local_dir(s.dest, path)) job.local_name = target.filename();
        job.on_command = s.trace;
        job.id = id;
        job.pool = s.pool;
        if (auto listed = folder::list(job, tree, m, s.stop); !listed) {
            what = std::format(" (cannot list it: {})", listed.error());
        } else {
            folder::mark_existing(job, tree, m);
            const auto t = folder::totals(tree);
            what = std::format(" ({} file{}, {}", t.files, t.files == 1 ? "" : "s", util::format_size(t.total));
            if (t.files_done > 0) what += std::format("; {} already there", t.files_done);
            if (const auto n = std::ranges::count_if(tree.files, [](const folder::File& f) { return !f.local.empty(); }); n > 0) {
                what += std::format("; {} numbered: another file has its name on Windows", n);
            }
            if (const auto n = folder::conflicts(job, tree, m).size(); n > 0) {
                what += std::format("; {} differ{} from yours: asked first", n, n == 1 ? "s" : "");
            }
            what += ")";
        }
    }
    std::println(stderr, "grab: would fetch {}{} {}{}  ->  {}  (up to {} connections)", mode_noun(s.opts.mode), position,
                 remote_spec(s.ctx.settings.rclone_remote, path), what, local, budget);
}

} // namespace

std::expected<Answer, int> choose_existing(const Options& opts, const std::vector<ExistingFile>& files,
                                         std::size_t count, bool offer_all) {
    if (opts.existing) return Answer{.choice = *opts.existing, .for_all = true};
    const bool unsure = std::ranges::any_of(files, [](const ExistingFile& f) { return !f.uncompared.empty(); });
    std::println(stderr, "grab: {} already there {} from the server's:",
                 count == 1 ? std::string("1 file") : std::format("{} files", count),
                 unsure ? "may differ" : count == 1 ? "differs" : "differ");
    for (const auto& f : files) std::println(stderr, "  {}", existing_line(f));
    if (count > files.size()) std::println(stderr, "  ... and {} more", count - files.size());
    if (!util::stdin_is_tty()) {
        std::println(stderr, "grab: error: nothing was replaced; to say what to do without being asked, add --existing "
                             "replace, both or skip");
        return std::unexpected(exit_download_failed);
    }
    for (;;) {
        std::print(stderr, "[r]eplace, [k]eep both, [s]kip, [q]uit{}",
                   offer_all ? " (R, K, S: the same for the rest): " : ": ");
        std::string line;
        if (!std::getline(std::cin, line)) return std::unexpected(exit_aborted);
        const std::string typed(util::trim(line));
        // A capital letter answers every later question of this run too.
        if (offer_all && typed == "R") return Answer{.choice = existing::Choice::replace, .for_all = true};
        if (offer_all && typed == "K") return Answer{.choice = existing::Choice::keep_both, .for_all = true};
        if (offer_all && typed == "S") return Answer{.choice = existing::Choice::skip, .for_all = true};
        const auto answer = util::to_lower(typed);
        if (answer == "r" || answer == "replace") return Answer{.choice = existing::Choice::replace};
        if (answer == "k" || answer == "keep both" || answer == "both") {
            return Answer{.choice = existing::Choice::keep_both};
        }
        if (answer == "s" || answer == "skip") return Answer{.choice = existing::Choice::skip};
        if (answer == "q" || answer == "quit") return std::unexpected(exit_aborted);
    }
}

DownloadTally download_with_grab(const engine::Context& ctx, const Options& opts, const std::vector<std::string>& paths,
                                 const std::unordered_map<std::string, std::uint64_t>& sizes,
                                 const std::unordered_map<std::string, std::int64_t>& modified,
                                 const std::filesystem::path& dest, ssh::Pools& pools) {
    LiveLine line;
    const std::string& server = ctx.settings.name;
    const auto config = opts.config.value_or(default_grab_config_path());
    const auto gui = window_settings(state_dir(config) / "gui.json");
    // The connection limits learned from refusals, by the window or an earlier command; what this
    // run learns is saved there too (review G195).
    const auto limits_file = learned_limits_path(config);
    const auto limits = load_learned_limits(limits_file, gui.server_limits);
    fetch::Connections connections([&](const std::string& name, int budget) {
        line.message(std::format("grab: {} refused a connection; using {} from now on", name, budget));
        if (auto saved = save_learned_limit(limits_file, name, budget, gui.server_limits); !saved) {
            line.message("grab: " + saved.error());
        }
    });
    std::optional<int> learned;
    if (const auto it = limits.find(server); it != limits.end()) learned = it->second;
    const int default_budget = budget::default_budget(ctx.remote.host);
    const int budget = budget::effective_budget(ctx.settings.max_connections, default_budget, learned);
    connections.configure(server, budget);
    rate::RateLimiter limiter;
    if (opts.limit) limiter.set_rate(*opts.limit);
    // Without --limit, the speed schedule set in grab's window decides, also mid-download: its
    // download speeds (↓).
    const auto rules = opts.limit ? std::vector<schedule::Rule>{} : gui.speed_schedule;
    if (auto note = schedule_note(rules, schedule::local_now(), schedule::Dir::down)) {
        std::println(stderr, "grab: {}", *note);
    }
    std::optional<ScheduleFollower> follower;
    if (!opts.dry_run && schedule::limits(rules, schedule::Dir::down)) {
        follower.emplace(rules, schedule::Dir::down, limiter, line);
    }
    const bool verify = opts.verify.value_or(gui.verify_downloads);
    if (verify) {
        std::println(stderr, "grab: downloads are checked against the server's checksum afterwards{}",
                     opts.verify ? "" : " (the window's setting; --no-verify skips it)");
    }

    StopOnCtrlC ctrl_c;
    Session s{ctx, opts, dest, line, connections, limiter, {}, ctrl_c.token()};
    s.verify = verify;
    s.files_at_once = std::clamp(gui.parallel, min_parallel, max_parallel);
    std::map<std::string, existing::Choice> answered;
    std::optional<existing::Choice> for_rest = opts.existing;
    s.answered = &answered;
    s.for_rest = &for_rest;
    s.several = paths.size() > 1;
    if (opts.verbose) {
        s.trace = [&line](const std::vector<std::string>& argv) { line.message("+ " + quote::display_cmdline(argv)); };
        std::println(stderr, "grab: up to {} connections to {}", budget, server);
    }

    DownloadTally tally;
    // The command's --bind, --contimeout and --timeout apply to grab's connections.
    auto target = engine::ssh_target(ctx, opts.connect);
    if (!target) {
        std::println(stderr, "grab: {}", target.error());
        tally.code = exit_download_failed;
        tally.failed = paths;
        return tally;
    }
    s.pool = pools.get(server, *target, budget);
    if (opts.verbose) std::println(stderr, "grab: {} through grab's own SSH client", server);
    const std::size_t total = paths.size();
    const auto targets = existing::pick_targets(dest, paths, opts.mode == Mode::folder);
    // A picked file's size and time (nanoseconds), when the pick list had them.
    auto known = [&](const std::string& path) -> std::optional<std::pair<std::uint64_t, std::optional<std::int64_t>>> {
        const auto size = sizes.find(path);
        if (size == sizes.end()) return std::nullopt;
        const auto when = modified.find(path);
        return std::pair{size->second, when == modified.end() ? std::nullopt : std::optional(when->second * 1'000'000'000)};
    };
    // Different files already where picked files go: one question for all of them, before
    // fetching any (a folder asks once it is listed). The answer goes for the files listed.
    if (!opts.dry_run && opts.mode == Mode::file && !for_rest) {
        std::vector<ExistingFile> found;
        std::vector<std::string> listed;
        for (std::size_t i = 0; i < total; ++i) {
            const auto k = known(paths[i]);
            if (!k || !existing::differs(targets[i], k->first, k->second)) continue;
            listed.push_back(paths[i]);
            if (listed.size() <= 10) {
                const auto seconds = k->second ? std::optional(*k->second / 1'000'000'000) : std::nullopt;
                found.push_back({.name = util::path_to_utf8(targets[i].filename()), .local = targets[i], .size = k->first,
                                 .modified = seconds});
            }
        }
        if (!listed.empty()) {
            const auto answer = choose_existing(opts, found, listed.size(), total > listed.size());
            if (!answer) {
                tally.code = answer.error();
                tally.failed = paths;
                return tally;
            }
            for (const auto& path : listed) answered[path] = answer->choice;
            if (answer->for_all) for_rest = answer->choice;
        }
    }
    // Free space for the files picked, all together, before fetching any (a folder checks once
    // it is listed). Files to be skipped don't count (review G113).
    if (!opts.dry_run && opts.mode == Mode::file) {
        std::uint64_t needed = 0;
        for (std::size_t i = 0; i < total; ++i) {
            const auto k = known(paths[i]);
            if (!k) continue;
            const auto it = answered.find(paths[i]);
            const auto choice = it != answered.end() ? it->second : for_rest.value_or(existing::Choice::ask);
            if (choice == existing::Choice::skip && existing::differs(targets[i], k->first, k->second)) continue;
            needed += space::still_needed(targets[i], paths[i], k->first);
        }
        if (const auto free = space::free_bytes(dest); free && needed > *free) {
            std::println(stderr, "grab: error: {}", space::shortage(space::drive_of(dest), needed, *free));
            tally.code = exit_download_failed;
            tally.failed = paths;
            return tally;
        }
    }
    for (std::size_t i = 0; i < total; ++i) {
        const std::string& path = paths[i];
        const int id = static_cast<int>(i) + 1;
        const std::string position = total > 1 ? std::format(" {}/{}", i + 1, total) : "";
        // Another pick has its name: a numbered one instead (pick_targets).
        const bool renamed = targets[i] != (opts.mode == Mode::folder ? folder::local_dir(dest, path)
                                                                      : dest / local::relative(remote_basename(path)));
        if (opts.dry_run) {
            const auto it = sizes.find(path);
            describe(s, path, it == sizes.end() ? std::nullopt : std::optional(it->second), id, position, budget,
                     targets[i]);
            continue;
        }
        std::println(stderr, "grab: {}{} {}  ->  {}{}", mode_noun(opts.mode), position,
                     remote_spec(ctx.settings.rclone_remote, path), util::path_to_utf8(targets[i]),
                     renamed ? "  (another pick has that name)" : "");
        const int code = opts.mode == Mode::file
                             ? fetch_file(s, path, id, targets[i])
                             : fetch_folder(s, path, id, renamed ? targets[i].filename() : std::filesystem::path());
        if (code == 0) {
            ++tally.done;
            continue;
        }
        if (tally.code == 0) tally.code = code;
        tally.failed.push_back(path);
        // Ctrl+C, or [q]uit at a question: the picks after it don't start (review G106).
        if (code == proc::exit_stopped || code == exit_aborted) {
            tally.stopped = code == proc::exit_stopped;
            tally.quit = code == exit_aborted;
            tally.not_started = total - i - 1;
            break;
        }
    }
    // Downloads that ended without a refusal: a learned limit goes up by one, so a drop that
    // wasn't the server's real limit wears off (review G10).
    if (learned && tally.done > 0 && connections.refusals(server) == 0) {
        if (auto saved = save_learned_limit(limits_file, server, budget::raised(*learned, default_budget), gui.server_limits);
            !saved) {
            std::println(stderr, "grab: {}", saved.error());
        }
    }
    return tally;
}

} // namespace grab
