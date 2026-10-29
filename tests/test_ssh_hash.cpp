#include <doctest/doctest.h>

// The server's MD5 of downloaded files through grab's own SSH client (ssh_hash.hpp; phase 5): the
// probe's answers, rclone.conf's keys, commands, batches and the pairing of md5sum's records, on
// an in-memory server whose md5sum runs as GNU's (or BusyBox's) does under a POSIX shell, a
// Windows shell or a Storage Box's.

#include "config.hpp"
#include "conn_budget.hpp"
#include "fake_command.hpp"
#include "fake_md5sum.hpp"
#include "live_import.hpp"
#include "process.hpp"
#include "quote.hpp"
#include "rclone_encoding.hpp"
#include "server_conf.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_hash.hpp"
#include "ssh_pool.hpp"
#include "ssh_walk.hpp"
#include "util.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace grab;
using namespace std::string_literals;
using namespace std::chrono_literals;

namespace {

using test::Box;
using test::is_probe;
using test::md5_hex;

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

ssh::HashFailure kind_of(const ssh::HashResult& r) { return r ? ssh::HashFailure::unpaired : r.error().kind; }
std::string why_of(const ssh::HashResult& r) { return r ? std::string() : r.error().why; }

} // namespace

TEST_CASE("ssh hash: the probe counts only md5sum's exact record for empty stdin") {
    const std::string nothing = "d41d8cd98f00b204e9800998ecf8427e";
    auto z = ssh::read_hash_probe(ended("", 0), nothing + "  -\0"s, true);
    CHECK(z.works);
    CHECK(z.z);
    CHECK(z.answer == "md5sum -z");
    auto plain = ssh::read_hash_probe(ended("", 0), nothing + "  -\n", false);
    CHECK(plain.works);
    CHECK_FALSE(plain.z);
    // The wrong end, a banner first, something after, a wrong exit.
    CHECK_FALSE(ssh::read_hash_probe(ended("", 0), nothing + "  -\n", true).works);
    CHECK_FALSE(ssh::read_hash_probe(ended("", 0), "Welcome to the box\n" + nothing + "  -\0"s, true).works);
    CHECK_FALSE(ssh::read_hash_probe(ended("", 0), nothing + "  -\0more"s, true).works);
    CHECK_FALSE(ssh::read_hash_probe(ended("", 1), nothing + "  -\0"s, true).works);
    CHECK_FALSE(ssh::read_hash_probe(ended("", 0), nothing + " *-\0"s, true).works);
    // A Windows shell keeps the backslash: md5sum (Git's, Cygwin's) looks for a file "\-".
    auto windows = ssh::read_hash_probe(ended("md5sum: '\\-': No such file or directory\n", 1), "", true);
    CHECK(windows == ssh::HashCheck{.works = false, .z = false, .home = {},
                                    .answer = "\"md5sum: '\\-': No such file or directory\", exit 1",
                                    .failure = ssh::HashFailure::no_checksums, .lasting = true});
    // A Storage Box without the command; no md5sum at all; BusyBox without -z.
    auto box = ssh::read_hash_probe(ended("Command not found. Use 'help' to get a list of available commands.\n", 8), "", true);
    CHECK(box.answer == "\"Command not found. Use 'help' to get a list of available commands.\", exit 8");
    CHECK(box.lasting);
    CHECK(ssh::read_hash_probe(ended("zsh:1: command not found: md5sum\n", 127), "", true).answer ==
          "\"zsh:1: command not found: md5sum\", exit 127");
    CHECK(ssh::read_hash_probe(ended("", 0), "", true).answer == "nothing, exit 0");
    // Refused, late or killed: may pass.
    auto refused = ssh::read_hash_probe(failed(ssh::ExecFailure::refused, "the server refused to run the command"), "", true);
    CHECK(refused.failure == ssh::HashFailure::refused);
    CHECK_FALSE(refused.lasting);
    auto late = ssh::read_hash_probe(failed(ssh::ExecFailure::deadline, "late"), "", true);
    CHECK(late == ssh::HashCheck{.works = false, .z = false, .home = {}, .answer = "no answer within 10 s",
                                 .failure = ssh::HashFailure::deadline, .lasting = false});
    CHECK_FALSE(ssh::read_hash_probe(ended("", -1, "TERM"), "", true).lasting);
    CHECK_FALSE(ssh::read_hash_probe(ended("", 137), "", true).lasting);
    CHECK(ssh::read_hash_probe(failed(ssh::ExecFailure::too_much, "big"), "", true).lasting);
    // Shown safely.
    CHECK(ssh::read_hash_probe(ended("", 2), "bad\x01\xFF\n", true).answer == "\"bad␁‛FF\", exit 2");
}

TEST_CASE("ssh hash: grab.conf's keys keep grab's check from servers it can't hash the same way") {
    RcloneRemote r;
    CHECK_FALSE(ssh::hash_blocked(r));
    r.shell_type = "unix";
    r.md5sum_command = "md5sum";
    CHECK_FALSE(ssh::hash_blocked(r));
    for (const char* shell : {"cmd", "PowerShell", "none"}) {
        r.shell_type = shell;
        const auto b = ssh::hash_blocked(r).value_or(ssh::HashFailed{});
        CHECK(b.kind == ssh::HashFailure::no_checksums);
        CHECK(b.why.find("shell_type") != std::string::npos);
    }
    r.shell_type = "unix";
    r.md5sum_command = "none";
    CHECK(ssh::hash_blocked(r) == ssh::HashFailed{.kind = ssh::HashFailure::no_checksums,
                                                  .why = "the server can't compute checksums (md5sum_command = none in grab.conf)"});
    r.md5sum_command = "md5 -r";
    CHECK(ssh::hash_blocked(r) == ssh::HashFailed{.kind = ssh::HashFailure::unsupported,
                                                  .why = "md5sum_command in grab.conf isn't supported by grab's check yet"});
    r.md5sum_command.reset();
    r.path_override = "@/volume1";
    CHECK(ssh::hash_blocked(r) == ssh::HashFailed{.kind = ssh::HashFailure::unsupported,
                                                  .why = "path_override in grab.conf isn't supported by grab's check yet"});
}

