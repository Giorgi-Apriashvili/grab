#include "server_files.hpp"

#include "browse.hpp"
#include "config.hpp"
#include "existing.hpp"
#include "fetch.hpp"
#include "rclone_encoding.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_hash.hpp"
#include "ssh_pool.hpp"
#include "upload.hpp"
#include "upload_existing.hpp"
#include "util.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grab::server_files {

using upload::Existing;

namespace {

// Folders listed at once on the connection, as a walk lists them (ssh::WalkOptions::in_flight).
constexpr std::size_t list_width = 32;
// LSTATs, MKDIRs and SETSTATs on their way at once (upload_check's, make_folders').
constexpr std::size_t batch = 256;
// REMOVEs and RMDIRs on their way at once: 64 removed 500-670 files a second on Main (SERVER-OPS.md,
// section 9).
constexpr std::size_t remove_width = 64;
// Small files copied at once (32: 126-170 files a second on Main), and the bytes they may hold
// together: the server copies one request after the other, so these are what a stop waits for.
constexpr std::size_t copy_width = 32;
constexpr std::uint64_t copy_window_bytes = std::uint64_t{32} << 20;
// A file this big at most is copied by one copy-data, as copy_range's first piece: any server is
// done with it well within a connection's timeout. Bigger ones go in pieces (sftp::copy_range).
constexpr std::uint64_t small_copy = sftp::copy_first_piece;
// The bits of a mode a copy keeps: read, write and execute (no set-id bits, no sticky bit).
constexpr std::uint32_t mode_bits = 0777;

// ---- paths -------------------------------------------------------------------------------------------

// A path in grab's form as the server takes it ("." for the login folder).
std::string raw_of(std::string_view path) { return path.empty() ? std::string(".") : rclone_enc::raw_path(path); }

// `name` (grab's form) in the folder `dir` (grab's form; "" the login folder).
std::string child(std::string_view dir, std::string_view name) {
    if (dir.empty()) return std::string(name);
    return std::format("{}{}{}", dir, dir.ends_with('/') ? "" : "/", name);
}

std::string parent_of(std::string_view path) { return upload::split_remote(path).first; }
std::string name_of(std::string_view path) { return upload::split_remote(path).second; }

// The folder as messages show it.
std::string shown(std::string_view dir) { return dir.empty() ? std::string("the login folder") : std::string(dir); }

std::string gone(const Where& w, std::string_view path) { return std::format("{}:{} is no longer there", w.server, path); }

// Why a file couldn't be read: gone, or the server's refusal.
std::string unread(const Where& w, std::string_view path, const sftp::Status& status) {
    if (status.code == sftp::Code::no_such_file) return gone(w, path);
    return std::format("{}:{} can't be read: {}", w.server, path, sftp::describe(status));
}

// `rel` (raw, '/' between names) below `root` (raw).
std::string below(std::string_view root, std::string_view rel) {
    if (rel.empty()) return std::string(root);
    if (root == "/") return std::format("/{}", rel);
    return std::format("{}/{}", root, rel);
}

// The same below a path in grab's form.
std::string below_grab(std::string_view path, std::string_view rel) {
    return rel.empty() ? std::string(path) : child(path, rclone_enc::from_raw_path(rel));
}

std::size_t depth_of(std::string_view path) { return static_cast<std::size_t>(std::ranges::count(path, '/')); }

Entry entry(Entry::State state, std::string from, std::string to, std::string why, std::optional<upload::Clash> clash = {}) {
    return Entry{.state = state, .from = std::move(from), .to = std::move(to), .why = std::move(why), .clash = std::move(clash)};
}

// ---- the connection ----------------------------------------------------------------------------------

// The connection an operation runs on, what its server offers, and whether a change was sent on
// it yet (until then, a reused connection that broke is replaced and the step tried again).
struct Conn {
    sftp::Client* c = nullptr;
    upload::Caps caps;
    bool changed = false;
};

// What the server offers that operations use, without asking anything (no limits@openssh.com:
// nothing here writes bytes).
upload::Caps caps_of(sftp::Client& c) {
    return upload::Caps{.write = upload::fallback_write,
                        .posix_rename = c.has_extension("posix-rename@openssh.com"),
                        .statvfs = c.has_extension("statvfs@openssh.com")};
}

// A change refused in `dir` (grab's form), in the uploads' plain words for a change on the server.
upload::Refusal refused(Conn& k, const Where& w, std::string_view dir, const sftp::Status& status, std::uint64_t needed = 0) {
    return upload::refusal(*k.c, k.caps, w.server, dir, raw_of(dir), status, needed, false);
}

void tell(const Where& w, std::vector<std::string> command) {
    if (w.on_command) w.on_command(command);
}

// Runs `step(conn)` on one connection of the server's pool, a seat of its budget held meanwhile
// (fetch::Listing: transfers above their shares hand one back). A reused connection the server
// closed while it sat idle is dropped, and the step tried once more on another while it had sent no
// change. A login that fails, a stop and a connection lost are the error.
template <class T, class Step>
std::expected<T, std::string> on_connection(const Where& w, const std::stop_token& stop, Step&& step) {
    if (!w.pool) return util::failf("no connection to {}", w.server);
    std::optional<fetch::Listing> seat;
    if (w.connections != nullptr) {
        seat.emplace(*w.connections, w.server, stop, 1);
        if (seat->connections() == 0) return util::fail("stopped");
    }
    for (bool retried = false;;) {
        std::optional<ssh::Pool::Lease> lease;
        Conn k;
        try {
            lease = w.pool->acquire(stop, ssh::interactive_retry);
            if (!lease) return util::fail("stopped");
            k.c = &(*lease)->sftp();
            k.caps = caps_of(*k.c);
            auto r = step(k);
            // Replies still on their way (CLOSEs not waited for): in, so the connection goes back
            // to the pool clean.
            while (k.c->pending() > 0) k.c->pump();
            return r;
        } catch (const ssh::Error& e) {
            return util::fail(e.what());
        } catch (const sftp::Unavailable& e) {
            if (lease) lease->broken();
            return util::fail(e.what());
        } catch (const sftp::Broken& e) {
            if (lease) lease->broken();
            if (lease && lease->reused() && !k.changed && !std::exchange(retried, true) && !stop.stop_requested()) continue;
            return util::failf("the connection to {} was lost: {}", w.server, e.what());
        }
    }
}

// Jobs 0..n-1 on `c`, at most `width` of them going at once, new ones only while `more()`:
// start(i, finish) sends job i's first requests, their callbacks send the next ones, and the last
// one calls finish() (exactly once).
void window(sftp::Client& c, std::size_t n, std::size_t width, const std::function<bool()>& more,
            const std::function<void(std::size_t, const std::function<void()>&)>& start) {
    std::size_t next = 0;
    std::size_t going = 0;
    const std::function<void()> finish = [&going] { --going; };
    for (;;) {
        while (going < width && next < n && more()) {
            ++going;
            start(next++, finish);
        }
        if (going == 0) break;
        c.pump();
    }
}

// ---- a walk that never goes through a link ------------------------------------------------------------

struct Node {
    std::string rel; // below the walk's root: raw names, '/' between them
    sftp::Attrs attrs;
};

struct Walked {
    std::vector<Node> folders; // parents first
    std::vector<Node> files;
    std::vector<Node> links;
    std::vector<Node> others; // sockets, pipes, devices
    std::vector<std::pair<std::string, sftp::Status>> unreadable; // folders that couldn't be listed
    bool stopped = false;
    [[nodiscard]] std::uint64_t bytes() const {
        std::uint64_t n = 0;
        for (const Node& f : files) n += f.attrs.size.value_or(0);
        return n;
    }
};

void sort_into(Walked& w, Node n) {
    if (n.attrs.is_file()) w.files.push_back(std::move(n));
    else if (n.attrs.is_link()) w.links.push_back(std::move(n));
    else w.others.push_back(std::move(n));
}

using Listed = std::expected<std::vector<sftp::Entry>, sftp::Status>;

// The folders `dirs` (raw) listed at once on `c`, up to list_width at a time: each one's entries but
// "." and "..", or its failure. `names` counts every name read; past sftp::max_listing a folder fails.
std::vector<Listed> list_many(sftp::Client& c, std::span<const std::string> dirs, std::size_t& names) {
    struct One {
        std::string handle;
        std::vector<sftp::Entry> entries;
        std::optional<sftp::Status> failed;
    };
    std::vector<One> ones(dirs.size());
    std::function<void(std::size_t, const std::function<void()>&)> read_more = [&](std::size_t i,
                                                                                  const std::function<void()>& finish) {
        c.readdir(ones[i].handle, [&, i, finish](std::expected<std::vector<sftp::Entry>, sftp::Status> r) {
            One& o = ones[i];
            bool end = !r || r->empty(); // no names but no end either: the end, as sftp::list takes it
            if (!r && r.error().code != sftp::Code::eof) o.failed = r.error();
            if (r) {
                for (sftp::Entry& e : *r) {
                    if (e.name == "." || e.name == "..") continue;
                    o.entries.push_back(std::move(e));
                    ++names;
                }
            }
            if (!end && names > sftp::max_listing) {
                o.failed = sftp::Status{.code = sftp::Code::failure,
                                        .message = std::format("more than {} names below it", sftp::max_listing)};
                end = true;
            }
            if (!end) return read_more(i, finish);
            c.close(o.handle); // not waited for: a handle that fails to close spoils nothing read
            finish();
        });
    };
    window(c, dirs.size(), list_width, [] { return true; }, [&](std::size_t i, const std::function<void()>& finish) {
        c.opendir(dirs[i], [&, i, finish](std::expected<std::string, sftp::Status> h) {
            if (!h) {
                ones[i].failed = h.error();
                return finish();
            }
            ones[i].handle = std::move(*h);
            read_more(i, finish);
        });
    });
    while (c.pending() > 0) c.pump(); // the CLOSEs
    std::vector<Listed> out;
    out.reserve(ones.size());
    for (One& o : ones) {
        if (o.failed) out.emplace_back(std::unexpected(std::move(*o.failed)));
        else out.emplace_back(std::move(o.entries));
    }
    return out;
}

// Everything below the folder `root` (raw; LSTATed as a real folder by the caller), never through a
// link: every name listed is LSTATed before it is sorted (a server's READDIR may follow links, so a
// link to a file or folder would be listed as one; OpenSSH's doesn't), and only a real folder is
// entered. Hidden names are names like any other. The root's own listing failing is the error; a folder below that
// can't be listed is in `unreadable`. `progress` after each level, with what was found so far.
std::expected<Walked, sftp::Status> walk(sftp::Client& c, const std::string& root, const std::stop_token& stop,
                                         const std::function<void(const Walked&)>& progress = {}) {
    Walked w;
    std::size_t names = 0;
    std::vector<std::string> level{std::string()};
    for (bool first = true; !level.empty(); first = false) {
        if (stop.stop_requested()) {
            w.stopped = true;
            return w;
        }
        std::vector<std::string> raws;
        raws.reserve(level.size());
        for (const std::string& rel : level) raws.push_back(below(root, rel));
        auto listed = list_many(c, raws, names);
        if (names > sftp::max_listing) {
            return std::unexpected(sftp::Status{.code = sftp::Code::failure,
                                                .message = std::format("more than {} names below it", sftp::max_listing)});
        }
        std::vector<Node> unsure; // every name listed: LSTATed before it is sorted
        for (std::size_t i = 0; i < level.size(); ++i) {
            if (!listed[i]) {
                if (first) return std::unexpected(listed[i].error());
                w.unreadable.emplace_back(level[i], listed[i].error());
                continue;
            }
            for (sftp::Entry& e : *listed[i]) {
                unsure.push_back(
                    Node{.rel = level[i].empty() ? std::move(e.name) : std::format("{}/{}", level[i], e.name), .attrs = e.attrs});
            }
        }
        std::vector<std::optional<sftp::Status>> failed(unsure.size());
        for (std::size_t at = 0; at < unsure.size(); at += batch) {
            for (std::size_t k = at; k < std::min(unsure.size(), at + batch); ++k) {
                c.lstat(below(root, unsure[k].rel), [&, k](std::expected<sftp::Attrs, sftp::Status> a) {
                    if (a) unsure[k].attrs = *a;
                    else failed[k] = a.error();
                });
            }
            while (c.pending() > 0) c.pump();
        }
        level.clear();
        for (std::size_t k = 0; k < unsure.size(); ++k) {
            Node& n = unsure[k];
            if (failed[k]) {
                // Gone meanwhile: nothing there to walk.
                if (failed[k]->code != sftp::Code::no_such_file) w.unreadable.emplace_back(n.rel, *failed[k]);
                continue;
            }
            if (n.attrs.is_dir()) {
                level.push_back(n.rel);
                w.folders.push_back(std::move(n));
            } else {
                sort_into(w, std::move(n));
            }
        }
        if (progress) progress(w);
    }
    return w;
}

// The first "name (n).ext" ("name (n)" for a folder) in `dir` nothing on the server has and none
// of `reserved` (the operation's own new names) is.
std::expected<std::string, sftp::Status> free_numbered(sftp::Client& c, std::string_view dir, std::string_view name, bool folder,
                                                       const std::set<std::string>& reserved) {
    for (int n = 2; n < 1000; ++n) {
        const std::string candidate = child(dir, folder ? std::format("{} ({})", name, n) : existing::numbered(name, n));
        if (reserved.contains(candidate)) continue;
        auto a = sftp::lstat(c, raw_of(candidate));
        if (!a && a.error().code == sftp::Code::no_such_file) return candidate;
        if (!a) return std::unexpected(a.error());
    }
    return std::unexpected(sftp::Status{.code = sftp::Code::failure, .message = "every numbered name up to 999 is taken"});
}

// What a server's answer to looking at a name means: nothing there (nullopt), or its own attributes
// (LSTAT's) and those of what it leads to (a link's: STAT's, else a file of unknown size and time).
struct There {
    sftp::Attrs own;
    sftp::Attrs through;
};

std::expected<std::optional<There>, sftp::Status> look_there(sftp::Client& c, const std::string& raw) {
    auto own = sftp::lstat(c, raw);
    if (!own) {
        if (own.error().code == sftp::Code::no_such_file) return std::optional<There>();
        return std::unexpected(own.error());
    }
    auto through = upload::look_at(c, raw);
    if (!through) return std::unexpected(through.error());
    return std::optional<There>(There{.own = *own, .through = through->value_or(*own)});
}

// The clash of the item `from` (its own attributes) with what is at its place.
upload::Clash clash_of(std::string_view to, const sftp::Attrs& from, const sftp::Attrs& there) {
    return upload::clash_with(name_of(to), from.size.value_or(0), from.mtime.value_or(0), there);
}

// The existing-file decision for one moved or copied as a whole (a move's item, a copy's folder):
// a folder on either side is never replaced, so a Replace choice is a skip saying why.
upload::Decision decide_whole(Existing choice, const upload::Clash& clash, bool from_folder, bool there_folder) {
    upload::Decision d = upload::decide(choice, clash);
    if (d.action != upload::Action::replace || (!from_folder && !there_folder)) return d;
    return upload::Decision{.action = upload::Action::skip,
                            .note = there_folder ? "a folder of that name is there, and folders are never replaced"
                                                 : "a file of that name is there, and a folder doesn't replace it"};
}

} // namespace

