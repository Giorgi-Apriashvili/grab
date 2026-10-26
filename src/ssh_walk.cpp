#include "ssh_walk.hpp"

#include "conn_budget.hpp"
#include "rclone_encoding.hpp"
#include "sftp.hpp"
#include "ssh.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <format>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <utility>

namespace grab::ssh {

namespace {

std::string utc(std::uint32_t seconds) {
    return std::format("{:%FT%TZ}", std::chrono::sys_seconds{std::chrono::seconds{seconds}});
}

std::string join(std::string_view dir, std::string_view name) {
    if (name.empty()) return std::string(dir);
    if (dir.empty() || dir == ".") return std::string(name);
    return dir.back() == '/' ? std::string(dir) + std::string(name) : std::string(dir) + "/" + std::string(name);
}

// Whether `path` is `root` or lies inside it (both server paths, as REALPATH gives them).
bool inside(std::string_view path, std::string_view root) {
    if (root == "/") return true;
    return path == root || (path.starts_with(root) && path.size() > root.size() && path[root.size()] == '/');
}

} // namespace

std::expected<Walk, std::string> walk(Pool& pool, std::string_view root, const WalkOptions& options,
                                      std::stop_token stop) {
    const std::string root_path = root.empty() ? std::string(".") : std::string(root);
    std::mutex m;
    std::condition_variable_any changed;
    std::deque<std::string> pending{std::string()}; // folders to read, relative to the root ("" = itself)
    int reading = 0;                                 // workers reading a folder now
    Walk result;
    std::optional<std::string> error;
    std::optional<std::string> root_real;     // REALPATH of the root, once known
    std::set<std::string> outside;            // REALPATHs of linked folders walked already

    std::string last_failure; // why the last worker gave up, if the folders outlast them all

    // A worker holds a connection only while folders wait to be read: an idle one gives its back
    // before waiting, and gets one before taking a folder, so a worker waiting for a connection
    // never holds a folder up (the pool may be at its limit, with downloads holding the rest).
    auto worker = [&] {
        std::optional<Pool::Lease> lease;
        int broken_in_a_row = 0;
        for (;;) {
            {
                std::unique_lock lock(m);
                if (pending.empty() && lease) {
                    lock.unlock();
                    lease.reset();
                    lock.lock();
                }
                changed.wait(lock, stop, [&] { return error || !pending.empty() || reading == 0; });
                if (stop.stop_requested() || error || pending.empty()) return; // done, or failed
            }
            if (!lease) {
                try {
                    lease = pool.acquire(stop);
                } catch (const Error& e) {
                    // Refused for good ends the walk; turned away (a full server), this worker
                    // leaves the folders to the others.
                    std::lock_guard lock(m);
                    last_failure = e.what();
                    if (e.kind == budget::Failure::fatal && !error) error = e.what();
                    changed.notify_all();
                    return;
                }
                if (!lease) return; // stopped
            }
            std::string rel;
            {
                std::lock_guard lock(m);
                if (pending.empty()) continue; // another worker took it meanwhile
                rel = std::move(pending.front());
                pending.pop_front();
                ++reading;
            }
            std::vector<Walked> found;
            std::vector<std::string> folders;
            std::optional<std::string> unreadable;
            bool again = false; // the folder goes back to the queue
            bool quit = false;  // this worker stops: its connection keeps breaking
            try {
                sftp::Client& c = (*lease)->sftp();
                const std::string dir = join(root_path, rel);
                const std::string raw_dir = rclone_enc::raw_path(dir);
                if (rel.empty()) {
                    auto real = sftp::realpath(c, raw_dir);
                    std::lock_guard lock(m);
                    if (real) root_real = *real;
                }
                auto entries = sftp::list(c, raw_dir);
                if (!entries) {
                    if (rel.empty()) {
                        std::lock_guard lock(m);
                        if (!error) error = std::format("cannot list {}: {}", root_path, sftp::describe(entries.error()));
                    } else {
                        unreadable = std::format("{}: {}", rel, sftp::describe(entries.error()));
                    }
                } else {
                    for (const auto& e : *entries) {
                        const std::string name = rclone_enc::from_raw_name(e.name);
                        const std::string child = rel.empty() ? name : rel + "/" + name;
                        sftp::Attrs a = e.attrs;
                        if (a.is_link()) {
                            // Followed as rclone does: what it leads to decides.
                            const std::string raw_child = raw_dir + "/" + e.name;
                            auto target = sftp::stat(c, raw_child);
                            if (!target) continue; // a broken link: nothing to fetch
                            a = *target;
                            if (a.is_dir()) {
                                auto real = sftp::realpath(c, raw_child);
                                std::lock_guard lock(m);
                                // Into the tree itself (a loop, or a folder walked anyway), or a
                                // folder another link led to already: not again.
                                if (!real || (root_real && inside(*real, *root_real)) || !outside.insert(*real).second) continue;
                            }
                        }
                        if (a.is_dir()) {
                            folders.push_back(child);
                            if (options.folders) found.push_back(Walked{child, 0, a.mtime ? utc(*a.mtime) : std::string(), true});
                        } else if (a.is_file()) {
                            found.push_back(Walked{child, a.size.value_or(0), a.mtime ? utc(*a.mtime) : std::string(), false});
                        }
                        // Sockets, devices and pipes hold nothing to download.
                    }
                }
                broken_in_a_row = 0;
            } catch (const sftp::Broken& e) {
                if (lease) lease->broken();
                lease.reset();
                again = true; // read again on a new connection
                quit = ++broken_in_a_row >= 3;
                std::lock_guard lock(m);
                last_failure = e.what();
            }
            {
                std::lock_guard lock(m);
                --reading;
                if (again) {
                    pending.push_back(std::move(rel));
                } else {
                    result.entries.insert(result.entries.end(), std::make_move_iterator(found.begin()),
                                          std::make_move_iterator(found.end()));
                    for (auto& f : folders) pending.push_back(std::move(f));
                    if (unreadable) result.unreadable.push_back(std::move(*unreadable));
                }
            }
            changed.notify_all();
            if (quit) return;
        }
    };
    {
        std::vector<std::jthread> workers;
        for (int i = 0; i < std::max(1, options.connections); ++i) workers.emplace_back(worker);
    }
    if (stop.stop_requested()) return Walk{};
    if (error) return std::unexpected(std::move(*error));
    if (!pending.empty()) return std::unexpected("the listing couldn't finish: " + last_failure);
    std::ranges::sort(result.entries, {}, &Walked::path);
    std::ranges::sort(result.unreadable);
    return result;
}

} // namespace grab::ssh
