#include "browse.hpp"

#include "json.hpp"
#include "listing.hpp"
#include "rclone.hpp"
#include "server_ops.hpp"
#include "util.hpp"

#include <algorithm>
#include <cctype>
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

bool maybe_file(const std::vector<Item>& items, std::string_view path) {
    return items.size() == 1 && !items.front().dir && items.front().name == remote_basename(path);
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

std::vector<std::string> listing_argv(const engine::Context& ctx, std::string_view path) {
    std::vector<std::string> argv{ctx.config.rclone, "lsjson", remote_spec(ctx.settings.rclone_remote, path),
                                  "--no-mimetype"};
    if (ctx.config.rclone_config) argv.insert(argv.end(), {"--config", util::path_to_utf8(*ctx.config.rclone_config)});
    const auto flags = engine::without_console_progress(ctx.settings.common_flags);
    argv.insert(argv.end(), flags.begin(), flags.end());
    return argv;
}

std::expected<std::vector<Item>, std::string> parse_lsjson(std::string_view json_text, std::string_view path,
                                                           bool skip_hidden) {
    auto doc = json::parse(json_text);
    if (!doc || doc->kind != json::Value::Kind::array) return util::fail("unexpected answer from rclone lsjson");
    std::vector<Item> items;
    items.reserve(doc->items.size());
    for (const auto& v : doc->items) {
        auto name = v.string_of("Name");
        if (!name || name->empty() || (skip_hidden && name->starts_with('.'))) continue;
        Item it;
        it.path = join_remote(path, *name);
        it.name = std::move(*name);
        const auto* is_dir = v.find("IsDir");
        it.dir = is_dir != nullptr && is_dir->kind == json::Value::Kind::boolean && is_dir->boolean;
        const auto* size = v.find("Size");
        if (!it.dir && size != nullptr && size->kind == json::Value::Kind::number && size->number >= 0) {
            it.size = static_cast<std::uint64_t>(size->number);
        }
        if (const auto time = v.string_of("ModTime")) it.modified = util::known_time(util::parse_rfc3339_seconds(*time));
        items.push_back(std::move(it));
    }
    sort_items(items);
    return items;
}

std::expected<std::vector<Item>, std::string> list_live(const engine::Context& ctx, std::string_view path,
                                                       const proc::RunOptions& run,
                                                       const engine::CommandHook& on_command) {
    const auto argv = listing_argv(ctx, path);
    if (on_command) on_command(argv);
    auto ran = proc::run_capture(argv, run);
    if (!ran) return util::fail(ran.error());
    if (run.stop.stop_requested()) return util::fail("stopped");
    if (ran->exit_code != 0) return util::fail(server_ops::clean_rclone_error(ran->err));
    return parse_lsjson(ran->out, path, ctx.settings.skip_hidden);
}

} // namespace grab::browse