// ---- names ---------------------------------------------------------------------------------------------

std::optional<std::string> bad_name(std::string_view shown_name) {
    // The name the server would get: '／' is a '/' there, '␀' a NUL, '．' a "." (rclone_enc).
    const std::string decoded = rclone_enc::decode(shown_name, rclone_enc::grab_path);
    const std::string_view name = decoded;
    if (name.empty()) return "a name can't be empty";
    if (name == "." || name == "..") return std::format("\"{}\" can't be a name", name);
    if (name.find('/') != std::string_view::npos) return "a name can't hold a /";
    if (name.find('\0') != std::string_view::npos) return "a name can't hold a NUL character";
    return std::nullopt;
}

namespace {

// The answer for a name taken in `dir`.
Named taken(std::string_view name, std::string_view dir) {
    return Named{.taken = true, .path = {}, .why = std::format("{} is already taken in {}", name, shown(dir))};
}

// The login folder, the server's top, or (`holding`) a folder holding the login folder, however
// each is spelled: canonical() with `home`, the login folder's real path (empty: only its spellings
// from the login folder itself, "", "." or "a/.."). Nothing renames, moves or deletes those. `real`:
// the path's real spelling (real_spellings) compared instead, the words still naming `path`. A
// path ending in '/' is refused as well: LSTAT, REMOVE and RENAME would follow a link at its end
// (a link to the login folder, "/data/me/"), which canonical() can't tell; once `home` is known,
// so the login folder's own spellings ("/home/u/") keep their words.
std::optional<std::string> top_folder(std::string_view path, std::string_view what, std::string_view home, bool holding,
                                      std::string_view real = {}) {
    const std::string at = canonical(real.empty() ? path : real, home);
    const std::string login = canonical("", home);
    if (at == "/") return std::format("the server's top folder can't be {}", what);
    if (at == login) return std::format("the login folder can't be {}", what);
    if (holding && browse::is_below(login, at)) return std::format("{} holds the login folder, so it can't be {}", path, what);
    if (!home.empty() && path.size() > 1 && path.ends_with('/')) {
        return std::format("{} can't be {}: name it without the '/' at its end", path, what);
    }
    return std::nullopt;
}

// The real spellings of `paths` (grab's form): each one's folder by its REALPATH (asked at once),
// its own name kept, so a link that is the path's last part stays the link (RENAME and REMOVE act
// on it alone) while a linked folder on the way, or ".." after one, leads where the server takes
// it. One whose folder can't be resolved stays as given.
std::vector<std::string> real_spellings(sftp::Client& c, std::span<const std::string> paths) {
    std::vector<std::string> out(paths.begin(), paths.end());
    for (std::size_t at = 0; at < paths.size(); at += batch) {
        for (std::size_t i = at; i < std::min(paths.size(), at + batch); ++i) {
            if (paths[i].empty() || paths[i] == "/") continue;
            c.realpath(raw_of(parent_of(paths[i])), [&out, paths, i](std::expected<std::string, sftp::Status> r) {
                if (r) out[i] = child(rclone_enc::from_raw_path(*r), name_of(paths[i]));
            });
        }
        while (c.pending() > 0) c.pump();
    }
    return out;
}

// top_folder() of each of `paths` by its real spelling as well: the login folder reached through a
// linked folder ("/home/u" with /home a link to /usr/home/u's parent, or "/a/l/.." with l a link
// into it) is refused before anything is sent, as by its own spellings.
std::optional<std::string> top_folder_really(sftp::Client& c, std::span<const std::string> paths, std::string_view what,
                                             std::string_view home, bool holding) {
    const std::vector<std::string> real = real_spellings(c, paths);
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (auto why = top_folder(paths[i], what, home, holding, real[i])) return why;
    }
    return std::nullopt;
}

// The sources of `items`.
std::vector<std::string> froms(std::span<const Item> items) {
    std::vector<std::string> out;
    out.reserve(items.size());
    for (const Item& it : items) out.push_back(it.from);
    return out;
}

// The login folder's real path (grab's form): Where::home, else the server's answer to REALPATH of
// "." (asked before any change, so the login folder is refused however it is spelled).
std::expected<std::string, std::string> home_of(Conn& k, const Where& w) {
    if (!w.home.empty()) return w.home;
    auto home = sftp::realpath(*k.c, ".");
    if (!home) return util::failf("{}: the login folder can't be looked at: {}", w.server, sftp::describe(home.error()));
    return rclone_enc::from_raw_path(*home);
}

} // namespace

std::expected<Named, std::string> rename(const Where& w, std::string_view path, std::string_view new_name, std::stop_token stop) {
    if (auto why = top_folder(path, "renamed", w.home, true)) return util::fail(*why);
    if (auto why = bad_name(new_name)) return util::fail(*why);
    const auto [dir, old] = upload::split_remote(path);
    const std::string to = child(dir, new_name); // grab's form, as every path here
    if (to == path) return Named{.taken = false, .path = to, .why = {}};
    return on_connection<Named>(w, stop, [&](Conn& k) -> std::expected<Named, std::string> {
        sftp::Client& c = *k.c;
        auto home = home_of(k, w);
        if (!home) return util::fail(home.error());
        if (auto why = top_folder(path, "renamed", *home, true)) return util::fail(*why);
        const std::string given(path);
        if (auto why = top_folder_really(c, std::span(&given, 1), "renamed", *home, true)) return util::fail(*why);
        if (auto there = sftp::lstat(c, raw_of(to))) return taken(new_name, dir);
        else if (there.error().code != sftp::Code::no_such_file) return util::fail(refused(k, w, dir, there.error()).why);
        k.changed = true;
        tell(w, {"sftp", "rename", std::string(path), to});
        auto moved = sftp::rename(c, raw_of(path), raw_of(to));
        if (moved) return Named{.taken = false, .path = to, .why = {}};
        // RENAME never replaces anything: a name taken meanwhile is still the user's to choose.
        if (sftp::lstat(c, raw_of(to))) return taken(new_name, dir);
        if (!sftp::lstat(c, raw_of(path))) return util::fail(gone(w, path));
        return util::fail(refused(k, w, dir, moved.error()).why);
    });
}

std::expected<Named, std::string> mkdir(const Where& w, std::string_view dir, std::string_view name, std::stop_token stop) {
    if (auto why = bad_name(name)) return util::fail(*why);
    const std::string to = child(dir, name); // grab's form, as every path here
    return on_connection<Named>(w, stop, [&](Conn& k) -> std::expected<Named, std::string> {
        sftp::Client& c = *k.c;
        if (auto there = sftp::lstat(c, raw_of(to))) return taken(name, dir);
        else if (there.error().code != sftp::Code::no_such_file) return util::fail(refused(k, w, dir, there.error()).why);
        k.changed = true;
        tell(w, {"sftp", "mkdir", to});
        auto made = sftp::mkdir(c, raw_of(to));
        if (made) return Named{.taken = false, .path = to, .why = {}};
        if (sftp::lstat(c, raw_of(to))) return taken(name, dir);
        if (made.error().code == sftp::Code::no_such_file) return util::fail(gone(w, dir));
        return util::fail(refused(k, w, dir, made.error()).why);
    });
}

// ---- move ------------------------------------------------------------------------------------------------

std::vector<Item> into(std::span<const std::string> paths, std::string_view dir) {
    std::vector<Item> items;
    items.reserve(paths.size());
    for (const std::string& p : paths) items.push_back(Item{.from = p, .to = child(dir, name_of(p)), .choice = {}});
    return items;
}

std::optional<std::string> refused_before(std::span<const Item> items, bool copying, std::string_view home) {
    const std::string_view what = copying ? "copied" : "moved";
    for (const Item& it : items) {
        // A copy leaves its source where it is: one holding the login folder may go elsewhere.
        if (auto why = top_folder(it.from, what, home, !copying)) return why;
        if (auto why = bad_name(name_of(it.to))) return std::format("{} can't be {} there: {}", it.from, what, *why);
        if (browse::is_below(it.to, it.from) || browse::is_below(canonical(it.to, home), canonical(it.from, home))) {
            return std::format("{} can't be {} into itself", name_of(it.from), what);
        }
    }
    return std::nullopt;
}

