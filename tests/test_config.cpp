#include "config.hpp"
#include "file_list.hpp"
#include "gui_state.hpp"
#include "ini.hpp"
#include "passwords.hpp"
#include "rclone_import.hpp"
#include "server_conf.hpp"
#include "servers.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (SetEnvironmentVariableW)
#else
#include <cstdlib> // setenv, unsetenv
#endif

using namespace grab;

namespace {
GrabConfig parse_ok(std::string_view text) {
    auto doc = ini::parse(text);
    REQUIRE(doc.has_value());
    auto cfg = parse_grab_config(*doc);
    REQUIRE_MESSAGE(cfg.has_value(), cfg.error_or(""));
    return *cfg;
}
} // namespace

TEST_CASE("minimal grab.conf gets built-in defaults") {
    const auto cfg = parse_ok("[hetzner]\nsearch_roots = /home/alice\n");
    CHECK(cfg.rclone == "rclone");
    CHECK(cfg.ssh == "ssh");
    CHECK_FALSE(cfg.rclone_config.has_value());
    CHECK_FALSE(cfg.default_remote.has_value());
    REQUIRE(cfg.remotes.size() == 1);
    const auto& r = cfg.remotes[0];
    CHECK(r.name == "hetzner");
    CHECK(r.rclone_remote == "hetzner");
    CHECK(r.search_roots == std::vector<std::string>{"/home/alice"});
    CHECK(r.max_depth == 4);
    CHECK(r.skip_hidden);
    CHECK(r.ssh_options.empty());
    CHECK(r.common_flags == util::split_args(default_common_flags));
    CHECK(r.folder_flags == util::split_args(default_folder_flags));
    CHECK(r.file_flags == util::split_args(default_file_flags));
}

TEST_CASE("explicit keys override defaults, blank flag keys mean no flags") {
    const auto cfg = parse_ok("[grab]\n"
                              "rclone = C:\\Tools\\rclone.exe\n"
                              "rclone_config = C:\\cfg\\rclone.conf\n"
                              "ssh = \n"
                              "default_remote = box\n"
                              "[box]\n"
                              "rclone_remote = hetzner\n"
                              "search_roots = /home/alice, /srv\n"
                              "max_depth = 2\n"
                              "skip_hidden = no\n"
                              "ssh_options = -o ServerAliveInterval=30\n"
                              "common_flags =\n"
                              "folder_flags = --transfers 8\n");
    CHECK(cfg.rclone == "C:\\Tools\\rclone.exe");
    REQUIRE(cfg.rclone_config.has_value());
    CHECK(util::path_to_utf8(*cfg.rclone_config) == "C:\\cfg\\rclone.conf");
    CHECK(cfg.ssh == "ssh"); // blank falls back
    CHECK(cfg.default_remote == "box");
    REQUIRE(cfg.remotes.size() == 1);
    const auto& r = cfg.remotes[0];
    CHECK(r.name == "box");
    CHECK(r.rclone_remote == "hetzner");
    CHECK(r.search_roots == std::vector<std::string>{"/home/alice", "/srv"});
    CHECK(r.max_depth == 2);
    CHECK_FALSE(r.skip_hidden);
    CHECK(r.ssh_options == std::vector<std::string>{"-o", "ServerAliveInterval=30"});
    CHECK(r.common_flags.empty());
    CHECK(r.folder_flags == std::vector<std::string>{"--transfers", "8"});
    CHECK(r.file_flags == util::split_args(default_file_flags)); // untouched
}

TEST_CASE("grab.conf validation errors") {
    // No servers yet is a valid file; asking for one explains how to add it.
    auto no_remotes = parse_grab_config(*ini::parse("[grab]\nrclone = rclone\n"));
    REQUIRE(no_remotes.has_value());
    auto none = no_remotes->select(std::nullopt);
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().find("grab server add") != std::string::npos);

    auto bad_find = parse_grab_config(*ini::parse("[h]\nfind = telnet\n"));
    CHECK_FALSE(bad_find.has_value());

    auto bad_depth = parse_grab_config(*ini::parse("[h]\nmax_depth = deep\n"));
    CHECK_FALSE(bad_depth.has_value());

    auto zero_depth = parse_grab_config(*ini::parse("[h]\nmax_depth = 0\n"));
    CHECK_FALSE(zero_depth.has_value());

    auto bad_bool = parse_grab_config(*ini::parse("[h]\nskip_hidden = maybe\n"));
    CHECK_FALSE(bad_bool.has_value());
}