TEST_CASE("ssh hash: rclone's shell probe and sum commands, read as rclone reads them (phase 7)") {
    CHECK(ssh::read_shell_answer(ended("", 0), "%ComSpec%\n") == "unix");
    CHECK(ssh::read_shell_answer(ended("", 0), "${ShellId}C:\\WINDOWS\\system32\\cmd.exe\r\n") == "cmd");
    CHECK(ssh::read_shell_answer(ended("", 0), "Microsoft.PowerShell%ComSpec%\r\n") == "powershell");
    CHECK(ssh::read_shell_answer(ended("", 0), "an unforeseen answer\n") == "unix"); // as rclone takes it
    // It ran and failed: a shell without echo, as rclone says.
    CHECK(ssh::read_shell_answer(ended("Command not found.\n", 8), "") == "none");
    // Nothing lasting learned: grab never guesses.
    CHECK_FALSE(ssh::read_shell_answer(failed(ssh::ExecFailure::refused, "refused"), "").has_value());
    CHECK_FALSE(ssh::read_shell_answer(failed(ssh::ExecFailure::deadline, "late"), "").has_value());
    CHECK_FALSE(ssh::read_shell_answer(failed(ssh::ExecFailure::stopped, "stopped"), "").has_value());
    CHECK_FALSE(ssh::read_shell_answer(ended("", -1, "TERM"), "").has_value());

    const std::string md5 = "d41d8cd98f00b204e9800998ecf8427e";
    CHECK(ssh::read_sum_answer(ended("", 0), md5 + "  -\n", md5) == true);
    CHECK(ssh::read_sum_answer(ended("", 0), md5 + "\n", md5) == true);                          // BSD md5 -r
    CHECK(ssh::read_sum_answer(ended("", 0), "\\D41D8CD98F00B204E9800998ECF8427E  -\n", md5) == true); // GNU's '\', case
    CHECK(ssh::read_sum_answer(ended("", 0), "Welcome\n" + md5 + "  -\n", md5) == false);
    CHECK(ssh::read_sum_answer(ended("not found", 127), "", md5) == false);
    CHECK_FALSE(ssh::read_sum_answer(failed(ssh::ExecFailure::refused, "refused"), "", md5).has_value());
}

TEST_CASE("ssh hash: what grab.conf doesn't say is asked once, over grab's own connection (phase 7)") {
    const std::string md5 = "d41d8cd98f00b204e9800998ecf8427e";
    const std::string sha1 = "da39a3ee5e6b4b0d3255bfef95601890afd80709";
    Box b;
    // A POSIX shell without md5sum but with BSD's md5, and GNU's sha1sum.
    b.also = [&](test::FakeCommand& c, std::string_view command) {
        c.err.clear();
        c.out.clear();
        c.status = 0;
        if (command == ssh::shell_probe) c.out = {"%ComSpec%\n"};
        else if (command == "md5 -r") c.out = {md5 + "\n"};
        else if (command == "sha1sum") c.out = {sha1 + "  -\n"};
        else c.status = 127;
    };
    auto pool = b.pool(1);
    const auto learned = ssh::learn_commands(*pool, {}, {});
    CHECK(learned == ssh::ServerCommands{.shell_type = "unix", .md5sum_command = "md5 -r", .sha1sum_command = "sha1sum"});
    CHECK(b.commands == std::vector<std::string>{std::string(ssh::shell_probe), "md5sum", "md5 -r", "sha1sum"});

    // What is known is not asked again.
    b.commands.clear();
    CHECK(ssh::learn_commands(*pool, learned, {}) == learned);
    CHECK(b.commands.empty());
    const ssh::ServerCommands shell_only{.shell_type = "unix", .md5sum_command = std::nullopt, .sha1sum_command = std::nullopt};
    CHECK(ssh::learn_commands(*pool, shell_only, {}) == learned);
    CHECK(b.commands == std::vector<std::string>{"md5sum", "md5 -r", "sha1sum"});
    // Where no shell runs, rclone asks no sums either.
    b.commands.clear();
    const ssh::ServerCommands none{.shell_type = "none", .md5sum_command = std::nullopt, .sha1sum_command = std::nullopt};
    CHECK(ssh::learn_commands(*pool, none, {}) == none);
    CHECK(b.commands.empty());

    // No sum command works: "none", as rclone saves it.
    Box bare;
    bare.also = [&](test::FakeCommand& c, std::string_view command) {
        c.err.clear();
        c.out.clear();
        c.status = command == ssh::shell_probe ? 0 : 127;
        if (command == ssh::shell_probe) c.out = {"%ComSpec%\n"};
    };
    CHECK(ssh::learn_commands(*bare.pool(1), {}, {}) ==
          ssh::ServerCommands{.shell_type = "unix", .md5sum_command = "none", .sha1sum_command = "none"});

    // Refused (an SFTP-only account, or one channel only): nothing learned, nothing guessed.
    Box refused;
    refused.refuse = true;
    CHECK(ssh::learn_commands(*refused.pool(1), {}, {}) == ssh::ServerCommands{});
    // A login that fails: the same.
    Box no_login;
    no_login.no_login = ssh::Error(budget::Failure::fatal, "the host key isn't trusted");
    CHECK(ssh::learn_commands(*no_login.pool(1), {}, {}) == ssh::ServerCommands{});
}

TEST_CASE("ssh hash: absolute paths, the command, its output cap and deadline") {
    CHECK(ssh::absolute_path("/home/u", "movies/a b.mkv") == "/home/u/movies/a b.mkv");
    CHECK(ssh::absolute_path("/", "x") == "/x");
    CHECK(ssh::absolute_path("/home/u", "/srv/x") == "/srv/x");
    // Raw bytes in single quotes, a quote as '\''; nothing a restricted shell lacks (env, 2>, pipes).
    const std::vector<std::string> paths{"/home/u/it's", "/home/u/-x", "/home/u/caf\xE9 $HOME;rm"};
    const std::string command = ssh::hash_command(paths, true);
    CHECK(command == "md5sum -z '/home/u/it'\\''s' '/home/u/-x' '/home/u/caf\xE9 $HOME;rm'");
    CHECK(ssh::hash_command(paths, false).starts_with("md5sum '/home/u/it'"));
    for (const char* bare : {"env", "2>", "|", "--"}) CHECK(command.find(bare) == std::string::npos);
    // Each record's digest, two spaces, its end and the name; twice the name without -z.
    const std::size_t names = paths[0].size() + paths[1].size() + paths[2].size();
    CHECK(ssh::hash_out_limit(paths, true) == std::size_t{3} * 36 + names);
    CHECK(ssh::hash_out_limit(paths, false) == std::size_t{3} * 36 + 2 * names);
    // 5 minutes and 10 MiB/s; no overflow for any size (G166).
    CHECK(ssh::hash_deadline(0) == std::chrono::minutes(5));
    CHECK(ssh::hash_deadline(std::uint64_t{10} << 20) == std::chrono::minutes(5) + std::chrono::seconds(1));
    CHECK(ssh::hash_deadline(std::uint64_t{30} << 30) == std::chrono::minutes(5) + std::chrono::milliseconds(3072 * 1000));
    CHECK(ssh::hash_deadline(~std::uint64_t{0}) == std::chrono::days(30));
    CHECK(ssh::hash_deadline(~std::uint64_t{0}, std::chrono::hours(24 * 365)) == std::chrono::days(30));
    CHECK(ssh::hash_deadline(0, 50ms) == 50ms);
}

