#include "servers.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace grab;
using servers::Auth;
using servers::NewServer;

TEST_CASE("server names") {
    CHECK_FALSE(servers::validate_name("hetzner").has_value());
    CHECK_FALSE(servers::validate_name("arch_box-2.eu").has_value());
    CHECK(servers::validate_name("").has_value());
    CHECK(servers::validate_name("grab").has_value());
    CHECK(servers::validate_name("GRAB").has_value());
    CHECK(servers::validate_name("my server").has_value());
    CHECK(servers::validate_name("a:b").has_value());
    CHECK(servers::validate_name("-x").has_value());
    CHECK(servers::validate_name(std::string(65, 'a')).has_value());
}

TEST_CASE("keyscan output and fingerprints") {
    const std::string scan = "# 203.0.113.10:2222 SSH-2.0-OpenSSH_9.6\n"
                             "[203.0.113.10]:2222 ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIB6Bsa4\r\n"
                             "\n"
                             "[203.0.113.10]:2222 ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABgQ\n";
    const auto lines = servers::parse_keyscan(scan);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "[203.0.113.10]:2222 ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIB6Bsa4");

    const std::string keygen = "256 SHA256:B6xZKyaND/JObPaHUZjijNrOUF32cgF/ECkNT8d4mQQ [203.0.113.10]:2222 (ED25519)\n"
                               "3072 SHA256:abc+def [203.0.113.10]:2222 (RSA)\n"
                               "garbage\n";
    CHECK(servers::parse_fingerprints(keygen) ==
          std::vector<servers::Fingerprint>{{"SHA256:B6xZKyaND/JObPaHUZjijNrOUF32cgF/ECkNT8d4mQQ", "ED25519"},
                                            {"SHA256:abc+def", "RSA"}});
    CHECK(servers::keyscan_argv("h", 23) ==
          std::vector<std::string>{"ssh-keyscan", "-T", "10", "-p", "23", "h"});
}

TEST_CASE("handshake fallback: records the key, offers no login, one key type at a time") {
    const auto argv = servers::handshake_argv("ssh", "box.example.com", 23, util::path_from_utf8("C:\\t\\k.txt"),
                                              "rsa-sha2-512");
    auto has = [&](std::string_view s) { return std::ranges::find(argv, s) != argv.end(); };
    CHECK(has("StrictHostKeyChecking=accept-new"));
    CHECK(has("PreferredAuthentications=none")); // nothing secret is ever sent
    CHECK(has("BatchMode=yes"));
    CHECK(has("UserKnownHostsFile=C:\\t\\k.txt"));
    CHECK(has("HostKeyAlgorithms=rsa-sha2-512"));
    CHECK(argv[argv.size() - 2] == "grab-hostkey@box.example.com");
    const auto any = servers::handshake_argv("ssh", "h", 22, util::path_from_utf8("C:\\t\\k.txt"));
    CHECK(std::ranges::none_of(any, [](const std::string& a) { return a.starts_with("HostKeyAlgorithms="); }));
    // ED25519, ECDSA and RSA are all covered: rclone may use any of them.
    const auto& algs = servers::handshake_key_algorithms();
    CHECK(std::ranges::find(algs, "ssh-ed25519") != algs.end());
    CHECK(std::ranges::find(algs, "ecdsa-sha2-nistp521") != algs.end());
    CHECK(std::ranges::find(algs, "rsa-sha2-512") != algs.end());
}

TEST_CASE("merge_known_hosts appends only new lines") {
    const std::vector<std::string> keys{"[h]:23 ssh-ed25519 AAAA", "[h]:23 ssh-rsa BBBB"};
    const auto first = servers::merge_known_hosts("", keys);
    CHECK(first == "[h]:23 ssh-ed25519 AAAA\n[h]:23 ssh-rsa BBBB\n");
    CHECK(servers::merge_known_hosts(first, keys) == first);
    CHECK(servers::merge_known_hosts("other key", {"[h]:23 ssh-ed25519 AAAA"}) ==
          "other key\n[h]:23 ssh-ed25519 AAAA\n");
}

TEST_CASE("rclone config create argv per auth type; no plaintext secret, pre-obscured only") {
    NewServer s;
    s.name = "box";
    s.host = "box.example.com";
    s.port = 23;
    s.user = "u1";
    const auto conf = util::path_from_utf8("C:\\grab\\rclone.conf");
    const auto kh = util::path_from_utf8("C:\\grab\\known_hosts");
    const auto tail = std::vector<std::string>{"known_hosts_file=" + util::path_to_utf8(kh), "--no-obscure",
                                               "--non-interactive", "--config", util::path_to_utf8(conf)};
    auto expect = [&](std::vector<std::string> middle) {
        std::vector<std::string> v{"rclone", "config", "create", "box", "sftp", "host=box.example.com",
                                   "port=23", "user=u1"};
        v.insert(v.end(), middle.begin(), middle.end());
        v.insert(v.end(), tail.begin(), tail.end());
        return v;
    };

    s.auth = Auth::password;
    CHECK(servers::create_argv("rclone", conf, s, std::string("OBSCURED"), kh) == expect({"pass=OBSCURED"}));

    s.auth = Auth::key_file;
    s.key_file = "E:\\keys\\server.pem";
    CHECK(servers::create_argv("rclone", conf, s, std::nullopt, kh) == expect({"key_file=E:\\keys\\server.pem"}));
    CHECK(servers::create_argv("rclone", conf, s, std::string("OBS2"), kh) ==
          expect({"key_file=E:\\keys\\server.pem", "key_file_pass=OBS2"}));

    s.auth = Auth::agent;
    CHECK(servers::create_argv("rclone", conf, s, std::nullopt, kh) == expect({"key_use_agent=true"}));

    CHECK(servers::obscure_argv("rclone") == std::vector<std::string>{"rclone", "obscure", "-"});
    CHECK(servers::delete_argv("rclone", conf, "box") ==
          std::vector<std::string>{"rclone", "config", "delete", "box", "--config", util::path_to_utf8(conf)});
    const auto update = servers::update_argv("rclone", conf, "box", "known_hosts_file", "C:\\kh");
    CHECK(std::ranges::find(update, "known_hosts_file=C:\\kh") != update.end());
    const auto probe = servers::probe_argv("rclone", conf, "box");
    CHECK(probe[2] == "box:");
    CHECK(std::ranges::find(probe, "--contimeout") != probe.end());
}

