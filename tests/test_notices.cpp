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

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (SetEnvironmentVariableW)
#else
#include <cstdlib> // setenv, unsetenv
#endif

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

// A grab.conf section with connection details, engine = grab unless `engine` says otherwise.
std::string section(std::string_view name, std::string_view more, std::string_view engine = "grab") {
    return std::format("[{}]\nhost = {}.example\nuser = u\nauth = agent\nengine = {}\n{}", name, name, engine, more);
}

// GRAB_ENGINE would decide which servers are grab's for the duration.
struct NoEngineOverride {
    std::optional<std::string> was = util::getenv_utf8("GRAB_ENGINE");
    NoEngineOverride() { set(nullptr); }
    ~NoEngineOverride() { set(was ? was->c_str() : nullptr); }
    NoEngineOverride(const NoEngineOverride&) = delete;
    NoEngineOverride& operator=(const NoEngineOverride&) = delete;
    static void set(const char* value) {
#ifdef _WIN32
        SetEnvironmentVariableW(L"GRAB_ENGINE", value != nullptr ? util::to_wide(value).c_str() : nullptr);
#else
        if (value != nullptr) setenv("GRAB_ENGINE", value, 1);
        else unsetenv("GRAB_ENGINE");
#endif
    }
};

} // namespace

TEST_CASE("notices: the old defaults say nothing, flag by flag (grab.conf.example, --init)") {
    const NoEngineOverride no_override;
    // Its server has no `engine` line, but no import moved it: no switch to tell of either.
    CHECK(notices_of(example_config).empty());
    CHECK(notices_of(generate_grab_config(nullptr, "")).empty());
    // The defaults written out on a grab server, and blank keys.
    CHECK(notices_of(section("Main", std::format("common_flags = {}\nfolder_flags = {}\nfile_flags = {}\nssh_options =\n",
                                                 default_common_flags, default_folder_flags, default_file_flags)))
              .empty());
    // A default flag left out says nothing either.
    CHECK(notices_of(section("Main", "common_flags = --sftp-chunk-size 255Ki\n")).empty());
}

TEST_CASE("notices: the switch to grab's own client, once, naming the servers it moved and the fallback (N0)") {
    const NoEngineOverride no_override;
    // Main and Edge have no `engine` line; Box's proxy keeps it on rclone (the guard says so),
    // Old asks for rclone, Opt chose grab before the switch, Bare has no details yet.
    const std::string base = "[Main]\nhost = m.example\nuser = u\nauth = agent\n"
                             "[Box]\nhost = b.example\nuser = u\nauth = password\n" +
                             section("Old", "", "rclone") + section("Opt", "") + "[Bare]\nmax_depth = 2\n";
    notices::Extras extras{{"Box", {{"socks_proxy", "fake-user:fake-secret@127.0.0.1:9"}}}};
    // All of them came from rclone.conf (the import moved them); New, below, was added since.
    const notices::Moved moved{"Main", "Box", "Old", "Opt", "Bare", "Edge"};
    const std::string edge = "[Edge]\nhost = e.example\nuser = u\nauth = agent\n";
    const auto n = notices_of(base + edge, extras, {}, moved);
    REQUIRE(n.size() == 2);
    CHECK(n[0].server == "grab");
    CHECK(n[0].key == "engine");
    CHECK(n[0].flag == "default");
    CHECK(n[0].digest == util::sha256_hex("grab"));
    CHECK(n[0].text == "Downloads, searches and lists now use grab's own SSH client, no longer rclone (Main, Edge); "
                       "their saved file lists are fetched again once. To go back to rclone for a server until a later "
                       "grab removes it, set engine = rclone in its section (grab config, or Open grab.conf in grab's "
                       "Settings).");
    // grab doesn't connect to Box, and says why.
    CHECK(n[1].key == "guard");
    CHECK(n[1].text.find("[Box] connects through a SOCKS proxy (rclone.socks_proxy in grab's store)") != std::string::npos);
    CHECK(n[1].text.find("so it doesn't connect to Box rather than go around it") != std::string::npos);

    // Once per configuration: a server added later doesn't bring it back.
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
    // Nor does engine = rclone tried on every server and taken out again (live on Main it showed
    // again): unlike the others, N0 stays shown once it was.
    REQUIRE(notices::take_new(file, notices_of(section("Old", "", "rclone"), {}, {}, moved)));
    fresh = notices::take_new(file, n);
    REQUIRE(fresh);
    REQUIRE(fresh->size() == 1);
    CHECK((*fresh)[0].key == "guard"); // Box's, which went and came back
    std::filesystem::remove_all(dir);

    // Nothing moved, nothing to say: every server on rclone by its own line, or for the run.
    CHECK(notices_of(section("Old", "", "rclone") + section("Opt", ""), {}, {}, moved).empty());
    // One added since never used rclone: nothing changed for it.
    const std::string added = "[New]\nhost = n.example\nuser = u\nauth = agent\n";
    const auto with_new = notices_of(base + edge + added, extras, {}, moved);
    REQUIRE_FALSE(with_new.empty());
    CHECK(with_new[0].text.find("(Main, Edge);") != std::string::npos);
    CHECK(notices_of(added, {}, {}, moved).empty());
    NoEngineOverride::set("rclone");
    CHECK(notices_of(base, {}, {}, moved).empty());
    // GRAB_ENGINE=grab (A/B tests): Box's proxy still keeps it out of N0; grab's client refuses it.
    NoEngineOverride::set("grab");
    const auto forced = notices_of(base, extras, {}, moved);
    REQUIRE_FALSE(forced.empty());
    CHECK(forced[0].text.find("(Main);") != std::string::npos);
    NoEngineOverride::set(nullptr);
}

