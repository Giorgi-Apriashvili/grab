#include "config.hpp"
#include "gui_state.hpp"
#include "ini.hpp"
#include "notices.hpp"
#include "passwords.hpp"
#include "rclone_import.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace grab;

namespace {

// `moved`: the servers the import moved, which N0 can name.
std::vector<notices::Notice> notices_of(std::string_view text, const notices::Extras& extras = {},
                                        const std::map<std::string, std::string>& variables = {},
                                        const notices::Moved& moved = {}) {
    const auto doc = ini::parse(text);
    REQUIRE(doc.has_value());
    const auto cfg = parse_grab_config(*doc);
    REQUIRE_MESSAGE(cfg.has_value(), cfg.error_or(""));
    return notices::settings_notices(*doc, *cfg, extras, [&variables](const std::string& name) -> std::optional<std::string> {
        const auto it = variables.find(name);
        if (it == variables.end()) return std::nullopt;
        return it->second;
    }, moved);
}

std::vector<std::string> texts_of(const std::vector<notices::Notice>& list) {
    std::vector<std::string> out;
    for (const auto& n : list) out.push_back(n.text);
    return out;
}

// A grab.conf section with connection details.
std::string section(std::string_view name, std::string_view more = {}) {
    return std::format("[{}]\nhost = {}.example\nuser = u\nauth = agent\n{}", name, name, more);
}

} // namespace

TEST_CASE("notices: the old defaults say nothing, flag by flag (grab.conf.example, --init)") {
    // Its server has no `engine` line, but no import moved it: no switch to tell of either.
    CHECK(notices_of(example_config).empty());
    CHECK(notices_of(generate_grab_config(nullptr, "")).empty());
    // The defaults written out, blank keys, `engine = grab`, and what --init wrote for 0.20.
    CHECK(notices_of(section("Main", std::format("common_flags = {}\nfolder_flags = {}\nfile_flags = {}\nssh_options =\n"
                                                 "engine = grab\n",
                                                 default_common_flags, default_folder_flags, default_file_flags)) +
                     "[grab]\nrclone =\nssh = ssh\n")
              .empty());
    // A default flag left out says nothing either, nor rclone and ssh as 0.20 ran them.
    CHECK(notices_of(section("Main", "common_flags = --sftp-chunk-size 255Ki\n") + "[grab]\nrclone = rclone.exe\nssh = SSH\n")
              .empty());
}