namespace {

// Why the server refused to move `from` to `to`: OpenSSH answers a RENAME onto another file system
// (rename(2)'s EXDEV) with "Failure", which statvfs's file system ids tell apart from the rest.
// A RENAME refused for permission may be either folder's (the name leaves one, comes into the
// other): the words name both.
upload::Refusal move_refused(Conn& k, const Where& w, const std::string& from, const std::string& to, const sftp::Status& status) {
    const std::string dir = parent_of(to);
    upload::Refusal r = refused(k, w, dir, status);
    if (status.code == sftp::Code::permission_denied && r.scope == upload::Refusal::Scope::folder) {
        r.why = std::format("{} doesn't let this account move {} from {} into {}", w.server, name_of(from), shown(parent_of(from)),
                            shown(dir));
    }
    if (status.code == sftp::Code::failure && k.caps.statvfs && r.scope == upload::Refusal::Scope::file) {
        auto here = sftp::statvfs(*k.c, raw_of(parent_of(from)));
        auto there = sftp::statvfs(*k.c, raw_of(dir));
        if (here && there && here->fsid != there->fsid) {
            r.why = std::format("{} can't move {} into {}: that folder is on another file system of the server, and grab moves "
                                "only within one",
                                w.server, name_of(from), shown(dir));
        }
    }
    return r;
}

// The real paths of `raws` (REALPATH, asked at once).
std::expected<std::vector<std::string>, sftp::Status> real_paths(sftp::Client& c, const std::vector<std::string>& raws) {
    std::vector<std::optional<std::expected<std::string, sftp::Status>>> got(raws.size());
    for (std::size_t i = 0; i < raws.size(); ++i) {
        c.realpath(raws[i], [&got, i](std::expected<std::string, sftp::Status> r) { got[i] = std::move(r); });
    }
    while (c.pending() > 0) c.pump();
    std::vector<std::string> out;
    for (auto& g : got) {
        if (!g) return std::unexpected(sftp::Status{.code = sftp::Code::failure, .message = "no answer to REALPATH"});
        if (!*g) return std::unexpected(g->error());
        out.push_back(std::move(**g));
    }
    return out;
}

// Whether `from` and `to` (grab's form) are one name spelled two ways, as the address bar allows
// ("x.txt" in the login folder and "/home/u/x.txt"): the same name in folders whose real paths are
// the same.
std::expected<bool, sftp::Status> same_place(sftp::Client& c, const std::string& from, const std::string& to) {
    if (name_of(from) != name_of(to)) return false;
    auto real = real_paths(c, {raw_of(parent_of(from)), raw_of(parent_of(to))});
    if (!real) return std::unexpected(real.error());
    return (*real)[0] == (*real)[1];
}

// Whether `to`'s folder is, by its real path, the folder `from` or one below it: a folder moved
// into itself under another spelling, which rename(2) refuses (EINVAL: OpenSSH's "Bad message").
bool into_itself(sftp::Client& c, const std::string& from, const std::string& to) {
    auto real = real_paths(c, {raw_of(from), raw_of(parent_of(to))});
    return real && ((*real)[1] == (*real)[0] || browse::is_below((*real)[1], (*real)[0]));
}

// One item of a move. What it did goes to `report`; false: a refusal that holds for everything
// left (the server read-only), in report.error.
bool move_one(Conn& k, const Where& w, const Item& it, std::size_t index, Existing existing, const std::set<std::string>& reserved,
              Report& report) {
    sftp::Client& c = *k.c;
    const auto from_there = sftp::lstat(c, raw_of(it.from));
    if (!from_there) {
        const std::string why = from_there.error().code == sftp::Code::no_such_file
                                    ? gone(w, it.from)
                                    : refused(k, w, parent_of(it.from), from_there.error()).why;
        report.entries.push_back(entry(Entry::State::failed, it.from, it.to, why));
        return true;
    }
    const sftp::Attrs& src = *from_there;
    const Existing choice = it.choice.value_or(existing);
    const std::string dir = parent_of(it.to);
    const auto count = [&] { (src.is_dir() ? report.folders : report.files) += 1; };
    // A refusal: the item's, or (a read-only server, a folder this account can't write) everything's.
    const auto fail = [&](const upload::Refusal& r) {
        if (r.scope == upload::Refusal::Scope::server) {
            report.error = r.why;
            return false;
        }
        report.entries.push_back(entry(Entry::State::failed, it.from, it.to, r.why));
        return true;
    };
    // A RENAME to `to` refused, looked into: the item gone meanwhile, the folder it goes into gone,
    // a folder into itself under another spelling, else the refusal's words.
    const auto not_moved = [&](const std::string& to, const sftp::Status& status) {
        if (!sftp::lstat(c, raw_of(it.from))) {
            report.entries.push_back(entry(Entry::State::failed, it.from, it.to, gone(w, it.from)));
            return true;
        }
        if (status.code == sftp::Code::no_such_file && !sftp::lstat(c, raw_of(dir))) {
            report.entries.push_back(entry(Entry::State::failed, it.from, it.to, gone(w, dir)));
            return true;
        }
        if (status.code == sftp::Code::bad_message && src.is_dir() && into_itself(c, it.from, to)) {
            report.entries.push_back(
                entry(Entry::State::failed, it.from, it.to, std::format("{} can't be moved into itself", name_of(it.from))));
            return true;
        }
        return fail(move_refused(k, w, it.from, to, status));
    };
    std::string to = it.to;
    for (int tries = 0; tries < 8; ++tries) {
        auto there = look_there(c, raw_of(to));
        if (!there) return fail(refused(k, w, dir, there.error()));
        if (!*there) {
            k.changed = true;
            tell(w, {"sftp", "rename", it.from, to});
            auto moved = sftp::rename(c, raw_of(it.from), raw_of(to));
            if (moved) {
                report.placed[index] = to;
                count();
                return true;
            }
            if (sftp::lstat(c, raw_of(to))) continue; // taken meanwhile: the choice decides
            return not_moved(to, moved.error());
        }
        const upload::Clash clash = clash_of(to, src, (*there)->through);
        const upload::Decision d = decide_whole(choice, clash, src.is_dir(), (*there)->own.is_dir());
        switch (d.action) {
        case upload::Action::ask:
            report.entries.push_back(entry(Entry::State::asking, it.from, it.to, "already there", clash));
            return true;
        case upload::Action::skip:
            report.entries.push_back(entry(Entry::State::skipped, it.from, it.to, d.note.empty() ? "already there" : d.note, clash));
            return true;
        case upload::Action::numbered: {
            // The moved one itself under another spelling: there already, not renamed beside itself.
            auto same = same_place(c, it.from, to);
            if (!same) return fail(refused(k, w, dir, same.error()));
            if (*same) {
                report.placed[index] = to;
                return true;
            }
            auto numbered = free_numbered(c, dir, name_of(it.to), src.is_dir(), reserved);
            if (!numbered) return fail(refused(k, w, dir, numbered.error()));
            to = *numbered;
            continue;
        }
        case upload::Action::replace: break;
        }
        // Replace (a file or link by one, never a folder: decide_whole). Without posix-rename the
        // one there is removed first: not when it is the moved one itself under another spelling.
        if (!k.caps.posix_rename) {
            auto same = same_place(c, it.from, to);
            if (!same) return fail(refused(k, w, dir, same.error()));
            if (*same) { // there already
                report.placed[index] = to;
                return true;
            }
        }
        k.changed = true;
        if (k.caps.posix_rename) { // in one step (onto itself: rename(2) does nothing)
            tell(w, {"sftp", "posix-rename", it.from, to});
            if (auto moved = sftp::posix_rename(c, raw_of(it.from), raw_of(to)); !moved) return not_moved(to, moved.error());
            report.placed[index] = to;
            count();
            return true;
        }
        // Without posix-rename: the one there goes first (only because the user chose to replace
        // it), then the moved one takes its name.
        tell(w, {"sftp", "rm", to});
        if (auto removed = sftp::remove(c, raw_of(to)); !removed && removed.error().code != sftp::Code::no_such_file) {
            return fail(refused(k, w, dir, removed.error()));
        }
        tell(w, {"sftp", "rename", it.from, to});
        if (auto moved = sftp::rename(c, raw_of(it.from), raw_of(to)); !moved) {
            if (sftp::lstat(c, raw_of(to))) continue; // taken again meanwhile
            report.entries.push_back(entry(Entry::State::failed, it.from, it.to,
                                           std::format("the old {} was removed, but {} couldn't take its name and is still at "
                                                       "{} ({})",
                                                       to, name_of(it.from), it.from, sftp::describe(moved.error()))));
            return true;
        }
        report.placed[index] = to;
        count();
        return true;
    }
    report.entries.push_back(entry(Entry::State::failed, it.from, it.to, "the name kept being taken on the server meanwhile"));
    return true;
}

// Each later item going where an earlier one goes is left out (one paste never puts two things
// at one name); true for those left out.
std::vector<char> doubles(std::span<const Item> items, Report& report) {
    std::vector<char> out(items.size(), 0);
    std::set<std::string> seen;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (seen.insert(items[i].to).second) continue;
        out[i] = 1;
        report.entries.push_back(entry(Entry::State::skipped, items[i].from, items[i].to,
                                       std::format("another item named {} goes there too", name_of(items[i].to))));
    }
    return out;
}

// The stopped or failed end of an operation on the connection.
void ended(const std::expected<bool, std::string>& ran, const std::stop_token& stop, Report& report) {
    if (ran) return;
    if (stop.stop_requested()) report.stopped = true;
    else if (report.error.empty()) report.error = ran.error();
}

// The report as it was before the step: each try of it on a connection (on_connection tries again
// on another when a reused one broke before any change) starts from there, so nothing it said is
// said twice. `entries`: those from before the step.
void restart(Report& report, std::size_t entries, std::size_t items) {
    report.files = 0;
    report.folders = 0;
    report.bytes = 0;
    report.placed.assign(items, std::string());
    report.entries.resize(entries);
    report.stopped = false;
    report.error.clear();
    report.left = 0;
}

} // namespace

Report move(const Where& w, std::span<const Item> items, Existing existing, std::stop_token stop) {
    Report report;
    report.placed.assign(items.size(), std::string());
    if (auto why = refused_before(items, false, w.home)) {
        report.error = *why;
        return report;
    }
    const auto left_out = doubles(items, report);
    const std::size_t before = report.entries.size();
    std::set<std::string> reserved;
    for (const Item& it : items) reserved.insert(it.to);
    const auto ran = on_connection<bool>(w, stop, [&](Conn& k) -> std::expected<bool, std::string> {
        restart(report, before, items.size());
        auto home = home_of(k, w);
        if (!home) return util::fail(home.error());
        if (auto why = refused_before(items, false, *home)) {
            report.error = *why;
            return true;
        }
        if (auto why = top_folder_really(*k.c, froms(items), "moved", *home, true)) {
            report.error = *why;
            return true;
        }
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (left_out[i]) continue;
            if (stop.stop_requested()) {
                report.stopped = true;
                break;
            }
            if (items[i].to == items[i].from) { // there already
                report.placed[i] = items[i].to;
                continue;
            }
            if (!move_one(k, w, items[i], i, existing, reserved, report)) break;
        }
        return true;
    });
    ended(ran, stop, report);
    return report;
}

// ---- copy: the plan ------------------------------------------------------------------------------------

namespace {

// A file or link to copy.
struct CopyFile {
    std::string from;          // grab's form
    std::string to;
    sftp::Attrs attrs;         // the source's own (LSTAT)
    Existing choice = Existing::ask;
    bool free = true;          // nothing at `to` when looked at: a plain RENAME puts it there
    bool link = false;         // made again as a link (SYMLINK), nothing copied
    std::string target;        // a link's, raw, as stored
    std::size_t item = 0;
    bool top = false;          // the item itself (Report::placed)
};

// A folder to make (or merge into).
struct CopyDir {
    std::string from;
    std::string to;
    sftp::Attrs attrs;
    bool make = true; // false: a folder is there already and is merged into
};

struct Plan {
    std::vector<CopyDir> dirs;   // parents first
    std::vector<CopyFile> files; // files and links
    std::vector<Entry> entries;  // decided before anything is copied: skipped, failed, asking
    std::vector<Entry> taken;    // every name taken, whatever the choice (look)
    std::vector<Entry> unwalked; // of them, the folders whose name is taken by a file (or a link): not walked
    std::optional<std::string> refused; // nothing may be copied at all
    std::size_t total_files = 0;   // walked, before the choices
    std::size_t total_folders = 0;
    std::uint64_t total_bytes = 0;
};

// A target looked at in a merged folder: the index of its file or folder, and what is there.
struct Looked {
    bool folder = false;
    std::size_t index = 0;
    std::string raw;
    std::optional<sftp::Attrs> own;
    std::optional<sftp::Attrs> through;
    std::optional<sftp::Status> failed;
};

// LSTAT of each (pipelined), then STAT of those that are links.
void look_all(sftp::Client& c, std::vector<Looked>& looked) {
    for (std::size_t at = 0; at < looked.size(); at += batch) {
        const std::size_t end = std::min(looked.size(), at + batch);
        for (std::size_t i = at; i < end; ++i) {
            c.lstat(looked[i].raw, [&looked, i](std::expected<sftp::Attrs, sftp::Status> a) {
                if (a) looked[i].own = *a;
                else if (a.error().code != sftp::Code::no_such_file) looked[i].failed = a.error();
            });
        }
        while (c.pending() > 0) c.pump();
        for (std::size_t i = at; i < end; ++i) {
            if (!looked[i].own) continue;
            if (!looked[i].own->is_link()) {
                looked[i].through = looked[i].own;
                continue;
            }
            c.stat(looked[i].raw, [&looked, i](std::expected<sftp::Attrs, sftp::Status> a) {
                looked[i].through =
                    a ? *a : sftp::Attrs{.size = {}, .permissions = 0100644, .mtime = {}, .uid = {}, .gid = {}, .atime = {}};
            });
        }
        while (c.pending() > 0) c.pump();
    }
}

// READLINK of each link (pipelined); one that can't be read is skipped with why.
void read_links(sftp::Client& c, std::vector<CopyFile>& files, std::vector<Entry>& entries) {
    std::vector<std::optional<sftp::Status>> failed(files.size());
    for (std::size_t at = 0; at < files.size(); at += batch) {
        for (std::size_t i = at; i < std::min(files.size(), at + batch); ++i) {
            if (!files[i].link) continue;
            c.readlink(raw_of(files[i].from), [&files, &failed, i](std::expected<std::string, sftp::Status> r) {
                if (r) files[i].target = std::move(*r);
                else failed[i] = r.error();
            });
        }
        while (c.pending() > 0) c.pump();
    }
    std::vector<CopyFile> kept;
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (!failed[i]) {
            kept.push_back(std::move(files[i]));
            continue;
        }
        entries.push_back(entry(Entry::State::skipped, files[i].from, files[i].to,
                                std::format("the link couldn't be read: {}", sftp::describe(*failed[i]))));
    }
    files = std::move(kept);
}