TEST_CASE("notices: common_flags' --bind, --contimeout and --timeout on a grab server read as the keys (N1)") {
    const NoEngineOverride no_override;
    const auto n = notices_of(section("Main", "common_flags = -P --bind 10.0.0.2 --contimeout=10s --timeout 2m\n"));
    CHECK(texts_of(n) ==
          std::vector<std::string>{
              "[Main] common_flags: grab reads --bind 10.0.0.2 as `bind = 10.0.0.2` for now. Write that line in [Main] "
              "(grab config); common_flags is only for rclone and goes away with it.",
              "[Main] common_flags: grab reads --contimeout 10s as `connect_timeout = 10s` for now. Write that line in "
              "[Main] (grab config); common_flags is only for rclone and goes away with it.",
              "[Main] common_flags: grab reads --timeout 2m as `io_timeout = 2m` for now. Write that line in [Main] (grab "
              "config); common_flags is only for rclone and goes away with it."});
    CHECK(n[0].server == "Main");
    CHECK(n[0].key == "common_flags");
    CHECK(n[0].flag == "--bind");
    CHECK(n[0].digest == util::sha256_hex("10.0.0.2"));
    // With the key set, the key wins everywhere.
    CHECK(texts_of(notices_of(section("Main", "bind = 10.0.0.3\ncommon_flags = --bind 10.0.0.2\n"))) ==
          std::vector<std::string>{"[Main] common_flags: --bind 10.0.0.2 does nothing while [Main] has `bind`, which wins "
                                   "on every connection; take it out of common_flags."});
    // On engine = rclone they keep feeding rclone as they did: nothing to say.
    CHECK(notices_of(section("Main", "common_flags = --bind 10.0.0.2\n", "rclone")).empty());
    // A blank --bind is no bind: nothing to say.
    CHECK(notices_of(section("Main", "common_flags = --bind=\n")).empty());
    // A value grab.conf wouldn't take is never suggested as a line (it would stop grab.conf loading).
    for (const auto* flags : {"--timeout off", "--timeout=0", "--timeout 48h", "--timeout 500ms"}) {
        const auto refused = texts_of(notices_of(section("Main", std::format("common_flags = {}\n", flags))));
        REQUIRE(refused.size() == 1);
        CHECK(refused[0].find("grab's own client refuses --timeout") != std::string::npos);
        CHECK(refused[0].find("io_timeout takes 1s to 24h") != std::string::npos);
        CHECK(refused[0].find("io_timeout =") == std::string::npos);
    }
    CHECK(texts_of(notices_of(section("Main", "common_flags = --sftp-connections 100\n"))) ==
          std::vector<std::string>{"[Main] common_flags: --sftp-connections 100 is for rclone only; to cap grab's "
                                   "connections to Main, set max_connections (1 to 64) in [Main]."});
    CHECK(texts_of(notices_of(section("Main", "common_flags = --sftp-connections 8\n")))[0].find("max_connections = 8") !=
          std::string::npos);
    CHECK(texts_of(notices_of(section("Main", "folder_flags = --timeout 0\n"))) ==
          std::vector<std::string>{"[Main] folder_flags: --timeout 0 reaches only rclone's folder batch; for every "
                                   "connection write an `io_timeout` line (io_timeout takes 1s to 24h)."});
}

TEST_CASE("notices: grab's store that can't be read is a guard notice for every server") {
    const NoEngineOverride no_override;
    const auto doc = ini::parse(section("Box", "", "rclone"));
    REQUIRE(doc.has_value());
    const auto cfg = parse_grab_config(*doc);
    REQUIRE(cfg.has_value());
    const auto n = notices::settings_notices(*doc, *cfg, {}, [](const std::string&) { return std::nullopt; }, {}, true);
    REQUIRE(n.size() == 1);
    CHECK(n[0].key == "guard");
    CHECK(n[0].flag == "grab's store, which can't be read");
}

