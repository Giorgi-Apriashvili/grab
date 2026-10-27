#include "ssh_find.hpp"

#include "listing.hpp"
#include "quote.hpp"
#include "rclone_encoding.hpp"
#include "util.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <utility>

namespace grab::ssh {

namespace {

constexpr std::string_view probe_ok = "grab-find-ok\n";
// The longest record taken from find: a path that long is a broken or hostile server. With
// -maxdepth N a real one is at most its root, N names of 255 bytes and about 40 more.
constexpr std::size_t max_record = std::size_t{64} * 1024;

// The check's answer for a server whose find grab can't use: it's walked.
FindCheck walk_because(std::string answer, bool lasting = true) {
    return FindCheck{.works = false, .answer = std::move(answer), .lasting = lasting};
}

std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// The probe on one of the pool's connections, its answer dated; nullopt when stopped. Throws
// Error.
std::optional<FindCheck> probe(Pool& pool, const std::stop_token& stop, Retry retry) {
    // The probe is safe to repeat: a reused connection the server closed while it sat idle is
    // dropped and the next one tried.
    for (;;) {
        auto lease = pool.acquire(stop, retry);
        if (!lease) return std::nullopt;
        std::string out;
        ExecOptions o;
        o.stop = stop;
        o.deadline = find_probe_deadline;
        o.out_limit = std::uint64_t{64} * 1024;
        o.out = [&](std::string_view piece) {
            out += piece;
            return true;
        };
        // SFTP started first, so the check sees what a pooled connection sees: a server allowing one
        // channel per login (MaxSessions 1) refuses the command on one whose SFTP runs, every time,
        // and walks, whatever order the connections were used in. A server without SFTP may still
        // run find; that connection isn't reused.
        try {
            [[maybe_unused]] const sftp::Client& started = (*lease)->sftp();
        } catch (const sftp::Broken&) {
        }
        std::expected<ExecResult, ExecError> ran;
        try {
            ran = (*lease)->exec(find_probe, o);
        } catch (const Error&) {
            lease->broken();
            if (lease->reused() && out.empty()) continue;
            throw;
        }
        if (!ran && ran.error().kind == ExecFailure::stopped) return std::nullopt;
        auto check = read_find_probe(ran, out);
        check.checked = unix_now();
        return check;
    }
}

// What the pool's check runs: the answer `memory` kept, else the probe (`asked` set), whose
// answer `memory` keeps when it's lasting.
std::optional<FindCheck> recall_or_probe(Pool& pool, const std::stop_token& stop, Retry retry,
                                         const FindMemory& memory, bool& asked) {
    if (memory.load) {
        if (auto saved = memory.load()) return saved;
    }
    asked = true;
    auto check = probe(pool, stop, retry);
    if (check && check->lasting && memory.save) memory.save(*check);
    return check;
}

// choose_lookup; `again` for recheck_lookup's, which doesn't ask a second time.
std::expected<Lookup, std::string> lookup(Pool& pool, GrabMethod method, std::string_view server,
                                          const std::stop_token& stop, Retry retry, const FindMemory& memory,
                                          bool again) {
    if (method == GrabMethod::walk) return Lookup{};
    bool asked = false;
    std::optional<FindCheck> check;
    try {
        check = pool.find_check(stop, [&] { return recall_or_probe(pool, stop, retry, memory, asked); });
    } catch (const Error& e) {
        return std::unexpected(std::string(e.what()));
    }
    if (!check) return std::unexpected(std::string("stopped"));
    if (!check->works && method == GrabMethod::find) {
        // An earlier run's answer fails nothing: the server may have a find by now.
        if (check->saved && !again) return recheck_lookup(pool, method, server, stop, retry, memory);
        return std::unexpected(std::format("{} has no GNU find for grab to use (find = ssh): its answer to `{}` was {}. "
                                           "Set find = auto or walk in [{}] to search it by listing folders",
                                           server, find_probe, check->answer, server));
    }
    return Lookup{.find = check->works,
                  .answer = check->answer,
                  .checked = check->checked,
                  .saved = check->saved,
                  .asked = asked,
                  .redone = {}};
}

// Whether `record` has the shape find_command prints, "d\t<time>\t<path>" or
// "f\t<size>\t<time>\t<path>", whatever its fields hold.
bool is_record(std::string_view record) {
    const std::ptrdiff_t tabs = record.starts_with("d\t") ? 2 : record.starts_with("f\t") ? 3 : 0;
    return tabs > 0 && std::ranges::count(record, '\t') >= tabs;
}

// Whether an exit status says something other than GNU find ran: it gives 0 or 1, a shell 126 or
// 127 for a command it can't run, a restricted shell what it likes. 128 and up: ended by a signal.
bool not_find_status(int status) { return status < 0 || (status >= 2 && status < 128); }

// A search word or pattern as the server's names are spelled: raw bytes, as raw_path makes
// grab's form of a name (‛E9 is the byte 0xE9). One that would hold a NUL goes as typed.
std::string raw_term(std::string_view term) {
    std::string raw = rclone_enc::raw_path(term);
    return raw.find('\0') == std::string::npos ? raw : std::string(term);
}

// Paths as find prints them (raw bytes, less a leading "./") in grab's form: the root a path lies
// under as the search names it, the rest part by part.
class PathMap {
public:
    explicit PathMap(const FindSpec& spec) : roots_(spec.roots) {
        for (const auto& r : roots_) printed_.push_back(r.empty() ? std::string() : rclone_enc::raw_path(r));
    }