TEST_CASE("remote selection") {
    const auto one = parse_ok("[only]\nsearch_roots = /x\n");
    auto s = one.select(std::nullopt);
    REQUIRE(s.has_value());
    CHECK((*s)->name == "only");
    CHECK_FALSE(one.select(std::string("nope")).has_value());

    const auto two = parse_ok("[a]\nsearch_roots = /x\n[b]\nsearch_roots = /y\n");
    CHECK_FALSE(two.select(std::nullopt).has_value()); // ambiguous
    auto b = two.select(std::string("b"));
    REQUIRE(b.has_value());
    CHECK((*b)->name == "b");

    const auto with_default = parse_ok("[grab]\ndefault_remote = b\n[a]\n[b]\n");
    auto d = with_default.select(std::nullopt);
    REQUIRE(d.has_value());
    CHECK((*d)->name == "b");

    const auto bad_default = parse_ok("[grab]\ndefault_remote = zzz\n[a]\n");
    CHECK_FALSE(bad_default.select(std::nullopt).has_value());
}

TEST_CASE("rclone.conf sftp remote is read for the ssh step") {
    auto doc = ini::parse("[hetzner]\n"
                          "type = sftp\n"
                          "host = 203.0.113.10\n"
                          "user = alice\n"
                          "port = 2222\n"
                          "key_pem = \n"
                          "key_file = E:\\keys\\server.pem\n"
                          "key_file_pass = obscured\n"
                          "shell_type = unix\n"
                          "known_hosts_file = C:\\Users\\alice\\.ssh\\known_hosts\n"
                          "[gdrive]\n"
                          "type = drive\n");
    REQUIRE(doc.has_value());

    auto r = parse_rclone_remote(*doc, "hetzner");
    REQUIRE_MESSAGE(r.has_value(), r.error_or(""));
    CHECK(r->name == "hetzner");
    CHECK(r->host == "203.0.113.10");
    CHECK(r->user == "alice");
    CHECK(r->port == 2222);
    CHECK(r->key_file == "E:\\keys\\server.pem");
    CHECK(r->known_hosts_file == "C:\\Users\\alice\\.ssh\\known_hosts");

    auto wrong_type = parse_rclone_remote(*doc, "gdrive");
    REQUIRE_FALSE(wrong_type.has_value());
    CHECK(wrong_type.error().find("sftp") != std::string::npos);

    auto missing = parse_rclone_remote(*doc, "nope");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().find("hetzner") != std::string::npos); // lists available
}

TEST_CASE("rclone remote defaults and validation") {
    auto minimal = parse_rclone_remote(*ini::parse("[s]\ntype=sftp\nhost=h\nuser=u\n"), "s");
    REQUIRE(minimal.has_value());
    CHECK(minimal->port == 22);
    CHECK_FALSE(minimal->key_file.has_value());
    CHECK_FALSE(minimal->known_hosts_file.has_value());
    CHECK_FALSE(minimal->shell_type.has_value());
    CHECK_FALSE(minimal->md5sum_command.has_value());
    CHECK_FALSE(minimal->path_override.has_value());

    // What rclone saved of the server's shell and commands, read for grab's verification.
    auto saved = parse_rclone_remote(*ini::parse("[s]\ntype=sftp\nhost=h\nuser=u\nshell_type = unix\n"
                                                 "md5sum_command = md5sum\npath_override = @/volume1\n"),
                                     "s");
    REQUIRE(saved.has_value());
    CHECK(saved->shell_type == "unix");
    CHECK(saved->md5sum_command == "md5sum");
    CHECK(saved->path_override == "@/volume1");

    CHECK_FALSE(parse_rclone_remote(*ini::parse("[s]\ntype=sftp\nuser=u\n"), "s").has_value());
    CHECK_FALSE(parse_rclone_remote(*ini::parse("[s]\ntype=sftp\nhost=h\n"), "s").has_value());
    CHECK_FALSE(parse_rclone_remote(*ini::parse("[s]\ntype=sftp\nhost=h\nuser=u\nport=99999\n"), "s")
                    .has_value());
}

