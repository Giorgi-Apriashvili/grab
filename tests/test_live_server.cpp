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
// The md5sum checks behind phase 5's verification (phase 5 step 1; phase5\box.ps1 runs them on a
// Storage Box) are a case of their own, on the same server and settings, read only as well:
//
//   GRAB_TEST_HASH        "all", or some of probe,home,sftp,speed,paths,batch,cap,tree,parallel,
//                         stop,silence (comma-separated)
//   GRAB_TEST_HASH_FILE   a file (grab's form, from the login folder or absolute) hashed twice,
//                         in each path form and quoting style, and in a batch
//   GRAB_TEST_HASH_MD5    its MD5 when known; else its bytes read over SFTP give it (up to 64
//                         MiB), else the forms are compared with each other
//   GRAB_TEST_HASH_BIG    a folder whose big files (walked 3 deep) are hashed for speed, 1, 2 and
//                         4 at once, a stop, and the long silences
//   GRAB_TEST_HASH_TREE   a folder of awkward names: each file hashed alone in both quoting
//                         styles, with and without -z, and all in one command, against its bytes
//                         read over SFTP
//   GRAB_TEST_HASH_SMALL  a small file for the batch and the command-length checks (default: the
//                         smallest ordinary name in GRAB_TEST_HASH_BIG, else GRAB_TEST_HASH_TREE)
//   GRAB_TEST_HASH_SPEED_GIB   the biggest file the speed checks take, in GiB (default 5)
//   GRAB_TEST_HASH_SILENT_GIB  the most the long silent md5sum reads, in GiB (default 20)
//   GRAB_TEST_HASH_SILENCE     seconds `tail -f` stays silent (default 90)
//
// And one on this PC: GRAB_TEST_MD5_FILE, a big local file hashed by Windows CNG (util::md5_file,
// and 1 MiB reads) and by OpenSSL's EVP, timed (phase 5 step 3: which local hasher).
//
// At most 4 logins at once. Lines start with "hook: ".

#include "browse.hpp"
#include "config.hpp"
#include "engine.hpp"
#include "fake_command.hpp"
#include "ini.hpp"
#include "quote.hpp"
#include "rclone_encoding.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_find.hpp"
#include "ssh_pool.hpp"
#include "ssh_walk.hpp"
#include "util.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)

#include <bcrypt.h>
#endif

using namespace grab;
using namespace std::string_literals;

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

// A command's run, for the log: what exec gave, the start of stdout, how long it took, or how the
// connection broke (ssh::Error: the result then means nothing).
struct Ran {
    std::expected<ssh::ExecResult, ssh::ExecError> result =
        std::unexpected(ssh::ExecError{.kind = ssh::ExecFailure::refused, .message = "not run", .err = {}});
    std::string out; // stdout's first bytes (`keep`)
    double took = 0; // seconds
    std::optional<std::string> lost;
};

// `command` on `session`; `o.out`, if any, still sees all of stdout.
Ran run_on(ssh::Session& session, std::string_view command, ssh::ExecOptions o, std::size_t keep) {
    Ran r;
    auto also = std::move(o.out);
    o.out = [&](std::string_view piece) {
        r.out.append(piece.substr(0, keep - std::min(keep, r.out.size())));
        return !also || also(piece);
    };
    const auto started = Clock::now();
    try {
        r.result = session.exec(command, o);
    } catch (const ssh::Error& e) {
        r.lost = e.what();
    }
    r.took = seconds(Clock::now() - started);
    return r;
}

// `command` on one of the pool's connections; a login that fails is a lost connection too, so
// threads can call it (no REQUIRE).
Ran run_leased(ssh::Pool& pool, std::string_view command, const ssh::ExecOptions& o, std::size_t keep = std::size_t{64} * 1024) {
    try {
        auto lease = pool.acquire({}, ssh::interactive_retry);
        if (!lease) {
            Ran r;
            r.lost = "no connection";
            return r;
        }
        Ran r = run_on(**lease, command, o, keep);
        if (r.lost) lease->broken();
        return r;
    } catch (const ssh::Error& e) {
        Ran r;
        r.lost = "no login: "s + e.what();
        return r;
    }
}

// Options for a short command: a deadline of `limit`, the product's keepalives.
ssh::ExecOptions within(std::chrono::seconds limit) {
    ssh::ExecOptions o;
    o.deadline = limit;
    return o;
}

// Options for a hash of big files: keepalives as the product sends them but no dead-link rule
// (whether the server answers them is what the silence checks find out), 10 minutes at most.
ssh::ExecOptions hashing() {
    ssh::ExecOptions o;
    o.dead_after = std::chrono::hours(1);
    o.deadline = std::chrono::minutes(10);
    return o;
}

// How a run ended, in a few words: "ended with exit 0", "failed: ...", "lost the connection: ...".
std::string ended_how(const Ran& r) {
    if (r.lost) return "lost the connection: " + *r.lost;
    if (!r.result) return "failed: " + r.result.error().message;
    return std::format("ended with exit {}{}", r.result->status, r.result->signal.empty() ? "" : ", signal " + r.result->signal);
}

// A command's whole answer, for the log: the command, then stdout (at most `show` bytes of it) and
// stderr.
void say_ran(std::string_view command, const Ran& r, std::size_t show = 300) {
    if (r.lost) {
        say(std::format("  {}: lost the connection after {:.2f} s: {}", visible(command, 200), r.took, *r.lost));
        return;
    }
    if (!r.result) {
        say(std::format("  {}: not run to its end in {:.2f} s: {}{}", visible(command, 200), r.took, r.result.error().message,
                        r.result.error().err.empty() ? "" : "; stderr \"" + visible(r.result.error().err) + "\""));
        return;
    }
    say(std::format("  {}: exit {}{} in {:.2f} s", visible(command, 200), r.result->status,
                    r.result->signal.empty() ? "" : ", signal " + r.result->signal, r.took));
    say(std::format("    stdout ({} bytes): \"{}\"", r.result->out_bytes, visible(r.out, show)));
    say(std::format("    stderr ({} bytes{}): \"{}\"", r.result->err.size(), r.result->err_cut ? ", cut" : "",
                    visible(r.result->err, show)));
}