    [[nodiscard]] std::string grab_form(std::string_view raw) const {
        std::optional<std::size_t> best;
        for (std::size_t i = 0; i < roots_.size(); ++i) {
            const std::string& p = printed_[i];
            const bool under = p.empty() ? !raw.starts_with('/')
                                         : raw.starts_with(p) && (raw.size() == p.size() || p.back() == '/' || raw[p.size()] == '/');
            if (under && (!best || p.size() > printed_[*best].size())) best = i;
        }
        if (!best) return rclone_enc::from_raw_path(raw);
        std::string_view rest = raw.substr(printed_[*best].size());
        if (rest.starts_with('/')) rest.remove_prefix(1);
        if (rest.empty()) return roots_[*best];
        return join_remote(roots_[*best], rclone_enc::from_raw_path(rest));
    }
    // A path in a message: "~" for the login folder itself.
    [[nodiscard]] std::string shown(std::string_view raw) const {
        if (raw == "." || raw.empty()) return "~";
        return grab_form(raw);
    }
    // Whether `raw` is root `i` itself, as find was given it ("." for the login folder).
    [[nodiscard]] bool is_root(std::size_t i, std::string_view raw) const {
        return roots_[i].empty() ? raw == "." : raw == printed_[i];
    }
    [[nodiscard]] std::size_t roots() const { return roots_.size(); }

private:
    const std::vector<std::string>& roots_;
    std::vector<std::string> printed_; // each root as its paths start
};

// The path in quotes at `text` (GNU find's quoting in the C locale: '...', with C escapes such
// as \' and \351 for bytes it won't print), as raw bytes less a leading "./"; nullopt when it
// isn't one.
std::optional<std::string> quoted_path(std::string_view text) {
    if (!text.starts_with('\'')) return std::nullopt;
    std::string raw;
    for (std::size_t i = 1; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\'') {
            if (raw.starts_with("./")) raw.erase(0, 2);
            return raw;
        }
        if (c != '\\' || i + 1 == text.size()) {
            raw += c;
            continue;
        }
        const char e = text[++i];
        if (e >= '0' && e <= '7') {
            int value = 0;
            for (int n = 0; n < 3 && i < text.size() && text[i] >= '0' && text[i] <= '7'; ++n, ++i) value = value * 8 + (text[i] - '0');
            --i;
            raw += static_cast<char>(value & 0xFF);
            continue;
        }
        switch (e) {
        case 'n': raw += '\n'; break;
        case 't': raw += '\t'; break;
        case 'r': raw += '\r'; break;
        case 'a': raw += '\a'; break;
        case 'b': raw += '\b'; break;
        case 'f': raw += '\f'; break;
        case 'v': raw += '\v'; break;
        default: raw += e; // \\, \', \", \?
        }
    }
    return std::nullopt; // no closing quote
}

// One line of find's stderr that grab understands: the path it's about and what it says, and
// whether that can mean a root that couldn't be read.
struct FindError {
    std::string path; // raw, less a leading "./"
    std::string why;
    bool unreadable = false;
};

std::optional<FindError> read_error_line(std::string_view line) {
    constexpr std::string_view prefix = "find: ";
    if (!line.starts_with(prefix)) return std::nullopt;
    line.remove_prefix(prefix.size());
    // "File system loop detected; the following directory is part of the cycle: './a/up'" (4.11);
    // before: "File system loop detected; './a/up' is part of the same file system loop as '.'."
    if (line.starts_with("File system loop detected;")) {
        const auto quote = line.find('\'');
        if (quote == std::string_view::npos) return std::nullopt;
        auto path = quoted_path(line.substr(quote));
        if (!path) return std::nullopt;
        return FindError{.path = std::move(*path), .why = "a link loop, skipped", .unreadable = false};
    }
    struct Known {
        std::string_view says;
        std::string_view why;
        bool unreadable;
    };
    static constexpr std::array known{
        Known{.says = "Permission denied", .why = "permission denied", .unreadable = true},
        Known{.says = "No such file or directory", .why = "no such file or folder", .unreadable = true},
        Known{.says = "Too many levels of symbolic links", .why = "a link loop, skipped", .unreadable = false},
    };
    for (const auto& k : known) {
        if (!line.ends_with(k.says) || line.size() < k.says.size() + 2) continue;
        const auto head = line.substr(0, line.size() - k.says.size());
        if (!head.ends_with(": ")) continue;
        auto path = quoted_path(head.substr(0, head.size() - 2));
        if (!path) continue;
        return FindError{.path = std::move(*path), .why = std::string(k.why), .unreadable = k.unreadable};
    }
    return std::nullopt;
}

} // namespace

