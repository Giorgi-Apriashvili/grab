// A server's connection details in grab.conf (server_conf.hpp): the schema, the one set of checks
// the command, the window and the import share, and what counts as pinned.

#include "config.hpp"
#include "ini.hpp"
#include "server_conf.hpp"
#include "servers.hpp"

#include <doctest/doctest.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace grab;
using server_conf::Auth;
using server_conf::Connection;

namespace {

std::expected<std::optional<Connection>, std::string> parse_section(std::string_view text, std::string_view name = "s") {
    const auto doc = ini::parse(text);
    REQUIRE(doc.has_value());
    const auto* s = doc->find(name);
    REQUIRE(s != nullptr);
    return server_conf::parse(*s);
}

} // namespace

TEST_CASE("server_conf: a section's connection details, and the lines grab writes for them") {
    const auto full = parse_section("[s]\nhost = box.example\nport = 23\nuser = u1\nauth = Key\n"
                                    "key_file = C:\\keys\\id\nshell_type = unix\nmd5sum_command = md5sum\n"
                                    "sha1sum_command = none\npath_override = /data\nrclone.ciphers = aes256-ctr\n"
                                    "rclone.description =\nrclone_remote = s\nsearch_roots = /srv\n");
    REQUIRE_MESSAGE(full.has_value(), full.error_or(""));
    REQUIRE(full->has_value());
    const Connection& c = **full;
    CHECK(c.host == "box.example");
    CHECK(c.port == 23);
    CHECK(c.user == "u1");
    CHECK(c.auth == Auth::key);
    CHECK(c.key_file == "C:\\keys\\id");
    CHECK(c.sha1sum_command == "none");
    CHECK(c.rclone == std::vector<std::pair<std::string, std::string>>{{"ciphers", "aes256-ctr"}});
    // Written back with servers::set_value, it reads the same.
    std::string text = "[s]\nrclone_remote = s\n";
    const auto lines = server_conf::lines(c);
    CHECK(lines.front() == std::pair<std::string, std::string>{"host", "box.example"});
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) text = servers::set_value(text, "s", it->first, it->second);
    CHECK(text.starts_with("[s]\nhost = box.example\nport = 23\nuser = u1\nauth = key\nkey_file = C:\\keys\\id\n"));
    CHECK(parse_section(text) == full);

    // No host: an old-style section, its server still in rclone.conf.
    CHECK(parse_section("[s]\nrclone_remote = s\nuser = u\n").value() == std::nullopt);
    CHECK(parse_section("[s]\nhost =\n").value() == std::nullopt);
    // Blank port: 22.
    const auto blank_port = parse_section("[s]\nhost = h\nuser = u\nauth = agent\nport =\n");
    REQUIRE(blank_port.has_value());
    CHECK(blank_port->value_or(Connection{}).port == 22);
    CHECK(blank_port->has_value());

    // The RcloneRemote grab's code reads it as: grab's known_hosts always.
    const auto r = server_conf::to_remote("s", c, "C:\\grab\\known_hosts");
    CHECK(r.host == "box.example");
    CHECK(r.key_file == "C:\\keys\\id");
    CHECK_FALSE(r.key_use_agent);
    CHECK(r.known_hosts_file == "C:\\grab\\known_hosts");
}

TEST_CASE("server_conf: clear messages for sections with a host but odd details") {
    const auto error = [](std::string_view text) { return parse_section(text).error_or(""); };
    CHECK(error("[s]\nhost = h\nuser = u\n").find("no login method: add auth = agent, key, password or stored_key") !=
          std::string::npos);
    CHECK(error("[s]\nhost = h\nuser = u\nauth = magic\n") == "[s] auth must be agent, key, password or stored_key, got 'magic'");
    CHECK(error("[s]\nhost = h\nuser = u\nauth = key\n") == "[s] auth = key needs a key_file");
    CHECK(error("[s]\nhost = h\nuser = u\nauth = password\nkey_file = k\n") ==
          "[s] key_file goes with auth = key or agent, not password");
    CHECK(error("[s]\nhost = h\nuser = u\nauth = agent\nport = 22x\n") == "[s] port must be a number from 1 to 65535, got '22x'");
    CHECK(error("[s]\nhost = h\nuser = u\nauth = agent\nport = 70000\n") ==
          "[s] port: the port must be from 1 to 65535");
    CHECK(error("[s]\nhost = -oProxyCommand=x\nuser = u\nauth = agent\n") == "[s] host: the host can't start with -");
    CHECK(error("[s]\nhost = h\nuser = -l\nauth = agent\n") == "[s] user: the user can't start with -");
    CHECK(error("[s]\nhost = h\nauth = agent\n") == "[s] user: a user is required");
    // grab.conf holds no secret: rclone settings outside the allowlist are refused.
    CHECK(error("[s]\nhost = h\nuser = u\nauth = password\nrclone.pass = x\n").find("rclone.pass can't be set in grab.conf") !=
          std::string::npos);
    CHECK(error("[s]\nhost = h\nuser = u\nauth = agent\nrclone.set_env = A=b\n").find("rclone.set_env") != std::string::npos);
}

