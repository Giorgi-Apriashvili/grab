# grab

[![build](https://github.com/Giorgi-Apriashvili/grab/actions/workflows/build.yml/badge.svg)](https://github.com/Giorgi-Apriashvili/grab/actions/workflows/build.yml)

Fetch a file or folder from a Linux server **by name**. `grab` finds it over ssh, then
downloads it over several connections at once. Downloads are resumable: Ctrl+C keeps what is
on disk, and running the same command again continues from there.

```
grab -f releases  E:\Backup     # folder: E:\Backup\releases\...
grab -s movie.mkv E:\Backup     # file:   E:\Backup\movie.mkv
```

`DEST` is the parent directory in both modes; the item is recreated inside it under its
remote name.

This page is the short introduction. Everything else (every option, the window, grab.conf,
connections, building and releasing) is in the full documentation,
[docs/doc.md](docs/doc.md).

## What it does

- **Finds by name.** Every word of `TARGET` must appear in the name, ignoring case, in any
  order; several matches give a numbered list, best first, to pick from. Searches use the
  server's saved file list when it is under 15 minutes old, which makes them instant; otherwise
  grab fetches a new list first. The server is searched with GNU find where it has it, else by
  listing its folders over SFTP, the way for a Hetzner Storage Box, which doesn't run arbitrary
  commands.
- **grab's own SSH client** (libssh2 and OpenSSL, built in) does all of it. It logs in once per
  connection and keeps the connections open between files, searches and downloads. Logging in
  never prompts: grab uses the keys in ssh-agent, or the password or key passphrase from its
  encrypted store. A server's host key is pinned when you add it. It connects to the server
  directly: SOCKS and HTTP proxies and jump hosts aren't supported yet.
- **Several connections, resumable.** A file comes in byte ranges over the server's connection
  budget (14 on Hetzner Storage Boxes, 12 elsewhere) into `<name>.grabpart`, renamed when
  complete. A folder is listed once; its files of 2 MiB and more come the same way, smaller ones
  beside them on every free connection. A file already there, with the same size and not older
  than the server's copy, is not fetched again.
- **The window.** `grab-gui.exe` (the **grab** Start-menu entry) is a window over the same
  engine, laid out like Explorer: browse and search a server's folders, preview images, text
  files and PDFs, play videos and audio in your own player streamed from the server, and follow
  a download queue with pause and resume, priorities, a speed limit and a speed schedule.

## Requirements

- Windows 10/11. Nothing else to install: grab's SSH client is built in, and servers are set up
  with `grab server add` (or Settings in the window). Neither rclone nor ssh.exe is needed.
- The window needs the Microsoft Edge WebView2 Runtime, which ships with Windows 11 and current
  Windows 10. The `grab` command works without it.

## Install

Download from the [Releases page](https://github.com/Giorgi-Apriashvili/grab/releases):

- `grab-x.y.z-windows-x64-setup.exe`: per-user installer, no admin rights. It installs
  `grab.exe` and the `grab-gui.exe` window in `%USERPROFILE%\programs\grab\bin` and adds that
  folder to your user PATH, adds a **grab** Start-menu entry, and writes a starter config to
  `%APPDATA%\grab\grab.conf` if you don't have one. Its uninstaller keeps your config and
  servers.
- `grab-x.y.z-windows-x64.zip`: the same programs without an installer. Unpack it anywhere,
  keeping the two `.exe` files together.

The installer is not code-signed, so SmartScreen shows "Windows protected your PC" the first
time; choose "More info → Run anyway".

```powershell
grab update --check   # is a newer release out?
grab update           # download, verify and install it over this copy
```

`grab update` checks the download against the release's `SHA256SUMS.txt` and changes nothing
before that check passes. The window offers the same update. More in
[Install](docs/doc.md#install).

## Quick start

1. **Add a server** (or use Settings, the gear, in the window):

   ```powershell
   grab server add
   ```

   It asks for the server's host, port and user, how to log in (a password, a key file with an
   optional passphrase, or ssh-agent) and which folders to search. It shows the server's host
   key fingerprints and pins them once you confirm, saves the server as a section of grab.conf
   (its password or passphrase goes to grab's encrypted store, never into grab.conf), and tests
   the connection. `grab server list` shows your servers; `grab server test NAME` tests one
   again.

2. **Fetch a file, then a folder:**

   ```powershell
   grab lioness E:\TV               # every file with "lioness" in its name: pick from the list
   grab -s "lioness s03e08" E:\TV   # narrowed to one episode
   grab -f releases E:\Backup       # the folder, as E:\Backup\releases\...
   ```

   With several matches, answer `3`, `1-5,8`, `a` for all, Enter for `[1]`, or `q`. One live
   line shows the progress; Ctrl+C stops and keeps the partial files, and the same command again
   continues. Without `DEST`, grab asks where to save after you have picked.

3. **Open the window:** the **grab** Start-menu entry (`grab-gui.exe`). Pick a server, click
   through its folders or search them, select rows, choose where to save, and Download.
   Double-click or Space opens a file: videos and audio play in the program Windows uses for
   them, streamed from the server; images, text files and PDFs open in a viewer.

## Basic usage

```
grab [-s|--file | -f|--folder] TARGET [DEST] [options]

  -r, --remote NAME    grab.conf section to use (default: [grab] default_remote)
  -n, --dry-run        resolve the target and show what would be fetched; transfers nothing
      --first          take the best match instead of asking
      --all            take every match instead of asking
      --exact          match TARGET as a whole name or glob, case-sensitive
      --type KINDS     only files of these kinds, comma-separated: video, audio, image, text,
                       document, archive, program, disc, other
      --min-size SIZE  only files at least this big: 700M, 1.5G, 20GiB (a bare number is MiB)
      --max-size SIZE  only files at most this big
      --limit RATE     cap the download speed: 5M, 800K, 2.5 (a bare number is MiB/s), or off
      --verify         check each download against the server's checksum (MD5) afterwards
      --existing WHAT  a different file already where a download goes: replace, both or skip
  -v, --verbose        say how grab connects and searches
```

- `TARGET` is a search: every word must appear in the name, ignoring case, in any order. A
  `TARGET` containing `*`, `?` or `[` is a glob instead, also case-insensitive; one containing
  `/` is a path, absolute or relative to the login home, checked as-is. Without `-s` or `-f`,
  grab looks for files.
- `DEST` is the local parent directory and is created if missing. Without it, grab asks where
  to save after you have picked.
- A different file already in `DEST` (another size, or older than the server's) is never
  replaced without asking: `[r]eplace, [k]eep both, [s]kip, [q]uit`. `--existing` answers in
  advance.
- Without `--limit`, grab follows the speed schedule set in the window; `--limit off` downloads
  without a limit.

```powershell
grab -n -f "season 2" E:\TV                          # show what would be fetched; fetch nothing
grab --all -s "lioness s03" E:\TV                    # every match, without asking
grab -f 'Season.0[1-3]*' E:\Backup                   # a glob
grab "limits" --type video --min-size 50M E:\Movies  # only videos of 50 MiB and more
grab -r box -s movie.mkv E:\Movies --limit 5M --verify
```

Exit codes: `0` ok, `1` usage, `2` config, `3` target not found or pick aborted, `4` remote
lookup failed (search or file list), `5` `grab update` failed, `6` a download failed, `130`
stopped by Ctrl+C. Every option, and what downloads do: [Usage](docs/doc.md#usage).

## Build from source

Toolchain: CMake ≥ 3.28, Ninja, and either LLVM clang-cl (default presets) or MSVC, plus
vcpkg for OpenSSL on Windows: the "vcpkg package manager" component of Visual Studio (found on
its own), or any vcpkg checkout named by `VCPKG_ROOT`. The first configure builds OpenSSL
(a few minutes).

```powershell
cmake --preset clang-cl-debug
cmake --build --preset clang-cl-debug
ctest --preset clang-cl-debug --output-on-failure
# binary: build\clang-cl-debug\grab.exe   (release: clang-cl-release)
```

`cmake --install build\clang-cl-release` puts `grab.exe` and `grab-gui.exe` in
`%USERPROFILE%\programs\grab\bin` (no admin rights needed) and adds that directory to your user
PATH. More in [Build](docs/doc.md#build) and [From source](docs/doc.md#from-source).

## Documentation

[docs/doc.md](docs/doc.md) is the full documentation:

- [How it works](docs/doc.md#how-it-works): the steps of a run
- [GUI](docs/doc.md#gui): the window's folders, search and filters, previews and Play,
  downloads, the queue, priorities, speed limit and schedule, Verify downloads, connections per
  server, updates
- [Configure](docs/doc.md#configure): servers, logins, grab's password store and host keys,
  [grab.conf](docs/doc.md#grabconf),
  [servers grab won't connect to](docs/doc.md#servers-grab-wont-connect-to),
  [notes about settings](docs/doc.md#notes-about-settings),
  [environment](docs/doc.md#environment)
- [Coming from an earlier grab](docs/doc.md#coming-from-an-earlier-grab) (0.20 and before),
  [Going back to 0.20](docs/doc.md#going-back-to-020) and
  [Going back to 0.19](docs/doc.md#going-back-to-019)
- [Usage](docs/doc.md#usage): every option, matching and picking, downloads, `--`, exit codes
- [Lookup methods](docs/doc.md#lookup-methods): find, the walk over SFTP, the find check
- [File lists](docs/doc.md#file-lists): the saved lists searches use
- [Connections](docs/doc.md#connections): `max_connections`, `bind`, the timeouts, and what
  grab fixes
- [Install](docs/doc.md#install), [Build](docs/doc.md#build), [Layout](docs/doc.md#layout),
  [Releasing](docs/doc.md#releasing)
- [Third-party software](docs/doc.md#third-party-software): versions, pins and how to bump them

## Third-party software

- **[rclone](https://rclone.org)**, © Nick Craig-Wood, MIT license
  ([licenses/rclone.txt](licenses/rclone.txt)): grab reuses its file-name encoding tables and its
  password-obscuring key, to name files and read old settings as grab 0.19 and before (which ran
  rclone) did. No rclone program ships with grab.
- **[libssh2](https://libssh2.org)**, BSD-3-Clause
  ([licenses/libssh2.txt](licenses/libssh2.txt)): built into grab, statically.
- **[OpenSSL](https://openssl-library.org)** 3.5 LTS, Apache-2.0
  ([licenses/openssl.txt](licenses/openssl.txt)): built into grab, statically.

Pins and how to bump them: [Third-party software](docs/doc.md#third-party-software).

## License

MIT, see [LICENSE](LICENSE).