// A file (or link) whose target was found taken: the choice's decision, into the plan.
void decide_file(Plan& p, CopyFile f, const sftp::Attrs& there) {
    const upload::Clash clash = clash_of(f.to, f.attrs, there);
    p.taken.push_back(entry(Entry::State::asking, f.from, f.to, "already there", clash));
    const upload::Decision d = upload::decide(f.choice, clash);
    switch (d.action) {
    case upload::Action::ask: p.entries.push_back(entry(Entry::State::asking, f.from, f.to, "already there", clash)); return;
    case upload::Action::skip:
        p.entries.push_back(entry(Entry::State::skipped, f.from, f.to, d.note.empty() ? "already there" : d.note, clash));
        return;
    case upload::Action::numbered:
    case upload::Action::replace:
        // Placed by upload::place with the choice, which looks again right before.
        f.free = false;
        p.files.push_back(std::move(f));
        return;
    }
}

// The folder item `it` (its source LSTATed as `src`) into the plan: walked, its target `to` made
// (`merge`: a folder is there, merged into: its folders and files looked at). False when it can't
// go: it couldn't be read (an entry says so), or a stop.
bool plan_folder(Conn& k, const Where& w, Plan& p, const Item& it, std::size_t index, const sftp::Attrs& src, const std::string& to,
                 bool merge, Existing choice, const std::stop_token& stop) {
    sftp::Client& c = *k.c;
    auto tree = walk(c, raw_of(it.from), stop);
    if (!tree) {
        p.entries.push_back(entry(Entry::State::failed, it.from, to,
                                  std::format("{}:{} can't be read: {}", w.server, it.from, sftp::describe(tree.error()))));
        return false;
    }
    if (tree->stopped) return false;
    for (const auto& [rel, status] : tree->unreadable) {
        p.entries.push_back(entry(Entry::State::failed, below_grab(it.from, rel), below_grab(to, rel),
                                  std::format("can't be read: {}", sftp::describe(status))));
    }
    for (const Node& n : tree->others) {
        p.entries.push_back(entry(Entry::State::skipped, below_grab(it.from, n.rel), below_grab(to, n.rel),
                                  "not a file, folder or link"));
    }
    // An upload's or copy's temp (".<name>.<token>.grabpart") is grab's half-written file, not the
    // user's: it isn't copied.
    for (auto* group : {&tree->files, &tree->links}) {
        std::erase_if(*group, [&](const Node& n) {
            if (!upload::is_temp_name(upload::split_remote(n.rel).second)) return false;
            p.entries.push_back(entry(Entry::State::skipped, below_grab(it.from, n.rel), below_grab(to, n.rel),
                                      "a file grab is still writing (an upload's or a copy's)"));
            return true;
        });
    }
    p.total_folders += 1 + tree->folders.size();
    p.total_files += tree->files.size() + tree->links.size();
    p.total_bytes += tree->bytes();
    p.dirs.push_back(CopyDir{.from = it.from, .to = to, .attrs = src, .make = !merge});
    const std::size_t first_dir = p.dirs.size();
    for (const Node& n : tree->folders) {
        p.dirs.push_back(CopyDir{.from = below_grab(it.from, n.rel), .to = below_grab(to, n.rel), .attrs = n.attrs, .make = true});
    }
    std::vector<CopyFile> files;
    const auto add = [&](const Node& n, bool link) {
        files.push_back(CopyFile{.from = below_grab(it.from, n.rel),
                                 .to = below_grab(to, n.rel),
                                 .attrs = n.attrs,
                                 .choice = choice,
                                 .free = true,
                                 .link = link,
                                 .target = {},
                                 .item = index,
                                 .top = false});
    };
    for (const Node& n : tree->files) add(n, false);
    for (const Node& n : tree->links) add(n, true);
    read_links(c, files, p.entries);
    if (!merge) {
        std::ranges::move(files, std::back_inserter(p.files));
        return true;
    }
    // Merged into: what is at each of its places.
    std::vector<Looked> looked;
    for (std::size_t d = first_dir; d < p.dirs.size(); ++d) {
        looked.push_back(Looked{.folder = true, .index = d, .raw = raw_of(p.dirs[d].to), .own = {}, .through = {}, .failed = {}});
    }
    for (std::size_t f = 0; f < files.size(); ++f) {
        looked.push_back(Looked{.folder = false, .index = f, .raw = raw_of(files[f].to), .own = {}, .through = {}, .failed = {}});
    }
    look_all(c, looked);
    std::vector<std::string> blocked; // folders that can't be made: nothing goes below them
    const auto is_blocked = [&](const std::string& path) {
        return std::ranges::any_of(blocked, [&](const std::string& b) { return browse::is_below(path, b); });
    };
    for (const Looked& l : looked) {
        if (!l.folder) continue;
        CopyDir& d = p.dirs[l.index];
        if (l.failed) {
            blocked.push_back(d.to);
            p.entries.push_back(entry(Entry::State::failed, d.from, d.to, refused(k, w, parent_of(d.to), *l.failed).why));
        } else if (l.own && l.own->is_dir()) {
            d.make = false; // there already: merged into
        } else if (l.own) {
            blocked.push_back(d.to);
            p.entries.push_back(entry(Entry::State::failed, d.from, d.to,
                                      std::format("{}:{} is a file, so the folder can't be made there", w.server, d.to)));
        }
    }
    std::erase_if(p.dirs, [&](const CopyDir& d) { return std::ranges::find(blocked, d.to) != blocked.end() || is_blocked(d.to); });
    for (const Looked& l : looked) {
        if (l.folder) continue;
        CopyFile& f = files[l.index];
        if (is_blocked(f.to)) continue;
        if (l.failed) {
            p.entries.push_back(entry(Entry::State::failed, f.from, f.to, refused(k, w, parent_of(f.to), *l.failed).why));
        } else if (!l.own) {
            p.files.push_back(std::move(f));
        } else {
            decide_file(p, std::move(f), *l.through);
        }
    }
    return true;
}

// What copying `items` takes: each item looked at, its folders walked, and the names taken in
// their places decided by the choices. Nothing is changed. `home`: the login folder's real path,
// for a folder copied onto itself under another spelling.
Plan plan_copy(Conn& k, const Where& w, std::span<const Item> items, Existing existing, Report& report, const std::stop_token& stop,
               std::string_view home) {
    sftp::Client& c = *k.c;
    Plan p;
    const auto left_out = doubles(items, report);
    std::set<std::string> reserved;
    for (const Item& it : items) reserved.insert(it.to);
    for (std::size_t i = 0; i < items.size() && !stop.stop_requested(); ++i) {
        if (left_out[i]) continue;
        const Item& it = items[i];
        const Existing choice = it.choice.value_or(existing);
        auto src = sftp::lstat(c, raw_of(it.from));
        if (!src) {
            const std::string why = src.error().code == sftp::Code::no_such_file
                                        ? gone(w, it.from)
                                        : std::format("{}:{} can't be read: {}", w.server, it.from, sftp::describe(src.error()));
            p.entries.push_back(entry(Entry::State::failed, it.from, it.to, why));
            continue;
        }
        if (src->is_dir() && (it.to == it.from || canonical(it.to, home) == canonical(it.from, home))) {
            p.refused = std::format("{} can't be copied onto itself", name_of(it.from));
            return p;
        }
        if (!src->is_dir() && !src->is_file() && !src->is_link()) {
            p.entries.push_back(entry(Entry::State::skipped, it.from, it.to, "not a file, folder or link"));
            continue;
        }
        const std::string dir = parent_of(it.to);
        auto there = look_there(c, raw_of(it.to));
        if (!there) {
            p.entries.push_back(entry(Entry::State::failed, it.from, it.to, refused(k, w, dir, there.error()).why));
            continue;
        }
        if (src->is_dir()) {
            std::string to = it.to;
            bool merge = false;
            if (*there && (*there)->own.is_dir()) {
                merge = true;
            } else if (*there) {
                const upload::Clash clash = clash_of(it.to, *src, (*there)->through);
                p.taken.push_back(entry(Entry::State::asking, it.from, it.to, "already there", clash));
                p.unwalked.push_back(p.taken.back());
                const upload::Decision d = decide_whole(choice, clash, true, false);
                if (d.action == upload::Action::ask) {
                    p.entries.push_back(entry(Entry::State::asking, it.from, it.to, "already there", clash));
                    continue;
                }
                if (d.action == upload::Action::skip || d.action == upload::Action::replace) {
                    p.entries.push_back(
                        entry(Entry::State::skipped, it.from, it.to, d.note.empty() ? "already there" : d.note, clash));
                    continue;
                }
                auto numbered = free_numbered(c, dir, name_of(it.to), true, reserved);
                if (!numbered) {
                    p.entries.push_back(entry(Entry::State::failed, it.from, it.to, refused(k, w, dir, numbered.error()).why));
                    continue;
                }
                to = *numbered;
                reserved.insert(to);
            }
            if (plan_folder(k, w, p, it, i, *src, to, merge, choice, stop)) report.placed[i] = to;
            continue;
        }
        ++p.total_files;
        p.total_bytes += src->is_file() ? src->size.value_or(0) : 0;
        CopyFile f{.from = it.from,
                   .to = it.to,
                   .attrs = *src,
                   .choice = choice,
                   .free = true,
                   .link = src->is_link(),
                   .target = {},
                   .item = i,
                   .top = true};
        if (f.link) {
            auto read = sftp::readlink(c, raw_of(it.from));
            if (!read) {
                p.entries.push_back(entry(Entry::State::skipped, it.from, it.to,
                                          std::format("the link couldn't be read: {}", sftp::describe(read.error()))));
                continue;
            }
            f.target = std::move(*read);
        }
        if (*there) decide_file(p, std::move(f), (*there)->through);
        else p.files.push_back(std::move(f));
    }
    return p;
}

// ---- copy: the run --------------------------------------------------------------------------------------

