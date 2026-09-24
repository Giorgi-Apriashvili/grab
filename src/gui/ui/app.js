'use strict';
// grab-gui front end. Talks to the C++ backend (src/gui/app.cpp) with JSON messages:
//   to backend:   init, prefs, search, cancelSearch, pickFolder, enqueue, cancelItem,
//                 pauseItem, resumeItem, retryItem, pauseAll, resumeAll, cancelAll,
//                 setParallel, setLimit, clearFinished, openFolder,
//                 pauseChild, resumeChild, skipChild,
//                 settings: servers, serverSave, serverConfirm, serverCancel, serverTrust,
//                 serverTest, serverRemove, serverDefault, pickFile, openConfig
//   from backend: init, searchResult, searchError, folderPicked, queue, error,
//                 settings: servers, settingsBusy, hostKeys, serverSaved, serverTested,
//                 serverError, filePicked

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
  parallel: 4,
  limitOn: false,
  limitMiBps: 5,
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
  $('#dest').value = state.destinations[state.remote] || state.defaultDest;
  if (!r) { info.hidden = true; return; }
  info.hidden = false;
  info.classList.toggle('error', !!r.error);
  info.textContent = r.error ? 'misconfigured' : `${r.user}@${r.host} · ${r.method === 'ssh' ? 'ssh + find' : 'rclone listing'}`;
  info.title = r.error || '';
}

