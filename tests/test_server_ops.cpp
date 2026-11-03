// server_ops: grab's servers in grab.conf, their passwords in grab's store and their host keys in
// grab's known_hosts, without rclone (RCLONE-REPLACEMENT.md, phase 7 steps 8 and 9). Every file
// here is made up, in a temp folder or the test runner's scratch APPDATA (test_main.cpp), and
// every secret is an obviously fake one.

#include "config.hpp"
#include "engine.hpp"
#include "file_list.hpp"
#include "gui_state.hpp"
#include "ini.hpp"
#include "passwords.hpp"
#include "scoped_env.hpp"
#include "server_conf.hpp"
#include "server_ops.hpp"
#include "servers.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (LockFileEx, SetFileAttributesW)
#endif

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace grab;
using servers::Auth;
using servers::NewServer;

namespace {

NewServer server(Auth auth, std::string name = "box") {
    NewServer s;
    s.name = std::move(name);
    s.host = "box.example.com";
    s.port = 23;
    s.user = "u1";
    s.auth = auth;
    return s;
}

const server_ops::HostKeys box_keys{.lines = {"[box.example.com]:23 ssh-ed25519 BOX"}, .fingerprints = {}};

// A scratch folder with its own grab.conf, removed afterwards with what grab keeps for it
// (state_dir: the store, limits, gui.json) and grab's known_hosts (the runner's scratch APPDATA).
struct Scratch {
    std::filesystem::path dir;
    std::filesystem::path conf;
    explicit Scratch(const std::string& name = "grab-test-server-ops") {
        dir = std::filesystem::temp_directory_path() / name;
        conf = dir / "grab.conf";
        REQUIRE(util::path_to_utf8(grab_known_hosts_path()).find("grab-tests-home") != std::string::npos);
        clean();
        std::filesystem::create_directories(dir);
    }
    ~Scratch() {
        try {
            clean();
        } catch (...) { // only std::bad_alloc, which a destructor must not throw
            std::fputs("grab_tests: a temp folder was left behind\n", stderr);
        }
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    void clean() const {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::remove_all(state_dir(conf), ec);
        std::filesystem::remove(grab_known_hosts_path(), ec);
        std::filesystem::remove_all(file_list::dir_for(conf), ec);
    }
    void write(const std::string& name, const std::string& text) const {
        std::ofstream(dir / name, std::ios::binary) << text;
    }
    [[nodiscard]] std::string read(const std::string& name) const { return util::read_file(dir / name).value_or(""); }
    [[nodiscard]] server_ops::Env env() const {
        auto e = server_ops::load_env(conf);
        REQUIRE_MESSAGE(e.has_value(), e.error_or(""));
        return std::move(*e);
    }
    [[nodiscard]] std::filesystem::path store() const { return passwords::path_in(state_dir(conf)); }
    [[nodiscard]] passwords::Store secrets() const { return passwords::load(store()).value_or(passwords::Store{}); }
    [[nodiscard]] std::string store_bytes() const { return util::read_file(store()).value_or(""); }
    static void pins(const std::string& text) {
        std::filesystem::create_directories(grab_known_hosts_path().parent_path());
        std::ofstream(grab_known_hosts_path(), std::ios::binary) << text;
    }
    [[nodiscard]] static std::string pins() { return util::read_file(grab_known_hosts_path()).value_or(""); }
};

const std::string grab_conf = "[grab]\n"
                              "default_remote = box\n"
                              "\n"
                              "# rclone remote [box]: old description\n"
                              "[box]\n"
                              "host = box.example.com\n"
                              "port = 23\n"
                              "user = u1\n"
                              "auth = password\n"
                              "shell_type = unix\n"
                              "md5sum_command = md5sum\n"
                              "rclone.ciphers = aes256-ctr\n"
                              "rclone_remote = box\n"
                              "# my own note about roots\n"
                              "search_roots = /home/movies\n"
                              "max_depth = 4\n"
                              "\n"
                              "[agent]\n"
                              "host = h.example.com\n"
                              "user = me\n"
                              "auth = agent\n"
                              "max_depth = 6\n";

passwords::Secret fake(std::string value, std::string user = "u1", std::string host = "box.example.com", int port = 23) {
    return passwords::Secret{.binding = {.user = std::move(user), .host = std::move(host), .port = port}, .value = std::move(value)};
}

} // namespace

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

TEST_CASE("a saved password stays with the user, host and port it was typed for (G40)") {
    server_ops::ServerInfo cur;
    cur.host = "box.example.com";
    cur.user = "u1";
    cur.port = 23;
    cur.auth = Auth::password;
    cur.secret_saved = true;
    auto wanted = server(Auth::password);
    CHECK_FALSE(server_ops::edit_needs_password(cur, wanted));
    // Nothing to keep: none saved for this login, a store that can't be read, no details at all.
    for (auto nothing : {+[](server_ops::ServerInfo& c) { c.secret_saved = false; },
                         +[](server_ops::ServerInfo& c) { c.secret_saved.reset(); },
                         +[](server_ops::ServerInfo& c) { c.error = "[box] has no connection details"; }}) {
        auto none = cur;
        nothing(none);
        CHECK(server_ops::edit_needs_password(none, wanted));
    }
    for (auto change : {+[](NewServer& s) { s.port = 22; }, +[](NewServer& s) { s.host = "evil.example"; },
                        +[](NewServer& s) { s.host = "BOX.example.com"; }, +[](NewServer& s) { s.user = "u2"; }}) {
        wanted = server(Auth::password);
        change(wanted);
        CHECK(server_ops::edit_needs_password(cur, wanted)); // the store's binding is exact
    }
    cur.auth = Auth::agent; // switching to a password always needs one
    CHECK(server_ops::edit_needs_password(cur, server(Auth::password)));
    CHECK_FALSE(server_ops::edit_needs_password(cur, server(Auth::agent)));

    // The backend refuses it too, whatever the window says, and the store keeps it for its login.
    Scratch s;
    s.write("grab.conf", grab_conf);
    REQUIRE(passwords::save(s.store(), {{"box", {{"password", fake("fake-pw")}}}}));
    for (auto change : {+[](NewServer& w) { w.host = "evil.example"; }, +[](NewServer& w) { w.port = 2222; },
                        +[](NewServer& w) { w.user = "root"; }}) {
        auto moved = server(Auth::password);
        change(moved);
        const auto refused = server_ops::update_server(s.env(), moved, std::nullopt, server_ops::HostKeys{
            .lines = {std::format("[{}]:{} ssh-ed25519 K", moved.host, moved.port)}, .fingerprints = {}});
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().find("enter the password again") != std::string::npos);
    }
    CHECK(s.read("grab.conf") == grab_conf);
    CHECK(s.secrets() == passwords::Store{{"box", {{"password", fake("fake-pw")}}}});
}

TEST_CASE("search_roots values and lead-in comments") {
    CHECK(search_roots_value({"", "/srv", "media"}) == "~, /srv, media");
    CHECK(search_roots_value({}).empty());

    const std::string text = "# rclone remote [box]: old\n[box]\nx = 1\n\n# my note\n[other]\ny = 2\n";
    CHECK(servers::replace_lead_in(text, "box", "# rclone remote [box]: new") ==
          "# rclone remote [box]: new\n[box]\nx = 1\n\n# my note\n[other]\ny = 2\n");
    CHECK(servers::replace_lead_in(text, "other", "# x") == text); // not grab's comment: untouched

    // grab's own lead-in since phase 7, and 0.19's (whose remote may have another name), are both grab's.
    CHECK(lead_in("box", "u@h:23, password, lookup via rclone lsf") ==
          "# server [box]: u@h:23, password, lookup via rclone lsf");
    const std::string mixed = "[grab]\n\n# server [box]: old\n[box]\nx = 1\n\n# rclone remote [hz3]: old\n[hetzner]\n"
                              "y = 2\n\n# server [box] is mine, not grab's\n[other]\nz = 3\n";
    const auto renewed = servers::replace_lead_in(mixed, "hetzner", lead_in("hetzner", "new"));
    CHECK(renewed.find("# server [hetzner]: new\n[hetzner]\n") != std::string::npos);
    CHECK(servers::replace_lead_in(mixed, "box", lead_in("box", "new")).find("# server [box]: new\n[box]\n") !=
          std::string::npos);
    // A comment naming another section isn't this one's lead-in.
    CHECK(servers::replace_lead_in(mixed, "other", lead_in("other", "new")) == mixed);
    CHECK(servers::remove_section(mixed, "box") ==
          "[grab]\n\n# rclone remote [hz3]: old\n[hetzner]\ny = 2\n\n# server [box] is mine, not grab's\n[other]\nz = 3\n");
    CHECK(servers::remove_section(mixed, "hetzner") ==
          "[grab]\n\n# server [box]: old\n[box]\nx = 1\n\n# server [box] is mine, not grab's\n[other]\nz = 3\n");
}