TEST_CASE("editor key and editor_command fallback chain") {
    CHECK(parse_ok("[grab]\neditor = code --wait\n[h]\n").editor == "code --wait");
    CHECK_FALSE(parse_ok("[grab]\neditor =\n[h]\n").editor.has_value());

    const auto file = util::path_from_utf8("C:\\Users\\alice\\AppData\\Roaming\\grab\\grab.conf");
    const auto f = util::path_to_utf8(file);

    CHECK(editor_command({"code --wait", "vim", ""}, file) ==
          std::vector<std::string>{"code", "--wait", f});
    CHECK(editor_command({"", "   ", "vim"}, file) == std::vector<std::string>{"vim", f});
    CHECK(editor_command({"\"C:\\Program Files\\Notepad++\\notepad++.exe\" -multiInst", "x"}, file) ==
          std::vector<std::string>{"C:\\Program Files\\Notepad++\\notepad++.exe", "-multiInst", f});

    const auto fallback = editor_command({}, file);
    REQUIRE(fallback.size() == 2);
    CHECK(fallback[1] == f);
#ifdef _WIN32
    CHECK(fallback[0] == "notepad");
#else
    CHECK(fallback[0] == "vi");
#endif
}

TEST_CASE("search_roots may be absolute or home-relative and are normalized") {
    const auto cfg = parse_ok("[h]\nsearch_roots = /home/, ., learning/, ~/tv, ~ , /\n");
    CHECK(cfg.remotes[0].search_roots ==
          std::vector<std::string>{"/home", "", "learning", "tv", "", "/"});
    CHECK(normalize_root("./x/") == "x");
    CHECK(normalize_root("~/") == "");
    CHECK(normalize_root("/") == "/");
}

TEST_CASE("find method: auto, ssh, or rclone (the same as walk)") {
    CHECK(parse_ok("[h]\n").remotes[0].find == FindMethod::auto_detect);
    CHECK(parse_ok("[h]\nfind = SSH\n").remotes[0].find == FindMethod::ssh);
    CHECK(parse_ok("[h]\nfind = rclone\n").remotes[0].find == FindMethod::rclone);
}

TEST_CASE("find method on grab's engine: auto asks the server, except a Storage Box; walk = rclone") {
    CHECK(parse_ok("[h]\nfind = walk\n").remotes[0].find == FindMethod::rclone);
    CHECK(parse_ok("[h]\nfind = Walk\n").remotes[0].find == FindMethod::rclone);
    auto bad = parse_grab_config(*ini::parse("[h]\nfind = telnet\n"));
    REQUIRE_FALSE(bad);
    CHECK(bad.error() == "[h] find must be auto, ssh, rclone or walk, got 'telnet'");

    RcloneRemote server;
    server.host = "65.21.224.123";
    RcloneRemote box;
    box.host = "u123456-sub1.Your-StorageBox.de";
    RemoteSettings s;
    CHECK(resolve_grab_method(s, server) == GrabMethod::probe); // key or password alike
    CHECK(resolve_grab_method(s, box) == GrabMethod::walk);
    s.find = FindMethod::ssh;
    CHECK(resolve_grab_method(s, server) == GrabMethod::find);
    CHECK(resolve_grab_method(s, box) == GrabMethod::find); // asked for: an error naming the answer
    s.find = FindMethod::rclone;
    CHECK(resolve_grab_method(s, server) == GrabMethod::walk);
}

TEST_CASE("known_hosts_file = none is treated as unset, never passed to ssh") {
    auto r = parse_rclone_remote(*ini::parse("[box]\ntype = sftp\nhost = h\nuser = u\nport = 23\n"
                                             "known_hosts_file = none\npass = obscured\n"),
                                 "box");
    REQUIRE(r.has_value());
    CHECK_FALSE(r->known_hosts_file.has_value());
    CHECK_FALSE(r->key_file.has_value());
    CHECK(r->port == 23);
}

namespace {
const char* const rclone_fixture = "[hetzner]\n"
                                   "type = sftp\n"
                                   "host = 203.0.113.10\n"
                                   "user = alice\n"
                                   "port = 2222\n"
                                   "key_file = E:\\keys\\server.pem\n"
                                   "key_file_pass = obscured\n"
                                   "[gdrive]\n"
                                   "type = drive\n"
                                   "[box]\n"
                                   "type = sftp\n"
                                   "host = box.example.com\n"
                                   "user = u1-sub1\n"
                                   "port = 23\n"
                                   "pass = obscured\n"
                                   "known_hosts_file = none\n"
                                   "[broken]\n"
                                   "type = sftp\n"
                                   "user = nohost\n";

void set_env(const char* name, const char* value) {
#ifdef _WIN32
    SetEnvironmentVariableW(util::to_wide(name).c_str(),
                            value ? util::to_wide(value).c_str() : nullptr);
#else
    if (value) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
}
} // namespace

