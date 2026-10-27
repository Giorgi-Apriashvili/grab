#include <doctest/doctest.h>

// A server's own answers to grab's SSH client, printed for the checks a person runs on their
// servers (RCLONE-REPLACEMENT.md, phase 4 step 20; phase4\box.ps1 runs it on a Storage Box). The
// server is loaded from the settings the programs use, as engine::ssh_target loads it, and nothing
// is written, on the server or here: the host-key scan's keys are compared with the pins, never
// saved, and the find check's answer isn't kept.
//
//   GRAB_TEST_SERVER   the server's name in grab.conf
//   GRAB_TEST_APPDATA  the %APPDATA% the programs use (the runner points APPDATA at a scratch
//                      folder, test_main.cpp); grab.conf there, or where GRAB_CONFIG says
//   GRAB_TEST_FOLDER   a folder to walk, twice: 32 and 64 folders in flight per connection
//                      (optional; GRAB_TEST_DEPTH its depth, 0 or unset: all)
//   GRAB_TEST_SILENT   a command that says nothing for several seconds, for the keepalive check;
//                      "md5sum": md5sum of GRAB_TEST_BIG_FILE (a path in grab's form) or else of
//                      the walk's biggest file, watched for at most 15 s (default: `sleep 6` where
//                      find works, else md5sum)
//   GRAB_TEST_NO_SCAN  set: no host-key scan (Main's fail2ban counts its failed negotiation)
//
// At most 4 logins at once. Lines start with "hook: ".

#include "browse.hpp"
#include "config.hpp"
#include "engine.hpp"
#include "rclone_encoding.hpp"
#include "ssh.hpp"
#include "ssh_find.hpp"
#include "ssh_pool.hpp"
#include "ssh_walk.hpp"
#include "util.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)
#endif

using namespace grab;

namespace {

using Clock = std::chrono::steady_clock;

double seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }

void say(const std::string& line) {
    std::fputs(("hook: " + line + "\n").c_str(), stdout);
    std::fflush(stdout);
}

// The length of the UTF-8 sequence `s` starts with; 0 when it isn't one.
// Overlongs, surrogates and leads past U+10FFFF aren't (rclone_enc::valid_utf8).
std::size_t utf8_length(std::string_view s) {
    for (std::size_t n = 2; n <= 4 && n <= s.size(); ++n) {
        if (rclone_enc::valid_utf8(s.substr(0, n))) return n;
    }
    return 0;
}

