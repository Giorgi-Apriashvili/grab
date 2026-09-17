#include "ini.hpp"

#include <doctest/doctest.h>

using namespace grab;

TEST_CASE("ini parses sections, comments, CRLF and blank values") {
    const std::string text = "; leading comment\r\n"
                             "# another\r\n"
                             "\r\n"
                             "[hetzner]\r\n"
                             "type = sftp\r\n"
                             "host=203.0.113.10\r\n"
                             "key_pem = \r\n"
                             "  port =   2222  \r\n"
                             "[other]\n"
                             "a = 1\n"
                             "a = 2\n";
    auto doc = ini::parse(text);
    REQUIRE(doc.has_value());
    REQUIRE(doc->sections.size() == 2);

    const auto* h = doc->find("hetzner");
    REQUIRE(h != nullptr);
    CHECK(h->get("type") == "sftp");
    CHECK(h->get("host") == "203.0.113.10");
    CHECK(h->get("port") == "2222");
    CHECK(h->get("key_pem") == "");
    CHECK_FALSE(h->get("missing").has_value());

    const auto* o = doc->find("other");
    REQUIRE(o != nullptr);
    CHECK(o->get("a") == "2"); // last wins
    CHECK(doc->find("nope") == nullptr);
}

TEST_CASE("ini keeps ; and # inside values and equals signs in values") {
    auto doc = ini::parse("[s]\nkey_file_pass = ab#cd;ef\nx = a=b=c\n");
    REQUIRE(doc.has_value());
    const auto* s = doc->find("s");
    REQUIRE(s != nullptr);
    CHECK(s->get("key_file_pass") == "ab#cd;ef");
    CHECK(s->get("x") == "a=b=c");
}

TEST_CASE("ini merges duplicate sections and keeps file order") {
    auto doc = ini::parse("[a]\nx=1\n[b]\ny=2\n[a]\nz=3\n");
    REQUIRE(doc.has_value());
    REQUIRE(doc->sections.size() == 2);
    CHECK(doc->sections[0].name == "a");
    CHECK(doc->sections[1].name == "b");
    CHECK(doc->find("a")->get("z") == "3");
    CHECK(doc->section_names() == std::vector<std::string>{"a", "b"});
}

TEST_CASE("ini puts keys before any header in the unnamed section") {
    auto doc = ini::parse("k = v\n[s]\n");
    REQUIRE(doc.has_value());
    REQUIRE(doc->find("") != nullptr);
    CHECK(doc->find("")->get("k") == "v");
}

TEST_CASE("ini reports malformed lines with numbers") {
    auto bad_header = ini::parse("[oops\n");
    REQUIRE_FALSE(bad_header.has_value());
    CHECK(bad_header.error().find("line 1") != std::string::npos);

    auto no_equals = ini::parse("[s]\n\njust text\n");
    REQUIRE_FALSE(no_equals.has_value());
    CHECK(no_equals.error().find("line 3") != std::string::npos);

    auto empty_key = ini::parse("[s]\n= v\n");
    REQUIRE_FALSE(empty_key.has_value());

    auto empty_section = ini::parse("[ ]\n");
    REQUIRE_FALSE(empty_section.has_value());
}

TEST_CASE("ini handles an empty document and a missing trailing newline") {
    auto empty = ini::parse("");
    REQUIRE(empty.has_value());
    CHECK(empty->sections.empty());

    auto no_nl = ini::parse("[s]\nk=v");
    REQUIRE(no_nl.has_value());
    CHECK(no_nl->find("s")->get("k") == "v");
}
