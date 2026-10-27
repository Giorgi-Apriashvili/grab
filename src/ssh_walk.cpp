#include "ssh_walk.hpp"

#include "conn_budget.hpp"
#include "rclone.hpp"
#include "rclone_encoding.hpp"
#include "ssh.hpp"
#include "util.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <format>
#include <iterator>
#include <list>
#include <mutex>
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

Walked entry(std::string path, const sftp::Attrs& a) {
    const bool dir = a.is_dir();
    return Walked{.path = std::move(path),
                  .size = dir ? 0 : a.size.value_or(0),
                  .modtime = a.mtime ? utc(*a.mtime) : std::string(),
                  .dir = dir,
                  .modified = a.mtime ? std::optional<std::int64_t>(*a.mtime) : std::nullopt};
}

// A folder to read: the root it lies under, its path below that root, and how deep.
struct Folder {
    std::size_t root = 0;
    std::string rel; // "" = the root itself
    int depth = 0;   // levels below the root
};

// READDIRs go out in rounds, the next once the last one is all in: two at first, so a folder of
// one batch (most) comes with its end in one round trip, then twice as many while batches keep
// coming, up to this many, so a folder of 18,000 names (180 batches from OpenSSH) takes about 25
// round trips instead of 180.
constexpr int max_readdirs = 8;

// A folder being read on a worker's connection. Its requests are in flight together with those
// of the worker's other folders (the root's REALPATH, OPENDIR, READDIRs, a STAT per link and a
// REALPATH per link to a folder); it's read once none is left.
struct Reading {
    Folder folder;
    std::string raw_dir;
    std::string handle;
    int readdirs = 0;                  // READDIRs in flight on `handle`
    int round = 2;                     // how many the next round sends
    bool listed = false;               // the end came (or a failure): no more READDIRs, then CLOSE
    int waiting = 0;                   // replies still to come
    std::optional<std::string> failed; // why it couldn't be read
    std::optional<std::string> real;   // the root's REALPATH (roots only)
    std::vector<Walked> found;         // paths relative to the root
    std::vector<Folder> folders;       // folders below it to read
    // Links to folders, walked or not once their REALPATHs are in (into the tree, or walked already).
    struct Link {
        Walked entry;
        Folder folder;
        std::optional<std::string> real;
    };
    std::vector<Link> links;
};

class Walker {
public:
    Walker(Pool& pool, std::span<const std::string> roots, const WalkOptions& options, bool several,
           std::stop_token stop)
        : pool_(pool), roots_(roots.begin(), roots.end()), options_(options), several_(several),
          stop_(std::move(stop)), workers_(std::max(1, options.connections)),
          in_flight_(static_cast<std::size_t>(std::max(1, options.in_flight))), root_real_(roots_.size()),
          outside_(roots_.size()) {
        for (std::size_t i = 0; i < roots_.size(); ++i) pending_.push_back(Folder{.root = i, .rel = {}, .depth = 0});
    }

    std::expected<Walk, std::string> run() {
        {
            std::vector<std::jthread> workers;
            for (int i = 0; i < workers_; ++i) workers.emplace_back([this] { work(); });
        }
        if (stop_.stop_requested()) return Walk{};
        if (error_) return std::unexpected(std::move(*error_));
        if (!pending_.empty()) return std::unexpected("the listing couldn't finish: " + last_failure_);
        std::ranges::sort(result_.unreadable);
        if (several_ && failed_roots_ == roots_.size())
            return std::unexpected("cannot list " + util::join(result_.unreadable, "; "));
        std::ranges::sort(result_.entries, {}, &Walked::path);
        return std::move(result_);
    }

private:
    [[nodiscard]] std::string root_path(std::size_t root) const {
        return roots_[root].empty() ? std::string(".") : roots_[root];
    }
    // A folder as messages name it: for walk() relative to the root (the root by its own path),
    // for walk_roots() joined onto its root ("~" for the login folder itself).
    [[nodiscard]] std::string shown(const Folder& f) const {
        if (!several_) return f.rel.empty() ? root_path(f.root) : f.rel;
        std::string path = join(roots_[f.root], f.rel);
        return path.empty() ? std::string("~") : path;
    }

    void fail(std::string why) {
        {
            std::scoped_lock lock(m_);
            if (!error_) error_ = std::move(why);
        }
        changed_.notify_all();
    }

    void work();
    void start(sftp::Client& c, std::list<Reading>& here, Folder folder);
    void read_more(sftp::Client& c, Reading& r);
    // false past max_bytes.
    [[nodiscard]] bool take(sftp::Client& c, Reading& r, const sftp::Entry& e);
    void add(Reading& r, std::string rel, int depth, const sftp::Attrs& a) const;
    void finish(Reading& r);

    Pool& pool_;
    const std::vector<std::string> roots_;
    const WalkOptions& options_;
    const bool several_;
    const std::stop_token stop_;
    const int workers_;
    const std::size_t in_flight_;
    std::atomic<std::size_t> looked_{0}; // names read
    std::atomic<std::uint64_t> bytes_{0}; // of the paths kept

