#include "ssh_hash.hpp"

#include "conn_budget.hpp"
#include "quote.hpp"
#include "rclone_encoding.hpp"
#include "sftp.hpp"
#include "util.hpp"

#include <algorithm>
#include <atomic>
#include <format>
#include <mutex>
#include <numeric>
#include <thread>
#include <utility>

namespace grab::ssh {

namespace {

constexpr std::string_view md5_of_nothing = "d41d8cd98f00b204e9800998ecf8427e";
constexpr std::string_view sha1_of_nothing = "da39a3ee5e6b4b0d3255bfef95601890afd80709";
constexpr std::size_t md5_digits = 32;

// A probe on `session`, its stdout in `out` (a few records' worth at most).
std::expected<ExecResult, ExecError> run_probe(Session& session, std::string_view command, const std::stop_token& stop,
                                               std::string& out) {
    out.clear();
    ExecOptions o;
    o.stop = stop;
    o.deadline = hash_probe_deadline;
    o.out_limit = 4096;
    o.out = [&](std::string_view piece) {
        out += piece;
        return true;
    };
    return session.exec(command, o);
}

HashCheck not_working(std::string answer, HashFailure failure, bool lasting) {
    return HashCheck{.works = false, .z = false, .home = {}, .answer = std::move(answer), .failure = failure, .lasting = lasting};
}

HashCheck login_failed(const Error& e) {
    // One the server will refuse again (the key, the password, the host key) holds for the pool's
    // life; a network failure or a busy server only for a while.
    return not_working(e.what(), HashFailure::login, e.kind == budget::Failure::fatal);
}

// The check (hash_check), asked on the pool's connections.
std::optional<HashCheck> ask(Pool& pool, const std::stop_token& stop, Retry retry) {
    std::string home;   // the login folder, once known
    bool fresh = false;   // asking on a new login after a refusal
    bool retried = false; // a reused connection broke and the next one was tried
    // A reused connection the server closed while it sat idle: the next one, once.
    auto again = [&](const Pool::Lease& lease) { return lease.reused() && !std::exchange(retried, true); };
    for (;;) {
        std::optional<Pool::Lease> lease;
        try {
            lease = fresh ? pool.acquire_new(stop, retry) : pool.acquire(stop, retry);
        } catch (const Error& e) {
            return login_failed(e);
        }
        if (!lease) return std::nullopt;
        // The folder the server's relative paths start from, as SFTP has it (the shell's pwd gave
        // the same on Main and a Storage Box). Asked first, so the probe runs beside SFTP as on any
        // pooled connection: where the server allows one channel per login (MaxSessions 1), it is
        // refused there and asked again on a new login with nothing else open.
        if (home.empty()) {
            try {
                auto real = sftp::realpath((*lease)->sftp(), ".");
                if (!real) {
                    return not_working("the login folder can't be found: " + sftp::describe(real.error()),
                                       HashFailure::lost, false);
                }
                if (!real->starts_with('/')) {
                    return not_working(std::format("the login folder isn't an absolute path (\"{}\")",
                                                   rclone_enc::shown_line(*real)),
                                       HashFailure::unsupported, true);
                }
                home = std::move(*real);
            } catch (const sftp::Unavailable& e) {
                lease->broken();
                return not_working(std::format("the login folder can't be found: {}", e.what()), HashFailure::lost, false);
            } catch (const sftp::Broken& e) {
                lease->broken();
                if (again(*lease)) continue;
                return not_working(std::format("the login folder can't be found: {}", e.what()), HashFailure::lost, false);
            }
        }
        std::string out;
        HashCheck check;
        try {
            auto ran = run_probe(**lease, hash_probe, stop, out);
            if (!ran && ran.error().kind == ExecFailure::stopped) return std::nullopt;
            if (!ran && ran.error().kind == ExecFailure::refused && !fresh) {
                fresh = true;
                continue;
            }
            check = read_hash_probe(ran, out, true);
            // md5sum answered, but not as asked: perhaps one without -z (BusyBox).
            if (!check.works && check.lasting) {
                auto plain = run_probe(**lease, hash_probe_plain, stop, out);
                if (!plain && plain.error().kind == ExecFailure::stopped) return std::nullopt;
                auto without = read_hash_probe(plain, out, false);
                if (without.works || !without.lasting) check = std::move(without);
            }
        } catch (const Error& e) {
            lease->broken();
            if (again(*lease)) continue;
            return not_working(std::format("the connection was lost: {}", e.what()), HashFailure::lost, false);
        }
        if (check.works) check.home = std::move(home);
        return check;
    }
}

// Why `path` (absolute, raw) can't go to the server's shell, if it can't.
std::optional<HashFailed> unsendable(std::string_view path, bool z, bool storage_box, const BatchLimits& limits) {
    const auto control = [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u < 0x20 || u == 0x7F;
    };
    // A NUL ends the argument; on a Storage Box, how its shell reads a line break inside quotes is
    // still untried, and it has rm, mv and cp.
    if (path.find('\0') != std::string_view::npos || !path.starts_with('/') ||
        (storage_box && std::ranges::any_of(path, control))) {
        return HashFailed{.kind = HashFailure::unsupported, .why = "the name can't be passed to the server's shell"};
    }
    // fish (a login shell some users pick) reads \' and \\ as escapes even inside single quotes,
    // where sh reads them as they are: a name holding either, or ending in a backslash (which
    // then escapes the closing quote), would end its quoting early there and run the rest. These
    // are the only places the two differ inside one quoted name. Writing the backslashes outside
    // the quotes instead would split GBK and Big5 names, whose second bytes can be a backslash.
    if (path.find("\\'") != std::string_view::npos || path.find("\\\\") != std::string_view::npos ||
        path.ends_with('\\')) {
        return HashFailed{.kind = HashFailure::unsupported, .why = "the name can't be passed to the server's shell"};
    }
    const std::string one(path);
    if (hash_command(std::span(&one, 1), z).size() > limits.command)
        return HashFailed{.kind = HashFailure::unsupported, .why = "the name is too long to pass to the server's shell"};
    return std::nullopt;
}

// One record of md5sum's output (without its end): the digest and the name, GNU's escapes undone.
struct Record {
    std::string_view digest;
    std::string name;
};

std::optional<Record> read_record(std::string_view line, bool z) {
    const bool escaped = !z && line.starts_with('\\');
    if (escaped) line.remove_prefix(1);
    if (line.size() <= md5_digits + 2) return std::nullopt;
    const auto lower_hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); };
    if (!std::ranges::all_of(line.substr(0, md5_digits), lower_hex) || line[md5_digits] != ' ' ||
        (line[md5_digits + 1] != ' ' && line[md5_digits + 1] != '*'))
        return std::nullopt;
    Record r{.digest = line.substr(0, md5_digits), .name = {}};
    const std::string_view name = line.substr(md5_digits + 2);
    if (!escaped) {
        r.name = name;
        return r;
    }
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (name[i] != '\\') {
            r.name += name[i];
            continue;
        }
        if (++i == name.size()) return std::nullopt;
        switch (name[i]) {
        case '\\': r.name += '\\'; break;
        case 'n': r.name += '\n'; break;
        case 'r': r.name += '\r'; break;
        default: return std::nullopt;
        }
    }
    return r;
}