// A command's whole answer, for the log.
void run_raw(ssh::Pool& pool, const std::string& command) {
    say_ran(command, run_leased(pool, command, within(std::chrono::seconds(10)), 4096));
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

// What arrived while a command said nothing: the moments the connection's received bytes moved,
// from a second in (the channel's opening and the exec reply) to its first output or its end.
struct Silence {
    std::ptrdiff_t answers = 0;
    double quiet = 0;   // seconds of silence watched
    double longest = 0; // the longest wait between arrivals (from the silence's start, to its end)
    std::string verdict;
};

Silence silence_of(const std::vector<Clock::time_point>& moved, Clock::time_point started,
                   std::optional<Clock::time_point> first_output, Clock::time_point ended) {
    const auto from = started + std::chrono::seconds(1);
    const auto to = first_output.value_or(ended);
    Silence s;
    s.quiet = std::max(0.0, seconds(to - from));
    auto last = from;
    for (const auto t : moved) {
        if (t <= from || t >= to) continue;
        ++s.answers;
        s.longest = std::max(s.longest, seconds(t - last));
        last = t;
    }
    if (to > last) s.longest = std::max(s.longest, seconds(to - last));
    s.verdict = s.quiet < 3.0     ? "unknown: the command wasn't silent long enough"
                : s.answers > 0   ? "ANSWERED"
                                  : "NOT ANSWERED";
    return s;
}

// A command run on `lease` while a thread watches its connection's received bytes: each answer to
// a keepalive is traffic, which is all that keeps a silent command (md5sum) from being taken for a
// dead link.
struct Watched {
    Ran ran;
    Silence silence;
};

Watched watch(ssh::Pool::Lease& lease, std::string_view command, ssh::ExecOptions x) {
    ssh::Connection* c = lease->connection();
    REQUIRE(c);
    std::mutex m;
    std::vector<Clock::time_point> moved;
    std::optional<Clock::time_point> first_output;
    std::atomic<bool> done = false;
    const auto started = Clock::now();
    std::jthread watcher([&] {
        std::uint64_t last = c->received();
        while (!done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            const auto now = c->received();
            if (now == last) continue;
            last = now;
            std::scoped_lock lock(m);
            moved.push_back(Clock::now());
        }
    });
    x.out = [&](std::string_view) {
        std::scoped_lock lock(m);
        if (!first_output) first_output = Clock::now();
        return true;
    };
    Watched w;
    w.ran = run_on(*lease, command, std::move(x), std::size_t{64} * 1024);
    if (w.ran.lost) lease.broken();
    const auto ended = Clock::now();
    done = true;
    watcher.join();
    w.silence = silence_of(moved, started, first_output, ended);
    return w;
}

// ---- md5sum's answers --------------------------------------------------------------------------

constexpr std::string_view md5_of_nothing = "d41d8cd98f00b204e9800998ecf8427e";
constexpr std::string_view sha256_of_nothing = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

// Whether `out` is exactly md5sum's (or sha256sum's) record for empty stdin: "<digest>  -" and a
// NUL (-z) or a line break; nothing before it (a banner) or after it.
bool empty_answer(std::string_view out, bool z, std::string_view digest = md5_of_nothing) {
    return out == std::string(digest) + "  -" + (z ? '\0' : '\n');
}

// One record of md5sum's output: the digest and the name, GNU's escapes undone.
struct Sum {
    std::string digest;
    std::string name;
    bool escaped = false; // started with GNU's '\': the name held a backslash, LF or CR
    bool operator==(const Sum&) const = default;
};
struct Sums {
    std::vector<Sum> records;
    std::string rest; // from the first byte that isn't a whole record on: a banner, cut output
};

// md5sum's records as they come: "<digest>  <name>" ("<digest> *<name>" in binary mode), each
// ended by a NUL with -z, else by a line break. Without -z GNU md5sum starts a record with '\'
// when the name holds a backslash or a line break (and a CR in newer coreutils) and writes those
// as \\, \n and \r. `digits`: 32 for MD5, 64 for SHA-256.
Sums read_sums(std::string_view out, bool z, std::size_t digits = 32) {
    Sums s;
    const char end = z ? '\0' : '\n';
    std::size_t at = 0;
    while (at < out.size()) {
        const std::size_t stop = out.find(end, at);
        if (stop == std::string_view::npos) break;
        std::string_view line = out.substr(at, stop - at);
        Sum sum;
        sum.escaped = line.starts_with('\\');
        if (sum.escaped) line.remove_prefix(1);
        const auto is_hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); };
        if (line.size() <= digits + 2 || !std::ranges::all_of(line.substr(0, digits), is_hex) || line[digits] != ' ' ||
            (line[digits + 1] != ' ' && line[digits + 1] != '*'))
            break;
        sum.digest = line.substr(0, digits);
        const std::string_view name = line.substr(digits + 2);
        bool bad = false;
        for (std::size_t i = 0; i < name.size(); ++i) {
            if (!sum.escaped || name[i] != '\\') {
                sum.name += name[i];
                continue;
            }
            const char next = i + 1 < name.size() ? name[i + 1] : '\0';
            if (next == '\\') {
                sum.name += '\\';
            } else if (next == 'n') {
                sum.name += '\n';
            } else if (next == 'r') {
                sum.name += '\r';
            } else {
                bad = true;
                break;
            }
            ++i;
        }
        if (bad) break;
        s.records.push_back(std::move(sum));
        at = stop + 1;
    }
    s.rest = out.substr(at);
    return s;
}

// One file's md5sum judged: "ok" when its one record names `raw` byte for byte, holds `expected`
// (when known) and md5sum said exit 0; else what was wrong. `digest` gets the record's sum.
std::string judged(const Ran& r, bool z, std::string_view raw, const std::optional<std::string>& expected,
                   std::string* digest = nullptr) {
    if (r.lost) return "LOST: " + *r.lost;
    if (!r.result) return "FAILED: " + r.result.error().message;
    const Sums sums = read_sums(r.out, z);
    if (sums.records.size() != 1 || !sums.rest.empty()) {
        return std::format("FAILED: {} record(s){} (exit {}{})", sums.records.size(),
                           sums.rest.empty() ? "" : ", other output \"" + visible(sums.rest, 80) + "\"", r.result->status,
                           r.result->err.empty() ? "" : "; stderr \"" + visible(r.result->err, 120) + "\"");
    }
    const Sum& s = sums.records.front();
    if (digest) *digest = s.digest;
    if (s.name != raw) return std::format("WRONG NAME \"{}\"", visible(s.name, 120));
    if (expected && s.digest != *expected) return "WRONG SUM " + s.digest;
    if (r.result->status != 0) return std::format("FAILED: exit {}", r.result->status);
    return s.escaped ? "ok (escaped)" : "ok";
}

// `raw` (from the login folder, or absolute) as an absolute path from `home`.
std::string absolute(std::string_view raw, std::string_view home) {
    if (raw.starts_with('/')) return std::string(raw);
    return std::string(home) + (home.ends_with('/') ? "" : "/") + std::string(raw);
}

// The forms a path is tried in: as it is, after "./", and absolute. An absolute one has only its own.
std::vector<std::pair<std::string, std::string>> path_forms(const std::string& raw, std::string_view home) {
    if (raw.starts_with('/')) return {{"absolute", raw}};
    return {{"relative", raw}, {"./path", "./" + raw}, {"absolute", absolute(raw, home)}};
}

// The two ways to quote a path for the server's shell.
struct Style {
    const char* name;
    std::string (*quote)(std::string_view);
};
const std::array<Style, 2> styles{Style{"single quotes", &quote::sh_single}, Style{"rclone's backslashes", &shell_escaped}};

// A name md5sum prints as it is: UTF-8, no control bytes or DEL, no backslash.
bool ordinary(std::string_view raw) {
    return rclone_enc::valid_utf8(raw) && std::ranges::none_of(raw, [](char c) {
               const auto u = static_cast<unsigned char>(c);
               return u < 0x20 || u == 0x7F || c == '\\';
           });
}

// The files of `files` from `least` to `most` bytes, biggest first.
std::vector<ssh::Walked> biggest_files(std::vector<ssh::Walked> files, std::uint64_t least, std::uint64_t most) {
    std::erase_if(files, [&](const ssh::Walked& f) { return f.dir || f.size < least || f.size > most; });
    std::ranges::stable_sort(files, [](const ssh::Walked& a, const ssh::Walked& b) { return a.size > b.size; });
    return files;
}

