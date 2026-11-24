#include "cli_upload.hpp"

#include "config.hpp"
#include "conn_budget.hpp"
#include "folder.hpp"
#include "folder_job.hpp"
#include "gui_state.hpp"
#include "local_name.hpp"
#include "local_walk.hpp"
#include "process.hpp"
#include "progress_line.hpp"
#include "quote.hpp"
#include "rclone_encoding.hpp"
#include "schedule.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_pool.hpp"
#include "upload.hpp"
#include "upload_check.hpp"
#include "upload_folder.hpp"
#include "util.hpp"
#include "verify.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <format>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <print>
#include <set>
#include <system_error>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)
#else
#include <cwctype>
#endif

namespace grab {

namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;

constexpr int exit_config = 2; // as main.cpp's: the server's login can't be made from its settings
// The files already on the server a question or -n lists, at most (as the download's question).
constexpr std::size_t listed_at_most = 10;
constexpr std::size_t all_of_them = std::numeric_limits<std::size_t>::max();

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

// "Main:/srv/photos"; "Main:~" for the login folder.
std::string shown(std::string_view server, std::string_view path) {
    return std::format("{}:{}", server, path.empty() ? std::string_view("~") : path);
}

// "2026-11-24 14:03", this PC's time.
std::string local_minute(std::int64_t unix_seconds) {
    const auto t = static_cast<std::time_t>(unix_seconds);
    std::tm tm{};
#ifdef _WIN32
    if (localtime_s(&tm, &t) != 0) return {};
#else
    if (localtime_r(&t, &tm) == nullptr) return {};
#endif
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

// "2023-05-06T11:18:12.123456789Z": a local file's time, as verify compares it.
std::string rfc3339_ns(std::int64_t ns) {
    return std::format("{:%FT%TZ}", std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{ns}});
}

// ---- the files already there ---------------------------------------------------------------------

// How the window's setting names a choice.
std::string_view setting_words(upload::Existing e) {
    switch (e) {
    case upload::Existing::ask: return "Ask";
    case upload::Existing::replace: return "Replace";
    case upload::Existing::different: return "Replace if different";
    case upload::Existing::newer: return "Replace if newer";
    case upload::Existing::skip: return "Skip";
    case upload::Existing::keep_both: return "Keep both";
    }
    return "Ask";
}

// "a.txt  (yours: 3 B, 2026-11-20 14:03; the server's: 5 B, 2026-11-19 09:00; yours is older)"
std::string clash_line(std::string_view name, const upload::Clash& c) {
    const std::string mine = std::format("yours: {}, {}", util::format_size(c.local_size), local_minute(c.local_mtime));
    if (c.remote_is_dir) return std::format("{}  ({}; the server has a folder of that name, never replaced)", name, mine);
    const std::string theirs = std::format("the server's: {}{}", util::format_size(c.remote_size),
                                           c.remote_mtime ? ", " + local_minute(*c.remote_mtime) : std::string());
    std::string_view how = "another size";
    switch (upload::kind(c)) {
    case upload::Kind::same: how = "the same"; break;
    case upload::Kind::newer_here: how = "yours is newer"; break;
    case upload::Kind::older_here: how = "yours is older"; break;
    case upload::Kind::different_size: break;
    }
    return std::format("{}  ({}; {}; {})", name, mine, theirs, how);
}

// "37 files already on Main:/srv: 30 the same, 5 newer here, 2 older here"
std::string taken_text(const upload::Taken& t, std::string_view where) {
    std::vector<std::string> parts;
    if (t.same > 0) parts.push_back(std::format("{} the same", progress::count(t.same)));
    if (t.newer > 0) parts.push_back(std::format("{} newer here", progress::count(t.newer)));
    if (t.older > 0) parts.push_back(std::format("{} older here", progress::count(t.older)));
    if (t.differs > 0) parts.push_back(std::format("{} of another size", progress::count(t.differs)));
    if (t.folders > 0) parts.push_back(std::format("{} a folder there", progress::count(t.folders)));
    return std::format("{} already on {}: {}", t.count == 1 ? std::string("1 file") : progress::count(t.count) + " files", where,
                       util::join(parts, ", "));
}

// What the answer does with the files listed: "5 replaced, 32 skipped".
std::string decided_text(const std::vector<upload::Listed>& listed, const upload::Answer& a) {
    std::size_t replaced = 0;
    std::size_t skipped = 0;
    std::size_t numbered = 0;
    std::size_t asked = 0;
    for (std::size_t k = 0; k < listed.size(); ++k) {
        const auto own = a.own.find(k);
        switch (upload::decide(own != a.own.end() ? own->second : a.all, listed[k].clash).action) {
        case upload::Action::replace: ++replaced; break;
        case upload::Action::skip: ++skipped; break;
        case upload::Action::numbered: ++numbered; break;
        case upload::Action::ask: ++asked; break;
        }
    }
    std::vector<std::string> parts;
    parts.push_back(replaced == 0 ? std::string("nothing replaced") : std::format("{} replaced", progress::count(replaced)));
    if (skipped > 0) parts.push_back(std::format("{} skipped", progress::count(skipped)));
    if (numbered > 0) parts.push_back(std::format("{} sent as a second copy (\"name (2).ext\")", progress::count(numbered)));
    if (asked > 0) parts.push_back(std::format("{} asked about first", progress::count(asked)));
    return util::join(parts, ", ");
}

// Whose choice it is: "--existing newer", or "your setting in the window (Replace if newer)".
std::string whose(const Options& opts, upload::Existing choice) {
    if (opts.upload_existing) return std::format("--existing {}", upload::name(choice));
    return std::format("your setting in the window ({})", setting_words(choice));
}

// A file there that is the same as yours (size and time): one an earlier run sent, as a rule.
bool same_there(const upload::Clash& c) { return !c.remote_is_dir && upload::kind(c) == upload::Kind::same; }

// The answer under a choice other than Ask: that choice for every file there, except that under
// Keep both a file that is the same is skipped, as under Ask: a second copy the same as the first
// is never wanted, and so the same command again after Ctrl+C goes on (review, chunk 7).
upload::Answer chosen(upload::Existing choice, const std::vector<upload::Listed>& listed) {
    upload::Answer answer{.all = choice, .own = {}, .unlisted = std::nullopt};
    if (choice != upload::Existing::keep_both) return answer;
    for (std::size_t k = 0; k < listed.size(); ++k) {
        if (same_there(listed[k].clash)) answer.own[k] = upload::Existing::skip;
    }
    return answer;
}

constexpr std::string_view existing_hint =
    "to say what to do without being asked, add --existing replace, different, newer, skip or both";

// A file already there, as the question lists it.
struct Asked {
    std::string name;
    upload::Clash clash;
};
// The answer: for all of them, and single files' own (by their place in the question's list).
struct Given {
    upload::Existing all = upload::Existing::ask;
    std::map<std::size_t, upload::Existing> own;
};

// The question about files already on the server (`files`, in `where`): the first ten listed,
// then one answer for all of them or, with several, one by one (a capital letter answers the rest
// too). The exit code instead: 3 when the user quits (or the input ends), 6 when there is no
// console to ask on, said with what to add.
std::expected<Given, int> ask_about(UploadSetup& s, const std::vector<Asked>& files, std::string_view where) {
    LiveLine& line = *s.line;
    upload::Taken taken;
    for (const auto& f : files) taken.add(f.clash);
    line.message("grab: " + taken_text(taken, where));
    for (std::size_t k = 0; k < files.size() && k < listed_at_most; ++k) line.message("  " + clash_line(files[k].name, files[k].clash));
    if (files.size() > listed_at_most) line.message(std::format("  ... and {} more", progress::count(files.size() - listed_at_most)));
    if (!s.console || !s.ask) {
        line.message(std::format("grab: error: nothing there was replaced; {}", existing_hint));
        return std::unexpected(exit_upload_failed);
    }
    const bool several = files.size() > 1;
    for (;;) {
        const auto typed = s.ask(several ? "[r]eplace, replace if [d]ifferent, replace if [n]ewer, [s]kip, [k]eep both, "
                                           "[o]ne by one, [q]uit: "
                                         : "[r]eplace, replace if [d]ifferent, replace if [n]ewer, [s]kip, [k]eep both, "
                                           "[q]uit: ");
        if (!typed) return std::unexpected(exit_upload_not_found);
        const auto t = read_typed(*typed, several);
        if (!t) continue;
        if (t->kind == Typed::Kind::quit) return std::unexpected(exit_upload_not_found);
        if (t->kind == Typed::Kind::choice) return Given{.all = t->choice, .own = {}};
        break; // one by one
    }
    Given given;
    std::optional<upload::Existing> rest;
    for (std::size_t k = 0; k < files.size(); ++k) {
        if (rest) {
            given.own[k] = *rest;
            continue;
        }
        line.message("  " + clash_line(files[k].name, files[k].clash));
        for (;;) {
            const auto typed = s.ask("  [r]eplace, if [d]ifferent, if [n]ewer, [s]kip, [k]eep both, [q]uit "
                                     "(R, D, N, S, K: the rest too): ");
            if (!typed) return std::unexpected(exit_upload_not_found);
            const auto t = read_typed(*typed, false);
            if (!t) continue;
            if (t->kind == Typed::Kind::quit) return std::unexpected(exit_upload_not_found);
            given.own[k] = t->choice;
            if (t->rest) rest = t->choice;
            break;
        }
    }
    return given;
}

// The answer about the files already there before anything is sent (UPLOADS.md decision 3): the
// choice for all of them is --existing, else the window's setting; under Ask the console's
// answer, where a file that is the same (size and time) is taken as one an earlier run sent (a
// run stopped and started again) and skipped without a question (under Keep both too: chosen).
// Under Ask, an item with nothing
// there asks about a file that turns up there later (upload::Answer::unlisted).
std::expected<upload::Answer, int> existing_answer(const Options& opts, UploadSetup& s, const upload::Check& check,
                                                   std::string_view where) {
    const upload::Existing choice = opts.upload_existing.value_or(s.setting);
    upload::Answer answer{.all = choice, .own = {}, .unlisted = std::nullopt};
    if (check.taken.count == 0 && choice != upload::Existing::ask) return answer;
    const auto listed = upload::taken_list(check, all_of_them);
    if (choice != upload::Existing::ask) {
        answer = chosen(choice, listed);
        s.line->message(std::format("grab: {}; {}: {}", taken_text(check.taken, where), whose(opts, choice),
                                    decided_text(listed, answer)));
        return answer;
    }
    answer.unlisted = upload::Existing::ask;
    std::vector<Asked> files;
    std::vector<std::size_t> at; // files[i] is listed[at[i]]
    std::size_t same = 0;
    for (std::size_t k = 0; k < listed.size(); ++k) {
        const auto& c = listed[k].clash;
        if (same_there(c)) {
            answer.own[k] = upload::Existing::skip;
            ++same;
            continue;
        }
        files.push_back(Asked{.name = listed[k].name, .clash = c});
        at.push_back(k);
    }
    if (same > 0) {
        s.line->message(std::format("grab: {} already on {} and the same (size and time): skipped",
                                    same == 1 ? std::string("1 file") : progress::count(same) + " files", where));
    }
    if (files.empty()) return answer;
    s.line->clear();
    auto given = ask_about(s, files, where);
    if (!given) return std::unexpected(given.error());
    answer.all = given->all;
    for (const auto& [i, c] : given->own) answer.own[at[i]] = c;
    return answer;
}

// ---- the server's side -----------------------------------------------------------------------------

// REMOTE_DIR on the server: a folder there, else why not (`missing`: nothing there, or a file).
struct DirLook {
    bool folder = false;
    bool missing = false;
    std::string why;
};
DirLook look_dir(UploadSetup& s, std::string_view dir) {
    const std::string where = dir.empty() ? std::string("the login folder") : std::string(dir);
    // One seat of the server's budget, as the check's own looks take.
    const fetch::Listing seat(*s.connections, s.server, s.stop, 1);
    if (s.stop.stop_requested()) return {.folder = false, .missing = false, .why = "stopped"};
    if (s.trace) s.trace({"sftp", "stat", dir.empty() ? std::string(".") : std::string(dir)});
    std::optional<ssh::Pool::Lease> lease;
    try {
        lease = s.pool->acquire(s.stop, ssh::interactive_retry);
        if (!lease) return {.folder = false, .missing = false, .why = "stopped"};
        const std::string raw = rclone_enc::raw_path(dir);
        const auto there = sftp::stat((*lease)->sftp(), raw.empty() ? std::string(".") : raw);
        if (there && there->is_dir()) return {.folder = true, .missing = false, .why = {}};
        if (there) return {.folder = false, .missing = true, .why = std::format("{} on {} is a file, not a folder", where, s.server)};
        if (there.error().code == sftp::Code::no_such_file) {
            return {.folder = false, .missing = true, .why = std::format("there is no folder {} on {}", where, s.server)};
        }
        return {.folder = false, .missing = false,
                .why = std::format("cannot look at {} on {}: {}", where, s.server, sftp::describe(there.error()))};
    } catch (const ssh::Error& e) {
        return {.folder = false, .missing = false, .why = std::format("Couldn't reach {}: {}", s.server, e.what())};
    } catch (const sftp::Broken& e) {
        if (lease) lease->broken();
        return {.folder = false, .missing = false, .why = std::format("Couldn't reach {}: {}", s.server, e.what())};
    }
}

// A small file's temp left on the server without a record (upload::Result::temp): of no use to a
// later run, which starts such a file over. Removed now; one that can't be is said.
void drop_temp(UploadSetup& s, const std::string& temp) {
    if (temp.empty()) return;
    if (auto gone = upload::remove_temp(*s.pool, temp); !gone) {
        s.line->message(std::format("grab: note: the temp file {} stays on {}: {}", temp, s.server, gone.error()));
    }
}

verify::Request verify_request(const UploadSetup& s, int id) {
    verify::Request r;
    r.ctx = s.ctx;
    r.pool = s.verify_pool ? s.verify_pool : s.pool;
    r.connections = s.connections;
    r.server = s.server;
    r.id = id;
    r.on_command = s.trace;
    return r;
}

// ---- -n ------------------------------------------------------------------------------------------

// What would go where, the server folder as far as it can be seen without writing, and the files
// already there with what the choice would do with them. Nothing is written.
void describe(const Options& opts, UploadSetup& s, const upload::Check& check) {
    LiveLine& line = *s.line;
    const std::size_t total = check.items.size();
    for (std::size_t k = 0; k < total; ++k) {
        const auto& p = check.items[k];
        const std::string position = total > 1 ? std::format(" {}/{}", k + 1, total) : "";
        const char* noun = p.folder ? "folder" : "file";
        const std::string local = util::path_to_utf8(p.source);
        if (!p.error.empty()) {
            line.message(std::format("grab: would not upload {}{} {}: {}", noun, position, local, p.error));
            continue;
        }
        const std::string what = p.folder ? std::format(" ({} file{}, {}; {})", progress::count(p.files), p.files == 1 ? "" : "s",
                                                        util::format_size(p.bytes),
                                                        p.merging ? "merged into the folder there" : "a new folder")
                                          : std::format(" ({})", util::format_size(p.bytes));
        line.message(std::format("grab: would upload {}{} {}{}  ->  {}  (up to {} connections)", noun, position, local, what,
                                 shown(s.server, p.target), s.budget));
    }
    const std::string where = shown(s.server, check.dir);
    line.message(std::format("grab: {}: a folder there{}; -n writes nothing, so whether this account may write in it shows "
                             "when it uploads",
                             where, check.free ? std::format(", {} free", util::format_size(*check.free)) : std::string()));
    if (upload::short_of_space(check)) {
        line.message(std::format("grab: note: {} to send is more than the {} free there", util::format_size(check.bytes),
                                 util::format_size(check.free.value_or(0))));
    }
    if (check.taken.count == 0) {
        line.message(std::format("grab: nothing of it is on {} yet", where));
        return;
    }
    const auto listed = upload::taken_list(check, all_of_them);
    const upload::Existing choice = opts.upload_existing.value_or(s.setting);
    const upload::Answer answer = chosen(choice, listed);
    line.message("grab: " + taken_text(check.taken, where));
    for (std::size_t k = 0; k < listed.size() && k < listed_at_most; ++k) {
        const auto& c = listed[k].clash;
        std::string then;
        if (choice == upload::Existing::ask || answer.own.contains(k)) {
            then = same_there(c) ? "skipped (the same)" : "asked";
        } else {
            switch (upload::decide(choice, c).action) {
            case upload::Action::replace: then = "replaced"; break;
            case upload::Action::skip: then = "skipped"; break;
            case upload::Action::numbered: then = "sent as a second copy"; break;
            case upload::Action::ask: then = "asked"; break;
            }
        }
        line.message(std::format("  {}: {}", clash_line(listed[k].name, c), then));
    }
    if (listed.size() > listed_at_most) line.message(std::format("  ... and {} more", progress::count(listed.size() - listed_at_most)));
    if (choice != upload::Existing::ask) {
        line.message(std::format("grab: {}: {}", whose(opts, choice), decided_text(listed, answer)));
        return;
    }
    const auto same = static_cast<std::size_t>(std::ranges::count_if(listed, [](const upload::Listed& l) { return same_there(l.clash); }));
    const std::size_t asked = listed.size() - same;
    const std::string which = asked == 1 ? std::string("the one that differs") : std::format("the {} that differ", progress::count(asked));
    if (asked == 0) {
        line.message("grab: the same ones are skipped; nothing to ask");
    } else if (s.console) {
        line.message(std::format("grab: the upload asks first about {} (--existing answers in advance)", which));
    } else {
        line.message(std::format("grab: the upload would ask first about {}, and without a console to ask on it stops: {}", which,
                                 existing_hint));
    }
}

// ---- one item ------------------------------------------------------------------------------------

// What the run keeps from item to item.
struct RunState {
    // The places of the picks (grab's form), which a Keep-both copy mustn't take before their
    // files get there (upload::Upload::reserved): made once for the run.
    std::shared_ptr<const std::unordered_set<std::string>> places;
    // The run's choice for a file already there (--existing, else the setting) is Replace: the
    // same command again would send the files that got there again (they are the same now).
    bool replaces = false;
    std::size_t sent = 0; // files sent by the items before this one
};

// The places of the picks other than `own`.
std::function<bool(std::string_view)> others(const RunState& run, std::string own) {
    return [places = run.places, own = std::move(own)](std::string_view path) {
        return path != own && places->contains(std::string(path));
    };
}

// How a run that stopped or failed part-way goes on (review, chunk 7): the same command again,
// whose look skips the files already there and the same as yours under every choice but
// Replace. Under Replace it would send them again (`sent_here` of this item's, and the earlier
// items'), so --existing different instead, which skips those and replaces the rest.
std::string go_on(const RunState& run, std::size_t sent_here, std::string_view to = "continue") {
    const std::size_t there = run.sent + sent_here;
    if (!run.replaces || there == 0) return std::format("run the same command again to {}", to);
    return std::format("run it again with --existing different to {} (--existing replace would send the {} already there again)",
                       to, there == 1 ? std::string("file") : progress::count(there) + " files");
}

// A server copy verify found different from your file.
struct Differs {
    fs::path local;
    std::string target; // where it goes by its own name (grab's form)
    std::string placed; // where it went: `target`, or a second copy's name (Keep both)
};

// How to send them again (review, chunk 7): the same command with --existing replace would send
// every file of the run again (each is the same as yours by size and time now), so each copy at
// its own name gets the command that sends it alone; a second copy (Keep both) no rerun can
// replace by its name, and --existing replace would replace the file you kept. The first ten.
void say_differs(const UploadSetup& s, const std::vector<Differs>& files) {
    LiveLine& line = *s.line;
    line.message(std::format("grab: your file{} as {}; to send {} again:", files.size() == 1 ? " is" : "s are",
                             files.size() == 1 ? "it was" : "they were", files.size() == 1 ? "it" : "them"));
    for (std::size_t k = 0; k < files.size() && k < listed_at_most; ++k) {
        const Differs& f = files[k];
        if (f.placed == f.target) {
            const std::string into = upload::split_remote(f.target).first;
            line.message(std::format("  grab upload \"{}\" -r {} \"{}\" --existing replace", util::path_to_utf8(f.local), s.server,
                                     into.empty() ? std::string(".") : into));
        } else {
            line.message(std::format("  {} (a second copy, Keep both): no rerun can replace that name, and --existing replace "
                                     "would replace the file you kept; remove it there, then send the file again",
                                     shown(s.server, f.placed)));
        }
    }
    if (files.size() > listed_at_most) line.message(std::format("  ... and {} more", progress::count(files.size() - listed_at_most)));
}

int send_file(UploadSetup& s, const upload::Check& check, std::size_t k, upload::Existing choice, int id, RunState& run) {
    LiveLine& line = *s.line;
    const auto& p = check.items[k];
    upload::Upload u;
    u.local = p.source;
    u.server = s.server;
    u.id = id;
    u.remote_path = p.target;
    u.existing = choice;
    u.limiter = s.limiter;
    u.on_command = s.trace;
    u.pool = s.pool;
    u.records_dir = s.records_dir;
    u.reserved = others(run, p.target);
    // Its speed is what the server acknowledged.
    std::atomic<std::uint64_t> sent{0};
    u.on_sent = [&sent](std::uint64_t n) { sent += n; };
    const auto start = Clock::now();
    upload::Result r;
    for (;;) {
        r = upload::run(u, *s.connections, [&](const fetch::Progress& pr) {
            line.show("↑ " + progress::line({.bytes = pr.bytes,
                                             .total = pr.total,
                                             .speed = pr.speed,
                                             .eta = pr.eta,
                                             .connections = pr.connections,
                                             .files_done = std::nullopt,
                                             .files = std::nullopt}));
        }, s.stop);
        drop_temp(s, r.temp);
        if (r.outcome != upload::Outcome::exists) break;
        // A file turned up at its name since the look, and the choice is to ask: nothing of it
        // was touched.
        line.clear();
        if (!r.clash) {
            line.message(std::format("grab: error: a file of that name is already on {}; {}", s.server, existing_hint));
            return exit_upload_failed;
        }
        const auto given = ask_about(s, {Asked{.name = p.name, .clash = *r.clash}}, shown(s.server, check.dir));
        if (!given) return given.error();
        u.existing = given->all;
    }
    const double secs = seconds_since(start);
    if (!r.note.empty() && r.outcome != upload::Outcome::skipped) line.message("grab: note: " + r.note);

    switch (r.outcome) {
    case upload::Outcome::done: {
        line.finish(progress::uploaded(1, r.size, sent.load(), shown(s.server, r.placed), secs));
        ++run.sent;
        if (!s.verify) return 0;
        const auto check_start = Clock::now();
        verify::Report checked;
        {
            const VerifyLine verifying(line, "verifying against the server's checksum...");
            // The server's copy against the local file it was made from; nothing local is touched.
            checked = upload::verify_file(verify_request(s, id), p.source, r.placed, r.size, rfc3339_ns(r.mtime_ns), s.stop);
        }
        if (s.stop.stop_requested()) {
            line.finish("stopped while verifying: the file is on the server, but not verified");
            return proc::exit_stopped;
        }
        if (checked.files.empty()) return 0;
        switch (checked.files.front().verdict) {
        case verify::Verdict::differs:
        case verify::Verdict::missing:
            line.clear();
            line.message(std::format("grab: error: {}", verify::summary(checked)));
            say_differs(s, {Differs{.local = p.source, .target = p.target, .placed = r.placed}});
            return exit_upload_failed;
        case verify::Verdict::unchecked: line.finish(verify::summary(checked)); return 0;
        case verify::Verdict::same: break;
        }
        line.finish(std::format("{} in {}", verify::summary(checked), progress::duration(seconds_since(check_start))));
        return 0;
    }
    case upload::Outcome::skipped:
        line.finish(r.note.empty() ? std::format("skipped: {} is there already", shown(s.server, p.target))
                                   : std::format("skipped {}: {}", shown(s.server, p.target), r.note));
        return 0;
    case upload::Outcome::stopped: {
        const auto saved = upload::saved_progress(s.records_dir, s.server, p.target);
        line.finish(saved && saved->second > 0 ? std::format("stopped at {}%; {}", saved->first * 100 / saved->second, go_on(run, 0))
                    : run.sent > 0         ? std::format("stopped; {}", go_on(run, 0))
                                           : "stopped");
        return proc::exit_stopped;
    }
    case upload::Outcome::failed:
    case upload::Outcome::exists:
        break;
    }
    line.clear();
    line.message("grab: error: " + r.error);
    if (upload::saved_progress(s.records_dir, s.server, p.target)) {
        line.message(std::format("grab: what was sent is kept on {}; {}", s.server, go_on(run, 0)));
    } else if (run.sent > 0) {
        line.message("grab: " + go_on(run, 0, "retry"));
    }
    return exit_upload_failed;
}

// What goes through the folder upload: a picked folder, or picked files of one local folder
// together (batch_tree).
struct Bundle {
    fs::path source;      // the local folder
    std::string target;   // the server folder its files go into (grab's form)
    std::string prefix;   // before its files' paths as listed: "photos/" for a folder, "" for picked files
    bool merging = false; // the server folder is there: nothing made
};

// The tree's files this run sent.
std::size_t sent_files(const upload::Tree& tree) {
    return static_cast<std::size_t>(
        std::ranges::count_if(tree.files, [](const folder::File& f) { return f.status == folder::State::done && f.fetched; }));
}

// Picked files [from, to) of one local folder as a folder upload's files (review, chunk 7): their
// small files then go many at once on every connection, as a folder's do, not one after another
// with a round trip for each step. Their target is REMOTE_DIR, which is there (merged into: nothing
// made or removed); the look's STATs are the server's side (no walk of REMOTE_DIR, which may be
// big), and the other picks' names count as taken there, so a Keep-both copy takes none of them.
// A numbered name taken unseen is found as the file is put in place (a late clash).
upload::Tree batch_tree(const upload::Check& check, std::size_t from, std::size_t to, const std::vector<upload::Existing>& choices) {
    upload::Tree t;
    for (std::size_t k = 0; k < check.items.size(); ++k) {
        const upload::Picked& p = check.items[k];
        if (!p.error.empty()) continue;
        const std::string name = upload::split_remote(p.target).second;
        if (k < from || k >= to) {
            t.on_server.insert(name);
            continue;
        }
        const std::size_t i = t.files.size();
        t.files.push_back(folder::make_file(name, p.bytes, rfc3339_ns(p.mtime_ns)));
        if (p.clash) {
            t.clashes[i] = *p.clash;
            t.on_server.insert(name);
        }
        if (choices[k] != upload::Existing::ask) t.choices[i] = choices[k];
    }
    t.listed = true;
    t.looked = true;
    t.merging = true;
    return t;
}

// Where the batch of picked files that starts at `k` ends: the plain files after it from the same
// local folder (a pattern's matches), each name once. Just `k` + 1 for a folder, or a file alone.
std::size_t batch_end(const upload::Check& check, std::size_t k) {
    const auto plain = [&](std::size_t i) { return !check.items[i].folder && check.items[i].error.empty(); };
    if (!plain(k)) return k + 1;
    const auto parent = local::key(check.items[k].source.parent_path());
    std::unordered_set<std::string> names{check.items[k].target};
    std::size_t to = k + 1;
    while (to < check.items.size() && plain(to) && local::key(check.items[to].source.parent_path()) == parent &&
           names.insert(check.items[to].target).second) {
        ++to;
    }
    return to;
}

int send_tree(UploadSetup& s, upload::Tree& tree, const Bundle& b, upload::Existing choice, int id, RunState& run) {
    LiveLine& line = *s.line;
    std::mutex m;
    upload::FolderJob job;
    job.connections = s.connections;
    job.files_at_once = [n = s.files_at_once] { return n; }; // the window's "At once"
    job.limiter = s.limiter;
    job.server = s.server;
    job.id = id;
    job.source = b.source;
    job.remote_path = b.target;
    job.existing = choice;
    job.on_command = s.trace;
    job.pool = s.pool;
    job.records_dir = s.records_dir;
    // Its speed and time left count what the server acknowledged (FolderLine).
    std::atomic<std::uint64_t> sent{0};
    job.on_sent = [&sent](std::uint64_t n) { sent += n; };
    {
        // The check's look: each name taken there gets its choice now (its own, else the item's).
        std::lock_guard lock(m);
        upload::apply(job, tree);
    }
    const std::string place = shown(s.server, b.target);
    const auto start = Clock::now();
    bool unanswered = false; // files there that no console could be asked about
    for (;;) {
        {
            const FolderLine sending(line, tree, m, *s.limiter, sent, "↑ ");
            upload::run(job, tree, m, s.stop);
        }
        if (s.stop.stop_requested()) break;
        // Files that turned up at their names since the look, under Ask: asked about now, the
        // rest of the folder sent already.
        std::vector<Asked> files;
        std::vector<std::size_t> index;
        {
            std::lock_guard lock(m);
            upload::Taken taken;
            for (const auto& w : upload::asking_list(tree, all_of_them, taken)) {
                files.push_back(Asked{.name = b.prefix + rclone_enc::raw_path(tree.files[w.index].path), .clash = w.clash});
                index.push_back(w.index);
            }
        }
        if (files.empty()) break;
        line.clear();
        const auto given = ask_about(s, files, place);
        if (!given) {
            if (given.error() == exit_upload_not_found) return given.error();
            unanswered = true; // those files stay failed
            break;
        }
        std::map<std::size_t, upload::Existing> own;
        for (const auto& [i, c] : given->own) own[index[i]] = c;
        if (given->all == upload::Existing::ask && own.size() < files.size()) { // nothing would change
            unanswered = true;
            break;
        }
        std::lock_guard lock(m);
        upload::answer(job, tree, given->all, own);
    }
    if (s.stop.stop_requested()) {
        // A big file's partial upload stays with its record for the next run; small files start
        // over, so their temps go now.
        upload::Leftovers left;
        folder::Totals t;
        std::size_t sent_here = 0;
        {
            std::lock_guard lock(m);
            left.temps.assign(tree.temps.begin(), tree.temps.end());
            tree.temps.clear();
            t = folder::totals(tree);
            sent_here = sent_files(tree);
        }
        if (!left.empty()) {
            const auto still = upload::clean(*s.pool, s.records_dir, s.server, std::move(left));
            for (const auto& temp : still.temps) {
                line.message(std::format("grab: note: the temp file {} stays on {}", temp, s.server));
            }
        }
        line.finish(std::format("stopped with {} of {} files done; {}", t.files_done, t.files, go_on(run, sent_here)));
        return proc::exit_stopped;
    }

    // --verify: what this run sent, against the server's checksums; nothing local is touched.
    std::string checked_text;
    if (s.verify) {
        const auto check_start = Clock::now();
        folder::Checked checked;
        {
            VerifyLine verifying(line, "verifying against the server's checksums...");
            auto base = verify_request(s, id);
            base.on_progress = [&verifying](std::size_t done, std::size_t total) { verifying.files(done, total); };
            checked = upload::verify_files(job, tree, m, std::move(base), s.stop);
        }
        if (s.stop.stop_requested()) {
            line.finish("stopped while verifying: the files are on the server, but not all verified");
            return proc::exit_stopped;
        }
        if (checked.files > 0) checked_text = std::format("{} in {}", checked.summary(), progress::duration(seconds_since(check_start)));
    }

    folder::Outcome o;
    std::size_t files_sent = 0;
    std::uint64_t bytes_sent = 0;
    std::size_t skipped = 0;
    std::vector<Differs> differs; // server copies verify found different: failed, to go again (Tree::resend)
    {
        std::lock_guard lock(m);
        o = folder::outcome(tree);
        for (std::size_t i = 0; i < tree.files.size(); ++i) {
            const auto& f = tree.files[i];
            if (f.status == folder::State::skipped) ++skipped;
            if (f.status == folder::State::failed && tree.resend.contains(i)) {
                differs.push_back(Differs{.local = local_walk::local_path(b.source, f.path),
                                          .target = folder::child_remote_path(b.target, f.path),
                                          .placed = upload::target_of(job, f)});
            }
            if (f.status != folder::State::done || !f.fetched) continue;
            ++files_sent;
            bytes_sent += f.size;
        }
    }
    if (o.failed > 0) {
        line.clear();
        if (!checked_text.empty()) line.finish(checked_text);
        line.message("grab: error: " + folder::failed_text(o));
        if (unanswered) line.message("grab: " + std::string(existing_hint));
        if (!differs.empty()) say_differs(s, differs);
        line.message(static_cast<std::size_t>(o.failed) > differs.size()
                         ? std::format("grab: the other files are on {}; {}", s.server, go_on(run, files_sent, "retry"))
                         : std::format("grab: the other files are on {}", s.server));
        run.sent += files_sent;
        return exit_upload_failed;
    }
    run.sent += files_sent;
    if (files_sent == 0) {
        line.finish(skipped > 0 ? std::format("nothing uploaded: {} already on {}, skipped",
                                              skipped == 1 ? std::string("its file is") : std::format("all {} files are", progress::count(skipped)),
                                              s.server)
                                : std::format("nothing to send: the folder has no files ({})",
                                              b.merging ? "the folder there is as it was" : "made on " + s.server));
        return 0;
    }
    std::string text = progress::uploaded(files_sent, bytes_sent, sent.load(), place, seconds_since(start));
    if (skipped > 0) text += std::format("; {} already there, skipped", progress::count(skipped));
    line.finish(text);
    if (!checked_text.empty()) line.finish(checked_text);
    return 0;
}

} // namespace

