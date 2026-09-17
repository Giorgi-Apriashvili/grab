#include "quote.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace grab;

TEST_CASE("windows_arg leaves plain arguments alone") {
    CHECK(quote::windows_arg("abc") == "abc");
    CHECK(quote::windows_arg("C:\\dir\\file.txt") == "C:\\dir\\file.txt");
    CHECK(quote::windows_arg("C:\\dir\\") == "C:\\dir\\");
    CHECK(quote::windows_arg("--opt=value") == "--opt=value");
}

TEST_CASE("windows_arg quotes spaces and escapes quotes per MSVC CRT rules") {
    CHECK(quote::windows_arg("a b") == "\"a b\"");
    CHECK(quote::windows_arg("") == "\"\"");
    CHECK(quote::windows_arg("a\"b") == "\"a\\\"b\"");
    CHECK(quote::windows_arg("C:\\my dir\\") == "\"C:\\my dir\\\\\"");
    CHECK(quote::windows_arg("back\\\\\"slash") == "\"back\\\\\\\\\\\"slash\"");
    CHECK(quote::windows_arg("tab\there") == "\"tab\there\"");
}

TEST_CASE("windows_cmdline joins with single spaces") {
    const std::vector<std::string> argv{"rclone", "copy", "hetzner:/home/alice/releases",
                                        "E:\\Backup\\my releases", "-P"};
    CHECK(quote::windows_cmdline(argv) ==
          "rclone copy hetzner:/home/alice/releases \"E:\\Backup\\my releases\" -P");
}

TEST_CASE("sh_single quotes for a POSIX shell") {
    CHECK(quote::sh_single("plain") == "'plain'");
    CHECK(quote::sh_single("it's") == "'it'\\''s'");
    CHECK(quote::sh_single("") == "''");
    CHECK(quote::sh_single("a b$c") == "'a b$c'");
}

TEST_CASE("sh_arg quotes only when needed") {
    CHECK(quote::sh_arg("--transfers=4") == "--transfers=4");
    CHECK(quote::sh_arg("/home/alice") == "/home/alice");
    CHECK(quote::sh_arg("a b") == "'a b'");
    CHECK(quote::sh_arg("") == "''");
    CHECK(quote::sh_arg("x*") == "'x*'");
}