// From `files` (biggest first), each one that still fits in `budget` bytes, skipping `used` paths;
// at most 32, so one command can name them all.
std::vector<ssh::Walked> up_to(const std::vector<ssh::Walked>& files, std::uint64_t budget,
                               const std::vector<std::string>& used = {}) {
    std::vector<ssh::Walked> picked;
    std::uint64_t total = 0;
    for (const auto& f : files) {
        if (picked.size() == 32) break;
        if (f.size > budget - total || std::ranges::find(used, f.path) != used.end()) continue;
        picked.push_back(f);
        total += f.size;
    }
    return picked;
}

std::uint64_t total_size(const std::vector<ssh::Walked>& files) {
    std::uint64_t total = 0;
    for (const auto& f : files) total += f.size;
    return total;
}

std::string hex(std::span<const unsigned char> bytes) {
    std::string out;
    for (const unsigned char b : bytes) out += std::format("{:02x}", b);
    return out;
}

// MD5 by OpenSSL, for the checks' own sums of what SFTP read.
std::string md5_of(std::string_view bytes) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    REQUIRE(EVP_Digest(bytes.data(), bytes.size(), digest.data(), &size, EVP_md5(), nullptr) == 1);
    return hex(std::span(digest.data(), size));
}

// A file's bytes read over SFTP, when it has at most `most`; nullopt when it's bigger or can't be read.
std::optional<std::string> read_over_sftp(ssh::Pool& pool, const std::string& raw, std::uint64_t most) {
    auto lease = pool.acquire({}, ssh::interactive_retry);
    REQUIRE(lease);
    try {
        auto& s = (*lease)->sftp();
        const auto attrs = sftp::stat(s, raw);
        if (!attrs || !attrs->size || *attrs->size > most) return std::nullopt;
        if (*attrs->size == 0) return std::string();
        auto handle = sftp::open_file(s, raw);
        if (!handle) return std::nullopt;
        std::string bytes(static_cast<std::size_t>(*attrs->size), '\0');
        bool past = false;
        const auto got = sftp::read_range(s, *handle, 0, *attrs->size, [&](std::uint64_t at, std::string_view piece) {
            if (at + piece.size() > bytes.size()) {
                past = true;
                return;
            }
            std::memcpy(bytes.data() + at, piece.data(), piece.size());
        });
        const auto closed = sftp::close_file(s, *handle);
        if (!got || !closed || past) return std::nullopt;
        return bytes;
    } catch (const sftp::Broken&) {
        lease->broken();
        return std::nullopt;
    }
}

// Every file below `root` (grab's form), `depth` levels deep (0: all).
std::vector<ssh::Walked> files_under(ssh::Pool& pool, const std::string& root, int depth) {
    const std::vector<std::string> roots{root};
    auto walked = ssh::walk_roots(pool, roots, ssh::WalkOptions{.connections = 2, .folders = false, .max_depth = depth}, {});
    if (!walked) {
        say(std::format("walk of {} failed: {}", root, walked.error()));
        return {};
    }
    for (const auto& u : walked->unreadable) say("  unreadable: " + u);
    return std::move(walked->entries);
}

std::uint64_t gib_from_env(const char* name, std::uint64_t fallback) {
    const auto v = util::getenv_utf8(name);
    return (v ? std::stoull(*v) : fallback) << 30;
}

double mib_per_s(std::uint64_t bytes, double took) { return took > 0 ? static_cast<double>(bytes) / 1048576.0 / took : 0; }

// libssh2 sends no packet of 35,000 - 256 bytes or more (MAX_SSH_PACKET_LEN in libssh2_priv.h,
// _libssh2_transport_send), and an exec request is 18 bytes besides its command: longer commands
// fail on grab's side before the server sees them.
constexpr std::size_t exec_command_max = 35'000 - 256 - 18 - 1;

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
            ssh::ExecOptions x;
            x.keepalive = std::chrono::seconds(2);
            x.dead_after = std::chrono::seconds(120); // never: this counts the answers
            // The verdict needs 3 s of silence, not the whole md5sum of a film.
            x.deadline = std::chrono::seconds(15);
            const Watched w = watch(*lease, silent, x);
            say(std::format("keepalives (want_reply, one per 2 s of silence) during `{}`: {} arrival(s) in {:.1f} s of "
                            "silence: {}; the command {} in {:.2f} s",
                            silent, w.silence.answers, w.silence.quiet, w.silence.verdict, ended_how(w.ran), w.ran.took));
        }
    }
    say(std::format("{} login(s) in all", logins.load()));
}

