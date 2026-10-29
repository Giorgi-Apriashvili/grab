// The bundled rclone with a remote given only in its environment and its config in memory
// (`--config NUL`), as grab starts it once grab.conf holds the servers (RCLONE-REPLACEMENT.md,
// Phase 7 step 1 and decision 1: each rclone process gets its one server's details in its own
// environment), and rclone's config reader as the oracle for the import of an rclone.conf. Each
// case runs the rclone.exe one folder above this test program with exactly the environment it
// gives it (no inherited RCLONE_* variable) and is skipped without one. Nothing here talks to a
// server: remotes are alias remotes on a temp folder, or sftp remotes on a closed 127.0.0.1 port,
// where a key or password rclone refuses fails before the connection and a refused dial
// ("127.0.0.1:<port>: connectex", Go's syscall name, never translated) means rclone took them.
// Every key and password is made up here.

#include "config.hpp"
#include "engine.hpp"
#include "json.hpp"
#include "passwords.hpp"
#include "process.hpp"
#include "quote.hpp"
#include "rclone.hpp"
#include "rclone_conf.hpp"
#include "secrets.hpp"
#include "server_conf.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <cstddef>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h> // IWYU pragma: keep (the socket API)

#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)

using namespace grab;
namespace fs = std::filesystem;

namespace {

using Vars = std::vector<std::pair<std::string, std::string>>;

std::optional<fs::path> bundled_rclone() {
    auto rclone = util::self_exe_path().parent_path().parent_path() / "rclone.exe";
    std::error_code ec;
    if (!fs::is_regular_file(rclone, ec)) return std::nullopt;
    return rclone;
}

// A fresh folder in %TEMP%, gone again with the object.
class Scratch {
public:
    explicit Scratch(std::string_view name) : dir_(fs::temp_directory_path() / util::path_from_utf8(name)) {
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_);
    }
    ~Scratch() {
        std::error_code ec;
        try {
            fs::remove_all(dir_, ec);
        } catch (...) { // only std::bad_alloc, which a destructor must not throw
            std::fputs("grab_tests: a temp folder was left behind\n", stderr);
        }
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    [[nodiscard]] const fs::path& dir() const { return dir_; }
    [[nodiscard]] fs::path file(std::string_view name) const { return dir_ / util::path_from_utf8(name); }
    // Writes `text` as it is (bytes).
    void write(std::string_view name, const std::string& text) const {
        REQUIRE(util::write_file_atomic(file(name), text).has_value());
    }

private:
    fs::path dir_;
};

struct Ran {
    int exit_code = -1;
    std::string out;
    std::string err;
};

// rclone with `args`, its environment grab's own without any RCLONE_* variable plus `vars`, as a
// server's variables reach it; stdin on NUL, stdout and stderr through files in `scratch`.
Ran run_rclone(const fs::path& rclone, const Scratch& scratch, std::vector<std::string> args, const Vars& vars = {}) {
    std::wstring env = proc::environment_without_rclone();
    while (!env.empty() && env.back() == L'\0') env.pop_back();
    if (!env.empty()) env.push_back(L'\0');
    for (const auto& [name, value] : vars) {
        env += util::to_wide(name + "=" + value);
        env.push_back(L'\0');
    }
    env.push_back(L'\0');

    args.insert(args.begin(), util::path_to_utf8(rclone));
    std::wstring cmdline = util::to_wide(quote::windows_cmdline(args));
    const auto out_file = scratch.dir() / "rclone-stdout.txt";
    const auto err_file = scratch.dir() / "rclone-stderr.txt";
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const auto open = [&](const fs::path& p, DWORD access, DWORD how) {
        return CreateFileW(p.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, how, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    };
    HANDLE in = open(L"NUL", GENERIC_READ, OPEN_EXISTING);
    HANDLE out = open(out_file, GENERIC_WRITE, CREATE_ALWAYS);
    HANDLE err = open(err_file, GENERIC_WRITE, CREATE_ALWAYS);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in;
    si.hStdOutput = out;
    si.hStdError = err;
    PROCESS_INFORMATION pi{};
    Ran ran;
    if (CreateProcessW(rclone.c_str(), cmdline.data(), nullptr, nullptr, TRUE,
                       CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, env.data(), nullptr, &si, &pi)) {
        if (WaitForSingleObject(pi.hProcess, 60000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        ran.exit_code = static_cast<int>(code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    for (HANDLE h : {in, out, err}) {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    ran.out = util::read_file(out_file).value_or("");
    ran.err = util::read_file(err_file).value_or("");
    return ran;
}

// A port on 127.0.0.1 nothing listens on: bound, then closed again.
int closed_port() {
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int length = sizeof(at);
    int port = 0;
    if (::bind(s, reinterpret_cast<const sockaddr*>(&at), sizeof(at)) == 0 &&
        ::getsockname(s, reinterpret_cast<sockaddr*>(&at), &length) == 0) {
        port = ntohs(at.sin_port);
    }
    closesocket(s);
    WSACleanup();
    return port;
}

// secrets::obscure, which has to work for these cases to mean anything.
std::string obscured(std::string_view plain) {
    auto text = secrets::obscure(plain);
    REQUIRE(text.has_value());
    return *text;
}

// A made-up private key in PEM, as OpenSSL writes it; `pass`: encrypted the old way (DEK-Info),
// the form Go's ssh package opens with a passphrase.
std::string made_up_key(const char* type, const char* curve, const std::string& pass = {}) {
    EVP_PKEY* pkey = curve != nullptr ? EVP_PKEY_Q_keygen(nullptr, nullptr, type, curve)
                                      : EVP_PKEY_Q_keygen(nullptr, nullptr, type);
    REQUIRE(pkey != nullptr);
    BIO* bio = BIO_new(BIO_s_mem());
    const auto* p = reinterpret_cast<const unsigned char*>(pass.data());
    const int written = pass.empty() ? PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr)
                                     : PEM_write_bio_PrivateKey_traditional(bio, pkey, EVP_aes_128_cbc(), p,
                                                                            static_cast<int>(pass.size()), nullptr,
                                                                            nullptr);
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    std::string pem(data, static_cast<std::size_t>(size));
    BIO_free(bio);
    EVP_PKEY_free(pkey);
    REQUIRE(written == 1);
    return pem;
}

// A PEM on one line with "\n" for its line breaks, as rclone.conf holds a key_pem.
std::string one_line(std::string_view pem) {
    std::string s;
    for (const char c : pem) {
        if (c == '\n') s += "\\n";
        else if (c != '\r') s += c;
    }
    while (s.ends_with("\\n")) s.resize(s.size() - 2);
    return s;
}

using Sections = std::map<std::string, std::map<std::string, std::string>>;

// `rclone config dump` of an rclone.conf holding `text`: section -> key -> value as rclone reads
// them, or rclone's message when it can't load the file.
std::expected<Sections, std::string> dump(const fs::path& rclone, const Scratch& scratch, std::string_view text) {
    scratch.write("dump.conf", std::string(text));
    const auto ran = run_rclone(rclone, scratch, {"config", "dump", "--config", util::path_to_utf8(scratch.file("dump.conf")),
                                                  "--ask-password=false"});
    if (ran.exit_code != 0) return std::unexpected(ran.err);
    const auto doc = json::parse(ran.out);
    REQUIRE(doc.has_value());
    REQUIRE(doc->kind == json::Value::Kind::object);
    Sections sections;
    for (std::size_t i = 0; i < doc->keys.size(); ++i) {
        auto& section = sections[doc->keys[i]];
        const auto& values = doc->items[i];
        for (std::size_t k = 0; k < values.keys.size(); ++k) section[values.keys[k]] = values.items[k].str;
    }
    return sections;
}

// An alias remote's listing of the folder it points at, or nullopt when rclone fails.
std::optional<std::string> listing(const Ran& ran) {
    if (ran.exit_code != 0) return std::nullopt;
    std::string s = ran.out;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

} // namespace

TEST_CASE("the bundled rclone finds a remote given only in its environment, under Go's upper-case name") {
    const auto rclone = bundled_rclone();
    if (!rclone) {
        MESSAGE("no bundled rclone.exe next to the build; skipped");
        return;
    }
    const Scratch scratch("grab-test-rclone-env-names");
    fs::create_directories(scratch.dir() / "target");
    scratch.write("target/marker.txt", "x");
    const auto target = util::path_to_utf8(scratch.dir() / "target");
    // rclone looks for RCLONE_CONFIG_<strings.ToUpper(name)>_<KEY>: Go's simple upper-case mapping
    // of every letter (Unicode 15 in rclone 1.75.1's Go), '.', '-', ' ' and the rest kept. Windows
    // matches variable names ignoring case only by its own, older table (ó and Cyrillic, but not
    // Georgian or the rest below; phase7\main-results.txt), so grab has to write Go's form.
    const std::vector<std::pair<std::string_view, std::string_view>> names = {
        {"Main", "MAIN"},
        {"box.eu", "BOX.EU"},
        {"my-box", "MY-BOX"},
        {"my box", "MY BOX"},
        {"box_1+x@y", "BOX_1+X@Y"},
        {"B\u00F3x", "B\u00D3X"},
        {"\u044F\u0449\u0438\u043A", "\u042F\u0429\u0418\u041A"},
        {"\u10E1\u10D4\u10E0\u10D5\u10D4\u10E0\u10D8", "\u1CA1\u1C94\u1CA0\u1C95\u1C94\u1CA0\u1C98"}, // Mkhedruli
        {"\u017Ftore", "STORE"},                                                                       // long s
        {"stra\u00DFe", "STRA\u00DFE"},                                                                // ß stays
        {"d\u0131al", "DIAL"},                                                                         // dotless i
        {"\u00B5box", "\u039CBOX"},                                                                    // micro sign
        {"\u01C5emal", "\u01C4EMAL"},                                                                  // title case
        {"\u03C3\u03C2", "\u03A3\u03A3"},                                                              // final sigma
        {"\U00010428x", "\U00010400X"},                                                                // Deseret
    };
    for (const auto& [name, upper] : names) {
        const std::string prefix = "RCLONE_CONFIG_" + std::string(upper) + "_";
        const auto ran = run_rclone(*rclone, scratch, {"lsf", std::string(name) + ":", "--config", "NUL"},
                                    {{prefix + "TYPE", "alias"}, {prefix + "REMOTE", target}});
        CHECK_MESSAGE(listing(ran) == "marker.txt", name, " ", ran.err);
    }
    // A name rclone takes for no remote at all, in a file or not: a combining accent (NFD).
    const auto nfd = run_rclone(*rclone, scratch, {"lsf", "cafe\u0301:", "--config", "NUL"},
                                {{"RCLONE_CONFIG_CAFE\u0301_TYPE", "alias"}, {"RCLONE_CONFIG_CAFE\u0301_REMOTE", target}});
    CHECK(nfd.exit_code != 0);
    CHECK(nfd.err.find("config name contains invalid characters") != std::string::npos);
}

TEST_CASE("rclone's config in memory: --config NUL says nothing, a lower-case nul is refused") {
    const auto rclone = bundled_rclone();
    if (!rclone) {
        MESSAGE("no bundled rclone.exe next to the build; skipped");
        return;
    }
    const Scratch scratch("grab-test-rclone-env-nul");
    fs::create_directories(scratch.dir() / "target");
    scratch.write("target/marker.txt", "x");
    const Vars vars{{"RCLONE_CONFIG_MAIN_TYPE", "alias"},
                    {"RCLONE_CONFIG_MAIN_REMOTE", util::path_to_utf8(scratch.dir() / "target")}};
    const auto ran = run_rclone(*rclone, scratch, {"lsf", "Main:", "--config", "NUL"}, vars);
    CHECK(listing(ran) == "marker.txt");
    CHECK(ran.err.empty()); // no "Config file not found" notice
    // Go's os.DevNull is "NUL" exactly; any other spelling is a file name Windows reserves.
    const auto lower = run_rclone(*rclone, scratch, {"lsf", "Main:", "--config", "nul"}, vars);
    CHECK(lower.exit_code != 0);
    CHECK(lower.err.find("reserved windows device name") != std::string::npos);
}

TEST_CASE("rclone's config reader, as `rclone config dump` shows it: the import's oracle") {
    const auto rclone = bundled_rclone();
    if (!rclone) {
        MESSAGE("no bundled rclone.exe next to the build; skipped");
        return;
    }
    const Scratch scratch("grab-test-rclone-env-dump");
    // Values: only backticks and triple double quotes are taken off; '=' or ':' ends the key;
    // a '#' or ';' after a value is part of it. %(key)s is DEFAULT's key if DEFAULT has it (even
    // blank), else this section's own raw value (never the parent's), else empty; the name is cut
    // with the cutsets "%(" and ")s", so %(pass)s reads key "pa"; substitution repeats up to 200
    // passes (chunk C: cap the expanded size or refuse a self-referencing value).
    const auto q = dump(*rclone, scratch,
                        "[q]\n"
                        "type = alias\n"
                        "dq = \"a b\"\n"
                        "sq = 'c d'\n"
                        "bq = `e f`\n"
                        "tq = \"\"\"g h\"\"\"\n"
                        "spaced =    padded value   \n"
                        "colon : by colon\n"
                        "eqcolon = a:b=c\n"
                        "hash = v # not a comment\n"
                        "semi = v ; not a comment\n"
                        "empty =\n"
                        "key with spaces = v\n"
                        "\"quoted key\" = v\n"
                        "sub = %(spaced)s/x\n"
                        "nosub = %(nosuch)s/x\n"
                        "back = C:\\keys\\id_rsa\n");
    REQUIRE_MESSAGE(q.has_value(), q.error());
    const auto& s = q->at("q");
    CHECK(s.at("dq") == "\"a b\"");
    CHECK(s.at("sq") == "'c d'");
    CHECK(s.at("bq") == "e f");
    CHECK(s.at("tq") == "g h");
    CHECK(s.at("spaced") == "padded value");
    CHECK(s.at("colon") == "by colon");
    CHECK(s.at("eqcolon") == "a:b=c");
    CHECK(s.at("hash") == "v # not a comment");
    CHECK(s.at("semi") == "v ; not a comment");
    CHECK(s.at("empty").empty());
    CHECK(s.at("key with spaces") == "v");
    CHECK(s.at("quoted key") == "v");
    CHECK(s.at("sub") == "padded value/x");
    CHECK(s.at("nosub") == "/x");
    CHECK(s.at("back") == "C:\\keys\\id_rsa");

    // %(key)s looks in DEFAULT first, a blank DEFAULT value included; names cut by cutsets; a dotted
    // section's %(key)s doesn't look in the parent; 200 passes.
    const auto sub = dump(*rclone, scratch,
                          "spaced = from-default\n"
                          "e =\n"
                          "[r]\n"
                          "type = alias\n"
                          "spaced = from-section\n"
                          "e = sec\n"
                          "sub = %(spaced)s/x\n"
                          "blank = %(e)s|\n"
                          "ps = %(pass)s|%(hosts)s\n"
                          "a = x%(a)s\n"
                          "[t]\n"
                          "type = alias\n"
                          "pa = PA\n"
                          "host = HO\n"
                          "ps = %(pass)s|%(hosts)s\n"
                          "[box]\n"
                          "dir = C:\\k\n"
                          "[box.eu]\n"
                          "own = %(dir)s\\id\n");
    REQUIRE_MESSAGE(sub.has_value(), sub.error());
    const auto& r = sub->at("r");
    CHECK(r.at("sub") == "from-default/x");
    CHECK(r.at("blank") == "|");
    CHECK(r.at("ps") == "|");
    CHECK(r.at("a") == std::string(201, 'x') + "%(a)s");
    CHECK(sub->at("t").at("ps") == "PA|HO");
    CHECK(sub->at("box.eu").at("own") == "\\id");

    // Comments are whole lines, indented or not; a BOM and CRLF are fine; keys before any section
    // go to "DEFAULT"; the same key again replaces it, a section again adds to it; names of keys
    // and sections are case-sensitive.
    const auto d = dump(*rclone, scratch,
                        "\xEF\xBB\xBFtop = x\r\n"
                        "# a comment\r\n"
                        "[d]\r\n"
                        "  ; an indented comment\r\n"
                        "type = alias\r\n"
                        "remote = first\r\n"
                        "remote = second\r\n"
                        "Remote = upper\r\n"
                        "[D]\r\n"
                        "type = alias\r\n"
                        "[ d ]\r\n"
                        "extra = again\r\n");
    REQUIRE_MESSAGE(d.has_value(), d.error());
    CHECK(*d == Sections{{"DEFAULT", {{"top", "x"}}},
                         {"d", {{"type", "alias"}, {"remote", "second"}, {"Remote", "upper"}, {"extra", "again"}}},
                         {"D", {{"type", "alias"}}}});

    // A dotted name's section holds only its own keys (the parent's come in when a key is read).
    const auto dotted = dump(*rclone, scratch, "[box]\ntype = sftp\nhost = h.example\n[box.eu]\nport = 23\n");
    REQUIRE(dotted.has_value());
    CHECK(dotted->at("box.eu") == std::map<std::string, std::string>{{"port", "23"}});

    // Files rclone refuses as a whole: grab's import notes them instead of reading half.
    for (const std::string_view broken : {"[s] # c\ntype = alias\n", "[]\ntype = alias\n", "[n]\njustakey\n",
                                          "[m]\nremote = \"\"\"line1\nline2\"\"\"\n",
                                          "# Encrypted rclone configuration File\n\nRCLONE_ENCRYPT_V0:\nQUJD\n"}) {
        CHECK_MESSAGE(!dump(*rclone, scratch, broken).has_value(), broken);
    }
}

TEST_CASE("a dotted name takes a key its section lacks from the parent section, as rclone reads it") {
    const auto rclone = bundled_rclone();
    if (!rclone) {
        MESSAGE("no bundled rclone.exe next to the build; skipped");
        return;
    }
    const Scratch scratch("grab-test-rclone-env-parent");
    fs::create_directories(scratch.dir() / "a");
    fs::create_directories(scratch.dir() / "b");
    scratch.write("a/in-a.txt", "x");
    scratch.write("b/in-b.txt", "x");
    const auto a = util::path_to_utf8(scratch.dir() / "a");
    const auto b = util::path_to_utf8(scratch.dir() / "b");
    scratch.write("parent.conf", "remote = " + b + "\n"
                                 "[p]\n"
                                 "type = alias\n"
                                 "remote = " + a + "\n"
                                 "[p.c]\n"
                                 "description = no type, no remote\n"
                                 "[p.c.d]\n"
                                 "[p.e]\n"
                                 "remote =\n"
                                 "[p.f]\n"
                                 "remote = " + b + "\n"
                                 "[P.g]\n"
                                 "[q.c]\n"
                                 "type = alias\n"
                                 "[p.x.y]\n"
                                 "description = no [p.x]\n"
                                 "[rr]\n"
                                 "type = alias\n");
    const auto conf = util::path_to_utf8(scratch.file("parent.conf"));
    const auto lsf = [&](const std::string& name) {
        return listing(run_rclone(*rclone, scratch, {"lsf", name + ":", "--config", conf}));
    };
    CHECK(lsf("p.c") == "in-a.txt");   // type and remote from [p]
    CHECK(lsf("p.c.d") == "in-a.txt"); // through [p.c] to [p]
    CHECK(lsf("p.f") == "in-b.txt");   // its own remote first
    CHECK(lsf("p.e") == std::nullopt); // a blank value stops the search: no remote
    CHECK(lsf("P.g") == std::nullopt); // parents by exact name: [P] isn't there
    CHECK(lsf("q.c") == std::nullopt);   // no [q]: the chain stops at a missing section
    CHECK(lsf("p.x.y") == std::nullopt); // no [p.x]: [p] isn't reached
    CHECK(lsf("rr") == std::nullopt);    // DEFAULT's remote is no fallback
    // The environment comes before the file, and so before the parent too.
    const auto env = run_rclone(*rclone, scratch, {"lsf", "p.c:", "--config", conf}, {{"RCLONE_CONFIG_P.C_REMOTE", b}});
    CHECK(listing(env) == "in-b.txt");
}

TEST_CASE("key_pem, a key's passphrase and a password reach rclone through the environment") {
    const auto rclone = bundled_rclone();
    if (!rclone) {
        MESSAGE("no bundled rclone.exe next to the build; skipped");
        return;
    }
    const Scratch scratch("grab-test-rclone-env-keys");
    scratch.write("known_hosts", "");
    const int port = closed_port();
    const Vars sftp{{"RCLONE_CONFIG_EK_TYPE", "sftp"},
                    {"RCLONE_CONFIG_EK_HOST", "127.0.0.1"},
                    {"RCLONE_CONFIG_EK_PORT", std::to_string(port)},
                    {"RCLONE_CONFIG_EK_USER", "t"},
                    {"RCLONE_CONFIG_EK_KNOWN_HOSTS_FILE", util::path_to_utf8(scratch.dir() / "known_hosts")}};
    // On a closed port: a key or password rclone can't use fails before the connection.
    const auto attempt = [&](const Vars& more) {
        Vars vars = sftp;
        vars.insert(vars.end(), more.begin(), more.end());
        const auto ran = run_rclone(*rclone, scratch,
                                    {"lsf", "EK:", "--config", "NUL", "--low-level-retries", "1", "--retries", "1"}, vars);
        const bool refused = ran.err.find("127.0.0.1:" + std::to_string(port) + ": connectex") != std::string::npos;
        return refused ? std::string("dialled") : ran.err;
    };
    const auto ed25519 = made_up_key("ED25519", nullptr);
    CHECK(attempt({{"RCLONE_CONFIG_EK_KEY_PEM", one_line(ed25519)}}) == "dialled");
    // rclone reads key_pem as the inside of a Go string literal: real line breaks are refused.
    CHECK(attempt({{"RCLONE_CONFIG_EK_KEY_PEM", ed25519}}).find("pem key not formatted properly") != std::string::npos);
    const auto ec = made_up_key("EC", "P-256", "fake-passphrase");
    CHECK(attempt({{"RCLONE_CONFIG_EK_KEY_PEM", one_line(ec)},
                   {"RCLONE_CONFIG_EK_KEY_FILE_PASS", obscured("fake-passphrase")}}) == "dialled");
    CHECK(attempt({{"RCLONE_CONFIG_EK_KEY_PEM", one_line(ec)}}).find("passphrase") != std::string::npos);
    CHECK(attempt({{"RCLONE_CONFIG_EK_KEY_PEM", one_line(ec)}, {"RCLONE_CONFIG_EK_KEY_FILE_PASS", "fake-passphrase"}})
              .find("is it obscured?") != std::string::npos);
    // A password: only in rclone's obscured form.
    CHECK(attempt({{"RCLONE_CONFIG_EK_PASS", obscured("fake-password")}}) == "dialled");
    CHECK(attempt({{"RCLONE_CONFIG_EK_PASS", "fake-password"}}).find("is it obscured?") != std::string::npos);
}

TEST_CASE("an inherited RCLONE_SFTP_* or a --sftp-* flag beats RCLONE_CONFIG_<NAME>_*: grab drops or refuses them") {
    const auto rclone = bundled_rclone();
    if (!rclone) {
        MESSAGE("no bundled rclone.exe next to the build; skipped");
        return;
    }
    const Scratch scratch("grab-test-rclone-env-wins");
    scratch.write("known_hosts", "");
    const int p1 = closed_port();
    int p2 = closed_port();
    while (p2 == p1) p2 = closed_port();
    const Vars server{{"RCLONE_CONFIG_WN_TYPE", "sftp"},
                      {"RCLONE_CONFIG_WN_HOST", "127.0.0.1"},
                      {"RCLONE_CONFIG_WN_PORT", std::to_string(p1)},
                      {"RCLONE_CONFIG_WN_USER", "t"},
                      {"RCLONE_CONFIG_WN_PASS", obscured("fake-password")},
                      {"RCLONE_CONFIG_WN_KNOWN_HOSTS_FILE", util::path_to_utf8(scratch.dir() / "known_hosts")}};
    // The port rclone dialled, from Go's refused dial (its syscall name is never translated).
    const auto dialled = [&](const Vars& more, const std::vector<std::string>& flags = {}) {
        Vars vars = server;
        vars.insert(vars.end(), more.begin(), more.end());
        std::vector<std::string> args{"lsf", "WN:", "--config", "NUL", "--low-level-retries", "1", "--retries", "1"};
        args.insert(args.end(), flags.begin(), flags.end());
        const auto err = run_rclone(*rclone, scratch, args, vars).err;
        for (const int port : {p1, p2, 22}) {
            if (err.find("127.0.0.1:" + std::to_string(port) + ": connectex") != std::string::npos) return port;
        }
        // An sshd on this PC's port 22 answers instead, without naming the port; p1 and p2 are closed.
        if (err.find("handshake failed") != std::string::npos) return 22;
        return 0;
    };
    CHECK(dialled({}) == p1);
    // rclone makes an inherited RCLONE_SFTP_PORT the flag's default, which a remote's own value
    // doesn't override unless it's the option's default anyway.
    CHECK(dialled({{"RCLONE_SFTP_PORT", std::to_string(p2)}}) == p2);
    CHECK(dialled({{"RCLONE_SFTP_PORT", "22"}}) == p1);
    CHECK(dialled({}, {"--sftp-port", std::to_string(p2)}) == p2);
    CHECK(dialled({}, {"--sftp-port", "22"}) == 22); // an explicit flag wins even at the default
    const auto missing = util::path_to_utf8(scratch.dir() / "inherited_known_hosts");
    const auto err = run_rclone(*rclone, scratch, {"lsf", "WN:", "--config", "NUL", "--low-level-retries", "1"},
                                [&] {
                                    Vars vars = server;
                                    vars.emplace_back("RCLONE_SFTP_KNOWN_HOSTS_FILE", missing);
                                    return vars;
                                }())
                         .err;
    CHECK(err.find("inherited_known_hosts") != std::string::npos);
}

TEST_CASE("rclone reveals what grab obscures, and grab reveals what rclone obscures") {
    const auto rclone = bundled_rclone();
    if (!rclone) {
        MESSAGE("no bundled rclone.exe next to the build; skipped");
        return;
    }
    const Scratch scratch("grab-test-rclone-env-obscure");
    for (const auto& plain : std::vector<std::string>{"fake-password", "", "x", "fake pass with spaces \"' and = and \\",
                                                      "fake-\u10DE\u10D0\u10E0\u10DD\u10DA\u10D8-\u00E9",
                                                      std::string(300, 'p')}) {
        const auto text = obscured(plain);
        CHECK(text != obscured(plain)); // a new IV each time
        CHECK(secrets::reveal(text) == plain);
        // "--": an obscured value can start with '-'.
        const auto ran = run_rclone(*rclone, scratch, {"reveal", "--", text});
        CHECK_MESSAGE(ran.exit_code == 0, ran.err);
        CHECK(ran.out == plain + "\n");
    }
    const auto ran = run_rclone(*rclone, scratch, {"obscure", "fake-password-2"});
    REQUIRE(ran.exit_code == 0);
    std::string by_rclone = ran.out;
    while (!by_rclone.empty() && (by_rclone.back() == '\n' || by_rclone.back() == '\r')) by_rclone.pop_back();
    CHECK(secrets::reveal(by_rclone) == "fake-password-2");
}

namespace {

// Server `name` of grab.conf, logging in with a made-up key file (or the passphrase-protected one
// and its passphrase in a store of `scratch`) to a closed local port, pinned in grab's known_hosts
// (the test runner's scratch APPDATA): its run of the bundled rclone, given the server by
// engine::rclone_env with --config NUL. "dialled" when rclone took every setting and dialled.
std::string dial_through_rclone_env(const fs::path& rclone, const Scratch& scratch, const std::string& name,
                                    const std::string& key_file, int port, const std::string& passphrase = {}) {
    server_conf::Connection c;
    c.host = "127.0.0.1";
    c.port = port;
    c.user = "t";
    c.auth = server_conf::Auth::key;
    c.key_file = key_file;
    RemoteSettings settings;
    settings.name = name;
    settings.rclone_remote = name;
    auto ctx = engine::make_context(GrabConfig{}, settings, c, scratch.file("grab.conf"));
    ctx.passwords = scratch.file("passwords");
    passwords::Store store;
    if (!passphrase.empty()) {
        store[name]["passphrase"] = passwords::Secret{.binding = {.user = "t", .host = "127.0.0.1", .port = port}, .value = passphrase};
    }
    REQUIRE(passwords::save(ctx.passwords, store));
    fs::create_directories(grab_known_hosts_path().parent_path());
    // A made-up key rclone can parse (it reads the file before it dials).
    std::ofstream(grab_known_hosts_path(), std::ios::binary)
        << "[127.0.0.1]:" << port << " ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOMqqnkVzrm0SdG6UOoqKLsabgH5C9okWi0dh2l9GKJl\n";
    auto env = engine::rclone_env(ctx);
    REQUIRE_MESSAGE(env, env.error_or(""));
    proc::RunOptions quiet;
    quiet.detached = true;
    quiet.environment = std::move(*env);
    const auto ran = proc::run_capture(std::vector<std::string>{util::path_to_utf8(rclone), "lsf", name + ":", "--config",
                                                                std::string(config_in_memory), "--low-level-retries",
                                                                "1", "--retries", "1"},
                                       quiet);
    fs::remove(grab_known_hosts_path());
    REQUIRE_MESSAGE(ran, ran.error_or(""));
    if (ran->err.find("127.0.0.1:" + std::to_string(port) + ": connectex") != std::string::npos) return "dialled";
    return ran->err;
}

} // namespace

TEST_CASE("grab's own path: rclone_env and --config NUL reach the bundled rclone, under the user's names (phase 7)") {
    const auto rclone = bundled_rclone();
    if (!rclone) {
        MESSAGE("no bundled rclone.exe next to the build; skipped");
        return;
    }
    const Scratch scratch("grab-test-rclone-env-grab");
    scratch.write("id_fake", made_up_key("ED25519", nullptr));
    scratch.write("id_fake_pass", made_up_key("EC", "P-256", "fake-passphrase"));
    const int port = closed_port();
    // Each run gets its one server from grab.conf's details: a Georgian name (Go's upper case,
    // which Windows' doesn't match), '-' and a space, a dotted name (in memory, no parent to take
    // keys from: G86).
    for (const std::string& name :
         {std::string("\u10E1\u10D4\u10E0\u10D5\u10D4\u10E0\u10D8"), std::string("my-box x"), std::string("box.eu")}) {
        CHECK_MESSAGE(dial_through_rclone_env(*rclone, scratch, name, util::path_to_utf8(scratch.file("id_fake")), port) ==
                          "dialled",
                      name);
    }
    // A passphrase from grab's store, obscured by grab, read by rclone; without it rclone stops
    // before it dials.
    CHECK(dial_through_rclone_env(*rclone, scratch, "pp", util::path_to_utf8(scratch.file("id_fake_pass")), port,
                                  "fake-passphrase") == "dialled");
    CHECK(dial_through_rclone_env(*rclone, scratch, "pp", util::path_to_utf8(scratch.file("id_fake_pass")), port)
              .find("passphrase") != std::string::npos);
}

TEST_CASE("a '$' in a key file's path reaches rclone as it is, not expanded a second time") {
    const auto rclone = bundled_rclone();
    if (!rclone) {
        MESSAGE("no bundled rclone.exe next to the build; skipped");
        return;
    }
    const Scratch scratch("grab-test-rclone-env-dir");
    fs::create_directories(scratch.dir() / "$sync");
    scratch.write("$sync/id_fake", made_up_key("ED25519", nullptr));
    const int port = closed_port();
    // rclone expands key_file's value (os.ExpandEnv): given as it is, "$sync" would turn empty
    // and the key wouldn't be found; through GRAB_RCLONE_KEY_FILE it is read and rclone dials.
    CHECK(dial_through_rclone_env(*rclone, scratch, "dd", util::path_to_utf8(scratch.dir() / "$sync" / "id_fake"), port) ==
          "dialled");
    const auto missing = dial_through_rclone_env(*rclone, scratch, "dd", util::path_to_utf8(scratch.dir() / "$sync" / "none"), port);
    CHECK_MESSAGE(missing.find("failed to read private key file") != std::string::npos, missing);
    CHECK_MESSAGE(missing.find("$sync") != std::string::npos, missing);
}

namespace {

// What grab's reader (rclone_conf.hpp) makes of `text`, in `dump`'s shape: every section's own
// keys, each as rclone would get it (a blank one as "").
std::expected<Sections, std::string> read_like_dump(std::string_view text) {
    const auto doc = rclone_conf::parse(text);
    if (!doc) return std::unexpected(doc.error());
    Sections sections;
    for (const auto& s : doc->sections) {
        auto& out = sections[s.name];
        for (const auto& key : s.keys) {
            const auto v = rclone_conf::get(*doc, s.name, key, false);
            REQUIRE_MESSAGE(v.has_value(), v.error());
            out[key] = *v ? (*v)->text : std::string();
        }
    }
    return sections;
}

} // namespace

TEST_CASE("grab's rclone.conf reader reads every shape as `rclone config dump` does (phase 7)") {
    const auto rclone = bundled_rclone();
    if (!rclone) {
        MESSAGE("no bundled rclone.exe next to the build; skipped");
        return;
    }
    const Scratch scratch("grab-test-rclone-conf-dump");
    const std::vector<std::string> texts = {
        // Keys: quoted in ", """ or ` (not trimmed inside), cut at '=' or ':', "-" numbered.
        "[k]\n"
        "type = alias\n"
        "\"quoted key\" = v1\n"
        "\"\"\"triple key\"\"\" = v2\n"
        "`tick key` = v3\n"
        "\" spaced \" = v4\n"
        "\"a=b\" : v5\n"
        "\"x\"junk = v6\n"
        "- = auto1\n"
        "- = auto2\n"
        "key:colon = x\n"
        "nb = \u00A0padded\u00A0\n"
        "\u00A0# a comment after a no-break space\n"
        "bq = `e f` trailing\n"
        "tq = \"\"\"g\"\"\" \"\"\"h\"\"\"\n"
        "short = `\n"
        "tick2 = ``\n"
        "dq = \"a b\"\n"
        "sq = 'c d'\n"
        "hash = v # not a comment\n"
        "back = C:\\keys\\id_rsa\n",
        // %(key)s: DEFAULT first (and through DEFAULT's own references), else the section's raw
        // value, substituted again on the next pass; cutsets; 200 passes; nothing else is a name.
        "spaced = from-default\n"
        "e =\n"
        "chain = %(e2)s!\n"
        "e2 = E2\n"
        "[r]\n"
        "type = alias\n"
        "spaced = from-section\n"
        "e = sec\n"
        "sub = %(spaced)s/x\n"
        "blank = %(e)s|\n"
        "ps = %(pass)s|%(hosts)s\n"
        "a = x%(a)s\n"
        "viadef = %(chain)s\n"
        "two = %(own)s+%(own)s\n"
        "own = O\n"
        "nested = %(inner)s\n"
        "inner = %(own)s-in\n"
        "noclose = %(abc\n"
        "emptyname = %()s\n"
        "weird = %((own)s\n"
        "[box]\n"
        "dir = C:\\k\n"
        "[box.eu]\n"
        "own = %(dir)s\\id\n",
        // Layout: a BOM, CRLF, comments, DEFAULT's keys before any section and under [DEFAULT],
        // a key again, a section again, case-sensitive names, [ x ], no final line break.
        "\xEF\xBB\xBFtop = x\r\n"
        "# a comment\r\n"
        "[d]\r\n"
        "  ; an indented comment\r\n"
        "type = alias\r\n"
        "remote = first\r\n"
        "remote = second\r\n"
        "Remote = upper\r\n"
        "[D]\r\n"
        "type = alias\r\n"
        "[ d ]\r\n"
        "extra = again\r\n"
        "[DEFAULT]\r\n"
        "late = y\r\n"
        "[last]\r\n"
        "type = alias\r\n"
        "nofinal = end"};
    for (const auto& text : texts) {
        const auto ours = read_like_dump(text);
        const auto theirs = dump(*rclone, scratch, text);
        REQUIRE_MESSAGE(theirs.has_value(), theirs.error());
        REQUIRE_MESSAGE(ours.has_value(), ours.error());
        CHECK(*ours == *theirs);
        for (const auto& [section, keys] : *theirs) {
            for (const auto& [key, value] : keys) {
                const auto mine = ours->find(section);
                CHECK_MESSAGE((mine != ours->end() && mine->second.contains(key) && mine->second.at(key) == value),
                              "[", section, "] ", key, " = ", value);
            }
        }
    }
    // What rclone refuses as a whole, grab's reader refuses too.
    for (const std::string_view broken :
         {"[s] # c\ntype = alias\n", "[]\ntype = alias\n", "[n]\njustakey\n", "[m]\nremote = \"\"\"line1\nline2\"\"\"\n",
          "[m]\nremote = `open\n", "[k]\n\"open = v\n", "[k]\n\"k\" v\n", "[k]\n= v\n", "[k]\n: v\n"}) {
        CHECK_MESSAGE(!rclone_conf::parse(broken).has_value(), broken);
        CHECK_MESSAGE(!dump(*rclone, scratch, broken).has_value(), broken);
    }
}

TEST_CASE("grab's reader follows a dotted name's parents as rclone does") {
    const auto rclone = bundled_rclone();
    if (!rclone) {
        MESSAGE("no bundled rclone.exe next to the build; skipped");
        return;
    }
    const Scratch scratch("grab-test-rclone-conf-parents");
    for (const auto* folder : {"a", "b", "m"}) {
        fs::create_directories(scratch.dir() / folder);
        scratch.write(std::string(folder) + "/in-" + folder + ".txt", "x");
    }
    const auto a = util::path_to_utf8(scratch.dir() / "a");
    const auto b = util::path_to_utf8(scratch.dir() / "b");
    const auto m = util::path_to_utf8(scratch.dir() / "m");
    const std::string text = "remote = " + b + "\n"
                             "[p]\ntype = alias\nremote = " + a + "\n"
                             "[p.c]\ndescription = no type, no remote\n"
                             "[p.c.d]\n"
                             "[p.e]\nremote =\n"
                             "[p.f]\nremote = " + b + "\n"
                             "[P.g]\n"
                             "[q.c]\ntype = alias\n"
                             "[p.x.y]\ndescription = no [p.x]\n"
                             "[rr]\ntype = alias\n"
                             // %(key)s in a parent's value reads the parent's own keys.
                             "[mm]\ntype = alias\nmdir = " + m + "\nremote = %(mdir)s\n"
                             "[mm.c]\ndescription = the remote of [mm]\n"
                             "[mm.d]\nremote = %(mdir)s\n";
    scratch.write("parent.conf", text);
    const auto doc = rclone_conf::parse(text);
    REQUIRE(doc.has_value());
    const auto conf = util::path_to_utf8(scratch.file("parent.conf"));
    // Not "p" itself, which rclone takes for drive P:.
    for (const std::string name : {"p.c", "p.c.d", "p.e", "p.f", "P.g", "q.c", "p.x.y", "rr", "mm", "mm.c", "mm.d"}) {
        // What grab says rclone lists: the alias's folder's one file, or nothing.
        std::optional<std::string> expected;
        const auto type = rclone_conf::get(*doc, name, "type");
        const auto remote = rclone_conf::get(*doc, name, "remote");
        REQUIRE((type.has_value() && remote.has_value()));
        if (*type && (*type)->text == "alias" && *remote) {
            expected = "in-" + util::path_to_utf8(util::path_from_utf8((*remote)->text).filename()) + ".txt";
        }
        const auto listed = listing(run_rclone(*rclone, scratch, {"lsf", name + ":", "--config", conf}));
        CHECK_MESSAGE(listed == expected, name);
    }
    // Which section a value came from, for the import's notes.
    const auto from = rclone_conf::get(*doc, "p.c.d", "remote");
    REQUIRE((from.has_value() && from->has_value()));
    CHECK((*from)->section == "p");
    const auto own = rclone_conf::get(*doc, "p.c.d", "remote", false);
    REQUIRE(own.has_value());
    CHECK_FALSE(own->has_value());
}

TEST_CASE("~, $VAR and ${RCLONE_CONFIG_DIR} in a key_file expand as rclone expands them") {
    const auto rclone = bundled_rclone();
    if (!rclone) {
        MESSAGE("no bundled rclone.exe next to the build; skipped");
        return;
    }
    const Scratch scratch("grab-test-rclone-conf-paths");
    const auto dir = scratch.dir() / "$sync";
    fs::create_directories(dir);
    // Outside "$sync": rclone would expand the '$' in a path written out (with_config_dir).
    scratch.write("known_hosts", "");
    const int port = closed_port();
    const std::string var = "GRAB_TEST_P7C_FOLDER";
    const std::string var_value = util::path_to_utf8(scratch.dir() / "v$x");
    const rclone_conf::Getenv getenv = [&](std::string_view name) -> std::optional<std::string> {
        if (util::to_lower(name) == util::to_lower(var)) return var_value;
        return util::getenv_utf8(std::string(name).c_str());
    };
    for (const std::string key_file :
         {"${RCLONE_CONFIG_DIR}/id_a", "$RCLONE_CONFIG_DIR\\id_b", "$rclone_config_dir/id_c", "~/grab-test-no-key",
          "~\\grab-test-no-key-2", "${GRAB_TEST_P7C_FOLDER}/id_d", "$GRAB_TEST_P7C_FOLDER\\id_e",
          "x$/id_f", "${}id_g", "~user/id_h", "C:\\keys\\$GRAB_TEST_P7C_FOLDER-id"}) {
        // Not a one-letter name, which rclone takes for a drive letter.
        scratch.write("$sync/rclone.conf", "[kf]\ntype = sftp\nhost = 127.0.0.1\nport = " + std::to_string(port) +
                                               "\nuser = t\nknown_hosts_file = " +
                                               util::path_to_utf8(scratch.file("known_hosts")) + "\nkey_file = " +
                                               key_file + "\n");
        const auto ran =
            run_rclone(*rclone, scratch,
                       {"lsf", "kf:", "--config", util::path_to_utf8(dir / "rclone.conf"), "--low-level-retries", "1",
                        "--retries", "1"},
                       {{var, var_value}});
        const auto expected = rclone_conf::expand_path(key_file, dir, getenv);
        // Go's "open <path>: ..." names the path it tried.
        CHECK_MESSAGE(ran.err.find("open " + expected + ":") != std::string::npos, key_file, " -> ", expected, "\n",
                      ran.err);
    }
}

#endif
