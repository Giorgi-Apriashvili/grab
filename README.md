# grab

Fetch a file or folder from a Linux server **by name**. `grab` finds it over ssh, then
downloads it with rclone using flags tuned for either one big file or a tree of files,
with rclone's live progress in your terminal.

```
grab -f releases  E:\Backup\releases     # folder: contents of the remote dir land in E:\Backup\releases
grab -s movie.mkv E:\Backup              # file:   E:\Backup\movie.mkv
```

## How it works

```
grab -f releases E:\Backup\releases
  ├─ 1. parse args      mode=folder, target="releases", dest="E:\Backup\releases"
  ├─ 2. load config     grab.conf  ->  which rclone remote, where to search, which flags
  │                     rclone.conf -> host / user / port / key for the ssh step
  ├─ 3. resolve remote  ssh user@host "find ROOTS -maxdepth N -name 'releases' -type d -print0"
  │                     0 hits -> exit 3   1 hit -> go   N hits -> numbered pick (or --first)
  ├─ 4. build rclone    rclone copy   hetzner:/home/alice/releases E:\Backup\releases  + flags
  │                     rclone copyto hetzner:/path/movie.mkv      E:\Backup\movie.mkv  + flags
  └─ 5. run rclone      console inherited (live -P progress), Ctrl+C goes to rclone
```

The ssh step prompts for your key passphrase once per run (Windows OpenSSH, no agent
needed). The rclone step never prompts because rclone.conf already carries the obscured
passphrase (`key_file_pass`).

## Requirements

- Windows 10/11 with the built-in OpenSSH client (`ssh.exe`), or any OS with `ssh` on PATH.
- [rclone](https://rclone.org) on PATH with an **sftp** remote already configured
  (`rclone config`). grab reads `host`, `user`, `port`, `key_file` and `known_hosts_file`
  from that remote for the ssh call.
- The private key's ACL must be restricted to your user (OpenSSH refuses world-readable keys).

## Build

Toolchain: CMake ≥ 3.28, Ninja, and either LLVM clang-cl (default presets) or MSVC.

```powershell
cmake --preset clang-cl-debug
cmake --build --preset clang-cl-debug
ctest --preset clang-cl-debug --output-on-failure
# binary: build\clang-cl-debug\grab.exe   (release: clang-cl-release)
```

In VS Code the CMake Tools extension picks up `CMakePresets.json`; clangd reads
`build/clang-cl-debug/compile_commands.json`; `launch.json` has a CodeLLDB dry-run config.

## Configure

```powershell
grab --init          # writes %APPDATA%\grab\grab.conf with comments, if absent
```

Minimal `grab.conf`:

```ini
[grab]
default_remote = hetzner

[hetzner]
rclone_remote = hetzner
search_roots  = /home/alice
max_depth     = 4
```

All keys and their defaults are documented in [grab.conf.example](grab.conf.example).
The config path can be overridden with `--config PATH` or the `GRAB_CONFIG` env var.
Add one `[section]` per server and choose with `-r NAME`.

## Usage

```
grab (-s|--file | -f|--folder) TARGET DEST [options] [-- extra rclone args]

  -r, --remote NAME    grab.conf section to use
  -c, --config PATH    grab.conf path
      --depth N        override max_depth for this run
      --first          take the shallowest match instead of asking
  -n, --dry-run        resolve the target and print the rclone command, transfer nothing
  -v, --verbose        echo the ssh and rclone command lines
      --init           write an example grab.conf
```

- `TARGET` is matched with `find -name`, so shell globs work: `grab -f 'Some.Movie*' E:\Backup\tr`.
- A `TARGET` starting with `/` is used as-is (existence and type are still checked).
- `DEST` is always a directory and is created if missing. Folder mode copies the *contents*
  of the remote folder into it. File mode writes `DEST\<name>`.
- Dot-directories are skipped during the search unless `skip_hidden = false` or the
  target itself starts with a dot.
- Anything after `--` is appended to the rclone command, e.g. `-- --bwlimit 10M`.

Exit codes: `0` ok, `1` usage, `2` config, `3` target not found or pick aborted,
`4` ssh failed, otherwise rclone's own code.

## Tuning

Defaults (override per remote in grab.conf):

| key            | default                                                                     | why |
|----------------|-----------------------------------------------------------------------------|-----|
| `common_flags` | `-P --sftp-chunk-size 255Ki`                                                | live progress; 255 KiB is the largest SFTP packet OpenSSH accepts, ~8x fewer round trips than the 32 KiB default |
| `folder_flags` | `--transfers 4 --checkers 8 --multi-thread-streams 4`                       | ≤16 concurrent connections, safely under sshd's default `MaxStartups`/`MaxSessions` |
| `file_flags`   | `--multi-thread-streams 8 --multi-thread-cutoff 64Mi --multi-thread-chunk-size 64Mi` | one big object: parallel range reads over 8 connections |

Concurrent SFTP connections ≈ `transfers × multi-thread-streams` (+ checkers). If the
server logs `MaxStartups` drops, lower one of them. `--bwlimit` can be passed after `--`.

## Layout

```
src/cli.*       argument parsing            src/remote.*   find command, ssh argv, match picking
src/ini.*       INI reader (grab + rclone)  src/rclone.*   rclone argv for both modes
src/config.*    grab.conf / rclone.conf     src/process.*  CreateProcess / posix_spawn wrappers
src/quote.*     Windows + sh quoting        src/main.cpp   the five steps
tests/          doctest unit tests (fetched by CMake)
```