TEST_CASE("notices: the switch to grab's own client, once, naming the servers it moved, without the fallback (N0)") {
    // Main and Edge have no `engine` line; Box's proxy keeps grab off it (the guard says so), Old
    // asked for rclone (N9 says it's ignored), Opt chose grab before, Bare has no details yet, Bound
    // has a bind left in common_flags (refused wherever it's used).
    const std::string base = "[Main]\nhost = m.example\nuser = u\nauth = agent\n"
                             "[Box]\nhost = b.example\nuser = u\nauth = password\n" +
                             section("Old", "engine = rclone\n") + section("Opt", "engine = grab\n") +
                             "[Bare]\nmax_depth = 2\n" + section("Bound", "common_flags = --bind 10.0.0.2\n");
    notices::Extras extras{{"Box", {{"socks_proxy", "fake-user:fake-secret@127.0.0.1:9"}}}};
    // All of them came from rclone.conf (the import moved them); New, below, was added since.
    const notices::Moved moved{"Main", "Box", "Old", "Opt", "Bare", "Edge", "Bound"};
    const std::string edge = "[Edge]\nhost = e.example\nuser = u\nauth = agent\n";
    const auto n = notices_of(base + edge, extras, {}, moved);
    REQUIRE(n.size() == 3);
    CHECK(n[0].server == "grab");
    CHECK(n[0].key == "engine");
    CHECK(n[0].flag == "default");
    CHECK(n[0].digest == util::sha256_hex("grab")); // 0.20's id: whoever saw it there doesn't again
    CHECK(n[0].text == "Downloads, searches and lists now use grab's own SSH client, no longer rclone (Main, Edge); "
                       "their saved file lists are fetched again once.");
    CHECK(n[1].key == "guard");
    CHECK(n[1].server == "Box");
    CHECK(n[2].key == "ignored:engine");
    CHECK(n[2].server == "Old");

    // Once per configuration: a server added later doesn't bring it back, nor does an engine line.
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-notices-n0";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto file = notices::path_in(dir);
    auto fresh = notices::take_new(file, notices_of(base, extras, {}, moved));
    REQUIRE(fresh);
    REQUIRE_FALSE(fresh->empty());
    CHECK((*fresh)[0].text.find("(Main);") != std::string::npos);
    fresh = notices::take_new(file, n);
    REQUIRE(fresh);
    CHECK(fresh->empty());
    REQUIRE(notices::take_new(file, notices_of(section("Old", "engine = rclone\n"), {}, {}, moved)));
    fresh = notices::take_new(file, n);
    REQUIRE(fresh);
    REQUIRE(fresh->size() == 1);
    CHECK((*fresh)[0].key == "guard"); // Box's, which went and came back
    std::filesystem::remove_all(dir);

    // One added since never used rclone: nothing changed for it. GRAB_ENGINE changes nothing.
    const std::string added = "[New]\nhost = n.example\nuser = u\nauth = agent\n";
    CHECK(notices_of(added, {}, {}, moved).empty());
    const auto with_variable = notices_of(base, extras, {{"GRAB_ENGINE", "rclone"}}, moved);
    REQUIRE_FALSE(with_variable.empty());
    CHECK(with_variable[0].text.find("(Main);") != std::string::npos);
}

TEST_CASE("notices: engine = rclone, GRAB_ENGINE=rclone, [grab] rclone and ssh are ignored, said once (N9, N10, N14)") {
    CHECK(texts_of(notices_of(section("Box", "engine = rclone\n"))) ==
          std::vector<std::string>{"[Box] engine = rclone is ignored: this grab has no rclone any more, so Box downloads, "
                                   "searches and lists with grab's own client. If that fails where rclone worked, grab "
                                   "0.20.1 (GitHub releases) still has the fallback; please report it."});
    const auto variable = notices_of(section("Box"), {}, {{"GRAB_ENGINE", " RClone "}});
    REQUIRE(variable.size() == 1);
    CHECK(variable[0].server == "grab");
    CHECK(variable[0].key == "ignored:GRAB_ENGINE");
    CHECK(variable[0].text == "GRAB_ENGINE=rclone is ignored: this grab has no rclone; every server uses grab's own client.");
    CHECK(notices_of(section("Box"), {}, {{"GRAB_ENGINE", "grab"}}).empty());
    CHECK(texts_of(notices_of("[grab]\nrclone = C:\\Tools\\rclone.exe\nssh = C:\\spy\\ssh-spy.exe\n")) ==
          std::vector<std::string>{"[grab] rclone and ssh are ignored: this grab runs neither; it connects with its own "
                                   "SSH client."});
    CHECK(texts_of(notices_of("[grab]\nrclone = C:\\Tools\\rclone.exe\n")) ==
          std::vector<std::string>{"[grab] rclone is ignored: this grab doesn't run rclone; it connects with its own SSH "
                                   "client."});
    CHECK(texts_of(notices_of("[grab]\nssh = plink\n")) ==
          std::vector<std::string>{"[grab] ssh is ignored: this grab doesn't run ssh.exe; it connects with its own SSH "
                                   "client."});
    // Never the path: keyed on the text alone.
    CHECK(notices_of("[grab]\nrclone = C:\\a\\rclone.exe\n") == notices_of("[grab]\nrclone = D:\\b\\rclone.exe\n"));
}

