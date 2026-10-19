'use strict';
// grab-gui front end. Talks to the C++ backend (src/gui/app.cpp) with JSON messages:
//   to backend:   init, prefs, search, cancelSearch, refreshLists, browse, suggest, pickFolder, enqueue, cancelItem,
//                 pauseItem, resumeItem, retryItem, pauseAll, resumeAll, cancelAll,
//                 setParallel, setLimit, setSchedule, setVerify, moveItem, setPriority, clearFinished, openFolder, copyText,
//                 pauseChild, resumeChild, skipChild, resolveExisting,
//                 updates: updateCheckNow, setUpdateCheck, updateStart, updateRestart, openReleasePage,
//                 settings: servers, serverSave, serverConfirm, serverCancel, serverTrust,
//                 serverTest, serverRemove, serverDefault, pickFile, openConfig
//   from backend: init, searchResult, searchError, browseResult, suggestions, activity, folderPicked, queue, error, update,
//                 spaceShort, existing,
//                 updateChecked, limit,
//                 settings: servers, settingsBusy, hostKeys, serverSaved, serverTested,
//                 serverError, filePicked

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
  searchId: 0,
  searching: false,
  searchStarted: 0,
  lastQuery: '',
  queue: [],
  parallel: 4,
  limitOn: false,        // the limit that applies now: own setting, speed schedule or a change
  limitMiBps: 5,
  limit: {},             // the backend's "limit" message: source, until, nextMiBps
  schedule: [],          // speed schedule rules, as the backend keeps them
  update: { state: '' }, // the backend's update banner: state, version, page, phase, error, completing
  lists: [],             // the last search's sources per server: age, live, refreshing, error
  updateLater: false,    // "Later": hidden until grab starts again
  // Browse mode: the folder shown, and what it holds. `server` '' is All servers' own top (the
  // servers themselves); `top` a server's top; `under` a search below the folder, while its
  // results show; `typed` the server of an address typed in the path bar, until its answer.
  browse: {
    id: 0, server: '', path: '', top: true, crumbs: [], items: [], severalRoots: false,
    source: '', age: 0, refreshing: false, listError: '', error: '', loading: false,
    under: null, pendingSelect: null, keepSelection: false, typed: null,
  },
  // The path bar as a field (the address bar): `typed` the text typed (the suggestions follow
  // it), `shown` the suggestions ({ label, text, server }), `sel` the highlighted one, `cache`
  // the folder names per "server\nparent" (null while asked), `want` the key last needed,
  // `error` why the last address failed.
  addr: { editing: false, typed: '', shown: [], sel: -1, cache: new Map(), want: '', error: '' },
  // Browse's last folder per server while the window runs (absent: its top); '*' -> the server
  // All servers had open. Not kept across starts: each start opens a server at its top.
  browsePaths: {},
  // Back and Forward: one history for the window (all servers) while it runs. `cur` the place
  // Browse is at or opening; `back` and `fwd` the places before and after it, nearest last. A
  // place: { remote (the server picker's choice), server, path, top, cursorKey, scroll, shown }.
  nav: { back: [], fwd: [], cur: null },
};

// ---- helpers -----------------------------------------------------------------------------

const webview = window.chrome && window.chrome.webview;
function send(msg) { if (webview) webview.postMessage(msg); }

function esc(s) {
  return String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
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
  info.textContent = r.error ? 'misconfigured' : `${r.user}@${r.host} · ${r.method === 'ssh' ? 'ssh + find' : 'rclone listing'}`;
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
  b.textContent = text || '';
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
  const errors = lists.filter((l) => l.error).map((l) => `${state.remote === ALL ? `${l.remote}: ` : ''}${l.error}`);
  el.innerHTML = (refreshing.length ? '<span class="spinner"></span>' : '') + esc(parts.join(' · ')) +
    (errors.length ? `${parts.length ? ' · ' : ''}<span class="bad">list refresh failed: ${esc(errors.join('; '))}</span>` : '');
  el.title = errors.join('\n');
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
  state.hits = sortRows(msg.hits);
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
    html += `<div class="row${sel ? ' sel' : ''}${i === state.cursor ? ' cursor' : ''}${h.dir ? ' folder' : ''}" data-i="${i}">` +
      `<input type="checkbox" tabindex="-1"${sel ? ' checked' : ''}${h.server ? ' disabled' : ''}>` +
      `<span class="name" title="${esc(h.path || h.name)}">${h.dir ? FOLDER_ICON : ''}${esc(h.name)}</span>` +
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
  if (state.searching && u) {
    empty.innerHTML = `<span><span class="spinner"></span>Searching ${u.all ? 'every server' : `${esc(u.name)} and its folders`}` +
      ` for “${esc(state.lastQuery)}”…</span>`;
  } else if (u && u.error) {
    empty.textContent = u.error;
  } else if (u) {
    empty.textContent = `Nothing ${u.all ? 'on any server' : `in ${u.name} or its folders`} matches “${u.query}”.`;
  } else if (b.loading) {
    empty.innerHTML = `<span><span class="spinner"></span>Opening ${esc(browseHere())}…</span>`;
  } else if (b.error) {
    empty.textContent = b.error;
  } else if (!state.remotes.length) {
    empty.innerHTML = '<div>No servers yet. Add the Linux server you want to download from.<br>' +
      '<button type="button" class="primary" data-act="add-server">Add a server</button></div>';
  } else if (filter) {
    empty.textContent = `Nothing in ${browseHere()} matches “${filter}”. Enter searches its folders too.`;
  } else {
    empty.textContent = 'This folder is empty.';
  }
}

function selectedHits() {
  const idx = state.selected.size ? [...state.selected].sort((a, b) => a - b) : (state.cursor >= 0 ? [state.cursor] : []);
  return idx.map((i) => state.hits[i]).filter((h) => h && !h.server); // a server row is not downloadable
}