TEST_CASE("ssh hash: batches: biggest first, within the caps, odd names alone without -z") {
    std::vector<ssh::HashFile> files;
    auto add = [&](std::string path, std::uint64_t size) {
        files.push_back(ssh::HashFile{.path = std::move(path), .size = size});
        return files.size() - 1;
    };
    // 300 small files: 256 to a command.
    for (int i = 0; i < 300; ++i) add(std::format("/h/{:03}.txt", i), 10);
    auto batches = ssh::plan_batches(files, true);
    REQUIRE(batches.size() == 2);
    CHECK(batches[0].size() == 256);
    CHECK(batches[1].size() == 44);
    CHECK(batches[0].front() == 0); // same sizes: in their order

    // 256 MiB and up alone, biggest first; up to 1 GiB of the rest to a command.
    files.clear();
    const auto small_one = add("/h/small", 1);
    const auto huge = add("/h/huge", std::uint64_t{30} << 30);
    const auto edge = add("/h/edge", std::uint64_t{256} << 20);
    std::vector<std::size_t> quarter;
    for (int i = 0; i < 5; ++i) quarter.push_back(add(std::format("/h/q{}", i), std::uint64_t{250} << 20));
    batches = ssh::plan_batches(files, true);
    REQUIRE(batches.size() == 4);
    CHECK(batches[0] == std::vector<std::size_t>{huge});
    CHECK(batches[1] == std::vector<std::size_t>{edge});
    CHECK(batches[2] == std::vector<std::size_t>{quarter[0], quarter[1], quarter[2], quarter[3]});
    CHECK(batches[3] == std::vector<std::size_t>{quarter[4], small_one});

    // The command line's cap: long names spread over commands, none past it.
    files.clear();
    for (int i = 0; i < 40; ++i) add("/h/" + std::string(1000, static_cast<char>('a' + i % 26)) + std::to_string(i), 1);
    batches = ssh::plan_batches(files, true);
    std::size_t total = 0;
    for (const auto& b : batches) {
        std::vector<std::string> paths;
        for (const auto i : b) paths.push_back(files[i].path);
        CHECK(ssh::hash_command(paths, true).size() <= std::size_t{32} * 1024);
        total += b.size();
    }
    CHECK(batches.size() == 2);
    CHECK(total == 40);
    ssh::BatchLimits tight;
    tight.files = 3;
    CHECK(ssh::plan_batches(files, true, tight).size() == 14);

    // Without -z a backslash, CR or LF in a name: alone. With -z they share a command.
    files.clear();
    add("/h/plain", 1);
    add("/h/back\\slash", 1);
    add("/h/new\nline", 1);
    add("/h/carriage\rreturn", 1);
    add("/h/other", 1);
    CHECK(ssh::plan_batches(files, true).size() == 1);
    batches = ssh::plan_batches(files, false);
    REQUIRE(batches.size() == 4);
    CHECK(std::ranges::count_if(batches, [](const auto& b) { return b.size() == 1; }) == 3);
    CHECK(std::ranges::find(batches, std::vector<std::size_t>{0, 4}) != batches.end());
}

TEST_CASE("ssh hash: a record counts only when it names the next path sent, byte for byte") {
    const std::string a(32, 'a'), b(32, 'b'), c(32, 'c');
    const std::vector<std::string> sent{"/h/x", "/h/new\nline", "/h/back\\slash", "/h/y"};
    using Sums = std::vector<std::optional<std::string>>;
    // -z: names as they are.
    CHECK(ssh::parse_sums_in_order(a + "  /h/x\0"s + b + "  /h/new\nline\0"s + c + "  /h/back\\slash\0"s + a + "  /h/y\0"s,
                                   sent, true) == Sums{a, b, c, a});
    // Without -z: GNU's '\' form, its escapes undone; a binary-mode '*'.
    CHECK(ssh::parse_sums_in_order(a + "  /h/x\n\\" + b + "  /h/new\\nline\n\\" + c + "  /h/back\\\\slash\n" + a + " */h/y\n",
                                   sent, false) == Sums{a, b, c, a});
    // A file md5sum couldn't read gives no record; the others still pair.
    CHECK(ssh::parse_sums_in_order(a + "  /h/x\0"s + a + "  /h/y\0"s, sent, true) == Sums{a, {}, {}, a});
    // Out of order, a name not sent, a record of another shape, a cut last record: unchecked.
    CHECK(ssh::parse_sums_in_order(a + "  /h/y\0"s + b + "  /h/x\0"s, sent, true) == Sums{{}, {}, {}, a});
    CHECK(ssh::parse_sums_in_order(a + "  /h/z\0"s + a + "  /h/x \0"s, sent, true) == Sums{{}, {}, {}, {}});
    CHECK(ssh::parse_sums_in_order(std::string(32, 'A') + "  /h/x\0"s, sent, true) == Sums{{}, {}, {}, {}});
    CHECK(ssh::parse_sums_in_order(a.substr(1) + "  /h/x\0"s, sent, true) == Sums{{}, {}, {}, {}});
    CHECK(ssh::parse_sums_in_order(a + " /h/x\0"s, sent, true) == Sums{{}, {}, {}, {}});
    CHECK(ssh::parse_sums_in_order(a + "  /h/x\0"s + a + "  /h/y", sent, true) == Sums{a, {}, {}, {}});
    CHECK(ssh::parse_sums_in_order("\\" + a + "  /h/x\\t\n" + a + "  /h/y\n", sent, false) == Sums{{}, {}, {}, a});
    // A banner glued to the first record (-z): that file unchecked, never paired with another sum.
    CHECK(ssh::parse_sums_in_order("Welcome\n" + a + "  /h/x\0"s + a + "  /h/y\0"s, sent, true) == Sums{{}, {}, {}, a});
    // Another md5sum without -z that doesn't escape: a line break splits the record, which pairs
    // with nothing; a backslash comes as it is.
    CHECK(ssh::parse_sums_in_order(b + "  /h/new\nline\n" + c + "  /h/back\\slash\n", sent, false) == Sums{{}, {}, c, {}});
    // -z output read without it, and the reverse: nothing pairs.
    CHECK(ssh::parse_sums_in_order(a + "  /h/x\0"s, sent, false) == Sums{{}, {}, {}, {}});
    CHECK(ssh::parse_sums_in_order(a + "  /h/x\n", sent, true) == Sums{{}, {}, {}, {}});
}