// Why md5sum gave no sum for `path`: the line of its stderr about it (the only line, when the
// command had only this file), else none.
std::string unpaired_why(std::string_view err, std::string_view path, bool alone) {
    for (std::string_view rest = err; !rest.empty();) {
        const auto nl = rest.find('\n');
        const std::string_view line = util::trim(rest.substr(0, nl));
        rest.remove_prefix(nl == std::string_view::npos ? rest.size() : nl + 1);
        if (!line.empty() && (alone || line.find(path) != std::string_view::npos))
            return std::format("no sum from the server ({})", rclone_enc::shown_line(line));
    }
    return "no sum from the server";
}

std::uint64_t saturating_add(std::uint64_t a, std::uint64_t b) { return a > ~std::uint64_t{0} - b ? ~std::uint64_t{0} : a + b; }

} // namespace

HashCheck read_hash_probe(const std::expected<ExecResult, ExecError>& ran, std::string_view out, bool z) {
    if (!ran) {
        switch (ran.error().kind) {
        case ExecFailure::refused: return not_working(ran.error().message, HashFailure::refused, false);
        case ExecFailure::deadline:
            return not_working(std::format("no answer within {} s", hash_probe_deadline.count()), HashFailure::deadline, false);
        case ExecFailure::too_much: return not_working("more output than md5sum gives", HashFailure::no_checksums, true);
        case ExecFailure::stopped: break;
        }
        return not_working("stopped", HashFailure::stopped, false);
    }
    const std::string exact = std::string(md5_of_nothing) + "  -" + (z ? '\0' : '\n');
    if (ran->status == 0 && ran->signal.empty() && out == exact) {
        return HashCheck{.works = true, .z = z, .home = {}, .answer = z ? "md5sum -z" : "md5sum", .failure = {}, .lasting = true};
    }
    const std::string status = ran->status >= 0     ? std::format("exit {}", ran->status)
                               : ran->signal.empty() ? std::string("no exit status")
                                                     : "ended by SIG" + ran->signal;
    // Ended by a signal (128 and up from a shell): something outside killed it, which may not
    // happen again.
    const bool lasting = ran->signal.empty() && ran->status < 128;
    std::string said = rclone_enc::shown_line(out);
    if (said.empty()) said = rclone_enc::shown_line(ran->err);
    if (said.empty()) return not_working("nothing, " + status, HashFailure::no_checksums, lasting);
    return not_working(std::format("\"{}\", {}", said, status), HashFailure::no_checksums, lasting);
}