// What a copy made that it removes when it ends early: its temps on the server not placed nor
// removed yet (raw), and the folders it made (grab's form, parents first), each told to
// Where::on_undo as it comes and goes (Undo). Outside the connection's step, so a connection lost on
// the way can still be cleaned after.
class Made {
public:
    explicit Made(const Where& w) : w_(w) {}
    [[nodiscard]] const std::set<std::string>& temps() const { return temps_; }
    [[nodiscard]] const std::vector<std::string>& folders() const { return folders_; }
    // A temp, from just before it is asked for: a connection lost before the answer can still have
    // made it (its random name is grab's alone, so removing it after is safe).
    void temp(const std::string& raw) {
        if (temps_.insert(raw).second) tell(Undo::Kind::temp, rclone_enc::from_raw_path(raw), true);
    }
    // Placed, removed, not made, or left for the user (the only copy of a file Replace removed).
    void temp_gone(const std::string& raw) {
        if (temps_.erase(raw) > 0) tell(Undo::Kind::temp, rclone_enc::from_raw_path(raw), false);
    }
    void folder(const std::string& path) {
        folders_.push_back(path);
        tell(Undo::Kind::folder, path, true);
    }
    void folder_gone(const std::string& path) {
        if (std::erase(folders_, path) > 0) tell(Undo::Kind::folder, path, false);
    }
    // The copy is over and its folders left are its own: it ran to its end, or they hold what it
    // placed (an empty one was removed).
    void folders_kept() {
        for (const std::string& f : std::exchange(folders_, {})) tell(Undo::Kind::folder, f, false);
    }

private:
    void tell(Undo::Kind kind, std::string path, bool made) const {
        if (w_.on_undo) w_.on_undo(Undo{.kind = kind, .path = std::move(path), .made = made});
    }
    const Where& w_;
    std::set<std::string> temps_;
    std::vector<std::string> folders_;
};

// A copy as it runs on its connection.
struct Copying {
    const Where& w;
    Conn& k;
    Report& report;
    const OnProgress& on_progress;
    const std::stop_token& stop;
    Progress progress;
    Made& made;
    std::set<std::string> reserved;  // every place the copy fills: no Keep-both copy takes one
    std::optional<std::string> fatal; // a refusal that ends the copy (the whole server's)
    std::set<std::string> blocked;    // folders that couldn't be made: nothing goes below them
    // Folders the server refused a new file in (no write access there), with its words: each later
    // file for one fails with them, unstarted; the rest go on.
    std::map<std::string, std::string> refused_dirs;

    [[nodiscard]] bool ending() const { return fatal.has_value() || stop.stop_requested(); }
    void notify() {
        if (on_progress) on_progress(progress);
    }
    void done(const CopyFile& f, const std::string& placed) {
        ++report.files;
        if (!f.link) report.bytes += f.attrs.size.value_or(0);
        if (f.top) report.placed[f.item] = placed;
        ++progress.files;
        progress.bytes = report.bytes;
        notify();
    }
    // A refusal of a write for `f`: the file's alone, its folder's (every new file there), or the
    // whole server's (read-only, full), which ends the copy.
    void refused_write(const CopyFile& f, const sftp::Status& status, std::uint64_t needed) {
        if (status.code == sftp::Code::no_such_file) { // its folder went meanwhile
            report.entries.push_back(entry(Entry::State::failed, f.from, f.to, gone(w, parent_of(f.to))));
            return;
        }
        const upload::Refusal r = refused(k, w, parent_of(f.to), status, needed);
        if (r.scope == upload::Refusal::Scope::server) {
            if (!fatal) fatal = r.why;
            return;
        }
        if (r.scope == upload::Refusal::Scope::folder) refused_dirs.emplace(parent_of(f.to), r.why);
        report.entries.push_back(entry(Entry::State::failed, f.from, f.to, r.why));
    }
    // `f`'s folder was refused a new file before: it fails with those words, unstarted (true).
    [[nodiscard]] bool in_refused_dir(const CopyFile& f) {
        const auto it = refused_dirs.find(parent_of(f.to));
        if (it == refused_dirs.end()) return false;
        report.entries.push_back(entry(Entry::State::failed, f.from, f.to, it->second));
        return true;
    }
    [[nodiscard]] bool below_blocked(const std::string& path) const {
        return std::ranges::any_of(blocked, [&](const std::string& b) { return path == b || browse::is_below(path, b); });
    }
};

// The temp a copy of `to` is written to first, in its folder (grab's form).
std::string temp_for(const std::string& to) {
    const auto [dir, name] = upload::split_remote(to);
    return upload::remote_child(dir, upload::temp_name(rclone_enc::raw_path(name), upload::new_token()));
}

// The folders to make, top-down, each level's MKDIRs together, each made no more open to others
// than its source (its mode with the owner's rwx added, so the copy can fill it; the exact mode
// comes last, set_dir_attrs): a copy stopped or failed midway leaves nothing more open than its
// source was. A name there already that is a folder is merged into; any other refusal fails that
// folder and what goes below it, or the whole copy (a read-only server).
void make_dirs(Copying& run, std::vector<CopyDir>& dirs) {
    sftp::Client& c = *run.k.c;
    std::vector<std::size_t> order;
    for (std::size_t d = 0; d < dirs.size(); ++d)
        if (dirs[d].make) order.push_back(d);
    std::ranges::stable_sort(order, {}, [&](std::size_t d) { return depth_of(dirs[d].to); });
    for (std::size_t from = 0; from < order.size() && !run.ending();) {
        const std::size_t level = depth_of(dirs[order[from]].to);
        std::size_t to = from;
        while (to < order.size() && depth_of(dirs[order[to]].to) == level) ++to;
        for (std::size_t at = from; at < to && !run.ending(); at += batch) {
            const std::size_t end = std::min(to, at + batch);
            std::vector<std::optional<sftp::Status>> failed(end - at);
            std::vector<std::optional<sftp::Attrs>> there(end - at);
            run.k.changed = true;
            for (std::size_t i = at; i < end; ++i) {
                const CopyDir& d = dirs[order[i]];
                if (run.below_blocked(d.to)) continue;
                tell(run.w, {"sftp", "mkdir", d.to});
                const sftp::Attrs mode{.size = {},
                                       .permissions = (d.attrs.permissions ? *d.attrs.permissions & mode_bits : 0) | 0700,
                                       .mtime = {},
                                       .uid = {},
                                       .gid = {},
                                       .atime = {}};
                c.mkdir(raw_of(d.to), mode, [&failed, i, at](std::expected<void, sftp::Status> r) {
                    if (!r) failed[i - at] = r.error();
                });
            }
            while (c.pending() > 0) c.pump();
            for (std::size_t i = at; i < end; ++i) {
                if (!failed[i - at] || failed[i - at]->code == sftp::Code::no_such_file) continue;
                c.lstat(raw_of(dirs[order[i]].to), [&there, i, at](std::expected<sftp::Attrs, sftp::Status> a) {
                    if (a) there[i - at] = *a;
                });
            }
            while (c.pending() > 0) c.pump();
            for (std::size_t i = at; i < end; ++i) {
                CopyDir& d = dirs[order[i]];
                if (run.below_blocked(d.to)) continue;
                if (!failed[i - at]) {
                    run.made.folder(d.to);
                    ++run.report.folders;
                    continue;
                }
                if (there[i - at] && there[i - at]->is_dir()) { // made meanwhile: merged into
                    d.make = false;
                    continue;
                }
                run.blocked.insert(d.to);
                std::string why;
                if (there[i - at]) {
                    why = std::format("{}:{} is a file, so the folder can't be made there", run.w.server, d.to);
                } else if (failed[i - at]->code == sftp::Code::no_such_file) {
                    why = gone(run.w, parent_of(d.to));
                } else {
                    const upload::Refusal r = refused(run.k, run.w, parent_of(d.to), *failed[i - at]);
                    if (r.scope == upload::Refusal::Scope::server && !run.fatal) run.fatal = r.why;
                    why = r.why;
                }
                if (!run.fatal) run.report.entries.push_back(entry(Entry::State::failed, d.from, d.to, why));
            }
        }
        from = to;
    }
}

// A file copied into its temp, put in place by the rules (upload::place): Replace and Keep both,
// a name taken meanwhile, Ask (the temp removed, an asking entry).
void place(Copying& run, const CopyFile& f, const std::string& temp) {
    sftp::Client& c = *run.k.c;
    const std::string temp_raw = raw_of(temp);
    std::string renaming;
    const upload::PlaceRequest request{.server = run.w.server,
                                       .temp = temp,
                                       .target = f.to,
                                       .existing = f.choice,
                                       .size = f.attrs.size.value_or(0),
                                       .mtime = f.attrs.mtime.value_or(0),
                                       .on_command = run.w.on_command,
                                       .reserved = [&run](std::string_view p) { return run.reserved.contains(std::string(p)); }};
    auto placed = upload::place(c, run.k.caps, request, renaming);
    if (!placed) {
        if (placed.error().kept_temp) {
            run.made.temp_gone(temp_raw); // the only copy of what was there now: it stays, where the words say
        } else if (auto removed = sftp::remove(c, temp_raw); removed || removed.error().code == sftp::Code::no_such_file) {
            run.made.temp_gone(temp_raw);
        }
        if (placed.error().scope == upload::Refusal::Scope::server && !run.fatal) run.fatal = placed.error().why;
        else run.report.entries.push_back(entry(Entry::State::failed, f.from, f.to, placed.error().why));
        return;
    }
    switch (placed->outcome) {
    case upload::Outcome::done:
        run.made.temp_gone(temp_raw);
        run.done(f, placed->placed);
        return;
    case upload::Outcome::skipped: // place removed the temp
        run.made.temp_gone(temp_raw);
        run.report.entries.push_back(entry(Entry::State::skipped, f.from, f.to, placed->note.empty() ? "already there" : placed->note,
                                           placed->clash));
        return;
    case upload::Outcome::exists: {
        // Ask: nothing placed, the temp goes; the file waits for the user's answer.
        if (auto removed = sftp::remove(c, temp_raw); removed || removed.error().code == sftp::Code::no_such_file) {
            run.made.temp_gone(temp_raw);
        }
        run.report.entries.push_back(entry(Entry::State::asking, f.from, f.to, "already there", placed->clash));
        return;
    }
    case upload::Outcome::stopped:
    case upload::Outcome::failed: return; // place gives neither
    }
}

// The attributes a copy gets: the source's mode (read, write and execute) and times.
sftp::Attrs copied_attrs(const sftp::Attrs& from) {
    return sftp::Attrs{.size = {},
                       .permissions = from.permissions ? std::optional<std::uint32_t>(*from.permissions & mode_bits) : std::nullopt,
                       .mtime = from.mtime,
                       .uid = {},
                       .gid = {},
                       .atime = from.mtime ? from.atime : std::nullopt};
}

