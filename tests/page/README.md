# Page-logic tests

`test.js` checks the window's page (`src/gui/ui`: `index.html`, `app.js`, `app.css`) on its own,
with no grab-gui and no server: the Downloads table (fixed columns, sorting, column widths, the
keys and the treegrid ARIA), every file of a folder download, the filter and Go to running, a
file's priority in its folder, the status bar's total speed and its graph, Waiting files, opening
finished files (double-click, Open, Ctrl+Enter, the question before a program runs), and timing
budgets for 10,000 and 177,191 files. It stubs what the host would send and records what
the page sends back.

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
