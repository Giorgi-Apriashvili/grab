#pragma once

// What the live cases that write on a server share (test_live_upload.cpp, test_live_ops.cpp): their
// "hook: " lines, names no earlier run left, each answer's code, the test files' bytes and MD5, the
// server's md5sum, and a file written from several connections at once.

#include "conn_budget.hpp"
#include "rclone_encoding.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_pool.hpp"
#include "util.hpp"

#include <doctest/doctest.h>
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
#include <format>
#include <latch>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace grab::test::live {

using Clock = std::chrono::steady_clock;

inline double seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }

inline void say(const std::string& line) {
    std::fputs(("hook: " + line + "\n").c_str(), stdout);
    std::fflush(stdout);
}

inline constexpr std::uint64_t mib = std::uint64_t{1} << 20;

// `name` in the folder `dir` (grab's form, "." the login folder), as raw bytes.
inline std::string in_dir(std::string_view dir, std::string_view name) {
    if (dir.empty() || dir == ".") return std::string(name);
    std::string raw = rclone_enc::raw_path(dir);
    if (!raw.ends_with('/')) raw += '/';
    return raw + std::string(name);
}

// Six random hex digits, for names no earlier run left.
inline std::string token() {
    std::random_device rd;
    return std::format("{:06x}", rd() & 0xFFFFFF);
}

// A request's outcome, as printed: 0, or the code and the server's own text.
template <class T> std::string code_of(const std::expected<T, sftp::Status>& r) {
    if (r) return "0";
    return std::format("{} \"{}\"", static_cast<std::uint32_t>(r.error().code), rclone_enc::shown_line(r.error().message));
}

// The bytes of the files written: a 4 MiB random block, each 4 MiB of a file XORed with its number,
// so no two blocks of a file (up to 1 GiB) are alike and a server that compresses or deduplicates
// gains nothing.
class Data {
public:
    static constexpr std::size_t block = std::size_t{4} << 20;
    explicit Data(std::uint64_t seed) : block_(block) {
        std::mt19937_64 rng(seed);
        for (std::size_t i = 0; i < block; i += 8) {
            const std::uint64_t v = rng();
            std::memcpy(block_.data() + i, &v, 8);
        }
    }
    void fill(std::uint64_t offset, std::span<char> out) const {
        std::size_t done = 0;
        while (done < out.size()) {
            const std::uint64_t at = offset + done;
            const auto in_block = static_cast<std::size_t>(at % block);
            const std::size_t n = std::min(out.size() - done, block - in_block);
            const auto key = static_cast<char>((at / block) * 0x9D + 1);
            for (std::size_t i = 0; i < n; ++i) out[done + i] = static_cast<char>(block_[in_block + i] ^ key);
            done += n;
        }
    }
    // MD5 of the first `size` bytes, as md5sum prints it.
    [[nodiscard]] std::string md5(std::uint64_t size) const {
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        REQUIRE(ctx != nullptr);
        REQUIRE(EVP_DigestInit_ex(ctx, EVP_md5(), nullptr) == 1);
        std::vector<char> buf(mib);
        for (std::uint64_t at = 0; at < size; at += buf.size()) {
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(buf.size(), size - at));
            fill(at, std::span(buf.data(), n));
            REQUIRE(EVP_DigestUpdate(ctx, buf.data(), n) == 1);
        }
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
        unsigned int len = 0;
        REQUIRE(EVP_DigestFinal_ex(ctx, digest.data(), &len) == 1);
        EVP_MD_CTX_free(ctx);
        std::string out;
        for (unsigned int i = 0; i < len; ++i) out += std::format("{:02x}", digest[i]);
        return out;
    }

private:
    std::vector<char> block_;
};

// MD5 of bytes, as md5sum prints it.
inline std::string md5_of(std::string_view bytes) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    REQUIRE(EVP_Digest(bytes.data(), bytes.size(), digest.data(), &size, EVP_md5(), nullptr) == 1);
    std::string out;
    for (unsigned int i = 0; i < size; ++i) out += std::format("{:02x}", digest[i]);
    return out;
}

// [0, size) in `parts` ranges, the last taking what is left.
inline std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges_of(std::uint64_t size, int parts) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
    const std::uint64_t each = size / static_cast<std::uint64_t>(parts);
    for (int i = 0; i < parts; ++i) {
        const std::uint64_t from = each * static_cast<std::uint64_t>(i);
        out.emplace_back(from, i + 1 == parts ? size - from : each);
    }
    return out;
}

inline std::uint64_t number_from_env(const char* name, std::uint64_t fallback) {
    const auto v = util::getenv_utf8(name);
    return v && !v->empty() ? std::stoull(*v) : fallback;
}

// The server's md5sum of `raw` (a plain name: letters, digits, '.', '-', '_', '/'), or why not.
inline std::expected<std::string, std::string> server_md5(ssh::Pool& pool, const std::string& raw) {
    try {
        auto lease = pool.acquire({}, ssh::interactive_retry);
        if (!lease) return std::unexpected(std::string("no connection"));
        std::string out;
        ssh::ExecOptions o;
        o.out = [&](std::string_view piece) {
            out.append(piece.substr(0, 4096 - std::min<std::size_t>(4096, out.size())));
            return true;
        };
        o.dead_after = std::chrono::hours(1);
        o.deadline = std::chrono::minutes(10);
        auto r = (*lease)->exec("md5sum " + raw, o);
        if (!r) return std::unexpected("not run: " + r.error().message);
        if (r->status != 0 || out.size() < 32) return std::unexpected(std::format("exit {}", r->status));
        return out.substr(0, 32);
    } catch (const ssh::Error& e) {
        return std::unexpected(std::string("connection: ") + e.what());
    }
}

