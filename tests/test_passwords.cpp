// grab's own password store (passwords.hpp): its text, its file in a temp folder, and the login
// each value is bound to. Every value here is made up.

#include "passwords.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#ifndef _WIN32
#include <system_error>
#endif

using namespace grab;
using passwords::Binding;
using passwords::Secret;
using passwords::Store;

namespace {

std::filesystem::path fresh_dir(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

Binding box() { return {.user = "u1", .host = "u1.your-storagebox.de", .port = 23}; }
Binding main_login() { return {.user = "dragon", .host = "main.example", .port = 39909}; }

Store sample() {
    return {{"Box", {{"password", {.binding = box(), .value = "fake-pass\twith tab\nand line"}}}},
            {"სერვერი",
             {{"passphrase", {.binding = main_login(), .value = "fake-პაროლი"}},
              {"key_pem", {.binding = main_login(), .value = "-----BEGIN FAKE-----\\nAAAA\\n-----END FAKE-----"}},
              {"rclone.set_env", {.binding = main_login(), .value = ""}}}}};
}

} // namespace

TEST_CASE("passwords: the store's text form survives a round trip; anything else is refused") {
    const auto store = sample();
    const auto text = passwords::to_text(store);
    CHECK(text.starts_with("grab-passwords 1\n"));
    CHECK(passwords::from_text(text) == store);
    CHECK(passwords::from_text("grab-passwords 1\n") == Store{});
    CHECK_FALSE(passwords::from_text("").has_value());
    CHECK_FALSE(passwords::from_text("grab-secrets 1\n").has_value());
    CHECK_FALSE(passwords::from_text("grab-passwords 1\nBox\tu\th\t22\tpassword\n").has_value()); // a field short
    CHECK_FALSE(passwords::from_text("grab-passwords 1\nBox\tu\th\t0\tpassword\tx\n").has_value()); // no such port
    CHECK_FALSE(passwords::from_text("grab-passwords 1\n\tu\th\t22\tpassword\tx\n").has_value());  // no server
    CHECK(Binding{.user = "u", .host = "h", .port = 2222}.text() == "u@h:2222");
}

TEST_CASE("passwords: a value is found only for the login it was saved for") {
    const auto store = sample();
    CHECK(passwords::value_for(store, "Box", passwords::password, box()) == "fake-pass\twith tab\nand line");
    // Another host, user or port (a hand edit of grab.conf) finds nothing.
    for (const auto& other : {Binding{.user = "u1", .host = "evil.example", .port = 23},
                              Binding{.user = "u2", .host = box().host, .port = 23},
                              Binding{.user = "u1", .host = box().host, .port = 22},
                              Binding{.user = "u1", .host = "U1.your-storagebox.de", .port = 23}}) {
        CHECK(passwords::value_for(store, "Box", passwords::password, other) == std::nullopt);
    }
    // find says what it was saved for, for "the saved password is for ...".
    const auto* saved = passwords::find(store, "Box", passwords::password);
    REQUIRE(saved != nullptr);
    CHECK(saved->binding.text() == "u1@u1.your-storagebox.de:23");
    CHECK(passwords::find(store, "Box", passwords::passphrase) == nullptr);
    CHECK(passwords::find(store, "box", passwords::password) == nullptr); // names as written
}

TEST_CASE("passwords: saved in the configuration's folder, sealed, and read back") {
    const auto dir = fresh_dir("grab-test-passwords");
    const auto file = passwords::path_in(dir);
    CHECK(file == dir / "passwords");
    CHECK(passwords::load(file) == Store{}); // none yet
    const auto store = sample();
    REQUIRE(passwords::save(file, store));
    CHECK(passwords::load(file) == store);
    const auto raw = util::read_file(file).value_or("");
#ifdef _WIN32
    CHECK(passwords::encrypted());
    CHECK(raw.starts_with("grab-passwords-sealed 1\n"));
    CHECK(raw.find("fake-pass") == std::string::npos);
    CHECK(raw.find("u1.your-storagebox.de") == std::string::npos);
    // Unsealed text isn't taken for a store.
    std::ofstream(file, std::ios::binary | std::ios::trunc) << passwords::to_text(store);
    CHECK(passwords::load(file).error_or("").find("can't be read") != std::string::npos);
#else
    CHECK_FALSE(passwords::encrypted());
    std::error_code ec;
    const auto perms = std::filesystem::status(file, ec).permissions();
    CHECK((perms & std::filesystem::perms::all) == (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write));
#endif
    // Damaged: an error, and the file is left alone.
    std::ofstream(file, std::ios::binary | std::ios::trunc) << "grab-passwords-sealed 1\nnot DPAPI";
    CHECK_FALSE(passwords::load(file).has_value());
    CHECK(util::read_file(file).value_or("") == "grab-passwords-sealed 1\nnot DPAPI");
    std::filesystem::remove_all(dir);
}