// Small files, `copy_width` at a time on the connection: each one's OPENs, then copy-data of the
// whole file (to its end), FSTATs of both, the times and mode (FSETSTAT, its refusal is kept as the
// server's own), both CLOSEs, then a RENAME into a free place; a place taken, or one with a choice
// to apply, goes to `later` for upload::place. A write refused pauses the window: its words decide
// after the replies came whether the rest go on.
void copy_small(Copying& run, const std::vector<const CopyFile*>& files, std::vector<std::pair<const CopyFile*, std::string>>& later) {
    sftp::Client& c = *run.k.c;
    struct Slot {
        const CopyFile* f = nullptr;
        std::string temp;
        std::string temp_raw;
        std::optional<std::expected<std::string, sftp::Status>> from, to;
        std::optional<sftp::Status> copy_failed;
        std::optional<sftp::Attrs> from_after, to_after;
        int left = 0;
    };
    std::size_t next = 0;
    while (next < files.size() && !run.ending()) {
        std::uint64_t in_flight = 0;
        std::size_t going = 0;
        // Writes refused, decided once the replies came (refused() asks statvfs, which can't wait
        // inside a reply's callback): the file, and the refusal.
        std::vector<std::pair<const CopyFile*, sftp::Status>> write_refused;
        const auto end = [&](const Slot& s) {
            in_flight -= s.f->attrs.size.value_or(0);
            --going;
        };
        const auto drop_temp = [&](const std::shared_ptr<Slot>& s) {
            c.remove(s->temp_raw, [&run, &end, s](std::expected<void, sftp::Status> r) {
                if (r || r.error().code == sftp::Code::no_such_file) run.made.temp_gone(s->temp_raw);
                end(*s);
            });
        };
        const auto copied = [&](const std::shared_ptr<Slot>& s) {
            if (--s->left > 0) return;
            const CopyFile& f = *s->f;
            if (s->copy_failed) {
                if (s->copy_failed->code == sftp::Code::bad_message) {
                    run.report.entries.push_back(entry(Entry::State::failed, f.from, f.to, sftp::describe(*s->copy_failed)));
                } else {
                    write_refused.emplace_back(&f, *s->copy_failed);
                }
                return drop_temp(s);
            }
            const bool same = s->from_after && s->to_after && s->from_after->size && s->from_after->size == s->to_after->size &&
                              s->from_after->size == f.attrs.size && s->from_after->mtime == f.attrs.mtime;
            if (!same) {
                run.report.entries.push_back(entry(Entry::State::failed, f.from, f.to, "it changed while it was copied"));
                return drop_temp(s);
            }
            if (run.stop.stop_requested()) return drop_temp(s);
            if (!f.free) {
                later.emplace_back(&f, s->temp);
                return end(*s);
            }
            tell(run.w, {"sftp", "rename", s->temp, f.to});
            c.rename(s->temp_raw, raw_of(f.to), [&run, &later, &end, s](std::expected<void, sftp::Status> r) {
                if (r) {
                    run.made.temp_gone(s->temp_raw);
                    run.done(*s->f, s->f->to);
                } else {
                    later.emplace_back(s->f, s->temp); // a name taken meanwhile, or a refusal: place says
                }
                end(*s);
            });
        };
        const auto both_open = [&](const std::shared_ptr<Slot>& s) {
            if (!s->from || !s->to) return; // the other's answer is still coming
            const CopyFile& f = *s->f;
            if (!*s->from || !*s->to || run.stop.stop_requested()) {
                if (*s->from) c.close(**s->from);
                if (!*s->from && !run.stop.stop_requested()) {
                    const sftp::Status& st = s->from->error();
                    run.report.entries.push_back(entry(Entry::State::failed, f.from, f.to, unread(run.w, f.from, st)));
                }
                if (*s->to) {
                    c.close(**s->to);
                    return drop_temp(s);
                }
                if (!run.stop.stop_requested() && *s->from) write_refused.emplace_back(&f, s->to->error());
                return end(*s);
            }
            const std::string& from = **s->from;
            const std::string& to = **s->to;
            s->left = 6;
            tell(run.w, {"sftp", "copy-data", f.from, s->temp});
            c.copy_data(from, 0, 0, to, 0, [s, copied](std::expected<void, sftp::Status> r) {
                if (!r) s->copy_failed = r.error();
                copied(s);
            });
            c.fstat(from, [s, copied](std::expected<sftp::Attrs, sftp::Status> a) {
                if (a) s->from_after = *a;
                copied(s);
            });
            c.fstat(to, [s, copied](std::expected<sftp::Attrs, sftp::Status> a) {
                if (a) s->to_after = *a;
                copied(s);
            });
            // Refused (a server that keeps its own times): the copy is placed all the same.
            c.fsetstat(to, copied_attrs(f.attrs), [s, copied](std::expected<void, sftp::Status>) { copied(s); });
            c.close(from, [s, copied](std::expected<void, sftp::Status>) { copied(s); });
            c.close(to, [s, copied](std::expected<void, sftp::Status>) { copied(s); });
        };
        const auto start = [&] {
            const CopyFile& f = *files[next++];
            auto s = std::make_shared<Slot>();
            s->f = &f;
            s->temp = temp_for(f.to);
            s->temp_raw = raw_of(s->temp);
            in_flight += f.attrs.size.value_or(0);
            ++going;
            run.k.changed = true;
            c.open(raw_of(f.from), [s, both_open](std::expected<std::string, sftp::Status> h) {
                s->from = std::move(h);
                both_open(s);
            });
            const sftp::Attrs mode{.size = {},
                                   .permissions = f.attrs.permissions ? std::optional<std::uint32_t>(*f.attrs.permissions & mode_bits)
                                                                      : std::nullopt,
                                   .mtime = {},
                                   .uid = {},
                                   .gid = {},
                                   .atime = {}};
            // A temp from the moment it is asked for: a connection lost before the answer can still
            // have made it (its random name is grab's alone, so removing it after is safe).
            run.made.temp(s->temp_raw);
            c.open(s->temp_raw, sftp::OpenFlags::write | sftp::OpenFlags::creat | sftp::OpenFlags::excl, mode,
                   [&run, s, both_open](std::expected<std::string, sftp::Status> h) {
                       if (!h) run.made.temp_gone(s->temp_raw); // not made: whatever is there is never removed
                       s->to = std::move(h);
                       both_open(s);
                   });
        };
        for (;;) {
            while (next < files.size() && going < copy_width && (going == 0 || in_flight < copy_window_bytes) &&
                   write_refused.empty() && !run.ending()) {
                if (run.below_blocked(files[next]->to) || run.in_refused_dir(*files[next])) {
                    ++next;
                    continue;
                }
                start();
            }
            if (going == 0) break;
            c.pump();
        }
        // The refusals' words (statvfs asked): one for the file alone, or the end of the copy.
        for (const auto& [f, status] : write_refused) run.refused_write(*f, status, f->attrs.size.value_or(0));
    }
}

// A big file, in pieces sized to the connection's timeout (sftp::copy_range), progress after each;
// a stop between pieces removes its temp.
void copy_big(Copying& run, const CopyFile& f) {
    sftp::Client& c = *run.k.c;
    if (run.in_refused_dir(f)) return;
    run.k.changed = true;
    auto from = sftp::open_file(c, raw_of(f.from));
    if (!from) {
        run.report.entries.push_back(entry(Entry::State::failed, f.from, f.to, unread(run.w, f.from, from.error())));
        return;
    }
    const auto close_from = [&] { c.close(*from); };
    // The source as it is now (the walk's look when the server won't say): it must stay so.
    const auto fstat_before = sftp::fstat(c, *from);
    const sftp::Attrs before = fstat_before && fstat_before->size ? *fstat_before : f.attrs;
    const std::uint64_t size = before.size.value_or(0);
    const std::string temp = temp_for(f.to);
    const std::string temp_raw = raw_of(temp);
    sftp::Attrs mode = copied_attrs(f.attrs);
    mode.mtime.reset();
    mode.atime.reset();
    tell(run.w, {"sftp", "copy-data", f.from, temp});
    run.made.temp(temp_raw); // from the moment it is asked for, as copy_small's
    auto to = sftp::open_file(c, temp_raw, sftp::OpenFlags::write | sftp::OpenFlags::creat | sftp::OpenFlags::excl, mode);
    if (!to) {
        run.made.temp_gone(temp_raw);
        close_from();
        run.refused_write(f, to.error(), size);
        return;
    }
    const std::uint64_t base = run.report.bytes;
    auto copied = sftp::copy_range(
        c, *from, 0, size, *to, 0, run.w.timeout,
        [&](std::uint64_t n) {
            run.progress.bytes = base + n;
            run.notify();
        },
        run.stop);
    const auto drop = [&] {
        c.close(*to);
        close_from();
        if (auto removed = sftp::remove(c, temp_raw); removed || removed.error().code == sftp::Code::no_such_file) {
            run.made.temp_gone(temp_raw);
        }
        run.progress.bytes = run.report.bytes;
    };
    if (!copied) {
        drop();
        if (copied.error().code == sftp::Code::eof) {
            run.report.entries.push_back(entry(Entry::State::failed, f.from, f.to, "it changed while it was copied"));
        } else {
            run.refused_write(f, copied.error(), size);
        }
        return;
    }
    if (*copied < size) { // stopped
        drop();
        return;
    }
    const auto after = sftp::fstat(c, *from);
    if (!after || after->size != before.size || after->mtime != before.mtime) {
        drop();
        run.report.entries.push_back(entry(Entry::State::failed, f.from, f.to, "it changed while it was copied"));
        return;
    }
    // Refused (a server that keeps its own times): the copy is placed all the same.
    [[maybe_unused]] auto timed = sftp::fsetstat(c, *to, copied_attrs(f.attrs));
    [[maybe_unused]] auto closed = sftp::close_file(c, *to);
    close_from();
    place(run, f, temp);
}

// A link made again as a link, under a temp name first and then put in place as a file is (the
// choice for a name taken). A server that makes none, or makes it the other way round (SYMLINK's
// paths in the draft's order: then the link it made, provably grab's, is removed again), skips it
// with the reason.
void copy_link(Copying& run, const CopyFile& f) {
    sftp::Client& c = *run.k.c;
    if (run.in_refused_dir(f)) return;
    const std::string temp = temp_for(f.to);
    const std::string temp_raw = raw_of(temp);
    run.k.changed = true;
    tell(run.w, {"sftp", "symlink", rclone_enc::from_raw_path(f.target), temp});
    run.made.temp(temp_raw); // from the moment it is asked for, as copy_small's
    auto made = sftp::symlink(c, temp_raw, f.target);
    if (!made) {
        run.made.temp_gone(temp_raw);
        if (made.error().code == sftp::Code::no_such_file && !sftp::lstat(c, raw_of(parent_of(f.to)))) {
            run.report.entries.push_back(entry(Entry::State::failed, f.from, f.to, gone(run.w, parent_of(f.to))));
            return;
        }
        const upload::Refusal r = refused(run.k, run.w, parent_of(f.to), made.error());
        if (r.scope == upload::Refusal::Scope::server && !run.fatal) {
            run.fatal = r.why;
            return;
        }
        run.report.entries.push_back(entry(Entry::State::skipped, f.from, f.to,
                                           std::format("the server didn't make the link: {}", sftp::describe(made.error()))));
        return;
    }
    auto back = sftp::readlink(c, temp_raw);
    if (!back || *back != f.target) {
        if (back) {
            if (sftp::remove(c, temp_raw)) run.made.temp_gone(temp_raw);
        } else {
            run.made.temp_gone(temp_raw); // nothing at the temp's name
            // The draft's order: a link at the target's path pointing to the temp's name, which
            // only this copy can have made.
            if (auto odd = sftp::readlink(c, f.target); odd && *odd == temp_raw) {
                [[maybe_unused]] auto removed = sftp::remove(c, f.target);
            }
        }
        run.report.entries.push_back(entry(Entry::State::skipped, f.from, f.to, "the server makes links the other way round"));
        return;
    }
    place(run, f, temp);
}

// Folders this copy made get their source's mode and times last, deepest first (adding their
// files changed the times; a folder that may only be read takes nothing more once it is).
void set_dir_attrs(Copying& run, const std::vector<CopyDir>& dirs) {
    sftp::Client& c = *run.k.c;
    std::vector<const CopyDir*> mine;
    for (const CopyDir& d : dirs)
        if (d.make && std::ranges::find(run.made.folders(), d.to) != run.made.folders().end()) mine.push_back(&d);
    std::ranges::stable_sort(mine, std::greater<>(), [](const CopyDir* d) { return depth_of(d->to); });
    for (std::size_t at = 0; at < mine.size(); at += batch) {
        for (std::size_t i = at; i < std::min(mine.size(), at + batch); ++i) {
            c.setstat(raw_of(mine[i]->to), copied_attrs(mine[i]->attrs), {}); // a server that keeps its own: no matter
        }
        while (c.pending() > 0) c.pump();
    }
}

// The temps a copy left (none, unless it ended early); after a stop or a failure that ended it
// (`ended`), also the folders it made that are still empty (RMDIR deepest first: one holding a file
// placed meanwhile stays, as does that file). A temp the server didn't remove stays in `made`.
void clean(sftp::Client& c, const Where& w, Made& made, std::size_t& folders, std::set<std::string>& removed, bool ended) {
    const std::set<std::string> temps = made.temps();
    for (const std::string& t : temps) {
        tell(w, {"sftp", "rm", rclone_enc::from_raw_path(t)});
        c.remove(t, [&made, t](std::expected<void, sftp::Status> r) {
            if (r || r.error().code == sftp::Code::no_such_file) made.temp_gone(t);
        });
    }
    while (c.pending() > 0) c.pump();
    if (!ended) return;
    std::map<std::size_t, std::vector<std::string>, std::greater<>> by_depth;
    for (const std::string& d : made.folders()) by_depth[depth_of(d)].push_back(d);
    for (const auto& [depth, level] : by_depth) {
        for (const std::string& d : level) {
            tell(w, {"sftp", "rmdir", d});
            c.rmdir(raw_of(d), [&folders, &made, &removed, d](std::expected<void, sftp::Status> r) {
                if (!r) return; // not empty: what was placed in it stays
                --folders;
                made.folder_gone(d);
                removed.insert(d);
            });
        }
        while (c.pending() > 0) c.pump();
    }
}

} // namespace

std::string no_copy_data(std::string_view server) {
    return std::format("{} doesn't copy files itself (its SSH server is older than OpenSSH 9.0)", server);
}