TEST_CASE("notices: common_flags is ignored, with what to write instead (N11)") {
    const auto n = notices_of(section("Main", "common_flags = -P --timeout 2m --contimeout=10s --sftp-connections 4 "
                                              "--sftp-chunk-size 64Ki\n"));
    REQUIRE(n.size() == 1);
    CHECK(n[0].server == "Main");
    CHECK(n[0].key == "ignored:common_flags");
    CHECK(n[0].flag.empty());
    CHECK(n[0].digest == util::sha256_hex(n[0].text));
    CHECK(n[0].text == "[Main] common_flags is ignored: its flags were for rclone, which this grab no longer has "
                       "(--timeout 2m: write io_timeout = 2m; --contimeout 10s: connect_timeout = 10s; "
                       "--sftp-connections 4: max_connections = 4).");
    // Never a line grab.conf would refuse, nor one whose key is set already.
    CHECK(texts_of(notices_of(section("Main", "common_flags = --timeout off --sftp-connections 100\n"))) ==
          std::vector<std::string>{"[Main] common_flags is ignored: its flags were for rclone, which this grab no longer "
                                   "has (--timeout off: io_timeout takes 1s to 24h; --sftp-connections 100: "
                                   "max_connections takes 1 to 64)."});
    CHECK(texts_of(notices_of(section("Main", "io_timeout = 1m\ncommon_flags = --timeout 2m --contimeout 9s\n"))) ==
          std::vector<std::string>{"[Main] common_flags is ignored: its flags were for rclone, which this grab no longer "
                                   "has (--contimeout 9s: write connect_timeout = 9s)."});
    CHECK(texts_of(notices_of(section("Main", "common_flags = --low-level-retries 3\n"))) ==
          std::vector<std::string>{"[Main] common_flags is ignored: its flags were for rclone, which this grab no longer "
                                   "has."});
    // A bind there with no `bind` key isn't ignored but refused (engine::connect_settings), every
    // time: no notice. With a `bind` key it is ignored like the rest.
    CHECK(notices_of(section("Main", "common_flags = -P --bind 10.0.0.2\n")).empty());
    CHECK(texts_of(notices_of(section("Main", "bind = 10.0.0.3\ncommon_flags = --bind 10.0.0.2\n"))) ==
          std::vector<std::string>{"[Main] common_flags is ignored: its flags were for rclone, which this grab no longer "
                                   "has."});
    // A guard flag's notice is the guard's, never also this one, and no proxy is quoted.
    const auto proxied = notices_of(section("Box", "common_flags = --sftp-socks-proxy fake-user:fake-secret@p:1\n"));
    REQUIRE(proxied.size() == 1);
    CHECK(proxied[0].key == "guard");
    CHECK(proxied[0].text.find("fake-") == std::string::npos);
    // Another value is another text, so it shows again; 0.20's notices.json entry for --timeout
    // 2m (N1, keyed on the value) hides nothing.
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-notices-n11";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto file = notices::path_in(dir);
    REQUIRE(util::write_file_atomic(file, std::format(R"({{"shown":[{{"server":"Main","key":"common_flags","flag":"--timeout","sha256":"{}"}}]}})",
                                                      util::sha256_hex("2m")) + "\n"));
    const auto first = notices_of(section("Main", "common_flags = --timeout 2m\n"));
    auto fresh = notices::take_new(file, first);
    REQUIRE(fresh);
    CHECK(*fresh == first);
    fresh = notices::take_new(file, first);
    REQUIRE(fresh);
    CHECK(fresh->empty());
    fresh = notices::take_new(file, notices_of(section("Main", "common_flags = --timeout 3m\n")));
    REQUIRE(fresh);
    CHECK(fresh->size() == 1);
    std::filesystem::remove_all(dir);
}