TEST_CASE("generate_grab_config writes one ready-to-use section per usable sftp remote") {
    auto rclone = ini::parse(rclone_fixture);
    REQUIRE(rclone.has_value());
    const auto text = generate_grab_config(&*rclone, "C:\\Users\\alice\\rclone.conf");

    // Skipped remotes are named with a reason, not given sections.
    CHECK(text.find("C:\\Users\\alice\\rclone.conf") != std::string::npos);
    CHECK(text.find("gdrive (drive)") != std::string::npos);
    CHECK(text.find("broken (sftp, incomplete") != std::string::npos);
    CHECK(text.find("[gdrive]") == std::string::npos);
    CHECK(text.find("[broken]") == std::string::npos);
    // Lead-ins as grab's own client (the default) searches.
    CHECK(text.find("# server [hetzner]: alice@203.0.113.10:2222, key file, lookup with find, else by listing "
                    "folders\n[hetzner]\n") != std::string::npos);
    CHECK(text.find("# rclone remote [") == std::string::npos);
    CHECK(text.find("grab server add | list | edit | test | trust | remove") != std::string::npos);
    CHECK(text.find("u1-sub1@box.example.com:23, password, lookup with find, else by listing folders") !=
          std::string::npos);
    // No rclone flag lines; the connection keys and the fallback commented out (grab's defaults
    // apply), and `find = auto`, never walk (0.19 refuses it).
    CHECK(text.find("common_flags") == std::string::npos);
    CHECK(text.find("folder_flags") == std::string::npos);
    CHECK(text.find("# bind = ") != std::string::npos);
    CHECK(text.find("# connect_timeout = 30s\n") != std::string::npos);
    CHECK(text.find("# io_timeout = 30s\n") != std::string::npos);
    CHECK(text.find("# engine = rclone\n") != std::string::npos);
    CHECK(text.find("\nengine") == std::string::npos);
    CHECK(text.find("\nfind = walk") == std::string::npos);

    const auto cfg = parse_ok(text);
    CHECK(cfg.default_remote == "hetzner");
    REQUIRE(cfg.remotes.size() == 2);
    CHECK(cfg.remotes[0].name == "hetzner");
    CHECK(cfg.remotes[1].name == "box");
    for (const auto& r : cfg.remotes) {
        CHECK(r.rclone_remote == r.name);
        CHECK(r.find == FindMethod::auto_detect);
        CHECK(r.search_roots.empty()); // blank = login home
        CHECK(r.max_depth == 4);
        // Flag keys are commented out, so the built-in defaults apply.
        CHECK(r.common_flags == util::split_args(default_common_flags));
        CHECK(r.folder_flags == util::split_args(default_folder_flags));
        CHECK(r.file_flags == util::split_args(default_file_flags));
        CHECK(r.ssh_options.empty());
    }
}

TEST_CASE("generate_grab_config names a remote called grab's section in its lead-in, so edits and removal find it") {
    auto rclone = ini::parse("[grab]\ntype = sftp\nhost = 203.0.113.10\nuser = alice\n");
    REQUIRE(rclone.has_value());
    const auto text = generate_grab_config(&*rclone, "x");
    CHECK(text.find("# server [grab_remote]: alice@203.0.113.10:22") != std::string::npos);
    CHECK(text.find("# server [grab]:") == std::string::npos);
    const auto replaced = servers::replace_lead_in(text, "grab_remote", "# server [grab_remote]: replaced");
    CHECK(replaced.find("# server [grab_remote]: replaced\n[grab_remote]\n") != std::string::npos);
    CHECK(replaced.find("alice@203.0.113.10") == std::string::npos);
    const auto removed = servers::remove_section(text, "grab_remote");
    CHECK(removed.find("[grab_remote]") == std::string::npos); // the section and its lead-in
    CHECK(removed.find("alice@") == std::string::npos);
}