// What phase 5's verification needs to know of a server's md5sum before it is built on it: what
// the probe answers, which path forms and quoting work, several files per command, the longest
// command, keepalives through a long silent hash, a stop mid-hash, and md5sum's speed (1, 2 and 4
// at once). Read only: nothing is written on the server, and nothing here but the log.
TEST_CASE("live server: md5sum through grab's own client, printed only (GRAB_TEST_HASH)") {
    const auto name = util::getenv_utf8("GRAB_TEST_SERVER");
    const auto appdata = util::getenv_utf8("GRAB_TEST_APPDATA");
    const auto parts = util::getenv_utf8("GRAB_TEST_HASH");
    if (!name || !appdata || !parts) {
        MESSAGE("skipped: GRAB_TEST_SERVER / GRAB_TEST_APPDATA / GRAB_TEST_HASH not set");
        return;
    }
#ifdef _WIN32
    const AppData settings(util::path_from_utf8(*appdata));
#endif
    const auto wanted = [&](std::string_view part) {
        if (*parts == "all" || *parts == "1") return true;
        const auto list = util::split(*parts, ',');
        return std::ranges::find(list, part) != list.end();
    };
    auto ctx = read_context(*name);
    REQUIRE_MESSAGE(ctx, ctx.error_or(""));
    auto target = engine::ssh_target(*ctx);
    REQUIRE_MESSAGE(target, target.error_or(""));
    const auto& t = *target;
    say(std::format("md5sum checks on {} = {}@{}:{} ({})", *name, t.user, t.host, t.port, *parts));

    std::atomic<int> logins = 0;
    ssh::Pool::Options o;
    o.limit = 4;
    o.logins_at_once = 4;
    o.idle_timer = false;
    auto pool = std::make_shared<ssh::Pool>([&] {
        ++logins;
        return ssh::open_session(t);
    }, o);

    // The login folder: SFTP's REALPATH of ".", which absolute paths are made from.
    std::string home;
    {
        auto lease = pool->acquire({}, ssh::interactive_retry);
        REQUIRE(lease);
        auto real = sftp::realpath((*lease)->sftp(), ".");
        REQUIRE_MESSAGE(real, sftp::describe(real.error_or(sftp::Status{})));
        home = *real;
    }
    say(std::format("login folder (SFTP REALPATH .): \"{}\"", visible(home)));

    // The probe as the product would send it, with empty stdin: -z, then without.
    const auto probe = [&](std::string_view command, bool z) {
        const Ran r = run_leased(*pool, command, within(std::chrono::seconds(10)), 4096);
        return std::pair{r, !r.lost && r.result && empty_answer(r.out, z)};
    };
    const auto z_probe = probe("md5sum -z ''\\-", true);
    const auto plain_probe = probe("md5sum ''\\-", false);
    const bool z = z_probe.second;
    const bool plain = plain_probe.second;
    say(std::format("probe: `md5sum -z ''\\-` {} ({:.2f} s); `md5sum ''\\-` {} ({:.2f} s)",
                    z ? "gives the empty answer" : "FAILS", z_probe.first.took, plain ? "gives the empty answer" : "FAILS",
                    plain_probe.first.took));
    const std::string md5sum = z ? "md5sum -z " : "md5sum ";

    if (wanted("probe")) {
        // The rclone.conf keys rclone saves for an SFTP remote's commands.
        if (ctx->config.rclone_config) {
            const auto doc = ini::parse_file(*ctx->config.rclone_config);
            const ini::Section* section = doc ? doc->find(ctx->settings.rclone_remote) : nullptr;
            const auto key = [&](std::string_view k) {
                const auto v = section ? section->get(k) : std::nullopt;
                return std::format("{} = {}", k, v ? "\"" + *v + "\"" : "(not set)");
            };
            say(std::format("rclone.conf [{}]: {}, {}, {}, {}", ctx->settings.rclone_remote, key("shell_type"),
                            key("md5sum_command"), key("sha1sum_command"), key("path_override")));
        }
        say("raw answers (stdin empty):");
        for (const std::string_view command :
             {"help", "md5sum --version", "sha256sum --version", "md5sum -z ''\\-", "md5sum ''\\-", "md5sum -z \\-",
              "md5sum -z -", "md5sum -", "sha256sum -"}) {
            const Ran r = run_leased(*pool, command, within(std::chrono::seconds(10)), 4096);
            say_ran(command, r, command == "help" ? 2000 : 300);
            if (command.starts_with("md5sum") && command.ends_with('-') && !r.lost && r.result) {
                const bool zz = command.find("-z") != std::string_view::npos;
                say(std::format("    the empty answer exactly: {}", empty_answer(r.out, zz) ? "yes" : "NO"));
            } else if (command == "sha256sum -" && !r.lost && r.result) {
                say(std::format("    the empty answer exactly: {}",
                                empty_answer(r.out, false, sha256_of_nothing) ? "yes" : "NO"));
            }
        }
    }

    if (wanted("home")) {
        const Ran r = run_leased(*pool, "pwd", within(std::chrono::seconds(10)), 4096);
        std::string pwd = r.out;
        if (pwd.ends_with('\n')) pwd.pop_back();
        say(std::format("pwd: \"{}\" ({}) in {:.2f} s: {}", visible(pwd), ended_how(r), r.took,
                        !r.lost && r.result && pwd == home ? "the same as REALPATH ." : "NOT the login folder SFTP has"));
    }

    if (wanted("sftp")) {
        auto lease = pool->acquire({}, ssh::interactive_retry);
        REQUIRE(lease);
        auto& s = (*lease)->sftp();
        std::string has;
        for (const std::string_view e :
             {"check-file", "check-file-name", "check-file-handle", "md5-hash", "md5-hash-handle", "posix-rename@openssh.com",
              "statvfs@openssh.com", "fstatvfs@openssh.com", "hardlink@openssh.com", "fsync@openssh.com",
              "lsetstat@openssh.com", "limits@openssh.com", "expand-path@openssh.com", "copy-data",
              "home-directory", "users-groups-by-id@openssh.com", "space-available", "copy-file"}) {
            if (s.has_extension(e)) has += (has.empty() ? "" : ", ") + std::string(e);
        }
        say(std::format("SFTP version {}; extensions grab knows of: {}", s.version(), has.empty() ? "none" : has));
        // A command beside SFTP on one login (MaxSessions).
        const Ran r = run_on(**lease, "md5sum -z ''\\-", within(std::chrono::seconds(10)), 4096);
        if (r.lost) lease->broken();
        say(std::format("md5sum on a login with SFTP open: {} in {:.2f} s{}", ended_how(r), r.took,
                        !r.lost && !r.result && r.result.error().kind == ssh::ExecFailure::refused ? " (REFUSED)" : ""));
    }

    // The files the rest hashes.
    const auto file = util::getenv_utf8("GRAB_TEST_HASH_FILE");
    std::string file_raw;
    std::uint64_t file_size = 0;
    std::optional<std::string> file_md5 = util::getenv_utf8("GRAB_TEST_HASH_MD5");
    if (file) {
        file_raw = rclone_enc::raw_path(*file);
        auto lease = pool->acquire({}, ssh::interactive_retry);
        REQUIRE(lease);
        const auto attrs = sftp::stat((*lease)->sftp(), file_raw);
        REQUIRE_MESSAGE(attrs, *file << ": " << sftp::describe(attrs.error_or(sftp::Status{})));
        file_size = attrs->size.value_or(0);
        lease->release();
        const char* whence = file_md5 ? "given" : "";
        if (!file_md5 && file_size <= (std::uint64_t{64} << 20)) {
            if (const auto bytes = read_over_sftp(*pool, file_raw, std::uint64_t{64} << 20)) {
                file_md5 = md5_of(*bytes);
                whence = "of its bytes read over SFTP";
            }
        }
        say(std::format("file {} ({}): MD5 {}", visible(*file), util::format_size(file_size),
                        file_md5 ? *file_md5 + " (" + whence + ")" : "unknown (the forms are compared with each other)"));
    }
    std::vector<ssh::Walked> big_files;
    if (const auto big = util::getenv_utf8("GRAB_TEST_HASH_BIG")) {
        const auto started = Clock::now();
        big_files = files_under(*pool, *big, 3);
        const double took = seconds(Clock::now() - started);
        // Only ordinary names go to the server's shell from here: a line break inside quotes is
        // still untried on a Storage Box (phase 5's control-byte rule); GRAB_TEST_HASH_TREE tries them.
        const auto odd = std::erase_if(big_files, [](const ssh::Walked& f) { return !ordinary(rclone_enc::raw_path(f.path)); });
        say(std::format("files under {} (3 deep): {} ({}) in {:.2f} s; {} more with names left out", *big, big_files.size(),
                        util::format_size(total_size(big_files)), took, odd));
    }
    std::vector<ssh::Walked> tree_files;
    const auto tree = util::getenv_utf8("GRAB_TEST_HASH_TREE");
    if (tree) tree_files = files_under(*pool, *tree, 0);
    // A small file of an ordinary name, for a batch and the command-length check.
    std::string small_raw;
    if (const auto given = util::getenv_utf8("GRAB_TEST_HASH_SMALL")) {
        small_raw = rclone_enc::raw_path(*given);
    } else {
        for (const auto* from : {&big_files, &tree_files}) {
            std::optional<ssh::Walked> best;
            for (const auto& f : *from) {
                if (f.size == 0 || f.size > (1 << 20) || !ordinary(rclone_enc::raw_path(f.path))) continue;
                if (!best || f.size < best->size) best = f;
            }
            if (best) {
                small_raw = rclone_enc::raw_path(best->path);
                break;
            }
        }
    }
    const std::string small_abs = small_raw.empty() ? "" : absolute(small_raw, home);
    std::optional<std::string> small_md5;
    if (!small_raw.empty()) {
        if (const auto bytes = read_over_sftp(*pool, small_raw, std::uint64_t{1} << 20)) small_md5 = md5_of(*bytes);
        say(std::format("small file \"{}\": {}", visible(small_raw), small_md5 ? "MD5 " + *small_md5 : "can't be read"));
    }
    const auto speed = biggest_files(big_files, std::uint64_t{256} << 20, gib_from_env("GRAB_TEST_HASH_SPEED_GIB", 5));
    const auto speed_command = [&](std::string_view program, const ssh::Walked& f) {
        return std::string(program) + quote::sh_single(absolute(rclone_enc::raw_path(f.path), home));
    };

    if (wanted("speed")) {
        if (file) {
            for (const char* when : {"first", "again"}) {
                const Ran r = run_leased(*pool, md5sum + quote::sh_single(absolute(file_raw, home)), hashing());
                std::string digest;
                const std::string verdict = judged(r, z, absolute(file_raw, home), file_md5, &digest);
                say(std::format("md5sum of the file ({}), {}: {:.2f} s, {:.0f} MiB/s: {}", util::format_size(file_size), when,
                                r.took, mib_per_s(file_size, r.took), verdict));
                if (!file_md5 && !digest.empty()) file_md5 = digest;
            }
        }
        if (!speed.empty()) {
            const auto& f = speed.front();
            for (const std::string_view program : {"md5sum ", "md5sum ", "sha256sum "}) {
                const Ran r = run_leased(*pool, speed_command(program, f), hashing());
                say(std::format("{}of {} ({}): {:.2f} s, {:.0f} MiB/s ({})", program, visible(f.path, 120),
                                util::format_size(f.size), r.took, mib_per_s(f.size, r.took), ended_how(r)));
            }
        }
    }

    if (wanted("paths") && file) {
        say("path forms and quoting styles, each record's name against the path byte for byte:");
        for (const auto& [form, raw] : path_forms(file_raw, home)) {
            for (const auto& style : styles) {
                const std::string command = md5sum + style.quote(raw);
                const Ran r = run_leased(*pool, command, hashing());
                say(std::format("  {}, {}: {} ({:.2f} s): {}", form, style.name, visible(command, 160), r.took,
                                judged(r, z, raw, file_md5)));
            }
        }
        if (z && plain) {
            const std::string whole = absolute(file_raw, home);
            const Ran r = run_leased(*pool, "md5sum " + quote::sh_single(whole), hashing());
            say(std::format("  absolute, single quotes, without -z: {}", judged(r, false, whole, file_md5)));
        }
    }

    if (wanted("batch") && !small_abs.empty()) {
        // Several files in one command, with one missing in the middle.
        const std::string missing = small_abs.substr(0, small_abs.rfind('/') + 1) + "grab-p5-no-such-file";
        std::vector<std::string> names{small_abs, missing};
        if (file) names.push_back(absolute(file_raw, home));
        std::string args;
        for (const auto& n : names) args += " " + quote::sh_single(n);
        const auto batch = [&](const std::string& command, bool zz) {
            const Ran r = run_leased(*pool, command, hashing());
            const Sums sums = read_sums(r.out, zz);
            std::string records;
            for (const auto& s : sums.records) {
                const bool known = (s.name == small_abs && small_md5 && s.digest == *small_md5) ||
                                   (file && s.name == absolute(file_raw, home) && file_md5 && s.digest == *file_md5);
                records += std::format("{}\"{}\" {}", records.empty() ? "" : ", ", visible(s.name, 100),
                                       known ? "right sum" : s.digest);
            }
            say(std::format("  {}: {}; {} record(s): {}{}", visible(command, 200), ended_how(r), sums.records.size(),
                            records, sums.rest.empty() ? "" : "; other output \"" + visible(sums.rest, 100) + "\""));
            if (!r.lost && r.result) say(std::format("    stderr: \"{}\"", visible(r.result->err)));
        };
        say("several files in one command (one missing in the middle):");
        if (z) batch("md5sum -z" + args, true);
        if (plain) batch("md5sum" + args, false);
        batch(md5sum + "--" + args, z);
    }

    if (wanted("cap") && !small_abs.empty()) {
        // The longest command: the small file named again and again.
        say(std::format("command length (the small file again and again; libssh2's limit is {} bytes):", exec_command_max));
        const std::string arg = " " + quote::sh_single(small_abs);
        const std::string head = md5sum.substr(0, md5sum.size() - 1);
        bool past_limit = false;
        for (const std::size_t size : {std::size_t{8} << 10, std::size_t{32} << 10, exec_command_max, exec_command_max + 1,
                                       std::size_t{64} << 10, std::size_t{128} << 10}) {
            if (past_limit && size > exec_command_max + 1) {
                say(std::format("  {} bytes: not tried, past libssh2's limit as the last one was", size));
                continue;
            }
            std::string command = head;
            std::size_t names = 0;
            while (command.size() + arg.size() <= size) {
                command += arg;
                ++names;
            }
            command.append(size - command.size(), ' ');
            std::size_t records = 0;
            ssh::ExecOptions x = hashing();
            x.deadline = std::chrono::minutes(2);
            x.out = [&, end = z ? '\0' : '\n'](std::string_view piece) {
                records += static_cast<std::size_t>(std::ranges::count(piece, end));
                return true;
            };
            const Ran r = run_leased(*pool, command, x, 256);
            say(std::format("  {} bytes ({} names): {} in {:.2f} s, {} record(s){}", command.size(), names, ended_how(r), r.took,
                            records, !r.lost && r.result && !r.result->err.empty() ? "; stderr \"" + visible(r.result->err, 160) + "\"" : ""));
            past_limit = past_limit || (size > exec_command_max && r.lost.has_value());
        }
    }

    if (wanted("tree") && !tree_files.empty()) {
        say(std::format("awkward names under {} ({} files), each hashed alone (single quotes -z, plain; backslashes -z, "
                        "plain) and all together, against its bytes read over SFTP:",
                        *tree, tree_files.size()));
        std::array<int, 4> ok{};
        std::vector<std::pair<std::string, std::string>> sums; // raw absolute path, MD5 of its bytes
        for (const auto& f : tree_files) {
            const std::string raw = absolute(rclone_enc::raw_path(f.path), home);
            const auto bytes = read_over_sftp(*pool, raw, std::uint64_t{1} << 20);
            if (!bytes) {
                say(std::format("  \"{}\": can't be read over SFTP", visible(raw)));
                continue;
            }
            const std::string md5 = md5_of(*bytes);
            sums.emplace_back(raw, md5);
            std::string cells;
            int i = 0;
            for (const auto& style : styles) {
                for (const bool zz : {true, false}) {
                    const Ran r = run_leased(*pool, (zz ? "md5sum -z " : "md5sum ") + style.quote(raw), hashing());
                    const std::string verdict = judged(r, zz, raw, md5);
                    if (verdict.starts_with("ok")) ++ok[static_cast<std::size_t>(i)];
                    cells += std::format("{}{}", cells.empty() ? "" : " | ", verdict);
                    ++i;
                }
            }
            say(std::format("  \"{}\": {}", visible(raw.substr(raw.rfind('/') + 1)), cells));
        }
        say(std::format("  alone: single quotes -z {} of {}, plain {}; backslashes -z {}, plain {}", ok[0], sums.size(), ok[1],
                        ok[2], ok[3]));
        for (const bool zz : {true, false}) {
            std::string command = zz ? "md5sum -z" : "md5sum";
            for (const auto& s : sums) command += " " + quote::sh_single(s.first);
            const Ran r = run_leased(*pool, command, hashing());
            const Sums got = read_sums(r.out, zz);
            std::size_t paired = 0;
            for (std::size_t k = 0; k < std::min(got.records.size(), sums.size()); ++k) {
                if (got.records[k].name == sums[k].first && got.records[k].digest == sums[k].second) ++paired;
            }
            say(std::format("  all in one command ({}, {} bytes): {}; {} of {} records paired in order with the right sum{}",
                            zz ? "-z" : "plain", command.size(), ended_how(r), paired, sums.size(),
                            got.rest.empty() ? "" : "; other output \"" + visible(got.rest, 100) + "\""));
        }
    }

    if (wanted("parallel") && speed.size() >= 2) {
        // 1, 2 and 4 md5sums at once, each on its own connection and file: the total speed.
        std::size_t next = 1; // speed.front() is the speed check's
        for (const std::size_t n : {std::size_t{1}, std::size_t{2}, std::size_t{4}}) {
            if (next + n > speed.size()) {
                say(std::format("{} at once: not enough big files", n));
                break;
            }
            std::vector<Ran> ran(n);
            std::uint64_t bytes = 0;
            const auto started = Clock::now();
            {
                std::vector<std::jthread> threads;
                for (std::size_t k = 0; k < n; ++k) {
                    const auto& f = speed[next + k];
                    bytes += f.size;
                    threads.emplace_back([&, k, command = speed_command(md5sum, f)] { ran[k] = run_leased(*pool, command, hashing()); });
                }
            }
            const double took = seconds(Clock::now() - started);
            std::string each;
            for (std::size_t k = 0; k < n; ++k) {
                each += std::format("{}{:.0f} MiB/s ({})", k ? ", " : "", mib_per_s(speed[next + k].size, ran[k].took),
                                    ended_how(ran[k]));
            }
            say(std::format("{} md5sum at once on different files: {} in {:.2f} s, {:.0f} MiB/s in all; each {}", n,
                            util::format_size(bytes), took, mib_per_s(bytes, took), each));
            next += n;
        }
    }

    if (wanted("stop") && (!speed.empty() || file)) {
        // A stop 2 s into md5sum of a big file: exec sends TERM and then waits up to 2 s for the
        // server to close the channel before it cuts the link, so a return within about 1 s of the
        // stop means the server closed it (the command ended). Then: does md5sum still run?
        const std::string raw = speed.empty() ? absolute(file_raw, home) : absolute(rclone_enc::raw_path(speed.front().path), home);
        const std::string command = md5sum + quote::sh_single(raw);
        auto lease = pool->acquire({}, ssh::interactive_retry);
        REQUIRE(lease);
        std::stop_source stop;
        Clock::time_point stopped_at{};
        ssh::ExecOptions x = hashing();
        x.stop = stop.get_token();
        std::jthread stopper([&] {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            stopped_at = Clock::now();
            stop.request_stop();
        });
        const Ran r = run_on(**lease, command, x, 4096);
        const auto returned = Clock::now();
        stopper.join();
        const bool cut = (*lease)->connection() && (*lease)->connection()->dropped();
        if (r.lost) lease->broken();
        lease->release();
        say(std::format("stop 2 s into `{}`: {}; exec returned {:.2f} s after the stop; connection {}", visible(command, 160),
                        ended_how(r), seconds(returned - stopped_at), cut ? "dropped" : "kept"));
        say_ran("pgrep -ax md5sum", run_leased(*pool, "pgrep -ax md5sum", within(std::chrono::seconds(10)), 4096));
        const Ran again = run_leased(*pool, command, hashing());
        say(std::format("the same md5sum right after: {:.2f} s ({})", again.took, ended_how(again)));
    }

    if (wanted("silence")) {
        const auto seconds_of = [](const char* env, int fallback) {
            const auto v = util::getenv_utf8(env);
            return std::chrono::seconds(v ? std::stoi(*v) : fallback);
        };
        const auto silence_for = seconds_of("GRAB_TEST_HASH_SILENCE", 90);
        const auto report = [&](std::string_view what, std::string_view command, const Watched& w) {
            say(std::format("{} `{}`: {} arrival(s) in {:.1f} s of silence, longest wait {:.1f} s: {}; the command {} after "
                            "{:.2f} s",
                            what, visible(command, 160), w.silence.answers, w.silence.quiet, w.silence.longest,
                            w.silence.verdict, ended_how(w.ran), w.ran.took));
        };
        const auto run_watched = [&](std::string_view command, const ssh::ExecOptions& x) {
            auto lease = pool->acquire({}, ssh::interactive_retry);
            REQUIRE(lease);
            return watch(*lease, command, x);
        };
        // Pure silence: tail -f of a file nobody writes, stopped by the deadline.
        const std::string tail_of = !small_abs.empty() ? small_abs : file ? absolute(file_raw, home) : "";
        const std::string tail = "tail -n 0 -f " + quote::sh_single(tail_of);
        ssh::ExecOptions x;
        x.keepalive = std::chrono::seconds(2);
        x.dead_after = std::chrono::hours(1);
        if (!tail_of.empty()) {
            x.deadline = silence_for;
            report("keepalives every 2 s, no dead-link rule, during", tail, run_watched(tail, x));
        }
        // md5sum over the biggest files, up to GRAB_TEST_HASH_SILENT_GIB: silent until it ends.
        const auto all = biggest_files(big_files, std::uint64_t{256} << 20, ~std::uint64_t{0});
        const auto first = up_to(all, gib_from_env("GRAB_TEST_HASH_SILENT_GIB", 20));
        double rate = 0; // MiB/s, for the second one
        std::vector<std::string> used;
        if (!first.empty()) {
            std::string command = md5sum.substr(0, md5sum.size() - 1);
            for (const auto& f : first) {
                command += " " + quote::sh_single(absolute(rclone_enc::raw_path(f.path), home));
                used.push_back(f.path);
            }
            x.deadline = std::chrono::minutes(10);
            const Watched w = run_watched(command, x);
            report(std::format("keepalives every 2 s, no dead-link rule, during md5sum of {} files ({}):", first.size(),
                               util::format_size(total_size(first))),
                   command, w);
            if (!w.ran.lost && w.ran.result) {
                const Sums sums = read_sums(w.ran.out, z);
                say(std::format("  {} record(s) of {}; {:.0f} MiB/s", sums.records.size(), first.size(),
                                mib_per_s(total_size(first), w.ran.took)));
                if (w.ran.result->status == 0) rate = mib_per_s(total_size(first), w.ran.took);
            }
        }
        // The product's keepalives (every 15 s, dead after 45 s), past 45 s of silence.
        ssh::ExecOptions product;
        product.deadline = std::chrono::seconds(60);
        if (!tail_of.empty()) report("the product's keepalives (15 s / 45 s), 60 s of", tail, run_watched(tail, product));
        if (rate > 0) {
            // Other files (not in the cache from the first), enough for about 75 s at the first one's speed.
            const auto budget = std::min(gib_from_env("GRAB_TEST_HASH_SILENT_GIB", 20),
                                         static_cast<std::uint64_t>(rate * 75.0) << 20);
            const auto second = up_to(all, budget, used);
            if (!second.empty()) {
                std::string command = md5sum.substr(0, md5sum.size() - 1);
                for (const auto& f : second) command += " " + quote::sh_single(absolute(rclone_enc::raw_path(f.path), home));
                product.deadline = std::chrono::minutes(10);
                report(std::format("the product's keepalives (15 s / 45 s) during md5sum of {} other files ({}):", second.size(),
                                   util::format_size(total_size(second))),
                       command, run_watched(command, product));
            }
        }
    }
    say(std::format("{} login(s) in all", logins.load()));
}

