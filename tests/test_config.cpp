#include "config.hpp"
#include "ini.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

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

TEST_CASE("find method: explicit, or auto from how the rclone remote authenticates") {
    CHECK(parse_ok("[h]\n").remotes[0].find == FindMethod::auto_detect);
    CHECK(parse_ok("[h]\nfind = SSH\n").remotes[0].find == FindMethod::ssh);
    CHECK(parse_ok("[h]\nfind = rclone\n").remotes[0].find == FindMethod::rclone);

    RcloneRemote with_key;
    with_key.key_file = "E:\\k.pem";
    RcloneRemote with_password; // no key_file: rclone.conf carries `pass`
    RemoteSettings automatic;
    CHECK(resolve_find_method(automatic, with_key) == FindMethod::ssh);
    CHECK(resolve_find_method(automatic, with_password) == FindMethod::rclone);

    RemoteSettings forced;
    forced.find = FindMethod::rclone;
    CHECK(resolve_find_method(forced, with_key) == FindMethod::rclone);
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
    CHECK(text.find("alice@203.0.113.10:2222, key file, lookup via ssh + find") != std::string::npos);
    CHECK(text.find("u1-sub1@box.example.com:23, password, lookup via rclone lsf") !=
          std::string::npos);

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

TEST_CASE("resolve_rclone_exe: bundled next to grab, else PATH, explicit path wins") {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "grab_test_bundle";
    fs::remove_all(dir);
    fs::create_directories(dir);
    GrabConfig cfg; // rclone = "rclone"
    CHECK(resolve_rclone_exe(cfg, dir) == "rclone"); // nothing bundled
#ifdef _WIN32
    const auto bundled = dir / "rclone.exe";
#else
    const auto bundled = dir / "rclone";
#endif
    std::ofstream(bundled) << "x";
    CHECK(resolve_rclone_exe(cfg, dir) == util::path_to_utf8(bundled));
    cfg.rclone = "";
    CHECK(resolve_rclone_exe(cfg, dir) == util::path_to_utf8(bundled));
    cfg.rclone = "RCLONE.EXE";
    CHECK(resolve_rclone_exe(cfg, dir) == util::path_to_utf8(bundled));
    cfg.rclone = "C:\\Tools\\rclone.exe";
    CHECK(resolve_rclone_exe(cfg, dir) == "C:\\Tools\\rclone.exe");
    fs::remove_all(dir);
}

TEST_CASE("import_rclone_remotes copies referenced sections verbatim, once, never touching the source") {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "grab_test_import";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const auto from = dir / "standard.conf";
    const auto to = dir / "grab" / "rclone.conf";
    const std::string standard = "[hetzner]\ntype = sftp\nhost = 203.0.113.10\nuser = alice\n"
                                 "key_file_pass = OBSCURED+/==\n\n[other]\ntype = drive\n";
    std::ofstream(from, std::ios::binary) << standard;

    auto r = import_rclone_remotes({"hetzner", "missing"}, from, to);
    CHECK(r.error.empty());
    CHECK(r.imported == std::vector<std::string>{"hetzner"});
    CHECK(r.not_found == std::vector<std::string>{"missing"});
    auto copied = ini::parse(*util::read_file(to));
    REQUIRE(copied.has_value());
    REQUIRE(copied->find("hetzner") != nullptr);
    CHECK(copied->find("hetzner")->get("key_file_pass") == "OBSCURED+/=="); // verbatim
    CHECK(copied->find("other") == nullptr);                               // not referenced
    CHECK(*util::read_file(from) == standard);                             // source untouched

    // Idempotent: nothing more to copy, file unchanged.
    const auto before = *util::read_file(to);
    auto again = import_rclone_remotes({"hetzner"}, from, to);
    CHECK(again.imported.empty());
    CHECK(*util::read_file(to) == before);

    // A later, second remote is appended without disturbing the first.
    std::ofstream(from, std::ios::app | std::ios::binary) << "[box]\ntype = sftp\nhost = h\nuser = u\n";
    auto more = import_rclone_remotes({"hetzner", "box"}, from, to);
    CHECK(more.imported == std::vector<std::string>{"box"});
    auto both = ini::parse(*util::read_file(to));
    CHECK(both->section_names() == std::vector<std::string>{"hetzner", "box"});
    fs::remove_all(dir);
}

TEST_CASE("agent remotes are searched over ssh") {
    auto r = parse_rclone_remote(*ini::parse("[a]\ntype = sftp\nhost = h\nuser = u\nkey_use_agent = true\n"), "a");
    REQUIRE(r.has_value());
    CHECK(r->key_use_agent);
    CHECK(resolve_find_method(RemoteSettings{}, *r) == FindMethod::ssh);
    CHECK(describe_remote(*r).find("ssh-agent") != std::string::npos);
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

TEST_CASE("example config parses and matches the defaults") {
    const auto cfg = parse_ok(example_config);
    CHECK(cfg.default_remote == "hetzner");
    REQUIRE(cfg.remotes.size() == 1);
    CHECK(cfg.remotes[0].common_flags == util::split_args(default_common_flags));
    CHECK(cfg.remotes[0].folder_flags == util::split_args(default_folder_flags));
    CHECK(cfg.remotes[0].file_flags == util::split_args(default_file_flags));
}
