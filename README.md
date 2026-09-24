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

With a key loaded in ssh-agent the ssh step never prompts; otherwise it asks for the key
passphrase once per run. The rclone step never prompts, since grab's rclone.conf carries the
obscured password or passphrase.

## Requirements

- Windows 10/11 with the built-in OpenSSH client (`ssh.exe`), or any OS with `ssh` on PATH.
- Nothing else: [rclone](https://rclone.org) ships inside grab (see *Third-party software*),
  and servers are set up with `grab server add`. You never need to run `rclone config`.
- A private key used for ssh must have an ACL restricted to your user (OpenSSH refuses
  world-readable keys).

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

- `grab-x.y.z-windows-x64-setup.exe`: per-user installer, no admin rights.
  - Installs `grab.exe`, the `grab-gui.exe` window and the bundled `rclone.exe` in
    `%USERPROFILE%\programs\grab\bin`, and adds that folder to your user PATH.
  - Adds a **grab** Start-menu entry (and optionally a desktop shortcut) for the window.
  - Writes a starter config to `%APPDATA%\grab\grab.conf` if you don't have one.
  - Registers an uninstaller (Settings → Apps) that removes the files, shortcuts, PATH entry
    and the window's browser cache, but keeps your config and servers.
  - If the Microsoft Edge WebView2 Runtime is missing, which is rare, the last page says so
    and gives the download link. The `grab` command works without it.
- `grab-x.y.z-windows-x64.zip`: the same programs without an installer. Unpack it anywhere,
  keeping the three `.exe` files together. Then start `grab-gui.exe` and add a server in
  Settings, or run `grab server add`.

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
An open grab window is closed for the update and started again afterwards; downloads running in
it are cancelled. A portable or `cmake --install` copy has its programs (`grab.exe`,
`grab-gui.exe`, `rclone.exe`) swapped in place. The replaced files are left as `*.exe.old` and
removed the next time grab runs. It uses the `curl.exe` and `tar.exe` that ship with Windows 10
and later. Versions before 0.2.0 have no `update` command; install 0.2.0 once from the Releases
page.

Portable copies of 0.2.0: that version's updater replaces only `grab.exe`. Afterwards grab
notices that `grab-gui.exe` and `rclone.exe` are missing and says so. Running `grab update` once
more adds them, even when the version is already current.

### From source

```powershell
cmake --install build\clang-cl-release
```

This puts `grab.exe`, `grab-gui.exe` and `rclone.exe` in `%USERPROFILE%\programs\grab\bin` (no admin rights needed) and
adds that directory to your user PATH, so `grab` works from any terminal opened afterwards.
The PATH step is idempotent and keeps existing `%VAR%` entries intact; skip it with
`-DGRAB_INSTALL_ADD_TO_PATH=OFF` at configure time, or install elsewhere with
`cmake --install build\clang-cl-release --prefix D:\tools\grab`. In VS Code, the task
"Install grab (Release)" (Terminal → Run Task) builds Release and installs it in one go,
whatever preset is active; "CMake: Install" from the Command Palette installs the active
preset's build instead. The Windows presets link the C runtime statically, so the
installed exe has no VC redistributable dependency.

## GUI

`grab-gui.exe` (the **grab** Start-menu entry) is a window over the same engine: pick a server, search by words, select
results (click, Shift/Ctrl-click, arrows, Ctrl+A), choose where to save, and follow the
download queue with live progress. It reads the same grab.conf and remembers the last server,
mode and destination per server in `%APPDATA%\grab\gui.json`.

- **Downloads** run several at a time: "At once" in the Downloads bar, 1–8, default 4. Each
  one can be paused, resumed or cancelled, or all together with Pause all / Resume all and
  Cancel all.
- **Resumable file downloads:** grab fetches a file in byte ranges, several `rclone cat`
  streams at once, into `<name>.grabpart`. It records each range's progress in
  `<name>.grabpart.json` and renames the file when it is complete.
  - Pausing keeps both; resuming continues from the last saved bytes.
  - This survives closing grab: unfinished downloads come back paused (the list is kept in
    `%APPDATA%\grab\queue.json`).
  - If the file changed on the server in the meantime, it starts over.
  - When a stream runs out of work, it splits the busiest remaining range, so the end of a
    file doesn't trickle in over one connection.
  - Folders are still copied by `rclone copy`: pausing stops it, and resuming skips the files
    already finished.
- **Connections per server:** every download from a server shares that server's connection
  budget.
  - The budget is 8 for Hetzner Storage Boxes and 12 elsewhere. Set it per server in
    Settings, or as `max_connections` in grab.conf.
  - A lone download uses the whole budget; several split it fairly. A small file that can
    only use one connection lends the rest to the others.
  - Connections open slightly staggered. If a server refuses one, grab lowers that server's
    budget and remembers it.
  - Measured on a Storage Box, where each connection is capped at about 2.5 MB/s: 1
    connection gives 2.5 MB/s, 8 give about 14 MB/s, and 14 about 19 MB/s. `rclone copyto`
    reached about 5 MB/s.

- **Settings** (the gear, or Ctrl+,) manages servers like `grab server` does: add, edit,
  test, trust, make default and remove. Each server's search folders and depth are edited
  there too. Adding a server, or changing its address, shows the host key fingerprints
  and writes nothing until you trust them. "Open grab.conf" covers the rarer settings
  (rclone flags, ssh options, find method).
- **Tray:** closing the window while downloads run keeps them going in the notification
  area, and grab exits by itself when they finish. When the window is idle, closing it
  quits. A notification reports each finished or failed download while the window isn't in
  front. Clicking it, or the tray icon, brings the window back.
- It needs the Microsoft Edge WebView2 Runtime, which ships with Windows 11 and current
  Windows 10. Without it, grab-gui offers the download link.
- It never prompts for an ssh passphrase, since it has no console. For key-based servers,
  load the key into ssh-agent (`ssh-add <key file>`). A search then fails fast with that
  hint instead of hanging.
- Start-up is about half a second. Memory use is ~20 MB for grab-gui plus the WebView2
  browser processes, typically 150–300 MB.
- UI development: set `GRAB_UI_DIR` to `src\gui\ui` and grab-gui serves the page from there,
  so edits need only a reload (Debug builds have DevTools and F5).

The GUI is built with the rest (`GRAB_BUILD_GUI=ON` by default); in a source build run
`build\<preset>\grab-gui.exe`, or `cmake --install` it with grab.

## Configure

### Servers

```powershell
grab server add              # step by step: host, port, user, login, folders to search
grab server list             # what is configured, and whether each host key is pinned
grab server trust NAME       # pin a server's host key (for servers imported without one)
grab server remove NAME
```

`grab server add` asks for:
- the server's host, port and user
- how to log in: a password, a key file (with an optional passphrase), or ssh-agent
- which folders to search

Then it:
- **pins the host key:** shows the server's fingerprints (ED25519, ECDSA and RSA, whichever
  it has) and saves them to `%APPDATA%\grab\known_hosts` once you confirm. From then on ssh
  and rclone refuse a server that presents a different key.