#ifdef _WIN32
namespace {

// MD5 by Windows CNG, fed piece by piece.
class CngMd5 {
public:
    CngMd5() {
        REQUIRE(BCryptOpenAlgorithmProvider(&alg_, BCRYPT_MD5_ALGORITHM, nullptr, 0) >= 0);
        REQUIRE(BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0) >= 0);
    }
    ~CngMd5() {
        if (hash_ != nullptr) BCryptDestroyHash(hash_);
        if (alg_ != nullptr) BCryptCloseAlgorithmProvider(alg_, 0);
    }
    CngMd5(const CngMd5&) = delete;
    CngMd5& operator=(const CngMd5&) = delete;
    // Checked once, in finish(): a check per piece would be timed too.
    void update(std::string_view bytes) {
        ok_ = BCryptHashData(hash_, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                             static_cast<ULONG>(bytes.size()), 0) >= 0 &&
              ok_;
    }
    std::string finish() {
        std::array<unsigned char, 16> digest{};
        REQUIRE(ok_);
        REQUIRE(BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0);
        return hex(digest);
    }

private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    bool ok_ = true;
};

// MD5 by OpenSSL's EVP, fed piece by piece.
class EvpMd5 {
public:
    EvpMd5() : ctx_(EVP_MD_CTX_new()) { REQUIRE((ctx_ != nullptr && EVP_DigestInit_ex(ctx_, EVP_md5(), nullptr) == 1)); }
    ~EvpMd5() { EVP_MD_CTX_free(ctx_); }
    EvpMd5(const EvpMd5&) = delete;
    EvpMd5& operator=(const EvpMd5&) = delete;
    void update(std::string_view bytes) { ok_ = EVP_DigestUpdate(ctx_, bytes.data(), bytes.size()) == 1 && ok_; }
    std::string finish() {
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
        unsigned int size = 0;
        REQUIRE(ok_);
        REQUIRE(EVP_DigestFinal_ex(ctx_, digest.data(), &size) == 1);
        return hex(std::span(digest.data(), size));
    }

private:
    EVP_MD_CTX* ctx_;
    bool ok_ = true;
};

