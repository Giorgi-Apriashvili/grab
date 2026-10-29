// The import of servers from rclone.conf into grab.conf, grab's store and known_hosts
// (rclone_import.hpp; RCLONE-REPLACEMENT.md, phase 7 steps 7 and 13), on made-up rclone.conf files
// of every shape the plan lists. The import is a pure function: these cases hand it texts and
// fakes, and nothing is read or written on disk. Every password, passphrase and key here is
// made up.

#include "config.hpp"
#include "engine.hpp"
#include "ini.hpp"
#include "passwords.hpp"
#include "rclone_import.hpp"
#include "secrets.hpp"
#include "server_conf.hpp"
#include "server_ops.hpp"
#include "servers.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (SetFileAttributesW)
#endif

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace grab;
namespace fs = std::filesystem;
using rclone_import::Inputs;
using rclone_import::Note;
using rclone_import::Plan;

namespace {

// A folder that is never created: the import only uses paths as names.
fs::path root() { return fs::temp_directory_path() / "grab-test-import-not-created"; }
std::string at(std::string_view name) { return util::path_to_utf8((root() / util::path_from_utf8(name)).lexically_normal()); }

std::string obscured(std::string_view plain) {
    auto text = secrets::obscure(plain);
    REQUIRE(text.has_value());
    return *text;
}

// Made-up machine state: files that "exist", known_hosts files a remote names.
struct World {
    std::set<std::string> files;
    std::map<std::string, rclone_import::HostKeysFile> host_keys;
    std::map<std::string, std::string> env{{"HOME", at("home")}};
};

Inputs inputs(const World& world, std::string grab_text, std::vector<rclone_import::Source> sources) {
    Inputs in;
    in.sources = std::move(sources);
    for (const auto& s : in.sources) {
        in.stamps.push_back({.file = util::path_to_utf8(s.file), .size = s.text ? static_cast<std::int64_t>(s.text->size()) : -1, .time = 1});
    }
    in.grab_text = std::move(grab_text);
    in.known_hosts = util::path_from_utf8(at("grab/known_hosts"));
    in.getenv = [&world](std::string_view name) -> std::optional<std::string> {
        const auto it = world.env.find(std::string(name));
        if (it == world.env.end()) return std::nullopt;
        return it->second;
    };
    in.exists = [&world](const fs::path& p) { return world.files.contains(util::path_to_utf8(p)); };
    in.read_host_keys = [&world](const fs::path& p) {
        const auto it = world.host_keys.find(util::path_to_utf8(p));
        return it == world.host_keys.end() ? rclone_import::HostKeysFile{.text = std::nullopt, .error = "no such file"}
                                           : it->second;
    };
    return in;
}

rclone_import::Source source(std::string_view file, std::optional<std::string> text, secrets::Store store = {}) {
    return {.file = util::path_from_utf8(at(file)), .text = std::move(text), .error = {}, .store = std::move(store)};
}

Plan run(const Inputs& in) {
    auto p = rclone_import::plan(in);
    REQUIRE_MESSAGE(p.has_value(), p.error_or(""));
    return std::move(*p);
}

// A section of the planned grab.conf, as grab reads it.
ini::Section section_of(const Plan& p, std::string_view name) {
    const auto doc = ini::parse(p.grab_text);
    REQUIRE(doc.has_value());
    const auto* s = doc->find(name);
    REQUIRE(s != nullptr);
    return *s;
}

server_conf::Connection connection_of(const Plan& p, std::string_view name) {
    const auto c = server_conf::parse(section_of(p, name));
    REQUIRE_MESSAGE(c.has_value(), c.error_or(""));
    REQUIRE(c->has_value());
    return **c;
}

std::vector<std::string> notes_of(const Plan& p, std::string_view server) {
    std::vector<std::string> out;
    for (const auto& n : p.notes) {
        if (n.server == server) out.push_back(n.text);
    }
    return out;
}

bool has_note(const Plan& p, std::string_view server, std::string_view part) {
    return std::ranges::any_of(notes_of(p, server), [&](const std::string& t) { return t.find(part) != std::string::npos; });
}

// The plan's output as the next run's input: what the apply step would have written.
Inputs after(const Inputs& in, const Plan& p) {
    Inputs next = in;
    next.grab_text = p.grab_text;
    next.store = p.store;
    next.record = p.record;
    for (const auto& line : p.host_key_lines) next.known_hosts_text += line + "\n";
    return next;
}

constexpr std::string_view grab_head = "[grab]\ndefault_remote = Main\n\n";

} // namespace