std::chrono::milliseconds close_wait(std::span<const std::chrono::milliseconds> copies, bool restarting,
                                     std::chrono::milliseconds cleanup) {
    std::chrono::milliseconds piece{0};
    if (!restarting) {
        for (const auto t : copies) piece = std::max(piece, t > std::chrono::milliseconds{0} ? std::min(t, close_piece_most) : close_piece_most);
    }
    return piece + cleanup;
}

Report copy(const Where& w, std::span<const Item> items, Existing existing, const OnProgress& progress, std::stop_token stop) {
    Report report;
    report.placed.assign(items.size(), std::string());
    if (auto why = refused_before(items, true, w.home)) {
        report.error = *why;
        return report;
    }
    Made made(w);
    std::set<std::string> removed; // folders made, then removed again by clean
    const auto ran = on_connection<bool>(w, stop, [&](Conn& k) -> std::expected<bool, std::string> {
        restart(report, 0, items.size());
        sftp::Client& c = *k.c;
        if (!c.has_extension(sftp::copy_data_extension)) return util::fail(no_copy_data(w.server));
        auto home = home_of(k, w);
        if (!home) return util::fail(home.error());
        if (auto why = refused_before(items, true, *home)) {
            report.error = *why;
            return true;
        }
        if (auto why = top_folder_really(c, froms(items), "copied", *home, false)) {
            report.error = *why;
            return true;
        }
        Plan p = plan_copy(k, w, items, existing, report, stop, *home);
        if (p.refused) {
            report.error = *p.refused;
            std::ranges::fill(report.placed, std::string());
            return true;
        }
        std::ranges::move(p.entries, std::back_inserter(report.entries));
        if (stop.stop_requested()) {
            report.stopped = true;
            std::ranges::fill(report.placed, std::string());
            return true;
        }
        Copying run{.w = w,
                    .k = k,
                    .report = report,
                    .on_progress = progress,
                    .stop = stop,
                    .progress = Progress{.bytes = 0, .total_bytes = 0, .files = 0, .total_files = p.files.size()},
                    .made = made,
                    .reserved = {},
                    .fatal = {},
                    .blocked = {},
                    .refused_dirs = {}};
        std::vector<const CopyFile*> small_files;
        std::vector<const CopyFile*> big_files;
        std::vector<const CopyFile*> links;
        for (const CopyFile& f : p.files) {
            run.reserved.insert(f.to);
            if (f.link) {
                links.push_back(&f);
                continue;
            }
            run.progress.total_bytes += f.attrs.size.value_or(0);
            (f.attrs.size.value_or(0) <= small_copy ? small_files : big_files).push_back(&f);
        }
        run.notify();
        make_dirs(run, p.dirs);
        std::vector<std::pair<const CopyFile*, std::string>> later;
        copy_small(run, small_files, later);
        for (const auto& [f, temp] : later) {
            if (run.ending()) break;
            place(run, *f, temp);
        }
        for (const CopyFile* f : big_files) {
            if (run.ending()) break;
            if (!run.below_blocked(f->to)) copy_big(run, *f);
        }
        for (const CopyFile* f : links) {
            if (run.ending()) break;
            if (!run.below_blocked(f->to)) copy_link(run, *f);
        }
        if (!run.ending()) set_dir_attrs(run, p.dirs);
        // The folder items that didn't get there.
        for (const std::string& b : run.blocked) {
            for (std::string& placed : report.placed)
                if (placed == b) placed.clear();
        }
        if (run.fatal) report.error = *run.fatal;
        if (stop.stop_requested()) report.stopped = true;
        if (run.ending() || !made.temps().empty()) clean(c, w, made, report.folders, removed, run.ending());
        return true;
    });
    ended(ran, stop, report);
    bool over = ran.has_value();
    // A connection lost on the way: its temps and empty folders removed over another, once.
    if (!ran && (!made.temps().empty() || !made.folders().empty())) {
        const auto cleaned = on_connection<bool>(w, {}, [&](Conn& k) -> std::expected<bool, std::string> {
            clean(*k.c, w, made, report.folders, removed, true);
            return true;
        });
        over = cleaned.has_value();
    }
    // The folders it made that are left are its own now: it ran to its end, or they hold what it
    // placed. Not when the cleaning couldn't run (grab quitting, the server out of reach): then they
    // and the temps left are what it couldn't undo (Where::on_undo has them).
    if (over) made.folders_kept();
    // The folder items whose folders were made and then removed again (empty, after a stop).
    for (std::string& placed : report.placed)
        if (removed.contains(placed)) placed.clear();
    return report;
}

std::expected<Look, std::string> look(const Where& w, std::span<const Item> items, bool copying, std::stop_token stop) {
    if (auto why = refused_before(items, copying, w.home)) return util::fail(*why);
    return on_connection<Look>(w, stop, [&](Conn& k) -> std::expected<Look, std::string> {
        // Before anything is walked: the question would ask about a copy that can't run.
        if (copying && !k.c->has_extension(sftp::copy_data_extension)) return util::fail(no_copy_data(w.server));
        auto home = home_of(k, w);
        if (!home) return util::fail(home.error());
        if (auto why = refused_before(items, copying, *home)) return util::fail(*why);
        if (auto why = top_folder_really(*k.c, froms(items), copying ? "copied" : "moved", *home, !copying)) return util::fail(*why);
        Look l;
        Report report;
        report.placed.assign(items.size(), std::string());
        if (copying) {
            const Plan p = plan_copy(k, w, items, Existing::ask, report, stop, *home);
            if (p.refused) return util::fail(*p.refused);
            l.files = p.total_files;
            l.folders = p.total_folders;
            l.bytes = p.total_bytes;
            // A folder whose name is taken by a file isn't walked by the plan (it goes only with
            // Keep both): counted here, so the question's counts hold all that may be copied.
            for (const Entry& e : p.unwalked) {
                if (!e.clash->remote_is_dir) l.folders_onto_files.add(*e.clash);
                auto tree = walk(*k.c, raw_of(e.from), stop);
                if (!tree || tree->stopped) continue;
                l.folders += 1 + tree->folders.size();
                l.files += tree->files.size() + tree->links.size();
                l.bytes += tree->bytes();
            }
            for (const Entry& e : p.taken) {
                l.taken.add(*e.clash);
                if (l.clashes.size() < max_clashes) l.clashes.push_back(e);
            }
        } else {
            sftp::Client& c = *k.c;
            for (const Item& it : items) {
                if (stop.stop_requested()) break;
                auto src = sftp::lstat(c, raw_of(it.from));
                if (!src) continue; // the move says so
                (src->is_dir() ? l.folders : l.files) += 1;
                if (src->is_file()) l.bytes += src->size.value_or(0);
                if (it.to == it.from) continue;
                auto there = look_there(c, raw_of(it.to));
                if (!there || !*there) continue;
                const upload::Clash clash = clash_of(it.to, *src, (*there)->through);
                l.taken.add(clash);
                if (src->is_dir() && !clash.remote_is_dir) l.folders_onto_files.add(clash);
                if (l.clashes.size() < max_clashes) {
                    l.clashes.push_back(entry(Entry::State::asking, it.from, it.to, "already there", clash));
                }
            }
        }
        if (stop.stop_requested()) return util::fail("stopped");
        return l;
    });
}

// ---- delete ----------------------------------------------------------------------------------------------

Report remove(const Where& w, std::span<const std::string> paths, const OnProgress& on_progress, std::stop_token stop) {
    Report report;
    for (const std::string& p : paths) {
        if (auto why = top_folder(p, "deleted", w.home, true)) {
            report.error = *why;
            return report;
        }
    }
    const auto ran = on_connection<bool>(w, stop, [&](Conn& k) -> std::expected<bool, std::string> {
        restart(report, 0, 0);
        sftp::Client& c = *k.c;
        auto home = home_of(k, w);
        if (!home) return util::fail(home.error());
        for (const std::string& p : paths) {
            if (auto why = top_folder(p, "deleted", *home, true)) {
                report.error = *why;
                return true;
            }
        }
        if (auto why = top_folder_really(c, paths, "deleted", *home, true)) {
            report.error = *why;
            return true;
        }
        // Every path looked at and every folder walked first: nothing is removed from a tree that
        // can't be read through.
        struct Target {
            std::string path;
            std::string raw;
            sftp::Attrs attrs;
            Walked tree;
        };
        std::vector<Target> targets;
        Progress progress;
        std::size_t total_folders = 0;
        for (const std::string& p : paths) {
            if (stop.stop_requested()) {
                report.stopped = true;
                return true;
            }
            const std::string raw = raw_of(p);
            auto a = sftp::lstat(c, raw);
            if (!a) {
                if (a.error().code == sftp::Code::no_such_file) {
                    report.entries.push_back(entry(Entry::State::skipped, p, {}, gone(w, p)));
                    continue;
                }
                report.error = std::format("{}:{} can't be looked at: {}", w.server, p, sftp::describe(a.error()));
                return true;
            }
            Target t{.path = p, .raw = raw, .attrs = *a, .tree = {}};
            if (a->is_dir()) {
                auto tree = walk(c, raw, stop);
                if (!tree) {
                    report.error = std::format("{}:{} can't be read: {}", w.server, p, sftp::describe(tree.error()));
                    return true;
                }
                if (tree->stopped) {
                    report.stopped = true;
                    return true;
                }
                if (!tree->unreadable.empty()) {
                    const auto& [rel, status] = tree->unreadable.front();
                    report.error = std::format("{}:{} can't be deleted: {} can't be read ({}); nothing was deleted", w.server, p,
                                               below_grab(p, rel), sftp::describe(status));
                    return true;
                }
                t.tree = std::move(*tree);
                progress.total_files += t.tree.files.size() + t.tree.links.size() + t.tree.others.size();
                progress.total_bytes += t.tree.bytes();
                total_folders += 1 + t.tree.folders.size();
            } else {
                ++progress.total_files;
                progress.total_bytes += a->is_file() ? a->size.value_or(0) : 0;
            }
            targets.push_back(std::move(t));
        }
        if (on_progress) on_progress(progress);
        // The first refusal: what it refused, and why. Nothing more is sent after it.
        std::optional<std::pair<std::string, sftp::Status>> refusal;
        const auto removed = [&](std::uint64_t bytes) {
            ++report.files;
            report.bytes += bytes;
            ++progress.files;
            progress.bytes = report.bytes;
            if (on_progress) on_progress(progress);
        };
        const auto more = [&] { return !refusal && !stop.stop_requested(); };
        for (const Target& t : targets) {
            if (!more()) break;
            k.changed = true;
            if (!t.attrs.is_dir()) { // a file, a link (itself), anything but a folder
                tell(w, {"sftp", "rm", t.path});
                auto r = sftp::remove(c, t.raw);
                if (!r && r.error().code != sftp::Code::no_such_file) {
                    refusal.emplace(t.path, r.error());
                    break;
                }
                removed(t.attrs.is_file() ? t.attrs.size.value_or(0) : 0);
                continue;
            }
            std::vector<const Node*> names;
            for (const auto* group : {&t.tree.files, &t.tree.links, &t.tree.others})
                for (const Node& n : *group) names.push_back(&n);
            window(c, names.size(), remove_width, more, [&](std::size_t i, const std::function<void()>& finish) {
                const Node& n = *names[i];
                tell(w, {"sftp", "rm", below_grab(t.path, n.rel)});
                c.remove(below(t.raw, n.rel), [&, i, finish](std::expected<void, sftp::Status> r) {
                    const Node& done = *names[i];
                    if (r || r.error().code == sftp::Code::no_such_file) {
                        removed(done.attrs.is_file() ? done.attrs.size.value_or(0) : 0);
                    }
                    else if (!refusal) refusal.emplace(below_grab(t.path, done.rel), r.error());
                    finish();
                });
            });
            // Its folders deepest first, then itself.
            std::map<std::size_t, std::vector<std::string>, std::greater<>> by_depth;
            for (const Node& n : t.tree.folders) by_depth[depth_of(n.rel)].push_back(n.rel);
            for (const auto& [depth, level] : by_depth) {
                if (!more()) break;
                window(c, level.size(), remove_width, more, [&](std::size_t i, const std::function<void()>& finish) {
                    const std::string& rel = level[i];
                    tell(w, {"sftp", "rmdir", below_grab(t.path, rel)});
                    c.rmdir(below(t.raw, rel), [&, rel, finish](std::expected<void, sftp::Status> r) {
                        if (r || r.error().code == sftp::Code::no_such_file) ++report.folders;
                        else if (!refusal) refusal.emplace(below_grab(t.path, rel), r.error());
                        finish();
                    });
                });
            }
            if (!more()) break;
            tell(w, {"sftp", "rmdir", t.path});
            if (auto r = sftp::rmdir(c, t.raw); r || r.error().code == sftp::Code::no_such_file) ++report.folders;
            else refusal.emplace(t.path, r.error());
        }
        const std::size_t all = progress.total_files + total_folders;
        report.left = all - std::min(all, report.files + report.folders);
        if (stop.stop_requested()) report.stopped = true;
        if (refusal) {
            const std::string why = refused(k, w, parent_of(refusal->first), refusal->second).why;
            const std::string what = refusal->second.code == sftp::Code::failure && why == sftp::describe(refusal->second)
                                         ? std::format("{}:{} couldn't be deleted ({})", w.server, refusal->first, why)
                                         : why;
            report.error = report.files + report.folders == 0
                               ? std::format("{}; nothing was deleted", what)
                               : std::format("{}; {} of {} files and {} of {} folders were deleted, {} still there", what,
                                             report.files, progress.total_files, report.folders, total_folders, report.left);
        }
        return true;
    });
    ended(ran, stop, report);
    return report;
}