TEST_CASE("notices: rclone's other connection flags, ssh_options and rclone settings on a grab server (N2-N6)") {
    const NoEngineOverride no_override;
    const auto n = notices_of(
        section("Main", "common_flags = -P --sftp-chunk-size 64Ki --sftp-connections 4 --low-level-retries 3 --dscp CS1 "
                        "--sftp-idle-timeout 5m\n"
                        "ssh_options = -o ConnectTimeout=10 -o BindAddress=10.0.0.2 -o ServerAliveInterval=30 -C "
                        "-o Ciphers=aes128-ctr\n"
                        "folder_flags = --transfers 8 --checkers 8 --bind 10.0.0.2 --sftp-concurrency 8\n"
                        "file_flags = --contimeout 9s\n"
                        "rclone.skip_links = true\nrclone.connections = 3\nrclone.chunk_size = 32Ki\nrclone.set_modtime = "
                        "true\n"));
    // One notice a line.
    CHECK(util::join(texts_of(n), "\n") ==
          "[Main] common_flags: --sftp-chunk-size 64Ki is for rclone only; grab's own client reads 255 KiB at a time, "
          "64 in flight.\n"
          "[Main] common_flags: --sftp-connections 4 is for rclone only; to cap grab's connections to Main, set "
          "max_connections = 4 in [Main].\n"
          "[Main] common_flags: --low-level-retries 3 is for rclone only; grab retries a failed piece by itself.\n"
          "[Main] common_flags: --dscp is for rclone only; it isn't used over SFTP.\n"
          "[Main] common_flags: --sftp-idle-timeout 5m is for rclone only; grab's own client closes idle connections "
          "itself.\n"
          "[Main] folder_flags: --bind 10.0.0.2 reaches only rclone's folder batch; write `bind = 10.0.0.2` for every "
          "connection.\n"
          "[Main] folder_flags: --sftp-concurrency 8 reaches only rclone's folder batch.\n"
          "[Main] file_flags: --contimeout 9s reaches nothing (it was for --rclone, which is gone); write "
          "`connect_timeout = 9s` for every connection.\n"
          "[Main] ssh_options is for ssh.exe, which grab's own client doesn't use (-o ConnectTimeout=10: use "
          "connect_timeout = 10s).\n"
          "[Main] ssh_options is for ssh.exe, which grab's own client doesn't use (-o BindAddress=10.0.0.2: use bind "
          "= 10.0.0.2).\n"
          "[Main] ssh_options is for ssh.exe, which grab's own client doesn't use (-o ServerAliveInterval=30: grab "
          "sends its own keepalives).\n"
          "[Main] ssh_options is for ssh.exe, which grab's own client doesn't use (-C).\n"
          "[Main] ssh_options is for ssh.exe, which grab's own client doesn't use (-o Ciphers).\n"
          "[Main] rclone.connections = 3 is for rclone only; to cap grab's connections to Main, set max_connections "
          "= 3 in [Main].\n"
          "[Main] rclone.skip_links = true is for rclone only: grab's own client follows links.");
    // file_flags' connection flags reach no internal run on any engine; ssh_options and the rest
    // say nothing on engine = rclone.
    CHECK(texts_of(notices_of(section("Main", "file_flags = --bind 10.0.0.2\nssh_options = -o ConnectTimeout=10\n"
                                              "folder_flags = --bind 10.0.0.2\n",
                                      "rclone"))) ==
          std::vector<std::string>{"[Main] file_flags: --bind 10.0.0.2 reaches nothing (it was for --rclone, which is "
                                   "gone); write `bind = 10.0.0.2` for every connection."});
}

TEST_CASE("notices: flags only the --rclone copies used, on any engine (N8), and keys grab doesn't know (N7)") {
    const NoEngineOverride no_override;
    CHECK(texts_of(notices_of(section("Main", "folder_flags = --transfers 16 --checkers 8\nfile_flags = -vv\n"
                                              "connect_timout = 10s\n",
                                      "rclone") +
                              "[grab]\nsh = ssh\n")) ==
          std::vector<std::string>{
              "[Main] folder_flags: --transfers 16 was for --rclone, which is gone; grab doesn't use it (files at once: "
              "At once in grab's window).",
              "[Main] file_flags: -vv was for --rclone, which is gone; grab doesn't use it (grab's own -v shows what it "
              "runs).",
              "[Main] connect_timout isn't a grab.conf setting (ignored).", "[grab] sh isn't a grab.conf setting (ignored)."});
}

