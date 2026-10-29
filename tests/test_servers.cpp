#include "servers.hpp"
#include "rclone.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

using namespace grab;

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

namespace {
// Test keys (made for these tests), and OpenSSH's hashed names for them (ssh-keygen -H).
constexpr std::string_view ed25519_key = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAINXtya8ykvjW/uP06S6LBhpJ4oA2XyaMoBsjOTzK04lA";
constexpr std::string_view ecdsa_key = "ecdsa-sha2-nistp256 AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBImRejS9"
                                       "ErAh1OOdC47ZSRieKYuog30KmbjNe2eU1a83lUtUlQ4zWKV3as/jvRuK0Y3ZoA59R25yQ3xwJpQyNMU=";
constexpr std::string_view hashed_box_23 = "|1|63e5q7mt43md8U/bjWpnzvn8MwI=|hYRuAw/Is4vG99BeZ3UIYjHhdfY="; // [box.example]:23
constexpr std::string_view hashed_plain_22 = "|1|w0BT/nrCcP1DHthwczpGUg9UMMI=|v3uHQtErA9kyMH4iIIW+tcsfwUY="; // plain.example
} // namespace

TEST_CASE("known_hosts: the names that stand for a host and port (G87)") {
    CHECK(servers::names_host("[box.example]:23", "box.example", 23));
    // Plain names as written: libssh2 and rclone compare them exactly, so another case is
    // another host's pin.
    CHECK_FALSE(servers::names_host("[Box.Example]:23", "box.example", 23));
    CHECK_FALSE(servers::names_host("[box.example]:23", "BOX.example", 23));
    CHECK_FALSE(servers::names_host("Plain.Example", "plain.example", 22));
    CHECK_FALSE(servers::names_host("box.example", "box.example", 23)); // port 22's
    CHECK_FALSE(servers::names_host("[box.example]:2222", "box.example", 23));
    CHECK_FALSE(servers::names_host("[box.example.org]:23", "box.example", 23));
    // Port 22: the plain host, or the bracketed form.
    CHECK(servers::names_host("plain.example", "plain.example", 22));
    CHECK(servers::names_host("[plain.example]:22", "plain.example", 22));
    CHECK_FALSE(servers::names_host("[plain.example]:23", "plain.example", 22));
    // Patterns stand for many hosts: never this one's alone.
    CHECK_FALSE(servers::names_host("*.example", "box.example", 22));
    CHECK_FALSE(servers::names_host("!box.example", "box.example", 22));
    // Hashed names, by their HMAC.
    CHECK(servers::names_host(hashed_box_23, "box.example", 23));
    CHECK(servers::names_host(hashed_box_23, "Box.Example", 23));
    CHECK_FALSE(servers::names_host(hashed_box_23, "box.example", 22));
    CHECK_FALSE(servers::names_host(hashed_box_23, "other.example", 23));
    CHECK(servers::names_host(hashed_plain_22, "plain.example", 22));
    CHECK_FALSE(servers::names_host("|1|not base64!|x", "box.example", 23));
    CHECK_FALSE(servers::names_host("|1|63e5q7mt43md8U/bjWpnzvn8MwI=", "box.example", 23));
}

TEST_CASE("known_hosts: forgetting a host and port takes out its pins and nothing else (G87)") {
    const std::string ed25519(ed25519_key);
    const std::string ecdsa(ecdsa_key);
    const std::string text = "# grab's pins\n"
                             "[box.example]:23 " + ed25519 + "\n" +
                             "[box.example]:23 " + ecdsa + " a comment\r\n" +
                             "other.example,[box.example]:23,10.0.0.5 " + ed25519 + "\n" +
                             "  [BOX.example]:23\t" + ed25519 + "\n" +
                             "box.example " + ed25519 + "\n" +              // port 22's
                             "[box.example]:2222 " + ed25519 + "\n" +       // another port
                             std::string(hashed_box_23) + " " + ed25519 + "\n" +
                             "@revoked [box.example]:23 " + ecdsa + "\n" + // not a pin
                             "*.example " + ed25519 + "\n" +
                             "\n" +
                             "[box.example]:23 " + ed25519;                  // no line break at the end
    CHECK(servers::forget_host(text, "box.example", 23) == "# grab's pins\n"
                                                           "other.example,10.0.0.5 " + ed25519 + "\n" +
                                                           "  [BOX.example]:23\t" + ed25519 + "\n" + // another case
                                                           "box.example " + ed25519 + "\n" +
                                                           "[box.example]:2222 " + ed25519 + "\n" +
                                                           "@revoked [box.example]:23 " + ecdsa + "\n" +
                                                           "*.example " + ed25519 + "\n" +
                                                           "\n");
    CHECK(servers::forget_host(text, "nowhere.example", 23) == text);
    CHECK(servers::forget_host("", "box.example", 23).empty());

    // Port 22: the plain host and [host]:22 go, other ports stay; CRLF stays CRLF.
    const std::string plain = "plain.example " + ed25519 + "\r\n" +
                              "[plain.example]:22 " + ecdsa + "\r\n" +
                              "[plain.example]:23 " + ed25519 + "\r\n" +
                              std::string(hashed_plain_22) + " " + ed25519 + "\r\n" +
                              "plain.example,plain2.example " + ecdsa + "\r\n";
    CHECK(servers::forget_host(plain, "plain.example", 22) == "[plain.example]:23 " + ed25519 + "\r\n" +
                                                                "plain2.example " + ecdsa + "\r\n");

    // Pinning again: the old pins replaced by the new ones, the rest kept.
    const std::string pinned = "[box.example]:23 " + ecdsa + "\nother.example " + ed25519 + "\n";
    CHECK(servers::merge_known_hosts(servers::forget_host(pinned, "box.example", 23), {"[box.example]:23 " + ed25519}) ==
          "other.example " + ed25519 + "\n[box.example]:23 " + ed25519 + "\n");
}