TEST_CASE("server_conf: the checks the command, the window and the import share (G28, G5, G169)") {
    using server_conf::check_host;
    CHECK_FALSE(check_host("box.example"));
    CHECK_FALSE(check_host("u123.your-storagebox.de"));
    CHECK_FALSE(check_host("65.21.224.123"));
    CHECK_FALSE(check_host("my_nas"));
    CHECK_FALSE(check_host("fe80::1"));
    CHECK_FALSE(check_host("fe80::1%eth0"));
    CHECK_FALSE(check_host("::ffff:10.0.0.1"));
    CHECK(check_host("") == "a host is required");
    CHECK(check_host("-oProxyCommand=calc") == "the host can't start with -");
    CHECK(check_host("[::1]") == "use an IPv6 address without brackets, or a host name");
    CHECK(check_host("a b") == "use a host name (letters, digits, - and .) or an IP address");
    CHECK(check_host(".example"));
    CHECK(check_host("h\u00F6st.example"));
    CHECK(check_host("h\nx"));
    CHECK(check_host(std::string(254, 'a')));

    using server_conf::check_value;
    CHECK_FALSE(check_value("C:\\Users\\a b\\id_\u10E1"));
    for (const auto bad : std::to_array<std::string_view>({"a\nb", "a\rb", "a\tb", "a\x7F", "a\u0085b", {"a\0b", 3}})) {
        CHECK(check_value(bad) == "it can't hold control characters or line breaks");
    }
    CHECK_FALSE(server_conf::check_user("john doe@example.com"));
    CHECK(server_conf::check_user("-x"));
    CHECK(server_conf::check_port(0));
    CHECK_FALSE(server_conf::check_port(65535));
    CHECK_FALSE(server_conf::check_key_file("C:\\k\\id", true));
    CHECK(server_conf::check_key_file("C:\\k\\id", false) == "there is no file C:\\k\\id");
    CHECK(server_conf::check_search_root("/srv/a,b") == "a folder with ',' in its name can't be a search folder (',' separates them)");
    CHECK_FALSE(server_conf::check_search_root("/srv/a b"));

    const std::vector<std::string> sections{"Main", "box", "\u10E1\u10D4\u10E0\u10D5\u10D4\u10E0\u10D8"};
    CHECK_FALSE(server_conf::check_new_name("Box.Guest", sections));
    CHECK(server_conf::check_new_name("x", sections) == "use at least 2 characters (rclone takes one letter for a drive)");
    CHECK(server_conf::check_new_name("box", sections) == "there is a server called box already");
    CHECK(server_conf::check_new_name("BOX", sections) ==
          "BOX differs from the server box only in case, which Windows doesn't tell apart");
    CHECK(server_conf::check_new_name("-x", sections));
    CHECK(server_conf::check_new_name("grab", sections));
    // Go's upper case: Georgian Mkhedruli and Mtavruli, long s, are one name to rclone.
    CHECK(server_conf::same_ignoring_case("\u10E1\u10D4\u10E0", "\u1CA1\u1C94\u1CA0"));
    CHECK(server_conf::same_ignoring_case("\u017Ftore", "STORE"));
    CHECK(server_conf::same_ignoring_case("Main", "mAIN"));
    CHECK_FALSE(server_conf::same_ignoring_case("box", "box2"));
    // An existing (imported) name only gets notes.
    CHECK(server_conf::name_problems("x", sections).size() == 1);
    CHECK(server_conf::name_problems("MAIN", sections) ==
          std::vector<std::string>{"its name differs from [Main] only in case, so Windows gives both one file list"});
    CHECK(server_conf::name_problems("\u10E1\u10D4\u10E0\u10D5\u10D4\u10E0\u10D8", sections).empty());

    Connection c;
    c.host = "h";
    c.user = "u";
    c.key_file = "k\nx";
    c.rclone = {{"pass", "x"}};
    CHECK(server_conf::check("s", c) == std::vector<std::string>{"[s] key_file: it can't hold control characters or line breaks",
                                                              "[s] rclone.pass: not a setting grab.conf can hold"});
    CHECK(server_conf::rclone_key_allowed("ciphers"));
    CHECK_FALSE(server_conf::rclone_key_allowed("ssh"));
    CHECK(server_conf::parse_auth(" Stored_Key ") == Auth::stored_key);
}