TEST_CASE("server list from grab.conf: details, pins read now (G209), sections without details") {
    Scratch s;
    s.write("grab.conf", grab_conf + "\n[bare]\nmax_depth = 2\n\n[odd]\nhost = h\nuser = u\nauth = magic\n"
                                     "\n[imported]\nhost = k.example\nuser = k\nauth = stored_key\nrclone_remote = old\n");
    Scratch::pins("[box.example.com]:23 ssh-ed25519 BOX\n");
    auto env = s.env();
    const auto list = server_ops::list_servers(env);
    REQUIRE(list.size() == 5);
    CHECK(list[0].name == "box");
    CHECK(list[0].is_default);
    CHECK(list[0].auth == Auth::password);
    CHECK(list[0].host == "box.example.com");
    CHECK(list[0].port == 23);
    CHECK(list[0].host_key_pinned);
    CHECK(list[0].search == "probe"); // find where the server has it, else by listing folders
    CHECK(list[0].search_roots == std::vector<std::string>{"/home/movies"});
    CHECK(list[1].name == "agent");
    CHECK(list[1].auth == Auth::agent);
    CHECK(list[1].search == "probe");
    CHECK(list[1].port == 22);
    CHECK_FALSE(list[1].host_key_pinned);
    CHECK(list[1].max_depth == 6);
    CHECK(list[1].error.empty());
    CHECK(list[2].error.find("no connection details") != std::string::npos);
    CHECK(list[3].error.find("auth must be") != std::string::npos);
    CHECK(list[4].auth == Auth::stored_key);
    CHECK(list[4].rclone_remote == "old");
    // Pinned is what the file holds when asked.
    Scratch::pins("h.example.com ssh-ed25519 H\n");
    const auto again = server_ops::list_servers(env);
    CHECK_FALSE(again[0].host_key_pinned);
    CHECK(again[1].host_key_pinned);
}