TEST_CASE("key types as ssh-keygen names them") {
    CHECK(servers::key_label("ssh-ed25519") == "ED25519");
    CHECK(servers::key_label("ecdsa-sha2-nistp521") == "ECDSA");
    CHECK(servers::key_label("ssh-rsa") == "RSA");
}

TEST_CASE("merge_known_hosts appends only new lines") {
    const std::vector<std::string> keys{"[h]:23 ssh-ed25519 AAAA", "[h]:23 ssh-rsa BBBB"};
    const auto first = servers::merge_known_hosts("", keys);
    CHECK(first == "[h]:23 ssh-ed25519 AAAA\n[h]:23 ssh-rsa BBBB\n");
    CHECK(servers::merge_known_hosts(first, keys) == first);
    CHECK(servers::merge_known_hosts("other key", {"[h]:23 ssh-ed25519 AAAA"}) ==
          "other key\n[h]:23 ssh-ed25519 AAAA\n");
}

TEST_CASE("rclone's probe: the server in its environment, its config in memory; nothing else builds rclone config") {
    const auto probe = servers::probe_argv("rclone", "box");
    CHECK(probe[2] == "box:");
    CHECK(std::ranges::find(probe, "--contimeout") != probe.end());
    CHECK(probe.back() == config_in_memory); // the server comes in its environment (engine::rclone_env)
    CHECK(std::ranges::find(probe, "config") == probe.end());
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
    const auto changed = servers::set_value(grab_conf, "grab", "default_remote", "box").value();
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

TEST_CASE("set_value refuses line breaks and control characters, which would start lines of their own (G28)") {
    const std::string text = "[box]\nhost = h\n";
    for (const std::string_view bad : {std::string_view("h\nauth = agent"), std::string_view("h\rx"),
                                       std::string_view("h\0x", 3), std::string_view("h\tx"),
                                       std::string_view("h\x7Fx"), std::string_view("h\xC2\x85x")}) {
        const auto set = servers::set_value(text, "box", "host", bad);
        REQUIRE_FALSE(set.has_value());
        CHECK(set.error().find("control characters or line breaks") != std::string::npos);
    }
    CHECK_FALSE(servers::set_value(text, "box\n[evil]", "host", "h").has_value());
    CHECK_FALSE(servers::set_value(text, "box", "host\nauth", "h").has_value());
    // Georgian and other letters are fine.
    CHECK(servers::set_value(text, "box", "user", "\u10D2\u10D8\u10DD").value() ==
          "[box]\nuser = \u10D2\u10D8\u10DD\nhost = h\n");
}

TEST_CASE("set_value puts a new key after the keys it follows, else after the header") {
    const std::vector<std::string_view> before{"host", "port", "user", "auth"};
    CHECK(servers::set_value("[s]\nhost = h\nauth = agent\nx = 1\n", "s", "key_file", "k", before).value() ==
          "[s]\nhost = h\nauth = agent\nkey_file = k\nx = 1\n");
    CHECK(servers::set_value("[s]\nx = 1\n", "s", "key_file", "k", before).value() == "[s]\nkey_file = k\nx = 1\n");
    // A key there already stays where it is.
    CHECK(servers::set_value("[s]\nkey_file = old\nhost = h\n", "s", "key_file", "k", before).value() ==
          "[s]\nkey_file = k\nhost = h\n");
}

TEST_CASE("a repeated key or section is written where ini::parse reads it: the last one wins") {
    // A host set twice: the last line takes the value, the earlier one goes.
    CHECK(servers::set_value("[Box]\nhost = old.example\nuser = u\nhost = old.example\n", "Box", "host", "new.example")
              .value() == "[Box]\nuser = u\nhost = new.example\n");
    // [Box] split in two blocks, host in the second.
    const std::string split = "[Box]\nuser = u\n\n[grab]\nx = 1\n\n[Box]\nhost = old.example\n";
    CHECK(servers::set_value(split, "Box", "host", "new.example").value() ==
          "[Box]\nuser = u\n\n[grab]\nx = 1\n\n[Box]\nhost = new.example\n");
    // In both: the first goes, the second takes the value.
    CHECK(servers::set_value("[Box]\nhost = a\n\n[Box]\nhost = b\n", "Box", "host", "c").value() ==
          "[Box]\n\n[Box]\nhost = c\n");
    // A new key goes in the last block.
    CHECK(servers::set_value(split, "Box", "port", "23").value() ==
          "[Box]\nuser = u\n\n[grab]\nx = 1\n\n[Box]\nport = 23\nhost = old.example\n");
    // A removal takes every block of that name.
    CHECK(servers::remove_section(split, "Box") == "[grab]\nx = 1\n");
    CHECK(servers::remove_section("[a]\n1 = 1\n\n# rclone remote [Box]: x\n[Box]\nhost = h\n\n[b]\n2 = 2\n\n[Box]\nuser = u\n",
                                  "Box") == "[a]\n1 = 1\n\n[b]\n2 = 2\n");
}
