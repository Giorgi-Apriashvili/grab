// grab's reading of an rclone.conf as rclone reads it (rclone_conf.hpp), without rclone: what
// rclone would hang or crash on, and the path expansion with a made-up environment.
// tests/test_rclone_env.cpp checks the same reader against the bundled rclone.

#include "rclone_conf.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

using namespace grab;

namespace {

std::optional<std::string> value(std::string_view text, std::string_view section, std::string_view key,
                                 bool parents = true) {
    const auto doc = rclone_conf::parse(text);
    REQUIRE_MESSAGE(doc.has_value(), doc.error_or(""));
    const auto v = rclone_conf::get(*doc, section, key, parents);
    REQUIRE_MESSAGE(v.has_value(), v.error_or(""));
    if (!*v) return std::nullopt;
    return (*v)->text;
}

} // namespace

TEST_CASE("rclone.conf as rclone reads it: quotes, comments, DEFAULT, %(key)s") {
    const std::string_view text = "top = T\n"
                                  "[s]\n"
                                  "\"q k\" = `v w` x\n"
                                  "plain = \"kept\"\n"
                                  "sub = %(top)s/%(own)s/%(nope)s\n"
                                  "own = O\n"
                                  "blank =\n"
                                  "hash = a # b\n"
                                  "; comment\n"
                                  "[s.c]\n"
                                  "own = C\n";
    CHECK(value(text, "s", "q k") == "v w");
    CHECK(value(text, "s", "plain") == "\"kept\"");
    CHECK(value(text, "s", "sub") == "T/O/");
    CHECK(value(text, "s", "blank") == std::nullopt); // blank is not set
    CHECK(value(text, "s", "hash") == "a # b");
    CHECK(value(text, "s", "top") == std::nullopt); // DEFAULT is no fallback
    // A dotted name: its own keys, else the parent's, substituted in the parent.
    CHECK(value(text, "s.c", "sub") == "T/O/");
    CHECK(value(text, "s.c", "sub", false) == std::nullopt);
    CHECK(value(text, "s.c", "own") == "C");
    const auto doc = rclone_conf::parse(text);
    REQUIRE(doc.has_value());
    CHECK(doc->find("s")->keys == std::vector<std::string>{"q k", "plain", "sub", "own", "blank", "hash"});
    CHECK(doc->find(rclone_conf::default_section)->keys == std::vector<std::string>{"top"});
    CHECK(rclone_conf::parse("[a]\nk = v\n[]\nx = y\n").error_or("") == "line 4: a key in a section without a name");
}

TEST_CASE("rclone.conf: what %(key)s would make rclone hang or crash is refused") {
    // rclone repeats 200 passes: a value that refers to itself once grows by a bit each pass.
    CHECK(value("[s]\na = x%(a)s\n", "s", "a") == std::string(201, 'x') + "%(a)s");
    // One that doubles each pass would need 2^200 bytes.
    const auto doubling = rclone_conf::parse("[s]\na = %(a)s%(a)s\n");
    REQUIRE(doubling.has_value());
    CHECK(rclone_conf::get(*doubling, "s", "a").error_or("").find("longer than 64 KiB") != std::string::npos);
    // A big raw value used many times in one pass is refused before that pass builds it.
    std::string many;
    for (int i = 0; i < 1000; ++i) many += "%(big)s";
    const auto big_refs = rclone_conf::parse("[s]\nbig = " + std::string(70000, 'x') + "\nv = " + many + "\n");
    REQUIRE(big_refs.has_value());
    CHECK(rclone_conf::get(*big_refs, "s", "v").error_or("").find("longer than 64 KiB") != std::string::npos);
    // DEFAULT's keys that refer to each other: rclone recurses until its stack runs out.
    const auto loop = rclone_conf::parse("a = %(b)s\nb = %(a)s\n[s]\nk = %(a)s\n");
    REQUIRE(loop.has_value());
    CHECK(rclone_conf::get(*loop, "s", "k").error_or("").find("go round in circles") != std::string::npos);
    // Many references that are each fine still come back.
    std::string wide = "[s]\n";
    std::string all;
    for (int i = 0; i < 150; ++i) {
        wide += "k" + std::to_string(i) + " = " + std::to_string(i) + "\n";
        all += "%(k" + std::to_string(i) + ")s,";
    }
    wide += "all = " + all + "\n";
    CHECK(value(wide, "s", "all").value_or("").starts_with("0,1,2,"));
}

TEST_CASE("rclone's expansion of key_file and known_hosts_file paths, with a made-up environment") {
    std::map<std::string, std::string> env{{"HOME", "H:\\home"}, {"USERPROFILE", "U:\\profile"}, {"KEYS", "K:\\$keys"}};
    const rclone_conf::Getenv getenv = [&](std::string_view name) -> std::optional<std::string> {
        const auto it = env.find(std::string(name));
        if (it == env.end()) return std::nullopt;
        return it->second;
    };
    const auto dir = util::path_from_utf8("C:\\cfg$x");
    const auto home = [](std::string_view rest) {
        return util::path_to_utf8((util::path_from_utf8("H:\\home") / util::path_from_utf8(rest)).lexically_normal());
    };
    CHECK(rclone_conf::expand_path("~/.ssh/id", dir, getenv) == home(".ssh/id"));
    CHECK(rclone_conf::expand_path("~\\.ssh\\id", dir, getenv) == home(".ssh\\id"));
    CHECK(rclone_conf::expand_path("~user/id", dir, getenv) == "~user/id"); // go-homedir can't, and leaves it
    CHECK(rclone_conf::expand_path("a~/id", dir, getenv) == "a~/id");
    // What a variable gives isn't expanded again: the '$' in the folder and in KEYS stay.
    CHECK(rclone_conf::expand_path("${RCLONE_CONFIG_DIR}/id", dir, getenv) == "C:\\cfg$x/id");
    CHECK(rclone_conf::expand_path("$KEYS/id", dir, getenv) == "K:\\$keys/id");
    CHECK(rclone_conf::expand_path("${KEYS}x", dir, getenv) == "K:\\$keysx");
    CHECK(rclone_conf::expand_path("$KEYSx", dir, getenv).empty()); // KEYSx isn't set
    CHECK(rclone_conf::expand_path("a$/b$", dir, getenv) == "a$/b$");
    CHECK(rclone_conf::expand_path("a${}b${c", dir, getenv) == "abc");
    CHECK(rclone_conf::expand_path("$$x", dir, getenv) == "x");
#ifdef _WIN32
    CHECK(rclone_conf::expand_path("$rclone_config_dir\\id", dir, getenv) == "C:\\cfg$x\\id");
#endif
    // HOME first, then USERPROFILE, then HOMEDRIVE and HOMEPATH.
    env.erase("HOME");
    CHECK(rclone_conf::expand_path("~", dir, getenv) == util::path_to_utf8(util::path_from_utf8("U:\\profile").lexically_normal()));
    env.erase("USERPROFILE");
    env["HOMEDRIVE"] = "D:";
    env["HOMEPATH"] = "\\users\\d";
    CHECK(rclone_conf::expand_path("~/k", dir, getenv) ==
          util::path_to_utf8((util::path_from_utf8("D:\\users\\d") / "k").lexically_normal()));
    env.erase("HOMEPATH");
    CHECK(rclone_conf::expand_path("~/k", dir, getenv) == "~/k");
}
