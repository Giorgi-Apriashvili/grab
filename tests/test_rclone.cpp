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

TEST_CASE("folder mode recreates the folder inside DEST, flags in common, mode, extra order") {
    const std::vector<std::string> common{"-P", "--sftp-chunk-size", "255Ki"};
    const std::vector<std::string> folder{"--transfers", "4"};
    const std::vector<std::string> extra{"--bwlimit", "10M"};

    RcloneRequest rq;
    rq.mode = Mode::folder;
    rq.rclone_exe = "rclone";
    rq.rclone_remote = "hetzner";
    rq.remote_path = "/home/alice/releases";
    rq.dest_dir = util::path_from_utf8("E:\\Backup");
    rq.common_flags = common;
    rq.mode_flags = folder;
    rq.extra = extra;

    CHECK(local_target(rq) == rq.dest_dir / "releases");
    const auto argv = build_rclone_argv(rq).value();
    CHECK(argv == std::vector<std::string>{"rclone", "copy", "hetzner:/home/alice/releases",
                                           util::path_to_utf8(rq.dest_dir / "releases"), "-P",
                                           "--sftp-chunk-size", "255Ki", "--transfers", "4",
                                           "--bwlimit", "10M"});
}

TEST_CASE("folder names with spaces and punctuation survive as one path") {
    RcloneRequest rq;
    rq.mode = Mode::folder;
    rq.rclone_exe = "rclone";
    rq.rclone_remote = "hetzner";
    rq.remote_path = "/home/alice/courses/Course - Learn to Program with C++ by Jane Doe";
    rq.dest_dir = util::path_from_utf8("C:\\Users\\alice\\Desktop");

    const auto argv = build_rclone_argv(rq).value();
    REQUIRE(argv.size() == 4);
    CHECK(argv[3] == util::path_to_utf8(
                         rq.dest_dir / "Course - Learn to Program with C++ by Jane Doe"));
}

TEST_CASE("file mode builds rclone copyto DEST\\name and passes --config when set") {
    // Same DEST\<name> rule as folder mode, but with copyto so rclone treats it as a file path.
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

    const auto argv = build_rclone_argv(rq).value();
    const auto expected_dest = util::path_to_utf8(rq.dest_dir / "Some.Movie.2013.mkv");
    CHECK(argv == std::vector<std::string>{
                      "C:\\Tools\\rclone.exe", "copyto",
                      "hetzner:/home/alice/releases/Some.Movie.2013/Some.Movie.2013.mkv",
                      expected_dest, "--config", util::path_to_utf8(*rq.rclone_config), "-P",
                      "--multi-thread-streams", "8"});
}

#ifdef _WIN32
TEST_CASE("rclone is told the local side so that it lands in grab's folder (G80)") {
    RcloneRequest rq;
    rq.mode = Mode::folder;
    rq.rclone_exe = "rclone";
    rq.rclone_remote = "r";
    rq.remote_path = "/media/Mission: Impossible (1996)";
    // A Save-to folder grab named earlier from a server's "Shows: 2024".
    rq.dest_dir = util::path_from_utf8("D:\\Shows\uFF1A 2024");
    CHECK(local_target(rq) == util::path_from_utf8("D:\\Shows\uFF1A 2024\\Mission\uFF1A Impossible (1996)"));
    // rclone encodes ':' itself: given grab's '：' it would write into "Mission‛： Impossible".
    CHECK(build_rclone_argv(rq).value()[3] == "D:\\Shows: 2024\\Mission: Impossible (1996)");

    rq.mode = Mode::file;
    rq.remote_path = "/media/a.txt";
    rq.dest_dir = util::path_from_utf8("D:\\in");
    rq.local_name = util::path_from_utf8("odd\u201Bname (2).txt"); // a ‛ rclone can't be made to write
    const auto refused = build_rclone_argv(rq);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().find("odd\u201Bname (2).txt") != std::string::npos);
}
#endif

TEST_CASE("empty flag lists produce a bare command") {
    RcloneRequest rq;
    rq.mode = Mode::folder;
    rq.rclone_exe = "rclone";
    rq.rclone_remote = "r";
    rq.remote_path = "/p";
    rq.dest_dir = util::path_from_utf8("D:\\out");
    const auto argv = build_rclone_argv(rq).value();
    CHECK(argv == std::vector<std::string>{"rclone", "copy", "r:/p",
                                           util::path_to_utf8(rq.dest_dir / "p")});
}

TEST_CASE("a remote path with no name to save it under (G15)") {
    CHECK(has_name("/srv/x"));
    CHECK(has_name("x/"));
    CHECK(has_name("a"));
    CHECK_FALSE(has_name("/"));
    CHECK_FALSE(has_name("//"));
    CHECK_FALSE(has_name(""));
    CHECK_FALSE(has_name("."));
    CHECK_FALSE(has_name("/srv/.."));
}