FindCheck read_find_probe(const std::expected<ExecResult, ExecError>& ran, std::string_view out) {
    if (!ran) {
        switch (ran.error().kind) {
        case ExecFailure::refused: return walk_because(ran.error().message);
        // A slow moment, perhaps: asked again after a while (Pool::find_check).
        case ExecFailure::deadline:
            return walk_because(std::format("no answer within {} s", find_probe_deadline.count()), false);
        case ExecFailure::too_much: return walk_because("more output than a check gives");
        case ExecFailure::stopped: break;
        }
        return walk_because("stopped");
    }
    if (ran->status == 0 && out == probe_ok) return FindCheck{.works = true, .answer = "GNU find"};
    const std::string status = ran->status >= 0     ? std::format("exit {}", ran->status)
                               : ran->signal.empty() ? std::string("no exit status")
                                                     : "ended by SIG" + ran->signal;
    // Ended by a signal (128 and up from a shell): something outside killed it, which may not
    // happen again, so it's asked again after a while rather than saved for a week.
    const bool lasting = ran->signal.empty() && ran->status < 128;
    std::string said = rclone_enc::shown_line(out);
    if (said.empty()) said = rclone_enc::shown_line(ran->err);
    if (said.empty()) return walk_because("nothing, " + status, lasting);
    return walk_because(std::format("\"{}\", {}", said, status), lasting);
}

std::optional<FindCheck> check_find(Pool& pool, std::stop_token stop, Retry retry, const FindMemory& memory) {
    bool asked = false;
    return pool.find_check(stop, [&] { return recall_or_probe(pool, stop, retry, memory, asked); });
}

std::expected<Lookup, std::string> choose_lookup(Pool& pool, GrabMethod method, std::string_view server,
                                                 std::stop_token stop, Retry retry, const FindMemory& memory) {
    return lookup(pool, method, server, stop, retry, memory, false);
}

std::expected<Lookup, std::string> recheck_lookup(Pool& pool, GrabMethod method, std::string_view server,
                                                  std::stop_token stop, Retry retry, const FindMemory& memory) {
    pool.forget_find_check();
    if (memory.save) memory.save(std::nullopt);
    // Asked now, whatever was saved.
    return lookup(pool, method, server, stop, retry, FindMemory{.load = {}, .save = memory.save}, true);
}

