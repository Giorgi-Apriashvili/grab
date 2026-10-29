#include "process.hpp"
#include "rclone_encoding.hpp"
#include "secrets.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
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

TEST_CASE("a server's settings reach rclone as RCLONE_CONFIG_<REMOTE>_<KEY>, upper case as Go makes it") {
    CHECK(secrets::env_name("Box.Guest", "pass") == "RCLONE_CONFIG_BOX.GUEST_PASS");
    // rclone keeps the remote's '-' (G6); only the key's become '_'.
    CHECK(secrets::env_name("my-box", "key_file_pass") == "RCLONE_CONFIG_MY-BOX_KEY_FILE_PASS");
    CHECK(secrets::env_name("my_box", "pass") == "RCLONE_CONFIG_MY_BOX_PASS");
    CHECK(secrets::env_name("box", "key-file-pass") == "RCLONE_CONFIG_BOX_KEY_FILE_PASS");
    CHECK(secrets::env_name("my box", "host") == "RCLONE_CONFIG_MY BOX_HOST");
    CHECK(secrets::env_prefix("Main") == "RCLONE_CONFIG_MAIN_");
    // Every letter by Go's simple upper-case mapping, which Windows' own ignoring of case doesn't
    // match for Georgian and the like (phase 7, chunk A): Mkhedruli becomes Mtavruli.
    CHECK(secrets::env_name("\u10E1\u10D4\u10E0\u10D5\u10D4\u10E0\u10D8", "pass") ==
          "RCLONE_CONFIG_\u1CA1\u1C94\u1CA0\u1C95\u1C94\u1CA0\u1C98_PASS");
    CHECK(secrets::env_prefix("\u0444\u0430\u0439\u043B\u044B") == "RCLONE_CONFIG_\u0424\u0410\u0419\u041B\u042B_");
}

TEST_CASE("Go's upper case: simple mappings of Unicode 15, as rclone 1.75.1 names a remote's variables") {
    using rclone_enc::go_upper;
    CHECK(go_upper("Main box-1.eu_x+y@z") == "MAIN BOX-1.EU_X+Y@Z");
    CHECK(go_upper("") == "");
    const std::vector<std::pair<std::string_view, std::string_view>> letters = {
        {"\u10D0\u10FA\u10FD\u10FF", "\u1C90\u1CBA\u1CBD\u1CBF"}, // Georgian Mkhedruli, both blocks
        {"\u10A0\u1C90", "\u10A0\u1C90"},                         // Asomtavruli, Mtavruli: upper already
        {"\u2D00", "\u10A0"},                                     // Nuskhuri
        {"\u017Ftore", "STORE"},                                  // long s
        {"stra\u00DFe", "STRA\u00DFE"},                           // sharp s: no simple mapping
        {"d\u0131al", "DIAL"},                                    // dotless i
        {"\u0130", "\u0130"},                                     // dotted capital I stays
        {"\u00B5\u00FF", "\u039C\u0178"},                         // micro sign, y diaeresis
        {"\u01C4\u01C5\u01C6", "\u01C4\u01C4\u01C4"},             // DZ with caron: upper, title, lower case
        {"\u03C3\u03C2\u0345", "\u03A3\u03A3\u0399"},             // sigma, final sigma, ypogegrammeni
        {"\u0101\u0100\u012F", "\u0100\u0100\u012E"},             // an upper/lower range by turns
        {"\u1F80\u1FB3", "\u1F88\u1FBC"},                         // Greek with ypogegrammeni: one letter
        {"\u0561\u0586", "\u0531\u0556"},                         // Armenian
        {"\u24D0\uFF41", "\u24B6\uFF21"},                         // circled, fullwidth
        {"\U00010428x", "\U00010400X"},                           // Deseret, outside the BMP
        {"\U0001E922", "\U0001E900"},                             // Adlam, the table's last range
        {"\uA7C1", "\uA7C0"},                                     // Unicode 14
        {"\u0264", "\u0264"},                                     // Unicode 16's mapping (U+A7CB): not in Go 1.26
        {"\u4E2D\u6587", "\u4E2D\u6587"},                         // no case
    };
    for (const auto& [in, out] : letters) CHECK_MESSAGE(go_upper(in) == out, in);
    // A byte that isn't UTF-8 is U+FFFD, as Go's range loop reads it.
    CHECK(go_upper("a\xFF" "b\xE1\x83") == "A\uFFFDB\uFFFD\uFFFD");
}

#ifdef _WIN32
TEST_CASE("rclone finds a remote given only in its run's environment, under any name grab gives it") {
    // The bundled rclone, one folder above this test program (build\<preset>\rclone.exe).
    const auto rclone = util::self_exe_path().parent_path().parent_path() / "rclone.exe";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(rclone, ec)) {
        MESSAGE("no bundled rclone.exe next to the build; skipped");
        return;
    }
    // An alias remote that only its run's environment defines, with its config in memory, as
    // engine::rclone_env gives a server: '-' (G6), Georgian (which Windows' ignoring of case
    // doesn't upper-case), a long s and a final sigma (phase 7, chunk A).
    const auto dir = fresh_dir("grab-test-env-name");
    std::filesystem::create_directories(dir / "target");
    write(dir / "target" / "found-it.txt", "x");
    for (const std::string name : {"grab-test-box", "\u10E1\u10D4\u10E0\u10D5\u10D4\u10E0\u10D8", "\u017Ftore",
                                   "\u03C3\u03C2 box"}) {
        proc::RunOptions quiet;
        quiet.detached = true;
        quiet.environment.set = {{secrets::env_name(name, "type"), "alias"},
                                 {secrets::env_name(name, "remote"), util::path_to_utf8(dir / "target")}};
        const auto ran = proc::run_capture(
            std::vector<std::string>{util::path_to_utf8(rclone), "lsf", name + ":", "--config", "NUL"}, quiet);
        REQUIRE(ran.has_value());
        CHECK_MESSAGE(ran->exit_code == 0, name, ": ", ran->err);
        CHECK_MESSAGE(ran->out.find("found-it.txt") != std::string::npos, name);
    }
    std::filesystem::remove_all(dir, ec);
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