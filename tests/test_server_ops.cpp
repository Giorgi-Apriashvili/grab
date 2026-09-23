#include "config.hpp"
#include "ini.hpp"
#include "server_ops.hpp"
#include "servers.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace grab;
using servers::Auth;
using servers::NewServer;
using Updates = std::vector<std::pair<std::string, std::string>>;

namespace {

ini::Section section_of(const std::string& text, const std::string& name) {
    auto doc = ini::parse(text);
    REQUIRE(doc.has_value());
    const auto* s = doc->find(name);
    REQUIRE(s != nullptr);
    return *s;
}

NewServer server(Auth auth) {
    NewServer s;
    s.name = "box";
    s.host = "box.example.com";
    s.port = 23;
    s.user = "u1";
    s.auth = auth;
    if (auth == Auth::key_file) s.key_file = "C:\\keys\\id";
    return s;
}

// A scratch folder with its own grab.conf and rclone.conf, removed afterwards.
struct Scratch {
    std::filesystem::path dir;
    Scratch() {
        dir = std::filesystem::temp_directory_path() / "grab-test-server-ops";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
    }
    ~Scratch() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    void write(const std::string& name, const std::string& text) const {
        std::ofstream(dir / name, std::ios::binary) << text;
    }
    [[nodiscard]] std::string read(const std::string& name) const { return util::read_file(dir / name).value_or(""); }
};

const std::string rclone_conf = "[box]\ntype = sftp\nhost = box.example.com\nport = 23\nuser = u1\n"
                                "pass = OBSCURED\nshell_type = none\nknown_hosts_file = C:\\kh\n\n"
                                "[agent]\ntype = sftp\nhost = h.example.com\nuser = me\nkey_use_agent = true\n";

std::string grab_conf(const std::filesystem::path& dir) {
    return "[grab]\n"
           "rclone_config = " + util::path_to_utf8(dir / "rclone.conf") + "\n"
           "default_remote = box\n"
           "\n"
           "# rclone remote [box]: old description\n"
           "[box]\n"
           "rclone_remote = box\n"
           "# my own note about roots\n"
           "search_roots = /home/movies\n"
           "max_depth = 4\n"
           "\n"
           "[agent]\n"
           "max_depth = 6\n";
}

} // namespace

TEST_CASE("rclone edit: nothing changed means no updates") {
    const auto current = section_of(rclone_conf, "box");
    CHECK(server_ops::rclone_edit_updates(current, server(Auth::password), std::nullopt).empty());
    // A missing port is rclone's default 22.
    auto agent = server(Auth::agent);
    agent.host = "h.example.com";
    agent.port = 22;
    agent.user = "me";
    CHECK(server_ops::rclone_edit_updates(section_of(rclone_conf, "agent"), agent, std::nullopt).empty());
}

TEST_CASE("rclone edit: address and user changes") {
    auto wanted = server(Auth::password);
    wanted.host = "new.example.com";
    wanted.port = 2222;
    wanted.user = "u2";
    CHECK(server_ops::rclone_edit_updates(section_of(rclone_conf, "box"), wanted, std::nullopt) ==
          Updates{{"host", "new.example.com"}, {"port", "2222"}, {"user", "u2"}});
}

TEST_CASE("rclone edit: a new password replaces the stored one; blank keeps it") {
    const auto current = section_of(rclone_conf, "box");
    CHECK(server_ops::rclone_edit_updates(current, server(Auth::password), std::string("NEWOBSCURED")) ==
          Updates{{"pass", "NEWOBSCURED"}});
}

TEST_CASE("rclone edit: switching the login method blanks the old method's keys") {
    const auto box = section_of(rclone_conf, "box");
    CHECK(server_ops::rclone_edit_updates(box, server(Auth::agent), std::nullopt) ==
          Updates{{"key_use_agent", "true"}, {"pass", ""}});
    CHECK(server_ops::rclone_edit_updates(box, server(Auth::key_file), std::string("PP")) ==
          Updates{{"key_file", "C:\\keys\\id"}, {"key_file_pass", "PP"}, {"pass", ""}});

    const auto keyed = section_of("[k]\nhost = box.example.com\nport = 23\nuser = u1\nkey_file = C:\\keys\\id\n"
                                  "key_file_pass = PP\n",
                                  "k");
    CHECK(server_ops::rclone_edit_updates(keyed, server(Auth::password), std::string("PW")) ==
          Updates{{"pass", "PW"}, {"key_file", ""}, {"key_file_pass", ""}});
    CHECK(server_ops::rclone_edit_updates(keyed, server(Auth::agent), std::nullopt) ==
          Updates{{"key_use_agent", "true"}, {"key_file", ""}, {"key_file_pass", ""}});

    auto agent_host = server(Auth::password);
    agent_host.host = "h.example.com";
    agent_host.port = 22;
    agent_host.user = "me";
    CHECK(server_ops::rclone_edit_updates(section_of(rclone_conf, "agent"), agent_host, std::string("PW")) ==
          Updates{{"pass", "PW"}, {"key_use_agent", "false"}});
}

TEST_CASE("rclone edit: secrets only ever appear obscured on the command line") {
    const auto updates = server_ops::rclone_edit_updates(section_of(rclone_conf, "box"), server(Auth::password),
                                                         std::string("OBSCURED2"));
    const auto argv = servers::update_argv("rclone", util::path_from_utf8("C:\\r.conf"), "box", updates);
    CHECK(argv == std::vector<std::string>{"rclone", "config", "update", "box", "pass=OBSCURED2", "--no-obscure",
                                           "--non-interactive", "--config", "C:\\r.conf"});
}