TEST_CASE("ssh hash: a folder's files hashed in one command, odd names and a missing one included") {
    Box box;
    std::vector<ssh::HashFile> files{box.file("movies/a.mkv", "first film"), box.file("it's here.txt", "quoted"),
                                     box.file("new\nline.txt", "line"), box.file("caf\xE9.txt", "latin1"),
                                     box.file("-dash", "dash"), ssh::HashFile{.path = "gone.txt", .size = 3}};
    box.files["/srv/abs.txt"] = "absolute";
    files.push_back(ssh::HashFile{.path = "/srv/abs.txt", .size = 8});
    auto pool = box.pool();
    std::vector<std::size_t> told;
    ssh::HashOptions o;
    o.done = [&](std::size_t i, const ssh::HashResult&) { told.push_back(i); };
    const auto sums = ssh::run_hashes(*pool, files, o);
    REQUIRE(sums.size() == files.size());
    CHECK(sums[0] == md5_hex("first film"));
    CHECK(sums[1] == md5_hex("quoted"));
    CHECK(sums[2] == md5_hex("line"));
    CHECK(sums[3] == md5_hex("latin1"));
    CHECK(sums[4] == md5_hex("dash"));
    CHECK(kind_of(sums[5]) == ssh::HashFailure::unpaired);
    CHECK(why_of(sums[5]) == "no sum from the server (md5sum: /home/test/gone.txt: No such file or directory)");
    CHECK(sums[6] == md5_hex("absolute"));
    std::ranges::sort(told);
    CHECK(told == std::vector<std::size_t>{0, 1, 2, 3, 4, 5, 6});
    // The probe once, then one command for all, absolute paths in single quotes.
    CHECK(box.count(ssh::hash_probe) == 1);
    CHECK(box.count(ssh::hash_probe_plain) == 0);
    const auto hashes = box.hashes();
    REQUIRE(hashes.size() == 1);
    CHECK(hashes[0].starts_with("md5sum -z '/home/test/"));
    CHECK(hashes[0].find("'/home/test/it'\\''s here.txt'") != std::string::npos);
    CHECK(hashes[0].find("'/home/test/-dash'") != std::string::npos);
    CHECK(box.logins == 1);
    CHECK(pool->idle() == 1);
    // The check is the pool's: a second run asks nothing again.
    CHECK(ssh::run_hashes(*pool, std::span(files).first(1), o)[0] == md5_hex("first film"));
    CHECK(box.count(ssh::hash_probe) == 1);
    CHECK(ssh::hash_check(*pool, {}) == ssh::HashCheck{.works = true, .z = true, .home = "/home/test",
                                                        .answer = "md5sum -z", .failure = {}, .lasting = true});
    // Nothing to hash: nothing asked.
    Box quiet;
    auto other = quiet.pool();
    CHECK(ssh::run_hashes(*other, {}, o).empty());
    CHECK(quiet.logins == 0);
}

TEST_CASE("ssh hash: an md5sum without -z: the plain probe, odd names alone, GNU's escapes or none") {
    for (const bool escapes : {true, false}) {
        CAPTURE(escapes);
        Box box;
        box.z = false;
        box.escapes = escapes;
        std::vector<ssh::HashFile> files{box.file("plain.txt", "p"), box.file("back\\slash.txt", "b"),
                                         box.file("new\nline.txt", "n"), box.file("other.txt", "o")};
        auto pool = box.pool();
        const auto sums = ssh::run_hashes(*pool, files);
        CHECK(box.count(ssh::hash_probe) == 1);
        CHECK(box.count(ssh::hash_probe_plain) == 1);
        CHECK(sums[0] == md5_hex("p"));
        CHECK(sums[1] == md5_hex("b"));
        CHECK(sums[3] == md5_hex("o"));
        if (escapes) {
            CHECK(sums[2] == md5_hex("n"));
        } else {
            // Its record split in two: unchecked, never another file's sum.
            CHECK(kind_of(sums[2]) == ssh::HashFailure::unpaired);
        }
        const auto hashes = box.hashes();
        CHECK(hashes.size() == 3); // the backslash and the line break alone
        for (const auto& h : hashes) CHECK(h.starts_with("md5sum '"));
    }
}

TEST_CASE("ssh hash: servers that can't: no md5sum, a Windows shell, a banner, a Storage Box, SFTP only") {
    struct Case {
        const char* name;
        std::function<void(Box&)> make;
        ssh::HashFailure failure;
        const char* says;
    };
    const std::vector<Case> cases{
        {.name = "Windows",
         .make = [](Box& b) { b.shell = Box::Shell::windows; },
         .failure = ssh::HashFailure::no_checksums,
         .says = R"(the server can't compute checksums (its answer to `md5sum -z ''\-` was "md5sum: ''\-: No such file or directory", exit 1))"},
        {.name = "banner",
         .make = [](Box& b) { b.banner = "Welcome to the box\n"; },
         .failure = ssh::HashFailure::no_checksums,
         .says = R"("Welcome to the box")"},
        {.name = "Storage Box",
         .make = [](Box& b) { b.shell = Box::Shell::restricted; },
         .failure = ssh::HashFailure::no_checksums,
         .says = "exit 8"},
        {.name = "no md5sum",
         .make =
             [](Box& b) {
                 b.also = [](test::FakeCommand& c, std::string_view) {
                     c.out.clear();
                     c.err = "zsh:1: command not found: md5sum\n";
                     c.status = 127;
                 };
             },
         .failure = ssh::HashFailure::no_checksums,
         .says = "command not found: md5sum"},
        {.name = "SFTP only",
         .make = [](Box& b) { b.refuse = true; },
         .failure = ssh::HashFailure::refused,
         .says = "the server refused to run the command"},
    };
    for (const auto& c : cases) {
        CAPTURE(c.name);
        Box box;
        c.make(box);
        std::vector<ssh::HashFile> files{box.file("a.txt", "a"), box.file("b.txt", "b")};
        auto pool = box.pool(2, 1, 0ms);
        const auto sums = ssh::run_hashes(*pool, files);
        for (const auto& s : sums) {
            CHECK(kind_of(s) == c.failure);
            CHECK_MESSAGE(why_of(s).find(c.says) != std::string::npos, why_of(s));
        }
        CHECK(box.hashes().empty()); // no name sent
    }
    // A lasting answer is kept for the pool's life; a refusal (on a new login too) as one that may
    // pass, so an SFTP-only account costs no login per verification.
    Box windows;
    windows.shell = Box::Shell::windows;
    auto pool = windows.pool(2, 1, 0ms);
    const std::vector<ssh::HashFile> one{windows.file("a.txt", "a")};
    CHECK(kind_of(ssh::run_hashes(*pool, one)[0]) == ssh::HashFailure::no_checksums);
    CHECK(kind_of(ssh::run_hashes(*pool, one)[0]) == ssh::HashFailure::no_checksums);
    CHECK(windows.count(ssh::hash_probe) == 1);
    Box sftp_only;
    sftp_only.refuse = true;
    auto refusing = sftp_only.pool();
    const std::vector<ssh::HashFile> a{sftp_only.file("a.txt", "a")};
    CHECK(kind_of(ssh::run_hashes(*refusing, a)[0]) == ssh::HashFailure::refused);
    CHECK(kind_of(ssh::run_hashes(*refusing, a)[0]) == ssh::HashFailure::refused);
    CHECK(sftp_only.count(ssh::hash_probe) == 2); // once, then once on a new login
    CHECK(sftp_only.logins == 2);
    auto brief = sftp_only.pool(2, 1, 0ms);
    CHECK(kind_of(ssh::run_hashes(*brief, a)[0]) == ssh::HashFailure::refused);
    CHECK(sftp_only.count(ssh::hash_probe) == 4);

    // rclone.conf says so: nothing runs, no login.
    Box saved;
    auto unused = saved.pool();
    ssh::HashOptions o;
    o.server.shell_type = "powershell";
    const auto blocked = ssh::run_hashes(*unused, std::vector<ssh::HashFile>{saved.file("a.txt", "a")}, o);
    CHECK(kind_of(blocked[0]) == ssh::HashFailure::no_checksums);
    CHECK(saved.logins == 0);
}

