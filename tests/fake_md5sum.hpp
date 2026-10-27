#pragma once

// A server whose md5sum runs as GNU's (or BusyBox's) does under a POSIX shell, a Windows shell or a
// Storage Box's, for the tests of grab's own verification (test_ssh_hash.cpp, test_verify.cpp).
// Its SFTP serves the same files (absolute paths) with their times, for the STAT after a mismatch.

#include "conn_budget.hpp"
#include "fake_command.hpp"
#include "fake_sftp.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_hash.hpp"
#include "ssh_pool.hpp"

#include <doctest/doctest.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grab::test {

using namespace std::string_literals;

inline std::string md5_hex(std::string_view bytes) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    REQUIRE(EVP_Digest(bytes.data(), bytes.size(), digest.data(), &size, EVP_md5(), nullptr) == 1);
    std::string hex;
    for (unsigned int i = 0; i < size; ++i) hex += std::format("{:02x}", digest[i]);
    return hex;
}

// The words of a command as a POSIX shell takes them: single quotes, a backslash outside them.
inline std::vector<std::string> sh_words(std::string_view command) {
    std::vector<std::string> words;
    std::string word;
    bool in_word = false;
    for (std::size_t i = 0; i < command.size(); ++i) {
        const char c = command[i];
        if (c == '\'') {
            in_word = true;
            for (++i; i < command.size() && command[i] != '\''; ++i) word += command[i];
        } else if (c == '\\' && i + 1 < command.size()) {
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

// ...and as cmd or PowerShell pass them to a program: quotes and backslashes kept.
inline std::vector<std::string> windows_words(std::string_view command) {
    std::vector<std::string> words;
    for (std::size_t at = 0; at < command.size();) {
        const auto space = command.find(' ', at);
        const auto end = space == std::string_view::npos ? command.size() : space;
        if (end > at) words.emplace_back(command.substr(at, end - at));
        at = end + 1;
    }
    return words;
}

// A server with SFTP (its login folder: /home/test, as FakeServer answers REALPATH) whose md5sum
// hashes `files` (absolute raw paths) as GNU's does, and whose SFTP STAT gives their sizes and
// `mtimes` (else 1700000000). Switches make it another server; `also` changes a command's channel
// last. It records the commands, and how many ran at once.
struct Box {
    enum class Shell { posix, windows, restricted };
    std::map<std::string, std::string> files;
    std::map<std::string, std::uint32_t> mtimes;
    Shell shell = Shell::posix;
    bool z = true;       // md5sum takes -z (BusyBox's doesn't)
    bool escapes = true; // without -z, GNU's '\' form for a name holding a backslash or line break
    std::string banner;  // printed before anything (a login shell's banner)
    bool refuse = false; // every command refused (an SFTP-only account)
    bool one_channel = false; // MaxSessions 1: a command's channel refused beside a running SFTP session
    std::function<void(test::FakeCommand&, std::string_view command)> also;
    std::function<bool(int login, std::string_view command)> lose; // the connection breaks instead
    std::optional<ssh::Error> no_login;                              // logins fail
    int fail_logins_from = 0;                                        // ...from this one on (0: as no_login says)

    std::mutex mutex;
    std::vector<std::string> commands;
    std::atomic<int> logins{0}, running{0}, most{0};

    // md5sum as the server runs `command`.
    void md5sum(test::FakeCommand& c, std::string_view command) const {
        const auto words = shell == Shell::windows ? windows_words(command) : sh_words(command);
        if (shell == Shell::restricted || words.empty() || words[0] != "md5sum") {
            c.err = shell == Shell::restricted ? "Command not found. Use 'help' to get a list of available commands.\n"
                                               : "sh: 1: " + (words.empty() ? ""s : words[0]) + ": not found\n";
            c.status = shell == Shell::restricted ? 8 : 127;
            return;
        }
        bool zz = false;
        std::string out = banner;
        int status = 0;
        for (std::size_t i = 1; i < words.size(); ++i) {
            const std::string& name = words[i];
            if (name == "-z") {
                if (!z) {
                    c.err = "md5sum: unrecognized option: z\n";
                    c.status = 1;
                    c.out = {banner};
                    return;
                }
                zz = true;
                continue;
            }
            const auto f = files.find(name);
            if (name != "-" && f == files.end()) {
                c.err += "md5sum: " + name + ": No such file or directory\n";
                status = 1;
                continue;
            }
            const std::string digest = md5_hex(name == "-" ? ""s : f->second);
            if (zz) {
                out += digest + "  " + name + '\0';
            } else if (escapes && name.find_first_of("\\\n\r") != std::string::npos) {
                std::string escaped;
                for (const char ch : name) escaped += ch == '\\' ? "\\\\"s : ch == '\n' ? "\\n"s : ch == '\r' ? "\\r"s : std::string(1, ch);
                out += "\\" + digest + "  " + escaped + "\n";
            } else {
                out += digest + "  " + name + "\n";
            }
        }
        // In pieces, as a channel hands them out.
        for (std::size_t at = 0; at < out.size(); at += 7) c.out.push_back(out.substr(at, 7));
        c.status = status;
    }

    struct Session : ssh::Session {
        Session(Box& b, int n) : box(b), login(n), client(fake) {
            fake.shared_files = &b.files;
            fake.shared_mtimes = &b.mtimes;
            client.start();
        }
        sftp::Client& sftp() override {
            started = true;
            return client;
        }
        std::expected<ssh::ExecResult, ssh::ExecError> exec(std::string_view command,
                                                             const ssh::ExecOptions& options) override {
            {
                std::scoped_lock lock(box.mutex);
                box.commands.emplace_back(command);
            }
            if (box.lose && box.lose(login, command)) {
                dropped = true;
                throw ssh::Error(budget::Failure::other, "the server stopped answering (nothing for 45 s)");
            }
            test::FakeCommand c;
            box.md5sum(c, command);
            if (box.refuse) c.refuse = true;
            if (box.one_channel && started) c.refuse_channel = "Channel open failure (connect failed)";
            if (box.also) box.also(c, command);
            const int now = ++box.running;
            for (int seen = box.most; now > seen && !box.most.compare_exchange_weak(seen, now);) {
            }
            auto r = ssh::run_command(c, command, options);
            --box.running;
            dropped = dropped || c.dropped;
            return r;
        }
        bool reusable() override { return !dropped && client.pending() == 0; }
        Box& box;
        int login;
        bool started = false; // its SFTP was used
        bool dropped = false;
        test::FakeServer fake;
        sftp::Client client;
    };

    std::shared_ptr<ssh::Pool> pool(int limit = 2, int hashes_at_once = 1,
                                    std::chrono::milliseconds expiry = ssh::find_check_fresh) {
        auto o = ssh::Pools::download_options(limit);
        o.idle_timer = false;
        o.hashes_at_once = hashes_at_once;
        o.find_check_expiry = expiry;
        return std::make_shared<ssh::Pool>([this]() -> std::unique_ptr<ssh::Session> {
            const int n = ++logins;
            if (no_login && n >= fail_logins_from) throw *no_login;
            return std::make_unique<Session>(*this, n);
        }, o);
    }

    // `content` at /home/test/<path>, as a file to hash from the login folder.
    ssh::HashFile file(const std::string& path, std::string content) {
        const auto size = content.size();
        files["/home/test/" + path] = std::move(content);
        return ssh::HashFile{.path = path, .size = size};
    }
    [[nodiscard]] std::ptrdiff_t count(std::string_view command) {
        std::scoped_lock lock(mutex);
        return std::ranges::count(commands, command);
    }
    // The md5sum commands that name files (not the probes).
    std::vector<std::string> hashes() {
        std::scoped_lock lock(mutex);
        std::vector<std::string> out;
        for (const auto& c : commands)
            if (c != ssh::hash_probe && c != ssh::hash_probe_plain) out.push_back(c);
        return out;
    }
};

inline bool is_probe(std::string_view command) { return command == ssh::hash_probe || command == ssh::hash_probe_plain; }

} // namespace grab::test