TEST_CASE("notices: folder_flags and file_flags are ignored (N12)") {
    CHECK(texts_of(notices_of(section("Main", "folder_flags = --transfers 16 --checkers 8\nfile_flags = --bind 10.0.0.2\n"))) ==
          std::vector<std::string>{"[Main] folder_flags and file_flags are ignored: they were rclone flags (files at once: "
                                   "At once in grab's window; speed: --limit)."});
    CHECK(texts_of(notices_of(section("Main", "file_flags = -vv\n"))) ==
          std::vector<std::string>{"[Main] file_flags is ignored: its flags were for rclone (files at once: At once in "
                                   "grab's window; speed: --limit)."});
    // folder_flags' bind with no `bind` key is the refusal; anything else there is still ignored.
    CHECK(notices_of(section("Main", "folder_flags = --transfers 8 --checkers 8 --bind 10.0.0.2\n")).empty());
    CHECK(texts_of(notices_of(section("Main", "folder_flags = --transfers 4 --bind 10.0.0.2\n")))[0].starts_with(
        "[Main] folder_flags is ignored"));
    // A proxy there routed 0.19's folder downloads: the guard's notice, never also this one.
    const auto proxied = notices_of(section("Box", "folder_flags = --transfers 8 --checkers 8 --sftp-socks-proxy "
                                                   "fake-user:fake-secret@p:1\n"));
    REQUIRE(proxied.size() == 1);
    CHECK(proxied[0].key == "guard");
    CHECK(proxied[0].text.find("folder_flags") != std::string::npos);
    CHECK(proxied[0].text.find("fake-") == std::string::npos);
}

TEST_CASE("notices: an algorithm list grab shares something with is ignored, not the guard's") {
    // In common_flags it is one more rclone flag (N11)...
    const auto flagged = notices_of(section("Main", "common_flags = -P --sftp-ciphers aes256-gcm@openssh.com\n"));
    REQUIRE(flagged.size() == 1);
    CHECK(flagged[0].key == "ignored:common_flags");
    // ...and a rclone.<key> line or the store's extra gets its own (N15).
    notices::Extras extras{{"Main", {{"macs", "hmac-sha2-256"}}}};
    CHECK(texts_of(notices_of(section("Main", "rclone.ciphers = aes256-gcm@openssh.com\n"), extras)) ==
          std::vector<std::string>{"[Main] rclone.macs is ignored: grab's own client offers its own list.",
                                   "[Main] rclone.ciphers is ignored: grab's own client offers its own list."});
    // One that shares nothing is the guard's alone.
    const auto refused = notices_of(section("Main", "rclone.ciphers = fake-cipher\n"));
    REQUIRE(refused.size() == 1);
    CHECK(refused[0].key == "guard");
}

TEST_CASE("notices: ssh_options is ignored, a jump route connects directly (N13, decision 4)") {
    CHECK(texts_of(notices_of(section("Main", "ssh_options = -o ConnectTimeout=10 -o ServerAliveInterval=30 "
                                              "-o ServerAliveCountMax=3 -C\n"))) ==
          std::vector<std::string>{"[Main] ssh_options is ignored: grab no longer runs ssh.exe (-o ConnectTimeout=10: write "
                                   "connect_timeout = 10s; ServerAliveInterval: grab sends its own keepalives)."});
    CHECK(texts_of(notices_of(section("Main", "connect_timeout = 5s\nssh_options = -o ConnectTimeout=10\n"))) ==
          std::vector<std::string>{"[Main] ssh_options is ignored: grab no longer runs ssh.exe."});
    for (const auto& [options, route] : {std::pair{"-J fake-user@fake-jump", "-J"},
                                         std::pair{"-o ProxyJump=fake-jump", "-o ProxyJump"},
                                         std::pair{"-o \"ProxyCommand fake-proxy %h %p\"", "-o ProxyCommand"}}) {
        const auto n = notices_of(section("Main", std::format("ssh_options = {}\n", options)));
        REQUIRE(n.size() == 1);
        CHECK(n[0].key == "ignored:ssh_options");
        CHECK(n[0].text == std::format("[Main] ssh_options is ignored: grab no longer runs ssh.exe. Main's searches went "
                                       "through its {}; grab now connects to Main directly for them too, as Main's "
                                       "downloads always did. Deleting the ssh_options line silences this note.",
                                       route));
        CHECK(n[0].text.find("fake-") == std::string::npos);
    }
    CHECK(texts_of(notices_of(section("Main", "ssh_options = -o ProxyCommand=none\n"))) ==
          std::vector<std::string>{"[Main] ssh_options is ignored: grab no longer runs ssh.exe."});
    // Its bind with no `bind` key is the refusal: nothing else, nothing to say.
    CHECK(notices_of(section("Main", "ssh_options = -o BindAddress=10.0.0.2\n")).empty());
    CHECK(notices_of(section("Main", "ssh_options = -b 10.0.0.2\n")).empty());
    CHECK_FALSE(notices_of(section("Main", "ssh_options = -b 10.0.0.2 -C\n")).empty());
    CHECK_FALSE(notices_of(section("Main", "bind = 10.0.0.3\nssh_options = -b 10.0.0.2\n")).empty());
}

