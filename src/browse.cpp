#include "browse.hpp"

#include "listing.hpp"
#include "rclone_encoding.hpp"
#include "remote.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_pool.hpp"
#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>

namespace grab::browse {

namespace {

// As a byte from 0 to 255, so names in other scripts (UTF-8) sort after digits and ASCII letters.
unsigned char lower(char c) { return static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(c))); }
bool digit(char c) { return c >= '0' && c <= '9'; }

// `path` relative to `root`, which holds it.
std::string_view relative(std::string_view path, std::string_view root) {
    if (path == root) return {};
    path.remove_prefix(root.size());
    while (path.starts_with('/')) path.remove_prefix(1);
    return path;
}

} // namespace

bool natural_less(std::string_view a, std::string_view b) {
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size()) {
        if (digit(a[i]) && digit(b[j])) {
            // Compare whole numbers: without leading zeros, the longer one is bigger.
            std::size_t ai = i;
            std::size_t bj = j;
            while (ai < a.size() && a[ai] == '0') ++ai;
            while (bj < b.size() && b[bj] == '0') ++bj;
            std::size_t ae = ai;
            std::size_t be = bj;
            while (ae < a.size() && digit(a[ae])) ++ae;
            while (be < b.size() && digit(b[be])) ++be;
            if (ae - ai != be - bj) return ae - ai < be - bj;
            if (const auto c = a.substr(ai, ae - ai).compare(b.substr(bj, be - bj)); c != 0) return c < 0;
            i = ae;
            j = be;
            continue;
        }
        const unsigned char x = lower(a[i]);
        const unsigned char y = lower(b[j]);
        if (x != y) return x < y;
        ++i;
        ++j;
    }
    if (a.size() - i != b.size() - j) return a.size() - i < b.size() - j;
    return a < b; // only case or leading zeros differ: still a strict order
}

void sort_items(std::vector<Item>& items) {
    std::ranges::stable_sort(items, [](const Item& x, const Item& y) {
        if (x.dir != y.dir) return x.dir;
        return natural_less(x.name, y.name);
    });
}

bool is_below(std::string_view path, std::string_view folder) {
    if (folder.empty()) return !path.empty() && !path.starts_with('/');
    if (folder == "/") return path.size() > 1 && path.starts_with('/');
    return path.size() > folder.size() + 1 && path.starts_with(folder) && path[folder.size()] == '/';
}

std::string address_path(std::string_view text) {
    text = util::trim(text);
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c != '/' || !out.ends_with('/')) out += c;
    }
    if (out == "~") return {};
    if (out.starts_with("~/")) out.erase(0, 2);
    while (out.size() > 1 && out.ends_with('/')) out.pop_back();
    return out;
}

std::optional<std::string> root_of(const std::vector<std::string>& roots, std::string_view path) {
    std::optional<std::string> best;
    for (const auto& r : roots) {
        if ((path == r || is_below(path, r)) && (!best || r.size() > best->size())) best = r;
    }
    return best;
}

Index::Index(std::shared_ptr<const file_list::List> list, std::vector<std::string> roots, int max_depth)
    : list_(std::move(list)), roots_(std::move(roots)) {
    if (roots_.empty()) roots_.emplace_back(); // the login home
    for (const auto& r : roots_) folders_[r].known = true;
    const auto& entries = list_->entries;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        const auto root = root_of(roots_, e.path);
        if (!root || e.path == *root) continue;
        folders_[parent_of(e.path)].children.push_back(i);
        if (e.dir) {
            const auto rel = relative(e.path, *root);
            const auto depth = static_cast<int>(std::ranges::count(rel, '/')) + 1;
            folders_[e.path].known = depth < max_depth; // one at max_depth is listed, not its contents
        }
    }
}

Item Index::item(const file_list::Entry& e) const {
    Item it{remote_basename(e.path), e.path, e.dir, e.size, std::nullopt, e.modified};
    if (e.dir) {
        it.size.reset();
        if (const auto f = folders_.find(e.path); f != folders_.end() && f->second.known) {
            it.items = f->second.children.size();
        }
    }
    return it;
}

Item Index::root_item(const std::string& root) const {
    Item it{root.empty() ? "~" : root, root, true, std::nullopt, std::nullopt};
    if (const auto f = folders_.find(root); f != folders_.end()) it.items = f->second.children.size();
    return it;
}

