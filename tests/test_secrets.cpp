#include "process.hpp"
#include "secrets.hpp"
#include "server_ops.hpp"
#include "servers.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (LockFileEx)
#endif

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

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
// The text of the store set aside in `dir` ("secrets.unreadable-<time>"), or "" when there is none.
std::string set_aside_in(const std::filesystem::path& dir) {
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (util::path_to_utf8(e.path().filename()).starts_with("secrets.unreadable-")) {
            return util::read_file(e.path()).value_or("");
        }
    }
    return {};
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
    // rclone keeps the remote's '-' (G6); only the key's become '_'.
    CHECK(secrets::env_name("my-box", "key_file_pass") == "RCLONE_CONFIG_MY-BOX_KEY_FILE_PASS");
    CHECK(secrets::env_name("my_box", "pass") == "RCLONE_CONFIG_MY_BOX_PASS");
    CHECK(secrets::env_name("box", "key-file-pass") == "RCLONE_CONFIG_BOX_KEY_FILE_PASS");

    secrets::export_to_env({{"grabtest-a", {{"pass", "X1"}}}, {"grabtest_b", {{"pass", "X2"}}}});
    CHECK(util::getenv_utf8("RCLONE_CONFIG_GRABTEST-A_PASS") == "X1");
    CHECK_FALSE(util::getenv_utf8("RCLONE_CONFIG_GRABTEST_A_PASS").has_value());
    CHECK(util::getenv_utf8("RCLONE_CONFIG_GRABTEST_B_PASS") == "X2");
    secrets::export_to_env({{"grabtest-a", {{"pass", "X3"}}}}); // b removed, a changed
    CHECK(util::getenv_utf8("RCLONE_CONFIG_GRABTEST-A_PASS") == "X3");
    CHECK_FALSE(util::getenv_utf8("RCLONE_CONFIG_GRABTEST_B_PASS").has_value());
    secrets::export_to_env({});
    CHECK_FALSE(util::getenv_utf8("RCLONE_CONFIG_GRABTEST-A_PASS").has_value());
}

#ifdef _WIN32
TEST_CASE("rclone itself finds what grab exports for a remote with '-' in its name (G6)") {
    // The bundled rclone, one folder above this test program (build\<preset>\rclone.exe).
    const auto rclone = util::self_exe_path().parent_path().parent_path() / "rclone.exe";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(rclone, ec)) {
        MESSAGE("no bundled rclone.exe next to the build; skipped");
        return;
    }
    // An alias remote whose folder comes only from the environment, as a password does.
    const auto dir = fresh_dir("grab-test-env-name");
    std::filesystem::create_directories(dir / "target");
    write(dir / "target" / "found-it.txt", "x");
    write(dir / "rclone.conf", "[grab-test-box]\ntype = alias\n");
    secrets::export_to_env({{"grab-test-box", {{"remote", util::path_to_utf8(dir / "target")}}}});
    proc::RunOptions quiet;
    quiet.detached = true;
    const auto ran = proc::run_capture(std::vector<std::string>{util::path_to_utf8(rclone), "lsf", "grab-test-box:",
                                                                "--config", util::path_to_utf8(dir / "rclone.conf")},
                                       quiet);
    secrets::export_to_env({});
    std::filesystem::remove_all(dir, ec);
    REQUIRE(ran.has_value());
    CHECK_MESSAGE(ran->exit_code == 0, ran->err);
    CHECK(ran->out.find("found-it.txt") != std::string::npos);
}
#endif

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
    CHECK(util::getenv_utf8("RCLONE_CONFIG_GRABTEST-BOX_PASS") == "OBS"); // rclone's name for it (G6)

    // A secret added to the file later (e.g. an import) joins the store; the others stay.
    write(conf, "[grabtest-box]\ntype = sftp\nhost = h\n[grabtest-two]\npass = OBS2\n");
    REQUIRE(secrets::prepare(conf).has_value());
    CHECK(secrets::load(secrets::path_beside(conf)) ==
          Store{{"grabtest-box", {{"pass", "OBS"}}}, {"grabtest-two", {{"pass", "OBS2"}}}});

#ifdef _WIN32
    // An unreadable store is an error while the file has nothing to fall back on...
    write(secrets::path_beside(conf), "grab-secrets-sealed 1\ngarbage");
    CHECK_FALSE(secrets::prepare(conf).has_value());
    // ...and the secrets the file still has start a new one, the old file set aside (G211).
    write(conf, "[grabtest-box]\npass = NEW\n");
    REQUIRE(secrets::prepare(conf).has_value());
    CHECK(secrets::load(secrets::path_beside(conf)) == Store{{"grabtest-box", {{"pass", "NEW"}}}});
    CHECK(set_aside_in(dir) == "grab-secrets-sealed 1\ngarbage");
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

TEST_CASE("a change of an undecryptable store sets it aside; one that can't be read waits (G211)") {
    const auto dir = fresh_dir("grab-test-secrets-change");
    const auto file = dir / "secrets";
    auto none = secrets::load_for_change(file);
    REQUIRE(none.has_value());
    CHECK(none->store.empty());
    CHECK_FALSE(none->set_aside.has_value());

    const Store store{{"Box", {{"pass", "OBS"}}}};
    REQUIRE(secrets::save(file, store));
    auto good = secrets::load_for_change(file);
    REQUIRE(good.has_value());
    CHECK(good->store == store);
    CHECK_FALSE(good->set_aside.has_value());

    write(file, "grab-secrets-sealed 1\nanother account's, or damaged");
    auto damaged = secrets::load_for_change(file);
    REQUIRE(damaged.has_value());
    CHECK(damaged->store.empty());
    REQUIRE(damaged->set_aside.has_value());
    CHECK_FALSE(std::filesystem::exists(file));
    CHECK(set_aside_in(dir) == "grab-secrets-sealed 1\nanother account's, or damaged");

#ifdef _WIN32
    // Can't be read at all for the moment (another program's lock): an error, the file left alone.
    REQUIRE(secrets::save(file, store));
    const HANDLE other = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(other != INVALID_HANDLE_VALUE);
    OVERLAPPED at{};
    REQUIRE(LockFileEx(other, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 100000, 0, &at));
    CHECK_FALSE(secrets::load_for_change(file).has_value());
    UnlockFileEx(other, 0, 100000, 0, &at);
    CloseHandle(other);
    CHECK(secrets::load(file) == store);
#endif
    std::filesystem::remove_all(dir);
}
TEST_CASE("reveal: rclone's obscured values, made plain") {
    // Made with `rclone obscure -` (rclone 1.75.1); each has its own random IV.
    CHECK(secrets::reveal("gLZBxgITy_HxlUNmOsLeEByAksnrlA") == "potato");
    CHECK(secrets::reveal("SQVNqPZeUEyKRI-mMCI1_q10s7JJ37ILmZMGTNXvhLHgK8mjn-hU5Ae3JHRuRK3I3cA") ==
          "p\xC3\xA4ssw\xC3\xB6rd with spaces & \xC3\xBCn\xC3\xAF" "code");
    CHECK(secrets::reveal("GhD9RfOnFe2AioBbLF8h2w") == "");
    CHECK_FALSE(secrets::reveal("not base64!"));
    CHECK_FALSE(secrets::reveal("c2hvcnQ")); // shorter than an IV
    CHECK_FALSE(secrets::reveal("gLZBxgITy_HxlUNmOsLeEB+AksnrlA")); // '+' isn't URL-safe base64
}