- **writes both config files:** the connection goes into grab's own
  `%APPDATA%\grab\rclone.conf`, the search settings into grab.conf.
- **tests the connection:** one check through rclone and, for key or agent logins, one
  through ssh.

Passwords and passphrases are never shown and never put on a command line. They are passed
to `rclone obscure` over stdin and stored obscured, as rclone does.

The commands also take answers from piped input, one per line, so they can be scripted. When
the input runs out, the command stops without changing anything.

Password servers are searched with rclone. Key and ssh-agent servers are searched with ssh +
find, which is faster; with a passphrase-protected key that needs the key in ssh-agent
(`ssh-add <key file>`).

**Coming from an earlier grab:** grab.conf used to point at remotes in the standard
rclone.conf. The first run of this version copies the remotes grab.conf uses into grab's own
file, once, and never modifies the original. `grab server list` then shows which host keys
still need `grab server trust`.

### grab.conf

```powershell
grab --init          # writes %APPDATA%\grab\grab.conf, if absent
grab config          # opens it in your editor, creating it first if needed
```

`--init` writes the `[grab]` block. If you already have sftp remotes in the standard
rclone.conf (`$RCLONE_CONFIG`, else `%APPDATA%\rclone\rclone.conf`), it imports them into grab's
own rclone.conf, one grab.conf section each:
- `search_roots` is blank, meaning the login home.
- `default_remote` is the first one.
- The rclone flag keys are written commented out, so later changes to the built-in defaults
  still reach you. Uncomment one to override it.

