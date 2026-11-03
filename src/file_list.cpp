#include "file_list.hpp"

#include "cli.hpp"
#include "config.hpp"
#include "match.hpp"
#include "remote.hpp"
#include "ssh_find.hpp"
#include "ssh_pool.hpp"
#include "ssh_walk.hpp"
#include "util.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <optional>
#include <stop_token>
#include <string>
#include <system_error>
#include <utility>

namespace grab::file_list {

namespace fs = std::filesystem;

std::string fingerprint(const engine::Context& ctx) {
    // The find setting, not the check's answer, so no check is needed to compare; the text as 0.20
    // wrote it for its own client, so those lists stay valid. Lists 0.20's rclone fallback made
    // ("ssh", "rclone") differ (links, names in grab's form) and are fetched again.
    const std::string_view method = ctx.grab_method == GrabMethod::find   ? "grab+find"
                                    : ctx.grab_method == GrabMethod::walk ? "grab+walk"
                                                                           : "grab+probe";
    return std::format("1|{}|{}|{}@{}:{}|depth={}|hidden={}|roots={}", ctx.settings.rclone_remote, method,
                       ctx.remote.user, ctx.remote.host, ctx.remote.port, ctx.settings.max_depth,
                       ctx.settings.skip_hidden ? "skip" : "keep", util::join(engine::search_roots(ctx), "\x1f"));
}

bool fresh(std::int64_t fetched, std::int64_t now) {
    const std::int64_t max_age = std::chrono::duration_cast<std::chrono::seconds>(fresh_for).count();
    return fetched > 0 && now >= fetched && now - fetched < max_age;
}

bool fresh(const List& list, std::int64_t now) { return list.dated && fresh(list.fetched, now); }

bool can_answer(const engine::Context& ctx, const engine::SearchRequest& req) {
    if (is_path_target(req.target)) return false;
    if (ctx.settings.skip_hidden && wants_hidden(make_query(req.target, req.exact))) return false;
    return !req.depth || *req.depth == ctx.settings.max_depth;
}

engine::SearchResult search(const engine::Context& ctx, const List& list, const engine::SearchRequest& req) {
    const Query query = make_query(req.target, req.exact);
    const bool folders = req.mode == Mode::folder;
    std::vector<RemoteEntry> found;
    for (const auto& e : list.entries) {
        if (e.dir != folders || !matches(query, e.path)) continue;
        found.push_back(RemoteEntry{e.path, folders ? std::nullopt : e.size, e.modified});
    }
    auto result = engine::make_result(ctx, req, std::move(found));
    result.notes = list.notes;
    return result;
}

void rank_across(std::vector<ServerHit>& hits, const engine::SearchRequest& req) {
    const Query query = make_query(req.target, req.exact);
    struct Keyed {
        int tier;
        std::ptrdiff_t depth;
        std::string folded;
        ServerHit h;
    };
    std::vector<Keyed> keyed;
    keyed.reserve(hits.size());
    for (auto& h : hits) {
        keyed.push_back(Keyed{match_tier(query, h.hit.path), std::ranges::count(h.hit.path, '/'),
                              util::to_lower(h.hit.path), std::move(h)});
    }
    std::ranges::stable_sort(keyed, [](const Keyed& a, const Keyed& b) {
        if (a.tier != b.tier) return a.tier < b.tier;
        if (a.depth != b.depth) return a.depth < b.depth;
        if (a.folded != b.folded) return a.folded < b.folded;
        if (a.h.hit.path != b.h.hit.path) return a.h.hit.path < b.h.hit.path;
        return a.h.server < b.h.server;
    });
    for (std::size_t i = 0; i < keyed.size(); ++i) hits[i] = std::move(keyed[i].h);
}

namespace {

std::optional<std::uint64_t> number(std::string_view digits) {
    std::uint64_t n = 0;
    const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), n);
    if (ec != std::errc{} || ptr != digits.data() + digits.size() || digits.empty()) return std::nullopt;
    return n;
}

// Calls `each` for every piece of `text` between `sep`s (the last one may lack its separator).
template <class Each>
void split_records(std::string_view text, char sep, Each&& each) {
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find(sep, start);
        if (end == std::string_view::npos) end = text.size();
        each(text.substr(start, end - start));
        start = end + 1;
    }
}

