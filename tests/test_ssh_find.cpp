#include <doctest/doctest.h>

#include "config.hpp"
#include "conn_budget.hpp"
#include "fake_command.hpp"
#include "file_list.hpp"
#include "match.hpp"
#include "rclone_encoding.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_find.hpp"
#include "ssh_pool.hpp"
#include "ssh_walk.hpp"
#include "util.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace grab;
using namespace std::string_literals;

namespace {

// A server answering commands as `answer` sets them up, in pooled sessions without SFTP. It
// records each command and what was done to its channel.
struct Shell {
    std::function<void(test::FakeCommand&, std::string_view command)> answer;
    std::mutex mutex;
    std::vector<std::string> commands;
    std::vector<std::string> log; // every command's channel log, one after another
    std::atomic<int> logins{0};
    std::atomic<int> sftp_starts{0}; // tries to start SFTP, which this server doesn't have

    struct Session : ssh::Session {
        explicit Session(Shell& s) : shell(s) {}
        sftp::Client& sftp() override {
            ++shell.sftp_starts;
            throw sftp::Broken("no SFTP in this test");
        }
        std::expected<ssh::ExecResult, ssh::ExecError> exec(std::string_view command,
                                                             const ssh::ExecOptions& options) override {
            test::FakeCommand c;
            shell.answer(c, command);
            auto r = ssh::run_command(c, command, options);
            dropped = dropped || c.dropped;
            std::scoped_lock lock(shell.mutex);
            shell.commands.emplace_back(command);
            shell.log.insert(shell.log.end(), c.log.begin(), c.log.end());
            return r;
        }
        bool reusable() override { return !dropped; }
        Shell& shell;
        bool dropped = false;
    };

    std::shared_ptr<ssh::Pool> pool(int limit = 2) {
        ssh::Pool::Options o;
        o.limit = limit;
        o.idle_timer = false;
        return std::make_shared<ssh::Pool>(
            [this] {
                ++logins;
                return std::make_unique<Session>(*this);
            },
            o);
    }
};

std::expected<ssh::ExecResult, ssh::ExecError> ended(std::string err, int status, std::string signal = {}) {
    ssh::ExecResult r;
    r.err = std::move(err);
    r.status = status;
    r.signal = std::move(signal);
    return r;
}

std::expected<ssh::ExecResult, ssh::ExecError> failed(ssh::ExecFailure kind, std::string message) {
    return std::unexpected(ssh::ExecError{.kind = kind, .message = std::move(message), .err = {}});
}

std::vector<std::string> paths_of(const std::vector<file_list::Entry>& entries) {
    std::vector<std::string> paths;
    for (const auto& e : entries) paths.push_back(e.path);
    return paths;
}

// Why find failed, for a message; "" when it didn't.
template <class T>
std::string why(const std::expected<T, ssh::FindFailed>& r) {
    return r ? std::string() : r.error().message;
}

enum class Login { sh, fish, pwsh };

// The bytes of a single quote at command[i] as `shell` reads it, 0 for none: ' everywhere, and
// under pwsh also the typographic ones, U+2018 to U+201B (UTF-8 E2 80 98 to E2 80 9B).
std::size_t quote_at(std::string_view command, std::size_t i, Login shell) {
    if (command[i] == '\'') return 1;
    if (shell != Login::pwsh || command.substr(i, 2) != "\xE2\x80" || i + 2 >= command.size()) return 0;
    const auto third = static_cast<unsigned char>(command[i + 2]);
    return third >= 0x98 && third <= 0x9B ? 3 : 0;
}

// The words of a command as a login shell reads them: POSIX sh takes everything inside single
// quotes as it is; fish reads \\ and \' there as escapes; pwsh also opens and closes them with
// U+2018 to U+201B, takes a doubled quote inside as one, and a backslash as a plain byte anywhere.
// A backslash outside quotes escapes the next character in sh and fish. nullopt: quotes that
// don't close (fish and pwsh: a syntax error).
std::optional<std::vector<std::string>> shell_words(std::string_view command, Login shell) {
    const bool fish = shell == Login::fish;
    std::vector<std::string> words;
    std::string word;
    bool in_word = false;
    for (std::size_t i = 0; i < command.size(); ++i) {
        const char c = command[i];
        if (const std::size_t open = quote_at(command, i, shell); open > 0) {
            in_word = true;
            bool closed = false;
            for (i += open; i < command.size(); ++i) {
                const std::size_t quote = quote_at(command, i, shell);
                const std::size_t next = quote > 0 && i + quote < command.size() ? quote_at(command, i + quote, shell) : 0;
                if (fish && command[i] == '\\' && i + 1 < command.size() && (command[i + 1] == '\\' || command[i + 1] == '\'')) {
                    word += command[++i];
                } else if (shell == Login::pwsh && next > 0) {
                    word += command.substr(i, quote); // a doubled quote: one
                    i += quote + next - 1;
                } else if (quote > 0) {
                    i += quote - 1;
                    closed = true;
                    break;
                } else {
                    word += command[i];
                }
            }
            if (!closed) return std::nullopt;
        } else if (c == '\\' && shell != Login::pwsh && i + 1 < command.size()) {
            in_word = true;
            word += command[++i];
        } else if (c == ' ') {
            if (in_word) words.push_back(std::exchange(word, {}));
            in_word = false;
        } else {
            in_word = true;
            word += c;
        }
    }
    if (in_word) words.push_back(word);
    return words;
}

// The check run by a server whose login shell is sh, fish or pwsh, its GNU find's -printf reading
// \\, \n and \c (stop printing).
void check_under(test::FakeCommand& c, std::string_view command, Login shell) {
    const auto words = shell_words(command, shell);
    if (!words) {
        c.err = shell == Login::pwsh ? "The string is missing the terminator: '.\n"
                                     : "fish: Unexpected end of string, quotes are not balanced\n";
        c.status = 127;
        return;
    }
    const auto format = std::ranges::find(*words, "-printf");
    REQUIRE(format != words->end());
    REQUIRE(format + 1 != words->end());
    const std::string& f = *(format + 1);
    std::string out;
    for (std::size_t i = 0; i < f.size(); ++i) {
        if (f[i] != '\\' || i + 1 == f.size()) {
            out += f[i];
            continue;
        }
        const char e = f[++i];
        if (e == 'c') break;
        out += e == 'n' ? '\n' : e;
    }
    c.out = {out};
}

} // namespace