// ---- LOCAL ---------------------------------------------------------------------------------------

namespace {

// A name as code points (a lone surrogate kept as it is), for matching.
std::u32string code_points(std::string_view utf8) {
    const std::wstring w = util::to_wide(utf8);
    std::u32string out;
    out.reserve(w.size());
    for (std::size_t i = 0; i < w.size(); ++i) {
        const auto c = static_cast<char32_t>(w[i]);
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < w.size() && w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF) {
            out += static_cast<char32_t>(0x10000 + ((c - 0xD800) << 10) + (static_cast<char32_t>(w[++i]) - 0xDC00));
        } else {
            out += c;
        }
    }
    return out;
}

// Upper case as Windows compares names (the BMP; others as they are).
char32_t upper(char32_t c) {
    if (c > 0xFFFF) return c;
#ifdef _WIN32
    auto w = static_cast<wchar_t>(c);
    CharUpperBuffW(&w, 1);
    return static_cast<char32_t>(w);
#else
    return static_cast<char32_t>(std::towupper(static_cast<std::wint_t>(c)));
#endif
}

} // namespace

bool local_glob_match(std::string_view pattern, std::string_view name) {
    const std::u32string p = code_points(pattern);
    const std::u32string n = code_points(name);
    constexpr auto none = std::u32string::npos;
    std::size_t pi = 0;
    std::size_t ni = 0;
    std::size_t star = none; // the last '*' seen in the pattern
    std::size_t mark = 0;    // where in the name it covers up to
    while (ni < n.size()) {
        if (pi < p.size() && p[pi] == U'*') {
            star = pi++;
            mark = ni;
        } else if (pi < p.size() && (p[pi] == U'?' || upper(p[pi]) == upper(n[ni]))) {
            ++pi;
            ++ni;
        } else if (star != none) {
            pi = star + 1;
            ni = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == U'*') ++pi;
    return pi == p.size();
}

std::expected<std::vector<fs::path>, std::string> expand_locals(const std::vector<std::string>& typed,
                                                                std::vector<std::string>* notes) {
    std::vector<fs::path> out;
    for (const auto& t : typed) {
        std::error_code ec;
        fs::path p = fs::absolute(util::path_from_utf8(t), ec);
        if (ec) return util::failf("{}: {}", t, ec.message());
        p = p.lexically_normal();
        if (!p.has_filename() && p != p.root_path()) p = p.parent_path(); // "E:\photos\" is the folder
        const fs::path dir = p.parent_path();
        if (util::path_to_utf8(dir).find_first_of("*?") != std::string::npos) {
            return util::failf("{}: * and ? pick names in the last part only", t);
        }
        const std::string last = util::path_to_utf8(p.filename());
        if (last.find_first_of("*?") == std::string::npos) {
            if (!fs::exists(fs::symlink_status(p, ec))) return util::failf("{}: no such file or folder", t);
            out.push_back(std::move(p));
            continue;
        }
        std::vector<fs::path> found;
        // Left out as an upload of the folder leaves them out: the user named none of them.
        std::size_t links = 0;
        std::set<std::string> partial; // a download's data file and its record count once
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = util::path_to_utf8(it->path().filename());
            if (!local_glob_match(last, name)) continue;
            switch (local_walk::left_out(it->path())) {
            case local_walk::Left::link: ++links; continue;
            case local_walk::Left::partial:
                partial.insert(util::to_lower(name.ends_with(".json") ? name.substr(0, name.size() - 5) : name));
                continue;
            case local_walk::Left::no: break;
            }
            found.push_back(it->path());
        }
        if (ec) return util::failf("{}: cannot list {}: {}", t, util::path_to_utf8(dir), ec.message());
        const std::size_t partials = partial.size();
        std::vector<std::string> left;
        if (links > 0) left.push_back(std::format("{} link{}", progress::count(links), links == 1 ? "" : "s"));
        if (partials > 0) left.push_back(std::format("{} partial download{}", progress::count(partials), partials == 1 ? "" : "s"));
        if (found.empty() && !left.empty()) {
            return util::failf("{}: only {} match{}, which grab leaves out (name them to send them)", t, util::join(left, " and "),
                               links + partials == 1 ? "es" : "");
        }
        if (found.empty()) return util::failf("{}: no file or folder matches", t);
        if (!left.empty() && notes != nullptr) {
            notes->push_back(std::format("{}: {} left out (name them to send them)", t, util::join(left, " and ")));
        }
        std::ranges::sort(found, {}, [](const fs::path& f) { return local::key(f.filename()); });
        out.insert(out.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
    }
    return out;
}

std::optional<Typed> read_typed(std::string_view line, bool one_by_one) {
    const std::string typed(util::trim(line));
    // A capital letter answers the rest too.
    const bool rest = typed.size() == 1 && std::string_view("RDNSKB").find(typed[0]) != std::string_view::npos;
    const std::string word = util::to_lower(typed);
    auto choice = [&](upload::Existing e) { return Typed{.kind = Typed::Kind::choice, .choice = e, .rest = rest}; };
    if (word == "r" || word == "replace") return choice(upload::Existing::replace);
    if (word == "d" || word == "different") return choice(upload::Existing::different);
    if (word == "n" || word == "newer") return choice(upload::Existing::newer);
    if (word == "s" || word == "skip") return choice(upload::Existing::skip);
    if (word == "k" || word == "b" || word == "both" || word == "keep both") return choice(upload::Existing::keep_both);
    if (one_by_one && (word == "o" || word == "one by one")) {
        return Typed{.kind = Typed::Kind::one_by_one, .choice = upload::Existing::skip, .rest = false};
    }
    if (word == "q" || word == "quit") return Typed{.kind = Typed::Kind::quit, .choice = upload::Existing::skip, .rest = false};
    return std::nullopt;
}

// ---- the run ---------------------------------------------------------------------------------------

int run_upload(const Options& opts, const std::vector<fs::path>& locals, UploadSetup& s) {
    LiveLine& line = *s.line;
    upload::Check check;
    check.server = s.server;
    check.dir = opts.remote_dir;
    const std::string where = shown(s.server, check.dir);

    // What goes: the files looked at, the folders walked (links left out, never followed).
    line.show("looking at what goes...", true);
    upload::look_local(check, locals, {}, s.stop);
    line.clear();
    if (s.stop.stop_requested()) {
        line.finish("stopped");
        return proc::exit_stopped;
    }
    for (const auto& p : check.items) {
        if (p.tree && !p.tree->unreadable.empty()) {
            line.message(std::format("grab: {}: {}; the rest goes", p.name, folder::unreadable_text(p.tree->unreadable)));
        }
    }
    if (check.links > 0) {
        line.message(std::format("grab: {} link{} or junction{} inside the folders left out (grab never follows them)",
                                 progress::count(check.links), check.links == 1 ? "" : "s", check.links == 1 ? "" : "s"));
    }
    if (check.long_names > 0) {
        line.message(std::format("grab: {} name{} longer than 255 bytes, which no Linux server takes: {} fail",
                                 progress::count(check.long_names), check.long_names == 1 ? " is" : "s are",
                                 check.long_names == 1 ? "that file will" : "those files will"));
    }
    // Nothing of it can go (unreadable, a name Windows can't open): said before anything connects.
    if (!upload::nothing_goes(check).empty()) {
        for (const auto& p : check.items) line.message(std::format("grab: error: {}: {}", util::path_to_utf8(p.source), p.error));
        return exit_upload_failed;
    }

    // Where it goes: REMOTE_DIR must be a folder there.
    if (const auto dir = look_dir(s, check.dir); !dir.folder) {
        if (s.stop.stop_requested()) {
            line.finish("stopped");
            return proc::exit_stopped;
        }
        line.message("grab: error: " + dir.why);
        return dir.missing ? exit_upload_not_found : exit_upload_failed;
    }
    // The server's side: whether this account may write there (-n: nothing written, only what can
    // be seen), and the names taken there.
    line.show(std::format("looking at {}...", where), true);
    upload::look_server(check, s.pool, *s.connections, s.stop, !opts.dry_run);
    line.clear();
    if (s.stop.stop_requested()) {
        line.finish("stopped");
        return proc::exit_stopped;
    }
    if (!check.writable.value_or(false)) {
        line.message("grab: error: " + (check.why.empty() ? std::format("{} can't be written to", where) : check.why));
        return exit_upload_failed;
    }
    std::size_t refused = 0; // items that can't go
    for (const auto& p : check.items) {
        if (p.error.empty()) continue;
        ++refused;
        if (!opts.dry_run) line.message(std::format("grab: error: {}: {}", util::path_to_utf8(p.source), p.error));
    }
    if (opts.dry_run) {
        describe(opts, s, check);
        return refused > 0 ? exit_upload_failed : 0;
    }
    if (refused == check.items.size()) return exit_upload_failed;
    if (upload::short_of_space(check)) {
        line.message(std::format("grab: note: {} to send, {} free on {}: it may not all fit", util::format_size(check.bytes),
                                 util::format_size(check.free.value_or(0)), s.server));
    }

    // The files already there: --existing, the window's setting, or the console's answer.
    auto answer = existing_answer(opts, s, check, where);
    if (!answer) return s.stop.stop_requested() ? proc::exit_stopped : answer.error(); // Ctrl+C at the question
    const auto choices = upload::apply_answer(check, *answer);

    RunState run;
    {
        auto places = std::make_shared<std::unordered_set<std::string>>();
        places->reserve(check.items.size());
        for (const auto& p : check.items) {
            if (p.error.empty()) places->insert(p.target);
        }
        run.places = std::move(places);
        run.replaces = opts.upload_existing.value_or(s.setting) == upload::Existing::replace;
    }
    const std::size_t total = check.items.size();
    std::size_t done = 0;
    std::vector<std::string> failed;
    int code = 0;
    bool stopped = false;
    bool quit = false;
    std::size_t not_started = 0;
    for (std::size_t k = 0; k < total;) {
        const auto& p = check.items[k];
        const std::string local = util::path_to_utf8(p.source);
        if (!p.error.empty()) {
            failed.push_back(local);
            if (code == 0) code = exit_upload_failed;
            ++k;
            continue;
        }
        const int id = static_cast<int>(k) + 1;
        const std::size_t to = batch_end(check, k);
        int c = 0;
        if (to - k > 1) {
            // Picked files of one folder, together.
            upload::Tree tree = batch_tree(check, k, to, choices);
            const Bundle b{.source = p.source.parent_path(), .target = check.dir, .prefix = {}, .merging = true};
            line.message(std::format("grab: {} files from {}  ->  {}{}", progress::count(to - k), util::path_to_utf8(b.source), where,
                                     total > to - k ? std::format("  (picks {}-{}/{})", k + 1, to, total) : ""));
            c = send_tree(s, tree, b, answer->unlisted.value_or(answer->all), id, run);
            if (c != 0 && s.stop.stop_requested()) c = proc::exit_stopped; // also Ctrl+C at a question
            const bool ended = c == proc::exit_stopped || c == exit_upload_not_found;
            for (std::size_t i = 0; i < tree.files.size(); ++i) {
                const auto st = tree.files[i].status;
                if (st == folder::State::done || st == folder::State::skipped) ++done;
                else if (ended) ++not_started;
                else failed.push_back(util::path_to_utf8(check.items[k + i].source));
            }
        } else {
            const std::string position = total > 1 ? std::format(" {}/{}", k + 1, total) : "";
            line.message(std::format("grab: {}{} {}  ->  {}{}", p.folder ? "folder" : "file", position, local, shown(s.server, p.target),
                                     p.folder ? (p.merging ? "  (merged into the folder there)" : "  (a new folder)") : ""));
            if (p.folder) {
                const Bundle b{.source = p.source, .target = p.target, .prefix = p.name + "/", .merging = p.merging};
                c = send_tree(s, *p.tree, b, choices[k], id, run);
            } else {
                c = send_file(s, check, k, choices[k], id, run);
            }
            if (c != 0 && s.stop.stop_requested()) c = proc::exit_stopped; // also Ctrl+C at a question
            if (c == 0) ++done;
            else failed.push_back(local);
        }
        k = to;
        if (c == 0) continue;
        if (code == 0) code = c;
        // Ctrl+C, or [q]uit at a question: the picks after it don't start.
        if (c == proc::exit_stopped || c == exit_upload_not_found) {
            stopped = c == proc::exit_stopped;
            quit = c == exit_upload_not_found;
            not_started += total - to;
            break;
        }
    }
    if (total > 1) {
        std::string summary = std::format("grab: {} of {} done", done, total);
        if (!failed.empty()) summary += std::format("; failed: {}", util::join(failed, ", "));
        if (stopped) summary += "; stopped after Ctrl+C";
        if (quit) summary += "; quit at the question";
        if (not_started > 0) summary += std::format(", {} not started", not_started);
        line.message(summary);
    }
    return code;
}

int upload_with_grab(const engine::Context& ctx, const Options& opts) {
    std::vector<std::string> notes;
    auto locals = expand_locals(opts.locals, &notes);
    if (!locals) {
        std::println(stderr, "grab: error: {}", locals.error());
        return exit_upload_not_found;
    }
    for (const auto& note : notes) std::println(stderr, "grab: {}", note);
    LiveLine line;
    const std::string& server = ctx.settings.name;
    const auto config = opts.config.value_or(default_grab_config_path());
    const auto gui = window_settings(state_dir(config) / "gui.json");
    // The connection limits learned from refusals, by the window or an earlier command, as a
    // download's (download_with_grab); what this run learns is saved there too.
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
    // --limit, else the speed schedule set in grab's window, also mid-upload: its upload speeds (↑).
    rate::RateLimiter limiter;
    if (opts.limit) limiter.set_rate(*opts.limit);
    const auto rules = opts.limit ? std::vector<schedule::Rule>{} : gui.speed_schedule;
    if (auto note = schedule_note(rules, schedule::local_now(), schedule::Dir::up)) std::println(stderr, "grab: {}", *note);
    std::optional<ScheduleFollower> follower;
    if (!opts.dry_run && schedule::limits(rules, schedule::Dir::up)) follower.emplace(rules, schedule::Dir::up, limiter, line);
    const bool verify = opts.verify.value_or(gui.verify_downloads);
    if (verify && !opts.dry_run) {
        std::println(stderr, "grab: uploads are checked against the server's checksum afterwards{}",
                     opts.verify ? "" : " (the window's setting; --no-verify skips it)");
    }
    // The command's --bind, --contimeout and --timeout apply to grab's connections.
    auto target = engine::ssh_target(ctx, opts.connect);
    if (!target) {
        std::println(stderr, "grab: error: {}", target.error());
        return exit_config;
    }
    ssh::Pools pools;
    StopOnCtrlC ctrl_c;
    UploadSetup s;
    s.server = server;
    s.ctx = &ctx;
    s.pool = pools.get(server, *target, budget);
    s.connections = &connections;
    s.budget = budget;
    s.records_dir = state_dir(config) / "uploads";
    s.setting = upload::parse_existing(gui.upload_existing).value_or(upload::Existing::ask);
    s.verify = verify;
    s.files_at_once = std::clamp(gui.parallel, min_parallel, max_parallel);
    s.limiter = &limiter;
    s.line = &line;
    s.console = util::stdin_is_tty();
    s.ask = [&line](const std::string& prompt) -> std::optional<std::string> {
        line.prompt(prompt);
        std::string typed;
        if (!std::getline(std::cin, typed)) return std::nullopt;
        return typed;
    };
    s.stop = ctrl_c.token();
    if (opts.verbose) {
        s.trace = [&line](const std::vector<std::string>& argv) { line.message("+ " + quote::display_cmdline(argv)); };
        std::println(stderr, "grab: up to {} connections to {}", budget, server);
        std::println(stderr, "grab: {} through grab's own SSH client", server);
    }
    const int code = run_upload(opts, *locals, s);
    // A clean run raises a learned limit by one, so a drop that wasn't the server's real limit
    // wears off (review G10), as downloads do.
    if (learned && code == 0 && !opts.dry_run && connections.refusals(server) == 0) {
        if (auto saved = save_learned_limit(limits_file, server, budget::raised(*learned, default_budget), gui.server_limits);
            !saved) {
            std::println(stderr, "grab: {}", saved.error());
        }
    }
    return code;
}

} // namespace grab