TEST_CASE("import: grab's own rclone.conf and its store become grab.conf sections, plain passwords and pins") {
    World world;
    const std::string main_key = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIFAKEFAKEFAKEFAKEFAKEFAKEFAKEFAKEFAKEFAKEFAKE";
    const std::string rclone = "[Main]\ntype = sftp\nhost = main.example\nport = 39909\nuser = dragon\n"
                               "key_use_agent = true\nknown_hosts_file = " + at("grab/known_hosts") + "\n"
                               "shell_type = unix\nmd5sum_command = md5sum\nsha1sum_command = sha1sum\n\n"
                               "[Box]\ntype = sftp\nhost = u1.your-storagebox.de\nport = 23\nuser = u1\n"
                               "known_hosts_file = " + at("grab/known_hosts") + "\nshell_type = unix\n";
    const secrets::Store old{{"Box", {{"pass", obscured("fake-box-password")}}}};
    const std::string grab = std::string(grab_head) +
                             "# rclone remote [Main]: dragon@main.example:39909, ssh-agent, lookup via ssh + find\n"
                             "[Main]\nrclone_remote = Main\nsearch_roots = /srv\nmax_depth = 3\n\n"
                             "[Box]\nrclone_remote = Box\nfind = rclone\n";
    auto in = inputs(world, grab, {source("grab/rclone.conf", rclone, old), source("rclone/rclone.conf", std::nullopt)});
    in.known_hosts_text = "[main.example]:39909 " + main_key + "\n[u1.your-storagebox.de]:23 " + main_key + "\n";
    const auto p = run(in);

    CHECK(p.failed.empty());
    REQUIRE(p.imported.size() == 2);
    const auto m = connection_of(p, "Main");
    CHECK(m.host == "main.example");
    CHECK(m.port == 39909);
    CHECK(m.user == "dragon");
    CHECK(m.auth == server_conf::Auth::agent);
    CHECK_FALSE(m.key_file.has_value());
    CHECK(m.shell_type == "unix");
    CHECK(m.md5sum_command == "md5sum");
    CHECK(m.sha1sum_command == "sha1sum");
    const auto b = connection_of(p, "Box");
    CHECK(b.auth == server_conf::Auth::password);
    CHECK(b.port == 23);
    // The password plain, bound to the login it was saved for; no secret in grab.conf.
    const passwords::Binding box_login{.user = "u1", .host = "u1.your-storagebox.de", .port = 23};
    CHECK(passwords::value_for(p.store, "Box", passwords::password, box_login) == "fake-box-password");
    CHECK(p.grab_text.find("fake-box-password") == std::string::npos);
    CHECK(p.grab_text.find("pass =") == std::string::npos);
    CHECK_FALSE(p.store.contains("Main"));
    // Everything else in the section stays as it was, the keys right after its header.
    CHECK(section_of(p, "Main").get("search_roots") == "/srv");
    CHECK(p.grab_text.find("[Main]\nhost = main.example\nport = 39909\nuser = dragon\nauth = agent\nshell_type = unix\n"
                           "md5sum_command = md5sum\nsha1sum_command = sha1sum\nrclone_remote = Main\n") != std::string::npos);
    CHECK(p.grab_text.find("# rclone remote [Main]") != std::string::npos);
    // Both pinned already in grab's known_hosts: nothing to copy, no note.
    CHECK(p.host_key_lines.empty());
    CHECK(std::ranges::all_of(p.imported, [](const rclone_import::Imported& i) { return i.pinned; }));
    CHECK(p.notes.empty());
    CHECK(p.record.imported == std::set<std::string, std::less<>>{"Box", "Main"});
    CHECK(p.record.banner == std::vector<std::string>{"Main", "Box"});
    CHECK(p.changes(in));

    // A second run over its own output changes nothing.
    const auto again = after(in, p);
    const auto p2 = run(again);
    CHECK_FALSE(p2.changes(again));
    CHECK(p2.imported.empty());
    CHECK(p2.notes.empty());
    CHECK(p2.grab_text == p.grab_text);

    // A name once imported is never imported again, even when its details are taken out by hand.
    auto edited = again;
    edited.grab_text = std::string(grab_head) + "[Main]\nrclone_remote = Main\n";
    CHECK_FALSE(run(edited).changes(edited));

    // grab 0.19's reading of grab.conf (by key name) sees the same servers as before.
    const auto before = parse_grab_config(*ini::parse(grab));
    const auto now = parse_grab_config(*ini::parse(p.grab_text));
    REQUIRE((before.has_value() && now.has_value()));
    REQUIRE(before->remotes.size() == now->remotes.size());
    for (std::size_t i = 0; i < before->remotes.size(); ++i) {
        const auto& x = before->remotes[i];
        const auto& y = now->remotes[i];
        CHECK(x.name == y.name);
        CHECK(x.rclone_remote == y.rclone_remote);
        CHECK(x.search_roots == y.search_roots);
        CHECK(x.max_depth == y.max_depth);
        CHECK(x.find == y.find);
        CHECK(x.common_flags == y.common_flags);
    }
}

TEST_CASE("import: a value grab's store holds for the same login stays; one for another login is replaced") {
    // grab.conf deleted and `grab --init` run: Box is imported again from the 0.19 files, whose
    // password is older than the one saved with Edit in grab.
    World world;
    const std::string rclone = "[Box]\ntype = sftp\nhost = u1.example\nport = 23\nuser = u1\npass = " +
                               obscured("fake-old-password") + "\n\n[Moved]\ntype = sftp\nhost = new.example\nuser = u\n"
                               "pass = " + obscured("fake-moved-password") + "\n";
    auto in = inputs(world, std::string(grab_head) + "[Box]\n[Moved]\n", {source("grab/rclone.conf", rclone)});
    const passwords::Binding box{.user = "u1", .host = "u1.example", .port = 23};
    const passwords::Binding old_moved{.user = "u", .host = "old.example", .port = 22};
    in.store = {{"Box", {{"password", {.binding = box, .value = "fake-new-password"}}}},
                {"Moved", {{"password", {.binding = old_moved, .value = "fake-elsewhere"}},
                           {"rclone.set_env", {.binding = old_moved, .value = "FAKE=1"}}}}};
    const auto p = run(in);
    REQUIRE(p.failed.empty());
    CHECK(passwords::value_for(p.store, "Box", passwords::password, box) == "fake-new-password");
    CHECK(has_note(p, "Box", "kept the password saved in grab"));
    const passwords::Binding moved{.user = "u", .host = "new.example", .port = 22};
    CHECK(p.store.at("Moved") == std::map<std::string, passwords::Secret, std::less<>>{
                                     {"password", {.binding = moved, .value = "fake-moved-password"}}});
    CHECK_FALSE(has_note(p, "Moved", "kept the password"));
    // The same value saved in both says nothing.
    in.store["Box"]["password"].value = "fake-old-password";
    CHECK_FALSE(has_note(run(in), "Box", "kept the password"));
}