TEST_CASE("ssh find: the command: -L, the C locale, raw roots quoted safely, stderr kept") {
    ssh::FindSpec spec;
    spec.roots = {"", "downloads", "/srv/caf‛E9", "-odd", "it's"};
    spec.max_depth = 3;
    CHECK(ssh::find_command(spec) ==
          "env LC_ALL=C find -L . './downloads' '/srv/caf\xE9' './-odd' './it'\\''s' -mindepth 1 -maxdepth 3 "
          "\\( -name '.*' -prune \\) -o \\( -type d -printf 'd\\t%T@\\t%p\\0' -o -type f -printf "
          "'f\\t%s\\t%T@\\t%p\\0' \\)");

    // Name searches filter on the server, words as raw bytes too; one kind or both.
    spec.roots = {""};
    spec.query = make_query("the Matrix it's", false);
    spec.folders = false;
    CHECK(ssh::find_command(spec) ==
          "env LC_ALL=C find -L . -mindepth 1 -maxdepth 3 \\( -name '.*' -prune \\) -o -iname '*the*' "
          "-iname '*matrix*' -iname '*it'\\''s*' -type f -printf 'f\\t%s\\t%T@\\t%p\\0'");
    spec.query = make_query("*.MKV", false);
    CHECK(ssh::find_command(spec).find(" -iname '*.MKV' -type f ") != std::string::npos);
    spec.query = make_query("caf‛E9.mkv", true);
    spec.folders = true;
    spec.files = false;
    CHECK(ssh::find_command(spec) ==
          "env LC_ALL=C find -L . -mindepth 1 -maxdepth 3 \\( -name '.*' -prune \\) -o -name 'caf\xE9.mkv' "
          "-type d -printf 'd\\t%T@\\t%p\\0'");
    // Looking for a dot-name: nothing pruned.
    spec.query = make_query("*.conf", false);
    spec.skip_hidden = true;
    CHECK(ssh::find_command(spec).find("-prune") != std::string::npos);
    spec.query = make_query(".bashrc", false);
    CHECK(ssh::find_command(spec).find("-prune") == std::string::npos);
    CHECK(ssh::find_command(spec).find("2>") == std::string::npos);
}

TEST_CASE("ssh find: the check counts exactly grab-find-ok\\ with exit 0; everything else walks") {
    CHECK(ssh::find_probe_ok == "grab-find-ok\\\n");
    CHECK(ssh::read_find_probe(ended("", 0), ssh::find_probe_ok) == ssh::FindCheck{true, "GNU find"});
    // What the check gave before it held backslashes, as a shell that takes them otherwise could.
    CHECK_FALSE(ssh::read_find_probe(ended("", 0), "grab-find-ok\n").works);
    // A shell that prints a banner first (G61), an SFTP-only account, no find or no env, a
    // restricted shell (a Storage Box), BusyBox or BSD find.
    CHECK(ssh::read_find_probe(ended("", 0), "Welcome to the box\ngrab-find-ok\\\n") ==
          ssh::FindCheck{false, "\"Welcome to the box\", exit 0"});
    CHECK(ssh::read_find_probe(ended("", 0), "") == ssh::FindCheck{false, "nothing, exit 0"});
    CHECK(ssh::read_find_probe(ended("bash: line 1: find: command not found\n", 127), "") ==
          ssh::FindCheck{false, "\"bash: line 1: find: command not found\", exit 127"});
    CHECK(ssh::read_find_probe(ended("", 1), "Command not found. Use 'help' to get a list of available commands.\n") ==
          ssh::FindCheck{false, "\"Command not found. Use 'help' to get a list of available commands.\", exit 1"});
    CHECK(ssh::read_find_probe(ended("find: unrecognized: -printf\n", 1), "") ==
          ssh::FindCheck{false, "\"find: unrecognized: -printf\", exit 1"});
    CHECK_FALSE(ssh::read_find_probe(ended("", 1), ssh::find_probe_ok).works);
    CHECK_FALSE(ssh::read_find_probe(ended("", 0), "grab-find-ok\\").works);
    CHECK_FALSE(ssh::read_find_probe(ended("", 0), "grab-find-ok\\\r\n").works);
    // Killed from outside: asked again after a while, never saved for a week.
    CHECK(ssh::read_find_probe(ended("", -1, "TERM"), "") ==
          ssh::FindCheck{.works = false, .answer = "nothing, ended by SIGTERM", .lasting = false});
    CHECK(ssh::read_find_probe(ended("", 137), "") ==
          ssh::FindCheck{.works = false, .answer = "nothing, exit 137", .lasting = false});
    // A server that sends no exit status does so every time.
    CHECK(ssh::read_find_probe(ended("", -1), "").lasting);
    // An answer is shown safely: control characters and bytes that aren't UTF-8 as rclone_enc does.
    CHECK(ssh::read_find_probe(ended("", 2), "bad\x01\xFF\n").answer == "\"bad␁‛FF\", exit 2");
    // Refused, or no answer in time.
    CHECK(ssh::read_find_probe(failed(ssh::ExecFailure::refused, "the server refused to run the command"), "") ==
          ssh::FindCheck{false, "the server refused to run the command"});
    // No answer in time may pass: not kept for good.
    CHECK(ssh::read_find_probe(failed(ssh::ExecFailure::deadline, "late"), "") ==
          ssh::FindCheck{false, "no answer within 10 s", false});
}

TEST_CASE("ssh find: a login shell that reads backslashes inside single quotes (fish) is walked") {
    // The danger: a folder from the server ending in a backslash (Browse lists it) ends its quotes
    // early under fish, and the rest of the command runs as the shell reads it.
    ssh::FindSpec spec;
    spec.roots = {"x\\"};
    const std::string command = ssh::find_command(spec);
    const auto words = shell_words(command, Login::sh).value_or(std::vector<std::string>{});
    REQUIRE(words.size() > 4);
    CHECK(words[4] == "./x\\");
    CHECK(shell_words(command, Login::fish) != shell_words(command, Login::sh));
    // The check before it held backslashes passed there too.
    test::FakeCommand old;
    check_under(old, "env LC_ALL=C find -L . -maxdepth 0 -printf 'grab-find-ok\\n'", Login::fish);
    CHECK(old.out == std::vector<std::string>{"grab-find-ok\n"});

    Shell sh;
    sh.answer = [](test::FakeCommand& c, std::string_view command) { check_under(c, command, Login::sh); };
    CHECK(ssh::check_find(*sh.pool(), {}).value_or(ssh::FindCheck{}).works);
    Shell fish;
    fish.answer = [](test::FakeCommand& c, std::string_view command) { check_under(c, command, Login::fish); };
    auto pool = fish.pool();
    const auto walked = ssh::check_find(*pool, {}).value_or(ssh::FindCheck{.works = true, .answer = "stopped"});
    CHECK_FALSE(walked.works);
    CHECK(walked.answer == "\"fish: Unexpected end of string, quotes are not balanced\", exit 127");
    const auto way = ssh::choose_lookup(*pool, GrabMethod::probe, "Fish", {});
    REQUIRE(way);
    CHECK_FALSE(way->find);
    CHECK(fish.commands == std::vector<std::string>{std::string(ssh::find_probe)}); // no find ran
}

