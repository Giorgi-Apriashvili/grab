#include "rclone_encoding.hpp"
#include "secrets.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
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