TEST_CASE("generate_grab_config without usable remotes is a [grab] block ready for `grab server add`") {
    for (const std::string& text : {generate_grab_config(nullptr, "x"),
                                    [] {
                                        auto only_drive = ini::parse("[gdrive]\ntype = drive\n");
                                        return generate_grab_config(&*only_drive, "x");
                                    }()}) {
        const auto cfg = parse_ok(text);
        CHECK(cfg.remotes.empty());
        CHECK_FALSE(cfg.default_remote.has_value());
        CHECK(cfg.rclone == "rclone"); // blank = bundled, else PATH
        CHECK_FALSE(cfg.rclone_config.has_value());
        CHECK(text.find("grab server add") != std::string::npos);
    }
}

TEST_CASE("the tests' settings are a scratch folder's: grab's files never resolve to the real ones") {
    // test_main.cpp points APPDATA, LOCALAPPDATA, HOME and USERPROFILE at a folder of this run
    // and clears RCLONE_CONFIG and GRAB_CONFIG before any test: every default path grab writes to
    // lands there (phase 7: the import writes grab.conf, the store, known_hosts, imported.json).
    const auto scratch = std::filesystem::temp_directory_path().lexically_normal();
    const auto inside = [&](const std::filesystem::path& p) {
        const auto text = util::to_lower(util::path_to_utf8(p.lexically_normal()));
        return text.starts_with(util::to_lower(util::path_to_utf8(scratch))) && text.find("grab-tests-home-") != std::string::npos;
    };
    CHECK(inside(util::config_home()));
    CHECK(inside(util::local_data_home()));
    CHECK(inside(default_grab_config_path()));
    CHECK(inside(grab_rclone_config_path()));
    CHECK(inside(grab_known_hosts_path()));
    CHECK(inside(standard_rclone_config_path()));
    CHECK(inside(state_dir(default_grab_config_path())));
    CHECK(inside(passwords::path_in(state_dir(default_grab_config_path()))));
    CHECK(inside(rclone_import::record_path(state_dir(default_grab_config_path()))));
    CHECK(inside(file_list::default_dir()));
    for (const char* name : {"HOME", "USERPROFILE"}) {
        const auto value = util::getenv_utf8(name);
        REQUIRE(value.has_value());
        CHECK(inside(util::path_from_utf8(*value)));
    }
    CHECK_FALSE(util::getenv_utf8("RCLONE_CONFIG").has_value());
    CHECK_FALSE(util::getenv_utf8("GRAB_CONFIG").has_value());
}

TEST_CASE("agent remotes say so in grab.conf's comment") {
    auto r = parse_rclone_remote(*ini::parse("[a]\ntype = sftp\nhost = h\nuser = u\nkey_use_agent = true\n"), "a");
    REQUIRE(r.has_value());
    CHECK(r->key_use_agent);
    CHECK(describe_remote(*r).find("ssh-agent") != std::string::npos);
}

TEST_CASE("a server's search method as lists and comments name it") {
    RcloneRemote key;
    key.user = "u";
    key.host = "h";
    key.key_file = "E:\\k.pem";
    RcloneRemote box;
    box.user = "u1";
    box.host = "u1.your-storagebox.de";
    box.port = 23;
    RemoteSettings s;
    CHECK(search_method(s, key) == "probe");
    CHECK(search_method(s, box) == "walk"); // a Storage Box isn't asked
    s.find = FindMethod::ssh;
    CHECK(search_method(s, box) == "find");
    s.find = FindMethod::rclone;
    CHECK(search_method(s, key) == "walk");
    // `engine = rclone` changes nothing: every server is searched by grab's own client.
    s.find = FindMethod::auto_detect;
    s.engine = Engine::rclone;
    CHECK(search_method(s, key) == "probe");

    // grab.conf's comment says the lookup as the caller resolved it; without one, the default
    // find setting's. No "via ssh + find" or "via rclone lsf" any more.
    CHECK(describe_remote(key) == "u@h:22, key file, lookup with find, else by listing folders");
    CHECK(describe_remote(box) == "u1@u1.your-storagebox.de:23, password, lookup by listing folders");
    RemoteSettings grab;
    grab.find = FindMethod::ssh;
    CHECK(describe_remote(key, search_method(grab, key)) == "u@h:22, key file, lookup with find");
    CHECK(describe_remote(key, "find", "key in grab's store") == "u@h:22, key in grab's store, lookup with find");
}