TEST_CASE("ssh find: a pwsh login shell, whose single quotes also end at U+2018 to U+201B, is walked") {
    // The danger: a folder named x’; touch pwned; ’y ends its quotes under pwsh, and touch runs.
    ssh::FindSpec spec;
    spec.roots = {"x\xE2\x80\x99; touch pwned; \xE2\x80\x99y"};
    const std::string command = ssh::find_command(spec);
    const auto words = shell_words(command, Login::sh).value_or(std::vector<std::string>{});
    REQUIRE(words.size() > 4);
    CHECK(words[4] == "./x\xE2\x80\x99; touch pwned; \xE2\x80\x99y");
    const auto pwsh_words = shell_words(command, Login::pwsh).value_or(std::vector<std::string>{});
    CHECK(std::ranges::find(pwsh_words, "touch") != pwsh_words.end());
    // The check before it passed there: backslashes are plain bytes in pwsh's single quotes.
    test::FakeCommand old;
    check_under(old, R"(env LC_ALL=C find -L . -maxdepth 0 -printf 'grab-find-ok\\\n\c\')", Login::pwsh);
    CHECK(old.out == std::vector<std::string>{std::string(ssh::find_probe_ok)});
    CHECK(old.status == 0);

    // The probe's ’ ends pwsh's string, and its last ' opens one that doesn't close.
    test::FakeCommand sh;
    check_under(sh, ssh::find_probe, Login::sh);
    CHECK(sh.out == std::vector<std::string>{std::string(ssh::find_probe_ok)});
    Shell pwsh;
    pwsh.answer = [](test::FakeCommand& c, std::string_view command) { check_under(c, command, Login::pwsh); };
    auto pool = pwsh.pool();
    const auto walked = ssh::check_find(*pool, {}).value_or(ssh::FindCheck{.works = true, .answer = "stopped"});
    CHECK_FALSE(walked.works);
    const auto way = ssh::choose_lookup(*pool, GrabMethod::probe, "Pwsh", {});
    REQUIRE(way);
    CHECK_FALSE(way->find);
    CHECK(pwsh.commands == std::vector<std::string>{std::string(ssh::find_probe)}); // no find ran
}

TEST_CASE("ssh find: the check runs once per pool, and a stopped one is asked again") {
    Shell s;
    s.answer = [](test::FakeCommand& c, std::string_view) { c.out = {"grab-find-", "ok\\\n"}; };
    auto pool = s.pool();
    auto first = ssh::check_find(*pool, {});
    REQUIRE(first);
    CHECK(first->works);
    CHECK(ssh::check_find(*pool, {}) == first);
    CHECK(s.commands == std::vector<std::string>{std::string(ssh::find_probe)});
    CHECK(s.log == std::vector<std::string>{"start", "eof", "close"});
    CHECK(pool->idle() == 1); // kept for the search that follows
    // SFTP tried first, as a pooled connection has it running (MaxSessions 1 then refuses find).
    CHECK(s.sftp_starts == 1);

    // Stopped before it ran, then while it ran (a server that never answers): not kept.
    Shell slow;
    slow.answer = [](test::FakeCommand& c, std::string_view) { c.forever = true; };
    auto other = slow.pool();
    std::stop_source before;
    before.request_stop();
    CHECK_FALSE(ssh::check_find(*other, before.get_token()));
    CHECK(slow.commands.empty());
    std::stop_source during;
    std::jthread stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        during.request_stop();
    });
    CHECK_FALSE(ssh::check_find(*other, during.get_token()));
    stopper.join();
    CHECK(slow.commands.size() == 1);
    CHECK(other->idle() == 0); // cut short: that connection isn't used again
    slow.answer = [](test::FakeCommand& c, std::string_view) { c.out = {std::string(ssh::find_probe_ok)}; };
    auto again = ssh::check_find(*other, {});
    REQUIRE(again);
    CHECK(again->works);
    CHECK(slow.commands.size() == 2);
}