TEST_CASE("ssh hash: MaxSessions 1: a command refused beside SFTP goes once to a new login") {
    Box box;
    box.one_channel = true;
    std::vector<ssh::HashFile> files{box.file("a.txt", "a"), box.file("b.txt", "b")};
    auto pool = box.pool(2);
    // A download left a connection with SFTP running.
    {
        auto lease = pool->acquire();
        REQUIRE(lease);
        [[maybe_unused]] auto& s = (*lease)->sftp();
    }
    const auto sums = ssh::run_hashes(*pool, files);
    CHECK(sums[0] == md5_hex("a"));
    CHECK(sums[1] == md5_hex("b"));
    CHECK(box.logins == 2); // the download's and one for commands, which the hashes then use too
    CHECK(box.count(ssh::hash_probe) == 2);
    CHECK(box.hashes().size() == 1);

    // A full pool: its oldest idle connection makes room.
    Box full;
    full.one_channel = true;
    std::vector<ssh::HashFile> more{full.file("a.txt", "a")};
    auto one = full.pool(1);
    {
        auto lease = one->acquire();
        REQUIRE(lease);
        [[maybe_unused]] auto& s = (*lease)->sftp();
    }
    CHECK(ssh::run_hashes(*one, more)[0] == md5_hex("a"));
    CHECK(full.logins == 2);
    CHECK(one->open() == 1);

    // Refused on the batch's reused connection too: once more on a new login.
    Box later;
    std::vector<ssh::HashFile> files2{later.file("a.txt", "a")};
    auto pool2 = later.pool(2);
    REQUIRE(ssh::hash_check(*pool2, {}));
    auto refusals = std::make_shared<std::atomic<int>>(0);
    later.also = [refusals](test::FakeCommand& c, std::string_view command) {
        if (!is_probe(command) && (*refusals)++ == 0) c.refuse_channel = "Channel open failure (connect failed)";
    };
    CHECK(ssh::run_hashes(*pool2, files2)[0] == md5_hex("a"));
    CHECK(later.logins == 2);
    // Refused on a new login as well: refused, not "no checksums".
    later.also = [](test::FakeCommand& c, std::string_view command) {
        if (!is_probe(command)) c.refuse = true;
    };
    const auto refused = ssh::run_hashes(*pool2, files2);
    CHECK(kind_of(refused[0]) == ssh::HashFailure::refused);
}

TEST_CASE("ssh hash: a login that fails is tried once, never once per file") {
    // Refused for good (the key, the host key): kept for the pool's life.
    Box box;
    box.no_login = ssh::Error(budget::Failure::fatal, "h refused the login: it accepted none of ssh-agent's keys");
    std::vector<ssh::HashFile> files{box.file("a.txt", "a"), box.file("b.txt", "b")};
    auto pool = box.pool();
    for (int run = 0; run < 3; ++run) {
        const auto sums = ssh::run_hashes(*pool, files);
        for (const auto& s : sums) {
            CHECK(kind_of(s) == ssh::HashFailure::login);
            CHECK(why_of(s) == "h refused the login: it accepted none of ssh-agent's keys");
        }
    }
    CHECK(box.logins == 1);
    // A network failure: kept for a while (find_check_fresh), then tried again.
    Box flaky;
    flaky.no_login = ssh::Error(budget::Failure::other, "connection failed (10060)");
    std::vector<ssh::HashFile> one{flaky.file("a.txt", "a")};
    auto held = flaky.pool();
    CHECK(kind_of(ssh::run_hashes(*held, one)[0]) == ssh::HashFailure::login);
    CHECK(kind_of(ssh::run_hashes(*held, one)[0]) == ssh::HashFailure::login);
    CHECK(flaky.logins == 1);
    auto brief = flaky.pool(2, 1, 0ms);
    CHECK(kind_of(ssh::run_hashes(*brief, one)[0]) == ssh::HashFailure::login);
    CHECK(kind_of(ssh::run_hashes(*brief, one)[0]) == ssh::HashFailure::login);
    CHECK(flaky.logins == 3);

    // After the check, a second connection's login fails: the run's other commands don't try, and
    // neither does the next run.
    Box second;
    second.no_login = ssh::Error(budget::Failure::fatal, "h refused the login: the password was refused");
    second.fail_logins_from = 2;
    second.also = [](test::FakeCommand& c, std::string_view command) {
        if (!is_probe(command)) c.silent = 100ms;
    };
    std::vector<ssh::HashFile> many;
    for (int i = 0; i < 6; ++i) many.push_back(second.file(std::format("{}.txt", i), std::to_string(i)));
    auto pool2 = second.pool(4, 4);
    ssh::HashOptions o;
    o.connections = 4;
    o.batches.files = 1;
    const auto sums = ssh::run_hashes(*pool2, many, o);
    const auto logins_then = second.logins.load();
    CHECK(logins_then == 2);
    CHECK(std::ranges::count_if(sums, [](const auto& s) { return kind_of(s) == ssh::HashFailure::login; }) >= 1);
    for (const auto& s : sums) CHECK((s.has_value() || kind_of(s) == ssh::HashFailure::login));
    for (const auto& s : ssh::run_hashes(*pool2, many, o)) CHECK(kind_of(s) == ssh::HashFailure::login);
    CHECK(second.logins == logins_then);
}