std::string find_command(const FindSpec& spec) {
    std::string cmd = "env LC_ALL=C find -L";
    for (const auto& root : spec.roots) {
        cmd += ' ';
        if (root.empty()) {
            cmd += '.';
            continue;
        }
        const std::string raw = rclone_enc::raw_path(root);
        cmd += quote::sh_single(raw.starts_with('/') ? raw : "./" + raw);
    }
    cmd += std::format(" -mindepth 1 -maxdepth {}", spec.max_depth);
    // Dot-entries are pruned as build_find_command prunes them, and names filtered on the server
    // so only hits cross the network.
    if (spec.skip_hidden && !(spec.query && wants_hidden(*spec.query))) cmd += " \\( -name '.*' -prune \\) -o";
    if (spec.query) {
        const Query& q = *spec.query;
        switch (q.kind) {
        case Query::Kind::words:
            for (const auto& term : q.terms) cmd += " -iname " + quote::sh_single("*" + escape_glob(raw_term(term)) + "*");
            break;
        case Query::Kind::glob: cmd += " -iname " + quote::sh_single(raw_term(q.text)); break;
        case Query::Kind::exact: cmd += " -name " + quote::sh_single(raw_term(q.text)); break;
        }
    }
    constexpr std::string_view folders = R"(-type d -printf 'd\t%T@\t%p\0')";
    constexpr std::string_view files = R"(-type f -printf 'f\t%s\t%T@\t%p\0')";
    if (spec.folders && spec.files) cmd += std::format(" \\( {} -o {} \\)", folders, files);
    else cmd += std::format(" {}", spec.folders ? folders : files);
    return cmd;
}

std::expected<std::vector<std::string>, FindFailed> read_find_errors(const FindSpec& spec, std::string_view err,
                                                                    int status, bool cut) {
    const PathMap paths(spec);
    std::vector<std::string> notes;
    std::vector<bool> failed(paths.roots(), false);
    std::string other;       // the first line that isn't one of those
    bool about_find = false; // a line among them not about a path: "command not found", an option refused
    // A cut most likely falls inside a line, which is left out rather than taken for an unknown one.
    std::string_view whole = err;
    if (cut) {
        const auto nl = whole.rfind('\n');
        whole = nl == std::string_view::npos ? std::string_view() : whole.substr(0, nl + 1);
    }
    for (std::string_view rest = whole; !rest.empty();) {
        const auto nl = rest.find('\n');
        const std::string_view line = util::trim(rest.substr(0, nl));
        rest.remove_prefix(nl == std::string_view::npos ? rest.size() : nl + 1);
        if (line.empty()) continue;
        auto e = read_error_line(line);
        if (!e) {
            if (other.empty()) other = rclone_enc::shown_line(line);
            // GNU find names the path first ("find: './x': ..."); a line that doesn't is about
            // find itself, or about a shell that has none.
            about_find = about_find || !line.starts_with("find: '");
            if (status == 0) notes.push_back(rclone_enc::shown_line(line)); // the results are whole all the same
            continue;
        }
        for (std::size_t i = 0; i < failed.size(); ++i) failed[i] = failed[i] || (e->unreadable && paths.is_root(i, e->path));
        notes.push_back(std::format("{}: {}", paths.shown(e->path), e->why));
    }
    if (cut) notes.emplace_back("(more find messages not kept)");
    if (status != 0 && status != 1) {
        const std::string said = rclone_enc::shown_line(err);
        return std::unexpected(FindFailed{
            .message = std::format("find on the server failed ({}){}",
                                   status < 0 ? std::string("no exit status") : std::format("exit {}", status),
                                   said.empty() ? std::string() : ": " + said),
            .about_find = not_find_status(status)});
    }
    if (status == 1 && !other.empty()) {
        return std::unexpected(FindFailed{.message = "find on the server failed: " + other, .about_find = about_find});
    }
    if (!failed.empty() && std::ranges::all_of(failed, [](bool f) { return f; }))
        return std::unexpected(FindFailed{.message = "cannot search " + util::join(notes, "; "), .about_find = false});
    return notes;
}

