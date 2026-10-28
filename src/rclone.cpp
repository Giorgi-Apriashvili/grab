#include "rclone.hpp"

#include "local_name.hpp"
#include "rclone_encoding.hpp"
#include "util.hpp"

#include <utility>

namespace grab {

std::string remote_basename(std::string_view remote_path) {
    while (remote_path.size() > 1 && remote_path.back() == '/') remote_path.remove_suffix(1);
    const auto slash = remote_path.rfind('/');
    if (slash == std::string_view::npos) return std::string(remote_path);
    return std::string(remote_path.substr(slash + 1));
}

bool has_name(std::string_view remote_path) {
    const auto name = remote_basename(remote_path);
    return !name.empty() && name != "/" && name != "." && name != "..";
}

bool name_not_utf8(std::string_view remote_path) {
    return !rclone_enc::valid_utf8(remote_path) || remote_path.find("\xEF\xBF\xBD") != std::string_view::npos;
}

std::string remote_spec(std::string_view rclone_remote, std::string_view remote_path) {
    std::string out(rclone_remote);
    out += ':';
    out += remote_path;
    return out;
}

std::filesystem::path local_target(const RcloneRequest& req) {
    // The server's name as a safe Windows one (local_name.hpp): it can't leave dest_dir.
    if (req.local_name) return req.dest_dir / req.local_name->filename();
    return req.dest_dir / local::relative(remote_basename(req.remote_path));
}

std::expected<std::vector<std::string>, std::string> build_rclone_argv(const RcloneRequest& req) {
    // rclone copy syncs the *contents* of the source directory into the destination, so the
    // folder is recreated by naming it explicitly; copyto writes the single file to that path.
    // rclone encodes every part of a local path again: grab's already safe name goes in the form
    // that comes back as itself (G80: "Mission： Impossible" became "Mission‛： Impossible").
    auto local = rclone_enc::local_path(local_target(req));
    if (!local) return util::fail(local.error());
    std::vector<std::string> argv{req.rclone_exe, req.mode == Mode::folder ? "copy" : "copyto",
                                  remote_spec(req.rclone_remote, req.remote_path), std::move(*local)};

    argv.insert(argv.end(), {"--config", std::string(config_in_memory)});
    argv.insert(argv.end(), req.common_flags.begin(), req.common_flags.end());
    argv.insert(argv.end(), req.mode_flags.begin(), req.mode_flags.end());
    argv.insert(argv.end(), req.extra.begin(), req.extra.end());
    return argv;
}

} // namespace grab