TEST_CASE("a remote named grab gets a non-colliding section name") {
    auto rclone = ini::parse("[grab]\ntype = sftp\nhost = h\nuser = u\n");
    REQUIRE(rclone.has_value());
    const auto cfg = parse_ok(generate_grab_config(&*rclone, "x"));
    REQUIRE(cfg.remotes.size() == 1);
    CHECK(cfg.remotes[0].name == "grab_remote");
    CHECK(cfg.remotes[0].rclone_remote == "grab");
    CHECK(cfg.default_remote == "grab_remote");
}

TEST_CASE("missing_remotes lists sftp remotes no section references") {
    auto rclone = ini::parse(rclone_fixture);
    REQUIRE(rclone.has_value());

    // A section named differently but pointing at `box` covers it.
    const auto cfg = parse_ok("[myserver]\nrclone_remote = box\n");
    const auto missing = missing_remotes(cfg, *rclone);
    REQUIRE(missing.size() == 1);
    CHECK(missing[0].name == "hetzner");

    const auto all = parse_ok("[hetzner]\n[b]\nrclone_remote = box\n");
    CHECK(missing_remotes(all, *rclone).empty());
}

TEST_CASE("encrypted rclone.conf is detected") {
    CHECK(is_encrypted_rclone_config("# Encrypted rclone configuration File\n\nRCLONE_ENCRYPT_V0:\nabc="));
    CHECK(is_encrypted_rclone_config("\xEF\xBB\xBF# Encrypted rclone configuration File\n"));
    CHECK(is_encrypted_rclone_config("RCLONE_ENCRYPT_V0:\nabc"));
    CHECK_FALSE(is_encrypted_rclone_config("[hetzner]\ntype = sftp\n"));
}

TEST_CASE("RCLONE_CONFIG overrides the default rclone.conf path") {
    const auto previous = util::getenv_utf8("RCLONE_CONFIG");
    set_env("RCLONE_CONFIG", "D:\\portable\\rclone.conf");
    CHECK(util::path_to_utf8(standard_rclone_config_path()) == "D:\\portable\\rclone.conf");
    set_env("RCLONE_CONFIG", nullptr);
    CHECK(standard_rclone_config_path().filename() == "rclone.conf");
    CHECK(standard_rclone_config_path().parent_path().filename() == "rclone");
    set_env("RCLONE_CONFIG", previous ? previous->c_str() : nullptr);
}

TEST_CASE("engine: grab's own client by default, rclone when asked or guarded, GRAB_ENGINE over every server") {
    const auto cfg = parse_ok("[a]\n[b]\nengine = grab\n[c]\nengine = RClone\n");
    REQUIRE(cfg.remotes.size() == 3);
    CHECK_FALSE(cfg.remotes[0].engine.has_value()); // absent: the default, resolved by effective_engine
    CHECK(cfg.remotes[1].engine == Engine::grab);
    CHECK(cfg.remotes[2].engine == Engine::rclone);
    CHECK_FALSE(parse_grab_config(*ini::parse("[h]\nengine = libssh\n")).has_value());

    const auto previous = util::getenv_utf8("GRAB_ENGINE");
    set_env("GRAB_ENGINE", nullptr);
    CHECK(effective_engine(cfg.remotes[0]) == Engine::grab); // the default since phase 8 (0.20)
    CHECK(effective_engine(cfg.remotes[1]) == Engine::grab);
    CHECK(effective_engine(cfg.remotes[2]) == Engine::rclone); // the fallback
    // A server grab's client can't serve as configured stays on rclone by itself; one that asks
    // for grab is refused where it's used (engine::ssh_target), never moved.
    CHECK(effective_engine(cfg.remotes[0], true) == Engine::rclone);
    CHECK(effective_engine(cfg.remotes[1], true) == Engine::grab);
    CHECK(effective_engine(cfg.remotes[2], true) == Engine::rclone);
    set_env("GRAB_ENGINE", "grab");
    CHECK(effective_engine(cfg.remotes[2]) == Engine::grab);
    CHECK(effective_engine(cfg.remotes[0], true) == Engine::grab);
    set_env("GRAB_ENGINE", "rclone");
    CHECK(effective_engine(cfg.remotes[0]) == Engine::rclone);
    CHECK(effective_engine(cfg.remotes[1]) == Engine::rclone);
    set_env("GRAB_ENGINE", "something else"); // ignored
    CHECK(effective_engine(cfg.remotes[0]) == Engine::grab);
    CHECK(effective_engine(cfg.remotes[2]) == Engine::rclone);
    set_env("GRAB_ENGINE", previous ? previous->c_str() : nullptr);
}

