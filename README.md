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

## How it works

```
grab -f releases E:\Backup
  ├─ 1. parse args      mode=folder, target="releases", dest="E:\Backup"
  ├─ 2. load config     grab.conf  ->  which rclone remote, where to search, which flags
  │                     rclone.conf -> host / user / port / key for the ssh step
  ├─ 3. resolve remote  ssh user@host "find ROOTS -maxdepth N -name 'releases' -type d -printf '%T@\t%p\0'"
  │                     (or `rclone lsf -R` for remotes without a key / shell, see Lookup methods)
  │                     0 hits -> exit 3   1 hit -> go   N hits -> numbered pick, several allowed
  ├─ 4. download        file:   byte ranges over up to 14 connections, `rclone cat --offset N
  │                             --count M` each, into movie.mkv.grabpart (progress in
  │                             movie.mkv.grabpart.json); renamed when complete
  │                     folder: listed once (rclone lsjson -R); files of 64 MiB and more as
  │                             above, the rest in one `rclone copy --files-from-raw` batch
  └─ 5. progress        one live line; Ctrl+C stops and keeps the partial files for next time
```

`--rclone` replaces steps 4 and 5 with a plain `rclone copyto` / `rclone copy` using
`file_flags` / `folder_flags`, with rclone's own `-P` progress; Ctrl+C then goes to rclone.