TEST_CASE("ssh hash: a deadline, a stop, a lost connection, a signal and too much output keep files unchecked") {
    // The deadline.
    {
        Box box;
        box.also = [](test::FakeCommand& c, std::string_view command) {
            if (!is_probe(command)) c.silent = 5s;
        };
        std::vector<ssh::HashFile> files{box.file("a.txt", "a")};
        auto pool = box.pool();
        ssh::HashOptions o;
        o.deadline = 50ms;
        const auto started = std::chrono::steady_clock::now();
        const auto sums = ssh::run_hashes(*pool, files, o);
        CHECK(std::chrono::steady_clock::now() - started < 3s);
        CHECK(sums[0] == std::unexpected(ssh::HashFailed{.kind = ssh::HashFailure::deadline, .why = "the server took too long"}));
    }
    // A stop mid-hash, and before it.
    {
        Box box;
        box.also = [](test::FakeCommand& c, std::string_view command) {
            if (!is_probe(command)) c.forever = true;
        };
        std::vector<ssh::HashFile> files{box.file("a.txt", "a"), box.file("b.txt", "b")};
        auto pool = box.pool();
        std::stop_source stop;
        std::jthread stopper([&] {
            std::this_thread::sleep_for(50ms);
            stop.request_stop();
        });
        ssh::HashOptions o;
        o.stop = stop.get_token();
        const auto started = std::chrono::steady_clock::now();
        const auto sums = ssh::run_hashes(*pool, files, o);
        CHECK(std::chrono::steady_clock::now() - started < 3s);
        for (const auto& s : sums) CHECK(kind_of(s) == ssh::HashFailure::stopped);
        for (const auto& s : ssh::run_hashes(*pool, files, o)) CHECK(kind_of(s) == ssh::HashFailure::stopped);
    }
    // The connection breaks mid-hash: a reused one is followed by the next; a new login's is lost.
    {
        Box box;
        box.lose = [](int, std::string_view command) { return !is_probe(command); };
        std::vector<ssh::HashFile> files{box.file("a.txt", "a")};
        auto pool = box.pool();
        const auto sums = ssh::run_hashes(*pool, files);
        CHECK(kind_of(sums[0]) == ssh::HashFailure::lost);
        CHECK(why_of(sums[0]).starts_with("the connection was lost: the server stopped answering"));
        CHECK(box.logins == 2);
        CHECK(box.hashes().size() == 2);
        // Only the idle one broke: the next login hashes.
        Box once;
        once.lose = [](int login, std::string_view command) { return login == 1 && !is_probe(command); };
        std::vector<ssh::HashFile> again{once.file("a.txt", "a")};
        auto pool2 = once.pool();
        CHECK(ssh::run_hashes(*pool2, again)[0] == md5_hex("a"));
        // Two reused ones break: the link, not an idle connection. Lost after one more try, not
        // hashed again on every idle connection left.
        Box many;
        std::vector<ssh::HashFile> one{many.file("a.txt", "a")};
        auto pool3 = many.pool(3);
        REQUIRE(ssh::hash_check(*pool3, {}));
        {
            auto l1 = pool3->acquire();
            auto l2 = pool3->acquire();
            auto l3 = pool3->acquire();
            REQUIRE((l1 && l2 && l3));
        }
        REQUIRE(pool3->idle() == 3);
        many.lose = [](int, std::string_view command) { return !is_probe(command); };
        const auto lost = ssh::run_hashes(*pool3, one);
        CHECK(kind_of(lost[0]) == ssh::HashFailure::lost);
        CHECK(many.hashes().size() == 2);
        CHECK(many.logins == 3);
    }
    // Ended by a signal; more output than md5sum gives.
    {
        Box box;
        std::vector<ssh::HashFile> files{box.file("a.txt", "a")};
        auto pool = box.pool();
        REQUIRE(ssh::hash_check(*pool, {}));
        box.also = [](test::FakeCommand& c, std::string_view) { c.signal = "KILL"; };
        auto sums = ssh::run_hashes(*pool, files);
        CHECK(sums[0] == std::unexpected(ssh::HashFailed{.kind = ssh::HashFailure::lost,
                                                         .why = "md5sum on the server was ended by SIGKILL"}));
        box.also = [](test::FakeCommand& c, std::string_view) { c.endless = true; };
        sums = ssh::run_hashes(*pool, files);
        CHECK(kind_of(sums[0]) == ssh::HashFailure::lost);
        // Exit 1 for another file's sake keeps the sums that came.
        box.also = [](test::FakeCommand& c, std::string_view) {
            c.err = "md5sum: /home/test/x: Permission denied\n";
            c.status = 1;
        };
        CHECK(ssh::run_hashes(*pool, files)[0] == md5_hex("a"));
    }
}