// Takes the field before the next tab off `rec`; nullopt when there is no tab.
std::optional<std::string_view> take_field(std::string_view& rec) {
    const auto tab = rec.find('\t');
    if (tab == std::string_view::npos) return std::nullopt;
    const auto field = rec.substr(0, tab);
    rec.remove_prefix(tab + 1);
    return field;
}

// Version 1 had no modification times; grab still reads it.
constexpr std::string_view header = "grab-file-list 2";
constexpr std::string_view header_v1 = "grab-file-list 1";

} // namespace

std::optional<Entry> parse_find_record(std::string_view rec) {
    Entry e;
    if (rec.starts_with("d\t")) {
        e.dir = true;
        rec.remove_prefix(2);
    } else if (rec.starts_with("f\t")) {
        rec.remove_prefix(2);
        const auto size = take_field(rec);
        if (!size) return std::nullopt;
        e.size = number(*size);
    } else {
        return std::nullopt;
    }
    const auto time = take_field(rec);
    if (!time) return std::nullopt;
    e.modified = util::known_time(util::parse_unix_time(*time));
    if (rec.starts_with("./")) rec.remove_prefix(2);
    if (!has_name(rec)) return std::nullopt; // "/", ".": nothing to save it under (review G15)
    e.path = std::string(rec);
    return e;
}

namespace {

// The list through grab's own SSH client: one find of both kinds, or one walk of every root.
std::expected<void, std::string> fetch_grab(const engine::Context& ctx, List& list, const std::vector<std::string>& roots,
                                            const std::stop_token& stop, const engine::CommandHook& on_command,
                                            const engine::GrabSide& grab) {
    auto pool = engine::grab_pool(ctx, grab);
    if (!pool) return util::fail(pool.error());
    auto way = engine::grab_lookup(ctx, **pool, grab, stop);
    if (!way) return util::fail(way.error());
    if (way->find) {
        const ssh::FindSpec spec{.roots = roots,
                                 .max_depth = ctx.settings.max_depth,
                                 .skip_hidden = ctx.settings.skip_hidden,
                                 .query = std::nullopt,
                                 .folders = true,
                                 .files = true};
        ssh::FindOptions options;
        options.stop = stop;
        options.progress = grab.progress;
        options.retry_refused = ssh::interactive_retry;
        auto found = engine::grab_find(ctx, **pool, grab, *way, spec, options, on_command);
        if (!found) return util::fail(found.error());
        if (*found) {
            list.entries = std::move((*found)->entries);
            list.notes = std::move((*found)->notes);
            return {};
        }
        // Asked again, the check says walk.
    }
    const int seat = grab.walk_seat ? grab.walk_seat() : 4;
    if (stop.stop_requested()) return util::fail("search cancelled");
    ssh::WalkOptions options{.connections = std::max(1, seat),
                             .folders = true,
                             .max_depth = ctx.settings.max_depth,
                             .skip_hidden = ctx.settings.skip_hidden};
    options.progress = grab.progress;
    options.retry_refused = ssh::interactive_retry;
    if (on_command) {
        std::vector<std::string> argv{"sftp", "walk"};
        for (const auto& r : roots) argv.push_back(r.empty() ? std::string(".") : r);
        argv.insert(argv.end(), {"--max-depth", std::to_string(ctx.settings.max_depth)});
        on_command(argv);
    }
    auto walked = ssh::walk_roots(**pool, roots, options, stop);
    if (stop.stop_requested()) return util::fail("search cancelled");
    if (!walked) return util::fail(walked.error());
    list.entries.reserve(walked->entries.size());
    for (auto& e : walked->entries) {
        list.entries.push_back(Entry{.path = std::move(e.path),
                                     .dir = e.dir,
                                     .size = e.dir ? std::nullopt : std::optional(e.size),
                                     .modified = e.modified});
    }
    list.notes = std::move(walked->unreadable);
    return {};
}

} // namespace

std::expected<List, std::string> fetch(const engine::Context& ctx, std::int64_t now, const proc::RunOptions& run,
                                       const engine::CommandHook& on_command, const engine::GrabSide& grab) {
    List list;
    list.fetched = now;
    list.fingerprint = fingerprint(ctx);
    if (auto got = fetch_grab(ctx, list, engine::search_roots(ctx), run.stop, on_command, grab); !got) {
        return util::fail(got.error());
    }
    return list;
}