// Bytes as a log can show them: printable ASCII and UTF-8 as they are, a line break as "\n",
// other bytes as "\xNN"; at most `max` bytes, then "...".
std::string visible(std::string_view s, std::size_t max = 300) {
    std::string out;
    const std::string_view head = s.substr(0, max);
    for (std::size_t i = 0; i < head.size(); ++i) {
        const auto c = static_cast<unsigned char>(head[i]);
        if (c == '\n') {
            out += "\\n";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (c >= 0x20 && c < 0x7F) {
            out += static_cast<char>(c);
        } else if (const std::size_t n = c >= 0x80 ? utf8_length(head.substr(i)) : 0; n > 0) {
            out += head.substr(i, n);
            i += n - 1;
        } else {
            out += std::format("\\x{:02X}", c);
        }
    }
    if (s.size() > max) out += "...";
    return out;
}

#ifdef _WIN32
// APPDATA pointed at `dir` while it lives, so grab's own files are the programs' ones.
class AppData {
public:
    explicit AppData(const std::filesystem::path& dir) : old_(util::getenv_utf8("APPDATA")) {
        SetEnvironmentVariableW(L"APPDATA", dir.c_str());
    }
    ~AppData() {
        SetEnvironmentVariableW(L"APPDATA", old_ ? util::path_from_utf8(*old_).c_str() : nullptr);
    }
    AppData(const AppData&) = delete;
    AppData& operator=(const AppData&) = delete;

private:
    std::optional<std::string> old_;
};
#endif

// The server as the programs load it, read only: engine::load_context would also import remotes
// into grab's rclone.conf and move secrets into the store when there are any, and needs rclone.exe.
std::expected<engine::Context, std::string> read_context(const std::string& name) {
    auto cfg = load_grab_config(default_grab_config_path());
    if (!cfg) return std::unexpected(cfg.error());
    auto selected = cfg->select(name);
    if (!selected) return std::unexpected(selected.error());
    engine::Context ctx;
    ctx.settings = **selected;
    ctx.config = std::move(*cfg);
    ctx.config.rclone_config = effective_rclone_config(ctx.config);
    auto remote = load_rclone_remote(*ctx.config.rclone_config, ctx.settings.rclone_remote);
    if (!remote) return std::unexpected(remote.error());
    ctx.remote = std::move(*remote);
    ctx.method = resolve_find_method(ctx.settings, ctx.remote);
    ctx.engine = Engine::grab;
    ctx.grab_method = resolve_grab_method(ctx.settings, ctx.remote);
    return ctx;
}

// A command's whole answer, for the log.
void run_raw(ssh::Pool& pool, const std::string& command) {
    auto lease = pool.acquire({}, ssh::interactive_retry);
    REQUIRE(lease);
    std::string out;
    ssh::ExecOptions o;
    o.out = [&](std::string_view piece) {
        out.append(piece.substr(0, std::min<std::size_t>(piece.size(), 4096 - std::min<std::size_t>(out.size(), 4096))));
        return true;
    };
    o.deadline = std::chrono::seconds(10);
    const auto started = Clock::now();
    auto ran = (*lease)->exec(command, o);
    const double took = seconds(Clock::now() - started);
    if (!ran) {
        say(std::format("  {}: not run to its end in {:.2f} s: {}{}", command, took, ran.error().message,
                        ran.error().err.empty() ? "" : "; stderr \"" + visible(ran.error().err) + "\""));
        return;
    }
    say(std::format("  {}: exit {}{} in {:.2f} s", command, ran->status,
                    ran->signal.empty() ? "" : ", signal " + ran->signal, took));
    say(std::format("    stdout ({} bytes): \"{}\"", ran->out_bytes, visible(out)));
    say(std::format("    stderr ({} bytes{}): \"{}\"", ran->err.size(), ran->err_cut ? ", cut" : "", visible(ran->err)));
}

// `path` (raw bytes) for the server's shell, much as rclone quotes it for md5sum: a backslash
// before every byte that isn't a letter, a digit or one of _/.- and a line break quoted ('
// line break ') as rclone has it (a backslash before one would join the lines). rclone also
// leaves ,:@ and bytes from 0x80 bare, where the backslash does nothing.
std::string shell_escaped(std::string_view path) {
    std::string out;
    for (const char c : path) {
        if (c == '\n') {
            out += "'\n'";
            continue;
        }
        const auto u = static_cast<unsigned char>(c);
        const bool plain = (u >= '0' && u <= '9') || (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || c == '_' ||
                           c == '/' || c == '.' || c == '-';
        if (!plain) out += '\\';
        out += c;
    }
    return out;
}

} // namespace

TEST_CASE("live server: what it answers grab's own client, printed only (GRAB_TEST_SERVER)") {
    const auto name = util::getenv_utf8("GRAB_TEST_SERVER");
    const auto appdata = util::getenv_utf8("GRAB_TEST_APPDATA");
    if (!name || !appdata) {
        MESSAGE("skipped: GRAB_TEST_SERVER / GRAB_TEST_APPDATA not set");
        return;
    }
#ifdef _WIN32
    const AppData home(util::path_from_utf8(*appdata));
#endif
    auto ctx = read_context(*name);
    REQUIRE_MESSAGE(ctx, ctx.error_or(""));
    auto target = engine::ssh_target(*ctx);
    REQUIRE_MESSAGE(target, target.error_or(""));
    const auto& t = *target;
    const char* method = ctx->grab_method == GrabMethod::walk   ? "walk"
                         : ctx->grab_method == GrabMethod::find ? "find only"
                                                                : "probe, then find or walk";
    say(std::format("server {} = {}@{}:{}, login by {}; searches on engine = grab: {}", *name, t.user, t.host, t.port,
                    t.agent ? "ssh-agent" : !t.key_file.empty() ? "a key file" : "a password", method));
    const std::string pinned_text = util::read_file(t.known_hosts).value_or("");
    const std::string pinned = ssh::pinned_algorithms(pinned_text, t.host, t.port);
    say(std::format("known_hosts {}: pinned for it: {}", util::path_to_utf8(t.known_hosts),
                    pinned.empty() ? "nothing" : pinned));

    // The host keys, scanned and compared with the pins; nothing is written.
    if (!util::getenv_utf8("GRAB_TEST_NO_SCAN")) {
        auto started = Clock::now();
        auto keys = ssh::scan_host_keys(t.host, t.port, std::chrono::seconds(10));
        if (!keys) {
            say(std::format("host-key scan failed in {:.2f} s: {}", seconds(Clock::now() - started), keys.error()));
        } else {
            say(std::format("host-key scan: {} key(s) in {:.2f} s", keys->size(), seconds(Clock::now() - started)));
            for (const auto& k : *keys) {
                const auto known = ssh::check_known_host(pinned_text, t.host, t.port, util::base64_decode(k.key).value_or(""));
                say(std::format("  {} {}: {}", k.type, k.fingerprint,
                                known == ssh::KnownHost::match      ? "pinned"
                                : known == ssh::KnownHost::mismatch ? "CHANGED: another key of this type is pinned"
                                                                    : "not pinned"));
            }
        }
    }

    std::atomic<int> logins = 0;
    ssh::Pool::Options o;
    o.limit = 4;
    o.logins_at_once = 4;
    o.idle_timer = false;
    auto pool = std::make_shared<ssh::Pool>([&] {
        ++logins;
        return ssh::open_session(t);
    }, o);

    // A login, and what it agreed.
    {
        auto started = Clock::now();
        auto lease = pool->acquire({}, ssh::interactive_retry);
        REQUIRE(lease);
        const auto* c = (*lease)->connection();
        say(std::format("login: {:.2f} s ({} ms in the handshake and login); {}", seconds(Clock::now() - started),
                        c ? c->login_time().count() : 0, c ? c->algorithms() : std::string()));
        started = Clock::now();
        const bool sftp = sftp::realpath((*lease)->sftp(), ".").has_value();
        say(std::format("SFTP started in {:.2f} s{}", seconds(Clock::now() - started), sftp ? "" : "; REALPATH . failed"));
    }

    // The find check, and the raw answers behind it.
    {
        const auto started = Clock::now();
        auto check = ssh::check_find(*pool, {}, ssh::interactive_retry);
        REQUIRE(check);
        say(std::format("find check: {} ({}{}) in {:.2f} s", check->works ? "find works" : "no find for grab",
                        check->answer, check->lasting ? "" : "; would be asked again in 5 min",
                        seconds(Clock::now() - started)));
        say("raw answers:");
        run_raw(*pool, std::string(ssh::find_probe));
        run_raw(*pool, "find --version");
    }

    // Browse of the login folder.
    {
        engine::GrabSide side;
        side.pool = pool;
        const auto started = Clock::now();
        auto items = browse::list_live(*ctx, "", {}, {}, side);
        if (!items) {
            say(std::format("Browse of the login folder failed in {:.2f} s: {}", seconds(Clock::now() - started),
                            items.error().message));
        } else {
            const auto folders = std::ranges::count_if(*items, [](const browse::Item& i) { return i.dir; });
            std::string first;
            for (std::size_t i = 0; i < std::min<std::size_t>(items->size(), 5); ++i) first += (i ? ", " : "") + (*items)[i].name;
            say(std::format("Browse of the login folder: {} folders, {} files in {:.2f} s ({}{})", folders,
                            static_cast<std::ptrdiff_t>(items->size()) - folders, seconds(Clock::now() - started), first,
                            items->size() > 5 ? ", ..." : ""));
        }
    }

    // A walk of the folder, 32 and then 64 folders in flight on each connection (how many handles
    // a Storage Box allows is unknown).
    std::optional<std::pair<std::string, std::uint64_t>> biggest; // a file for md5sum
    if (const auto folder = util::getenv_utf8("GRAB_TEST_FOLDER")) {
        const int depth = std::stoi(util::getenv_utf8("GRAB_TEST_DEPTH").value_or("0"));
        for (const int in_flight : {32, 64}) {
            ssh::WalkOptions w{.connections = 4, .folders = true, .max_depth = depth, .skip_hidden = ctx->settings.skip_hidden,
                               .in_flight = in_flight};
            const std::vector<std::string> roots{*folder};
            const auto started = Clock::now();
            auto walked = ssh::walk_roots(*pool, roots, w, {});
            const double took = seconds(Clock::now() - started);
            if (!walked) {
                say(std::format("walk of {} ({} in flight): failed in {:.2f} s: {}", *folder, in_flight, took, walked.error()));
                continue;
            }
            std::size_t files = 0;
            std::uint64_t bytes = 0;
            for (const auto& e : walked->entries) {
                if (e.dir) continue;
                ++files;
                bytes += e.size;
                // Best a name the server's shell takes as it is.
                const auto raw = rclone_enc::raw_path(e.path);
                const bool plain = shell_escaped(raw) == raw;
                const bool better = !biggest || (plain && shell_escaped(biggest->first) != biggest->first) ||
                                    (plain == (shell_escaped(biggest->first) == biggest->first) && e.size > biggest->second);
                if (better) biggest = std::pair{raw, e.size};
            }
            say(std::format("walk of {} (depth {}, {} folders in flight per connection, 4 connections): {} entries ({} files, "
                            "{}) in {:.2f} s, {} unreadable",
                            *folder, depth, in_flight, walked->entries.size(), files, util::format_size(bytes), took,
                            walked->unreadable.size()));
            for (std::size_t i = 0; i < std::min<std::size_t>(walked->unreadable.size(), 3); ++i)
                say("  unreadable: " + walked->unreadable[i]);
        }
    }

    // Keepalives sent while a command says nothing: each answer is traffic on the connection, which
    // is all that keeps a silent command (P5's md5sum) from being taken for a dead link.
    {
        std::string silent = util::getenv_utf8("GRAB_TEST_SILENT").value_or("");
        if (silent.empty()) {
            auto check = ssh::check_find(*pool, {});
            silent = check && check->works ? "sleep 6" : "md5sum";
        }
        if (silent == "md5sum") {
            const auto big = util::getenv_utf8("GRAB_TEST_BIG_FILE");
            const std::optional<std::string> raw = big ? std::optional(rclone_enc::raw_path(*big))
                                                   : biggest ? std::optional(biggest->first)
                                                             : std::nullopt;
            silent = raw ? "md5sum " + shell_escaped(*raw) : "";
        }
        if (silent.empty()) {
            say("keepalives: not checked (no command: give GRAB_TEST_SILENT, GRAB_TEST_BIG_FILE or GRAB_TEST_FOLDER)");
        } else {
            auto lease = pool->acquire({}, ssh::interactive_retry);
            REQUIRE(lease);
            ssh::Connection* c = (*lease)->connection();
            REQUIRE(c);
            std::mutex m;
            std::vector<std::pair<Clock::time_point, std::uint64_t>> seen; // received() as it moved
            std::optional<Clock::time_point> first_output;
            std::atomic<bool> done = false;
            const auto started = Clock::now();
            std::jthread watch([&] {
                std::uint64_t last = c->received();
                while (!done) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    const auto now = c->received();
                    if (now == last) continue;
                    last = now;
                    std::scoped_lock lock(m);
                    seen.emplace_back(Clock::now(), now);
                }
            });
            ssh::ExecOptions x;
            x.keepalive = std::chrono::seconds(2);
            x.dead_after = std::chrono::seconds(120); // never: this counts the answers
            // The verdict needs 3 s of silence, not the whole md5sum of a film.
            x.deadline = std::chrono::seconds(15);
            x.out = [&](std::string_view) {
                std::scoped_lock lock(m);
                if (!first_output) first_output = Clock::now();
                return true;
            };
            std::expected<ssh::ExecResult, ssh::ExecError> ran;
            std::optional<std::string> lost; // the connection, with what was measured until then
            try {
                ran = (*lease)->exec(silent, x);
            } catch (const ssh::Error& e) {
                lost = e.what();
                lease->broken();
            }
            const auto ended = Clock::now();
            done = true;
            watch.join();
            // Silence: from a second in (the channel's opening and the exec reply) to its first
            // output or its end.
            const auto from = started + std::chrono::seconds(1);
            const auto to = first_output.value_or(ended);
            const auto answers = std::ranges::count_if(seen, [&](const auto& s) { return s.first > from && s.first < to; });
            const double quiet = std::max(0.0, seconds(to - from));
            std::string verdict = quiet < 3.0     ? "unknown: the command wasn't silent long enough"
                                  : answers > 0   ? "ANSWERED"
                                                  : "NOT ANSWERED";
            say(std::format("keepalives (want_reply, one per 2 s of silence) during `{}`: {} arrival(s) in {:.1f} s of "
                            "silence: {}; the command {} in {:.2f} s",
                            silent, answers, quiet, verdict,
                            lost  ? "lost the connection: " + *lost
                            : ran ? std::format("ended with exit {}", ran->status)
                                  : "failed: " + ran.error().message,
                            seconds(ended - started)));
        }
    }
    say(std::format("{} login(s) in all", logins.load()));
}
