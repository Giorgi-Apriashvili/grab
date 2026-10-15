#include "secrets.hpp"
#include "server_ops.hpp"
#include "servers.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

using namespace grab;
using secrets::Store;

namespace {

std::filesystem::path fresh_dir(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

void write(const std::filesystem::path& p, std::string_view text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

} // namespace

TEST_CASE("the secret store survives its saved form, encrypted") {
    const Store store{{"Box", {{"pass", "OBSCURED1"}}},
                      {"Box.Guest", {{"pass", "OB\tSCURED2"}}},
                      {"key-server", {{"key_file_pass", "PP"}, {"key_pem", "-----BEGIN\nKEY\n-----END"}}}};
    const auto back = secrets::from_text(secrets::to_text(store));
    REQUIRE(back.has_value());
    CHECK(*back == store);
    CHECK_FALSE(secrets::from_text("nope\n").has_value());
    CHECK_FALSE(secrets::from_text("grab-secrets 1\nBox\tpass\n").has_value()); // a field missing

    const auto dir = fresh_dir("grab-test-secrets");
    const auto file = dir / "secrets";
    CHECK(secrets::load(file) == Store{}); // none yet
    REQUIRE(secrets::save(file, store));
    CHECK(secrets::load(file) == store);
#ifdef _WIN32
    const auto raw = *util::read_file(file);
    CHECK(raw.starts_with("grab-secrets-sealed 1\n"));
    CHECK(raw.find("OBSCURED1") == std::string::npos);
    CHECK(raw.find("Box") == std::string::npos);
#endif
    write(file, secrets::to_text(store)); // unencrypted: not accepted
    CHECK_FALSE(secrets::load(file).has_value());
    std::filesystem::remove_all(dir);
}

TEST_CASE("secrets reach rclone as RCLONE_CONFIG_<REMOTE>_<KEY>") {
    CHECK(secrets::env_name("Box.Guest", "pass") == "RCLONE_CONFIG_BOX.GUEST_PASS");
    CHECK(secrets::env_name("my-box", "key_file_pass") == "RCLONE_CONFIG_MY_BOX_KEY_FILE_PASS");

    secrets::export_to_env({{"grabtest-a", {{"pass", "X1"}}}, {"grabtest-b", {{"pass", "X2"}}}});
    CHECK(util::getenv_utf8("RCLONE_CONFIG_GRABTEST_A_PASS") == "X1");
    CHECK(util::getenv_utf8("RCLONE_CONFIG_GRABTEST_B_PASS") == "X2");
    secrets::export_to_env({{"grabtest-a", {{"pass", "X3"}}}}); // b removed, a changed
    CHECK(util::getenv_utf8("RCLONE_CONFIG_GRABTEST_A_PASS") == "X3");
    CHECK_FALSE(util::getenv_utf8("RCLONE_CONFIG_GRABTEST_B_PASS").has_value());
    secrets::export_to_env({});
    CHECK_FALSE(util::getenv_utf8("RCLONE_CONFIG_GRABTEST_A_PASS").has_value());
}

TEST_CASE("secret lines leave rclone.conf, everything else stays") {
    const std::string conf = "# grab's servers\r\n"
                             "[Box]\r\n"
                             "type = sftp\r\n"
                             "host = u1.your-storagebox.de\r\n"
                             "pass = OBSCURED1\r\n"
                             "port = 23\r\n"
                             "\r\n"
                             "[keyed]\r\n"
                             "key_file = E:\\k.pem\r\n"
                             "key_file_pass=PP\r\n"
                             "; pass = not a value, a comment\r\n"
                             "[agent]\r\n"
                             "key_use_agent = true\r\n"
                             "pass =\r\n";
    const auto t = secrets::take_secrets(conf);
    CHECK(t.found == Store{{"Box", {{"pass", "OBSCURED1"}}}, {"keyed", {{"key_file_pass", "PP"}}}});
    CHECK(t.text == "# grab's servers\r\n"
                    "[Box]\r\n"
                    "type = sftp\r\n"
                    "host = u1.your-storagebox.de\r\n"
                    "port = 23\r\n"
                    "\r\n"
                    "[keyed]\r\n"
                    "key_file = E:\\k.pem\r\n"
                    "; pass = not a value, a comment\r\n"
                    "[agent]\r\n"
                    "key_use_agent = true\r\n");
    CHECK(secrets::take_secrets(t.text).found.empty());
}

TEST_CASE("prepare moves secrets out of rclone.conf once and hands them over") {
    const auto dir = fresh_dir("grab-test-prepare");
    const auto conf = dir / "rclone.conf";
    write(conf, "[grabtest-box]\ntype = sftp\nhost = h\npass = OBS\n");
    REQUIRE(secrets::prepare(conf).has_value());
    CHECK(*util::read_file(conf) == "[grabtest-box]\ntype = sftp\nhost = h\n");
    CHECK(secrets::load(secrets::path_beside(conf)) == Store{{"grabtest-box", {{"pass", "OBS"}}}});
    CHECK(util::getenv_utf8("RCLONE_CONFIG_GRABTEST_BOX_PASS") == "OBS");

    // A secret added to the file later (e.g. an import) joins the store; the others stay.
    write(conf, "[grabtest-box]\ntype = sftp\nhost = h\n[grabtest-two]\npass = OBS2\n");
    REQUIRE(secrets::prepare(conf).has_value());
    CHECK(secrets::load(secrets::path_beside(conf)) ==
          Store{{"grabtest-box", {{"pass", "OBS"}}}, {"grabtest-two", {{"pass", "OBS2"}}}});

#ifdef _WIN32
    // An unreadable store is an error while the file has nothing to fall back on...
    write(secrets::path_beside(conf), "grab-secrets-sealed 1\ngarbage");
    CHECK_FALSE(secrets::prepare(conf).has_value());
    // ...and is replaced by the secrets the file still has.
    write(conf, "[grabtest-box]\npass = NEW\n");
    REQUIRE(secrets::prepare(conf).has_value());
    CHECK(secrets::load(secrets::path_beside(conf)) == Store{{"grabtest-box", {{"pass", "NEW"}}}});
#endif
    secrets::export_to_env({});
    std::filesystem::remove_all(dir);
}

TEST_CASE("a server's secrets follow its login method") {
    using servers::Auth;
    Store store{{"Box", {{"pass", "OLD"}}}, {"Other", {{"pass", "KEEP"}}}};
    server_ops::apply_auth(store, "Box", Auth::password, std::nullopt); // blank: keeps the password
    CHECK(store.at("Box") == std::map<std::string, std::string>{{"pass", "OLD"}});
    server_ops::apply_auth(store, "Box", Auth::password, std::string("NEW"));
    CHECK(store.at("Box").at("pass") == "NEW");
    server_ops::apply_auth(store, "Box", Auth::key_file, std::string("PP")); // the password goes
    CHECK(store.at("Box") == std::map<std::string, std::string>{{"key_file_pass", "PP"}});
    server_ops::apply_auth(store, "Box", Auth::agent, std::nullopt);
    CHECK_FALSE(store.contains("Box"));
    server_ops::apply_auth(store, "New", Auth::password, std::nullopt); // nothing to keep
    CHECK_FALSE(store.contains("New"));
    CHECK(store.at("Other").at("pass") == "KEEP");
}
