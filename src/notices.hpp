#pragma once

// One-time notices about the settings grab's own SSH client reads differently from rclone, or not
// at all (RCLONE-REPLACEMENT.md, phase 8 step 5): common_flags' --bind, --contimeout and --timeout
// read as the new keys (N1), rclone's other connection flags (N2, N3), ssh_options (N4), connection
// flags in folder_flags and file_flags (N5), rclone settings that change what grab does (N6), keys
// grab doesn't know (N7), flags only --rclone copies use (N8), and what keeps grab's client off a
// server (guard.hpp). Only for what differs from the old defaults, flag by flag: grab.conf.example's
// lines and `--init`'s commented ones say nothing. No text quotes a value but a bind, a time, a
// size or a count: a proxy, an ssh command or a ProxyCommand can hold a user:password.
//
// Each shows once per configuration, in whichever program meets it first (the command prints
// "grab: note: ..." on stderr, the window shows it where the import's notice goes), and again when
// its value changes: notices.json in the configuration's state_dir keeps server, key, flag and a
// SHA-256 of the value, never the value; of the text instead for the guard's and the rclone
// settings' (N6), which can hold a secret. `grab server test` and -v repeat a server's every time.

#include "config.hpp"
#include "ini.hpp"

#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace grab::notices {

struct Notice {
    std::string server; // the section: "Main", "grab"
    std::string key;    // "common_flags", "ssh_options", "rclone.skip_links", an unknown key, "guard"
    std::string flag;   // "--bind", "-o ConnectTimeout", the guard's setting; "" for a whole key
    std::string digest; // SHA-256 of the value it is about
    std::string text;   // what is shown
    bool operator==(const Notice&) const = default;
};

// grab's store's rclone extras per server, those saved for its login: server -> (key without
// "rclone.", value).
using Extras = std::map<std::string, std::vector<std::pair<std::string, std::string>>, std::less<>>;
// An inherited environment variable, or nullopt.
using Environment = std::function<std::optional<std::string>(const std::string&)>;

// Every notice the configuration `doc` (parsed as `cfg`) has, server by server in file order:
// on engine = grab servers (effective_engine) N1-N6, on any N5 for file_flags, N7, N8 and the
// guard's reasons (with `extras` and `environment`; `store_unreadable`: grab's store exists but
// can't be read, so every server with connection details counts as routed).
[[nodiscard]] std::vector<Notice> settings_notices(const ini::Document& doc, const GrabConfig& cfg, const Extras& extras,
                                                   const Environment& environment, bool store_unreadable = false);

// The notices of the configuration at `grab_conf`, read now: grab.conf, grab's store (one
// decryption, for the extras) and this process's environment.
[[nodiscard]] std::expected<std::vector<Notice>, std::string> for_config(const std::filesystem::path& grab_conf);

// The file that keeps which were shown: "notices.json" in `state_dir`.
[[nodiscard]] std::filesystem::path path_in(const std::filesystem::path& state_dir);

// Those of `current` (a configuration's every notice) not shown yet. notices.json then holds
// `current`, so one that goes and comes back shows again; written under the settings lock in one
// step, only when it changes. One that can't be read counts as empty.
[[nodiscard]] std::expected<std::vector<Notice>, std::string> take_new(const std::filesystem::path& file,
                                                                      const std::vector<Notice>& current);

} // namespace grab::notices