// A file's MD5 by `Hash`, read `chunk` bytes at a time with ReadFile.
template <class Hash> std::string md5_by_readfile(const std::filesystem::path& p, std::size_t chunk) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    Hash hash;
    std::vector<char> buffer(chunk);
    bool ok = true;
    for (;;) {
        DWORD got = 0;
        ok = ReadFile(h, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr) != 0;
        if (!ok || got == 0) break;
        hash.update(std::string_view(buffer.data(), got));
    }
    CloseHandle(h);
    REQUIRE(ok);
    return hash.finish();
}

} // namespace

// Which local hasher phase 5 takes: Windows CNG (util::md5_file, today's 64 KiB reads through a
// C++ stream, and 1 MiB reads) against OpenSSL's EVP MD5 (1 MiB reads), on a big file read from
// the cache, and on a buffer in memory. Printed only.
TEST_CASE("local MD5: Windows CNG against OpenSSL's EVP, timed only (GRAB_TEST_MD5_FILE)") {
    const auto file = util::getenv_utf8("GRAB_TEST_MD5_FILE");
    if (!file) {
        MESSAGE("skipped: GRAB_TEST_MD5_FILE not set");
        return;
    }
    const auto path = util::path_from_utf8(*file);
    const auto size = std::filesystem::file_size(path);
    // Once to bring it into the cache; its sum is the one every other run must give.
    auto started = Clock::now();
    const auto reference = util::md5_file(path);
    REQUIRE_MESSAGE(reference, reference.error_or(""));
    say(std::format("{} ({}): first read, util::md5_file: {:.2f} s, {:.0f} MiB/s; MD5 {}", *file, util::format_size(size),
                    seconds(Clock::now() - started), mib_per_s(size, seconds(Clock::now() - started)), *reference));
    // Three runs of each, every one giving `want`.
    const auto timed = [&](std::string_view what, std::uint64_t bytes, const std::function<std::string()>& run,
                           const std::string& want) {
        std::vector<double> rates;
        for (int i = 0; i < 3; ++i) {
            const auto t0 = Clock::now();
            const std::string digest = run();
            rates.push_back(mib_per_s(bytes, seconds(Clock::now() - t0)));
            CHECK(digest == want);
        }
        std::ranges::sort(rates);
        say(std::format("  {}: {:.0f} / {:.0f} / {:.0f} MiB/s (slowest / median / fastest of 3)", what, rates[0], rates[1],
                        rates[2]));
    };
    say("the file, from the cache:");
    timed("util::md5_file (CNG, 64 KiB stream reads)", size, [&] { return util::md5_file(path).value_or(""); }, *reference);
    timed("CNG, 1 MiB ReadFile", size, [&] { return md5_by_readfile<CngMd5>(path, std::size_t{1} << 20); }, *reference);
    timed("EVP, 1 MiB ReadFile", size, [&] { return md5_by_readfile<EvpMd5>(path, std::size_t{1} << 20); }, *reference);
    timed("CNG, 64 KiB ReadFile", size, [&] { return md5_by_readfile<CngMd5>(path, std::size_t{64} << 10); }, *reference);
    timed("EVP, 64 KiB ReadFile", size, [&] { return md5_by_readfile<EvpMd5>(path, std::size_t{64} << 10); }, *reference);

    // Hashing alone: 256 MiB in memory, fed 1 MiB at a time.
    std::string buffer(std::size_t{256} << 20, '\0');
    std::mt19937_64 random(std::random_device{}());
    for (std::size_t i = 0; i + 8 <= buffer.size(); i += 8) {
        const std::uint64_t v = random();
        std::memcpy(buffer.data() + i, &v, 8);
    }
    const auto in_pieces = [&]<class Hash>(Hash& hash) {
        for (std::size_t at = 0; at < buffer.size(); at += std::size_t{1} << 20)
            hash.update(std::string_view(buffer).substr(at, std::size_t{1} << 20));
        return hash.finish();
    };
    const std::string want = md5_of(buffer);
    say("256 MiB in memory:");
    timed("CNG", buffer.size(), [&] {
        CngMd5 h;
        return in_pieces(h);
    }, want);
    timed("EVP", buffer.size(), [&] {
        EvpMd5 h;
        return in_pieces(h);
    }, want);
}
#endif