TEST_CASE("import: an explicit rclone.conf's inline secrets win over the store beside it; a standard one is read too") {
    World world;
    world.files.insert(at("keys/id_k"));
    const std::string rclone = "[inline]\ntype = sftp\nhost = in.example\nuser = u\npass = " + obscured("fake-inline") +
                               "\n\n[keyed]\ntype = sftp\nhost = k.example\nuser = u\nkey_file = " + at("keys/id_k") +
                               "\npass = " + obscured("fake-beside-key") + "\n";
    const secrets::Store old{{"inline", {{"pass", obscured("fake-older")}}},
                             {"keyed", {{"key_file_pass", obscured("fake-passphrase")}}}};
    auto in = inputs(world, std::string(grab_head) + "[inline]\n[keyed]\n", {source("mine/rclone.conf", rclone, old)});
    const auto p = run(in);
    REQUIRE(p.failed.empty());
    const passwords::Binding inline_login{.user = "u", .host = "in.example", .port = 22};
    CHECK(passwords::value_for(p.store, "inline", passwords::password, inline_login) == "fake-inline");
    const auto k = connection_of(p, "keyed");
    CHECK(k.auth == server_conf::Auth::key);
    CHECK(k.key_file == at("keys/id_k"));
    const passwords::Binding keyed{.user = "u", .host = "k.example", .port = 22};
    CHECK(passwords::value_for(p.store, "keyed", passwords::passphrase, keyed) == "fake-passphrase");
    // A password beside a key is kept: rclone tries it after the key.
    CHECK(passwords::value_for(p.store, "keyed", passwords::password, keyed) == "fake-beside-key");
    // Not pinned (no known_hosts_file; grab's has none for them): named, to Trust.
    CHECK(has_note(p, "inline", "use Trust"));

    // grab's own rclone.conf first, then the standard one.
    auto both = inputs(world, std::string(grab_head) + "[std]\n[own]\n",
                       {source("grab/rclone.conf", "[own]\ntype = sftp\nhost = own.example\nuser = o\n"),
                        source("rclone/rclone.conf", "[std]\ntype = sftp\nhost = std.example\nuser = s\n"
                                                     "[own]\ntype = sftp\nhost = other.example\nuser = x\n")});
    const auto p2 = run(both);
    CHECK(connection_of(p2, "std").host == "std.example");
    CHECK(connection_of(p2, "own").host == "own.example");
    CHECK(p2.imported.at(0).from == util::path_from_utf8(at("rclone/rclone.conf")));
}

TEST_CASE("import: encrypted, non-sftp, missing and broken sources leave a section without details, with a note") {
    World world;
    const std::string grab = std::string(grab_head) + "[enc]\n[alias]\n[gone]\n[nohost]\n[dash]\n";
    const auto in = inputs(world, grab,
                           {source("grab/rclone.conf", "[alias]\ntype = alias\nremote = C:\\x\n"
                                                       "[nohost]\ntype = sftp\nuser = u\n"
                                                       "[dash]\ntype = sftp\nhost = -oProxyCommand=calc\nuser = u\n"),
                            source("rclone/rclone.conf",
                                   "# Encrypted rclone configuration File\n\nRCLONE_ENCRYPT_V0:\nFAKEFAKEFAKE\n")});
    const auto p = run(in);
    CHECK(p.imported.empty());
    CHECK(p.failed == std::vector<std::string>{"enc", "alias", "gone", "nohost", "dash"});
    CHECK(has_note(p, "enc", "is encrypted"));
    CHECK(has_note(p, "alias", "is a alias remote; grab needs sftp"));
    CHECK(has_note(p, "gone", "is encrypted")); // looked for in the standard one, which is encrypted
    CHECK(has_note(p, "nohost", "has no host"));
    CHECK(has_note(p, "dash", "host: the host can't start with -")); // G5
    CHECK(p.grab_text == grab);
    CHECK(p.store.empty());

    // Tried again only when a source file changes.
    const auto same = after(in, p);
    const auto p2 = run(same);
    CHECK_FALSE(p2.changes(same));
    CHECK(p2.notes.empty());
    auto changed = same;
    changed.stamps.front().time = 2;
    const auto p3 = run(changed);
    CHECK(p3.failed.size() == 5);
    CHECK(p3.changes(changed));

    // No rclone.conf at all.
    const auto none = inputs(world, std::string(grab_head) + "[x1]\n",
                             {source("grab/rclone.conf", std::nullopt), source("rclone/rclone.conf", std::nullopt)});
    const auto p4 = run(none);
    CHECK(has_note(p4, "x1", "there is no rclone remote [x1] in "));
    // A file rclone refuses as a whole.
    const auto broken = inputs(world, std::string(grab_head) + "[x1]\n", {source("grab/rclone.conf", "[x1] # c\n")});
    CHECK(has_note(run(broken), "x1", "rclone can't read"));

    // A wrong rclone_remote corrected in grab.conf is tried again, though no source file changed.
    const std::string rclone = "[Main]\ntype = sftp\nhost = main.example\nuser = u\nkey_use_agent = true\n";
    const auto typo = inputs(world, std::string(grab_head) + "[Main2]\nrclone_remote = Mian\n",
                             {source("grab/rclone.conf", rclone)});
    const auto p5 = run(typo);
    CHECK(has_note(p5, "Main2", "there is no rclone remote [Mian]"));
    CHECK(p5.record.failed.at("Main2").remote == "Mian");
    const auto unchanged = after(typo, p5);
    CHECK_FALSE(run(unchanged).changes(unchanged)); // not tried again
    auto fixed = unchanged;
    fixed.grab_text = std::string(grab_head) + "[Main2]\nrclone_remote = Main\n";
    const auto p6 = run(fixed);
    CHECK(p6.failed.empty());
    CHECK(connection_of(p6, "Main2").host == "main.example");
    CHECK_FALSE(p6.record.failed.contains("Main2"));
}

TEST_CASE("import: a dotted name takes non-secret keys from its parent, never a secret (G86)") {
    World world;
    world.files.insert(at("keys/box"));
    const std::string rclone = "[box]\ntype = sftp\nhost = box.example\nuser = u\nkey_file = " + at("keys/box") +
                               "\nkey_file_pass = " + obscured("fake-box-passphrase") +
                               "\n\n[box.eu]\nport = 23\n\n[box.pw]\nhost = pw.example\nkey_file =\n"
                               "[p]\ntype = sftp\nhost = p.example\nuser = u\npass = " + obscured("fake-p-password") +
                               "\n[p.c]\nport = 2222\n";
    const secrets::Store old{{"box.pw", {{"pass", obscured("fake-pw-password")}}}};
    const auto p = run(inputs(world, std::string(grab_head) + "[box]\n[box.eu]\n[box.pw]\n[p]\n[p.c]\n",
                              {source("grab/rclone.conf", rclone, old)}));
    REQUIRE(p.failed.empty());
    const auto eu = connection_of(p, "box.eu");
    CHECK(eu.host == "box.example");
    CHECK(eu.port == 23);
    CHECK(eu.auth == server_conf::Auth::key);
    CHECK(eu.key_file == at("keys/box"));
    const auto& imported_eu = *std::ranges::find(p.imported, std::string("box.eu"), &rclone_import::Imported::name);
    CHECK(imported_eu.from_parent == std::vector<std::string>{"host", "key_file", "user"});
    CHECK(has_note(p, "box.eu", "host, key_file, type, user came from [box]"));
    // [box]'s passphrase isn't box.eu's.
    CHECK(passwords::find(p.store, "box.eu", passwords::passphrase) == nullptr);
    CHECK(passwords::find(p.store, "box", passwords::passphrase) != nullptr);
    // ...but rclone, reading the file, used it: the user is told to enter it.
    CHECK(has_note(p, "box.eu", "[box]'s key passphrase isn't taken for it (secrets aren't inherited): enter it with Edit"));
    CHECK_FALSE(has_note(p, "box.pw", "isn't taken")); // its own password; no key, so no passphrase
    // A parent's password alone: a password login with none saved, and the same note.
    CHECK(connection_of(p, "p.c").auth == server_conf::Auth::password);
    CHECK(connection_of(p, "p.c").port == 2222);
    CHECK(passwords::find(p.store, "p.c", passwords::password) == nullptr);
    CHECK(has_note(p, "p.c", "[p]'s password isn't taken for it"));
    CHECK(connection_of(p, "p").auth == server_conf::Auth::password);
    // A blank key_file stops the chain: box.pw logs in with its own password, not [box]'s key.
    const auto pw = connection_of(p, "box.pw");
    CHECK(pw.host == "pw.example");
    CHECK(pw.auth == server_conf::Auth::password);
    CHECK_FALSE(pw.key_file.has_value());
    CHECK(passwords::value_for(p.store, "box.pw", passwords::password, {.user = "u", .host = "pw.example", .port = 22}) ==
          "fake-pw-password");
}