TEST_CASE("notices: rclone settings that changed what rclone did are ignored (N15), keys grab doesn't know (N7)") {
    notices::Extras extras{{"Main", {{"set_env", "FAKE=secret"}}}};
    CHECK(texts_of(notices_of(section("Main", "rclone.skip_links = true\nrclone.connections = 3\nrclone.chunk_size = 32Ki\n"
                                              "rclone.set_modtime = true\nrclone.idle_timeout = 5m\nconnect_timout = 10s\n") +
                                  "[grab]\nsh = ssh\n",
                              extras)) ==
          std::vector<std::string>{
              "[Main] rclone.set_env is ignored: rclone is gone, and grab's own client sets no environment on the server.",
              "[Main] rclone.connections = 3 is ignored: rclone is gone; to cap grab's connections to Main, set "
              "max_connections = 3 in [Main].",
              "[Main] rclone.idle_timeout = 5m is ignored: rclone is gone, and grab's own client closes idle connections "
              "itself.",
              "[Main] rclone.skip_links = true is ignored: rclone is gone, and grab's own client follows links.",
              "[Main] connect_timout isn't a grab.conf setting (ignored).",
              "[grab] sh isn't a grab.conf setting (ignored)."});
    const auto n = notices_of(section("Main", "max_connections = 4\nrclone.connections = 3\n"));
    REQUIRE(n.size() == 1);
    CHECK(n[0].key == "ignored:rclone.connections");
    CHECK(n[0].text == "[Main] rclone.connections = 3 is ignored: rclone is gone.");
}

TEST_CASE("notices: grab's store that can't be read is a guard notice for every server") {
    const auto doc = ini::parse(section("Box", "engine = rclone\n"));
    REQUIRE(doc.has_value());
    const auto cfg = parse_grab_config(*doc);
    REQUIRE(cfg.has_value());
    const auto n = notices::settings_notices(*doc, *cfg, {}, [](const std::string&) { return std::nullopt; }, {}, true);
    REQUIRE(n.size() == 2);
    CHECK(n[0].key == "ignored:engine");
    CHECK(n[1].key == "guard");
    CHECK(n[1].flag == "grab's store, which can't be read");
}

TEST_CASE("notices: what keeps grab off a server, named but never quoted") {
    notices::Extras extras{{"Box", {{"socks_proxy", "fake-user:fake-secret@127.0.0.1:9"}}}};
    const auto n = notices_of(section("Box", "common_flags = --sftp-http-proxy fake-secret-proxy:3128\n"), extras,
                              {{"RCLONE_SFTP_USE_FSTAT", "true"}});
    REQUIRE(n.size() == 3);
    for (const auto& one : n) {
        CHECK(one.key == "guard");
        CHECK(one.text.find("fake-") == std::string::npos);
        CHECK(one.text.find("127.0.0.1") == std::string::npos);
        CHECK(one.text.find("which grab can't, so it doesn't connect to Box") != std::string::npos);
        CHECK(one.text.find("engine = rclone") == std::string::npos);
        // Keyed on the text, never the value: notices.json is no guessing oracle for a proxy's password.
        CHECK(one.digest == util::sha256_hex(one.text));
    }
    CHECK(n[0].flag == "common_flags' --sftp-http-proxy");
    CHECK(n[1].flag == "rclone.socks_proxy in grab's store");
    CHECK(n[2].flag == "RCLONE_SFTP_USE_FSTAT");
}