// ---- the hooks' own helpers, without a server ---------------------------------------------------

TEST_CASE("hook helpers: md5sum's records read back, with and without -z") {
    const std::string a(32, 'a');
    const std::string b(32, 'b');
    // -z: NUL-ended, names as they are.
    auto s = read_sums(a + "  /x/new\nline\0"s + b + " */y\0"s, true);
    REQUIRE(s.records.size() == 2);
    CHECK(s.records[0] == Sum{a, "/x/new\nline", false});
    CHECK(s.records[1] == Sum{b, "/y", false});
    CHECK(s.rest.empty());
    // Without -z: GNU's '\' and its escapes undone.
    s = read_sums("\\" + a + "  /x/a\\nb\\\\c\\rd\n" + b + "  /plain name\n", false);
    REQUIRE(s.records.size() == 2);
    CHECK(s.records[0] == Sum{a, "/x/a\nb\\c\rd", true});
    CHECK(s.records[1] == Sum{b, "/plain name", false});
    // A banner, an unknown escape, a cut record and a short digest are no records.
    s = read_sums("Welcome!\n" + a + "  /x\n", false);
    CHECK(s.records.empty());
    CHECK(s.rest == "Welcome!\n" + a + "  /x\n");
    s = read_sums(a + "  /x\n\\" + b + "  /y\\t\n", false);
    CHECK(s.records.size() == 1);
    CHECK(s.rest == "\\" + b + "  /y\\t\n");
    s = read_sums(a + "  /x\n" + b + "  /y", false);
    CHECK(s.records.size() == 1);
    CHECK(s.rest == b + "  /y");
    CHECK(read_sums(a.substr(1) + "  /x\n", false).records.empty());
    CHECK(read_sums(std::string(32, 'A') + "  /x\n", false).records.empty()); // md5sum prints lower case
    CHECK(read_sums(a + "  \n", false).records.empty());                      // no name
    // SHA-256's 64 digits.
    CHECK(read_sums(std::string(sha256_of_nothing) + "  -\n", false, 64).records.size() == 1);
}