std::expected<Found, FindFailed> run_find(Pool& pool, const FindSpec& spec, const FindOptions& options) {
    // Not about find unless said: no login, a lost connection, a cap.
    auto failed = [](std::string message, bool about_find = false) {
        return std::unexpected(FindFailed{.message = std::move(message), .about_find = about_find});
    };
    std::optional<Pool::Lease> lease;
    try {
        lease = pool.acquire(options.stop, options.retry_refused);
    } catch (const Error& e) {
        return failed(e.what());
    }
    if (!lease) return Found{}; // stopped
    const PathMap paths(spec);
    Found found;
    std::string partial; // a record whose NUL hasn't come yet
    std::size_t records = 0;
    std::uint64_t bytes = 0;         // of the paths kept
    std::optional<FindFailed> ended; // why the output was cut short
    const std::string too_many_bytes =
        std::format("listing too large: more than {} of paths", util::format_size(options.max_bytes));
    // Keeps a record (one with nothing to save it under is left out, review G15); else why find
    // is to stop.
    auto take = [&](std::string_view record) -> std::optional<FindFailed> {
        auto e = file_list::parse_find_record(record);
        if (!e) {
            if (is_record(record)) return std::nullopt;
            // A shell's banner or a restricted shell's answer: not the find the check found.
            return FindFailed{.message = std::format("the server's find printed something that isn't a find record: \"{}\"",
                                                     rclone_enc::shown_line(record)),
                              .about_find = true};
        }
        e->path = paths.grab_form(e->path);
        bytes += e->path.size();
        if (bytes > options.max_bytes) return FindFailed{.message = too_many_bytes, .about_find = false};
        found.entries.push_back(std::move(*e));
        return std::nullopt;
    };
    ExecOptions exec;
    exec.stop = options.stop;
    exec.out_limit = options.max_bytes; // a backstop: the paths kept reach it first
    exec.out = [&](std::string_view piece) {
        for (auto nul = piece.find('\0'); nul != std::string_view::npos; nul = piece.find('\0')) {
            if (++records > options.max_entries) {
                ended = FindFailed{.message = std::format("listing too large: more than {} entries", options.max_entries),
                                   .about_find = false};
                return false;
            }
            if (partial.empty()) {
                ended = take(piece.substr(0, nul));
            } else {
                partial.append(piece.substr(0, nul));
                ended = take(partial);
                partial.clear();
            }
            if (ended) return false;
            piece.remove_prefix(nul + 1);
        }
        partial.append(piece);
        if (partial.size() > max_record) {
            ended = FindFailed{.message = "the server's find printed a path of more than 64 KiB", .about_find = false};
            return false;
        }
        if (options.progress) options.progress(records);
        return true;
    };
    std::expected<ExecResult, ExecError> ran;
    for (;;) {
        try {
            ran = (*lease)->exec(find_command(spec), exec);
            break;
        } catch (const Error& e) {
            lease->broken();
            // Before any record, find is safe to repeat: a reused connection the server closed
            // while it sat idle is dropped and the next one tried.
            if (!lease->reused() || records > 0 || !partial.empty()) {
                return failed(std::format("find on the server failed: {}", e.what()));
            }
        }
        lease.reset();
        try {
            lease = pool.acquire(options.stop, options.retry_refused);
        } catch (const Error& e) {
            return failed(e.what());
        }
        if (!lease) return Found{}; // stopped
    }
    if (!ran) {
        if (ran.error().kind == ExecFailure::stopped) return Found{};
        if (ended) return std::unexpected(std::move(*ended));
        if (ran.error().kind == ExecFailure::too_much) return failed(too_many_bytes);
        // Refused: an account that may run no commands (any more).
        return failed("find on the server failed: " + ran.error().message, ran.error().kind == ExecFailure::refused);
    }
    if (!ran->signal.empty()) return failed("find on the server was ended by SIG" + ran->signal);
    // The last record, without its NUL.
    if (!partial.empty()) {
        if (auto e = take(partial)) return std::unexpected(std::move(*e));
    }
    auto notes = read_find_errors(spec, ran->err, ran->status, ran->err_cut);
    if (!notes) return std::unexpected(std::move(notes.error()));
    found.notes = std::move(*notes);
    return found;
}

} // namespace grab::ssh