TEST_CASE("an edit needs the host key again when the address changes or none is pinned") {
    server_ops::ServerInfo cur;
    cur.host = "box.example.com";
    cur.port = 23;
    cur.host_key_pinned = true;
    auto wanted = server(Auth::password);
    CHECK_FALSE(server_ops::edit_needs_host_key(cur, wanted));
    wanted.port = 22;
    CHECK(server_ops::edit_needs_host_key(cur, wanted));
    wanted = server(Auth::password);
    wanted.host = "other";
    CHECK(server_ops::edit_needs_host_key(cur, wanted));
    cur.host_key_pinned = false;
    CHECK(server_ops::edit_needs_host_key(cur, server(Auth::password)));
}

TEST_CASE("rclone errors lose their log prefix") {
    CHECK(server_ops::clean_rclone_error("2026/09/24 00:38:10 CRITICAL: Failed to create file system for \"hz3:\": "
                                         "ssh: this private key is passphrase protected\nmore\n") ==
          "Failed to create file system for \"hz3:\": ssh: this private key is passphrase protected");
    CHECK(server_ops::clean_rclone_error("2026/09/24 00:38:10 ERROR : x") == "x");
    CHECK(server_ops::clean_rclone_error("Error: unknown flag\n") == "Error: unknown flag");
    CHECK(server_ops::clean_rclone_error("").empty());
}

TEST_CASE("search_roots values and lead-in comments") {
    CHECK(search_roots_value({"", "/srv", "media"}) == "~, /srv, media");
    CHECK(search_roots_value({}).empty());

    const std::string text = "# rclone remote [box]: old\n[box]\nx = 1\n\n# my note\n[other]\ny = 2\n";
    CHECK(servers::replace_lead_in(text, "box", "# rclone remote [box]: new") ==
          "# rclone remote [box]: new\n[box]\nx = 1\n\n# my note\n[other]\ny = 2\n");
    // Only grab's own generated line is replaced, never a user's comment.
    CHECK(servers::replace_lead_in(text, "other", "# rclone remote [other]: new") == text);
    CHECK(servers::replace_lead_in(text, "missing", "# x") == text);
}

TEST_CASE("server list from grab.conf and rclone.conf") {
    Scratch s;
    s.write("rclone.conf", rclone_conf);
    s.write("grab.conf", grab_conf(s.dir));
    auto env = server_ops::load_env(s.dir / "grab.conf", s.dir);
    REQUIRE(env.has_value());
    CHECK(env->imported.empty()); // rclone_config is explicit: no import
    const auto list = server_ops::list_servers(*env);
    REQUIRE(list.size() == 2);
    CHECK(list[0].name == "box");
    CHECK(list[0].is_default);
    CHECK(list[0].auth == Auth::password);
    CHECK(list[0].host_key_pinned);
    CHECK_FALSE(list[0].ssh_search);
    CHECK(list[0].search_roots == std::vector<std::string>{"/home/movies"});
    CHECK(list[1].name == "agent");
    CHECK_FALSE(list[1].is_default);
    CHECK(list[1].auth == Auth::agent);
    CHECK(list[1].ssh_search);
    CHECK_FALSE(list[1].host_key_pinned);
    CHECK(list[1].max_depth == 6);
    CHECK(list[1].error.empty());
}

TEST_CASE("editing search folders keeps comments and refreshes grab's description line") {
    Scratch s;
    s.write("rclone.conf", rclone_conf);
    s.write("grab.conf", grab_conf(s.dir));
    auto env = server_ops::load_env(s.dir / "grab.conf", s.dir);
    REQUIRE(env.has_value());
    auto wanted = server(Auth::password); // rclone side unchanged: no rclone run needed
    wanted.search_roots = {"", "/srv/tv"};
    wanted.max_depth = 7;
    REQUIRE(server_ops::update_server(*env, wanted, std::nullopt, std::nullopt).has_value());
    const auto text = s.read("grab.conf");
    CHECK(text.find("search_roots = ~, /srv/tv\n") != std::string::npos);
    CHECK(text.find("max_depth = 7\n") != std::string::npos);
    CHECK(text.find("# my own note about roots\n") != std::string::npos);
    CHECK(text.find("# rclone remote [box]: u1@box.example.com:23, password, lookup via rclone lsf\n") !=
          std::string::npos);
    CHECK(text.find("[agent]\nmax_depth = 6\n") != std::string::npos);
    CHECK(s.read("rclone.conf") == rclone_conf);
}

TEST_CASE("default server and removal with an explicit rclone.conf") {
    Scratch s;
    s.write("rclone.conf", rclone_conf);
    s.write("grab.conf", grab_conf(s.dir));
    auto env = server_ops::load_env(s.dir / "grab.conf", s.dir);
    REQUIRE(env.has_value());
    REQUIRE(server_ops::set_default(*env, "agent").has_value());
    CHECK(s.read("grab.conf").find("default_remote = agent\n") != std::string::npos);
    CHECK_FALSE(server_ops::set_default(*env, "nope").has_value());

    env = server_ops::load_env(s.dir / "grab.conf", s.dir);
    REQUIRE(env.has_value());
    auto note = server_ops::remove_server(*env, "agent");
    REQUIRE(note.has_value());
    CHECK(note->empty());
    const auto text = s.read("grab.conf");
    CHECK(text.find("[agent]") == std::string::npos);
    CHECK(text.find("default_remote = box\n") != std::string::npos); // moved to the next server
    CHECK(s.read("rclone.conf") == rclone_conf); // a shared rclone.conf is never edited
}