TEST_CASE("bind, connect_timeout and io_timeout: read, checked, blank when absent (phase 8)") {
    using std::chrono::milliseconds;
    const auto cfg = parse_ok("[a]\nbind = 10.0.0.2\nconnect_timeout = 1m30s\nio_timeout = 45s\n"
                              "[b]\nconnect_timeout = off\nio_timeout = 24h\nbind = fe80::1%eth0\n[c]\nbind =\n");
    REQUIRE(cfg.remotes.size() == 3);
    CHECK(cfg.remotes[0].bind == "10.0.0.2");
    CHECK(cfg.remotes[0].connect_timeout == milliseconds(90'000));
    CHECK(cfg.remotes[0].io_timeout == milliseconds(45'000));
    CHECK(cfg.remotes[1].connect_timeout == milliseconds(0)); // off: as long as TCP tries
    CHECK(cfg.remotes[1].io_timeout == milliseconds(86'400'000));
    CHECK(cfg.remotes[1].bind == "fe80::1%eth0");
    CHECK_FALSE(cfg.remotes[2].bind); // blank: not set, nothing added anywhere
    CHECK_FALSE(cfg.remotes[2].connect_timeout);
    CHECK_FALSE(cfg.remotes[2].io_timeout);
    CHECK(cfg.unknown_keys.empty());

    // A bad value fails the load as a bad max_depth does, naming the section and the key.
    auto error_of = [](std::string_view text) {
        const auto doc = ini::parse(text);
        REQUIRE(doc.has_value());
        const auto parsed = parse_grab_config(*doc);
        REQUIRE_FALSE(parsed.has_value());
        return parsed.error();
    };
    CHECK(error_of("[Main]\nio_timeout = abc\n") ==
          "[Main] io_timeout: not a time grab can read (such as 30s or 1m30s; 1s to 24h)");
    for (const char* io : {"off", "0", "500ms", "25h", "30d"}) { // never off: a stalled read couldn't be cancelled
        CHECK_MESSAGE(error_of(std::format("[Main]\nio_timeout = {}\n", io)).starts_with("[Main] io_timeout: "), io);
    }
    CHECK(error_of("[Main]\nconnect_timeout = soon\n") ==
          "[Main] connect_timeout: not a time grab can read (such as 30s, 1m30s or off)");
    CHECK(error_of("[Main]\nbind = -oProxyCommand=x\n") == "[Main] bind: the address can't start with -");
    CHECK(error_of("[Main]\nbind = 10.0.0.2 -x\n").starts_with("[Main] bind: "));
}

TEST_CASE("keys grab doesn't read are collected for a notice, never an error (N7)") {
    const auto cfg = parse_ok("[grab]\nrclone =\nsh = ssh\n[Main]\nhost = h\nconnect_timout = 10s\nrclone.skip_links = true\n"
                              "auth = agent\nuser = u\nbind = 10.0.0.2\n");
    CHECK(cfg.unknown_keys == std::vector<std::pair<std::string, std::string>>{{"grab", "sh"}, {"Main", "connect_timout"}});
}

TEST_CASE("the new keys sit beside the old ones, which keep their meaning for rclone and grab 0.19") {
    // 0.19.0's parse_grab_config (v0.19.0 config.cpp) reads only the keys it knew and skips any
    // other: it sees this section's common_flags as written, its bind lost (docs/doc.md says so,
    // in Going back to 0.19).
    const auto cfg = parse_ok("[Main]\nhost = h\nbind = 10.0.0.2\nconnect_timeout = 10s\nio_timeout = 1m\n"
                              "common_flags = --bind 10.0.0.2\n");
    REQUIRE(cfg.remotes.size() == 1);
    CHECK(cfg.remotes[0].common_flags == std::vector<std::string>{"--bind", "10.0.0.2"});
}

TEST_CASE("example config parses and matches the defaults") {
    const auto cfg = parse_ok(example_config);
    CHECK(cfg.default_remote == "hetzner");
    REQUIRE(cfg.remotes.size() == 1);
    CHECK(cfg.remotes[0].common_flags == util::split_args(default_common_flags));
    CHECK(cfg.remotes[0].folder_flags == util::split_args(default_folder_flags));
    CHECK(cfg.remotes[0].file_flags == util::split_args(default_file_flags));
    // Its server's connection details are grab.conf's own (phase 7), not an rclone remote's.
    auto doc = ini::parse(example_config);
    REQUIRE(doc);
    const auto* section = doc->find("hetzner");
    REQUIRE(section != nullptr);
    const auto c = server_conf::parse(*section);
    REQUIRE_MESSAGE(c.has_value(), c.error_or(""));
    REQUIRE(c->has_value());
    const auto& conn = c->value();
    CHECK(conn.host == "files.example.com");
    CHECK(conn.auth == server_conf::Auth::key);
    CHECK(std::string_view(example_config).find("grab's own rclone.conf") == std::string_view::npos);
    // grab's own client by default: the engine line and the connection keys commented out, no
    // rclone flag keys, `find = auto` (0.19 refuses walk).
    CHECK_FALSE(cfg.remotes[0].engine.has_value());
    CHECK_FALSE(cfg.remotes[0].bind.has_value());
    CHECK_FALSE(cfg.remotes[0].io_timeout.has_value());
    CHECK(cfg.remotes[0].find == FindMethod::auto_detect);
    CHECK(cfg.unknown_keys.empty());
    const std::string_view text = example_config;
    for (const char* line : {"\n# engine = rclone\n", "\n# bind = ", "\n# connect_timeout = 30s\n", "\n# io_timeout = 30s\n",
                             "\nfind = auto\n"}) {
        CHECK_MESSAGE(text.find(line) != std::string_view::npos, line);
    }
    for (const char* key : {"\ncommon_flags", "\nfolder_flags", "\nfile_flags", "\nssh_options", "\nengine"}) {
        CHECK_MESSAGE(text.find(key) == std::string_view::npos, key);
    }
}

TEST_CASE("grab.conf.example is the example built into grab, as shipped") {
    auto shipped = util::read_file(util::path_from_utf8(GRAB_SOURCE_DIR) / "grab.conf.example");
    REQUIRE_MESSAGE(shipped, shipped.error_or(""));
    std::erase(*shipped, '\r'); // a checkout may have made it CRLF
    CHECK(*shipped == example_config);
}

TEST_CASE("the settings lock has one holder at a time, and a thread may take it again") {
    auto first = lock_settings();
    REQUIRE(first.has_value());
    {
        auto again = lock_settings(); // the same thread: a change inside a change
        REQUIRE(again.has_value());
    }
    // Still held after the inner one ends: another thread (or grab) waits.
    std::atomic<bool> got{false};
    std::thread other([&] {
        auto lock = lock_settings();
        got = lock.has_value();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK_FALSE(got.load());
    { auto done = std::move(*first); } // let go
    other.join();
    CHECK(got.load());
}
TEST_CASE("each configuration keeps its window state and lists apart; the default one's stay put (G35, G219)") {
    const auto home = util::config_home() / "grab";
    CHECK_FALSE(config_folder_name(home / "grab.conf").has_value());
    CHECK(state_dir(home / "grab.conf") == home);
    CHECK(file_list::dir_for(home / "grab.conf") == file_list::default_dir());

    const auto work = std::filesystem::temp_directory_path() / "grab-test-configs" / "work" / "grab.conf";
    const auto name = config_folder_name(work);
    const std::string own = name.value_or("");
    CHECK(own.starts_with("work-"));
    CHECK(own.size() == std::string("work-").size() + 8);
    CHECK(config_folder_name(work.parent_path() / "other.conf") == name); // one per folder, as the window
    CHECK(config_folder_name(util::path_from_utf8(util::to_lower(util::path_to_utf8(work)))) == name);
    CHECK(config_folder_name(work.parent_path().parent_path() / "play" / "grab.conf") != name);
    CHECK(state_dir(work) == home / "configs" / util::path_from_utf8(own));
    CHECK(file_list::dir_for(work) == file_list::default_dir() / util::path_from_utf8(own));
    CHECK(config_key(work.parent_path()).size() == 16);
}