    std::mutex m_;
    std::condition_variable_any changed_;
    std::deque<Folder> pending_; // folders to read
    int reading_ = 0;            // folders taken and not read yet
    Walk result_;
    std::optional<std::string> error_;
    std::string last_failure_;                          // why the last worker gave up, if the folders outlast them all
    std::size_t failed_roots_ = 0;
    std::vector<std::optional<std::string>> root_real_; // each root's REALPATH, once known
    std::vector<std::set<std::string>> outside_;        // per root: REALPATHs of linked folders walked already
};

// A worker holds a connection only while folders wait to be read: an idle one gives its back
// before waiting, and gets one before taking folders, so a worker waiting for a connection never
// holds folders up (the pool may be at its limit, with downloads holding the rest).
void Walker::work() {
    std::optional<Pool::Lease> lease;
    std::list<Reading> here; // the folders in flight on this worker's connection
    int broken_in_a_row = 0;
    // Gives the connection back once the CLOSEs still on their way are in, so it can be reused.
    auto give_back = [&] {
        try {
            sftp::Client& c = (*lease)->sftp();
            while (c.pending() > 0) c.pump();
        } catch (const sftp::Broken&) {
            lease->broken();
        }
        lease.reset();
    };
    for (;;) {
        {
            std::unique_lock lock(m_);
            if (here.empty()) {
                if (pending_.empty() && lease) {
                    lock.unlock();
                    give_back();
                    lock.lock();
                }
                changed_.wait(lock, stop_, [&] { return error_ || !pending_.empty() || reading_ == 0; });
                if (stop_.stop_requested() || error_ || pending_.empty()) return; // done, or failed
            } else if (stop_.stop_requested() || error_) {
                return; // what's in flight goes with the connection, which isn't reused
            }
        }
        if (!lease) {
            try {
                lease = pool_.acquire(stop_);
            } catch (const Error& e) {
                // Refused for good ends the walk; turned away (a full server), this worker
                // leaves the folders to the others.
                std::scoped_lock lock(m_);
                last_failure_ = e.what();
                if (e.kind == budget::Failure::fatal && !error_) error_ = e.what();
                changed_.notify_all();
                return;
            }
            if (!lease) return; // stopped
        }
        std::vector<Folder> taken;
        {
            std::scoped_lock lock(m_);
            // Its share of what waits, so that the other connections get some too.
            const auto workers = static_cast<std::size_t>(workers_);
            const std::size_t share = std::max<std::size_t>(1, (pending_.size() + workers - 1) / workers);
            for (std::size_t n = std::min(in_flight_ - here.size(), share); n > 0 && !pending_.empty(); --n) {
                taken.push_back(std::move(pending_.front()));
                pending_.pop_front();
                ++reading_;
            }
        }
        sftp::Client& c = (*lease)->sftp();
        for (auto& f : taken) start(c, here, std::move(f));
        if (here.empty()) continue; // another worker took them meanwhile
        try {
            c.pump();
            broken_in_a_row = 0;
        } catch (const sftp::Broken& e) {
            lease->broken();
            lease.reset();
            const bool quit = ++broken_in_a_row >= 3; // its connections keep breaking
            {
                std::scoped_lock lock(m_);
                last_failure_ = e.what();
                for (auto& r : here) pending_.push_back(std::move(r.folder)); // read again on a new connection
                reading_ -= static_cast<int>(here.size());
            }
            here.clear();
            changed_.notify_all();
            if (quit) return;
            continue;
        }
        std::list<Reading> done;
        for (auto it = here.begin(); it != here.end();) {
            auto next = std::next(it);
            if (it->waiting == 0) done.splice(done.end(), here, it);
            it = next;
        }
        if (done.empty()) continue;
        {
            std::scoped_lock lock(m_);
            for (auto& r : done) finish(r);
            if (options_.progress) options_.progress(looked_.load());
        }
        changed_.notify_all();
    }
}

void Walker::start(sftp::Client& c, std::list<Reading>& here, Folder folder) {
    Reading& r = here.emplace_back();
    r.folder = std::move(folder);
    r.raw_dir = rclone_enc::raw_path(join(root_path(r.folder.root), r.folder.rel));
    if (r.folder.rel.empty()) {
        ++r.waiting;
        c.realpath(r.raw_dir, [&r](std::expected<std::string, sftp::Status> real) {
            --r.waiting;
            if (real) r.real = std::move(*real);
        });
    }
    ++r.waiting;
    c.opendir(r.raw_dir, [this, &c, &r](std::expected<std::string, sftp::Status> handle) {
        --r.waiting;
        if (!handle) {
            r.failed = sftp::describe(handle.error());
            return;
        }
        r.handle = std::move(*handle);
        read_more(c, r);
    });
}

