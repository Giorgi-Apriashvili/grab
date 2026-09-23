'use strict';
// grab-gui front end. Talks to the C++ backend (src/gui/app.cpp) with JSON messages:
//   to backend:   init, prefs, search, cancelSearch, pickFolder, enqueue, cancelItem,
//                 retryItem, clearFinished, openFolder
//   from backend: init, searchResult, searchError, folderPicked, queue, error

const $ = (sel) => document.querySelector(sel);
const ROW = 32; // keep in sync with --row in app.css

const state = {
  remotes: [],
  remote: '',
  mode: 'file',
  destinations: {},
  defaultDest: '',
  hits: [],
  selected: new Set(),
  cursor: -1,
  anchor: -1,
  searchId: 0,
  searching: false,
  searchStarted: 0,
  lastQuery: '',
  queue: [],
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

// ---- header: remote, mode ----------------------------------------------------------------

function renderRemotes() {
  const sel = $('#remote');
  sel.innerHTML = state.remotes.map((r) =>
    `<option value="${esc(r.name)}"${r.name === state.remote ? ' selected' : ''}>${esc(r.name)}</option>`).join('');
  renderRemoteInfo();
}

function renderRemoteInfo() {
  const info = $('#remote-info');
  const r = currentRemote();
  if (!r) { info.hidden = true; return; }
  info.hidden = false;
  info.classList.toggle('error', !!r.error);
  info.textContent = r.error ? 'misconfigured' : `${r.user}@${r.host} · ${r.method === 'ssh' ? 'ssh + find' : 'rclone listing'}`;
  info.title = r.error || '';
  $('#dest').value = state.destinations[state.remote] || state.defaultDest;
}

function renderMode() {
  for (const b of document.querySelectorAll('.segmented button')) {
    b.setAttribute('aria-checked', String(b.dataset.mode === state.mode));
  }
}

// ---- banner and search -------------------------------------------------------------------

function showBanner(text) {
  const b = $('#banner');
  b.hidden = !text;
  b.textContent = text || '';
}

function setSearching(on) {
  state.searching = on;
  const btn = $('#search-btn');
  btn.textContent = on ? 'Cancel' : 'Search';
  btn.classList.toggle('primary', !on);
  renderEmpty();
}

function startSearch() {
  if (state.searching) { send({ type: 'cancelSearch' }); return; }
  const query = $('#query').value.trim();
  if (!query) { $('#query').focus(); return; }
  showBanner('');
  state.searchId += 1;
  state.lastQuery = query;
  state.searchStarted = performance.now();
  state.hits = [];
  state.selected.clear();
  state.cursor = state.anchor = -1;
  setSearching(true);
  renderRows();
  send({ type: 'search', id: state.searchId, remote: state.remote, query, mode: state.mode, exact: $('#exact').checked });
}

function onSearchResult(msg) {
  if (msg.id !== state.searchId) return;
  state.hits = msg.hits;
  state.cursor = state.hits.length ? 0 : -1;
  state.anchor = state.cursor;
  setSearching(false);
  $('#results').scrollTop = 0;
  renderRows();
  const secs = (msg.elapsedMs / 1000).toFixed(1);
  state.resultNote = `${msg.hits.length} ${state.mode === 'folder' ? 'folder' : 'file'}${msg.hits.length === 1 ? '' : 's'} in ${secs} s`;
  state.searchedWhere = `${msg.roots.join(', ')}, depth ${msg.maxDepth}`;
  renderSummary();
  if (state.hits.length) $('#results').focus();
}

function onSearchError(msg) {
  if (msg.id !== state.searchId) return;
  setSearching(false);
  if (!msg.cancelled) showBanner(msg.message);
  state.resultNote = msg.cancelled ? 'Search cancelled' : '';
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
  for (let i = first; i < last; ++i) {
    const h = state.hits[i];
    const sel = state.selected.has(i);
    html += `<div class="row${sel ? ' sel' : ''}${i === state.cursor ? ' cursor' : ''}" data-i="${i}">` +
      `<input type="checkbox" tabindex="-1"${sel ? ' checked' : ''}>` +
      `<span class="name" title="${esc(h.path)}">${esc(h.name)}</span>` +
      `<span class="size">${fmtSize(h.size)}</span>` +
      `<span class="dir">${esc(parentOf(h.path))}</span></div>`;
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
  const r = currentRemote();
  if (state.searching) {
    empty.innerHTML = `<span><span class="spinner"></span>Searching ${esc(state.remote)} for “${esc(state.lastQuery)}”…</span>`;
  } else if (state.lastQuery && state.resultNote && !state.resultNote.startsWith('Search cancelled')) {
    empty.textContent = `No ${state.mode === 'folder' ? 'folders' : 'files'} matching “${state.lastQuery}” (searched ${state.searchedWhere}).`;
  } else if (!state.remotes.length) {
    empty.textContent = 'No servers configured. Run “grab --init” or “grab config” in a terminal.';
  } else {
    empty.textContent = r && r.error ? r.error : 'Search a server by name. Every word must appear, in any order.';
  }
}

function selectedHits() {
  const idx = state.selected.size ? [...state.selected].sort((a, b) => a - b) : (state.cursor >= 0 ? [state.cursor] : []);
  return idx.map((i) => state.hits[i]);
}

function renderSummary() {
  const sel = [...state.selected].map((i) => state.hits[i]);
  const bytes = sel.reduce((sum, h) => sum + (h.size || 0), 0);
  let text = state.resultNote || '';
  if (sel.length) text += `${text ? ' · ' : ''}${sel.length} selected${bytes ? ` (${fmtSize(bytes)})` : ''}`;
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
  // Keep the cursor row in view.
  const list = $('#results');
  const top = i * ROW;
  if (top < list.scrollTop) list.scrollTop = top;
  else if (top + ROW > list.scrollTop + list.clientHeight) list.scrollTop = top + ROW - list.clientHeight;
  renderRows();
}

function rowIndex(ev) {
  const row = ev.target.closest('.row');
  return row ? Number(row.dataset.i) : -1;
}

// ---- downloads ---------------------------------------------------------------------------

function download(hits) {
  if (!hits.length) return;
  const dest = $('#dest').value.trim();
  if (!/^[a-zA-Z]:[\\/]|^\\\\[^\\]/.test(dest)) {
    showBanner(dest ? `“${dest}” is not a full folder path. Use something like C:\\Users\\you\\Downloads, or pick one with Browse.`
                    : 'Choose a folder to save to.');
    $('#dest').focus();
    return;
  }
  showBanner('');
  // Skip anything already waiting or running for the same place.
  const busy = new Set(state.queue.filter((q) => q.status === 'queued' || q.status === 'running')
    .map((q) => `${q.remote}|${q.path}|${q.dest}`));
  const items = hits.filter((h) => !busy.has(`${state.remote}|${h.path}|${dest}`))
    .map((h) => ({ path: h.path, name: h.name, size: h.size }));
  if (!items.length) return;
  state.destinations[state.remote] = dest;
  send({ type: 'enqueue', remote: state.remote, mode: state.mode, dest, items });
}

const queueEls = new Map(); // id -> element; updated in place so buttons don't flicker

function renderQueue() {
  const panel = $('#downloads');
  panel.hidden = state.queue.length === 0;
  const box = $('#queue');
  const seen = new Set();
  for (const q of state.queue) {
    seen.add(q.id);
    let el = queueEls.get(q.id);
    if (!el) {
      el = document.createElement('div');
      el.innerHTML = '<div class="q-top"><span class="q-name"></span><span class="q-meta"></span><span class="q-actions"></span></div>' +
        '<div class="q-bar"><div></div></div><div class="q-sub"></div>';
      el.dataset.id = q.id;
      queueEls.set(q.id, el);
      box.appendChild(el);
    }
    el.className = `q-item st-${q.status}`;
    el.querySelector('.q-name').textContent = q.name;
    el.querySelector('.q-name').title = `${q.remote}:${q.path}`;
    const pct = q.total ? Math.min(100, (100 * q.bytes) / q.total) : 0;
    el.querySelector('.q-bar > div').style.width = `${q.status === 'done' ? 100 : pct}%`;
    let meta = '';
    if (q.status === 'running') {
      meta = q.total ? `${fmtSize(q.bytes)} of ${fmtSize(q.total)}` : fmtSize(q.bytes);
      if (q.speed > 0) meta += ` · ${fmtSize(q.speed)}/s`;
      const eta = fmtEta(q.eta);
      if (eta) meta += ` · ${eta}`;
    } else {
      meta = { queued: 'Queued', done: q.total ? `Done · ${fmtSize(q.total)}` : 'Done', failed: 'Failed', cancelled: 'Cancelled' }[q.status];
    }
    el.querySelector('.q-meta').textContent = meta;
    const actions = { queued: [['cancel', 'Cancel']], running: [['cancel', 'Cancel']], done: [['open', 'Show in folder']],
      failed: [['retry', 'Retry']], cancelled: [['retry', 'Retry']] }[q.status];
    const actionKey = actions.map((a) => a[0]).join(',');
    const actionBox = el.querySelector('.q-actions');
    if (actionBox.dataset.key !== actionKey) {
      actionBox.dataset.key = actionKey;
      actionBox.innerHTML = actions.map(([act, label]) => `<button type="button" data-act="${act}">${label}</button>`).join('');
    }
    const sub = el.querySelector('.q-sub');
    sub.textContent = q.status === 'failed' && q.error ? q.error : `→ ${q.dest}`;
  }
  for (const [id, el] of queueEls) {
    if (!seen.has(id)) { el.remove(); queueEls.delete(id); }
  }
  const running = state.queue.filter((q) => q.status === 'running').length;
  const queued = state.queue.filter((q) => q.status === 'queued').length;
  const parts = [];
  if (running) parts.push(`${running} running`);
  if (queued) parts.push(`${queued} queued`);
  $('#dl-summary').textContent = parts.join(' · ');
  $('#clear-finished').disabled = !state.queue.some((q) => ['done', 'failed', 'cancelled'].includes(q.status));
}

// ---- messages from the backend -----------------------------------------------------------

function onInit(msg) {
  state.remotes = msg.remotes;
  state.remote = msg.remote;
  state.mode = msg.mode;
  state.destinations = msg.destinations;
  state.defaultDest = msg.defaultDest;
  $('#version').textContent = `v${msg.version}`;
  renderRemotes();
  renderMode();
  renderRows();
  if (msg.configError) showBanner(`${msg.configError}\n\nConfig file: ${msg.configPath}`);
}

if (webview) {
  webview.addEventListener('message', (ev) => {
    const msg = ev.data;
    switch (msg.type) {
      case 'init': onInit(msg); break;
      case 'searchResult': onSearchResult(msg); break;
      case 'searchError': onSearchError(msg); break;
      case 'folderPicked': $('#dest').value = msg.path; state.destinations[state.remote] = msg.path; break;
      case 'queue': state.queue = msg.items; renderQueue(); break;
      case 'error': showBanner(msg.message); break;
    }
  });
}

// ---- events ------------------------------------------------------------------------------

$('#search-form').addEventListener('submit', (ev) => { ev.preventDefault(); startSearch(); });

$('#remote').addEventListener('change', (ev) => {
  state.remote = ev.target.value;
  state.hits = [];
  state.selected.clear();
  state.resultNote = '';
  state.lastQuery = '';
  showBanner('');
  renderRemoteInfo();
  renderRows();
  send({ type: 'prefs', remote: state.remote });
});

for (const b of document.querySelectorAll('.segmented button')) {
  b.addEventListener('click', () => {
    if (state.mode === b.dataset.mode) return;
    state.mode = b.dataset.mode;
    renderMode();
    send({ type: 'prefs', mode: state.mode });
    if (state.lastQuery && !state.searching) startSearch(); // same words, other kind
  });
}

$('#results').addEventListener('scroll', scheduleRows, { passive: true });
window.addEventListener('resize', scheduleRows);

$('#rows').addEventListener('mousedown', (ev) => {
  const i = rowIndex(ev);
  if (i < 0) return;
  ev.preventDefault();
  $('#results').focus();
  const onBox = ev.target.matches('input[type=checkbox]');
  setCursor(i, { extend: ev.shiftKey, toggle: onBox || ev.ctrlKey });
});
$('#rows').addEventListener('dblclick', (ev) => {
  const i = rowIndex(ev);
  if (i >= 0) download([state.hits[i]]);
});

$('#select-all').addEventListener('change', (ev) => {
  state.selected.clear();
  if (ev.target.checked) state.hits.forEach((_, i) => state.selected.add(i));
  renderRows();
});

$('#results').addEventListener('keydown', (ev) => {
  const page = Math.max(1, Math.floor($('#results').clientHeight / ROW) - 1);
  const move = { ArrowDown: 1, ArrowUp: -1, PageDown: page, PageUp: -page }[ev.key];
  if (move !== undefined) {
    ev.preventDefault();
    setCursor(Math.max(0, state.cursor) + move, { extend: ev.shiftKey, keep: ev.ctrlKey });
  } else if (ev.key === 'Home' || ev.key === 'End') {
    ev.preventDefault();
    setCursor(ev.key === 'Home' ? 0 : state.hits.length - 1, { extend: ev.shiftKey });
  } else if (ev.key === ' ') {
    ev.preventDefault();
    if (state.cursor >= 0) setCursor(state.cursor, { toggle: true });
  } else if (ev.key === 'a' && ev.ctrlKey) {
    ev.preventDefault();
    state.hits.forEach((_, i) => state.selected.add(i));
    renderRows();
  } else if (ev.key === 'Enter') {
    ev.preventDefault();
    download(selectedHits());
  }
});

$('#download').addEventListener('click', () => download(selectedHits()));
$('#browse').addEventListener('click', () => send({ type: 'pickFolder', current: $('#dest').value.trim() }));
$('#dest').addEventListener('change', (ev) => { state.destinations[state.remote] = ev.target.value.trim(); });
$('#clear-finished').addEventListener('click', () => send({ type: 'clearFinished' }));

$('#queue').addEventListener('click', (ev) => {
  const btn = ev.target.closest('button[data-act]');
  if (!btn) return;
  const id = Number(btn.closest('.q-item').dataset.id);
  const type = { cancel: 'cancelItem', retry: 'retryItem', open: 'openFolder' }[btn.dataset.act];
  send({ type, id });
});

document.addEventListener('keydown', (ev) => {
  if ((ev.key === 'f' && ev.ctrlKey) || (ev.key === '/' && document.activeElement.tagName !== 'INPUT')) {
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
renderMode();
renderRows();