std::optional<HashCheck> hash_check(Pool& pool, std::stop_token stop, Retry retry) {
    return pool.hash_check(stop, [&] { return ask(pool, stop, retry); });
}

HashFailed hash_check_failed(const HashCheck& check) {
    if (check.failure == HashFailure::no_checksums) {
        return HashFailed{.kind = check.failure,
                          .why = std::format("the server can't compute checksums (its answer to `{}` was {})", hash_probe,
                                             check.answer)};
    }
    return HashFailed{.kind = check.failure, .why = check.answer};
}

std::optional<HashFailed> hash_blocked(const RcloneRemote& remote) {
    if (remote.shell_type) {
        const std::string shell = util::to_lower(*remote.shell_type);
        if (shell == "cmd" || shell == "powershell" || shell == "none") {
            return HashFailed{.kind = HashFailure::no_checksums,
                              .why = std::format("the server can't compute checksums (shell_type = {} in grab.conf)", shell)};
        }
    }
    if (remote.md5sum_command && util::to_lower(*remote.md5sum_command) == "none") {
        return HashFailed{.kind = HashFailure::no_checksums,
                          .why = "the server can't compute checksums (md5sum_command = none in grab.conf)"};
    }
    if (remote.md5sum_command && *remote.md5sum_command != "md5sum") {
        return HashFailed{.kind = HashFailure::unsupported,
                          .why = "md5sum_command in grab.conf isn't supported by grab's check yet"};
    }
    if (remote.path_override) {
        return HashFailed{.kind = HashFailure::unsupported,
                          .why = "path_override in grab.conf isn't supported by grab's check yet"};
    }
    return std::nullopt;
}

std::optional<std::string> read_shell_answer(const std::expected<ExecResult, ExecError>& ran, std::string_view out) {
    if (!ran || !ran->signal.empty() || ran->status < 0 || ran->status >= 128) return std::nullopt;
    if (ran->status != 0) return "none"; // a shell that runs no such command
    const std::string said = util::to_lower(util::trim(out));
    if (said.starts_with("microsoft.powershell")) return "powershell";
    // cmd: "${ShellId}C:\Windows\system32\cmd.exe". Only with the program in it, as rclone checks,
    // so an unforeseen POSIX shell's answer isn't taken for cmd.
    if (!said.ends_with("%comspec%") && (said.contains(".exe") || said.contains(".com"))) return "cmd";
    return "unix";
}