TEST_CASE("import: paths as rclone expands them, made absolute; a backticked key_file") {
    World world;
    world.env["USERPROFILE"] = at("profile");
    world.env["KEYS"] = at("env-keys");
    const std::string rclone =
        "[tilde]\ntype = sftp\nhost = t.example\nuser = u\nkey_file = ~/.ssh/id_ed25519\n"
        "[dir]\ntype = sftp\nhost = d.example\nuser = u\nkey_file = ${RCLONE_CONFIG_DIR}/id_d\n"
        "[var]\ntype = sftp\nhost = v.example\nuser = u\nkey_file = $KEYS/id_v\n"
        "[tick]\ntype = sftp\nhost = b.example\nuser = u\nkey_file = `" + at("my keys/id b") + "`\n"
        "[rel]\ntype = sftp\nhost = r.example\nuser = u\nkey_file = id_rel\n";
    world.files = {at("home/.ssh/id_ed25519"), at("$cfg/id_d"), at("env-keys/id_v"), at("my keys/id b")};
    const auto p = run(inputs(world, std::string(grab_head) + "[tilde]\n[dir]\n[var]\n[tick]\n[rel]\n",
                              {source("$cfg/rclone.conf", rclone)}));
    REQUIRE(p.failed.empty());
    CHECK(connection_of(p, "tilde").key_file == at("home/.ssh/id_ed25519")); // HOME before USERPROFILE
    CHECK(connection_of(p, "dir").key_file == at("$cfg/id_d"));              // the file's folder, its '$' kept
    CHECK(connection_of(p, "var").key_file == at("env-keys/id_v"));
    CHECK(connection_of(p, "tick").key_file == at("my keys/id b"));
    CHECK(connection_of(p, "rel").key_file == at("$cfg/id_rel"));
    CHECK(has_note(p, "rel", "doesn't say which folder it is in"));
    CHECK(has_note(p, "rel", "isn't there"));
    CHECK_FALSE(has_note(p, "tilde", "isn't there"));
}

TEST_CASE("import: login methods in rclone's order (agent with key_file, key_pem, ask_password)") {
    World world;
    world.files.insert(at("keys/id_agent.pub"));
    const std::string pem = "-----BEGIN OPENSSH PRIVATE KEY-----\\nFAKEFAKEFAKE\\n-----END OPENSSH PRIVATE KEY-----";
    const std::string rclone =
        "[agent]\ntype = sftp\nhost = a.example\nuser = u\nkey_use_agent = true\nkey_file = " + at("keys/id_agent") + "\n"
        "[pem]\ntype = sftp\nhost = p.example\nuser = u\nkey_pem = " + pem + "\nkey_file_pass = " +
        obscured("fake-pem-pass") + "\nkey_file = " + at("keys/ignored") + "\n"
        "[ask]\ntype = sftp\nhost = q.example\nuser = u\nask_password = true\n"
        "[plain]\ntype = sftp\nhost = n.example\nuser = u\n"
        "[badbool]\ntype = sftp\nhost = x.example\nuser = u\nkey_use_agent = yes\n"
        "[badpass]\ntype = sftp\nhost = y.example\nuser = u\npass = not-obscured\n";
    const auto p = run(inputs(world, std::string(grab_head) + "[agent]\n[pem]\n[ask]\n[plain]\n[badbool]\n[badpass]\n",
                              {source("grab/rclone.conf", rclone)}));
    const auto agent = connection_of(p, "agent");
    CHECK(agent.auth == server_conf::Auth::agent);
    CHECK(agent.key_file == at("keys/id_agent")); // rclone's filter: only that agent key
    CHECK_FALSE(has_note(p, "agent", "isn't there")); // its .pub is there
    const auto pemc = connection_of(p, "pem");
    CHECK(pemc.auth == server_conf::Auth::stored_key);
    CHECK_FALSE(pemc.key_file.has_value()); // rclone takes key_pem over key_file
    const passwords::Binding pem_login{.user = "u", .host = "p.example", .port = 22};
    CHECK(passwords::value_for(p.store, "pem", passwords::key_pem, pem_login) == pem); // as rclone keeps it
    CHECK(passwords::value_for(p.store, "pem", passwords::passphrase, pem_login) == "fake-pem-pass");
    CHECK(connection_of(p, "ask").auth == server_conf::Auth::password);
    CHECK_FALSE(p.store.contains("ask"));
    CHECK(has_note(p, "ask", "ask_password"));
    CHECK(connection_of(p, "plain").auth == server_conf::Auth::agent); // rclone's fallback
    CHECK(has_note(p, "badbool", "rclone can't read either"));
    CHECK(connection_of(p, "badpass").auth == server_conf::Auth::password);
    CHECK(has_note(p, "badpass", "can't be read (it isn't in rclone's obscured form)"));
    CHECK_FALSE(p.store.contains("badpass"));
}