// ---- properties ------------------------------------------------------------------------------------------

std::expected<Properties, std::string> properties(const Where& w, std::string_view path, std::stop_token stop) {
    return on_connection<Properties>(w, stop, [&](Conn& k) -> std::expected<Properties, std::string> {
        sftp::Client& c = *k.c;
        const std::string raw = raw_of(path);
        auto a = sftp::lstat(c, raw);
        if (!a) {
            if (a.error().code == sftp::Code::no_such_file) return util::fail(gone(w, path));
            return util::failf("{}:{} can't be looked at: {}", w.server, path, sftp::describe(a.error()));
        }
        Properties p;
        p.attrs = *a;
        p.kind = a->is_dir()    ? Properties::Kind::folder
                 : a->is_file() ? Properties::Kind::file
                 : a->is_link() ? Properties::Kind::link
                                : Properties::Kind::other;
        if (a->is_link()) {
            if (auto to = sftp::readlink(c, raw)) p.link = rclone_enc::from_raw_path(*to);
            if (auto through = sftp::stat(c, raw)) p.target = *through;
        }
        if (a->uid && a->gid) {
            p.owner = std::to_string(*a->uid);
            p.group = std::to_string(*a->gid);
            if (c.has_extension(sftp::users_groups_extension)) {
                const std::uint32_t uid = *a->uid;
                const std::uint32_t gid = *a->gid;
                if (auto named = sftp::users_groups_by_id(c, std::span(&uid, 1), std::span(&gid, 1))) {
                    if (!named->users.empty() && !named->users[0].empty()) p.owner = rclone_enc::from_raw_name(named->users[0]);
                    if (!named->groups.empty() && !named->groups[0].empty()) p.group = rclone_enc::from_raw_name(named->groups[0]);
                    p.names = true;
                }
            }
        }
        if (k.caps.statvfs) {
            // A folder's own file system; a file's or link's is its folder's.
            const std::string on = a->is_dir() ? raw : raw_of(parent_of(path));
            if (auto v = sftp::statvfs(c, on)) {
                p.free = v->free_bytes();
                p.read_only = v->read_only();
            }
        }
        return p;
    });
}

std::expected<Size, std::string> folder_size(const Where& w, std::span<const std::string> paths, const OnProgress& progress,
                                             std::stop_token stop) {
    return on_connection<Size>(w, stop, [&](Conn& k) -> std::expected<Size, std::string> {
        sftp::Client& c = *k.c;
        Size s;
        for (const std::string& p : paths) {
            if (stop.stop_requested()) return util::fail("stopped");
            const std::string raw = raw_of(p);
            auto a = sftp::lstat(c, raw);
            if (!a) {
                if (a.error().code == sftp::Code::no_such_file) return util::fail(gone(w, p));
                return util::failf("{}:{} can't be looked at: {}", w.server, p, sftp::describe(a.error()));
            }
            if (!a->is_dir()) {
                if (a->is_link()) {
                    ++s.links;
                } else {
                    ++s.files;
                    s.bytes += a->size.value_or(0);
                }
                continue;
            }
            const Size before = s;
            auto tree = walk(c, raw, stop, [&](const Walked& so_far) {
                if (!progress) return;
                progress(Progress{.bytes = before.bytes + so_far.bytes(),
                                  .total_bytes = 0,
                                  .files = before.files + so_far.files.size() + so_far.others.size(),
                                  .total_files = 0});
            });
            if (!tree) return util::failf("{}:{} can't be read: {}", w.server, p, sftp::describe(tree.error()));
            if (tree->stopped) return util::fail("stopped");
            s.folders += 1 + tree->folders.size();
            s.files += tree->files.size() + tree->others.size();
            s.links += tree->links.size();
            s.bytes += tree->bytes();
            for (const auto& [rel, status] : tree->unreadable) {
                s.unreadable.push_back(std::format("{}: {}", below_grab(p, rel), sftp::describe(status)));
            }
        }
        return s;
    });
}

std::expected<std::string, std::string> hash(const Where& w, const RcloneRemote& server, std::string_view path, std::uint64_t size,
                                             ssh::Sum sum, std::stop_token stop) {
    if (!w.pool) return util::failf("no connection to {}", w.server);
    // One connection of the server's budget, as Verify's commands take theirs.
    std::optional<fetch::Listing> seat;
    if (w.connections != nullptr) {
        seat.emplace(*w.connections, w.server, stop, 1);
        if (seat->connections() == 0) return util::fail("stopped");
    }
    ssh::HashOptions o;
    o.sum = sum;
    o.stop = stop;
    o.connections = 1;
    o.server = server;
    o.retry_refused = ssh::interactive_retry;
    const ssh::HashFile file{.path = rclone_enc::raw_path(path), .size = size};
    const auto sums = ssh::run_hashes(*w.pool, std::span(&file, 1), o);
    if (sums.empty()) return util::fail("stopped");
    if (!sums[0]) return util::fail(sums[0].error().why);
    return *sums[0];
}

// ---- around them -----------------------------------------------------------------------------------------

std::expected<Account, std::string> account(const Where& w, std::stop_token stop) {
    return on_connection<Account>(w, stop, [&](Conn& k) -> std::expected<Account, std::string> {
        sftp::Client& c = *k.c;
        auto home = sftp::realpath(c, ".");
        if (!home) return util::failf("{}: the login folder can't be looked at: {}", w.server, sftp::describe(home.error()));
        Account a{.home = rclone_enc::from_raw_path(*home),
                  .read_only = false,
                  .free = std::nullopt,
                  .copy_data = c.has_extension(sftp::copy_data_extension)};
        if (k.caps.statvfs) {
            if (auto v = sftp::statvfs(c, *home)) {
                a.read_only = v->read_only();
                a.free = v->free_bytes();
            }
        }
        return a;
    });
}

std::vector<std::string> resolved(const Where& w, std::span<const std::string> paths, std::stop_token stop) {
    std::vector<std::string> out(paths.begin(), paths.end());
    [[maybe_unused]] auto asked = on_connection<bool>(w, stop, [&](Conn& k) -> std::expected<bool, std::string> {
        sftp::Client& c = *k.c;
        std::vector<std::optional<std::string>> real(paths.size());
        for (std::size_t at = 0; at < paths.size(); at += batch) {
            for (std::size_t i = at; i < std::min(paths.size(), at + batch); ++i) {
                if (paths[i].empty() || paths[i] == "/") continue;
                c.realpath(raw_of(parent_of(paths[i])), [&real, i](std::expected<std::string, sftp::Status> r) {
                    if (r) real[i] = std::move(*r);
                });
            }
            while (c.pending() > 0) c.pump();
        }
        for (std::size_t i = 0; i < paths.size(); ++i) {
            if (real[i]) out[i] = child(rclone_enc::from_raw_path(*real[i]), name_of(paths[i]));
        }
        return true;
    });
    return out;
}

std::vector<std::string> followed(const Where& w, std::span<const std::string> paths, std::stop_token stop) {
    std::vector<std::string> out(paths.begin(), paths.end());
    [[maybe_unused]] auto asked = on_connection<bool>(w, stop, [&](Conn& k) -> std::expected<bool, std::string> {
        sftp::Client& c = *k.c;
        for (std::size_t at = 0; at < paths.size(); at += batch) {
            for (std::size_t i = at; i < std::min(paths.size(), at + batch); ++i) {
                if (paths[i].empty() || paths[i] == "/") continue;
                c.realpath(raw_of(paths[i]), [&out, i](std::expected<std::string, sftp::Status> r) {
                    if (r) out[i] = rclone_enc::from_raw_path(*r);
                });
            }
            while (c.pending() > 0) c.pump();
        }
        return true;
    });
    return out;
}

std::string canonical(std::string_view path, std::string_view home) {
    std::string whole = !home.empty() && !path.starts_with('/') ? child(home, path) : std::string(path);
    const bool absolute = whole.starts_with('/');
    std::vector<std::string_view> parts;
    for (std::size_t at = 0; at <= whole.size();) {
        const std::size_t slash = std::min(whole.find('/', at), whole.size());
        const std::string_view part = std::string_view(whole).substr(at, slash - at);
        at = slash + 1;
        if (part.empty() || part == ".") continue;
        if (part == ".." && !parts.empty() && parts.back() != "..") parts.pop_back();
        else if (part != ".." || !absolute) parts.push_back(part); // ".." above "/" is "/"
    }
    std::string out = absolute ? "/" : "";
    for (std::size_t i = 0; i < parts.size(); ++i) out += std::format("{}{}", i == 0 ? "" : "/", parts[i]);
    return out;
}

std::optional<std::string> busy_refusal(std::span<const Busy> busy, std::span<const std::string> paths, std::string_view server,
                                        std::string_view home) {
    for (const std::string& p : paths) {
        const std::string pc = canonical(p, home);
        for (const Busy& b : busy) {
            const std::string bc = canonical(b.path, home);
            const bool same = pc == bc;
            const bool holds = !same && browse::is_below(bc, pc);
            const bool inside = !same && !holds && browse::is_below(pc, bc);
            if (!same && !holds && !inside) continue;
            const std::string_view doing = b.kind == Busy::Kind::download ? "being downloaded"
                                           : b.kind == Busy::Kind::upload ? "being uploaded"
                                                                          : "playing";
            const std::string_view then =
                b.kind == Busy::Kind::play ? "stop playing it first" : "wait until it ends, or cancel it first";
            if (same) return std::format("{}:{} is {}: {}", server, p, doing, then);
            if (holds) return std::format("{}:{} holds {}, which is {}: {}", server, p, b.path, doing, then);
            return std::format("{}:{} is inside {}, which is {}: {}", server, p, b.path, doing, then);
        }
    }
    return std::nullopt;
}

Invalidation invalidation(std::string_view server, const Changed& changed, std::string_view home) {
    Invalidation out;
    out.list = std::string(server);
    const auto mark = [&](const std::string& folder, bool below) {
        const auto it = std::ranges::find(out.folders, folder, &Mark::folder);
        if (it == out.folders.end()) out.folders.push_back(Mark{.folder = folder, .below = below});
        else it->below = it->below || below;
    };
    for (const auto* group : {&changed.from, &changed.to}) {
        for (const std::string& given : *group) {
            // As given, and (the login folder known) its other spellings: absolute, and from the
            // login folder for one below it, as Browse and search may each have it.
            std::vector<std::string> spellings{given};
            if (!home.empty()) {
                const std::string whole = canonical(given, home);
                const std::string top = canonical(home, {});
                spellings.push_back(whole);
                if (whole == top) spellings.emplace_back();
                else if (browse::is_below(whole, top)) spellings.push_back(whole.substr(top.size() + (top == "/" ? 0 : 1)));
            }
            for (const std::string& p : spellings) {
                if (std::ranges::find(out.forget, p) != out.forget.end()) continue;
                // Its folder lists it no more, or anew; a folder's own listings below it are gone or new.
                if (!p.empty() && p != "/") mark(parent_of(p), false);
                mark(p, true);
                out.forget.push_back(p);
            }
        }
    }
    return out;
}

} // namespace grab::server_files
