#include "json.hpp"

#include <doctest/doctest.h>

using namespace grab;

TEST_CASE("json parses scalars, nesting and escapes") {
    auto v = json::parse(R"( {"a": 1.5, "b": [true, false, null, "x"], "c": {"d": -2e3},
                             "s": "q\"\\/\né😀", "empty": {}, "arr": []} )");
    REQUIRE_MESSAGE(v.has_value(), v.error_or(""));
    REQUIRE(v->kind == json::Value::Kind::object);
    CHECK(v->find("a")->number == 1.5);
    const auto* b = v->find("b");
    REQUIRE(b->items.size() == 4);
    CHECK(b->items[0].boolean);
    CHECK_FALSE(b->items[1].boolean);
    CHECK(b->items[1].kind == json::Value::Kind::boolean);
    CHECK(b->items[2].kind == json::Value::Kind::null);
    CHECK(b->items[3].str == "x");
    CHECK(v->find("c")->find("d")->number == -2000.0);
    CHECK(v->string_of("s") == "q\"\\/\n\xC3\xA9\xF0\x9F\x98\x80");
    CHECK(v->find("empty")->keys.empty());
    CHECK(v->find("arr")->items.empty());
    CHECK(v->find("missing") == nullptr);
    CHECK_FALSE(v->string_of("a").has_value()); // not a string
}

TEST_CASE("json rejects malformed input") {
    CHECK_FALSE(json::parse("").has_value());
    CHECK_FALSE(json::parse("{").has_value());
    CHECK_FALSE(json::parse(R"({"a" 1})").has_value());
    CHECK_FALSE(json::parse(R"({"a": 1,})").has_value());
    CHECK_FALSE(json::parse(R"([1 2])").has_value());
    CHECK_FALSE(json::parse(R"("unterminated)").has_value());
    CHECK_FALSE(json::parse(R"("bad \x escape")").has_value());
    CHECK_FALSE(json::parse("{} trailing").has_value());
    CHECK_FALSE(json::parse(std::string(200, '[')).has_value()); // too deep
}