TEST_CASE("import: an old store that can't be read keeps password servers back; agent ones come") {
    World world;
    const std::string rclone = "[pw]\ntype = sftp\nhost = pw.example\nuser = u\n"
                               "[ag]\ntype = sftp\nhost = ag.example\nuser = u\nkey_use_agent = true\n";
    auto src = source("grab/rclone.conf", rclone);
    src.store = std::unexpected(std::string("the saved passwords in secrets can't be read (fake)"));
    const auto p = run(inputs(world, std::string(grab_head) + "[pw]\n[ag]\n", {src}));
    CHECK(p.failed == std::vector<std::string>{"pw"});
    CHECK(has_note(p, "pw", "can't be read (fake)"));
    CHECK(connection_of(p, "ag").auth == server_conf::Auth::agent);
}

TEST_CASE("import: rclone settings go to grab.conf when allowlisted, else to the store") {
    World world;
    const std::string rclone = "[s]\ntype = sftp\nhost = s.example\nuser = u\nciphers = aes256-ctr\n"
                               "pubkey_file = ${RCLONE_CONFIG_DIR}/cert.pub\nset_env = FAKE_TOKEN=fake-secret\n"
                               "ssh = ssh -i fake\nfoo_bar = fake-unknown\nFoo = not-an-option\npath_override = /data\n"
                               "description = my box\n";
    const auto p = run(inputs(world, std::string(grab_head) + "[s]\n", {source("grab/rclone.conf", rclone)}));
    const auto c = connection_of(p, "s");
    CHECK(c.path_override == "/data");
    CHECK(c.rclone == std::vector<std::pair<std::string, std::string>>{
                          {"ciphers", "aes256-ctr"}, {"description", "my box"}, {"pubkey_file", at("grab/cert.pub")}});
    CHECK(c == p.imported.front().connection); // as planned, so as grab reads it back
    const passwords::Binding login{.user = "u", .host = "s.example", .port = 22};
    CHECK(passwords::value_for(p.store, "s", "rclone.set_env", login) == "FAKE_TOKEN=fake-secret");
    CHECK(passwords::value_for(p.store, "s", "rclone.ssh", login) == "ssh -i fake");
    CHECK(passwords::value_for(p.store, "s", "rclone.foo_bar", login) == "fake-unknown");
    CHECK(passwords::find(p.store, "s", "rclone.Foo") == nullptr); // rclone never read it
    CHECK(p.grab_text.find("fake-secret") == std::string::npos);
    CHECK(p.grab_text.find("fake-unknown") == std::string::npos);
}

TEST_CASE("import: a remote's own known_hosts lines for its host:port are copied (decision 2)") {
    World world;
    const std::string key = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIFAKEFAKEFAKEFAKEFAKEFAKEFAKEFAKEFAKEFAKEFAKE";
    const std::string kh = "# a comment\n"
                           "other.example,[h.example]:2222 " + key + " c1\n"
                           "[h.example]:22 " + key + " wrong-port\n"
                           "@cert-authority [h.example]:2222 " + key + "\n"
                           "*.example " + key + "\n"
                           "[w.example]:2222 " + key + "\n";
    world.host_keys[at("own/kh")] = {.text = kh, .error = {}};
    world.host_keys[at("own/big")] = {.text = std::nullopt, .error = "over 1 MiB"};
    const std::string rclone = "[h]\ntype = sftp\nhost = h.example\nport = 2222\nuser = u\nknown_hosts_file = " +
                               at("own/kh") + "\n"
                               "[w]\ntype = sftp\nhost = w.example\nport = 22\nuser = u\nknown_hosts_file = " +
                               at("own/kh") + "\n"
                               "[big]\ntype = sftp\nhost = big.example\nuser = u\nknown_hosts_file = " + at("own/big") + "\n"
                               "[unc]\ntype = sftp\nhost = unc.example\nuser = u\nknown_hosts_file = \\\\server\\share\\kh\n"
                               "[none]\ntype = sftp\nhost = none.example\nuser = u\nknown_hosts_file = none\n"
                               "[again]\ntype = sftp\nhost = h.example\nport = 2222\nuser = v\nknown_hosts_file = " +
                               at("own/kh") + "\n";
    const auto in = inputs(world, std::string(grab_head) + "[h]\n[w]\n[big]\n[unc]\n[none]\n[again]\n",
                           {source("grab/rclone.conf", rclone)});
    const auto p = run(in);
    REQUIRE(p.failed.empty());
    // Only h's line, cut down to its own name; a second server at that address copies nothing more.
    CHECK(p.host_key_lines == std::vector<std::string>{"[h.example]:2222 " + key + " c1"});
    CHECK(has_note(p, "h", "a @cert-authority line for it in"));
    CHECK(std::ranges::find(p.imported, std::string("again"), &rclone_import::Imported::name)->pinned);
    // w.example on 22 has only the wildcard line.
    CHECK(has_note(p, "w", "a wildcard line (*.example)"));
    CHECK(has_note(p, "w", "isn't trusted yet: use Trust"));
    CHECK(has_note(p, "big", "over 1 MiB"));
    CHECK(has_note(p, "unc", "network or device path"));
    CHECK(has_note(p, "none", "isn't trusted yet"));
    // grab's known_hosts already pins it: nothing is copied.
    auto pinned = in;
    pinned.known_hosts_text = "[h.example]:2222 ssh-rsa AAAAFAKE\n";
    CHECK(run(pinned).host_key_lines.empty());

    // The lines themselves: a hashed name stays hashed; markers and patterns are only noted.
    const auto lines = rclone_import::host_key_lines(kh, "h.example", 2222);
    CHECK(lines.lines.size() == 1);
    CHECK(lines.skipped == std::vector<std::string>{"a @cert-authority line"});
    CHECK(rclone_import::host_key_lines("[h.example]:2222,!h.example " + key + "\n", "h.example", 2222).lines.size() == 1);
    CHECK(rclone_import::host_key_lines("[h.example]:2222,![h.example]:2222 " + key + "\n", "h.example", 2222)
              .lines.empty());
    // A key the file revokes, for any host, isn't copied (Go's knownhosts refuses it); others are.
    const std::string k1 = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIFAKEREVOKEDFAKEREVOKEDFAKEREVOKEDFAKEREVOKED";
    const std::string k2 = "ecdsa-sha2-nistp256 AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYFAKEFAKEFAKE";
    const auto revoked = rclone_import::host_key_lines(
        "@revoked * " + k1 + "\n[h.example]:2222 " + k1 + "\n[h.example]:2222 " + k2 + "\n", "h.example", 2222);
    CHECK(revoked.lines == std::vector<std::string>{"[h.example]:2222 " + k2});
    CHECK(revoked.skipped == std::vector<std::string>{"a line with a key that file revokes"});
    CHECK(rclone_import::host_key_lines("@revoked other.example " + k1 + "\n[h.example]:2222 " + k1 + "\n", "h.example",
                                        2222)
              .lines.empty());
    CHECK(rclone_import::network_or_device("\\\\server\\share\\kh"));
    CHECK(rclone_import::network_or_device("//server/share/kh"));
    CHECK(rclone_import::network_or_device("\\\\.\\pipe\\x"));
    CHECK(rclone_import::network_or_device("C:\\x\\NUL"));
    CHECK(rclone_import::network_or_device("C:\\x\\com1.txt"));
    CHECK(rclone_import::network_or_device("\\?" "?\\UNC\\s\\x")); // NT's namespace
    CHECK(rclone_import::network_or_device("/?" "?/C:/x"));
    CHECK(rclone_import::network_or_device("\\?" "?\\C:\\x"));
    CHECK_FALSE(rclone_import::network_or_device("C:\\x\\known_hosts"));
    CHECK_FALSE(rclone_import::network_or_device("C:\\x\\console"));
}