TEST_CASE("notices: each shows once per configuration, again when its text changes (notices.json)") {
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-notices";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto file = notices::path_in(dir);
    const auto first = notices_of(section("Main", "engine = rclone\ncommon_flags = --sftp-chunk-size 64Ki\n"));
    REQUIRE(first.size() == 2);
    auto fresh = notices::take_new(file, first);
    REQUIRE(fresh);
    CHECK(*fresh == first);
    fresh = notices::take_new(file, first);
    REQUIRE(fresh);
    CHECK(fresh->empty()); // shown once
    // One that goes and comes back shows again: the file keeps the configuration's current ones.
    const auto one = notices_of(section("Main", "engine = rclone\n"));
    REQUIRE(notices::take_new(file, one));
    fresh = notices::take_new(file, first);
    REQUIRE(fresh);
    REQUIRE(fresh->size() == 1);
    CHECK((*fresh)[0].key == "ignored:common_flags");
    // A file that can't be read counts as empty.
    REQUIRE(util::write_file_atomic(file, "not json"));
    fresh = notices::take_new(file, first);
    REQUIRE(fresh);
    CHECK(fresh->size() == 2);
    // The command and the window take them under the settings lock: each shows in one of them.
    std::filesystem::remove(file);
    std::atomic<std::size_t> shown{0};
    std::vector<std::jthread> takers;
    for (int i = 0; i < 4; ++i) {
        takers.emplace_back([&] {
            if (auto got = notices::take_new(file, first)) shown += got->size();
        });
    }
    takers.clear();
    CHECK(shown == first.size());
    std::filesystem::remove_all(dir);
}

TEST_CASE("notices: a configuration's notices from its grab.conf, its store's extras and the environment") {
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-notices-config";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto conf = dir / "grab.conf";
    REQUIRE(util::write_file_atomic(conf, section("Box")));
    const passwords::Binding login{.user = "u", .host = "Box.example", .port = 22};
    std::filesystem::create_directories(state_dir(conf));
    REQUIRE(passwords::save(passwords::path_in(state_dir(conf)),
                            {{"Box", {{"rclone.socks_proxy", passwords::Secret{.binding = login, .value = "127.0.0.1:9"}}}}}));
    const auto n = notices::for_config(conf);
    REQUIRE_MESSAGE(n, n.error_or(""));
    REQUIRE(n->size() == 1);
    CHECK((*n)[0].flag == "rclone.socks_proxy in grab's store");
    // N0 names a server the import moved (imported.json), not one added in grab.conf since.
    REQUIRE(util::write_file_atomic(conf, section("Box") + "[Main]\nhost = m.example\nuser = u\nauth = agent\n" +
                                              "[New]\nhost = n.example\nuser = u\nauth = agent\n"));
    const auto without = notices::for_config(conf);
    REQUIRE_MESSAGE(without, without.error_or(""));
    CHECK(std::ranges::none_of(*without, [](const notices::Notice& x) { return x.key == "engine"; }));
    rclone_import::Record record;
    record.imported = {"Box", "Main"};
    REQUIRE(util::write_file_atomic(rclone_import::record_path(state_dir(conf)), rclone_import::to_json(record)));
    const auto with = notices::for_config(conf);
    REQUIRE_MESSAGE(with, with.error_or(""));
    REQUIRE(with->size() == 2);
    CHECK((*with)[0].key == "engine");
    CHECK((*with)[0].text.find("(Main);") != std::string::npos);
    // Not the default configuration: grab's own old files, if any, aren't its to say.
    CHECK(std::ranges::none_of(*with, [](const notices::Notice& x) { return x.key == "old files"; }));
    std::filesystem::remove_all(passwords::path_in(state_dir(conf)).parent_path());
    std::filesystem::remove_all(dir);
}
