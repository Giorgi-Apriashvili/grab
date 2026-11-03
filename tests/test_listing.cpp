#include "listing.hpp"

#include <doctest/doctest.h>

using namespace grab;

TEST_CASE("remote path helpers") {
    CHECK(join_remote("", "learning/x") == "learning/x");
    CHECK(join_remote("/", "x") == "/x");
    CHECK(join_remote("/home", "x") == "/home/x");
    CHECK(join_remote("learning/", "x") == "learning/x");
    CHECK(parent_of("learning/x") == "learning");
    CHECK(parent_of("/x") == "/");
    CHECK(parent_of("/a/b/") == "/a");
    CHECK(parent_of("x") == "");
}

TEST_CASE("a listing's hits: kind, dot-names below the start, the name as the query asks") {
    const HitFilter words(Mode::file, "lioness s03", false, true);
    CHECK(words.keeps("tv/Lioness.S03E08.mkv", false));
    CHECK_FALSE(words.keeps("tv/Lioness.S03E08.mkv", true)); // a folder isn't a file hit
    CHECK_FALSE(words.keeps("Lioness S03/readme.txt", false)); // only the name counts
    CHECK_FALSE(words.keeps(".cache/Lioness.S03E08.mkv", false)); // below a dot-folder
    CHECK_FALSE(words.keeps("", false));
    // Not skipped: a dot-folder's files are hits too; so when the query wants a dot-name.
    CHECK(HitFilter(Mode::file, "lioness s03", false, false).keeps(".cache/Lioness.S03E08.mkv", false));
    CHECK(HitFilter(Mode::folder, ".git", false, true).keeps("src/.git", true));
    // A glob, --exact, and a path target (its name exactly, wherever the listing started).
    CHECK(HitFilter(Mode::file, "*.MKV", false, true).keeps("a/x.mkv", false));
    CHECK_FALSE(HitFilter(Mode::file, "X.mkv", true, true).keeps("a/x.mkv", false));
    const HitFilter path(Mode::folder, "/srv/.config/app", false, true);
    CHECK(path.keeps("app", true));
    CHECK_FALSE(path.keeps("apps", true));
}