TEST_CASE("import: Georgian and case-only names; sections that share a remote; grab.conf's own details win") {
    World world;
    const std::string geo = "\u10E1\u10D4\u10E0\u10D5\u10D4\u10E0\u10D8";
    const std::string rclone = "[" + geo + "]\ntype = sftp\nhost = ge.example\nuser = u\npass = " +
                               obscured("fake-\u10DE\u10D0\u10E0\u10DD\u10DA\u10D8") +
                               "\n[box]\ntype = sftp\nhost = box.example\nuser = u\npass = " + obscured("fake-shared") +
                               "\n[Box]\ntype = sftp\nhost = BOX.example\nuser = u\n[x]\ntype = sftp\nhost = x.example\nuser = u\n";
    const std::string grab = std::string(grab_head) + "[" + geo + "]\n[box]\n[Box]\n[copy]\nrclone_remote = box\n"
                             "[x]\n[hand]\nhost = hand.example\nuser = h\nauth = agent\n";
    const auto p = run(inputs(world, grab, {source("grab/rclone.conf", rclone)}));
    CHECK(p.failed.empty());
    CHECK(connection_of(p, geo).host == "ge.example");
    CHECK(passwords::value_for(p.store, geo, passwords::password, {.user = "u", .host = "ge.example", .port = 22}) ==
          "fake-\u10DE\u10D0\u10E0\u10DD\u10DA\u10D8");
    CHECK(notes_of(p, geo) == std::vector<std::string>{"its host key isn't trusted yet: use Trust (grab server trust " +
                                                       geo + ")"});
    CHECK(has_note(p, "box", "differs from [Box] only in case"));
    CHECK(has_note(p, "Box", "differs from [box] only in case"));
    CHECK(has_note(p, "x", "one character"));
    // Each section that uses one remote gets its own copy and its own stored password.
    CHECK(connection_of(p, "copy").host == "box.example");
    CHECK(passwords::value_for(p.store, "copy", passwords::password, {.user = "u", .host = "box.example", .port = 22}) ==
          "fake-shared");
    // A section with a host is never touched.
    CHECK(std::ranges::none_of(p.imported, [](const auto& i) { return i.name == "hand"; }));
    CHECK(section_of(p, "hand").values.size() == 3);
}

TEST_CASE("imported.json keeps what the import did") {
    rclone_import::Record r;
    r.imported = {"Main", "\u10E1\u10D4\u10E0\u10D5\u10D4\u10E0\u10D8"};
    r.failed["Box"] = {.remote = "Box.eu",
                       .stamps = {{.file = "C:\\x\\rclone.conf", .size = 120, .time = 133744204123456789},
                                  {.file = "C:\\x\\secrets", .size = -1, .time = 0}}};
    r.banner = {"Main"};
    const auto text = rclone_import::to_json(r);
    CHECK(rclone_import::from_json(text) == r);
    CHECK(text.find("133744204123456789") != std::string::npos); // exact, not a double
    CHECK_FALSE(rclone_import::from_json("{}").has_value());
    CHECK_FALSE(rclone_import::from_json("not json").has_value());
    CHECK(rclone_import::record_path(root()) == root() / "imported.json");

    GrabConfig cfg;
    CHECK(rclone_import::source_files(cfg) ==
          std::vector<fs::path>{grab_rclone_config_path(), standard_rclone_config_path()});
    cfg.rclone_config = root() / "mine.conf";
    CHECK(rclone_import::source_files(cfg) == std::vector<fs::path>{root() / "mine.conf"});
}

// ---- the apply step, on disk ---------------------------------------------------------------------
// Made-up files only: grab's old rclone.conf and 'secrets', rclone's standard rclone.conf and
// grab's known_hosts all live in the test runner's scratch APPDATA (test_main.cpp), grab.conf in
// a temp folder, its state_dir (store, imported.json) under the scratch APPDATA too.

namespace {

// A configuration of grab 0.19: grab's rclone.conf with an agent server whose own known_hosts
// pins it, a password server (its password in the old store), a dotted name under it and a
// Georgian one; rclone's standard rclone.conf with another agent server.
struct Old {
    fs::path dir = fs::temp_directory_path() / "grab-test-import-apply";
    fs::path conf = dir / "grab.conf";
    fs::path own = grab_rclone_config_path();
    fs::path standard = standard_rclone_config_path();
    fs::path old_store = secrets::path_beside(grab_rclone_config_path());
    rclone_import::Files files = rclone_import::files_for(conf);
    std::string grab_text;
    std::string own_text;
    std::string standard_text;
    std::string old_store_bytes;

