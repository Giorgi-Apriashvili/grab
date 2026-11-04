#pragma once

// One-time notices about the settings of grab 0.20 and before that this grab doesn't use any more
// (RCLONE-REPLACEMENT.md, phase 9 step 7), now that it has no rclone and runs no ssh.exe: the switch
// of imported servers to grab's own SSH client (N0), `engine = rclone` (N9), GRAB_ENGINE=rclone
// (N10), common_flags with what to write instead (N11), folder_flags and file_flags (N12),
// ssh_options, with a jump route it held (N13, the user's decision 4), [grab] rclone and ssh (N14),
// the rclone settings that changed what rclone did (N15), keys grab doesn't know (N7), what keeps
// grab off a server (guard.hpp), and grab's own old rclone.conf and 'secrets' while they stay
// (rclone_import::old_files_kept) and once deleted (rclone_import::Record::old_files). Only for
// what differs from the old defaults, flag by flag: grab.conf.example's lines and `--init`'s
// commented ones say nothing. No text quotes a value but a bind, a time or a count: a proxy, an ssh
// command or a ProxyCommand can hold a user:password. A bind left in an old key with no `bind` key
// isn't a notice: every connection refuses that server and says why (engine::connect_settings).
//
// Each shows once per configuration, in whichever program meets it first (the command prints
// "grab: note: ..." on stderr, the window shows it where the import's notice goes), and again when
// its text changes: notices.json in the configuration's state_dir keeps server, key, flag and a
// SHA-256 of the text (of the value for N7), never a value. Their keys are names 0.20 never used
// ("ignored:common_flags"), so what 0.20's notices.json holds hides none of them; N0 keeps 0.20's
// id, so whoever saw it there doesn't again. `grab server test` and -v repeat a server's every time.

#include "config.hpp"
#include "ini.hpp"

#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace grab::notices {

struct Notice {
    std::string server; // the section: "Main", "grab"
    std::string key;    // "ignored:common_flags", "ignored:engine", an unknown key, "guard", "old files"
    std::string flag;   // the guard's setting; "" for the rest
    std::string digest; // SHA-256 of the text (of the value for an unknown key)
    std::string text;   // what is shown
    bool operator==(const Notice&) const = default;
};

// grab's store's rclone extras per server, those saved for its login: server -> (key without
// "rclone.", value).
using Extras = std::map<std::string, std::vector<std::pair<std::string, std::string>>, std::less<>>;
// An inherited environment variable, or nullopt.
using Environment = std::function<std::optional<std::string>(const std::string&)>;
// The servers the import from rclone.conf moved into grab.conf (imported.json's names): every
// server from before 0.20 reaches grab.conf that way, so these are the ones that used rclone.
using Moved = std::set<std::string, std::less<>>;

// Every notice the configuration `doc` (parsed as `cfg`) has: first N0 (server "grab", key
// "engine", flag "default", keyed on the default itself so it shows once whatever it names) when
// a server of `moved` with connection details and no `engine` line is one grab connects to; then
// N10 (`environment`'s GRAB_ENGINE) and N14; then server by server in file order N9, N11, N12, N13
// and, for one with connection details, N15 and the guard's reasons (with `extras` and
// `environment`; `store_unreadable`: grab's store exists but can't be read, so every server with
// connection details counts as routed); then N7.
[[nodiscard]] std::vector<Notice> settings_notices(const ini::Document& doc, const GrabConfig& cfg, const Extras& extras,
                                                   const Environment& environment, const Moved& moved,
                                                   bool store_unreadable = false);

// The notices of the configuration at `grab_conf`, read now: grab.conf, grab's store (one
// decryption, for the extras), imported.json (no N0 when it can't be read), this process's
// environment, and last the note on grab's own old files (key "old files", keyed on its text): why
// they stay, when the configuration is the default one and they do, else the note imported.json
// keeps from the run that deleted them, or couldn't (rclone_import::Record::old_files).
[[nodiscard]] std::expected<std::vector<Notice>, std::string> for_config(const std::filesystem::path& grab_conf);

// The file that keeps which were shown: "notices.json" in `state_dir`.
[[nodiscard]] std::filesystem::path path_in(const std::filesystem::path& state_dir);

// Those of `current` (a configuration's every notice) not shown yet. notices.json then holds
// `current`, so one that goes and comes back shows again, except N0, which stays once shown;
// written under the settings lock in one step, only when it changes. One that can't be read
// counts as empty.
[[nodiscard]] std::expected<std::vector<Notice>, std::string> take_new(const std::filesystem::path& file,
                                                                      const std::vector<Notice>& current);

// for_config, then take_new in the configuration's notices.json: those not shown yet (every one
// when that can't be saved: said every time rather than never), none when they can't be read.
[[nodiscard]] std::vector<Notice> take_fresh(const std::filesystem::path& grab_conf);

} // namespace grab::notices
