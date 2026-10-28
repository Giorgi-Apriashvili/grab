#include "config.hpp"
#include "ini.hpp"
#include "secrets.hpp"
#include "server_ops.hpp"
#include "servers.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (LockFileEx, SetFileAttributesW)
#endif

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
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

TEST_CASE("a saved password stays with the host and user it was typed for (G40)") {
    server_ops::ServerInfo cur;
    cur.host = "box.example.com";
    cur.user = "u1";
    cur.port = 23;
    cur.auth = Auth::password;
    auto wanted = server(Auth::password);
    CHECK_FALSE(server_ops::edit_needs_password(cur, wanted));
    wanted.port = 22; // the same machine on another port
    CHECK_FALSE(server_ops::edit_needs_password(cur, wanted));
    wanted.host = "BOX.example.com"; // host names ignore case
    CHECK_FALSE(server_ops::edit_needs_password(cur, wanted));
    wanted.host = "evil.example";
    CHECK(server_ops::edit_needs_password(cur, wanted));
    wanted = server(Auth::password);
    wanted.user = "u2";
    CHECK(server_ops::edit_needs_password(cur, wanted));
    cur.auth = Auth::agent; // switching to a password always needs one
    CHECK(server_ops::edit_needs_password(cur, server(Auth::password)));
    CHECK_FALSE(server_ops::edit_needs_password(cur, server(Auth::agent)));

    // The backend refuses it too, whatever the window says.
    Scratch s;
    s.write("rclone.conf", rclone_conf);
    s.write("grab.conf", grab_conf(s.dir));
    auto env = server_ops::load_env(s.dir / "grab.conf", s.dir);
    REQUIRE(env.has_value());
    auto moved = server(Auth::password);
    moved.host = "evil.example";
    const auto refused = server_ops::update_server(*env, moved, std::nullopt, std::nullopt);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().find("enter the password again") != std::string::npos);
    CHECK(s.read("rclone.conf") == rclone_conf);
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
    CHECK(list[0].engine == Engine::rclone);
    CHECK(list[0].search == "rclone");
    CHECK(list[0].search_roots == std::vector<std::string>{"/home/movies"});
    CHECK(list[1].name == "agent");
    CHECK_FALSE(list[1].is_default);
    CHECK(list[1].auth == Auth::agent);
    CHECK(list[1].search == "ssh");
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

