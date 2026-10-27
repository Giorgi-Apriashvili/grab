#include "verify.hpp"

#include "config.hpp"
#include "listing.hpp"
#include "rclone_encoding.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_hash.hpp"
#include "util.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <expected>
#include <format>
#include <mutex>
#include <numeric>
#include <optional>
#include <span>
#include <stop_token>
#include <system_error>
#include <thread>
#include <utility>

namespace grab::verify {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view stopped = "stopped";

// The file's path on the server as raw bytes, from the login folder or absolute.
std::string raw_of(const Request& r, const File& f) {
    return rclone_enc::raw_path(r.remote_dir.empty() ? f.path : join_remote(r.remote_dir, f.path));
}

// -v: an md5sum command in rclone-command style, by the folder holding its paths and their number
// (a file alone by its path), in grab's form, so a name's control bytes can't rewrite the console.
std::vector<std::string> exec_line(std::span<const std::string> paths, bool z) {
    std::vector<std::string> argv{"exec", "md5sum"};
    if (z) argv.emplace_back("-z");
    if (paths.size() == 1) {
        argv.push_back(rclone_enc::from_raw_path(paths.front()));
        return argv;
    }
    std::string_view common = paths.front();
    for (const auto& p : paths) {
        std::size_t n = 0;
        while (n < common.size() && n < p.size() && common[n] == p[n]) ++n;
        common = common.substr(0, n);
    }
    const auto slash = common.rfind('/');
    common = slash == std::string_view::npos ? std::string_view{} : common.substr(0, slash == 0 ? 1 : slash);
    argv.push_back(rclone_enc::from_raw_path(common));
    argv.push_back(std::format("({} files)", paths.size()));
    return argv;
}

std::optional<std::int64_t> seconds_of(std::string_view modtime) {
    const auto ns = util::parse_rfc3339(modtime);
    if (!ns) return std::nullopt;
    // Whole seconds, as SFTP gives them, rounded down also before 1970.
    return *ns >= 0 ? *ns / 1'000'000'000 : -((-*ns + 999'999'999) / 1'000'000'000);
}

// Whether the server's file is as the download fetched it: its size, and its time where the
// download knew one.
bool server_unchanged(const sftp::Attrs& now, const File& f) {
    if (now.size != f.size) return false;
    const auto then = seconds_of(f.modtime);
    if (!then) return !now.mtime;
    return now.mtime && std::int64_t{*now.mtime} == *then;
}

// The differing ones (indices into r.files), proven or kept: one SFTP STAT each on one connection,
// then a look at the copy. Anything that isn't proven keeps the copy.
void prove(const Request& r, std::span<const std::size_t> differing, const std::stop_token& stop,
           const std::function<void(std::size_t, Result)>& give) {
    auto keep_all = [&](std::size_t from, const std::string& why) {
        for (std::size_t j = from; j < differing.size(); ++j) give(differing[j], Result{Verdict::unchecked, why});
    };
    std::optional<ssh::Pool::Lease> lease;
    bool retried = false; // a reused connection broke and the next one was tried
    for (std::size_t j = 0; j < differing.size();) {
        if (stop.stop_requested()) return keep_all(j, std::string(stopped));
        const File& f = r.files[differing[j]];
        const std::string raw = raw_of(r, f);
        std::expected<sftp::Attrs, sftp::Status> now;
        try {
            if (!lease) lease = r.pool->acquire(stop, ssh::interactive_retry);
            if (!lease) return keep_all(j, std::string(stopped));
            if (r.on_command) r.on_command({"sftp", "stat", rclone_enc::from_raw_path(raw)});
            now = sftp::stat((*lease)->sftp(), raw);
        } catch (const ssh::Error& e) {
            return keep_all(j, std::format("the server's copy can't be looked at: {}", e.what()));
        } catch (const sftp::Unavailable& e) {
            if (lease) lease->broken();
            return keep_all(j, std::format("the server's copy can't be looked at: {}", e.what()));
        } catch (const sftp::Broken& e) {
            if (!lease) return keep_all(j, std::format("the server's copy can't be looked at: {}", e.what()));
            lease->broken();
            // A reused connection the server closed while it sat idle: the next one, once.
            const bool again = lease->reused() && !std::exchange(retried, true);
            lease.reset();
            if (again) continue;
            return keep_all(j, std::format("the server's copy can't be looked at: {}", e.what()));
        }
        if (!now) {
            give(differing[j], Result{Verdict::unchecked,
                                      "the server's copy can't be looked at: " + sftp::describe(now.error())});
        } else if (!server_unchanged(*now, f)) {
            give(differing[j], Result{Verdict::unchecked, "the server's copy changed since the download"});
        } else if (!copy_unchanged(f.local, f.size, f.modtime)) {
            give(differing[j], Result{Verdict::unchecked, "your copy changed since the download"});
        } else {
            give(differing[j], Result{Verdict::differs, {}});
        }
        ++j;
    }
}

} // namespace