std::optional<std::vector<Item>> Index::folder(std::string_view path) const {
    const auto f = folders_.find(path);
    if (f == folders_.end() || !f->second.known) return std::nullopt;
    std::vector<Item> items;
    items.reserve(f->second.children.size());
    for (const auto c : f->second.children) items.push_back(item(list_->entries[c]));
    sort_items(items);
    return items;
}

std::optional<Item> Index::entry(std::string_view path) const {
    const auto f = folders_.find(parent_of(path));
    if (f == folders_.end() || !f->second.known) return std::nullopt;
    for (const auto c : f->second.children) {
        if (list_->entries[c].path == path) return item(list_->entries[c]);
    }
    return std::nullopt;
}

Index::Below Index::below(std::string_view path) const {
    Below out;
    const auto f = folders_.find(path);
    if (f == folders_.end() || !f->second.known) return out;
    out.complete = true;
    std::vector<const Folder*> todo{&f->second};
    while (!todo.empty()) {
        const Folder* at = todo.back();
        todo.pop_back();
        for (const auto c : at->children) {
            const auto& e = list_->entries[c];
            if (!e.dir) {
                out.files.push_back(&e);
                if (!e.size) out.complete = false;
                continue;
            }
            const auto sub = folders_.find(e.path);
            if (sub == folders_.end() || !sub->second.known) out.complete = false; // deeper than the list
            else todo.push_back(&sub->second);
        }
    }
    return out;
}

std::vector<Item> Index::top() const {
    if (roots_.size() == 1) return folder(roots_.front()).value_or(std::vector<Item>{});
    std::vector<Item> items;
    for (const auto& r : roots_) items.push_back(root_item(r));
    return items;
}

std::vector<Crumb> crumbs(const std::vector<std::string>& roots, std::string_view path) {
    std::vector<Crumb> out;
    const std::vector<std::string> all = roots.empty() ? std::vector<std::string>{""} : roots;
    const auto root = root_of(all, path);
    const std::string base = root ? *root : (path.starts_with('/') ? "/" : "");
    if (!root || all.size() > 1) out.push_back(Crumb{base.empty() ? "~" : base, base});
    std::string at = base;
    std::string_view rel = relative(path, base);
    while (!rel.empty()) {
        const auto slash = rel.find('/');
        const auto name = rel.substr(0, slash);
        at = join_remote(at, name);
        if (!name.empty()) out.push_back(Crumb{std::string(name), at});
        rel.remove_prefix(slash == std::string_view::npos ? rel.size() : slash + 1);
    }
    return out;
}