TEST_CASE("server list: the guard's reasons, the connection keys; Trust and the host-key fetch refused for a guarded server") {
    Scratch s;
    s.write("grab.conf", grab_conf + "bind = 10.0.0.2\nconnect_timeout = 1500ms\nio_timeout = 2m\n"
                                     "\n[plain]\nhost = p.example\nuser = p\nauth = agent\ncommon_flags = --bind 10.0.0.7\n"
                                     "\n[routed]\nhost = r.example\nuser = r\nauth = agent\n"
                                     "\n[jumped]\nhost = j.example\nuser = j\nauth = agent\n"
                                     "ssh_options = -o ProxyJump=fake-jump.example\n"
                                     "\n[quirky]\nauth = agent\nuser = q\nhost = q.example\n"
                                     "rclone.disable_concurrent_reads = true\n");
    REQUIRE(passwords::save(s.store(), {{"box", {{"password", fake("fake-pw")},
                                                 {"rclone.socks_proxy", fake("fake-user:fake-pass@127.0.0.1:9")}}},
                                        {"agent", {{"rclone.http_proxy", fake("p:1", "someone-else", "h.example.com", 22)}}},
                                        {"routed", {{"rclone.ssh", fake("ssh fake-jump", "r", "r.example", 22)}}}}));
    const auto env = s.env();
    const auto list = server_ops::list_servers(env);
    REQUIRE(list.size() == 6);
    // box: its store's proxy (bound to its login); agent: one saved for another login doesn't count.
    REQUIRE(list[0].guard.size() == 1);
    CHECK(list[0].guard[0].where == "rclone.socks_proxy in grab's store");
    CHECK(list[1].guard.empty());
    CHECK(list[1].search == "probe");
    // An agent server routed by its store's ssh command: searched as any other, never by rclone.
    REQUIRE(list[3].guard.size() == 1);
    CHECK(list[3].search == "probe");
    // ssh_options' jump host is no reason (decision 4); a quirk switch in the section is.
    CHECK(list[4].guard.empty());
    REQUIRE(list[5].guard.size() == 1);
    CHECK(list[5].guard[0].where == "rclone.disable_concurrent_reads");
    CHECK(list[1].connection_keys == "bind 10.0.0.2, connect_timeout 1500ms, io_timeout 120s");
    CHECK(list[0].connection_keys.empty());
    // Trust and the host-key fetch of an Edit: refused for a guarded server, naming the setting
    // and how to clear it, never its value, before anything connects.
    for (const char* name : {"box", "routed", "quirky"}) {
        const auto refused = server_ops::reach_for(env, name);
        REQUIRE_FALSE(refused);
        CHECK(refused.error().starts_with(std::format("[{}] ", name)));
        CHECK(refused.error().find("fake-") == std::string::npos);
        CHECK(refused.error().find("Connect directly") == std::string::npos);
    }
    CHECK(server_ops::reach_for(env, "box").error().ends_with(
        "the import kept rclone.socks_proxy in grab's store: remove box and add it again (Remove and Add in grab's "
        "Settings, or `grab server remove box` and `grab server add`)"));
    CHECK(server_ops::reach_for(env, "jumped").has_value());
    // The host-key fetch connects from the server's bind, within its connect time (common_flags'
    // --bind where the key is blank); a new server has none.
    const auto agent = server_ops::reach_for(env, "agent");
    REQUIRE(agent);
    CHECK(agent->bind == "10.0.0.2");
    CHECK(agent->bind_from == "bind in [agent]");
    CHECK(agent->connect_timeout == std::chrono::milliseconds(1'500));
    const auto plain = server_ops::reach_for(env, "plain");
    REQUIRE(plain);
    CHECK(plain->bind == "10.0.0.7");
    CHECK(plain->bind_from == "common_flags' --bind");
    CHECK_FALSE(plain->connect_timeout);
    const auto none = server_ops::reach_for(env, "new");
    REQUIRE(none);
    CHECK(none->bind.empty());
    {
        // An inherited RCLONE_SFTP_SOCKS_PROXY covers every server, a new one's host-key fetch too.
        const test::ScopedEnv proxy("RCLONE_SFTP_SOCKS_PROXY", "fake-user:fake-pass@127.0.0.1:1080");
        for (const char* name : {"new", "agent"}) {
            const auto refused = server_ops::reach_for(env, name);
            REQUIRE_FALSE(refused);
            CHECK(refused.error().find("RCLONE_SFTP_SOCKS_PROXY is set in grab's environment and covers every server") !=
                  std::string::npos);
            CHECK(refused.error().find("fake-") == std::string::npos);
        }
    }
    CHECK(server_ops::reach_for(env, "new").has_value());
}

TEST_CASE("an edit of search folders keeps comments, refreshes grab's line and leaves the store alone (G85)") {
    Scratch s;
    s.write("grab.conf", grab_conf);
    REQUIRE(passwords::save(s.store(), {{"box", {{"password", fake("fake-pw")}}}}));
    const auto sealed = s.store_bytes();
    auto wanted = server(Auth::password); // no password typed: the saved one stays
    wanted.search_roots = {"", "/srv/tv"};
    wanted.max_depth = 7;
    auto done = server_ops::update_server(s.env(), wanted, std::nullopt, std::nullopt);
    REQUIRE_MESSAGE(done.has_value(), done.error_or(""));
    CHECK(done->empty());
    const auto text = s.read("grab.conf");
    CHECK(text.find("search_roots = ~, /srv/tv\n") != std::string::npos);
    CHECK(text.find("max_depth = 7\n") != std::string::npos);
    CHECK(text.find("# my own note about roots\n") != std::string::npos);
    // grab 0.19's lead-in becomes grab's own.
    CHECK(text.find("# server [box]: u1@box.example.com:23, password, lookup with find, else by listing folders\n[box]\n") !=
          std::string::npos);
    CHECK(text.find("# rclone remote [box]") == std::string::npos);
    // The connection lines and what rclone found are as they were.
    CHECK(text.find("host = box.example.com\nport = 23\nuser = u1\nauth = password\nshell_type = unix\n"
                    "md5sum_command = md5sum\nrclone.ciphers = aes256-ctr\n") != std::string::npos);
    CHECK(text.find("[agent]\nhost = h.example.com\nuser = me\nauth = agent\nmax_depth = 6\n") != std::string::npos);
    CHECK(s.store_bytes() == sealed); // byte for byte: not even saved again
}

TEST_CASE("grab's comment above a section says the lookup (describe_servers, an edit)") {
    Scratch s;
    s.write("grab.conf", grab_conf);
    // A proxy its store keeps changes nothing there: grab's own lookup, never "via rclone lsf".
    REQUIRE(passwords::save(s.store(), {{"box", {{"password", fake("fake-pw")},
                                                 {"rclone.socks_proxy", fake("fake-user:fake-pass@127.0.0.1:9")}}}}));
    const std::string said =
        "# server [box]: u1@box.example.com:23, password, lookup with find, else by listing folders\n[box]\n";
    const auto described = server_ops::describe_servers(s.env());
    REQUIRE_MESSAGE(described.has_value(), described.error_or(""));
    auto text = s.read("grab.conf");
    CHECK(text.find(said) != std::string::npos);
    CHECK(text.find("# rclone remote [box]") == std::string::npos);
    CHECK(text.find("fake-pass") == std::string::npos);
    // Nothing changed: grab.conf isn't written again.
    const auto before = std::filesystem::last_write_time(s.dir / "grab.conf");
    REQUIRE(server_ops::describe_servers(s.env()).has_value());
    CHECK(std::filesystem::last_write_time(s.dir / "grab.conf") == before);
    // An edit writes the same.
    auto wanted = server(Auth::password);
    wanted.max_depth = 7;
    const auto done = server_ops::update_server(s.env(), wanted, std::nullopt, std::nullopt);
    REQUIRE_MESSAGE(done.has_value(), done.error_or(""));
    text = s.read("grab.conf");
    CHECK(text.find(said) != std::string::npos);
}

TEST_CASE("a server's connection limit is written only once set, and blank means automatic") {
    Scratch s;
    s.write("grab.conf", grab_conf);
    REQUIRE(passwords::save(s.store(), {{"box", {{"password", fake("fake-pw")}}}}));
    auto env = s.env();
    CHECK_FALSE(server_ops::list_servers(env)[0].max_connections.has_value());
    CHECK(server_ops::list_servers(env)[0].default_connections == 12);

    auto wanted = server(Auth::password);
    wanted.search_roots = {"/home/movies"};
    REQUIRE(server_ops::update_server(env, wanted, std::nullopt, std::nullopt).has_value());
    CHECK(s.read("grab.conf").find("max_connections") == std::string::npos); // untouched while automatic

    wanted.max_connections = 5;
    REQUIRE(server_ops::update_server(s.env(), wanted, std::nullopt, std::nullopt).has_value());
    CHECK(s.read("grab.conf").find("max_connections = 5\n") != std::string::npos);
    CHECK(server_ops::list_servers(s.env())[0].max_connections == 5);

    wanted.max_connections.reset();
    REQUIRE(server_ops::update_server(s.env(), wanted, std::nullopt, std::nullopt).has_value());
    CHECK(s.read("grab.conf").find("max_connections =\n") != std::string::npos);
    CHECK_FALSE(server_ops::list_servers(s.env())[0].max_connections.has_value());

    // Out of range is a config error.
    s.write("grab.conf", grab_conf + "max_connections = 0\n");
    CHECK_FALSE(server_ops::load_env(s.conf).has_value());
}

TEST_CASE("a change starts from grab.conf as it is now, not as it was loaded (G175)") {
    Scratch s;
    s.write("grab.conf", grab_conf);
    const auto env = s.env();
    // Another grab adds a server after `env` was loaded.
    s.write("grab.conf", grab_conf + "\n[late]\nhost = l.example\nuser = l\nauth = agent\n");
    REQUIRE(server_ops::set_default(env, "late"));
    const auto text = s.read("grab.conf");
    CHECK(text.find("default_remote = late\n") != std::string::npos);
    CHECK(text.find("[late]\nhost = l.example\n") != std::string::npos);
}

TEST_CASE("a new server's name: no other section's, ignoring case; 2 characters; one only grab's old rclone.conf has is free") {
    Scratch s;
    s.write("grab.conf", grab_conf + "\n[\xE1\x83\xA1\xE1\x83\x94\xE1\x83\xA0\xE1\x83\x95\xE1\x83\x94\xE1\x83\xA0\xE1\x83\x98]\n");
    // grab's old rclone.conf still lists a remote "nas": it doesn't count.
    std::filesystem::create_directories(grab_rclone_config_path().parent_path());
    std::ofstream(grab_rclone_config_path(), std::ios::binary) << "[nas]\ntype = sftp\nhost = nas.example\nuser = n\n";
    const auto env = s.env();
    CHECK(server_ops::name_taken(env, "box").value_or("").find("already") != std::string::npos);
    CHECK(server_ops::name_taken(env, "BOX").value_or("").find("only in case") != std::string::npos);
    CHECK(server_ops::name_taken(env, "x").has_value()); // one letter: a drive to rclone (G169)
    CHECK(server_ops::name_taken(env, "grab").has_value());
    CHECK_FALSE(server_ops::name_taken(env, "nas").has_value());
    CHECK_FALSE(server_ops::name_taken(env, "box.eu").has_value());
    std::filesystem::remove(grab_rclone_config_path());
}

TEST_CASE("a server's details are checked as grab.conf takes them (G5, G28)") {
    auto s = server(Auth::agent);
    CHECK_FALSE(server_ops::check_server(s).has_value());
    s.host = "-oProxyCommand=evil";
    CHECK(server_ops::check_server(s).value_or(server_ops::Problem{}).field == "host");
    s = server(Auth::agent);
    s.user = "-l";
    CHECK(server_ops::check_server(s).value_or(server_ops::Problem{}).field == "user");
    s = server(Auth::agent);
    s.user = "u\nauth = agent";
    CHECK(server_ops::check_server(s).value_or(server_ops::Problem{}).field == "user");
    s = server(Auth::agent);
    s.port = 0;
    CHECK(server_ops::check_server(s).value_or(server_ops::Problem{}).field == "port");
    s = server(Auth::key);
    s.key_file = (std::filesystem::temp_directory_path() / "grab-no-such-key").string();
    CHECK(server_ops::check_server(s).value_or(server_ops::Problem{}).field == "keyFile");
    s = server(Auth::agent);
    s.search_roots = {"/a,b"};
    CHECK(server_ops::check_server(s).value_or(server_ops::Problem{}).field == "roots");
}

TEST_CASE("add: keys pinned, the password in the store for its login, the section in grab.conf, no rclone") {
    Scratch s;
    s.write("grab.conf", "[grab]\n");
    const auto added = server_ops::add_server(s.env(), server(Auth::password), std::string("fake-pw"), box_keys);
    REQUIRE_MESSAGE(added.has_value(), added.error_or(""));
    CHECK(Scratch::pins() == "[box.example.com]:23 ssh-ed25519 BOX\n");
    CHECK(s.secrets() == passwords::Store{{"box", {{"password", fake("fake-pw")}}}});
    const auto text = s.read("grab.conf");
    CHECK(text.find("default_remote = box\n") != std::string::npos);
    CHECK(text.find("[box]\nhost = box.example.com\nport = 23\nuser = u1\nauth = password\n") != std::string::npos);
    CHECK(text.find("rclone_remote") == std::string::npos); // a new section's remote is its name
    CHECK(text.find("fake-pw") == std::string::npos);
    CHECK_FALSE(std::filesystem::exists(grab_rclone_config_path()));
    // It reads back as a server.
    const auto list = server_ops::list_servers(s.env());
    REQUIRE(list.size() == 1);
    CHECK(list[0].error.empty());
    CHECK(list[0].host_key_pinned);

    // A password server without one, a stored key, a taken name: refused, nothing written.
    const auto before = s.read("grab.conf");
    auto two = server(Auth::password, "two");
    CHECK_FALSE(server_ops::add_server(s.env(), two, std::nullopt, box_keys).has_value());
    two.auth = Auth::stored_key;
    CHECK_FALSE(server_ops::add_server(s.env(), two, std::nullopt, box_keys).has_value());
    CHECK_FALSE(server_ops::add_server(s.env(), server(Auth::agent, "Box"), std::nullopt, box_keys).has_value());
    CHECK(s.read("grab.conf") == before);
}

TEST_CASE("Georgian, dotted and case-only names through add, edit and remove") {
    Scratch s;
    // A Georgian name comes from the import (a new one keeps validate_name's ASCII rule, chunk C):
    // its section as the import writes it, its password in the store.
    const std::string georgian = "\xE1\x83\xA1\xE1\x83\x94\xE1\x83\xA0\xE1\x83\x95\xE1\x83\x94\xE1\x83\xA0\xE1\x83\x98";
    s.write("grab.conf", "[grab]\ndefault_remote = " + georgian + "\n\n[" + georgian +
                             "]\nhost = box.example.com\nport = 23\nuser = u1\nauth = password\n");
    REQUIRE(passwords::save(s.store(), {{georgian, {{"password", fake("fake-" + georgian)}}}}));
    Scratch::pins("[box.example.com]:23 ssh-ed25519 BOX\n");
    CHECK(server_ops::add_server(s.env(), server(Auth::agent, "\xE1\x83\xA1\xE1\x83\x94"), std::nullopt, box_keys)
              .error_or("")
              .find("letters, digits") != std::string::npos);
    for (const std::string& name : {std::string("box.eu"), std::string("my-box_1")}) {
        CAPTURE(name);
        auto added = server_ops::add_server(s.env(), server(Auth::password, name), std::string("fake-" + name), box_keys);
        REQUIRE_MESSAGE(added.has_value(), added.error_or(""));
    }
    // One differing from box.eu only in case: refused (one list file, one RCLONE_CONFIG_ name).
    CHECK_FALSE(server_ops::add_server(s.env(), server(Auth::agent, "BOX.EU"), std::nullopt, box_keys).has_value());
    auto list = server_ops::list_servers(s.env());
    REQUIRE(list.size() == 3);
    CHECK(list[0].name == georgian);
    CHECK(list[0].is_default);
    CHECK(s.secrets().at(georgian).at("password").value == "fake-" + georgian);

    // Edit the Georgian one's folders: its password stays; then to ssh-agent: it goes.
    auto edit = server(Auth::password, georgian);
    edit.search_roots = {"/srv"};
    REQUIRE(server_ops::update_server(s.env(), edit, std::nullopt, std::nullopt).has_value());
    CHECK(s.secrets().at(georgian).contains("password"));
    edit.auth = Auth::agent;
    REQUIRE(server_ops::update_server(s.env(), edit, std::nullopt, std::nullopt).has_value());
    CHECK_FALSE(s.secrets().contains(georgian));
    CHECK(s.read("grab.conf").find("host = box.example.com\nport = 23\nuser = u1\nauth = agent\n") != std::string::npos);

    // Remove box.eu: its section and password go, the default stays on the Georgian one, and
    // the host keys stay (two servers are still there).
    auto removed = server_ops::remove_server(s.env(), "box.eu");
    REQUIRE(removed.has_value());
    CHECK(removed->empty());
    CHECK(s.read("grab.conf").find("[box.eu]") == std::string::npos);
    CHECK_FALSE(s.secrets().contains("box.eu"));
    CHECK(s.secrets().contains("my-box_1"));
    CHECK(Scratch::pins() == "[box.example.com]:23 ssh-ed25519 BOX\n");
    REQUIRE(server_ops::remove_server(s.env(), georgian).has_value());
    CHECK(s.read("grab.conf").find("default_remote = my-box_1\n") != std::string::npos);
    REQUIRE(server_ops::remove_server(s.env(), "my-box_1").has_value());
    CHECK(s.secrets().empty());
    CHECK(Scratch::pins().empty()); // the last server there: its keys go
}

TEST_CASE("an edit changes the store only with the login method, the key file or the secret, and moves what belongs to the server (G85)") {
    Scratch s;
    s.write("grab.conf", grab_conf);
    Scratch::pins("[box.example.com]:23 ssh-ed25519 BOX\n");
    const passwords::Store start{{"box", {{"password", fake("fake-pw")}, {"rclone.set_env", fake("FAKE=1")}}}};
    REQUIRE(passwords::save(s.store(), start));

    // A new password replaces the old one.
    REQUIRE(server_ops::update_server(s.env(), server(Auth::password), std::string("fake-new"), std::nullopt));
    CHECK(s.secrets().at("box").at("password") == fake("fake-new"));
    CHECK(s.secrets().at("box").at("rclone.set_env") == fake("FAKE=1"));

    // To a key file with a passphrase: the password goes, the passphrase comes, the extra stays.
    const auto key = std::filesystem::temp_directory_path() / "grab-test-server-ops-key";
    std::ofstream(key, std::ios::binary) << "not a real key";
    auto keyed = server(Auth::key);
    keyed.key_file = util::path_to_utf8(key);
    REQUIRE(server_ops::update_server(s.env(), keyed, std::string("fake-phrase"), std::nullopt));
    CHECK(s.secrets() == passwords::Store{{"box", {{"passphrase", fake("fake-phrase")}, {"rclone.set_env", fake("FAKE=1")}}}});
    CHECK(s.read("grab.conf").find("auth = key\n") != std::string::npos);
    CHECK(s.read("grab.conf").find("key_file = " + util::path_to_utf8(key) + "\n") != std::string::npos);

    // To another port with its keys: the passphrase and the extra move with it (bound to the new
    // login), what the old address answered goes, and so do its keys (nobody else is there).
    keyed.port = 2223;
    const auto moved = server_ops::update_server(
        s.env(), keyed, std::nullopt, server_ops::HostKeys{.lines = {"[box.example.com]:2223 ssh-ed25519 NEW"}, .fingerprints = {}});
    REQUIRE_MESSAGE(moved.has_value(), moved.error_or(""));
    CHECK(s.secrets() == passwords::Store{{"box", {{"passphrase", fake("fake-phrase", "u1", "box.example.com", 2223)},
                                                   {"rclone.set_env", fake("FAKE=1", "u1", "box.example.com", 2223)}}}});
    const auto text = s.read("grab.conf");
    CHECK(text.find("port = 2223\n") != std::string::npos);
    CHECK(text.find("shell_type =\n") != std::string::npos); // blank: unknown, asked again
    CHECK(text.find("md5sum_command =\n") != std::string::npos);
    CHECK(text.find("rclone.ciphers = aes256-ctr\n") != std::string::npos); // a setting of the user's: kept
    CHECK(Scratch::pins() == "[box.example.com]:2223 ssh-ed25519 NEW\n");

    // The same key file: its passphrase stays.
    REQUIRE(server_ops::update_server(s.env(), keyed, std::nullopt, std::nullopt));
    CHECK(s.secrets().at("box").at("passphrase") == fake("fake-phrase", "u1", "box.example.com", 2223));
    // Another key file with a passphrase typed: it replaces the old one.
    const auto other = std::filesystem::temp_directory_path() / "grab-test-server-ops-key2";
    std::ofstream(other, std::ios::binary) << "not a real key either";
    keyed.key_file = util::path_to_utf8(other);
    REQUIRE(server_ops::update_server(s.env(), keyed, std::string("fake-phrase2"), std::nullopt));
    CHECK(s.secrets().at("box").at("passphrase") == fake("fake-phrase2", "u1", "box.example.com", 2223));
    // Another key file and nothing typed: the old key's passphrase goes (rclone refuses an
    // unencrypted key given one); the extra stays.
    keyed.key_file = util::path_to_utf8(key);
    REQUIRE(server_ops::update_server(s.env(), keyed, std::nullopt, std::nullopt));
    CHECK(s.secrets() == passwords::Store{{"box", {{"rclone.set_env", fake("FAKE=1", "u1", "box.example.com", 2223)}}}});

    // A stored key stays only where it was.
    CHECK_FALSE(server_ops::update_server(s.env(), server(Auth::stored_key), std::nullopt, std::nullopt).has_value());
    std::filesystem::remove(key);
    std::filesystem::remove(other);
}

TEST_CASE("a section without connection details is filled in by Edit") {
    Scratch s;
    s.write("grab.conf", "[grab]\n\n[old]\nrclone_remote = gone\nmax_depth = 3\n");
    auto list = server_ops::list_servers(s.env());
    REQUIRE(list.size() == 1);
    CHECK_FALSE(list[0].error.empty());
    CHECK(server_ops::edit_needs_host_key(list[0], server(Auth::agent, "old")));
    auto wanted = server(Auth::agent, "old");
    wanted.max_depth = 3;
    const auto done = server_ops::update_server(s.env(), wanted, std::nullopt, box_keys);
    REQUIRE_MESSAGE(done.has_value(), done.error_or(""));
    CHECK(s.read("grab.conf").find("host = box.example.com\nport = 23\nuser = u1\nauth = agent\nrclone_remote = gone\n") !=
          std::string::npos);
    list = server_ops::list_servers(s.env());
    CHECK(list[0].error.empty());
    CHECK(list[0].host_key_pinned);
}

TEST_CASE("pinning replaces a host's old keys and keeps every other line (G87)") {
    Scratch s;
    const auto file = s.dir / "known_hosts";
    s.write("known_hosts", "# mine\n"
                           "[box.example.com]:23 ssh-ed25519 OLD1\n"
                           "box.example.com,[box.example.com]:23 ssh-rsa OLD2\n"
                           "[other.example.com]:23 ssh-ed25519 KEEP\n");
    REQUIRE(server_ops::pin_host_keys(file, "box.example.com", 23, {"[box.example.com]:23 ssh-ed25519 NEW"}));
    CHECK(s.read("known_hosts") == "# mine\n"
                                   "box.example.com ssh-rsa OLD2\n"
                                   "[other.example.com]:23 ssh-ed25519 KEEP\n"
                                   "[box.example.com]:23 ssh-ed25519 NEW\n");
    // Keys read for another address, or none, are refused, and nothing changes.
    const auto before = s.read("known_hosts");
    CHECK_FALSE(server_ops::pin_host_keys(file, "box.example.com", 23, {"box.example.com ssh-ed25519 PORT22"}));
    CHECK_FALSE(server_ops::pin_host_keys(file, "box.example.com", 23, {}));
    CHECK(s.read("known_hosts") == before);
    REQUIRE(server_ops::forget_host_keys(file, "box.example.com", 23));
    CHECK(s.read("known_hosts") == "# mine\nbox.example.com ssh-rsa OLD2\n[other.example.com]:23 ssh-ed25519 KEEP\n");
    // A missing known_hosts has nothing to forget, and isn't made.
    REQUIRE(server_ops::forget_host_keys(s.dir / "none", "box.example.com", 23));
    CHECK_FALSE(std::filesystem::exists(s.dir / "none"));
}

TEST_CASE("trust pins only: grab.conf and the store stay as they are") {
    Scratch s;
    s.write("grab.conf", grab_conf);
    REQUIRE(passwords::save(s.store(), {{"box", {{"password", fake("fake-pw")}}}}));
    const auto sealed = s.store_bytes();
    Scratch::pins("[box.example.com]:23 ssh-ed25519 OLD\nh.example.com ssh-ed25519 H\n");
    REQUIRE(server_ops::trust_server(s.env(), "box", server_ops::HostKeys{.lines = {"[box.example.com]:23 ssh-ed25519 NEW"}, .fingerprints = {}}));
    CHECK(Scratch::pins() == "h.example.com ssh-ed25519 H\n[box.example.com]:23 ssh-ed25519 NEW\n");
    CHECK(s.read("grab.conf") == grab_conf);
    CHECK(s.store_bytes() == sealed);
    CHECK_FALSE(server_ops::trust_server(s.env(), "nope", box_keys).has_value());
}

TEST_CASE("remove: the section, the default moved on, its password, its keys, list, learned limit and timings (G87, G122)") {
    Scratch s;
    // twin: a second login on box's machine, whose keys are box's too.
    s.write("grab.conf", grab_conf + "\n[twin]\nhost = box.example.com\nport = 23\nuser = u2\nauth = agent\n");
    const std::string others = "[box.example.com]:2222 ssh-ed25519 OTHERPORT\nunrelated.example ssh-ed25519 KEEP\n";
    Scratch::pins("[box.example.com]:23 ssh-ed25519 BOX\nh.example.com ssh-ed25519 H\n[h.example.com]:22 ssh-rsa H22\n" +
                  others);
    REQUIRE(passwords::save(s.store(), {{"box", {{"password", fake("fake-pw")}}}, {"agent", {{"rclone.encoding", fake("Slash", "me", "h.example.com", 22)}}}}));
    // What grab learned for box: a list, a limit, a window timing.
    const auto list_file = file_list::path_for(file_list::dir_for(s.conf), "box");
    std::filesystem::create_directories(list_file.parent_path());
    std::ofstream(list_file, std::ios::binary) << "x";
    const auto limits = learned_limits_path(s.conf);
    REQUIRE(save_learned_limit(limits, "box", 3));
    REQUIRE(save_learned_limit(limits, "agent", 5));
    GuiState gui;
    gui.timings = {{"lsjson:box", 900}, {"walk:box", 100}, {"lsjson:box2", 50}, {"find:agent", 70}};
    REQUIRE(save_gui_state(state_dir(s.conf) / "gui.json", gui));

    auto removed = server_ops::remove_server(s.env(), "box");
    REQUIRE(removed.has_value());
    CHECK_MESSAGE(removed->empty(), *removed);
    const auto text = s.read("grab.conf");
    CHECK(text.find("[box]") == std::string::npos);
    CHECK(text.find("# rclone remote [box]") == std::string::npos);
    CHECK(text.find("default_remote = agent\n") != std::string::npos);
    CHECK(s.secrets() == passwords::Store{{"agent", {{"rclone.encoding", fake("Slash", "me", "h.example.com", 22)}}}});
    CHECK(Scratch::pins().find("[box.example.com]:23 ssh-ed25519 BOX\n") != std::string::npos); // twin is there
    CHECK_FALSE(std::filesystem::exists(list_file));
    CHECK(load_learned_limits(limits) == std::map<std::string, int>{{"agent", 5}});
    const auto timings = read_gui_state(state_dir(s.conf) / "gui.json");
    REQUIRE(timings);
    REQUIRE(*timings);
    CHECK(timings.value_or(std::nullopt).value_or(GuiState{}).timings == std::map<std::string, int>{{"lsjson:box2", 50}, {"find:agent", 70}});

    REQUIRE(server_ops::remove_server(s.env(), "twin").has_value());
    CHECK(Scratch::pins().find("BOX") == std::string::npos); // nobody is there now
    // Port 22: the plain host's line and [host]:22's.
    REQUIRE(server_ops::remove_server(s.env(), "agent").has_value());
    CHECK(Scratch::pins() == others);
    CHECK(s.secrets().empty());
}

TEST_CASE("a remote of an explicit rclone_config that uses grab's known_hosts keeps its host's keys (G87)") {
    Scratch s;
    // [mine] isn't a grab server's but checks with grab's known_hosts (spelled in capitals: the
    // same file all the same). grab's old rclone.conf doesn't count this way.
    std::string upper = util::path_to_utf8(grab_known_hosts_path());
    std::ranges::transform(upper, upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    s.write("rclone.conf", "[mine]\ntype = sftp\nhost = h.example.com\nuser = me\nknown_hosts_file = " + upper + "\n");
    s.write("grab.conf", "[grab]\nrclone_config = " + util::path_to_utf8(s.dir / "rclone.conf") + "\n\n" +
                             grab_conf.substr(grab_conf.find("\n[box]") + 1));
    const std::string pins = "[box.example.com]:23 ssh-ed25519 BOX\nh.example.com ssh-ed25519 H\n";
    Scratch::pins(pins);
    for (const char* name : {"box", "agent"}) {
        auto note = server_ops::remove_server(s.env(), name);
        REQUIRE(note.has_value());
        CHECK(note->empty());
    }
    CHECK(Scratch::pins() == "h.example.com ssh-ed25519 H\n");
    CHECK(s.read("rclone.conf").find("[mine]") != std::string::npos); // never written
}

TEST_CASE("an edit to another host takes the old one's keys out unless another server is there; G122 on any move") {
    Scratch s;
    s.write("grab.conf", grab_conf + "\n[twin]\nhost = box.example.com\nport = 23\nuser = u2\nauth = agent\n");
    Scratch::pins("[box.example.com]:23 ssh-ed25519 BOX\nh.example.com ssh-ed25519 H\n");
    const auto limits = learned_limits_path(s.conf);
    REQUIRE(save_learned_limit(limits, "agent", 4));

    // agent moves to another port, with its keys there: the old port's go, and so does the limit
    // it learned there.
    auto agent = server(Auth::agent, "agent");
    agent.host = "h.example.com";
    agent.port = 2200;
    agent.user = "me";
    agent.max_depth = 6;
    auto done = server_ops::update_server(s.env(), agent, std::nullopt,
                                          server_ops::HostKeys{.lines = {"[h.example.com]:2200 ssh-ed25519 H2200"}, .fingerprints = {}});
    REQUIRE_MESSAGE(done.has_value(), done.error_or(""));
    CHECK(done->empty());
    CHECK(Scratch::pins() == "[box.example.com]:23 ssh-ed25519 BOX\n[h.example.com]:2200 ssh-ed25519 H2200\n");
    CHECK(load_learned_limits(limits).empty());

    // box moves to another host; twin stays at the old one, whose keys stay.
    auto box = server(Auth::agent);
    box.host = "box2.example.com";
    done = server_ops::update_server(s.env(), box, std::nullopt,
                                     server_ops::HostKeys{.lines = {"[box2.example.com]:23 ssh-ed25519 BOX2"}, .fingerprints = {}});
    REQUIRE_MESSAGE(done.has_value(), done.error_or(""));
    CHECK(Scratch::pins() == "[box.example.com]:23 ssh-ed25519 BOX\n[h.example.com]:2200 ssh-ed25519 H2200\n"
                             "[box2.example.com]:23 ssh-ed25519 BOX2\n");
    // A user changed alone: no keys asked, but what grab learned for the login goes.
    REQUIRE(save_learned_limit(limits, "twin", 2));
    auto twin = server(Auth::agent, "twin");
    twin.user = "u3";
    REQUIRE(server_ops::update_server(s.env(), twin, std::nullopt, std::nullopt));
    CHECK(load_learned_limits(limits).empty());
}

#ifdef _WIN32
TEST_CASE("a known_hosts that can't be read is never replaced by the new pins alone (G89)") {
    Scratch s;
    const std::string pinned = "other.example.invalid ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOMqqnkVzrm0SdG6UOoqKLsabgH5C9okWi0dh2l9GKJl\n";
    s.write("known_hosts", pinned);
    const auto file = s.dir / "known_hosts";
    // Another program holds a lock on it for a moment: reading fails, replacing would work.
    const HANDLE other = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(other != INVALID_HANDLE_VALUE);
    OVERLAPPED at{};
    REQUIRE(LockFileEx(other, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 100000, 0, &at));
    const auto result = server_ops::pin_host_keys(file, "new.example.invalid", 22, {"new.example.invalid ssh-ed25519 BBBB"});
    const auto forgot = server_ops::forget_host_keys(file, "other.example.invalid", 22);
    UnlockFileEx(other, 0, 100000, 0, &at);
    CloseHandle(other);
    CHECK_FALSE(result.has_value());
    CHECK_FALSE(forgot.has_value());
    CHECK(s.read("known_hosts") == pinned);
    // A missing one is simply started.
    REQUIRE(server_ops::pin_host_keys(s.dir / "new_known_hosts", "new.example.invalid", 22,
                                      {"new.example.invalid ssh-ed25519 BBBB"}).has_value());
    CHECK(s.read("new_known_hosts").find("new.example.invalid ssh-ed25519 BBBB") != std::string::npos);
}

TEST_CASE("an add whose grab.conf can't be written puts the store back, so the name stays free (G218)") {
    Scratch s;
    s.write("grab.conf", grab_conf);
    REQUIRE(passwords::save(s.store(), {{"box", {{"password", fake("fake-pw")}}}}));
    const auto fresh = server(Auth::password, "fresh");
    SetFileAttributesW(s.conf.c_str(), FILE_ATTRIBUTE_READONLY);
    const auto added = server_ops::add_server(s.env(), fresh, std::string("fake-fresh"), box_keys);
    SetFileAttributesW(s.conf.c_str(), FILE_ATTRIBUTE_NORMAL);
    REQUIRE_FALSE(added.has_value());
    CHECK(s.read("grab.conf") == grab_conf);
    CHECK(s.secrets() == passwords::Store{{"box", {{"password", fake("fake-pw")}}}});
    // The same add now goes through.
    const auto again = server_ops::add_server(s.env(), fresh, std::string("fake-fresh"), box_keys);
    REQUIRE_MESSAGE(again.has_value(), again.error_or(""));
    CHECK(s.read("grab.conf").find("[fresh]\nhost = box.example.com\n") != std::string::npos);
    CHECK(s.secrets().at("fresh").at("password") == fake("fake-fresh"));
}

TEST_CASE("a store that can't be decrypted is moved aside by a change, which names who needs a secret again") {
    Scratch s;
    s.write("grab.conf", grab_conf + "\n[kept]\nhost = k.example.com\nuser = k\nauth = stored_key\n");
    std::filesystem::create_directories(s.store().parent_path());
    const std::string unreadable = "grab-passwords-sealed 1\nanother account's";
    std::ofstream(s.store(), std::ios::binary) << unreadable;
    // An agent server is added all the same, the old file kept beside the new one.
    const auto added = server_ops::add_server(s.env(), server(Auth::agent, "two"), std::nullopt, box_keys);
    REQUIRE_MESSAGE(added.has_value(), added.error_or(""));
    CHECK(added->find("moved aside to ") != std::string::npos);
    CHECK(added->find("enter the password of box again with Edit") != std::string::npos);
    CHECK(added->find("kept logged in with a key kept in grab's store") != std::string::npos);
    CHECK(s.read("grab.conf").find("[two]\n") != std::string::npos);
    std::vector<std::filesystem::path> aside;
    for (const auto& e : std::filesystem::directory_iterator(s.store().parent_path())) {
        if (e.path().filename().string().starts_with("passwords.unreadable-")) aside.push_back(e.path());
    }
    REQUIRE(aside.size() == 1);
    CHECK(util::read_file(aside.front()).value_or("") == unreadable);
    // Once set aside, the password of box is typed again with Edit, and nothing more is said.
    auto box = server(Auth::password, "box");
    box.port = 23;
    box.user = "u1";
    const auto edited = server_ops::update_server(s.env(), box, std::string("fake-pw2"), std::nullopt);
    REQUIRE_MESSAGE(edited.has_value(), edited.error_or(""));
    CHECK(edited->find("moved aside") == std::string::npos);
    CHECK(s.secrets() == passwords::Store{{"box", {{"password", fake("fake-pw2")}}}});
}

TEST_CASE("an unchanged Edit of an agent server moves an undecryptable store aside, so grab connects again") {
    Scratch s;
    s.write("grab.conf", grab_conf);
    Scratch::pins("h.example.com ssh-ed25519 FAKEKEY\n");
    std::filesystem::create_directories(s.store().parent_path());
    std::ofstream(s.store(), std::ios::binary) << "grab-passwords-sealed 1\nanother account's";
    RemoteSettings settings;
    settings.name = "agent";
    settings.rclone_remote = "agent";
    server_conf::Connection c;
    c.host = "h.example.com";
    c.user = "me";
    c.auth = server_conf::Auth::agent;
    // The store may keep its proxy, so grab doesn't connect to it, nor fetch its host key...
    CHECK(engine::ssh_target(engine::make_context(GrabConfig{}, settings, c, s.conf)).error_or("").find(
              "saving agent with Edit (no change needed) sets the unreadable store aside") != std::string::npos);
    CHECK_FALSE(server_ops::reach_for(s.env(), "agent"));
    // ...until it is saved with nothing changed: the agent server needs no store, but the file
    // that can't be read goes aside and the note names who needs a secret again.
    auto agent = server(Auth::agent, "agent");
    agent.host = "h.example.com";
    agent.port = 22;
    agent.user = "me";
    agent.max_depth = 6;
    const auto edited = server_ops::update_server(s.env(), agent, std::nullopt, std::nullopt);
    REQUIRE_MESSAGE(edited.has_value(), edited.error_or(""));
    CHECK(edited->find("moved aside to ") != std::string::npos);
    // The proxy or ssh command the store kept goes with it: the note says so (docs/doc.md, Servers).
    CHECK(edited->find("with any proxy or ssh command a server kept there (such a server now connects directly)") !=
          std::string::npos);
    CHECK(edited->find("enter the password of box again with Edit") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(s.store()));
    const auto target = engine::ssh_target(engine::make_context(GrabConfig{}, settings, c, s.conf));
    CHECK_MESSAGE(target.has_value(), target.error_or(""));
    CHECK(server_ops::reach_for(s.env(), "agent").has_value());
}

TEST_CASE("an Edit of a server never pinned gets past an undecryptable store: its host-key fetch goes on") {
    Scratch s;
    s.write("grab.conf", grab_conf);
    std::filesystem::create_directories(s.store().parent_path());
    std::ofstream(s.store(), std::ios::binary) << "grab-passwords-sealed 1\nanother account's";
    // Trust stays refused; the Edit the refusal names fetches the host key, as the save sets the
    // store aside.
    const auto trust = server_ops::reach_for(s.env(), "agent");
    REQUIRE_FALSE(trust.has_value());
    CHECK(trust.error().find("saving agent with Edit (no change needed)") != std::string::npos);
    CHECK(server_ops::reach_for(s.env(), "agent", server_ops::Purpose::edit).has_value());
    auto agent = server(Auth::agent, "agent");
    agent.host = "h.example.com";
    agent.port = 22;
    agent.user = "me";
    agent.max_depth = 6;
    const server_ops::HostKeys keys{.lines = {"h.example.com ssh-ed25519 FAKEKEY"}, .fingerprints = {}};
    const auto edited = server_ops::update_server(s.env(), agent, std::nullopt, keys);
    REQUIRE_MESSAGE(edited.has_value(), edited.error_or(""));
    CHECK(edited->find("moved aside to ") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(s.store()));
    CHECK(Scratch::pins() == "h.example.com ssh-ed25519 FAKEKEY\n");
    CHECK(server_ops::reach_for(s.env(), "agent").has_value());
}

TEST_CASE("an Edit's host-key fetch counts what the save leaves: section lines and store extras of any login") {
    Scratch s;
    // Details that can't be read: the section's own rclone.* line and the store's proxy both stay
    // through the save, so the fetch is refused too.
    s.write("grab.conf", "[grab]\ndefault_remote = box\n\n[box]\nhost = box.example.com\nport = abc\nuser = u1\n"
                         "auth = password\nrclone.disable_concurrent_reads = true\n"
                         "\n[stored]\nhost = s.example.com\nport = abc\nuser = s\nauth = agent\n"
                         "\n[agent]\nhost = h.example.com\nuser = me\nauth = agent\n");
    REQUIRE(passwords::save(s.store(), {{"stored", {{"rclone.socks_proxy", fake("fake-user:fake-pass@127.0.0.1:9", "s",
                                                                                 "s.example.com", 22)}}},
                                        {"agent", {{"rclone.http_proxy", fake("p:1", "someone-else", "h.example.com", 22)}}}}));
    const auto env = s.env();
    const auto box = server_ops::reach_for(env, "box", server_ops::Purpose::edit);
    REQUIRE_FALSE(box.has_value());
    CHECK(box.error().find("rclone.disable_concurrent_reads") != std::string::npos);
    const auto stored = server_ops::reach_for(env, "stored", server_ops::Purpose::edit);
    REQUIRE_FALSE(stored.has_value());
    CHECK(stored.error().find("rclone.socks_proxy in grab's store") != std::string::npos);
    CHECK(stored.error().find("fake-") == std::string::npos);
    // A proxy kept for another login: no reason for Trust, but an Edit binds it to the new login.
    CHECK(server_ops::reach_for(env, "agent").has_value());
    const auto agent = server_ops::reach_for(env, "agent", server_ops::Purpose::edit);
    REQUIRE_FALSE(agent.has_value());
    CHECK(agent.error().find("rclone.http_proxy in grab's store") != std::string::npos);
}

TEST_CASE("an unchanged Edit goes on when the store can't be read at all") {
    // A folder in the store's place can't be read nor moved aside by passwords::load_for_change:
    // an Edit that needs no store still saves, one that types a password stops.
    Scratch s;
    s.write("grab.conf", grab_conf);
    std::filesystem::create_directories(s.store());
    auto agent = server(Auth::agent, "agent");
    agent.host = "h.example.com";
    agent.port = 22;
    agent.user = "me";
    agent.max_depth = 7;
    const auto edited = server_ops::update_server(s.env(), agent, std::nullopt, std::nullopt);
    REQUIRE_MESSAGE(edited.has_value(), edited.error_or(""));
    CHECK(s.read("grab.conf").find("max_depth = 7") != std::string::npos);
    CHECK_FALSE(server_ops::update_server(s.env(), server(Auth::password), std::string("fake-pw"), std::nullopt));
}

TEST_CASE("a removal moves an undecryptable store aside too, and goes on") {
    Scratch s;
    s.write("grab.conf", grab_conf);
    std::filesystem::create_directories(s.store().parent_path());
    std::ofstream(s.store(), std::ios::binary) << "grab-passwords-sealed 1\nanother account's";
    auto removed = server_ops::remove_server(s.env(), "box");
    REQUIRE(removed.has_value());
    CHECK(removed->find("moved aside to ") != std::string::npos);
    CHECK(removed->find("password of box") == std::string::npos); // it is gone
    CHECK_FALSE(std::filesystem::exists(s.store()));
}
#endif

TEST_CASE("the grab.conf grab writes still loads in grab 0.19's reader, rclone_remote kept") {
    // 0.19 reads grab.conf with the same parse_grab_config, which takes keys by name: the new ones
    // are ignored, and an imported section finds its old remote through rclone_remote.
    Scratch s;
    s.write("grab.conf", grab_conf);
    REQUIRE(server_ops::add_server(s.env(), server(Auth::agent, "fresh"), std::nullopt, box_keys));
    auto doc = ini::parse(s.read("grab.conf"));
    REQUIRE(doc);
    auto cfg = parse_grab_config(*doc);
    REQUIRE(cfg);
    REQUIRE(cfg->remotes.size() == 3);
    CHECK(cfg->remotes[0].rclone_remote == "box");
    CHECK(cfg->remotes[2].name == "fresh");
    CHECK(cfg->remotes[2].rclone_remote == "fresh");
    CHECK(cfg->remotes[2].max_depth == 4);
    CHECK(cfg->remotes[2].common_flags == util::split_args(default_common_flags));
}

TEST_CASE("server ops: a test stands on grab's login") {
    server_ops::TestResult t;
    CHECK_FALSE(t.ok()); // nothing tested yet
    t.grab_ok = true;
    CHECK(t.ok());
    t.grab_ok = false;
    CHECK_FALSE(t.ok());
    server_ops::TestResult guarded;
    guarded.unusable = "[Box] connects through a SOCKS proxy (rclone.socks_proxy), ...";
    CHECK_FALSE(guarded.ok());
}

TEST_CASE("server ops: Test is grab's login only, whatever `engine` says; a guarded or unpinned server fails before anything dials") {
    // Every server fails without the network: grab's login binds to TEST-NET's 192.0.2.1, which
    // this PC hasn't got; the old `rclone =` names a program that isn't there and isn't asked.
    Scratch s;
    s.write("grab.conf", "[grab]\nrclone = " + util::path_to_utf8(s.dir / "no-rclone.exe") +
                             "\n\n[g]\nhost = 127.0.0.1\nport = 1\nuser = me\nauth = agent\nengine = grab\n"
                             "common_flags = --bind 192.0.2.1\n"
                             "\n[r]\nhost = 127.0.0.1\nport = 1\nuser = me\nauth = agent\nengine = rclone\nfind = rclone\n"
                             "common_flags = --bind 192.0.2.1\n"
                             "\n[u]\nhost = 127.0.0.1\nport = 2\nuser = me\nauth = agent\n"
                             "\n[p]\nhost = 127.0.0.1\nport = 2\nuser = me\nauth = agent\n"
                             "common_flags = --sftp-socks-proxy fake-user:fake-pass@127.0.0.1:1080\n");
    Scratch::pins("[127.0.0.1]:1 ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOMqqnkVzrm0SdG6UOoqKLsabgH5C9okWi0dh2l9GKJl\n");
    const auto env = s.env();
    for (const char* name : {"g", "r"}) {
        const auto t = server_ops::test_connection(env, name);
        REQUIRE(t.has_value());
        CHECK(t->grab_ok == false);
        CHECK(t->grab_error.find("--bind") != std::string::npos);
        CHECK(t->grab_error.find("engine = rclone") == std::string::npos);
        CHECK(t->unusable.empty());
        CHECK_FALSE(t->ok());
        CHECK(server_ops::test_lines(*t) == std::vector<std::string>{"grab: FAILED: " + t->grab_error});
    }
    // [u]: no key for [127.0.0.1]:2: Trust, nothing dialled.
    const auto u = server_ops::test_connection(env, "u");
    REQUIRE(u.has_value());
    CHECK(u->grab_error.find("host key not trusted: use Trust") != std::string::npos);
    CHECK_FALSE(u->ok());
    // [p]: a proxy grab can't use, and not pinned either: the reason, which says how to clear it
    // (Trust would be refused too).
    const auto p = server_ops::test_connection(env, "p");
    REQUIRE(p.has_value());
    CHECK_FALSE(p->grab_ok.has_value()); // nothing tried
    CHECK(p->unusable.starts_with("[p] connects through a SOCKS proxy (common_flags' --sftp-socks-proxy)"));
    CHECK(p->unusable.find("fake-") == std::string::npos);
    CHECK_FALSE(p->ok());
    CHECK(server_ops::test_lines(*p) == std::vector<std::string>{"not usable: " + p->unusable});
    // A section without details can't be tested.
    s.write("grab.conf", "[grab]\n\n[bare]\nmax_depth = 2\n");
    CHECK_FALSE(server_ops::test_connection(s.env(), "bare").has_value());
}

TEST_CASE("server list says whether each login's secret is saved for that very login, never the value (phase 7)") {
    Scratch s;
    s.write("grab.conf", grab_conf + "\n[keyed]\nhost = k.example\nuser = k\nauth = key\nkey_file = C:\\k\\id\n"
                                     "\n[pem]\nhost = p.example\nuser = p\nauth = stored_key\n"
                                     "\n[moved]\nhost = m.example\nport = 2222\nuser = m\nauth = password\n");
    REQUIRE(passwords::save(s.store(), {{"box", {{"password", fake("fake-pw")}}},
                                        {"keyed", {{"passphrase", fake("fake-pass", "k", "k.example", 22)}}},
                                        {"moved", {{"password", fake("fake-old", "m", "m.example", 22)}}}}));
    auto env = s.env();
    auto list = server_ops::list_servers(env);
    REQUIRE(list.size() == 5);
    CHECK(list[0].secret_saved == true);           // box: its password, for u1@box.example.com:23
    CHECK_FALSE(list[1].secret_saved.has_value()); // agent: nothing to save
    CHECK(list[2].secret_saved == true);           // keyed: its passphrase
    CHECK(list[3].secret_saved == false);          // pem: no key in the store
    CHECK(list[4].secret_saved == false);          // moved: saved for port 22, not 2222
    CHECK(server_ops::login_text(list[0]) == "password saved");
    CHECK(server_ops::login_text(list[1]) == "ssh-agent");
    CHECK(server_ops::login_text(list[2]) == "key file, passphrase saved");
    CHECK(server_ops::login_text(list[3]) == "no key in grab's store (grab server edit pem)");
    CHECK(server_ops::login_text(list[4]) == "no password saved (grab server edit moved)");
    CHECK(server_ops::host_key_text(list[0]) == "host key not trusted (grab server trust box)");
    Scratch::pins("[box.example.com]:23 ssh-ed25519 BOX\n");
    CHECK(server_ops::host_key_text(server_ops::list_servers(env)[0]) == "host key pinned");
    for (const auto& info : list) {
        CHECK(server_ops::login_text(info).find("fake-") == std::string::npos);
        CHECK(info.store_error.empty());
    }
#ifdef _WIN32
    // A store that can't be read: unknown, and why, for the servers that need one.
    std::ofstream(s.store(), std::ios::binary) << "grab-passwords-sealed 1\nanother account's";
    list = server_ops::list_servers(env);
    CHECK_FALSE(list[0].secret_saved.has_value());
    CHECK_FALSE(list[0].store_error.empty());
    CHECK(server_ops::login_text(list[0]).starts_with("password (grab's saved passwords can't be read: "));
    // ssh-agent needs none to log in, but the store may hold a route: the guard counts it as one,
    // so Trust is refused until an Edit sets the store aside.
    CHECK_FALSE(list[1].store_error.empty());
    CHECK(server_ops::login_text(list[1]) == "ssh-agent");
    REQUIRE(list[1].guard.size() == 1);
    CHECK(list[1].guard[0].where == "grab's store, which can't be read");
    CHECK_FALSE(server_ops::reach_for(env, list[1].name).has_value());
    // So an edit can't keep "the saved one".
    CHECK(server_ops::edit_needs_password(list[0], server(Auth::password)));
#endif
}

TEST_CASE("an edit starts from the server's answers; a section without details from blank ones") {
    Scratch s;
    s.write("grab.conf", grab_conf + "max_connections = 6\n\n[bare]\nsearch_roots = ~, /srv\nmax_depth = 2\n");
    const auto list = server_ops::list_servers(s.env());
    REQUIRE(list.size() == 3);
    const auto box = server_ops::edit_defaults(list[0]);
    CHECK(box.name == "box");
    CHECK(box.host == "box.example.com");
    CHECK(box.port == 23);
    CHECK(box.user == "u1");
    CHECK(box.auth == Auth::password);
    CHECK(box.search_roots == std::vector<std::string>{"/home/movies"});
    CHECK(box.max_depth == 4);
    const auto agent = server_ops::edit_defaults(list[1]);
    CHECK(agent.auth == Auth::agent);
    CHECK(agent.max_depth == 6);
    CHECK(agent.max_connections == 6);
    const auto bare = server_ops::edit_defaults(list[2]);
    CHECK(bare.name == "bare");
    CHECK(bare.host.empty());
    CHECK(bare.user.empty());
    CHECK(bare.port == 22);
    CHECK(bare.search_roots == std::vector<std::string>{"", "/srv"}); // "~" is the login home
    CHECK(bare.max_depth == 2);
    // Its edit asks for the host key and a password, whatever is typed.
    auto wanted = bare;
    wanted.host = "box.example.com";
    wanted.user = "u1";
    CHECK(server_ops::edit_needs_host_key(list[2], wanted));
    CHECK(server_ops::edit_needs_password(list[2], wanted));
}

TEST_CASE("a test's lines as the command prints them: one grab line, or why the server isn't usable") {
    server_ops::TestResult t;
    t.grab_ok = true;
    t.grab_search = "with find (GNU find)";
    t.checksums = "checksums: md5sum";
    CHECK(server_ops::test_lines(t) ==
          std::vector<std::string>{"grab: OK, home folder listed, searches with find (GNU find), checksums: md5sum"});
    t.checksums = "verify can't run: no md5sum";
    CHECK(server_ops::test_lines(t) == std::vector<std::string>{"grab: OK, home folder listed, searches with find (GNU "
                                                                "find), verify can't run: no md5sum"});
    t.grab_ok = false;
    t.grab_error = "Box: host key not trusted: use Trust";
    CHECK(server_ops::test_lines(t) == std::vector<std::string>{"grab: FAILED: Box: host key not trusted: use Trust"});
    server_ops::TestResult g;
    g.unusable = "[Box] needs one read at a time (rclone.disable_concurrent_reads), which grab can't, ...";
    CHECK(server_ops::test_lines(g) == std::vector<std::string>{"not usable: " + g.unusable});
    for (const auto& line : server_ops::test_lines(t)) {
        CHECK(line.find("rclone (") == std::string::npos);
        CHECK(line.find("ssh (") == std::string::npos);
    }
}

TEST_CASE("the window's notice after an import names the servers moved and what each still needs") {
    auto info = [](std::string name, bool pinned, Auth auth, std::optional<bool> saved, std::string error = {}) {
        server_ops::ServerInfo s;
        s.name = std::move(name);
        s.host_key_pinned = pinned;
        s.auth = auth;
        s.secret_saved = saved;
        s.error = std::move(error);
        return s;
    };
    const std::string georgian = "\xE1\x83\x91\xE1\x83\x9D\xE1\x83\xA5\xE1\x83\xA1\xE1\x83\x98"; // ბოქსი
    const std::vector<server_ops::ServerInfo> servers{
        info("Main", true, Auth::agent, std::nullopt), info("Box", false, Auth::password, true),
        info(georgian, true, Auth::password, false), info("old", false, Auth::password, std::nullopt, "[old] has no details")};
    const auto text = server_ops::import_banner(
        {"Main", "Box", georgian, "gone"}, servers,
        {"[Box] its host key isn't trusted yet: use Trust (grab server trust Box)",
         "[" + georgian + "] rclone asked for its password each time (ask_password), which grab can't do in the "
                          "background: enter it with Edit",
         "[Main] key_file came from [main], as rclone reads [main.x]", "[Late] not imported: there is no rclone remote [Late]"});
    const auto lines = util::split(text, '\n');
    REQUIRE(lines.size() == 5);
    CHECK(lines[0] == "Servers are kept in grab.conf now: the connection details of Main, Box, " + georgian +
                          " moved there from rclone.conf, which stays as it was (grab no longer writes it). Their "
                          "passwords are in grab's own encrypted store.");
    CHECK(lines[1] == "Box: its host key isn't trusted yet: use Trust in Settings, which shows its fingerprints to compare.");
    CHECK(lines[2] == georgian + ": no password is saved for it: enter it with Edit.");
    // The import's notes the lines above don't already say ("gone" was removed since).
    CHECK(lines[3] == "[Main] key_file came from [main], as rclone reads [main.x]");
    CHECK(lines[4] == "[Late] not imported: there is no rclone remote [Late]");
    CHECK(text.find("gone") == std::string::npos);
    // Nothing moved that is still there, and no notes: nothing to say.
    CHECK(server_ops::import_banner({"gone"}, servers, {}).empty());
    CHECK(server_ops::import_banner({}, servers, {}).empty());
    // A section without details says so.
    CHECK(server_ops::import_banner({"old"}, servers, {}).find("\n[old] has no details") != std::string::npos);
    // Only ssh-agent servers moved: no password to speak of.
    CHECK(server_ops::import_banner({"Main"}, servers, {}) ==
          "Servers are kept in grab.conf now: the connection details of Main moved there from rclone.conf, which stays "
          "as it was (grab no longer writes it).");
}