std::optional<bool> read_sum_answer(const std::expected<ExecResult, ExecError>& ran, std::string_view out,
                                    std::string_view empty_sum) {
    if (!ran || !ran->signal.empty() || ran->status < 0 || ran->status >= 128) return std::nullopt;
    if (ran->status != 0) return false;
    std::string_view said = util::trim(out);
    while (said.starts_with('\\')) said.remove_prefix(1);
    return util::to_lower(said.substr(0, said.find(' '))) == empty_sum;
}

ServerCommands learn_commands(Pool& pool, const ServerCommands& known, std::stop_token stop, Retry retry) {
    ServerCommands out = known;
    const auto done = [&] { return out.shell_type && (*out.shell_type == "none" || (out.md5sum_command && out.sha1sum_command)); };
    if (done()) return out;
    std::optional<Pool::Lease> lease;
    try {
        lease = pool.acquire(stop, retry);
    } catch (const Error&) {
        return out; // the login's failure is the caller's to tell
    }
    if (!lease) return out;
    std::string said;
    try {
        if (!out.shell_type) {
            const auto ran = run_probe(**lease, shell_probe, stop, said); // before `said` is read
            out.shell_type = read_shell_answer(ran, said);
            if (!out.shell_type) return out;
        }
        if (done()) return out;
        const auto first_working = [&](std::span<const std::string_view> commands,
                                       std::string_view empty_sum) -> std::optional<std::string> {
            for (const auto command : commands) {
                const auto ran = run_probe(**lease, command, stop, said);
                const auto works = read_sum_answer(ran, said, empty_sum);
                if (!works) return std::nullopt;
                if (*works) return std::string(command);
            }
            return "none";
        };
        if (!out.md5sum_command) out.md5sum_command = first_working(md5_commands, md5_of_nothing);
        if (!out.sha1sum_command) out.sha1sum_command = first_working(sha1_commands, sha1_of_nothing);
    } catch (const Error&) {
        lease->broken();
    }
    return out;
}

std::string absolute_path(std::string_view home, std::string_view raw) {
    if (raw.starts_with('/')) return std::string(raw);
    std::string path(home);
    if (!path.ends_with('/')) path += '/';
    path += raw;
    return path;
}

std::string hash_command(std::span<const std::string> paths, bool z) {
    std::string cmd = z ? "md5sum -z" : "md5sum";
    for (const auto& p : paths) cmd += " " + quote::sh_single(p);
    return cmd;
}

std::vector<std::vector<std::size_t>> plan_batches(std::span<const HashFile> files, bool z, const BatchLimits& limits) {
    std::vector<std::size_t> order(files.size());
    std::ranges::iota(order, std::size_t{0});
    std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) { return files[a].size > files[b].size; });
    const std::size_t head = hash_command({}, z).size();
    std::vector<std::vector<std::size_t>> batches;
    std::vector<std::size_t> open; // the command being filled
    std::size_t length = head;
    std::uint64_t bytes = 0;
    for (const std::size_t i : order) {
        const HashFile& f = files[i];
        if (f.size >= limits.alone || (!z && f.path.find_first_of("\\\r\n") != std::string::npos)) {
            batches.push_back({i});
            continue;
        }
        const std::size_t arg = 1 + quote::sh_single(f.path).size();
        if (!open.empty() && (open.size() >= limits.files || length + arg > limits.command || bytes + f.size > limits.bytes)) {
            batches.push_back(std::exchange(open, {}));
            length = head;
            bytes = 0;
        }
        open.push_back(i);
        length += arg;
        bytes += f.size;
    }
    if (!open.empty()) batches.push_back(std::move(open));
    return batches;
}

std::uint64_t hash_out_limit(std::span<const std::string> paths, bool z) {
    std::uint64_t most = 0;
    for (const auto& p : paths) most += 36 + (z ? 1 : 2) * std::uint64_t{p.size()};
    return most;
}