TEST_CASE("ssh find: a caller waiting for another's check can be stopped, and then asks itself") {
    ssh::Pool::Options o;
    o.idle_timer = false;
    ssh::Pool pool([]() -> std::unique_ptr<ssh::Session> { throw std::logic_error("no logins here"); }, o);
    std::stop_source first_stop;
    std::atomic<bool> running{false};
    std::jthread first([&] {
        auto answer = pool.find_check({}, [&]() -> std::optional<ssh::FindCheck> {
            running = true;
            // Blocked, as a check waiting for a connection or a slow server is, until stopped.
            while (!first_stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return std::nullopt;
        });
        CHECK_FALSE(answer);
    });
    while (!running) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::stop_source second_stop;
    std::jthread stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        second_stop.request_stop();
    });
    const auto at = std::chrono::steady_clock::now();
    int asked = 0;
    auto second = pool.find_check(second_stop.get_token(), [&]() -> std::optional<ssh::FindCheck> {
        ++asked;
        return ssh::FindCheck{.works = true, .answer = "GNU find"};
    });
    CHECK_FALSE(second);
    CHECK(asked == 0);
    CHECK(std::chrono::steady_clock::now() - at < std::chrono::seconds(5));
    stopper.join();

    // The first one's check ends without an answer: a caller waiting then asks itself.
    std::jthread third([&] {
        auto answer = pool.find_check({}, [&]() -> std::optional<ssh::FindCheck> {
            ++asked;
            return ssh::FindCheck{.works = true, .answer = "GNU find"};
        });
        REQUIRE(answer);
        CHECK(answer->works);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(asked == 0); // still waiting for the first
    first_stop.request_stop();
    first.join();
    third.join();
    CHECK(asked == 1);

    // A check that throws lets the next caller ask.
    ssh::Pool other([]() -> std::unique_ptr<ssh::Session> { throw std::logic_error("no logins here"); }, o);
    CHECK_THROWS_AS((void)other.find_check({}, []() -> std::optional<ssh::FindCheck> {
        throw ssh::Error(budget::Failure::other, "no login");
    }), ssh::Error);
    auto after = other.find_check(
        {}, [] { return std::optional(ssh::FindCheck{.works = false, .answer = "nothing, exit 0"}); });
    REQUIRE(after);
    CHECK_FALSE(after->works);
}

TEST_CASE("ssh find: no answer in time is asked again after a while; a definite one is kept") {
    Shell s;
    int asked = 0;
    s.answer = [&](test::FakeCommand& c, std::string_view) {
        ++asked;
        c.out = {std::string(ssh::find_probe_ok)};
    };
    ssh::Pool::Options o;
    o.idle_timer = false;
    o.find_check_expiry = std::chrono::milliseconds(0);
    auto make = [&] {
        return std::make_shared<ssh::Pool>([&s] { return std::make_unique<Shell::Session>(s); }, o);
    };
    auto pool = make();
    // A deadline as the check's own, without waiting 10 s for it.
    auto late = pool->find_check({}, [] {
        return std::optional(ssh::read_find_probe(failed(ssh::ExecFailure::deadline, "late"), ""));
    });
    REQUIRE(late);
    CHECK_FALSE(late->lasting);
    auto again = ssh::check_find(*pool, {});
    REQUIRE(again);
    CHECK(again->works);
    CHECK(again->lasting);
    CHECK(asked == 1);
    CHECK(ssh::check_find(*pool, {}) == again);
    CHECK(asked == 1); // kept

    // Within the expiry, the late answer stands.
    o.find_check_expiry = std::chrono::minutes(5);
    pool = make();
    late = pool->find_check({}, [] {
        return std::optional(ssh::read_find_probe(failed(ssh::ExecFailure::deadline, "late"), ""));
    });
    REQUIRE(late);
    CHECK(ssh::check_find(*pool, {}) == late);
    CHECK(asked == 1);
}

TEST_CASE("ssh find: a lasting answer holds for find_check_trust, a saved one from when it was checked") {
    ssh::Pool::Options o;
    o.idle_timer = false;
    o.find_check_trust = std::chrono::hours(1);
    ssh::Pool pool([]() -> std::unique_ptr<ssh::Session> { throw std::logic_error("no logins here"); }, o);
    int asked = 0;
    const auto now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    auto saved_at = [&](std::int64_t checked) {
        return [&asked, checked] {
            ++asked;
            return std::optional(
                ssh::FindCheck{.works = true, .answer = "GNU find", .lasting = true, .checked = checked, .saved = true});
        };
    };
    // Checked half an hour ago: kept for the rest of the hour.
    REQUIRE(pool.find_check({}, saved_at(now - 1800)));
    auto kept = pool.find_check({}, saved_at(now));
    REQUIRE(kept);
    CHECK(kept->checked == now - 1800);
    CHECK(asked == 1);
    // Forgotten: asked again. Checked two hours ago: past it at once, so the next caller asks.
    pool.forget_find_check();
    REQUIRE(pool.find_check({}, saved_at(now - 7200)));
    REQUIRE(pool.find_check({}, saved_at(now)));
    CHECK(asked == 3);
    REQUIRE(pool.find_check({}, saved_at(now)));
    CHECK(asked == 3);
}

TEST_CASE("ssh find: the check takes an answer from memory, keeps a new lasting one there, never a late one") {
    Shell s;
    s.answer = [](test::FakeCommand& c, std::string_view) { c.out = {std::string(ssh::find_probe_ok)}; };
    std::vector<std::optional<ssh::FindCheck>> saves;
    ssh::FindMemory memory;
    memory.save = [&](const std::optional<ssh::FindCheck>& c) { saves.push_back(c); };
    // Nothing saved: the probe, its answer dated and saved.
    auto pool = s.pool();
    auto asked = ssh::choose_lookup(*pool, GrabMethod::probe, "Main", {}, std::nullopt, memory);
    REQUIRE(asked);
    CHECK(asked->asked);
    CHECK_FALSE(asked->saved);
    REQUIRE(saves.size() == 1);
    REQUIRE(saves[0]);
    CHECK(saves[0]->works);
    CHECK(saves[0]->checked > 0);
    CHECK(asked->checked == saves[0]->checked);
    // Known to the pool now: neither asked nor saved again.
    auto known = ssh::choose_lookup(*pool, GrabMethod::probe, "Main", {}, std::nullopt, memory);
    REQUIRE(known);
    CHECK_FALSE(known->asked);
    CHECK(saves.size() == 1);
    CHECK(s.commands.size() == 1);

    // Saved by an earlier run: a new pool takes it, nothing asked.
    memory.load = [] {
        return std::optional(
            ssh::FindCheck{.works = false, .answer = "nothing, exit 0", .lasting = true, .checked = 1'800'000'000, .saved = true});
    };
    pool = s.pool();
    auto recalled = ssh::choose_lookup(*pool, GrabMethod::probe, "Main", {}, std::nullopt, memory);
    REQUIRE(recalled);
    CHECK_FALSE(recalled->find);
    CHECK(recalled->saved);
    CHECK_FALSE(recalled->asked);
    CHECK(recalled->checked == 1'800'000'000);
    CHECK(s.commands.size() == 1);
    // Asked again: the saved one dropped first, then the new one saved.
    auto again = ssh::recheck_lookup(*pool, GrabMethod::probe, "Main", {}, std::nullopt, memory);
    REQUIRE(again);
    CHECK(again->find);
    CHECK(again->asked);
    REQUIRE(saves.size() == 3);
    CHECK_FALSE(saves[1]);
    REQUIRE(saves[2]);
    CHECK(saves[2]->works);
    CHECK(s.commands.size() == 2);
}

TEST_CASE("ssh find: walk asks nothing; auto walks where find fails; ssh insists, naming the answer") {
    Shell box;
    box.answer = [](test::FakeCommand& c, std::string_view) {
        c.out = {"Command not found. Use 'help' to get a list of available commands.\n"};
        c.status = 1;
    };
    auto pool = box.pool();
    auto walked = ssh::choose_lookup(*pool, GrabMethod::walk, "Box", {});
    REQUIRE(walked);
    CHECK_FALSE(walked->find);
    CHECK(walked->answer.empty());
    CHECK(box.commands.empty());

    auto probed = ssh::choose_lookup(*pool, GrabMethod::probe, "Box", {});
    REQUIRE(probed);
    CHECK_FALSE(probed->find);
    CHECK(probed->answer.find("Command not found") != std::string::npos);

    auto insisted = ssh::choose_lookup(*pool, GrabMethod::find, "Box", {});
    REQUIRE_FALSE(insisted);
    CHECK(insisted.error().find("Box has no GNU find") == 0);
    CHECK(insisted.error().find("\"Command not found. Use 'help'") != std::string::npos);
    CHECK(insisted.error().find("find = auto or walk in [Box]") != std::string::npos);
    CHECK(box.commands.size() == 1); // asked once

    Shell full;
    full.answer = [](test::FakeCommand& c, std::string_view) { c.out = {std::string(ssh::find_probe_ok)}; };
    auto good = full.pool();
    for (const auto method : {GrabMethod::probe, GrabMethod::find}) {
        auto chosen = ssh::choose_lookup(*good, method, "Main", {});
        REQUIRE(chosen);
        CHECK(chosen->find);
        CHECK(chosen->answer == "GNU find");
    }
}

TEST_CASE("ssh find: records parsed as they stream, paths in grab's form under their roots, no nameless hits") {
    Shell s;
    s.answer = [](test::FakeCommand& c, std::string_view) {
        c.out = {"d\t1700000000.5\t./downloads/tv\0f\t12"s,
                 "34\t1700000001.0\t./downloads/tv/caf\xE9.mkv\0f\t5\t1700000002\t/srv/x\x01y/a.txt\0"s,
                 // Nothing to save these under (G15).
                 "f\t1\t1\t/\0d\t1\t.\0d\t1\t./..\0f\t1\t1\t./downloads/..\0"s,
                 "f\t3\t1700000003\t./notes.txt\0f\t9\t1700000004\t./la"s, "st.txt"s};
    };
    auto pool = s.pool();
    ssh::FindSpec spec;
    spec.roots = {"", "downloads", "/srv"};
    std::vector<std::size_t> progress;
    ssh::FindOptions o;
    o.progress = [&](std::size_t n) { progress.push_back(n); };
    auto found = ssh::run_find(*pool, spec, o);
    REQUIRE_MESSAGE(found, why(found));
    CHECK(paths_of(found->entries) == std::vector<std::string>{"downloads/tv", "downloads/tv/caf‛E9.mkv",
                                                               "/srv/x␁y/a.txt", "notes.txt", "last.txt"});
    CHECK(found->entries[0].dir);
    CHECK(found->entries[0].modified == 1700000000);
    CHECK(found->entries[1].size == 1234);
    CHECK(found->entries[4].size == 9);
    CHECK(found->notes.empty());
    REQUIRE_FALSE(progress.empty());
    CHECK(progress.back() == 8); // NUL-ended records
    REQUIRE(s.commands.size() == 1);
    CHECK(s.commands[0] == ssh::find_command(spec));
    CHECK(pool->idle() == 1);
    // Back to the server's bytes: what downloads hand the server.
    CHECK(rclone_enc::raw_path(found->entries[1].path) == "downloads/tv/caf\xE9.mkv");
}

TEST_CASE("ssh find: a name with a tab or a line break comes whole, in grab's form") {
    Shell s;
    // The path is the record's last field: a tab in it is part of the name, and records end at NUL,
    // not at a line break.
    s.answer = [](test::FakeCommand& c, std::string_view) {
        c.out = {"f\t1\t1700000000\t./a\tb.txt\0d\t1700000000\t./nl\nname\0f\t22\t1700000000\t./nl\nname/x\t1\t2\0"s};
    };
    auto pool = s.pool();
    ssh::FindSpec spec;
    spec.roots = {""};
    auto found = ssh::run_find(*pool, spec, {});
    REQUIRE_MESSAGE(found, why(found));
    CHECK(paths_of(found->entries) == std::vector<std::string>{"a␉b.txt", "nl␊name", "nl␊name/x␉1␉2"});
    CHECK(found->entries[2].size == 22);
    CHECK(rclone_enc::raw_path(found->entries[2].path) == "nl\nname/x\t1\t2");
}

TEST_CASE("ssh find: output that isn't find's records fails it, as being about find") {
    ssh::FindSpec spec;
    spec.roots = {""};
    Shell s;
    auto pool = s.pool();
    // A malformed record among good ones: find is stopped there.
    s.answer = [](test::FakeCommand& c, std::string_view) { c.out = {"f\t1\t1\t./a.txt\0garbage\0f\t1\t1\t./b.txt\0"s}; };
    auto found = ssh::run_find(*pool, spec, {});
    REQUIRE_FALSE(found);
    CHECK(found.error().message == "the server's find printed something that isn't a find record: \"garbage\"");
    CHECK(found.error().about_find);
    // A banner before the first record (review G61), and a restricted shell's answer with no NUL.
    s.answer = [](test::FakeCommand& c, std::string_view) { c.out = {"Welcome to the box\nf\t1\t1\t./a.txt\0"s}; };
    found = ssh::run_find(*pool, spec, {});
    REQUIRE_FALSE(found);
    CHECK(found.error().message == "the server's find printed something that isn't a find record: \"Welcome to the box\"");
    CHECK(found.error().about_find);
    s.answer = [](test::FakeCommand& c, std::string_view) {
        c.out = {"Command not found. Use 'help' to get a list of available commands.\n"};
        c.status = 1;
    };
    found = ssh::run_find(*pool, spec, {});
    REQUIRE_FALSE(found);
    CHECK(found.error().message.find("isn't a find record: \"Command not found.") != std::string::npos);
    CHECK(found.error().about_find);
}

TEST_CASE("ssh find: stderr: unreadable folders and loops are notes; anything else, or every root, an error") {
    ssh::FindSpec spec;
    spec.roots = {"", "downloads"};
    const std::string err = "find: './den\\351 it\\'s': Permission denied\n"
                            "find: File system loop detected; the following directory is part of the cycle: "
                            "'./downloads/again/up'\n"
                            "find: './self': Too many levels of symbolic links\n"
                            "find: File system loop detected; './a/up' is part of the same file system loop as '.'.\n";
    auto notes = ssh::read_find_errors(spec, err, 1);
    REQUIRE_MESSAGE(notes, why(notes));
    CHECK(*notes == std::vector<std::string>{"den‛E9 it's: permission denied", "downloads/again/up: a link loop, skipped",
                                             "self: a link loop, skipped", "a/up: a link loop, skipped"});
    // One root missing: a note; every root failing: an error, though not about find.
    notes = ssh::read_find_errors(spec, "find: './downloads': No such file or directory\n", 1);
    REQUIRE(notes);
    CHECK(*notes == std::vector<std::string>{"downloads: no such file or folder"});
    notes = ssh::read_find_errors(spec, "find: '.': Permission denied\nfind: './downloads': No such file or directory\n", 1);
    REQUIRE_FALSE(notes);
    CHECK(notes.error().message == "cannot search ~: permission denied; downloads: no such file or folder");
    CHECK_FALSE(notes.error().about_find);
    // Anything else fails the search with that line: about find itself when the line names no
    // path, as a find that isn't GNU's or a shell without one says it.
    notes = ssh::read_find_errors(spec, "find: './x': Permission denied\nfind: unknown predicate `-printf'\n", 1);
    REQUIRE_FALSE(notes);
    CHECK(notes.error().message == "find on the server failed: find: unknown predicate `-printf'");
    CHECK(notes.error().about_find);
    notes = ssh::read_find_errors(spec, "find: './x': Input/output error\n", 1);
    REQUIRE_FALSE(notes);
    CHECK(notes.error().message == "find on the server failed: find: './x': Input/output error");
    CHECK_FALSE(notes.error().about_find);
    notes = ssh::read_find_errors(spec, "Segmentation fault\n", 139);
    REQUIRE_FALSE(notes);
    CHECK(notes.error().message == "find on the server failed (exit 139): Segmentation fault");
    CHECK_FALSE(notes.error().about_find); // ended by a signal
    // Statuses GNU find doesn't give: no find there, a command the shell can't run, no status.
    for (const int status : {127, 126, 2, -1}) {
        notes = ssh::read_find_errors(spec, "bash: line 1: env: command not found\n", status);
        REQUIRE_FALSE(notes);
        CHECK(notes.error().about_find);
    }
    CHECK(notes.error().message == "find on the server failed (no exit status): bash: line 1: env: command not found");
    notes = ssh::read_find_errors(spec, "", 127);
    REQUIRE_FALSE(notes);
    CHECK(notes.error().message == "find on the server failed (exit 127)");
    CHECK(notes.error().about_find);
    // Exit 0: the results are whole, a warning is a note.
    notes = ssh::read_find_errors(spec, "", 0);
    REQUIRE(notes);
    CHECK(notes->empty());
    notes = ssh::read_find_errors(spec, "find: warning: something odd\n", 0);
    REQUIRE(notes);
    CHECK(*notes == std::vector<std::string>{"find: warning: something odd"});
    // Absolute roots in grab's form.
    spec.roots = {"/srv/caf‛E9"};
    notes = ssh::read_find_errors(spec, "find: '/srv/caf\\351/sub': Permission denied\n", 1);
    REQUIRE(notes);
    CHECK(*notes == std::vector<std::string>{"/srv/caf‛E9/sub: permission denied"});

    // stderr cut short (past ExecOptions::err_limit), most likely inside a line: that line is left
    // out and a note says so, rather than failing the search for a line it can't read.
    spec.roots = {""};
    const std::string cut_err = "find: './a': Permission denied\nfind: './b': Permiss";
    notes = ssh::read_find_errors(spec, cut_err, 1, true);
    REQUIRE_MESSAGE(notes, why(notes));
    CHECK(*notes == std::vector<std::string>{"a: permission denied", "(more find messages not kept)"});
    notes = ssh::read_find_errors(spec, cut_err, 1);
    REQUIRE_FALSE(notes);
    CHECK(notes.error().message == "find on the server failed: find: './b': Permiss");
    CHECK_FALSE(notes.error().about_find);
    notes = ssh::read_find_errors(spec, "find: './a': Permiss", 0, true);
    REQUIRE(notes);
    CHECK(*notes == std::vector<std::string>{"(more find messages not kept)"});
}

TEST_CASE("ssh find: run_find's notes, refusals, signals, and its caps drop the connection") {
    ssh::FindSpec spec;
    spec.roots = {""};
    Shell s;
    auto pool = s.pool();

    s.answer = [](test::FakeCommand& c, std::string_view) {
        c.out = {"f\t1\t1\t./a.txt\0"s};
        c.err = "find: './x': Permission denied\n";
        c.status = 1;
    };
    auto found = ssh::run_find(*pool, spec, {});
    REQUIRE_MESSAGE(found, why(found));
    CHECK(paths_of(found->entries) == std::vector<std::string>{"a.txt"});
    CHECK(found->notes == std::vector<std::string>{"x: permission denied"});
    CHECK(pool->idle() == 1);

    // A refused command leaves the connection as it was; it's about find (an account that may
    // run no commands now).
    s.answer = [](test::FakeCommand& c, std::string_view) { c.refuse = true; };
    found = ssh::run_find(*pool, spec, {});
    REQUIRE_FALSE(found);
    CHECK(found.error().message == "find on the server failed: the server refused to run the command");
    CHECK(found.error().about_find);
    CHECK(pool->idle() == 1);

    s.answer = [](test::FakeCommand& c, std::string_view) { c.signal = "KILL"; };
    found = ssh::run_find(*pool, spec, {});
    REQUIRE_FALSE(found);
    CHECK(found.error().message == "find on the server was ended by SIGKILL");
    CHECK_FALSE(found.error().about_find);

    // Past the entry cap: told to end, and the connection isn't used again.
    s.answer = [](test::FakeCommand& c, std::string_view) {
        for (int i = 0; i < 20; ++i) c.out.push_back("f\t1\t1\t./f" + std::to_string(i) + '\0');
    };
    s.log.clear();
    ssh::FindOptions capped;
    capped.max_entries = 5;
    found = ssh::run_find(*pool, spec, capped);
    REQUIRE_FALSE(found);
    CHECK(found.error().message == "listing too large: more than 5 entries");
    CHECK_FALSE(found.error().about_find);
    CHECK(std::ranges::count(s.log, "TERM") == 1);
    CHECK(pool->idle() == 0);

    // Past the byte cap of the paths kept (review G168): "f0" to "f9" are 2 bytes each.
    s.log.clear();
    ssh::FindOptions bytes_capped;
    bytes_capped.max_bytes = 15;
    found = ssh::run_find(*pool, spec, bytes_capped);
    REQUIRE_FALSE(found);
    CHECK(found.error().message == "listing too large: more than 15 B of paths");
    CHECK_FALSE(found.error().about_find);
    CHECK(std::ranges::count(s.log, "TERM") == 1);
    CHECK(pool->idle() == 0);

    // A stderr past err_limit: the search stands, with a note.
    s.answer = [](test::FakeCommand& c, std::string_view) {
        c.out = {"f\t1\t1\t./a.txt\0"s};
        for (int i = 0; i < 5000; ++i) c.err += std::format("find: './d{}': Permission denied\n", i);
        c.status = 1;
    };
    found = ssh::run_find(*pool, spec, {});
    REQUIRE_MESSAGE(found, why(found));
    CHECK(paths_of(found->entries) == std::vector<std::string>{"a.txt"});
    REQUIRE(found->notes.size() > 100);
    CHECK(found->notes.back() == "(more find messages not kept)");

    // Endless output without a record's end.
    s.answer = [](test::FakeCommand& c, std::string_view) { c.endless = true; };
    found = ssh::run_find(*pool, spec, {});
    REQUIRE_FALSE(found);
    CHECK(found.error().message == "the server's find printed a path of more than 64 KiB");
    CHECK_FALSE(found.error().about_find);
    CHECK(pool->idle() == 0);

    // Stopped: nothing, and no error.
    std::stop_source stop;
    stop.request_stop();
    ssh::FindOptions stopped;
    stopped.stop = stop.get_token();
    found = ssh::run_find(*pool, spec, stopped);
    REQUIRE(found);
    CHECK(found->entries.empty());
}

TEST_CASE("find output for lists: no hit without a name to save it under (G15)") {
    const auto entries = file_list::parse_find("f\t1\t1\t/\0d\t1\t.\0d\t1\t./..\0f\t2\t1\t./a/b.txt\0"s);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].path == "a/b.txt");
    CHECK_FALSE(file_list::parse_find_record("d\t1\t./"));
    const auto x = file_list::parse_find_record("d\t1\t./x");
    REQUIRE(x);
    CHECK(x->path == "x");
}

TEST_CASE("a server path of raw bytes in grab's form, part by part") {
    CHECK(rclone_enc::from_raw_path("/srv/caf\xE9/x\x01y/f.bin") == "/srv/caf‛E9/x␁y/f.bin");
    CHECK(rclone_enc::from_raw_path("") == "");
    for (const std::string raw : {"/srv/caf\xE9/a b", "rel/x\x01y", "plain"}) {
        CHECK_MESSAGE(rclone_enc::raw_path(rclone_enc::from_raw_path(raw)) == raw, raw);
    }
}

namespace {

// Live tests run against a real server only when GRAB_TEST_SSH=user@host:port and
// GRAB_TEST_KNOWN_HOSTS name one (logging in through ssh-agent), as in test_ssh.cpp.
std::optional<ssh::Target> live_target() {
    auto where = util::getenv_utf8("GRAB_TEST_SSH");
    auto known = util::getenv_utf8("GRAB_TEST_KNOWN_HOSTS");
    if (!where || !known) return std::nullopt;
    const std::string& w = *where;
    ssh::Target t;
    t.user = w.substr(0, w.find('@'));
    t.host = w.substr(w.find('@') + 1, w.rfind(':') - w.find('@') - 1);
    t.port = std::stoi(w.substr(w.rfind(':') + 1));
    t.known_hosts = util::path_from_utf8(*known);
    t.agent = true;
    return t;
}

std::shared_ptr<ssh::Pool> live_pool(const ssh::Target& t, int limit) {
    ssh::Pool::Options o;
    o.limit = limit;
    o.logins_at_once = limit;
    return std::make_shared<ssh::Pool>([t] { return ssh::open_session(t); }, o);
}

// A command on one of the pool's connections: its exit status, and stdout and stderr together.
std::pair<int, std::string> live_run(ssh::Pool& pool, const std::string& command) {
    auto lease = pool.acquire();
    REQUIRE(lease);
    std::string out;
    ssh::ExecOptions o;
    o.out = [&](std::string_view piece) {
        out += piece;
        return true;
    };
    auto ran = (*lease)->exec(command, o);
    REQUIRE_MESSAGE(ran.has_value(), (ran ? "" : ran.error().message));
    return {ran->status, out + ran->err};
}

// "d <path>" or "f <path> <size>", for comparing sets.
std::set<std::string> keys(const std::vector<file_list::Entry>& entries) {
    std::set<std::string> out;
    for (const auto& e : entries) out.insert(e.dir ? "d " + e.path : std::format("f {} {}", e.path, e.size.value_or(0)));
    return out;
}
std::set<std::string> keys(const ssh::Walk& walk) {
    std::set<std::string> out;
    for (const auto& e : walk.entries) out.insert(e.dir ? "d " + e.path : std::format("f {} {}", e.path, e.size));
    return out;
}

} // namespace

// find and the walk on a /tmp tree of odd names and links, made for the test and deleted after:
// both give the same entries, except what find -L lists twice behind a link into the tree, and
// both say what they couldn't read. At most 2 logins.
TEST_CASE("ssh find: live, find and the walk agree on odd names and links (GRAB_TEST_SSH)") {
    auto t = live_target();
    if (!t) {
        MESSAGE("skipped: GRAB_TEST_SSH / GRAB_TEST_KNOWN_HOSTS not set");
        return;
    }
    auto pool = live_pool(*t, 2);
    const std::string base = std::format("/tmp/grab-p4-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string tree = base + "/tree";
    struct Cleanup {
        ssh::Pool& pool;
        std::string base;
        ~Cleanup() {
            try {
                if (auto lease = pool.acquire()) {
                    [[maybe_unused]] auto gone = (*lease)->exec("chmod -R u+rwx '" + base + "'; rm -rf '" + base + "'", {});
                }
            } catch (const std::exception& e) {
                std::fprintf(stderr, "couldn't delete %s on the server: %s\n", base.c_str(), e.what());
            }
        }
    } cleanup{.pool = *pool, .base = base};
    const std::string make =
        "set -e; umask 022; T='" + tree + "'; mkdir -p \"$T\" '" + base + "/outside'; cd \"$T\"; "
        "echo plain > plain.txt; printf 1234 > \"$(printf 'caf\\351.txt')\"; echo x > \"$(printf 'nl\\nname.txt')\"; "
        "echo x > \"$(printf 'x\\001y')\"; echo x > \"$(printf '\\342\\200\\233quote.txt')\"; echo x > \"it's\"; "
        "mkdir .hidden; echo x > .hidden/inside.txt; echo x > .dot.txt; "
        "mkdir -p a/b/c/d; echo deep > a/b/c/d/deep.txt; echo x > a/one.txt; "
        "mkdir locked; echo x > locked/secret.txt; chmod 000 locked; "
        "echo far > '" + base + "/outside/far.txt'; "
        "mkdir lnk; ln -s ../plain.txt lnk/tofile; ln -s '" + base + "/outside' lnk/outer; ln -s ../a lnk/inner; "
        "ln -s .. lnk/loop; ln -s nowhere lnk/broken; ln -s self lnk/self; "
        "ln -s \"$T/a\" '" + base + "/rootlink'";
    auto made = live_run(*pool, make);
    REQUIRE_MESSAGE(made.first == 0, made.second);

    auto check = ssh::check_find(*pool, {});
    REQUIRE(check);
    MESSAGE("the find check: " << check->answer);
    REQUIRE(check->works);

    ssh::FindSpec spec;
    spec.roots = {tree};
    spec.max_depth = 4;
    auto found = ssh::run_find(*pool, spec, {});
    REQUIRE_MESSAGE(found, why(found));
    auto walked = ssh::walk_roots(*pool, spec.roots,
                                  ssh::WalkOptions{.connections = 2, .folders = true, .max_depth = 4, .skip_hidden = true}, {});
    REQUIRE_MESSAGE(walked, walked.error_or(""));
    const auto by_find = keys(found->entries);
    const auto by_walk = keys(*walked);
    MESSAGE(by_find.size() << " entries by find, " << by_walk.size() << " by the walk");
    for (const auto& n : found->notes) MESSAGE("find note: " << n);
    for (const auto& n : walked->unreadable) MESSAGE("walk note: " << n);

    // Names in grab's form either way; dot-names and what's too deep left out.
    CHECK(by_walk.contains("f " + tree + "/caf‛E9.txt 4"));
    CHECK(by_walk.contains("f " + tree + "/nl␊name.txt 2"));
    CHECK(by_walk.contains("f " + tree + "/x␁y 2"));
    CHECK(by_walk.contains("f " + tree + "/" + rclone_enc::from_raw_name("‛quote.txt") + " 2"));
    CHECK(by_walk.contains("f " + tree + "/it's 2"));
    CHECK(by_walk.contains("f " + tree + "/lnk/tofile 6"));
    CHECK(by_walk.contains("f " + tree + "/lnk/outer/far.txt 4"));
    CHECK(by_walk.contains("d " + tree + "/a/b/c/d")); // depth 4
    CHECK_FALSE(by_walk.contains("f " + tree + "/a/b/c/d/deep.txt 5"));
    for (const auto& k : by_walk) CHECK_MESSAGE(k.find("/.") == std::string::npos, k);
    // Everything walked, find found too; find's extra entries are behind the link into the tree.
    for (const auto& k : by_walk) CHECK_MESSAGE(by_find.contains(k), "only walked: ", k);
    for (const auto& k : by_find) {
        if (!by_walk.contains(k)) CHECK_MESSAGE(k.find(tree + "/lnk/inner") != std::string::npos, "only found: ", k);
    }
    // What couldn't be read, by both; find's link loops too.
    CHECK(std::ranges::count(found->notes, tree + "/locked: permission denied") == 1);
    CHECK(std::ranges::count(walked->unreadable, tree + "/locked: permission denied") == 1);
    CHECK(std::ranges::count(found->notes, tree + "/lnk/self: a link loop, skipped") == 1);
    CHECK(std::ranges::count(found->notes, tree + "/lnk/loop: a link loop, skipped") == 1);

    // A root that is a link, and one that isn't there.
    spec.roots = {base + "/rootlink", base + "/missing"};
    found = ssh::run_find(*pool, spec, {});
    REQUIRE_MESSAGE(found, why(found));
    walked = ssh::walk_roots(*pool, spec.roots, ssh::WalkOptions{.connections = 2, .folders = true, .max_depth = 4}, {});
    REQUIRE_MESSAGE(walked, walked.error_or(""));
    CHECK(keys(found->entries) == keys(*walked));
    CHECK(keys(*walked).contains("f " + base + "/rootlink/b/c/d/deep.txt 5"));
    CHECK(found->notes == std::vector<std::string>{base + "/missing: no such file or folder"});
    CHECK(walked->unreadable == std::vector<std::string>{base + "/missing: no such file or folder"});
}

// Timings of the find check, find and the walk (GRAB_TEST_WALK_IN_FLIGHT folders per connection,
// the walk's default if unset; with GRAB_TEST_WALK_OLD=1 also a folder at a time, as before) on a
// tree of the server, read only.
// GRAB_TEST_WALK_ROOT names it, GRAB_TEST_WALK_DEPTH the depth (0 or unset: all),
// GRAB_TEST_WALK_DUMP a folder for each method's paths relative to the root, sorted, folders
// ending in '/' (as `rclone lsf -R` prints them). Dot-names left out, as lists leave them out. At
// most 4 logins.
TEST_CASE("ssh find: live timings of find and the walk on a tree (GRAB_TEST_WALK_ROOT)") {
    auto t = live_target();
    auto root = util::getenv_utf8("GRAB_TEST_WALK_ROOT");
    if (!t || !root) {
        MESSAGE("skipped: GRAB_TEST_SSH / GRAB_TEST_KNOWN_HOSTS / GRAB_TEST_WALK_ROOT not set");
        return;
    }
    const int depth = std::stoi(util::getenv_utf8("GRAB_TEST_WALK_DEPTH").value_or("0"));
    const auto dump = util::getenv_utf8("GRAB_TEST_WALK_DUMP");
    using Clock = std::chrono::steady_clock;
    auto seconds = [](Clock::duration d) { return std::chrono::duration<double>(d).count(); };
    auto write = [&](const std::string& name, std::vector<std::string> lines) {
        if (!dump) return;
        std::ranges::sort(lines);
        std::string text;
        for (const auto& l : lines) text += l + "\n";
        REQUIRE(util::write_file_atomic(util::path_from_utf8(*dump) / util::path_from_utf8(name), text));
    };
    auto relative = [&](const std::string& path, bool dir) {
        std::string rel = path.substr(root->size() + 1);
        return dir ? rel + "/" : rel;
    };
    auto pool = live_pool(*t, 4);

    auto started = Clock::now();
    auto check = ssh::check_find(*pool, {});
    REQUIRE(check);
    MESSAGE("check: " << check->answer << " in " << seconds(Clock::now() - started) << " s (with a login)");

    ssh::FindSpec spec;
    spec.roots = {*root};
    spec.max_depth = depth > 0 ? depth : 1000;
    started = Clock::now();
    auto found = ssh::run_find(*pool, spec, {});
    REQUIRE_MESSAGE(found, why(found));
    MESSAGE("find: " << found->entries.size() << " entries in " << seconds(Clock::now() - started) << " s, "
                     << found->notes.size() << " notes");
    std::vector<std::string> lines;
    for (const auto& e : found->entries) lines.push_back(relative(e.path, e.dir));
    write("find.txt", std::move(lines));

    auto timed_walk = [&](int in_flight, const std::string& name) {
        ssh::WalkOptions o{.connections = 4, .folders = true, .max_depth = depth, .skip_hidden = true, .in_flight = in_flight};
        const auto at = Clock::now();
        auto w = ssh::walk_roots(*pool, spec.roots, o, {});
        REQUIRE_MESSAGE(w, w.error_or(""));
        MESSAGE(name << ": " << w->entries.size() << " entries in " << seconds(Clock::now() - at) << " s, "
                     << w->unreadable.size() << " unreadable");
        std::vector<std::string> walked;
        for (const auto& e : w->entries) walked.push_back(relative(e.path, e.dir));
        write(name + ".txt", std::move(walked));
    };
    const auto in_flight_set = util::getenv_utf8("GRAB_TEST_WALK_IN_FLIGHT");
    const int in_flight = in_flight_set ? std::stoi(*in_flight_set) : ssh::WalkOptions{}.in_flight;
    timed_walk(in_flight, "walk");
    timed_walk(in_flight, "walk-again");
    if (util::getenv_utf8("GRAB_TEST_WALK_OLD")) timed_walk(1, "walk-one-at-a-time");
}
