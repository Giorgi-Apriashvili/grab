# Page-logic tests

`test.js` checks the window's page (`src/gui/ui`: `index.html`, `app.js`, `app.css`) on its own,
with no grab-gui and no server: the Downloads table (fixed columns, sorting, column widths, the
keys and the treegrid ARIA), every file of a folder download, the filter and Go to running, a
file's priority in its folder, the status bar's total speeds (↓ and ↑) and their graph, the two
speed limits' MiB/s boxes (each: typing sets nothing; Enter or leaving it sets, confirmed by the
backend's answer: the pending look, the badge; the two apart, and fitting the narrowest window),
the speed schedule's ↓ and ↑ speeds, Waiting files, opening finished files (double-click, Open, Ctrl+Enter,
the question before a program runs), resizing Browse and Downloads (the grip on the panel's top
edge and the head's free space: drags, clicks, keys, double-clicks, limits, nothing in the head
moving), the uploads in the window (their ↑ rows, the Show filter's Downloads and Uploads, the
head's count, every row action on an upload row, Browse's upload entries and their refusals, the
confirm dialog's states), drops from Explorer (no drag ever left to the browser; the overlay over
Browse, the folder row it targets, the refusals and where to drop instead; the overlay going with
a drop, the drag leaving or cancelled, Escape and dragovers stopping; a drop's Files posted beside
uploadDrop, never a file WebView2 couldn't get; 2,000 files at once; the page opened again by the
window), Settings → Appearance (its radio group's keys and ARIA, setTheme, the backend's word),
Browse's changes on a server, Quick access's pinned folders (the button and its list per server and
under All servers, its keys and ARIA, Pin and Unpin in every menu, Ctrl+D and Alt+1-9, Undo,
Rename, reordering by keys and by a drag kept apart from Explorer's drops, the pins in the address
field's list, a pin that didn't open with Unpin, the Delete and Remove dialogs' words, 100 pins),
the address field's error shown while it is open, and timing budgets for 10,000 and 177,191 files. It stubs what the host would send and records what the page sends back.

`run_page_test.ps1` copies the page into a scratch folder with `test.js` loaded after `app.js`
(and the Content-Security-Policy meta removed: its `script-src 'self'` does not cover a `file://`
page), opens it in a headless Microsoft Edge with a profile of its own on a free DevTools port,
waits for the results, closes that Edge and prints the failures, the `INFO` lines and one summary
line. Nothing is built.

## Run

```powershell
pwsh -File tests/page/run_page_test.ps1           # from anywhere; paths are the script's own
ctest --preset clang-cl-debug -L page             # the same through CTest (a configured build)
```

- `-Work <folder>`: the scratch folder, kept afterwards. Default: a new folder in the temp folder,
  removed afterwards.
- `-Keep`: keep that default folder: the page and two screenshots of the final state,
  `shot-1250.png` and `shot-716.png` (taken whenever the folder is kept).
- `-Full`: print every check, not only the failures and the `INFO` lines.
- `-Edge <path>`: the `msedge.exe` to use.

Exit code: 0 every check passed; 1 a check failed, `test.js` threw or gave no result within
180 s; 77 skipped, Edge not found. CTest reports 77 as skipped (`ctest -L page -V` shows why), and
`ctest -LE page` runs everything else. CI runs the script directly in the clang-cl job, after the
unit tests, so a runner without Edge fails there.

## Needs

- **Microsoft Edge**: `msedge.exe` under `Microsoft\Edge\Application` in Program Files (x86) or
  Program Files, or registered under App Paths. The WebView2 Runtime alone is not enough.
- **PowerShell 7** (`pwsh`). CTest looks for it when the build is configured and skips the test
  without it.

## Adding checks

`check(name, got, want)` compares JSON forms. When `test.js` is done it writes a
`<pre id="test-out">`: the first line `ALL PASS (0)` or `FAILED (<count>)`, then a `PASS`, `FAIL`,
`INFO` or `EXCEPTION` line per entry. The page's functions are globals of `app.js`, so `test.js`
calls them directly.