std::chrono::milliseconds hash_deadline(std::uint64_t bytes, std::chrono::milliseconds base) {
    constexpr auto most = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::days(30));
    // 100 ms per MiB, in whole MiB and the rest, so no product overflows.
    constexpr std::uint64_t mib = std::uint64_t{1} << 20;
    const std::uint64_t ms = (bytes / mib) * 100 + (bytes % mib) * 100 / mib;
    const auto time = std::chrono::milliseconds(static_cast<std::int64_t>(std::min<std::uint64_t>(ms, most.count())));
    return std::min(std::max(base, std::chrono::milliseconds(0)) + time, most);
}

std::vector<std::optional<std::string>> parse_sums_in_order(std::string_view out, std::span<const std::string> sent,
                                                           bool z) {
    std::vector<std::optional<std::string>> sums(sent.size());
    const char end = z ? '\0' : '\n';
    std::size_t next = 0; // the first path sent that no record has named or passed
    for (std::size_t at = 0; at < out.size() && next < sent.size();) {
        const std::size_t stop = out.find(end, at);
        if (stop == std::string_view::npos) break; // cut short: not a record
        const auto record = read_record(out.substr(at, stop - at), z);
        at = stop + 1;
        if (!record) continue;
        for (std::size_t k = next; k < sent.size(); ++k) {
            if (sent[k] != record->name) continue;
            sums[k] = std::string(record->digest);
            next = k + 1;
            break;
        }
    }
    return sums;
}