namespace {

ListError list_error(ListError::Kind kind, std::string message) { return ListError{.kind = kind, .message = std::move(message)}; }

ListError::Kind kind_of(const sftp::Status& status) {
    switch (status.code) {
    case sftp::Code::no_such_file: return ListError::Kind::not_found;
    case sftp::Code::permission_denied: return ListError::Kind::denied;
    default: return ListError::Kind::other;
    }
}

// READDIRs go out in rounds as the walk sends them (ssh_walk.cpp): two, then twice as many while
// batches keep coming, up to this many.
constexpr int max_readdirs = 8;

// One folder on one connection: OPENDIR, READDIRs ahead, a STAT for each link, CLOSE; every reply
// is in when it returns, so the connection can be used again. `stop` is looked at between replies.
std::expected<std::vector<Item>, ListError> read_folder(sftp::Client& c, std::string_view path, bool skip_hidden,
                                                        const std::stop_token& stop) {
    const std::string raw = rclone_enc::raw_path(path.empty() ? std::string_view(".") : path);
    std::optional<std::expected<std::string, sftp::Status>> opened;
    c.opendir(raw, [&](std::expected<std::string, sftp::Status> handle) { opened = std::move(handle); });
    while (!opened) c.pump();
    if (!*opened) {
        // OpenSSH answers OPENDIR on a file with "no such file" (ENOTDIR): one STAT tells a file
        // (a link to one too) from nothing there.
        const sftp::Status why = opened->error();
        if (auto attrs = sftp::stat(c, raw); attrs && !attrs->is_dir()) {
            return std::unexpected(list_error(ListError::Kind::file, std::format("{} is a file, not a folder", path)));
        }
        return std::unexpected(list_error(kind_of(why), sftp::describe(why)));
    }
    const std::string handle = std::move(**opened);

    std::vector<Item> items;
    std::optional<ListError> failed;
    std::size_t names = 0;
    std::uint64_t bytes = 0; // of the names and paths kept: a name can be as long as a packet (review G168)
    bool listed = false; // the end came, or a failure: no more READDIRs, then CLOSE
    int readdirs = 0;    // in flight
    int round = 2;
    // An entry as an item: what a link leads to decides (a broken one is nothing); sockets,
    // devices and pipes hold nothing to download.
    auto add = [&items](Item it, const sftp::Attrs& a) {
        if (!a.is_dir() && !a.is_file()) return;
        it.dir = a.is_dir();
        if (!it.dir) it.size = a.size;
        if (a.mtime) it.modified = *a.mtime;
        items.push_back(std::move(it));
    };
    std::function<void()> read_more = [&] {
        for (int i = 0; i < round; ++i) {
            ++readdirs;
            c.readdir(handle, [&](std::expected<std::vector<sftp::Entry>, sftp::Status> batch) {
                --readdirs;
                if (batch) names += batch->size();
                if (!batch) {
                    // The end, or a failure before it; after the end, the READDIRs sent ahead
                    // answer with it again.
                    if (!listed && batch.error().code != sftp::Code::eof) {
                        failed = list_error(kind_of(batch.error()), sftp::describe(batch.error()));
                    }
                    listed = true;
                } else if (names > sftp::max_listing) {
                    failed = list_error(ListError::Kind::other,
                                        std::format("the folder holds more than {} entries", sftp::max_listing));
                    listed = true;
                } else if (batch->empty()) {
                    // No names but no end either: taken as the end, as OpenSSH's sftp does, so a
                    // server that never says EOF can't keep it asking.
                    listed = true;
                } else if (!listed) {
                    for (auto& e : *batch) {
                        // ".", ".." and names with nothing to save them under are never items (G15).
                        if (!has_name(e.name) || (skip_hidden && e.name.starts_with('.'))) continue;
                        std::string name = rclone_enc::from_raw_name(e.name);
                        Item it{.name = name, .path = join_remote(path, name), .dir = false, .size = {}, .items = {}};
                        bytes += it.name.size() + it.path.size();
                        if (bytes > sftp::max_listing_bytes) {
                            failed = list_error(ListError::Kind::other,
                                                std::format("listing too large: more than {} of names",
                                                            util::format_size(sftp::max_listing_bytes)));
                            listed = true;
                            break;
                        }
                        if (!e.attrs.is_link()) {
                            add(std::move(it), e.attrs);
                            continue;
                        }
                        c.stat(raw == "." ? e.name : raw + (raw.ends_with('/') ? "" : "/") + e.name,
                               [&add, it = std::move(it)](std::expected<sftp::Attrs, sftp::Status> target) mutable {
                                   if (target) add(std::move(it), *target);
                               });
                    }
                }
                if (!listed && readdirs == 0) read_more();
                else if (listed && readdirs == 0) c.close(handle); // its answer isn't waited for
            });
        }
        round = std::min(round * 2, max_readdirs);
    };
    read_more();
    while (c.pending() > 0) {
        if (stop.stop_requested()) return std::unexpected(list_error(ListError::Kind::other, "stopped"));
        c.pump();
    }
    if (failed) return std::unexpected(std::move(*failed));
    sort_items(items);
    return items;
}

} // namespace

std::expected<std::vector<Item>, ListError> list_live(const engine::Context& ctx, std::string_view path,
                                                     const proc::RunOptions& run, const engine::CommandHook& on_command,
                                                     const engine::GrabSide& grab) {
    const std::stop_token& stop = run.stop;
    auto pool = engine::grab_pool(ctx, grab);
    if (!pool) return std::unexpected(list_error(ListError::Kind::other, pool.error()));
    if (on_command) on_command({"sftp", "readdir", path.empty() ? std::string(".") : std::string(path)});
    // A connection that sat idle may have been closed by the server meanwhile: reading a folder is
    // safe to repeat, so a broken reused one is dropped and the next one tried.
    for (;;) {
        std::optional<ssh::Pool::Lease> lease;
        try {
            lease = (*pool)->acquire(stop, ssh::interactive_retry);
        } catch (const ssh::Error& e) {
            return std::unexpected(list_error(ListError::Kind::other, e.what()));
        }
        if (!lease) return std::unexpected(list_error(ListError::Kind::other, "stopped"));
        try {
            return read_folder((*lease)->sftp(), path, ctx.settings.skip_hidden, stop);
        } catch (const sftp::Broken& e) {
            lease->broken();
            if (!lease->reused()) return std::unexpected(list_error(ListError::Kind::other, e.what()));
        }
    }
}

} // namespace grab::browse
