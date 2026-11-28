'use strict';
// grab-gui front end. Talks to the C++ backend (src/gui/app.cpp) with JSON messages:
//   to backend:   init, prefs, search, cancelSearch, refreshLists, browse, suggest, pickFolder, enqueue, cancelItem,
//                 pauseItem, resumeItem, retryItem, pauseAll, resumeAll, cancelAll,
//                 setParallel, setLimit, setSchedule, setVerify, moveItem, setPriority, clearFinished, openFolder, copyText,
//                 watchFiles, unwatchFiles, fileAction, setFilePriority, showFile, openFile, openDir, setExpanded,
//                 resolveExisting, play, previewCancel, uploadPick, uploadDrop, uploadCheckCancel, uploadStart,
//                 setUploadExisting, setTheme,
//                 Browse's changes on servers: filesAccount, filesRename, filesMkdir, filesLook, filesPaste,
//                 filesSize, filesDelete, filesProps, filesHash, filesCancel,
//                 Quick access: pinsAdd, pinsRemove, pinsRename, pinsMove,
//                 updates: updateCheckNow, setUpdateCheck, updateStart, updateRestart, updateDismiss, openReleasePage,
//                 settings: servers, serverSave, serverConfirm, serverCancel, serverTrust,
//                 serverTest, serverRemove, serverDefault, pickFile, openConfig
//   from backend: init, searchResult, searchError, browseResult, suggestions, activity, folderPicked, queue, queueDelta,
//                 files, fileDelta, speed, openAsk, downloadsNote, error, update, spaceShort, existing,
//                 updateChecked, limit, played, uploadCheck, uploadExisting, theme, pageRestored (from the window itself),
//                 filesAccount, filesDone, filesProgress, pins,
//                 settings: servers, settingsBusy, hostKeys, serverSaved, serverTested,
//                 serverError, filePicked, importBanner

const $ = (sel) => document.querySelector(sel);
const ROW = 32; // keep in sync with --row in app.css

const state = {
  remotes: [],
  remote: '',
  destinations: {},
  defaultDest: '',
  hits: [],
  selected: new Set(),
  cursor: -1,
  anchor: -1,
  // The rows' order: key '' (the default), name, size or modified. A Size sort's `then` is the
  // sort before it, which orders the folders (they have no size); null for their default order.
  sort: { key: '', dir: 'asc', then: null },
  // What the list shows: file types (icon kinds, 'folder', 'file' for others) and size bounds in
  // bytes (null: none), with the size fields' text. For the window's run; a recent search
  // brings its own.
  filters: { kinds: new Set(), min: null, max: null, minText: '', maxText: '' },
  results: [],           // the search's hits before the filters
  filterTotal: 0,        // the rows before the filters (after the words that filter a folder)
  recent: [],            // recent searches, newest first (the backend keeps them sealed)
  recentLoaded: false,
  recentSel: -1,
  pendingSearch: null,   // a recent search, run once its folder is open
  searchId: 0,
  searching: false,
  searchStarted: 0,
  lastQuery: '',
  queue: [],
  parallel: 4,
  totalSpeed: 0,         // what all downloads receive now, bytes/s (the backend's 'speed')
  upSpeed: 0,            // what the servers acknowledged of all uploads now (its 'up')
  // The two limits that apply now, downloads' (↓) and uploads' (↑), as the backend's "limit"
  // message has them: on, mibps (own setting, speed schedule or a change), source, until, nextMiBps.
  limits: { down: { on: false, mibps: 5 }, up: { on: false, mibps: 5 } },
  schedule: [],          // speed schedule rules, as the backend keeps them ('mibps' ↓, 'upMiBps' ↑)
  uploadExisting: 'ask', // Settings → Uploads: a file already on the server (ask, replace, different, newer, skip, both)
  theme: 'auto',         // Settings → Appearance: auto (follow Windows), light or dark
  update: { state: '' }, // the backend's update banner: state, version, page, phase, error, completing, note
  lists: [],             // the last search's sources per server: age, live, refreshing, error, notes
  updateLater: false,    // "Later": hidden until grab starts again
  // Browse mode: the folder shown, and what it holds. `server` '' is All servers' own top (the
  // servers themselves); `top` a server's top; `under` a search below the folder, while its
  // results show; `typed` the server of an address typed in the path bar, until its answer.
  browse: {
    id: 0, server: '', path: '', top: true, crumbs: [], items: [], severalRoots: false,
    source: '', age: 0, refreshing: false, listError: '', error: '', loading: false,
    under: null, pendingSelect: null, keepSelection: false, typed: null,
    uploadable: false, // the backend's word: the folder shown takes uploads (not the made-up top of several roots)
  },
  // The path bar as a field (the address bar): `typed` the text typed (the suggestions follow
  // it; '' untouched), `field` the field's own text (what was typed, or the address it opened
  // with), `shown` the suggestions ({ label, text, server } or a pin's { label, text, where, pin:
  // { server, path } }), `sel` the highlighted one, `cache` the folder names per "server\nparent"
  // (null while asked), `want` the key last needed, `error` why the last address (or pin) failed,
  // `unpin` the pin it was when it isn't there or can't be listed ({ server, path }, else null).
  addr: { editing: false, typed: '', field: '', shown: [], sel: -1, cache: new Map(), want: '', error: '', unpin: null },
  // Browse's last folder per server while the window runs (absent: its top); '*' -> the server
  // All servers had open. Not kept across starts: each start opens a server at its top.
  browsePaths: {},
  // Back and Forward: one history for the window (all servers) while it runs. `cur` the place
  // Browse is at or opening; `back` and `fwd` the places before and after it, nearest last. A
  // place: { remote (the server picker's choice), server, path, top, cursorKey, scroll, shown }.
  nav: { back: [], fwd: [], cur: null },
};

// Browse's changes on servers (SERVER-OPS.md, chunk 2; see "Browse's changes on a server" below).
const ops = {
  token: 0,
  // Each server's account as the backend said once Browse opened it: { readOnly, free }. Absent:
  // not asked yet, or the last ask failed (asked again the next time). `asking`: asked now.
  accounts: new Map(),
  asking: new Set(),
  // What Cut or Copy holds: { server, op: 'cut' | 'copy', items: [{ path, name, dir }], keys: the
  // cut rows' rowKey, faded until pasted }.
  clip: null,
  running: null,    // the change (or a paste's look) running: { token, op, what, text, progress }
  result: null,     // the last one's result in the footer: { text, bad, title, until }
  refusal: null,    // a refusal said while one runs, over its progress for a while: { text, until }
  waits: new Map(), // token -> { done(msg), progress(msg) }: who takes the backend's answers
};

// Quick access (PINS.md, chunk 2; see "Quick access" below). `pins`: each server's pinned folders
// in the user's order, as the backend keeps them ({ server: [{ path, name? }] }, grab.conf's servers
// only); `gone`: "server\npath" -> why a pin didn't open this session ('not found', 'can’t be
// listed'), dimmed until it opens again; `open`: the list shown, `sel` its highlighted row (-1
// none); `drag`: a press or drag on a row; `want`: the change asked for, said in the footer once
// the backend's list shows it done ({ done(), text, act }); `follow`: "server\npath" of a pin
// moved, whose row stays highlighted.
const quick = { pins: {}, gone: new Map(), open: false, sel: -1, drag: null, want: null, follow: '' };
// Beside a pin's "Not found on Main: …" (the footer, the address field's list).
const UNPIN_GONE = ' · <button type="button" class="link" data-act="unpin-gone">Unpin</button>';

// ---- helpers -----------------------------------------------------------------------------

const webview = window.chrome && window.chrome.webview;
function send(msg) { if (webview) webview.postMessage(msg); }

// Bidi controls in a server's text shown as marks, so a name can't read backwards:
// "invoice⟨U+202E⟩fdp.exe", not "invoiceexe.pdf" (review G51; the tray does the same).
const BIDI = /[\u061C\u200E\u200F\u202A-\u202E\u2066-\u2069]/g;
function bidiVisible(s) {
  return String(s).replace(BIDI, (c) => `⟨U+${c.charCodeAt(0).toString(16).toUpperCase().padStart(4, '0')}⟩`);
}

function esc(s) {
  return bidiVisible(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

function fmtSize(bytes) {
  if (bytes == null) return '';
  if (bytes < 1024) return `${bytes} B`;
  const units = ['KiB', 'MiB', 'GiB', 'TiB', 'PiB'];
  let v = bytes / 1024, u = 0;
  while (v >= 1024 && u < units.length - 1) { v /= 1024; ++u; }
  return `${v.toFixed(1)} ${units[u]}`;
}

function fmtEta(sec) {
  if (sec == null || !isFinite(sec)) return '';
  sec = Math.round(sec);
  if (sec < 60) return `${sec} s left`;
  const m = Math.round(sec / 60);
  if (m < 60) return `${m} min left`;
  return `${Math.floor(m / 60)} h ${m % 60} min left`;
}

function parentOf(path) {
  const i = path.lastIndexOf('/');
  return i <= 0 ? (i === 0 ? '/' : '') : path.slice(0, i);
}

function currentRemote() { return state.remotes.find((r) => r.name === state.remote); }
const ALL = '*'; // "All servers" in the server picker

// "45 s", "12 min", "3 h", "2 days"
function fmtAge(sec) {
  if (sec < 90) return `${Math.max(0, Math.round(sec))} s`;
  if (sec < 90 * 60) return `${Math.round(sec / 60)} min`;
  if (sec < 36 * 3600) return `${Math.round(sec / 3600)} h`;
  return `${Math.round(sec / 86400)} days`;
}

const pad2 = (n) => String(n).padStart(2, '0');

// A last-modified time (Unix seconds) in local time: "2026-10-16 14:32", with `seconds`
// "2026-10-16 14:32:05"; '' when unknown.
function fmtDate(sec, seconds = false) {
  if (!Number.isFinite(sec) || sec <= 0) return '';
  const d = new Date(sec * 1000);
  if (Number.isNaN(d.getTime())) return '';
  const text = `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())} ${pad2(d.getHours())}:${pad2(d.getMinutes())}`;
  return seconds ? `${text}:${pad2(d.getSeconds())}` : text;
}

// ---- header: the server ------------------------------------------------------------------

function renderRemotes() {
  const sel = $('#remote');
  const all = state.remotes.length > 1
    ? `<option value="${ALL}"${state.remote === ALL ? ' selected' : ''}>All servers</option>` : '';
  sel.innerHTML = all + state.remotes.map((r) =>
    `<option value="${esc(r.name)}"${r.name === state.remote ? ' selected' : ''}>${esc(r.name)}</option>`).join('');
  renderRemoteInfo();
}

function renderRemoteInfo() {
  const info = $('#remote-info');
  const r = currentRemote();
  $('#dest').value = state.destinations[state.remote] || state.defaultDest;
  if (state.remote === ALL) {
    info.hidden = false;
    info.classList.remove('error');
    info.textContent = `${state.remotes.length} servers · from their file lists`;
    info.title = '';
    return;
  }
  if (!r) { info.hidden = true; return; }
  info.hidden = false;
  info.classList.toggle('error', !!r.error);
  info.textContent = r.error ? 'misconfigured' : `${r.user}@${r.host} · ${SEARCH_TEXT[r.method] || SEARCH_TEXT.probe}`;
  info.title = r.error || '';
}

// The search box: its placeholder names the folder; ✕ shows with words typed, results shown or
// a search running, and a spinner while one runs.
function renderSearchBox() {
  const q = $('#query');
  q.placeholder = `Search ${browseHere()}`;
  q.title = 'Typing filters this folder. Enter searches it and every folder below it; Esc goes back.';
  $('#search-clear').hidden = !q.value && !state.browse.under && !state.searching;
  $('#search-spin').hidden = !state.searching;
}

function exactOn() { return $('#exact').getAttribute('aria-pressed') === 'true'; }

// ---- banner and search -------------------------------------------------------------------

function showBanner(text) {
  const b = $('#banner');
  b.hidden = !text;
  b.textContent = bidiVisible(text || '');
}

function setSearching(on) {
  state.searching = on;
  renderSearchBox();
  renderEmpty();
}

// A search still running stops, and its answer, still on its way, is ignored.
function stopSearch() {
  if (state.searching) {
    send({ type: 'cancelSearch' });
    setSearching(false);
  }
  state.searchId += 1;
}

// Enter in the search box: this folder and every folder below it (searchBelow); with no words,
// back to the folder.
function startSearch() {
  const query = $('#query').value.trim();
  if (!query) {
    closeBelow();
    $('#query').focus();
    return;
  }
  searchBelow(query);
}

// Where the results came from: "from the file list, 3 min old · refreshing…", per server under
// All servers; failures in red.
function renderListStatus() {
  const el = $('#list-status');
  const lists = state.lists;
  const refreshing = lists.filter((l) => l.refreshing).map((l) => l.remote);
  const parts = [];
  if (state.remote === ALL) {
    const aged = lists.filter((l) => typeof l.age === 'number');
    if (aged.length) parts.push(`from file lists, up to ${fmtAge(Math.max(...aged.map((l) => l.age)))} old`);
    const missing = lists.filter((l) => typeof l.age !== 'number' && !l.live && l.refreshing).map((l) => l.remote);
    if (missing.length) parts.push(`fetching the lists of ${missing.join(', ')}`);
    else if (refreshing.length) parts.push(`refreshing ${refreshing.join(', ')}`);
  } else if (lists.length) {
    const l = lists[0];
    if (typeof l.age === 'number') parts.push(`from the file list, ${fmtAge(l.age)} old`);
    if (l.refreshing) parts.push(l.live || typeof l.age !== 'number' ? 'building the file list' : 'refreshing');
  }
  // What a lookup couldn't look at (folders it couldn't read, a search root, link loops): the
  // first, and all of them in the tooltip.
  const notes = lists.flatMap((l) => (l.notes || []).map((n) => `${state.remote === ALL ? `${l.remote}: ` : ''}${n}`));
  if (notes.length) parts.push(`not searched: ${notes[0]}${notes.length > 1 ? ` (and ${notes.length - 1} more)` : ''}`);
  const errors = lists.filter((l) => l.error).map((l) => `${state.remote === ALL ? `${l.remote}: ` : ''}${l.error}`);
  el.innerHTML = (refreshing.length ? '<span class="spinner"></span>' : '') + esc(parts.join(' · ')) +
    (errors.length ? `${parts.length ? ' · ' : ''}<span class="bad">list refresh failed: ${esc(errors.join('; '))}</span>` : '');
  el.title = [...errors, ...notes].join('\n');
  $('#refresh-lists').hidden = !lists.some((l) => typeof l.age === 'number') || refreshing.length > 0;
}

function onSearchResult(msg) {
  if (msg.id !== state.searchId) return;
  if (!state.browse.under) return; // the results were closed
  // An update (the same search after a list refresh) keeps the selection and cursor by path.
  const selectedKeys = msg.update ? new Set([...state.selected].map((i) => rowKey(state.hits[i]))) : null;
  const cursorKey = msg.update && state.hits[state.cursor] ? rowKey(state.hits[state.cursor]) : null;
  const shown = msg.update && cursorShown(); // a sorted update can move the cursor row far away
  msg.hits.forEach((h, i) => { h.ord = i; }); // best match first: the default order
  state.results = msg.hits;
  state.filterTotal = msg.hits.length;
  state.hits = sortRows(applyFilters(msg.hits, false));
  state.lists = msg.lists || [];
  if (msg.update) {
    state.selected = new Set();
    state.hits.forEach((h, i) => { if (selectedKeys.has(rowKey(h))) state.selected.add(i); });
    const c = cursorKey ? state.hits.findIndex((h) => rowKey(h) === cursorKey) : -1;
    state.cursor = c >= 0 ? c : (state.hits.length ? 0 : -1);
  } else {
    state.cursor = state.hits.length ? 0 : -1;
  }
  state.anchor = state.cursor;
  // The hits' servers' accounts, for the menus' greying (each asked once a session): an All
  // servers search can hold hits of a server Browse hasn't opened.
  for (const s of new Set(msg.hits.map(serverOf))) if (state.remotes.some((r) => r.name === s)) askAccount(s);
  // Before rendering: an empty result shows "No files matching …" from these.
  const n = msg.hits.length;
  const noun = `result${n === 1 ? '' : 's'}`;
  const live = state.lists.length && state.lists.every((l) => l.live);
  state.resultNote = live ? `${n} ${noun} in ${(msg.elapsedMs / 1000).toFixed(1)} s` : `${n} ${noun}`;
  renderBrowseBar();
  renderListStatus();
  if (state.searching) setSearching(false);
  if (!msg.update) $('#results').scrollTop = 0;
  renderRows(); // also renders the summary
  if (shown) keepInView(state.cursor); // after renderRows sized the list; scrolling draws again
  if (!msg.update && state.hits.length) $('#results').focus();
}

function onSearchError(msg) {
  if (msg.id !== state.searchId) return;
  const u = state.browse.under;
  if (u) u.error = msg.cancelled ? 'Search stopped.' : msg.message; // shown where results would be
  else if (!msg.cancelled) showBanner(msg.message);
  state.resultNote = msg.cancelled ? 'Search stopped' : '';
  setSearching(false);
  renderSummary();
}

// ---- results list (virtualized: only rows in view exist in the DOM) ---------------------

let renderQueued = false;
function scheduleRows() {
  if (renderQueued) return;
  renderQueued = true;
  requestAnimationFrame(() => { renderQueued = false; renderRows(); });
}

function renderRows() {
  const list = $('#results');
  const n = state.hits.length;
  $('#spacer').style.height = `${n * ROW}px`;
  const first = Math.max(0, Math.floor(list.scrollTop / ROW) - 10);
  const last = Math.min(n, Math.ceil((list.scrollTop + list.clientHeight) / ROW) + 10);
  let html = '';
  const under = state.browse.under;
  $('#dir-head').textContent = under ? (under.all ? 'Server · Folder' : 'Folder') : state.browse.server ? 'Contents' : 'Login';
  const now = Date.now() / 1000;
  for (let i = first; i < last; ++i) {
    const h = state.hits[i];
    const sel = state.selected.has(i);
    let third;
    if (under) {
      // A search result's folder, from the folder searched (its own name when the hit is right
      // in it), with its server under All servers: a link that opens it with the hit picked.
      const parent = parentOf(h.path);
      const shown = under.whole ? (parent || '~') : (relativeTo(parent, under.path) || under.name);
      third = `${under.all ? `<span class="srv">${esc(h.remote)}</span>` : ''}` +
        `<span class="go" data-go="${i}" title="Open ${esc(parent || '~')}">${esc(shown)}</span>`;
    } else if (h.server) {
      third = esc(h.info);
    } else {
      third = h.items != null ? `${h.items} item${h.items === 1 ? '' : 's'}` : '';
    }
    // The date; its title adds the seconds and the age: "2026-10-16 14:32:05 · 3 days ago" (a
    // date ahead of this PC's clock: "in 2 days").
    const date = fmtDate(h.modified);
    const age = now - h.modified;
    const dateTitle = date ? ` title="${fmtDate(h.modified, true)} · ${age >= 0 ? `${fmtAge(age)} ago` : `in ${fmtAge(-age)}`}"` : '';
    const cut = ops.clip && ops.clip.keys.has(rowKey(h)); // faded until pasted
    html += `<div class="row${sel ? ' sel' : ''}${i === state.cursor ? ' cursor' : ''}${h.dir ? ' folder' : ''}${cut ? ' cut' : ''}" data-i="${i}">` +
      `<input type="checkbox" tabindex="-1"${sel ? ' checked' : ''}${h.server ? ' disabled' : ''}>` +
      `<span class="name" title="${esc(h.path || h.name)}">${itemIcon(h.name, h.dir)}${esc(h.name)}</span>` +
      `<span class="size">${fmtSize(h.size)}</span>` +
      `<span class="modified"${dateTitle}>${date}</span>` +
      `<span class="dir">${third}</span></div>`;
  }
  const rows = $('#rows');
  rows.style.transform = `translateY(${first * ROW}px)`;
  rows.innerHTML = html;
  renderEmpty();
  renderSummary();
}

function renderEmpty() {
  const empty = $('#empty');
  if (state.hits.length) { empty.hidden = true; return; }
  empty.hidden = false;
  const b = state.browse;
  const filter = $('#query').value.trim();
  const u = b.under;
  if (filtersOn() && state.filterTotal > 0 && !state.searching && !b.loading) {
    empty.textContent = `None of the ${state.filterTotal} ${u ? 'results' : 'items here'} ${state.filterTotal === 1 ? 'matches' : 'match'} the filters.`;
  } else if (state.searching && u) {
    empty.innerHTML = `<span><span class="spinner"></span>Searching ${u.all ? 'every server' : `${esc(u.name)} and its folders`}` +
      ` for “${esc(state.lastQuery)}”…</span>`;
  } else if (u && u.error) {
    empty.textContent = bidiVisible(u.error);
  } else if (u) {
    empty.textContent = bidiVisible(`Nothing ${u.all ? 'on any server' : `in ${u.name} or its folders`} matches “${u.query}”.`);
  } else if (b.loading) {
    empty.innerHTML = `<span><span class="spinner"></span>Opening ${esc(browseHere())}…</span>`;
  } else if (b.error) {
    empty.textContent = bidiVisible(b.error);
  } else if (!state.remotes.length) {
    empty.innerHTML = '<div>No servers yet. Add the Linux server you want to download from.<br>' +
      '<button type="button" class="primary" data-act="add-server">Add a server</button></div>';
  } else if (filter) {
    empty.textContent = bidiVisible(`Nothing in ${browseHere()} matches “${filter}”. Enter searches its folders too.`);
  } else {
    empty.textContent = 'This folder is empty.';
  }
}

function selectedHits() {
  const idx = state.selected.size ? [...state.selected].sort((a, b) => a - b) : (state.cursor >= 0 ? [state.cursor] : []);
  return idx.map((i) => state.hits[i]).filter((h) => h && !h.server); // a server row is not downloadable
}

function renderSummary() {
  renderFilterStrip();
  const sel = [...state.selected].map((i) => state.hits[i]);
  const bytes = sel.reduce((sum, h) => sum + (h.size || 0), 0);
  const unknown = sel.some((h) => h.size == null); // folders have no size: at least this much
  let text = state.resultNote || '';
  if (sel.length) text += `${text ? ' · ' : ''}${sel.length} selected${bytes ? ` (${fmtSize(bytes)}${unknown ? '+' : ''})` : ''}`;
  $('#summary').textContent = bidiVisible(text);
  const count = selectedHits().length;
  const btn = $('#download');
  btn.disabled = count === 0 || !state.remote;
  btn.textContent = count > 1 ? `Download ${count}` : 'Download';
  const all = $('#select-all');
  all.checked = state.hits.length > 0 && state.selected.size === state.hits.length;
  all.indeterminate = state.selected.size > 0 && state.selected.size < state.hits.length;
}

function setCursor(i, { extend = false, toggle = false, keep = false } = {}) {
  const n = state.hits.length;
  if (!n) return;
  i = Math.max(0, Math.min(n - 1, i));
  if (extend && state.anchor >= 0) {
    state.selected.clear();
    const [a, b] = state.anchor < i ? [state.anchor, i] : [i, state.anchor];
    for (let k = a; k <= b; ++k) state.selected.add(k);
  } else if (toggle) {
    if (state.selected.has(i)) state.selected.delete(i); else state.selected.add(i);
    state.anchor = i;
  } else if (!keep) {
    state.selected.clear();
    state.selected.add(i);
    state.anchor = i;
  }
  state.cursor = i;
  keepInView(i);
  renderRows();
}

// Scrolls the list just enough to show row `i`.
// Whether the cursor row is on screen now: an update that moves it keeps it there, but a list
// scrolled away from it stays where the user put it.
function cursorShown() {
  const list = $('#results');
  return state.cursor >= 0 && state.cursor * ROW >= list.scrollTop &&
    (state.cursor + 1) * ROW <= list.scrollTop + list.clientHeight;
}

function keepInView(i) {
  const list = $('#results');
  const top = i * ROW;
  if (top < list.scrollTop) list.scrollTop = top;
  else if (top + ROW > list.scrollTop + list.clientHeight) list.scrollTop = top + ROW - list.clientHeight;
}

function rowIndex(ev) {
  const row = ev.target.closest('.row');
  return row ? Number(row.dataset.i) : -1;
}

// A row's identity across a new order or a refreshed list: its server's row in All servers' top,
// else its server and path.
function rowKey(h) {
  return h.server ? `srv:${h.server}` : `${h.remote || ''}|${h.path}`;
}

// ---- sorting: a click on Name, Size or Modified ------------------------------------------

// The first click's direction: names A to Z, the largest and the newest first. The second click
// reverses it; the third returns to the default order.
const SORT_FIRST = { name: 'asc', size: 'desc', modified: 'desc' };

// The column the rows are sorted by, '' for the default order (best match for search results,
// folders first A to Z in a folder).
function sortKey() {
  return state.sort.key;
}

// A saved sort from the backend; anything else is the default order. A Size sort's `then` must
// be a Name or Modified sort; another is dropped.
function sortFrom(v) {
  const ok = (s) => s && typeof s === 'object' && typeof s.key === 'string' && Object.hasOwn(SORT_FIRST, s.key) &&
    (s.dir === 'asc' || s.dir === 'desc');
  if (!ok(v)) return { key: '', dir: 'asc', then: null };
  const then = v.key === 'size' && ok(v.then) && v.then.key !== 'size' ? { key: v.then.key, dir: v.then.dir } : null;
  return { key: v.key, dir: v.dir, then };
}

// The sort as gui.json keeps it: null for the default order.
function sortToSave(s) {
  if (!s.key) return null;
  return s.then ? { key: s.key, dir: s.dir, then: { key: s.then.key, dir: s.then.dir } } : { key: s.key, dir: s.dir };
}

// `s` with A to Z in lower case, and nothing else changed (as browse::natural_less compares).
function asciiLower(s) {
  return /[A-Z]/.test(s) ? s.replace(/[A-Z]+/g, (m) => m.toLowerCase()) : s;
}

const isDigit = (c) => c >= 48 && c <= 57; // '0' to '9'

// browse::natural_less as a three-way compare: digit runs as whole numbers ("file 9" before
// "file 10"), A to Z without case, any other character by code unit. `la` and `lb` are `a` and
// `b` through asciiLower (the same length).
function naturalCompare(a, b, la = asciiLower(a), lb = asciiLower(b)) {
  let i = 0;
  let j = 0;
  while (i < la.length && j < lb.length) {
    const x = la.charCodeAt(i);
    const y = lb.charCodeAt(j);
    if (isDigit(x) && isDigit(y)) {
      // Whole numbers: without leading zeros, the longer one is bigger.
      let ai = i;
      let bj = j;
      while (ai < la.length && la.charCodeAt(ai) === 48) ++ai;
      while (bj < lb.length && lb.charCodeAt(bj) === 48) ++bj;
      let ae = ai;
      let be = bj;
      while (ae < la.length && isDigit(la.charCodeAt(ae))) ++ae;
      while (be < lb.length && isDigit(lb.charCodeAt(be))) ++be;
      if (ae - ai !== be - bj) return (ae - ai) - (be - bj);
      for (let k = 0; k < ae - ai; ++k) {
        const d = la.charCodeAt(ai + k) - lb.charCodeAt(bj + k);
        if (d) return d;
      }
      i = ae;
      j = be;
      continue;
    }
    // Code points, as the backend's UTF-8 byte order has them (UTF-16 units differ above U+FFFF).
    if (x !== y) return la.codePointAt(i) - lb.codePointAt(j);
    ++i;
    ++j;
  }
  const rest = (la.length - i) - (lb.length - j);
  if (rest) return rest;
  return a < b ? -1 : a > b ? 1 : 0; // only case or leading zeros differ: still a strict order
}

// The rows in the chosen order, as a new array (`rows` may be the folder's own items). Browse
// keeps folders above files. Folders have no size, so a Size sort puts the files first, by size,
// and the folders after them in the sort before it (`then`), or in their default order.
function sortRows(rows) {
  const key = sortKey();
  if (key !== 'size') return ordered(rows, key, state.sort.dir, true);
  const then = state.sort.then || { key: '', dir: 'asc' };
  return [...ordered(rows.filter((h) => !h.dir), 'size', state.sort.dir, false),
    ...ordered(rows.filter((h) => h.dir), then.key, then.dir, false)];
}

// `rows` by `key` ('' for the default order) in direction `dir`, as a new array; folders above
// files when `folders`. Rows without a size or date go last both ways; ties keep the default
// order (`ord`, stamped where the rows arrive).
function ordered(rows, key, dir, folders) {
  if (!key) return rows.slice().sort((x, y) => x.ord - y.ord);
  const desc = dir === 'desc';
  // What each row is compared by, worked out once rather than in every comparison.
  const value = key === 'name' ? (h) => asciiLower(h.name)
    : key === 'size' ? (h) => (Number.isFinite(h.size) ? h.size : null)
      : (h) => (Number.isFinite(h.modified) && h.modified > 0 ? h.modified : null);
  const items = rows.map((h) => ({ h, folder: folders && !!h.dir, v: value(h) }));
  items.sort((x, y) => {
    if (x.folder !== y.folder) return x.folder ? -1 : 1;
    let c = 0;
    if (key === 'name') c = naturalCompare(x.h.name, y.h.name, x.v, y.v);
    else if (x.v !== null && y.v !== null) c = x.v - y.v;
    else if (x.v !== y.v) return x.v === null ? 1 : -1; // unknown last, both ways
    if (c) return desc ? -c : c;
    return x.h.ord - y.h.ord;
  });
  return items.map((x) => x.h);
}

// The header's arrow on the sorted column.
function renderSortHead() {
  const key = sortKey();
  for (const b of document.querySelectorAll('.results-head [data-sort]')) {
    if (b.dataset.sort === key) b.dataset.dir = state.sort.dir;
    else delete b.dataset.dir;
  }
}

// The rows in the current order: the selection, cursor and anchor stay on the same rows, and the
// cursor row in view.
function resort() {
  const keyAt = (i) => (state.hits[i] ? rowKey(state.hits[i]) : null);
  const picked = new Set([...state.selected].map(keyAt));
  const cursorKey = keyAt(state.cursor);
  const anchorKey = keyAt(state.anchor);
  state.hits = sortRows(state.hits);
  state.selected = new Set();
  state.cursor = state.anchor = -1;
  state.hits.forEach((h, i) => {
    const k = rowKey(h);
    if (picked.has(k)) state.selected.add(i);
    if (k === cursorKey && state.cursor < 0) state.cursor = i;
    if (k === anchorKey && state.anchor < 0) state.anchor = i;
  });
  if (state.cursor >= 0) keepInView(state.cursor);
  renderSortHead();
  renderRows();
}

// A click on a column: its first direction, then the other, then the default order; kept in
// gui.json for the next searches, folders and starts.
function clickSort(key) {
  const s = state.sort;
  if (s.key !== key) {
    // Size orders the files; the folders after them keep the sort before it.
    const then = key === 'size' && s.key ? { key: s.key, dir: s.dir } : null;
    state.sort = { key, dir: SORT_FIRST[key], then };
  } else if (s.dir === SORT_FIRST[key]) {
    state.sort = { ...s, dir: s.dir === 'asc' ? 'desc' : 'asc' };
  } else {
    state.sort = { key: '', dir: 'asc', then: null };
  }
  resort();
  send({ type: 'prefs', sort: sortToSave(state.sort) });
}

// The mouse leaves the focus where it was (the arrow keys go on moving in the results); Tab
// reaches the headers, and Enter or Space clicks them.
$('.results-head').addEventListener('mousedown', (ev) => {
  if (!ev.target.closest('#select-all')) ev.preventDefault();
});
$('.results-head').addEventListener('click', (ev) => {
  const b = ev.target.closest('button[data-sort]');
  if (b && !b.disabled) clickSort(b.dataset.sort);
});

// ---- browse mode: a server's folders -----------------------------------------------------

// ---- the status bar's activity: what grab is doing while it takes a while -----------------

// From the backend's `activity` messages: { id, text, detail, bg, estimate, done, total }, then
// { id, end }. Kept here with when each started, showed and ended.
const activities = new Map();
let activityFrame = 0;
const SHOW_AFTER = 100;    // ms: instant work never shows
const SHOW_AT_LEAST = 400; // ms on screen once shown, so it doesn't flash
const FADE = 350;          // ms, after the bar filled

function onActivity(msg) {
  const now = performance.now();
  let a = activities.get(msg.id);
  if (msg.end) {
    if (!a) return;
    a.end = now;
    if (!a.shown) activities.delete(msg.id); // over before it showed: nothing to see
  } else if (!a) {
    a = { id: msg.id, text: msg.text, detail: msg.detail || '', bg: !!msg.bg, estimate: msg.estimate || 3000,
      done: msg.done, total: msg.total, start: now, shown: 0, end: 0 };
    activities.set(msg.id, a);
  } else {
    if (msg.text) a.text = msg.text;
    if (msg.detail) a.detail = msg.detail;
    if (msg.estimate) a.estimate = msg.estimate; // the step turned out to be another (find, a walk)
    if (msg.total) {
      // Counted from now on: the real numbers fill what the estimate left, so the bar goes on.
      if (!a.total) a.base = a.fill || 0;
      a.done = msg.done;
      a.total = msg.total;
    }
  }
  if (!activityFrame) activityFrame = requestAnimationFrame(drawActivity);
}

// How full the bar is: real progress where it was counted (filling what an estimate before it
// left); else the time so far against the time the same step took before, 85% by then and
// creeping on after, never quite full until it ends.
function activityFraction(a, now) {
  if (a.end) return 1;
  if (a.total > 0) return (a.base || 0) + (1 - (a.base || 0)) * Math.min(1, a.done / a.total);
  const t = now - a.start;
  const est = Math.max(300, a.estimate);
  return t < est ? 0.85 * (t / est) : 0.85 + 0.14 * (1 - Math.exp(-(t - est) / (est * 1.5)));
}

// Every frame while anything runs: the latest thing waited on (else background work), its bar,
// its message with the seconds so far, "+N" for the others (listed on hover with the command).
function drawActivity() {
  activityFrame = 0;
  const now = performance.now();
  for (const [id, a] of activities) {
    if (a.end && (!a.shown || now > Math.max(a.end, a.shown + SHOW_AT_LEAST) + FADE)) activities.delete(id);
  }
  const all = [...activities.values()];
  const live = all.filter((a) => !a.end);
  const latest = (list) => list.filter((a) => a.shown || now - a.start >= SHOW_AFTER)
    .sort((x, y) => y.start - x.start)[0];
  const shown = latest(live.filter((a) => !a.bg)) || latest(all.filter((a) => !a.bg)) || latest(live) || latest(all);
  // A bar never goes back: when real numbers take over from the estimate, it waits for them.
  for (const a of all) a.fill = Math.max(a.fill || 0, activityFraction(a, now));
  const el = $('#activity');
  el.hidden = !shown;
  $('footer.action').classList.toggle('busy', !!shown);
  if (shown) {
    if (!shown.shown) shown.shown = now;
    const fading = !!shown.end && now >= Math.max(shown.end, shown.shown + SHOW_AT_LEAST);
    el.classList.toggle('bg', shown.bg);
    el.classList.toggle('done', !!shown.end);
    el.classList.toggle('fading', fading);
    $('#act-fill').style.width = `${(shown.fill * 100).toFixed(1)}%`;
    const secs = Math.floor((now - shown.start) / 1000);
    const text = bidiVisible(shown.text + (!shown.end && secs >= 2 ? ` · ${secs} s` : ''));
    if ($('#act-text').textContent !== text) $('#act-text').textContent = text;
    const others = live.filter((a) => a !== shown);
    $('#act-more').hidden = !others.length;
    $('#act-more').textContent = `+${others.length}`;
    el.title = bidiVisible([shown.detail, ...others.map((a) => `Also: ${a.text}`)].filter(Boolean).join('\n'));
  }
  if (activities.size) activityFrame = requestAnimationFrame(drawActivity);
}

// ---- previews: play a video or audio file, view an image, text file or PDF ---------------

// The same lists as the backend's (src/preview.cpp).
const KIND_OF = new Map();
for (const [kind, list] of Object.entries({
  video: 'mkv mp4 m4v avi mov wmv flv webm ts m2ts mts mpg mpeg vob ogv 3gp divx rmvb',
  audio: 'mp3 flac m4a m4b aac ogg oga opus wav wma ape aiff aif alac mka dts ac3 wv',
  image: 'jpg jpeg jfif png gif webp avif bmp ico svg',
  text: 'nfo txt srt ass ssa vtt log md ini cfg conf json xml csv cue sfv m3u m3u8 yml yaml toml diz',
  pdf: 'pdf',
})) for (const ext of list.split(' ')) KIND_OF.set(ext, kind);

// '' for a folder, a server or a file without a preview.
function kindOf(h) {
  if (!h || h.dir || h.server) return '';
  const dot = h.name.lastIndexOf('.');
  return dot < 0 ? '' : KIND_OF.get(h.name.slice(dot + 1).toLowerCase()) || '';
}
const plays = (kind) => kind === 'video' || kind === 'audio';
const views = (kind) => kind === 'image' || kind === 'text' || kind === 'pdf';

// Double-click, Space or the menu's Play / Preview: a video or audio file plays in the program
// Windows uses for it; an image, text file or PDF opens in the viewer. False for other files.
function openFile(h) {
  const kind = kindOf(h);
  if (plays(kind)) {
    send({ type: 'play', remote: h.remote || state.remote, path: h.path, size: h.size });
    return true;
  }
  if (views(kind)) {
    openViewer(h);
    return true;
  }
  return false;
}

// A short note in the footer, e.g. "Playing x.mkv in mpc-hc64.exe".
function flash(text, ms = 6000) {
  state.flash = { text, until: Date.now() + ms };
  renderBrowseBar();
  setTimeout(renderBrowseBar, ms + 50);
}

// The row's size and time go along, so the backend knows whether a preview it kept still holds;
// `ahead`: a neighbour fetched before it is shown (it waits for a free connection).
function previewUrl(h, ahead = false) {
  const q = [];
  if (Number.isFinite(h.size)) q.push(`size=${h.size}`);
  if (Number.isFinite(h.modified)) q.push(`modified=${h.modified}`);
  if (ahead) q.push('ahead');
  const base = `https://grab.ui/preview/${encodeURIComponent(h.remote || state.remote)}/${encodeURIComponent(h.path)}`;
  return q.length ? `${base}?${q.join('&')}` : base;
}

// The viewer goes through the list's images, text files and PDFs, as they were when it opened.
// `blob`: the image shown, as a blob: URL, let go when it goes.
const viewer = { open: false, list: [], index: -1, token: 0, returnTo: null, blob: null };

function dropViewerBlob() {
  if (viewer.blob) URL.revokeObjectURL(viewer.blob);
  viewer.blob = null;
}

// The backend stops fetching what the viewer no longer wants: all but the one shown and its
// neighbours, or everything once it closes.
function cancelPreviews() {
  const keep = viewer.open
    ? [viewer.list[viewer.index], viewer.list[viewer.index + 1], viewer.list[viewer.index - 1]]
      .filter((n) => views(kindOf(n))).map((n) => previewUrl(n))
    : [];
  send({ type: 'previewCancel', keep });
}

function openViewer(h) {
  viewer.list = state.hits.filter((x) => views(kindOf(x)));
  viewer.index = Math.max(0, viewer.list.indexOf(h));
  viewer.open = true;
  viewer.returnTo = document.activeElement;
  $('#viewer').hidden = false;
  showViewerItem();
  $('#viewer-body').focus();
}

function closeViewer() {
  if (!viewer.open) return;
  viewer.open = false;
  viewer.token += 1;
  cancelPreviews();
  $('#viewer').hidden = true;
  $('#viewer-body').replaceChildren();
  dropViewerBlob();
  // The list's cursor goes to the last one viewed, as in Explorer.
  const last = viewer.list[viewer.index];
  const i = last ? state.hits.indexOf(last) : -1;
  if (i >= 0) setCursor(i);
  $('#results').focus();
}

function moveViewer(step) {
  const i = viewer.index + step;
  if (!viewer.open || i < 0 || i >= viewer.list.length) return;
  viewer.index = i;
  showViewerItem();
}

function showViewerItem() {
  const h = viewer.list[viewer.index];
  const kind = kindOf(h);
  const url = previewUrl(h);
  const token = ++viewer.token;
  const info = [fmtSize(h.size), fmtDate(h.modified)].filter(Boolean);
  $('#viewer-name').textContent = bidiVisible(h.name);
  $('#viewer-name').title = bidiVisible(h.path);
  $('#viewer-info').textContent = info.join(' · ');
  $('#viewer-pos').textContent = viewer.list.length > 1 ? `${viewer.index + 1} / ${viewer.list.length}` : '';
  $('#viewer-prev').disabled = viewer.index <= 0;
  $('#viewer-next').disabled = viewer.index >= viewer.list.length - 1;
  const body = $('#viewer-body');
  body.className = `viewer-body ${kind}`;
  body.innerHTML = '<span class="spinner"></span>';
  dropViewerBlob();
  cancelPreviews();
  const failed = (text) => {
    if (token !== viewer.token) return;
    const p = document.createElement('div');
    p.className = 'viewer-error';
    p.textContent = bidiVisible(text || 'Couldn’t load the preview.');
    body.replaceChildren(p);
  };
  if (kind === 'image') {
    const img = new Image();
    img.className = 'fit';
    img.alt = h.name;
    img.onload = () => {
      if (token !== viewer.token) return;
      body.replaceChildren(img);
      $('#viewer-info').textContent = [`${img.naturalWidth} × ${img.naturalHeight}`, ...info].join(' · ');
      fetchAhead();
    };
    // Bytes that aren't a picture the browser can show: a fixed message, never the bytes (G38).
    img.onerror = () => failed('This image can’t be shown.');
    img.draggable = false; // a drag moves the view (below), never the picture itself
    // A click: the full size (scroll or drag to see the rest), the spot clicked staying under
    // the pointer. A click on it at full size goes back (the panning handlers below).
    img.addEventListener('click', (ev) => {
      if (!img.classList.contains('fit')) return;
      const was = img.getBoundingClientRect();
      const fx = (ev.clientX - was.left) / was.width;
      const fy = (ev.clientY - was.top) / was.height;
      img.classList.remove('fit');
      body.classList.add('full');
      const now = img.getBoundingClientRect(); // at full size, centred where it fits
      body.scrollLeft += now.left + fx * now.width - ev.clientX;
      body.scrollTop += now.top + fy * now.height - ev.clientY;
    });
    // One request gives the picture or, failing, why: a failed or cancelled one isn't fetched
    // again for its reason (G38).
    fetch(url).then(async (r) => {
      if (token !== viewer.token) return;
      if (!r.ok) return failed(await r.text());
      const blob = await r.blob();
      if (token !== viewer.token) return;
      viewer.blob = URL.createObjectURL(blob);
      img.src = viewer.blob;
    }).catch(() => failed(''));
  } else if (kind === 'text') {
    fetch(url).then(async (r) => {
      const text = await r.text();
      if (token !== viewer.token) return;
      if (!r.ok) return failed(text);
      const pre = document.createElement('pre');
      pre.className = 'viewer-text';
      pre.textContent = text;
      body.replaceChildren(pre);
      fetchAhead();
      const total = Number(r.headers.get('X-Grab-Total'));
      if (total) {
        const note = document.createElement('div');
        note.className = 'viewer-note';
        note.textContent = `Showing the first 1.0 MiB of ${fmtSize(total)}`;
        body.append(note);
      }
    }, () => failed(''));
  } else {
    const frame = document.createElement('iframe');
    frame.className = 'viewer-pdf';
    frame.title = bidiVisible(h.name);
    frame.src = url;
    body.replaceChildren(frame);
    fetchAhead();
  }
  // The neighbours' images and text, fetched ahead once this one is in (so they don't slow it
  // down); the backend keeps them.
  function fetchAhead() {
    if (token !== viewer.token) return;
    for (const n of [viewer.list[viewer.index + 1], viewer.list[viewer.index - 1]]) {
      const k = kindOf(n);
      if (k === 'image' || k === 'text') fetch(previewUrl(n, true)).catch(() => {});
    }
  }
}

$('#viewer-prev').addEventListener('click', () => moveViewer(-1));
$('#viewer-next').addEventListener('click', () => moveViewer(1));
$('#viewer-close').addEventListener('click', closeViewer);
$('#viewer-download').addEventListener('click', () => {
  const h = viewer.list[viewer.index];
  if (h) download([h]);
});

// A zoomed image moves with the pointer, dragged with any button; a left click on it without
// moving goes back to the fitted image. Right and middle start no menu or auto-scroll there.
const pan = { id: null, x: 0, y: 0, left: 0, top: 0, moved: false, onImage: false };
{
  const body = $('#viewer-body');
  const zoomed = () => body.classList.contains('full');
  body.addEventListener('pointerdown', (ev) => {
    if (!zoomed() || ev.button > 2 || pan.id !== null) return;
    // Not on a scrollbar: those still scroll.
    const r = body.getBoundingClientRect();
    if (ev.clientX - r.left >= body.clientLeft + body.clientWidth || ev.clientY - r.top >= body.clientTop + body.clientHeight) {
      return;
    }
    ev.preventDefault();
    Object.assign(pan, { id: ev.pointerId, x: ev.clientX, y: ev.clientY, left: body.scrollLeft, top: body.scrollTop,
      moved: false, onImage: ev.target.tagName === 'IMG' });
    body.setPointerCapture(ev.pointerId);
    body.classList.add('panning');
  });
  body.addEventListener('pointermove', (ev) => {
    if (ev.pointerId !== pan.id) return;
    const dx = ev.clientX - pan.x;
    const dy = ev.clientY - pan.y;
    if (Math.abs(dx) + Math.abs(dy) > 3) pan.moved = true;
    body.scrollLeft = pan.left - dx;
    body.scrollTop = pan.top - dy;
  });
  const endPan = (ev) => {
    if (ev.pointerId !== pan.id) return;
    pan.id = null;
    body.classList.remove('panning');
    if (ev.type === 'pointerup' && ev.button === 0 && !pan.moved && pan.onImage) {
      body.querySelector('img')?.classList.add('fit');
      body.classList.remove('full');
    }
  };
  body.addEventListener('pointerup', endPan);
  body.addEventListener('pointercancel', endPan);
  body.addEventListener('mousedown', (ev) => { if (ev.button === 1 && zoomed()) ev.preventDefault(); });
  body.addEventListener('contextmenu', (ev) => { if (zoomed()) ev.preventDefault(); });
}
// Esc or Space closes, ←/→ go through the others; nothing else reaches the list meanwhile.
document.addEventListener('keydown', (ev) => {
  if (!viewer.open || modal) return;
  if (ev.key === 'Escape' || (ev.key === ' ' && !(ev.target instanceof HTMLInputElement))) {
    ev.preventDefault();
    ev.stopImmediatePropagation();
    closeViewer();
  } else if (ev.key === 'ArrowLeft' || ev.key === 'ArrowRight') {
    ev.preventDefault();
    ev.stopImmediatePropagation();
    moveViewer(ev.key === 'ArrowLeft' ? -1 : 1);
  } else if (ev.key !== 'Tab') {
    ev.stopImmediatePropagation();
  }
}, true);

const FOLDER_ICON = '<svg class="ico" viewBox="0 0 16 16" aria-hidden="true"><path d="M1 3.5C1 2.7 1.7 2 2.5 2h3.6l1.6 1.6h5.8c.8 0 1.5.7 1.5 1.5v7.4c0 .8-.7 1.5-1.5 1.5h-11C1.7 14 1 13.3 1 12.5z"/></svg>';

// A file's type for its icon: the preview lists (KIND_OF) and more; 'file' for anything else.
const ICON_KIND = new Map(KIND_OF);
ICON_KIND.set('pdf', 'document');
for (const [kind, list] of Object.entries({
  image: 'tif tiff heic heif psd cr2 nef dng arw',
  text: 'sub idx html htm rtf',
  document: 'epub mobi azw3 djvu doc docx xls xlsx ppt pptx odt ods odp',
  archive: 'zip rar 7z tar gz tgz bz2 tbz2 xz txz zst lz lzma cab',
  program: 'exe msi msix appx bat cmd ps1 sh apk jar deb rpm',
  disc: 'iso img mdf mds nrg dmg vhd vhdx vmdk',
})) for (const ext of list.split(' ')) ICON_KIND.set(ext, kind);

function iconKind(name) {
  const dot = name.lastIndexOf('.');
  if (dot <= 0) return 'file';
  const ext = name.slice(dot + 1).toLowerCase();
  // Split archives: .r00, .z01, .001
  return ICON_KIND.get(ext) || (/^(r\d\d|z\d\d|\d{3})$/.test(ext) ? 'archive' : 'file');
}

// A page in the type's colour, with a mark cut out of it in the panel's colour.
const FILE_ICONS = (() => {
  const page = '<path d="M3.5 1h6L14 5.5v8c0 .8-.7 1.5-1.5 1.5h-9C2.7 15 2 14.3 2 13.5v-11C2 1.7 2.7 1 3.5 1z"/>' +
    '<path class="fold" d="M9.5 1v3c0 .8.7 1.5 1.5 1.5h3z"/>';
  const marks = {
    video: '<path d="M6.5 7.6v5l4-2.5z"/>',
    audio: '<path d="M8.9 6.8h1.4l1.4 1v1.1l-1.4-.8v3.6a1.6 1.6 0 1 1-1.4-1.6z"/>',
    image: '<path d="M4.5 12.8l2.4-3.1 1.6 1.9 1.3-1.4 1.7 2.6z"/><circle cx="10.1" cy="7.9" r="1"/>',
    text: '<path d="M5 7.3h6v1H5zm0 2h6v1H5zm0 2h4v1H5z"/>',
    document: '<path d="M5 7.3h6v1H5zm0 2h6v1H5zm0 2h6v1H5z"/>',
    archive: '<path d="M7 1.6h1v1H7zm1 1h1v1H8zm-1 1h1v1H7zm1 1h1v1H8zm-1 1h1v1H7zm-.6 2.4h3.2v4H6.4zm.9 1.2v1.4h1.4V8.2z" fill-rule="evenodd"/>',
    program: '<path d="M4.5 7.2h7v5.6h-7zm1 1.8v2.8h5V9z" fill-rule="evenodd"/>',
    disc: '<path d="M8 6.8a3 3 0 1 1 0 6 3 3 0 0 1 0-6zm0 2.2a.8.8 0 1 0 0 1.6.8.8 0 0 0 0-1.6z" fill-rule="evenodd"/>',
    file: '',
  };
  return Object.fromEntries(Object.entries(marks).map(([k, m]) =>
    [k, `<svg class="ico f-${k}" viewBox="0 0 16 16" aria-hidden="true">${page}<g class="mark">${m}</g></svg>`]));
})();

// The icon of a file (its name or path), or of a folder.
function itemIcon(name, dir) {
  return dir ? FOLDER_ICON : FILE_ICONS[iconKind(name.slice(name.lastIndexOf('/') + 1))];
}

// ---- filters: file types and sizes ---------------------------------------------------------

const FILTER_LABEL = {
  folder: 'folders', video: 'video', audio: 'audio', image: 'images', text: 'text', document: 'documents',
  archive: 'archives', program: 'programs', disc: 'disc images', file: 'other files',
};

const filtersOn = () => state.filters.kinds.size > 0 || state.filters.min !== null || state.filters.max !== null;

// A size as typed: 700M, 700 MB, 1.5G, 20GiB, 500B (K/M/G/T are 1024-based); a bare number is
// MiB. null for anything else. The same rules as the command's --min-size (src/file_type.cpp).
function parseSize(text) {
  let s = text.replace(/\s+/g, '').toLowerCase();
  if (!s) return null;
  let unit = 2 ** 20;
  if (s.endsWith('ib')) {
    s = s.slice(0, -2);
    if (!/[kmgt]$/.test(s)) return null;
  } else if (s.endsWith('b')) {
    s = s.slice(0, -1);
    if (!s) return null;
    if (!/[kmgt]$/.test(s)) unit = 1;
  }
  const m = /^(\d+(?:\.\d*)?|\.\d+)([kmgt])?$/.exec(s);
  if (!m) return null;
  if (m[2]) unit = 1024 ** ('kmgt'.indexOf(m[2]) + 1);
  return Math.round(Number(m[1]) * unit);
}

// Whether a row shows. Types and sizes pick files; folders show with Folders, or with no type
// picked, and always in a folder view (`inFolder`), to keep moving through them. A file of
// unknown size doesn't show while a size bound is set.
function passesFilters(h, inFolder) {
  const f = state.filters;
  if (h.server) return true;
  if (h.dir) return inFolder || f.kinds.size === 0 || f.kinds.has('folder');
  if (f.kinds.size && !f.kinds.has(iconKind(h.name))) return false;
  if (f.min !== null || f.max !== null) {
    if (typeof h.size !== 'number') return false;
    if (f.min !== null && h.size < f.min) return false;
    if (f.max !== null && h.size > f.max) return false;
  }
  return true;
}
const applyFilters = (list, inFolder) => (filtersOn() ? list.filter((h) => passesFilters(h, inFolder)) : list.slice());

// "video or audio · at least 1.0 GiB"
function filtersText() {
  const f = state.filters;
  const parts = [];
  if (f.kinds.size) parts.push([...f.kinds].map((k) => FILTER_LABEL[k]).join(' or '));
  if (f.min !== null && f.max !== null) parts.push(`${fmtSize(f.min)} to ${fmtSize(f.max)}`);
  else if (f.min !== null) parts.push(`at least ${fmtSize(f.min)}`);
  else if (f.max !== null) parts.push(`at most ${fmtSize(f.max)}`);
  return parts.join(' · ');
}

// The list again with the filters as they are now; the selection and cursor stay on their rows.
function refilter() {
  if (!state.browse.under) {
    showFolder({ keep: true });
  } else {
    const picked = new Set([...state.selected].map((i) => state.hits[i] && rowKey(state.hits[i])));
    const cursorKey = state.hits[state.cursor] ? rowKey(state.hits[state.cursor]) : null;
    state.hits = sortRows(applyFilters(state.results, false));
    state.selected = new Set();
    state.hits.forEach((h, i) => { if (picked.has(rowKey(h))) state.selected.add(i); });
    const c = cursorKey ? state.hits.findIndex((h) => rowKey(h) === cursorKey) : -1;
    state.cursor = c >= 0 ? c : (state.hits.length ? 0 : -1);
    state.anchor = state.cursor;
    renderRows();
  }
  renderFilters();
}

// The button, the panel's chips and fields, and the strip above the list.
function renderFilters() {
  const f = state.filters;
  const n = f.kinds.size + (f.min !== null || f.max !== null ? 1 : 0);
  const btn = $('#filters-btn');
  btn.innerHTML = n ? `Filters<span class="f-count">${n}</span>` : 'Filters';
  btn.setAttribute('aria-pressed', String(n > 0));
  btn.title = n ? `Showing only ${filtersText()}` : 'Show only some file types or sizes';
  for (const chip of document.querySelectorAll('#f-kinds .chip')) {
    chip.setAttribute('aria-pressed', String(f.kinds.has(chip.dataset.kind)));
  }
  for (const chip of document.querySelectorAll('#f-presets .chip')) {
    const on = (parseSize(chip.dataset.min) ?? null) === f.min && (parseSize(chip.dataset.max) ?? null) === f.max;
    chip.setAttribute('aria-pressed', String(on && n > 0));
  }
  for (const [id, text] of [['#f-min', f.minText], ['#f-max', f.maxText]]) {
    const input = $(id);
    if (document.activeElement !== input) input.value = text;
    input.classList.toggle('bad', !!input.value.trim() && parseSize(input.value) === null);
  }
  renderFilterStrip();
}

function renderFilterStrip() {
  const strip = $('#filter-strip');
  const on = filtersOn() && (!!state.browse.under || !!state.browse.server);
  strip.hidden = !on;
  if (!on) return;
  strip.innerHTML = `<span class="what">Showing ${state.hits.length} of ${state.filterTotal} · ${esc(filtersText())}</span>` +
    '<button type="button" class="link" data-act="clear-filters">Clear filters</button>';
}

function clearFilters() {
  state.filters = { kinds: new Set(), min: null, max: null, minText: '', maxText: '' };
  refilter();
}

function openFilters(open) {
  $('#filters').hidden = !open;
  $('#filters-btn').setAttribute('aria-expanded', String(open));
  if (open) {
    showRecent(false);
    renderFilters();
  }
}

// A size field typed: applied when it reads as a size (or is empty); red otherwise.
function onSizeInput(which) {
  const input = $(which === 'min' ? '#f-min' : '#f-max');
  const text = input.value.trim();
  const bytes = text ? parseSize(text) : null;
  input.classList.toggle('bad', !!text && bytes === null);
  if (text && bytes === null) return;
  state.filters[which] = bytes;
  state.filters[`${which}Text`] = text;
  refilter();
}

// ---- recent searches -----------------------------------------------------------------------

const RECENT_MAX = 20;

const recentKey = (e) => [e.q, e.remote, e.server, e.top ? '' : e.path, e.exact].join('\u0001');

// A search that runs (Enter, or a recent one again) goes to the top, with where it ran and the
// filters on at the time.
function recordRecent(query) {
  const b = state.browse;
  const f = state.filters;
  const e = {
    q: query, remote: state.remote, server: b.server, path: b.top ? '' : b.path, top: !b.server || b.top,
    exact: exactOn(), kinds: [...f.kinds], min: f.min, max: f.max, minText: f.minText, maxText: f.maxText,
  };
  const key = recentKey(e);
  state.recent = [e, ...state.recent.filter((r) => recentKey(r) !== key)].slice(0, RECENT_MAX);
  send({ type: 'setRecent', items: state.recent });
}

// "Main › downloads/tv · Exact · video, at least 1.0 GiB"
function recentWhere(e) {
  const where = e.server ? (e.top || !e.path ? e.server : `${e.server} › ${e.path}`) : 'All servers';
  const extra = [];
  if (e.exact) extra.push('Exact');
  const kinds = (e.kinds || []).map((k) => FILTER_LABEL[k]).filter(Boolean);
  if (kinds.length) extra.push(kinds.join(' or '));
  if (typeof e.min === 'number' && typeof e.max === 'number') extra.push(`${fmtSize(e.min)} to ${fmtSize(e.max)}`);
  else if (typeof e.min === 'number') extra.push(`≥ ${fmtSize(e.min)}`);
  else if (typeof e.max === 'number') extra.push(`≤ ${fmtSize(e.max)}`);
  return [where, ...extra].join(' · ');
}

function showRecent(open) {
  const box = $('#recent');
  if (open && !state.recent.length) open = false;
  box.hidden = !open;
  if (!open) {
    state.recentSel = -1;
    return;
  }
  $('#filters').hidden = true;
  $('#filters-btn').setAttribute('aria-expanded', 'false');
  box.innerHTML = '<div class="r-head"><span>Recent searches</span></div>' + state.recent.map((e, i) =>
    `<div class="opt${i === state.recentSel ? ' on' : ''}" role="option" data-i="${i}" title="${esc(`${e.q} — ${recentWhere(e)}`)}">` +
    `<span class="r-q">${esc(e.q)}</span><span class="r-where">${esc(recentWhere(e))}</span>` +
    `<button type="button" class="r-del" data-del="${i}" title="Remove from recent searches" aria-label="Remove">×</button></div>`).join('') +
    '<div class="r-foot"><span></span><button type="button" class="link" data-act="clear-recent">Clear history</button></div>';
}

// Runs a recent search again where it ran: that server and folder, Exact and the filters.
function runRecent(e) {
  showRecent(false);
  const known = e.remote === ALL ? state.remotes.length > 1 : state.remotes.some((r) => r.name === e.remote);
  if (!known || (e.server && !state.remotes.some((r) => r.name === e.server))) {
    showBanner(`“${e.q}” was searched on a server that isn't configured any more.`);
    return;
  }
  state.filters = {
    kinds: new Set((e.kinds || []).filter((k) => FILTER_LABEL[k])),
    min: typeof e.min === 'number' ? e.min : null, max: typeof e.max === 'number' ? e.max : null,
    minText: e.minText || '', maxText: e.maxText || '',
  };
  $('#exact').setAttribute('aria-pressed', String(!!e.exact));
  renderFilters();
  if (e.remote !== state.remote) pickRemote(e.remote, false);
  const b = state.browse;
  const here = b.inited && !b.loading && (b.server || '') === (e.server || '') &&
    (!e.server || (b.top ? e.top : !e.top && b.path === e.path));
  state.pendingSearch = e.q;
  if (here) {
    runPendingSearch(true);
    return;
  }
  browseTo(e.server || '', e.path || '', !!e.top);
  if (!state.browse.loading) runPendingSearch(true); // All servers' top opens at once
}

function runPendingSearch(ok) {
  const q = state.pendingSearch;
  state.pendingSearch = null;
  if (!ok || !q) return;
  $('#query').value = q;
  renderSearchBox();
  searchBelow(q);
}

function removeRecent(i) {
  state.recent.splice(i, 1);
  send({ type: 'setRecent', items: state.recent });
  if (state.recentSel >= state.recent.length) state.recentSel = state.recent.length - 1;
  showRecent(state.recent.length > 0);
  $('#query').focus();
}

// Sets an icon element's SVG, only when it changes (queue rows are updated in place often).
function setIcon(el, name, dir) {
  const key = dir ? ':dir' : iconKind(name.slice(name.lastIndexOf('/') + 1));
  if (el.dataset.k === key) return;
  el.dataset.k = key;
  el.innerHTML = itemIcon(name, dir);
}

// `path` without its `folder` prefix, for search results below a folder ('' for the folder itself).
function relativeTo(path, folder) {
  if (path === folder) return '';
  if (!folder) return path;
  const base = folder.endsWith('/') ? folder : `${folder}/`;
  return path.startsWith(base) ? path.slice(base.length) : path;
}

// The folder's name for messages: "movies", the server at its top, "All servers".
function browseHere() {
  const b = state.browse;
  if (!b.server) return 'All servers';
  return b.crumbs.length ? b.crumbs[b.crumbs.length - 1].name : b.server;
}

// The folder's rows: its items, filtered by the words typed (every word, in any order).
function filteredItems() {
  const words = $('#query').value.trim().toLowerCase().split(/\s+/).filter(Boolean);
  const named = !words.length ? state.browse.items : state.browse.items.filter((it) => {
    const name = it.name.toLowerCase();
    return words.every((w) => name.includes(w));
  });
  state.filterTotal = named.length;
  return applyFilters(named, true);
}

// Shows the folder in the results table. `keep`: the selection and cursor stay on the same
// items (a filter change, a refreshed list); `select`: the item with that path (or server)
// gets the cursor.
function showFolder({ keep = false, select = null } = {}) {
  const picked = keep ? new Set([...state.selected].map((i) => state.hits[i] && rowKey(state.hits[i]))) : new Set();
  const cursorKey = keep && state.hits[state.cursor] ? rowKey(state.hits[state.cursor]) : null;
  const shown = keep && select === null && cursorShown();
  state.hits = sortRows(filteredItems());
  state.selected = new Set();
  state.hits.forEach((h, i) => { if (picked.has(rowKey(h))) state.selected.add(i); });
  let cursor = cursorKey ? state.hits.findIndex((h) => rowKey(h) === cursorKey) : -1;
  if (select !== null) {
    const i = state.hits.findIndex((h) => (h.server ? h.server === select : h.path === select));
    if (i >= 0) {
      cursor = i;
      state.selected = new Set([i]);
    }
  }
  state.cursor = cursor >= 0 ? cursor : (state.hits.length ? 0 : -1);
  state.anchor = state.cursor;
  const folders = state.hits.filter((h) => h.dir).length;
  const files = state.hits.length - folders;
  const count = (n, one) => `${n} ${one}${n === 1 ? '' : 's'}`;
  state.resultNote = state.browse.server ? `${count(folders, 'folder')}, ${count(files, 'file')}` : '';
  const list = $('#results');
  if (state.cursor >= 0 && select !== null) {
    list.scrollTop = Math.max(0, state.cursor * ROW - list.clientHeight / 2);
  } else if (!keep) {
    list.scrollTop = 0;
  }
  renderRows();
  if (shown && state.cursor >= 0) keepInView(state.cursor);
}

// Opens a folder: `server` '' is All servers' own top; `top` a server's top. The answer comes
// as "browseResult"; the old rows stay until then.
// `history`: a move Back or Forward (not a new place), with the `scroll` the place had.
function browseTo(server, path, top, { select = null, live = false, keep = false, history = false, scroll = null } = {}) {
  const b = state.browse;
  // Another place: the words in the search box were for the last one, as in Explorer.
  const elsewhere = server !== b.server || (top || !server ? !b.top : b.top || path !== b.path);
  if (elsewhere) $('#query').value = '';
  if (!history) navTo({ remote: state.remote, server, path: top ? '' : path, top: !server || top });
  b.inited = true;
  if (b.under) {
    stopSearch();
    state.lists = []; // the search's sources: the footer shows the folder's own from now on
    renderListStatus();
  }
  b.under = null;
  b.typed = null;
  b.pendingScroll = scroll;
  state.addr.error = '';
  state.addr.unpin = null;
  if (state.addr.editing) endEdit(false);
  if (!server) {
    Object.assign(b, { server: '', path: '', top: true, crumbs: [], error: '', loading: false, source: '', uploadable: false });
    b.items = state.remotes.map((r, i) => ({ name: r.name, path: '', dir: true, server: r.name, remote: r.name,
      info: r.error ? 'misconfigured' : `${r.user}@${r.host}`, ord: i }));
    remember();
    showFolder({ select });
    navShown();
    renderBrowseBar();
    return;
  }
  b.id += 1;
  const sameFolder = b.server === server && b.path === path && b.top === top;
  if (b.server !== server || top) b.crumbs = []; // the bar shows the new place's path when it opens
  Object.assign(b, { server, path, top, loading: true, error: '', pendingSelect: select, keepSelection: keep || (live && sameFolder) });
  renderBrowseBar();
  renderEmpty();
  send({ type: 'browse', id: b.id, remote: server, path, top, live });
}

// Where browse is, to reopen there while the window runs.
function remember() {
  const b = state.browse;
  if (state.remote === ALL) state.browsePaths[ALL] = b.server;
  if (b.server) {
    if (b.top) delete state.browsePaths[b.server];
    else state.browsePaths[b.server] = b.path;
  }
  // For a page opened again by the window (onPageRestored): it outlives the page, not grab.
  try { sessionStorage.setItem('grab.paths', JSON.stringify(state.browsePaths)); } catch (e) { /* no storage */ }
}

// Browse mode starts where it last was for the server picked in this run (All servers: in the
// server it had open, else its list of servers); the first time, at the server's top.
function enterBrowse() {
  const all = state.remote === ALL;
  const server = all ? (state.browsePaths[ALL] || '') : state.remote;
  if (!server || !state.remotes.some((r) => r.name === server)) { browseTo('', '', true); return; }
  const path = state.browsePaths[server];
  browseTo(server, path === undefined ? '' : path, path === undefined);
}

function onBrowseResult(msg) {
  const b = state.browse;
  if (msg.id !== b.id) return; // an older request
  const wasLoading = b.loading;
  if (msg.typed || msg.pin) {
    // An address typed in the path bar, or a pin opened (its path as saved): a failure leaves the
    // folder on screen as it was, and says why there (with Unpin when the pin's folder isn't there
    // or can't be listed: its pin dims in Quick access until it opens again).
    if (!b.typed || b.typed.server !== msg.remote) return;
    const pin = msg.pin && typeof b.typed.pin === 'string' ? b.typed.pin : null;
    b.typed = null;
    b.loading = false;
    if (pin !== null) {
      const key = goneKey(msg.remote, pin);
      if (msg.error && msg.failure) quick.gone.set(key, msg.failure === 'denied' ? 'can’t be listed' : 'not found');
      else if (!msg.error) quick.gone.delete(key);
    }
    if (msg.error) {
      state.addr.error = msg.error;
      state.addr.unpin = pin !== null && msg.failure ? { server: msg.remote, path: pin } : null;
      renderBrowseBar();
      renderSuggest();
      return;
    }
    navNote(); // the place on screen, before it goes
    if (msg.remote !== state.remote && state.remote !== ALL) pickRemote(msg.remote, false);
    navTo({ remote: state.remote, server: msg.remote, path: msg.top ? '' : msg.path, top: msg.top });
    b.server = msg.remote;
    b.pendingSelect = msg.select || null;
    b.pendingScroll = null;
    b.keepSelection = false;
    if (state.addr.editing) endEdit(document.activeElement === $('#address'));
  } else if (msg.remote !== b.server || b.typed) {
    return;
  }
  Object.assign(b, {
    loading: false, error: msg.error || '', path: msg.path, top: msg.top, crumbs: msg.crumbs || [],
    source: msg.source || '', age: msg.age || 0, refreshing: !!msg.refreshing, listError: msg.listError || '',
    severalRoots: !!msg.severalRoots, uploadable: !!msg.uploadable && !msg.error,
  });
  // `ord`: the backend's order (folders first, A to Z), the default one.
  b.items = (msg.items || []).map((it, i) => ({ ...it, remote: msg.remote, ord: i }));
  // Read-only or not, once a session per server: the menus grey what it refuses.
  if (!msg.error) askAccount(msg.remote);
  if (!b.under) {
    showFolder({ keep: msg.update || b.keepSelection, select: b.pendingSelect });
    if (!msg.error && wasLoading) remember();
    navShown();
    if (state.pendingSearch && wasLoading) runPendingSearch(!msg.error);
  }
  b.pendingSelect = null;
  b.keepSelection = false;
  renderBrowseBar();
}

// ---- back and forward: the places Browse opened -------------------------------------------

const NAV_MAX = 50; // places kept behind

function samePlace(a, b) {
  return !!a && !!b && a.remote === b.remote && a.server === b.server && a.top === b.top && (a.top || a.path === b.path);
}

// Notes the row picked and the scroll of the place on screen, to come back to them. Not while a
// search's results below it show, nor when its rows were just cleared (a server switch).
function navNote() {
  const c = state.nav.cur;
  if (!c || !c.shown || state.browse.under || !state.hits.length) return;
  const h = state.hits[state.cursor];
  c.cursorKey = h ? (h.server || h.path) : null;
  c.scroll = $('#results').scrollTop;
}

// A new place opens (not by Back or Forward): the one before goes on the back list, and Forward
// empties. The same place again (Refresh, back to Browse from Files) changes nothing.
function navTo(place) {
  const n = state.nav;
  if (samePlace(n.cur, place)) return;
  navNote();
  if (n.cur) {
    n.back.push(n.cur);
    if (n.back.length > NAV_MAX) n.back.shift();
  }
  n.fwd = [];
  n.cur = { ...place, cursorKey: null, scroll: null, shown: false };
}

// The place opened is on screen: its row and scroll can be noted from now on. A move Back or
// Forward gets its scroll back (and its row, picked when it opened).
function navShown() {
  const b = state.browse;
  const c = state.nav.cur;
  if (c && samePlace(c, { remote: state.remote, server: b.server, path: b.path, top: b.top })) c.shown = true;
  if (b.pendingScroll != null) {
    $('#results').scrollTop = b.pendingScroll;
    if (state.cursor >= 0) keepInView(state.cursor);
    renderRows();
  }
  b.pendingScroll = null;
}

// Back (-1) or Forward (+1). Back first closes the results of a search, as Esc does; Forward
// closes them and goes on. Another server's place switches the server picker to it; a place on
// a server no longer configured is skipped.
function navGo(dir) {
  if (settings.open || modal) return;
  if (viewer.open) { // Back (or Forward) leaves the viewer first
    closeViewer();
    return;
  }
  const n = state.nav;
  if (state.browse.under) {
    closeBelow();
    if (dir < 0) return;
  }
  const from = dir < 0 ? n.back : n.fwd;
  const to = dir < 0 ? n.fwd : n.back;
  let place = null;
  while (from.length && !place) {
    const p = from.pop();
    if (!p.server || state.remotes.some((r) => r.name === p.server)) place = p;
  }
  if (!place) {
    renderBrowseBar();
    return;
  }
  navNote();
  if (n.cur) to.push(n.cur);
  n.cur = { ...place, shown: false };
  if (place.remote !== state.remote) pickRemote(place.remote, false);
  browseTo(place.server, place.path, place.top, { select: place.cursorKey, history: true, scroll: place.scroll });
}

// A place's name for the buttons' hints: the folder, the server at its top, "All servers"; with
// the server added when it isn't the one on screen.
function placeName(p) {
  if (!p.server) return 'All servers';
  const name = p.top ? p.server : (p.path.split('/').filter(Boolean).pop() || '/');
  return p.server === state.browse.server || p.top ? name : `${name} on ${p.server}`;
}

function openRow(h) {
  if (h.server) browseTo(h.server, '', true);
  else if (h.dir) browseTo(state.browse.server || h.remote, h.path, false);
}

function browseUp() {
  const b = state.browse;
  if (b.under) { closeBelow(); return; }
  if (!b.server) return;
  if (b.top) {
    if (state.remote === ALL) browseTo('', '', true, { select: b.server });
    return;
  }
  const c = b.crumbs;
  if (c.length >= 2) browseTo(b.server, c[c.length - 2].path, false, { select: b.path });
  else browseTo(b.server, '', true, { select: b.path });
}

// A search result's folder: it opens with the result picked, and the results close, with their
// words.
function openHitFolder(h) {
  const server = h.remote || state.remote;
  if (state.browse.under) {
    stopSearch();
    state.lists = [];
    renderListStatus();
    $('#query').value = '';
  }
  if (state.remote === ALL) state.browsePaths[ALL] = server;
  browseTo(server, parentOf(h.path), false, { select: h.path });
}

// Enter in the search box: the folder and everything below it, folders and files, from the
// server's list (or live, for a folder deeper than the list). At All servers' top that is every
// server, and at the top of a server with several search roots, all of them (`whole`: no one
// folder holds them).
function searchBelow(query) {
  const b = state.browse;
  const all = !b.server;
  const whole = all || (b.top && b.severalRoots);
  showBanner('');
  b.under = { query, path: b.path, all, whole, name: browseHere() };
  state.addr.error = ''; // a failed address is behind us
  state.addr.unpin = null;
  recordRecent(query);
  state.results = [];
  state.filterTotal = 0;
  state.searchId += 1;
  state.lastQuery = query;
  state.searchStarted = performance.now();
  state.hits = [];
  state.selected.clear();
  state.cursor = state.anchor = -1;
  state.lists = [];
  renderListStatus();
  renderBrowseBar();
  setSearching(true);
  renderRows();
  const msg = { type: 'search', id: state.searchId, remote: all ? ALL : b.server, query, mode: 'any', exact: exactOn() };
  if (!whole) msg.under = b.path;
  send(msg);
}

// Back from search results to the folder.
function closeBelow() {
  const b = state.browse;
  if (!b.under) return;
  stopSearch();
  b.under = null;
  state.lists = [];
  renderListStatus();
  $('#query').value = '';
  showFolder();
  renderBrowseBar();
}

function renderBrowseBar() {
  const a = state.addr;
  $('#crumbs').hidden = a.editing;
  $('#browse-blank').hidden = a.editing;
  $('#address-box').hidden = !a.editing;
  const b = state.browse;
  // The path: [All servers ›] server › folder › folder; every step but the last opens.
  const steps = [];
  if (state.remote === ALL) steps.push({ name: 'All servers', server: '' });
  if (b.server) {
    steps.push({ name: b.server, server: b.server, top: true });
    for (const c of b.crumbs) steps.push({ name: c.name, server: b.server, path: c.path });
  }
  // Search results end the path, as in Explorer.
  if (b.under) steps.push({ name: `Search results for “${b.under.query}”`, results: true });
  b.steps = steps;
  // `data-menu`: each step's right-click menu (crumbMenu).
  $('#crumbs').innerHTML = steps.map((s, i) => (s.results
    ? `<button type="button" class="crumb here results" data-edit data-menu="${i}" tabindex="-1">${esc(s.name)}</button>`
    : i === steps.length - 1
    ? `<button type="button" class="crumb here" data-edit data-menu="${i}" tabindex="-1" title="${esc(s.path ?? s.name)}">${esc(s.name)}</button>`
    : `<button type="button" class="crumb" data-step="${i}" data-menu="${i}" title="${esc(s.path ?? s.name)}">${esc(s.name)}</button>`))
    .join('<span class="sep">›</span>');
  $('#browse-up').disabled = !b.under && (!b.server || (b.top && state.remote !== ALL));
  const n = state.nav;
  const back = $('#browse-back');
  const fwd = $('#browse-fwd');
  back.disabled = !b.under && !n.back.length;
  back.title = b.under ? 'Back to the folder (Alt+←)'
    : n.back.length ? `Back to ${placeName(n.back[n.back.length - 1])} (Alt+←)` : 'Back (Alt+←)';
  fwd.disabled = !n.fwd.length;
  fwd.title = n.fwd.length ? `Forward to ${placeName(n.fwd[n.fwd.length - 1])} (Alt+→)` : 'Forward (Alt+→)';
  $('#browse-refresh').hidden = !b.server || !!b.under;
  $('#browse-refresh').disabled = b.loading;
  renderSearchBox();
  renderQuick();
  // Where the folder's rows came from, in the footer (search results have their own, beside it).
  // An address (or pin) that didn't open says why here once the field is closed (in its list
  // while it is open), until Browse goes somewhere.
  const status = $('#browse-status');
  // A long error shrinks on its own: the Unpin link after it stays in sight.
  const unpinLink = a.error && !b.under && !a.editing && !!a.unpin && !(state.flash && state.flash.until > Date.now());
  status.classList.toggle('has-link', unpinLink);
  if (state.flash && state.flash.until > Date.now()) {
    status.textContent = bidiVisible(state.flash.text);
  } else if (a.error && !b.under && !a.editing) {
    status.innerHTML = `<span class="bad" title="${esc(a.error)}">${esc(a.error)}</span>` +
      (a.unpin ? `<span class="tail">${UNPIN_GONE}</span>` : '');
  } else if (b.under) {
    status.textContent = '';
  } else if (b.loading) {
    status.textContent = ''; // a folder listed from the server shows as an activity
  } else if (!b.server) {
    status.textContent = '';
  } else {
    const parts = [];
    if (b.source === 'list') parts.push(`from the file list, ${fmtAge(b.age)} old`);
    else if (b.source === 'live') parts.push(b.age < 5 ? 'listed from the server' : `listed from the server ${fmtAge(b.age)} ago`);
    if (b.refreshing) parts.push('refreshing the list');
    status.innerHTML = (b.refreshing ? '<span class="spinner"></span>' : '') + esc(parts.join(' · ')) +
      (b.listError ? ` · <span class="bad" title="${esc(b.listError)}">list refresh failed</span>` : '');
  }
  status.title = status.textContent; // in full, when a narrow window cuts it short
}

// ---- the address bar: the path bar turned into a field ------------------------------------

// The place shown, as an address: the folder's path ('' at a server's top); in All servers
// "Server:path".
function currentAddress() {
  const b = state.browse;
  const path = b.top ? '' : b.path;
  if (state.remote !== ALL) return path;
  return b.server ? `${b.server}:${path}` : '';
}

// A configured server's name, matched without case; '' when there is none.
function findRemote(name) {
  const n = name.trim().toLowerCase();
  const r = state.remotes.find((x) => x.name.toLowerCase() === n);
  return r ? r.name : '';
}

// The server and path a typed address names: "Server:path" when the part before the first ':'
// is a server's name (so "Star Wars: x" stays a path), else a path on the server on screen.
function parseAddress(text) {
  const t = text.trim();
  const colon = t.indexOf(':');
  const named = colon > 0 ? findRemote(t.slice(0, colon)) : '';
  if (named) return { server: named, path: t.slice(colon + 1).trim() };
  return { server: state.remote === ALL ? state.browse.server : state.remote, path: t };
}

// A click on the path bar's empty part or its current folder, Ctrl+L, Alt+D or F4: the path as
// text, all of it selected, and Quick access's pins listed under it.
function startEdit() {
  if (settings.open || modal) return;
  const a = state.addr;
  const input = $('#address');
  closePins();
  if (!a.editing) {
    Object.assign(a, { editing: true, typed: '', field: currentAddress(), shown: [], sel: -1, cache: new Map(), want: '', error: '', unpin: null });
    input.value = a.field;
    const example = state.remotes.length ? state.remotes[0].name : 'Main';
    input.placeholder = state.remote === ALL ? `A server and a path, e.g. ${example}:downloads`
      : `Type a path on ${state.remote}`;
    renderBrowseBar();
    updateSuggest();
  }
  input.focus();
  input.select();
}

// The path bar again; `focus`: the results take the keyboard (Esc, a folder opened). An error
// shown in the field's list goes to the footer (PINS.md 3.10), as one for an address still being
// opened does when it comes.
function endEdit(focus = true) {
  const a = state.addr;
  if (!a.editing) return;
  Object.assign(a, { editing: false, shown: [], sel: -1 });
  renderSuggest();
  renderBrowseBar();
  if (focus) $('#results').focus();
}

// Enter: opens the typed address. A server's top opens at once; a path goes to the backend,
// which also opens a file's folder with it picked. The folder on screen stays until the answer.
function goAddress() {
  const a = state.addr;
  const { server, path } = parseAddress($('#address').value);
  a.shown = [];
  a.sel = -1;
  renderSuggest();
  if (!server) {
    const example = state.remotes.length ? state.remotes[0].name : 'Main';
    a.error = `Start with a server's name, e.g. ${example}:downloads`;
    a.unpin = null;
    renderBrowseBar();
    renderSuggest();
    return;
  }
  const b = state.browse;
  if (b.under) closeBelow();
  if (!path) {
    endEdit();
    if (server !== state.remote && state.remote !== ALL) pickRemote(server, false);
    browseTo(server, '', true);
    return;
  }
  a.error = '';
  a.unpin = null;
  renderSuggest();
  b.id += 1;
  b.typed = { server };
  b.loading = true;
  renderBrowseBar();
  send({ type: 'browse', id: b.id, remote: server, path, typed: true });
}

const SUGGEST_MAX = 12;   // rows in the address field's list
const SUGGEST_PINS = 5;   // pins among them once something is typed

// Quick access's pins for the address field's list, as Quick access shows them: all of them (up
// to SUGGEST_MAX) while nothing is typed; else those whose name or path holds the text, ignoring
// case (up to SUGGEST_PINS). In All servers (or for another server's pin) as "Server:path".
function addressPins(text) {
  const t = text.trim().toLowerCase();
  const list = pinRows().map((r) => {
    const where = state.remote === ALL || r.server !== state.remote ? `${r.server}:${r.path}` : r.path;
    return { label: r.name, text: where, where, pin: { server: r.server, path: r.path } };
  });
  if (!t) return list.slice(0, SUGGEST_MAX);
  return list.filter((s) => [s.label, s.pin.path, s.text].some((x) => x.toLowerCase().includes(t))).slice(0, SUGGEST_PINS);
}

// The suggestions for the text typed: Quick access's pins first (addressPins), then the folders of
// its parent that start with its last part (from the list and this session's live listings, asked
// once per folder), up to 12 in all; in All servers' own top, the servers.
function updateSuggest() {
  const a = state.addr;
  a.sel = -1;
  a.shown = [];
  const text = a.typed;
  const pinned = addressPins(text);
  if (!text.trim()) {
    a.shown = pinned;
    renderSuggest();
    return;
  }
  const colon = text.indexOf(':');
  const named = colon > 0 ? findRemote(text.slice(0, colon)) : '';
  if (!named && state.remote === ALL && !state.browse.server) {
    const word = text.trim().toLowerCase();
    a.shown = [...pinned, ...state.remotes.filter((r) => r.name.toLowerCase().startsWith(word))
      .map((r) => ({ label: r.name, text: `${r.name}:`, server: true }))].slice(0, SUGGEST_MAX);
    renderSuggest();
    return;
  }
  const server = named || (state.remote === ALL ? state.browse.server : state.remote);
  const rest = (named ? text.slice(colon + 1) : text).trimStart();
  const slash = rest.lastIndexOf('/');
  const parent = rest.slice(0, slash + 1);
  const word = rest.slice(slash + 1).toLowerCase();
  if (!server || (slash < 0 && !word)) {
    a.shown = pinned;
    renderSuggest();
    return;
  }
  const key = `${server}\n${parent}`;
  a.want = key;
  if (!a.cache.has(key)) {
    a.cache.set(key, null);
    send({ type: 'suggest', remote: server, path: parent });
  }
  const prefix = named ? `${named}:` : '';
  a.shown = [...pinned, ...(a.cache.get(key) || []).filter((n) => n.toLowerCase().startsWith(word)).slice(0, SUGGEST_MAX - pinned.length)
    .map((n) => ({ label: n, text: `${prefix}${parent}${n}` }))];
  renderSuggest();
}

function onSuggestions(msg) {
  const a = state.addr;
  const key = `${msg.remote}\n${msg.path}`;
  if (!a.cache.has(key)) return; // asked during an earlier edit
  a.cache.set(key, msg.folders || []);
  if (a.editing && a.want === key && a.sel < 0) updateSuggest();
  // The server's list is being fetched: the next keystroke asks again.
  if (msg.later) a.cache.delete(key);
}

// The field's list: the pins under "Quick access", the folders (or servers), and why the last
// address or pin didn't open (PINS.md 3.10: the field keeps the focus after Enter, so it shows
// here; the footer shows it once the field closes).
function renderSuggest() {
  const a = state.addr;
  const box = $('#suggest');
  const input = $('#address');
  const why = a.editing && !!a.error;
  const open = a.editing && (a.shown.length > 0 || why);
  box.hidden = !open;
  input.setAttribute('aria-expanded', String(open));
  if (why) input.setAttribute('aria-describedby', 'address-why');
  else input.removeAttribute('aria-describedby');
  if (!open) {
    box.innerHTML = '';
    return;
  }
  const option = (s, i) => `<div class="opt${i === a.sel ? ' on' : ''}" role="option" data-i="${i}" aria-selected="${i === a.sel}"` +
    `${s.pin ? ` title="${esc(s.where)}"` : ''}>${s.server ? '' : FOLDER_ICON}` +
    `${s.pin ? `<span class="s-name">${esc(s.label)}</span><span class="s-path">${esc(s.where)}</span>` : `<span>${esc(s.label)}</span>`}</div>`;
  const pins = a.shown.filter((s) => s.pin).length; // first
  box.innerHTML = (pins ? '<div role="group" aria-labelledby="suggest-qa"><div class="s-head" id="suggest-qa">Quick access</div>' +
    `${a.shown.slice(0, pins).map(option).join('')}</div>` : '') +
    a.shown.slice(pins).map((s, k) => option(s, pins + k)).join('') +
    (why ? `<div class="err" id="address-why" role="alert">${esc(a.error)}${a.unpin ? UNPIN_GONE : ''}</div>` : '');
  const on = box.querySelector('.on');
  if (on) on.scrollIntoView({ block: 'nearest' });
}

$('#browse-bar').addEventListener('click', (ev) => {
  if (state.addr.editing) return;
  const t = ev.target;
  if (t.closest('[data-edit], .address-ico') || ['crumbs', 'browse-blank', 'browse-bar'].includes(t.id) ||
    t.classList.contains('sep')) {
    startEdit();
  }
});
$('#address').addEventListener('input', () => {
  const a = state.addr;
  a.typed = $('#address').value;
  a.field = a.typed;
  if (a.error) {
    a.error = '';
    a.unpin = null;
    renderBrowseBar();
  }
  updateSuggest();
});
$('#address').addEventListener('keydown', (ev) => {
  const a = state.addr;
  const input = $('#address');
  if (ev.key === 'Enter') {
    // A pin highlighted opens as saved; anything else as typed.
    ev.preventDefault();
    const s = a.shown[a.sel];
    if (s && s.pin) openPin(s.pin.server, s.pin.path);
    else goAddress();
  } else if (ev.key === 'Escape') {
    // The suggestions close first (the field's own text comes back), then the field.
    ev.preventDefault();
    ev.stopPropagation();
    if (a.shown.length) {
      if (a.sel >= 0) input.value = a.field;
      a.shown = [];
      a.sel = -1;
      renderSuggest();
    } else {
      endEdit();
    }
  } else if ((ev.key === 'ArrowDown' || ev.key === 'ArrowUp') && a.shown.length) {
    // Through the suggestions and back to the field's own text; the field shows the one highlighted.
    ev.preventDefault();
    const n = a.shown.length;
    if (ev.key === 'ArrowDown') a.sel = a.sel + 1 >= n ? -1 : a.sel + 1;
    else a.sel = a.sel < 0 ? n - 1 : a.sel - 1;
    input.value = a.sel >= 0 ? a.shown[a.sel].text : a.field;
    renderSuggest();
  } else if (ev.key === 'Tab' && !ev.shiftKey && !ev.ctrlKey && !ev.altKey && a.shown.length) {
    // Completes the highlighted suggestion (a pin only when highlighted), else the first folder or
    // server, and goes on into it; with none, Tab leaves the field as usual.
    const pick = a.sel >= 0 ? a.shown[a.sel] : a.shown.find((s) => !s.pin);
    if (!pick) return;
    ev.preventDefault();
    input.value = pick.server ? pick.text : `${pick.text}/`;
    a.typed = input.value;
    a.field = a.typed;
    updateSuggest();
  }
});
$('#address').addEventListener('blur', () => endEdit(false));
$('#suggest').addEventListener('mousedown', (ev) => ev.preventDefault()); // the field keeps the focus
$('#suggest').addEventListener('click', (ev) => {
  if (ev.target.closest('[data-act="unpin-gone"]')) {
    unpinGone();
    return;
  }
  const o = ev.target.closest('[data-i]');
  if (!o) return;
  const s = state.addr.shown[Number(o.dataset.i)];
  if (s.pin) {
    openPin(s.pin.server, s.pin.path);
    return;
  }
  $('#address').value = s.text;
  goAddress();
});
$('#browse-status').addEventListener('click', (ev) => {
  if (ev.target.closest('[data-act="unpin-gone"]')) unpinGone();
});

$('#crumbs').addEventListener('click', (ev) => {
  const btn = ev.target.closest('[data-step]');
  if (!btn) return;
  $('#results').focus(); // the crumbs are drawn again: the keyboard goes on in the folder
  const s = state.browse.steps[Number(btn.dataset.step)];
  if (!s.server) browseTo('', '', true, { select: state.browse.server });
  else if (s.top) browseTo(s.server, '', true);
  else browseTo(s.server, s.path, false);
});
// A crumb's own menu, by the mouse, the menu key or Shift+F10 (on a crumb that has the focus).
$('#crumbs').addEventListener('contextmenu', (ev) => {
  const btn = ev.target instanceof Element ? ev.target.closest('[data-menu]') : null;
  if (!btn) return;
  ev.preventDefault();
  showMenu(ev.clientX, ev.clientY, crumbMenu(state.browse.steps[Number(btn.dataset.menu)]));
});
$('#crumbs').addEventListener('keydown', (ev) => {
  if (!((ev.key === 'F10' && ev.shiftKey) || ev.key === 'ContextMenu')) return;
  const btn = ev.target instanceof Element ? ev.target.closest('[data-menu]') : null;
  if (!btn) return;
  ev.preventDefault();
  const r = btn.getBoundingClientRect();
  showMenu(r.left, r.bottom, crumbMenu(state.browse.steps[Number(btn.dataset.menu)]), true);
});

// A crumb's menu: Quick access for a folder's (not the server's top, All servers or search
// results), its path, and the address bar to type in.
function crumbMenu(s) {
  const items = [];
  const folder = !!s && !!s.server && !s.top && !s.results && !!s.path;
  if (folder) {
    const b = state.browse;
    const here = !b.under && b.server === s.server && !b.top && b.path === s.path;
    items.push(pinEntry(s.server, s.path, here), '-', { label: 'Copy path', act: () => copyText([s.path]) });
  }
  items.push({ label: 'Edit address', key: 'Ctrl+L', act: () => startEdit() });
  return items;
}

// ---- Quick access: pinned folders per server (PINS.md, chunk 2) ------------------------------
// The Quick access button between Up and the address bar opens the pins of the server on screen
// (under All servers, every server's under its name, the one on screen first); the same pins come
// first in the address field's list. A folder row's, a folder hit's, the list's empty part's and a
// crumb's menus pin and unpin, as Ctrl+D pins the open folder (never unpins); Alt+1 to Alt+9 open
// the first nine. The backend owns the list: the page asks one change at a time (pinsAdd,
// pinsRemove, pinsRename, pinsMove) and shows what "pins" posts back, also after grab's own
// renames, moves and deletes carried pins along. Nothing asks a server anything but opening a pin,
// which goes as a typed address does: its folder exactly as saved, the window where it was until
// the answer, a failure said in the footer with Unpin beside it.

const pinsOf = (server) => (server && Array.isArray(quick.pins[server]) ? quick.pins[server] : []);
const goneKey = (server, path) => `${server}\n${path}`;

// The name a pin shows: its own, else its folder's last name (pins::shown_name).
function pinName(p) {
  if (p.name) return p.name;
  let path = p.path;
  while (path.length > 1 && path.endsWith('/')) path = path.slice(0, -1);
  return path.slice(path.lastIndexOf('/') + 1) || path;
}

// The index of `server`'s pin of `path`, -1 for none: the same spelling first, else the same folder
// as server_files::canonical makes it with the account's home (pins::find).
function pinIndex(server, path) {
  const list = pinsOf(server);
  const same = list.findIndex((p) => p.path === path);
  if (same >= 0 || !list.length) return same;
  const a = ops.accounts.get(server);
  const home = (a && a.home) || '';
  const at = serverCanonical(path, home);
  return list.findIndex((p) => serverCanonical(p.path, home) === at);
}

// The pins as the list shows them, in groups: the server picked's; under All servers each server's
// with any, the one on screen first, then the picker's order. A row: { server, path, name, own (its
// own name, '' none), i (its place in its server's list), gone (why it didn't open, '' none) }.
function pinGroups() {
  const names = state.remotes.map((r) => r.name);
  if (state.remote !== ALL) {
    return names.includes(state.remote) ? [{ server: state.remote, pins: pinRowsOf(state.remote) }] : [];
  }
  const on = state.browse.server;
  const order = names.includes(on) ? [on, ...names.filter((n) => n !== on)] : names;
  return order.map((server) => ({ server, pins: pinRowsOf(server) })).filter((g) => g.pins.length);
}

function pinRowsOf(server) {
  return pinsOf(server).map((p, i) => ({ server, path: p.path, name: pinName(p), own: p.name || '', i,
    gone: quick.gone.get(goneKey(server, p.path)) || '' }));
}

const pinRows = () => pinGroups().flatMap((g) => g.pins);

// `server`'s pins at or below `paths`, compared as server_files::canonical makes them with the
// account's home (pins::drop_below): what a delete of `paths` unpins.
function pinsBelow(server, paths) {
  const a = ops.accounts.get(server);
  const home = (a && a.home) || '';
  const gone = paths.map((p) => serverCanonical(p, home));
  return pinsOf(server).filter((p) => {
    const at = serverCanonical(p.path, home);
    return gone.some((g) => at === g || at.startsWith(g.endsWith('/') ? g : `${g}/`));
  });
}

// Why the open folder can't be pinned now, or '': one folder of one server (not a server's top,
// All servers' or search results, nor the login folder '~' of several roots, which has no path),
// opened.
function pinHereWhy() {
  const b = state.browse;
  if (!b.server || b.under || b.top || !b.path) return 'Open a folder of a server first.';
  if (b.loading) return `${browseHere()} is still opening.`;
  return '';
}

// "Pin to Quick access" or "Unpin from Quick access" for `path` of `server`, as it is now; `here`:
// the open folder's, whose Pin is Ctrl+D (which never unpins).
function pinEntry(server, path, here = false) {
  if (pinIndex(server, path) >= 0) return { label: 'Unpin from Quick access', act: () => unpinFolder(server, path) };
  const why = here ? pinHereWhy() : '';
  return { label: 'Pin to Quick access', key: here ? 'Ctrl+D' : '', disabled: !!why, why, act: () => pinFolder(server, path) };
}

// Pins `path` of `server`, last; Undo gives the pin's own `name` and its place `at` back. Said once
// the backend's list holds it ("Pinned tv to Quick access"); a folder pinned already says so.
function pinFolder(server, path, { name = '', at = null } = {}) {
  const there = pinIndex(server, path);
  if (there >= 0) {
    opsSay(`${pinName(pinsOf(server)[there])} is already in Quick access`, true);
    return;
  }
  const msg = { type: 'pinsAdd', server, path };
  if (name) msg.name = name;
  if (at !== null) msg.at = at;
  quick.want = { done: () => pinIndex(server, path) >= 0, text: `Pinned ${name || pinName({ path })} to Quick access`, act: null };
  send(msg);
}

// Ctrl+D and the footer's "Pin this folder": the open folder.
function pinHere() {
  const why = pinHereWhy();
  if (why) {
    opsSay(why, true);
    return;
  }
  pinFolder(state.browse.server, state.browse.path);
}

// Unpins without asking: "Unpinned tv · Undo" in the footer for a while, Undo pinning it back
// with its name and place.
function unpinFolder(server, path) {
  const i = pinIndex(server, path);
  if (i < 0) return;
  const p = pinsOf(server)[i];
  quick.gone.delete(goneKey(server, p.path));
  quick.want = { done: () => pinIndex(server, p.path) < 0, text: `Unpinned ${pinName(p)}`,
    act: { label: 'Undo', run: () => pinFolder(server, p.path, { name: p.name || '', at: i }) } };
  send({ type: 'pinsRemove', server, path: p.path });
}

// Unpin beside a pin's "Not found on Main: …": that pin, and the message goes.
function unpinGone() {
  const a = state.addr;
  const u = a.unpin;
  if (!u) return;
  a.error = '';
  a.unpin = null;
  unpinFolder(u.server, u.path);
  renderBrowseBar();
  renderSuggest();
}

// A pin one place up (-1) or down (+1) in its server's list (Alt+↑/↓, Move up/down), or to `to`
// (a drop); its row stays highlighted.
function movePin(r, to) {
  if (to < 0 || to >= pinsOf(r.server).length || to === r.i) return;
  quick.follow = goneKey(r.server, r.path);
  send({ type: 'pinsMove', server: r.server, path: r.path, to });
}

// Opens a pin as a typed address opens (PINS.md 3.3): its folder exactly as saved, the folder on
// screen staying until the answer (onBrowseResult); a failure leaves the window where it was.
// Under All servers it stays there; else the server picker follows once it opened.
function openPin(server, path) {
  if (!state.remotes.some((r) => r.name === server)) {
    opsSay(`${server} isn’t one of your servers any more.`, true);
    return;
  }
  if (settings.open) closeSettings();
  // From the list: the keyboard goes on in the folder, as after any folder opened.
  if (quick.open) {
    closePins();
    $('#results').focus({ preventScroll: true });
  }
  const b = state.browse;
  if (b.under) closeBelow();
  const a = state.addr;
  Object.assign(a, { error: '', unpin: null, shown: [], sel: -1 });
  b.id += 1;
  b.typed = { server, pin: path };
  b.loading = true;
  renderBrowseBar();
  renderSuggest();
  send({ type: 'browse', id: b.id, remote: server, path, pin: true });
}

// Alt+1 to Alt+9: the nth pin as the list shows it.
function openNthPin(n) {
  const rows = pinRows();
  if (n > rows.length) {
    const where = state.remote === ALL ? '' : ` on ${state.remote}`;
    opsSay(`Quick access has ${rows.length ? counted(rows.length, 'pin') : 'no pins'}${where}.`, true);
    return;
  }
  openPin(rows[n - 1].server, rows[n - 1].path);
}

// Alt+1 to Alt+9's digit (the top row's, whatever the layout: its code; not the numpad's, which
// types Alt codes), else 0.
function pinDigit(ev) {
  const m = /^Digit([1-9])$/.exec(ev.code || '');
  if (m) return Number(m[1]);
  return !ev.code && /^[1-9]$/.test(ev.key) ? Number(ev.key) : 0;
}

// A pin's own menu (right-click or Shift+F10 in the list).
function pinMenu(r) {
  const last = pinsOf(r.server).length - 1;
  return [
    { label: 'Open', key: 'Enter', act: () => openPin(r.server, r.path) },
    { label: 'Rename…', key: 'F2', act: () => renamePin(r) },
    '-',
    { label: 'Move up', key: 'Alt+↑', disabled: r.i === 0, why: r.i === 0 ? `${r.name} is first in Quick access on ${r.server}` : '',
      act: () => movePin(r, r.i - 1) },
    { label: 'Move down', key: 'Alt+↓', disabled: r.i === last, why: r.i === last ? `${r.name} is last in Quick access on ${r.server}` : '',
      act: () => movePin(r, r.i + 1) },
    '-',
    { label: 'Copy path', act: () => copyText([r.path]) },
    '-',
    { label: 'Unpin from Quick access', key: 'Del', act: () => unpinFolder(r.server, r.path) },
  ];
}

// Why `name` can't be a pin's own name, or '' (pins::bad_name; empty is the folder's own name).
function pinBadName(name) {
  if ([...name].length > 255) return 'A pin’s name can be at most 255 characters.';
  for (const c of name) {
    const n = c.codePointAt(0);
    if (n < 0x20 || (n >= 0x7f && n <= 0x9f)) return 'A pin’s name can’t hold control characters.';
  }
  return '';
}

// Rename…: the pin's own name in the name box that Rename uses; empty (or the folder's own name)
// is the folder's name again, which then follows the folder's renames. Only the pin changes.
function renamePin(r) {
  const folder = pinName({ path: r.path });
  showModal({
    title: `Rename the pin ${r.name}`,
    ok: 'Rename',
    html: `<p>${esc(`Only the pin’s name changes: the folder on ${r.server} keeps its name.`)}</p>` +
      '<div class="fo-name"><label for="pin-name">Name</label>' +
      '<input id="pin-name" type="text" spellcheck="false" autocomplete="off" aria-describedby="pin-why">' +
      '<div id="pin-why" class="fo-why" role="alert"></div></div>',
    onOk: () => {
      const typed = $('#pin-name').value;
      const name = typed === folder ? '' : typed;
      if (name === r.own || pinBadName(name)) return;
      quick.want = null;
      send({ type: 'pinsRename', server: r.server, path: r.path, name });
    },
  });
  const input = $('#pin-name');
  input.value = r.name;
  input.placeholder = folder;
  input.focus();
  input.select();
}

$('#modal-body').addEventListener('input', (ev) => {
  if (ev.target.id !== 'pin-name') return;
  const why = pinBadName(ev.target.value);
  $('#pin-why').textContent = why;
  $('#modal-ok').disabled = !!why;
});
$('#modal-body').addEventListener('keydown', (ev) => {
  if (ev.target.id !== 'pin-name' || ev.key !== 'Enter') return;
  ev.preventDefault();
  if (!$('#modal-ok').disabled) $('#modal-ok').click();
});

// The pins as the backend has them now: in every init, and after every change ("pins"); its
// refusal said in the footer, or the change asked for once done.
function takePins(pins) {
  quick.pins = pins && typeof pins === 'object' && !Array.isArray(pins) ? pins : {};
}

function onPins(msg) {
  takePins(msg.pins);
  const w = quick.want;
  if (msg.error) {
    quick.want = null;
    opsSay(msg.error, true);
  } else if (w && w.done()) {
    quick.want = null;
    opsSay(w.text, false, '', w.act);
  }
  if (quick.follow) {
    const i = pinRows().findIndex((r) => goneKey(r.server, r.path) === quick.follow);
    if (i >= 0 && quick.open) quick.sel = i;
    quick.follow = '';
  }
  renderQuick();
  // The address field's list, untouched (the pins it shows) and nothing highlighted.
  if (state.addr.editing && state.addr.sel < 0) updateSuggest();
}

// The button (filled while the open folder is pinned) and the list, when open.
function renderQuick() {
  const b = state.browse;
  const on = !!b.server && !b.under && !b.top && pinIndex(b.server, b.path) >= 0;
  const btn = $('#pins-btn');
  btn.classList.toggle('on', on);
  btn.setAttribute('aria-label', on ? 'Quick access (this folder is pinned)' : 'Quick access');
  if (quick.open && !quick.drag) renderPinsList();
}

function renderPinsList() {
  const groups = pinGroups();
  const rows = groups.flatMap((g) => g.pins);
  if (quick.sel >= rows.length) quick.sel = rows.length - 1;
  const option = (p) => {
    const k = rows.indexOf(p);
    const on = k === quick.sel;
    return `<div class="opt${on ? ' on' : ''}${p.gone ? ' gone' : ''}" role="option" id="pin-o${k}" data-i="${k}" aria-selected="${on}"` +
      ` aria-label="${esc(`${p.name}, ${p.path}${p.gone ? `, ${p.gone}` : ''}`)}"${k < 9 ? ` aria-keyshortcuts="Alt+${k + 1}"` : ''}` +
      ` title="${esc(`${p.server}:${p.path}`)}">${FOLDER_ICON}<span class="p-name">${esc(p.name)}</span>` +
      `<span class="p-path">${esc(p.path)}</span>${p.gone ? `<span class="p-note">${esc(p.gone)}</span>` : ''}` +
      `${k < 9 ? `<span class="p-key">Alt+${k + 1}</span>` : ''}` +
      `<button type="button" class="p-del" data-del="${k}" tabindex="-1" title="Unpin from Quick access" aria-hidden="true">×</button></div>`;
  };
  const list = $('#pins-list');
  list.innerHTML = !groups.length
    ? '<div class="p-empty">Nothing pinned yet. Right-click a folder and choose Pin to Quick access.</div>'
    : groups.map((g, gi) => `<div role="group" aria-labelledby="pins-g${gi}"><div class="p-head" id="pins-g${gi}">${esc(`Quick access on ${g.server}`)}</div>` +
      (g.pins.length ? g.pins.map(option).join('')
        : `<div class="p-empty">${esc(`Nothing pinned on ${g.server} yet. Right-click a folder and choose Pin to Quick access.`)}</div>`) +
      '</div>').join('');
  if (quick.sel >= 0) list.setAttribute('aria-activedescendant', `pin-o${quick.sel}`);
  else list.removeAttribute('aria-activedescendant');
  // The footer: the open folder, pinned or not (greyed with the reason where none is open).
  const b = state.browse;
  const why = pinHereWhy();
  const pinned = !why && pinIndex(b.server, b.path) >= 0;
  const here = $('#pins-here');
  here.disabled = !!why;
  here.title = why;
  if (why) here.setAttribute('aria-description', why);
  else here.removeAttribute('aria-description');
  here.innerHTML = `<span class="text">${pinned ? 'Unpin this folder' : 'Pin this folder'}</span>` +
    `${why ? `<span class="note">${esc(why)}</span>` : ''}${pinned ? '' : '<span class="key">Ctrl+D</span>'}`;
  const on = list.querySelector('.opt.on');
  if (on) on.scrollIntoView({ block: 'nearest' });
}

// Opens the list (the focus in it, so its keys work) or closes it. `at`: the row highlighted,
// 'first' or 'last' (by the keys); none by the mouse until the pointer is on a row.
function showPins(open, at = '') {
  quick.open = open;
  $('#pins').hidden = !open;
  $('#pins-btn').setAttribute('aria-expanded', String(open));
  if (!open) {
    endPinDrag(false);
    quick.sel = -1;
    return;
  }
  const n = pinRows().length;
  quick.sel = !n ? -1 : at === 'first' ? 0 : at === 'last' ? n - 1 : -1;
  renderPinsList();
  $('#pins-list').focus({ preventScroll: true });
}

// The list closed; `focusButton`: the keyboard back on the button (Esc, Tab).
function closePins(focusButton = false) {
  if (!quick.open) return;
  showPins(false);
  if (focusButton) $('#pins-btn').focus();
}

function setPinSel(i) {
  quick.sel = i;
  renderPinsList();
}

// Dragging a pin within its server's pins: the pointer captured from the press, a drop line
// between the rows; a press that doesn't move is a click, which opens the pin. Pointer events
// only: the uploads' drops from Explorer (HTML5 drags, whose dragover the page takes) never meet it.
const PIN_SLOP = 4; // px a press moves before it drags

function pinDropAt(y) {
  const d = quick.drag;
  const list = $('#pins-list');
  // Near the list's top or bottom edge it scrolls.
  const lr = list.getBoundingClientRect();
  if (y < lr.top + 16) list.scrollTop -= 8;
  else if (y > lr.bottom - 16) list.scrollTop += 8;
  const rows = pinRows();
  const els = [...list.querySelectorAll('.opt[data-i]')].filter((el) => {
    const r = rows[Number(el.dataset.i)];
    return r && r.server === d.row.server;
  });
  if (!els.length) return;
  const count = pinsOf(d.row.server).length;
  // The gap before the row whose middle is below the pointer; after the last, the end.
  let gap = count;
  let mark = els[els.length - 1].getBoundingClientRect().bottom;
  for (const el of els) {
    const r = el.getBoundingClientRect();
    if (y < r.top + r.height / 2) {
      gap = rows[Number(el.dataset.i)].i;
      mark = r.top;
      break;
    }
  }
  d.to = gap > d.row.i ? gap - 1 : gap;
  const line = $('#pins-drop');
  line.hidden = d.to === d.row.i;
  line.style.top = `${mark - $('#pins').getBoundingClientRect().top}px`;
}

function endPinDrag(drop) {
  const d = quick.drag;
  if (!d) return;
  quick.drag = null;
  document.documentElement.classList.remove('pins-dragging');
  $('#pins-drop').hidden = true;
  if (!drop) {
    if (quick.open) renderPinsList();
    return;
  }
  if (!d.moved) openPin(d.row.server, d.row.path);
  else if (d.to !== null && d.to !== d.row.i) movePin(d.row, d.to);
  else renderPinsList();
}

{
  const btn = $('#pins-btn');
  const box = $('#pins');
  const list = $('#pins-list');
  const rowAt = (ev) => {
    const o = ev.target instanceof Element ? ev.target.closest('#pins-list .opt[data-i]') : null;
    return o ? Number(o.dataset.i) : -1;
  };
  btn.addEventListener('click', () => (quick.open ? closePins(true) : showPins(true)));
  // On the button: Enter, Space or ↓ open the list on its first row, ↑ on its last.
  btn.addEventListener('keydown', (ev) => {
    if (ev.altKey || ev.ctrlKey || ev.metaKey) return;
    const at = { Enter: 'first', ' ': 'first', ArrowDown: 'first', ArrowUp: 'last' }[ev.key];
    if (!at) return;
    ev.preventDefault();
    showPins(true, at);
  });
  box.addEventListener('mousedown', (ev) => ev.preventDefault()); // the list keeps the focus
  // A click outside closes it (not one in a pin's menu or a dialog the list opened).
  document.addEventListener('mousedown', (ev) => {
    if (quick.open && ev.target instanceof Element && !ev.target.closest('#pins, #pins-btn, #menu, #modal')) closePins();
  });
  // The keys (PINS.md 3.6): ↑/↓, Home/End, PageUp/PageDown move; Enter opens; Esc (and Tab) close,
  // the focus back on the button; Del unpins (with Undo), F2 renames, Alt+↑/↓ move the pin; a
  // letter goes to the next pin whose name starts with it; Shift+F10 or the menu key open its menu.
  list.addEventListener('keydown', (ev) => {
    const rows = pinRows();
    const n = rows.length;
    const r = rows[quick.sel];
    const k = ev.key;
    const page = 10;
    let to = null;
    if (ev.altKey && !ev.ctrlKey && !ev.metaKey && !ev.shiftKey && (k === 'ArrowUp' || k === 'ArrowDown')) {
      if (r) movePin(r, r.i + (k === 'ArrowUp' ? -1 : 1));
    } else if (ev.altKey || ev.ctrlKey || ev.metaKey) {
      return; // Alt+1-9, Ctrl+D and the window's keys
    } else if (k === 'ArrowDown') {
      to = quick.sel < 0 ? 0 : quick.sel + 1;
    } else if (k === 'ArrowUp') {
      to = quick.sel < 0 ? n - 1 : quick.sel - 1;
    } else if (k === 'PageDown' || k === 'PageUp') {
      to = Math.max(0, quick.sel) + (k === 'PageDown' ? page : -page);
    } else if (k === 'Home' || k === 'End') {
      to = k === 'Home' ? 0 : n - 1;
    } else if (k === 'Enter') {
      if (r) openPin(r.server, r.path);
    } else if (k === 'Escape' || k === 'Tab') {
      closePins(true);
    } else if (k === 'Delete') {
      if (r) unpinFolder(r.server, r.path);
    } else if (k === 'F2' && !ev.shiftKey) {
      if (r) renamePin(r);
    } else if ((k === 'F10' && ev.shiftKey) || k === 'ContextMenu') {
      const el = r && list.querySelector(`.opt[data-i="${quick.sel}"]`);
      if (el) {
        const box2 = el.getBoundingClientRect();
        showMenu(box2.left + 40, box2.bottom, pinMenu(r), true);
      }
    } else if ([...k].length === 1 && k !== ' ' && n) {
      // The next pin whose name starts with the letter, from the highlighted one on and around.
      const want = k.toLowerCase();
      for (let j = 1; j <= n; ++j) {
        const at = (Math.max(quick.sel, -1) + j + n) % n;
        if (rows[at].name.toLowerCase().startsWith(want)) {
          to = at;
          break;
        }
      }
    } else {
      return;
    }
    ev.preventDefault();
    ev.stopPropagation(); // not also the window's (Esc stops a search)
    if (to !== null && n) {
      $('#pins-here').classList.remove('hot'); // the keys' row is the one shaded
      setPinSel(Math.max(0, Math.min(n - 1, to)));
    }
  });
  // The footer is shaded while the pointer is on it, until the keys move the highlight: one entry
  // shaded, as in the menus.
  $('#pins-here').addEventListener('pointerenter', (ev) => ev.currentTarget.classList.add('hot'));
  $('#pins-here').addEventListener('pointerleave', (ev) => ev.currentTarget.classList.remove('hot'));
  // The focus gone elsewhere by the keyboard (Ctrl+F, say), or the window left: the list closes.
  // Not for a pin's menu or a dialog it opened, nor the button.
  box.addEventListener('focusout', (ev) => {
    const to = ev.relatedTarget;
    if (quick.open && !(to instanceof Element && to.closest('#pins, #pins-btn, #menu, #modal'))) closePins();
  });
  window.addEventListener('blur', () => closePins());
  // The highlighted row follows the pointer (none outside the rows), so one row is shaded at most.
  list.addEventListener('pointermove', (ev) => {
    const d = quick.drag;
    if (d && ev.pointerId === d.pointerId) {
      if (!d.moved) {
        if (Math.abs(ev.clientY - d.y0) < PIN_SLOP) return;
        d.moved = true;
        document.documentElement.classList.add('pins-dragging');
      }
      pinDropAt(ev.clientY);
      return;
    }
    const i = rowAt(ev);
    if (i !== quick.sel) setPinSel(i);
  });
  list.addEventListener('pointerleave', () => {
    if (!quick.drag && quick.sel >= 0) setPinSel(-1);
  });
  list.addEventListener('pointerdown', (ev) => {
    const i = rowAt(ev);
    const r = pinRows()[i];
    if (!r || ev.button !== 0 || quick.drag || ev.target.closest('.p-del')) return;
    closeMenu();
    quick.sel = i;
    quick.drag = { row: r, pointerId: ev.pointerId, y0: ev.clientY, moved: false, to: null };
    list.setPointerCapture(ev.pointerId);
  });
  list.addEventListener('pointerup', (ev) => {
    if (quick.drag && ev.pointerId === quick.drag.pointerId) endPinDrag(true);
  });
  list.addEventListener('pointercancel', (ev) => {
    if (quick.drag && ev.pointerId === quick.drag.pointerId) endPinDrag(false);
  });
  list.addEventListener('lostpointercapture', (ev) => {
    if (quick.drag && ev.pointerId === quick.drag.pointerId) endPinDrag(false);
  });
  // The ×: unpin. A click with no pointer behind it (a screen reader's) opens the row's pin.
  list.addEventListener('click', (ev) => {
    const del = ev.target instanceof Element ? ev.target.closest('[data-del]') : null;
    const rows = pinRows();
    if (del) {
      const r = rows[Number(del.dataset.del)];
      if (r) unpinFolder(r.server, r.path);
      return;
    }
    const r = rows[rowAt(ev)];
    if (r && ev.detail === 0) openPin(r.server, r.path);
  });
  list.addEventListener('contextmenu', (ev) => {
    ev.preventDefault();
    const i = rowAt(ev);
    const r = pinRows()[i];
    if (!r) return;
    setPinSel(i);
    showMenu(ev.clientX, ev.clientY, pinMenu(r));
  });
  $('#pins-here').addEventListener('click', () => {
    const b = state.browse;
    if (!pinHereWhy() && pinIndex(b.server, b.path) >= 0) unpinFolder(b.server, b.path);
    else pinHere();
    list.focus({ preventScroll: true });
  });
}
$('#browse-up').addEventListener('click', browseUp);
$('#browse-back').addEventListener('click', () => navGo(-1));
$('#browse-fwd').addEventListener('click', () => navGo(1));
// The mouse's side buttons: Back and Forward, on release as in browsers (the press does nothing).
document.addEventListener('mousedown', (ev) => { if (ev.button === 3 || ev.button === 4) ev.preventDefault(); }, true);
document.addEventListener('mouseup', (ev) => {
  if (ev.button !== 3 && ev.button !== 4) return;
  ev.preventDefault();
  navGo(ev.button === 3 ? -1 : 1);
}, true);
$('#browse-refresh').addEventListener('click', () => {
  const b = state.browse;
  if (b.server) askAccount(b.server, true); // read-only or not, asked again too
  if (b.server) browseTo(b.server, b.path, b.top, { live: true });
  else enterBrowse(); // the list of servers: built again
});
$('#query').addEventListener('input', () => {
  showRecent(false);
  if (!state.browse.under) showFolder({ keep: true });
  renderSearchBox();
});
// Esc anywhere in the search box: a search running stops, then the results close, then the
// words go.
$('#search-form').addEventListener('keydown', (ev) => {
  if (ev.key !== 'Escape') return;
  ev.preventDefault();
  ev.stopPropagation();
  clearSearch();
});
// Exact and ✕ leave the typing where it is.
for (const id of ['#exact', '#search-clear', '#filters-btn']) $(id).addEventListener('mousedown', (ev) => ev.preventDefault());
function clearSearch() {
  if (state.searching) {
    stopSearch();
    if (state.browse.under) state.browse.under.error = 'Search stopped. Enter searches again; Esc goes back to the folder.';
    renderEmpty();
  } else if (state.browse.under) {
    closeBelow();
  } else if ($('#query').value) {
    $('#query').value = '';
    showFolder({ keep: true });
    renderSearchBox();
  }
}
$('#search-clear').addEventListener('click', () => {
  clearSearch();
  $('#query').focus();
});
// Exact: on until turned off; results on screen are searched again with it.
$('#exact').addEventListener('click', () => {
  $('#exact').setAttribute('aria-pressed', String(!exactOn()));
  if (state.browse.under && $('#query').value.trim()) startSearch();
});

// ---- right-click menus -------------------------------------------------------------------

let menuItems = null;  // the open menu's entries
let menuReturn = null; // where the focus goes back to

// items: { label, act, key, disabled, checked, why, note } entries, '-' for a separator, { heading }.
// `checked` true or false makes an entry one of a group (a check mark on the current one). A
// greyed entry's `why` is its tooltip, `note` a few words of it beside its label. `byKeys`: opened
// by the menu key or Shift+F10, so the first entry starts shaded for Enter; opened by the mouse,
// nothing is shaded until the pointer is on an entry.
function showMenu(x, y, entries, byKeys = false) {
  // Separators only between entries: none first, last or twice in a row (a finished download has
  // no actions before "Show in folder", say).
  const items = [];
  for (const it of entries) {
    if (it === '-' && (!items.length || items[items.length - 1] === '-')) continue;
    items.push(it);
  }
  while (items[items.length - 1] === '-') items.pop();
  const menu = $('#menu');
  menuItems = items;
  menuReturn = document.activeElement;
  // A heading (a read-only server's reason) describes the whole menu to a screen reader, and a
  // greyed entry carries its own reason: the arrow keys never reach either.
  const heading = items.findIndex((it) => it && it.heading);
  if (heading >= 0) menu.setAttribute('aria-describedby', 'menu-heading');
  else menu.removeAttribute('aria-describedby');
  menu.innerHTML = items.map((it, i) => {
    if (it === '-') return '<hr>';
    if (it.heading) return `<div class="heading"${i === heading ? ' id="menu-heading"' : ''}>${esc(it.heading)}</div>`;
    const radio = it.checked !== undefined;
    return `<button type="button" role="${radio ? 'menuitemradio' : 'menuitem'}" data-i="${i}"` +
      `${radio ? ` aria-checked="${it.checked}"` : ''}${it.disabled ? ' disabled' : ''}` +
      `${it.disabled && it.why ? ` title="${esc(it.why)}" aria-description="${esc(it.why)}"` : ''}>` +
      `<span class="check">${it.checked ? '✓' : ''}</span><span class="text">${esc(it.label)}</span>` +
      `${it.disabled && it.note ? `<span class="note">${esc(it.note)}</span>` : ''}` +
      `${it.key ? `<span class="key">${esc(it.key)}</span>` : ''}</button>`;
  }).join('');
  menu.hidden = false;
  // Inside the window: to the left of the pointer near the right edge, above it near the bottom.
  const r = menu.getBoundingClientRect();
  menu.style.left = `${Math.max(4, Math.min(x, innerWidth - r.width - 4))}px`;
  menu.style.top = `${Math.max(4, y + r.height > innerHeight - 4 ? y - r.height : y)}px`;
  // The shaded entry is the focused one (app.css), and the focus follows the pointer (below), so
  // only one entry is ever shaded. Opened by the mouse the menu itself has the focus: the arrow
  // keys and Escape work, and nothing is shaded.
  const first = byKeys ? menu.querySelector('button:not(:disabled)') : null;
  (first || menu).focus();
}

function closeMenu(restoreFocus = false) {
  const menu = $('#menu');
  if (menu.hidden) return;
  menu.hidden = true;
  menu.innerHTML = '';
  menuItems = null;
  if (restoreFocus && menuReturn) menuReturn.focus();
  menuReturn = null;
}

$('#menu').addEventListener('click', (ev) => {
  const b = ev.target.closest('button[data-i]');
  if (!b || b.disabled) return;
  const it = menuItems[Number(b.dataset.i)];
  closeMenu(true);
  it.act();
});
$('#menu').addEventListener('keydown', (ev) => {
  const buttons = [...$('#menu').querySelectorAll('button:not(:disabled)')];
  const at = buttons.indexOf(document.activeElement);
  const n = buttons.length;
  let to = -1;
  if (ev.key === 'ArrowDown') to = (at + 1) % n;
  else if (ev.key === 'ArrowUp') to = at < 0 ? n - 1 : (at + n - 1) % n; // nothing shaded yet: the last
  else if (ev.key === 'Home') to = 0;
  else if (ev.key === 'End') to = n - 1;
  if (to >= 0) {
    buttons[to].focus();
  } else if (ev.key === 'Escape' || ev.key === 'Tab' || (ev.key === 'F10' && ev.shiftKey) || ev.key === 'ContextMenu') {
    closeMenu(true);
  } else if (at >= 0 || (ev.key !== 'Enter' && ev.key !== ' ')) {
    return; // Enter and Space press the shaded entry; with none shaded they do nothing
  }
  ev.preventDefault();
  ev.stopPropagation(); // not also the window's keys (Escape closes Settings, say)
});
// The shaded entry follows the pointer: the one under it, none over a greyed entry, a line or
// outside the menu. The keys go on from there, as in Windows' menus.
$('#menu').addEventListener('pointermove', (ev) => {
  const b = ev.target instanceof Element ? ev.target.closest('#menu button') : null;
  const to = b && !b.disabled ? b : $('#menu');
  if (document.activeElement !== to) to.focus();
});
$('#menu').addEventListener('pointerleave', () => {
  if (!$('#menu').hidden && document.activeElement !== $('#menu')) $('#menu').focus();
});
document.addEventListener('mousedown', (ev) => { if (!ev.target.closest('#menu')) closeMenu(); }, true);
document.addEventListener('wheel', () => closeMenu(), { passive: true });
window.addEventListener('blur', () => closeMenu());
window.addEventListener('resize', () => closeMenu());

// Download to...: the folder picker first (from the Save-to folder); Save to itself stays.
let downloadToPicks = null;
function downloadTo(picks) {
  downloadToPicks = picks;
  send({ type: 'pickFolder', current: $('#dest').value.trim(), purpose: 'downloadTo' });
}

function onFolderPicked(msg) {
  if (msg.purpose === 'downloadTo') {
    if (downloadToPicks) download(downloadToPicks, msg.path);
    downloadToPicks = null;
    return;
  }
  $('#dest').value = msg.path;
  state.destinations[state.remote] = msg.path;
}

function copyText(lines) {
  send({ type: 'copyText', text: lines.join('\n') });
}

// The rows a menu acts on: the selection, or the row under the cursor.
function menuRows() {
  const idx = state.selected.size ? [...state.selected].sort((a, b) => a - b) : (state.cursor >= 0 ? [state.cursor] : []);
  return idx.map((i) => state.hits[i]).filter(Boolean);
}

// A search result or Browse row: open, download, show it in Browse, copy its path or name.
function resultMenu() {
  const rows = menuRows();
  const one = rows.length === 1 ? rows[0] : null;
  if (one && one.server) return [{ label: 'Open', key: 'Enter', act: () => openRow(one) }];
  const picks = rows.filter((h) => !h.server);
  if (!picks.length) return [];
  const opens = one && one.dir;
  const items = [];
  if (opens) items.push({ label: 'Open', key: 'Enter', act: () => openRow(one) });
  // Quick access (PINS.md): one folder row (a search hit on its own server), pinned or not; not the
  // login folder '~' of several roots, which has no path.
  if (opens && one.path) items.push(pinEntry(serverOf(one), one.path));
  const kind = kindOf(one);
  if (kind) items.push({ label: plays(kind) ? 'Play' : 'Preview', key: 'Space', act: () => openFile(one) });
  items.push({ label: picks.length > 1 ? `Download ${picks.length}` : 'Download', key: opens ? '' : 'Enter',
    act: () => download(picks) });
  items.push({ label: 'Download to…', act: () => downloadTo(picks) });
  // In a folder the row is already where it lives.
  if (one && state.browse.under) items.push({ label: 'Open folder location', act: () => openHitFolder(one) });
  // Changes on the server (SERVER-OPS.md): Cut, Copy, Paste, Rename, Delete.
  items.push('-', ...changeEntries(picks, one));
  // Into one folder (UPLOADS.md 5.10); a search result says why not.
  if (opens) {
    items.push('-', ...uploadEntries(state.browse.server || one.remote, one.path,
      state.browse.under ? 'Uploads go into a folder you have open, not into search results: open this folder first.' : ''));
  } else if (state.browse.server) {
    // Else the open folder's own, named so they aren't taken for the row's: a folder whose rows
    // fill the list has no empty part to right-click.
    items.push('-', ...uploadEntries(state.browse.server, state.browse.path, uploadRefusal(), browseHere()));
  }
  items.push('-');
  items.push({ label: picks.length > 1 ? `Copy ${picks.length} paths` : 'Copy path', act: () => copyText(picks.map((h) => h.path)) });
  items.push({ label: picks.length > 1 ? `Copy ${picks.length} names` : 'Copy name', act: () => copyText(picks.map((h) => h.name)) });
  const propsWhy = picks.length > 1 ? 'Properties shows one item at a time: pick one.' : '';
  items.push('-', { label: 'Properties', key: 'Alt+Enter', disabled: !!propsWhy, why: propsWhy, act: () => rowProps(one) });
  return items;
}

$('#rows').addEventListener('contextmenu', (ev) => {
  if (rowIndex(ev) < 0) return;
  ev.preventDefault();
  const items = resultMenu();
  if (items.length) showMenu(ev.clientX, ev.clientY, items);
});
// The list's empty part: the open folder's own menu (UPLOADS.md 5.10).
$('#results').addEventListener('contextmenu', (ev) => {
  if (ev.target.closest('.row') || ev.target.closest('button')) return;
  ev.preventDefault();
  showMenu(ev.clientX, ev.clientY, folderMenu());
});

// ---- uploads (UPLOADS.md 5.10): into a folder of a server, from this PC ----------------------

// Why the folder Browse shows takes no upload, or '': uploads go into one folder of one server,
// never into search results, All servers' list of servers or the made-up top of several roots.
function uploadRefusal() {
  const b = state.browse;
  if (!b.server) return 'Open a folder of a server first: uploads go into one folder of one server.';
  if (b.under) return 'Search results aren’t a folder: close the search (Esc) to upload into the folder.';
  if (b.loading) return `${browseHere()} is still opening.`;
  if (b.top && b.severalRoots && !b.uploadable) return 'This is the list of the server’s search roots, not a folder: open one of them first.';
  if (b.error || !b.uploadable) return `Uploads can’t go into ${browseHere()} now.`;
  return '';
}

// "Upload files here…" and "Upload folder here…" into `dir` of `server` (with `name`: "Upload
// files to <name>…"), or, with `why`, saying why not.
function uploadEntries(server, dir, why, name = '') {
  const go = (folders) => () => {
    if (why) flash(why);
    else pickUpload(server, dir, folders);
  };
  const where = name ? `to ${name}` : 'here';
  return [{ label: `Upload files ${where}…`, act: go(false) }, { label: `Upload folder ${where}…`, act: go(true) }];
}

// The open folder's menu, on the list's empty part: Quick access, Paste and New folder into it,
// its uploads, its Properties.
function folderMenu() {
  const b = state.browse;
  const items = [];
  // Pinned or not; never at a top or in search results, which aren't a folder to pin.
  if (b.server && !b.under && !b.top) items.push(pinEntry(b.server, b.path, true), '-');
  const readOnly = b.server && !b.under ? readOnlyWhy(b.server) : '';
  if (readOnly) items.push({ heading: readOnly });
  const here = hereWhy();
  const paste = here ? { why: here, note: '' } : pasteWhy(b.server);
  items.push({ label: 'Paste', key: 'Ctrl+V', disabled: !!paste, why: paste ? paste.why : '', note: paste ? paste.note : '',
    act: () => pasteInto(b.server, b.path, browseHere()) });
  items.push({ label: 'New folder…', key: 'Ctrl+Shift+N', disabled: !!here, why: here, act: () => newFolder() });
  items.push('-', ...uploadEntries(b.server, b.path, uploadRefusal()));
  const props = folderPropsWhy();
  items.push('-', { label: 'Properties', key: 'Alt+Enter', disabled: !!props, why: props, act: () => folderProps() });
  return items;
}

// The dialog's pick: its token, and the server folder. The backend opens Windows' file dialog,
// then checks what was picked and the folder ("uploadCheck").
const upload = { token: 0, pending: null };
function pickUpload(server, dir, folders) {
  const token = ++upload.token;
  upload.pending = { token, server, dir };
  send({ type: 'uploadPick', token, server, dir, folders });
}

const fmtNum = (n) => Number(n || 0).toLocaleString('en-US');
const counted = (n, one, many = `${one}s`) => `${fmtNum(n)} ${n === 1 ? one : many}`;
// A path in a server folder, grab's form: "" is the login folder.
const serverChild = (dir, rel) => (!dir ? rel : dir.endsWith('/') ? `${dir}${rel}` : `${dir}/${rel}`);

// The confirm dialog's text (UPLOADS.md 5.10): what goes (each item, its files and size; links left
// out, names too long for a server), where (a new folder, or merged into one there), whether the
// server takes it, and the files already there with the question about them (`clash`, clashHtml).
function uploadHtml(m, clash = null) {
  const where = `${m.server}:${m.dir || '~'}`;
  const items = m.items || [];
  let html = '<div class="up-check">';
  if (!items.length) html += '<p class="muted"><span class="spinner"></span>Looking at what you picked…</p>';
  else {
    html += '<ul class="up-what">';
    for (const it of items.slice(0, 5)) {
      const size = it.dir ? `${counted(it.files, 'file')}, ${fmtSize(it.bytes)}` : fmtSize(it.bytes);
      html += `<li>${itemIcon(it.name, it.dir)}<span class="up-name">${esc(it.name)}${it.dir ? '/' : ''}</span>` +
        `${it.error ? `<span class="up-bad">${esc(it.error)}</span>` : `<span class="muted">${esc(size)}</span>`}</li>`;
    }
    if (m.count > 5) html += `<li class="muted">…and ${fmtNum(m.count - 5)} more</li>`;
    html += '</ul>';
  }
  const notes = [];
  if (m.phase === 'local' && items.length) notes.push(`Looking at your files… ${counted(m.files, 'file')} so far`);
  else if (m.count > 1) notes.push(`${counted(m.files, 'file')}, ${fmtSize(m.bytes)} in all`);
  if (m.skippedLinks) notes.push(`${counted(m.skippedLinks, 'link')} left out: grab doesn’t follow links or junctions`);
  if (m.unreadable) notes.push(`${counted(m.unreadable, 'folder or name', 'folders or names')} couldn’t be read: left out`);
  if (m.longNames) notes.push(`${counted(m.longNames, 'name')} longer than a server takes (255 bytes): ${m.longNames === 1 ? 'that file fails' : 'those files fail'}`);
  if (notes.length) html += `<p class="muted up-notes">${notes.map(esc).join('<br>')}</p>`;
  // Where it goes: a new folder, or merged into one there (known once the server was looked at).
  const looked = m.phase === 'ready';
  const folders = items.filter((it) => it.dir && !it.error);
  html += `<p class="up-where">To <b>${esc(where)}</b>`;
  if (looked && folders.length === 1) html += folders[0].merging ? `: merged into the folder “${esc(folders[0].name)}” there` : ': a new folder';
  else if (looked && folders.length > 1) {
    const merged = folders.filter((it) => it.merging).length;
    if (merged) html += `: ${counted(merged, 'folder')} merged into ${merged === 1 ? 'one' : 'those'} of the same name there`;
  }
  html += '</p>';
  // The server's side.
  if (m.phase === 'server') {
    html += `<p class="up-server muted"><span class="spinner"></span>Checking ${esc(m.server)}…</p>`;
  } else if (m.phase === 'ready') {
    html += `<p class="up-server up-ok">✓ You can write here${m.free != null ? ` · ${esc(fmtSize(m.free))} free` : ''}</p>`;
    if (m.short) {
      html += `<p class="up-server up-warn">Not enough space: ${esc(fmtSize(m.free))} free, ${esc(fmtSize(m.bytes))} needed. ` +
        'Upload anyway only if room will be made first.</p>';
    }
  } else if (m.phase === 'refused') {
    html += `<p class="up-server up-bad">${esc(m.why || 'This can’t be uploaded.')}</p>`;
  }
  // The files already there, and what happens to them.
  const e = m.existing || {};
  if (looked && e.count > 0 && clash) html += clashHtml(clash);
  else if (looked) html += '<p class="up-existing muted">Nothing of it is there yet.</p>';
  return `${html}</div>`;
}

// ---- the files already on the server (UPLOADS.md decision 3, chunk 5) ----------------------------
// The upload dialog's question, and the one an upload waits with when files turned up at their
// names later: what is there and yours, how they compare, one choice for all of them (Skip first
// under the setting Ask, else the setting) and one per file, what that does, and "Always do this",
// which saves the choice for all as the setting. With the setting not Ask, the dialog says it and
// its Change opens the question for this upload. Nothing there is replaced unless chosen.

const UP_CHOICES = [['replace', 'Replace'], ['different', 'Replace if different'], ['newer', 'Replace if newer'],
  ['skip', 'Skip'], ['both', 'Keep both']];
const upChoiceName = (c) => (c === 'ask' ? 'Ask' : (UP_CHOICES.find(([k]) => k === c) || [c, c])[1]);
const UP_KINDS = { same: 'the same', newer: 'newer here', older: 'older here', differs: 'another size', folder: 'a folder there' };
const UP_EFFECTS = { replace: 'replaced', skip: 'skipped', both: 'second copy' };
// The same question for a paste on the server (SERVER-OPS.md decision 4): the one copied or moved
// against the one already there.
const PASTE_KINDS = { ...UP_KINDS, newer: 'newer', older: 'older' };
// How a file there is taken: a folder moved or copied onto a file of its name is never put in its
// place, as a folder there isn't (server_files decide_whole).
const clashKind = (f) => (f && f.fromDir ? 'folder' : f && f.kind);

// What `choice` does to a file of `kind` (upload_existing.cpp decide): 'replace', 'skip' or
// 'both'. A folder of that name there is never replaced.
function upEffect(choice, kind) {
  if (choice === 'both') return 'both';
  if (kind === 'folder' || choice === 'skip') return 'skip';
  if (choice === 'replace') return 'replace';
  if (choice === 'different') return kind === 'same' ? 'skip' : 'replace';
  if (choice === 'newer') return kind === 'newer' ? 'replace' : 'skip';
  return 'skip';
}

// The question's state: `ex` the files there ({count, same, newer, older, differs, folders,
// files: [first ones]}), `forAll`, `own` (a listed file's place -> its own choice), `always`, and
// `open`: the full question shown (always under Ask; with a setting, after Change).
function clashState(ex, server, setting) {
  const ask = !setting || setting === 'ask';
  return { ex, server, setting: setting || 'ask', forAll: ask ? 'skip' : setting, own: new Map(), always: false, open: ask };
}

// What the dialog's answer sends: the choice for all, the listed files' own, "Always do this".
function clashAnswer(c) {
  return { choice: c.forAll, overrides: Object.fromEntries(c.own), always: c.always };
}

// "37 already there: 30 the same, 5 newer here, 2 older here" (a paste's: "5 newer, 2 older")
function clashSummary(ex, paste = '') {
  const parts = [[ex.same, 'the same'], [ex.newer, paste ? 'newer' : 'newer here'], [ex.older, paste ? 'older' : 'older here'], [ex.differs, 'of another size'],
    [ex.folders, ex.folders === 1 ? 'a folder there' : 'folders there']].filter(([n]) => n > 0).map(([n, w]) => `${fmtNum(n)} ${w}`);
  return `${fmtNum(ex.count)} already there${parts.length ? `: ${parts.join(', ')}` : ''}`;
}

// The names taken by kind as a choice meets them: a paste's folders onto a file of their name
// (`foldersOntoFiles`, counted in their kinds too) taken out of their kinds, as 'folder' (never
// replaced, as a folder there isn't).
function clashKinds(ex) {
  const o = ex.foldersOntoFiles || {};
  const n = (k) => Math.max(0, (ex[k] || 0) - (o[k] || 0));
  return { same: n('same'), newer: n('newer'), older: n('older'), differs: n('differs'), folder: (ex.folders || 0) + (o.count || 0) - (o.folders || 0) };
}

// How many a choice for all replaces, of every file there (not only those listed).
function replacedBy(choice, ex) {
  const k = clashKinds(ex);
  return { replace: k.same + k.newer + k.older + k.differs, different: k.newer + k.older + k.differs, newer: k.newer, skip: 0,
    both: 0 }[choice];
}

// What happens to all of them: each listed file by its own choice or the one for all, the rest by
// the one for all, counted by their kinds less the listed ones'.
function clashOutcome(c) {
  const ex = c.ex;
  const files = ex.files || [];
  const tally = { replace: 0, skip: 0, both: 0 };
  const rest = clashKinds(ex);
  files.forEach((f, i) => {
    const kind = clashKind(f);
    tally[upEffect(c.own.get(i) || c.forAll, kind)]++;
    if (rest[kind] > 0) rest[kind]--;
  });
  for (const [kind, n] of Object.entries(rest)) tally[upEffect(c.forAll, kind)] += n;
  const parts = [];
  if (tally.replace) parts.push(`${fmtNum(tally.replace)} replaced`);
  if (tally.both) parts.push(`${fmtNum(tally.both)} ${c.paste ? 'put beside as' : 'sent as a second copy'} (“name (2)”)`);
  if (tally.skip) parts.push(`${fmtNum(tally.skip)} skipped`);
  const none = tally.replace ? '' : `: nothing on ${c.server} is replaced`;
  return `${parts.join(', ')}${none}`;
}

// A listed file's row: its name and how it compares, yours, the server's, its choice, what it does.
function clashRow(c, f, i) {
  const when = (sec) => (fmtDate(sec) ? `<span class="muted">${esc(fmtDate(sec))}</span>` : '');
  const folder = f.kind === 'folder';
  const own = c.own.get(i) || '';
  const options = [['', 'As for all'], ...UP_CHOICES.filter(([k]) => clashKind(f) !== 'folder' || k === 'skip' || k === 'both')]
    .map(([k, label]) => `<option value="${k}"${k === own ? ' selected' : ''}>${esc(label)}</option>`).join('');
  const effect = upEffect(own || c.forAll, clashKind(f));
  const kinds = c.paste ? PASTE_KINDS : UP_KINDS;
  return `<tr data-i="${i}"><td><span class="up-file">${esc(f.name)}${f.fromDir ? '/' : ''}</span> <span class="up-kind k-${esc(f.kind)}">${esc(kinds[f.kind] || '')}</span></td>` +
    `<td>${f.fromDir ? '<span class="muted">a folder</span>' : `${esc(fmtSize(f.mineSize))}<br>${when(f.mineModified)}`}</td>` +
    `<td>${folder ? '<span class="muted">a folder</span>' : `${esc(fmtSize(f.size))}<br>${when(f.modified)}`}</td>` +
    `<td><select data-i="${i}" aria-label="${esc(`What to do with ${f.name}`)}">${options}</select></td>` +
    `<td class="up-effect e-${effect}">${UP_EFFECTS[effect]}</td></tr>`;
}

// The question (or, with a setting and no Change yet, the setting and Change).
function clashHtml(c) {
  const ex = c.ex;
  let html = `<section class="up-clash" aria-labelledby="up-clash-sum"><p id="up-clash-sum" class="up-existing">${esc(clashSummary(ex, c.paste))}</p>`;
  if (!c.open) {
    html += `<p class="up-setting">Files already there: <b>${esc(upChoiceName(c.setting))}</b> (your setting) · ` +
      '<button type="button" class="link" data-act="up-change" aria-expanded="false">Change</button></p>' +
      `<p class="up-outcome muted" aria-live="polite">${esc(clashOutcome(c))}</p></section>`;
    return html;
  }
  html += '<fieldset class="up-all"><legend>For all of them</legend>';
  for (const [k, label] of UP_CHOICES) {
    const n = replacedBy(k, ex);
    const what = k === 'skip' ? 'nothing replaced' : k === 'both' ? `${c.paste ? 'this one' : 'yours'} as “name (2)”`
      : n === 0 ? 'none replaced' : `${fmtNum(n)} replaced`;
    html += `<label><input type="radio" name="up-all" value="${k}"${k === c.forAll ? ' checked' : ''}> ${esc(label)}` +
      ` <span class="muted">${esc(what)}</span></label>`;
  }
  html += '</fieldset>';
  const files = ex.files || [];
  if (files.length) {
    html += `<div class="up-clash-list"><table class="existing up-clashes" aria-label="${esc(`Files already on ${c.server}`)}">` +
      `<thead><tr><th scope="col">Name</th><th scope="col">${c.paste ? (c.paste === 'copy' ? 'Copying' : 'Moving') : 'Yours'}</th>` +
      `<th scope="col">${c.paste ? 'Already there' : 'On the server'}</th>` +
      '<th scope="col">What to do</th><th scope="col"><span class="sr-only">Then</span></th></tr></thead><tbody>' +
      files.map((f, i) => clashRow(c, f, i)).join('') + '</tbody></table></div>';
  }
  const more = ex.count - files.length;
  if (more > 0) html += `<p class="muted up-more">…and ${fmtNum(more)} more, which go as for all.</p>`;
  if (ex.folders) html += `<p class="muted up-more">A folder of that name there is never replaced: Replace skips it.</p>`;
  html += `<p class="up-outcome" aria-live="polite">${esc(clashOutcome(c))}</p>`;
  // A paste has no setting to keep: it asks each time (SERVER-OPS.md decision 4).
  if (!c.paste) {
    html += `<label class="up-always"><input type="checkbox" id="up-always"${c.always ? ' checked' : ''}> Always do this: ` +
      `<span class="up-always-what">${esc(upChoiceName(c.forAll))}</span> without asking (Settings → Uploads)</label>`;
  }
  return `${html}</section>`;
}

// A choice changed in the open question: what it does is said again where it shows.
function onClashChange(ev) {
  const c = modal && modal.clash;
  const el = ev.target;
  if (!c || !(el instanceof HTMLElement)) return;
  if (el.name === 'up-all') {
    c.forAll = el.value;
    const what = $('#modal-body .up-always-what');
    if (what) what.textContent = upChoiceName(c.forAll);
  } else if (el.id === 'up-always') {
    c.always = el.checked;
    return;
  } else if (el.tagName === 'SELECT' && el.dataset.i != null) {
    const i = Number(el.dataset.i);
    if (el.value) c.own.set(i, el.value); else c.own.delete(i);
  } else {
    return;
  }
  const files = c.ex.files || [];
  for (const td of document.querySelectorAll('#modal-body .up-clashes td.up-effect')) {
    const i = Number(td.parentElement.dataset.i);
    const effect = upEffect(c.own.get(i) || c.forAll, clashKind(files[i]));
    td.className = `up-effect e-${effect}`;
    td.textContent = UP_EFFECTS[effect];
  }
  const out = $('#modal-body .up-outcome');
  if (out) out.textContent = clashOutcome(c);
}

// Change, with the setting shown: the question for this upload, its choice for all the setting's.
function onClashClick(ev) {
  const c = modal && modal.clash;
  const btn = ev.target instanceof Element ? ev.target.closest('[data-act="up-change"]') : null;
  if (!c || !btn) return;
  c.open = true;
  const section = $('#modal-body .up-clash');
  if (section) section.outerHTML = clashHtml(c);
  $('#modal .modal').classList.add('wide');
  const checked = $('#modal-body .up-all input:checked');
  if (checked) checked.focus();
}

// An upload waiting with the question about files that turned up on its server after its check
// (UPLOADS.md 5.5): the same question, starting on Skip; Resume sends the answer.
function upLateDialog(q) {
  const ask = q.ask;
  const one = ask.count === 1;
  const c = clashState(ask, q.remote, 'ask');
  showModal({
    title: one && ask.files.length ? `“${ask.files[0].name}” is already on ${q.remote}` : `${fmtNum(ask.count)} files are already on ${q.remote}`,
    html: `<p>${one ? 'A file of that name is' : 'Files of those names are'} on ${esc(q.remote)} now that weren’t there when ` +
      `the upload was checked. Nothing of ${one ? 'it' : 'theirs'} was touched: choose what happens.</p>` + clashHtml(c),
    ok: 'Resume',
    wide: true,
    onOk: () => {
      if (c.always) setUploadExisting(c.forAll, false);
      send({ type: 'resolveExisting', id: q.id, ...clashAnswer(c) });
    },
  });
  modal.clash = c;
}

// Settings → Uploads: what an upload does with a file already on the server. `tell`: the backend
// too (it says so back with "uploadExisting").
function setUploadExisting(choice, tell = true) {
  state.uploadExisting = choice;
  const sel = $('#up-existing-set');
  if (sel && sel.value !== choice) sel.value = choice;
  if (tell) send({ type: 'setUploadExisting', choice });
}

// The backend's check of a pick (UPLOADS.md 5.10): the dialog opens with the first, shows each
// next, and Upload is enabled once the server takes it ("Upload anyway" when it says it has too
// little room). Cancel or Esc drops it.
function onUploadCheck(msg) {
  const p = upload.pending;
  if (!p || msg.token !== p.token) return; // an older pick, or one cancelled
  try { sessionStorage.removeItem('grab.drag'); } catch (e) { /* no storage */ } // a drop's files reached grab
  if (msg.phase === 'cancelled') { // the file dialog was closed
    upload.pending = null;
    return;
  }
  // A drop refused before anything was looked at (nothing came with it, or only WebView2's copies
  // of files from inside a .zip): said in the footer, no dialog.
  if (msg.phase === 'refused' && !msg.count && !(modal && modal.upload === msg.token)) {
    upload.pending = null;
    flash(msg.why || 'This can’t be uploaded.', 12000);
    return;
  }
  const ours = modal && modal.upload === msg.token;
  if (!ours && modal) { // another question is open: this one isn't asked over it
    upload.pending = null;
    send({ type: 'uploadCheckCancel', token: msg.token });
    return;
  }
  // The files already there (once the server was looked at): the question, its choices kept
  // should the check say more.
  const ex = msg.existing || {};
  const setting = msg.setting || state.uploadExisting;
  let clash = ours ? modal.clash : null;
  if (msg.phase === 'ready' && ex.count > 0 && !clash) clash = clashState(ex, msg.server, setting);
  if (ours) {
    $('#modal-body').innerHTML = uploadHtml(msg, clash);
  } else {
    let dialog = null; // this dialog's modal: gone from `modal` once it closes
    showModal({ title: `Upload to ${msg.server}:${msg.dir || '~'}`, html: uploadHtml(msg, clash), ok: 'Upload',
      onOk: () => {
        upload.pending = null;
        // Nothing there now: the setting goes with it (Ask: one that turns up later is asked about).
        const c = dialog.clash;
        const answer = c ? clashAnswer(c) : { choice: dialog.setting, overrides: {}, always: false };
        if (answer.always) setUploadExisting(answer.choice, false);
        send({ type: 'uploadStart', token: msg.token, ...answer });
      },
      onCancel: () => { upload.pending = null; send({ type: 'uploadCheckCancel', token: msg.token }); } });
    dialog = modal;
    modal.upload = msg.token;
  }
  modal.clash = clash;
  modal.setting = setting;
  $('#modal .modal').classList.toggle('wide', !!clash && clash.open);
  const ok = $('#modal-ok');
  const ready = msg.phase === 'ready';
  ok.disabled = !ready;
  ok.textContent = ready && msg.short ? 'Upload anyway' : 'Upload';
  ok.classList.toggle('danger', ready && !!msg.short);
}

// Browse at an upload's folder on its server, the upload (or its file `index`) picked: its row's
// menu and Show button, Enter once it is done, a double-click.
function showOnServer(q, index = null) {
  const st = index === null ? null : dl.stores.get(q.id);
  const target = st ? serverChild(q.path, st.path[index]) : q.path;
  const folder = st ? parentOf(target) : q.into || '';
  if (!state.remotes.some((r) => r.name === q.remote)) {
    dlNote(`${q.remote} isn’t one of your servers any more.`);
    return;
  }
  if (settings.open) closeSettings();
  if (state.remote !== q.remote && state.remote !== ALL) pickRemote(q.remote, false);
  browseTo(q.remote, folder, false, { select: target });
}

// Where a download lands on this PC: DEST\<name>, and a folder's file `i` below that (5.10): the
// name grab saved it under when it isn't the server's.
function localPath(q, st, i) {
  const base = q.local || `${q.dest.replace(/[\\/]+$/, '')}\\${q.name}`;
  if (!st) return base;
  return `${base}\\${st.local.get(i) || st.path[i].replace(/\//g, '\\')}`;
}

// ---- Browse's changes on a server (SERVER-OPS.md, chunk 2) -------------------------------------
// Right-click entries and Explorer's keys on Browse rows and search results (each carries its
// server and path): Cut, Copy and Paste (a move or copy by the server itself, same server only),
// Rename (F2), Delete (Del, after a confirmation), New folder (Ctrl+Shift+N) and Properties
// (Alt+Enter). A server whose statvfs says read-only (Box.Guest) greys every change, saying why;
// elsewhere grab tries, and says the server's refusal. Cut and Copy fill the page's own clipboard:
// cut rows fade until pasted, the footer says what it holds, Esc clears it, a move empties it and a
// copy keeps it for more pastes. One change at a time: its progress in the footer with Cancel,
// then a short result there. Rename and New folder keep their name box open for a name taken.

const OPS_RESULT_MS = 8000;  // a result stays in the footer this long
const OPS_ERROR_MS = 20000;  // a refusal or failure longer
const OPS_SHOWN = 5;         // names a dialog lists, at most

const serverOf = (h) => h.remote || state.browse.server;
const baseName = (path) => path.slice(path.lastIndexOf('/') + 1);
// "holiday", or "3 items": what a change of `items` ({ name }) is called.
const itemsName = (items) => (items.length === 1 ? items[0].name : `${fmtNum(items.length)} items`);
const serverPlace = (server, dir) => `${server}:${dir || '~'}`;

// Browse opened `server` (Refresh: `again`): its account, once a session, for the menus' greying.
function askAccount(server, again = false) {
  if (!server || ops.asking.has(server) || (ops.accounts.has(server) && !again)) return;
  ops.asking.add(server);
  send({ type: 'filesAccount', server, again });
}

function onFilesAccount(msg) {
  ops.asking.delete(msg.server);
  if (msg.error) ops.accounts.delete(msg.server); // asked again the next time Browse opens it
  else {
    ops.accounts.set(msg.server, { readOnly: !!msg.readOnly, free: msg.free ?? null, home: msg.home || '',
      copyData: msg.copyData !== false });
  }
  renderQuick(); // the login folder's real path: a pin spelled another way is the open folder too
}

// Why nothing may change on `server`, or '': statvfs says its file system is read-only for this
// account (upload::refusal's words).
function readOnlyWhy(server) {
  const a = ops.accounts.get(server);
  return a && a.readOnly ? `${server} is read-only for this account` : '';
}

// `path` (grab's form) as one spelling for its place, as server_files::canonical makes it: below
// `home` (the login folder's real path) when it doesn't start with '/', its "." and ".." taken away
// by their names ("a/../x" is "x").
function serverCanonical(path, home) {
  const whole = home && !path.startsWith('/') ? serverChild(home, path) : path;
  const absolute = whole.startsWith('/');
  const parts = [];
  for (const part of whole.split('/')) {
    if (!part || part === '.') continue;
    if (part === '..' && parts.length && parts[parts.length - 1] !== '..') parts.pop();
    else if (part !== '..' || !absolute) parts.push(part); // ".." above "/" is "/"
  }
  return (absolute ? '/' : '') + parts.join('/');
}

// Why `path` of `server` can't be cut, renamed or deleted, or '': the server's top folder, the login
// folder, or a folder holding the login folder, however each is spelled (the login folder's real
// path from the account; the backend refuses them too).
function topWhy(server, path) {
  const a = ops.accounts.get(server);
  const home = (a && a.home) || '';
  const at = serverCanonical(path, home);
  const login = serverCanonical('', home);
  if (at === '/') return 'the server’s top folder can’t be renamed, moved or deleted';
  if (at === login) return 'the login folder can’t be renamed, moved or deleted';
  if (login.startsWith('/') && login.startsWith(`${at}/`)) return `${baseName(path)} holds the login folder, which can’t be renamed, moved or deleted`;
  return '';
}

// Why another change can't start now, or '': one runs (the backend refuses a second too).
function opsBusyWhy() {
  return ops.running ? `${ops.running.what} is still going: wait until it ends, or cancel it first` : '';
}

// Why the picked rows can't be cut, renamed or deleted, or '': rows of one server it may change,
// none of them the login folder, "/" or a folder holding the login folder.
function changeWhy(picks) {
  if (!picks.length) return 'Pick a file or folder first.';
  if (new Set(picks.map(serverOf)).size > 1) return 'Pick items of one server at a time.';
  for (const h of picks) {
    const top = topWhy(serverOf(h), h.path);
    if (top) return top;
  }
  return readOnlyWhy(serverOf(picks[0]));
}

// Why nothing can go into the open folder (a paste, a new folder), or '': one folder of one server,
// open, listed, and writable.
function hereWhy() {
  const b = state.browse;
  if (!b.server) return 'Open a folder of a server first.';
  if (b.under) return 'Search results aren’t a folder: close the search (Esc) first.';
  if (b.loading) return `${browseHere()} is still opening.`;
  if (b.top && b.severalRoots && !b.uploadable) return 'This is the list of the server’s search roots, not a folder: open one of them first.';
  if (b.error || !b.uploadable) return `Nothing can go into ${browseHere()} now.`;
  return readOnlyWhy(b.server);
}

// Why the clipboard can't be pasted on `server`, with a few words of it for the greyed entry; null
// when it can.
function pasteWhy(server) {
  const c = ops.clip;
  if (!c) return { why: 'Nothing to paste: Cut or Copy files or folders first.', note: 'nothing cut or copied' };
  if (c.server !== server) {
    return { why: `Not yet: copies between servers. What you cut or copied is on ${c.server}: paste it in a folder of ${c.server}.`,
      note: 'not yet: copies between servers' };
  }
  const readOnly = readOnlyWhy(server);
  if (readOnly) return { why: readOnly, note: 'read-only' };
  // A copy needs the server's copy-data (OpenSSH 9.0 and later); a move is a rename, which any does.
  const a = ops.accounts.get(server);
  if (c.op === 'copy' && a && !a.copyData) {
    return { why: `${server} doesn’t copy files itself (its SSH server is older than OpenSSH 9.0)`, note: 'the server can’t copy' };
  }
  return null;
}

// Cut, Copy, Paste, Rename and Delete for the menu of the rows `picks` (`one`: the only one).
function changeEntries(picks, one) {
  const b = state.browse;
  const why = changeWhy(picks);
  const readOnly = readOnlyWhy(serverOf(picks[0]));
  const mixed = new Set(picks.map(serverOf)).size > 1 ? 'Pick items of one server at a time.' : '';
  const items = [];
  if (readOnly) items.push({ heading: readOnly });
  items.push({ label: 'Cut', key: 'Ctrl+X', disabled: !!why, why, act: () => holdRows('cut', picks) });
  items.push({ label: 'Copy', key: 'Ctrl+C', disabled: !!mixed, why: mixed, act: () => holdRows('copy', picks) });
  // Paste: into a folder row (one you have open, not a search result), else into the open folder,
  // named so it isn't taken for the row's (a folder whose rows fill the list has no empty part).
  if (one && one.dir) {
    const p = b.under ? { why: 'Paste goes into a folder you have open, not into search results: open this folder first.', note: 'open it first' }
      : pasteWhy(serverOf(one));
    items.push({ label: 'Paste', key: '', disabled: !!p, why: p ? p.why : '', note: p ? p.note : '', act: () => pasteInto(serverOf(one), one.path, one.name) });
  } else if (b.server && !b.under) {
    const here = hereWhy();
    const p = here ? { why: here, note: '' } : pasteWhy(b.server);
    items.push({ label: `Paste into ${browseHere()}`, key: 'Ctrl+V', disabled: !!p, why: p ? p.why : '', note: p ? p.note : '',
      act: () => pasteInto(b.server, b.path, browseHere()) });
  }
  items.push('-');
  const renameWhy = why || (picks.length > 1 ? 'Rename one item at a time.' : '');
  items.push({ label: 'Rename…', key: 'F2', disabled: !!renameWhy, why: renameWhy, act: () => renameRow(picks[0]) });
  items.push({ label: picks.length > 1 ? `Delete ${picks.length}…` : 'Delete…', key: 'Del', disabled: !!why, why, act: () => deleteRows(picks) });
  return items;
}

// ---- the clipboard ----

// Cut or Copy: the rows are held for a paste on their server (one server at a time).
function holdRows(op, picks) {
  const why = op === 'cut' ? changeWhy(picks) : (new Set(picks.map(serverOf)).size > 1 ? 'Pick items of one server at a time.' : '');
  if (!picks.length || why) {
    opsSay(why || 'Pick a file or folder first.', true);
    return;
  }
  const server = serverOf(picks[0]);
  ops.clip = { server, op, items: picks.map((h) => ({ path: h.path, name: h.name, dir: !!h.dir })),
    keys: new Set(op === 'cut' ? picks.map((h) => `${server}|${h.path}`) : []) };
  renderClip();
  renderRows();
}

function clearClip() {
  if (!ops.clip) return;
  ops.clip = null;
  renderClip();
  renderRows();
}

// The footer's hint: "2 items ready to paste · Esc clears", the names and what Paste does on hover.
function renderClip() {
  const el = $('#clip');
  const c = ops.clip;
  el.hidden = !c;
  if (!c) {
    el.textContent = '';
    el.title = '';
    return;
  }
  const n = c.items.length;
  el.textContent = `${n === 1 ? '1 item' : `${fmtNum(n)} items`} ready to paste · Esc clears`;
  const names = c.items.slice(0, 10).map((i) => i.name).join(', ') + (n > 10 ? ` and ${fmtNum(n - 10)} more` : '');
  el.title = bidiVisible(`${c.op === 'cut' ? 'Cut' : 'Copied'} on ${c.server}: ${names}. Paste (Ctrl+V) in a folder of ${c.server} ` +
    `${c.op === 'cut' ? 'moves' : 'copies'} ${n === 1 ? 'it' : 'them'} there.`);
}

// ---- the footer's line: the change running, then its result ----

function setRunning(r) {
  ops.running = r;
  ops.result = null;
  ops.refusal = null;
  renderOps();
}

function endRunning(token) {
  if (!ops.running || ops.running.token !== token) return;
  ops.running = null;
  ops.refusal = null;
  renderOps();
}

// A result or refusal in the footer, for a while (`title`: more on hover). A refusal while a
// change or look runs (another change asked for meanwhile) shows over its progress for a while;
// the progress comes back after it (the browse bar's flash hides while the list refreshes).
// `act`: the result's own action beside it while it shows ({ label, run }: Unpin's Undo).
function opsSay(text, bad = false, title = '', act = null) {
  if (bad && ops.running) {
    ops.refusal = { text, until: Date.now() + OPS_RESULT_MS };
    renderOps();
    setTimeout(renderOps, OPS_RESULT_MS + 50);
    return;
  }
  const ms = bad ? OPS_ERROR_MS : OPS_RESULT_MS;
  ops.result = { text, bad, title, until: Date.now() + ms, act };
  renderOps();
  setTimeout(renderOps, ms + 50);
}

// "1.2 of 3.4 GiB": the bytes done in the unit of their total.
function fmtSizeOf(done, total) {
  if (total < 1024) return `${done} of ${total} B`;
  const units = ['KiB', 'MiB', 'GiB', 'TiB', 'PiB'];
  let d = done / 1024, t = total / 1024, u = 0;
  while (t >= 1024 && u < units.length - 1) { d /= 1024; t /= 1024; ++u; }
  return `${d.toFixed(1)} of ${t.toFixed(1)} ${units[u]}`;
}

// What the change running says now: "Copying holiday to Main:/srv · 1.2 of 3.4 GiB", "Deleting
// 1,204 of 2,000 files".
function runningText(r) {
  const p = r.progress;
  if (r.op === 'delete' && p && p.totalFiles > 0) return `Deleting ${fmtNum(p.files)} of ${counted(p.totalFiles, 'file')}`;
  if (r.op === 'copy' && p && p.totalBytes > 0) return `${r.text} · ${fmtSizeOf(p.bytes, p.totalBytes)}`;
  if (r.op === 'copy' && p && p.totalFiles > 0) return `${r.text} · ${fmtNum(p.files)} of ${counted(p.totalFiles, 'file')}`;
  return `${r.text}…`;
}

function renderOps() {
  const el = $('#ops');
  const r = ops.running;
  const res = !r && ops.result && ops.result.until > Date.now() ? ops.result : null;
  const no = r && ops.refusal && ops.refusal.until > Date.now() ? ops.refusal : null;
  el.hidden = !r && !res;
  el.classList.toggle('bad', (!!res && res.bad) || !!no);
  el.classList.toggle('running', !!r);
  $('#ops-cancel').hidden = !r;
  const act = res && res.act;
  $('#ops-act').hidden = !act;
  $('#ops-sep').hidden = !act;
  if (act && $('#ops-act').textContent !== act.label) $('#ops-act').textContent = act.label;
  const text = no ? no.text : r ? runningText(r) : res ? res.text : '';
  const shown = bidiVisible(text);
  if ($('#ops-text').textContent !== shown) $('#ops-text').textContent = shown;
  el.title = bidiVisible(no ? `${no.text}\n${runningText(r)}` : r ? text : res ? [res.text, res.title].filter(Boolean).join('\n') : '');
}

// Cancel stops the change running. A paste's look is only a look: its answer is dropped at once
// (one crossing the Cancel then starts nothing) and nothing changed.
$('#ops-cancel').addEventListener('click', () => {
  const r = ops.running;
  if (!r) return;
  if (r.op === 'look') {
    dropAsk(r.token);
    endRunning(r.token);
    opsSay('Paste cancelled: nothing changed');
    return;
  }
  send({ type: 'filesCancel', token: r.token });
});
// A result's action (Undo): once, while it shows.
$('#ops-act').addEventListener('click', () => {
  const res = ops.result;
  if (!res || !res.act || res.until <= Date.now()) return;
  ops.result = null;
  renderOps();
  res.act.run();
});

function onFilesDone(msg) {
  const w = ops.waits.get(msg.token);
  if (!w) return;
  ops.waits.delete(msg.token);
  w.done(msg);
}

function onFilesProgress(msg) {
  const w = ops.waits.get(msg.token);
  if (w && w.progress) w.progress(msg);
  if (ops.running && ops.running.token === msg.token) {
    ops.running.progress = msg;
    renderOps();
  }
}

// A task's answers go to `done` (and `progress`); its token.
function filesAsk(msg, done, progress = null) {
  const token = ++ops.token;
  ops.waits.set(token, { done, progress });
  send({ ...msg, token });
  return token;
}

// A dialog's task no longer wanted: stopped, its answer dropped.
function dropAsk(token) {
  if (!token || !ops.waits.has(token)) return;
  ops.waits.delete(token);
  send({ type: 'filesCancel', token });
}

// ---- what a change did ----

// "1,204 files and 25 folders", "nothing".
function filesAndFolders(files, folders) {
  return [files ? counted(files, 'file') : '', folders ? counted(folders, 'folder') : ''].filter(Boolean).join(' and ') || 'nothing';
}

// The result of a move, copy or delete (`msg`: the backend's report) in a line: what it did, what
// it skipped or couldn't do with the first reason, why it ended early; every entry on hover.
function reportText(op, msg, job) {
  const done = filesAndFolders(msg.files, msg.folders);
  const to = job ? ` to ${serverPlace(job.server, job.dir)}` : '';
  let text;
  if (msg.error) {
    // A delete's says what went and what is left; a copy's or move's gets what it did first.
    text = op !== 'delete' && (msg.files || msg.folders) ? `${msg.error} (${done} ${op === 'copy' ? 'copied' : 'moved'} first)` : msg.error;
  } else if (msg.stopped) {
    const verb = { copy: 'Copy', move: 'Move', delete: 'Delete' }[op];
    const did = { copy: 'copied', move: 'moved', delete: 'deleted' }[op];
    text = `${verb} cancelled: ${done} ${did}${op === 'delete' && msg.left ? `, ${fmtNum(msg.left)} still there` : ''}`;
  } else if (op === 'delete') {
    text = `Deleted ${done}`;
  } else if (op === 'copy') {
    text = `Copied ${done}${msg.bytes ? ` (${fmtSize(msg.bytes)})` : ''}${to}`;
  } else {
    text = job && job.paths.length === 1 && msg.files + msg.folders === 1 ? `Moved ${job.clip.items[0].name}${to}` : `Moved ${done}${to}`;
  }
  const entries = msg.entries || [];
  const first = (state) => entries.find((e) => e.state === state);
  const said = (state, n) => {
    const e = first(state);
    return `; ${counted(n, 'item')} ${state}${e ? `: ${baseName(e.from)}: ${e.why}` : ''}`;
  };
  if (msg.skipped) text += said('skipped', msg.skipped);
  if (msg.failed) text += said('failed', msg.failed);
  const more = msg.skipped + msg.failed - entries.length;
  const title = entries.map((e) => `${e.from}: ${e.state}, ${e.why}`).join('\n') + (more > 0 ? `\n…and ${fmtNum(more)} more` : '');
  return { text, title, bad: !!msg.error || msg.failed > 0 };
}

// After a change on `server`: search results drop what went (`gone`, each with all below it) and
// show what was renamed (`renamed`: [from, to]) by its new name. Browse's own folder comes again
// from the backend (files_changed).
function afterChange(server, gone, renamed = []) {
  if (!state.browse.under) return;
  const at = (p, g) => p === g || p.startsWith(g.endsWith('/') ? g : `${g}/`);
  const before = state.results.length;
  state.results = state.results.filter((h) => serverOf(h) !== server || !gone.some((g) => at(h.path, g)));
  let changed = state.results.length !== before;
  for (const [from, to] of renamed) {
    for (const h of state.results) {
      if (serverOf(h) !== server || !at(h.path, from)) continue;
      h.path = to + h.path.slice(from.length);
      if (h.path === to) h.name = baseName(to);
      changed = true;
    }
  }
  if (!changed) return;
  state.filterTotal = state.results.length;
  refilter();
}

// ---- paste ----

// The clipboard into `dir` of `server` (`name`: the folder's, for the words): the names taken
// there looked at first; none, it goes at once (a name taken meanwhile is skipped); else the
// uploads' question, relabelled, starting on Skip.
function pasteInto(server, dir, name) {
  const p = pasteWhy(server);
  const why = (p && p.why) || opsBusyWhy();
  if (why) {
    opsSay(why, true);
    return;
  }
  const c = ops.clip;
  const job = { op: c.op === 'cut' ? 'move' : 'copy', server, dir, name, clip: c, paths: c.items.map((i) => i.path) };
  const token = filesAsk({ type: 'filesLook', server, op: job.op, paths: job.paths, dir }, (msg) => onPasteLook(job, msg));
  setRunning({ token, op: 'look', what: `Looking at ${itemsName(c.items)}`, text: `Looking at ${itemsName(c.items)} and ${name}` });
}

function onPasteLook(job, msg) {
  endRunning(msg.token);
  if (msg.stopped) {
    opsSay('Paste cancelled: nothing changed');
    return;
  }
  if (msg.error) {
    opsSay(msg.error, true);
    return;
  }
  const ex = msg.existing || { count: 0 };
  if (!ex.count) {
    startPaste(job, 'skip', []);
    return;
  }
  const c = clashState(ex, job.server, 'ask');
  c.paste = job.op;
  for (const f of ex.files || []) f.fromDir = job.clip.items.some((i) => i.dir && i.path === f.from);
  const verb = job.op === 'copy' ? 'Copy' : 'Move';
  const counts = msg.folders ? `${filesAndFolders(msg.files, 0)} in ${counted(msg.folders, 'folder')}` : filesAndFolders(msg.files, 0);
  showModal({
    title: `${verb} ${itemsName(job.clip.items)} to ${serverPlace(job.server, job.dir)}`,
    html: `<p>${esc(`${job.op === 'copy' ? 'Copying' : 'Moving'} ${counts}${job.op === 'copy' && msg.bytes ? `, ${fmtSize(msg.bytes)}` : ''}.`)} ` +
      `${esc(job.server)} has some of these names in ${esc(job.name)} already: choose what happens to them.</p>` + clashHtml(c),
    ok: verb,
    wide: true,
    onOk: () => startPaste(job, c.forAll, [...c.own].map(([i, choice]) => ({ from: ex.files[i].from, to: ex.files[i].to, choice }))),
    onCancel: () => opsSay('Paste cancelled: nothing changed'),
  });
  modal.clash = c;
}

function startPaste(job, choice, own) {
  const token = filesAsk({ type: 'filesPaste', server: job.server, op: job.op, paths: job.paths, dir: job.dir, choice, own },
    (msg) => onPasteDone(job, msg));
  const verb = job.op === 'copy' ? 'Copying' : 'Moving';
  setRunning({ token, op: job.op, what: `${verb} ${itemsName(job.clip.items)}`,
    text: `${verb} ${itemsName(job.clip.items)} to ${serverPlace(job.server, job.dir)}` });
}

function onPasteDone(job, msg) {
  endRunning(msg.token);
  if (msg.ok === undefined) { // refused before anything was looked at
    opsSay(msg.error, true);
    return;
  }
  const r = reportText(job.op, msg, job);
  if (job.op === 'move') {
    // Moved: what left its place goes from the results; a move that ran to its end empties the
    // clipboard (what it skipped stays where it was).
    afterChange(job.server, msg.moved || []);
    if (msg.ok && ops.clip === job.clip) clearClip();
  }
  opsSay(r.text, r.bad, r.title);
}

// ---- rename and new folder ----

// Why `name` can't be a file's or folder's name on the server, or '' (server_files::bad_name: '／'
// is a '/' there, '␀' a NUL, '．' a '.', unless quoted by ‛).
function badName(name) {
  const raw = name.replace(/‛./gu, 'q').replace(/／/g, '/').replace(/␀/g, '\0').replace(/．/g, '.');
  if (!name) return 'A name can’t be empty.';
  if (raw === '.' || raw === '..') return `“${raw}” can’t be a name.`;
  if (raw.includes('/')) return 'A name can’t hold a /.';
  if (raw.includes('\0')) return 'A name can’t hold a NUL character.';
  return '';
}

// The small dialog with a name box: bad names said as they are typed; OK (or Enter) sends
// `message(name)`; a name taken or a refusal keeps it open and says so; Esc cancels. `after(msg)`
// once the backend did it.
function nameDialog({ title, ok, value, select, message, after }) {
  const d = { token: 0 };
  showModal({
    title,
    ok,
    stay: true,
    html: '<div class="fo-name"><label for="fo-name">Name</label>' +
      '<input id="fo-name" type="text" spellcheck="false" autocomplete="off" aria-describedby="fo-why">' +
      '<div id="fo-why" class="fo-why" role="alert"></div></div>',
    onOk: () => {
      if (d.token) return; // its answer is on its way
      const name = $('#fo-name').value;
      const why = badName(name);
      if (why) {
        showNameWhy(why);
        return;
      }
      if (name === value && ok === 'Rename') { // the same name: nothing to do
        dropModal();
        return;
      }
      const busy = opsBusyWhy();
      if (busy) {
        showNameWhy(busy);
        return;
      }
      $('#modal-ok').disabled = true;
      $('#fo-name').disabled = true;
      d.token = filesAsk(message(name), (msg) => {
        d.token = 0;
        const mine = modal && modal.name === d;
        if (msg.ok) {
          if (mine) dropModal();
          after(msg);
          return;
        }
        if (!mine) {
          opsSay(msg.taken || msg.error, true);
          return;
        }
        $('#fo-name').disabled = false;
        showNameWhy(msg.taken || msg.error);
        $('#fo-name').focus();
        $('#fo-name').select();
      });
    },
    onCancel: () => { d.token = 0; },
  });
  modal.name = d;
  const input = $('#fo-name');
  input.value = value;
  input.focus();
  input.setSelectionRange(0, select);
  $('#modal-ok').disabled = !!badName(value);
}

function showNameWhy(why) {
  const el = $('#fo-why');
  if (!el) return;
  el.textContent = bidiVisible(why || '');
  $('#modal-ok').disabled = !!why;
}

$('#modal-body').addEventListener('input', (ev) => {
  if (ev.target.id !== 'fo-name') return;
  showNameWhy(badName(ev.target.value));
});
$('#modal-body').addEventListener('keydown', (ev) => {
  if (ev.target.id !== 'fo-name' || ev.key !== 'Enter') return;
  ev.preventDefault();
  if (!$('#modal-ok').disabled) $('#modal-ok').click();
});

function renameRow(h) {
  const why = changeWhy(h ? [h] : []) || opsBusyWhy();
  if (why) {
    opsSay(why, true);
    return;
  }
  const server = serverOf(h);
  const dot = h.dir ? -1 : h.name.lastIndexOf('.');
  nameDialog({
    title: `Rename ${h.name}`,
    ok: 'Rename',
    value: h.name,
    select: dot > 0 ? dot : h.name.length, // a file's name without its extension, as Explorer picks it
    message: (name) => ({ type: 'filesRename', server, path: h.path, name }),
    after: (msg) => {
      afterChange(server, [], [[h.path, msg.path]]);
      const b = state.browse;
      if (!b.under && b.server === server) b.pendingSelect = msg.path; // picked once the folder comes again
      if (ops.clip && ops.clip.server === server && ops.clip.items.some((i) => i.path === h.path)) clearClip();
      opsSay(`Renamed ${h.name} to ${baseName(msg.path)}`);
    },
  });
}

// A name for a new folder in the open folder no row has: "New folder", "New folder (2)", ...
function freeFolderName() {
  const taken = new Set(state.browse.items.map((it) => it.name));
  let name = 'New folder';
  for (let n = 2; taken.has(name); ++n) name = `New folder (${n})`;
  return name;
}

function newFolder() {
  const b = state.browse;
  const why = hereWhy() || opsBusyWhy();
  if (why) {
    opsSay(why, true);
    return;
  }
  const server = b.server;
  const dir = b.path;
  const name = freeFolderName();
  nameDialog({
    title: `New folder in ${browseHere()}`,
    ok: 'Create',
    value: name,
    select: name.length,
    message: (n) => ({ type: 'filesMkdir', server, dir, name: n }),
    after: (msg) => {
      const shown = state.browse;
      if (!shown.under && shown.server === server && shown.path === dir) shown.pendingSelect = msg.path;
      opsSay(`Made the folder ${baseName(msg.path)} in ${serverPlace(server, dir)}`);
    },
  });
}

// ---- delete ----

// The confirmation: what goes (files, folders, the total size, the first names), for good; a red
// Delete, the focus on Cancel. With folders, the backend counts them first (with Cancel), and
// refuses a path a transfer or Play uses before anything is asked.
function deleteRows(picks) {
  const why = changeWhy(picks) || opsBusyWhy();
  if (why) {
    opsSay(why, true);
    return;
  }
  const server = serverOf(picks[0]);
  // `pins`: Quick access's pins at or below what goes, which go with it (PINS.md decision 6).
  const d = { server, picks, counting: picks.some((h) => h.dir), counts: null, progress: null, error: '', token: 0,
    pins: pinsBelow(server, picks.map((h) => h.path)) };
  if (!d.counting) d.counts = { files: picks.length, folders: 0, links: 0, bytes: picks.reduce((n, h) => n + (h.size || 0), 0), unreadableCount: 0 };
  showModal({
    title: picks.length === 1 ? `Delete ${picks[0].name}?` : `Delete ${fmtNum(picks.length)} items?`,
    html: deleteHtml(d),
    ok: 'Delete',
    danger: true,
    onOk: () => startDelete(d),
    onCancel: () => dropAsk(d.token),
  });
  modal.del = d;
  $('#modal-ok').disabled = d.counting;
  if (!d.counting) return;
  d.token = filesAsk({ type: 'filesSize', server, paths: picks.map((h) => h.path), check: true }, (msg) => {
    d.token = 0;
    d.counting = false;
    if (msg.error) d.error = msg.error;
    else d.counts = msg;
    showDelete(d);
  }, (msg) => {
    d.progress = msg;
    showDelete(d);
  });
}

function showDelete(d) {
  if (!modal || modal.del !== d) return;
  $('#modal-body').innerHTML = deleteHtml(d);
  $('#modal-ok').disabled = d.counting || !!d.error || !d.counts || d.counts.unreadableCount > 0;
}

function deleteHtml(d) {
  const shown = d.picks.slice(0, OPS_SHOWN);
  let html = '<div class="fo-del">' +
    `<p>${esc(`From ${d.server}, for good: a server has no Recycle Bin to bring ${d.picks.length === 1 ? 'it' : 'them'} back from.`)}</p>` +
    '<ul class="up-what">' + shown.map((h) => `<li>${itemIcon(h.name, h.dir)}<span class="up-name">${esc(h.name)}${h.dir ? '/' : ''}</span>` +
      `<span class="muted">${h.dir ? '' : esc(fmtSize(h.size))}</span></li>`).join('');
  if (d.picks.length > OPS_SHOWN) html += `<li class="muted">…and ${fmtNum(d.picks.length - OPS_SHOWN)} more</li>`;
  html += '</ul>';
  if (d.pins.length) {
    html += `<p class="fo-pins">${esc(d.pins.length === 1 ? `${pinName(d.pins[0])} is in Quick access: its pin goes too.`
      : `${fmtNum(d.pins.length)} of these folders are in Quick access: their pins go too.`)}</p>`;
  }
  if (d.error) {
    html += `<p class="up-bad">${esc(d.error)}</p>`;
  } else if (d.counting) {
    const p = d.progress;
    html += `<p class="fo-counts muted"><span class="spinner"></span>Counting what is in ${d.picks.length === 1 ? 'the folder' : 'the folders'}…` +
      `${p && p.files ? ` ${esc(counted(p.files, 'file'))} so far` : ''}</p>`;
  } else {
    const c = d.counts;
    const parts = [c.files ? counted(c.files, 'file') : '', c.links ? counted(c.links, 'link') : '', c.folders ? counted(c.folders, 'folder') : '']
      .filter(Boolean);
    const what = parts.length > 1 ? `${parts.slice(0, -1).join(', ')} and ${parts[parts.length - 1]}` : parts[0] || 'nothing';
    html += `<p class="fo-counts"><b>${esc(what)}</b>${c.bytes ? ` · ${esc(fmtSize(c.bytes))}` : ''}</p>`;
    if (c.unreadableCount > 0) {
      html += `<p class="up-warn">${esc(`${counted(c.unreadableCount, 'folder')} below can’t be read, so nothing would be deleted: ` +
        `${(c.unreadable || [])[0] || ''}`)}</p>`;
    }
  }
  return `${html}</div>`;
}

function startDelete(d) {
  const busy = opsBusyWhy();
  if (busy) {
    opsSay(busy, true);
    return;
  }
  const paths = d.picks.map((h) => h.path);
  const token = filesAsk({ type: 'filesDelete', server: d.server, paths }, (msg) => {
    endRunning(msg.token);
    if (msg.ok === undefined) { // refused before anything was looked at
      opsSay(msg.error, true);
      return;
    }
    afterChange(d.server, msg.ok ? paths : []);
    if (ops.clip && ops.clip.server === d.server && ops.clip.items.some((i) => paths.includes(i.path))) clearClip();
    const r = reportText('delete', msg);
    // A delete that ran to its end unpinned what it deleted (the backend's "pins" shows it).
    if (msg.ok && d.pins.length) r.text += `; unpinned ${d.pins.length === 1 ? pinName(d.pins[0]) : counted(d.pins.length, 'folder')}`;
    opsSay(r.text, r.bad, r.title);
  });
  setRunning({ token, op: 'delete', what: `Deleting ${itemsName(d.picks)}`, text: `Deleting ${itemsName(d.picks)}` });
}

// ---- Properties ----

const KIND_WORD = { file: 'File', folder: 'Folder', link: 'Link', other: 'Other (a device, socket or pipe)' };

// "rwxr-xr-x" from a mode's permission bits (set-id and sticky as ls shows them).
function rwx(mode) {
  const letters = 'rwxrwxrwx';
  const s = [...letters].map((c, i) => (mode & (1 << (8 - i)) ? c : '-'));
  if (mode & 0o4000) s[2] = mode & 0o100 ? 's' : 'S';
  if (mode & 0o2000) s[5] = mode & 0o010 ? 's' : 'S';
  if (mode & 0o1000) s[8] = mode & 0o001 ? 't' : 'T';
  return s.join('');
}

function rowProps(h) {
  if (!h || h.server) return;
  showProps(serverOf(h), h.path, h.name);
}

// Why the open folder has no Properties, or ''.
function folderPropsWhy() {
  const b = state.browse;
  if (!b.server) return 'Open a folder of a server first.';
  if (b.under) return 'Search results aren’t a folder: close the search (Esc) first.';
  if (b.top && b.severalRoots && !b.uploadable) return 'This is the list of the server’s search roots, not a folder: open one of them first.';
  return '';
}

function folderProps() {
  const why = folderPropsWhy();
  if (why) {
    opsSay(why, true);
    return;
  }
  showProps(state.browse.server, state.browse.path, browseHere());
}

// The read-only dialog: name, where, type, size, times, permissions, owner and group, a link's
// target, the server's free space; a folder's size and a file's MD5 or SHA-256 on buttons, each
// with its progress and Cancel.
function showProps(server, path, name) {
  const p = { server, path, name, data: null, error: '', task: null, size: null, md5: null, sha256: null, token: 0 };
  showModal({ title: `Properties of ${name}`, html: propsHtml(p), ok: 'Close', cancel: null, onOk: () => endProps(p), onCancel: () => endProps(p) });
  modal.props = p;
  p.token = filesAsk({ type: 'filesProps', server, path }, (msg) => {
    p.token = 0;
    if (msg.error) p.error = msg.error;
    else p.data = msg;
    showPropsAgain(p);
  });
}

function endProps(p) {
  dropAsk(p.token);
  if (p.task) dropAsk(p.task.token);
}

function showPropsAgain(p) {
  if (!modal || modal.props !== p) return;
  const focused = document.activeElement && document.activeElement.dataset ? document.activeElement.dataset.act : '';
  $('#modal-body').innerHTML = propsHtml(p);
  const again = focused && $(`#modal-body [data-act="${focused}"]`);
  (again && !again.disabled ? again : $('#modal-ok')).focus();
}

function propsHtml(p) {
  const row = (label, value) => `<tr><th scope="row">${esc(label)}</th><td>${value}</td></tr>`;
  let html = '<table class="fo-props"><tbody>' + row('Name', `<span class="fo-text">${esc(p.name)}</span>`) +
    row('Where', `<span class="fo-text">${esc(serverPlace(p.server, p.path))}</span>`);
  if (p.error) return `${html}</tbody></table><p class="up-bad">${esc(p.error)}</p>`;
  const d = p.data;
  if (!d) return `${html}</tbody></table><p class="muted"><span class="spinner"></span>Looking at it on ${esc(p.server)}…</p>`;
  const t = d.target;
  const file = d.kind === 'file' || (d.kind === 'link' && t && t.kind === 'file');
  html += row('Type', esc(d.kind === 'link' ? `Link to ${t ? (t.kind === 'folder' ? 'a folder' : t.kind === 'file' ? 'a file' : 'something else') : 'nothing (its target is gone)'}`
    : KIND_WORD[d.kind] || d.kind));
  if (d.kind === 'link') html += row('Points to', `<span class="fo-text">${esc(d.link || '')}</span>`);
  const bytes = d.kind === 'link' ? t && t.size : d.size;
  if (file && bytes != null) html += row('Size', esc(`${fmtSize(bytes)} (${fmtNum(bytes)} bytes)`));
  if (d.kind === 'folder') html += row('Size', sizeCell(p));
  if (d.modified) html += row('Modified', esc(fmtDate(d.modified, true)));
  if (d.accessed) html += row('Accessed', esc(fmtDate(d.accessed, true)));
  if (d.mode != null) html += row('Permissions', `<code>${esc(rwx(d.mode))}</code> <span class="muted">${(d.mode & 0o7777).toString(8).padStart(3, '0')}</span>`);
  if (d.owner) html += row('Owner', esc(d.owner));
  if (d.group) html += row('Group', esc(d.group));
  if (file) html += row('Checksums', sumCell(p, 'md5', 'MD5') + sumCell(p, 'sha256', 'SHA-256'));
  if (d.free != null) html += row(`Free on ${p.server}`, esc(`${fmtSize(d.free)}${d.readOnly ? ' · read-only for this account' : ''}`));
  else if (d.readOnly) html += row(p.server, 'Read-only for this account');
  return `${html}</tbody></table>`;
}

// A folder's size: its button, its count running (with Cancel), or what it found.
function sizeCell(p) {
  const t = p.task;
  if (t && t.kind === 'size') {
    const pr = t.progress;
    return `<span class="spinner"></span>Counting…${pr && pr.files ? ` ${esc(counted(pr.files, 'file'))}, ${esc(fmtSize(pr.bytes))} so far` : ''} ` +
      '<button type="button" class="link" data-act="fo-cancel">Cancel</button>';
  }
  if (p.size && p.size.error) return `<span class="up-bad">${esc(p.size.error)}</span> <button type="button" data-act="fo-size">Folder size</button>`;
  if (p.size) {
    const s = p.size;
    const parts = [counted(s.files, 'file'), s.links ? counted(s.links, 'link') : '', counted(s.folders, 'folder')].filter(Boolean);
    return esc(`${fmtSize(s.bytes)} (${fmtNum(s.bytes)} bytes) · ${parts.join(', ')}`) +
      (s.unreadableCount ? `<br><span class="up-warn">${esc(`${counted(s.unreadableCount, 'folder')} couldn’t be read: not counted`)}</span>` : '');
  }
  return `<button type="button" data-act="fo-size"${p.task ? ' disabled' : ''}>Folder size</button>`;
}

// A file's sum on the server: its button, the server computing it (with Cancel), or the sum.
function sumCell(p, sum, label) {
  const t = p.task;
  if (t && t.kind === sum) {
    return `<div class="fo-sum"><span class="spinner"></span>${esc(label)}: computing on ${esc(p.server)}… ` +
      '<button type="button" class="link" data-act="fo-cancel">Cancel</button></div>';
  }
  const got = p[sum];
  if (got && got.hash) return `<div class="fo-sum">${esc(label)}: <code class="fo-hash">${esc(got.hash)}</code></div>`;
  return `<div class="fo-sum"><button type="button" data-act="fo-${sum}"${p.task ? ' disabled' : ''}>${esc(label)}</button>` +
    `${got && got.error ? ` <span class="up-bad">${esc(got.error)}</span>` : ''}</div>`;
}

$('#modal-body').addEventListener('click', (ev) => {
  const p = modal && modal.props;
  const btn = ev.target instanceof Element ? ev.target.closest('button[data-act^="fo-"]') : null;
  if (!p || !btn || btn.disabled) return;
  const act = btn.dataset.act;
  if (act === 'fo-cancel') {
    if (p.task) dropAsk(p.task.token);
    p.task = null;
    showPropsAgain(p);
    return;
  }
  if (p.task || !p.data) return;
  const kind = act.slice(3);
  if (kind === 'size') {
    const task = { kind, token: 0, progress: null };
    task.token = filesAsk({ type: 'filesSize', server: p.server, paths: [p.path] }, (msg) => {
      if (p.task === task) p.task = null;
      p.size = msg.error ? { error: msg.error } : msg;
      showPropsAgain(p);
    }, (msg) => {
      task.progress = msg;
      showPropsAgain(p);
    });
    p.task = task;
  } else if (kind === 'md5' || kind === 'sha256') {
    const d = p.data;
    const bytes = d.kind === 'link' ? d.target && d.target.size : d.size;
    const task = { kind, token: 0 };
    task.token = filesAsk({ type: 'filesHash', server: p.server, path: p.path, size: bytes || 0, sum: kind }, (msg) => {
      if (p.task === task) p.task = null;
      p[kind] = msg.error ? { error: msg.error } : { hash: msg.hash };
      showPropsAgain(p);
    });
    p.task = task;
  }
  showPropsAgain(p);
});

// ---- the keys (in the list only: never in a text field; the Downloads table and the recent
// searches keep their own Del) ----

// Explorer's keys for these in the list; whether one was taken.
function opsKey(ev) {
  const k = ev.key;
  const plain = !ev.ctrlKey && !ev.altKey && !ev.metaKey;
  const ctrl = ev.ctrlKey && !ev.altKey && !ev.metaKey;
  const b = state.browse;
  const picks = () => menuRows().filter((h) => !h.server);
  if (k === 'F2' && plain && !ev.shiftKey) {
    const p = picks();
    if (p.length > 1) opsSay('Rename one item at a time.', true);
    else renameRow(p[0]);
  } else if (k === 'Delete' && plain) {
    deleteRows(picks());
  } else if (ctrl && !ev.shiftKey && (k === 'x' || k === 'X')) {
    holdRows('cut', picks());
  } else if (ctrl && !ev.shiftKey && (k === 'c' || k === 'C')) {
    holdRows('copy', picks());
  } else if (ctrl && !ev.shiftKey && (k === 'v' || k === 'V')) {
    // Into the open folder, as in Explorer (a folder row's menu pastes into that folder).
    const here = hereWhy();
    if (here) opsSay(here, true);
    else pasteInto(b.server, b.path, browseHere());
  } else if (ctrl && ev.shiftKey && (k === 'n' || k === 'N')) {
    newFolder();
  } else if (k === 'Enter' && ev.altKey && !ev.ctrlKey && !ev.metaKey) {
    const p = picks();
    if (p.length > 1) opsSay('Properties shows one item at a time: pick one.', true);
    else if (p.length === 1) rowProps(p[0]);
    else folderProps();
  } else if (k === 'Escape' && plain && ops.clip && !b.under) {
    clearClip();
  } else {
    return false;
  }
  return true;
}

// ---- downloads ---------------------------------------------------------------------------

// To the Save-to folder, or with `to` (Download to...) to that folder this once.
// A folder as Windows names it, for telling two downloads to one place: one kind of separator,
// none at the end, case ignored ("D:\Media\" and "d:/media" are one folder).
const placeKey = (dest) => dest.replace(/\//g, '\\').replace(/\\+$/, '').toLowerCase();

function download(hits, to = null) {
  if (!hits.length) return;
  const dest = to || $('#dest').value.trim();
  if (!/^[a-zA-Z]:[\\/]|^\\\\[^\\]/.test(dest)) {
    showBanner(dest ? `“${dest}” is not a full folder path. Use something like C:\\Users\\you\\Downloads, or pick one with Choose.`
                    : 'Choose a folder to save to.');
    $('#dest').focus();
    return;
  }
  showBanner('');
  // Skip anything already waiting or running for the same place, and two downloads into the same
  // file (the same name from two servers under All servers): they would write it at once.
  const active = state.queue.filter((q) => ['queued', 'running', 'paused'].includes(q.status));
  const busy = new Set(active.map((q) => `${q.remote}|${q.path}|${placeKey(q.dest)}`));
  const targets = new Set(active.map((q) => `${placeKey(q.dest)}|${q.name.toLowerCase()}`));
  const clashes = [];
  // Each hit goes to its own server's queue (they differ under All servers).
  const groups = new Map();
  for (const h of hits) {
    if (busy.has(`${h.remote}|${h.path}|${placeKey(dest)}`)) continue;
    const target = `${placeKey(dest)}|${h.name.toLowerCase()}`;
    if (targets.has(target)) { clashes.push(h.name); continue; }
    targets.add(target);
    // Folders and files mix: each goes as its own kind of download.
    const mode = h.dir ? 'folder' : 'file';
    const key = `${h.remote}|${mode}`;
    if (!groups.has(key)) groups.set(key, { remote: h.remote, mode, items: [] });
    groups.get(key).items.push({ path: h.path, name: h.name, size: h.size, modified: h.modified });
  }
  if (clashes.length) {
    showBanner(`Not downloaded again into the same folder: ${clashes.join(', ')}. ` +
      'A download to that name is already in the queue; pick another folder for the other copy.');
  }
  if (!groups.size) return;
  if (!to) state.destinations[state.remote] = dest;
  // One request: the backend checks the free space for all of it (see onSpaceShort).
  send({ type: 'enqueue', dest, groups: [...groups.values()], remember: !to });
  if (state.remote === ALL) send({ type: 'prefs', remote: ALL }); // stay on All servers next time
  lastDownload = { dest, groups: [...groups.values()], remember: !to };
}

let lastDownload = null; // the last Download, to send again with "Download anyway"

// The selection won't fit on its drive (with what downloads already queued there still need):
// nothing was queued; "Download anyway" queues it without the check.
function onSpaceShort(msg) {
  const d = lastDownload;
  if (!d) return;
  const queued = msg.queued > 0 ? `, and downloads already queued there still need ${fmtSize(msg.queued)}` : '';
  const atLeast = msg.partial ? ' (at least: the file list doesn’t hold all of a folder)' : '';
  showModal({
    title: `Not enough space on ${msg.drive}`,
    html: `<p>${d.groups.reduce((n, g) => n + g.items.length, 0) > 1 ? 'These downloads need' : 'This download needs'}` +
      ` about ${esc(fmtSize(msg.need))}${esc(atLeast)}${esc(queued)}. ${esc(msg.drive)} has ${esc(fmtSize(msg.free))} free.</p>` +
      '<p class="muted">Free some space first, or download anyway if you’ll make room before it’s needed.</p>',
    ok: 'Download anyway',
    onOk: () => send({ type: 'enqueue', dest: d.dest, groups: d.groups, remember: d.remember, existing: d.existing, force: true }),
  });
}

// Different files already where a download goes (another size, or older than the server's):
// replace them, keep both (the new one as "name (2).ext"), or skip them and keep yours.
// `ask`: {count, files: [{name, size, modified, mineSize, mineModified}]}.
function existingDialog(ask, onChoice) {
  const when = (sec) => (fmtDate(sec) ? ` · ${esc(fmtDate(sec))}` : '');
  const rows = ask.files.map((f) => `<tr><td>${esc(f.name)}</td><td>${esc(fmtSize(f.mineSize))}${when(f.mineModified)}</td>` +
    `<td>${esc(fmtSize(f.size))}${when(f.modified)}</td></tr>`).join('');
  const more = ask.count > ask.files.length ? `<p class="muted">…and ${ask.count - ask.files.length} more.</p>` : '';
  const one = ask.count === 1;
  showModal({
    title: one && ask.files.length ? `“${ask.files[0].name}” is already there` : `${ask.count} files are already there`,
    html: `<p>${one ? 'A different file of that name is' : 'Different files of those names are'} already in the folder:</p>` +
      `<table class="existing"><tr><th></th><th>Yours</th><th>The server’s</th></tr>${rows}</table>${more}` +
      `<p class="muted">Keep both saves the new ${one ? 'one' : 'ones'} as “name (2)”. Skip keeps yours and doesn’t download ${one ? 'it' : 'them'}.</p>`,
    ok: 'Replace',
    danger: true,
    onOk: () => onChoice('replace'),
    extra: [{ label: 'Skip', act: () => onChoice('skip') }, { label: 'Keep both', act: () => onChoice('both') }],
  });
}

// Nothing was queued: files are already there. The answer sends the same Download again.
function onExisting(msg) {
  const d = lastDownload;
  if (!d) return;
  existingDialog(msg.ask, (choice) => {
    d.existing = choice;
    send({ type: 'enqueue', dest: d.dest, groups: d.groups, remember: d.remember, existing: choice });
  });
}

// Resume; a download waiting with that question asks it first, an upload its own (upLateDialog).
function resumeItem(q) {
  if (q.ask && isUp(q)) upLateDialog(q);
  else if (q.ask) existingDialog(q.ask, (choice) => send({ type: 'resolveExisting', id: q.id, choice }));
  else send({ type: 'resumeItem', id: q.id });
}

// ---- the Downloads table (DOWNLOADS-UI.md 5): a header row over a virtual body ---------------
//
// A row per download, its message rows under it (5.5) and, under an open folder, every one of its
// files, which stay when they are done (decision 2): they come by subscription (6.3), once in full
// ("files", in chunks) and then the rows that changed ("fileDelta"), into a store of columns per
// folder. Every row is 28 px. Only the rows in view, and 10 more each side, have an element, from
// a pool reused as the list scrolls. Columns have fixed widths and numbers fixed formats, and a
// cell is written only when its value changes: nothing moves while downloading, and a button under
// the mouse stays the same button.

const DL_POOL = 150; // row elements at most
const DL_OVER = 10;  // rows drawn beyond the view on each side
// The columns in CSS px, as app.css's .downloads --w-* (keep in sync), with .dl-grid's gap and its
// padding each side. Narrow panels hide ETA, then Speed, then Priority, then go compact (5.2):
// steps 0-3 by the body's width alone, never the data, so the layout can't flicker as values
// change. Compact columns need 604 px, which the smallest window's panel has.
const DL_W = { handle: 44, name: 160, status: 180, progress: 112, size: 156, speed: 84, eta: 64, priority: 84, actions: 132 };
const DL_GAP = 8;
const DL_PAD = 8;
const DL_HIDES = [[], ['eta'], ['eta', 'speed'], ['eta', 'speed', 'priority']];
const DL_NEED = DL_HIDES.map((hide) => {
  const shown = Object.entries(DL_W).filter(([k]) => !hide.includes(k)).map(([, px]) => px);
  return 2 * DL_PAD + shown.reduce((a, b) => a + b, 0) + DL_GAP * (shown.length - 1);
});
const DL_COMPACT = DL_HIDES.length; // the last step
const DL_HEAD_NARROW = 900;         // px of panel: the head's summary moves into the title's tooltip

// The status words (5.3); a running upload's is Uploading (UPLOADS.md 5.10).
const DL_WORD = { queued: 'Queued', running: 'Downloading', paused: 'Paused', done: 'Done', failed: 'Failed',
  cancelled: 'Cancelled', skipped: 'Skipped', uploading: 'Uploading' };
const PHASE_WORD = { listing: 'Listing', checking: 'Checking', verifying: 'Verifying' };
const PHASE_TIP = { listing: 'Listing the folder on the server', verifying: 'Verifying against the server’s checksums',
  checking: 'Checking the files already here and the free space' };
const UP_PHASE_TIP = { listing: 'Listing your folder', verifying: 'Verifying against the server’s checksums',
  checking: 'Looking at the folder on the server: the files already there' };
// An upload's row (UPLOADS.md 5.10): "↑ name → Server:/folder" in the same table.
const isUp = (q) => q.dir === 'up';
const PRIORITIES = [['high', 'High'], ['normal', 'Normal'], ['low', 'Low']];
const prioLabel = (p) => (PRIORITIES.find(([k]) => k === p) || PRIORITIES[1])[1];
const UNFINISHED = ['queued', 'running', 'paused', 'failed']; // what Cancel, the priority and the moves apply to
const startedBytes = (q) => UNFINISHED.includes(q.status) && q.bytes > 0;

// The action slots' icons, shown instead of the words in compact columns.
const slotSvg = (body, stroke) => `<svg viewBox="0 0 16 16" aria-hidden="true"${stroke
  ? ' fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"'
  : ' fill="currentColor"'}>${body}</svg>`;
const SLOT_ICONS = {
  pause: slotSvg('<path d="M4.5 3h2.5v10H4.5zM9 3h2.5v10H9z"/>'),
  resume: slotSvg('<path d="M5 2.8v10.4L13 8z"/>'),
  retry: slotSvg('<path d="M12.8 8.6a4.8 4.8 0 1 1-1.4-4"/><path d="M12 1.8v3h-3"/>', true),
  cancel: slotSvg('<path d="M4 4l8 8M12 4l-8 8"/>', true),
  show: slotSvg('<path d="M1.8 4.2c0-.7.5-1.2 1.2-1.2h3l1.4 1.4H13c.7 0 1.2.5 1.2 1.2v6.2c0 .7-.5 1.2-1.2 1.2H3c-.7 0-1.2-.5-1.2-1.2z"/>', true),
  skip: slotSvg('<path d="M3 3v10l7-5z"/><path d="M11 3h2v10h-2z"/>'),
};

// The fixed formats (5.1): sizes with 1 decimal, a whole percent rounded down (99.6 shows 99%),
// a speed, and a time left as a clock.
function fmtSizePair(done, total) { return `${fmtSize(done)} / ${fmtSize(total)}`; }
// A file count that fits the Status cell's fixed 74 px: from 100,000 in thousands, rounded down so
// the done side never reaches the total early. The tooltip has the exact numbers.
function fmtCount(n) { return n < 100000 ? String(n) : `${Math.floor(n / 1000)}k`; }
function fmtPct(done, total) { return `${Math.min(100, Math.floor((100 * done) / total))}%`; }
function fmtSpeed(bytesPerSec) { return `${fmtSize(Math.round(bytesPerSec))}/s`; }
function fmtClock(sec) {
  if (sec == null || !isFinite(sec) || sec < 0) return '';
  sec = Math.round(sec);
  if (sec >= 100 * 3600) return '>99 h';
  const h = Math.floor(sec / 3600);
  const m = Math.floor(sec / 60) % 60;
  return h ? `${h}:${pad2(m)}:${pad2(sec % 60)}` : `${m}:${pad2(sec % 60)}`;
}

// The rows (rebuilt only when their structure changes), the elements showing them, the narrow
// step, the selection (downloads by row key, or files of one folder by index), the keyboard's
// cursor, a drag by the ≡ handle and the column widths dragged by hand.
//
// The rows are entries (dl.top): a download, a message row, or an open folder's files as one block
// that stands for the rows of its store's view, so a 177,000-file folder makes no row objects until
// they show (dlRow). Row keys: 'd<id>' a download, 'm<id>:<what>' a message row, 'f<id>:<index>'
// a file, by its index in the folder's listing (stable, also across restarts).
const dl = {
  top: [],               // { kind: 'dl' | 'msg' | 'block', key, id, what, sub, scope, n }
  starts: [],            // each entry's first row
  n: 0,                  // rows in all
  topIndex: new Map(),   // a download or message row's key -> its entry
  blockAt: new Map(),    // folder id -> its block's entry
  shape: '',             // the entries' keys and the blocks' views: a new one means the structure changed
  byId: new Map(),       // download id -> its item in state.queue
  stores: new Map(),     // folder id -> its files (newStore), while it is open
  watching: new Set(),   // folders whose files the backend sends (watchFiles)
  pool: [],              // every row element made
  bound: new Map(),      // row key -> the element showing it
  step: 0,
  need: DL_NEED,         // the panel width each step needs, with the widths below
  frame: 0,
  filesFrame: 0,         // a frame asked for by "fileDelta"
  sel: new Set(),        // downloads' keys (a message row stands for its download)
  fsel: null,            // or files of one folder: { id, on: Uint8Array by index }
  anchor: '',
  // The cursor row's key ('' before the first click or key), and its download and index, where
  // it goes when its row goes (a folded folder's file goes to the folder).
  cursor: '',
  cursorId: 0,
  cursorAt: 0,
  drag: null,
  follow: null,          // a download moved from the menu or the keys: { id, order } until it moved
  widths: {},            // column -> px dragged by hand (gui.json downloads.widths); absent: DL_W
  resize: null,          // while a title's edge is dragged
  keyAt: 0,              // when the table last had a key (performance.now())
  show: 'all',           // the filter, for the session: 'all', 'unfinished', 'failed', 'downloads' or 'uploads'
  lastRun: -1,           // the row 'Go to running' went to, and where the body was scrolled then
  lastRunTop: -1,
  goPending: 0,          // a folder 'Go to running' opened: the jump comes with its files
  // The first row in view at the last paint (its entry's key, its download's, its row in the entry,
  // its index, and how far the body is scrolled into it): rows coming or going above it don't
  // move it (dlKeepView).
  viewKey: '',
  viewDl: '',
  viewPos: 0,
  viewAt: 0,
  viewOff: 0,
  moving: null,          // a download the user moved: { id, order } until the new order came
};
const folderOpen = new Map(); // folder id -> expanded, as the user toggled it in this window

// A row as the selection counts it: a message row stands for its download; and its level, which a
// selection keeps to: downloads ('d'), or the files of one folder.
const selKey = (r) => (r.kind === 'msg' ? `d${r.id}` : r.key);
const scopeOf = (r) => (r.kind === 'file' ? r.scope : 'd');

// The table keeps the keyboard focus (one Tab stop, 5.8): a row's button never holds it.
function dlFocus() { $('#queue').focus({ preventScroll: true }); }

// Open while the folder is active, folded when it is finished, unless the user toggled it (kept in
// queue.json: 'expanded').
function folderIsOpen(q) {
  if (folderOpen.has(q.id)) return folderOpen.get(q.id);
  return typeof q.expanded === 'boolean' ? q.expanded : !['done', 'cancelled'].includes(q.status);
}

// ---- a folder's files (6.1, 6.3) ----------------------------------------------------------------
//
// A store of columns by the file's index: typed arrays for the numbers, arrays for the names, maps
// for what few files have (a local name, an error). `view` holds the indices shown, in order (the
// filter, then the sort), `pos` each index's place in it (-1: not shown).

// folder::State's order, as "files" and "fileDelta" send it, then 'waiting': a big file that has
// its place but waits for the folder's small files, which hold the connections (queue_view.hpp
// held_code). The folder counts it as queued.
const FILE_STATES = ['queued', 'running', 'paused', 'done', 'failed', 'skipped', 'waiting'];
const FS = { queued: 0, running: 1, paused: 2, done: 3, failed: 4, skipped: 5, waiting: 6 };
const WAIT_WHY = 'Waits for the folder’s small files: they use its connections; big files follow';
const PRIORITY_OF_WEIGHT = { 4: 'high', 2: 'normal', 1: 'low' };
const WEIGHT_OF_PRIORITY = { high: 4, normal: 2, low: 1 };
// Above this many files a folder is sorted by a live column (Status, Progress, Speed, ETA) only
// when its title is clicked: its arrow is hollow, "the order from your click" (5.6).
const DL_LIVE_FILES = 5000;
const DL_LIVE_COLS = new Set(['status', 'progress', 'speed', 'eta', 'priority']);

function newStore(id, gen, total) {
  return {
    id, gen, total, loaded: 0,
    path: new Array(total), local: new Map(), named: new Set(), errors: new Map(),
    size: new Float64Array(total), bytes: new Float64Array(total),
    speed: new Float32Array(total), eta: new Float32Array(total).fill(NaN),
    status: new Uint8Array(total), prio: new Uint8Array(total).fill(2), verified: new Uint8Array(total),
    mixed: false,          // priorities differ: the default order isn't the folder order
    mixedChanged: false,   // a priority changed since the view was made
    keys: null,            // lowercased paths, made for the first sort by name
    order: null,           // every file in the order of the sort (the view before the filter)
    view: new Int32Array(0), pos: new Int32Array(total).fill(-1), viewVer: 0,
    at: 0,                 // when the view last changed from the data
    changed: false,        // data came since the view was last made
  };
}

// File `i` as one object, for a row's cells and tooltips.
function fileOf(st, i) {
  return {
    path: st.path[i] || '', local: st.local.get(i) || '', named: st.named.has(i), status: FILE_STATES[st.status[i]] || 'queued',
    bytes: st.bytes[i], size: st.size[i], speed: st.speed[i], eta: Number.isNaN(st.eta[i]) ? null : st.eta[i],
    error: st.errors.get(i) || '', verified: !!st.verified[i], priority: PRIORITY_OF_WEIGHT[st.prio[i]] || 'normal',
  };
}

// A file's size: as listed, or once done as fetched (its bytes).
const fileSize = (st, i) => (st.status[i] === FS.done ? st.bytes[i] || st.size[i] : st.size[i]);

// The filter (decision 2): what a download or a file must be to show. Unfinished is what still
// comes (queued, running, paused, failed); Failed the failed ones, and folders with failed files;
// Downloads and Uploads one direction (UPLOADS.md 5.10), every file of its folders.
function dlMatches(q) {
  if (dl.show === 'all') return true;
  if (dl.show === 'downloads') return !isUp(q);
  if (dl.show === 'uploads') return isUp(q);
  if (dl.show === 'failed') return q.status === 'failed' || !!(q.files && q.files.failed > 0);
  return UNFINISHED.includes(q.status);
}
function fileMatches(st, i) {
  if (dl.show === 'all' || dl.show === 'downloads' || dl.show === 'uploads') return true;
  const s = st.status[i];
  return dl.show === 'failed' ? s === FS.failed : s !== FS.done && s !== FS.skipped;
}

// What file `i` sorts by on `key`, as its row shows it; NaN when it shows none (those go last both
// ways). Status by the file's own state.
const FILE_RANK = [1, 0, 2, 6, 3, 5, 0.5]; // by FILE_STATES: running first, as DL_STATUS_RANK; waiting next
function fileSortValue(st, i, key) {
  const s = st.status[i];
  switch (key) {
    case 'status': return FILE_RANK[s];
    case 'progress':
      if (s === FS.done) return 1;
      return st.size[i] > 0 ? Math.min(1, st.bytes[i] / st.size[i]) : NaN;
    case 'size': { const n = fileSize(st, i); return n > 0 ? n : NaN; }
    case 'speed': return s === FS.running && st.speed[i] > 0 ? st.speed[i] : NaN;
    case 'eta': return s === FS.running && st.eta[i] >= 0 ? st.eta[i] : NaN;
    case 'priority': return s === FS.done || s === FS.skipped ? NaN : st.prio[i];
    default: return NaN;
  }
}

// The loaded files in order: the default order is the start order (priority first, then folder
// order); a sort keeps ties in it. Names compare naturally on keys made once per file. While the
// list still comes (`sorted` false) the default order: sorting 177,000 rows at every chunk would
// take far longer than the list.
function fileOrder(st, sorted = true) {
  const n = st.loaded;
  let order = new Array(n);
  for (let i = 0; i < n; ++i) order[i] = i;
  if (st.mixed) order.sort((a, b) => st.prio[b] - st.prio[a] || a - b);
  const key = dlSort.key;
  if (!key || !sorted) return order;
  const rank = new Int32Array(n);
  order.forEach((i, k) => { rank[i] = k; });
  const desc = dlSort.dir === 'desc';
  if (key === 'name') {
    if (!st.keys || st.keys.length < n) st.keys = Array.from({ length: n }, (_, i) => asciiLower(st.path[i] || ''));
    const keys = st.keys;
    order = order.sort((a, b) => {
      const c = naturalCompare(st.path[a], st.path[b], keys[a], keys[b]);
      return c ? (desc ? -c : c) : rank[a] - rank[b];
    });
    return order;
  }
  const v = new Float64Array(n);
  for (let i = 0; i < n; ++i) v[i] = fileSortValue(st, i, key);
  return order.sort((a, b) => {
    const x = v[a];
    const y = v[b];
    const xn = Number.isNaN(x);
    const yn = Number.isNaN(y);
    if (xn || yn) return xn === yn ? rank[a] - rank[b] : xn ? 1 : -1;
    const c = x - y;
    return c ? (desc ? -c : c) : rank[a] - rank[b];
  });
}

// The view from `order`: the files that pass the filter, and each index's place.
function setFileView(st, order) {
  const view = new Int32Array(order.length);
  let k = 0;
  for (const i of order) if (fileMatches(st, i)) view[k++] = i;
  const next = view.subarray(0, k);
  if (next.length === st.view.length && next.every((i, p) => st.view[p] === i)) return false;
  st.pos.fill(-1);
  next.forEach((i, p) => { st.pos[i] = p; });
  st.view = next;
  st.viewVer++;
  return true;
}

// The view again: at once for a click, the filter, rows arriving (`force`); from changed data (a
// live sort, rows leaving under the filter) at most once a second and not while dlResortHeld, as
// the downloads (5.6). A folder of more than DL_LIVE_FILES files keeps the order of the click and
// only lets rows leave. Returns whether the rows changed.
const liveSort = (st) => !!dlSort.key && DL_LIVE_COLS.has(dlSort.key) && st.total <= DL_LIVE_FILES;

function updateFileView(st, force) {
  const now = performance.now();
  if (!force) {
    if (!st.changed) return false;
    if (!liveSort(st) && dl.show === 'all' && !st.mixedChanged) { st.changed = false; return false; }
    const wait = st.at + DL_RESORT_MS - now;
    if (wait > 0 || dlResortHeld()) {
      setDlPending(dlSort.pendingDl);
      dlRecheckIn(Math.max(wait, 250));
      return false;
    }
  }
  const complete = st.loaded >= st.total;
  if (force || !st.order || !complete || liveSort(st) || st.mixedChanged) st.order = fileOrder(st, complete);
  // else the click's order stays (a big folder): only the filter moves rows
  st.changed = false;
  st.mixedChanged = false;
  st.at = now;
  return setFileView(st, st.order);
}

// Whether a file view waits for its second or for the pointer (the title's dot).
const filesPending = () => [...dl.stores.values()].some((st) => st.changed && st.order &&
  (liveSort(st) || dl.show !== 'all' || st.mixedChanged));

// The backend's 'files': a chunk of a folder's rows, [path, size, status, bytes, priority, local,
// error, verified, named], the first from 0. A chunk not following the last asks for all again.
function onFiles(msg) {
  if (!dl.watching.has(msg.id)) return; // folded since
  let st = dl.stores.get(msg.id);
  if (msg.from === 0) {
    st = newStore(msg.id, msg.gen, msg.total);
    dl.stores.set(msg.id, st);
    if (dl.fsel && dl.fsel.id === msg.id) dl.fsel = null;
    renderDlSortHead(); // a big folder: the arrow of a live sort goes hollow
  } else if (!st || st.gen !== msg.gen || st.loaded !== msg.from) {
    send({ type: 'watchFiles', id: msg.id });
    return;
  }
  const rows = msg.rows;
  for (let k = 0; k < rows.length; ++k) {
    const r = rows[k];
    const i = msg.from + k;
    if (i >= st.total) break;
    st.path[i] = r[0];
    st.size[i] = r[1];
    st.status[i] = r[2];
    st.bytes[i] = r[3];
    st.prio[i] = r[4];
    if (r[4] !== 2) st.mixed = true;
    if (r[5] != null) st.local.set(i, r[5]);
    if (r[6] != null) st.errors.set(i, r[6]);
    if (r[7]) st.verified[i] = 1;
    if (r[8]) st.named.add(i);
  }
  st.loaded = Math.min(st.total, msg.from + rows.length);
  st.keys = null;
  updateFileView(st, true);
  buildDlRows();
  renderDlBody();
  if (dl.goPending === msg.id && st.loaded === st.total) { dl.goPending = 0; goToRunning(); }
}

// The backend's 'fileDelta': the rows that changed since the last flush, [index, status, bytes,
// speed, eta, priority, error, verified]. Drawn in the next frame, with the others of this flush.
function onFileDelta(msg) {
  const st = dl.stores.get(msg.id);
  if (!st || st.gen !== msg.gen) return; // another generation: its 'files' come
  for (const r of msg.rows) {
    const i = r[0];
    if (!(i >= 0 && i < st.total)) continue;
    st.status[i] = r[1];
    st.bytes[i] = r[2];
    st.speed[i] = r[3];
    st.eta[i] = r[4] == null ? NaN : r[4];
    if (st.prio[i] !== r[5]) {
      st.prio[i] = r[5];
      st.mixed = true;
      st.mixedChanged = true;
    }
    if (r[6] != null) st.errors.set(i, r[6]); else st.errors.delete(i);
    st.verified[i] = r[7] ? 1 : 0;
  }
  st.changed = true;
  if (!dl.filesFrame) dl.filesFrame = requestAnimationFrame(refreshFiles);
}

// Changed data in the stores: the views that follow it, and the rows in view.
function refreshFiles() {
  dl.filesFrame = 0;
  let moved = false;
  for (const st of dl.stores.values()) moved = updateFileView(st, false) || moved;
  if (moved) buildDlRows();
  if (!filesPending() && !dlSort.pendingDl) setDlPending(false);
  renderDlBody();
}

// The backend sends the files of each open, listed folder (watchFiles), and stops for a folded one
// (unwatchFiles), whose store goes: a 177,000-file folder holds tens of MB.
function syncWatches() {
  for (const q of state.queue) {
    if (!q.files || !folderIsOpen(q) || dl.watching.has(q.id)) continue;
    dl.watching.add(q.id);
    send({ type: 'watchFiles', id: q.id });
  }
  for (const id of [...dl.watching]) {
    const q = dl.byId.get(id);
    if (q && q.files && folderIsOpen(q)) continue;
    dl.watching.delete(id);
    if (dl.stores.delete(id)) renderDlSortHead();
    if (dl.fsel && dl.fsel.id === id) dl.fsel = null;
    if (q) send({ type: 'unwatchFiles', id });
  }
}

// The message rows under a download (5.5): what needs the user, ellipsized, in full in the
// tooltip. Plain information (already there, the verify summary) goes in the Status tooltip.
function dlMessages(q) {
  const out = [];
  if (q.status === 'failed' && q.error) out.push('error');
  // Why it waits paused: the question about different files already there, or the free space.
  if (q.status === 'paused' && q.waiting) out.push('note');
  if (q.unreadable) out.push('unreadable');
  return out;
}

// The entries: each download (in the order shown), its message rows, and an open folder's files
// as one block: a "Loading" row while its list comes, then the rows of its store's view.
function dlEntriesOf() {
  const top = [];
  for (const q of dlOrdered()) {
    top.push({ kind: 'dl', key: `d${q.id}`, id: q.id, scope: 'd' });
    for (const what of dlMessages(q)) top.push({ kind: 'msg', key: `m${q.id}:${what}`, id: q.id, what });
    if (!q.files || !folderIsOpen(q)) continue;
    const st = dl.stores.get(q.id);
    if (!st || st.loaded < st.total) top.push({ kind: 'msg', key: `m${q.id}:loading`, id: q.id, what: 'loading', sub: true });
    if (st && st.view.length) top.push({ kind: 'block', key: `b${q.id}`, id: q.id, n: st.view.length });
  }
  return top;
}

// The rows again, when their structure changed: a download added, removed or moved, a folder
// opened or folded, a message row coming or going, a folder's view sorted, filtered or grown.
// Byte ticks leave them as they are. Returns whether they changed.
function buildDlRows() {
  const top = dlEntriesOf();
  const shape = top.map((e) => (e.kind === 'block' ? `${e.key}:${dl.stores.get(e.id).viewVer}` : e.key)).join('\0');
  if (shape === dl.shape) return false;
  dl.shape = shape;
  dl.top = top;
  dl.starts = new Array(top.length);
  dl.topIndex = new Map();
  dl.blockAt = new Map();
  let n = 0;
  top.forEach((e, k) => {
    dl.starts[k] = n;
    if (e.kind === 'block') dl.blockAt.set(e.id, k); else dl.topIndex.set(e.key, k);
    n += e.kind === 'block' ? e.n : 1;
  });
  dl.n = n;
  for (const key of dl.sel) if (!dl.topIndex.has(key)) dl.sel.delete(key);
  if (dl.fsel && !dl.blockAt.has(dl.fsel.id)) dl.fsel = null;
  if (dlIndexOf(dl.anchor) === undefined) dl.anchor = '';
  // A cursor row that went (a folded folder's file, one the filter took, a cleared download) goes
  // to its download, or else to the row now where it was.
  if (dl.cursor && dlIndexOf(dl.cursor) === undefined) {
    const up = dlIndexOf(`d${dl.cursorId}`);
    const i = up !== undefined ? up : Math.min(dl.cursorAt, n - 1);
    if (i >= 0) setDlCursor(dlRow(i)); else dl.cursor = '';
  } else if (dl.cursor) {
    dl.cursorAt = dlIndexOf(dl.cursor);
  }
  $('#queue').setAttribute('aria-rowcount', String(n + 1)); // and the header row
  dlKeepView();
  return true;
}

// Rows that came or went above the ones in view by themselves (a folder's list growing, a finished
// folder folding, rows the filter took) leave the view where it was: rows never move under the
// mouse (12). The entry of the first row in view (a download, a message row, a folder's files)
// stays where it was, at the same row of a folder's files, so a new order of those files shows in
// place; when the entry went, its download's row comes to the top. What the user asked for
// scrolls by itself (a sort or a filter set clears the anchor, Go to running, keeping a moved row
// in view) and still wins.
function dlKeepView() {
  if (!dl.viewKey || $('#downloads').hidden) return;
  const startOf = (key) => {
    const k = key[0] === 'b' ? dl.blockAt.get(Number(key.slice(1))) : dl.topIndex.get(key);
    return k === undefined ? undefined : [dl.starts[k], dl.top[k].kind === 'block' ? dl.top[k].n : 1];
  };
  let at = startOf(dl.viewKey);
  if (at === undefined) {
    at = startOf(dl.viewDl);
    if (at === undefined) return;
    dl.viewKey = dl.viewDl;
    dl.viewPos = 0;
    dl.viewOff = 0;
  }
  const i = at[0] + Math.min(dl.viewPos, at[1] - 1);
  if (i === dl.viewAt) return;
  dl.viewAt = i;
  $('#dl-spacer').style.height = `${dl.n * DL_ROW}px`; // before the scroll, which it bounds
  $('#dl-body').scrollTop = i * DL_ROW + dl.viewOff;
}

// The entry holding row `i` (a binary search over the entries' first rows).
function dlEntryAt(i) {
  let lo = 0;
  let hi = dl.top.length - 1;
  while (lo < hi) {
    const mid = (lo + hi + 1) >> 1;
    if (dl.starts[mid] <= i) lo = mid; else hi = mid - 1;
  }
  return lo;
}

// Row `i` of the table, or undefined past its ends. A file's row is made as it is asked for.
function dlRow(i) {
  if (!(i >= 0 && i < dl.n)) return undefined;
  const k = dlEntryAt(i);
  const e = dl.top[k];
  if (e.kind !== 'block') return e;
  const index = dl.stores.get(e.id).view[i - dl.starts[k]];
  return { kind: 'file', key: `f${e.id}:${index}`, id: e.id, index, scope: `f${e.id}` };
}

// The row index of a row key, or undefined when no row has it now.
function dlIndexOf(key) {
  if (!key) return undefined;
  if (key[0] === 'f') {
    const colon = key.indexOf(':');
    const id = Number(key.slice(1, colon));
    const index = Number(key.slice(colon + 1));
    const k = dl.blockAt.get(id);
    const st = dl.stores.get(id);
    if (k === undefined || !st || !(index >= 0 && index < st.total)) return undefined;
    const p = st.pos[index];
    return p >= 0 ? dl.starts[k] + p : undefined;
  }
  const k = dl.topIndex.get(key);
  return k === undefined ? undefined : dl.starts[k];
}

// Where the keyboard's cursor is (5.8); the selection is left as it is.
function setDlCursor(r) {
  dl.cursor = r.key;
  dl.cursorId = r.id;
  dl.cursorAt = dlIndexOf(r.key) ?? 0;
}

// ---- the selection (5.8): downloads by key, or files of one folder by index -------------------

function isSel(r) {
  if (r.kind === 'file') return !!(dl.fsel && dl.fsel.id === r.id && dl.fsel.on[r.index]);
  return dl.sel.has(selKey(r));
}
const hasSel = () => dl.sel.size > 0 || !!(dl.fsel && dl.fsel.on.some((x) => x));
function clearSel() {
  dl.sel.clear();
  dl.fsel = null;
}
// Adds a row to the selection, or drops it; a file of another folder than the selected ones (or
// a download among files) starts the selection again, which keeps to one level.
function markSel(r, on = true) {
  if (r.kind === 'file') {
    const st = dl.stores.get(r.id);
    if (!st) return;
    if (!dl.fsel || dl.fsel.id !== r.id) {
      dl.sel.clear();
      dl.fsel = { id: r.id, on: new Uint8Array(st.total) };
    }
    dl.fsel.on[r.index] = on ? 1 : 0;
  } else {
    dl.fsel = null;
    if (on) dl.sel.add(selKey(r)); else dl.sel.delete(selKey(r));
  }
}
// The rows from `a` to `b` of one level (`scope`) become the selection.
function selectRows(a, b, scope) {
  clearSel();
  if (a > b) [a, b] = [b, a];
  if (scope === 'd') {
    for (let k = dlEntryAt(a); k < dl.top.length && dl.starts[k] <= b; ++k) {
      const e = dl.top[k];
      if (e.kind !== 'block' && dl.starts[k] >= a) dl.sel.add(selKey(e));
    }
    return;
  }
  const id = Number(scope.slice(1));
  const k = dl.blockAt.get(id);
  const st = dl.stores.get(id);
  if (k === undefined || !st) return;
  const start = dl.starts[k];
  dl.fsel = { id, on: new Uint8Array(st.total) };
  for (let i = Math.max(a, start); i <= Math.min(b, start + st.view.length - 1); ++i) dl.fsel.on[st.view[i - start]] = 1;
}

// The selected downloads' rows in the table's order.
function selectedDl() {
  return [...dl.sel].map(dlIndexOf).filter((i) => i !== undefined).sort((a, b) => a - b).map(dlRow);
}

// The selected files shown, in the table's order: { q, st, list: [index, ...] }, or null.
function selectedFiles() {
  const f = dl.fsel;
  const st = f && dl.stores.get(f.id);
  const q = f && dl.byId.get(f.id);
  if (!st || !q) return null;
  const list = [];
  for (const i of st.view) if (f.on[i]) list.push(i);
  return list.length ? { q, st, list } : null;
}

// The first selected row in the table's order, or undefined.
function firstSelected() {
  const files = selectedFiles();
  if (files) return dlIndexOf(`f${files.q.id}:${files.list[0]}`);
  const rows = selectedDl();
  return rows.length ? dlIndexOf(rows[0].key) : undefined;
}

// File indices as the ranges "fileAction" takes: [[from, to], ...], `to` included.
function rangesOf(list) {
  const sorted = Int32Array.from(list).sort();
  const out = [];
  for (const i of sorted) {
    const last = out[out.length - 1];
    if (last && last[1] === i - 1) last[1] = i;
    else if (!last || last[1] < i) out.push([i, i]);
  }
  return out;
}

// What a progress bar and its percent show: full (done, or being verified), a part, or '-' while
// the size isn't known.
function progressOf(bytes, total, full) {
  if (full) return { frac: 1, pct: '100%' };
  if (!(total > 0)) return { frac: 0, pct: '-' };
  return { frac: Math.min(1, bytes / total), pct: fmtPct(bytes, total) };
}

// "85.7 MiB / 8.0 GiB" while partial, else the total; compact columns: the total, the pair in the
// tooltip.
function sizeOf(bytes, total, full) {
  if (!(total > 0)) return { size: bytes > 0 ? fmtSize(bytes) : '', sizeTip: '' };
  const partial = !full && bytes > 0 && bytes < total;
  if (dl.step === DL_COMPACT) return { size: fmtSize(total), sizeTip: partial ? `${fmtSize(bytes)} of ${fmtSize(total)}` : '' };
  return { size: partial ? fmtSizePair(bytes, total) : fmtSize(total), sizeTip: '' };
}

// The two action slots (5.4): [act, label, tooltip] or null, which keeps its place. A finished
// upload's Show opens Browse where it went.
function dlSlots(q) {
  const up = isUp(q);
  const cancel = ['cancel', 'Cancel', up ? 'Cancel the upload' : 'Cancel the download'];
  switch (q.status) {
    case 'queued': case 'running': return [['pause', 'Pause'], cancel];
    case 'paused': return [['resume', 'Resume'], cancel];
    case 'failed': return [q.bytes > 0 ? ['retry', 'Resume', up ? 'Resume from what was sent' : 'Resume from what was downloaded'] : ['retry', 'Retry'], cancel];
    case 'cancelled': return [['retry', 'Retry'], null];
    case 'done': case 'skipped': return [['show', 'Show', up ? 'Show on server' : 'Show in folder'], null];
    default: return [null, null];
  }
}

function dlView(q) {
  const st = q.status;
  const up = isUp(q);
  const phase = st === 'running' && PHASE_WORD[q.phase] ? q.phase : '';
  const fetching = st === 'running' && !phase;
  const f = q.files;
  let word = phase ? PHASE_WORD[phase] : DL_WORD[up && st === 'running' ? 'uploading' : st] || st;
  if (st === 'queued' && q.position) word = `Queued #${q.position}`;
  const tip = [phase ? (up ? UP_PHASE_TIP : PHASE_TIP)[phase] : st === 'queued' && q.position ? `Queued: #${q.position} to start` : word];
  let count = '';
  if (phase === 'verifying' && q.verified) {
    count = `${fmtCount(q.verified.done)}/${fmtCount(q.verified.total)}`;
    tip[0] += `: ${q.verified.done} of ${q.verified.total} files`;
  } else if (f) {
    count = `${fmtCount(f.done)}/${fmtCount(f.total - f.skipped)}`;
  }
  if (f) {
    const more = [['skipped', f.skipped], ['failed', f.failed], ['paused', f.paused]].filter(([, n]) => n > 0);
    tip.push([`${f.done} of ${f.total - f.skipped} files done`, ...more.map(([w, n]) => `${n} ${w}`)].join(' · '));
    // Big files that have their places but wait for its small files (Waiting on their rows).
    if (st === 'running' && f.held > 0) {
      tip.push(`${f.held === 1 ? 'A big file waits' : `${f.held} big files wait`}${WAIT_WHY.slice('Waits'.length)}`);
    }
  }
  if (fetching) {
    const parts = [];
    if (q.speed > 0) parts.push(fmtSpeed(q.speed));
    if (fmtEta(q.eta)) parts.push(fmtEta(q.eta));
    if (q.connections > 0) parts.push(`${q.connections} connection${q.connections === 1 ? '' : 's'}`);
    if (parts.length) tip.push(parts.join(' · '));
  }
  if (q.note) tip.push(q.note);
  if (dl.step >= 3 && UNFINISHED.includes(st)) tip.push(`Priority: ${prioLabel(q.priority)}`); // its column is hidden
  const full = st === 'done' || phase === 'verifying';
  const mark = q.check === 'ok' ? '✓' : q.check === 'warn' ? '⚠︎' : '';
  return {
    // Dimmed while it waits to leave under the filter.
    cls: `st-${up && st === 'running' ? 'uploading' : st}${up ? ' up' : ''}${dlMatches(q) ? '' : ' leave'}`,
    grip: true,
    toggle: q.files ? (folderIsOpen(q) ? '▾' : '▸') : '',
    icon: [q.name, q.mode === 'folder'],
    text: `${up ? '↑ ' : ''}${q.name}${q.mode === 'folder' ? '/' : ''}`,
    dest: up ? `→ ${q.remote}:${q.into || '~'}` : `→ ${q.dest}`,
    nameTip: up ? `${q.source}\n→ ${q.remote}:${q.path}` : `${q.remote}:${q.path}\n→ ${q.local || q.dest}`,
    word,
    wordCls: st === 'failed' ? 'bad' : '',
    statusTip: tip.join('\n'),
    count,
    mark,
    markCls: q.check || '',
    markTip: mark ? q.note || q.error || (q.check === 'ok' ? 'Verified' : 'Not verified') : '',
    ...progressOf(q.bytes, q.total, full),
    ...sizeOf(q.bytes, q.total, full),
    speed: fetching && q.speed > 0 ? fmtSpeed(q.speed) : '',
    speedTip: fetching && q.connections > 0 ? `${q.connections} connection${q.connections === 1 ? '' : 's'}` : '',
    eta: fetching ? fmtClock(q.eta) : '',
    etaTip: fetching ? fmtEta(q.eta) : '',
    prio: UNFINISHED.includes(st) ? q.priority : '',
    prioTip: 'Bandwidth priority: how running downloads share the speed',
    slots: dlSlots(q),
  };
}

// A file's status follows its folder's (5.3): queued under a paused folder it is Paused (it read
// "Queued" before), under a failed one Stopped, and unfinished under a cancelled one Cancelled,
// all muted, the tooltip saying why.
function fileStatus(q, c) {
  if (c.status === 'done') return { word: 'Done' };
  if (c.status === 'skipped') return c.error ? { word: 'Skipped', why: `Skipped: ${c.error}` } : { word: 'Skipped' };
  if (q.status === 'cancelled') return { word: 'Cancelled', muted: true, why: 'The folder was cancelled', over: true };
  if (c.status === 'running') return { word: isUp(q) ? 'Uploading' : 'Downloading' };
  if (c.status === 'waiting') return { word: 'Waiting', muted: true, why: WAIT_WHY };
  if (c.status === 'paused') return { word: 'Paused' };
  if (c.status === 'failed') return { word: 'Failed', bad: true, why: c.error || 'Failed' };
  if (q.status === 'paused') return { word: 'Paused', muted: true, why: 'The folder is paused', withFolder: true };
  if (q.status === 'failed') return { word: 'Stopped', muted: true, why: `The folder stopped: ${q.error}`, withFolder: true };
  return { word: 'Queued' };
}

// A file's two slots (5.4): Pause or Resume and Skip while it comes, Retry and Skip once failed,
// Show once done (an upload's: on the server); Skip alone while it waits with its folder; none
// once skipped or cancelled.
function fileSlots(c, s, up = false) {
  if (s.over || c.status === 'skipped') return [null, null];
  const skip = ['skip', 'Skip', 'Skip this file'];
  switch (c.status) {
    case 'done': return [['show', 'Show', up ? 'Show on server' : 'Show in folder'], null];
    case 'failed': return [['retry', 'Retry', up ? 'Upload this file again' : 'Download this file again'], skip];
    case 'paused': return [['resume', 'Resume'], skip];
    default: return [s.withFolder ? null : ['pause', 'Pause'], skip];
  }
}

// Whether file `i` has a priority to set (DOWNLOADS-UI.md 7): unfinished, its folder not cancelled.
function fileHasPrio(q, st, i) {
  const s = st.status[i];
  return q.status !== 'cancelled' && s !== FS.done && s !== FS.skipped;
}

function fileView(q, st, i) {
  const c = fileOf(st, i);
  const s = fileStatus(q, c);
  const running = c.status === 'running' && !s.over;
  const tip = [s.why || s.word];
  if (running && (c.speed > 0 || fmtEta(c.eta))) tip.push([c.speed > 0 ? fmtSpeed(c.speed) : '', fmtEta(c.eta)].filter(Boolean).join(' · '));
  const prio = fileHasPrio(q, st, i) ? c.priority : '';
  if (dl.step >= 3 && prio) tip.push(`Priority: ${prioLabel(prio)}`); // its column is hidden
  const full = c.status === 'done';
  const total = full ? c.bytes || c.size : c.size; // done: as fetched
  // Saved under another name: one grab gave it (Keep both, or another file of the folder has its
  // name on Windows), shown after an arrow; or the server's name made safe for Windows. An
  // upload's file goes under its own name.
  const up = isUp(q);
  const name = c.local.slice(c.local.lastIndexOf('\\') + 1);
  return {
    cls: `st-${up && c.status === 'running' ? 'uploading' : c.status}${s.muted ? ' w-muted' : ''}${fileMatches(st, i) ? '' : ' leave'}`,
    grip: false,
    toggle: '',
    icon: [c.path, false],
    text: c.named ? `${c.path} → ${name}` : c.path,
    dest: '',
    nameTip: up ? `${c.path}\n→ ${q.remote}:${serverChild(q.path, c.path)}`
      : c.named ? `${c.path}\nSaved as ${name}: a file with its name on Windows is there or in the folder`
        : c.local ? `${c.path}\nSaved as ${c.local}: Windows can’t take its name as it is` : c.path,
    word: s.word,
    wordCls: s.bad ? 'bad' : s.muted ? 'muted' : '',
    statusTip: tip.join('\n'),
    count: '',
    mark: c.verified ? '✓' : '',
    markCls: c.verified ? 'ok' : '',
    markTip: c.verified ? 'Verified against the server’s checksum' : '',
    ...progressOf(c.bytes, total, full),
    ...sizeOf(c.bytes, total, full),
    speed: running && c.speed > 0 ? fmtSpeed(c.speed) : '',
    speedTip: '',
    eta: running ? fmtClock(c.eta) : '',
    etaTip: running ? fmtEta(c.eta) : '',
    prio,
    prioTip: 'Priority in the folder: higher ones start first, and running files of 2 MiB and more share ' +
      'the folder’s connections and speed by it',
    slots: fileSlots(c, s, up),
  };
}

function msgView(q, r) {
  switch (r.what) {
    case 'error': return { msg: q.error, cls: 'm-error' };
    case 'note': return { msg: q.waiting, cls: 'm-note' };
    case 'unreadable': return { msg: q.unreadable, cls: 'm-warn' };
    default: { // the list of a folder's files on its way
      const st = dl.stores.get(q.id);
      const total = st ? st.total : q.files ? q.files.total : 0;
      return { msg: `Loading the list of ${total.toLocaleString('en-US')} files…${st && st.loaded ? ` (${st.loaded.toLocaleString('en-US')})` : ''}`, cls: 'm-sub' };
    }
  }
}

// A row element of the pool: a treegrid row (5.8) with an id of its own, which
// aria-activedescendant names while it shows the cursor row.
function newDlRow() {
  const el = document.createElement('div');
  el.id = `dl-r${dl.pool.length}`;
  el.setAttribute('role', 'row');
  el.innerHTML = '<span class="c-handle" role="gridcell"><span class="dl-grip">≡</span>' +
    '<button type="button" class="dl-toggle" tabindex="-1" data-act="toggle"></button></span>' +
    '<span class="c-name" role="gridcell"><span class="dl-ico"></span><span class="dl-text"></span><span class="dl-dest"></span></span>' +
    '<span class="c-status" role="gridcell"><span class="dl-word"></span><span class="dl-count"></span><span class="dl-mark"></span></span>' +
    '<span class="c-progress" role="gridcell"><span class="dl-bar"><i></i></span><span class="dl-pct"></span></span>' +
    '<span class="c-size" role="gridcell"></span><span class="c-speed" role="gridcell"></span><span class="c-eta" role="gridcell"></span>' +
    '<span class="c-prio" role="gridcell"><select class="dl-prio" tabindex="-1" aria-label="Priority">' +
    PRIORITIES.map(([p, label]) => `<option value="${p}">${label}</option>`).join('') + '</select></span>' +
    '<span class="c-actions" role="gridcell"><span class="dl-slot"></span><span class="dl-slot"></span></span>' +
    '<span class="c-msg" role="gridcell"></span>';
  const one = (sel) => el.querySelector(sel);
  el.c = {
    grip: one('.dl-grip'),
    toggle: one('.dl-toggle'), name: one('.c-name'), ico: one('.dl-ico'), text: one('.dl-text'), dest: one('.dl-dest'),
    status: one('.c-status'), word: one('.dl-word'), count: one('.dl-count'), mark: one('.dl-mark'),
    bar: one('.dl-bar > i'), pct: one('.dl-pct'), size: one('.c-size'), speed: one('.c-speed'), eta: one('.c-eta'),
    prio: one('.dl-prio'), slots: [...el.querySelectorAll('.dl-slot')], msg: one('.c-msg'),
  };
  el.v = {};       // each cell's value as last written
  el.dlKey = '';   // the row it shows
  el.pinned = false;
  $('#dl-body').append(el);
  dl.pool.push(el);
  return el;
}

// Writes a cell only when its value changed (6.1): a node rewritten on every tick loses a click
// on a button under the mouse (the double-click bug of 2026-10-16).
function put(el, key, value, write) {
  if (el.v[key] === value) return;
  el.v[key] = value;
  write(value);
}

function slotHtml(s) {
  if (!s) return '';
  const [act, label, tip = label] = s;
  return `<button type="button" tabindex="-1" data-act="${act}" title="${tip}" aria-label="${tip}">${SLOT_ICONS[act]}<span>${label}</span></button>`;
}

function paintDlRow(el, r, i) {
  const q = dl.byId.get(r.id);
  const st = r.kind === 'file' ? dl.stores.get(r.id) : null;
  if (!q || (r.kind === 'file' && !st)) return;
  const c = el.c;
  put(el, 'hidden', false, (h) => { el.hidden = h; });
  put(el, 'y', i, (y) => { el.style.transform = `translateY(${y * DL_ROW}px)`; });
  put(el, 'key', r.key, (k) => { el.dataset.key = k; });
  const view = r.kind === 'dl' ? dlView(q) : r.kind === 'file' ? fileView(q, st, r.index) : msgView(q, r);
  const d = dl.drag;
  const selected = isSel(r);
  const cls = `dl-row dl-grid k-${r.kind} ${view.cls}${selected ? ' sel' : ''}${r.key === dl.cursor ? ' cursor' : ''}` +
    `${d && d.moved && d.key === r.key ? ' dragging' : ''}`;
  put(el, 'cls', cls, (v) => { el.className = v; });
  // The treegrid's row (5.8): its place counting the header row, downloads at level 1 and what is
  // under them at 2, a folder's ▸/▾ and whether it is selected (a message row: its download).
  put(el, 'rowIndex', i + 2, (v) => el.setAttribute('aria-rowindex', String(v)));
  put(el, 'level', r.kind === 'dl' ? 1 : 2, (v) => el.setAttribute('aria-level', String(v)));
  put(el, 'selected', selected, (v) => el.setAttribute('aria-selected', String(v)));
  put(el, 'expanded', r.kind === 'dl' && q.files ? String(folderIsOpen(q)) : '', (v) => {
    if (v) el.setAttribute('aria-expanded', v); else el.removeAttribute('aria-expanded');
  });
  if (r.kind === 'msg') {
    put(el, 'msg', view.msg, (t) => { c.msg.textContent = bidiVisible(t); c.msg.title = bidiVisible(t); });
    return; // its other cells are hidden
  }
  // The ≡ drags in the queue order only (5.6).
  put(el, 'grip', dlSort.key ? `Dragging works in the queue order: ${dlQueueOrderHint()}` : 'Drag to change the order',
    (t) => { c.grip.title = t; });
  put(el, 'toggle', view.toggle, (t) => {
    c.toggle.textContent = t;
    c.toggle.style.visibility = t ? '' : 'hidden';
    c.toggle.title = t === '▾' ? 'Hide files' : 'Show files';
    c.toggle.setAttribute('aria-expanded', String(t === '▾'));
  });
  setIcon(c.ico, view.icon[0], view.icon[1]);
  put(el, 'text', view.text, (t) => { c.text.textContent = bidiVisible(t); });
  put(el, 'dest', view.dest, (t) => { c.dest.textContent = bidiVisible(t); });
  put(el, 'nameTip', view.nameTip, (t) => { c.name.title = bidiVisible(t); });
  put(el, 'word', view.word, (t) => { c.word.textContent = t; });
  put(el, 'wordCls', view.wordCls, (v) => { c.word.className = `dl-word ${v}`; });
  put(el, 'statusTip', view.statusTip, (t) => { c.status.title = bidiVisible(t); });
  put(el, 'count', view.count, (t) => { c.count.textContent = t; });
  put(el, 'mark', view.mark, (t) => { c.mark.textContent = t; });
  put(el, 'markCls', view.markCls, (v) => { c.mark.className = `dl-mark ${v}`; });
  put(el, 'markTip', view.markTip, (t) => { c.mark.title = bidiVisible(t); });
  put(el, 'frac', view.frac, (f) => { c.bar.style.transform = `scaleX(${f})`; });
  put(el, 'pct', view.pct, (t) => { c.pct.textContent = t; });
  put(el, 'size', view.size, (t) => { c.size.textContent = t; });
  put(el, 'sizeTip', view.sizeTip, (t) => { c.size.title = t; });
  put(el, 'speed', view.speed, (t) => { c.speed.textContent = t; });
  put(el, 'speedTip', view.speedTip, (t) => { c.speed.title = t; });
  put(el, 'eta', view.eta, (t) => { c.eta.textContent = t; });
  put(el, 'etaTip', view.etaTip, (t) => { c.eta.title = t; });
  put(el, 'prio', view.prio, (p) => {
    c.prio.style.visibility = p ? '' : 'hidden';
    c.prio.className = `dl-prio prio-${p || 'normal'}`;
  });
  put(el, 'prioTip', view.prioTip, (t) => { c.prio.title = t; });
  if (view.prio && document.activeElement !== c.prio && c.prio.value !== view.prio) c.prio.value = view.prio;
  view.slots.forEach((s, k) => put(el, `slot${k}`, s ? s.join('|') : '', () => {
    if (c.slots[k].contains(document.activeElement)) dlFocus();
    c.slots[k].innerHTML = slotHtml(s);
  }));
}

// Binds the rows in view (and DL_OVER more each side) to elements: one already showing a row
// keeps it, so a row that only moves keeps its element; the others come from the pool. Two rows
// keep their element out of view too: the cursor row, so aria-activedescendant never names an
// element recycled for another row (5.8), and a pinned one (the row being dragged).
function renderDlBody() {
  const body = $('#dl-body');
  if ($('#downloads').hidden) return;
  const n = dl.n;
  $('#dl-spacer').style.height = `${n * DL_ROW}px`;
  const first = Math.max(0, Math.floor(body.scrollTop / DL_ROW) - DL_OVER);
  const last = Math.min(n, first + DL_POOL - 2, Math.ceil((body.scrollTop + body.clientHeight) / DL_ROW) + DL_OVER);
  const want = new Map(); // row key -> [row, index]
  const keep = (i) => { const r = dlRow(i); if (r) want.set(r.key, [r, i]); };
  for (let i = first; i < last; ++i) keep(i);
  const viewAt = Math.floor(body.scrollTop / DL_ROW);
  if (viewAt < n) {
    const k = dlEntryAt(viewAt);
    dl.viewKey = dl.top[k].key;
    dl.viewDl = `d${dl.top[k].id}`;
    dl.viewPos = viewAt - dl.starts[k];
    dl.viewAt = viewAt;
    dl.viewOff = body.scrollTop - viewAt * DL_ROW;
  } else {
    dl.viewKey = '';
  }
  const cursorAt = dlIndexOf(dl.cursor);
  if (cursorAt !== undefined) keep(cursorAt);
  for (const el of dl.pool) {
    if (!el.pinned) continue;
    const i = dlIndexOf(el.dlKey);
    if (i !== undefined) keep(i);
  }
  const free = [];
  for (const el of dl.pool) {
    if (el.dlKey && want.has(el.dlKey)) continue;
    if (el.dlKey) dl.bound.delete(el.dlKey);
    if (el.contains(document.activeElement)) dlFocus(); // keys never act on a recycled row (5.8)
    el.dlKey = '';
    // One element per data-key: a freed one loses its key, written again when it shows a row.
    if (el.v.key !== undefined) { delete el.v.key; delete el.dataset.key; }
    free.push(el);
  }
  for (const [key, [r, i]] of want) {
    let el = dl.bound.get(key);
    if (!el) {
      el = free.pop() || newDlRow();
      el.dlKey = key;
      dl.bound.set(key, el);
    }
    paintDlRow(el, r, i);
  }
  for (const el of free) put(el, 'hidden', true, (h) => { el.hidden = h; });
  const grid = $('#queue');
  const cur = dl.bound.get(dl.cursor);
  if (!cur) grid.removeAttribute('aria-activedescendant');
  else if (grid.getAttribute('aria-activedescendant') !== cur.id) grid.setAttribute('aria-activedescendant', cur.id);
}

function scheduleDl() {
  if (!dl.frame) dl.frame = requestAnimationFrame(() => { dl.frame = 0; renderDlBody(); });
}

// The narrow step and the head's summary, from the panel's width (and the widths dragged by hand,
// a setting: never the data). Not while a title's edge is dragged: a column appearing or going
// then would move the edge away from the pointer.
function updateDlStep() {
  const panel = $('#downloads');
  const width = $('#dl-body').clientWidth;
  if (panel.hidden || !width || dl.resize) return;
  let step = dl.need.findIndex((need) => width >= need);
  if (step < 0) step = DL_COMPACT;
  if (panel.dataset.step !== String(step)) {
    dl.step = step;
    panel.dataset.step = String(step);
    // A title's edge drags while a column shown follows it; compact columns keep their widths.
    const cols = dlResizable();
    for (const h of document.querySelectorAll('.dl-head .dl-resize')) {
      const k = cols.indexOf(h.dataset.col);
      h.hidden = k < 0 || k === cols.length - 1;
    }
    renderDlSortHead(); // a sorted column hidden or shown again: Name's title shows its sort
    scheduleDl(); // compact sizes, the Status tooltip's priority and the grips' tooltip follow it
  }
  panel.classList.toggle('narrow-head', panel.clientWidth < DL_HEAD_NARROW);
}

function rowOfEl(el) {
  const i = el ? dlIndexOf(el.dlKey) : undefined;
  return i === undefined ? null : dlRow(i);
}

// Scrolls the body just enough to show row `i`.
function dlKeepInView(i) {
  const body = $('#dl-body');
  const top = i * DL_ROW;
  if (top < body.scrollTop) body.scrollTop = top;
  else if (top + DL_ROW > body.scrollTop + body.clientHeight) body.scrollTop = top + DL_ROW - body.clientHeight;
}

// ---- sorting the downloads (5.6) and the column widths (5.7) ---------------------------------
//
// A click on a title sorts the downloads by that column, a second click the other way, a third
// goes back to the queue order (the default), as in Browse. Ties keep the queue order. A folder's
// files are sorted the same way under it (updateFileView), ties in their start order. gui.json
// keeps the sort and the widths (downloads.sort, downloads.widths) through "prefs". The filter
// (Show) is for the session.

// The first click's direction: names A to Z, what runs first (DL_STATUS_RANK), the most done, the
// largest, the fastest, the least time left, High first.
const DL_SORT_FIRST = { name: 'asc', status: 'asc', progress: 'desc', size: 'desc', speed: 'desc', eta: 'asc', priority: 'desc' };
const DL_STATUS_RANK = { running: 0, queued: 1, paused: 2, failed: 3, cancelled: 4, skipped: 5, done: 6 };
const PRIORITY_RANK = { high: 3, normal: 2, low: 1 };
const DL_RESORT_MS = 1000; // a new order from changed values comes at most once a second
const DL_KEY_HOLD_MS = 2000; // and not this soon after a key in the table (the cursor's row stays put)
const DL_COL_LABEL = { name: 'Name', status: 'Status', progress: 'Progress', size: 'Size', speed: 'Speed', eta: 'ETA', priority: 'Priority' };

// The sort: `key` the column ('' for the queue order) and `dir`. Sorted or filtered, `order` holds
// the download ids as shown; `at` when a new order last came; `pending` while one waits (a dot by
// the arrow), `pendingDl` while the downloads' own does; `over` while the pointer is over the
// table; `force` makes the next order come at once (a click, the filter, gui.json); `timer` and
// `due` the next look at a waiting one.
const dlSort = { key: '', dir: 'asc', order: null, at: 0, pending: false, pendingDl: false, over: false, force: false, timer: 0, due: 0 };

// Fetching: running, past listing and checking and not yet verifying (when a speed and a time
// left show).
const dlFetching = (q) => q.status === 'running' && !PHASE_WORD[q.phase];

// What a download sorts by on `key`, as its row shows it; null when it shows none (a size not
// known yet, no speed while not fetching, no priority once finished): those go last both ways.
function dlSortValue(q, key) {
  switch (key) {
    case 'name': return asciiLower(q.name);
    case 'status': return DL_STATUS_RANK[q.status] ?? Object.keys(DL_STATUS_RANK).length;
    case 'progress':
      if (q.status === 'done' || (q.status === 'running' && q.phase === 'verifying')) return 1;
      return q.total > 0 ? Math.min(1, q.bytes / q.total) : null;
    case 'size': return q.total > 0 ? q.total : null;
    case 'speed': return dlFetching(q) && q.speed > 0 ? q.speed : null;
    case 'eta': return dlFetching(q) && Number.isFinite(q.eta) && q.eta >= 0 ? q.eta : null;
    default: return UNFINISHED.includes(q.status) ? PRIORITY_RANK[q.priority] || PRIORITY_RANK.normal : null;
  }
}

// Compares two of { q, v (dlSortValue), ord (place in the queue) } for the sort: names naturally
// ("file 9" before "file 10"), numbers by value; no value last both ways; ties in queue order.
function dlCompare(key, dir) {
  const desc = dir === 'desc';
  return (x, y) => {
    if (x.v === null || y.v === null) return x.v === y.v ? x.ord - y.ord : x.v === null ? 1 : -1;
    const c = key === 'name' ? naturalCompare(x.q.name, y.q.name, x.v, y.v) : x.v - y.v;
    if (c) return desc ? -c : c;
    return x.ord - y.ord;
  };
}

// The downloads in the order shown.
function dlOrdered() {
  return dlSort.order ? dlSort.order.map((id) => dl.byId.get(id)).filter(Boolean) : state.queue;
}

// A look at what waits (dlRecheck) in `ms`, unless one comes sooner already.
function dlRecheckIn(ms) {
  const due = performance.now() + ms;
  if (dlSort.timer && dlSort.due <= due) return;
  clearTimeout(dlSort.timer);
  dlSort.due = due;
  dlSort.timer = setTimeout(dlRecheck, ms);
}

const sameIds = (a, b) => a.length === b.length && a.every((id, i) => id === b[i]);

// Sets the order shown from state.queue; returns whether it changed. The queue order follows the
// queue. Sorted or filtered, a click, the filter or gui.json's sort comes at once, and so does a
// download added or removed: it goes in its place, the others stay as they are. A new order from
// changed values (a status, the bytes, the speed, a priority, a size once known, a download the
// filter no longer shows) waits: at most once a second, and not while the pointer is over the
// table, a menu is open or a drag runs, so a row never moves under the mouse (5.6). The title's dot
// says one waits; it comes when the pointer leaves. A row waiting to leave is dimmed.
function updateDlOrder() {
  const shown = dlSort.order;
  if (!dlSort.key && dl.show === 'all') {
    dlSort.order = null;
    dlSort.force = false;
    setDlPending(false);
    return shown !== null;
  }
  const cmp = dlSort.key ? dlCompare(dlSort.key, dlSort.dir) : (x, y) => x.ord - y.ord;
  const items = state.queue.map((q, ord) => ({ id: q.id, q, ord, v: dlSort.key ? dlSortValue(q, dlSort.key) : 0 }));
  const want = items.filter((x) => dlMatches(x.q)).sort(cmp).map((x) => x.id);
  const now = performance.now();
  if (!shown || dlSort.force) {
    dlSort.force = false;
    dlSort.order = want;
    dlSort.at = now;
    setDlPending(false);
    return !shown || !sameIds(shown, want);
  }
  // The rows shown stay; new downloads the filter shows go before the first one they sort ahead of.
  const byId = new Map(items.map((x) => [x.id, x]));
  const kept = shown.filter((id) => byId.has(id));
  const known = new Set(kept);
  const fresh = items.filter((x) => !known.has(x.id) && dlMatches(x.q)).sort(cmp);
  let order = kept;
  if (!dlSort.key) {
    // Queue order changes only by the user's moves, adds and removes: those show at once, and only
    // rows that stopped matching the filter wait.
    order = items.filter((x) => known.has(x.id) || dlMatches(x.q)).map((x) => x.id);
  } else if (fresh.length) {
    order = [];
    let k = 0;
    for (const id of kept) {
      while (k < fresh.length && cmp(fresh[k], byId.get(id)) < 0) order.push(fresh[k++].id);
      order.push(id);
    }
    while (k < fresh.length) order.push(fresh[k++].id);
  }
  const changed = !sameIds(order, shown);
  if (sameIds(order, want)) {
    dlSort.order = want;
    setDlPending(false);
    return changed;
  }
  const wait = dlSort.at + DL_RESORT_MS - now;
  if (wait <= 0 && !dlResortHeld()) {
    dlSort.order = want;
    dlSort.at = now;
    setDlPending(false);
    return true;
  }
  dlSort.order = order;
  setDlPending(true);
  dlRecheckIn(Math.max(wait, 250));
  return changed;
}

// Whatever keeps a new order waiting besides the once-a-second rule. The pointer counts by its
// events and by :hover, which also catches it over the table once a menu above the table closes;
// the keys count for a while after the last one, so the cursor's row doesn't run off while the
// keys walk the rows.
function dlResortHeld() {
  return dlSort.over || $('#queue').matches(':hover') || !$('#menu').hidden || !!dl.drag || !!dl.resize ||
    (document.activeElement === $('#queue') && performance.now() - dl.keyAt < DL_KEY_HOLD_MS);
}

// Again, for an order that waits (the downloads', a folder's files'): its second passed, the
// pointer left, a menu closed.
function dlRecheck() {
  dlSort.timer = 0;
  if (!dlSort.pending || $('#downloads').hidden) return;
  let moved = updateDlOrder();
  for (const st of dl.stores.values()) moved = updateFileView(st, false) || moved;
  setDlPending(dlSort.pendingDl);
  if (moved) {
    buildDlRows();
    renderDlBody();
  }
}

// Whether the downloads' new order waits (`on`); the title's dot shows while it or a folder's
// files' does.
function setDlPending(on) {
  dlSort.pendingDl = on;
  const pending = on || filesPending();
  if (dlSort.pending === pending) return;
  dlSort.pending = pending;
  renderDlSortHead();
}

// The sorted column when the narrow step hides it (compact hides what step 3 does), else ''.
function dlSortHidden() {
  const hides = DL_HIDES[Math.min(dl.step, DL_HIDES.length - 1)];
  return dlSort.key && hides.includes(dlSort.key) ? dlSort.key : '';
}

// How to get back to the queue order, for the moves' note and the grip's tooltip.
function dlQueueOrderHint() {
  const col = DL_COL_LABEL[dlSort.key];
  return dlSortHidden() ? `sorted by ${col}, whose column is hidden: click Name’s title, or right-click the titles → Queue order`
    : `click the ${col} title until its arrow goes`;
}

// The titles: the arrow (and aria-sort) on the sorted one, the dot while a new order waits; the
// ≡ grips dimmed while sorted. When the narrow step hides the sorted column, Name's title shows
// the arrow and the column ("Name · by Speed"), and a click on it goes back to the queue order.
function renderDlSortHead() {
  $('#downloads').classList.toggle('sorted', !!dlSort.key);
  const hidden = dlSortHidden();
  const mark = hidden ? 'name' : dlSort.key;
  for (const b of document.querySelectorAll('.dl-head [data-sort]')) {
    const on = !!mark && b.dataset.sort === mark;
    const cell = b.parentElement;
    const lbl = b.querySelector('.lbl');
    if (b.dataset.tip === undefined) b.dataset.tip = b.title;
    if (lbl.dataset.text === undefined) lbl.dataset.text = lbl.textContent;
    if (on) {
      b.dataset.dir = dlSort.dir;
      cell.setAttribute('aria-sort', hidden ? 'other' : dlSort.dir === 'asc' ? 'ascending' : 'descending');
    } else {
      delete b.dataset.dir;
      cell.removeAttribute('aria-sort');
    }
    const text = on && hidden ? `${lbl.dataset.text} · by ${DL_COL_LABEL[hidden]}` : lbl.dataset.text;
    if (lbl.textContent !== text) lbl.textContent = text;
    // Hollow: a big folder shown keeps the order of the click (5.6).
    const frozen = on && !hidden && DL_LIVE_COLS.has(dlSort.key) && [...dl.stores.values()].some((st) => st.total > DL_LIVE_FILES);
    b.querySelector('.arrow').textContent = on ? (dlSort.dir === 'asc' ? (frozen ? '▵' : '▴') : (frozen ? '▿' : '▾')) : '';
    b.toggleAttribute('data-pending', on && dlSort.pending);
    let tip = on && hidden ? `Sorted by ${DL_COL_LABEL[hidden]} (its column is hidden): click for the queue order` : b.dataset.tip;
    if (frozen) tip += `\nA folder of more than ${DL_LIVE_FILES.toLocaleString('en-US')} files keeps the order of your click; it isn’t sorted again as the numbers change`;
    b.title = on && dlSort.pending ? `${tip}\nA new order waits until the pointer leaves the table` : tip;
  }
}

// A click on a title: its first direction, then the other, then the queue order; the cursor row
// stays in view. Name's title showing a hidden column's sort goes back to the queue order.
function clickDlSort(key) {
  if (key === 'name' && dlSortHidden()) {
    dlSort.key = '';
  } else if (dlSort.key !== key) {
    dlSort.key = key;
    dlSort.dir = DL_SORT_FIRST[key];
  } else if (dlSort.dir === DL_SORT_FIRST[key]) {
    dlSort.dir = dlSort.dir === 'asc' ? 'desc' : 'asc';
  } else {
    dlSort.key = '';
  }
  applyDlSort();
}

// The sort just set, shown at once and kept in gui.json.
function applyDlSort() {
  showDlOrder();
  saveDlView();
}

// The order and the filter just set, shown at once: the downloads and every open folder's files.
function showDlOrder() {
  dl.viewKey = ''; // the user's new order shows from where the body is scrolled
  dlSort.force = true;
  updateDlOrder();
  for (const st of dl.stores.values()) updateFileView(st, true);
  setDlPending(false);
  renderDlSortHead();
  buildDlRows();
  const cur = dlIndexOf(dl.cursor);
  if (cur !== undefined) dlKeepInView(cur);
  renderDlBody();
}

// gui.json's downloads view, as "prefs" sends it whole: the sort (null for the queue order) and
// the widths dragged by hand.
function saveDlView() {
  send({ type: 'prefs', downloads: { sort: dlSort.key ? { key: dlSort.key, dir: dlSort.dir } : null, widths: { ...dl.widths } } });
}

// The columns whose title's right edge drags, in order, each with its least width. Name stretches:
// it has no width of its own, only its least, and its edge trades with Status's width.
const DL_COLS = ['name', 'status', 'progress', 'size', 'speed', 'eta', 'priority'];
// The least widths hold the fixed formats: Status its word ("Downloading", "Queued #123") beside
// the count's 74 px and the mark's 16, Size a pair ("1023.9 MiB / 1023.9 GiB"), Speed
// "1023.9 KiB/s", ETA "99:59:59", Progress the bar's 16 px and the percent.
const DL_MIN_W = { name: DL_W.name, status: 174, progress: 64, size: 124, speed: 66, eta: 44, priority: 64 };
const DL_MAX_W = 4000; // gui_state's max_column_px

function dlWidth(col) { return dl.widths[col] ?? DL_W[col]; }

// The columns shown now that a drag can resize; none in compact columns, which keep their own.
function dlResizable() {
  return dl.step === DL_COMPACT ? [] : DL_COLS.filter((c) => !DL_HIDES[dl.step].includes(c));
}

// The panel width each narrow step needs (DL_NEED with the widths dragged by hand).
function dlNeeds() {
  return DL_HIDES.map((hide) => {
    const shown = Object.keys(DL_W).filter((k) => !hide.includes(k)).map(dlWidth);
    return 2 * DL_PAD + shown.reduce((a, b) => a + b, 0) + DL_GAP * (shown.length - 1);
  });
}

function setDlWidth(col, px) {
  px = Math.round(px);
  if (px === DL_W[col]) delete dl.widths[col];
  else dl.widths[col] = px;
}

// The widths onto the panel (app.css --u-<column>), and the narrow steps from them.
function applyDlWidths(steps = true) {
  const panel = $('#downloads');
  for (const col of DL_COLS) {
    if (col === 'name') continue;
    if (dl.widths[col] !== undefined) panel.style.setProperty(`--u-${col}`, `${dl.widths[col]}px`);
    else panel.style.removeProperty(`--u-${col}`);
  }
  dl.need = dlNeeds();
  if (steps) updateDlStep();
}

// A drag of `col`'s right edge from where it started (5.7): `col` follows the pointer and the next
// column shown gives or takes the room, so nothing else moves (Name's edge: Status gives or takes
// it). Neither gets under its least width.
function startDlResize(h, x, pointerId) {
  const cols = dlResizable();
  const col = h.dataset.col;
  const next = cols[cols.indexOf(col) + 1];
  if (!cols.includes(col) || !next) return false;
  const a0 = col === 'name' ? $('.dl-head .c-name').getBoundingClientRect().width : dlWidth(col);
  dl.resize = { pointerId, h, col, next, x0: x, a0, b0: dlWidth(next), moved: false };
  h.classList.add('dragging');
  document.documentElement.classList.add('dl-resizing');
  return true;
}

function moveDlResize(x) {
  const z = dl.resize;
  const lo = Math.ceil(DL_MIN_W[z.col] - z.a0);
  const hi = Math.floor(z.b0 - DL_MIN_W[z.next]);
  const d = lo > hi ? 0 : Math.min(hi, Math.max(lo, Math.round(x - z.x0)));
  if (z.col !== 'name') setDlWidth(z.col, z.a0 + d);
  setDlWidth(z.next, z.b0 - d);
  if (d) z.moved = true;
  applyDlWidths(false);
}

function endDlResize() {
  const z = dl.resize;
  if (!z) return;
  dl.resize = null;
  z.h.classList.remove('dragging');
  document.documentElement.classList.remove('dl-resizing');
  applyDlWidths();
  if (z.moved) saveDlView();
  dlRecheck();
}

// A double-click on the edge: the columns its drag resizes get their default widths again.
function resetDlWidths(col) {
  const cols = dlResizable();
  const next = cols[cols.indexOf(col) + 1];
  if (!cols.includes(col) || !next) return;
  const before = JSON.stringify(dl.widths);
  delete dl.widths[col];
  delete dl.widths[next];
  applyDlWidths();
  if (JSON.stringify(dl.widths) !== before) saveDlView();
}

// The titles' menu: every column at its default width. It works at every step, compact too,
// where no edge shows to double-click.
function resetAllDlWidths() {
  if (!Object.keys(dl.widths).length) return;
  dl.widths = {};
  applyDlWidths();
  saveDlView();
}

// gui.json's downloads view from the first init: a sort on a known column and direction, widths of
// known columns, each kept within its least and DL_MAX_W; anything else is the default.
function applyDlView(v) {
  const s = v && typeof v === 'object' ? v.sort : null;
  if (s && typeof s === 'object' && Object.hasOwn(DL_SORT_FIRST, s.key) && (s.dir === 'asc' || s.dir === 'desc')) {
    dlSort.key = s.key;
    dlSort.dir = s.dir;
  }
  const w = v && typeof v === 'object' ? v.widths : null;
  if (w && typeof w === 'object') {
    for (const col of DL_COLS) {
      if (col === 'name' || typeof w[col] !== 'number' || !Number.isFinite(w[col])) continue;
      setDlWidth(col, Math.min(DL_MAX_W, Math.max(DL_MIN_W[col], w[col])));
    }
  }
  dlSort.force = true;
  renderDlSortHead();
  applyDlWidths();
  renderQueue();
}

{
  const head = $('.dl-head');
  // The mouse leaves the focus where it was (the keys go on in the table); Tab reaches the titles,
  // and Enter or Space clicks them.
  head.addEventListener('mousedown', (ev) => ev.preventDefault());
  head.addEventListener('click', (ev) => {
    const b = ev.target.closest('button[data-sort]');
    if (b) clickDlSort(b.dataset.sort);
  });
  head.addEventListener('pointerdown', (ev) => {
    const h = ev.target.closest('.dl-resize');
    if (!h || ev.button !== 0 || dl.resize || dl.drag) return;
    ev.preventDefault();
    closeMenu();
    if (startDlResize(h, ev.clientX, ev.pointerId)) h.setPointerCapture(ev.pointerId);
  });
  head.addEventListener('pointermove', (ev) => {
    if (dl.resize && ev.pointerId === dl.resize.pointerId) moveDlResize(ev.clientX);
  });
  const end = (ev) => { if (dl.resize && ev.pointerId === dl.resize.pointerId) endDlResize(); };
  head.addEventListener('pointerup', end);
  head.addEventListener('pointercancel', end);
  head.addEventListener('lostpointercapture', end);
  head.addEventListener('dblclick', (ev) => {
    const h = ev.target.closest('.dl-resize');
    if (h) resetDlWidths(h.dataset.col);
  });
  // A right-click on the titles (or the menu key on one): back to the queue order, even when the
  // sorted column is hidden, and the default widths, even in compact columns.
  head.addEventListener('contextmenu', (ev) => {
    ev.preventDefault();
    let [x, y] = [ev.clientX, ev.clientY];
    const byKeys = !x && !y;
    if (byKeys) { const box = ev.target.getBoundingClientRect(); [x, y] = [box.left, box.bottom]; } // from the keys
    showMenu(x, y, [
      { label: 'Queue order', disabled: !dlSort.key, act: () => { dlSort.key = ''; applyDlSort(); } },
      { label: 'Reset column widths', disabled: !Object.keys(dl.widths).length, act: resetAllDlWidths },
    ], byKeys);
  });
}

// ---- selection, menus, moves and the confirmation ------------------------------------------

// A click selects a row, Ctrl+click toggles one, Shift+click extends from the last one clicked,
// within one scope: downloads, or the files of one folder. A message row stands for its download.
function selectDl(r, { toggle = false, extend = false } = {}) {
  const key = selKey(r);
  const scope = scopeOf(r);
  const at = dlIndexOf(dl.anchor);
  const anchor = at === undefined ? null : dlRow(at);
  const same = anchor && scopeOf(anchor) === scope;
  if (extend && same) {
    selectRows(at, dlIndexOf(key), scope);
  } else if (toggle && same) {
    markSel(r, !isSel(r));
    dl.anchor = key;
  } else {
    clearSel();
    markSel(r);
    dl.anchor = key;
  }
  renderDlBody();
}

// The keyboard's cursor to row `i` (5.8), kept in view: the row alone becomes the selection; with
// `extend` (Shift) the rows of the anchor's level from the anchor to it; with `keep` (Ctrl) the
// selection stays. The cursor goes through every row; a message row selects its download.
function moveDlCursor(i, { extend = false, keep = false } = {}) {
  const n = dl.n;
  if (!n) return;
  i = Math.max(0, Math.min(n - 1, i));
  const at = dlIndexOf(dl.cursor);
  const from = at === undefined ? null : dlRow(at);
  const r = dlRow(i);
  setDlCursor(r);
  if (extend) {
    if (dlIndexOf(dl.anchor) === undefined) dl.anchor = selKey(from || r);
    const a = dlIndexOf(dl.anchor);
    selectRows(a, i, scopeOf(dlRow(a)));
  } else if (!keep) {
    clearSel();
    markSel(r);
    dl.anchor = selKey(r);
  }
  dlKeepInView(i);
  renderDlBody();
}

// Ctrl+A: every row of the cursor row's level, all downloads or all files of its folder (those
// the filter shows).
function selectDlLevel() {
  const at = dlIndexOf(dl.cursor);
  const r = dlRow(at === undefined ? 0 : at);
  if (!r) return;
  const scope = scopeOf(r);
  if (scope === 'd') {
    selectRows(0, dl.n - 1, 'd');
  } else {
    const k = dl.blockAt.get(r.id);
    selectRows(dl.starts[k], dl.starts[k] + dl.top[k].n - 1, scope);
  }
  const anchor = dlIndexOf(dl.anchor);
  if (anchor === undefined || scopeOf(dlRow(anchor)) !== scope) dl.anchor = selKey(r);
  renderDlBody();
}

// → on a folded folder opens it, on an open one goes to its first file; ← folds an open folder,
// or from a row under a download goes up to it.
function dlRight(r) {
  const q = dl.byId.get(r.id);
  if (r.kind !== 'dl' || !q || !q.files) return;
  if (!folderIsOpen(q)) {
    setFolderOpen(q, true);
    return;
  }
  const k = dl.blockAt.get(q.id);
  if (k !== undefined) moveDlCursor(dl.starts[k]);
}

function dlLeft(r) {
  const q = dl.byId.get(r.id);
  if (r.kind !== 'dl') moveDlCursor(dlIndexOf(`d${r.id}`));
  else if (q && q.files && folderIsOpen(q)) setFolderOpen(q, false);
}

// A folder's ▸/▾ by the user: kept in queue.json, and its files asked for or let go.
function setFolderOpen(q, open) {
  folderOpen.set(q.id, open);
  send({ type: 'setExpanded', id: q.id, open });
  syncWatches();
  buildDlRows();
  renderDlBody();
}

function waiting(q) { return q.status === 'queued' || q.status === 'paused'; }

// Where a move puts a download: the `before` of moveItem (0 = the end), or undefined when it can't
// go that way.
function moveTarget(id, how) {
  const ids = state.queue.map((q) => q.id);
  const at = ids.indexOf(id);
  const last = ids.length - 1;
  if (at < 0) return undefined;
  if (how === 'up' || how === 'top') return at > 0 ? (how === 'top' ? ids[0] : ids[at - 1]) : undefined;
  if (at >= last) return undefined;
  return how === 'down' && at + 2 <= last ? ids[at + 2] : 0;
}

function moveDownload(id, how) {
  const before = moveTarget(id, how);
  if (before === undefined) return;
  dl.follow = { id, order: state.queue.map((q) => q.id).join() }; // kept in view once it moved
  dl.moving = { id, order: dl.follow.order };
  send({ type: 'moveItem', id, before });
}

// Cancel or Skip that deletes partly downloaded files asks first (5.8): Delete, and the menu on
// more than one row (`ask`). A row's own button acts at once.
// `gone`: what goes with them ("their partly downloaded files are deleted").
function confirmDelete(ask, started, title, what, ok, go, gone = 'their partly downloaded files are deleted',
  keep = 'Keep downloading') {
  if (!ask || !started) { go(); return; }
  showModal({
    title,
    html: `<p>${what}, and ${gone}.</p>`,
    ok,
    danger: true,
    cancel: keep,
    onOk: go,
  });
}

function cancelDownloads(qs, ask) {
  const list = qs.filter((q) => UNFINISHED.includes(q.status));
  if (!list.length) return;
  const one = list.length === 1;
  const ups = list.filter(isUp).length;
  const kind = ups === 0 ? 'downloads' : ups === list.length ? 'uploads' : 'transfers';
  const gone = ups === 0 ? 'their partly downloaded files are deleted'
    : ups === list.length ? 'what they sent but didn’t finish is removed from the server'
      : 'partly downloaded files are deleted, and what uploads sent but didn’t finish is removed from the server';
  confirmDelete(ask, list.some(startedBytes), one ? `Cancel “${list[0].name}”?` : `Cancel ${list.length} ${kind}?`,
    `${one ? 'It stops' : 'They stop'} (files a folder finished are kept)`,
    one ? (ups ? 'Cancel upload' : 'Cancel download') : `Cancel ${list.length}`,
    () => list.forEach((q) => send({ type: 'cancelItem', id: q.id })), gone,
    ups === 0 ? 'Keep downloading' : ups === list.length ? 'Keep uploading' : 'Keep going');
}

// What a file's slots offer (fileSlots), from its state alone.
function fileActs(q, st, i) {
  const c = { status: FILE_STATES[st.status[i]] || 'queued', error: '' };
  return fileSlots(c, fileStatus(q, c), isUp(q));
}

// Files of a folder, by index, in one "fileAction" (one save and one flush in the backend).
function fileAction(q, st, act, list) {
  send({ type: 'fileAction', id: q.id, gen: st.gen, act, ranges: rangesOf(list) });
}

// Their priority in the folder (DOWNLOADS-UI.md 7), those it changes, in one "setFilePriority".
function setFilePriority(q, st, list, priority) {
  const w = WEIGHT_OF_PRIORITY[priority];
  const changing = list.filter((i) => fileHasPrio(q, st, i) && st.prio[i] !== w);
  if (w && changing.length) send({ type: 'setFilePriority', id: q.id, gen: st.gen, ranges: rangesOf(changing), priority });
}

function skipFiles(q, st, list, ask) {
  const skipping = list.filter((i) => fileActs(q, st, i)[1]?.[0] === 'skip');
  if (!skipping.length) return;
  const one = skipping.length === 1;
  confirmDelete(ask, skipping.some((i) => st.bytes[i] > 0), one ? `Skip “${st.path[skipping[0]]}”?` : `Skip ${skipping.length} files?`,
    `${one ? 'It isn’t' : 'They aren’t'} downloaded`, one ? 'Skip file' : `Skip ${skipping.length}`,
    () => fileAction(q, st, 'skip', skipping));
}

// Delete: cancels the selected downloads or skips the selected files, asking first when that
// deletes partly downloaded files.
function deleteSelection() {
  const files = selectedFiles();
  if (files) {
    skipFiles(files.q, files.st, files.list, true);
    return;
  }
  const rows = selectedDl();
  if (rows.length) cancelDownloads(rows.map((r) => dl.byId.get(r.id)).filter(Boolean), true);
}

// Downloads: pause, resume or retry, cancel, priority, the moves (one waiting download), show it,
// copy its path or its error. With several selected, each entry acts on the ones it applies to.
function downloadsMenu(qs) {
  const n = qs.length;
  const one = n === 1 ? qs[0] : null;
  const counted = (label, list) => (n > 1 ? `${label} ${list.length}` : label);
  const items = [];
  const pausing = qs.filter((q) => q.status === 'queued' || q.status === 'running');
  // Several: those without the question about files already there (its own Resume asks it).
  const resuming = one ? qs.filter((q) => q.status === 'paused') : qs.filter((q) => q.status === 'paused' && !q.ask);
  const retrying = qs.filter((q) => q.status === 'failed' || q.status === 'cancelled');
  const cancelling = qs.filter((q) => UNFINISHED.includes(q.status));
  // One finished download opens (chunk H): a file in its app, a folder in Explorer.
  if (one && dlOpenKind({ kind: 'dl', id: one.id })) {
    items.push({ label: one.mode === 'folder' ? 'Open folder' : 'Open', key: 'Ctrl+Enter', act: () => dlOpen({ kind: 'dl', id: one.id }) }, '-');
  }
  if (pausing.length) items.push({ label: counted('Pause', pausing), act: () => pausing.forEach((q) => send({ type: 'pauseItem', id: q.id })) });
  if (resuming.length) items.push({ label: counted('Resume', resuming), act: () => resuming.forEach(resumeItem) });
  if (retrying.length) {
    items.push({ label: one ? (one.status === 'failed' && one.bytes > 0 ? 'Resume' : 'Retry') : `Retry ${retrying.length}`,
      act: () => retrying.forEach((q) => send({ type: 'retryItem', id: q.id })) });
  }
  if (cancelling.length) items.push({ label: counted('Cancel', cancelling), key: 'Delete', act: () => cancelDownloads(cancelling, n > 1) });
  if (cancelling.length) {
    items.push('-', { heading: 'Priority' });
    for (const [p, label] of PRIORITIES) {
      items.push({ label, checked: cancelling.every((q) => q.priority === p),
        act: () => cancelling.forEach((q) => { if (q.priority !== p) send({ type: 'setPriority', id: q.id, priority: p }); }) });
    }
  }
  // A listed folder's files, all at once (DOWNLOADS-UI.md 7): finished ones keep theirs.
  const folders = cancelling.filter((q) => q.mode === 'folder' && q.files && q.files.total > 0 && q.gen);
  if (folders.length) {
    items.push('-');
    for (const [p, label] of PRIORITIES) {
      items.push({ label: `Set all files to ${label}`,
        act: () => folders.forEach((q) => send({ type: 'setFilePriority', id: q.id, gen: q.gen, ranges: [[0, q.files.total - 1]], priority: p })) });
    }
  }
  // A waiting download moves in the queue (decision 4); Alt+↑/↓ does the first two. Only in the
  // queue order: sorted by a column, the rows don't show it (5.9).
  if (one && waiting(one)) {
    items.push('-');
    for (const [how, label, key] of [['up', 'Move up', 'Alt+↑'], ['down', 'Move down', 'Alt+↓'], ['top', 'Move to top'], ['bottom', 'Move to bottom']]) {
      items.push({ label, key, disabled: !!dlSort.key || moveTarget(one.id, how) === undefined, act: () => moveDownload(one.id, how) });
    }
  }
  items.push('-');
  // A download's local copy; an upload's server side (Browse there) and its source (UPLOADS.md 5.10).
  const downs = qs.filter((q) => !isUp(q));
  const ups = qs.filter(isUp);
  if (downs.length) {
    items.push({ label: 'Show in folder', act: () => send({ type: 'openFolder', id: downs[0].id }) });
    items.push({ label: downs.length > 1 ? `Copy ${downs.length} paths` : 'Copy path', act: () => copyText(downs.map((q) => localPath(q))) });
  }
  if (ups.length) {
    items.push({ label: 'Show on server', act: () => showOnServer(ups[0]) });
    items.push({ label: 'Show in Explorer', act: () => send({ type: 'openFolder', id: ups[0].id }) });
    items.push({ label: ups.length > 1 ? `Copy ${ups.length} server paths` : 'Copy server path', act: () => copyText(ups.map((q) => q.path)) });
  }
  const errors = qs.filter((q) => q.status === 'failed' && q.error);
  if (errors.length) items.push({ label: errors.length > 1 ? `Copy ${errors.length} errors` : 'Copy error', act: () => copyText(errors.map((q) => q.error)) });
  return items;
}

// Files of a folder download: their own pause, resume, retry and skip, Show in folder (the first
// of them, 5.10), their paths and errors. Each entry acts on the files it applies to, at once.
function filesMenu(q, st, list) {
  const n = list.length;
  const counted = (label, l) => (n > 1 ? `${label} ${l.length}` : label);
  const by = { pause: [], resume: [], retry: [], skip: [] };
  for (const i of list) {
    for (const slot of fileActs(q, st, i)) if (slot && by[slot[0]]) by[slot[0]].push(i);
  }
  const items = [];
  // One finished file opens in its app (chunk H); double-click and Ctrl+Enter do the same. Not an
  // upload's: it isn't grab's.
  if (n === 1 && st.status[list[0]] === FS.done && !isUp(q)) {
    items.push({ label: 'Open', key: 'Ctrl+Enter', act: () => dlOpen({ kind: 'file', id: q.id, index: list[0] }) }, '-');
  }
  if (by.pause.length) items.push({ label: counted('Pause', by.pause), act: () => fileAction(q, st, 'pause', by.pause) });
  if (by.resume.length) items.push({ label: counted('Resume', by.resume), act: () => fileAction(q, st, 'resume', by.resume) });
  if (by.retry.length) items.push({ label: counted('Retry', by.retry), act: () => fileAction(q, st, 'retry', by.retry) });
  if (by.skip.length) items.push({ label: counted('Skip', by.skip), key: 'Delete', act: () => skipFiles(q, st, by.skip, n > 1) });
  // Their priority in the folder: the unfinished ones (DOWNLOADS-UI.md 7).
  const prioritizing = list.filter((i) => fileHasPrio(q, st, i));
  if (prioritizing.length) {
    items.push('-', { heading: 'Priority' });
    for (const [p, label] of PRIORITIES) {
      items.push({ label, checked: prioritizing.every((i) => st.prio[i] === WEIGHT_OF_PRIORITY[p]),
        act: () => setFilePriority(q, st, prioritizing, p) });
    }
  }
  if (items.length) items.push('-');
  if (isUp(q)) {
    items.push({ label: 'Show on server', act: () => showOnServer(q, list[0]) });
    items.push({ label: 'Show in Explorer', act: () => send({ type: 'showFile', id: q.id, gen: st.gen, index: list[0] }) });
    items.push({ label: n > 1 ? `Copy ${n} server paths` : 'Copy server path', act: () => copyText(list.map((i) => serverChild(q.path, st.path[i]))) });
  } else {
    items.push({ label: 'Show in folder', act: () => send({ type: 'showFile', id: q.id, gen: st.gen, index: list[0] }) });
    items.push({ label: n > 1 ? `Copy ${n} paths` : 'Copy path', act: () => copyText(list.map((i) => localPath(q, st, i))) });
  }
  const errors = list.filter((i) => st.status[i] === FS.failed && st.errors.get(i));
  if (errors.length) {
    items.push({ label: errors.length > 1 ? `Copy ${errors.length} errors` : 'Copy error',
      act: () => copyText(errors.map((i) => (n > 1 ? `${st.path[i]}: ${st.errors.get(i)}` : st.errors.get(i)))) });
  }
  return items;
}

function dlMenu() {
  const files = selectedFiles();
  if (files) return filesMenu(files.q, files.st, files.list);
  const rows = selectedDl();
  return rows.length ? downloadsMenu(rows.map((r) => dl.byId.get(r.id)).filter(Boolean)) : [];
}

// What Open does on a row (DOWNLOADS-UI.md, chunk H), only once it is finished: 'file' opens a
// file download's file or a folder's file in its app, 'folder' a folder download's folder in
// Explorer; '' for anything else (unfinished, failed, cancelled, skipped, a message row).
function dlOpenKind(r) {
  const q = r && dl.byId.get(r.id);
  if (!q || isUp(q)) return ''; // an upload's file is the user's own, not one grab wrote
  if (r.kind === 'file') {
    const st = dl.stores.get(r.id);
    return st && st.status[r.index] === FS.done ? 'file' : '';
  }
  if (r.kind !== 'dl' || q.status !== 'done') return '';
  if (q.kept) return ''; // Skip kept the user's own file: grab downloaded nothing to open
  return q.mode === 'folder' ? 'folder' : 'file';
}

// Opens a finished row (dlOpenKind). The backend finds the file from its own state, by the row's
// id (and a folder's generation and index), asks first for a program (openAsk) and says in the
// head when the file is gone or changed (downloadsNote). Whether it did anything.
function dlOpen(r) {
  const kind = dlOpenKind(r);
  if (kind === 'folder') send({ type: 'openDir', id: r.id });
  else if (kind === 'file' && r.kind === 'file') send({ type: 'openFile', id: r.id, gen: dl.stores.get(r.id).gen, index: r.index });
  else if (kind === 'file') send({ type: 'openFile', id: r.id });
  return kind !== '';
}

// The backend's question before a downloaded program, script, installer or shortcut runs: Run or
// Cancel, the focus on Cancel, Esc cancels.
function onOpenAsk(msg) {
  if (modal) return; // another question is open: this one isn't asked over it
  showModal({
    title: `Run “${msg.name}”?`,
    html: `<p>It came from ${esc(msg.server)}.</p>`,
    ok: 'Run',
    danger: true,
    // The same file again, by the same row; the backend runs it only if it is still finished.
    onOk: () => send({ type: 'openFile', id: msg.id, ...(msg.index === undefined ? {} : { gen: msg.gen, index: msg.index }), run: true }),
  });
}

// A row's buttons: the folder's ▸/▾ and the two slots.
function dlAct(r, act) {
  const q = dl.byId.get(r.id);
  if (!q) return;
  if (act === 'toggle') {
    setFolderOpen(q, !folderIsOpen(q));
  } else if (r.kind === 'file') {
    const st = dl.stores.get(r.id);
    if (!st) return;
    if (act === 'show' && isUp(q)) showOnServer(q, r.index);
    else if (act === 'show') send({ type: 'showFile', id: q.id, gen: st.gen, index: r.index });
    else if (['pause', 'resume', 'retry', 'skip'].includes(act)) fileAction(q, st, act, [r.index]);
  } else if (act === 'show' && isUp(q)) {
    showOnServer(q);
  } else if (act === 'resume') {
    resumeItem(q);
  } else {
    const type = { pause: 'pauseItem', retry: 'retryItem', cancel: 'cancelItem', show: 'openFolder' }[act];
    if (type) send({ type, id: q.id });
  }
}

// ---- dragging a download by its ≡ handle (5.9) -----------------------------------------------
// With pointer capture: the rows under it are recycled as the body scrolls, which an HTML5 drag
// wouldn't survive. The drop goes before the next download row after the drop point, so a point
// among an open folder's files is after that folder.

function dragTarget() {
  const d = dl.drag;
  const body = $('#dl-body');
  const r = body.getBoundingClientRect();
  const y = Math.min(Math.max(d.y, r.top), r.bottom) - r.top + body.scrollTop;
  const n = dl.n;
  let k = Math.max(0, Math.min(n, Math.round(y / DL_ROW))); // the gap above row k
  if (k < n) { // on to the next download row: past message rows and an open folder's files
    let e = dlEntryAt(k);
    if (dl.starts[e] < k) ++e;
    while (e < dl.top.length && dl.top[e].kind !== 'dl') ++e;
    k = e < dl.top.length ? dl.starts[e] : n;
  }
  const before = k < n ? dlRow(k).id : 0;
  const ids = state.queue.map((q) => q.id);
  const at = ids.indexOf(d.id);
  const next = at >= 0 && at + 1 < ids.length ? ids[at + 1] : 0;
  d.before = at < 0 || before === d.id || before === next ? undefined : before; // undefined: it stays
  const mark = $('#dl-drop');
  mark.hidden = d.before === undefined;
  mark.style.transform = `translateY(${k * DL_ROW}px)`;
}

// Near the body's top or bottom edge the list scrolls, faster the closer the pointer.
function dragScroll() {
  const d = dl.drag;
  if (!d) return;
  const body = $('#dl-body');
  const r = body.getBoundingClientRect();
  const step = d.y < r.top + DL_ROW ? -Math.min(20, Math.ceil((r.top + DL_ROW - d.y) / 3))
    : d.y > r.bottom - DL_ROW ? Math.min(20, Math.ceil((d.y - r.bottom + DL_ROW) / 3)) : 0;
  const before = body.scrollTop;
  if (step) body.scrollTop += step;
  if (!step || body.scrollTop === before) { d.scroll = 0; return; }
  renderDlBody();
  dragTarget();
  d.scroll = requestAnimationFrame(dragScroll);
}

function endDrag(drop) {
  const d = dl.drag;
  if (!d) return;
  dl.drag = null;
  cancelAnimationFrame(d.scroll);
  d.el.pinned = false;
  document.documentElement.classList.remove('dl-dragging');
  $('#dl-drop').hidden = true;
  if (drop && d.moved && d.before !== undefined) {
    dl.moving = { id: d.id, order: state.queue.map((q) => q.id).join() };
    send({ type: 'moveItem', id: d.id, before: d.before });
  }
  renderDlBody();
}

{
  const body = $('#dl-body');
  body.addEventListener('scroll', scheduleDl, { passive: true });
  const observer = new ResizeObserver(() => { updateDlStep(); scheduleDl(); });
  observer.observe(body);
  observer.observe($('#downloads'));

  body.addEventListener('mousedown', (ev) => {
    const r = rowOfEl(ev.target.closest('.dl-row'));
    if (!r || (ev.button !== 0 && ev.button !== 2)) return;
    // A row's button acts on its click but never takes the focus: the table keeps it (one tab
    // stop, 5.8), so Delete and Alt+arrows go on working and no key lands on a recycled row.
    if (ev.target.closest('button')) { ev.preventDefault(); dlFocus(); return; }
    // The priority opens; the grip drags (in the queue order; sorted, it is part of the row).
    if (ev.target.closest('select') || (ev.target.closest('.dl-grip') && !dlSort.key)) return;
    ev.preventDefault();
    setDlCursor(r); // first: taking the focus would otherwise put the cursor on a row in view
    dlFocus();
    // Right-click: a row outside the selection becomes the selection; inside, it stays.
    if (ev.button === 2) { if (!isSel(r)) selectDl(r); else renderDlBody(); return; }
    selectDl(r, { toggle: ev.ctrlKey, extend: ev.shiftKey });
    // A double-click opens a finished file (chunk H), by the second press's click count, as in
    // Browse; a folder's row and an unfinished one do nothing more than a click. An upload's row
    // (or file) opens Browse where it goes.
    if (ev.button === 0 && ev.detail === 2 && !ev.ctrlKey && !ev.shiftKey) {
      const q = dl.byId.get(r.id);
      if (dlOpenKind(r) === 'file') dlOpen(r);
      else if (q && isUp(q) && r.kind !== 'msg') showOnServer(q, r.kind === 'file' ? r.index : null);
    }
  });
  body.addEventListener('contextmenu', (ev) => {
    const r = rowOfEl(ev.target.closest('.dl-row'));
    if (!r) return;
    ev.preventDefault();
    if (!isSel(r)) selectDl(r); // on a button or the priority
    const items = dlMenu();
    if (items.length) showMenu(ev.clientX, ev.clientY, items);
  });
  body.addEventListener('click', (ev) => {
    const btn = ev.target.closest('button[data-act]');
    const r = btn && rowOfEl(btn.closest('.dl-row'));
    if (r) dlAct(r, btn.dataset.act);
  });
  body.addEventListener('change', (ev) => {
    if (!ev.target.matches('.dl-prio')) return;
    const r = rowOfEl(ev.target.closest('.dl-row'));
    if (r && r.kind === 'dl') send({ type: 'setPriority', id: r.id, priority: ev.target.value });
    if (r && r.kind === 'file') {
      const q = dl.byId.get(r.id);
      const st = dl.stores.get(r.id);
      if (q && st) setFilePriority(q, st, [r.index], ev.target.value);
    }
    dlFocus(); // back to the table: its element may show another row later
  });

  // The body scrolls but is no Tab stop of its own (tabindex -1: a scroller without one would be
  // one); a click on its empty part or its scrollbar gives the focus to the table.
  body.addEventListener('focus', dlFocus);
  const grid = $('#queue');
  // Tabbed to without a cursor: the selection's first row, or the first row in view. Drawn in the
  // next frame: the focus can come from inside renderDlBody (a recycled row's button).
  grid.addEventListener('focus', () => {
    if (dlIndexOf(dl.cursor) !== undefined || !dl.n) return;
    const i = firstSelected() ?? Math.min(dl.n - 1, Math.ceil(body.scrollTop / DL_ROW));
    setDlCursor(dlRow(i));
    scheduleDl();
  });
  // The keys (5.8): the cursor goes through the rows, Shift extends the selection, Ctrl moves the
  // cursor alone, Ctrl+Space toggles its row; → and ← open and fold folders; Enter shows a
  // finished download, Ctrl+Enter opens it (chunk H); Delete cancels or skips; Alt+↑/↓ moves a
  // waiting download; Shift+F10 or the menu key opens the menu. Space is left free (Browse plays
  // with it).
  grid.addEventListener('keydown', (ev) => {
    if (ev.target !== grid || modal) return; // a title's button keeps its own keys
    if (!['Shift', 'Control', 'Alt', 'Meta'].includes(ev.key)) dl.keyAt = performance.now(); // holds a new order
    const n = dl.n;
    const at = dlIndexOf(dl.cursor);
    const r = at === undefined ? null : dlRow(at);
    const page = Math.max(1, Math.floor(body.clientHeight / DL_ROW) - 1);
    const move = { ArrowDown: 1, ArrowUp: -1, PageDown: page, PageUp: -page }[ev.key];
    if (ev.altKey && !ev.ctrlKey && !ev.shiftKey && (ev.key === 'ArrowUp' || ev.key === 'ArrowDown')) {
      ev.preventDefault();
      ev.stopPropagation();
      const rows = dl.fsel ? [] : selectedDl();
      const q = rows.length === 1 && rows[0].kind === 'dl' ? dl.byId.get(rows[0].id) : null;
      if (!q || !waiting(q)) return;
      if (dlSort.key) dlNote(`Moves follow the queue order: ${dlQueueOrderHint()}.`);
      else moveDownload(q.id, ev.key === 'ArrowUp' ? 'up' : 'down');
    } else if (ev.altKey || ev.metaKey) {
      // not the table's
    } else if (move !== undefined && n) {
      ev.preventDefault();
      moveDlCursor(at === undefined ? 0 : at + move, { extend: ev.shiftKey, keep: ev.ctrlKey && !ev.shiftKey });
    } else if ((ev.key === 'Home' || ev.key === 'End') && n) {
      ev.preventDefault();
      moveDlCursor(ev.key === 'Home' ? 0 : n - 1, { extend: ev.shiftKey, keep: ev.ctrlKey && !ev.shiftKey });
    } else if ((ev.key === 'ArrowRight' || ev.key === 'ArrowLeft') && !ev.ctrlKey && !ev.shiftKey) {
      ev.preventDefault();
      if (r) (ev.key === 'ArrowRight' ? dlRight : dlLeft)(r);
    } else if (ev.key === ' ') {
      ev.preventDefault(); // nothing scrolls
      if (ev.ctrlKey && r) selectDl(r, { toggle: true });
    } else if ((ev.key === 'a' || ev.key === 'A') && ev.ctrlKey && !ev.shiftKey) {
      ev.preventDefault();
      selectDlLevel();
    } else if (ev.key === 'Enter' && !ev.ctrlKey && !ev.shiftKey) {
      ev.preventDefault();
      const q = r && r.kind === 'dl' ? dl.byId.get(r.id) : null;
      const st = r && r.kind === 'file' ? dl.stores.get(r.id) : null;
      const fq = st ? dl.byId.get(r.id) : null;
      if (q && (q.status === 'done' || q.status === 'skipped')) {
        if (isUp(q)) showOnServer(q); else send({ type: 'openFolder', id: q.id });
      } else if (st && st.status[r.index] === FS.done) {
        if (fq && isUp(fq)) showOnServer(fq, r.index); else send({ type: 'showFile', id: r.id, gen: st.gen, index: r.index });
      }
    } else if (ev.key === 'Enter' && ev.ctrlKey && !ev.shiftKey) {
      ev.preventDefault();
      if (r) dlOpen(r); // the cursor's row, once finished
    } else if (ev.key === 'Delete' && !ev.ctrlKey) {
      ev.preventDefault();
      deleteSelection();
    } else if ((ev.key === 'F10' && ev.shiftKey) || ev.key === 'ContextMenu') {
      ev.preventDefault();
      if (!hasSel() && r) selectDl(r); // nothing selected: the cursor row
      const first = firstSelected();
      if (first === undefined) return;
      // At the cursor row when it is selected, else at the first selected one.
      const key = r && isSel(r) ? r.key : dlRow(first).key;
      dlKeepInView(dlIndexOf(key));
      renderDlBody();
      const box = dl.bound.get(key).getBoundingClientRect();
      showMenu(box.left + 40, box.bottom, dlMenu(), true);
    }
  });
  // A new order waiting for the pointer to leave the table comes once it has (5.6).
  grid.addEventListener('pointerenter', () => { dlSort.over = true; });
  grid.addEventListener('pointerleave', () => { dlSort.over = false; dlRecheck(); });

  body.addEventListener('pointerdown', (ev) => {
    const grip = ev.target.closest('.dl-grip');
    const el = grip && grip.closest('.dl-row');
    const r = rowOfEl(el);
    if (!r || r.kind !== 'dl' || ev.button !== 0 || dl.drag || dlSort.key) return;
    ev.preventDefault();
    closeMenu();
    grip.setPointerCapture(ev.pointerId);
    el.pinned = true;
    dl.drag = { id: r.id, key: r.key, pointerId: ev.pointerId, el, y0: ev.clientY, y: ev.clientY, moved: false, before: undefined, scroll: 0 };
  });
  body.addEventListener('pointermove', (ev) => {
    const d = dl.drag;
    if (!d || ev.pointerId !== d.pointerId) return;
    d.y = ev.clientY;
    if (!d.moved) {
      if (Math.abs(d.y - d.y0) < 4) return;
      d.moved = true;
      document.documentElement.classList.add('dl-dragging');
      renderDlBody();
    }
    dragTarget();
    if (!d.scroll) dragScroll();
  });
  body.addEventListener('pointerup', (ev) => { if (dl.drag && ev.pointerId === dl.drag.pointerId) endDrag(true); });
  body.addEventListener('pointercancel', (ev) => { if (dl.drag && ev.pointerId === dl.drag.pointerId) endDrag(false); });
  body.addEventListener('lostpointercapture', (ev) => { if (dl.drag && ev.pointerId === dl.drag.pointerId) endDrag(false); });
}

// A short note in the Downloads head, in place of the summary for `ms` (also in a narrow head).
// Not the footer's flash: Settings or a running activity can hide that while the table has the keys.
let dlNoteTimer = 0;
function dlNote(text, ms = 6000) {
  const note = $('#dl-note');
  clearTimeout(dlNoteTimer);
  note.hidden = false;
  note.textContent = text;
  note.title = text;
  $('#downloads').classList.add('noting');
  dlNoteTimer = setTimeout(() => {
    note.hidden = true;
    note.textContent = '';
    note.title = '';
    $('#downloads').classList.remove('noting');
  }, ms);
}

// 'Go to running' (decision 2): the next row downloading now below the one it went to last (or
// below the top of the view, once scrolled since), around to the first: a running file of an open
// folder, or a running download that isn't one. A folded running folder opens, and the jump comes
// once its files are in.
function goToRunning() {
  const body = $('#dl-body');
  const from = body.scrollTop === dl.lastRunTop ? dl.lastRun : Math.floor(body.scrollTop / DL_ROW) - 1;
  let first = -1;
  let next = -1;
  const consider = (i) => {
    if (first < 0) first = i;
    if (next < 0 && i > from) next = i;
  };
  let folded = null;
  for (let k = 0; k < dl.top.length && next < 0; ++k) {
    const e = dl.top[k];
    if (e.kind === 'dl') {
      const q = dl.byId.get(e.id);
      if (!q || q.status !== 'running') continue;
      if (!q.files) consider(dl.starts[k]);
      else if (!folderIsOpen(q) && !folded) folded = q;
    } else if (e.kind === 'block') {
      const st = dl.stores.get(e.id);
      for (let p = 0; p < st.view.length; ++p) {
        if (st.status[st.view[p]] !== FS.running) continue;
        consider(dl.starts[k] + p);
        if (next >= 0) break;
      }
    }
  }
  const target = next >= 0 ? next : first;
  if (target < 0) {
    if (folded) {
      dl.goPending = folded.id;
      setFolderOpen(folded, true);
    } else {
      dlNote('Nothing is downloading now.', 3000);
    }
    return;
  }
  body.scrollTop = Math.max(0, (target - 1) * DL_ROW); // the second row in view
  dl.lastRun = target;
  dl.lastRunTop = body.scrollTop;
  renderDlBody();
}

// Show: All / Unfinished / Failed (decision 2) / Downloads / Uploads, for this session: rows that no
// longer pass dim and leave at most once a second, never while the pointer is over the table.
function setDlShow(show) {
  dl.show = ['unfinished', 'failed', 'downloads', 'uploads'].includes(show) ? show : 'all';
  $('#dl-show').value = dl.show;
  showDlOrder();
}

function renderQueue() {
  const panel = $('#downloads');
  // Hidden with an empty queue, its grip with it; back at the saved split.
  const shown = !panel.hidden;
  panel.hidden = state.queue.length === 0;
  if (!shown && !panel.hidden) applySplit();
  dl.byId = new Map(state.queue.map((q) => [q.id, q]));
  for (const id of folderOpen.keys()) if (!dl.byId.has(id)) folderOpen.delete(id);
  syncWatches();
  updateDlOrder();
  // A download the user moved that was the first in view: the view stays, the rows move.
  const m = dl.moving;
  if (m && state.queue.map((q) => q.id).join() !== m.order) {
    dl.moving = null;
    if (dl.viewDl === `d${m.id}`) dl.viewKey = '';
  }
  buildDlRows();
  if (!panel.hidden) {
    updateDlStep();
    renderDlBody();
    if (dl.drag && dl.drag.moved) dragTarget(); // rows may have come or gone under the pointer
    const f = dl.follow;
    if (f && state.queue.map((q) => q.id).join() !== f.order) {
      dl.follow = null;
      const i = dlIndexOf(`d${f.id}`);
      if (i !== undefined) { dlKeepInView(i); renderDlBody(); }
    }
  }
  const count = (st) => state.queue.filter((q) => q.status === st).length;
  const uploading = state.queue.filter((q) => q.status === 'running' && isUp(q)).length;
  const running = count('running');
  const queued = count('queued');
  const paused = count('paused');
  // The title stays "Downloads"; running uploads have their own count (UPLOADS.md decision 2).
  const parts = [];
  if (running - uploading) parts.push(`${running - uploading} running`);
  if (queued) parts.push(`${queued} queued`);
  if (paused) parts.push(`${paused} paused`);
  if (uploading) parts.push(`${uploading} uploading`);
  const summary = parts.join(' · ');
  if ($('#dl-summary').textContent !== summary) $('#dl-summary').textContent = summary; // announced when it changes
  $('#downloads .title').title = summary; // a narrow panel shows it only here
  // Pause all while anything runs or waits; otherwise Resume all for paused ones.
  const pauseAll = $('#pause-all');
  const resuming = running + queued === 0 && paused > 0;
  pauseAll.textContent = resuming ? 'Resume all' : 'Pause all';
  pauseAll.dataset.act = resuming ? 'resumeAll' : 'pauseAll';
  pauseAll.disabled = running + queued + paused === 0;
  $('#cancel-all').disabled = running + queued + paused + count('failed') === 0;
  $('#clear-finished').disabled = !state.queue.some((q) => ['done', 'failed', 'cancelled'].includes(q.status));
  $('#dl-go').disabled = running === 0;
  renderParallel();
  renderTotalSpeed();
}

// The backend's 'queue': every download (they came, went or moved, or the page loaded).
function onQueue(msg) {
  state.queue = msg.items;
  if (msg.parallel) state.parallel = msg.parallel;
  renderQueue();
}

// The backend's 'queueDelta': only the downloads whose row changed, in their places.
function onQueueDelta(msg) {
  const at = new Map(state.queue.map((q, k) => [q.id, k]));
  for (const item of msg.items) {
    const k = at.get(item.id);
    if (k !== undefined) state.queue[k] = item;
  }
  if (msg.parallel) state.parallel = msg.parallel;
  renderQueue();
}

// ---- the split between the view above (Browse or Settings) and Downloads -----------------------

// The Downloads panel's share of H, the height the view above and the panel take together: what
// gui.json keeps (0.15–0.85, 0.45 by default), the same in Browse and Settings, so it survives
// resizing, maximizing and DPI changes. The panel's height follows from it, within limits applied
// again on every resize: Downloads keeps its head, the table's header row and 3 rows; the view
// above its fixed parts and 4 rows of its list. When both don't fit (a small window with a
// banner), Downloads gives way first, down to 1 row, then the list above shrinks.
// It moves by the grip on the panel's top edge (the separator, with the keys) and by the head's
// free space (splitZone).
const SPLIT = { def: 0.45, min: 0.15, max: 0.85 };
const SPLIT_SLOP = 3; // px a press moves before it drags: a click stays a click
const DL_HEAD_ROW = 26; // the Downloads table's header row and rows (DOWNLOADS-UI.md 5.1)
const DL_ROW = 28;
const LIST_MIN = 4 * ROW;
// `share` as last set (by gui.json, a drag, a key or a reset) and `saved` as last sent; `px` the
// panel's height now; `drag` while the mouse holds the grip or the head (`moved` once past the
// slop); `movedAt` when a drag last ended; `zonePresses` how many presses in a row landed on a zone
// (a double-click resets only when both did); `loaded` once the first init set it.
const split = { share: SPLIT.def, saved: SPLIT.def, px: 0, drag: null, movedAt: -1e9, zonePresses: 0, loaded: false,
  keyTimer: 0 };

// The height of `box`'s parts that don't stretch: its shown, in-flow children but `list`, and of
// the one holding `list` only its own borders, padding and other children.
function fixedHeight(box, list) {
  let sum = 0;
  for (const el of box.children) {
    if (el === list) continue;
    const cs = getComputedStyle(el);
    if (cs.display === 'none' || cs.position === 'absolute' || cs.position === 'fixed') continue;
    sum += parseFloat(cs.marginTop) + parseFloat(cs.marginBottom);
    if (el.contains(list)) {
      sum += parseFloat(cs.borderTopWidth) + parseFloat(cs.borderBottomWidth) + parseFloat(cs.paddingTop) +
        parseFloat(cs.paddingBottom) + fixedHeight(el, list);
    } else {
      sum += el.getBoundingClientRect().height;
    }
  }
  return sum;
}

// H and the panel's least and greatest height now, in px.
function splitLimits() {
  const view = settings.open ? $('#settings-view') : $('#search-view');
  const list = settings.open ? $('#settings-view .settings-body') : $('#results');
  const panel = $('#downloads');
  const H = document.body.clientHeight - view.getBoundingClientRect().top - $('.bar.status').getBoundingClientRect().height;
  const ps = getComputedStyle(panel);
  const around = parseFloat(ps.marginTop) + parseFloat(ps.marginBottom);
  const room = H - around - fixedHeight(view, list) - LIST_MIN; // the most Downloads may take
  const head = $('.downloads-head').getBoundingClientRect().height + parseFloat(ps.borderTopWidth) +
    parseFloat(ps.borderBottomWidth) + DL_HEAD_ROW;
  let lo = room >= head + 3 * DL_ROW ? head + 3 * DL_ROW : Math.max(head + DL_ROW, room);
  let hi = Math.max(lo, room);
  lo = Math.ceil(Math.max(lo, Math.min(SPLIT.min * H, hi)));
  hi = Math.max(lo, Math.floor(Math.min(hi, Math.max(SPLIT.max * H, lo))));
  return { H, lo, hi };
}

// Sets the panel's height to `px` within `lim`; returns the height set.
function setSplitPx(px, lim) {
  px = Math.min(lim.hi, Math.max(lim.lo, Math.round(px)));
  if (px !== split.px) {
    split.px = px;
    $('#downloads').style.setProperty('--dl-h', `${px}px`);
  }
  // Its value is the view above's share, in %, as the separator's own pane.
  const grip = $('#split-grip');
  const pct = (h) => String(Math.round(100 * (1 - h / lim.H)));
  grip.setAttribute('aria-valuenow', pct(px));
  grip.setAttribute('aria-valuemin', pct(lim.hi));
  grip.setAttribute('aria-valuemax', pct(lim.lo));
  return px;
}

function applySplit() {
  if ($('#downloads').hidden) return;
  const lim = splitLimits();
  setSplitPx(split.share * lim.H, lim);
}

// A share from the panel's height now, kept for gui.json once the mouse or the keys let go.
function takeSplit(px, lim) {
  split.share = Math.round((10000 * px) / lim.H) / 10000;
}

function saveSplit() {
  if (split.share === split.saved) return;
  split.saved = split.share;
  send({ type: 'prefs', split: split.share });
}

function resetSplit() {
  clearTimeout(split.keyTimer);
  split.share = SPLIT.def;
  applySplit();
  saveSplit();
}

// Where a press moves the split: the grip, or the Downloads head's free space (the head itself, its
// title and its spacer), never one of its controls or texts (Show, Go to running, At once, Pause
// all, Cancel all, Clear finished, the summary, the note).
function splitZone(target) {
  const head = $('.downloads-head');
  return target === $('#split-grip') || target === head ||
    (target.parentElement === head && target.matches('.title, .spacer'));
}

{
  const grip = $('#split-grip');
  const head = $('.downloads-head');
  const endDrag = (ev) => {
    const d = split.drag;
    if (!d || ev.pointerId !== d.id) return;
    split.drag = null;
    if (d.moved) { // a click changes nothing
      split.movedAt = performance.now();
      grip.classList.remove('dragging');
      document.documentElement.classList.remove('split-drag');
      saveSplit();
    }
    applySplit(); // limits that changed during the press take effect now
  };
  // Every press, wherever it lands: a double-click whose first press was on a control (the second
  // drifting a pixel or two off it) is no reset.
  document.addEventListener('pointerdown', (ev) => {
    split.zonePresses = ev.button === 0 && splitZone(ev.target) ? split.zonePresses + 1 : 0;
  }, true);
  for (const el of [grip, head]) {
    // The mouse leaves the focus where it was (the arrow keys go on moving in Browse); Tab reaches
    // the grip.
    el.addEventListener('mousedown', (ev) => { if (splitZone(ev.target)) ev.preventDefault(); });
    el.addEventListener('pointerdown', (ev) => {
      if (ev.button !== 0 || split.drag || !splitZone(ev.target)) return;
      split.drag = { id: ev.pointerId, y: ev.clientY, px: split.px, lim: splitLimits(), moved: false };
      el.setPointerCapture(ev.pointerId);
    });
    // The panel's edge follows the pointer from the press on, once it moved past the slop.
    el.addEventListener('pointermove', (ev) => {
      const d = split.drag;
      if (!d || ev.pointerId !== d.id) return;
      const dy = ev.clientY - d.y;
      if (!d.moved) {
        if (Math.abs(dy) < SPLIT_SLOP) return;
        d.moved = true;
        grip.classList.add('dragging');
        document.documentElement.classList.add('split-drag');
      }
      takeSplit(setSplitPx(d.px - dy, d.lim), d.lim);
    });
    el.addEventListener('pointerup', endDrag);
    el.addEventListener('pointercancel', endDrag);
    el.addEventListener('lostpointercapture', endDrag);
    // Back to the default when both presses were on a zone; not for the second click of a drag that
    // just ended.
    el.addEventListener('dblclick', (ev) => {
      if (splitZone(ev.target) && split.zonePresses >= 2 && !split.drag && performance.now() - split.movedAt > 500) {
        resetSplit();
      }
    });
  }
  // ↑/↓ move it 16 px, PgUp/PgDn a tenth of H, Home/End to the ends (Home: the list above at its
  // smallest); Enter goes back to the default.
  grip.addEventListener('keydown', (ev) => {
    if (ev.altKey || ev.ctrlKey || ev.metaKey || split.drag) return;
    if (ev.key === 'Enter') {
      ev.preventDefault();
      resetSplit();
      return;
    }
    const lim = splitLimits();
    const page = Math.round(lim.H / 10);
    const to = { ArrowUp: split.px + 16, ArrowDown: split.px - 16, PageUp: split.px + page, PageDown: split.px - page,
      Home: lim.hi, End: lim.lo }[ev.key];
    if (to === undefined) return;
    ev.preventDefault();
    takeSplit(setSplitPx(to, lim), lim);
    clearTimeout(split.keyTimer);
    split.keyTimer = setTimeout(saveSplit, 500); // once the keys stop
  });
  // Again whenever the window, a banner or the list above changes size (a banner inside a view
  // shrinks only the list): in the next frame, so the change it makes isn't reported back within
  // this one. The limits don't depend on the list's own height, so this settles in one round.
  let frame = 0;
  const observer = new ResizeObserver(() => {
    if (!frame) frame = requestAnimationFrame(() => { frame = 0; if (!split.drag) applySplit(); });
  });
  for (const el of [document.body, $('#search-view'), $('#settings-view'), $('#results'),
                    $('#settings-view .settings-body')]) observer.observe(el);
  // Browse draws only the rows in view: a taller list needs more of them.
  new ResizeObserver(() => scheduleRows()).observe($('#results'));
}

// ---- status bar: total speeds, their last minute, and the two speed limits -------------------

// The totals are what all downloads receive now and what the servers acknowledged of all uploads,
// measured where the bytes arrive (the backend's 'speed': recent averages over about 5 s), not the
// rows' speeds added up. While nothing uploads and the upload limit is off, the status bar reads as
// before uploads came: "↓ 12.3 MiB/s total · 2 running", the download limit after it ("· 12 MiB/s
// limit"), running or not. Otherwise both directions, in one unit, each with its limit beside it
// while that is on: "↓ 11.9 of 12 · ↑ 3.9 of 4 MiB/s · 2 running · 1 uploading"; the tooltip says
// it in words. The text gives way (…) before the limits' controls on its right do.
function renderTotalSpeed() {
  const running = state.queue.filter((q) => q.status === 'running' && !isUp(q)).length;
  const uploading = state.queue.filter((q) => q.status === 'running' && isUp(q)).length;
  const { down, up } = state.limits;
  const el = $('#total-speed');
  const limit = $('#total-limit');
  const words = (what, speed, l) => `${what}: ${fmtSpeed(speed)}${l.on ? ` (limit ${fmtMiBps(l.mibps)})` : ''}`;
  if (!uploading && !up.on) {
    limit.hidden = !down.on;
    limit.textContent = down.on ? `· ${fmtMiBps(down.mibps)} limit` : '';
    if (!running) {
      el.removeAttribute('title');
      el.textContent = 'No downloads running';
      el.classList.remove('limited');
      return;
    }
    el.title = `${words('Downloads', state.totalSpeed, down)} · ${running} running`; // when the text gives way
    el.textContent = `↓ ${fmtSpeed(state.totalSpeed)} total · ${running} running`;
    el.classList.toggle('limited', down.on);
    return;
  }
  el.classList.remove('limited'); // each direction's part says it
  el.title = `${words('Downloads', state.totalSpeed, down)} · ${words('Uploads', state.upSpeed, up)}`;
  if (!running && !uploading) {
    // Nothing moves: the limits that are on, after it.
    el.textContent = 'No downloads or uploads running';
    limit.hidden = false; // the upload limit is on here
    limit.textContent = down.on ? `· ↓ ${fmtLimit(down.mibps)} · ↑ ${fmtMiBps(up.mibps)} limits`
      : `· ↑ ${fmtMiBps(up.mibps)} upload limit`;
    return;
  }
  limit.hidden = true;
  limit.textContent = '';
  const speeds = sharedSpeeds([state.totalSpeed, state.upSpeed], down.on || up.on);
  const part = (arrow, text, l) => {
    const s = document.createElement('span');
    s.textContent = `${arrow} ${text}${l.on ? ` of ${fmtLimit(l.mibps)}` : ''}`;
    s.classList.toggle('limited', l.on);
    return s;
  };
  const counts = `${running ? ` · ${running} running` : ''}${uploading ? ` · ${uploading} uploading` : ''}`;
  el.replaceChildren(part('↓', speeds.texts[0], down), ' · ', part('↑', speeds.texts[1], up), ` ${speeds.unit}${counts}`);
}

// Speeds side by side in one unit, the larger one's (as fmtSpeed would show it), or MiB/s (`mib`:
// the limits' unit, while one shows beside them): { texts, unit }.
function sharedSpeeds(values, mib) {
  const units = ['KiB/s', 'MiB/s', 'GiB/s', 'TiB/s', 'PiB/s'];
  let div = 1;
  let unit = 'B/s';
  if (mib) {
    div = 1048576;
    unit = 'MiB/s';
  } else {
    let v = Math.max(...values.map((x) => Math.round(x)));
    if (v >= 1024) {
      let u = 0;
      v /= 1024;
      div = 1024;
      while (v >= 1024 && u < units.length - 1) { v /= 1024; div *= 1024; ++u; }
      unit = units[u];
    }
  }
  return { texts: values.map((x) => (div === 1 ? String(Math.round(x)) : (x / div).toFixed(1))), unit };
}

function onSpeed(msg) {
  state.totalSpeed = msg.total > 0 ? msg.total : 0;
  state.upSpeed = msg.up > 0 ? msg.up : 0;
  renderTotalSpeed();
}

// The graph left of the totals: each once a second over the last minute, the newest on the right,
// scaled to the minute's peak of both: downloads in the accent colour over a shaded area, uploads
// as a line of their own colour (only while they moved). Its size is fixed and it sits before the
// text, so nothing in the status bar moves as the numbers change; with nothing moved in the last
// minute it is blank (its place kept).
const SPEED_SAMPLES = 60;
const speedHist = [];
const upHist = [];
function sampleSpeed() {
  if (speedHist.length >= SPEED_SAMPLES) speedHist.shift();
  if (upHist.length >= SPEED_SAMPLES) upHist.shift();
  speedHist.push(state.totalSpeed);
  upHist.push(state.upSpeed);
  drawSpeedGraph();
}

function drawSpeedGraph() {
  const box = $('#speed-graph');
  const downPeak = speedHist.reduce((m, v) => Math.max(m, v), 0);
  const upPeak = upHist.reduce((m, v) => Math.max(m, v), 0);
  const peak = Math.max(downPeak, upPeak);
  box.classList.toggle('idle', !(peak > 0));
  if (!(peak > 0)) return;
  const y = (v) => (15 - (14 * v) / peak).toFixed(1); // 1 px of room at the top
  const path = (hist) => {
    const from = SPEED_SAMPLES - hist.length;
    return hist.map((v, k) => `${k ? 'L' : 'M'}${from + k} ${y(v)}`).join('');
  };
  const line = path(speedHist);
  const from = SPEED_SAMPLES - speedHist.length;
  box.querySelector('.line').setAttribute('d', line);
  box.querySelector('.area').setAttribute('d', `${line}L${SPEED_SAMPLES - 1} 16L${from} 16Z`);
  box.querySelector('.up-line').setAttribute('d', upPeak > 0 ? path(upHist) : '');
  box.title = upPeak > 0
    ? `Total speeds over the last minute: ↓ downloads (peak ${fmtSpeed(downPeak)}), ↑ uploads (peak ${fmtSpeed(upPeak)})`
    : `Total speed over the last minute (peak ${fmtSpeed(peak)})`;
}
setInterval(sampleSpeed, 1000);

// The speed schedule's part in the status bar: why a limit is what it is, and until when. The
// download limit's alone as before; the upload limit's marked ↑, both apart when they differ.
function limitHintText(l) {
  const until = l.until ? ` until ${l.until}` : '';
  if (l.source === 'schedule') return `Scheduled${until}`;
  if (l.source === 'change') return l.until ? `Schedule resumes at ${l.until}` : 'Schedule paused';
  return '';
}

function renderLimitHint() {
  const d = limitHintText(state.limits.down);
  const u = limitHintText(state.limits.up);
  const hint = $('#limit-hint');
  hint.textContent = !u ? d : !d ? `↑ ${u}` : d === u ? d : `↓ ${d} · ↑ ${u}`;
  hint.hidden = !d && !u;
  hint.title = `Speed schedule (Settings)${d ? `\nDownloads: ${d}` : ''}${u ? `\nUploads: ${u}` : ''}`;
}

function fmtLimit(v) {
  return String(Math.round(v * 10) / 10);
}

function renderLimit() {
  for (const dir of LIMIT_DIRS) {
    const c = limitCtl[dir];
    const l = state.limits[dir];
    c.sw.checked = l.on;
    c.box.disabled = !l.on;
    // Not over what is being typed; but a box only focused shows the speed that applies, so that
    // leaving it doesn't set the one it showed before (a change by the schedule meanwhile).
    if (document.activeElement !== c.box || !c.edited) c.box.value = fmtLimit(l.mibps);
    showLimitPending(c); // the speed that applies may now be the one typed
  }
  renderLimitHint();
  renderTotalSpeed();
}

// The backend's "limit": both directions' limits that apply, the rules, and with the answer to a
// set its token and direction.
function onLimit(msg) {
  for (const dir of LIMIT_DIRS) if (msg[dir]) state.limits[dir] = msg[dir];
  // Re-rendered only when they differ (by field: WebView2 hands over objects with sorted keys),
  // so the echo of an edit doesn't take the focus from the next field.
  const rules = msg.rules || [];
  const key = (list) => JSON.stringify(list.map((r) => [r.days, r.from, r.to, r.mibps, r.upMiBps]));
  if (key(rules) !== key(state.schedule)) {
    state.schedule = rules;
    renderSchedule();
  }
  renderLimit();
  renderScheduleNow();
  for (const dir of LIMIT_DIRS) {
    const c = limitCtl[dir];
    if (msg.token !== undefined && (msg.dir || 'down') === dir) {
      confirmLimit(c, msg);
      continue;
    }
    // Another change meanwhile (the schedule's, or an override ending): the badge would say a
    // limit that no longer applies.
    const s = c.shown;
    const l = state.limits[dir];
    if (s && (l.on !== s.on || (s.on && Math.abs(l.mibps - s.mibps) > 1e-6))) endLimitBadge(c);
  }
}

// Sets a direction's limit to what state.limits has for it. The backend answers with a 'limit'
// message that carries the set's token and direction (sent even when nothing changed), and
// confirmLimit shows it.
function sendLimit(c) {
  const l = state.limits[c.dir];
  const token = ++c.token;
  c.asked = { token, on: l.on, mibps: l.mibps };
  send({ type: 'setLimit', dir: c.dir, on: l.on, mibps: l.mibps, token });
}

// The MiB/s boxes, ↓ downloads' (#limit-*) and ↑ uploads' (#ulimit-*), each the same control
// (limitControl) with its own switch, state and badge. Typing only edits: Enter or leaving the box
// sets the value, and Escape puts the speed that applies back and sets nothing. While the box
// holds a speed that isn't the one that applies, it looks pending: a dashed amber border and
// "Enter to set" above it. One that isn't a speed from 0.1 to 10000 MiB/s sets nothing, and the
// box says why (red, a note above it, from LIMIT_NOTE_MS after typing stops: '0' on the way to
// '0.5' is no mistake) rather than snapping back silently; leaving the box then puts the speed
// that applies back in it.
// A set is confirmed only by the backend's answer to it, with the limit asked for: the box
// flashes green with a check mark and a badge above it says "✓ Download limit · 12 MiB/s" (or
// "✓ Upload limit · 4 MiB/s"), fading after LIMIT_BADGE_MS; the status bar then shows the limit
// for as long as it applies (renderTotalSpeed). Enter on the speed that applies sets it again, for
// the reassurance; leaving the box with it sets nothing. The switch confirms the same way
// ("Download limit off", neutral). Changes made by the speed schedule keep their own hint
// (renderLimitHint), no badge.
const LIMIT_WHY_MS = 4000;   // a refusal's note, once the box was left
const LIMIT_NOTE_MS = 400;   // a refusal's note while typing, once typing stops
const LIMIT_BADGE_MS = 1800; // the confirmation, before it fades
const LIMIT_FADE_MS = 250;   // its fade (app.css .limit-badge.out)
const LIMIT_DIRS = ['down', 'up'];
const LIMIT_WORDS = { down: 'Download', up: 'Upload' };

// A direction's control: its elements, and `edited` (typed in since the box was focused, or since
// Enter or Escape), the timers, the last set asked for and `shown` (what the badge confirms,
// { on, mibps }, while it shows).
function limitControl(dir) {
  const id = (part) => $(`#${dir === 'up' ? 'ulimit' : 'limit'}-${part}`);
  return {
    dir,
    word: LIMIT_WORDS[dir],
    sw: id('on'),
    box: id('value'),
    wrap: id('box'),
    why: id('why'),
    tip: id('tip'),
    badge: id('badge'),
    edited: false,
    typed: 0,
    whyTimer: 0,
    badgeTimer: 0,
    fade: 0,
    token: 0,
    asked: null,
    shown: null,
  };
}
const limitCtl = { down: limitControl('down'), up: limitControl('up') };

// The box's text as MiB/s: { mibps } or { why }.
function limitValue(text) {
  const t = text.trim();
  const v = Number(t.replace(',', '.'));
  if (!t) return { why: 'Type a speed in MiB/s, from 0.1 to 10000' };
  if (!Number.isFinite(v)) return { why: `“${t.length > 12 ? `${t.slice(0, 12)}…` : t}” isn’t a number: type MiB/s, from 0.1 to 10000` };
  if (v < 0.1) return { why: 'At least 0.1 MiB/s' };
  if (v > 10000) return { why: 'At most 10000 MiB/s' };
  return { mibps: v };
}

// Shows why control `c`'s box value can't be set (`why`), or nothing; `ms`: hidden again after that.
function showLimitWhy(c, why, ms = 0) {
  clearTimeout(c.whyTimer);
  c.whyTimer = 0;
  c.why.textContent = why || '';
  c.why.hidden = !why;
  c.box.classList.toggle('bad', !!why && document.activeElement === c.box);
  c.box.setAttribute('aria-invalid', String(!!why && document.activeElement === c.box));
  if (why && ms) c.whyTimer = setTimeout(() => showLimitWhy(c, ''), ms);
  showLimitPending(c);
}

// The box's text is the speed that applies: not typed in (it shows that speed, rounded: '2.6' for
// 2.55), or typed as that number ('12.0' for 12; '2.6' isn't 2.55).
function isAppliedLimit(c, text) {
  const v = limitValue(text);
  return !c.edited || (v.mibps !== undefined && Math.abs(v.mibps - state.limits[c.dir].mibps) < 1e-9);
}

// The pending look, while the box (being edited) holds a speed other than the one that applies;
// a note saying why a value doesn't set takes its place above the box.
function showLimitPending(c) {
  const pending = state.limits[c.dir].on && document.activeElement === c.box &&
    limitValue(c.box.value).mibps !== undefined && !isAppliedLimit(c, c.box.value);
  c.wrap.classList.toggle('pending', pending);
  c.tip.hidden = !pending || !c.why.hidden;
  if (pending || !c.why.hidden) endLimitBadge(c);
}

function onLimitTyped(c) {
  c.edited = true;
  clearTimeout(c.typed);
  c.typed = 0;
  const v = limitValue(c.box.value);
  if (!v.why) showLimitWhy(c, '');
  else if (!c.why.hidden) showLimitWhy(c, v.why); // saying why already: the new reason at once
  else {
    c.typed = setTimeout(() => {
      c.typed = 0;
      showLimitWhy(c, v.why);
    }, LIMIT_NOTE_MS);
    showLimitPending(c);
  }
}

// Enter (`enter`) or leaving the box: a speed sets the limit (on leaving, only one typed that isn't
// the speed that applies); one that isn't a speed is said. The speed that applies goes back in the
// box.
function commitLimitValue(c, enter) {
  clearTimeout(c.typed);
  c.typed = 0;
  const l = state.limits[c.dir];
  const v = limitValue(c.box.value);
  if (enter || c.edited) {
    if (v.why) {
      showLimitWhy(c, v.why, LIMIT_WHY_MS);
    } else if (enter || !isAppliedLimit(c, c.box.value)) {
      showLimitWhy(c, '');
      // Not typed in: the speed that applies, exactly (the box shows it rounded).
      if (c.edited) l.mibps = v.mibps;
      sendLimit(c);
    }
  }
  c.edited = false;
  c.box.value = fmtLimit(l.mibps);
  c.box.classList.remove('bad');
  c.box.setAttribute('aria-invalid', 'false');
  showLimitPending(c);
}

// Escape: the speed that applies back in the box; nothing is set.
function cancelLimitEdit(c) {
  c.edited = false;
  clearTimeout(c.typed);
  c.typed = 0;
  c.box.value = fmtLimit(state.limits[c.dir].mibps);
  showLimitWhy(c, '');
}

// The backend's answer to a set (its token, for this control's direction): confirmed when the
// limit asked for is the one that applies now. An answer to an earlier set, overtaken by a later
// one, shows nothing.
function confirmLimit(c, msg) {
  const asked = c.asked;
  if (!asked || msg.token !== asked.token) return;
  c.asked = null;
  const l = state.limits[c.dir];
  if (l.on !== asked.on || (asked.on && Math.abs(l.mibps - asked.mibps) > 1e-6)) return; // not what applies
  showLimitSet(c, asked.on ? l.mibps : null);
}

// The confirmation: the box flashes green with a check mark (a limit set), and the badge above it
// says what was set (`mibps`; null: switched off, neutral) for LIMIT_BADGE_MS, then fades out.
// Another set meanwhile starts it again from the beginning. Not while a value typed meanwhile
// waits to be set (after the switch, which puts the focus in the box): "Enter to set" has the
// place above the box then.
function showLimitSet(c, mibps) {
  endLimitBadge(c);
  showLimitWhy(c, '');
  if (c.wrap.classList.contains('pending')) return;
  void c.badge.offsetWidth; // the animations start again
  const on = mibps !== null;
  const shown = c.badge.querySelector('.shown');
  if (on) {
    const mark = document.createElement('span');
    mark.className = 'mark';
    mark.textContent = '✓';
    shown.replaceChildren(mark, ` ${c.word} limit · ${fmtMiBps(mibps)}`);
  } else {
    shown.textContent = `${c.word} limit off`;
  }
  c.badge.querySelector('.said').textContent = on ? `${c.word} speed limit set to ${fmtMiBps(mibps)}`
    : `${c.word} speed limit off`;
  c.badge.classList.toggle('off', !on);
  c.badge.classList.add('show');
  c.shown = { on, mibps };
  c.wrap.classList.toggle('set', on);
  c.badgeTimer = setTimeout(() => {
    c.badgeTimer = 0;
    c.badge.classList.add('out');
    c.wrap.classList.remove('set');
    c.fade = setTimeout(() => endLimitBadge(c), LIMIT_FADE_MS);
  }, LIMIT_BADGE_MS);
}

// The badge and the box's flash gone, at once: at the end of the fade, or when what is typed
// next (pending, or why it can't be set) takes the place above the box.
function endLimitBadge(c) {
  clearTimeout(c.badgeTimer);
  clearTimeout(c.fade);
  c.badgeTimer = 0;
  c.fade = 0;
  c.shown = null;
  c.badge.classList.remove('show', 'out', 'off');
  c.wrap.classList.remove('set');
  c.badge.querySelector('.shown').textContent = '';
  c.badge.querySelector('.said').textContent = '';
}

function renderParallel() {
  $('#parallel').textContent = String(state.parallel);
  $('#parallel-down').disabled = state.parallel <= 1;
  $('#parallel-up').disabled = state.parallel >= 8;
}

function setParallel(n) {
  n = Math.max(1, Math.min(8, n));
  if (n === state.parallel) return;
  state.parallel = n;
  renderParallel();
  send({ type: 'setParallel', value: n });
}

function cancelAll() {
  const started = state.queue.some((q) => ['running', 'paused', 'failed'].includes(q.status) && q.bytes > 0);
  if (!started) { send({ type: 'cancelAll' }); return; }
  const ups = state.queue.some((q) => isUp(q) && UNFINISHED.includes(q.status));
  showModal({
    title: ups ? 'Cancel all downloads and uploads?' : 'Cancel all downloads?',
    html: `<p>Every running, queued and paused ${ups ? 'download and upload' : 'download'} stops, and their partly ` +
      `downloaded files are deleted${ups ? ' (what uploads sent but didn’t finish is removed from the server)' : ''}. ` +
      `Finished ${ups ? 'ones' : 'downloads'} are kept.</p>`,
    ok: 'Cancel all',
    danger: true,
    cancel: ups ? 'Keep going' : 'Keep downloading',
    onOk: () => send({ type: 'cancelAll' }),
  });
}

// ---- messages from the backend -----------------------------------------------------------

function onInit(msg) {
  if (!state.recentLoaded) {
    state.recent = Array.isArray(msg.recent) ? msg.recent : [];
    state.recentLoaded = true;
  }
  // The split and the Downloads table's view from the first init only: init comes again after
  // every server change, which must not move the panes back.
  if (!split.loaded) {
    split.loaded = true;
    if (typeof msg.split === 'number' && isFinite(msg.split)) {
      split.share = split.saved = Math.min(SPLIT.max, Math.max(SPLIT.min, msg.split));
    }
    applySplit();
    applyDlView(msg.downloads); // its sort and column widths
  }
  // Sent again after every server change; folders typed here but not used yet win.
  state.destinations = { ...msg.destinations, ...state.destinations };
  state.remotes = msg.remotes;
  state.remote = msg.remote;
  // Quick access's pins, from every init: a Remove or an Add changed them.
  takePins(msg.pins);
  // Sent again after a server change: each server's account is asked again as Browse opens it,
  // and what Cut or Copy held on a server gone is let go.
  ops.accounts.clear();
  if (ops.clip && !state.remotes.some((r) => r.name === ops.clip.server)) clearClip();
  if (state.browse.server && state.remotes.some((r) => r.name === state.browse.server)) askAccount(state.browse.server);
  const sort = sortFrom(msg.sort);
  if (JSON.stringify(sortToSave(sort)) !== JSON.stringify(sortToSave(state.sort))) {
    state.sort = sort;
    resort();
  }
  state.defaultDest = msg.defaultDest;
  if (msg.parallel) state.parallel = msg.parallel;
  if (msg.limit) onLimit(msg.limit);
  $('#version').textContent = `v${msg.version}`;
  if (typeof msg.updateCheck === 'boolean') $('#update-auto').checked = msg.updateCheck;
  if (typeof msg.verifyDownloads === 'boolean') $('#verify-on').checked = msg.verifyDownloads;
  if (msg.uploadExisting) setUploadExisting(msg.uploadExisting, false);
  setTheme(msg.theme, false);
  renderRemotes();
  renderSortHead();
  // Sent again after a server change: browse goes on where it is, if that server still exists.
  // The list of servers (All servers' top, or nothing before the first one) is built again from
  // them: after adding the first server, it opens (it stayed an empty page until a restart).
  const b = state.browse;
  if (!b.inited || !b.server || !state.remotes.some((r) => r.name === b.server)) enterBrowse();
  renderQuick();
  renderRows();
  if (msg.configError) {
    showBanner(`${msg.configError}\n\nConfig file: ${msg.configPath}`);
    state.configBanner = true;
  } else if (state.configBanner) {
    showBanner('');
    state.configBanner = false;
  }
}

// ---- updates ------------------------------------------------------------------------------

const UPDATE_PHASE = {
  checking: 'checking the release…',
  downloading: 'downloading…',
  verified: 'verified, installing…',
  installing: 'installing; grab closes and opens again by itself…',
  done: 'finishing…',
};

function renderUpdate() {
  const u = state.update;
  const b = $('#update-banner');
  const show = !!u.state && !(u.state === 'available' && state.updateLater);
  b.hidden = !show;
  b.classList.toggle('failed', u.state === 'failed');
  if (!show) return;
  const v = `<b>${esc(u.version)}</b>`;
  if (u.state === 'available') {
    b.innerHTML = `<span class="text">${u.completing ? `This folder lacks programs grab ${v} comes with` : `grab ${v} is available`}</span>` +
      (u.page ? '<button type="button" class="link" data-act="notes">What\'s new</button>' : '') +
      `<button type="button" class="primary" data-act="update">${u.completing ? 'Add them' : 'Update'}</button>` +
      '<button type="button" class="subtle" data-act="later">Later</button>';
  } else if (u.state === 'running') {
    // A try GitHub failed says so ("GitHub answered 504 ...; trying again in 3 s (try 2 of 6)").
    const step = u.note ? esc(u.note) : UPDATE_PHASE[u.phase] || 'starting…';
    b.innerHTML = `<span class="text"><span class="spinner"></span>Updating to ${v}: ${step}</span>`;
  } else if (u.state === 'installed') {
    const active = state.queue.some((q) => q.status === 'running' || q.status === 'queued');
    b.innerHTML = `<span class="text">grab ${v} is installed. Restart grab to use it` +
      `${active ? '; running downloads continue after the restart' : ''}.</span>` +
      '<button type="button" class="primary" data-act="restart">Restart grab</button>';
  } else if (u.state === 'failed') {
    b.innerHTML = `<span class="text">Update failed: ${esc(u.error)}</span>` +
      '<button type="button" data-act="update">Try again</button>' +
      '<button type="button" class="subtle" data-act="dismiss">Close</button>';
  }
}

$('#update-banner').addEventListener('click', (ev) => {
  const el = ev.target.closest('[data-act]');
  if (!el) return;
  const act = el.dataset.act;
  if (act === 'update') send({ type: 'updateStart' });
  else if (act === 'notes') send({ type: 'openReleasePage' });
  else if (act === 'restart') { el.disabled = true; send({ type: 'updateRestart' }); }
  else if (act === 'later') { state.updateLater = true; renderUpdate(); }
  else if (act === 'dismiss') { state.update = { state: '' }; renderUpdate(); send({ type: 'updateDismiss' }); }
});

// ---- the one-time notice after grab moved servers from rclone.conf into grab.conf -----------

function showImportBanner(text) {
  const b = $('#import-banner');
  b.hidden = !text;
  if (!text) return;
  b.innerHTML = '<span class="text"></span>' +
    '<button type="button" data-act="settings">Settings</button>' +
    '<button type="button" class="subtle" data-act="ok">OK</button>';
  b.querySelector('.text').textContent = bidiVisible(text);
}

$('#import-banner').addEventListener('click', (ev) => {
  const el = ev.target.closest('[data-act]');
  if (!el) return;
  showImportBanner('');
  if (el.dataset.act === 'settings' && !settings.open) openSettings();
});

$('#update-auto').addEventListener('change', (ev) => send({ type: 'setUpdateCheck', on: ev.target.checked }));
$('#verify-on').addEventListener('change', (ev) => send({ type: 'setVerify', on: ev.target.checked }));

// Settings → Appearance: Auto (follow Windows), Light or Dark, a radio group. The window applies it
// at once (WebView2's prefers-color-scheme, which app.css's colours follow, and the title bar);
// `tell`: the backend too (it says so back with "theme"). Anything else is Auto.
const THEMES = ['auto', 'light', 'dark'];
function setTheme(mode, tell = true) {
  if (!THEMES.includes(mode)) mode = 'auto';
  state.theme = mode;
  for (const b of $('#theme-set').querySelectorAll('button[data-mode]')) {
    const on = b.dataset.mode === mode;
    b.setAttribute('aria-checked', String(on));
    b.tabIndex = on ? 0 : -1; // one Tab stop, the chosen one; the arrows move between them
  }
  if (tell) send({ type: 'setTheme', mode });
}
$('#theme-set').addEventListener('click', (ev) => {
  const b = ev.target.closest('button[data-mode]');
  if (b && b.dataset.mode !== state.theme) setTheme(b.dataset.mode);
});
// A radio group's keys: ← ↑ the one before, → ↓ the next (around the ends), Home and End the first
// and the last; each chooses as a click does. Space and Enter press the focused one.
$('#theme-set').addEventListener('keydown', (ev) => {
  if (ev.altKey || ev.ctrlKey || ev.metaKey) return;
  const at = THEMES.indexOf(state.theme);
  const to = { ArrowLeft: at - 1, ArrowUp: at - 1, ArrowRight: at + 1, ArrowDown: at + 1, Home: 0, End: THEMES.length - 1 }[ev.key];
  if (to === undefined) return;
  ev.preventDefault();
  const mode = THEMES[(to + THEMES.length) % THEMES.length];
  if (mode !== state.theme) setTheme(mode);
  $('#theme-set').querySelector(`[data-mode="${mode}"]`).focus();
});

$('#refresh-lists').addEventListener('click', () => {
  send({ type: 'refreshLists', remote: state.remote });
  for (const l of state.lists) if (typeof l.age === 'number') l.refreshing = true;
  renderListStatus();
});
$('#update-now').addEventListener('click', () => {
  $('#update-now').disabled = true;
  const r = $('#update-result');
  r.className = 'muted';
  r.innerHTML = '<span class="spinner"></span>Checking…';
  send({ type: 'updateCheckNow' });
});

function onUpdateChecked(msg) {
  $('#update-now').disabled = false;
  const r = $('#update-result');
  r.className = msg.ok ? (msg.available ? 'good' : 'muted') : 'bad';
  r.textContent = bidiVisible(msg.text);
  if (msg.available) { state.updateLater = false; renderUpdate(); }
}

// ---- settings: speed schedule -------------------------------------------------------------

const DAYS = ['mon', 'tue', 'wed', 'thu', 'fri', 'sat', 'sun'];
const DAY_LABELS = ['Mo', 'Tu', 'We', 'Th', 'Fr', 'Sa', 'Su'];
const DAY_NAMES = ['Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday', 'Sunday'];
const MAX_RULES = 20; // schedule::max_rules

function fmtMiBps(mibps) { return `${fmtLimit(mibps)} MiB/s`; }

// "18", "1830", "18:30" or "8:30" as "HH:MM"; "24:00" only where `allow24`. null if not a time.
function parseTime(text, allow24) {
  const m = /^(\d{1,2})(?::?(\d{2}))?$/.exec(text.trim());
  if (!m) return null;
  const h = Number(m[1]);
  const min = m[2] === undefined ? 0 : Number(m[2]);
  if (min > 59 || h > 24 || (h === 24 && (min !== 0 || !allow24))) return null;
  return `${String(h).padStart(2, '0')}:${String(min).padStart(2, '0')}`;
}

// The end as people say it: midnight is "24:00" (kept as "00:00", the way the backend does).
function toText(to) { return to === '00:00' ? '24:00' : to; }

// A rule's speeds, by their keys in the backend's JSON: downloads (↓) 'mibps', uploads (↑)
// 'upMiBps'; a direction the rule doesn't limit has none (its box blank).
const RULE_SPEEDS = { mibps: { arrow: '↓', what: 'Download' }, upMiBps: { arrow: '↑', what: 'Upload' } };
function ruleSpeedText(r, field) { return r[field] === undefined ? '' : fmtLimit(r[field]); }

function ruleRow(r, i) {
  const days = DAYS.map((d, k) =>
    `<button type="button" data-day="${d}" aria-pressed="${r.days.includes(d)}" title="${DAY_NAMES[k]}">${DAY_LABELS[k]}</button>`).join('');
  const nextDay = r.to !== '00:00' && r.to <= r.from;
  const speeds = Object.entries(RULE_SPEEDS).map(([field, s]) =>
    `<span class="rule-dir" aria-hidden="true">${s.arrow}</span>` +
    `<input type="text" class="num" data-field="${field}" value="${ruleSpeedText(r, field)}" inputmode="decimal" placeholder="–" ` +
    `aria-label="${s.what} speed in MiB/s" title="${s.what}s at most this fast at these times; blank: not limited by this time">`).join('');
  return `<div class="rule" data-i="${i}">` +
    `<span class="segmented days" role="group" aria-label="Days">${days}</span>` +
    `<input type="text" class="time" data-field="from" value="${r.from}" aria-label="From" spellcheck="false">` +
    '<span class="muted">–</span>' +
    `<input type="text" class="time" data-field="to" value="${toText(r.to)}" aria-label="To" spellcheck="false">` +
    `<span class="next-day muted${nextDay ? '' : ' off'}">next day</span>` +
    speeds +
    '<span class="muted">MiB/s</span>' +
    '<button type="button" class="subtle icon-btn" data-act="remove" title="Remove this time" aria-label="Remove this time">✕</button>' +
    '<span class="rule-why" role="status" hidden></span>' +
    '</div>';
}

function renderSchedule() {
  $('#schedule-rules').innerHTML = state.schedule.map(ruleRow).join('');
  $('#schedule-add').disabled = state.schedule.length >= MAX_RULES;
}

// Where the schedule has a direction at now: the limit it sets, or none and its next. With rules for
// downloads only, as before uploads came; otherwise each direction marked.
function scheduleNowText(l) {
  const until = l.until ? ` until ${l.until}` : '';
  if (l.source === 'schedule') return `Now: ${fmtMiBps(l.mibps)}${l.until ? until : ', all week'}`;
  if (l.source === 'change') return `Now: ${l.on ? fmtMiBps(l.mibps) : 'no limit'} (your change)${until}`;
  if (l.until && l.nextMiBps) return `No scheduled limit now; next: ${l.until} at ${fmtMiBps(l.nextMiBps)}`;
  return 'No scheduled limit now';
}

function renderScheduleNow() {
  const el = $('#schedule-now');
  const has = (field) => state.schedule.some((r) => r[field] !== undefined);
  const down = scheduleNowText(state.limits.down);
  const up = scheduleNowText(state.limits.up);
  if (!state.schedule.length) el.textContent = '';
  else if (!has('upMiBps')) el.textContent = down;
  else if (!has('mibps')) el.textContent = `↑ ${up}`;
  else el.textContent = `↓ ${down} · ↑ ${up}`;
}

function sendSchedule() {
  send({ type: 'setSchedule', rules: state.schedule });
}

// Why a rule's field didn't take what was typed: said beside the row for a while.
function showRuleWhy(row, why) {
  const note = row.querySelector('.rule-why');
  clearTimeout(Number(note.dataset.timer || 0));
  note.textContent = why;
  note.hidden = !why;
  if (why) note.dataset.timer = String(setTimeout(() => { note.hidden = true; note.textContent = ''; }, LIMIT_WHY_MS));
}

// A field of a rule was edited: a valid value is kept and sent, anything else snaps back (a speed
// that isn't one says why). A blank speed box: that direction isn't limited by the rule, as long
// as the other one is.
function commitRuleField(input) {
  const row = input.closest('.rule');
  const i = Number(row.dataset.i);
  const r = state.schedule[i];
  if (!r) return;
  const field = input.dataset.field;
  let changed = false;
  if (RULE_SPEEDS[field]) {
    const other = field === 'mibps' ? 'upMiBps' : 'mibps';
    const t = input.value.trim();
    if (!t) {
      if (r[field] !== undefined && r[other] === undefined) {
        showRuleWhy(row, 'A time needs a speed: ↓ for downloads, ↑ for uploads, or both');
      } else if (r[field] !== undefined) {
        delete r[field];
        changed = true;
        showRuleWhy(row, '');
      }
    } else {
      const v = limitValue(t);
      if (v.why) showRuleWhy(row, v.why);
      else {
        changed = r[field] === undefined || Math.abs(v.mibps - r[field]) > 1e-9;
        r[field] = v.mibps;
        showRuleWhy(row, '');
      }
    }
    input.value = ruleSpeedText(r, field);
  } else {
    const t = parseTime(input.value, field === 'to');
    if (t) {
      const kept = field === 'to' && t === '24:00' ? '00:00' : t;
      changed = kept !== r[field];
      r[field] = kept;
    }
    input.value = field === 'to' ? toText(r.to) : r.from;
    input.closest('.rule').querySelector('.next-day').classList.toggle('off', !(r.to !== '00:00' && r.to <= r.from));
  }
  if (changed) sendSchedule();
}

$('#schedule-add').addEventListener('click', () => {
  if (state.schedule.length >= MAX_RULES) return;
  state.schedule.push({ days: [...DAYS], from: '18:00', to: '00:00', mibps: 5 });
  renderSchedule();
  sendSchedule();
  const rows = document.querySelectorAll('#schedule-rules .rule');
  rows[rows.length - 1].querySelector('[data-field=from]').focus();
});

$('#schedule-rules').addEventListener('click', (ev) => {
  const row = ev.target.closest('.rule');
  if (!row) return;
  const i = Number(row.dataset.i);
  const r = state.schedule[i];
  const day = ev.target.closest('[data-day]');
  if (day && r) {
    const d = day.dataset.day;
    const on = r.days.includes(d);
    if (on && r.days.length === 1) return; // a time needs at least one day
    r.days = on ? r.days.filter((x) => x !== d) : DAYS.filter((x) => x === d || r.days.includes(x));
    day.setAttribute('aria-pressed', String(!on));
    sendSchedule();
  } else if (ev.target.closest('[data-act=remove]')) {
    state.schedule.splice(i, 1);
    renderSchedule();
    sendSchedule();
  }
});

$('#schedule-rules').addEventListener('change', (ev) => {
  if (ev.target.matches('input[data-field]')) commitRuleField(ev.target);
});
$('#schedule-rules').addEventListener('keydown', (ev) => {
  const input = ev.target.closest('input[data-field]');
  if (!input) return;
  if (ev.key === 'Enter') { ev.preventDefault(); commitRuleField(input); input.blur(); }
  else if (ev.key === 'Escape') {
    const r = state.schedule[Number(input.closest('.rule').dataset.i)];
    const f = input.dataset.field;
    input.value = RULE_SPEEDS[f] ? ruleSpeedText(r, f) : f === 'to' ? toText(r.to) : r.from;
    input.blur();
  }
});

$('#limit-hint').addEventListener('click', () => {
  if (!settings.open) openSettings();
  else if (settings.form) closeForm();
  $('#schedule-panel').scrollIntoView({ block: 'nearest' });
});

// ---- settings: servers -------------------------------------------------------------------

const settings = {
  open: false,
  servers: [],
  form: null,     // null, or { editing, name, origAuth, ... } while the add/edit form is shown
  auth: 'password',
  // The form's Connect choice (PROXIES.md 3.5): 'none' (directly), 'socks5', 'http', or null while
  // an Edit whose proxy grab can't offer (proxyWhy) has to choose.
  proxy: 'none',
  tests: {},      // server name -> serverTested message, or { pending: true }
  busy: false,
  status: '',
};

const AUTH_TEXT = { password: 'password', key: 'key file', agent: 'ssh-agent', stored_key: 'key in grab\'s store' };
// How a server is searched (the backend's search_method): find, the folders listed by grab, or
// find where grab's check finds it.
const SEARCH_TEXT = {
  find: 'find',
  walk: 'listing folders',
  probe: 'find or listing folders',
};
const SEARCHED_TEXT = {
  find: 'with find',
  walk: 'by listing folders',
  probe: 'with find, else by listing folders',
};

function showSettingsBanner(text) {
  const b = $('#settings-banner');
  b.hidden = !text;
  b.textContent = bidiVisible(text || '');
}

function setStatus(text) {
  settings.status = text;
  renderStatus();
}

function renderStatus() {
  const el = $('#settings-status');
  const text = settings.status || (settings.busy ? 'Working…' : '');
  el.innerHTML = text ? `${settings.busy ? '<span class="spinner"></span>' : ''}${esc(text)}` : '';
  $('#settings-cancel').hidden = !settings.busy;
  $('#form-save').disabled = settings.busy;
  $('#add-server').disabled = settings.busy || !!settings.form;
}

function refreshServers() {
  if (!settings.busy) send({ type: 'servers' });
}

function openSettings(addFirst = false) {
  settings.open = true;
  document.body.classList.add('in-settings');
  $('#search-view').hidden = true;
  $('#settings-view').hidden = false;
  showSettingsBanner('');
  refreshServers();
  if (addFirst) openForm(null); else closeForm();
}

function closeSettings() {
  settings.form = null;
  settings.open = false;
  document.body.classList.remove('in-settings');
  $('#settings-view').hidden = true;
  $('#search-view').hidden = false;
  $('#query').focus();
}

// `route`: the proxy the server's card says it goes through ("the SOCKS5 proxy 127.0.0.1:9050"),
// for a test still running; a result says the one its login went through (`through`).
function testLine(t, route = '') {
  if (!t) return '';
  if (t.pending) {
    return `<span class="muted"><span class="spinner"></span>Testing the connection${route ? ` through ${esc(route)}` : ''}…</span>`;
  }
  if (t.error) return `<span><span class="bad">✗ Test failed:</span> <span class="detail">${esc(t.error)}</span></span>`;
  // A server grab doesn't connect to: why, and nothing tried.
  if (t.unusable) return `<span><span class="bad">✗ Not usable:</span> <span class="detail">${esc(t.unusable)}</span></span>`;
  const via = t.through ? ` through ${esc(t.through)}` : '';
  // As `grab server test` says it: grab's login listed the home folder.
  if (t.ok) {
    const how = ' · home folder listed' + (t.grabSearch ? ` · searches ${esc(t.grabSearch)}` : '') +
      (t.checksums ? ` · ${esc(t.checksums)}` : '');
    return `<span class="good">✓ Connection OK${via}</span><span class="muted">${how}</span>`;
  }
  return `<span><span class="bad">✗ Connection${via} failed:</span> <span class="detail">${esc(t.grabError || 'failed')}</span></span>`;
}

// The proxy a server goes through, never its password, and where a kept one comes from:
// " · through the SOCKS5 proxy alice@127.0.0.1:9050"; '' directly.
function routeText(s) {
  if (!s.route) return '';
  const kept = s.routeFrom === 'kept'
    ? ' (kept in grab\'s store: from rclone, or a proxy line since removed; Edit writes it into grab.conf)' : '';
  return ` · through ${esc(s.route)}${kept}`;
}

// How a server logs in, and whether what it needs is saved (never the value): "password
// (saved)", "password (none saved: enter it with Edit)".
function loginText(s) {
  const how = AUTH_TEXT[s.auth];
  if (s.auth === 'password' && s.secretSaved === true) return `${how} (saved)`;
  if (s.auth === 'password' && s.secretSaved === false) return `${how} <span class="warn">(none saved: enter it with Edit)</span>`;
  if (s.auth === 'key' && s.secretSaved === true) return `${how} (passphrase saved)`;
  if (s.auth === 'stored_key' && s.secretSaved === false) return `${how} <span class="warn">(not in the store: choose another login with Edit)</span>`;
  return how;
}

function serverCard(s) {
  const btn = (act, label, cls = '') =>
    `<button type="button" class="${cls}" data-act="${act}" data-name="${esc(s.name)}"${settings.busy ? ' disabled' : ''}>${label}</button>`;
  const actions = [];
  if (!s.error) {
    actions.push(btn('test', 'Test'));
    // Not for a server grab doesn't connect to (its card says why): Trust would be refused.
    if (!s.pinned && !s.guard) actions.push(btn('trust', 'Trust…'));
  }
  // A section without connection details (or with ones grab can't read) is filled in with Edit.
  actions.push(btn('edit', 'Edit'));
  if (!s.error && !s.isDefault) actions.push(btn('default', 'Make default'));
  actions.push(btn('remove', 'Remove', 'danger'));

  const where = s.roots.length ? s.roots.join(', ') : '~';
  const meta = s.error
    ? `<span class="bad">${esc(s.error)}</span>`
    : `${esc(s.user)}@${esc(s.host)}:${s.port}${routeText(s)} · ${loginText(s)}` +
      ` · searched ${SEARCHED_TEXT[s.search] || SEARCHED_TEXT.probe} in ${esc(where)}, depth ${s.depth}` +
      ` · up to ${s.maxConnections || s.autoConnections} download connections${s.maxConnections ? '' : ' (automatic)'}` +
      (s.connectionKeys ? ` · ${esc(s.connectionKeys)}` : '');
  const status = [];
  if (!s.error && !s.guard) {
    status.push(s.pinned ? '<span class="good">✓ Host key pinned</span>'
      : '<span class="warn">⚠ Host key not trusted yet: grab won\'t connect to this server until you pin its key with Trust….</span>');
  }
  // Why grab doesn't connect to it at all (a proxy, an ssh command...), and how to change that.
  if (!s.error && s.guard) status.push(`<span class="bad">✗ Not usable: ${esc(s.guard)}</span>`);
  const test = testLine(settings.tests[s.name], s.route);
  return `<div class="server">` +
    `<div class="server-top"><span class="server-name">${esc(s.name)}</span>` +
    `${s.isDefault ? '<span class="badge">default</span>' : ''}` +
    `<span class="server-actions">${actions.join('')}</span></div>` +
    `<div class="server-meta">${meta}</div>` +
    `<div class="server-status">${status.map((x) => `<span>${x}</span>`).join('')}${test}</div></div>`;
}

function renderServers() {
  const list = $('#server-list');
  list.hidden = !!settings.form;
  $('#updates-panel').hidden = !!settings.form;
  $('#schedule-panel').hidden = !!settings.form;
  $('#downloads-panel').hidden = !!settings.form;
  $('#uploads-panel').hidden = !!settings.form;
  $('#appearance-panel').hidden = !!settings.form;
  $('#server-form').hidden = !settings.form;
  renderStatus();
  if (settings.form) return;
  list.innerHTML = settings.servers.length ? settings.servers.map(serverCard).join('')
    : '<div class="settings-empty">No servers yet. Add the Linux server you want to download from: ' +
      'grab connects over SFTP/SSH with a password, a key file or ssh-agent.<br>' +
      '<button type="button" class="primary" data-act="add">Add a server</button></div>';
}

// A saved password stays only with the user, host and port it was typed for, compared exactly
// as the backend's binding does (G40); a key's passphrase moves with the server but stays only
// with its key file.
function keepsSecret(auth) {
  const f = settings.form;
  if (!f || !f.editing || f.origAuth !== auth || !f.secretSaved) return false;
  if (auth === 'key') return $('#f-key').value.trim() === f.origKey;
  return auth !== 'password' || ($('#f-host').value.trim() === f.origHost &&
    $('#f-user').value.trim() === f.origUser && intOf('#f-port') === f.origPort);
}

function setSecretPlaceholder() {
  const auth = settings.auth;
  const f = settings.form;
  $('#f-secret').placeholder = keepsSecret(auth)
    ? (auth === 'key' ? 'Leave blank to keep the saved passphrase' : 'Leave blank to keep the saved password')
    : auth === 'key' ? 'Only if the key has one'
    : f && f.editing && f.origAuth === 'password' && f.secretSaved ? 'Enter it again for the new user, host or port' : '';
}

function setAuth(auth) {
  settings.auth = auth;
  for (const b of $('#f-auth').querySelectorAll('button')) b.setAttribute('aria-checked', String(b.dataset.auth === auth));
  for (const el of document.querySelectorAll('[data-auth-only]')) el.hidden = !el.dataset.authOnly.split(' ').includes(auth);
  $('#secret-label').textContent = auth === 'key' ? 'Passphrase' : 'Password';
  setSecretPlaceholder();
  // grab logs in by itself, with the passphrase too, and searches every server the same way: find
  // where the server has GNU find, else by listing folders.
  $('#secret-hint').textContent =
    'Kept in grab\'s own password store, encrypted for your Windows account; never in grab.conf or on a command line.';
  const searches = 'searches use find where the server has it, else grab lists its folders.';
  $('#auth-hint').textContent = {
    password: `Works without a shell (e.g. storage boxes); ${searches}`,
    key: 'Searches use find where the server has it, else grab lists its folders.',
    agent: `Uses the keys loaded in the Windows ssh-agent (ssh-add); ${searches}`,
  }[auth];
}

// ---- the route (PROXIES.md 3.5) ----------------------------------------------------------------
// Connect: Directly, or through a SOCKS5 or HTTP proxy with its host, port, user and password, as
// `grab server add` asks. An Edit starts from the backend's proxyOffer (what a save that keeps it
// leaves: its `proxy` line, else one grab's store keeps for it, said where from), never a saved
// password, only that one is saved; with none offered (proxyWhy) no choice is on until one is made.

const PROXY_KIND = { socks5: 'SOCKS5', http: 'HTTP' };

// The proxy's host as typed, an IPv6 address without its brackets (grab.conf's form).
function proxyHostOf() {
  return $('#f-proxy-host').value.trim().replace(/^\[(.*)\]$/, '$1');
}

function setProxyKind(kind) {
  settings.proxy = kind;
  for (const b of $('#f-proxy').querySelectorAll('button')) b.setAttribute('aria-checked', String(b.dataset.proxy === kind));
  const via = kind === 'socks5' || kind === 'http';
  for (const el of document.querySelectorAll('[data-proxy-only]')) el.hidden = !via;
  setProxySecretPlaceholder();
}

// The proxy's saved password stays only with the proxy and user it was typed for (as the backend
// keeps it: the same kind, host, port and user as offered); otherwise blank means none.
function keepsProxySecret() {
  const f = settings.form;
  const o = f && f.proxyOffer;
  if (!f || !f.editing || !o || !o.passwordSaved) return false;
  return settings.proxy === o.kind && proxyHostOf() === o.host && intOf('#f-proxy-port') === o.port &&
    $('#f-proxy-user').value.trim() === o.user;
}

// The password goes to the proxy only with a user (as the backend sends it).
function setProxySecretPlaceholder() {
  const f = settings.form;
  const input = $('#f-proxy-secret');
  const user = $('#f-proxy-user').value.trim() !== '';
  input.disabled = !user;
  if (!user) input.value = '';
  input.placeholder = !user ? 'Only with a proxy user'
    : keepsProxySecret() ? 'Leave blank to keep the saved password'
    : f && f.editing && f.proxyOffer && f.proxyOffer.passwordSaved ? 'Type it again for this proxy and user, or leave blank for none'
    : 'Only if the proxy wants one';
}

// Under Connect: why an Edit has to choose (no proxy offered), or where an offered one comes from.
function renderProxyFrom() {
  const f = settings.form;
  const el = $('#proxy-from');
  const o = f && f.proxyOffer;
  const choose = !!(f && f.editing && !o);
  let text = '';
  if (choose) {
    text = `${f.proxyWhy ? `${f.proxyWhy.replace(/\.$/, '')}. ` : ''}Choose Directly, SOCKS5 proxy or HTTP proxy.`;
  } else if (o && o.from === 'kept') {
    text = `${f.name} goes through ${o.said}, kept in grab's store (from rclone, or a proxy line since removed): ` +
      'Save writes it into grab.conf.';
  }
  el.textContent = bidiVisible(text);
  el.hidden = !text;
  el.classList.toggle('warn', choose);
}

function clearFieldErrors() {
  for (const el of document.querySelectorAll('.field-error')) el.textContent = '';
}

function openForm(name) {
  const s = name ? settings.servers.find((x) => x.name === name) : null;
  // A section without connection details (s.error) starts from a blank host and user.
  const known = s && !s.error;
  settings.form = { editing: !!s, name: s ? s.name : '', origAuth: known ? s.auth : null,
    origHost: known ? s.host : '', origUser: known ? s.user : '', origPort: known ? s.port : 22,
    origKey: known ? (s.keyFile || '') : '', secretSaved: !!known && s.secretSaved === true,
    // What Connect starts from: a new server connects directly (PROXIES.md decision 2).
    proxyOffer: s ? s.proxyOffer || null : null, proxyWhy: s ? s.proxyWhy || '' : '' };
  $('#form-title').textContent = s ? (known ? `Edit ${s.name}` : `Fill in ${s.name}'s connection details`) : 'Add a server';
  $('#f-name').value = s ? s.name : '';
  $('#f-name').disabled = !!s;
  $('#f-host').value = known ? s.host : '';
  $('#f-port').value = known ? s.port : 22;
  $('#f-user').value = known ? s.user : '';
  $('#f-key').value = known ? s.keyFile : '';
  $('#f-secret').value = '';
  $('#f-roots').value = s ? s.roots.join('\n') : '';
  $('#f-depth').value = s ? s.depth : 4;
  $('#f-conns').value = s && s.maxConnections ? s.maxConnections : '';
  $('#f-conns').placeholder = s ? String(s.autoConnections) : '';
  $('#conns-hint').textContent = s ? `blank = automatic (now ${s.autoConnections})` : 'blank = automatic';
  setAuth(known ? s.auth : 'password');
  const offer = settings.form.proxyOffer;
  const via = !!offer && offer.kind !== 'none';
  $('#f-proxy-host').value = via ? offer.host : '';
  $('#f-proxy-port').value = via ? offer.port : '';
  $('#f-proxy-user').value = via ? offer.user : '';
  $('#f-proxy-secret').value = '';
  setProxyKind(!s ? 'none' : offer ? offer.kind : null);
  renderProxyFrom();
  clearFieldErrors();
  showSettingsBanner('');
  renderServers();
  (s ? $('#f-host') : $('#f-name')).focus();
}

function closeForm() {
  settings.form = null;
  $('#f-secret').value = '';
  $('#f-proxy-secret').value = '';
  renderServers();
}

function intOf(id) {
  const v = $(id).value.trim();
  return /^\d{1,6}$/.test(v) ? Number(v) : 0;
}

function submitForm() {
  if (settings.busy || !settings.form) return;
  clearFieldErrors();
  showSettingsBanner('');
  if (!settings.proxy) {
    onServerError({ field: 'connect', message: 'choose how grab connects to it: Directly, SOCKS5 proxy or HTTP proxy' });
    return;
  }
  const via = settings.proxy !== 'none';
  const msg = {
    type: 'serverSave',
    editing: settings.form.editing,
    name: settings.form.editing ? settings.form.name : $('#f-name').value.trim(),
    host: $('#f-host').value.trim(),
    port: intOf('#f-port'),
    user: $('#f-user').value.trim(),
    auth: settings.auth,
    keyFile: $('#f-key').value.trim(),
    secret: settings.auth === 'agent' ? '' : $('#f-secret').value,
    roots: $('#f-roots').value.split(/\r?\n/).map((r) => r.trim()).filter(Boolean),
    depth: intOf('#f-depth'),
    connections: $('#f-conns').value.trim() === '' ? 0 : (intOf('#f-conns') || -1),
    // The route, always given; a field left blank is the backend's to name.
    proxyKind: settings.proxy,
    proxyHost: via ? proxyHostOf() : '',
    proxyPort: !via || $('#f-proxy-port').value.trim() === '' ? 0 : (intOf('#f-proxy-port') || -1),
    proxyUser: via ? $('#f-proxy-user').value.trim() : '',
    // Blank keeps the saved one while the proxy and its user stay the same.
    proxySecret: via && !$('#f-proxy-secret').disabled ? $('#f-proxy-secret').value : '',
  };
  send(msg);
  $('#f-secret').value = ''; // the backend holds it until the host key is trusted
  $('#f-proxy-secret').value = '';
  setStatus(via ? `Checking the host key through the ${PROXY_KIND[settings.proxy]} proxy…` : 'Checking the host key…');
}

// ---- modal -------------------------------------------------------------------------------

let modal = null; // { onOk, onCancel, stay }
let modalReturn = null; // where the focus goes back to (the Downloads table after Delete's question)

// `stay`: OK runs onOk and the dialog stays until it's closed (dropModal: Rename's answer came).
// `cancel` null: no Cancel button (Properties' only button is OK, "Close").
function showModal({ title, html, ok, cancel = 'Cancel', danger = false, onOk, onCancel, extra = [], wide = false, stay = false }) {
  if (!$('#modal').contains(document.activeElement)) modalReturn = document.activeElement;
  modal = { onOk, onCancel, stay };
  $('#modal-title').textContent = bidiVisible(title);
  $('#modal-body').innerHTML = html;
  $('#modal .modal').classList.toggle('wide', wide); // a table of files (the upload question's)
  $('#modal-cancel').textContent = cancel || '';
  $('#modal-cancel').hidden = !cancel;
  const okBtn = $('#modal-ok');
  okBtn.textContent = ok;
  okBtn.disabled = false; // the upload dialog's waits for its checks (onUploadCheck)
  okBtn.classList.toggle('danger', danger);
  // More choices, between Cancel and OK: each closes the dialog and acts.
  document.querySelectorAll('#modal .modal-extra').forEach((b) => b.remove());
  for (const x of extra) {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'modal-extra';
    b.textContent = x.label;
    b.addEventListener('click', () => {
      if (!modal) return;
      modal = null;
      $('#modal').hidden = true;
      returnFromModal();
      x.act();
    });
    okBtn.before(b);
  }
  $('#modal').hidden = false;
  (cancel ? $('#modal-cancel') : okBtn).focus(); // the safe choice is the default
}

function closeModal(accepted) {
  if (!modal) return;
  const m = modal;
  modal = null;
  $('#modal').hidden = true;
  returnFromModal();
  if (accepted) m.onOk && m.onOk(); else m.onCancel && m.onCancel();
}

// Closes the dialog without calling its choices (one that acted already: Rename's answer came).
function dropModal() {
  if (!modal) return;
  modal = null;
  $('#modal').hidden = true;
  returnFromModal();
}

// The focus back where it was before the dialog, as a menu does; first, so what the dialog's
// choice does (another dialog, a field to fill) can take it.
function returnFromModal() {
  const el = modalReturn;
  modalReturn = null;
  if (el && el !== document.body && el.isConnected) el.focus({ preventScroll: true });
}

function onHostKeys(msg) {
  setStatus('');
  const rows = msg.fingerprints.length
    ? msg.fingerprints.map((f) => `<tr><td>${esc(f.type)}</td><td><code>${esc(f.hash)}</code></td></tr>`).join('')
    : msg.lines.map((l) => `<tr><td>key</td><td><code>${esc(l)}</code></td></tr>`).join('');
  showModal({
    title: msg.kind === 'trust' ? `Trust ${msg.name}?` : 'Trust this server?',
    html: `<p><b>${esc(msg.host)}:${msg.port}</b> presents these host keys:</p>` +
      `<table class="fingerprints">${rows}</table>` +
      '<p class="muted">Compare them with the fingerprints your provider shows, or run ' +
      '<code>ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub</code> on the server. ' +
      'If they differ, choose Cancel: something else may be answering at this address.</p>' +
      // Read through a proxy, one of them new: the proxy could have shown its own.
      (msg.note ? `<p class="proxy-note">${esc(msg.note)}</p>` : '') +
      '<p class="muted">Once trusted, searches and downloads refuse a server that presents another key.</p>',
    ok: msg.kind === 'add' ? 'Trust and add' : msg.kind === 'edit' ? 'Trust and save' : 'Trust',
    onOk: () => { setStatus('Saving…'); send({ type: 'serverConfirm', trust: true }); },
    onCancel: () => { send({ type: 'serverConfirm', trust: false }); setStatus(''); },
  });
}

function onServerError(msg) {
  setStatus('');
  const target = msg.field && settings.form && document.querySelector(`.field-error[data-for="${msg.field}"]`);
  if (target) {
    target.textContent = bidiVisible(msg.message);
    const input = { name: '#f-name', host: '#f-host', port: '#f-port', user: '#f-user', keyFile: '#f-key',
      secret: '#f-secret', depth: '#f-depth', connections: '#f-conns', connect: '#f-proxy button',
      proxy: '#f-proxy-host', proxyPort: '#f-proxy-port', proxyUser: '#f-proxy-user',
      proxySecret: '#f-proxy-secret' }[msg.field];
    if (input) $(input).focus();
  } else if (settings.open) {
    showSettingsBanner(msg.message);
  } else {
    showBanner(msg.message);
  }
}

function onServerSaved(msg) {
  if (settings.form) closeForm();
  settings.tests[msg.name] = { pending: true };
  setStatus('');
  renderServers();
}

function onSettingsAction(act, name) {
  if (act === 'add') return openForm(null);
  if (act === 'edit') return openForm(name);
  if (act === 'test') {
    settings.tests[name] = { pending: true };
    renderServers();
    return send({ type: 'serverTest', name });
  }
  if (act === 'trust') {
    setStatus('Checking the host key…');
    return send({ type: 'serverTrust', name });
  }
  if (act === 'default') return send({ type: 'serverDefault', name });
  if (act === 'remove') {
    // Its pins in Quick access go too (PINS.md 3.4).
    const pinned = pinsOf(name).length > 0;
    showModal({
      title: `Remove ${name}?`,
      html: `<p>grab forgets this server: its section in grab.conf, its saved password, its file list${pinned ? ', its pinned folders' : ''} and ` +
        'what grab learned about it are deleted, and its host keys too unless another server uses that address. ' +
        'Files you already downloaded stay where they are, and rclone.conf isn\'t changed.</p>',
      ok: 'Remove',
      danger: true,
      onOk: () => { delete settings.tests[name]; send({ type: 'serverRemove', name }); },
    });
  }
}

if (webview) {
  webview.addEventListener('message', (ev) => {
    const msg = ev.data;
    switch (msg.type) {
      case 'init': onInit(msg); break;
      case 'searchResult': onSearchResult(msg); break;
      case 'searchError': onSearchError(msg); break;
      case 'browseResult': onBrowseResult(msg); break;
      case 'spaceShort': onSpaceShort(msg); break;
      case 'existing': onExisting(msg); break;
      case 'uploadCheck': onUploadCheck(msg); break;
      case 'uploadExisting': setUploadExisting(msg.choice, false); break;
      case 'filesAccount': onFilesAccount(msg); break;
      case 'filesDone': onFilesDone(msg); break;
      case 'filesProgress': onFilesProgress(msg); break;
      case 'pins': onPins(msg); break;
      case 'theme': setTheme(msg.mode, false); break;
      case 'activity': onActivity(msg); break;
      case 'suggestions': onSuggestions(msg); break;
      case 'folderPicked': onFolderPicked(msg); break;
      case 'queue':
        onQueue(msg);
        if (state.update.state === 'installed') renderUpdate();
        break;
      case 'queueDelta':
        onQueueDelta(msg);
        if (state.update.state === 'installed') renderUpdate();
        break;
      case 'files': onFiles(msg); break;
      case 'fileDelta': onFileDelta(msg); break;
      case 'speed': onSpeed(msg); break;
      case 'openAsk': onOpenAsk(msg); break;
      case 'downloadsNote': dlNote(msg.text); break;
      case 'update': state.update = msg; renderUpdate(); break;
      case 'updateChecked': onUpdateChecked(msg); break;
      case 'limit': onLimit(msg); break;
      case 'error':
        if (state.flash) state.flash.until = 0; // "Starting …" is over
        showBanner(msg.message);
        break;
      case 'played': flash(`Playing ${msg.name} in ${msg.program}`); break;
      case 'servers':
        settings.servers = msg.items;
        renderServers();
        break;
      case 'settingsBusy':
        settings.busy = msg.busy;
        if (!msg.busy) {
          settings.status = '';
          // A test still "pending" now was cancelled: its result comes before this message.
          for (const k of Object.keys(settings.tests)) if (settings.tests[k].pending) delete settings.tests[k];
        }
        if (settings.form) renderStatus(); else renderServers();
        break;
      case 'hostKeys': onHostKeys(msg); break;
      case 'serverSaved': onServerSaved(msg); break;
      case 'serverTested': settings.tests[msg.name] = msg; renderServers(); break;
      case 'serverError': onServerError(msg); break;
      case 'filePicked': $('#f-key').value = msg.path; setSecretPlaceholder(); break;
      case 'importBanner': showImportBanner(msg.text); break;
      case 'pageRestored': onPageRestored(msg); break;
    }
  });
}

// ---- drops from Explorer: uploads (UPLOADS.md 5.10, chunk 4) --------------------------------
// Files and folders dragged in from Explorer onto Browse go into the folder row under the pointer,
// else into the folder Browse shows: the same check and dialog as Upload files here…. Every drag
// over the window is kept from navigating it (dragover and drop are taken everywhere, but text
// over a text field, which goes in as before); a drag of text alone shows nothing, a link is refused, and Browse's refusals (search results, All servers,
// the list of a server's search roots, a folder still opening) and the places outside Browse say
// why, or where to drop. The overlay goes with the drag: a drop anywhere, the drag leaving the
// window or cancelled (Escape: WebView2 then says dragleave at 0,0), or no dragover for
// dropIdleMs (dragover comes every 50 ms while a drag is over the page, moving or not).

let dropIdleMs = 1000; // the page tests shorten it
// UPLOADS.md decision 7, as the host says it too (upload_check.hpp drop_extract_first).
const EXTRACT_FIRST = 'Extract files from a .zip first: a file inside a .zip (or a mail’s attachment) isn’t on this PC ' +
  'as a file until it is extracted or saved. Then drop it from there.';
// As the host says it too (upload_check.hpp drop_link).
const DROP_FILES = 'Drop files or folders from Explorer: grab uploads files, not links.';
// `shown`: the overlay is up; `place`: where a drop would go ({ server, dir, row } or { refusal,
// area }); `idle`: the timer that takes it down when dragovers stop.
const drops = { shown: false, place: null, idle: 0 };

// Posts `msg` with File objects beside it, which the host gets as ICoreWebView2File with their
// paths (a dropped folder too). False when this WebView2 can't. Replaceable: the page tests stub it.
function postWithObjects(msg, objects) {
  if (!webview || !webview.postMessageWithAdditionalObjects) return false;
  webview.postMessageWithAdditionalObjects(msg, objects);
  return true;
}

// What a drag carries, by its types (its names can't be read before the drop): 'files', 'link' or
// '' (text alone, or nothing). A link dragged from a page in Edge or Chrome comes as text with a
// text/uri-list and no file (the .url shortcut they offer is marked for Chromium to ignore). A
// drag with files and a URL is 'files' until the drop (linkDrop): an Internet shortcut (.url)
// dragged from Explorer carries its URL too.
function dragKind(dt) {
  const types = dt ? [...dt.types] : [];
  if (types.includes('Files')) return 'files';
  return types.includes('text/uri-list') ? 'link' : '';
}

// Whether a drop of `files` (each a File, or null for one WebView2 couldn't get) with a URL beside
// them is a link from a browser, not files from Explorer: Explorer's drags carry a URL only for an
// Internet shortcut (.url), the URL WebView2 reads from it, while a page's address dragged from
// Edge's address bar (or a link from a browser that doesn't mark its shortcut) comes with the
// shortcut WebView2 saved for the drop, named .download. One still named .url in WebView2's temp
// folder is told by grab (upload_check.hpp drop_refusal, the message's `url`).
function linkDrop(dt, files) {
  return [...dt.types].includes('text/uri-list') && !files.some((f) => f && /\.url$/i.test(f.name));
}

// Where a drop at `target` (the element under the pointer) goes: { server, dir, row } (row: the
// folder row's index, else -1), or { refusal, area } with the reason and the part of the window
// it is about ('browse', 'downloads', 'status' or 'window').
function dropPlace(target) {
  const t = target instanceof Element ? target : null;
  if (!$('#modal').hidden) return { refusal: 'Finish the open dialog first, then drop again.', area: 'window' };
  if (viewer.open) return { refusal: 'Close the preview first, then drop on a folder in Browse.', area: 'window' };
  if (settings.open) return { refusal: 'Close Settings first, then drop on a folder in Browse.', area: 'window' };
  // Outside Browse (the Downloads table, the status bar): where to drop instead.
  const outside = t && t.closest('#downloads, .bar.status');
  if (outside) {
    return { refusal: 'Drop on Browse, above, to upload: into the folder it shows, or onto a folder in it.',
      area: outside.id === 'downloads' ? 'downloads' : 'status' };
  }
  const b = state.browse;
  const el = t && t.closest('#rows .row');
  const i = el ? Number(el.dataset.i) : -1;
  const h = i >= 0 ? state.hits[i] : null;
  // A folder row (not a server at All servers' top): that folder, as its menu's Upload entries.
  if (h && h.dir && !h.server && !b.loading) {
    if (b.under) return { refusal: 'Uploads go into a folder you have open, not into search results: open this folder first.', area: 'browse' };
    return { server: b.server || h.remote, dir: h.path, row: i };
  }
  const why = uploadRefusal();
  if (why) return { refusal: why, area: 'browse' };
  return { server: b.server, dir: b.path, row: -1 };
}

// The overlay over the part of the window the drag is over, saying where a drop goes or why not;
// the folder row a drop goes into is highlighted.
function showDropPlace(place) {
  drops.shown = true;
  // A PDF preview is a frame whose drag events never reach this page: while a drag is on, the
  // frame lets them through (html.dropping), so the preview's refusal shows over it too. A drag
  // that enters the window straight onto the frame isn't seen until it crosses the bar or margin.
  document.documentElement.classList.add('dropping');
  drops.place = place;
  const o = $('#drop-overlay');
  const area = place.refusal ? place.area : 'browse';
  const el = { browse: '#results', downloads: '#downloads', status: '.bar.status' }[area];
  const r = el ? $(el).getBoundingClientRect() : { left: 0, top: 0, width: innerWidth, height: innerHeight };
  Object.assign(o.style, { left: `${r.left}px`, top: `${r.top}px`, width: `${r.width}px`, height: `${r.height}px` });
  o.classList.toggle('refused', !!place.refusal);
  const text = place.refusal || `Drop to upload to ${place.server}:${place.dir || '~'}`;
  const label = o.querySelector('.drop-label');
  if (label.textContent !== text) label.textContent = bidiVisible(text);
  o.hidden = false;
  markDropRow(place.refusal ? -1 : place.row);
}

function markDropRow(i) {
  for (const el of document.querySelectorAll('#rows .row.drop-target')) {
    if (Number(el.dataset.i) !== i) el.classList.remove('drop-target');
  }
  if (i < 0) return;
  const el = document.querySelector(`#rows .row[data-i="${i}"]`);
  if (el) el.classList.add('drop-target');
}

// The drag is over (dropped, gone, cancelled or quiet): the overlay goes. A refusal still shown
// stays a moment in the footer (`say`), so a drop let go where none is taken says why.
function endDropOverlay(say = false) {
  clearTimeout(drops.idle);
  drops.idle = 0;
  const refusal = drops.place && drops.place.refusal;
  drops.shown = false;
  drops.place = null;
  document.documentElement.classList.remove('dropping');
  $('#drop-overlay').hidden = true;
  markDropRow(-1);
  try { sessionStorage.removeItem('grab.drag'); } catch (e) { /* no storage */ }
  if (say && refusal) flash(refusal);
}

// A text field a drag of text may go into (or move within): the search box, the path bar, a form's
// field. Not a checkbox or switch (selectionStart is null there), so a link dropped on one is still
// taken and can't open a window.
function textField(target) {
  const el = target instanceof Element ? target.closest('input, textarea, [contenteditable]') : null;
  return !!el && (el.isContentEditable || (!el.readOnly && !el.disabled && el.selectionStart !== null));
}

function onDragOver(ev) {
  const dt = ev.dataTransfer;
  const kind = dragKind(dt);
  // Text, or a link (Chromium gives any text that reads as a URL a text/uri-list too: a path like
  // Main:/srv/tv or C:\Users\...): a text field takes it as it always did.
  if (kind !== 'files' && textField(ev.target)) {
    if (drops.shown) endDropOverlay();
    return;
  }
  if (!kind) { // text: nothing to say, nothing to drop
    if (drops.shown) endDropOverlay();
    ev.preventDefault();
    if (dt) dt.dropEffect = 'none';
    return;
  }
  ev.preventDefault(); // never the browser's own handling: it would open what was dropped
  if (!drops.shown) {
    // A file drag is over the page: if the page is lost now (WebView2 ends it for a file it
    // can't hand over), the page opened again says why (onPageRestored).
    try { sessionStorage.setItem('grab.drag', String(Date.now())); } catch (e) { /* no storage */ }
  }
  const place = kind === 'link' ? { refusal: DROP_FILES, area: 'window' } : dropPlace(ev.target);
  if (dt) dt.dropEffect = place.refusal ? 'none' : 'copy';
  showDropPlace(place);
  clearTimeout(drops.idle);
  drops.idle = setTimeout(() => endDropOverlay(), dropIdleMs);
}

function onDragLeave(ev) {
  // Leaving an element for another inside the page says dragleave too; out of the window (or a
  // cancelled drag, or one let go where no drop is taken) WebView2 says it at 0,0.
  const out = (ev.clientX === 0 && ev.clientY === 0) || ev.clientX < 0 || ev.clientY < 0 ||
    ev.clientX >= innerWidth || ev.clientY >= innerHeight;
  if (out && drops.shown) endDropOverlay(true);
}

function onDrop(ev) {
  const dt = ev.dataTransfer;
  const kind = dragKind(dt);
  if (kind !== 'files' && textField(ev.target)) { // text or a link into a text field: the field's own handling
    endDropOverlay();
    return;
  }
  ev.preventDefault();
  const place = kind === 'link' ? { refusal: DROP_FILES } : dropPlace(ev.target);
  endDropOverlay();
  if (!kind) return;
  // All of it read now: the drop's data is gone once the event returns.
  const items = [...dt.items].filter((i) => i.kind === 'file');
  const files = items.map((i) => i.getAsFile());
  // A link is refused wherever it is let go (not sent to Browse first).
  if (kind === 'files' && (!items.length || linkDrop(dt, files))) {
    flash(DROP_FILES);
    return;
  }
  if (place.refusal) {
    flash(place.refusal);
    return;
  }
  const entries = items.map((i) => (i.webkitGetAsEntry ? i.webkitGetAsEntry() : null));
  // A file WebView2 couldn't get (from inside a .zip whose contents didn't come): a File with no
  // entry behind it. Never posted: WebView2 ends the page for such a File.
  if (files.some((f) => !f) || entries.some((e) => !e)) {
    flash(EXTRACT_FIRST, 12000);
    return;
  }
  const token = ++upload.token;
  upload.pending = { token, server: place.server, dir: place.dir };
  // Until grab answers (onUploadCheck): if WebView2 ends the page over these files after all, the
  // page opened again says why.
  try { sessionStorage.setItem('grab.drag', String(Date.now())); } catch (e) { /* no storage */ }
  let posted = false;
  try {
    // `url`: a URL came with the files (Explorer's .url shortcuts, or a browser's), for the host's
    // refusal of WebView2's own copies.
    const url = [...dt.types].includes('text/uri-list');
    posted = postWithObjects({ type: 'uploadDrop', token, server: place.server, dir: place.dir, url }, files);
  } catch (e) { /* refused by WebView2: said below */ }
  if (!posted) {
    upload.pending = null;
    try { sessionStorage.removeItem('grab.drag'); } catch (e) { /* no storage */ }
    flash('This window can’t take dropped files: use Upload files here… instead.');
  }
}

document.addEventListener('dragenter', onDragOver, true);
document.addEventListener('dragover', onDragOver, true);
document.addEventListener('dragleave', onDragLeave, true);
document.addEventListener('drop', onDrop, true);
// The page gets no keys during a drag from another program (Escape comes as dragleave), but a
// stray overlay goes with Escape too.
document.addEventListener('keydown', (ev) => { if (ev.key === 'Escape' && drops.shown) endDropOverlay(); }, true);

// What the page before this one held, when the window opened it again (onPageRestored): Browse's
// folders (remember) and a file drag over it. sessionStorage outlives the page in the window, never
// grab itself, so a new start finds nothing. Read before anything here writes it: Browse opens
// those folders again at init.
const pageWas = (() => {
  try {
    return { paths: JSON.parse(sessionStorage.getItem('grab.paths') || 'null'), drag: Number(sessionStorage.getItem('grab.drag') || 0) };
  } catch (e) {
    return { paths: null, drag: 0 };
  }
})();
if (pageWas.paths && typeof pageWas.paths === 'object' && !Array.isArray(pageWas.paths)) state.browsePaths = { ...pageWas.paths };

// grab's page was opened again by the window (its renderer ended, or an error page came in its
// place: webview.cpp restore_page). The queue, the settings and everything else the backend holds
// came back with init, and Browse with the folders it had open (pageWas); a search, the selection
// and an open dialog are gone, as the banner says.
function onPageRestored(msg) {
  const how = msg.reason === 'error page' ? 'was replaced by an error page' : 'was ended by WebView2';
  let text = `grab’s window ${how} and was opened again: downloads and uploads went on; a search, the selection and an open dialog are gone.`;
  if (pageWas.drag && Date.now() - pageWas.drag < 60000) text += ` It happened during a drop. ${EXTRACT_FIRST}`;
  pageWas.drag = 0;
  try { sessionStorage.removeItem('grab.drag'); } catch (e) { /* no storage */ }
  showBanner(text);
}

// ---- events ------------------------------------------------------------------------------

$('#search-form').addEventListener('submit', (ev) => {
  ev.preventDefault();
  if (!$('#recent').hidden && state.recentSel >= 0) {
    runRecent(state.recent[state.recentSel]);
    return;
  }
  showRecent(false);
  openFilters(false);
  startSearch();
});

// Filters: the button opens the panel; chips, sizes and presets apply at once.
$('#filters-btn').addEventListener('click', () => openFilters($('#filters').hidden));
$('#filters').addEventListener('click', (ev) => {
  const kind = ev.target.closest('#f-kinds .chip');
  if (kind) {
    const k = kind.dataset.kind;
    if (state.filters.kinds.has(k)) state.filters.kinds.delete(k); else state.filters.kinds.add(k);
    refilter();
    return;
  }
  const preset = ev.target.closest('#f-presets .chip');
  if (preset) {
    const f = state.filters;
    const same = preset.getAttribute('aria-pressed') === 'true';
    f.minText = same ? '' : preset.dataset.min;
    f.maxText = same ? '' : preset.dataset.max;
    f.min = f.minText ? parseSize(f.minText) : null;
    f.max = f.maxText ? parseSize(f.maxText) : null;
    $('#f-min').value = f.minText;
    $('#f-max').value = f.maxText;
    refilter();
    return;
  }
  if (ev.target.closest('#f-clear')) {
    clearFilters();
    $('#f-min').value = '';
    $('#f-max').value = '';
  }
});
$('#f-min').addEventListener('input', () => onSizeInput('min'));
$('#f-max').addEventListener('input', () => onSizeInput('max'));
// Esc in the panel closes it.
$('#filters').addEventListener('keydown', (ev) => {
  if (ev.key === 'Escape') {
    ev.preventDefault();
    ev.stopPropagation();
    openFilters(false);
    $('#filters-btn').focus();
  }
});
$('#filter-strip').addEventListener('click', (ev) => {
  if (ev.target.closest('[data-act="clear-filters"]')) clearFilters();
});
// Outside the panels closes them.
document.addEventListener('mousedown', (ev) => {
  if (!$('#filters').hidden && !ev.target.closest('#filters') && !ev.target.closest('#filters-btn')) openFilters(false);
  if (!$('#recent').hidden && !ev.target.closest('#recent') && ev.target !== $('#query')) showRecent(false);
});

// Recent searches: a click into the empty search box, or ↓ there.
$('#query').addEventListener('mousedown', () => {
  if (!$('#query').value) setTimeout(() => showRecent($('#recent').hidden), 0);
});
$('#query').addEventListener('keydown', (ev) => {
  const box = $('#recent');
  if (ev.key === 'ArrowDown' || ev.key === 'ArrowUp') {
    if (box.hidden) {
      if (ev.key === 'ArrowDown' && !$('#query').value && state.recent.length) {
        ev.preventDefault();
        state.recentSel = 0;
        showRecent(true);
      }
      return;
    }
    ev.preventDefault();
    const n = state.recent.length;
    state.recentSel = ev.key === 'ArrowDown' ? (state.recentSel + 1) % n : (state.recentSel <= 0 ? n - 1 : state.recentSel - 1);
    showRecent(true);
    box.querySelector('.opt.on')?.scrollIntoView({ block: 'nearest' });
  } else if (ev.key === 'Delete' && !box.hidden && state.recentSel >= 0) {
    ev.preventDefault();
    removeRecent(state.recentSel);
  } else if (ev.key === 'Escape' && !box.hidden) {
    ev.preventDefault();
    ev.stopPropagation();
    showRecent(false);
  }
});
$('#recent').addEventListener('mousedown', (ev) => ev.preventDefault()); // the search box keeps the focus
$('#recent').addEventListener('click', (ev) => {
  const del = ev.target.closest('[data-del]');
  if (del) {
    removeRecent(Number(del.dataset.del));
    return;
  }
  if (ev.target.closest('[data-act="clear-recent"]')) {
    state.recent = [];
    send({ type: 'setRecent', items: [] });
    showRecent(false);
    return;
  }
  const opt = ev.target.closest('.opt[data-i]');
  if (opt) runRecent(state.recent[Number(opt.dataset.i)]);
});

// The server picked (also by an address typed for another server); `enter`: Browse opens its
// folder (not when a typed address opens one instead).
function pickRemote(name, enter = true) {
  navNote();
  if (state.browse.under) stopSearch();
  state.browse.under = null;
  state.remote = name;
  $('#remote').value = name;
  state.hits = [];
  state.selected.clear();
  state.resultNote = '';
  state.lastQuery = '';
  state.lists = [];
  renderListStatus();
  showBanner('');
  renderRemoteInfo();
  $('#query').value = '';
  if (enter) enterBrowse();
  renderRows();
  send({ type: 'prefs', remote: state.remote });
}
$('#remote').addEventListener('change', (ev) => pickRemote(ev.target.value));

$('#results').addEventListener('scroll', scheduleRows, { passive: true });
$('#results').addEventListener('blur', endTypeAhead);
window.addEventListener('resize', scheduleRows);

$('#rows').addEventListener('mousedown', (ev) => {
  const i = rowIndex(ev);
  if (i < 0 || ev.button === 3 || ev.button === 4) return; // the side buttons are Back and Forward
  ev.preventDefault();
  if (ev.button === 2) {
    // As in Explorer: a row outside the selection becomes the selection; inside, it stays.
    $('#results').focus();
    if (state.selected.has(i)) setCursor(i, { keep: true });
    else setCursor(i);
    return;
  }
  const go = ev.target.closest('[data-go]');
  if (go && ev.button === 0) { openHitFolder(state.hits[Number(go.dataset.go)]); return; }
  $('#results').focus();
  const onBox = ev.target.matches('input[type=checkbox]');
  // A double-click: every press draws the rows anew, so the browser's own dblclick event never
  // reaches a row; the second press's click count says it instead. A folder opens (Browse), a
  // file downloads.
  if (ev.detail === 2 && ev.button === 0 && !onBox && !ev.shiftKey && !ev.ctrlKey) {
    const h = state.hits[i];
    if (h.dir || h.server) openRow(h);
    else if (!openFile(h)) download([h]);
    return;
  }
  setCursor(i, { extend: ev.shiftKey, toggle: onBox || ev.ctrlKey });
});

$('#select-all').addEventListener('change', (ev) => {
  state.selected.clear();
  if (ev.target.checked) state.hits.forEach((h, i) => { if (!h.server) state.selected.add(i); });
  renderRows();
});

// ---- type-ahead: typing a name's first letters picks it, as in Explorer ------------------

const TYPE_PAUSE = 1000; // ms: after a longer pause the next letter starts over
const typeAhead = { text: '', at: 0, timer: 0 };

// A character typed in the list: the row whose name starts with what has been typed gets picked
// (cursor and selection, as a click), searching on from the cursor and around. A new first letter
// looks past the cursor row; more letters keep it while it still matches; the same letter again
// and again steps through the rows starting with it. Space belongs to a prefix being typed
// ("the m"), and otherwise ticks the checkbox as before. Returns whether the key was taken.
function typeAheadKey(ev) {
  if (ev.ctrlKey || ev.altKey || ev.metaKey || [...ev.key].length !== 1 || ev.key === '/') return false;
  const t = typeAhead;
  const now = performance.now();
  const fresh = !t.text || now - t.at > TYPE_PAUSE;
  if (ev.key === ' ' && fresh) return false;
  t.text = fresh ? ev.key : t.text + ev.key;
  t.at = now;
  const typed = t.text.toLowerCase();
  const cycling = [...typed].every((c) => c === typed[0]);
  const want = cycling ? typed[0] : typed;
  const n = state.hits.length;
  let hit = -1;
  const from = fresh || cycling ? state.cursor + 1 : Math.max(0, state.cursor);
  for (let k = 0; k < n && hit < 0; ++k) {
    const i = (from + k) % n;
    if (state.hits[i].name.toLowerCase().startsWith(want)) hit = i;
  }
  if (hit >= 0) setCursor(hit);
  showTypeHint(hit >= 0 || !n);
  return true;
}

// The hint at the bottom of the list: what was typed, red when no name starts with it. It goes
// with the pause that ends the prefix.
function showTypeHint(found) {
  const hint = $('#type-hint');
  hint.textContent = typeAhead.text;
  hint.classList.toggle('miss', !found);
  hint.hidden = false;
  clearTimeout(typeAhead.timer);
  typeAhead.timer = setTimeout(endTypeAhead, TYPE_PAUSE);
}

function endTypeAhead() {
  clearTimeout(typeAhead.timer);
  typeAhead.text = '';
  $('#type-hint').hidden = true;
}

$('#results').addEventListener('keydown', (ev) => {
  if (typeAheadKey(ev)) {
    ev.preventDefault();
    return;
  }
  // Any other key (but Shift and the like, which come before capitals) ends the prefix.
  if (!['Shift', 'Control', 'Alt', 'Meta', 'CapsLock'].includes(ev.key)) endTypeAhead();
  if (ev.key === 'Backspace' || (ev.key === 'ArrowUp' && ev.altKey)) {
    ev.preventDefault();
    browseUp();
    return;
  }
  if (ev.key === 'Escape' && state.browse.under) {
    ev.preventDefault();
    closeBelow();
    return;
  }
  // Changes on the server: F2, Del, Ctrl+X/C/V, Ctrl+Shift+N, Alt+Enter, Esc (the clipboard).
  if (opsKey(ev)) {
    ev.preventDefault();
    return;
  }
  const page = Math.max(1, Math.floor($('#results').clientHeight / ROW) - 1);
  const move = { ArrowDown: 1, ArrowUp: -1, PageDown: page, PageUp: -page }[ev.key];
  if (move !== undefined) {
    ev.preventDefault();
    setCursor(Math.max(0, state.cursor) + move, { extend: ev.shiftKey, keep: ev.ctrlKey });
  } else if (ev.key === 'Home' || ev.key === 'End') {
    ev.preventDefault();
    setCursor(ev.key === 'Home' ? 0 : state.hits.length - 1, { extend: ev.shiftKey });
  } else if (ev.key === ' ') {
    // Space plays or previews the file under the cursor; Ctrl+Space, or Space on anything
    // without a preview, ticks its checkbox.
    ev.preventDefault();
    if (!ev.ctrlKey && openFile(state.hits[state.cursor])) return;
    if (state.cursor >= 0) setCursor(state.cursor, { toggle: true });
  } else if (ev.key === 'a' && ev.ctrlKey) {
    ev.preventDefault();
    state.hits.forEach((_, i) => state.selected.add(i));
    renderRows();
  } else if ((ev.key === 'F10' && ev.shiftKey) || ev.key === 'ContextMenu') {
    ev.preventDefault();
    // No row under the cursor (an empty folder): the open folder's own menu, as on the list's empty part.
    if (state.cursor < 0 || !state.hits[state.cursor]) {
      const r = $('#results').getBoundingClientRect();
      showMenu(r.left + 40, r.top + 8, folderMenu(), true);
      return;
    }
    const row = document.querySelector(`#rows .row[data-i="${state.cursor}"]`);
    if (!row) return;
    const r = row.getBoundingClientRect();
    showMenu(r.left + 40, r.bottom, resultMenu(), true);
  } else if (ev.key === 'Enter') {
    ev.preventDefault();
    // Enter opens the folder under the cursor, unless several rows are picked.
    const h = state.hits[state.cursor];
    if (h && (h.dir || h.server) && state.selected.size <= 1) openRow(h);
    else download(selectedHits());
  }
});

$('#download').addEventListener('click', () => download(selectedHits()));
$('#choose-dest').addEventListener('click', () => send({ type: 'pickFolder', current: $('#dest').value.trim() }));
$('#dest').addEventListener('change', (ev) => { state.destinations[state.remote] = ev.target.value.trim(); });
$('#clear-finished').addEventListener('click', () => send({ type: 'clearFinished' }));
$('#dl-show').addEventListener('change', (ev) => setDlShow(ev.target.value));
$('#dl-go').addEventListener('click', goToRunning);
$('#pause-all').addEventListener('click', (ev) => send({ type: ev.currentTarget.dataset.act || 'pauseAll' }));
$('#cancel-all').addEventListener('click', cancelAll);
for (const c of Object.values(limitCtl)) {
  c.sw.addEventListener('change', (ev) => {
    state.limits[c.dir].on = ev.target.checked;
    renderLimit();
    sendLimit(c);
    // The box focused with its text selected, so a speed typed replaces the one shown (with the
    // caret at its start, 6 typed over 8 made 68).
    if (state.limits[c.dir].on) {
      c.box.focus();
      c.box.select();
    }
  });
  c.box.addEventListener('input', () => onLimitTyped(c));
  // The window losing focus (Alt+Tab, minimized) blurs the box too but leaves it the active
  // element: the user didn't leave it, so nothing is set; the typed value and its pending look wait.
  c.box.addEventListener('blur', (ev) => {
    if (document.activeElement !== ev.target) commitLimitValue(c, false);
  });
  c.box.addEventListener('keydown', (ev) => {
    if (ev.key === 'Enter') { ev.preventDefault(); commitLimitValue(c, true); ev.target.blur(); }
    else if (ev.key === 'Escape') { cancelLimitEdit(c); ev.target.blur(); }
  });
}
$('#parallel-down').addEventListener('click', () => setParallel(state.parallel - 1));
$('#parallel-up').addEventListener('click', () => setParallel(state.parallel + 1));

$('#empty').addEventListener('click', (ev) => {
  if (ev.target.closest('[data-act="add-server"]')) openSettings(true);
});

// settings
$('#settings-btn').addEventListener('click', () => (settings.open ? closeSettings() : openSettings()));
$('#settings-back').addEventListener('click', closeSettings);
$('#add-server').addEventListener('click', () => openForm(null));
$('#open-config').addEventListener('click', () => send({ type: 'openConfig' }));
$('#server-list').addEventListener('click', (ev) => {
  const btn = ev.target.closest('button[data-act]');
  if (btn && !btn.disabled) onSettingsAction(btn.dataset.act, btn.dataset.name);
});
$('#server-form').addEventListener('submit', (ev) => { ev.preventDefault(); submitForm(); });
$('#form-cancel').addEventListener('click', () => {
  if (settings.busy) send({ type: 'serverCancel' });
  closeForm();
});
$('#settings-cancel').addEventListener('click', () => send({ type: 'serverCancel' }));
$('#f-auth').addEventListener('click', (ev) => {
  const b = ev.target.closest('button[data-auth]');
  if (b) setAuth(b.dataset.auth);
});
$('#f-host').addEventListener('input', setSecretPlaceholder);
$('#f-user').addEventListener('input', setSecretPlaceholder);
$('#f-port').addEventListener('input', setSecretPlaceholder);
$('#f-key').addEventListener('input', setSecretPlaceholder);
$('#f-proxy').addEventListener('click', (ev) => {
  const b = ev.target.closest('button[data-proxy]');
  if (b) setProxyKind(b.dataset.proxy);
});
for (const id of ['#f-proxy-host', '#f-proxy-port', '#f-proxy-user']) $(id).addEventListener('input', setProxySecretPlaceholder);
$('#f-key-browse').addEventListener('click', () => send({ type: 'pickFile', current: $('#f-key').value.trim() }));
$('#modal-ok').addEventListener('click', () => {
  if (modal && modal.stay) modal.onOk(); // it closes once its answer came
  else closeModal(true);
});
$('#modal-cancel').addEventListener('click', () => closeModal(false));
// The upload question's choices and its Change (clashHtml), whichever dialog shows it.
$('#modal-body').addEventListener('change', onClashChange);
$('#modal-body').addEventListener('click', onClashClick);
$('#up-existing-set').addEventListener('change', (ev) => setUploadExisting(ev.target.value));
// grab.conf may have been edited in another program ("Open grab.conf").
window.addEventListener('focus', () => {
  if (settings.open && !settings.form && !modal) refreshServers();
});

document.addEventListener('keydown', (ev) => {
  if (modal) {
    if (ev.key === 'Escape') { ev.preventDefault(); closeModal(false); }
    return;
  }
  if (ev.key === ',' && ev.ctrlKey) {
    ev.preventDefault();
    if (settings.open) closeSettings(); else openSettings();
  } else if (settings.open) {
    if (ev.key === 'Escape') {
      ev.preventDefault();
      if (settings.form) $('#form-cancel').click(); else closeSettings();
    }
  } else if ((ev.ctrlKey && !ev.altKey && (ev.key === 'l' || ev.key === 'L')) ||
    (ev.altKey && !ev.ctrlKey && (ev.key === 'd' || ev.key === 'D')) || (ev.key === 'F4' && !ev.altKey && !ev.ctrlKey)) {
    // To the address bar, as in Explorer.
    ev.preventDefault();
    startEdit();
  } else if (ev.ctrlKey && !ev.altKey && !ev.shiftKey && !ev.metaKey && (ev.key === 'd' || ev.key === 'D')) {
    // Quick access (PINS.md 3.6): pins the open folder, never unpins (as a browser's bookmark key).
    ev.preventDefault();
    if (!viewer.open) pinHere();
  } else if (ev.altKey && !ev.ctrlKey && !ev.shiftKey && !ev.metaKey && pinDigit(ev)) {
    // Alt+1 to Alt+9: the first nine pins as the list shows them. Not AltGr (Ctrl+Alt), which
    // types characters with digits on some layouts.
    ev.preventDefault();
    if (!viewer.open) openNthPin(pinDigit(ev));
  } else if ((ev.altKey && !ev.ctrlKey && !ev.shiftKey && (ev.key === 'ArrowLeft' || ev.key === 'ArrowRight')) ||
    ev.key === 'BrowserBack' || ev.key === 'BrowserForward') {
    ev.preventDefault();
    navGo(ev.key === 'ArrowLeft' || ev.key === 'BrowserBack' ? -1 : 1);
  } else if ((ev.key === 'f' && ev.ctrlKey) || (ev.key === '/' && document.activeElement.tagName !== 'INPUT')) {
    ev.preventDefault();
    $('#query').focus();
    $('#query').select();
  } else if (ev.key === 'Escape' && state.searching) {
    send({ type: 'cancelSearch' });
  }
});

// ---- start -------------------------------------------------------------------------------

if (webview) {
  send({ type: 'init' });
} else {
  showBanner('This page is the grab-gui interface; open it through grab-gui.exe.');
}
renderSearchBox();
renderSortHead();
renderFilters();
renderRows();