// A small file's bytes read over SFTP (at most `most`), or the failure's code.
inline std::expected<std::string, sftp::Status> read_small(sftp::Client& s, const std::string& raw, std::uint64_t most = mib) {
    auto h = sftp::open_file(s, raw);
    if (!h) return std::unexpected(h.error());
    auto attrs = sftp::fstat(s, *h);
    std::string got(static_cast<std::size_t>(std::min<std::uint64_t>(attrs && attrs->size ? *attrs->size : 0, most)), '\0');
    auto r = sftp::read_range(s, *h, 0, got.size(), [&](std::uint64_t at, std::string_view bytes) {
        std::memcpy(got.data() + at, bytes.data(), bytes.size());
    });
    if (auto closed = sftp::close_file(s, *h); !closed) {
        // A handle that fails to close doesn't spoil what was read.
    }
    if (!r) return std::unexpected(r.error());
    return got;
}

// A new file of `data` (created exclusively), closed.
inline std::expected<void, sftp::Status> create_file(sftp::Client& s, const std::string& raw, std::string_view data) {
    auto h = sftp::open_file(s, raw, sftp::OpenFlags::write | sftp::OpenFlags::creat | sftp::OpenFlags::excl);
    if (!h) return std::unexpected(h.error());
    auto written = sftp::write_range(s, *h, 0, data.size(),
                                     [&](std::uint64_t at, std::uint32_t n) { return data.substr(at, n); });
    auto closed = sftp::close_file(s, *h);
    if (!written) return written;
    return closed;
}

inline const char* yes(bool b) { return b ? "yes" : "NO"; }

// One connection's part of a file written at its offsets: OPEN (no creat, no trunc), write_range,
// CLOSE. Its bytes, its time from the start to the last acknowledgement, or what failed.
struct Part {
    std::uint64_t bytes = 0;
    double took = 0;
    std::string failed;
    int login_retries = 0;
};

// A lease, its login tried again (2 s apart, up to 3 times in all) when it failed for a reason
// that may pass: a busy server drops a connect now and then among many logins in a row. Before
// the start, so the time measured doesn't include it.
inline std::optional<ssh::Pool::Lease> acquire_retrying(ssh::Pool& pool, int& retries) {
    for (int attempt = 1;; ++attempt) {
        try {
            return pool.acquire({}, ssh::interactive_retry);
        } catch (const ssh::Error& e) {
            if (e.kind == budget::Failure::fatal || attempt == 3) throw;
            ++retries;
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }
}

// `conns` connections write the file `raw` (already made, empty) of `size` bytes in as many ranges,
// each with its own lease, all starting together once every one has its login and handle. The
// parts and the time from the start to the last acknowledgement.
inline std::pair<std::vector<Part>, double> write_parallel(ssh::Pool& pool, const std::string& raw, std::uint64_t size, int conns,
                                                    const Data& data, int depth, std::uint64_t unacked, std::uint32_t chunk) {
    const auto ranges = ranges_of(size, conns);
    std::vector<Part> parts(static_cast<std::size_t>(conns));
    std::latch ready(conns);
    std::latch go(1);
    std::atomic<std::int64_t> last_ns{0};
    Clock::time_point started;
    std::vector<std::jthread> threads;
    for (int i = 0; i < conns; ++i) {
        threads.emplace_back([&, i] {
            Part& p = parts[static_cast<std::size_t>(i)];
            bool counted = false;
            const auto arrive = [&] {
                if (!counted) ready.count_down();
                counted = true;
            };
            try {
                auto lease = acquire_retrying(pool, p.login_retries);
                if (!lease) {
                    p.failed = "no connection";
                    arrive();
                    return;
                }
                try {
                    auto& s = (*lease)->sftp();
                    auto h = sftp::open_file(s, raw, sftp::OpenFlags::write);
                    arrive();
                    go.wait();
                    if (!h) {
                        p.failed = "OPEN " + code_of(h);
                        return;
                    }
                    const auto [from, length] = ranges[static_cast<std::size_t>(i)];
                    std::vector<char> buf(chunk);
                    auto r = sftp::write_range(
                        s, *h, from, length,
                        [&](std::uint64_t at, std::uint32_t n) {
                            data.fill(at, std::span(buf.data(), n));
                            return std::string_view(buf.data(), n);
                        },
                        [&](std::uint64_t, std::uint32_t n) { p.bytes += n; }, depth, unacked, chunk);
                    const auto now = Clock::now();
                    p.took = seconds(now - started);
                    std::int64_t ns = (now - started).count();
                    for (std::int64_t seen = last_ns.load(); ns > seen && !last_ns.compare_exchange_weak(seen, ns);) {
                    }
                    if (!r) p.failed = "WRITE " + code_of(r);
                    if (auto closed = sftp::close_file(s, *h); !closed && p.failed.empty()) p.failed = "CLOSE " + code_of(closed);
                } catch (const sftp::Broken& e) {
                    lease->broken();
                    p.failed = std::string("connection lost: ") + e.what();
                    arrive();
                }
            } catch (const ssh::Error& e) {
                p.failed = std::string("no login: ") + e.what();
                arrive();
            }
        });
    }
    ready.wait();
    started = Clock::now();
    go.count_down();
    threads.clear();
    return {std::move(parts), std::chrono::duration<double>(Clock::duration(last_ns.load())).count()};
}

inline const char* pass(bool b) { return b ? "PASS" : "FAIL"; }

} // namespace grab::test::live