TEST_CASE("a server's connection limit is written only once set, and blank means automatic") {
    Scratch s;
    s.write("rclone.conf", rclone_conf);
    s.write("grab.conf", grab_conf(s.dir));
    auto env = server_ops::load_env(s.dir / "grab.conf", s.dir);
    REQUIRE(env.has_value());
    CHECK_FALSE(server_ops::list_servers(*env)[0].max_connections.has_value());
    CHECK(server_ops::list_servers(*env)[0].default_connections == 12);

    auto wanted = server(Auth::password);
    wanted.search_roots = {"/home/movies"};
    REQUIRE(server_ops::update_server(*env, wanted, std::nullopt, std::nullopt).has_value());
    CHECK(s.read("grab.conf").find("max_connections") == std::string::npos); // untouched while automatic

    wanted.max_connections = 5;
    env = server_ops::load_env(s.dir / "grab.conf", s.dir);
    REQUIRE(server_ops::update_server(*env, wanted, std::nullopt, std::nullopt).has_value());
    CHECK(s.read("grab.conf").find("max_connections = 5\n") != std::string::npos);
    env = server_ops::load_env(s.dir / "grab.conf", s.dir);
    CHECK(server_ops::list_servers(*env)[0].max_connections == 5);

    wanted.max_connections.reset();
    REQUIRE(server_ops::update_server(*env, wanted, std::nullopt, std::nullopt).has_value());
    CHECK(s.read("grab.conf").find("max_connections =\n") != std::string::npos);
    env = server_ops::load_env(s.dir / "grab.conf", s.dir);
    CHECK_FALSE(server_ops::list_servers(*env)[0].max_connections.has_value());

    // Out of range is a config error.
    s.write("grab.conf", grab_conf(s.dir) + "max_connections = 0\n");
    CHECK_FALSE(server_ops::load_env(s.dir / "grab.conf", s.dir).has_value());
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

TEST_CASE("grab's own rclone.conf loses its passwords to the encrypted store on load") {
    // The test runner points APPDATA at a scratch folder (test_main.cpp), so this is grab's
    // own rclone.conf there, never the real one.
    const auto own = grab_rclone_config_path();
    REQUIRE(util::path_to_utf8(own).find("grab-tests-home") != std::string::npos);
    Scratch s;
    std::filesystem::create_directories(own.parent_path());
    std::ofstream(own, std::ios::binary) << rclone_conf;
    s.write("grab.conf", "[grab]\ndefault_remote = box\n\n[box]\nmax_depth = 4\n\n[agent]\n");
    auto env = server_ops::load_env(s.dir / "grab.conf", s.dir);
    REQUIRE(env.has_value());
    CHECK(env->secrets_error.empty());
    CHECK(util::read_file(own)->find("pass") == std::string::npos);
    const auto store = secrets::load(secrets::path_beside(own));
    REQUIRE(store.has_value());
    CHECK(store->at("box").at("pass") == "OBSCURED");
    CHECK(util::getenv_utf8("RCLONE_CONFIG_BOX_PASS") == "OBSCURED");
    // The rest of the server is still read from rclone.conf.
    const auto list = server_ops::list_servers(*env);
    REQUIRE(list.size() == 2);
    CHECK(list[0].host == "box.example.com");
    CHECK(list[0].auth == Auth::password);
    secrets::export_to_env({});
    std::filesystem::remove(own);
    std::filesystem::remove(secrets::path_beside(own));
}

TEST_CASE("a change starts from grab.conf as it is now, not as it was loaded (G175)") {
    Scratch s;
    s.write("rclone.conf", rclone_conf);
    s.write("grab.conf", grab_conf(s.dir));
    auto env = server_ops::load_env(s.dir / "grab.conf", s.dir);
    REQUIRE(env.has_value());
    // Another grab adds a folder to box meanwhile; this one's change doesn't undo it.
    s.write("grab.conf", servers::set_value(grab_conf(s.dir), "box", "search_roots", "/home/movies, /home/tv"));
    REQUIRE(server_ops::set_default(*env, "agent").has_value());
    const auto text = s.read("grab.conf");
    CHECK(text.find("default_remote = agent\n") != std::string::npos);
    CHECK(text.find("search_roots = /home/movies, /home/tv\n") != std::string::npos);
}

TEST_CASE("a new server's name is checked against an rclone.conf that has to be readable (G217)") {
    Scratch s;
    s.write("rclone.conf", rclone_conf + "\n[nas]\ntype = sftp\nhost = nas.example.invalid\n");
    s.write("grab.conf", grab_conf(s.dir));
    auto env = server_ops::load_env(s.dir / "grab.conf", s.dir);
    REQUIRE(env.has_value());
    auto doc = server_ops::read_rclone_conf(*env);
    REQUIRE(doc.has_value());
    CHECK(server_ops::name_taken(*env, *doc, "box") == "grab.conf already has a server called box");
    CHECK(server_ops::name_taken(*env, *doc, "nas") == "grab's rclone.conf already has a remote called nas");
    CHECK_FALSE(server_ops::name_taken(*env, *doc, "fresh").has_value());

    // One grab can't parse is an error, not "no remotes"; the add stops before anything is written.
    const std::string broken = rclone_conf + "\n[nas]\ntype = sftp\nthis line is not ini\n";
    s.write("rclone.conf", broken);
    CHECK_FALSE(server_ops::read_rclone_conf(*env).has_value());
    auto nas = server(Auth::agent);
    nas.name = "nas";
    nas.host = "new.example.invalid";
    const server_ops::HostKeys keys{.lines = {"[new.example.invalid]:23 ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOMqqnkVzrm0SdG6UOoqKLsabgH5C9okWi0dh2l9GKJl"}, .fingerprints = {}};
    CHECK_FALSE(server_ops::add_server(*env, nas, std::nullopt, keys).has_value());
    CHECK(s.read("rclone.conf") == broken);
    CHECK(s.read("grab.conf") == grab_conf(s.dir));
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

namespace {
// A second login on box's host and port (the same machine): its host keys are box's too.
constexpr std::string_view twin_remote =
    "\n[twin]\ntype = sftp\nhost = box.example.com\nport = 23\nuser = u2\nkey_use_agent = true\n";
} // namespace

TEST_CASE("removing a server takes its host keys out, unless another server is at its host and port (G87)") {
    // grab's own known_hosts in the test runner's scratch APPDATA (test_main.cpp).
    const auto known = grab_known_hosts_path();
    REQUIRE(util::path_to_utf8(known).find("grab-tests-home") != std::string::npos);
    std::filesystem::create_directories(known.parent_path());
    Scratch s;
    s.write("rclone.conf", rclone_conf + std::string(twin_remote));
    s.write("grab.conf", grab_conf(s.dir) + "\n[twin]\n");
    const std::string others = "[box.example.com]:2222 ssh-ed25519 OTHERPORT\nunrelated.example ssh-ed25519 KEEP\n";
    std::ofstream(known, std::ios::binary) << "[box.example.com]:23 ssh-ed25519 BOX\n"
                                              "h.example.com ssh-ed25519 H\n"
                                              "[h.example.com]:22 ssh-rsa H22\n" + others;
    auto remove = [&](const char* name) {
        auto env = server_ops::load_env(s.dir / "grab.conf", s.dir);
        REQUIRE(env.has_value());
        // An explicit rclone_config: the remote stays there, but its known_hosts_file isn't
        // grab's, so it doesn't need grab's keys.
        auto note = server_ops::remove_server(*env, name);
        REQUIRE(note.has_value());
        CHECK(note->empty());
        return util::read_file(known).value_or("");
    };
    // twin is still at box's host and port: the keys stay.
    CHECK(remove("box").find("[box.example.com]:23 ssh-ed25519 BOX\n") != std::string::npos);
    // Now nobody is.
    CHECK(remove("twin").find("BOX") == std::string::npos);
    // Port 22: the plain host's line and [host]:22's.
    CHECK(remove("agent") == others);
    CHECK(s.read("rclone.conf") == rclone_conf + std::string(twin_remote));
    std::filesystem::remove(known);
}

TEST_CASE("a remote that points at grab's known_hosts keeps its host's keys when a server is removed (G87)") {
    const auto known = grab_known_hosts_path();
    REQUIRE(util::path_to_utf8(known).find("grab-tests-home") != std::string::npos);
    std::filesystem::create_directories(known.parent_path());
    Scratch s;
    // box's remote uses grab's known_hosts, as `grab server add` makes it; [mine] isn't a grab
    // server's, but uses grab's known_hosts too (spelled in capitals: the same file all the same).
    std::string conf = rclone_conf;
    conf.replace(conf.find("C:\\kh"), 5, util::path_to_utf8(known));
    std::string upper = util::path_to_utf8(known);
    std::ranges::transform(upper, upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    conf += "\n[mine]\ntype = sftp\nhost = h.example.com\nuser = me\nknown_hosts_file = " + upper + "\n";
    s.write("rclone.conf", conf);
    s.write("grab.conf", grab_conf(s.dir));
    const std::string pins = "[box.example.com]:23 ssh-ed25519 BOX\nh.example.com ssh-ed25519 H\n";
    std::ofstream(known, std::ios::binary) << pins;
    for (const char* name : {"box", "agent"}) {
        auto env = server_ops::load_env(s.dir / "grab.conf", s.dir);
        REQUIRE(env.has_value());
        auto note = server_ops::remove_server(*env, name);
        REQUIRE(note.has_value());
        CHECK(note->empty());
    }
    // box's remote stays in the explicit rclone_config, still trusting grab's keys; [mine] too.
    CHECK(util::read_file(known).value_or("") == pins);
    CHECK(s.read("rclone.conf") == conf);
    std::filesystem::remove(known);
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

TEST_CASE("an add that fails after rclone made the remote takes it out again, so the name stays free (G218)") {
    // The bundled rclone, one folder above this test program (build\<preset>\rclone.exe).
    const auto rclone_dir = util::self_exe_path().parent_path().parent_path();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(rclone_dir / "rclone.exe", ec)) {
        MESSAGE("no bundled rclone.exe next to the build; skipped");
        return;
    }
    Scratch s;
    s.write("rclone.conf", rclone_conf);
    s.write("grab.conf", grab_conf(s.dir));
    auto env = server_ops::load_env(s.dir / "grab.conf", rclone_dir);
    REQUIRE(env.has_value());
    auto fresh = server(Auth::agent);
    fresh.name = "fresh";
    fresh.host = "fresh.example.invalid";
    const server_ops::HostKeys keys{.lines = {"[fresh.example.invalid]:23 ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOMqqnkVzrm0SdG6UOoqKLsabgH5C9okWi0dh2l9GKJl"}, .fingerprints = {}};

    // grab.conf can't be replaced (read-only), so the last step fails.
    const auto conf = s.dir / "grab.conf";
    SetFileAttributesW(conf.c_str(), FILE_ATTRIBUTE_READONLY);
    const auto added = server_ops::add_server(*env, fresh, std::nullopt, keys);
    SetFileAttributesW(conf.c_str(), FILE_ATTRIBUTE_NORMAL);
    REQUIRE_FALSE(added.has_value());
    CHECK(s.read("rclone.conf").find("[fresh]") == std::string::npos);
    CHECK(s.read("grab.conf") == grab_conf(s.dir));

    // The same add now goes through.
    env = server_ops::load_env(conf, rclone_dir);
    REQUIRE(env.has_value());
    const auto again = server_ops::add_server(*env, fresh, std::nullopt, keys);
    CHECK_MESSAGE(again.has_value(), again.error_or(""));
    CHECK(s.read("rclone.conf").find("[fresh]") != std::string::npos);
    CHECK(s.read("grab.conf").find("[fresh]") != std::string::npos);
    std::filesystem::remove(grab_known_hosts_path(), ec);
}

TEST_CASE("an edit to another host or port takes the old one's keys out, unless another server is there (G87)") {
    const auto rclone_dir = util::self_exe_path().parent_path().parent_path();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(rclone_dir / "rclone.exe", ec)) {
        MESSAGE("no bundled rclone.exe next to the build; skipped");
        return;
    }
    const auto known = grab_known_hosts_path();
    REQUIRE(util::path_to_utf8(known).find("grab-tests-home") != std::string::npos);
    std::filesystem::create_directories(known.parent_path());
    Scratch s;
    s.write("rclone.conf", rclone_conf + std::string(twin_remote));
    s.write("grab.conf", grab_conf(s.dir) + "\n[twin]\n");
    std::ofstream(known, std::ios::binary) << "[box.example.com]:23 ssh-ed25519 BOX\nh.example.com ssh-ed25519 H\n";
    auto load = [&] {
        auto env = server_ops::load_env(s.dir / "grab.conf", rclone_dir);
        REQUIRE(env.has_value());
        return std::move(*env);
    };
    auto pins = [&] { return util::read_file(known).value_or(""); };

    // agent moves to another port, with its keys there: the old port's go.
    NewServer agent;
    agent.name = "agent";
    agent.host = "h.example.com";
    agent.port = 2200;
    agent.user = "me";
    agent.auth = Auth::agent;
    agent.max_depth = 6;
    auto done = server_ops::update_server(load(), agent, std::nullopt,
                                          server_ops::HostKeys{.lines = {"[h.example.com]:2200 ssh-ed25519 H2200"}, .fingerprints = {}});
    REQUIRE_MESSAGE(done.has_value(), done.error_or(""));
    CHECK(done->empty());
    CHECK(pins() == "[box.example.com]:23 ssh-ed25519 BOX\n[h.example.com]:2200 ssh-ed25519 H2200\n");

    // box moves to another host; twin stays at the old one, whose keys stay.
    NewServer box = server(Auth::agent);
    box.host = "box2.example.com";
    done = server_ops::update_server(load(), box, std::nullopt,
                                     server_ops::HostKeys{.lines = {"[box2.example.com]:23 ssh-ed25519 BOX2"}, .fingerprints = {}});
    REQUIRE_MESSAGE(done.has_value(), done.error_or(""));
    CHECK(pins() == "[box.example.com]:23 ssh-ed25519 BOX\n[h.example.com]:2200 ssh-ed25519 H2200\n"
                    "[box2.example.com]:23 ssh-ed25519 BOX2\n");

    // Trusting twin again replaces its host and port's keys.
    REQUIRE(server_ops::trust_server(load(), "twin",
                                     server_ops::HostKeys{.lines = {"[box.example.com]:23 ssh-ed25519 BOXNEW"}, .fingerprints = {}}));
    CHECK(pins() == "[h.example.com]:2200 ssh-ed25519 H2200\n[box2.example.com]:23 ssh-ed25519 BOX2\n"
                    "[box.example.com]:23 ssh-ed25519 BOXNEW\n");
    CHECK(s.read("rclone.conf").find("known_hosts_file = " + util::path_to_utf8(known)) != std::string::npos);
    std::filesystem::remove(known, ec);
}

TEST_CASE("removing a server from grab's own rclone.conf deletes its remote, and with it its host keys (G87)") {
    const auto rclone_dir = util::self_exe_path().parent_path().parent_path();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(rclone_dir / "rclone.exe", ec)) {
        MESSAGE("no bundled rclone.exe next to the build; skipped");
        return;
    }
    const auto own = grab_rclone_config_path();
    const auto known = grab_known_hosts_path();
    REQUIRE(util::path_to_utf8(own).find("grab-tests-home") != std::string::npos);
    std::filesystem::create_directories(own.parent_path());
    std::ofstream(own, std::ios::binary) << "[box]\ntype = sftp\nhost = box.example.com\nport = 23\nuser = u1\n"
                                            "key_use_agent = true\nknown_hosts_file = " + util::path_to_utf8(known) + "\n";
    std::ofstream(known, std::ios::binary) << "[box.example.com]:23 ssh-ed25519 BOX\nh.example.com ssh-ed25519 H\n";
    Scratch s;
    s.write("grab.conf", "[grab]\ndefault_remote = box\n\n[box]\nmax_depth = 4\n");
    auto env = server_ops::load_env(s.dir / "grab.conf", rclone_dir);
    REQUIRE(env.has_value());
    auto note = server_ops::remove_server(*env, "box");
    REQUIRE(note.has_value());
    CHECK_MESSAGE(note->empty(), *note);
    CHECK(util::read_file(own).value_or("").find("[box]") == std::string::npos);
    CHECK(util::read_file(known).value_or("") == "h.example.com ssh-ed25519 H\n");
    std::filesystem::remove(own, ec);
    std::filesystem::remove(secrets::path_beside(own), ec);
    std::filesystem::remove(known, ec);
}
#endif

TEST_CASE("a change sets aside a password store it can't decrypt, and names who needs a password again (G211)") {
    const auto own = grab_rclone_config_path();
    REQUIRE(util::path_to_utf8(own).find("grab-tests-home") != std::string::npos);
    Scratch s;
    std::filesystem::create_directories(own.parent_path());
    std::ofstream(own, std::ios::binary) << "[box]\ntype = sftp\nhost = box.example.com\nport = 23\nuser = u1\n\n"
                                            "[two]\ntype = sftp\nhost = two.example.com\nuser = u2\n\n"
                                            "[agent]\ntype = sftp\nhost = h.example.com\nuser = me\nkey_use_agent = true\n";
    const auto store = secrets::path_beside(own);
    std::ofstream(store, std::ios::binary) << "grab-secrets-sealed 1\nanother account's";
    s.write("grab.conf", "[grab]\ndefault_remote = box\n\n[box]\nmax_depth = 4\n\n[two]\n\n[agent]\n");
    auto env = server_ops::load_env(s.dir / "grab.conf", s.dir);
    REQUIRE(env.has_value());
    CHECK_FALSE(env->secrets_error.empty());

    // box gets a new password: the store starts over, and two's password is the one to enter again.
    const auto note = server_ops::update_server(*env, server(Auth::password), std::string("NEWOBS"), std::nullopt);
    REQUIRE(note.has_value());
    CHECK(note->find("couldn't be read and were moved to") != std::string::npos);
    CHECK(note->find("enter the password of two again") != std::string::npos);
    CHECK(secrets::load(store) == secrets::Store{{"box", {{"pass", "NEWOBS"}}}});
    bool kept = false;
    for (const auto& e : std::filesystem::directory_iterator(own.parent_path())) {
        if (util::path_to_utf8(e.path().filename()).starts_with("secrets.unreadable-")) {
            kept = util::read_file(e.path()).value_or("") == "grab-secrets-sealed 1\nanother account's";
            std::filesystem::remove(e.path());
        }
    }
    CHECK(kept);
    // Readable now: the next change has nothing to say.
    env = server_ops::load_env(s.dir / "grab.conf", s.dir);
    REQUIRE(env.has_value());
    CHECK(server_ops::update_server(*env, server(Auth::password), std::nullopt, std::nullopt) == std::string());
    secrets::export_to_env({});
    std::filesystem::remove(own);
    std::filesystem::remove(store);
}
TEST_CASE("server ops: a test fails on grab's login only where grab downloads (engine = grab)") {
    server_ops::TestResult t;
    t.rclone_ok = true;
    t.grab_ok = false;
    t.grab_error = "h's host key isn't trusted yet";
    CHECK(t.ok()); // engine = rclone: grab only verifies there, a warning
    t.grab_engine = true;
    CHECK_FALSE(t.ok());
    t.grab_ok = true;
    CHECK(t.ok());
    t.rclone_ok = false;
    CHECK_FALSE(t.ok());
}
TEST_CASE("server ops: on engine = grab a test asks rclone nothing and stands on grab's login") {
    // rclone downloads nothing there (previews and Play go through grab's client since phase 6), so
    // test_connection leaves rclone_ok unset.
    server_ops::TestResult t;
    t.grab_engine = true;
    CHECK_FALSE(t.ok()); // nothing tested yet
    t.grab_ok = true;
    CHECK(t.ok());
    t.grab_ok = false;
    CHECK_FALSE(t.ok());
    // engine = rclone without its listing's answer is no failure either (a grab warning at most).
    server_ops::TestResult r;
    r.grab_ok = false;
    CHECK(r.ok());
    r.rclone_ok = false;
    CHECK_FALSE(r.ok());
}
TEST_CASE("server ops: Test runs rclone's listing on engine = rclone only") {
    if (util::getenv_utf8("GRAB_ENGINE")) {
        MESSAGE("GRAB_ENGINE is set; skipped");
        return;
    }
    // Both servers fail without the network: grab's login binds to TEST-NET's 192.0.2.1, which
    // this PC hasn't got, and the rclone asked is a program that isn't there.
    Scratch s;
    s.write("rclone.conf", "[h]\ntype = sftp\nhost = 127.0.0.1\nport = 1\nuser = me\nkey_use_agent = true\n");
    s.write("grab.conf", "[grab]\nrclone = " + util::path_to_utf8(s.dir / "no-rclone.exe") +
                             "\nrclone_config = " + util::path_to_utf8(s.dir / "rclone.conf") +
                             "\n\n[g]\nrclone_remote = h\nengine = grab\ncommon_flags = --bind 192.0.2.1\n"
                             "\n[r]\nrclone_remote = h\nengine = rclone\nfind = rclone\ncommon_flags = --bind 192.0.2.1\n");
    auto env = server_ops::load_env(s.dir / "grab.conf", s.dir);
    REQUIRE(env.has_value());
    const auto g = server_ops::test_connection(*env, "g");
    REQUIRE(g.has_value());
    CHECK(g->grab_engine);
    CHECK_FALSE(g->rclone_ok.has_value()); // not asked
    CHECK(g->grab_ok == false);
    CHECK(g->grab_error.find("--bind") != std::string::npos);
    CHECK_FALSE(g->ok());
    const auto r = server_ops::test_connection(*env, "r");
    REQUIRE(r.has_value());
    CHECK_FALSE(r->grab_engine);
    CHECK(r->rclone_ok == false); // asked, and it isn't there
    CHECK_FALSE(r->rclone_error.empty());
    CHECK(r->grab_ok == false); // previews, Play and Verify can't run: a warning
    CHECK_FALSE(r->ok());
}
