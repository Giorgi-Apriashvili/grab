#include "engine.hpp"

#include "json.hpp"
#include "listing.hpp"
#include "match.hpp"
#include "rclone.hpp"
#include "remote.hpp"
#include "util.hpp"

#include <format>
#include <unordered_map>
#include <utility>

namespace grab::engine {

std::expected<Context, std::string> load_context(const std::filesystem::path& config_path,
                                                 const std::optional<std::string>& remote_name) {
    auto cfg = load_grab_config(config_path);
    if (!cfg) return util::fail(cfg.error());
    auto selected = cfg->select(remote_name);
    if (!selected) return util::fail(selected.error());
    RemoteSettings settings = **selected;

    const auto rclone_conf = cfg->rclone_config.value_or(default_rclone_config_path());
    auto remote = load_rclone_remote(rclone_conf, settings.rclone_remote);
    if (!remote) return util::fail(remote.error());

    const FindMethod method = resolve_find_method(settings, *remote);
    return Context{std::move(*cfg), std::move(settings), std::move(*remote), method};
}

namespace {

// The last non-empty line of a child's stderr, without a trailing period (the caller's
// message continues after it).
std::string last_line(std::string_view text) {
    text = util::trim(text);
    const auto nl = text.rfind('\n');
    auto line = util::trim(nl == std::string_view::npos ? text : text.substr(nl + 1));
    while (line.ends_with('.')) line.remove_suffix(1);
    return std::string(line);
}

std::expected<std::vector<RemoteEntry>, std::string>
lookup_via_ssh(const Context& ctx, const SearchRequest& req, const std::vector<std::string>& roots,
               int max_depth, const proc::RunOptions& run, const CommandHook& on_command) {
    FindRequest find;
    find.mode = req.mode;
    find.target = req.target;
    find.exact = req.exact;
    find.roots = roots;
    find.max_depth = max_depth;
    find.skip_hidden = ctx.settings.skip_hidden;

    std::vector<std::string> ssh_options = ctx.settings.ssh_options;
    if (req.batch_ssh) ssh_options.insert(ssh_options.end(), {"-o", "BatchMode=yes"});
    const auto argv =
        build_ssh_argv(ctx.config.ssh, ctx.remote, ssh_options, build_find_command(find));
    if (on_command) on_command(argv);

    auto res = proc::run_capture(argv, run);
    if (!res) return util::fail(res.error());
    if (res->exit_code == proc::exit_stopped && run.stop.stop_requested()) {
        return util::fail("search cancelled");
    }
    if (res->exit_code == 255) {
        const std::string detail = run.detached ? last_line(res->err) : std::string{};
        std::string msg = std::format("ssh to {}@{}:{} failed (exit 255)", ctx.remote.user,
                                      ctx.remote.host, ctx.remote.port);
        if (!detail.empty()) msg += ": " + detail;
        else if (!run.detached) msg += "; see the message above";
        if (req.batch_ssh) {
            msg += ". grab cannot answer a passphrase prompt here; load the key into ssh-agent "
                   "(ssh-add <key file>)";
        }
        msg += std::format(". If this host has no shell (e.g. a storage box), set `find = rclone` "
                           "in [{}]",
                           ctx.settings.name);
        return util::fail(msg);
    }
    return parse_find_output(res->out, req.mode == Mode::file);
}

std::expected<std::vector<RemoteEntry>, std::string>
lookup_via_rclone(const Context& ctx, const SearchRequest& req, const std::vector<std::string>& roots,
                  int max_depth, const proc::RunOptions& run, const CommandHook& on_command) {
    // A path target is checked with a single listing of its parent, whatever the roots.
    const std::vector<std::string> bases =
        is_path_target(req.target) ? std::vector<std::string>{""} : roots;

    std::vector<RemoteEntry> found;
    for (const auto& root : bases) {
        ListRequest list;
        list.mode = req.mode;
        list.target = req.target;
        list.exact = req.exact;
        list.root = root;
        list.max_depth = max_depth;
        list.skip_hidden = ctx.settings.skip_hidden;
        list.rclone_exe = ctx.config.rclone;
        list.rclone_config = ctx.config.rclone_config;
        list.rclone_remote = ctx.settings.rclone_remote;

        const auto argv = build_lsf_argv(list);
        if (on_command) on_command(argv);

        auto res = proc::run_capture(argv, run);
        if (!res) return util::fail(res.error());
        if (res->exit_code == proc::exit_stopped && run.stop.stop_requested()) {
            return util::fail("search cancelled");
        }
        if (res->exit_code != 0) {
            const std::string detail = run.detached ? last_line(res->err) : std::string{};
            return util::failf("rclone lsf of '{}' failed (exit {}){}", argv[2], res->exit_code,
                               detail.empty() ? (run.detached ? "" : "; see the message above")
                                              : ": " + detail);
        }
        auto entries = parse_lsf_output(list, res->out);
        found.insert(found.end(), std::make_move_iterator(entries.begin()),
                     std::make_move_iterator(entries.end()));
    }
    return found;
}

} // namespace

std::expected<SearchResult, std::string> search(const Context& ctx, const SearchRequest& req,
                                                const proc::RunOptions& run,
                                                const CommandHook& on_command) {
    SearchResult result;
    result.max_depth = req.depth.value_or(ctx.settings.max_depth);
    result.roots = ctx.settings.search_roots;
    if (result.roots.empty()) result.roots.emplace_back(); // the login home

    auto entries = ctx.method == FindMethod::ssh
                       ? lookup_via_ssh(ctx, req, result.roots, result.max_depth, run, on_command)
                       : lookup_via_rclone(ctx, req, result.roots, result.max_depth, run, on_command);
    if (!entries) return util::fail(entries.error());

    // Rank by path, then reattach sizes.
    std::unordered_map<std::string, std::optional<std::uint64_t>> sizes;
    std::vector<std::string> paths;
    paths.reserve(entries->size());
    for (auto& e : *entries) {
        if (sizes.emplace(e.path, e.size).second) paths.push_back(std::move(e.path));
    }
    rank_matches(paths, make_query(req.target, req.exact));
    result.hits.reserve(paths.size());
    for (auto& p : paths) {
        const auto size = sizes[p];
        std::string name = remote_basename(p);
        result.hits.push_back(Hit{std::move(p), std::move(name), size});
    }
    return result;
}

std::vector<std::string> download_argv(const Context& ctx, Mode mode, const std::string& remote_path,
                                       const std::filesystem::path& dest_dir,
                                       std::span<const std::string> extra) {
    RcloneRequest rq;
    rq.mode = mode;
    rq.rclone_exe = ctx.config.rclone;
    rq.rclone_config = ctx.config.rclone_config;
    rq.rclone_remote = ctx.settings.rclone_remote;
    rq.remote_path = remote_path;
    rq.dest_dir = dest_dir;
    rq.common_flags = ctx.settings.common_flags;
    rq.mode_flags = mode == Mode::folder ? ctx.settings.folder_flags : ctx.settings.file_flags;
    rq.extra = extra;
    return build_rclone_argv(rq);
}

std::optional<Progress> parse_stats_line(std::string_view line) {
    if (line.find("\"stats\"") == std::string_view::npos) return std::nullopt; // cheap pre-check
    auto doc = json::parse(line);
    if (!doc) return std::nullopt;
    const json::Value* stats = doc->find("stats");
    if (stats == nullptr || stats->kind != json::Value::Kind::object) return std::nullopt;

    auto number = [](const json::Value* v) -> std::optional<double> {
        if (v == nullptr || v->kind != json::Value::Kind::number) return std::nullopt;
        return v->number;
    };
    auto to_bytes = [](double d) { return d > 0 ? static_cast<std::uint64_t>(d) : std::uint64_t{0}; };

    Progress p;
    p.bytes = to_bytes(number(stats->find("bytes")).value_or(0));
    p.total = to_bytes(number(stats->find("totalBytes")).value_or(0));
    p.speed = number(stats->find("speed")).value_or(0);
    p.eta = number(stats->find("eta"));

    // Early in a transfer the overall speed is still 0 while each running transfer already
    // reports its own; the ETA can be null. Fill both in from what is known.
    double transferring_speed = 0;
    if (const auto* list = stats->find("transferring");
        list != nullptr && list->kind == json::Value::Kind::array) {
        for (const auto& t : list->items) {
            transferring_speed += number(t.find("speed")).value_or(0);
            if (p.current.empty()) {
                if (auto name = t.string_of("name")) p.current = *name;
            }
        }
    }
    if (p.speed <= 0) p.speed = transferring_speed;
    if (!p.eta && p.speed > 0 && p.total > p.bytes) {
        p.eta = static_cast<double>(p.total - p.bytes) / p.speed;
    }
    return p;
}

std::optional<std::string> parse_error_line(std::string_view line) {
    if (line.find("\"level\"") == std::string_view::npos) return std::nullopt;
    auto doc = json::parse(line);
    if (!doc || doc->string_of("level") != "error") return std::nullopt;
    return doc->string_of("msg");
}

std::vector<std::string> without_console_progress(std::span<const std::string> flags) {
    std::vector<std::string> out;
    for (const auto& f : flags) {
        if (f == "-P" || f == "--progress" || f == "--stats-one-line" ||
            f == "--stats-one-line-date" || f == "-q" || f == "--quiet") {
            continue;
        }
        out.push_back(f);
    }
    return out;
}

std::expected<DownloadResult, std::string>
download(const Context& ctx, Mode mode, const std::string& remote_path,
         const std::filesystem::path& dest_dir, const std::function<void(const Progress&)>& on_progress,
         const proc::RunOptions& run, std::span<const std::string> extra,
         const CommandHook& on_command) {
    const auto common = without_console_progress(ctx.settings.common_flags);
    const auto mode_flags = without_console_progress(
        mode == Mode::folder ? ctx.settings.folder_flags : ctx.settings.file_flags);
    std::vector<std::string> tail(extra.begin(), extra.end());
    // Appended last so they win over anything configured.
    tail.insert(tail.end(), {"--use-json-log", "--stats", "500ms", "--stats-log-level", "NOTICE"});

    RcloneRequest rq;
    rq.mode = mode;
    rq.rclone_exe = ctx.config.rclone;
    rq.rclone_config = ctx.config.rclone_config;
    rq.rclone_remote = ctx.settings.rclone_remote;
    rq.remote_path = remote_path;
    rq.dest_dir = dest_dir;
    rq.common_flags = common;
    rq.mode_flags = mode_flags;
    rq.extra = tail;
    const auto argv = build_rclone_argv(rq);
    if (on_command) on_command(argv);

    DownloadResult result;
    auto code = proc::run_streaming(
        argv,
        [&](std::string_view line) {
            if (auto p = parse_stats_line(line)) {
                if (on_progress) on_progress(*p);
            } else if (auto msg = parse_error_line(line)) {
                result.last_error = std::move(*msg);
            }
        },
        run);
    if (!code) return util::fail(code.error());
    result.exit_code = *code;
    if (result.exit_code == proc::exit_stopped) {
        remove_partials(dest_dir / util::path_from_utf8(remote_basename(remote_path)), mode);
    }
    return result;
}

std::size_t remove_partials(const std::filesystem::path& local_target, Mode mode) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::size_t removed = 0;
    auto is_partial_of = [](const fs::path& p, const std::wstring& stem) {
        const std::wstring name = p.filename().wstring();
        return name.size() > stem.size() + 1 && name.starts_with(stem + L".") &&
               name.ends_with(L".partial");
    };

    if (mode == Mode::file) {
        const std::wstring stem = local_target.filename().wstring();
        for (const auto& e : fs::directory_iterator(local_target.parent_path(), ec)) {
            if (e.is_regular_file(ec) && is_partial_of(e.path(), stem) && fs::remove(e.path(), ec)) {
                ++removed;
            }
        }
        return removed;
    }
    // Folder: rclone writes each file as "<file>.<random>.partial" next to its final name.
    for (auto it = fs::recursive_directory_iterator(local_target, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == L".partial" &&
            fs::remove(it->path(), ec)) {
            ++removed;
        }
    }
    return removed;
}

} // namespace grab::engine