fs::path default_dir() {
    return util::local_data_home() / "grab" / "lists";
}

fs::path dir_for(const fs::path& grab_conf) {
    const auto own = config_folder_name(grab_conf);
    return own ? default_dir() / util::path_from_utf8(*own) : default_dir();
}

fs::path path_for(const fs::path& dir, std::string_view server) {
    std::string name;
    bool changed = server.empty() || server.starts_with('.');
    for (const char c : server) {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                          c == '-' || c == '_';
        name += safe ? c : '_';
        changed = changed || !safe;
    }
    if (changed) {
        std::uint32_t h = 2166136261U; // FNV-1a
        for (const char c : server) h = (h ^ static_cast<unsigned char>(c)) * 16777619U;
        name += std::format("-{:08x}", h);
    }
    return dir / util::path_from_utf8(name + ".list");
}

std::string to_text(const List& list) {
    std::string out = std::format("{}\nfetched\t{}\nfingerprint\t{}\n", header, list.fetched, util::escape_field(list.fingerprint));
    for (const auto& e : list.entries) {
        if (e.dir) {
            out += "d\t";
        } else {
            out += e.size ? std::format("f\t{}\t", *e.size) : std::string("f\t\t");
        }
        if (e.modified) out += std::to_string(*e.modified);
        out += '\t';
        out += util::escape_field(e.path);
        out += '\n';
    }
    return out;
}

std::optional<List> from_text(std::string_view text) {
    List list;
    bool ok = true;
    int line_no = 0;
    split_records(text, '\n', [&](std::string_view line) {
        if (!ok) return;
        if (line.ends_with('\r')) line.remove_suffix(1);
        const int n = line_no++;
        if (n == 0) {
            ok = line == header || line == header_v1;
            list.dated = line == header;
            return;
        }
        if (line.empty()) return;
        const auto tab = line.find('\t');
        if (tab == std::string_view::npos) {
            ok = false;
            return;
        }
        const auto kind = line.substr(0, tab);
        auto rest = line.substr(tab + 1);
        if (kind == "fetched") {
            const auto t = number(rest);
            ok = t.has_value();
            if (t) list.fetched = static_cast<std::int64_t>(*t);
        } else if (kind == "fingerprint") {
            auto fp = util::unescape_field(rest);
            ok = fp.has_value();
            if (fp) list.fingerprint = std::move(*fp);
        } else if (kind == "d" || kind == "f") {
            Entry e;
            e.dir = kind == "d";
            if (!e.dir) {
                const auto size = take_field(rest);
                if (!size) {
                    ok = false;
                    return;
                }
                e.size = number(*size);
            }
            if (list.dated) {
                const auto time = take_field(rest);
                if (!time) {
                    ok = false;
                    return;
                }
                e.modified = util::known_time(util::parse_unix_time(*time));
            }
            auto path = util::unescape_field(rest);
            if (!path || path->empty()) {
                ok = false;
                return;
            }
            e.path = std::move(*path);
            list.entries.push_back(std::move(e));
        }
        // Unknown kinds are skipped, so later versions may add fields.
    });
    if (!ok || line_no == 0) return std::nullopt;
    return list;
}

namespace {
// A saved list is this header followed by its text, encrypted for the current Windows user
// (util::protect_for_user): the file names it holds are unreadable to other accounts and to
// anyone with only a copy of the file.
constexpr std::string_view sealed_header = "grab-file-list-sealed 1\n";
constexpr std::string_view purpose = "grab file list";
} // namespace

std::optional<List> load(const fs::path& file) {
    auto data = util::read_file(file);
    if (!data || !data->starts_with(sealed_header)) return std::nullopt; // fetched anew
    auto text = util::unprotect_for_user(std::string_view(*data).substr(sealed_header.size()), purpose);
    if (!text) return std::nullopt; // another user's, or damaged
    return from_text(*text);
}

bool save(const fs::path& file, const List& list) {
    const auto sealed = util::protect_for_user(to_text(list), purpose);
    return sealed && util::write_file_atomic(file, std::string(sealed_header) + *sealed).has_value();
}

} // namespace grab::file_list
