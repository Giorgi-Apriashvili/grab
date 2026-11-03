# grab documentation

This is grab's full documentation; [README.md](../README.md) is the short introduction.

## Contents

- [How it works](#how-it-works)
- [Requirements](#requirements)
- [Build](#build)
- [Install](#install)
  - [From a release (Windows)](#from-a-release-windows)
  - [Updating](#updating)
  - [From source](#from-source)
- [GUI](#gui)
- [Configure](#configure)
  - [Servers](#servers)
    - [Coming from an earlier grab](#coming-from-an-earlier-grab)
    - [Going back to 0.19](#going-back-to-019)
  - [grab.conf](#grabconf)
    - [grab's own client and the rclone fallback](#grabs-own-client-and-the-rclone-fallback)
    - [Notes about settings](#notes-about-settings)
    - [Environment](#environment)
- [Usage](#usage)
- [Lookup methods](#lookup-methods)
- [File lists](#file-lists)
- [Connections](#connections)
- [Layout](#layout)
- [Releasing](#releasing)
- [Third-party software](#third-party-software)
- [License](#license)

## How it works

```
grab -f releases E:\Backup
  ├─ 1. parse args      mode=folder, target="releases", dest="E:\Backup"
  ├─ 2. load config     grab.conf  ->  the server (host, port, user, login), where to search;
  │                     its password or key passphrase from grab's encrypted store
  ├─ 3. resolve remote  the server's saved file list, or over one SSH connection:
  │                     find -L ROOTS -maxdepth N -iname '*releases*' ... where the server has
  │                     GNU find, else its folders listed over SFTP (see Lookup methods)
  │                     0 hits -> exit 3   1 hit -> go   N hits -> numbered pick, several allowed
  ├─ 4. download        file:   byte ranges over up to 14 connections, each asking for exactly
  │                             its bytes (255 KiB reads, up to 64 in flight per connection),
  │                             into movie.mkv.grabpart (progress in movie.mkv.grabpart.json);
  │                             renamed when complete
  │                     folder: listed once over SFTP; files of 2 MiB and more as above, as many
  │                             at a time as At once; smaller ones beside them on every free
  │                             connection, many at a time
  └─ 5. progress        one live line; Ctrl+C stops and keeps the partial files for next time
```

All of it goes through grab's own SSH client (libssh2 and OpenSSL, built in), which logs in once
per connection and keeps the connections open between files, searches and downloads. Logging in
never prompts: grab uses the keys in ssh-agent, or the password or key passphrase from its
encrypted store (see [Servers](#servers)).

rclone, which grab used for all of this up to 0.19, is still inside grab as a fallback until a
later grab removes it: `engine = rclone` in a server's section downloads, searches and lists that
server through rclone and ssh.exe as 0.19 did (see
[grab's own client and the rclone fallback](#grabs-own-client-and-the-rclone-fallback)). Measured
on a Storage Box sub-account: a 3.6 MiB file 4.9 s through grab's client against 10.9 s through
rclone, a 3-file folder 7.3 s against 10.5 s, its file list 3.5 s against 9.6 s.

## Requirements

- Windows 10/11. Nothing to install: grab's SSH client is built in, servers are set up with
  `grab server add` (or Settings in the window), and the [rclone](https://rclone.org) for the
  fallback ships inside grab (see *Third-party software*). You never need to run `rclone config`.
- `ssh.exe` (the OpenSSH client built into Windows 10/11) only for servers on `engine = rclone`
  whose searches use ssh + find, and for their line in Test. A private key ssh.exe uses must have
  an ACL restricted to your user (OpenSSH refuses world-readable keys).

## Build

Toolchain: CMake ≥ 3.28, Ninja, and either LLVM clang-cl (default presets) or MSVC, plus
vcpkg for OpenSSL on Windows: the "vcpkg package manager" component of Visual Studio (found on
its own), or any vcpkg checkout named by `VCPKG_ROOT`. The first configure builds OpenSSL
(a few minutes); vcpkg keeps it in its cache for every later build. A build folder configured
before vcpkg was needed must be configured once with `--fresh`.

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
  - Installs `grab.exe`, the `grab-gui.exe` window and the bundled `rclone.exe` (for servers on
    the rclone fallback) in `%USERPROFILE%\programs\grab\bin`, and adds that folder to your user
    PATH.
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
it stop with their partial files kept. A portable or `cmake --install` copy has its programs
(`grab.exe`, `grab-gui.exe`, `rclone.exe`) swapped in place. The replaced files are left as
`*.exe.old` and removed the next time grab or its window starts. It uses the `curl.exe` and `tar.exe` that ship with Windows 10
and later. Versions before 0.2.0 have no `update` command; install 0.2.0 once from the Releases
page.

The window offers the same update: see [GUI](#gui) → Updates.

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

`grab-gui.exe` (the **grab** Start-menu entry) is a window over the same engine, laid out like
Explorer: pick a server, click through its folders or search them, select rows (click,
Shift/Ctrl-click, arrows, Ctrl+A), choose where to save, and follow the download queue with
live progress. It reads the same grab.conf and remembers the last server, the destination per
server and the rows' sort in `%APPDATA%\grab\gui.json`. Should that file ever be damaged, the
window starts with the defaults, keeps the old file as `gui.json.bad-<date>-<time>` and says
so; the command line, which follows Verify downloads and the speed schedule from it, says so
too and goes on without them.

- **Folders:** the address bar shows where you are (`Box.Guest › movies › georgian.movies`),
  with ↑ (Backspace) to go up and ⟳ to list the folder from the server again.
  - Folders open instantly from the server's saved file list (see [File lists](#file-lists)),
    with their date and item count. Folders deeper than the list (past `max_depth`) are listed
    from the server (one SFTP listing; `rclone lsjson` on `engine = rclone`, about 2 s on a
    Storage Box) and remembered while the window is open.
  - Each start opens the server at its top; while the window is open, each server reopens
    where you left it.
  - **Type-ahead**, as in Explorer: with the list focused, typing a name's first letters picks
    it ("mob" finds `MobLand…`); the same letter again steps through the names starting with
    it, and a pause of a second starts over. What you typed shows at the bottom of the list,
    red when no name starts with it.
  - **Back and Forward**, as in Explorer: the mouse's side buttons, Alt+← and Alt+→, or the ←
    and → buttons (hovering names where they go). Back returns to the folder before, with the
    same row picked and the same scroll; with search results showing, it closes them first.
    One history covers every server while the window is open: going back to another server's
    folder switches the server picker too. It keeps up to 50 folders.
  - **The address bar takes a typed path.** Click its empty part or the current folder, or
    press Ctrl+L, Alt+D or F4, then type or paste a path and press Enter; Esc cancels.
    - You can type `downloads/tv`, `/etc/ssh`, `~` or `~/movies`. `Box.Guest:movies` opens
      another server, and in All servers the address always starts with the server.
    - The path can be anywhere the server lets you list, not only inside the searched folders.
      Such folders are listed live, and the address bar shows them from `/`.
    - A file's path (from Copy path, say) opens its folder with the file picked.
    - A path that doesn't exist leaves the field open with a message, and the folder on screen
      stays as it was.
    - While you type, a list suggests the folders that start with what you typed. ↑/↓ pick
      one; Tab completes it and adds `/` to go on into it. The suggestions come from the file
      list and from folders already opened, so they never wait on the server.
- **Search** (the box beside the address bar):
  - Typing filters the folder at once. Enter searches the folder and every folder below it,
    folders and files together; at a server's top that is the whole server. The address bar
    then ends with "Search results for …", and Esc or ✕ goes back to the folder (or stops a
    search still running).
  - **Exact** (in the box) matches the whole name, case-sensitive. It stays on until you turn
    it off, and results on screen are searched again when you switch it.
  - Results come from the saved file lists, so they appear at once, with "from the file list,
    3 min old" at the bottom. A list older than 15 minutes is refreshed in the background, and
    the results update when the new list arrives; the selection is kept. "Refresh" fetches it
    at once. A server without a list yet is searched live the first time while its list is
    built, and so is a folder deeper than the list.
  - A result's Folder column opens its folder with the result picked.
  - **Filters** (in the box) show only some file types and sizes, in search results and in the
    open folder, at once:
    - Types, several at a time: Folders, Video, Audio, Images, Text, Documents, Archives,
      Programs, Disc images, Other (the kinds the icons show).
    - Size: at least and at most (`700M`, `1.5G`, `20 GiB`; a bare number is MiB), or one of
      the presets (under 100 MiB, 100 MiB – 1 GiB, 1 – 10 GiB, over 10 GiB). A file whose size
      isn't known is hidden while a size is set.
    - Folders show with Folders, or when no type is picked; in an open folder its folders always
      show, so you can keep going.
    - A strip above the list says what's on: "Showing 12 of 340 · video · at least 1.0 GiB ·
      Clear filters". Select all and Download take what's shown. Filters last while the window
      is open.
  - **Recent searches:** click into the empty search box, or press ↓ there, for the last 20
    searches with where they ran and their filters. Click or Enter runs one again there (that
    server and folder, Exact and the filters); × removes one, "Clear history" all of them. They
    are kept in `%APPDATA%\grab\recent`, encrypted for your Windows account (DPAPI) like the
    file lists. A file that can't be read is kept as `recent.bad-<date>-<time>`, and the list
    starts empty.
- **All servers** (at the top of the server picker) starts with one folder per server, and a
  search there covers every server's list at once. Results show their server, and Download
  sends each pick to its own server, into the same Save-to folder. Two picks with the same name
  aren't downloaded into one folder.
- **File icons:** each file shows its type by icon and colour: video, audio, image, text
  (`.nfo`, subtitles, logs), PDF and other documents (e-books, Office), archives (including split
  parts like `.r00` and `.001`), programs (`.exe`, `.msi`, scripts) and disc images (`.iso`,
  `.vhdx`); anything else gets a plain file icon. The Downloads list shows them too.
- **Modified and sorting:** every row shows when the file or folder was last modified
  (`2026-10-16 14:32`, your local time; hovering adds the seconds and the age). A folder's date
  is its own, as file managers show it: it changes when something directly inside it is added,
  removed or renamed.
  - Click Name, Size or Modified to sort: the first click gives A→Z, largest first or newest
    first, the second reverses it, the third goes back to the default order (folders first A→Z
    in a folder, best match for search results). An arrow marks the sorted column; Tab to a
    header and press Enter or Space from the keyboard.
  - Folders stay above files when sorting by name or date, and rows without a size or date go
    last. The selection stays on the same items.
  - Folders have no size (a folder's total isn't known without listing everything inside it).
    Size sorts the files, and the folders follow them in the order you had before: sort by
    Modified, then Size, and the files go largest first with the folders after them, newest
    first.
  - The sort stays, across searches, folders and restarts, until you change it.
- **The status bar shows what grab is doing** whenever something takes a while, with a loading
  bar: "Listing movies on Box.Guest… 12 items so far", "Searching Main with find… 37 found so
  far", "Preparing S04E07.mkv: 18 of 38 MiB", "Loading cover.jpg: 3.2 of 8.0 MiB",
  "Refreshing Box.Guest's file list: 3,100 of ~4,925 entries".
  - The bar shows real progress where grab can count (bytes, entries against the last list,
    servers searched); otherwise it fills over how long the same step took last time on that
    server (remembered in gui.json), creeping on near the end until it's done. A shimmer runs
    along it and the seconds count up, so it never looks stuck.
  - Background work (list refreshes, the update check) shows greyed, only while nothing you're
    waiting on runs. With several at once, "+2" lists the others; hovering also shows the exact
    command running.
  - Instant things (folders from the file list, list searches) show nothing.
- **Previews: double-click or Space opens a file, as in Explorer.**
  - **Videos and audio** play in the program Windows uses for them (VLC, MPC-HC, mpv...),
    streamed from the server, with seeking and the subtitles inside the file.
    - **Warm start:** what a player reads while it opens a file (its start up to the first
      video plus 8 MiB, and its last 4 MiB) is fetched first, and the player starts once it is
      in: it then opens at once (MPC-HC gives up after ~10 s on a file that comes slowly). The
      status bar shows "Preparing <name>: 18 of 38 MiB". An anime release with 28 MB of
      subtitle fonts before its first frame shows it about 3 s after the double-click from a
      fast server (9–15 s when rclone served it).
    - Every byte comes over grab's own SSH connections to the server, whatever its engine
      (previews and Play never start rclone). The player gets grab's own address (a small
      server on this PC only: 127.0.0.1, a random port and secret path); grab reads ahead of
      what the player reads (32 MiB on a Storage Box, 16 MiB elsewhere) and keeps up to 192 MiB
      of a file in memory, two files at most once nothing plays them, freed five minutes after
      their last read.
    - **Connections:** Play takes up to 8 of a Storage Box's connections (2 elsewhere) for the
      whole video, paused too, ahead of downloads: downloads from that server step aside for
      it (it gets fewer beside several of them) and get them back 10 s after the player stops
      reading. While nothing else uses that server, Play keeps them until it frees the file
      (five minutes), so a seek after a pause needs no new login. Two of them stay idle for a
      seek on a Storage Box (one elsewhere; fewer when Play has only a few). While Play fetches the start or a seek, the downloads from that server
      keep as few requests on their way as Play does, so they don't hold its bytes back; a
      download that steps aside with little on its way hands its connection to Play, no login
      needed. On a Storage Box its logins go 8 at once.
    - A seek outside what's in memory gets its first 4 MiB in small pieces over every idle
      connection at once: 0.4–0.7 s on a fast server, longer on a Storage Box, whose
      connections give 1–2.5 MB/s each.
    - When grab can't log in (a key not in ssh-agent, a host key not trusted yet) Play says why
      ("Can't play <name>: ..."); a file that gets no bytes for 30 s stops with the reason.
    - A Store app as the default player (Films & TV) can't open a stream; grab says so.
  - **Images, text files and PDFs** open in a viewer over the list: ←/→ go through the list's
    others (fetched ahead), Esc or Space closes, and Download saves the one shown.
    - Images fit the window; a click shows full size, zoomed on the spot clicked. Zoomed in, drag with any mouse button to move around; a click goes back. jpg, png, gif, webp, avif, bmp, ico, svg.
    - Text (.nfo, .txt, .srt, .log, .md, .json and more) shows its first MiB; .nfo files use
      the DOS code page, so their ASCII art lines up.
    - PDFs use the browser engine's built-in viewer (up to 128 MiB). A link inside one opens
      in your browser only after grab asks, showing the site it really goes to; nothing from a
      preview can replace grab's window.
    - Nothing is written to disk: previews go from the stream to the window in memory.
    - They come over grab's own SSH connections on every server (whatever its engine): one
      connection of the server's budget, kept a minute after the last preview, plus free ones
      for a big image or PDF. Moving on cancels the one still loading. A login that fails is
      tried again only after Test, Trust or a save in Settings (or 30 s, when it may pass by
      itself), so flipping through images never hammers a server that refuses you.
  - Previews and Play open the file by its path: no folder on the way needs to be listable (a
    Storage Box's `/` isn't).
  - Double-click on any other file still downloads it; Enter and the Download button always
    download. Right-click has Play or Preview first; Ctrl+Space ticks a row's checkbox.
- **Download** takes any mix of folders and files. "Choose…" picks the Save-to folder.
  - **Free space is checked first.** What the selection still needs on the Save-to drive
    (files less what's already there, folders as the file list holds them), plus what the
    downloads already queued for that drive still need, is compared with the free space. If it
    won't fit, a dialog says so before anything is queued: "Not enough space on E:", with the
    numbers, and Download anyway or Cancel.
  - A folder deeper than the file list is checked once it's listed, before anything is fetched:
    if it can't fit, it waits paused with the reason. Resume goes ahead (after freeing space,
    or anyway).
  - **Server names become safe Windows names,** the way rclone writes them, so nothing lands
    outside the Save-to folder, whatever a file is called on the server (`..\x`, `C:\x`,
    `\\host\share\x`). `\ : * ? " < > |` become look-alikes (`＼ ： ＊ ？ ＂ ＜ ＞ ｜`), control
    characters `␀`–`␟`, a trailing space or dot `␠` or `．`; a server name that already holds
    rclone's look-alikes keeps them as rclone does (`AC／DC`). Windows device names get an
    underscore (`CON_`, `nul_.txt`), and so do names shaped like 8.3 short names (`REPORT~1_.DOC`:
    on a drive that makes short names, `REPORT~1.DOC` could be your "Report for the board.docx").
    Bidi controls, which make a name read backwards (`invoice` + U+202E + `fdp.exe` shows as
    `invoiceexe.pdf`), are left out of the saved name (`invoicefdp.exe`) and show as `⟨U+202E⟩` in
    the window and its notifications. A folder like `Mission: Impossible (1996)` lands in one
    `Mission： Impossible (1996)`, also when the Save-to folder's own name has such characters.
    - On `engine = rclone`, grab fetches such renamed files itself and checks them by their MD5
      sums. rclone encodes every local path it's given once more, so grab tells it each folder
      and file name in the form that comes back as grab's; a folder whose name rclone can't be
      told (a `‛` in the Save-to folder's name) is fetched by grab's own client, and checked by its
      MD5 sums.
  - **Names that are one on Windows** in a server folder (`README` and `readme`, folders `Docs`
    and `docs`) all arrive: the later file gets a number (`readme (2)`), which the download's
    file list and the command say. The same goes for a name spelled two ways in Unicode (`café`),
    which Windows keeps apart. Names with a line break arrive too (`report␊notes.txt`), and so do
    server names that aren't UTF-8 (an old Latin-1 name).
    - On `engine = rclone`, which would copy one of such names and drop the others (a folder with
      all it holds), grab fetches these itself and checks them by their MD5 sums. A name that
      isn't UTF-8 can't be given to rclone: such a file fails there, saying so.
  - **Paths past 260 characters** (deep folders with long release names) work where Windows'
    long paths are on (the "Enable Win32 long paths" policy, or `LongPathsEnabled` in the
    registry; many PCs have it). Where they're off, a file whose path would be too long fails
    at once, saying so, instead of failing midway or being missed on the next run.
  - **A different file already there is never replaced without asking.** When a file of the
    download is already in the Save-to folder with another size, or older than the server's,
    grab asks first, listing yours and the server's: **Replace**, **Keep both** (the new one is
    saved as "name (2).ext", as Explorer does) or **Skip** (yours stays; that file isn't
    downloaded). Files, and folders the file list holds, are asked about before anything is
    queued, and the answer covers the files the question listed. A folder the file list holds
    only partly asks for itself once it's listed, and waits paused; Resume shows the question.
    A copy that matches the server's is no question: it isn't fetched again.
  - **The same file or folder sent again to the same place is the same download,** however the
    folder is spelled (`D:/Media/` and `d:\media` are one): one already queued, running or paused
    stays as it is, and a failed one starts again from its partial file.
  - **A downloaded program never runs in place of a tool.** The programs grab starts by name
    (ssh, curl, tar, the editor, Explorer) come from System32, the Windows folder or
    PATH's full folders, never from the current folder or grab's own, so an `ssh.exe` that lands
    in the folder you run `grab` from is never started. A full path in grab.conf is used as given.

- **Right-click menus:**
  - **On a row:** Open (folders), Download, Download to… (pick a folder for this download only;
    Save to stays), Open folder location (search results), Copy path (the server path) and Copy
    name. With several rows selected, they act on all of them.
  - **On a download:** Pause, Resume or Retry, Cancel, Priority, Move to top, Show in folder and
    Copy path (where it lands on this PC). A file inside a folder download has Pause, Resume,
    Skip and Copy path.
  - Shift+F10 or the menu key opens the menu for the row under the cursor.
- **Downloads** run several at a time: "At once" in the Downloads bar, 1–8, default 4, is how many
  files download at the same time, all downloads together. Each file of a folder counts (on
  grab's own client each one of 2 MiB and more; smaller ones go beside them, see Folders): at 1
  a folder goes file by file; at 4 it runs up to 4 of its files, fewer while other downloads run.
  A file then gets more of the server's connections, so fewer at once costs no speed. The
  grab command downloads a folder's files as many at a time as the window's At once. Each
  download can be paused, resumed or cancelled, or all together with Pause all / Resume all and
  Cancel all.
- **Resumable file downloads:** grab fetches a file in byte ranges over several connections at
  once, each asking the server for exactly its range's bytes (`rclone cat` streams on `engine =
  rclone`), into `<name>.grabpart`. It records each range's progress in `<name>.grabpart.json`
  and renames the file when it is complete. A partial file from either engine resumes on the
  other.
  - Pausing keeps both; resuming continues from the last saved bytes.
  - grab reuses or deletes only partial files it made: their `.json` record names the server
    and its file (the same path on another server is another file). When something else
    already has those names (your own files, or files of the folder
    being downloaded), it uses `<name>.2.grabpart` and so on, and leaves them alone. Cancelling
    deletes only grab's partial files, and, on `engine = rclone`, of rclone's temporary
    `.partial` files only the ones its stopped copy had just made.
  - This survives closing grab: unfinished downloads come back paused (the list is kept in
    `%APPDATA%\grab\queue.json`). Should the list be damaged, the queue starts empty, the old
    file is kept as `queue.json.bad-<date>-<time>` and the window says so.
  - If the file changed on the server in the meantime, it starts over.
  - A file already in the destination, with the same size and not older than the server's
    copy, is not fetched again ("already there, nothing fetched").
  - When a stream runs out of work, it splits the busiest remaining range, so the end of a
    file doesn't trickle in over one connection.
- **Queue order:** the list order is the order in which waiting downloads start. Drag a row
  by its ≡ handle, or use ↑ / ↓ / ⤒ on waiting rows ("Queued · #1" is next). The order is kept
  across restarts.
- **Bandwidth priority** (High / Normal / Low on each download) decides how running downloads
  share, weighted 4 : 2 : 1. Low always keeps moving, and unused share is lent to the others.
  - Downloads from the same server split its connections by weight. On a Storage Box, where
    each connection is capped, that is the bandwidth split: High vs Low measured 7 vs 1
    connections, about 12 vs 1.4 MiB/s.
  - Under a speed limit, the limit is split by weight exactly, across servers too (measured
    3.0 vs 0.8 of 4 MiB/s).
  - With no limit, downloads from *different* servers compete on your line directly.
    Priority then acts only through their connection counts.
- **Speed limit:** the bar below the downloads shows the total speed. On its right, "Limit
  speed" caps all downloads together, in MiB/s; the switch keeps the value, and both are
  remembered.
  - Changes apply at once. Downloads are paced by a shared token bucket: while limited, grab's
    client asks each connection for one read at a time, after taking its tokens, so the network
    really follows the limit (on `engine = rclone`, rclone is told not to read ahead).
  - On `engine = rclone`, a folder's small-file batch gets rclone's `--bwlimit` and counts
    against the same total.
- **Verify downloads** (Settings → Downloads, off by default): after each download, the server
  computes an MD5 checksum of its copy (`md5sum`, run through grab's own SSH connection) while
  grab computes yours, and the two are compared. The row shows "Verifying the checksum…" (a
  folder: "Verifying the checksums… 12 of 40"), then "checksum verified (MD5)".
  - Both sides read the file once more, at the same time: about 2 seconds per GB in tests (an
    858 MiB file from a VPS took 1.8 s, a 15-file, 352 MiB folder 1.1 s; `rclone check` took 3.0
    and 4.1 s).
  - Each file is compared by its own path, whatever its name here: a Keep-both copy, a device
    name such as `CON.txt`, names that differ only in case or in how é is spelled, a name that
    isn't UTF-8.
  - A copy that differs is deleted and the download fails; Retry downloads it again. In a
    folder, only the files that differ are deleted and fetched again. grab deletes a copy only
    after one more look shows that neither side changed since the download: a file changed on
    the server since, or your copy changed since (a player writing its tags, say), is kept and
    marked "not verified: the server's copy changed since the download" (or "your copy").
  - Every server is checked this way, whatever its engine, so grab logs in with its own client
    (its host key pinned; Trust… once for a server set up outside grab). A server it can't log in
    to, one without `md5sum` (Windows, an SFTP-only account), a name the server's shell can't be
    given, or a server that takes too long leaves the download done and the copy kept, marked
    "not verified" and why. The server's Test says "checksums: md5sum", or why not.
  - Files that were already there are not checked, since nothing was downloaded.
  - The `grab` command follows the same setting (see [Usage](#usage)).
- **Speed schedule** (Settings → Speed schedule) caps the speed at set times, for example 5
  MiB/s on weekdays from 18:00 to 24:00.
  - Each time has its days, a from–to time and a speed. A time that ends at or before its
    start runs into the next day ("22:00–06:00", marked "next day"), and equal times are a
    whole day. Where times overlap, the lower speed wins.
  - When a time starts, Limit speed switches on at its speed, and the status bar says
    "Scheduled until 24:00". When it ends, your own Limit speed setting applies again.
  - Changing Limit speed during a scheduled time lasts until the schedule's next change
    ("Schedule resumes at 24:00"), also across a restart. Editing the schedule ends it.
  - The `grab` command follows the same schedule (see [Usage](#usage)). Each program caps its
    own downloads.
  - At each change, downloads keep their connections and just follow the new speed. On `engine
    = rclone`, a folder's small-file batch starts again with the new cap (the files it had in
    flight start over), and the other downloads reconnect when the limit switches on or off.
- **Folders** are listed once and shown as a tree under their row, collapsible with ▸/▾.
  - Finished files fold into a "N done" line, and a long queue shows its next 20 files.
  - Each file can be paused, resumed or skipped. The folder's own buttons act on all of its
    files.
  - Files of 2 MiB and more take the resumable path above, as many at a time as At once allows,
    their pieces spread over the server's connections.
  - Smaller files go beside them on every free connection, many at a time (up to 32 files and 8
    MiB of reads in flight per connection), over connections already logged in, since a new SSH
    login per small file would dominate. Pausing drops the progress of small files that were in
    flight.
  - On `engine = rclone` the line is at 64 MiB: smaller files go in one `rclone copy` batch that
    copies as many at a time as At once allows. Its connections follow its share: when another
    download from the same server starts, it starts again with fewer within a few seconds, and
    with more once they're free again (the small files in flight start over).
  - The tree and each file's state survive closing grab. Files already complete on disk are
    not fetched again.
- **Connections per server:** every download from a server shares that server's connection
  budget.
  - The budget is 14 for Hetzner Storage Boxes and 12 elsewhere. Set it per server in
    Settings, or as `max_connections` in grab.conf.
  - A lone download uses the whole budget; several split it fairly. A small file that can
    only use one connection lends the rest to the others.
  - The budget is a hard limit: with more downloads queued for a server than it has
    connections, the window starts only that many, and the rest wait their turn. Each file's
    first look at the server (its size and date) takes one of them too.
  - Play takes up to 8 connections of a Storage Box's budget (2 elsewhere), ahead of
    downloads, while a video plays (see Previews). Previews take one connection of the budget,
    ahead of downloads, and only free ones beyond it.
  - If a server refuses a connection while others of grab's get data, grab lowers that server's
    budget and remembers it (in `limits.json` next to
    gui.json, shared with the `grab` command). Each download from that server that ends
    without a refusal raises it by one again, back to the default, so a drop caused by a
    passing hiccup wears off.
  - A server that refuses everything is full with something else's connections: grab asks
    again one connection at a time, 1, 2, 4, 8, then 15 seconds apart, and after 5 minutes
    without any data the download fails ("the server keeps refusing connections"); Retry or
    Resume picks it up again. A refused login or a changed host key stops the download at
    once with the server's message, instead of trying again.
  - The window and a `grab` command running at the same time each use the whole budget; only
    what they learn is shared.
  - Measured on a Storage Box, where each connection is capped at about 2.5 MB/s: 1
    connection gives 2.5 MB/s, 8 give about 14 MB/s, and 14 about 19 MB/s. `rclone copyto`
    reached about 5 MB/s.
  - Hetzner documents 10 connections per Storage Box account, but boxes accepted 16 on one
    account, and 28 across a main account and its sub-account at once. Searches, file lists
    and folder listings that walk the server's folders take their connections from the same
    budget (see [File lists](#file-lists)).
  - Which connections, how long grab waits for a server, and the local address to connect from
    are set per server: see [Connections](#connections).

- **Updates:** once a day, when the window opens, grab asks GitHub for the latest release (one
  request; a failure stays silent). A newer one shows a banner: `grab 0.7.0 is available ·
  What's new · Update · Later`.
  - Update runs `grab update` (the same SHA-256 check) and the banner follows it:
    downloading, verifying, installing.
  - An installed copy: the installer closes the window and opens it again. A portable copy:
    the programs are swapped, then "Restart grab" opens the new version.
  - Downloads running at that moment continue by themselves in the new version; paused ones
    stay paused.
  - Later hides the banner until the window opens again. Settings → Updates has the daily
    check's switch and "Check now".
- **Settings** (the gear, or Ctrl+,) manages servers like `grab server` does: add, edit,
  test, trust, make default and remove. Each server's search folders and depth are edited
  there too. Adding a server, or changing its address, shows the host key fingerprints
  and writes nothing until you trust them. A saved password stays with the user, host and port
  it was typed for: changing any of them asks for it again. A server without connection details
  (one grab couldn't import, see [Servers](#servers)) opens with Edit to fill them in. "Open
  grab.conf" covers the rarer settings: `bind`, `connect_timeout` and `io_timeout` (see
  [Connections](#connections)), the find method, and `engine = rclone` for the fallback. A
  server card says "rclone (fallback)" for a server on rclone, and the settings' one-time notes
  (see [Notes about settings](#notes-about-settings)) show once in the notice above the search.
- **The window's page** is grab's own and nothing else: it only shows https://grab.ui/index.html,
  only that page's messages reach grab, and files dragged in from outside are ignored.
- **Tray:** closing the window while downloads run keeps them going in the notification
  area, and grab exits by itself when they finish. When the window is idle, closing it
  quits. A notification reports each finished or failed download while the window isn't in
  front. Clicking it, or the tray icon, brings the window back.
- It needs the Microsoft Edge WebView2 Runtime, which ships with Windows 11 and current
  Windows 10. Without it, grab-gui offers the download link.
- It never prompts for a passphrase, since it has no console: grab's client takes a key file's
  passphrase from grab's store (Settings → Edit), or the key from ssh-agent. On `engine =
  rclone`, searches through ssh + find need a passphrase-protected key loaded into ssh-agent
  (`ssh-add <key file>`); without it a search fails fast with that hint instead of hanging.
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
grab server list             # each server's login, saved password, engine and search, host key
grab server edit NAME        # the same questions, with the current answers as defaults
grab server test NAME        # log in as downloads, searches and verification do
grab server trust NAME       # pin a server's host key (for servers imported without one)
grab server remove NAME
```

Each server is a section of grab.conf with its connection details:

```ini
[box]
host = u123456.your-storagebox.de
port = 23
user = u123456
auth = password
```

- `auth = agent`: the keys loaded in the Windows ssh-agent (`ssh-add`); with `key_file`, only
  that key.
- `auth = key`: `key_file`, with its passphrase, if it has one, in grab's password store.
- `auth = password`: the password in grab's password store.
- `auth = stored_key`: a key an rclone.conf held inline (`key_pem`), imported into grab's store
  with its passphrase; grab's client (from memory, as it reads key files) and rclone both log in
  with it. Edit can switch it to a key file or ssh-agent.

How every connection to it is made (`bind`, `connect_timeout`, `io_timeout`, `max_connections`)
is in [Connections](#connections); which client does the work, in
[grab's own client and the rclone fallback](#grabs-own-client-and-the-rclone-fallback).

`grab server add` asks for:
- the server's host, port and user
- how to log in: a password, a key file (with an optional passphrase), or ssh-agent
- which folders to search

Then it:
- **pins the host key:** shows the server's fingerprints (ED25519, ECDSA and RSA, whichever
  it has; grab reads them itself, without logging in) and saves them to
  `%APPDATA%\grab\known_hosts` once you confirm, in place of any it held for that address. From
  then on grab, ssh and rclone refuse a server that presents a different key, and a server
  without a pinned key isn't contacted at all until you trust it. Changing a server's host or
  port, or removing it, takes its keys out again unless another server uses the same host and
  port. A server that offers only old key types (SHA-1 `ssh-rsa`, DSA) or old key exchanges
  can't be added.
- **saves the server:** its section goes into grab.conf, its password or passphrase into grab's
  password store.
- **tests the connection:** grab's own client logs in, lists the home folder and says whether
  searches will use find or list folders, and whether Verify can run. On a server on `engine =
  rclone` it adds one check through rclone (its downloads) and, for key or agent logins
  searched with ssh + find, one through ssh.exe (its searches); grab's own login is then only a
  warning when it fails (previews, Play and Verify can't run). The server's shell and checksum
  commands, which rclone would otherwise ask the server on each of its runs and Verify goes by,
  are noted in the section as `shell_type`, `md5sum_command` and `sha1sum_command`.

`grab server edit NAME` asks the same questions with the current answers in brackets; Enter
keeps one. The name stays, since partial downloads, file lists, limits and the saved password
go by it. Enter keeps the saved password only while user, host and port stay the same: for
another login it asks again, so a password never goes to a machine or account it wasn't typed
for. A new host or port shows the host key to trust first; a new user, host or port also drops
what grab learned about the old one (its connection limit, the window's timings, the shell and
checksum commands). A section without connection details is filled in the same way.

`grab server list` shows each server's login (`password saved` or `no password saved`, a key
file's `passphrase saved`), its engine and how it is searched (`engine grab, search with find or
by listing folders`, `engine rclone (fallback), search with ssh + find`), `host key pinned` or
`host key not trusted (grab server trust NAME)`, and the connection keys that are set (`bind
10.0.0.2`). A server kept on rclone by itself gets a second line saying why (see
[grab's own client and the rclone fallback](#grabs-own-client-and-the-rclone-fallback)).
`grab server test NAME` prints what Test in Settings shows, and repeats the server's
[notes about settings](#notes-about-settings):

```
Testing box (u123456@u123456.your-storagebox.de:23) ...
  grab (downloads, searches, previews, Play, verify): OK, home folder listed, searches by listing folders (a Storage Box has no find, so grab doesn't ask), checksums: md5sum
```

On `engine = rclone` the same server shows `rclone (downloads): OK` first and `grab (previews,
Play, verify): ...` after it.

Passwords and passphrases are never shown and never put on a command line.
- grab keeps them in its own store, `passwords` in `%APPDATA%\grab` (a configuration elsewhere
  keeps its own, see [grab.conf](#grabconf)), encrypted for your Windows account (DPAPI). Other
  accounts, backups and copies of the file can't read them; programs running as you can, as
  grab does, without asking you for anything. grab.conf never holds one: it is plain text that
  editors and sync tools open.
- Each one is kept with the user@host:port it was typed for, and grab sends it nowhere else: a
  hand edit of `host` in grab.conf, or a grab.conf copied from somewhere else, gets "the saved
  password is for ...: enter it again with Edit" instead.
- grab's own client gets them in memory. When grab runs rclone (`engine = rclone`), it hands
  that rclone run alone the server's details and password (in rclone's obscured form, which
  rclone reads like its config), in its own environment, with rclone's config kept in memory
  (`--config NUL`): never on disk, on a command line, or in grab's own environment, where every
  program grab starts would see it.
- If the store can't be read (another Windows account, a damaged file, or a Windows password an
  admin reset), downloads, searches, lists, previews, Play and Verify stop for every server of
  that configuration and say why: the store may also hold a server's rclone settings (a proxy,
  an ssh command, see [Coming from an earlier grab](#coming-from-an-earlier-grab)), which a
  connection must not go past. Save a server in Settings → Edit (or `grab server edit NAME`):
  for a password server, enter its password again; one that logs in with ssh-agent or a key
  file needs no change. Saving sets the whole store aside, with any proxy or ssh command a
  server kept there: that server then connects directly. Before saving, put the proxy in its
  `common_flags` (`--sftp-socks-proxy`, `--sftp-http-proxy`); an ssh command comes back only
  with the old file, once it can be read again.
  - That first change keeps the old store as `passwords.unreadable-<date>-<time>` (it may become
    readable again, e.g. once a domain controller is reachable after a reset), starts a new one
    and names the other servers whose passwords need entering again.
- Elsewhere than Windows the store is a plain file only you can read (mode 0600), not
  encrypted.

The commands also take answers from piped input, one per line, so they can be scripted. When
the input runs out, the command stops without changing anything.

Every server is searched with find where it has GNU find (grab checks once a week and remembers
the answer in `limits.json`; Test checks again), else by listing its folders, whatever its login
(see [Lookup methods](#lookup-methods)). On `engine = rclone`, password servers are searched with
rclone's listing and key and ssh-agent servers with ssh + find, as up to 0.19.

#### Coming from an earlier grab

Up to 0.19, a grab.conf section named a remote in an rclone.conf (grab's own
`%APPDATA%\grab\rclone.conf`, or the one `rclone_config =` names) that held the server's
address and login, and grab kept the passwords in `%APPDATA%\grab\secrets`. The first run of
0.20, the command or the window, moves each such server into grab.conf, once:
- Its section gets `host`, `port`, `user`, `auth` and `key_file` from its rclone remote (as
  rclone reads it: a dotted name like `box.eu` takes what it lacks from `[box]`), and
  `shell_type`, `md5sum_command` and `sha1sum_command` as rclone saved them. `rclone_remote`,
  your comments and every other setting stay.
- Its password or passphrase goes from the old store (or the rclone.conf) into the new
  `passwords` store, taken out of rclone's obscured form and encrypted for your account.
- When its remote named its own known_hosts file, the host keys there for that host and port are
  copied into grab's known_hosts (unless grab's has some already), so it stays as trusted as it
  was. Keys it can't copy (a network path, a file over 1 MiB, wildcard or `@cert-authority`
  lines) are left out with a note to use Trust.
- Other rclone settings stay with the server: harmless ones as `rclone.<key>` lines in
  grab.conf, any that could hold a secret in the password store, both only for its rclone runs.
- `imported.json` beside the store records what was moved, so each server is moved once: one
  you remove later doesn't come back from the old files.
- The command prints one line naming the servers moved, and a note for each one that needs
  something (Trust, its password, its details); the window says the same once, in a notice
  above the search.

rclone.conf, the old `secrets` store and `%APPDATA%\rclone\rclone.conf` are only read, never
changed, and grab doesn't write any rclone.conf again. A section it can't move (no such remote,
not sftp, an encrypted rclone.conf, an old store it can't read for a password server) keeps no
details: Settings and `grab server list` say why, Edit fills it in, and the next run tries
again once that rclone.conf changes.

The same first run of 0.20 also moves the work onto grab's own client. Up to 0.19 every download,
search and list went through rclone and ssh.exe; from 0.20 a server without an `engine` line
uses grab's own client, unless its rclone settings ask for something that client can't do (a
proxy, an ssh command: it then stays on rclone by itself, see
[grab's own client and the rclone fallback](#grabs-own-client-and-the-rclone-fallback)):
- One note says so, once, naming the servers the import moved (not a server added since, nor
  one with an `engine` line): "Downloads, searches and lists now use grab's own SSH client, no
  longer rclone (Main, Box); their saved file lists are fetched again once. To go back to rclone
  for a server until a later grab removes it, set engine = rclone in its section (grab config,
  or Open grab.conf in grab's Settings)."
- Each server's saved file list is fetched again once (a list remembers how it was made).
- Of `common_flags`, grab's client takes `--bind`, `--contimeout` and `--timeout` as `bind`,
  `connect_timeout` and `io_timeout` while those are blank; other flags you changed there, in
  `folder_flags`, `file_flags` or `ssh_options` get a note once (see
  [Notes about settings](#notes-about-settings)). grab.conf isn't rewritten for any of this.

#### Going back to 0.19

0.19.0 reads the same grab.conf: it ignores the new keys and finds each moved server through
`rclone_remote` in the rclone.conf and `secrets` that 0.20 left as they were. So it sees the
servers as they were when you upgraded:
- a password changed in 0.20 isn't seen: 0.19 keeps logging in with the old one, which can lock
  a Storage Box account or set off a server's fail2ban (change it in 0.19 too);
- servers added in 0.20 show as not usable (0.19 looks for them in rclone.conf). With a blank
  `rclone_config =`, if `%APPDATA%\rclone\rclone.conf` has a remote of the same name, 0.19
  copies it into its rclone.conf and logs in with it, which may be another address or account;
- a server removed and added again in 0.20 under the same name is the old remote to 0.19 (old
  address, old password);
- on the way forward again, addresses and passwords changed in 0.19 aren't seen by 0.20:
  grab.conf's details win;
- 0.19's rclone still notes in that rclone.conf what it finds out about a server (its shell,
  for one), as it always did;
- 0.19 has only rclone: it ignores `engine` lines, and every server downloads, searches and lists
  through rclone and ssh.exe again;
- 0.19 ignores `bind`, `connect_timeout` and `io_timeout`, so a bind written only as `bind =` is
  lost there. To keep one for 0.19, keep `--bind` in `common_flags` too (0.20 notes once that
  `bind` wins over it; nothing breaks);
- 0.19 refuses the whole grab.conf on `find = walk`: write `find = rclone`, which 0.20 reads as
  walk.

If `rclone_config =` points at an rclone.conf you also use with rclone on its own, servers you
add or change in grab no longer show up there.

### grab.conf

```powershell
grab --init          # writes %APPDATA%\grab\grab.conf, if absent
grab config          # opens it in your editor, creating it first if needed
```

`--init` writes the `[grab]` block. If you already have sftp remotes in the standard
rclone.conf (`$RCLONE_CONFIG`, else `%APPDATA%\rclone\rclone.conf`), it adds one grab.conf section
for each, with its connection details and its password moved as described in
[Coming from an earlier grab](#coming-from-an-earlier-grab) (rclone.conf is only read):
- `search_roots` is blank, meaning the login home.
- `default_remote` is the first one.
- `find = auto`, with commented-out `bind`, `connect_timeout` and `io_timeout` lines (see
  [Connections](#connections)) and a commented-out `engine = rclone` for the fallback.

Run `--init` again later: it never rewrites an existing file, but prints ready-to-paste
sections for sftp remotes added to the standard rclone.conf since; the next grab run moves
their details in as above.

`grab config` uses the `editor` key in `[grab]` (for example `editor = code --wait`),
otherwise `$VISUAL`, then `$EDITOR`, and finally Notepad. It reads only that key, so a
config with a syntax error can still be opened and fixed.

Minimal `grab.conf`:

```ini
[grab]
default_remote = hetzner

[hetzner]
host          = files.example.com
user          = alice
auth          = agent
search_roots  = /home/alice
max_depth     = 4
```

`grab server add` writes sections like this for you.

All keys and their defaults are documented in [grab.conf.example](../grab.conf.example).
The config path can be overridden with `--config PATH` or the `GRAB_CONFIG` env var.
grab-gui then runs one window per configuration folder, and each configuration other than the
default keeps its own window settings, queue and recent searches in
`%APPDATA%\grab\configs\<folder>-<id>` and its file lists in
`%LOCALAPPDATA%\grab\lists\<folder>-<id>` (the command reads the same), and its saved passwords
there too, since two configurations can have servers of the same name. Moving such a
configuration's folder leaves its passwords behind (`grab server list` then says "no password
saved"), as it leaves its lists. grab's known_hosts is shared by all of them.
Add one `[section]` per server and choose with `-r NAME`.

#### grab's own client and the rclone fallback

Every server downloads, searches, lists and browses through grab's own SSH client unless its
section says `engine = rclone`; previews, Play and Verify use grab's client on every server,
whatever its engine. A search or a list uses the server's GNU find when it has one and the `find`
setting allows it, else it lists folders over SFTP, several at a time; links are followed. It
logs in once and keeps the connections open between files, asks the server for exactly the bytes
it needs, and continues partial downloads rclone started (and the other way round). It also
downloads names that aren't UTF-8, and a subfolder the server won't list is reported instead of
failing the whole folder.

**The fallback.** `engine = rclone` in a server's section (`grab config`, or Open grab.conf in
Settings) sends its downloads, searches and lists through the bundled rclone, and ssh.exe for
ssh + find, as 0.19 did, until a later grab removes it. `GRAB_ENGINE=rclone` in the environment
does the same for every server of one run. It is there for a server grab's client fails on where
rclone works: when the key exchange or the choice of algorithms fails, or the server's SFTP
answers in a way grab can't read, the error ends "...; for Box's downloads, searches and lists
to go through rclone until a later grab removes it, set engine = rclone in [Box] (grab config, or
Open grab.conf in grab's Settings)". A wrong password, a refused key or a timeout fail the same
way on both, so they don't say it. Such a server shows `engine rclone (fallback)` in `grab server
list`, "rclone (fallback)" on its card, and `-v` says "Box through rclone (the fallback)". Only
servers on rclone use:
- `common_flags`, `folder_flags` and `file_flags`: rclone flags (see [Connections](#connections)
  for which of them still do something);
- `ssh_options`: options for ssh.exe, which searches with ssh + find and runs Test's ssh line;
- `rclone.<key>` lines and the rclone settings the import kept in grab's store;
- `[grab]`'s `rclone =` (blank: the rclone.exe beside grab) and `ssh =` (blank or `ssh`: Windows'
  ssh.exe).

**Servers kept on rclone.** Some rclone settings ask for what grab's client can't do: a SOCKS or
HTTP proxy, an ssh command in place of the SSH client, a ProxyCommand or a jump host, another
SFTP subsystem or server command, a certificate login, insecure ciphers, cipher, key exchange,
MAC or host key lists that share nothing with grab's, one read at a time
(`disable_concurrent_reads`) or fstat in place of stat (`use_fstat`). grab looks for them in
`rclone.<key>` lines, the rclone settings the import kept in its store, `--sftp-*` flags in
`common_flags`, `-o ProxyCommand`, `-o ProxyJump` or `-J` in `ssh_options`, and `RCLONE_SFTP_*`
variables in the environment (`RCLONE_SFTP_SOCKS_PROXY`, `RCLONE_SFTP_HTTP_PROXY`...).
- A server with one of them and no `engine` line stays on rclone by itself. grab's own client
  refuses it rather than connect past what you set up, so previews, Play and Verify are off for
  it; through a proxy or an ssh command its searches and lists go by rclone's listing (ssh.exe
  wouldn't take that way either). `engine = grab` (or `GRAB_ENGINE=grab`) on it is refused where
  it's used, with the same reason.
- `grab server list`, Test and `-v` say why every time: "[Box] connects through a SOCKS proxy
  (rclone.socks_proxy), which grab's own client can't: its downloads go through rclone, and its
  searches and lists by rclone's listing; previews, Play and Verify are off for it". The reason
  names the setting, never its value.
- With an `ssh` setting, rclone hands the connection to that command, which picks the host and
  checks host keys itself, not against grab's known_hosts; the reason says so.
- Trust, and the host key check of Add and Edit, connect directly from this PC: for a server
  that goes through a proxy or a command they ask first.
- A password store that can't be read counts as such a setting for every server (see
  [Servers](#servers)).

#### Notes about settings

Settings that mean something else, or nothing, on grab's own client get a note, once per
configuration, in whichever program meets them first: the command prints `grab: note: ...`, the
window shows it in the notice above the search. `grab server test NAME` and `-v` repeat a
server's notes every time. A note comes again when its setting changes.
- The switch to grab's own client, after the import (see
  [Coming from an earlier grab](#coming-from-an-earlier-grab)).
- `common_flags`: "[Main] common_flags: grab reads --bind 10.0.0.2 as `bind = 10.0.0.2` for now.
  Write that line in [Main] (grab config); common_flags is only for rclone and goes away with
  it." (likewise `--contimeout`, `--timeout`); "[Main] common_flags: --sftp-connections 4 is for
  rclone only; to cap grab's connections to Main, set max_connections = 4 in [Main]."; other
  flags with what grab does instead.
- `folder_flags` and `file_flags`: a `--bind` there ("reaches only rclone's folder batch; write
  `bind = 10.0.0.2` for every connection"), and flags that were for `--rclone`, which is gone.
- `ssh_options` on a server on grab's client: what to use instead (`-o ConnectTimeout=10`:
  `connect_timeout = 10s`; BindAddress: `bind`; ServerAliveInterval: grab sends its own
  keepalives).
- `rclone.<key>` settings that change what happens on grab's client (`skip_links`: grab follows
  links; `connections`: `max_connections`; `chunk_size`, `concurrency`, `idle_timeout`, `set_env`,
  `set_modtime`).
- A server kept on rclone, with the reason.
- A key grab.conf doesn't know: "[Main] connect_timout isn't a grab.conf setting (ignored)."

Only flags that differ from the old defaults are noted, flag by flag, so a grab.conf with the
defaults gets none. Notes name settings and quote binds, times, sizes and counts, never a proxy
or an ssh command (they can hold a password). `notices.json` beside grab's other state
(`%APPDATA%\grab`, or the configuration's own folder) remembers which were shown, by a SHA-256,
never the setting's value.

#### Environment

- `GRAB_CONFIG`: grab.conf's path, as `--config`.
- `GRAB_ENGINE=grab` or `rclone`: every server's engine, whatever grab.conf says, for the
  fallback or to compare the two; a server kept on rclone by itself is refused under `grab`.
- `RCLONE_CONFIG`: the standard rclone.conf that `--init` and the import read.
- rclone runs grab starts (`engine = rclone`) get one server's `RCLONE_CONFIG_<SERVER>_*`
  variables from grab alone (see [Connections](#connections)); `RCLONE_CONFIG_*` variables of
  your own are left out of them, and grab-gui drops any it inherited when it starts. Players,
  editors, ssh and the installer get no server's details at all.

## Usage

```
grab [-s|--file | -f|--folder] TARGET [DEST] [options] [-- FLAGS]
grab config [-c PATH]

  -r, --remote NAME    grab.conf section to use
  -c, --config PATH    grab.conf path
      --depth N        override max_depth for this run
      --limit RATE     cap the download speed: 5M, 800K, 2.5 (a bare number is MiB/s), or
                       off. Without --limit, the speed schedule set in grab's window applies
      --first          take the best match instead of asking
      --all            take every match instead of asking
      --exact          match TARGET as a whole name or glob, case-sensitive
      --type KINDS     only files of these kinds, comma-separated: video, audio, image, text,
                       document, archive, program, disc, other (e.g. --type video,audio)
      --min-size SIZE  only files at least this big: 700M, 1.5G, 20GiB (a bare number is MiB)
      --max-size SIZE  only files at most this big
      --fresh          fetch the server's file list now instead of using a saved one
      --verify         check each download against the server's checksum (MD5) afterwards;
      --no-verify      don't. Without either, the setting in grab's window applies (off at first)
      --existing WHAT  a different file already where a download goes (another size, or
                       older than the server's): replace it, both (keep yours and save the
                       new one as "name (2).ext") or skip it. Without it grab asks first, or
                       stops when there is no console to ask on
  -- FLAGS             connection flags for this run: --bind ADDRESS, --contimeout TIME (or
                       off) and --timeout TIME (1s to 24h on grab's own client) win over the
                       server's bind, connect_timeout and io_timeout. A server on engine =
                       rclone also takes rclone's other connection flags (--sftp-*...), but
                       not its address, login or host keys (grab server edit NAME)
  -n, --dry-run        resolve the target and show what would be fetched; transfers nothing
                       and creates no folders
  -v, --verbose        say how grab connects and searches, the settings' notes again, and the
                       command lines of servers on rclone (engine = rclone) before they run
      --init           write grab.conf to the config path if absent, with the sftp remotes of
                       the standard rclone.conf as servers (rclone.conf is only read)
```

- Searches use the server's saved [file list](#file-lists) when it is under 15 minutes old,
  which makes them instant. Otherwise grab fetches a new list first ("fetching the file list
  of Box…") and saves it. `--fresh` always fetches.
- `TARGET` is a search: every word must appear in the name, ignoring case, in any order.
  `grab -s lioness E:\TV` lists every file with "lioness" in its name.
  `grab -s "lioness s03e08" E:\TV` narrows that to one episode, and so does
  `grab -s "the unravelling" E:\TV`. When a live search uses find, the filtering happens on the
  server (`find -iname`), so only hits travel back.
- A `TARGET` containing `*`, `?` or `[` is a glob instead, also case-insensitive:
  `grab -f 'Season.0[1-3]*' E:\Backup`. `--exact` matches the whole name, case-sensitive.
- Matches show their size and date in the list, folders their date:
  `[3] movies/x.avi  (897.9 MiB, 2023-10-15)`. find gets sizes and dates from GNU find's
  `-printf`, which any mainstream Linux server has; the find check sends a server without it to
  the folder walk, which reads them over SFTP (on `engine = rclone`, use `find = rclone` for
  such a server).
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
- Downloads use the same engine as the window: a file in byte ranges over the server's
  connection budget (`max_connections`; 14 on Storage Boxes, 12 elsewhere), a folder listed
  once with files of 2 MiB and more fetched the same way and smaller ones on every free
  connection (on `engine = rclone`: 64 MiB, and one rclone batch). Picks run one after
  another, each with the whole budget.
  - Progress is one line, redrawn in place:
    `412.0 MiB / 898.0 MiB  45%  18.0 MiB/s  14 conns  ETA 0:26` (folders add `4/13 files`).
    When stderr is not a console (a log file, a script), only the start and end lines appear.
  - Free space is checked before fetching: the files picked together, and each folder right
    after it's listed. If it won't fit, grab stops with `grab: error: not enough free space on
    E: (48.2 GiB needed, 31.0 GiB free)` (exit code `6`), keeping any partial files.
  - Ctrl+C stops cleanly and keeps the partial files: `stopped at 35%; run the same command
    again to continue`. A second Ctrl+C ends grab at once.
  - A file already in DEST with the same size, and not older than the server's copy, is not
    fetched again (`already there, nothing fetched`); in a folder, the same goes per file.
  - Server names become safe Windows names, as in the window (see [GUI](#gui)): nothing lands
    outside DEST.
  - A different file already in DEST (another size, or older than the server's) is never
    replaced without asking. grab lists them, yours and the server's, and asks:
    `[r]eplace, [k]eep both, [s]kip, [q]uit` (Keep both saves `name (2).ext`). Picked files are
    asked about together before fetching any, each folder once it's listed; an answer goes for
    what that question listed. With several picks, `R`, `K` or `S` (capital) answers every later
    question of the run too, and `q` stops the run: the picks after it don't start.
    `--existing replace`, `both` or `skip` answers everything in advance; without a console to
    ask on, grab stops with exit code `6` and replaces nothing unless `--existing` is given.
  - Keep both doesn't download again what an earlier Keep both kept: a numbered copy that is
    already the server's file counts as there (`already there as movie (2).mkv`).
  - Picks with the same name (`sample.txt` in two server folders, three `Season 1` folders) land
    under numbered names in pick order: `sample (2).txt`, `Season 1 (2)`.
  - `--limit` caps all connections together, exactly.
  - `--verify` (or the window's Verify downloads setting) checks each download against the
    server's MD5 afterwards: `checksum verified (MD5) in 0:02`. A copy that differs is deleted
    and grab exits with 6; the same command again downloads it again. One that can't be checked
    is kept: `not verified: <why>`. `--no-verify` skips the check for one run. With `-v`, each
    check shows as `+ exec md5sum -z <folder> (N files)`.
  - Without `--limit`, grab follows the window's [speed schedule](#gui), also mid-download:
    `grab: speed limited to 5 MiB/s until 24:00 by the speed schedule`, and a line at each
    change. `--limit RATE` replaces the schedule, and `--limit off` downloads without a limit.
  - Measured on a Storage Box: one 898 MiB file took 1:09 in byte ranges (`rclone cat`
    streams, before grab had its own client) and 1:50 with a plain `rclone copyto`, minutes
    apart.
- `--type`, `--min-size` and `--max-size` narrow the matching files before the pick list (and
  `--first` / `--all`): `grab "limits" --type video --min-size 50M`. The kinds are the window's
  (by extension: `.mkv` is video, `.r00` an archive). A file whose size isn't known doesn't
  pass a size bound. They pick files, so they don't go with `-f`; when nothing is left, grab
  says so: `grab: error: 12 files found, but none is video, at least 1.0 GiB` (exit code `3`).
- After `--` go connection flags for this run, checked once the server's engine is known and
  before anything connects or any folder is made; a flag that isn't taken stops the run (exit
  code `1`) saying what to use instead, never ignored.
  - On grab's own client: `--bind ADDRESS`, `--contimeout TIME` (`off` for none) and `--timeout
    TIME` (1 s to 24 h), which win over the server's `bind`, `connect_timeout` and `io_timeout` for every
    connection of the run; `--bind=` (blank) connects from any address. Anything else is refused
    with what to use: `-- --sftp-connections 4`: `max_connections` in grab.conf; `--bwlimit`,
    `--tpslimit`: `--limit`; `--transfers`: At once; `--dry-run`: `-n`; `--low-level-retries`:
    grab retries a failed piece by itself; chunk size, concurrency and multi-thread flags: grab
    reads 255 KiB at a time, 64 in flight, with nothing to set; a proxy, an ssh command, another
    SFTP subsystem or cipher flags: `engine = rclone` for that server.
  - On `engine = rclone`: rclone's connection flags (`--sftp-*`, `--bind`, `--contimeout`,
    `--timeout`, `--low-level-retries`, `--user-agent`, the certificate flags, `--tpslimit`,
    `--dscp`, `--password-command`), last on each rclone command line; `--bind` and
    `--contimeout` also reach ssh.exe as `-o BindAddress` and `-o ConnectTimeout`. Any other flag
    is refused, `-- --dry-run` with "grab's own -n". A proxy or ssh command flag there makes the
    run search by rclone's listing and turns Verify off for it, as such a setting in grab.conf
    does.
  - On both, the `--sftp-*` flags that would replace the server's address, login or host keys
    (`--sftp-host`, `-port`, `-user`, `-pass`, `-key-file`... `-known-hosts-file`, `-ssh`) are
    refused, naming the setting or command that changes it.
- `--rclone`, the plain rclone copy of earlier versions, is gone: it stops before anything
  runs (exit code `1`) with "--rclone is gone: grab no longer runs a plain rclone copy. To
  download from a server through rclone until a later grab removes it, set engine = rclone in
  [Box] (grab config)".
- `-v` says how grab connects and searches ("Main through grab's own SSH client", "the file list
  of Main comes with find (GNU find, checked on 2026-11-03)"), each SFTP read and command it
  sends (`+ sftp read ... --offset 0 --count 1048576`, `+ exec md5sum -z ...`), the server's
  [notes about settings](#notes-about-settings) again, and on `engine = rclone` the rclone and
  ssh command lines before they run.

Exit codes: `0` ok, `1` usage (also a flag after `--` that isn't taken, and `--rclone`), `2`
config, `3` target not found or pick aborted, `4` remote lookup failed (search or file list),
`5` `grab update` failed, `6` a download failed, `130` stopped by Ctrl+C.

## Lookup methods

Per server, `find =` in grab.conf picks how `TARGET` is located, and how its
[file list](#file-lists) is made:

| `find`               | how                                                                  |
|----------------------|----------------------------------------------------------------------|
| `auto` (the default) | find where the find check says the server has GNU find, else the walk; a Storage Box always the walk |
| `ssh`                | find only                                                            |
| `walk` (or `rclone`) | the walk                                                             |

- **find:** one command over an SSH connection grab already holds:
  `env LC_ALL=C find -L ROOTS -mindepth 1 -maxdepth N -iname '*WORD*' ... -printf ...`. A single
  round trip, filtered on the server; it needs a shell and GNU find there.
- **The walk:** grab lists the folders over SFTP itself, several folders in flight per
  connection and up to 8 connections, and filters on this PC. It needs no shell, which makes it
  the way for a Hetzner Storage Box, which doesn't run arbitrary commands. Links are followed (a
  link to a folder only when it leads outside the tree, and once). A folder the server won't
  list is reported, and the rest is searched.
- **The find check** (`auto`): a short command whose exact answer shows that the server's find
  works and that nothing (text a shell's start-up files print) gets into its output. The answer
  is kept a week per login in `limits.json`, beside gui.json, for the window and the command;
  Test asks again. A find that fails because of find itself sends the server to the walk too.
  Storage Boxes (`*.your-storagebox.de`) aren't asked: they have no find.

On a Storage Box use `search_roots = /home` or leave it blank for the home directory; `/`
itself is not listable there. `search_roots` entries may be absolute (`/srv`) or relative to
the login home (`learning`); blank or `.` means the home directory.

**On `engine = rclone`** (the fallback) the methods are 0.19's: `auto` is ssh + find (`ssh
user@host "find ROOTS -maxdepth N -iname '*WORD*' ..."` through ssh.exe) when the server logs
in with a key file or ssh-agent, else rclone's listing (`rclone lsf REMOTE:ROOT -R --max-depth
N`, filtered locally); `ssh` is ssh + find; `walk` and `rclone` are rclone's listing. ssh.exe
gets grab's known_hosts and the server's key file from grab.conf, never your own ssh config's
host keys, and asks for a key's passphrase unless the key is in ssh-agent. A server that goes
through a proxy or an ssh command is always searched by rclone's listing (ssh.exe wouldn't take
that way).

## File lists

A Storage Box has no shell, so every search there walks its folders over SFTP: 8–18 s on the
test boxes. Instead, grab keeps a list of each server's files and folders and searches that
on your PC in milliseconds, with the same matching and ranking as a live search.

| Server (depth 4) | live search | list size | fetching the list |
|------------------|-------------|-----------|-------------------|
| Storage Box      | 18.2 s      | 6,329 entries, 0.6 MB | 18 s |
| Storage Box sub-account | 8.4 s | 4,681 entries, 0.4 MB | 8 s |
| Server with ssh  | 1.2 s       | 17,387 entries, 1.9 MB | 2–4 s |

(Measured through rclone's listing and ssh + find. grab's own walk fetched the sub-account's
list in 3.5 s where rclone took 9.6 s.)

- A list covers what a search looks at: the search roots down to `max_depth`, dot-names left
  out as searches leave them out. It is fetched in one go, with find or the walk (see
  [Lookup methods](#lookup-methods); on `engine = rclone` ssh + find or `rclone lsf -R`).
  Lists are kept per engine: after a switch between grab's client and rclone the list is
  fetched again once.
- It is fresh for 15 minutes. The `grab` command then fetches a new one before searching;
  the window answers from the old one at once and refreshes it in the background.
- When fetching a list fails, the window doesn't try again by itself for a minute, then 2, 4,
  8 and at most 15 minutes after each further failure; the Refresh button tries at once.
- Walking a server opens up to 8 connections at once. In the window they come out
  of the server's connection budget, as downloads' do: all 8 when nothing else runs, a share
  while downloads run (they hand theirs back within moments). A search for files and folders
  walks once.
- Changing a server's address, login, search roots or depth makes its list stale; removing a
  server deletes its list.
- A path target, a search for a dot-name, and `--depth` other than the configured one search
  the server live, as before.
- Lists carry each entry's date. Lists saved by grab 0.10.0 or earlier have none and are
  fetched again once: the window answers from them at once and fills in the dates after its
  background refresh, the command fetches first. An older grab fetches its own list again
  rather than misread a newer one.
- The lists are in `%LOCALAPPDATA%\grab\lists`, one `<server>.list` each, kept on this PC only.
  They are encrypted for your Windows account (DPAPI), so other accounts, backups and anyone
  with only a copy of the files can't read the file names. Programs running as you can, as
  grab does, without a password. Saving or loading a 2 MB list takes 15–21 ms.
  - A list that can't be decrypted (another account's, damaged, or after an admin reset your
    Windows password) is fetched again.
  - Delete the folder to remove them.

## Connections

Per server in grab.conf (Open grab.conf in Settings, or `grab config`). The window has no fields
for `bind` and the times; its server card and `grab server list` show the ones that are set.

| key               | default | what |
|-------------------|---------|------|
| `max_connections` | blank = automatic: 14 on Storage Boxes, 12 elsewhere | how many connections downloads (and walks) may hold to the server at once, all downloads together; 1–64. Also in Settings → Edit. Automatic is lowered when the server refuses connections and raised again by clean downloads |
| `bind`            | blank = any | the local address to connect from (one of this PC's addresses, to pick a network) |
| `connect_timeout` | `30s`; `off` = as long as TCP tries | how long a new connection may take to reach the server |
| `io_timeout`      | `30s`; `1s` to `24h` | the longest single wait after that: the handshake, the login, each read or listing. A download's piece that waits longer fails and is tried again |

Times are written as `30s`, `1m30s`, `500ms` or `2h`. A bad value stops grab before it does
anything: `[Main] io_timeout: not a time grab can read (such as 30s or 1m30s; 1s to 24h)`.
`io_timeout` has no `off`: a read that waits for ever couldn't be cancelled. A run's `-- --bind`,
`--contimeout` and `--timeout` win over the three keys (see [Usage](#usage)).

The keys reach every connection to the server: grab's own client, the host key fetch of Trust,
Add and Edit (which keeps its own 10 s limit for the handshake), and on `engine = rclone` every
rclone run (`--bind`, `--contimeout`, `--timeout`) and ssh.exe (`-o BindAddress`, `-o
ConnectTimeout` in whole seconds, rounded up). A key left blank adds nothing there, so rclone
keeps its own times. On every command line the order is the same: `common_flags`' connection
flags, grab's fixed values, the keys, the run's flags after `--`; ssh.exe gets grab's options
before `ssh_options` (it takes the first value it sees, rclone the last).

The rest is grab's, with nothing to set:
- **Reads:** 255 KiB at a time (the largest SFTP packet OpenSSH accepts), up to 64 in flight per
  connection, each for exactly the bytes a range still needs. Under a speed limit, one at a time
  and smaller, so the limit holds.
- **Files at once:** "At once" in the window's Downloads bar (1–8, default 4), which the command
  follows too; a big file's pieces spread over the server's connections, small files of a folder
  go beside them (see [GUI](#gui)).
- **Speed:** Limit speed and the speed schedule in the window, `--limit` for a command.
- **Staying connected:** connections stay open between files, searches and downloads, and close
  by themselves after 15 to 60 seconds without use (Play keeps its own up to five minutes, see
  Previews). A command the server works on quietly (find, md5sum over a big folder) gets a
  keepalive every 15 s, and the link counts as lost only after 45 s without an answer.
- **Retries:** a piece that fails is tried again; a server that refuses connections gets fewer
  (see Connections per server, under [GUI](#gui)).

**On `engine = rclone`** (the fallback) these keys feed rclone and ssh.exe as well:

| key            | default | what still reaches rclone |
|----------------|---------|---------------------------|
| `common_flags` | `-P --sftp-chunk-size 255Ki --sftp-disable-hashcheck` | its connection flags, on every rclone run: 255 KiB reads (~8x fewer round trips than rclone's 32 KiB), and no `md5sum` after each of rclone's own copies (10–30% faster). Its `--bind`, `--contimeout` and `--timeout` also reach grab's own client while `bind`, `connect_timeout` and `io_timeout` are blank, on any engine |
| `folder_flags` | `--transfers 8 --checkers 8` | its connection flags, on the folder batch only; the batch gets its `--transfers` from grab (the connections it may use) |
| `file_flags`   | `--multi-thread-streams 8 --multi-thread-cutoff 64Mi --multi-thread-chunk-size 64Mi` | nothing: it was for `--rclone`, which is gone |
| `ssh_options`  | blank | ssh.exe's options, for ssh + find and Test's ssh line |

Connection flags are `--sftp-*`, `--bind`, `--contimeout`, `--timeout`, `--low-level-retries`,
`--user-agent`, the certificate flags, `--tpslimit`, `--dscp` and `--password-command`. Other
flags reach no run: grab's rclone runs depend on rclone's exact output, so a progress, logging,
filter or comparison flag would break them (`-vP` failed every download, `--size-only` made
verification compare sizes only); a changed one gets a [note](#notes-about-settings). The
`--sftp-*` flags that would replace the server itself (`--sftp-host`, `-port`, `-user`, `-pass`,
`-ask-password`, `-key-pem`, `-key-file`, `-key-file-pass`, `-pubkey`, `-pubkey-file`,
`-key-use-agent`, `-known-hosts-file`, `-ssh`) are refused, in `common_flags` and after `--`.
Likewise rclone runs see no `RCLONE_*` environment variable but `RCLONE_PASSWORD_COMMAND` and the
`RCLONE_SFTP_*` ones that don't set the server's address, login or host keys: an
`RCLONE_PROGRESS=true` meant for interactive rclone would put progress text into the bytes grab
reads. Each rclone run gets its one server's details from grab alone, in its own environment,
with rclone's config in memory (`--config NUL`).

`--sftp-disable-hashcheck` skips the `md5sum` rclone runs on the server after each of its own
copies (the small-file batch of an `engine = rclone` server); SSH already protects the bytes in
flight. It doesn't touch grab's own check: to compare downloads with the server's copy end to
end, turn on Verify downloads (or `--verify`), whatever `common_flags` say.

## Layout

```
src/cli.*       argument parsing            src/remote.*   find command, ssh argv, match picking
src/ini.*       INI reader (grab + rclone)  src/listing.*  rclone lsf lookup, glob matching
src/config.*    grab.conf                   src/rclone.*   rclone copy argv (engine = rclone)
src/quote.*     Windows + sh quoting        src/process.*  CreateProcess / posix_spawn wrappers
src/util.*      strings, paths, environment src/main.cpp   the five steps
src/engine.*    search + download for both  src/servers.*  server argv, host keys, conf edits
src/server_ops.* add/edit/trust/remove/test src/update.*   `grab update`, the window's check
src/server_cli.cpp `grab server ...`: the questions, list and test lines
src/server_conf.* a server's grab.conf keys (host, port, user, auth...) and the checks they pass
src/passwords.* grab's password store: DPAPI, each value bound to its user@host:port
src/rclone_conf.* rclone.conf read as rclone reads it (for the import only)
src/rclone_import.* the one-time move of 0.19's servers from rclone.conf into grab.conf
src/rclone_flags.* which rclone flags reach rclone runs (engine = rclone); what to use instead after --
src/guard.*     settings grab's own client can't serve (a proxy, an ssh command...): kept on rclone, why
src/notices.*   the one-time notes about settings (notices.json)
src/fetch.*     resumable ranged downloads  src/conn_budget.* per-server connection sharing
src/folder.*    folder listing, rclone batch lines, which files the tree shows
src/folder_job.*    a folder download, shared by the window and the command
src/cli_download.*  the command's downloads: live progress line, Ctrl+C
src/progress_line.* that line's text
src/rate.*      speed limit: token bucket, --limit / --bwlimit rates
src/schedule.*  speed schedule: rules, the limit at a time of the week, its next change
src/verify.*    checking downloads: the server's md5sum and grab's MD5, by path; what proves a mismatch
src/play_stream.* Play: the player's local stream over grab's connections (warm start, cache, seeks)
src/gui/play_server.* the player, started without rclone's variables
src/ssh_read.*  one file read over pooled connections (previews); src/preview_jobs.* the viewer's fetches and cache
src/local_name.* server names as safe Windows names (rclone's encoding, device names)
src/rclone_encoding.* rclone's own name encoding; local paths as rclone must be told them
src/existing.*  a different file already there: replace, keep both ("name (2).ext") or skip
src/file_type.* file kinds by extension (as the window's icons and filters), --type/--min-size
src/browse.*    browse mode: a server's folders from its file list, deeper ones live
src/file_list.* saved file lists: fetch, search, freshness, the .list files
src/secrets.*   0.19's 'secrets' store, read by the import; rclone's obscured form
src/ui_origin.* what the window may show and whose messages count (exact addresses)
src/ssh.*       grab's own SSH connections (libssh2): host keys, login, SFTP channels, commands
src/sftp.*      grab's own SFTP client: pipelined reads with exact bounds, listings, stat
src/ssh_pool.*  a server's connections kept open and lent to downloads, searches and Browse
src/ssh_target.* a server's grab.conf section and saved password as an SSH login
src/ssh_walk.*  a server's folder tree over SFTP: several folders in flight per connection
src/ssh_find.*  searches and lists with the server's GNU find over a command; the find check
src/ssh_hash.*  the server's MD5 for verification: md5sum over a command, batched; the hash check
cmake/vcpkg*    OpenSSL from vcpkg; cmake/vcpkg-ports/openssl pins 3.5 LTS
src/gui/        grab-gui: WebView2 host, tray, settings backend, ui/ (HTML, CSS, JS)
installer/      Inno Setup script           tools/         make_icon.py (src/gui/grab.ico)
tests/          doctest unit tests (fetched by CMake)
```

## Releasing

GitHub Actions ([build.yml](../.github/workflows/build.yml)) builds and tests every push and pull
request with MSVC and clang-cl. Pushing a version tag additionally runs the `dist` target, which
packs the portable zip and compiles the Inno Setup installer from
[installer/grab.iss.in](../installer/grab.iss.in), writes `SHA256SUMS.txt` for both, and attaches
all three to a GitHub Release. `grab update` depends on that checksum file.

1. Bump `project(grab VERSION x.y.z)` in `CMakeLists.txt` and commit.
2. `git tag vx.y.z` and `git push origin master --tags`.

The workflow refuses a tag that doesn't match the project version. Locally,
`cmake --build --preset clang-cl-release --target dist` always produces the zip and also the
installer when Inno Setup 6 is installed (`winget install JRSoftware.InnoSetup`).

## Third-party software

- **[rclone](https://rclone.org)** v1.75.1, © Nick Craig-Wood, MIT license
  ([licenses/rclone.txt](../licenses/rclone.txt)). `rclone.exe` ships next to `grab.exe`
  (81 MB, which is why the installer is about 22 MB) for the servers on the fallback (`engine =
  rclone`, or kept there by their settings) until a later grab removes it; nothing else runs
  it. grab uses it rather than any rclone on PATH (only when it is missing does grab look on
  PATH). Set `rclone = <path>` in `[grab]` to use another copy. Another copy must be 1.67 or
  newer (folder downloads use `--sftp-connections`); grab checks once, where it runs rclone,
  and says so when it's older. For dates in listings it must also understand `--time-format
  unix` (1.72 and newer; older ones show rows without dates).
  - **Pin:** the build downloads the official Windows zip and checks it against the pinned
    SHA-256, which is rclone's own published `SHA256SUMS` value.
  - **To bump:** change `GRAB_RCLONE_VERSION` and the hash in `CMakeLists.txt`, then rebuild.
    `grab update` delivers the new rclone together with grab.
- **[libssh2](https://libssh2.org)**, BSD-3-Clause ([licenses/libssh2.txt](../licenses/libssh2.txt)),
  and **[OpenSSL](https://openssl-library.org)** 3.5 LTS, Apache-2.0
  ([licenses/openssl.txt](../licenses/openssl.txt)): built into grab, statically: grab's own SSH
  client, which downloads, searches, lists, browses, verifies, previews and plays.
  - **Pins:** libssh2 is a master commit (it has made no release since 1.11.1 in 2024 and its
    security fixes land on master), checked by SHA-256 in `CMakeLists.txt`. OpenSSL comes from
    vcpkg through grab's copy of its port, `cmake/vcpkg-ports/openssl` (vcpkg itself has moved
    on to 3.6), checked by SHA-512 and built without automatic loading of `openssl.cnf`.
  - **To bump:** see the comments by the pins (`CMakeLists.txt`,
    `cmake/vcpkg-ports/openssl/portfile.cmake`). Both are checked at every grab release.

## License

MIT, see [LICENSE](../LICENSE).
