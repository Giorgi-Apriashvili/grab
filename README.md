# grab

[![build](https://github.com/Giorgi-Apriashvili/grab/actions/workflows/build.yml/badge.svg)](https://github.com/Giorgi-Apriashvili/grab/actions/workflows/build.yml)

Fetch a file or folder from a Linux server **by name**. `grab` finds it over ssh, then
downloads it with rclone using flags tuned for either one big file or a tree of files,
with rclone's live progress in your terminal.

```
grab -f releases  E:\Backup     # folder: E:\Backup\releases\...
grab -s movie.mkv E:\Backup     # file:   E:\Backup\movie.mkv
```

`DEST` is the parent directory in both modes; the item is recreated inside it under its
remote name.

## How it works

```
grab -f releases E:\Backup
  ├─ 1. parse args      mode=folder, target="releases", dest="E:\Backup"
  ├─ 2. load config     grab.conf  ->  which rclone remote, where to search, which flags
  │                     rclone.conf -> host / user / port / key for the ssh step
  ├─ 3. resolve remote  ssh user@host "find ROOTS -maxdepth N -name 'releases' -type d -print0"
  │                     (or `rclone lsf -R` for remotes without a key / shell, see Lookup methods)
  │                     0 hits -> exit 3   1 hit -> go   N hits -> numbered pick, several allowed
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

## Install

### From a release (Windows)

Download from the [Releases page](https://github.com/Giorgi-Apriashvili/grab/releases):

- `grab-x.y.z-windows-x64-setup.exe`: per-user installer, no admin rights. It puts `grab.exe`
  in `%USERPROFILE%\programs\grab\bin`, adds that folder to your user PATH, writes a commented
  default config to `%APPDATA%\grab\grab.conf` if you don't have one, and registers an
  uninstaller (Settings → Apps) that removes the files and the PATH entry but keeps your config.
- `grab-x.y.z-windows-x64.zip`: the same files without an installer; put `grab.exe` wherever
  you like and run `grab --init` once.

The installer is not code-signed, so SmartScreen shows "Windows protected your PC" the first
time; choose "More info → Run anyway". The exe links the C runtime statically and has no other
dependencies.

### Updating

```powershell
grab update --check   # is a newer release out?
grab update           # download, verify and install it over this copy
```

`grab update` reads the latest GitHub release and, when it is newer, downloads the matching
asset and checks it against the release's `SHA256SUMS.txt`. It refuses anything that doesn't
match. Nothing is changed before that check passes. A copy installed with the setup exe is
upgraded by running the new installer silently, so Add/Remove Programs shows the new version.
A portable or `cmake --install` copy has its `grab.exe` swapped in place. The replaced binary is
left as `grab.exe.old` and removed the next time grab runs. It uses the `curl.exe` and `tar.exe`
that ship with Windows 10 and later. Versions before 0.2.0 have no `update` command; install
0.2.0 once from the Releases page.

### From source

```powershell
cmake --install build\clang-cl-release
```

This puts `grab.exe` in `%USERPROFILE%\programs\grab\bin` (no admin rights needed) and
adds that directory to your user PATH, so `grab` works from any terminal opened afterwards.
The PATH step is idempotent and keeps existing `%VAR%` entries intact; skip it with
`-DGRAB_INSTALL_ADD_TO_PATH=OFF` at configure time, or install elsewhere with
`cmake --install build\clang-cl-release --prefix D:\tools\grab`. In VS Code, the task
"Install grab (Release)" (Terminal → Run Task) builds Release and installs it in one go,
whatever preset is active; "CMake: Install" from the Command Palette installs the active
preset's build instead. The Windows presets link the C runtime statically, so the
installed exe has no VC redistributable dependency.

## Configure

```powershell
grab --init          # writes %APPDATA%\grab\grab.conf from your rclone.conf, if absent
grab config          # opens it in your editor, creating it first if needed
```

`--init` reads rclone.conf (`$RCLONE_CONFIG`, else `%APPDATA%\rclone\rclone.conf`) and writes
one section per sftp remote. `search_roots` is left blank, meaning the login home, and
`default_remote` is the first remote, so grab works right away. The rclone flag keys are
written commented out, so later improvements to the built-in defaults still reach you;
uncomment one to override it. Non-sftp remotes are listed in a comment. With no usable
rclone.conf (missing, encrypted, or no sftp remote) you get the generic example to edit
instead. Run `--init` again later: it never rewrites an existing file, but prints ready-to-paste
sections for sftp remotes you have added to rclone.conf since.

`grab config` uses the `editor` key in `[grab]` (for example `editor = code --wait`),
otherwise `$VISUAL`, then `$EDITOR`, and finally Notepad. It reads only that key, so a
config with a syntax error can still be opened and fixed.

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
grab config [-c PATH]

  -r, --remote NAME    grab.conf section to use
  -c, --config PATH    grab.conf path
      --depth N        override max_depth for this run
      --first          take the best match instead of asking
      --all            take every match instead of asking
      --exact          match TARGET as a whole name or glob, case-sensitive
  -n, --dry-run        resolve the target and print the rclone command, transfer nothing
  -v, --verbose        echo the ssh and rclone command lines
      --init           write an example grab.conf
```

- `TARGET` is a search: every word must appear in the name, ignoring case, in any order.
  `grab -s lioness E:\TV` lists every file with "lioness" in its name.
  `grab -s "lioness s03e08" E:\TV` narrows that to one episode, and so does
  `grab -s "the unravelling" E:\TV`. With the ssh lookup the filtering happens on the server
  (`find -iname`), so only hits travel back.
- A `TARGET` containing `*`, `?` or `[` is a glob instead, also case-insensitive:
  `grab -f 'Season.0[1-3]*' E:\Backup`. `--exact` matches the whole name, case-sensitive.
- With several matches you get a numbered list, best first. Exact names rank first, then
  names starting with your first word, then the shallowest, then alphabetical, so a
  season lists in episode order. Answer `3`, `1-5,8`, `a` for all, Enter for `[1]`, or `q`.
  Each pick is downloaded in turn and a summary follows. `--first` and `--all` answer
  without asking, for scripts.
- A `TARGET` containing `/` is a path, absolute or relative to the login home, and is
  checked as-is instead of searched for.
- `DEST` is the local parent directory and is created if missing. Both modes write
  `DEST\<name>`: folder mode recreates the folder there and copies its contents into it,
  file mode writes the single file. `grab -f releases E:\Backup` yields `E:\Backup\releases\...`.
- Dot-directories are skipped during the search unless `skip_hidden = false` or the
  target itself starts with a dot.
- Anything after `--` is appended to the rclone command, e.g. `-- --bwlimit 10M`.

Exit codes: `0` ok, `1` usage, `2` config, `3` target not found or pick aborted,
`4` remote lookup failed (ssh or rclone listing), `5` `grab update` failed, otherwise
rclone's own code.

## Lookup methods

Per remote, `find =` in grab.conf picks how `TARGET` is located:

| method   | how                                                          | auto picks it when |
|----------|--------------------------------------------------------------|--------------------|
| `ssh`    | one `ssh user@host "find ROOTS -maxdepth N -iname '*WORD*' ..."` | the rclone remote has a `key_file` |
| `rclone` | `rclone lsf REMOTE:ROOT -R --max-depth N`, filtered locally   | otherwise (password remotes, storage boxes) |

`ssh` is a single round trip but needs a shell on the server and prompts for the key
passphrase. `rclone` walks the tree over SFTP, a few seconds for a few hundred directories,
needs no shell and no prompt, and is the right choice for a Hetzner Storage Box, which
does not run arbitrary commands. On a Storage Box use `search_roots = /home` or leave it
blank for the home directory; `/` itself is not listable there.

`search_roots` entries may be absolute (`/srv`) or relative to the login home
(`learning`); blank or `.` means the home directory. `known_hosts_file = none` in
rclone.conf is honoured as "no known_hosts file" and never passed to ssh.

## Tuning

Defaults (override per remote in grab.conf):

| key            | default                                                                     | why |
|----------------|-----------------------------------------------------------------------------|-----|
| `common_flags` | `-P --sftp-chunk-size 255Ki --sftp-disable-hashcheck`                       | live progress; 255 KiB is the largest SFTP packet OpenSSH accepts, ~8x fewer round trips than the 32 KiB default; skipping the post-transfer server-side `md5sum` measured 10–30% faster |
| `folder_flags` | `--transfers 8 --checkers 8`                                                | more transfers hide per-file round trips on trees of small files; with rclone's default 4 streams that is ≤32 connections, which both test servers accepted without errors |
| `file_flags`   | `--multi-thread-streams 8 --multi-thread-cutoff 64Mi --multi-thread-chunk-size 64Mi` | one big object: parallel range reads over 8 connections. Keep chunks large: 8Mi measured ~2x slower than 64Mi because every chunk re-opens the file and re-ramps the SFTP pipeline |

Concurrent SFTP connections ≈ `transfers × multi-thread-streams` (+ checkers). If the
server logs `MaxStartups` drops, lower one of them. `--bwlimit` can be passed after `--`.
`--sftp-disable-hashcheck` trades end-to-end checksum verification for speed; SSH already
protects the bytes in flight, so what remains undetected is disk-level corruption. Remove it
from `common_flags` if that matters more than minutes saved on large files.

## Layout

```
src/cli.*       argument parsing            src/remote.*   find command, ssh argv, match picking
src/ini.*       INI reader (grab + rclone)  src/listing.*  rclone lsf lookup, glob matching
src/config.*    grab.conf / rclone.conf     src/rclone.*   rclone argv for both modes
src/quote.*     Windows + sh quoting        src/process.*  CreateProcess / posix_spawn wrappers
src/util.*      strings, paths, environment src/main.cpp   the five steps
tests/          doctest unit tests (fetched by CMake)
```

## Releasing

GitHub Actions ([build.yml](.github/workflows/build.yml)) builds and tests every push and pull
request with MSVC and clang-cl. Pushing a version tag additionally runs the `dist` target, which
packs the portable zip and compiles the Inno Setup installer from
[installer/grab.iss.in](installer/grab.iss.in), writes `SHA256SUMS.txt` for both, and attaches
all three to a GitHub Release. `grab update` depends on that checksum file.

1. Bump `project(grab VERSION x.y.z)` in `CMakeLists.txt` and commit.
2. `git tag vx.y.z` and `git push origin master --tags`.

The workflow refuses a tag that doesn't match the project version. Locally,
`cmake --build --preset clang-cl-release --target dist` always produces the zip and also the
installer when Inno Setup 6 is installed (`winget install JRSoftware.InnoSetup`).

## License

MIT, see [LICENSE](LICENSE).