TEST_CASE("to_remote carries auth and pinned host keys") {
    NewServer s;
    s.name = "n";
    s.host = "h";
    s.user = "u";
    s.port = 2222;
    s.auth = Auth::key_file;
    s.key_file = "E:\\k.pem";
    auto r = servers::to_remote(s, util::path_from_utf8("C:\\kh"));
    CHECK(r.key_file == "E:\\k.pem");
    CHECK_FALSE(r.key_use_agent);
    CHECK(r.known_hosts_file == "C:\\kh");
    CHECK(r.port == 2222);
    s.auth = Auth::agent;
    r = servers::to_remote(s, {});
    CHECK(r.key_use_agent);
    CHECK_FALSE(r.key_file.has_value());
    CHECK_FALSE(r.known_hosts_file.has_value());
}

namespace {
const char* const grab_conf =
    "# grab.conf - header comment\n"
    "\n"
    "[grab]\n"
    "# rclone executable.\n"
    "rclone =\n"
    "default_remote = hetzner\n"
    "\n"
    "# rclone remote [hetzner]: alice@h:22, key file, lookup via ssh + find\n"
    "[hetzner]\n"
    "rclone_remote = hetzner\n"
    "search_roots =\n"
    "# file_flags = --multi-thread-streams 8\n"
    "\n"
    "# rclone remote [box]: u@b:23, password, lookup via rclone lsf\n"
    "[box]\n"
    "rclone_remote = box\n"
    "max_depth = 3\n"
    "# common_flags = -P\n";
}

TEST_CASE("remove_section keeps everything else, comments included") {
    const auto without_hetzner = servers::remove_section(grab_conf, "hetzner");
    CHECK(without_hetzner ==
          "# grab.conf - header comment\n"
          "\n"
          "[grab]\n"
          "# rclone executable.\n"
          "rclone =\n"
          "default_remote = hetzner\n"
          "\n"
          "# rclone remote [box]: u@b:23, password, lookup via rclone lsf\n"
          "[box]\n"
          "rclone_remote = box\n"
          "max_depth = 3\n"
          "# common_flags = -P\n");
    const auto without_box = servers::remove_section(grab_conf, "box");
    CHECK(without_box.find("[box]") == std::string::npos);
    CHECK(without_box.find("# rclone remote [box]") == std::string::npos);
    CHECK(without_box.find("# file_flags = --multi-thread-streams 8") != std::string::npos); // hetzner's own
    CHECK(without_box.ends_with("# file_flags = --multi-thread-streams 8\n"));
    CHECK(servers::remove_section(grab_conf, "nope") == grab_conf);

    // CRLF files stay CRLF.
    const auto crlf = servers::remove_section("[a]\r\nx = 1\r\n\r\n[b]\r\ny = 2\r\n", "a");
    CHECK(crlf == "[b]\r\ny = 2\r\n");
}

TEST_CASE("append_section and set_value") {
    CHECK(servers::append_section("[grab]\nrclone =\n\n\n", "[x]\na = 1\n") == "[grab]\nrclone =\n\n[x]\na = 1\n");
    CHECK(servers::append_section("", "[x]\na = 1\n") == "[x]\na = 1\n");
    CHECK(servers::append_section("[grab]", "[x]\n") == "[grab]\n\n[x]\n");

    // Replaces the existing line in the right section only.
    const auto changed = servers::set_value(grab_conf, "grab", "default_remote", "box");
    CHECK(changed.find("default_remote = box\n") != std::string::npos);
    CHECK(changed.find("default_remote = hetzner") == std::string::npos);
    CHECK(changed.size() == std::string(grab_conf).size() - std::string("hetzner").size() + 3);
    // Blank value.
    CHECK(servers::set_value("[grab]\ndefault_remote = x\n", "grab", "default_remote", "") ==
          "[grab]\ndefault_remote =\n");
    // Missing key: inserted after the header.
    CHECK(servers::set_value("[grab]\nrclone =\n", "grab", "default_remote", "a") ==
          "[grab]\ndefault_remote = a\nrclone =\n");
    // Missing section: added at the top.
    CHECK(servers::set_value("[a]\nx = 1\n", "grab", "default_remote", "a") ==
          "[grab]\ndefault_remote = a\n\n[a]\nx = 1\n");
    // A commented-out key is not mistaken for the real one.
    CHECK(servers::set_value("[grab]\n# default_remote = old\n", "grab", "default_remote", "new") ==
          "[grab]\ndefault_remote = new\n# default_remote = old\n");
}
