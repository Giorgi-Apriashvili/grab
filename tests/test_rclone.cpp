#include "rclone.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace grab;

TEST_CASE("remote_basename ignores trailing slashes") {
    CHECK(remote_basename("/home/alice/releases/movie.mkv") == "movie.mkv");
    CHECK(remote_basename("/home/alice/releases/") == "releases");
    CHECK(remote_basename("plain") == "plain");
}

TEST_CASE("remote_spec joins remote and absolute path") {
    CHECK(remote_spec("hetzner", "/home/alice/releases") == "hetzner:/home/alice/releases");
}

TEST_CASE("folder mode builds rclone copy into DEST with common, mode and extra flags in order") {
    const std::vector<std::string> common{"-P", "--sftp-chunk-size", "255Ki"};
    const std::vector<std::string> folder{"--transfers", "4"};
    const std::vector<std::string> extra{"--bwlimit", "10M"};

    RcloneRequest rq;
    rq.mode = Mode::folder;
    rq.rclone_exe = "rclone";
    rq.rclone_remote = "hetzner";
    rq.remote_path = "/home/alice/releases";
    rq.dest_dir = util::path_from_utf8("E:\\Backup\\releases");
    rq.common_flags = common;
    rq.mode_flags = folder;
    rq.extra = extra;

    const auto argv = build_rclone_argv(rq);
    CHECK(argv == std::vector<std::string>{"rclone", "copy", "hetzner:/home/alice/releases",
                                           util::path_to_utf8(rq.dest_dir), "-P",
                                           "--sftp-chunk-size", "255Ki", "--transfers", "4",
                                           "--bwlimit", "10M"});
}

TEST_CASE("file mode builds rclone copyto DEST\\name and passes --config when set") {
    const std::vector<std::string> common{"-P"};
    const std::vector<std::string> file{"--multi-thread-streams", "8"};

    RcloneRequest rq;
    rq.mode = Mode::file;
    rq.rclone_exe = "C:\\Tools\\rclone.exe";
    rq.rclone_config = util::path_from_utf8("C:\\cfg\\rclone.conf");
    rq.rclone_remote = "hetzner";
    rq.remote_path = "/home/alice/releases/Some.Movie.2013/Some.Movie.2013.mkv";
    rq.dest_dir = util::path_from_utf8("E:\\Backup");
    rq.common_flags = common;
    rq.mode_flags = file;

    const auto argv = build_rclone_argv(rq);
    const auto expected_dest = util::path_to_utf8(rq.dest_dir / "Some.Movie.2013.mkv");
    CHECK(argv == std::vector<std::string>{
                      "C:\\Tools\\rclone.exe", "copyto",
                      "hetzner:/home/alice/releases/Some.Movie.2013/Some.Movie.2013.mkv",
                      expected_dest, "--config", util::path_to_utf8(*rq.rclone_config), "-P",
                      "--multi-thread-streams", "8"});
}

TEST_CASE("empty flag lists produce a bare command") {
    RcloneRequest rq;
    rq.mode = Mode::folder;
    rq.rclone_exe = "rclone";
    rq.rclone_remote = "r";
    rq.remote_path = "/p";
    rq.dest_dir = util::path_from_utf8("D:\\out");
    const auto argv = build_rclone_argv(rq);
    CHECK(argv == std::vector<std::string>{"rclone", "copy", "r:/p", util::path_to_utf8(rq.dest_dir)});
}