TEST_CASE("notices: what keeps grab's client off a server, on any engine, named but never quoted") {
    const NoEngineOverride no_override;
    notices::Extras extras{{"Box", {{"socks_proxy", "fake-user:fake-secret@127.0.0.1:9"}}}};
    const auto n = notices_of(section("Box", "common_flags = --sftp-http-proxy fake-secret-proxy:3128\n", "rclone"), extras,
                              {{"RCLONE_SFTP_USE_FSTAT", "true"}});
    REQUIRE(n.size() == 3);
    for (const auto& one : n) {
        CHECK(one.key == "guard");
        CHECK(one.text.find("fake-") == std::string::npos);
        CHECK(one.text.find("127.0.0.1") == std::string::npos);
        CHECK(one.text.find("which grab can't, so it doesn't connect to Box") != std::string::npos);
        CHECK(one.text.find("engine = rclone") == std::string::npos);
    }
    CHECK(n[0].flag == "common_flags' --sftp-http-proxy");
    // Keyed on the text, never the value: notices.json is no guessing oracle for a proxy's password.
    CHECK(n[0].digest == util::sha256_hex(n[0].text));
    CHECK(n[1].digest == util::sha256_hex(n[1].text));
    CHECK(n[1].flag == "rclone.socks_proxy in grab's store");
    CHECK(n[2].flag == "RCLONE_SFTP_USE_FSTAT");
    // A common_flags proxy isn't also an N2 on a grab server: the guard's notice says it.
    const auto on_grab = notices_of(section("Box", "common_flags = --sftp-socks-proxy p:1\n"));
    REQUIRE(on_grab.size() == 1);
    CHECK(on_grab[0].text.ends_with("take --sftp-socks-proxy out of common_flags in [Box] (grab config)"));
}

TEST_CASE("notices: each shows once per configuration, again when its value changes (notices.json)") {
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-notices";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto file = notices::path_in(dir);
    const NoEngineOverride no_override;
    const auto first = notices_of(section("Main", "common_flags = --bind 10.0.0.2 --sftp-chunk-size 64Ki\n"));
    REQUIRE(first.size() == 2);
    auto fresh = notices::take_new(file, first);
    REQUIRE(fresh);
    CHECK(*fresh == first);
    fresh = notices::take_new(file, first);
    REQUIRE(fresh);
    CHECK(fresh->empty()); // shown once
    // The file holds digests, never a value.
    const auto saved = util::read_file(file).value_or("");
    CHECK(saved.find("10.0.0.2") == std::string::npos);
    CHECK(saved.find(util::sha256_hex("10.0.0.2")) != std::string::npos);
    // Another value is another notice; the unchanged one stays quiet.
    const auto changed = notices_of(section("Main", "common_flags = --bind 10.0.0.3 --sftp-chunk-size 64Ki\n"));
    fresh = notices::take_new(file, changed);
    REQUIRE(fresh);
    REQUIRE(fresh->size() == 1);
    CHECK((*fresh)[0].flag == "--bind");
    // One that goes and comes back shows again: the file keeps the configuration's current ones.
    REQUIRE(notices::take_new(file, {}));
    fresh = notices::take_new(file, changed);
    REQUIRE(fresh);
    CHECK(fresh->size() == 2);
    // A file that can't be read counts as empty.
    REQUIRE(util::write_file_atomic(file, "not json"));
    fresh = notices::take_new(file, changed);
    REQUIRE(fresh);
    CHECK(fresh->size() == 2);
    // The command and the window take them under the settings lock: each shows in one of them.
    std::filesystem::remove(file);
    std::atomic<std::size_t> shown{0};
    std::vector<std::jthread> takers;
    for (int i = 0; i < 4; ++i) {
        takers.emplace_back([&] {
            if (auto got = notices::take_new(file, changed)) shown += got->size();
        });
    }
    takers.clear();
    CHECK(shown == changed.size());
    std::filesystem::remove_all(dir);
}

TEST_CASE("notices: a configuration's notices from its grab.conf, its store's extras and the environment") {
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-notices-config";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto conf = dir / "grab.conf";
    REQUIRE(util::write_file_atomic(conf, section("Box", "", "rclone")));
    const passwords::Binding login{.user = "u", .host = "Box.example", .port = 22};
    std::filesystem::create_directories(state_dir(conf));
    REQUIRE(passwords::save(passwords::path_in(state_dir(conf)),
                            {{"Box", {{"rclone.socks_proxy", passwords::Secret{.binding = login, .value = "127.0.0.1:9"}}}}}));
    const NoEngineOverride no_override;
    const auto n = notices::for_config(conf);
    REQUIRE_MESSAGE(n, n.error_or(""));
    REQUIRE(n->size() == 1);
    CHECK((*n)[0].flag == "rclone.socks_proxy in grab's store");
    // N0 names a server the import moved (imported.json), not one added in grab.conf since.
    REQUIRE(util::write_file_atomic(conf, section("Box", "", "rclone") + "[Main]\nhost = m.example\nuser = u\nauth = agent\n" +
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
    std::filesystem::remove_all(passwords::path_in(state_dir(conf)).parent_path());
    std::filesystem::remove_all(dir);
}