Run `--init` again later: it never rewrites an existing file, but prints ready-to-paste
sections for sftp remotes added to the standard rclone.conf since.

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

`grab server add` writes sections like this for you.

All keys and their defaults are documented in [grab.conf.example](grab.conf.example).
The config path can be overridden with `--config PATH` or the `GRAB_CONFIG` env var.
Add one `[section]` per server and choose with `-r NAME`.

## Usage

```
grab [-s|--file | -f|--folder] TARGET [DEST] [options] [-- extra rclone args]
grab config [-c PATH]

  -r, --remote NAME    grab.conf section to use
  -c, --config PATH    grab.conf path
      --depth N        override max_depth for this run
      --first          take the best match instead of asking
      --all            take every match instead of asking
      --exact          match TARGET as a whole name or glob, case-sensitive
  -n, --dry-run        resolve the target and print the rclone command; transfers nothing and
                       creates no folders
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
- File matches show their size in the list. The ssh lookup gets sizes from GNU find's `-printf`,
  which any mainstream Linux server has; a server without it finds no files over ssh, so use
  `find = rclone` for it.
- With several matches you get a numbered list, best first. Exact names rank first, then
  names starting with your first word, then the shallowest, then alphabetical, so a
  season lists in episode order. Answer `3`, `1-5,8`, `a` for all, Enter for `[1]`, or `q`.
  Each pick is downloaded in turn and a summary follows. `--first` and `--all` answer
  without asking, for scripts.
- A `TARGET` containing `/` is a path, absolute or relative to the login home, and is
  checked as-is instead of searched for.
- Without `-s` or `-f`, grab looks for files: `grab lioness` is `grab -s lioness`.
- `DEST` is the local parent directory and is created if missing. Both modes write
  `DEST\<name>`: folder mode recreates the folder there and copies its contents into it,
  file mode writes the single file. `grab -f releases E:\Backup` yields `E:\Backup\releases\...`.
- `DEST` is optional. Without it, grab asks where to save after you have picked. Enter
  means the current directory, and a pasted path in quotes is fine. With no terminal to
  ask on (piped or redirected input, scheduled tasks), a missing `DEST` is an error before
  any search runs.
- `grab config` and `grab update` are commands. To search for a file literally named
  `config` or `update`, add `-s`.
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

These flags drive the `grab` command. grab-gui fetches single files with its own ranged
streams (see [GUI](#gui)) and uses `common_flags` for them; its connection count per server is
`max_connections` (blank = automatic: 8 on Storage Boxes, 12 elsewhere, lowered when a
server refuses connections).

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
src/engine.*    search + download for both  src/servers.*  server argv, host keys, conf edits
src/server_ops.* add/edit/trust/remove/test src/update.*   `grab update`
src/fetch.*     resumable ranged downloads  src/conn_budget.* per-server connection sharing
src/gui/        grab-gui: WebView2 host, tray, settings backend, ui/ (HTML, CSS, JS)
installer/      Inno Setup script           tools/         make_icon.py (src/gui/grab.ico)
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

## Third-party software

- **[rclone](https://rclone.org)** v1.75.1, © Nick Craig-Wood, MIT license
  ([licenses/rclone.txt](licenses/rclone.txt)). `rclone.exe` ships next to `grab.exe`
  (81 MB, which is why the installer is about 22 MB), and grab uses it rather than any rclone
  on PATH. Set `rclone = <path>` in `[grab]` to use another copy.
  - **Pin:** the build downloads the official Windows zip and checks it against the pinned
    SHA-256, which is rclone's own published `SHA256SUMS` value.
  - **To bump:** change `GRAB_RCLONE_VERSION` and the hash in `CMakeLists.txt`, then rebuild.
    `grab update` delivers the new rclone together with grab.

## License

MIT, see [LICENSE](LICENSE).