    Old() {
        REQUIRE(util::path_to_utf8(own).find("grab-tests-home") != std::string::npos);
        REQUIRE(util::path_to_utf8(standard).find("grab-tests-home") != std::string::npos);
        clean();
        fs::create_directories(dir);
        fs::create_directories(own.parent_path());
        fs::create_directories(standard.parent_path());
        const std::string georgian = "\xE1\x83\xA1\xE1\x83\x94\xE1\x83\xA0\xE1\x83\x95\xE1\x83\x94\xE1\x83\xA0\xE1\x83\x98";
        grab_text = "[grab]\ndefault_remote = Main\n\n# rclone remote [Main]: me@main.example:2222\n[Main]\n"
                    "rclone_remote = Main\nsearch_roots = /srv\n\n[Box]\nmax_depth = 3\n\n[Box.eu]\n\n[" +
                    georgian + "]\n\n[Std]\nengine = grab\n";
        write(conf, grab_text);
        write(dir / "main_known_hosts", "[main.example]:2222 ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIMAIN\n");
        own_text = "[Main]\ntype = sftp\nhost = main.example\nport = 2222\nuser = me\nkey_use_agent = true\n"
                   "known_hosts_file = " + util::path_to_utf8(dir / "main_known_hosts") + "\nshell_type = unix\n"
                   "md5sum_command = md5sum\nsha1sum_command = sha1sum\n\n"
                   "[Box]\ntype = sftp\nhost = box.example\nport = 23\nuser = u1\n\n"
                   "[Box.eu]\nport = 24\n\n[" + georgian + "]\ntype = sftp\nhost = ge.example\nuser = g\nkey_use_agent = true\n";
        write(own, own_text);
        standard_text = "[Std]\ntype = sftp\nhost = std.example\nuser = s\nkey_use_agent = true\n";
        write(standard, standard_text);
        REQUIRE(secrets::save(old_store, {{"Box", {{"pass", obscured("fake-box-password")}}}}));
        old_store_bytes = util::read_file(old_store).value_or("");
    }
    ~Old() {
        try {
            clean();
        } catch (...) { // only std::bad_alloc, which a destructor must not throw
            std::fputs("grab_tests: a temp folder was left behind\n", stderr);
        }
    }
    Old(const Old&) = delete;
    Old& operator=(const Old&) = delete;
    void clean() const {
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::remove_all(files.state_dir, ec);
        for (const auto& f : {own, standard, old_store, grab_known_hosts_path()}) fs::remove(f, ec);
    }
    static void write(const fs::path& p, const std::string& text) { REQUIRE(util::write_file_atomic(p, text)); }
    [[nodiscard]] std::string grab() const { return util::read_file(conf).value_or(""); }
    [[nodiscard]] passwords::Store store() const { return passwords::load(passwords::path_in(files.state_dir)).value_or(passwords::Store{}); }
    [[nodiscard]] static std::string pins() { return util::read_file(grab_known_hosts_path()).value_or(""); }
    [[nodiscard]] std::optional<rclone_import::Record> record() const {
        auto text = util::read_file(rclone_import::record_path(files.state_dir));
        if (!text) return std::nullopt;
        return rclone_import::from_json(*text);
    }
    // What grab never writes, as it was.
    void untouched() const {
        CHECK(util::read_file(own).value_or("") == own_text);
        CHECK(util::read_file(standard).value_or("") == standard_text);
        CHECK(util::read_file(old_store).value_or("") == old_store_bytes);
    }
};

struct Done {
    std::string grab;
    passwords::Store store;
    std::string pins;
    std::optional<rclone_import::Record> record;
    bool operator==(const Done&) const = default;
};
Done done_of(const Old& old) { return Done{.grab = old.grab(), .store = old.store(), .pins = Old::pins(), .record = old.record()}; }

} // namespace

TEST_CASE("apply: grab 0.19's servers into grab.conf, the store and known_hosts; rclone.conf and the old store untouched") {
    Old old;
    const auto applied = rclone_import::apply(old.files);
    REQUIRE_MESSAGE(applied.has_value(), applied.error_or(""));
    std::vector<std::string> names;
    for (const auto& s : applied->imported) names.push_back(s.name);
    const std::string georgian = "\xE1\x83\xA1\xE1\x83\x94\xE1\x83\xA0\xE1\x83\x95\xE1\x83\x94\xE1\x83\xA0\xE1\x83\x98";
    CHECK(names == std::vector<std::string>{"Main", "Box", "Box.eu", georgian, "Std"});
    const auto text = old.grab();
    CHECK(text.find("[Main]\nhost = main.example\nport = 2222\nuser = me\nauth = agent\nshell_type = unix\n"
                    "md5sum_command = md5sum\nsha1sum_command = sha1sum\nrclone_remote = Main\nsearch_roots = /srv\n") !=
          std::string::npos);
    CHECK(text.find("# rclone remote [Main]: me@main.example:2222\n") != std::string::npos); // comments kept
    CHECK(text.find("[Box]\nhost = box.example\nport = 23\nuser = u1\nauth = password\nmax_depth = 3\n") != std::string::npos);
    // The dotted name: its parent's host and user, its own port; the parent's password isn't its.
    CHECK(text.find("[Box.eu]\nhost = box.example\nport = 24\nuser = u1\nauth = agent\n") != std::string::npos);
    CHECK(text.find("[Std]\nhost = std.example\nport = 22\nuser = s\nauth = agent\nengine = grab\n") != std::string::npos);
    CHECK(text.find("fake-box-password") == std::string::npos);
    CHECK(old.store() == passwords::Store{{"Box", {{"password", passwords::Secret{.binding = {.user = "u1", .host = "box.example", .port = 23},
                                                                                  .value = "fake-box-password"}}}}});
    CHECK(Old::pins() == "[main.example]:2222 ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIMAIN\n");
    const auto record = old.record();
    REQUIRE(record.has_value());
    CHECK(record.value_or(rclone_import::Record{}).imported.size() == 5);
    CHECK(record.value_or(rclone_import::Record{}).banner == names);
    old.untouched();
    // Every section now loads as a server.
    auto doc = ini::parse(text);
    REQUIRE(doc);
    for (const auto& s : doc->sections) {
        if (s.name.empty() || s.name == "grab") continue;
        CAPTURE(s.name);
        const auto c = server_conf::parse(s);
        REQUIRE_MESSAGE(c.has_value(), c.error_or(""));
        CHECK(c->has_value());
    }

    // A second run changes nothing, and doesn't even look further than grab.conf and the stamps.
    const auto before = done_of(old);
    const auto store_bytes = util::read_file(passwords::path_in(old.files.state_dir)).value_or("");
    const auto again = rclone_import::apply(old.files);
    REQUIRE(again.has_value());
    CHECK(again->imported.empty());
    CHECK(again->notes.empty());
    CHECK(done_of(old) == before);
    CHECK(util::read_file(passwords::path_in(old.files.state_dir)).value_or("") == store_bytes);
    old.untouched();
}