std::vector<HashResult> run_hashes(Pool& pool, std::span<const HashFile> files, const HashOptions& options) {
    const HashFailed stopped{.kind = HashFailure::stopped, .why = "stopped"};
    std::vector<HashResult> results(files.size(), std::unexpected(stopped));
    std::mutex mutex; // results, `done` and `no_login`
    std::optional<HashFailed> no_login; // a login failed: no more commands start
    std::mutex getting;                 // a command getting its connection
    auto give = [&](std::size_t i, HashResult r) {
        std::scoped_lock lock(mutex);
        results[i] = std::move(r);
        if (options.done) options.done(i, results[i]);
    };
    auto give_all = [&](const HashFailed& why) {
        for (std::size_t i = 0; i < files.size(); ++i) give(i, std::unexpected(why));
    };
    if (files.empty()) return results;
    if (auto blocked = hash_blocked(options.server)) {
        give_all(*blocked);
        return results;
    }
    const auto check = hash_check(pool, options.stop, options.retry_refused);
    if (!check) {
        give_all(stopped);
        return results;
    }
    if (!check->works) {
        give_all(hash_check_failed(*check));
        return results;
    }
    const bool z = check->z;

    // The paths md5sum gets, and the files whose names can't go to the server's shell.
    const bool storage_box = budget::is_storage_box(options.server.host);
    std::vector<HashFile> sent;
    std::vector<std::size_t> from; // sent[k] is files[from[k]]
    for (std::size_t i = 0; i < files.size(); ++i) {
        std::string path = absolute_path(check->home, files[i].path);
        if (auto why = unsendable(path, z, storage_box, options.batches)) {
            give(i, std::unexpected(std::move(*why)));
            continue;
        }
        sent.push_back(HashFile{.path = std::move(path), .size = files[i].size});
        from.push_back(i);
    }
    const auto batches = plan_batches(sent, z, options.batches);

    auto run_batch = [&](const std::vector<std::size_t>& batch) {
        std::vector<std::string> paths;
        std::uint64_t bytes = 0;
        for (const std::size_t k : batch) {
            paths.push_back(sent[k].path);
            bytes = saturating_add(bytes, sent[k].size);
        }
        auto fail_batch = [&](const HashFailed& why) {
            for (const std::size_t k : batch) give(from[k], std::unexpected(why));
        };
        const std::string command = hash_command(paths, z);
        std::string out;
        ExecOptions x;
        x.stop = options.stop;
        x.deadline = hash_deadline(bytes, options.deadline);
        x.out_limit = hash_out_limit(paths, z);
        x.out = [&](std::string_view piece) {
            out += piece;
            return true;
        };
        std::expected<ExecResult, ExecError> ran;
        bool fresh = false;   // on a new login after a refusal
        bool retried = false; // a reused connection broke and the next one was tried
        for (;;) {
            std::optional<Pool::Lease> lease;
            {
                // One of the run's commands at a time gets a connection, so two never log in at
                // once only to fail twice.
                std::scoped_lock one(getting);
                std::optional<HashFailed> refused_login;
                {
                    std::scoped_lock lock(mutex);
                    refused_login = no_login;
                }
                if (refused_login) {
                    fail_batch(*refused_login);
                    return;
                }
                try {
                    lease = fresh ? pool.acquire_new(options.stop, options.retry_refused)
                                  : pool.acquire(options.stop, options.retry_refused);
                } catch (const Error& e) {
                    const HashCheck failed = login_failed(e);
                    pool.set_hash_check(failed);
                    const HashFailed why = hash_check_failed(failed);
                    {
                        std::scoped_lock lock(mutex);
                        no_login = why;
                    }
                    fail_batch(why);
                    return;
                }
            }
            if (!lease) {
                fail_batch(stopped);
                return;
            }
            out.clear();
            if (options.on_command) {
                std::scoped_lock lock(mutex);
                options.on_command(paths, z);
            }
            try {
                ran = (*lease)->exec(command, x);
            } catch (const Error& e) {
                lease->broken();
                // A reused connection the server closed while it sat idle: the next one, once. A
                // second break is the link or the server, not an idle connection, and hashing the
                // batch again on each idle one would only repeat it.
                if (lease->reused() && !std::exchange(retried, true)) continue;
                fail_batch(HashFailed{.kind = HashFailure::lost, .why = std::format("the connection was lost: {}", e.what())});
                return;
            }
            // MaxSessions 1: refused beside the SFTP session a reused connection may have running.
            if (!ran && ran.error().kind == ExecFailure::refused && lease->reused() && !fresh) {
                fresh = true;
                continue;
            }
            break;
        }
        if (!ran) {
            switch (ran.error().kind) {
            case ExecFailure::stopped: fail_batch(stopped); break;
            case ExecFailure::deadline: fail_batch(HashFailed{.kind = HashFailure::deadline, .why = "the server took too long"}); break;
            case ExecFailure::too_much:
                fail_batch(HashFailed{.kind = HashFailure::lost, .why = "the server's md5sum said more than its sums"});
                break;
            case ExecFailure::refused: fail_batch(HashFailed{.kind = HashFailure::refused, .why = ran.error().message}); break;
            }
            return;
        }
        if (!ran->signal.empty()) {
            fail_batch(HashFailed{.kind = HashFailure::lost, .why = "md5sum on the server was ended by SIG" + ran->signal});
            return;
        }
        const auto sums = parse_sums_in_order(out, paths, z);
        for (std::size_t j = 0; j < batch.size(); ++j) {
            if (sums[j]) {
                give(from[batch[j]], *sums[j]);
            } else {
                give(from[batch[j]], std::unexpected(HashFailed{
                                         .kind = HashFailure::unpaired,
                                         .why = unpaired_why(ran->err, paths[j], batch.size() == 1)}));
            }
        }
    };

    std::atomic<std::size_t> next{0};
    auto work = [&] {
        for (std::size_t b = next++; b < batches.size(); b = next++) {
            const auto& batch = batches[b];
            std::optional<HashFailed> refused_login;
            {
                std::scoped_lock lock(mutex);
                refused_login = no_login;
            }
            if (refused_login || options.stop.stop_requested()) {
                for (const std::size_t k : batch) give(from[k], std::unexpected(refused_login.value_or(stopped)));
                continue;
            }
            const auto turn = pool.hash_turn(options.stop);
            if (!turn) {
                for (const std::size_t k : batch) give(from[k], std::unexpected(stopped));
                continue;
            }
            run_batch(batch);
        }
    };
    const auto workers =
        std::min({static_cast<std::size_t>(std::max(options.connections, 1)),
                  static_cast<std::size_t>(pool.hashes_at_once()), batches.size()});
    {
        std::vector<std::jthread> helpers;
        for (std::size_t w = 1; w < workers; ++w) helpers.emplace_back(work);
        work();
    }
    return results;
}

} // namespace grab::ssh