std::size_t Report::count(Verdict v) const {
    return static_cast<std::size_t>(std::ranges::count(files, v, &Result::verdict));
}

bool copy_unchanged(const fs::path& local, std::uint64_t size, std::string_view modtime) {
    std::error_code ec;
    const auto now = fs::file_size(local, ec);
    if (ec || now != size) return false;
    const auto then = util::parse_rfc3339(modtime);
    if (!then) return modtime.empty(); // the download set no time
    const auto written = fs::last_write_time(local, ec);
    if (ec) return false;
    const auto since_1970 = std::chrono::clock_cast<std::chrono::system_clock>(written).time_since_epoch();
    const std::int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(since_1970).count();
    constexpr std::int64_t slack = 2'000'000'000; // FAT keeps times to 2 s
    return ns >= *then - slack && ns <= *then + slack;
}

Report run(const Request& r, std::stop_token stop) {
    Report report;
    report.files.assign(r.files.size(), Result{Verdict::unchecked, std::string(stopped)});
    std::mutex mutex; // report, `settled` and the hooks
    std::size_t settled = 0;
    // A file's result, under `mutex`.
    auto give_locked = [&](std::size_t i, Result result) {
        report.files[i] = std::move(result);
        ++settled;
        if (r.on_file) r.on_file(i, report.files[i], settled);
        if (r.on_progress) r.on_progress(settled, r.files.size());
    };
    auto give = [&](std::size_t i, Result result) {
        std::scoped_lock lock(mutex);
        give_locked(i, std::move(result));
    };

    // A copy that is gone fails at once (nothing to delete; downloading it again brings it back).
    std::vector<std::size_t> hashed; // indices into r.files, hashed on both sides
    for (std::size_t i = 0; i < r.files.size(); ++i) {
        std::error_code ec;
        if (!fs::is_regular_file(r.files[i].local, ec)) give(i, Result{Verdict::missing, "the downloaded copy is gone"});
        else hashed.push_back(i);
    }
    if (hashed.empty()) return report;
    if (!r.pool) {
        const std::string why = r.no_pool.empty() ? "grab can't log in to the server" : r.no_pool;
        for (const std::size_t i : hashed) give(i, Result{Verdict::unchecked, why});
        return report;
    }

    // Seats from the server's budget, as walks take them (review G191): one for a file; for a
    // folder up to its share, no more than the hash commands the pool runs at once.
    int held = 1;
    if (r.connections != nullptr) {
        if (!r.joined) r.connections->join(r.server, r.id);
        held = r.many ? r.connections->reserve(r.server, r.id, stop, std::max(r.pool->hashes_at_once(), 1))
                      : (r.connections->acquire(r.server, r.id, stop, false) ? 1 : 0);
    }
    auto done_with_seats = [&] {
        if (r.connections == nullptr) return;
        if (held > 0) r.connections->release(r.server, r.id, held);
        if (!r.joined) r.connections->leave(r.server, r.id);
    };
    if (held == 0) {
        for (const std::size_t i : hashed) give(i, Result{Verdict::unchecked, std::string(stopped)});
        done_with_seats();
        return report;
    }

    // Both sides at once: the server's md5sum on this thread, grab's MD5 of the copies beside it.
    struct Sides {
        std::optional<ssh::HashResult> server;
        std::optional<std::expected<std::string, std::string>> local;
    };
    std::vector<Sides> sides(hashed.size());
    std::vector<std::size_t> differing; // indices into r.files whose sums differ, proven at the end
    // hashed[k]'s result once both sides are known, under `mutex`.
    auto pair_up = [&](std::size_t k) {
        const Sides& s = sides[k];
        if (!s.server || !s.local) return;
        const std::size_t i = hashed[k];
        if (!*s.server) return give_locked(i, Result{Verdict::unchecked, s.server->error().why});
        if (!*s.local) {
            const std::string& e = s.local->error();
            return give_locked(i, Result{Verdict::unchecked, e == stopped ? e : "your copy can't be read: " + e});
        }
        if (**s.server == **s.local) return give_locked(i, Result{Verdict::same, {}});
        differing.push_back(i);
    };

    std::vector<ssh::HashFile> files;
    for (const std::size_t i : hashed) files.push_back(ssh::HashFile{.path = raw_of(r, r.files[i]), .size = r.files[i].size});
    // The copies in the order the server takes them: the biggest first.
    std::vector<std::size_t> order(hashed.size());
    std::ranges::iota(order, std::size_t{0});
    std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) { return files[a].size > files[b].size; });
    // A copy's hash stops once its server side failed: the result is the server's why anyway, and
    // a failed login (one for every file) would otherwise still read all of them from the disk.
    std::vector<std::stop_source> cancel(hashed.size());
    std::atomic<std::size_t> next{0};
    auto hash_copies = [&] {
        for (std::size_t j = next++; j < order.size(); j = next++) {
            const std::size_t k = order[j];
            std::stop_callback forward(stop, [&cancel, k] { cancel[k].request_stop(); });
            util::FileHashOptions o;
            o.stop = cancel[k].get_token();
            auto sum = util::md5_file(r.files[hashed[k]].local, o);
            std::scoped_lock lock(mutex);
            sides[k].local = std::move(sum);
            pair_up(k);
        }
    };
    {
        std::vector<std::jthread> copies;
        const auto workers = std::min(order.size(), static_cast<std::size_t>(util::local_hashes_at_once));
        for (std::size_t w = 0; w < workers; ++w) copies.emplace_back(hash_copies);

        ssh::HashOptions o;
        o.stop = stop;
        o.connections = held;
        o.deadline = r.deadline;
        if (r.ctx != nullptr) o.server = r.ctx->remote;
        o.retry_refused = ssh::interactive_retry; // a verification waited on, as Browse is
        o.done = [&](std::size_t k, const ssh::HashResult& result) {
            std::scoped_lock lock(mutex);
            sides[k].server = result;
            if (!result) cancel[k].request_stop();
            pair_up(k);
        };
        if (r.on_command) o.on_command = [&](std::span<const std::string> paths, bool z) { r.on_command(exec_line(paths, z)); };
        const auto sums = ssh::run_hashes(*r.pool, files, o);
        std::scoped_lock lock(mutex);
        for (std::size_t k = 0; k < sides.size(); ++k) {
            if (sides[k].server) continue; // `done` gave every one; this only makes sure
            sides[k].server = sums[k];
            if (!sums[k]) cancel[k].request_stop();
            pair_up(k);
        }
    }
    if (!differing.empty()) {
        std::ranges::sort(differing);
        prove(r, differing, stop, give);
    }
    // A pool made only to verify (engine = rclone) drops its idle connections before the seats go
    // back: rclone's own logins don't count them, and could otherwise pass the server's cap.
    if (r.close_idle_after) r.pool->close_idle(ssh::Pool::Clock::now() + std::chrono::hours(1));
    done_with_seats();
    return report;
}