TEST_CASE("apply: a removed server doesn't come back; a failed one is tried again when its source changes") {
    Old old;
    REQUIRE(rclone_import::apply(old.files));
    // [Box] removed by hand, then a section of that name without details: never imported again.
    auto text = old.grab();
    text = servers::remove_section(text, "Box");
    text += "\n[Box]\n\n[Late]\n";
    Old::write(old.conf, text);
    auto applied = rclone_import::apply(old.files);
    REQUIRE(applied.has_value());
    CHECK(applied->imported.empty());
    CHECK(applied->failed == std::vector<std::string>{"Late"}); // no such remote yet
    const auto failed_once = old.grab();
    // Nothing changed: not tried again.
    applied = rclone_import::apply(old.files);
    REQUIRE(applied.has_value());
    CHECK(applied->failed.empty());
    // 0.19 adds [Late] to its rclone.conf: imported on the next run.
    Old::write(old.own, util::read_file(old.own).value_or("") + "\n[Late]\ntype = sftp\nhost = late.example\nuser = l\n"
                                                                 "key_use_agent = true\n");
    applied = rclone_import::apply(old.files);
    REQUIRE(applied.has_value());
    REQUIRE(applied->imported.size() == 1);
    CHECK(applied->imported[0].name == "Late");
    CHECK(old.grab().find("[Box]\n\n") != std::string::npos); // still without details
    CHECK(old.grab() != failed_once);
}

TEST_CASE("apply: a 'passwords' store that can't be loaded stops the import, nothing written") {
    Old old;
    fs::create_directories(old.files.state_dir);
    const auto store = passwords::path_in(old.files.state_dir);
    Old::write(store, "grab-passwords-sealed 1\nanother account's");
    const auto applied = rclone_import::apply(old.files);
    REQUIRE_FALSE(applied.has_value());
    CHECK(applied.error().find("saved passwords") != std::string::npos);
    CHECK(applied.error().find("can't be read") != std::string::npos);
    CHECK(old.grab() == old.grab_text);
    CHECK(util::read_file(store).value_or("") == "grab-passwords-sealed 1\nanother account's");
    CHECK_FALSE(fs::exists(grab_known_hosts_path()));
    CHECK_FALSE(old.record().has_value());
    old.untouched();
}

TEST_CASE("apply: no grab.conf, or nothing waiting, is nothing to do") {
    Old old;
    fs::remove(old.conf);
    const auto none = rclone_import::apply(old.files);
    REQUIRE(none.has_value());
    CHECK(none->imported.empty());
    CHECK_FALSE(fs::exists(old.conf));
    Old::write(old.conf, "[grab]\n\n[x]\nhost = x.example\nuser = x\nauth = agent\n");
    CHECK(rclone_import::apply(old.files).has_value());
    CHECK_FALSE(old.record().has_value()); // not even the record
}

#ifdef _WIN32
TEST_CASE("apply: each write failing in turn leaves a state the next run finishes") {
    // The reference: one run that goes through.
    Done reference;
    {
        Old old;
        REQUIRE(rclone_import::apply(old.files));
        reference = done_of(old);
    }
    const auto readonly = [](const fs::path& p, bool on) {
        SetFileAttributesW(p.c_str(), on ? FILE_ATTRIBUTE_READONLY : FILE_ATTRIBUTE_NORMAL);
    };
    for (const std::string_view which : {"store", "known_hosts", "grab.conf", "imported.json"}) {
        CAPTURE(which);
        Old old;
        fs::create_directories(old.files.state_dir);
        fs::path target;
        if (which == "store") {
            target = passwords::path_in(old.files.state_dir);
            REQUIRE(passwords::save(target, {}));
        } else if (which == "known_hosts") {
            target = grab_known_hosts_path();
            Old::write(target, "");
        } else if (which == "grab.conf") {
            target = old.conf;
        } else {
            target = rclone_import::record_path(old.files.state_dir);
            Old::write(target, rclone_import::to_json({}));
        }
        readonly(target, true);
        const auto failed = rclone_import::apply(old.files);
        readonly(target, false);
        REQUIRE_FALSE(failed.has_value());
        CHECK(failed.error().find("stopped") != std::string::npos);
        CHECK(failed.error().find("fake-box-password") == std::string::npos);
        old.untouched();
        // The next run finishes it.
        const auto next = rclone_import::apply(old.files);
        REQUIRE_MESSAGE(next.has_value(), next.error_or(""));
        const auto now = done_of(old);
        CHECK(now.grab == reference.grab);
        CHECK(now.store == reference.store);
        CHECK(now.pins == reference.pins);
        if (which == "imported.json") {
            // grab.conf was written: every server is done, and the next run has nothing left to
            // import; only the record of it (the window's banner) is missing.
            REQUIRE(now.record.has_value());
            CHECK(now.record.value_or(rclone_import::Record{}).imported.empty());
        } else {
            CHECK(now.record == reference.record);
        }
        old.untouched();
    }
}
#endif

TEST_CASE("the programs run the import: load_context and load_env fill a section in from rclone.conf") {
    Old old;
    // load_context imports, then loads the server from grab.conf.
    auto ctx = engine::load_context(old.conf, std::string("Std"));
    REQUIRE_MESSAGE(ctx.has_value(), ctx.error_or(""));
    CHECK(ctx->connection.host == "std.example");
    CHECK(ctx->remote.known_hosts_file == util::path_to_utf8(grab_known_hosts_path()));
    CHECK(ctx->imported.size() == 5);
    // The window's UI thread doesn't: a section still without details is said to be so.
    Old::write(old.conf, old.grab() + "\n[Late]\n");
    auto bare = engine::load_context(old.conf, std::string("Late"), false);
    REQUIRE_FALSE(bare.has_value());
    CHECK(bare.error().find("no connection details") != std::string::npos);
    // load_env does too, and says why it couldn't.
    auto env = server_ops::load_env(old.conf, old.dir);
    REQUIRE(env.has_value());
    CHECK(env->imported.empty());
    REQUIRE(env->import_notes.size() == 1);
    CHECK(env->import_notes[0].starts_with("[Late] not imported: there is no rclone remote [Late]"));
    old.untouched();
}
