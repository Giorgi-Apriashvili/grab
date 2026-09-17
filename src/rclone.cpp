#include "rclone.hpp"

#include "util.hpp"

namespace grab {

std::string remote_basename(std::string_view remote_path) {
    while (remote_path.size() > 1 && remote_path.back() == '/') remote_path.remove_suffix(1);
    const auto slash = remote_path.rfind('/');
    if (slash == std::string_view::npos) return std::string(remote_path);
    return std::string(remote_path.substr(slash + 1));
}

std::string remote_spec(std::string_view rclone_remote, std::string_view remote_path) {
    std::string out(rclone_remote);
    out += ':';
    out += remote_path;
    return out;
}

std::filesystem::path local_target(const RcloneRequest& req) {
    return req.dest_dir / util::path_from_utf8(remote_basename(req.remote_path));
}

std::vector<std::string> build_rclone_argv(const RcloneRequest& req) {
    // rclone copy syncs the *contents* of the source directory into the destination, so the
    // folder is recreated by naming it explicitly; copyto writes the single file to that path.
    std::vector<std::string> argv{req.rclone_exe, req.mode == Mode::folder ? "copy" : "copyto",
                                  remote_spec(req.rclone_remote, req.remote_path),
                                  util::path_to_utf8(local_target(req))};

    if (req.rclone_config) {
        argv.push_back("--config");
        argv.push_back(util::path_to_utf8(*req.rclone_config));
    }
    argv.insert(argv.end(), req.common_flags.begin(), req.common_flags.end());
    argv.insert(argv.end(), req.mode_flags.begin(), req.mode_flags.end());
    argv.insert(argv.end(), req.extra.begin(), req.extra.end());
    return argv;
}

} // namespace grab