function renderMode() {
  for (const b of document.querySelectorAll('#search-form .segmented button')) {
    b.setAttribute('aria-checked', String(b.dataset.mode === state.mode));
  }
  $('#query').placeholder = state.mode === 'folder' ? 'Search folders by name, e.g. lioness season 3'
    : 'Search files by name, e.g. lioness s03e08';
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
  // Before rendering: an empty result shows "No files matching …" from these.
  const secs = (msg.elapsedMs / 1000).toFixed(1);
  state.resultNote = `${msg.hits.length} ${state.mode === 'folder' ? 'folder' : 'file'}${msg.hits.length === 1 ? '' : 's'} in ${secs} s`;
  state.searchedWhere = `${msg.roots.join(', ')}, depth ${msg.maxDepth}`;
  setSearching(false);
  $('#results').scrollTop = 0;
  renderRows(); // also renders the summary
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
    empty.innerHTML = '<div>No servers yet. Add the Linux server you want to download from.<br>' +
      '<button type="button" class="primary" data-act="add-server">Add a server</button></div>';
  } else {
    empty.textContent = r && r.error ? r.error
      : `Search ${state.remote || 'a server'} for ${state.mode === 'folder' ? 'folders' : 'files'} by name. Every word must appear, in any order.`;
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
  const busy = new Set(state.queue.filter((q) => ['queued', 'running', 'paused'].includes(q.status))
    .map((q) => `${q.remote}|${q.path}|${q.dest}`));
  const items = hits.filter((h) => !busy.has(`${state.remote}|${h.path}|${dest}`))
    .map((h) => ({ path: h.path, name: h.name, size: h.size }));
  if (!items.length) return;
  state.destinations[state.remote] = dest;
  send({ type: 'enqueue', remote: state.remote, mode: state.mode, dest, items });
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
  for (const q of state.queue) {
    seen.add(q.id);
    let el = queueEls.get(q.id);
    if (!el) {
      el = document.createElement('div');
      el.innerHTML = '<div class="q-top"><button type="button" class="q-toggle" data-act="toggle" hidden></button>' +
        '<span class="q-name"></span><span class="q-meta"></span><span class="q-actions"></span></div>' +
        '<div class="q-bar"><div></div></div><div class="q-sub"></div><div class="q-tree" hidden></div>';
      el.dataset.id = q.id;
      queueEls.set(q.id, el);
      box.appendChild(el);
    }
    el.className = `q-item st-${q.status}`;
    el.querySelector('.q-name').textContent = q.mode === 'folder' ? `${q.name}/` : q.name;
    el.querySelector('.q-name').title = `${q.remote}:${q.path}`;
    const pct = q.total ? Math.min(100, (100 * q.bytes) / q.total) : 0;
    el.querySelector('.q-bar > div').style.width = `${q.status === 'done' ? 100 : pct}%`;
    const sizes = q.total ? `${fmtSize(q.bytes)} of ${fmtSize(q.total)}` : fmtSize(q.bytes);
    // A listed folder counts its files: "12 of 40 files · 2.1 of 5.6 GiB".
    const files = q.files ? `${q.files.done} of ${q.files.total - q.files.skipped} files · ` : '';
    let meta = '';
    if (q.status === 'running') {
      meta = files + sizes;
      if (q.speed > 0) meta += ` · ${fmtSize(q.speed)}/s`;
      const eta = fmtEta(q.eta);
      if (eta) meta += ` · ${eta}`;
      if (q.connections > 0) meta += ` · ${q.connections} connection${q.connections === 1 ? '' : 's'}`;
    } else if (q.status === 'paused' || (q.status === 'queued' && q.bytes > 0)) {
      meta = `${q.status === 'paused' ? 'Paused' : 'Queued'} · ${files}${sizes}`;
    } else if (q.status === 'done' && q.files) {
      meta = `Done · ${q.files.done} files · ${fmtSize(q.total)}${q.files.skipped ? ` · ${q.files.skipped} skipped` : ''}`;
    } else {
      meta = { queued: 'Queued', done: q.total ? `Done · ${fmtSize(q.total)}` : 'Done', failed: 'Failed', cancelled: 'Cancelled' }[q.status];
    }
    el.querySelector('.q-meta').textContent = meta;
    const actions = {
      queued: [['pause', 'Pause'], ['cancel', 'Cancel']],
      running: [['pause', 'Pause'], ['cancel', 'Cancel']],
      paused: [['resume', 'Resume'], ['cancel', 'Cancel']],
      done: [['open', 'Show in folder']],
      failed: [['retry', q.bytes > 0 ? 'Resume' : 'Retry'], ['cancel', 'Cancel']],
      cancelled: [['retry', 'Retry']],
    }[q.status];
    const actionKey = actions.map((a) => a.join(':')).join(',');
    const actionBox = el.querySelector('.q-actions');
    if (actionBox.dataset.key !== actionKey) {
      actionBox.dataset.key = actionKey;
      actionBox.innerHTML = actions.map(([act, label]) => `<button type="button" data-act="${act}">${label}</button>`).join('');
    }
    const sub = el.querySelector('.q-sub');
    sub.textContent = q.status === 'failed' && q.error ? q.error : q.note ? `→ ${q.dest} · ${q.note}` : `→ ${q.dest}`;
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

function fmtLimit(v) {
  return String(Math.round(v * 10) / 10);
}

function renderLimit() {
  $('#limit-on').checked = state.limitOn;
  const box = $('#limit-value');
  box.disabled = !state.limitOn;
  if (document.activeElement !== box) box.value = fmtLimit(state.limitMiBps);
  renderTotalSpeed();
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
  state.mode = msg.mode;
  state.defaultDest = msg.defaultDest;
  if (msg.parallel) state.parallel = msg.parallel;
  if (typeof msg.limitOn === 'boolean') state.limitOn = msg.limitOn;
  if (msg.limitMiBps) state.limitMiBps = msg.limitMiBps;
  renderLimit();
  $('#version').textContent = `v${msg.version}`;
  renderRemotes();
  renderMode();
  renderRows();
  if (msg.configError) {
    showBanner(`${msg.configError}\n\nConfig file: ${msg.configPath}`);
    state.configBanner = true;
  } else if (state.configBanner) {
    showBanner('');
    state.configBanner = false;
  }
}

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

function showModal({ title, html, ok, cancel = 'Cancel', danger = false, onOk, onCancel }) {
  modal = { onOk, onCancel };
  $('#modal-title').textContent = title;
  $('#modal-body').innerHTML = html;
  $('#modal-cancel').textContent = cancel;
  const okBtn = $('#modal-ok');
  okBtn.textContent = ok;
  okBtn.classList.toggle('danger', danger);
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
      case 'folderPicked': $('#dest').value = msg.path; state.destinations[state.remote] = msg.path; break;
      case 'queue':
        state.queue = msg.items;
        if (msg.parallel) state.parallel = msg.parallel;
        renderQueue();
        break;
      case 'error': showBanner(msg.message); break;
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

// Files and Folders are separate searches: switching starts clean and waits for new words.
function switchMode(mode) {
  if (state.mode === mode) return;
  if (state.searching) {
    send({ type: 'cancelSearch' });
    setSearching(false);
  }
  state.searchId += 1; // a result still on its way belongs to the other mode: ignore it
  state.mode = mode;
  state.hits = [];
  state.selected.clear();
  state.cursor = state.anchor = -1;
  state.lastQuery = '';
  state.resultNote = '';
  state.searchedWhere = '';
  $('#query').value = '';
  $('#results').scrollTop = 0;
  showBanner('');
  renderMode();
  renderRows();
  send({ type: 'prefs', mode });
  $('#query').focus();
}

for (const b of document.querySelectorAll('#search-form .segmented button')) {
  b.addEventListener('click', () => switchMode(b.dataset.mode));
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
  const type = { cancel: 'cancelItem', retry: 'retryItem', open: 'openFolder', pause: 'pauseItem',
    resume: 'resumeItem' }[act];
  send({ type, id });
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
renderMode();
renderRows();
