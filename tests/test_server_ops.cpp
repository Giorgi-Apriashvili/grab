// server_ops: grab's servers in grab.conf, their passwords in grab's store and their host keys in
// grab's known_hosts, without rclone (RCLONE-REPLACEMENT.md, phase 7 steps 8 and 9). Every file
// here is made up, in a temp folder or the test runner's scratch APPDATA (test_main.cpp), and
// every secret is an obviously fake one.

#include "browse.hpp"
#include "config.hpp"
#include "engine.hpp"
#include "file_list.hpp"
#include "guard.hpp"
#include "gui_state.hpp"
#include "ini.hpp"
#include "notices.hpp"
#include "passwords.hpp"
#include "proxy.hpp"
#include "scoped_env.hpp"
#include "server_conf.hpp"
#include "server_ops.hpp"
#include "server_prompt.hpp"
#include "servers.hpp"
#include "ssh.hpp"
#include "ssh_pool.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#ifdef _WIN32
#include "fake_proxy.hpp"

#include <windows.h> // IWYU pragma: keep (LockFileEx, SetFileAttributesW)
#endif

#include <algorithm>
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
                                     "rclone.disable_concurrent_reads = true\n"
                                     "\n[moved]\nhost = m.example\nuser = m\nauth = agent\n");
    REQUIRE(passwords::save(s.store(), {{"box", {{"password", fake("fake-pw")},
                                                 {"rclone.socks_proxy", fake("fake-user:fake-pass@127.0.0.1:9")}}},
                                        {"moved", {{"rclone.http_proxy", fake("http://p:1", "someone-else", "m.example", 22)}}},
                                        {"routed", {{"rclone.ssh", fake("ssh fake-jump", "r", "r.example", 22)}}}}));
    const auto env = s.env();
    const auto list = server_ops::list_servers(env);
    REQUIRE(list.size() == 7);
    // box: the proxy its store keeps for its login is its route again (PROXIES.md decision 4), no
    // reason; moved: one saved for another login refuses it (0.21-0.25 skipped it and connected
    // directly).
    CHECK(list[0].guard.empty());
    CHECK(list[0].proxy_from == guard::ProxyFrom::kept);
    CHECK(list[0].proxy == proxy::Proxy{.kind = proxy::Kind::socks5, .host = "127.0.0.1", .port = 9, .user = "fake-user",
                                        .password = {}});
    CHECK(list[1].guard.empty());
    CHECK(list[1].proxy_from == guard::ProxyFrom::none);
    CHECK(list[1].search == "probe");
    REQUIRE(list[6].guard.size() == 1);
    CHECK(list[6].guard[0].where == "rclone.http_proxy in grab's store, saved for another login");
    // An agent server routed by its store's ssh command: searched as any other, never by rclone.
    REQUIRE(list[3].guard.size() == 1);
    CHECK(list[3].search == "probe");
    // ssh_options' jump host is no reason (decision 4); a quirk switch in the section is.
    CHECK(list[4].guard.empty());
    REQUIRE(list[5].guard.size() == 1);
    CHECK(list[5].guard[0].where == "rclone.disable_concurrent_reads");
    // Why grab doesn't connect to one, as Test and the window's card say it: the guard's reason, or
    // a bind left in common_flags with no `bind` key (phase 9 step 7).
    CHECK(list[0].unusable.empty());
    CHECK(list[1].unusable.empty());
    CHECK(list[2].unusable.starts_with("[plain] common_flags' --bind 10.0.0.7 isn't read any more: write bind = 10.0.0.7"));
    CHECK(list[4].unusable.empty());
    CHECK(list[5].unusable.starts_with("[quirky] needs one read at a time (rclone.disable_concurrent_reads)"));
    CHECK(list[6].unusable ==
          "[moved] has an HTTP proxy (rclone.http_proxy in grab's store, saved for another login), so grab doesn't "
          "connect to moved rather than go around it: Edit moved and set its proxy again, or choose Directly");
    CHECK(list[1].connection_keys == "bind 10.0.0.2, connect_timeout 1500ms, io_timeout 120s");
    CHECK(list[0].connection_keys.empty());
    // Trust and the host-key fetch of an Edit: refused for a guarded server, naming the setting
    // and how to clear it, never its value, before anything connects.
    for (const char* name : {"routed", "quirky", "moved"}) {
        const auto refused = server_ops::reach_for(env, name);
        REQUIRE_FALSE(refused);
        CHECK(refused.error().starts_with(std::format("[{}] ", name)));
        CHECK(refused.error().find("fake-") == std::string::npos);
        CHECK(refused.error().find("Connect directly") == std::string::npos);
    }
    CHECK(server_ops::reach_for(env, "routed").error().ends_with(
        "the import kept rclone.ssh in grab's store: remove routed and add it again (Remove and Add in grab's "
        "Settings, or `grab server remove routed` and `grab server add`)"));
    CHECK(server_ops::reach_for(env, "jumped").has_value());
    // Trust goes through box's kept proxy, its password with it, 2 min to connect (none written).
    const auto box = server_ops::reach_for(env, "box");
    REQUIRE(box);
    CHECK(box->proxy == proxy::Proxy{.kind = proxy::Kind::socks5, .host = "127.0.0.1", .port = 9, .user = "fake-user",
                                     .password = "fake-pass"});
    CHECK(box->connect_timeout == engine::proxy_connect_timeout);
    // The host-key fetch connects from the server's bind, within its connect time; a new server
    // has none. A bind left in common_flags with no `bind` key refuses it, rather than fetch from
    // another address (phase 9 step 7).
    const auto agent = server_ops::reach_for(env, "agent");
    REQUIRE(agent);
    CHECK(agent->bind == "10.0.0.2");
    CHECK(agent->bind_from == "bind in [agent]");
    CHECK(agent->connect_timeout == std::chrono::milliseconds(1'500));
    CHECK(agent->proxy.kind == proxy::Kind::none);
    CHECK(server_ops::reach_for(env, "plain").error_or("") ==
          "[plain] common_flags' --bind 10.0.0.7 isn't read any more: write bind = 10.0.0.7 in [plain] (grab config) and "
          "take it out of common_flags. Until then grab doesn't connect to plain rather than connect from another address");
    CHECK_FALSE(server_ops::reach_for(env, "plain", server_ops::Purpose::edit));
    const auto none = server_ops::reach_for(env, "new");
    REQUIRE(none);
    CHECK(none->bind.empty());
    CHECK(none->proxy.kind == proxy::Kind::none);
    {
        // An inherited RCLONE_SFTP_SOCKS_PROXY covers every server, a new one's host-key fetch too.
        const test::ScopedEnv proxy("RCLONE_SFTP_SOCKS_PROXY", "fake-user:fake-pass@127.0.0.1:1080");
        for (const char* name : {"new", "agent", "box"}) {
            const auto refused = server_ops::reach_for(env, name);
            REQUIRE_FALSE(refused);
            CHECK(refused.error().find("clear RCLONE_SFTP_SOCKS_PROXY for grab") != std::string::npos);
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
    CHECK(server_ops::name_taken(env, "x").has_value()); // one letter: a drive to grab 0.20 and earlier (G169)
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
    CHECK(edited->find("with any proxy or ssh command a server kept there (such a server now connects directly, or "
                       "through the proxy its grab.conf section gives it, whose password is typed again with Edit)") !=
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
    // Details that can't be read: the section's own rclone.* line stays through the save, so the
    // fetch is refused too; the store's proxy stays too, so the fetch goes through it.
    s.write("grab.conf", "[grab]\ndefault_remote = box\n\n[box]\nhost = box.example.com\nport = abc\nuser = u1\n"
                         "auth = password\nrclone.disable_concurrent_reads = true\n"
                         "\n[stored]\nhost = s.example.com\nport = abc\nuser = s\nauth = agent\n"
                         "\n[agent]\nhost = h.example.com\nuser = me\nauth = agent\n"
                         "\n[two]\nhost = t.example.com\nuser = t\nauth = agent\n");
    REQUIRE(passwords::save(s.store(),
                            {{"stored", {{"rclone.socks_proxy", fake("fake-user:fake-pass@127.0.0.1:9", "s", "s.example.com", 22)}}},
                             {"agent", {{"rclone.http_proxy", fake("http://p:1", "someone-else", "h.example.com", 22)}}},
                             {"two", {{"rclone.http_proxy", fake("http://p:1", "t", "t.example.com", 22)},
                                      {"rclone.socks_proxy", fake("q:2", "t", "t.example.com", 22)}}}}));
    const auto env = s.env();
    const auto box = server_ops::reach_for(env, "box", server_ops::Purpose::edit);
    REQUIRE_FALSE(box.has_value());
    CHECK(box.error().find("rclone.disable_concurrent_reads") != std::string::npos);
    const auto stored = server_ops::reach_for(env, "stored", server_ops::Purpose::edit);
    REQUIRE_MESSAGE(stored.has_value(), stored.error_or(""));
    CHECK(stored->proxy == proxy::Proxy{.kind = proxy::Kind::socks5, .host = "127.0.0.1", .port = 9, .user = "fake-user",
                                        .password = "fake-pass"});
    CHECK(stored->connect_timeout == engine::proxy_connect_timeout);
    // A proxy kept for another login refuses Trust (0.21-0.25 skipped it and fetched directly);
    // an Edit binds it to the new login, so its fetch goes through it.
    const auto trust = server_ops::reach_for(env, "agent");
    REQUIRE_FALSE(trust.has_value());
    CHECK(trust.error().find("rclone.http_proxy in grab's store, saved for another login") != std::string::npos);
    const auto agent = server_ops::reach_for(env, "agent", server_ops::Purpose::edit);
    REQUIRE_MESSAGE(agent.has_value(), agent.error_or(""));
    CHECK(agent->proxy == proxy::Proxy{.kind = proxy::Kind::http, .host = "p", .port = 1, .user = {}, .password = {}});
    // Two kept: refused for both.
    for (const auto purpose : {server_ops::Purpose::trust, server_ops::Purpose::edit}) {
        const auto two = server_ops::reach_for(env, "two", purpose);
        REQUIRE_FALSE(two.has_value());
        CHECK(two.error().find("has both a SOCKS and an HTTP proxy") != std::string::npos);
    }
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

TEST_CASE("server ops: Test is grab's login only, whatever `engine` says; a guarded, bound or unpinned server fails before anything dials") {
    // Every server fails without the network: grab's login binds to TEST-NET's 192.0.2.1, which
    // this PC hasn't got; the old `rclone =` names a program that isn't there and isn't asked.
    Scratch s;
    s.write("grab.conf", "[grab]\nrclone = " + util::path_to_utf8(s.dir / "no-rclone.exe") +
                             "\n\n[g]\nhost = 127.0.0.1\nport = 1\nuser = me\nauth = agent\nengine = grab\n"
                             "bind = 192.0.2.1\n"
                             "\n[r]\nhost = 127.0.0.1\nport = 1\nuser = me\nauth = agent\nengine = rclone\nfind = rclone\n"
                             "bind = 192.0.2.1\ncommon_flags = --bind 10.0.0.9\n"
                             "\n[b]\nhost = 127.0.0.1\nport = 1\nuser = me\nauth = agent\n"
                             "ssh_options = -o BindAddress=192.0.2.1\n"
                             "\n[u]\nhost = 127.0.0.1\nport = 2\nuser = me\nauth = agent\n"
                             "\n[p]\nhost = 127.0.0.1\nport = 2\nuser = me\nauth = agent\n"
                             "common_flags = --sftp-socks-proxy fake-user:fake-pass@127.0.0.1:1080\n");
    Scratch::pins("[127.0.0.1]:1 ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOMqqnkVzrm0SdG6UOoqKLsabgH5C9okWi0dh2l9GKJl\n");
    const auto env = s.env();
    for (const char* name : {"g", "r"}) {
        const auto t = server_ops::test_connection(env, name);
        REQUIRE(t.has_value());
        CHECK(t->grab_ok == false);
        CHECK(t->grab_error.find("bind in [") != std::string::npos);
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
    CHECK(p->unusable.starts_with("[p] has a SOCKS proxy where grab doesn't read one (common_flags' --sftp-socks-proxy)"));
    CHECK(p->unusable.find("fake-") == std::string::npos);
    CHECK_FALSE(p->ok());
    CHECK(server_ops::test_lines(*p) == std::vector<std::string>{"not usable: " + p->unusable});
    // [b]: a bind left in ssh_options with no `bind` key: the refusal, nothing tried.
    const auto b = server_ops::test_connection(env, "b");
    REQUIRE(b.has_value());
    CHECK_FALSE(b->grab_ok.has_value());
    CHECK(b->unusable.starts_with("[b] ssh_options' -o BindAddress=192.0.2.1 isn't read any more: write bind = 192.0.2.1"));
    CHECK(server_ops::test_lines(*b) == std::vector<std::string>{"not usable: " + b->unusable});
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
                          " moved there from rclone.conf. Their passwords are in grab's own encrypted store.");
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
          "Servers are kept in grab.conf now: the connection details of Main moved there from rclone.conf.");
}

// ---- proxies (PROXIES.md, chunk 1a) -----------------------------------------------------------

namespace {

passwords::Binding box_login() { return passwords::Binding{.user = "u1", .host = "box.example.com", .port = 23}; }

proxy::Proxy socks(int port, std::string user = {}, std::string password = {}) {
    return proxy::Proxy{.kind = proxy::Kind::socks5, .host = "127.0.0.1", .port = port, .user = std::move(user),
                        .password = std::move(password)};
}

passwords::Secret bound(const proxy::Proxy& p, std::string value) {
    return passwords::Secret{.binding = guard::proxy_login(p), .value = std::move(value)};
}

} // namespace

TEST_CASE("proxies: Add writes the proxy line, its password bound to the proxy, and the copy older grabs refuse for") {
    Scratch s;
    s.write("grab.conf", grab_conf);
    auto add = server(Auth::agent, "px");
    add.proxy = socks(9050, "alice", "fake-proxy-pw");
    const auto added = server_ops::add_server(s.env(), add, std::nullopt, box_keys);
    REQUIRE_MESSAGE(added.has_value(), added.error_or(""));
    const auto text = s.read("grab.conf");
    CHECK(text.find("[px]\nhost = box.example.com\nport = 23\nuser = u1\nauth = agent\nproxy = socks5://alice@127.0.0.1:9050\n") !=
          std::string::npos);
    CHECK(text.find("fake-") == std::string::npos);
    // The password bound to the proxy; the copy in rclone's form bound to the server's login, as
    // the import kept one, which 0.21-0.25's guard finds for that login and refuses the server for.
    CHECK(s.secrets() == passwords::Store{{"px", {{"proxy_password", bound(add.proxy.value(), "fake-proxy-pw")},
                                                  {"rclone.socks_proxy",
                                                   passwords::Secret{.binding = box_login(),
                                                                     .value = "alice:fake-proxy-pw@127.0.0.1:9050"}}}}});
    CHECK(guard::store_extras(s.secrets(), "px", box_login()) ==
          std::vector<std::pair<std::string, std::string>>{{"socks_proxy", "alice:fake-proxy-pw@127.0.0.1:9050"}});
    // This grab goes through the line, with its password, wherever it builds a Target.
    const auto ctx = engine::load_context(s.conf, std::string("px"), false);
    REQUIRE_MESSAGE(ctx.has_value(), ctx.error_or(""));
    CHECK(ctx->connection.proxy == socks(9050, "alice"));
    const auto target = engine::ssh_target(*ctx);
    REQUIRE_MESSAGE(target.has_value(), target.error_or(""));
    CHECK(target->proxy == socks(9050, "alice", "fake-proxy-pw"));
    CHECK(target->connect_timeout == engine::proxy_connect_timeout);
    const auto listed = server_ops::list_servers(s.env());
    const auto px = std::ranges::find(listed, "px", &server_ops::ServerInfo::name);
    REQUIRE(px != listed.end());
    CHECK(px->proxy == socks(9050, "alice"));
    CHECK(px->proxy_from == guard::ProxyFrom::line);
    CHECK(px->unusable.empty());

    // Without a user nothing is kept but the copy; an HTTP proxy's copy is rclone's URL.
    auto plain = server(Auth::agent, "web");
    plain.proxy = proxy::Proxy{.kind = proxy::Kind::http, .host = "proxy.example", .port = 3128, .user = {},
                               .password = "fake-ignored"};
    REQUIRE(server_ops::add_server(s.env(), plain, std::nullopt, box_keys));
    CHECK(s.secrets().at("web") ==
          std::map<std::string, passwords::Secret, std::less<>>{
              {"rclone.http_proxy", passwords::Secret{.binding = box_login(), .value = "http://proxy.example:3128"}}});
    // Directly (or nothing said): no line, nothing kept.
    auto direct = server(Auth::agent, "direct");
    direct.proxy = proxy::Proxy{};
    REQUIRE(server_ops::add_server(s.env(), direct, std::nullopt, box_keys));
    CHECK_FALSE(s.secrets().contains("direct"));
    const auto doc = ini::parse(s.read("grab.conf"));
    REQUIRE(doc.has_value());
    REQUIRE(doc->find("direct") != nullptr);
    CHECK_FALSE(doc->find("direct")->get("proxy").has_value());
    // Removed: its copy goes with the rest.
    REQUIRE(server_ops::remove_server(s.env(), "px"));
    CHECK_FALSE(s.secrets().contains("px"));
}

TEST_CASE("proxies: an Edit keeps the proxy unless it sets one; its password stays the proxy's, the copy follows the login") {
    Scratch s;
    s.write("grab.conf", grab_conf);
    auto add = server(Auth::agent, "px");
    add.proxy = socks(9050, "alice", "fake-proxy-pw");
    REQUIRE(server_ops::add_server(s.env(), add, std::nullopt, box_keys));

    // The window's Edit and `grab server edit` don't ask for it yet: it stays, store untouched.
    auto edit = server(Auth::agent, "px");
    edit.max_depth = 7;
    const auto sealed = s.store_bytes();
    REQUIRE(server_ops::update_server(s.env(), edit, std::nullopt, std::nullopt));
    CHECK(s.read("grab.conf").find("proxy = socks5://alice@127.0.0.1:9050\n") != std::string::npos);
    CHECK(s.store_bytes() == sealed);

    // The server's login moves: the proxy's password stays bound to the proxy (not rebound), the
    // copy for older grabs moves with the server.
    edit.port = 24;
    const server_ops::HostKeys keys24{.lines = {"[box.example.com]:24 ssh-ed25519 BOX"}, .fingerprints = {}};
    REQUIRE(server_ops::update_server(s.env(), edit, std::nullopt, keys24));
    const passwords::Binding moved{.user = "u1", .host = "box.example.com", .port = 24};
    CHECK(s.secrets() == passwords::Store{{"px", {{"proxy_password", bound(socks(9050, "alice"), "fake-proxy-pw")},
                                                  {"rclone.socks_proxy",
                                                   passwords::Secret{.binding = moved,
                                                                     .value = "alice:fake-proxy-pw@127.0.0.1:9050"}}}}});

    // The same proxy and user, nothing typed: the saved password stays.
    edit.proxy = socks(9050, "alice");
    REQUIRE(server_ops::update_server(s.env(), edit, std::nullopt, std::nullopt));
    CHECK(s.secrets().at("px").at("proxy_password").value == "fake-proxy-pw");
    // Another port (or user), nothing typed: the password typed for the old one goes.
    edit.proxy = socks(9051, "alice");
    REQUIRE(server_ops::update_server(s.env(), edit, std::nullopt, std::nullopt));
    CHECK(s.read("grab.conf").find("proxy = socks5://alice@127.0.0.1:9051\n") != std::string::npos);
    CHECK(s.secrets() == passwords::Store{{"px", {{"rclone.socks_proxy",
                                                   passwords::Secret{.binding = moved, .value = "alice@127.0.0.1:9051"}}}}});
    // Typed: saved for that proxy.
    edit.proxy = socks(9051, "alice", "fake-new-pw");
    REQUIRE(server_ops::update_server(s.env(), edit, std::nullopt, std::nullopt));
    CHECK(s.secrets().at("px").at("proxy_password") == bound(socks(9051, "alice"), "fake-new-pw"));
    // An HTTP proxy: its own copy, the SOCKS one gone; the password never in grab.conf.
    edit.proxy = proxy::Proxy{.kind = proxy::Kind::http, .host = "proxy.example", .port = 3128, .user = "bob",
                              .password = "fake-b@d"};
    REQUIRE(server_ops::update_server(s.env(), edit, std::nullopt, std::nullopt));
    CHECK(s.read("grab.conf").find("proxy = http://bob@proxy.example:3128\n") != std::string::npos);
    CHECK(s.read("grab.conf").find("fake-") == std::string::npos);
    const auto store = s.secrets();
    CHECK_FALSE(store.at("px").contains("rclone.socks_proxy"));
    CHECK(store.at("px").at("rclone.http_proxy") ==
          passwords::Secret{.binding = moved, .value = "http://bob:fake-b%40d@proxy.example:3128"});
    // Directly: the line, the password and the copy all go.
    edit.proxy = proxy::Proxy{};
    REQUIRE(server_ops::update_server(s.env(), edit, std::nullopt, std::nullopt));
    CHECK(s.read("grab.conf").find("proxy = ") == std::string::npos); // "proxy =", blank: directly
    CHECK_FALSE(s.secrets().contains("px"));
    // A check like the form's: a SOCKS5 password too long is refused before anything is written.
    edit.proxy = socks(9050, "alice", std::string(256, 'x'));
    CHECK(server_ops::check_server(edit).value_or(server_ops::Problem{}).field == "proxySecret");
    edit.proxy = socks(0);
    CHECK(server_ops::check_server(edit).value_or(server_ops::Problem{}).field == "proxy");
    edit.proxy = socks(1080);
    edit.host = "fe80::1%eth0";
    CHECK(server_ops::check_server(edit).value_or(server_ops::Problem{}).field == "host");
}

TEST_CASE("proxies: an Edit moves a proxy kept from rclone into the line (3.7); one grab can't read stops a keeping Edit") {
    Scratch s;
    s.write("grab.conf", grab_conf + "\n[bad]\nhost = b.example\nuser = b\nauth = agent\nproxy = socks5://127.0.0.1\n");
    REQUIRE(passwords::save(s.store(), {{"box", {{"password", fake("fake-pw")},
                                                 {"rclone.socks_proxy", fake("fake-user:fake-pass@127.0.0.1:9")}}}}));
    // Until then it goes through the kept one (decision 4).
    auto box_list = server_ops::list_servers(s.env());
    CHECK(box_list[0].proxy_from == guard::ProxyFrom::kept);
    auto wanted = server(Auth::password); // nothing typed, nothing else changed
    REQUIRE(server_ops::update_server(s.env(), wanted, std::nullopt, std::nullopt));
    CHECK(s.read("grab.conf").find("auth = password\nproxy = socks5://fake-user@127.0.0.1:9\nshell_type = unix\n") !=
          std::string::npos);
    CHECK(s.read("grab.conf").find("fake-pass") == std::string::npos);
    CHECK(s.secrets() ==
          passwords::Store{{"box", {{"password", fake("fake-pw")},
                                    {"proxy_password", bound(socks(9, "fake-user"), "fake-pass")},
                                    {"rclone.socks_proxy", fake("fake-user:fake-pass@127.0.0.1:9")}}}});
    box_list = server_ops::list_servers(s.env());
    CHECK(box_list[0].proxy_from == guard::ProxyFrom::line);
    CHECK(box_list[0].proxy == socks(9, "fake-user"));
    // The decision-4 notice is done with: the line says it now.
    const auto n = notices::for_config(s.conf);
    REQUIRE(n.has_value());
    CHECK(std::ranges::none_of(*n, [](const notices::Notice& x) { return x.key == "proxy:kept"; }));

    // A line grab can't read: an Edit that would keep it stops, rather than blank it (the server
    // would connect directly); one that sets the proxy goes.
    const auto before = s.read("grab.conf");
    auto bad = server(Auth::agent, "bad");
    bad.host = "b.example";
    bad.port = 22;
    bad.user = "b";
    const auto refused = server_ops::update_server(s.env(), bad, std::nullopt, std::nullopt);
    REQUIRE_FALSE(refused);
    CHECK(refused.error() ==
          "[bad] proxy: give the proxy's port (socks5://host:1080); correct the proxy line in [bad] (grab config) first");
    CHECK(s.read("grab.conf") == before);
    CHECK_FALSE(server_ops::reach_for(s.env(), "bad", server_ops::Purpose::edit));
    CHECK_FALSE(server_ops::reach_for(s.env(), "bad"));
    bad.proxy = socks(1080);
    REQUIRE(server_ops::update_server(s.env(), bad, std::nullopt, std::nullopt));
    CHECK(s.read("grab.conf").find("auth = agent\nproxy = socks5://127.0.0.1:1080\n") != std::string::npos);
}

TEST_CASE("proxies: an Edit that moves the login rebinds the kept proxies with it, never the proxy password") {
    // Two kept (the server stays refused until one is chosen): nothing is moved into the line, and
    // what the store keeps follows the server, but the proxy's password stays the proxy's.
    Scratch s;
    s.write("grab.conf", grab_conf);
    REQUIRE(passwords::save(s.store(), {{"agent", {{"proxy_password", bound(socks(9050, "alice"), "fake-proxy-pw")},
                                                   {"rclone.socks_proxy", fake("127.0.0.1:9050", "me", "h.example.com", 22)},
                                                   {"rclone.http_proxy", fake("http://p:1", "me", "h.example.com", 22)}}}}));
    auto agent = server(Auth::agent, "agent");
    agent.host = "h.example.com";
    agent.port = 2222;
    agent.user = "me";
    const server_ops::HostKeys keys{.lines = {"[h.example.com]:2222 ssh-ed25519 FAKEKEY"}, .fingerprints = {}};
    REQUIRE(server_ops::update_server(s.env(), agent, std::nullopt, keys));
    CHECK(s.read("grab.conf").find("proxy =") == std::string::npos);
    const auto store = s.secrets();
    CHECK(store.at("agent").at("proxy_password") == bound(socks(9050, "alice"), "fake-proxy-pw"));
    CHECK(store.at("agent").at("rclone.socks_proxy") == fake("127.0.0.1:9050", "me", "h.example.com", 2222));
    CHECK(store.at("agent").at("rclone.http_proxy") == fake("http://p:1", "me", "h.example.com", 2222));
    CHECK(server_ops::list_servers(s.env())[1].unusable.find("has both a SOCKS and an HTTP proxy") != std::string::npos);
}

#ifdef _WIN32
TEST_CASE("proxies: every way grab reaches a server dials its proxy: Test, Trust, an Edit's fetch, searches, lists, Browse, pools") {
    // x.invalid exists nowhere: only a proxy that is asked for it by name gets anywhere, so a
    // direct dial (or a lookup of the name) would fail with another message. The proxy takes the
    // login and answers "connection refused" for the server (REP 05).
    test::ProxyScript script;
    script.user = "alice";
    script.password = "fake-proxy-pw";
    script.rep = 0x05;
    test::FakeProxy proxied(script);
    test::ProxyScript open_script;
    open_script.rep = 0x05;
    test::FakeProxy kept(open_script); // decision 4: a proxy kept from rclone, no login
    Scratch s;
    s.write("grab.conf", std::format("[grab]\ndefault_remote = px\n\n[px]\nhost = x.invalid\nport = 23\nuser = u\n"
                                     "auth = agent\nproxy = socks5://alice@127.0.0.1:{}\n"
                                     "\n[kp]\nhost = k.invalid\nport = 23\nuser = u\nauth = agent\n",
                                     proxied.port));
    Scratch::pins("[x.invalid]:23 ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOMqqnkVzrm0SdG6UOoqKLsabgH5C9okWi0dh2l9GKJl\n"
                  "[k.invalid]:23 ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOMqqnkVzrm0SdG6UOoqKLsabgH5C9okWi0dh2l9GKJl\n");
    REQUIRE(passwords::save(s.store(), {{"px", {{"proxy_password", bound(proxied.proxy("alice"), "fake-proxy-pw")}}},
                                        {"kp", {{"rclone.socks_proxy", fake(std::format("127.0.0.1:{}", kept.port), "u",
                                                                            "k.invalid", 23)}}}}));
    const std::string refused = "x.invalid refused the connection on port 23";
    // Each way, one more connection at the proxy, asking for x.invalid by name with the login.
    int seen = 0;
    const auto dialled = [&](std::string_view what, const std::string& error) {
        CAPTURE(what);
        CHECK_MESSAGE(error.find(refused) != std::string::npos, error);
        CHECK(error.find("fake-proxy-pw") == std::string::npos);
        const auto requests = proxied.requests();
        REQUIRE(static_cast<int>(requests.size()) > seen);
        for (auto i = static_cast<std::size_t>(seen); i < requests.size(); ++i) {
            CHECK(requests[i].login == std::string("\x01\x05", 2) + "alice" + "\x0D" + "fake-proxy-pw");
            CHECK(requests[i].connect == std::string("\x05\x01\x00\x03\x09", 5) + "x.invalid" + std::string("\x00\x17", 2));
        }
        seen = static_cast<int>(requests.size());
    };

    const auto env = s.env();
    // Test.
    const auto tested = server_ops::test_connection(env, "px");
    REQUIRE(tested.has_value());
    CHECK(tested->grab_ok == false);
    dialled("Test", tested->grab_error);
    // Trust's host-key scan, and an Edit's.
    for (const auto purpose : {server_ops::Purpose::trust, server_ops::Purpose::edit}) {
        const auto reach = server_ops::reach_for(env, "px", purpose);
        REQUIRE_MESSAGE(reach.has_value(), reach.error_or(""));
        const auto keys = server_ops::scan_host_keys("x.invalid", 23, {}, *reach);
        REQUIRE_FALSE(keys.has_value());
        dialled("scan", keys.error());
    }
    // The context the window and the command load (engine::load_context): searches, lists, Browse
    // and the pools downloads, uploads, previews, Play and Verify take.
    const auto ctx = engine::load_context(s.conf, std::string("px"), false);
    REQUIRE_MESSAGE(ctx.has_value(), ctx.error_or(""));
    engine::SearchRequest req;
    req.target = "x";
    dialled("search", engine::search(*ctx, req).error_or(""));
    dialled("search_both", engine::search_both(*ctx, req).error_or(""));
    dialled("file list", file_list::fetch(*ctx, 1).error_or(""));
    const auto browsed = browse::list_live(*ctx, "", {});
    REQUIRE_FALSE(browsed.has_value());
    dialled("Browse", browsed.error().message);
    const auto target = engine::ssh_target(*ctx);
    REQUIRE(target.has_value());
    ssh::Pools pools;
    std::string pooled;
    try {
        (void)pools.get("px", *target, 2)->acquire();
    } catch (const ssh::Error& e) {
        pooled = e.what();
    }
    dialled("pool", pooled);

    // A proxy kept from rclone for the login: Test goes through it, never directly.
    const auto through_kept = server_ops::test_connection(env, "kp");
    REQUIRE(through_kept.has_value());
    CHECK(through_kept->grab_error.find("k.invalid refused the connection on port 23") != std::string::npos);
    REQUIRE(kept.requests().size() == 1);
    CHECK(kept.requests()[0].connect == std::string("\x05\x01\x00\x03\x09", 5) + "k.invalid" + std::string("\x00\x17", 2));
}

TEST_CASE("proxies: a wrong proxy password is fatal and tried once; a dead proxy fails, nothing goes directly") {
    test::ProxyScript script;
    script.user = "alice";
    script.password = "fake-right-pw";
    Scratch s;
    Scratch::pins("[x.invalid]:23 ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOMqqnkVzrm0SdG6UOoqKLsabgH5C9okWi0dh2l9GKJl\n");
    int port = 0;
    {
        test::FakeProxy proxied(script);
        port = proxied.port;
        s.write("grab.conf", std::format("[px]\nhost = x.invalid\nport = 23\nuser = u\nauth = agent\n"
                                         "proxy = socks5://alice@127.0.0.1:{}\n",
                                         port));
        REQUIRE(passwords::save(s.store(), {{"px", {{"proxy_password", bound(proxied.proxy("alice"), "fake-wrong-pw")}}}}));
        const auto tested = server_ops::test_connection(s.env(), "px");
        REQUIRE(tested.has_value());
        CHECK(tested->grab_error.find("the proxy turned down the user name and password") != std::string::npos);
        CHECK(tested->grab_error.find("fake-") == std::string::npos);
        CHECK(proxied.connections() == 1);
    }
    // Nothing listening there any more.
    s.write("grab.conf", std::format("[px]\nhost = x.invalid\nport = 23\nuser = u\nauth = agent\n"
                                     "proxy = socks5://127.0.0.1:{}\n",
                                     port));
    const auto dead = server_ops::test_connection(s.env(), "px");
    REQUIRE(dead.has_value());
    CHECK(dead->grab_error.find("can't reach the proxy 127.0.0.1:") != std::string::npos);
    CHECK(dead->grab_error.find("x.invalid") == std::string::npos);
}
#endif

// ---- proxies: `grab server` (PROXIES.md, chunk 1b) --------------------------------------------

TEST_CASE("proxies: an Edit offers the proxy a save keeps: the line, a kept one of any login, never two or one grab can't read") {
    Scratch s;
    s.write("grab.conf", grab_conf + "\n[px]\nhost = p.example\nuser = p\nauth = agent\nproxy = socks5://alice@127.0.0.1:9050\n"
                                     "\n[nopw]\nhost = n.example\nuser = n\nauth = agent\nproxy = http://bob@[::1]:3128\n"
                                     "\n[plain]\nhost = d.example\nuser = d\nauth = agent\n"
                                     "\n[two]\nhost = t.example\nuser = t\nauth = agent\n"
                                     "\n[odd]\nhost = o.example\nuser = o\nauth = agent\n"
                                     "\n[bad]\nhost = b.example\nuser = b\nauth = agent\nproxy = socks5://127.0.0.1\n");
    REQUIRE(passwords::save(s.store(),
                            {{"px", {{"proxy_password", bound(socks(9050, "alice"), "fake-proxy-pw")}}},
                             {"box", {{"rclone.socks_proxy", fake("fake-user:fake-pass@127.0.0.1:9")}}},
                             {"agent", {{"rclone.http_proxy", fake("http://p:1", "someone-else", "h.example.com", 22)}}},
                             {"two", {{"rclone.http_proxy", fake("http://p:1", "t", "t.example", 22)},
                                      {"rclone.socks_proxy", fake("q:2", "t", "t.example", 22)}}},
                             {"odd", {{"rclone.http_proxy", fake("https://fake-user:fake-pass@p:1", "o", "o.example", 22)}}}}));
    const auto env = s.env();
    // The line, never its password: only that one is saved for it.
    const auto px = server_ops::edit_proxy(env, "px");
    CHECK(px.proxy == socks(9050, "alice"));
    CHECK(px.from == guard::ProxyFrom::line);
    CHECK(px.password_saved);
    CHECK(px.why.empty());
    const auto nopw = server_ops::edit_proxy(env, "nopw");
    CHECK(nopw.proxy == proxy::Proxy{.kind = proxy::Kind::http, .host = "::1", .port = 3128, .user = "bob", .password = {}});
    CHECK_FALSE(nopw.password_saved);
    // Kept from rclone for its login (decision 4): offered, its password saved with it.
    const auto box = server_ops::edit_proxy(env, "box");
    CHECK(box.proxy == socks(9, "fake-user"));
    CHECK(box.from == guard::ProxyFrom::kept);
    CHECK(box.password_saved);
    // Kept for another login: a save takes it in (chunk 1a), so it is what Enter keeps.
    const auto agent = server_ops::edit_proxy(env, "agent");
    CHECK(agent.proxy == proxy::Proxy{.kind = proxy::Kind::http, .host = "p", .port = 1, .user = {}, .password = {}});
    CHECK(agent.from == guard::ProxyFrom::kept);
    CHECK_FALSE(agent.password_saved);
    // Directly.
    const auto plain = server_ops::edit_proxy(env, "plain");
    CHECK(plain.proxy == proxy::Proxy{});
    CHECK(plain.from == guard::ProxyFrom::none);
    // Nothing offered, and why: both kinds kept, one grab can't use, a line grab can't read.
    const auto two = server_ops::edit_proxy(env, "two");
    CHECK_FALSE(two.proxy.has_value());
    CHECK(two.why.find("[two] has both a SOCKS and an HTTP proxy") != std::string::npos);
    const auto odd = server_ops::edit_proxy(env, "odd");
    CHECK_FALSE(odd.proxy.has_value());
    CHECK(odd.why.find("which grab can't use: grab can't use HTTPS proxies yet: use http://") != std::string::npos);
    CHECK(odd.why.find("fake-") == std::string::npos);
    const auto bad = server_ops::edit_proxy(env, "bad");
    CHECK_FALSE(bad.proxy.has_value());
    CHECK(bad.why == "[bad] proxy: give the proxy's port (socks5://host:1080)");
}

TEST_CASE("proxies: the host-key fetch of grab server add and edit goes through the proxy given, its password as the save keeps it") {
    Scratch s;
    s.write("grab.conf", grab_conf + "\n[px]\nhost = p.example\nuser = p\nauth = agent\nconnect_timeout = 5s\n"
                                     "proxy = socks5://alice@127.0.0.1:9050\n"
                                     "\n[two]\nhost = t.example\nuser = t\nauth = agent\n"
                                     "\n[bad]\nhost = b.example\nuser = b\nauth = agent\nproxy = socks5://127.0.0.1\n");
    REQUIRE(passwords::save(s.store(), {{"px", {{"proxy_password", bound(socks(9050, "alice"), "fake-proxy-pw")}}},
                                        {"two", {{"rclone.http_proxy", fake("http://p:1", "t", "t.example", 22)},
                                                 {"rclone.socks_proxy", fake("q:2", "t", "t.example", 22)}}},
                                        // What a removed server of the name a new one takes left behind.
                                        {"fresh", {{"proxy_password", bound(socks(9050, "alice"), "fake-left-behind")}}}}));
    const auto env = s.env();
    // A new server: through the proxy given, its password as typed, 2 min to connect.
    auto add = server(Auth::agent, "fresh");
    add.proxy = socks(9050, "alice", "fake-typed");
    auto reach = server_ops::reach_for_save(env, add);
    REQUIRE_MESSAGE(reach.has_value(), reach.error_or(""));
    CHECK(reach->proxy == socks(9050, "alice", "fake-typed"));
    CHECK(reach->connect_timeout == engine::proxy_connect_timeout);
    // Nothing typed: none, whatever the store keeps under that name (Add never uses it).
    add.proxy = socks(9050, "alice");
    reach = server_ops::reach_for_save(env, add);
    REQUIRE(reach.has_value());
    CHECK(reach->proxy == socks(9050, "alice"));
    // No user: no password goes.
    add.proxy = socks(9050, {}, "fake-typed");
    CHECK(server_ops::reach_for_save(env, add).value_or(ssh::Reach{}).proxy == socks(9050));
    // Directly: no proxy, no time of its own.
    add.proxy = proxy::Proxy{};
    reach = server_ops::reach_for_save(env, add);
    REQUIRE(reach.has_value());
    CHECK(reach->proxy == proxy::Proxy{});
    CHECK_FALSE(reach->connect_timeout.has_value());

    // An Edit: the same proxy and user, nothing typed: the saved password; another port or user:
    // none; typed: the typed one. Its written connect_timeout wins.
    auto px = server(Auth::agent, "px");
    px.host = "p.example";
    px.port = 22;
    px.user = "p";
    for (const auto& [given, want] : {std::pair{socks(9050, "alice"), socks(9050, "alice", "fake-proxy-pw")},
                                      std::pair{socks(9051, "alice"), socks(9051, "alice")},
                                      std::pair{socks(9050, "bob"), socks(9050, "bob")},
                                      std::pair{socks(9050, "alice", "fake-typed"), socks(9050, "alice", "fake-typed")}}) {
        px.proxy = given;
        reach = server_ops::reach_for_save(env, px);
        REQUIRE_MESSAGE(reach.has_value(), reach.error_or(""));
        CHECK(reach->proxy == want);
        CHECK(reach->connect_timeout == std::chrono::milliseconds(5'000));
    }
    // Unset: as reach_for's Edit has it (the line).
    px.proxy.reset();
    CHECK(server_ops::reach_for_save(env, px).value_or(ssh::Reach{}).proxy == socks(9050, "alice", "fake-proxy-pw"));

    // What the save replaces doesn't refuse a fetch that gives the proxy: both kinds kept, a line
    // grab can't read. Unset, they stay, and refuse.
    auto two = server(Auth::agent, "two");
    two.host = "t.example";
    two.port = 22;
    two.user = "t";
    two.proxy = proxy::Proxy{};
    CHECK(server_ops::reach_for_save(env, two).has_value());
    two.proxy.reset();
    CHECK(server_ops::reach_for_save(env, two).error_or("").find("has both a SOCKS and an HTTP proxy") != std::string::npos);
    auto bad = server(Auth::agent, "bad");
    bad.host = "b.example";
    bad.port = 22;
    bad.user = "b";
    bad.proxy = socks(1080);
    CHECK(server_ops::reach_for_save(env, bad).value_or(ssh::Reach{}).proxy == socks(1080));
    bad.proxy.reset();
    CHECK_FALSE(server_ops::reach_for_save(env, bad).has_value());

    // An inherited RCLONE_SFTP_SOCKS_PROXY refuses every server, whatever proxy is given.
    {
        const test::ScopedEnv inherited("RCLONE_SFTP_SOCKS_PROXY", "fake-user:fake-pass@127.0.0.1:1080");
        add.proxy = socks(9050);
        px.proxy = socks(9050);
        for (const NewServer* wanted : {&add, &px}) {
            const auto refused = server_ops::reach_for_save(env, *wanted);
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().find("clear RCLONE_SFTP_SOCKS_PROXY for grab") != std::string::npos);
            CHECK(refused.error().find("fake-") == std::string::npos);
        }
    }
}

TEST_CASE("proxies: with a proxy line grab can't read, the same proxy given with no password has none, as Edit's prompt says") {
    Scratch s;
    s.write("grab.conf", grab_conf + "\n[bad]\nhost = b.example\nuser = b\nauth = agent\nproxy = socks5://alice@127.0.0.1\n");
    // The copy for older grabs every saved proxy has, with its password.
    REQUIRE(passwords::save(s.store(),
                            {{"bad", {{"rclone.socks_proxy", fake("alice:fake-pw@127.0.0.1:9050", "b", "b.example", 22)}}}}));
    // Edit offers nothing, so its prompt says Enter means no password.
    const auto offer = server_ops::edit_proxy(s.env(), "bad");
    CHECK_FALSE(offer.proxy.has_value());
    CHECK_FALSE(offer.password_saved);
    CHECK_FALSE(offer.why.empty());

    auto bad = server(Auth::agent, "bad");
    bad.host = "b.example";
    bad.port = 22;
    bad.user = "b";
    bad.proxy = socks(9050, "alice");
    // The fetch sends none.
    const auto reach = server_ops::reach_for_save(s.env(), bad);
    REQUIRE_MESSAGE(reach.has_value(), reach.error_or(""));
    CHECK(reach->proxy == socks(9050, "alice"));
    // The save keeps none: no proxy_password, a copy without it.
    REQUIRE(server_ops::update_server(s.env(), bad, std::nullopt, std::nullopt));
    CHECK(s.read("grab.conf").find("auth = agent\nproxy = socks5://alice@127.0.0.1:9050\n") != std::string::npos);
    CHECK(s.secrets() == passwords::Store{{"bad", {{"rclone.socks_proxy", fake("alice@127.0.0.1:9050", "b", "b.example", 22)}}}});
    // Typed: kept, as anywhere.
    bad.proxy = socks(9050, "alice", "fake-typed");
    REQUIRE(server_ops::update_server(s.env(), bad, std::nullopt, std::nullopt));
    CHECK(s.secrets().at("bad").at("proxy_password") == bound(socks(9050, "alice"), "fake-typed"));
}

TEST_CASE("proxies: grab server list says the route and where it is from; the fetch's note only for a new key read through a proxy") {
    Scratch s;
    s.write("grab.conf", grab_conf + "\n[px]\nhost = p.example\nuser = p\nauth = agent\nproxy = socks5://alice@127.0.0.1:9050\n");
    REQUIRE(passwords::save(s.store(), {{"px", {{"proxy_password", bound(socks(9050, "alice"), "fake-proxy-pw")}}},
                                        {"box", {{"rclone.socks_proxy", fake("fake-user:fake-pass@127.0.0.1:9")}}}}));
    const auto listed = server_ops::list_servers(s.env());
    REQUIRE(listed.size() == 3);
    CHECK(server_ops::route_text(listed[0]) ==
          "through the SOCKS5 proxy fake-user@127.0.0.1:9 (kept in grab's store: from rclone, or a proxy line since "
          "removed; grab server edit box writes it into grab.conf)");
    CHECK(server_ops::route_text(listed[1]).empty()); // agent: directly
    CHECK(server_ops::route_text(listed[2]) == "through the SOCKS5 proxy alice@127.0.0.1:9050 (in grab.conf)");
    for (const auto& info : listed) CHECK(server_ops::route_text(info).find("fake-p") == std::string::npos);

    const server_ops::HostKeys keys{.lines = {"[x.example]:23 ssh-ed25519 KEY1", "[x.example]:23 ssh-rsa KEY2"},
                                    .fingerprints = {}};
    const auto known = grab_known_hosts_path();
    const std::string note = "Read through the SOCKS5 proxy alice@127.0.0.1:9050. A proxy can show its own key here; if "
                             "you can, compare these fingerprints with the server's owner.";
    const auto through = socks(9050, "alice", "fake-proxy-pw");
    // No known_hosts yet: new.
    CHECK(server_ops::fetch_note(known, "x.example", 23, keys, through) == note);
    // Directly: nothing to add.
    CHECK(server_ops::fetch_note(known, "x.example", 23, keys, proxy::Proxy{}).empty());
    // One of the two pinned: still a new one.
    Scratch::pins("[x.example]:23 ssh-ed25519 KEY1\n");
    CHECK(server_ops::fetch_note(known, "x.example", 23, keys, through) == note);
    // Both pinned, in any of the forms known_hosts has: nothing new.
    Scratch::pins("# mine\n[x.example]:23 ssh-ed25519 KEY1\nother.example,[x.example]:23 ssh-rsa KEY2 a comment\n");
    CHECK(server_ops::fetch_note(known, "x.example", 23, keys, through).empty());
    // The same keys pinned for another port, or only revoked: new.
    Scratch::pins("[x.example]:24 ssh-ed25519 KEY1\n[x.example]:24 ssh-rsa KEY2\n");
    CHECK(server_ops::fetch_note(known, "x.example", 23, keys, through) == note);
    Scratch::pins("@revoked [x.example]:23 ssh-ed25519 KEY1\n@revoked [x.example]:23 ssh-rsa KEY2\n");
    CHECK(server_ops::fetch_note(known, "x.example", 23, keys, through) == note);
    // Port 22 is written without brackets.
    Scratch::pins("x.example ssh-ed25519 KEY1\n");
    const server_ops::HostKeys at22{.lines = {"x.example ssh-ed25519 KEY1"}, .fingerprints = {}};
    CHECK(server_ops::fetch_note(known, "x.example", 22, at22, through).empty());
    CHECK(server_ops::fetch_note(known, "x.example", 22, keys, through) == note);
}

TEST_CASE("proxies: grab server edit's questions with Enter throughout keep a proxy kept from rclone, which the save writes into grab.conf") {
    Scratch s;
    s.write("grab.conf", grab_conf);
    REQUIRE(passwords::save(s.store(), {{"box", {{"password", fake("fake-pw")},
                                                 {"rclone.socks_proxy", fake("fake-user:fake-pass@127.0.0.1:9")}}}}));
    const auto env = s.env();
    const auto listed = server_ops::list_servers(env);
    REQUIRE_FALSE(listed.empty());
    auto wanted = server_ops::edit_defaults(listed[0]);
    // Enter at every proxy question; a question asked again (no default to keep) ends the input,
    // so a regression fails rather than asking forever.
    std::vector<std::string> said;
    int asked = 0;
    const server_prompt::Console enter{
        .ask = [&](std::string_view, std::string_view def) -> std::optional<std::string> {
            if (++asked > 3) return std::nullopt;
            return std::string(def);
        },
        .secret = [](std::string_view) -> std::optional<std::string> { return std::string(); },
        .say = [&](std::string_view line) { said.emplace_back(line); },
    };
    wanted.proxy = server_prompt::ask_proxy(enter, server_ops::edit_proxy(env, "box"), wanted);
    REQUIRE(wanted.proxy.has_value());
    CHECK(*wanted.proxy == socks(9, "fake-user"));
    CHECK(said.size() == 1); // that it is kept in grab's store
    const auto done = server_ops::update_server(env, wanted, std::nullopt, std::nullopt);
    REQUIRE_MESSAGE(done.has_value(), done.error_or(""));
    CHECK(s.read("grab.conf").find("auth = password\nproxy = socks5://fake-user@127.0.0.1:9\n") != std::string::npos);
    CHECK(s.secrets() ==
          passwords::Store{{"box", {{"password", fake("fake-pw")},
                                    {"proxy_password", bound(socks(9, "fake-user"), "fake-pass")},
                                    {"rclone.socks_proxy", fake("fake-user:fake-pass@127.0.0.1:9")}}}});
    const auto after = server_ops::list_servers(s.env());
    CHECK(server_ops::route_text(after[0]) == "through the SOCKS5 proxy fake-user@127.0.0.1:9 (in grab.conf)");
    // The next edit offers it from the line, its password saved.
    const auto offer = server_ops::edit_proxy(s.env(), "box");
    CHECK(offer.from == guard::ProxyFrom::line);
    CHECK(offer.password_saved);
}

#ifdef _WIN32
TEST_CASE("proxies: an Edit offers the line's proxy without a password when the store can't be read") {
    Scratch s;
    s.write("grab.conf", grab_conf + "\n[px]\nhost = p.example\nuser = p\nauth = agent\nproxy = socks5://alice@127.0.0.1:9050\n");
    std::filesystem::create_directories(s.store().parent_path());
    std::ofstream(s.store(), std::ios::binary) << "grab-passwords-sealed 1\nanother account's";
    const auto env = s.env();
    const auto px = server_ops::edit_proxy(env, "px");
    CHECK(px.proxy == socks(9050, "alice"));
    CHECK(px.from == guard::ProxyFrom::line);
    CHECK_FALSE(px.password_saved); // the save sets the store aside, with it
    const auto box = server_ops::edit_proxy(env, "box");
    CHECK(box.proxy == proxy::Proxy{});
}

TEST_CASE("proxies: grab server add and edit fetch the host key through the proxy given, with the typed or the saved password") {
    // x.invalid exists nowhere: only the proxy, asked for it by name, gets anywhere. It takes the
    // login and answers "connection refused" for the server (REP 05).
    test::ProxyScript script;
    script.user = "alice";
    script.password = "fake-proxy-pw";
    script.rep = 0x05;
    test::FakeProxy proxied(script);
    Scratch s;
    s.write("grab.conf", std::format("[px]\nhost = x.invalid\nport = 23\nuser = u\nauth = agent\n"
                                     "proxy = socks5://alice@127.0.0.1:{}\n",
                                     proxied.port));
    REQUIRE(passwords::save(s.store(), {{"px", {{"proxy_password", bound(proxied.proxy("alice"), "fake-proxy-pw")}}}}));
    const auto env = s.env();
    const std::string login = std::string("\x01\x05", 2) + "alice" + "\x0D" + "fake-proxy-pw";
    const std::string connect = std::string("\x05\x01\x00\x03\x09", 5) + "x.invalid" + std::string("\x00\x17", 2);
    // Add: the typed password.
    auto add = server(Auth::agent, "fresh");
    add.host = "x.invalid";
    add.proxy = proxied.proxy("alice", "fake-proxy-pw");
    // Edit: Enter kept the saved one.
    auto edit = server(Auth::agent, "px");
    edit.host = "x.invalid";
    edit.user = "u";
    edit.proxy = proxied.proxy("alice");
    std::size_t seen = 0;
    for (const NewServer* wanted : {&add, &edit}) {
        CAPTURE(wanted->name);
        const auto reach = server_ops::reach_for_save(env, *wanted);
        REQUIRE_MESSAGE(reach.has_value(), reach.error_or(""));
        const auto keys = server_ops::scan_host_keys("x.invalid", 23, {}, *reach);
        REQUIRE_FALSE(keys.has_value());
        CHECK(keys.error().find("x.invalid refused the connection on port 23") != std::string::npos);
        CHECK(keys.error().find("fake-") == std::string::npos);
        const auto requests = proxied.requests();
        REQUIRE(requests.size() > seen);
        for (auto i = seen; i < requests.size(); ++i) {
            CHECK(requests[i].login == login);
            CHECK(requests[i].connect == connect);
        }
        seen = requests.size();
    }
    // A wrong password typed: turned down, fatal, nothing tried again.
    add.proxy = proxied.proxy("alice", "fake-wrong-pw");
    const auto reach = server_ops::reach_for_save(env, add);
    REQUIRE(reach.has_value());
    const auto keys = server_ops::scan_host_keys("x.invalid", 23, {}, *reach);
    REQUIRE_FALSE(keys.has_value());
    CHECK(keys.error().find("the proxy turned down the user name and password") != std::string::npos);
    CHECK(keys.error().find("fake-") == std::string::npos);
    CHECK(proxied.requests().size() == seen + 1);
}
#endif