TEST_CASE("ssh hash: a Storage Box gets no name with a control byte; a name past the cap goes nowhere") {
    Box box;
    std::vector<ssh::HashFile> files{box.file("plain.txt", "p"), box.file("new\nline.txt", "n"), box.file("del\x7F.txt", "d"),
                                     box.file("tab\there.txt", "t")};
    auto pool = box.pool();
    ssh::HashOptions o;
    o.server.host = "u123456-sub1.your-storagebox.de";
    const auto sums = ssh::run_hashes(*pool, files, o);
    CHECK(sums[0] == md5_hex("p"));
    for (std::size_t i = 1; i < sums.size(); ++i) {
        CHECK(sums[i] == std::unexpected(ssh::HashFailed{.kind = ssh::HashFailure::unsupported,
                                                         .why = "the name can't be passed to the server's shell"}));
    }
    const auto hashes = box.hashes();
    REQUIRE(hashes.size() == 1);
    CHECK(hashes[0] == "md5sum -z '/home/test/plain.txt'");
    // Elsewhere they go.
    Box server;
    std::vector<ssh::HashFile> same{server.file("new\nline.txt", "n"), server.file("tab\there.txt", "t")};
    auto pool2 = server.pool();
    const auto there = ssh::run_hashes(*pool2, same);
    CHECK(there[0] == md5_hex("n"));
    CHECK(there[1] == md5_hex("t"));
    // A name whose command alone would pass the cap, or holding a NUL, is never sent.
    std::vector<ssh::HashFile> odd{server.file(std::string(40'000, 'x'), "long"), ssh::HashFile{.path = "nul\0x"s, .size = 1}};
    const auto none = ssh::run_hashes(*pool2, odd);
    CHECK(why_of(none[0]) == "the name is too long to pass to the server's shell");
    CHECK(why_of(none[1]) == "the name can't be passed to the server's shell");
    CHECK(server.hashes().size() == 1);
}

TEST_CASE("ssh hash: a name fish would read otherwise inside single quotes is never sent") {
    // fish reads \' and \\ as escapes inside single quotes: a\';x would end the quoting and run x,
    // and a name ending in \ would leave the next name unquoted.
    Box box;
    std::vector<ssh::HashFile> files{box.file("plain.txt", "p"), box.file("a\\';x", "q"), box.file("x\\", "e"),
                                     box.file("next.txt", "n"), box.file("a\\\\b", "d"), box.file("one\\slash", "s")};
    auto pool = box.pool();
    const auto sums = ssh::run_hashes(*pool, files);
    CHECK(sums[0] == md5_hex("p"));
    CHECK(sums[3] == md5_hex("n"));
    CHECK(sums[5] == md5_hex("s")); // a lone backslash before anything else reads the same
    for (const std::size_t i : std::array<std::size_t, 3>{1, 2, 4}) {
        CAPTURE(files[i].path);
        CHECK(sums[i] == std::unexpected(ssh::HashFailed{.kind = ssh::HashFailure::unsupported,
                                                         .why = "the name can't be passed to the server's shell"}));
    }
    const auto hashes = box.hashes();
    REQUIRE(hashes.size() == 1);
    for (const char* bad : {"a\\'", "x\\'", "a\\\\b"}) CHECK(hashes[0].find(bad) == std::string::npos);
}

TEST_CASE("ssh hash: hash commands per server at once, however many runs share the pool") {
    Box box;
    box.also = [](test::FakeCommand& c, std::string_view command) {
        if (!is_probe(command)) c.silent = 30ms;
    };
    std::vector<ssh::HashFile> files;
    for (int i = 0; i < 8; ++i) files.push_back(box.file(std::format("{}.txt", i), std::to_string(i)));
    ssh::HashOptions o;
    o.connections = 4;
    o.batches.files = 1;
    // One at a time on an ordinary server, whatever the runs' seats.
    auto pool = box.pool(4, 1);
    REQUIRE(ssh::hash_check(*pool, {}));
    {
        std::jthread other([&] { [[maybe_unused]] const auto sums = ssh::run_hashes(*pool, files, o); });
        const auto sums = ssh::run_hashes(*pool, files, o);
        for (std::size_t i = 0; i < sums.size(); ++i) CHECK(sums[i] == md5_hex(std::to_string(i)));
    }
    CHECK(box.most == 1);
    // Two on a Storage Box's pool; a run with one seat runs one.
    Box box2;
    box2.also = box.also;
    for (int i = 0; i < 8; ++i) box2.file(std::format("{}.txt", i), std::to_string(i));
    auto pool2 = box2.pool(4, 2);
    REQUIRE(ssh::hash_check(*pool2, {}));
    CHECK(ssh::run_hashes(*pool2, files, o)[7] == md5_hex("7"));
    CHECK(box2.most == 2);
    box2.most = 0;
    o.connections = 1;
    CHECK(ssh::run_hashes(*pool2, files, o)[7] == md5_hex("7"));
    CHECK(box2.most == 1);
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

// A small file's bytes over SFTP.
std::optional<std::string> read_over_sftp(ssh::Pool& pool, const std::string& raw) {
    auto lease = pool.acquire();
    REQUIRE(lease);
    auto& s = (*lease)->sftp();
    const auto attrs = sftp::stat(s, raw);
    if (!attrs || !attrs->size) return std::nullopt;
    if (*attrs->size == 0) return std::string();
    auto handle = sftp::open_file(s, raw);
    if (!handle) return std::nullopt;
    std::string bytes(static_cast<std::size_t>(*attrs->size), '\0');
    const auto got = sftp::read_range(s, *handle, 0, *attrs->size, [&](std::uint64_t at, std::string_view piece) {
        if (at + piece.size() <= bytes.size()) std::ranges::copy(piece, bytes.begin() + static_cast<std::ptrdiff_t>(at));
    });
    const auto closed = sftp::close_file(s, *handle);
    if (!got || !closed) return std::nullopt;
    return bytes;
}

// The awkward names phase 5's checks use, each file holding its own name, under the folder the
// script is run in (bash).
constexpr std::string_view odd_tree = R"sh(
mk() { mkdir -p "$(dirname -- "$1")"; printf '%s' "$1" > "./$1"; }
mk 'plain.txt'; mk 'with space.txt'; mk "it's.txt"; mk "two''quotes.txt"; mk 'double"quote.txt'
mk 'back\slash.txt'; mk 'ends with backslash\'; mk $'new\nline.txt'; mk $'carriage\rreturn.txt'
mk $'both\r\nends.txt'; mk $'tab\there.txt'; mk $'del\x7f.txt'; mk $'ctl\x01char.txt'; mk $'esc\x1b[31mred.txt'
mk $'caf\xe9 latin1.txt'; mk $'caf\xc3\xa9 nfc.txt'; mk $'cafe\xcc\x81 nfd.txt'; mk '-leading-dash.txt'; mk '-'
mk '--'; mk 'dollar$HOME.txt'; mk 'back`tick`.txt'; mk 'semi;colon.txt'; mk 'amp&and.txt'; mk 'pipe|bar.txt'
mk 'star*.txt'; mk 'question?.txt'; mk 'bracket[a].txt'; mk 'brace{a,b}.txt'; mk '~tilde-first'; mk '#hash.txt'
mk 'bang!.txt'; mk 'percent%s.txt'; mk 'trailing space '; mk 'trailing.'; mk 'paren(1).txt'; mk 'lt<gt>.txt'
mk "$(printf 'L%.0s' $(seq 1 200)).txt"; mk 'sub dir/inner file.txt'; mk $'sub\nfolder/x.txt'
mk $'quote\'d dir/y\'s.txt'; mk $'\xe2\x80\x9bquote.txt'; : > empty.bin; head -c 3145733 /dev/urandom > big.bin
)sh";

} // namespace

// A /tmp tree of awkward names on the server, made for the test and deleted after: the sums
// run_hashes gives (-z where the server takes it), and those of md5sum without -z (GNU's
// escapes), equal the MD5 of each file's bytes read over SFTP and hashed here by util::md5_file.
// With GRAB_TEST_RCLONE_CONF / GRAB_TEST_REMOTE (the same server), also `rclone hashsum md5`'s for
// every name rclone gives (GRAB_TEST_RCLONE: rclone.exe, else the one the build bundles). At most 2
// logins of grab's own, then rclone's one.
TEST_CASE("ssh hash: live, odd names hashed on the server equal local MD5 and rclone's (GRAB_TEST_SSH)") {
    auto t = live_target();
    if (!t) {
        MESSAGE("skipped: GRAB_TEST_SSH / GRAB_TEST_KNOWN_HOSTS not set");
        return;
    }
    ssh::Pool::Options po;
    po.limit = 2;
    po.logins_at_once = 2;
    auto pool = std::make_shared<ssh::Pool>([t] { return ssh::open_session(*t); }, po);
    const std::string base = std::format("/tmp/grab-p5b-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    struct Cleanup {
        ssh::Pool& pool;
        std::string base;
        ~Cleanup() {
            try {
                if (auto lease = pool.acquire()) {
                    [[maybe_unused]] auto gone = (*lease)->exec("rm -rf '" + base + "'", {});
                }
            } catch (const std::exception& e) {
                for (const char* s : {"couldn't delete ", base.c_str(), " on the server: ", e.what(), "\n"}) std::fputs(s, stderr);
            } catch (...) {
                std::fputs("couldn't delete the test's folder on the server\n", stderr);
            }
        }
    } cleanup{.pool = *pool, .base = base};
    {
        auto lease = pool->acquire();
        REQUIRE(lease);
        const std::string script = "set -e; mkdir -p '" + base + "'; cd '" + base + "'" + std::string(odd_tree);
        auto made = (*lease)->exec("bash -c " + quote::sh_single(script), {});
        REQUIRE_MESSAGE(made, (made ? "" : made.error().message));
        REQUIRE_MESSAGE(made->status == 0, made->err);
    }

    auto walked = ssh::walk_roots(*pool, std::vector<std::string>{base},
                                  ssh::WalkOptions{.connections = 2, .folders = false, .max_depth = 0, .skip_hidden = false}, {});
    REQUIRE_MESSAGE(walked, walked.error_or(""));
    std::vector<ssh::HashFile> files;
    std::vector<std::string> shown; // grab's form, as a download names them
    for (const auto& e : walked->entries) {
        files.push_back(ssh::HashFile{.path = rclone_enc::raw_path(e.path), .size = e.size});
        shown.push_back(e.path);
    }
    REQUIRE(files.size() == 44);
    auto check = ssh::hash_check(*pool, {});
    REQUIRE(check);
    MESSAGE("the check: " << check->answer << ", login folder " << check->home);
    REQUIRE(check->works);
    // One from the login folder (made absolute from SFTP's REALPATH), and one that isn't there.
    std::string up;
    for (const char c : check->home)
        if (c == '/') up += "../";
    files.push_back(ssh::HashFile{.path = up + files.front().path.substr(1), .size = files.front().size});
    shown.push_back(shown.front());
    files.push_back(ssh::HashFile{.path = base + "/no such file", .size = 1});
    shown.emplace_back("(missing)");

    ssh::HashOptions o;
    const auto conf = util::getenv_utf8("GRAB_TEST_RCLONE_CONF");
    const auto name = util::getenv_utf8("GRAB_TEST_REMOTE");
    if (conf && name) {
        auto server = test::live_server(util::path_from_utf8(*conf), *name, grab_known_hosts_path());
        REQUIRE_MESSAGE(server, server.error_or(""));
        // Its keys as rclone saved them (shell_type, md5sum_command), as the import takes them.
        o.server = server_conf::to_remote(*name, server->server.connection, grab_known_hosts_path());
    }
    o.batches.files = 16; // several commands
    const auto started = std::chrono::steady_clock::now();
    const auto sums = ssh::run_hashes(*pool, files, o);
    MESSAGE(files.size() << " files hashed in "
                         << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() << " s");
    CHECK(why_of(sums.back()).starts_with("no sum from the server (md5sum: "));

    // Each file's bytes, read over SFTP and hashed here.
    const auto local = std::filesystem::temp_directory_path() / "grab-tests-p5-live";
    std::filesystem::remove_all(local);
    std::filesystem::create_directories(local);
    std::map<std::string, std::string> by_raw; // the tree's raw paths -> the server's sums
    std::vector<bool> unsent(files.size());     // a name fish would read otherwise: never sent
    for (std::size_t i = 0; i + 1 < files.size(); ++i) {
        CAPTURE(shown[i]);
        if (files[i].path.ends_with('\\')) {
            CHECK(why_of(sums[i]) == "the name can't be passed to the server's shell");
            unsent[i] = true;
            continue;
        }
        REQUIRE_MESSAGE(sums[i], why_of(sums[i]));
        const auto bytes = read_over_sftp(*pool, ssh::absolute_path(check->home, files[i].path));
        REQUIRE(bytes);
        const auto copy = local / std::to_string(i);
        REQUIRE(util::write_file_atomic(copy, *bytes));
        CHECK(util::md5_file(copy) == *sums[i]);
        if (i + 2 < files.size()) by_raw[files[i].path] = *sums[i];
    }
    std::filesystem::remove_all(local);

    // md5sum without -z: GNU's escapes undone, odd names alone.
    {
        std::vector<ssh::HashFile> sent;
        std::vector<std::size_t> from; // sent[k] is files[from[k]]
        for (std::size_t i = 0; i + 2 < files.size(); ++i) {
            if (unsent[i]) continue;
            sent.push_back(ssh::HashFile{.path = ssh::absolute_path(check->home, files[i].path), .size = files[i].size});
            from.push_back(i);
        }
        std::size_t paired = 0;
        const auto batches = ssh::plan_batches(sent, false);
        for (const auto& b : batches) {
            std::vector<std::string> paths;
            for (const auto k : b) paths.push_back(sent[k].path);
            auto lease = pool->acquire();
            REQUIRE(lease);
            std::string out;
            ssh::ExecOptions x;
            x.out = [&](std::string_view piece) {
                out += piece;
                return true;
            };
            x.out_limit = ssh::hash_out_limit(paths, false);
            auto ran = (*lease)->exec(ssh::hash_command(paths, false), x);
            REQUIRE_MESSAGE(ran, (ran ? "" : ran.error().message));
            const auto got = ssh::parse_sums_in_order(out, paths, false);
            for (std::size_t j = 0; j < b.size(); ++j) {
                CAPTURE(shown[from[b[j]]]);
                CHECK(got[j] == sums[from[b[j]]].value_or("none"));
                paired += got[j].has_value() ? 1 : 0;
            }
        }
        MESSAGE("without -z: " << paired << " of " << sent.size() << " paired, in " << batches.size() << " commands");
        CHECK(paired == sent.size());
    }

    // rclone's own sums for the same tree, its names as raw bytes (the SFTP backend's encoding),
    // one per line: a name holding a line break can't be read back.
    if (!conf || !name) {
        MESSAGE("rclone not asked: GRAB_TEST_RCLONE_CONF / GRAB_TEST_REMOTE not set");
        return;
    }
    const std::string rclone = util::getenv_utf8("GRAB_TEST_RCLONE")
                                   .value_or(util::path_to_utf8(util::self_exe_path().parent_path().parent_path() / "rclone.exe"));
    const std::vector<std::string> argv{rclone, "hashsum", "md5", *name + ":" + base, "--config", *conf,
                                        "--checkers", "1", "--sftp-connections", "1"};
    proc::RunOptions ro;
    ro.detached = true;
    const auto ran = proc::run_capture(argv, ro);
    REQUIRE_MESSAGE(ran, ran.error_or(""));
    MESSAGE("rclone exit " << ran->exit_code << (ran->err.empty() ? "" : "; stderr: " + ran->err.substr(0, 2000)));
    std::size_t same = 0, other = 0, unknown = 0;
    const auto readable =
        std::ranges::count_if(by_raw, [](const auto& f) { return f.first.find('\n') == std::string::npos; });
    for (std::string_view rest = ran->out; !rest.empty();) {
        const auto nl = rest.find('\n');
        const std::string_view line = rest.substr(0, nl);
        rest.remove_prefix(nl == std::string_view::npos ? rest.size() : nl + 1);
        if (line.size() < 34 || line.substr(32, 2) != "  ") continue;
        const std::string path = base + "/" + std::string(line.substr(34));
        if (path.ends_with('\\')) continue; // never sent (above)
        const auto it = by_raw.find(path);
        if (it == by_raw.end()) {
            ++unknown;
            MESSAGE("rclone names a path grab's walk doesn't: " << rclone_enc::from_raw_path(path));
        } else if (it->second == line.substr(0, 32)) {
            ++same;
        } else {
            ++other;
            MESSAGE("DIFFERENT for " << rclone_enc::from_raw_path(path) << ": rclone " << line.substr(0, 32) << ", grab "
                                     << it->second);
        }
    }
    MESSAGE("rclone hashsum md5: " << same << " the same, " << other << " different, " << unknown << " not matched by name");
    CHECK(other == 0);
    CHECK(same == static_cast<std::size_t>(readable));
}