void Walker::read_more(sftp::Client& c, Reading& r) {
    if (r.listed || r.readdirs > 0) return; // the last round isn't all in yet
    for (int i = 0; i < r.round; ++i) {
        ++r.readdirs;
        ++r.waiting;
        c.readdir(r.handle, [this, &c, &r](std::expected<std::vector<sftp::Entry>, sftp::Status> batch) {
            --r.readdirs;
            --r.waiting;
            if (!batch) {
                // The end, or a failure before it; after the end, the READDIRs sent ahead answer
                // with it again.
                if (!r.listed && batch.error().code != sftp::Code::eof) r.failed = sftp::describe(batch.error());
                r.listed = true;
            } else if (looked_.fetch_add(batch->size()) + batch->size() > options_.max_entries) {
                fail(std::format("listing too large: more than {} entries", options_.max_entries));
                r.listed = true;
            } else {
                for (const auto& e : *batch) {
                    if (take(c, r, e)) continue;
                    fail(std::format("listing too large: more than {} of paths", util::format_size(options_.max_bytes)));
                    r.listed = true;
                    break;
                }
            }
            if (!r.listed) read_more(c, r);
            else if (r.readdirs == 0) c.close(r.handle); // its answer isn't waited for
        });
    }
    r.round = std::min(r.round * 2, max_readdirs);
}

bool Walker::take(sftp::Client& c, Reading& r, const sftp::Entry& e) {
    // ".", ".." and names with nothing to save them under are never entries (review G15).
    if (!has_name(e.name)) return true;
    if (options_.skip_hidden && e.name.starts_with('.')) return true;
    const int depth = r.folder.depth + 1;
    std::string rel = join(r.folder.rel, rclone_enc::from_raw_name(e.name));
    if (bytes_.fetch_add(rel.size()) + rel.size() > options_.max_bytes) return false;
    if (!e.attrs.is_link()) {
        add(r, std::move(rel), depth, e.attrs);
        return true;
    }
    // Followed as rclone does: what it leads to decides, and a broken link is nothing.
    std::string raw_child = join(r.raw_dir, e.name);
    ++r.waiting;
    c.stat(raw_child, [this, &c, &r, rel = std::move(rel), raw_child, depth](
                          std::expected<sftp::Attrs, sftp::Status> target) mutable {
        --r.waiting;
        if (!target) return;
        if (!target->is_dir()) {
            add(r, std::move(rel), depth, *target);
            return;
        }
        const std::size_t at = r.links.size();
        r.links.push_back(Reading::Link{.entry = entry(rel, *target),
                                        .folder = Folder{.root = r.folder.root, .rel = rel, .depth = depth},
                                        .real = std::nullopt});
        ++r.waiting;
        c.realpath(raw_child, [&r, at](std::expected<std::string, sftp::Status> real) {
            --r.waiting;
            if (real) r.links[at].real = std::move(*real);
        });
    });
    return true;
}

void Walker::add(Reading& r, std::string rel, int depth, const sftp::Attrs& a) const {
    if (a.is_dir()) {
        if (options_.folders) r.found.push_back(entry(rel, a));
        if (options_.max_depth <= 0 || depth < options_.max_depth)
            r.folders.push_back(Folder{.root = r.folder.root, .rel = std::move(rel), .depth = depth});
    } else if (a.is_file()) {
        r.found.push_back(entry(std::move(rel), a));
    }
    // Sockets, devices and pipes hold nothing to download.
}

// Called with m_ held.
void Walker::finish(Reading& r) {
    --reading_;
    const Folder& f = r.folder;
    if (r.failed) {
        std::string why = std::format("{}: {}", shown(f), *r.failed);
        if (f.rel.empty()) {
            if (!several_) {
                if (!error_) error_ = "cannot list " + why;
                return;
            }
            ++failed_roots_;
        }
        result_.unreadable.push_back(std::move(why));
        return;
    }
    if (f.rel.empty()) root_real_[f.root] = r.real;
    const auto& root_real = root_real_[f.root];
    for (auto& link : r.links) {
        // Into the tree itself (a loop, or a folder walked anyway), or a folder another link led
        // to already: not again.
        if (!link.real || (root_real && inside(*link.real, *root_real)) || !outside_[f.root].insert(*link.real).second)
            continue;
        if (options_.folders) r.found.push_back(std::move(link.entry));
        if (options_.max_depth <= 0 || link.folder.depth < options_.max_depth) r.folders.push_back(std::move(link.folder));
    }
    for (auto& e : r.found) {
        if (several_) e.path = join(roots_[f.root], e.path);
        result_.entries.push_back(std::move(e));
    }
    for (auto& sub : r.folders) pending_.push_back(std::move(sub));
}

} // namespace

std::expected<Walk, std::string> walk(Pool& pool, std::string_view root, const WalkOptions& options,
                                      std::stop_token stop) {
    const std::string one(root);
    return Walker(pool, std::span(&one, 1), options, false, std::move(stop)).run();
}

std::expected<Walk, std::string> walk_roots(Pool& pool, std::span<const std::string> roots, const WalkOptions& options,
                                            std::stop_token stop) {
    if (roots.empty()) return Walk{};
    return Walker(pool, roots, options, true, std::move(stop)).run();
}

} // namespace grab::ssh