TEST_CASE("hook helpers: the probe's answer is md5sum's empty record and nothing else") {
    CHECK(empty_answer("d41d8cd98f00b204e9800998ecf8427e  -\0"s, true));
    CHECK(empty_answer("d41d8cd98f00b204e9800998ecf8427e  -\n", false));
    CHECK_FALSE(empty_answer("d41d8cd98f00b204e9800998ecf8427e  -\n", true));
    CHECK_FALSE(empty_answer("d41d8cd98f00b204e9800998ecf8427e  -\0"s, false));
    CHECK_FALSE(empty_answer("Welcome\nd41d8cd98f00b204e9800998ecf8427e  -\n", false)); // a banner (G61)
    CHECK_FALSE(empty_answer("", true));                                               // cmd: no such file `\-`, on stderr
    CHECK(empty_answer(std::string(sha256_of_nothing) + "  -\n", false, sha256_of_nothing));
}

TEST_CASE("hook helpers: path forms, quoting styles and the files picked") {
    auto forms = path_forms("movies/a b.avi", "/home/u");
    REQUIRE(forms.size() == 3);
    CHECK(forms[0].second == "movies/a b.avi");
    CHECK(forms[1].second == "./movies/a b.avi");
    CHECK(forms[2].second == "/home/u/movies/a b.avi");
    CHECK(path_forms("/tmp/x", "/home/u").size() == 1);
    CHECK(absolute("x", "/") == "/x");
    CHECK(shell_escaped("a b'c\nd\\") == "a\\ b\\'c'\n'd\\\\");
    CHECK(quote::sh_single("it's") == "'it'\\''s'");
    CHECK(ordinary("a b é.txt"));
    CHECK_FALSE(ordinary("a\\b"));
    CHECK_FALSE(ordinary("a\tb"));
    CHECK_FALSE(ordinary("caf\xE9"));

    const auto file = [](std::string path, std::uint64_t size) {
        ssh::Walked w;
        w.path = std::move(path);
        w.size = size;
        return w;
    };
    std::vector<ssh::Walked> files{file("a", 5), file("b", 50), file("c", 20), file("d", 30), file("e", 1)};
    files.push_back(file("dir", 99));
    files.back().dir = true;
    const auto big = biggest_files(files, 5, 40);
    REQUIRE(big.size() == 3);
    CHECK(big[0].path == "d");
    CHECK(big[1].path == "c");
    CHECK(big[2].path == "a");
    const auto fit = up_to(biggest_files(files, 1, 100), 60);
    REQUIRE(fit.size() == 3); // 50, then 5 and 1 (30 and 20 don't fit beside 50)
    CHECK(fit[0].path == "b");
    CHECK(total_size(fit) == 56);
    CHECK(up_to(biggest_files(files, 1, 100), 60, {"b"}).size() == 4); // d, c, a, e
}

TEST_CASE("hook helpers: keepalive answers counted in a command's silence") {
    const auto t0 = Clock::now();
    const auto at = [&](double s) { return t0 + std::chrono::milliseconds(static_cast<int>(s * 1000)); };
    // Answers every 2 s from 1 s in to the end at 10 s; the exec reply (0.5 s) isn't one.
    auto s = silence_of({at(0.5), at(3), at(5), at(7), at(9)}, t0, std::nullopt, at(10));
    CHECK(s.answers == 4);
    CHECK(s.quiet == doctest::Approx(9.0));
    CHECK(s.longest == doctest::Approx(2.0));
    CHECK(s.verdict == "ANSWERED");
    // Output at 6 s ends the silence: what comes after doesn't count.
    s = silence_of({at(3), at(8)}, t0, at(6), at(10));
    CHECK(s.answers == 1);
    CHECK(s.longest == doctest::Approx(3.0));
    s = silence_of({}, t0, std::nullopt, at(10));
    CHECK(s.verdict == "NOT ANSWERED");
    CHECK(s.longest == doctest::Approx(9.0));
    CHECK(silence_of({at(2)}, t0, std::nullopt, at(3)).verdict.starts_with("unknown"));
}

TEST_CASE("hook helpers: a run keeps the start of stdout, judged by its one record") {
    struct Session : ssh::Session {
        test::FakeCommand fake;
        sftp::Client& sftp() override { throw sftp::Broken("no SFTP in this test"); }
        std::expected<ssh::ExecResult, ssh::ExecError> exec(std::string_view command,
                                                             const ssh::ExecOptions& options) override {
            return ssh::run_command(fake, command, options);
        }
    } session;
    const std::string sum(32, 'c');
    session.fake.out = {sum + "  /a/b\0"s};
    Ran r = run_on(session, "md5sum -z '/a/b'", within(std::chrono::seconds(5)), 1024);
    CHECK(r.out == sum + "  /a/b\0"s);
    CHECK(judged(r, true, "/a/b", sum) == "ok");
    CHECK(judged(r, true, "/a/b", std::string(32, 'd')).starts_with("WRONG SUM"));
    CHECK(judged(r, true, "/a/c", std::nullopt).starts_with("WRONG NAME"));
    CHECK(judged(r, false, "/a/b", std::nullopt).starts_with("FAILED: 0 record(s)")); // read as lines: no whole record

    // Only `keep` bytes are kept, though `out` sees them all.
    session.fake = {};
    session.fake.out = {std::string(3000, 'x')};
    std::size_t seen = 0;
    ssh::ExecOptions o = within(std::chrono::seconds(5));
    o.out = [&](std::string_view piece) {
        seen += piece.size();
        return true;
    };
    r = run_on(session, "yes", o, 100);
    CHECK(r.out.size() == 100);
    CHECK(seen == 3000);
    REQUIRE(r.result);
    CHECK(r.result->out_bytes == 3000);

    // A link that dies: the run says so instead of throwing.
    session.fake = {};
    session.fake.forever = true;
    session.fake.answers = false;
    ssh::ExecOptions dead;
    dead.keepalive = std::chrono::milliseconds(10);
    dead.dead_after = std::chrono::milliseconds(50);
    r = run_on(session, "md5sum -z '/big'", dead, 100);
    REQUIRE(r.lost);
    CHECK(r.lost.value_or("").find("stopped answering") != std::string::npos);
    CHECK(judged(r, true, "/big", std::nullopt).starts_with("LOST"));
}