Report file(Request base, File copy, std::stop_token stop) {
    base.files = {std::move(copy)};
    base.many = false;
    return run(base, std::move(stop));
}

std::string summary(const Report& r, bool folder) {
    const std::size_t n = r.files.size();
    // Why the unchecked ones weren't verified: the reason they share, else the first one's.
    std::string why;
    bool one_reason = true;
    for (const auto& f : r.files) {
        if (f.verdict != Verdict::unchecked) continue;
        if (why.empty()) why = f.why;
        else if (f.why != why) one_reason = false;
    }
    if (!one_reason) why += ", and other reasons";
    if (!folder && n == 1) {
        switch (r.files.front().verdict) {
        case Verdict::same: return "checksum verified (MD5)";
        case Verdict::differs: return "differs from the server's copy (MD5)";
        case Verdict::missing: return "the downloaded copy is gone";
        case Verdict::unchecked: break;
        }
        return "not verified: " + why;
    }
    auto count = [](std::size_t k) { return std::format("{} file{}", k, k == 1 ? "" : "s"); };
    const std::size_t same = r.count(Verdict::same);
    const std::size_t differ = r.count(Verdict::differs);
    const std::size_t missing = r.count(Verdict::missing);
    const std::size_t unchecked = r.count(Verdict::unchecked);
    if (differ > 0 || missing > 0) {
        std::string text;
        if (differ > 0) {
            text = n == 1 ? "1 file differs from the server's copy (MD5)"
                          : std::format("{} of {} files differ from the server's copy (MD5)", differ, n);
        }
        if (missing > 0) {
            if (!text.empty()) text += "; ";
            text += missing == 1 ? "1 downloaded copy is gone" : std::format("{} downloaded copies are gone", missing);
        }
        return text;
    }
    if (unchecked > 0 && same == 0) return "not verified: " + why;
    if (unchecked > 0) return std::format("{} verified (MD5); {} could not be checked ({})", count(same), unchecked, why);
    return std::format("{} verified (MD5)", count(same));
}

} // namespace grab::verify