TEST_CASE("server_conf: pinned is a line for [host]:port in grab's known_hosts (G209)") {
    const std::string key = " ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIFAKE";
    const std::string kh = "# [c.example]:2222" + key + "\n"
                           "@cert-authority [ca.example]:2222" + key + "\n"
                           "a.example,[b.example]:2222" + key + "\n"
                           "[nokey.example]:22 ssh-ed25519\n"
                           "  [d.example]:23" + key + "\r\n";
    CHECK(server_conf::pinned(kh, "a.example", 22));
    CHECK(server_conf::pinned(kh, "b.example", 2222));
    CHECK(server_conf::pinned(kh, "d.example", 23));
    CHECK_FALSE(server_conf::pinned(kh, "a.example", 2222));
    CHECK_FALSE(server_conf::pinned(kh, "b.example", 22));
    CHECK_FALSE(server_conf::pinned(kh, "c.example", 2222));  // a comment
    CHECK_FALSE(server_conf::pinned(kh, "ca.example", 2222)); // a marker line
    CHECK_FALSE(server_conf::pinned(kh, "nokey.example", 22));
    CHECK_FALSE(server_conf::pinned(kh, "A.example", 22)); // names as written, as rclone matches them
    CHECK_FALSE(server_conf::pinned("", "a.example", 22));
    // A hashed name (ssh-keygen -H) of [h.example]:2222 with a made-up salt.
    const std::string hashed = "|1|AAAAAAAAAAAAAAAAAAAAAAAAAAA=|";
    CHECK_FALSE(server_conf::pinned(hashed + "AAAAAAAAAAAAAAAAAAAAAAAAAAA=" + key + "\n", "h.example", 2222));
}

TEST_CASE("server_conf: grab 0.19's reading of grab.conf ignores the new keys") {
    // parse_grab_config with ini::parse is how v0.19.0 reads grab.conf (both unchanged in what
    // they do with a section's keys): by name, so host, auth and rclone.* lines change nothing.
    const std::string old = "[grab]\ndefault_remote = Box\n\n[Box]\nrclone_remote = Box\nsearch_roots = /a, b\n"
                            "max_depth = 3\nfind = rclone\nmax_connections = 5\n";
    const std::string now = "[grab]\ndefault_remote = Box\n\n[Box]\nhost = u1.your-storagebox.de\nport = 23\nuser = u1\n"
                            "auth = password\nkey_file = C:\\k\\id\nshell_type = unix\nmd5sum_command = md5sum\n"
                            "sha1sum_command = sha1sum\npath_override = /x\nrclone.ciphers = aes256-ctr\n"
                            "rclone.description = d\nrclone_remote = Box\nsearch_roots = /a, b\nmax_depth = 3\nfind = rclone\n"
                            "max_connections = 5\n";
    const auto a = parse_grab_config(*ini::parse(old));
    const auto b = parse_grab_config(*ini::parse(now));
    REQUIRE((a.has_value() && b.has_value()));
    REQUIRE(b->remotes.size() == 1);
    const auto& x = a->remotes.front();
    const auto& y = b->remotes.front();
    CHECK(x.name == y.name);
    CHECK(x.rclone_remote == y.rclone_remote);
    CHECK(x.search_roots == y.search_roots);
    CHECK(x.max_depth == y.max_depth);
    CHECK(x.find == y.find);
    CHECK(x.max_connections == y.max_connections);
    CHECK(x.engine == y.engine);
    CHECK(x.common_flags == y.common_flags);
    CHECK(a->default_remote == b->default_remote);
}