function renderSummary() {
  const sel = [...state.selected].map((i) => state.hits[i]);
  const bytes = sel.reduce((sum, h) => sum + (h.size || 0), 0);
  const unknown = sel.some((h) => h.size == null); // folders have no size: at least this much
  let text = state.resultNote || '';
  if (sel.length) text += `${text ? ' · ' : ''}${sel.length} selected${bytes ? ` (${fmtSize(bytes)}${unknown ? '+' : ''})` : ''}`;
  $('#summary').textContent = text;
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
    const text = shown.text + (!shown.end && secs >= 2 ? ` · ${secs} s` : '');
    if ($('#act-text').textContent !== text) $('#act-text').textContent = text;
    const others = live.filter((a) => a !== shown);
    $('#act-more').hidden = !others.length;
    $('#act-more').textContent = `+${others.length}`;
    el.title = [shown.detail, ...others.map((a) => `Also: ${a.text}`)].filter(Boolean).join('\n');
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
    send({ type: 'play', remote: h.remote || state.remote, path: h.path });
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

function previewUrl(h) {
  return `https://grab.ui/preview/${encodeURIComponent(h.remote || state.remote)}/${encodeURIComponent(h.path)}`;
}

// The viewer goes through the list's images, text files and PDFs, as they were when it opened.
const viewer = { open: false, list: [], index: -1, token: 0, returnTo: null };

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
  $('#viewer').hidden = true;
  $('#viewer-body').replaceChildren();
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
  $('#viewer-name').textContent = h.name;
  $('#viewer-name').title = h.path;
  $('#viewer-info').textContent = info.join(' · ');
  $('#viewer-pos').textContent = viewer.list.length > 1 ? `${viewer.index + 1} / ${viewer.list.length}` : '';
  $('#viewer-prev').disabled = viewer.index <= 0;
  $('#viewer-next').disabled = viewer.index >= viewer.list.length - 1;
  const body = $('#viewer-body');
  body.className = `viewer-body ${kind}`;
  body.innerHTML = '<span class="spinner"></span>';
  const failed = (text) => {
    if (token !== viewer.token) return;
    const p = document.createElement('div');
    p.className = 'viewer-error';
    p.textContent = text || 'Couldn’t load the preview.';
    body.replaceChildren(p);
  };
  // An error's reason, from the backend's answer.
  const reason = () => fetch(url).then((r) => r.text()).then(failed, () => failed(''));
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
    img.onerror = reason;
    // A click: the full size (scroll to see the rest), and back.
    img.addEventListener('click', () => {
      img.classList.toggle('fit');
      body.classList.toggle('full', !img.classList.contains('fit'));
    });
    img.src = url;
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
    frame.title = h.name;
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
      if (k === 'image') new Image().src = `${previewUrl(n)}?ahead`;
      else if (k === 'text') fetch(`${previewUrl(n)}?ahead`).catch(() => {});
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
  if (!words.length) return state.browse.items;
  return state.browse.items.filter((it) => {
    const name = it.name.toLowerCase();
    return words.every((w) => name.includes(w));
  });
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
  if (b.under) stopSearch();
  b.under = null;
  b.typed = null;
  b.pendingScroll = scroll;
  state.addr.error = '';
  if (state.addr.editing) endEdit(false);
  if (!server) {
    Object.assign(b, { server: '', path: '', top: true, crumbs: [], error: '', loading: false, source: '' });
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
  if (msg.typed) {
    // An address typed in the path bar: a failure leaves the folder on screen as it was.
    if (!b.typed || b.typed.server !== msg.remote) return;
    b.typed = null;
    b.loading = false;
    if (msg.error) {
      state.addr.error = msg.error;
      renderBrowseBar();
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
    severalRoots: !!msg.severalRoots,
  });
  // `ord`: the backend's order (folders first, A to Z), the default one.
  b.items = (msg.items || []).map((it, i) => ({ ...it, remote: msg.remote, ord: i }));
  if (!b.under) {
    showFolder({ keep: msg.update || b.keepSelection, select: b.pendingSelect });
    if (!msg.error && wasLoading) remember();
    navShown();
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
  $('#crumbs').innerHTML = steps.map((s, i) => (s.results
    ? `<button type="button" class="crumb here results" data-edit tabindex="-1">${esc(s.name)}</button>`
    : i === steps.length - 1
    ? `<button type="button" class="crumb here" data-edit tabindex="-1" title="${esc(s.path ?? s.name)}">${esc(s.name)}</button>`
    : `<button type="button" class="crumb" data-step="${i}" title="${esc(s.path ?? s.name)}">${esc(s.name)}</button>`))
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
  // Where the folder's rows came from, in the footer (search results have their own, beside it).
  const status = $('#browse-status');
  if (state.flash && state.flash.until > Date.now()) {
    status.textContent = state.flash.text;
  } else if (a.error && !b.under && !a.editing) {
    status.innerHTML = `<span class="bad" title="${esc(a.error)}">${esc(a.error)}</span>`;
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
// text, all of it selected.
function startEdit() {
  if (settings.open || modal) return;
  const a = state.addr;
  const input = $('#address');
  if (!a.editing) {
    Object.assign(a, { editing: true, typed: '', shown: [], sel: -1, cache: new Map(), want: '', error: '' });
    input.value = currentAddress();
    const example = state.remotes.length ? state.remotes[0].name : 'Main';
    input.placeholder = state.remote === ALL ? `A server and a path, e.g. ${example}:downloads`
      : `Type a path on ${state.remote}`;
    renderBrowseBar();
    renderSuggest();
  }
  input.focus();
  input.select();
}

// The path bar again; `focus`: the results take the keyboard (Esc, a folder opened). An error
// shown goes with the field; one for an address still being opened shows when it comes.
function endEdit(focus = true) {
  const a = state.addr;
  if (!a.editing) return;
  Object.assign(a, { editing: false, shown: [], sel: -1 });
  if (!state.browse.typed) a.error = '';
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
    renderBrowseBar();
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
  b.id += 1;
  b.typed = { server };
  b.loading = true;
  renderBrowseBar();
  send({ type: 'browse', id: b.id, remote: server, path, typed: true });
}

// The suggestions for the text typed: the folders of its parent that start with its last part
// (from the list and this session's live listings, asked once per folder), up to 12; in All
// servers' own top, the servers.
function updateSuggest() {
  const a = state.addr;
  a.sel = -1;
  a.shown = [];
  const text = a.typed;
  const colon = text.indexOf(':');
  const named = colon > 0 ? findRemote(text.slice(0, colon)) : '';
  if (!named && state.remote === ALL && !state.browse.server) {
    const word = text.trim().toLowerCase();
    if (word) {
      a.shown = state.remotes.filter((r) => r.name.toLowerCase().startsWith(word))
        .map((r) => ({ label: r.name, text: `${r.name}:`, server: true }));
    }
    renderSuggest();
    return;
  }
  const server = named || (state.remote === ALL ? state.browse.server : state.remote);
  const rest = (named ? text.slice(colon + 1) : text).trimStart();
  const slash = rest.lastIndexOf('/');
  const parent = rest.slice(0, slash + 1);
  const word = rest.slice(slash + 1).toLowerCase();
  if (!server || (slash < 0 && !word)) {
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
  a.shown = (a.cache.get(key) || []).filter((n) => n.toLowerCase().startsWith(word)).slice(0, 12)
    .map((n) => ({ label: n, text: `${prefix}${parent}${n}` }));
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

function renderSuggest() {
  const a = state.addr;
  const box = $('#suggest');
  const open = a.editing && a.shown.length > 0;
  box.hidden = !open;
  $('#address').setAttribute('aria-expanded', String(open));
  if (!open) {
    box.innerHTML = '';
    return;
  }
  box.innerHTML = a.shown.map((s, i) => `<div class="opt${i === a.sel ? ' on' : ''}" role="option" data-i="${i}"` +
    ` aria-selected="${i === a.sel}">${s.server ? '' : FOLDER_ICON}<span>${esc(s.label)}</span></div>`).join('');
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
  if (a.error) {
    a.error = '';
    renderBrowseBar();
  }
  updateSuggest();
});
$('#address').addEventListener('keydown', (ev) => {
  const a = state.addr;
  const input = $('#address');
  if (ev.key === 'Enter') {
    ev.preventDefault();
    goAddress();
  } else if (ev.key === 'Escape') {
    // The suggestions close first (the text typed comes back), then the field.
    ev.preventDefault();
    ev.stopPropagation();
    if (a.shown.length) {
      if (a.sel >= 0) input.value = a.typed;
      a.shown = [];
      a.sel = -1;
      renderSuggest();
    } else {
      endEdit();
    }
  } else if ((ev.key === 'ArrowDown' || ev.key === 'ArrowUp') && a.shown.length) {
    // Through the suggestions and back to the text typed; the field shows the one highlighted.
    ev.preventDefault();
    const n = a.shown.length;
    if (ev.key === 'ArrowDown') a.sel = a.sel + 1 >= n ? -1 : a.sel + 1;
    else a.sel = a.sel < 0 ? n - 1 : a.sel - 1;
    input.value = a.sel >= 0 ? a.shown[a.sel].text : a.typed;
    renderSuggest();
  } else if (ev.key === 'Tab' && !ev.shiftKey && !ev.ctrlKey && !ev.altKey && a.shown.length) {
    // Completes the highlighted (or first) folder and goes on into it.
    ev.preventDefault();
    const pick = a.shown[Math.max(a.sel, 0)];
    input.value = pick.server ? pick.text : `${pick.text}/`;
    a.typed = input.value;
    updateSuggest();
  }
});
$('#address').addEventListener('blur', () => endEdit(false));
$('#suggest').addEventListener('mousedown', (ev) => ev.preventDefault()); // the field keeps the focus
$('#suggest').addEventListener('click', (ev) => {
  const o = ev.target.closest('[data-i]');
  if (!o) return;
  $('#address').value = state.addr.shown[Number(o.dataset.i)].text;
  goAddress();
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
  if (b.server) browseTo(b.server, b.path, b.top, { live: true });
});
$('#query').addEventListener('input', () => {
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
for (const id of ['#exact', '#search-clear']) $(id).addEventListener('mousedown', (ev) => ev.preventDefault());
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

// items: { label, act, key, disabled, checked } entries, '-' for a separator, { heading }.
// `checked` true or false makes an entry one of a group (a check mark on the current one).
function showMenu(x, y, entries) {
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
  menu.innerHTML = items.map((it, i) => {
    if (it === '-') return '<hr>';
    if (it.heading) return `<div class="heading">${esc(it.heading)}</div>`;
    const radio = it.checked !== undefined;
    return `<button type="button" role="${radio ? 'menuitemradio' : 'menuitem'}" data-i="${i}"` +
      `${radio ? ` aria-checked="${it.checked}"` : ''}${it.disabled ? ' disabled' : ''}>` +
      `<span class="check">${it.checked ? '✓' : ''}</span><span class="text">${esc(it.label)}</span>` +
      `${it.key ? `<span class="key">${esc(it.key)}</span>` : ''}</button>`;
  }).join('');
  menu.hidden = false;
  // Inside the window: to the left of the pointer near the right edge, above it near the bottom.
  const r = menu.getBoundingClientRect();
  menu.style.left = `${Math.max(4, Math.min(x, innerWidth - r.width - 4))}px`;
  menu.style.top = `${Math.max(4, y + r.height > innerHeight - 4 ? y - r.height : y)}px`;
  const first = menu.querySelector('button:not(:disabled)');
  if (first) first.focus();
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
  else if (ev.key === 'ArrowUp') to = (at + n - 1) % n;
  else if (ev.key === 'Home') to = 0;
  else if (ev.key === 'End') to = n - 1;
  if (to >= 0) {
    buttons[to].focus();
  } else if (ev.key === 'Escape' || ev.key === 'Tab' || (ev.key === 'F10' && ev.shiftKey) || ev.key === 'ContextMenu') {
    closeMenu(true);
  } else {
    return; // Enter and Space press the focused entry
  }
  ev.preventDefault();
  ev.stopPropagation(); // not also the window's keys (Escape closes Settings, say)
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
  const kind = kindOf(one);
  if (kind) items.push({ label: plays(kind) ? 'Play' : 'Preview', key: 'Space', act: () => openFile(one) });
  items.push({ label: picks.length > 1 ? `Download ${picks.length}` : 'Download', key: opens ? '' : 'Enter',
    act: () => download(picks) });
  items.push({ label: 'Download to…', act: () => downloadTo(picks) });
  // In a folder the row is already where it lives.
  if (one && state.browse.under) items.push({ label: 'Open folder location', act: () => openHitFolder(one) });
  items.push('-');
  items.push({ label: picks.length > 1 ? `Copy ${picks.length} paths` : 'Copy path', act: () => copyText(picks.map((h) => h.path)) });
  items.push({ label: picks.length > 1 ? `Copy ${picks.length} names` : 'Copy name', act: () => copyText(picks.map((h) => h.name)) });
  return items;
}

$('#rows').addEventListener('contextmenu', (ev) => {
  if (rowIndex(ev) < 0) return;
  ev.preventDefault();
  const items = resultMenu();
  if (items.length) showMenu(ev.clientX, ev.clientY, items);
});

// Where a download lands on this PC: DEST\<name>, and a folder's files below that.
function localPath(q, child) {
  const base = q.local || `${q.dest.replace(/[\\/]+$/, '')}\\${q.name}`;
  if (!child) return base;
  const c = (q.children || []).find((x) => x.path === child);
  return `${base}\\${c && c.local ? c.local : child.replace(/\//g, '\\')}`;
}

// A download: pause, resume or retry, cancel, priority, move to the top, show it, copy its path.
function queueMenu(q) {
  const items = [];
  const active = ['queued', 'running', 'paused'].includes(q.status);
  if (q.status === 'queued' || q.status === 'running') items.push({ label: 'Pause', act: () => send({ type: 'pauseItem', id: q.id }) });
  if (q.status === 'paused') items.push({ label: 'Resume', act: () => resumeItem(q) });
  if (q.status === 'failed' || q.status === 'cancelled') {
    items.push({ label: q.status === 'failed' && q.bytes > 0 ? 'Resume' : 'Retry', act: () => send({ type: 'retryItem', id: q.id }) });
  }
  if (active || q.status === 'failed') items.push({ label: 'Cancel', act: () => send({ type: 'cancelItem', id: q.id }) });
  if (active || q.status === 'failed') {
    items.push('-', { heading: 'Priority' });
    for (const [p, label] of [['high', 'High'], ['normal', 'Normal'], ['low', 'Low']]) {
      items.push({ label, checked: q.priority === p, act: () => send({ type: 'setPriority', id: q.id, priority: p }) });
    }
  }
  if (q.status === 'queued' || q.status === 'paused') {
    const first = state.queue[0] && state.queue[0].id === q.id;
    items.push('-', { label: 'Move to top', disabled: first, act: () => send({ type: 'moveItem', id: q.id, before: state.queue[0].id }) });
  }
  items.push('-');
  items.push({ label: 'Show in folder', act: () => send({ type: 'openFolder', id: q.id }) });
  items.push({ label: 'Copy path', act: () => copyText([localPath(q)]) });
  return items;
}

// A file of a folder download: its own pause, resume and skip, and its path.
function childMenu(q, path) {
  const c = (q.children || []).find((x) => x.path === path);
  const items = [];
  if (c) {
    const act = (type) => () => send({ type, id: q.id, path });
    if (c.status === 'paused') items.push({ label: 'Resume', act: act('resumeChild') });
    else if (c.status !== 'failed') items.push({ label: 'Pause', act: act('pauseChild') });
    items.push({ label: 'Skip', act: act('skipChild') }, '-');
  }
  items.push({ label: 'Copy path', act: () => copyText([localPath(q, path)]) });
  return items;
}

$('#queue').addEventListener('contextmenu', (ev) => {
  const el = ev.target.closest('.q-item');
  if (!el) return;
  const q = state.queue.find((x) => x.id === Number(el.dataset.id));
  if (!q) return;
  ev.preventDefault();
  const child = ev.target.closest('.q-child[data-path]');
  showMenu(ev.clientX, ev.clientY, child ? childMenu(q, child.dataset.path) : queueMenu(q));
});

// ---- downloads ---------------------------------------------------------------------------

// To the Save-to folder, or with `to` (Download to...) to that folder this once.
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
  const busy = new Set(active.map((q) => `${q.remote}|${q.path}|${q.dest}`));
  const targets = new Set(active.map((q) => `${q.dest.toLowerCase()}|${q.name.toLowerCase()}`));
  const clashes = [];
  // Each hit goes to its own server's queue (they differ under All servers).
  const groups = new Map();
  for (const h of hits) {
    if (busy.has(`${h.remote}|${h.path}|${dest}`)) continue;
    const target = `${dest.toLowerCase()}|${h.name.toLowerCase()}`;
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

// Resume; a download waiting with that question asks it first.
function resumeItem(q) {
  if (q.ask) existingDialog(q.ask, (choice) => send({ type: 'resolveExisting', id: q.id, choice }));
  else send({ type: 'resumeItem', id: q.id });
}

const queueEls = new Map(); // id -> element; updated in place so buttons don't flicker
const folderOpen = new Map(); // folder id -> expanded, once the user toggled it

// A folder download's files under its row:
//   |--- 28 done · 2 skipped
//   |--- Lecture 13.mp4   40% ...   [Pause][Skip]
//   |--- …8 more queued
// Open while the folder is active, folded when it is finished, unless the user toggled it.
function renderTree(el, q) {
  const toggle = el.querySelector('.q-toggle');
  const tree = el.querySelector('.q-tree');
  if (!q.files) {
    toggle.hidden = true;
    tree.hidden = true;
    return;
  }
  const open = folderOpen.has(q.id) ? folderOpen.get(q.id) : !['done', 'cancelled'].includes(q.status);
  toggle.hidden = false;
  toggle.textContent = open ? '▾' : '▸';
  toggle.title = open ? 'Hide files' : 'Show files';
  tree.hidden = !open;
  if (!open) return;

  const finished = [];
  if (q.files.done) finished.push(`${q.files.done} done`);
  if (q.files.skipped) finished.push(`${q.files.skipped} skipped`);
  const rows = new Map([...tree.querySelectorAll('.q-child')].map((r) => [r.dataset.key, r]));
  const wanted = [];
  if (finished.length) wanted.push({ key: ':done', text: finished.join(' · ') });
  for (const c of q.children) wanted.push({ key: c.path, child: c });
  if (q.moreQueued) wanted.push({ key: ':more', text: `…${q.moreQueued} more queued` });

  let prev = null;
  for (const w of wanted) {
    let row = rows.get(w.key);
    if (!row) {
      row = document.createElement('div');
      row.dataset.key = w.key;
      row.innerHTML = '<span class="q-branch">|---</span><span class="q-cname"></span>' +
        '<span class="q-cbar"><span></span></span><span class="q-cmeta"></span><span class="q-cactions"></span>';
    }
    rows.delete(w.key);
    // Keep the order of `wanted` without re-creating rows (a row being clicked must survive).
    const at = prev ? prev.nextSibling : tree.firstChild;
    if (row !== at) tree.insertBefore(row, at);
    prev = row;
    if (!w.child) {
      row.className = 'q-child q-summary';
      row.querySelector('.q-cname').textContent = w.text;
      row.querySelector('.q-cmeta').textContent = '';
      row.querySelector('.q-cbar').hidden = true;
      row.querySelector('.q-cactions').innerHTML = '';
      continue;
    }
    const c = w.child;
    row.className = `q-child cst-${c.status}`;
    row.dataset.path = c.path;
    const name = row.querySelector('.q-cname');
    name.textContent = c.path;
    name.title = c.path;
    const bar = row.querySelector('.q-cbar');
    bar.hidden = !(c.status === 'running' || c.status === 'paused' || c.bytes > 0);
    bar.firstChild.style.width = `${c.size ? Math.min(100, (100 * c.bytes) / c.size) : 0}%`;
    let meta;
    if (c.status === 'running') {
      meta = c.size ? `${fmtSize(c.bytes)} of ${fmtSize(c.size)}` : fmtSize(c.bytes);
      if (c.speed > 0) meta += ` · ${fmtSize(c.speed)}/s`;
      const eta = fmtEta(c.eta);
      if (eta) meta += ` · ${eta}`;
    } else if (c.status === 'paused') {
      meta = `Paused · ${fmtSize(c.bytes)} of ${fmtSize(c.size)}`;
    } else if (c.status === 'failed') {
      meta = `Failed: ${c.error || 'error'}`;
    } else {
      meta = `Queued · ${fmtSize(c.size)}`;
    }
    row.querySelector('.q-cmeta').textContent = meta;
    row.querySelector('.q-cmeta').title = c.status === 'failed' ? c.error : '';
    const actions = c.status === 'paused' ? [['cresume', 'Resume'], ['cskip', 'Skip']]
      : c.status === 'failed' ? [['cskip', 'Skip']]
        : [['cpause', 'Pause'], ['cskip', 'Skip']];
    const key = actions.map((a) => a[0]).join(',');
    const box = row.querySelector('.q-cactions');
    if (box.dataset.key !== key) {
      box.dataset.key = key;
      box.innerHTML = actions.map(([act, label]) => `<button type="button" data-act="${act}">${label}</button>`).join('');
    }
  }
  for (const row of rows.values()) row.remove();
}

function renderQueue() {
  const panel = $('#downloads');
  panel.hidden = state.queue.length === 0;
  const box = $('#queue');
  const seen = new Set();
  let prevEl = null;
  for (const q of state.queue) {
    seen.add(q.id);
    let el = queueEls.get(q.id);
    if (!el) {
      el = document.createElement('div');
      el.innerHTML = '<div class="q-top"><span class="q-handle" title="Drag to change the order">≡</span>' +
        '<button type="button" class="q-toggle" data-act="toggle" hidden></button>' +
        '<span class="q-name"></span>' +
        '<select class="q-prio" title="Bandwidth priority: how running downloads share the speed" aria-label="Bandwidth priority">' +
        '<option value="high">High</option><option value="normal">Normal</option><option value="low">Low</option></select>' +
        '<span class="q-meta"></span><span class="q-actions"></span></div>' +
        '<div class="q-bar"><div></div></div><div class="q-sub"></div><div class="q-tree" hidden></div>';
      el.dataset.id = q.id;
      queueEls.set(q.id, el);
    }
    // Rows follow the queue order; moved in place, not re-created (a row may be mid-click).
    const at = prevEl ? prevEl.nextSibling : box.firstChild;
    if (el !== at) box.insertBefore(el, at);
    prevEl = el;
    el.className = `q-item st-${q.status}`;
    const active = ['queued', 'running', 'paused'].includes(q.status);
    const prio = el.querySelector('.q-prio');
    prio.hidden = !active;
    prio.className = `q-prio prio-${q.priority}`;
    if (document.activeElement !== prio) prio.value = q.priority;
    el.querySelector('.q-name').textContent = q.mode === 'folder' ? `${q.name}/` : q.name;
    el.querySelector('.q-name').title = `${q.remote}:${q.path}`;
    const pct = q.total ? Math.min(100, (100 * q.bytes) / q.total) : 0;
    el.querySelector('.q-bar > div').style.width = `${q.status === 'done' || q.verifying ? 100 : pct}%`;
    const sizes = q.total ? `${fmtSize(q.bytes)} of ${fmtSize(q.total)}` : fmtSize(q.bytes);
    // A listed folder counts its files: "12 of 40 files · 2.1 of 5.6 GiB".
    const files = q.files ? `${q.files.done} of ${q.files.total - q.files.skipped} files · ` : '';
    let meta = '';
    if (q.status === 'running' && q.verifying) {
      meta = `${files}Verifying the checksum…`;
    } else if (q.status === 'running') {
      meta = files + sizes;
      if (q.speed > 0) meta += ` · ${fmtSize(q.speed)}/s`;
      const eta = fmtEta(q.eta);
      if (eta) meta += ` · ${eta}`;
      if (q.connections > 0) meta += ` · ${q.connections} connection${q.connections === 1 ? '' : 's'}`;
    } else if (q.status === 'queued') {
      meta = `Queued · #${q.position}${q.bytes > 0 ? ` · ${files}${sizes}` : ''}`;
    } else if (q.status === 'paused') {
      meta = `Paused · ${files}${sizes}`;
    } else if (q.status === 'done' && q.files) {
      meta = `Done · ${q.files.done} files · ${fmtSize(q.total)}${q.files.skipped ? ` · ${q.files.skipped} skipped` : ''}`;
    } else {
      meta = { queued: 'Queued', done: q.total ? `Done · ${fmtSize(q.total)}` : 'Done', failed: 'Failed', cancelled: 'Cancelled' }[q.status];
    }
    el.querySelector('.q-meta').textContent = meta;
    // Waiting rows can be moved in the queue: up, down, to the top.
    const move = [['up', '↑', 'Move up'], ['down', '↓', 'Move down'], ['top', '⤒', 'Move to the top']];
    const actions = {
      queued: [...move, ['pause', 'Pause'], ['cancel', 'Cancel']],
      running: [['pause', 'Pause'], ['cancel', 'Cancel']],
      paused: [...move, ['resume', 'Resume'], ['cancel', 'Cancel']],
      done: [['open', 'Show in folder']],
      failed: [['retry', q.bytes > 0 ? 'Resume' : 'Retry'], ['cancel', 'Cancel']],
      cancelled: [['retry', 'Retry']],
    }[q.status];
    const actionKey = actions.map((a) => a.join(':')).join(',');
    const actionBox = el.querySelector('.q-actions');
    if (actionBox.dataset.key !== actionKey) {
      actionBox.dataset.key = actionKey;
      actionBox.innerHTML = actions.map(([act, label, title]) =>
        `<button type="button" data-act="${act}"${title ? ` class="q-move" title="${title}" aria-label="${title}"` : ''}>${label}</button>`).join('');
    }
    const sub = el.querySelector('.q-sub');
    // A paused download's reason (the free-space check) comes first; the whole line shows on hover.
    sub.textContent = q.status === 'failed' && q.error ? q.error
      : q.note && q.status === 'paused' ? `${q.note} · → ${q.dest}`
        : q.note ? `→ ${q.dest} · ${q.note}` : `→ ${q.dest}`;
    sub.title = sub.textContent;
    renderTree(el, q);
  }
  for (const [id, el] of queueEls) {
    if (!seen.has(id)) { el.remove(); queueEls.delete(id); folderOpen.delete(id); }
  }
  const count = (st) => state.queue.filter((q) => q.status === st).length;
  const running = count('running');
  const queued = count('queued');
  const paused = count('paused');
  const parts = [];
  if (running) parts.push(`${running} running`);
  if (queued) parts.push(`${queued} queued`);
  if (paused) parts.push(`${paused} paused`);
  $('#dl-summary').textContent = parts.join(' · ');
  // Pause all while anything runs or waits; otherwise Resume all for paused ones.
  const pauseAll = $('#pause-all');
  const resuming = running + queued === 0 && paused > 0;
  pauseAll.textContent = resuming ? 'Resume all' : 'Pause all';
  pauseAll.dataset.act = resuming ? 'resumeAll' : 'pauseAll';
  pauseAll.disabled = running + queued + paused === 0;
  $('#cancel-all').disabled = running + queued + paused + count('failed') === 0;
  $('#clear-finished').disabled = !state.queue.some((q) => ['done', 'failed', 'cancelled'].includes(q.status));
  renderParallel();
  renderTotalSpeed();
}

// ---- status bar: total speed and the speed limit -------------------------------------------

function renderTotalSpeed() {
  const running = state.queue.filter((q) => q.status === 'running');
  const el = $('#total-speed');
  if (!running.length) {
    el.textContent = 'No downloads running';
    el.classList.remove('limited');
    return;
  }
  const speed = running.reduce((sum, q) => sum + (q.speed || 0), 0);
  el.textContent = `↓ ${fmtSize(speed)}/s total · ${running.length} running${state.limitOn ? ' (limited)' : ''}`;
  el.classList.toggle('limited', state.limitOn);
}

// The speed schedule's part in the status bar: why the limit is what it is, and until when.
function renderLimitHint() {
  const l = state.limit;
  const hint = $('#limit-hint');
  const until = l.until ? ` until ${l.until}` : '';
  if (l.source === 'schedule') hint.textContent = `Scheduled${until}`;
  else if (l.source === 'change') hint.textContent = l.until ? `Schedule resumes at ${l.until}` : 'Schedule paused';
  hint.hidden = l.source !== 'schedule' && l.source !== 'change';
}

function fmtLimit(v) {
  return String(Math.round(v * 10) / 10);
}

function renderLimit() {
  $('#limit-on').checked = state.limitOn;
  const box = $('#limit-value');
  box.disabled = !state.limitOn;
  if (document.activeElement !== box) box.value = fmtLimit(state.limitMiBps);
  renderLimitHint();
  renderTotalSpeed();
}

function onLimit(msg) {
  state.limitOn = msg.on;
  state.limitMiBps = msg.mibps;
  state.limit = msg;
  // Re-rendered only when they differ (by field: WebView2 hands over objects with sorted keys),
  // so the echo of an edit doesn't take the focus from the next field.
  const rules = msg.rules || [];
  const key = (list) => JSON.stringify(list.map((r) => [r.days, r.from, r.to, r.mibps]));
  if (key(rules) !== key(state.schedule)) {
    state.schedule = rules;
    renderSchedule();
  }
  renderLimit();
  renderScheduleNow();
}

function sendLimit() {
  send({ type: 'setLimit', on: state.limitOn, mibps: state.limitMiBps });
}

// A new value from the box; anything that isn't a number from 0.1 to 10000 snaps back.
function commitLimitValue() {
  const box = $('#limit-value');
  const v = Number(box.value.trim().replace(',', '.'));
  if (Number.isFinite(v) && v >= 0.1 && v <= 10000) {
    const changed = Math.abs(v - state.limitMiBps) > 1e-9;
    state.limitMiBps = v;
    if (changed) sendLimit();
  }
  box.value = fmtLimit(state.limitMiBps);
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
  showModal({
    title: 'Cancel all downloads?',
    html: '<p>Every running, queued and paused download stops, and their partly downloaded files are deleted. ' +
      'Finished downloads are kept.</p>',
    ok: 'Cancel all',
    danger: true,
    cancel: 'Keep downloading',
    onOk: () => send({ type: 'cancelAll' }),
  });
}

// ---- messages from the backend -----------------------------------------------------------

function onInit(msg) {
  // Sent again after every server change; folders typed here but not used yet win.
  state.destinations = { ...msg.destinations, ...state.destinations };
  state.remotes = msg.remotes;
  state.remote = msg.remote;
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
  renderRemotes();
  renderSortHead();
  // Sent again after a server change: browse goes on where it is, if that server still exists.
  if (!state.browse.inited || (state.browse.server && !state.remotes.some((r) => r.name === state.browse.server))) {
    enterBrowse();
  }
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
    b.innerHTML = `<span class="text"><span class="spinner"></span>Updating to ${v}: ${UPDATE_PHASE[u.phase] || 'starting…'}</span>`;
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
  else if (act === 'dismiss') { state.update = { state: '' }; renderUpdate(); }
});

$('#update-auto').addEventListener('change', (ev) => send({ type: 'setUpdateCheck', on: ev.target.checked }));
$('#verify-on').addEventListener('change', (ev) => send({ type: 'setVerify', on: ev.target.checked }));

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
  r.textContent = msg.text;
  if (msg.available) { state.updateLater = false; renderUpdate(); }
}

// ---- settings: speed schedule -------------------------------------------------------------

const DAYS = ['mon', 'tue', 'wed', 'thu', 'fri', 'sat', 'sun'];
const DAY_LABELS = ['Mo', 'Tu', 'We', 'Th', 'Fr', 'Sa', 'Su'];
const DAY_NAMES = ['Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday', 'Sunday'];
const MAX_RULES = 20; // schedule::max_rules

function fmtSpeed(mibps) { return `${fmtLimit(mibps)} MiB/s`; }

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

function ruleRow(r, i) {
  const days = DAYS.map((d, k) =>
    `<button type="button" data-day="${d}" aria-pressed="${r.days.includes(d)}" title="${DAY_NAMES[k]}">${DAY_LABELS[k]}</button>`).join('');
  const nextDay = r.to !== '00:00' && r.to <= r.from;
  return `<div class="rule" data-i="${i}">` +
    `<span class="segmented days" role="group" aria-label="Days">${days}</span>` +
    `<input type="text" class="time" data-field="from" value="${r.from}" aria-label="From" spellcheck="false">` +
    '<span class="muted">–</span>' +
    `<input type="text" class="time" data-field="to" value="${toText(r.to)}" aria-label="To" spellcheck="false">` +
    `<span class="next-day muted${nextDay ? '' : ' off'}">next day</span>` +
    `<input type="text" class="num" data-field="mibps" value="${fmtLimit(r.mibps)}" inputmode="decimal" aria-label="Speed in MiB/s">` +
    '<span class="muted">MiB/s</span>' +
    '<button type="button" class="subtle icon-btn" data-act="remove" title="Remove this time" aria-label="Remove this time">✕</button>' +
    '</div>';
}

function renderSchedule() {
  $('#schedule-rules').innerHTML = state.schedule.map(ruleRow).join('');
  $('#schedule-add').disabled = state.schedule.length >= MAX_RULES;
}

function renderScheduleNow() {
  const l = state.limit;
  const el = $('#schedule-now');
  const until = l.until ? ` until ${l.until}` : '';
  if (!state.schedule.length) el.textContent = '';
  else if (l.source === 'schedule') el.textContent = `Now: ${fmtSpeed(l.mibps)}${l.until ? until : ', all week'}`;
  else if (l.source === 'change') el.textContent = `Now: ${l.on ? fmtSpeed(l.mibps) : 'no limit'} (your change)${until}`;
  else if (l.until && l.nextMiBps) el.textContent = `No scheduled limit now; next: ${l.until} at ${fmtSpeed(l.nextMiBps)}`;
  else el.textContent = 'No scheduled limit now';
}

function sendSchedule() {
  send({ type: 'setSchedule', rules: state.schedule });
}

// A field of a rule was edited: a valid value is kept and sent, anything else snaps back.
function commitRuleField(input) {
  const i = Number(input.closest('.rule').dataset.i);
  const r = state.schedule[i];
  if (!r) return;
  const field = input.dataset.field;
  let changed = false;
  if (field === 'mibps') {
    const v = Number(input.value.trim().replace(',', '.'));
    if (Number.isFinite(v) && v >= 0.1 && v <= 10000) {
      changed = Math.abs(v - r.mibps) > 1e-9;
      r.mibps = v;
    }
    input.value = fmtLimit(r.mibps);
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
    input.value = f === 'mibps' ? fmtLimit(r.mibps) : f === 'to' ? toText(r.to) : r.from;
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
  form: null,     // null, or { editing, name, origAuth } while the add/edit form is shown
  auth: 'password',
  tests: {},      // server name -> serverTested message, or { pending: true }
  busy: false,
  status: '',
};

const AUTH_TEXT = { password: 'password', key: 'key file', agent: 'ssh-agent' };

function showSettingsBanner(text) {
  const b = $('#settings-banner');
  b.hidden = !text;
  b.textContent = text || '';
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

function testLine(t) {
  if (!t) return '';
  if (t.pending) return '<span class="muted"><span class="spinner"></span>Testing the connection…</span>';
  if (t.ok) return '<span class="good">✓ Connection OK</span>';
  const parts = [];
  if (t.rcloneOk) parts.push('<span class="good">✓ Downloads (rclone)</span>');
  else parts.push(`<span class="bad">✗ Downloads (rclone):</span> <span class="detail">${esc(t.rcloneError || 'failed')}</span>`);
  if (t.sshOk === true) parts.push('<span class="good">✓ Searches (ssh)</span>');
  else if (t.sshOk === false) parts.push(`<span class="bad">✗ Searches (ssh):</span> <span class="detail">${esc(t.sshError || 'failed')}</span>`);
  return parts.map((p) => `<span>${p}</span>`).join('');
}

function serverCard(s) {
  const btn = (act, label, cls = '') =>
    `<button type="button" class="${cls}" data-act="${act}" data-name="${esc(s.name)}"${settings.busy ? ' disabled' : ''}>${label}</button>`;
  const actions = [];
  if (!s.error) {
    actions.push(btn('test', 'Test'));
    if (!s.pinned) actions.push(btn('trust', 'Trust…'));
    actions.push(btn('edit', 'Edit'));
    if (!s.isDefault) actions.push(btn('default', 'Make default'));
  }
  actions.push(btn('remove', 'Remove', 'danger'));

  const where = s.roots.length ? s.roots.join(', ') : '~';
  const meta = s.error
    ? `<span class="bad">${esc(s.error)}</span>`
    : `${esc(s.user)}@${esc(s.host)}:${s.port} · ${AUTH_TEXT[s.auth]} · searched with ${s.search === 'ssh' ? 'ssh + find' : 'rclone listing'} in ${esc(where)}, depth ${s.depth}` +
      ` · up to ${s.maxConnections || s.autoConnections} download connections${s.maxConnections ? '' : ' (automatic)'}`;
  const status = [];
  if (!s.error) {
    status.push(s.pinned ? '<span class="good">✓ Host key pinned</span>'
      : '<span class="warn">⚠ Host key not checked: any server answering at this address is accepted. Use Trust… to pin it.</span>');
  }
  const test = testLine(settings.tests[s.name]);
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
  $('#server-form').hidden = !settings.form;
  renderStatus();
  if (settings.form) return;
  list.innerHTML = settings.servers.length ? settings.servers.map(serverCard).join('')
    : '<div class="settings-empty">No servers yet. Add the Linux server you want to download from: ' +
      'grab connects over SFTP/SSH with a password, a key file or ssh-agent.<br>' +
      '<button type="button" class="primary" data-act="add">Add a server</button></div>';
}

function setAuth(auth) {
  settings.auth = auth;
  for (const b of $('#f-auth').querySelectorAll('button')) b.setAttribute('aria-checked', String(b.dataset.auth === auth));
  for (const el of document.querySelectorAll('[data-auth-only]')) el.hidden = !el.dataset.authOnly.split(' ').includes(auth);
  const keep = settings.form && settings.form.editing && settings.form.origAuth === auth;
  $('#secret-label').textContent = auth === 'key' ? 'Passphrase' : 'Password';
  $('#f-secret').placeholder = keep ? (auth === 'key' ? 'Leave blank to keep the saved passphrase' : 'Leave blank to keep the saved password')
    : (auth === 'key' ? 'Only if the key has one' : '');
  $('#secret-hint').textContent = auth === 'key'
    ? 'Searches use ssh, which cannot ask for a passphrase here: load such a key into ssh-agent (ssh-add) and choose ssh-agent instead.'
    : 'Stored obscured in grab\'s rclone.conf, never in plain text on a command line.';
  $('#auth-hint').textContent = {
    password: 'Works without a shell (e.g. storage boxes); searches use rclone\'s listing.',
    key: 'Searches use ssh + find on the server, which is fastest.',
    agent: 'Uses the keys loaded in the Windows ssh-agent (ssh-add); searches use ssh + find.',
  }[auth];
}

function clearFieldErrors() {
  for (const el of document.querySelectorAll('.field-error')) el.textContent = '';
}

function openForm(name) {
  const s = name ? settings.servers.find((x) => x.name === name) : null;
  settings.form = { editing: !!s, name: s ? s.name : '', origAuth: s ? s.auth : null };
  $('#form-title').textContent = s ? `Edit ${s.name}` : 'Add a server';
  $('#f-name').value = s ? s.name : '';
  $('#f-name').disabled = !!s;
  $('#f-host').value = s ? s.host : '';
  $('#f-port').value = s ? s.port : 22;
  $('#f-user').value = s ? s.user : '';
  $('#f-key').value = s ? s.keyFile : '';
  $('#f-secret').value = '';
  $('#f-roots').value = s ? s.roots.join('\n') : '';
  $('#f-depth').value = s ? s.depth : 4;
  $('#f-conns').value = s && s.maxConnections ? s.maxConnections : '';
  $('#f-conns').placeholder = s ? String(s.autoConnections) : '';
  $('#conns-hint').textContent = s ? `blank = automatic (now ${s.autoConnections})` : 'blank = automatic';
  setAuth(s ? s.auth : 'password');
  clearFieldErrors();
  showSettingsBanner('');
  renderServers();
  (s ? $('#f-host') : $('#f-name')).focus();
}

function closeForm() {
  settings.form = null;
  $('#f-secret').value = '';
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
  send({
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
  });
  $('#f-secret').value = ''; // the backend holds it until the host key is trusted
  setStatus('Checking the host key…');
}

// ---- modal -------------------------------------------------------------------------------

let modal = null; // { onOk, onCancel }

function showModal({ title, html, ok, cancel = 'Cancel', danger = false, onOk, onCancel, extra = [] }) {
  modal = { onOk, onCancel };
  $('#modal-title').textContent = title;
  $('#modal-body').innerHTML = html;
  $('#modal-cancel').textContent = cancel;
  const okBtn = $('#modal-ok');
  okBtn.textContent = ok;
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
      x.act();
    });
    okBtn.before(b);
  }
  $('#modal').hidden = false;
  $('#modal-cancel').focus(); // the safe choice is the default
}

function closeModal(accepted) {
  if (!modal) return;
  const m = modal;
  modal = null;
  $('#modal').hidden = true;
  if (accepted) m.onOk && m.onOk(); else m.onCancel && m.onCancel();
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
    target.textContent = msg.message;
    const input = { name: '#f-name', host: '#f-host', port: '#f-port', user: '#f-user', keyFile: '#f-key',
      secret: '#f-secret', depth: '#f-depth', connections: '#f-conns' }[msg.field];
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
    showModal({
      title: `Remove ${name}?`,
      html: '<p>grab forgets this server: its section in grab.conf and its connection in grab\'s ' +
        'rclone.conf are deleted. Files you already downloaded stay where they are.</p>',
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
      case 'activity': onActivity(msg); break;
      case 'suggestions': onSuggestions(msg); break;
      case 'folderPicked': onFolderPicked(msg); break;
      case 'queue':
        state.queue = msg.items;
        if (msg.parallel) state.parallel = msg.parallel;
        renderQueue();
        if (state.update.state === 'installed') renderUpdate();
        break;
      case 'update': state.update = msg; renderUpdate(); break;
      case 'updateChecked': onUpdateChecked(msg); break;
      case 'limit': onLimit(msg); break;
      case 'error':
        if (state.flash) state.flash.until = 0; // "Starting …" is over
        showBanner(msg.message);
        break;
      case 'played': flash(`Playing ${msg.name} in ${msg.program}`); break;
      case 'servers': settings.servers = msg.items; renderServers(); break;
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
      case 'filePicked': $('#f-key').value = msg.path; break;
    }
  });
}

// ---- events ------------------------------------------------------------------------------

$('#search-form').addEventListener('submit', (ev) => { ev.preventDefault(); startSearch(); });

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
    const row = document.querySelector(`#rows .row[data-i="${state.cursor}"]`);
    if (!row) return;
    const r = row.getBoundingClientRect();
    showMenu(r.left + 40, r.bottom, resultMenu());
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
$('#pause-all').addEventListener('click', (ev) => send({ type: ev.currentTarget.dataset.act || 'pauseAll' }));
$('#cancel-all').addEventListener('click', cancelAll);
$('#limit-on').addEventListener('change', (ev) => {
  state.limitOn = ev.target.checked;
  renderLimit();
  sendLimit();
  if (state.limitOn) $('#limit-value').focus();
});
$('#limit-value').addEventListener('change', commitLimitValue);
$('#limit-value').addEventListener('keydown', (ev) => {
  if (ev.key === 'Enter') { ev.preventDefault(); commitLimitValue(); ev.target.blur(); }
  else if (ev.key === 'Escape') { ev.target.value = fmtLimit(state.limitMiBps); ev.target.blur(); }
});
$('#parallel-down').addEventListener('click', () => setParallel(state.parallel - 1));
$('#parallel-up').addEventListener('click', () => setParallel(state.parallel + 1));

$('#queue').addEventListener('click', (ev) => {
  const btn = ev.target.closest('button[data-act]');
  if (!btn) return;
  const id = Number(btn.closest('.q-item').dataset.id);
  const act = btn.dataset.act;
  if (act === 'toggle') {
    folderOpen.set(id, btn.textContent !== '▾');
    renderQueue();
    return;
  }
  const child = btn.closest('.q-child');
  if (child) {
    const type = { cpause: 'pauseChild', cresume: 'resumeChild', cskip: 'skipChild' }[act];
    send({ type, id, path: child.dataset.path });
    return;
  }
  if (act === 'up' || act === 'down' || act === 'top') {
    const idx = state.queue.findIndex((q) => q.id === id);
    const ids = state.queue.map((q) => q.id);
    const before = act === 'top' ? ids[0]
      : act === 'up' ? ids[idx - 1]
        : (idx + 2 < ids.length ? ids[idx + 2] : 0); // down: before the one after next, or the end
    if (before !== undefined && before !== id && !(act === 'down' && idx === ids.length - 1)) {
      send({ type: 'moveItem', id, before });
    }
    return;
  }
  if (act === 'resume') {
    const q = state.queue.find((x) => x.id === id);
    if (q) resumeItem(q);
    return;
  }
  const type = { cancel: 'cancelItem', retry: 'retryItem', open: 'openFolder', pause: 'pauseItem' }[act];
  send({ type, id });
});

$('#queue').addEventListener('change', (ev) => {
  if (!ev.target.matches('.q-prio')) return;
  const id = Number(ev.target.closest('.q-item').dataset.id);
  send({ type: 'setPriority', id, priority: ev.target.value });
});

// Drag a row by its ≡ handle to change the queue order.
let dragId = 0;
function clearDropMarks() {
  for (const el of document.querySelectorAll('.drop-before, .drop-after')) el.classList.remove('drop-before', 'drop-after');
}
$('#queue').addEventListener('mousedown', (ev) => {
  const handle = ev.target.closest('.q-handle');
  if (handle) handle.closest('.q-item').draggable = true;
});
$('#queue').addEventListener('dragstart', (ev) => {
  const item = ev.target.closest('.q-item');
  if (!item || !item.draggable) return;
  dragId = Number(item.dataset.id);
  item.classList.add('dragging');
  ev.dataTransfer.effectAllowed = 'move';
  ev.dataTransfer.setData('text/plain', String(dragId));
});
$('#queue').addEventListener('dragover', (ev) => {
  const item = ev.target.closest('.q-item');
  if (!dragId || !item) return;
  ev.preventDefault();
  clearDropMarks();
  const r = item.getBoundingClientRect();
  item.classList.add(ev.clientY < r.top + r.height / 2 ? 'drop-before' : 'drop-after');
});
$('#queue').addEventListener('drop', (ev) => {
  const item = ev.target.closest('.q-item');
  if (!dragId || !item) return;
  ev.preventDefault();
  const r = item.getBoundingClientRect();
  let before = Number(item.dataset.id);
  if (ev.clientY >= r.top + r.height / 2) {
    const next = item.nextElementSibling;
    before = next ? Number(next.dataset.id) : 0;
  }
  if (before !== dragId) send({ type: 'moveItem', id: dragId, before });
  clearDropMarks();
});
$('#queue').addEventListener('dragend', (ev) => {
  const item = ev.target.closest('.q-item');
  if (item) { item.draggable = false; item.classList.remove('dragging'); }
  dragId = 0;
  clearDropMarks();
});

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
$('#f-key-browse').addEventListener('click', () => send({ type: 'pickFile', current: $('#f-key').value.trim() }));
$('#modal-ok').addEventListener('click', () => closeModal(true));
$('#modal-cancel').addEventListener('click', () => closeModal(false));
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
renderRows();