With a key loaded in ssh-agent the ssh step never prompts; otherwise it asks for the key
passphrase once per run. The rclone step never prompts: grab hands rclone the password or
passphrase from its encrypted store (see [Servers](#servers)).

## Requirements

- Windows 10/11 with the built-in OpenSSH client (`ssh.exe`), or any OS with `ssh` on PATH.
- Nothing else: [rclone](https://rclone.org) ships inside grab (see *Third-party software*),
  and servers are set up with `grab server add`. You never need to run `rclone config`.
- A private key used for ssh must have an ACL restricted to your user (OpenSSH refuses
  world-readable keys).

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
    from the server (`rclone lsjson`, about 2 s on a Storage Box) and remembered while the
    window is open.
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
  bar: "Listing movies on Box.Guest… 12 items so far", "Searching Main with ssh + find… 37
  found so far", "Starting a stream from Box.Guest", "Loading cover.jpg: 3.2 of 8.0 MiB",
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
      video plus 8 MiB, and its last 4 MiB) is fetched first, over up to 6 connections at once
      from the server's budget, and the player starts once it is in: it then opens at once.
      The status bar shows "Preparing <name>: 18 of 38 MiB". This matters on a Storage Box,
      where each read starts slowly: an anime release with 28 MB of subtitle fonts before its
      first frame opened in ~12 s instead of 20–28 s (MPC-HC gave up after ~10 s), and an AVI
      in 8 s instead of 29 s.
    - The player gets grab's own address (a small server on this PC only: 127.0.0.1, a random
      port and secret path); everything else comes from a read-only `rclone serve http` for the
      server, started meanwhile and stopped with the window. A file's warm part stays in memory
      while it is played (two files at most, five minutes after their last read).
    - A seek outside the warm part takes about 1 s on a fast server and 2–8 s on a Storage Box.
      A Storage Box streams at about 2.5 MB/s (its cap per connection): plenty for a 1080p
      release, not for a big remux.
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
  - Play's streams come from `rclone serve http`, which reads every folder on the way to a file, so
    grab serves a file from the search root it is under (or the login home, or else its own
    folder), never from a `/` the server may not let you list (a Storage Box's). When a folder on
    the way can't be listed all the same, it serves the file from its own folder; when that
    can't be listed either, the viewer says so ("Main doesn't let you list /srv/private.").
    At most 8 such streams run at once.
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
    the window and its notifications. grab fetches such renamed files itself and checks them by
    their MD5 sums. rclone encodes every local path it's given once more, so
    grab tells it each folder and file name in the form that comes back as grab's: a folder like
    `Mission: Impossible (1996)` lands in one `Mission： Impossible (1996)`, also when the Save-to
    folder's own name has such characters. A folder whose name rclone can't be told (a `‛` in the
    Save-to folder's name) is fetched by grab's own engine, and checked by its MD5 sums.
  - **Names that are one on Windows** in a server folder (`README` and `readme`, folders `Docs`
    and `docs`) all arrive: the later file gets a number (`readme (2)`), which the download's
    file list and the command say. rclone would copy one of them and drop the others (a folder
    with all it holds), so grab fetches these itself and checks them by their MD5 sums; the same
    goes for a name spelled two ways in Unicode (`café`), which Windows keeps apart. Names with a
    line break arrive too (`report␊notes.txt`). A server name that isn't UTF-8 (an old Latin-1
    name) can't be given to rclone: such a file fails saying so; rename it on the server.
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
  files download at the same time, all downloads together. Each file of a folder counts: at 1 a
  folder goes file by file; at 4 it runs up to 4 of its files, fewer while other downloads run.
  A file then gets more of the server's connections, so fewer at once costs no speed. The
  grab command downloads a folder's files as many at a time as the window's At once. Each
  download can be paused, resumed or cancelled, or all together with Pause all / Resume all and
  Cancel all.
- **Resumable file downloads:** grab fetches a file in byte ranges, several `rclone cat`
  streams at once, into `<name>.grabpart`. It records each range's progress in
  `<name>.grabpart.json` and renames the file when it is complete.
  - Pausing keeps both; resuming continues from the last saved bytes.
  - grab reuses or deletes only partial files it made: their `.json` record names the server
    and its file (the same path on another server is another file). When something else
    already has those names (your own files, or files of the folder
    being downloaded), it uses `<name>.2.grabpart` and so on, and leaves them alone. Cancelling
    deletes only grab's partial files, and of rclone's temporary `.partial` files only the ones
    its stopped copy had just made.
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
  - Changes apply at once. Streams through grab are paced by a shared token bucket, and while
    limited, rclone is told not to read ahead, so the network really follows the limit.
  - A folder's small-file batch gets rclone's `--bwlimit` and counts against the same total.
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
  - At each change, a folder's small-file batch starts again with the new cap; the files it
    had in flight start over. Other downloads keep their connections and just follow the new
    speed; they reconnect only when the limit switches on or off.
- **Folders** are listed once and shown as a tree under their row, collapsible with ▸/▾.
  - Finished files fold into a "N done" line, and a long queue shows its next 20 files.
  - Each file can be paused, resumed or skipped. The folder's own buttons act on all of its
    files.
  - Files of 64 MiB and more take the resumable path above, as many at a time as At once allows.
  - Smaller files go in one `rclone copy` batch that reuses its connections, since a new SSH
    login per small file would dominate; it copies as many at a time as At once allows. Pausing drops the progress of small files that were
    in flight.
  - The batch's connections follow its share: when another download from the same server
    starts, it starts again with fewer within a few seconds, and with more once they're free
    again (the small files in flight start over).
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
  - Play's player uses its own `rclone serve`, outside the budget, with at most two
    connections. Previews take one connection of the budget, ahead of downloads, and only free
    ones beyond it.
  - Connections open slightly staggered. If a server refuses one while others of grab's get
    data, grab lowers that server's budget and remembers it (in `limits.json` next to
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
    and folder listings that walk the server over rclone take their connections from the same
    budget (see [File lists](#file-lists)).

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
  and writes nothing until you trust them. A saved password stays with the host and user it
  was typed for: changing either asks for it again. "Open grab.conf" covers the rarer settings
  (rclone flags, ssh options, find method).
- **The window's page** is grab's own and nothing else: it only shows https://grab.ui/index.html,
  only that page's messages reach grab, and files dragged in from outside are ignored.
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
  it has; grab reads them itself, without logging in) and saves them to
  `%APPDATA%\grab\known_hosts` once you confirm, in place of any it held for that address. From
  then on grab, ssh and rclone refuse a server that presents a different key. Changing a
  server's host or port, or removing it, takes its keys out again unless another server uses the
  same host and port. A server that offers only old key types (SHA-1 `ssh-rsa`, DSA) or old key
  exchanges can't be added.
- **writes both config files:** the connection goes into grab's own
  `%APPDATA%\grab\rclone.conf`, the search settings into grab.conf.
- **tests the connection:** one check through rclone and, for key or agent logins, one
  through ssh; with `engine = grab`, a login through grab's own client instead of ssh, which
  also says whether searches will use find or list folders.

Passwords and passphrases are never shown and never put on a command line. They are not kept
in rclone.conf either, where rclone would store them only "obscured", which anyone who can read
the file can reverse.
- grab keeps them in `%APPDATA%\grab\secrets`, encrypted for your Windows account (DPAPI). Other
  accounts, backups and copies of the file can't read them; programs running as you can, as
  grab does, without asking you for anything.
- When grab runs rclone, it hands them over as environment variables
  (`RCLONE_CONFIG_<SERVER>_PASS`, which rclone reads like its config; a server called `my-box`
  keeps its dash: `RCLONE_CONFIG_MY-BOX_PASS`), never on disk unencrypted or on a command line.
- A grab.conf `rclone_config =` that points at your own rclone.conf is used as it is: its
  passwords stay there.
- If the store can't be read (another Windows account, a damaged file, or a Windows password an
  admin reset), servers with passwords say so: enter the password again in Settings → Edit, or
  remove and add the server again. Servers without passwords keep working.
  - That first change keeps the old store as `secrets.unreadable-<date>-<time>` (it may become
    readable again, e.g. once a domain controller is reachable after a reset) and names the
    other servers whose passwords need entering again.

**Upgrading from 0.7.0 or earlier:** the first run moves the passwords out of grab's rclone.conf
into the encrypted store by itself. They leave the file only after the store holds them. The
standard `%APPDATA%\rclone\rclone.conf`, which grab only reads, is never changed, so remotes you
keep there still hold obscured passwords.

The commands also take answers from piped input, one per line, so they can be scripted. When
the input runs out, the command stops without changing anything.

Password servers are searched with rclone. Key and ssh-agent servers are searched with ssh +
find, which is faster; with a passphrase-protected key that needs the key in ssh-agent
(`ssh-add <key file>`). With `engine = grab` every server is searched with find where it has
GNU find (grab checks once a week and remembers the answer in `limits.json`; Test checks again),
else by listing its folders.

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
grab-gui then runs one window per configuration folder, and each configuration other than the
default keeps its own window settings, queue and recent searches in
`%APPDATA%\grab\configs\<folder>-<id>` and its file lists in
`%LOCALAPPDATA%\grab\lists\<folder>-<id>` (the command reads the same). grab's rclone.conf,
saved passwords and known_hosts are shared by all of them.
Add one `[section]` per server and choose with `-r NAME`.

**grab's own engine (preview).** grab is replacing rclone with its own SSH client. Put
`engine = grab` in a server's section to try it: files and folders then download through it, and
searches, file lists and Browse go through it too (Play and checks still go through rclone; previews use grab's client on every server).
A search or a list uses the server's GNU find when it has one and the `find` setting allows it,
else it lists folders over SFTP, several at a time; links are followed. It logs in once and
keeps the connections open between files, asks the server for exactly the bytes it needs, and
continues partial downloads rclone started (and the other way round). In a folder, files of 2 MiB
and more follow "At once" and spread over the connections; smaller ones go beside them on every
free connection, many at a time. It also downloads names that aren't UTF-8, and a subfolder the
server won't list is reported instead of failing the whole folder. Of `common_flags` it takes
`--bind`, `--contimeout` and `--timeout` (`-v` names the ones it leaves out). Test in Settings
logs in through it and says whether searches use find or list folders. `GRAB_ENGINE=grab` or
`rclone` in the environment overrides every server.

## Usage

```
grab [-s|--file | -f|--folder] TARGET [DEST] [options] [-- extra rclone args]
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
      --rclone         download with plain rclone copyto / copy and rclone's own progress
  -n, --dry-run        resolve the target and show what would be fetched (with --rclone: the
                       rclone command); transfers nothing and creates no folders
  -v, --verbose        echo the ssh and rclone command lines
      --init           write an example grab.conf
```

- Searches use the server's saved [file list](#file-lists) when it is under 15 minutes old,
  which makes them instant. Otherwise grab fetches a new list first ("fetching the file list
  of Box…") and saves it. `--fresh` always fetches.
- `TARGET` is a search: every word must appear in the name, ignoring case, in any order.
  `grab -s lioness E:\TV` lists every file with "lioness" in its name.
  `grab -s "lioness s03e08" E:\TV` narrows that to one episode, and so does
  `grab -s "the unravelling" E:\TV`. With the ssh lookup the filtering happens on the server
  (`find -iname`), so only hits travel back.
- A `TARGET` containing `*`, `?` or `[` is a glob instead, also case-insensitive:
  `grab -f 'Season.0[1-3]*' E:\Backup`. `--exact` matches the whole name, case-sensitive.
- Matches show their size and date in the list, folders their date:
  `[3] movies/x.avi  (897.9 MiB, 2023-10-15)`. The ssh lookup gets sizes and dates from GNU
  find's `-printf`, which any mainstream Linux server has; a server without it finds nothing
  over ssh, so use `find = rclone` for it.
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
  once with big files fetched the same way and small ones in one rclone batch. Picks run one
  after another, each with the whole budget.
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
    ask on, grab stops with exit code `6` and replaces nothing unless `--existing` is given. When
    the check can't reach the server (`--rclone`), a file there is asked about as one that
    couldn't be compared.
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
  - Measured on a Storage Box: one 898 MiB file took 1:09 this way and 1:50 with `--rclone`,
    minutes apart.
- `--type`, `--min-size` and `--max-size` narrow the matching files before the pick list (and
  `--first` / `--all`): `grab "limits" --type video --min-size 50M`. The kinds are the window's
  (by extension: `.mkv` is video, `.r00` an archive). A file whose size isn't known doesn't
  pass a size bound. They pick files, so they don't go with `-f`; when nothing is left, grab
  says so: `grab: error: 12 files found, but none is video, at least 1.0 GiB` (exit code `3`).
- `--rclone` downloads with plain `rclone copyto` / `rclone copy` instead, with rclone's own
  progress and `file_flags` / `folder_flags`; `--limit` becomes rclone's `--bwlimit`, and the
  speed schedule a weekly `--bwlimit` timetable ("Mon-18:00,5120K Tue-00:00,off ..."). With
  `--verify`, rclone compares checksums after each transfer itself. grab looks first and asks
  the same question before rclone would replace a file (another size, or a different time); a
  folder offers Replace or Skip, since rclone copies it under the server's names, and Skip runs
  rclone with `--ignore-existing`.
- After `--` go rclone flags. grab's own downloads take only connection flags there
  (`--sftp-*`, `--bind`, `--contimeout`, `--timeout`, certificates); any other one is refused
  before anything runs, with what to use instead (`-- --dry-run`: grab's own `-n`; `-- --bwlimit`:
  `--limit`, which also shares the speed across connections). With `--rclone`, every flag after
  `--` goes to the rclone copy.

Exit codes: `0` ok, `1` usage, `2` config, `3` target not found or pick aborted,
`4` remote lookup failed (ssh or rclone listing), `5` `grab update` failed, `6` a download
failed, `130` stopped by Ctrl+C. With `--rclone`, a failed download exits with rclone's own
code.

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

## File lists

A Storage Box has no shell, so every search there walks its folders over SFTP: 8–18 s on the
test boxes. Instead, grab keeps a list of each server's files and folders and searches that
on your PC in milliseconds, with the same matching and ranking as a live search.

| Server (depth 4) | live search | list size | fetching the list |
|------------------|-------------|-----------|-------------------|
| Storage Box      | 18.2 s      | 6,329 entries, 0.6 MB | 18 s |
| Storage Box sub-account | 8.4 s | 4,681 entries, 0.4 MB | 8 s |
| Server with ssh  | 1.2 s       | 17,387 entries, 1.9 MB | 2–4 s |

- A list covers what a search looks at: the search roots down to `max_depth`, dot-names left
  out as searches leave them out. It is fetched in one go (ssh + find, or `rclone lsf -R`).
- It is fresh for 15 minutes. The `grab` command then fetches a new one before searching;
  the window answers from the old one at once and refreshes it in the background.
- When fetching a list fails, the window doesn't try again by itself for a minute, then 2, 4,
  8 and at most 15 minutes after each further failure; the Refresh button tries at once.
- Walking a server over rclone opens up to 8 connections at once. In the window they come out
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

## Tuning

Defaults (override per remote in grab.conf):

| key            | default                                                                     | why |
|----------------|-----------------------------------------------------------------------------|-----|
| `common_flags` | `-P --sftp-chunk-size 255Ki --sftp-disable-hashcheck`                       | live progress; 255 KiB is the largest SFTP packet OpenSSH accepts, ~8x fewer round trips than the 32 KiB default; skipping the post-transfer server-side `md5sum` measured 10–30% faster |
| `folder_flags` | `--transfers 8 --checkers 8`                                                | more transfers hide per-file round trips on trees of small files; with rclone's default 4 streams that is ≤32 connections, which both test servers accepted without errors |
| `file_flags`   | `--multi-thread-streams 8 --multi-thread-cutoff 64Mi --multi-thread-chunk-size 64Mi` | one big object: parallel range reads over 8 connections. Keep chunks large: 8Mi measured ~2x slower than 64Mi because every chunk re-opens the file and re-ramps the SFTP pipeline |

All three are for `--rclone`, the plain rclone copy, which gets them as they are. grab's own
rclone calls (its downloads, listings, searches and Play) take only
their connection flags: `--sftp-*`, `--bind`, `--contimeout`, `--timeout`,
`--low-level-retries`, `--user-agent`, the certificate flags, `--tpslimit`, `--dscp` and
`--password-command`. Those calls depend on rclone's exact output, so a progress, logging,
filter or comparison flag there would break them (`-vP` failed every download, `--size-only`
made verification compare sizes only). Likewise they see no `RCLONE_*` environment variable but
`RCLONE_CONFIG*`, `RCLONE_PASSWORD_COMMAND` and `RCLONE_SFTP_*`: an `RCLONE_PROGRESS=true` meant
for interactive rclone would put progress text into the bytes grab reads. A folder's small-file
batch gets its `--transfers` from grab (the connections it may use), and grab's own engine
fetches a file in byte ranges over `max_connections` (blank = automatic: 14 on Storage Boxes,
12 elsewhere, lowered when a server refuses connections and raised again by clean downloads),
in the window and in the `grab` command alike.

With `--rclone`, concurrent SFTP connections ≈ `transfers × multi-thread-streams`
(+ checkers). If the server logs `MaxStartups` drops, lower one of them.
`--sftp-disable-hashcheck` skips the `md5sum` rclone runs on the server after each of its own
copies (`--rclone`, and the small-file batch of an engine = rclone server); SSH already protects
the bytes in flight. It doesn't touch grab's own check: to compare downloads with the server's
copy end to end, turn on Verify downloads (or `--verify`), whatever `common_flags` say.

## Layout

```
src/cli.*       argument parsing            src/remote.*   find command, ssh argv, match picking
src/ini.*       INI reader (grab + rclone)  src/listing.*  rclone lsf lookup, glob matching
src/config.*    grab.conf / rclone.conf     src/rclone.*   rclone argv for both modes
src/quote.*     Windows + sh quoting        src/process.*  CreateProcess / posix_spawn wrappers
src/util.*      strings, paths, environment src/main.cpp   the five steps
src/engine.*    search + download for both  src/servers.*  server argv, host keys, conf edits
src/server_ops.* add/edit/trust/remove/test src/update.*   `grab update`, the window's check
src/fetch.*     resumable ranged downloads  src/conn_budget.* per-server connection sharing
src/folder.*    folder listing, rclone batch lines, which files the tree shows
src/folder_job.*    a folder download, shared by the window and the command
src/cli_download.*  the command's downloads: live progress line, Ctrl+C
src/progress_line.* that line's text
src/rate.*      speed limit: token bucket, --limit / --bwlimit rates
src/schedule.*  speed schedule: rules, the limit at a time of the week, rclone's timetable
src/verify.*    checking downloads: the server's md5sum and grab's MD5, by path; what proves a mismatch
src/gui/play_server.* Play's warm start: the player's local stream (start and end from memory)
src/local_name.* server names as safe Windows names (rclone's encoding, device names)
src/rclone_encoding.* rclone's own name encoding; local paths as rclone must be told them
src/existing.*  a different file already there: replace, keep both ("name (2).ext") or skip
src/file_type.* file kinds by extension (as the window's icons and filters), --type/--min-size
src/browse.*    browse mode: a server's folders from its file list, deeper ones live
src/file_list.* saved file lists: fetch, search, freshness, the .list files
src/secrets.*   passwords kept out of rclone.conf: encrypted store, handed to rclone
src/ui_origin.* what the window may show and whose messages count (exact addresses)
src/ssh.*       grab's own SSH connections (libssh2): host keys, login, SFTP channels, commands
src/sftp.*      grab's own SFTP client: pipelined reads with exact bounds, listings, stat
src/ssh_pool.*  a server's connections kept open and lent to downloads, searches and Browse
src/ssh_target.* a server's rclone.conf section and stored secrets as an SSH login
src/ssh_walk.*  a server's folder tree over SFTP: several folders in flight per connection
src/ssh_find.*  searches and lists with the server's GNU find over a command; the find check
src/ssh_hash.*  the server's MD5 for verification: md5sum over a command, batched; the hash check
cmake/vcpkg*    OpenSSL from vcpkg; cmake/vcpkg-ports/openssl pins 3.5 LTS
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
  on PATH (only when it is missing does grab look on PATH). Set `rclone = <path>` in `[grab]`
  to use another copy. Another copy must be 1.67 or newer (folder downloads use
  `--sftp-connections`); grab checks once and says so when it's older. For dates in listings it
  must also understand `--time-format unix` (1.72 and newer; older ones show rows without dates).
  - **Pin:** the build downloads the official Windows zip and checks it against the pinned
    SHA-256, which is rclone's own published `SHA256SUMS` value.
  - **To bump:** change `GRAB_RCLONE_VERSION` and the hash in `CMakeLists.txt`, then rebuild.
    `grab update` delivers the new rclone together with grab.
- **[libssh2](https://libssh2.org)**, BSD-3-Clause ([licenses/libssh2.txt](licenses/libssh2.txt)),
  and **[OpenSSL](https://openssl-library.org)** 3.5 LTS, Apache-2.0
  ([licenses/openssl.txt](licenses/openssl.txt)): built into grab, statically, for the SSH client
  that is replacing rclone (in progress; not used by the programs yet).
  - **Pins:** libssh2 is a master commit (it has made no release since 1.11.1 in 2024 and its
    security fixes land on master), checked by SHA-256 in `CMakeLists.txt`. OpenSSL comes from
    vcpkg through grab's copy of its port, `cmake/vcpkg-ports/openssl` (vcpkg itself has moved
    on to 3.6), checked by SHA-512 and built without automatic loading of `openssl.cnf`.
  - **To bump:** see the comments by the pins (`CMakeLists.txt`,
    `cmake/vcpkg-ports/openssl/portfile.cmake`). Both are checked at every grab release.

## License

MIT, see [LICENSE](LICENSE).
