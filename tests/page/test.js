'use strict';
// Page-logic checks for the Downloads table (chunks B1, B2, C, D, G, H and I: the table, sorting,
// column widths, keys and the treegrid ARIA, every file of a folder by subscription, the filter, Go
// to running, the performance numbers, a file's priority in its folder, the status bar's total and
// its graph, the MiB/s box and its confirmation, Waiting files, opening finished files, the split's
// grip and the head's free space; uploads in the window and drops from Explorer, UPLOADS.md chunks 3
// and 4; Settings → Appearance), run in headless Edge by run_page_test.ps1.
const out = [];
window.pageTestOut = out; // read by run_page_test.ps1 when test.js never finishes, to name where it stopped
let failed = 0;
function check(name, got, want) {
  const ok = JSON.stringify(got) === JSON.stringify(want);
  if (!ok) failed++;
  out.push(`${ok ? 'PASS' : 'FAIL'} ${name}${ok ? '' : `: got ${JSON.stringify(got)}, want ${JSON.stringify(want)}`}`);
}
const sent = [];
// Capture what the page sends.
const realSend = send;
// eslint-disable-next-line no-global-assign
send = (m) => sent.push(m);

const tick = () => new Promise((r) => setTimeout(r, 0));
const frame = () => new Promise((r) => requestAnimationFrame(() => r()));

(async () => {
try {
  // formats
  check('fmtPct 99.6 rounds down', fmtPct(996, 1000), '99%');
  check('fmtPct 29', fmtPct(29, 100), '29%');
  check('fmtPct full', fmtPct(100, 100), '100%');
  check('fmtPct over', fmtPct(120, 100), '100%');
  check('fmtClock m:ss', fmtClock(312), '5:12');
  check('fmtClock h:mm:ss', fmtClock(3912), '1:05:12');
  check('fmtClock >99 h', fmtClock(100 * 3600), '>99 h');
  check('fmtClock 99:59:59', fmtClock(100 * 3600 - 1), '99:59:59');
  check('fmtClock null', fmtClock(null), '');
  check('fmtSpeed', fmtSpeed(12.3 * 1048576), '12.3 MiB/s');
  check('fmtSpeed bytes', fmtSpeed(512.4), '512 B/s');
  check('fmtSizePair', fmtSizePair(85.7 * 1048576, 8 * 1073741824), '85.7 MiB / 8.0 GiB');
  check('fmtMiBps kept', fmtMiBps(5), '5 MiB/s');
  check('narrow needs', DL_NEED, [1096, 1024, 932, 840]);

  const folder = (over) => ({
    id: 1, remote: 'Main', mode: 'folder', path: '/tmp/grab-x/f', name: 'f', dest: 'C:\\D', local: 'C:\\D\\f',
    status: 'running', bytes: 1000, total: 4000, speed: 2 * 1048576, eta: 312, connections: 4, note: '', error: '',
    priority: 'normal', files: { total: 10, queued: 3, running: 2, paused: 0, done: 4, failed: 0, skipped: 1 },
    children: [
      { path: 'a.bin', local: 'a.bin', status: 'running', bytes: 100, size: 400, speed: 1048576, eta: 30, error: '' },
      { path: 'b.bin', local: 'b.bin', status: 'queued', bytes: 0, size: 400, speed: 0, eta: null, error: '' },
      { path: 'c.bin', local: 'c.bin', status: 'paused', bytes: 50, size: 400, speed: 0, eta: null, error: '' },
      { path: 'd.bin', local: 'd.bin', status: 'failed', bytes: 0, size: 400, speed: 0, eta: null, error: 'no such file' },
    ],
    moreQueued: 2, ...over,
  });
  const file = (id, over) => ({ id, remote: 'Main', mode: 'file', path: `/tmp/x${id}.bin`, name: `x${id}.bin`, dest: 'C:\\D',
    local: `C:\\D\\x${id}.bin`, status: 'queued', bytes: 0, total: 0, speed: 0, eta: null, connections: 0, note: '', error: '',
    priority: 'normal', position: id, ...over });
  // A folder's files as the backend sends them to the open folder's watch (6.3): 'files', then
  // 'fileDelta' rows [index, status, bytes, speed, eta, priority, error, verified].
  const kidRow = (c) => [c.path, c.size, FS[c.status], c.bytes, 2, c.local ?? null, c.error || null, c.verified ? 1 : 0, c.named ? 1 : 0];
  const feed = (id, list = folder().children, gen = 1) => onFiles({ type: 'files', id, gen, from: 0, total: list.length, rows: list.map(kidRow) });
  const delta = (id, rows, gen = 1) => { onFileDelta({ type: 'fileDelta', id, gen, rows }); refreshFiles(); };
  const rowOf = (key) => dlRow(dlIndexOf(key));
  const selKeys = () => [...dl.sel, ...(dl.fsel ? [...dl.fsel.on].flatMap((x, i) => (x ? [`f${dl.fsel.id}:${i}`] : [])) : [])].sort();
  const keys = () => Array.from({ length: dl.n }, (_, i) => dlRow(i).key);

  // words and slots
  check('word running', dlView(folder()).word, 'Downloading');
  check('word listing', dlView(folder({ phase: 'listing' })).word, 'Listing');
  check('word checking', dlView(folder({ phase: 'checking' })).word, 'Checking');
  check('word verifying count', [dlView(folder({ phase: 'verifying', verified: { done: 12, total: 40 } })).word,
    dlView(folder({ phase: 'verifying', verified: { done: 12, total: 40 } })).count], ['Verifying', '12/40']);
  check('phase ignored unless running', dlView(folder({ status: 'paused', phase: 'listing' })).word, 'Paused');
  check('queued #2', dlView(file(2, { position: 2 })).word, 'Queued #2');
  check('folder count', dlView(folder()).count, '4/9');
  check('folder pct', [dlView(folder()).pct, dlView(folder()).frac], ['25%', 0.25]);
  check('folder size pair', dlView(folder()).size, `${fmtSize(1000)} / ${fmtSize(4000)}`);
  check('speed eta', [dlView(folder()).speed, dlView(folder()).eta], ['2.0 MiB/s', '5:12']);
  check('unknown size', [dlView(file(3)).pct, dlView(file(3)).size], ['-', '']);
  check('done full', [dlView(file(3, { status: 'done', total: 10, bytes: 10 })).pct, dlView(file(3, { status: 'done', total: 10, bytes: 10 })).size], ['100%', '10 B']);
  check('marker ok', dlView(file(3, { status: 'done', check: 'ok', note: 'checksum verified (MD5)' })).mark, '✓');
  check('marker warn tip', dlView(file(3, { status: 'done', check: 'warn', note: 'not verified: x' })).markTip, 'not verified: x');
  check('slots queued', dlSlots(file(1)).map((s) => s && s[0]), ['pause', 'cancel']);
  check('slots running', dlSlots(folder()).map((s) => s && s[0]), ['pause', 'cancel']);
  check('slots paused', dlSlots(folder({ status: 'paused' })).map((s) => s && s[0]), ['resume', 'cancel']);
  check('slots failed no bytes', dlSlots(file(1, { status: 'failed' })).map((s) => s && s[1]), ['Retry', 'Cancel']);
  check('slots failed bytes', dlSlots(file(1, { status: 'failed', bytes: 5 })).map((s) => s && s[1]), ['Resume', 'Cancel']);
  check('slots cancelled', dlSlots(file(1, { status: 'cancelled' })).map((s) => s && s[0]), ['retry', null]);
  check('slots done', dlSlots(file(1, { status: 'done' })).map((s) => s && s[0]), ['show', null]);

  const kid = (st, q) => { const c = folder().children.find((x) => x.status === st); return [fileStatus(q, c), fileSlots(c, fileStatus(q, c)).map((s) => s && s[0])]; };
  const fw = (st, over) => { const q = folder(over); const [s, slots] = kid(st, q); return [s.word, !!s.muted, slots]; };
  check('file queued under running', fw('queued'), ['Queued', false, ['pause', 'skip']]);
  check('file running', fw('running'), ['Downloading', false, ['pause', 'skip']]);
  check('file paused by itself', fw('paused'), ['Paused', false, ['resume', 'skip']]);
  check('file failed: Retry and Skip', fw('failed'), ['Failed', false, ['retry', 'skip']]);
  check('file done: Show', fileSlots({ status: 'done' }, fileStatus(folder(), { status: 'done' })).map((x) => x && x[0]), ['show', null]);
  check('file done under a cancelled folder: Show', fileSlots({ status: 'done' }, fileStatus(folder({ status: 'cancelled' }), { status: 'done' })).map((x) => x && x[0]), ['show', null]);
  check('file skipped: none', fileSlots({ status: 'skipped' }, fileStatus(folder(), { status: 'skipped' })).map((x) => x && x[0]), [null, null]);
  check('file skipped for a reason: the word, the reason in its tooltip',
    [fileStatus(folder(), { status: 'skipped', error: 'a folder of that name is there' }), fileStatus(folder(), { status: 'skipped', error: '' })],
    [{ word: 'Skipped', why: 'Skipped: a folder of that name is there' }, { word: 'Skipped' }]);
  check('file failed under a paused folder: Retry', fw('failed', { status: 'paused' }), ['Failed', false, ['retry', 'skip']]);
  check('file queued under paused folder', fw('queued', { status: 'paused' }), ['Paused', true, [null, 'skip']]);
  check('file paused under paused folder', fw('paused', { status: 'paused' }), ['Paused', false, ['resume', 'skip']]);
  check('file queued under failed folder', fw('queued', { status: 'failed', error: '1 file failed' }), ['Stopped', true, [null, 'skip']]);
  check('stopped tip', kid('queued', folder({ status: 'failed', error: 'boom' }))[0].why, 'The folder stopped: boom');
  check('file under cancelled folder', fw('running', { status: 'cancelled' }), ['Cancelled', true, [null, null]]);
  check('paused tip', kid('queued', folder({ status: 'paused' }))[0].why, 'The folder is paused');

  // moves
  state.queue = [file(1), file(2), file(3)];
  check('move up first', moveTarget(1, 'up'), undefined);
  check('move up', moveTarget(2, 'up'), 1);
  check('move down', moveTarget(1, 'down'), 3);
  check('move down to end', moveTarget(2, 'down'), 0);
  check('move down last', moveTarget(3, 'down'), undefined);
  check('move top', moveTarget(3, 'top'), 1);
  check('move bottom', moveTarget(1, 'bottom'), 0);
  check('move bottom last', moveTarget(3, 'bottom'), undefined);

  // the table on data
  state.queue = [file(5, { status: 'failed', error: 'connection lost', bytes: 10, total: 100 }), folder({ unreadable: '1 folder couldn\'t be read: z: permission denied' }), file(6, { status: 'paused', waiting: 'Not enough space on E:. Free some space and Resume, or Resume to download anyway.' })];
  sent.length = 0;
  renderQueue();
  check('an open folder asks for its files', sent.map((m) => `${m.type}:${m.id}`), ['watchFiles:1']);
  check('loading row until they come', keys(), ['d5', 'm5:error', 'd1', 'm1:unreadable', 'm1:loading', 'd6', 'm6:note']);
  feed(1);
  check('row kinds', keys(), ['d5', 'm5:error', 'd1', 'm1:unreadable', 'f1:0', 'f1:1', 'f1:2', 'f1:3', 'd6', 'm6:note']);
  const el = (key) => document.querySelector(`.dl-row[data-key="${CSS.escape(key)}"]`);
  check('error message row', [el('m5:error').querySelector('.c-msg').textContent, el('m5:error').classList.contains('m-error')], ['connection lost', true]);
  check('file row word', el('f1:1').querySelector('.dl-word').textContent, 'Queued');
  check('file row name', el('f1:1').querySelector('.dl-text').textContent, 'b.bin');
  check('pool small', dl.pool.length <= 150, true);

  // Review fixes after B1: counts that fit, plain notes in the tooltip, recycled rows.
  check('fmtCount small', fmtCount(99999), '99999');
  check('fmtCount big rounds down', [fmtCount(177191), fmtCount(176999)], ['177k', '176k']);
  check('count 12345/177191', dlView(folder({ files: { total: 177191, queued: 0, running: 1, paused: 0, done: 12345, failed: 0, skipped: 0 } })).count, '12345/177k');
  check('verifying big count', dlView(folder({ phase: 'verifying', verified: { done: 123456, total: 177191 } })).count, '123k/177k');
  check('paused summary is no message row', dlMessages(folder({ status: 'paused', note: '39 files verified (MD5)' })), []);
  check('paused summary in the tooltip', dlView(folder({ status: 'paused', note: '39 files verified (MD5)' })).statusTip.includes('39 files verified (MD5)'), true);
  check('waiting is a message row', dlMessages(file(9, { status: 'paused', waiting: 'Not enough space' })), ['note']);
  check('m6:note shows waiting', el('m6:note').querySelector('.c-msg').textContent.startsWith('Not enough space on E:.'), true);
  check('row controls not tabbable', [...$('#dl-body').querySelectorAll('button, select')].every((n) => n.tabIndex === -1), true);
  {
    // A short name keeps its full width beside a long destination.
    const long = file(30, { name: 's4.bin', dest: `C:\Users\${'x'.repeat(200)}` });
    const saved = state.queue;
    state.queue = [long];
    renderQueue();
    const t = el('d30').querySelector('.dl-text');
    check('short name not cut', t.scrollWidth <= t.clientWidth, true);
    // Recycling: a freed element loses its data-key and the focus.
    const many = [];
    for (let k = 0; k < 400; ++k) many.push(file(100 + k));
    state.queue = many;
    renderQueue();
    const btn = el('d100').querySelector('.dl-slot button');
    btn.focus();
    const body = $('#dl-body');
    body.scrollTop = 300 * DL_ROW;
    renderDlBody();
    const keys = [...document.querySelectorAll('.dl-row[data-key]')].map((n) => n.dataset.key);
    check('one element per data-key', keys.length === new Set(keys).size, true);
    check('hidden rows have no data-key', document.querySelectorAll('.dl-row[hidden][data-key]').length, 0);
    check('focus leaves a recycled row', document.activeElement === $('#queue'), true);
    body.scrollTop = 0;
    state.queue = saved;
    renderQueue();
  }

  // Fold the folder: kept in queue.json, its files let go; open again, asked for again.
  sent.length = 0;
  el('d1').querySelector('.dl-toggle').click();
  check('folded', keys(), ['d5', 'm5:error', 'd1', 'm1:unreadable', 'd6', 'm6:note']);
  check('fold sent', sent.map((m) => `${m.type}:${m.id}:${m.open}`), ['setExpanded:1:false', 'unwatchFiles:1:undefined']);
  check('its store went', dl.stores.has(1), false);
  sent.length = 0;
  el('d1').querySelector('.dl-toggle').click();
  check('open sent', sent.map((m) => `${m.type}:${m.id}:${m.open}`), ['setExpanded:1:true', 'watchFiles:1:undefined']);
  feed(1);

  // Cells don't move while values change.
  const panel = $('#downloads');
  const rects = () => [...el('d1').querySelectorAll('.c-handle, .c-name, .c-status, .c-progress, .c-size, .c-speed, .c-eta, .c-prio, .c-actions, button, select, .dl-word, .dl-count, .dl-mark, .dl-bar, .dl-pct')]
    .map((n) => { const r = n.getBoundingClientRect(); return [Math.round(r.x * 100) / 100, Math.round(r.width * 100) / 100]; });
  const before = JSON.stringify(rects());
  const buttonBefore = el('d1').querySelector('.dl-slot button');
  let moved = 0;
  for (let k = 0; k < 40; ++k) {
    state.queue = [state.queue[0], folder({ bytes: 1000 + k * 77777, total: 4000000, speed: k * 123456, eta: k * 3700, connections: k % 9,
      files: { total: 5965, queued: 10, running: 2, paused: 0, done: 84 + k * 50, failed: 0, skipped: 2 }, unreadable: 'x' }), state.queue[2]];
    renderQueue();
    if (JSON.stringify(rects()) !== before) moved++;
  }
  check('nothing moves over 40 ticks', moved, 0);
  check('button element kept', el('d1').querySelector('.dl-slot button') === buttonBefore, true);
  check('panel step at this width', panel.dataset.step, String(dl.step));

  // Selection, menu, the new confirmation.
  sent.length = 0;
  selectDl(rowOf('d5'));
  selectDl(rowOf('d6'), { toggle: true });
  check('selection', selKeys(), ['d5', 'd6']);
  check('sel class', [el('d5').classList.contains('sel'), el('m5:error').classList.contains('sel'), el('d1').classList.contains('sel')], [true, true, false]);
  const menu = downloadsMenu(selectedDl().map((r) => dl.byId.get(r.id)));
  check('multi menu labels', menu.filter((m) => m.label).map((m) => m.label), ['Resume 1', 'Retry 1', 'Cancel 2', 'High', 'Normal', 'Low', 'Show in folder', 'Copy 2 paths', 'Copy error']);
  menu.find((m) => m.label === 'Cancel 2').act();
  check('multi cancel asks', [$('#modal').hidden, sent.length], [false, 0]);
  closeModal(true);
  check('then cancels both', sent.map((m) => `${m.type}:${m.id}`), ['cancelItem:5', 'cancelItem:6']);
  sent.length = 0;
  selectDl(rowOf('d6'));
  const one = downloadsMenu([dl.byId.get(6)]);
  check('one menu has moves', one.filter((m) => m.label && m.label.startsWith('Move')).map((m) => `${m.label}${m.disabled ? '-' : '+'}`), ['Move up+', 'Move down-', 'Move to top+', 'Move to bottom-']);
  one.find((m) => m.label === 'Cancel').act();
  check('single menu cancel acts at once (no bytes)', sent.map((m) => m.type), ['cancelItem']);
  sent.length = 0;
  deleteSelection(); // Delete on one row without started bytes: at once
  check('delete without bytes acts', [$('#modal').hidden, sent.map((m) => m.type)], [true, ['cancelItem']]);
  sent.length = 0;
  selectDl(rowOf('d5'));
  deleteSelection(); // with started bytes: asks
  check('delete with bytes asks', [$('#modal').hidden, sent.length], [false, 0]);
  closeModal(false);
  check('keep downloading sends nothing', sent.length, 0);

  // files: shift-extends within the folder only
  selectDl(rowOf('f1:0'));
  selectDl(rowOf('f1:3'), { extend: true });
  check('file range', selKeys(), ['f1:0', 'f1:1', 'f1:2', 'f1:3']);
  selectDl(rowOf('d6'), { extend: true });
  check('other scope resets', selKeys(), ['d6']);
  selectDl(rowOf('f1:0'));
  selectDl(rowOf('f1:2'), { toggle: true });
  const fm = dlMenu();
  check('files menu', fm.filter((m) => m.label).map((m) => m.label), ['Pause 1', 'Resume 1', 'Skip 2', 'High', 'Normal', 'Low', 'Show in folder', 'Copy 2 paths']);
  sent.length = 0;
  fm.find((m) => m.label === 'Skip 2').act();
  check('multi skip asks (started bytes)', [$('#modal').hidden, sent.length], [false, 0]);
  closeModal(true);
  check('skips both in one fileAction', sent.map((m) => `${m.type}:${m.id}:${m.gen}:${m.act}:${JSON.stringify(m.ranges)}`), ['fileAction:1:1:skip:[[0,0],[2,2]]']);

  // Drag target: from the row model, past an open folder's files.
  state.queue = [file(1), folder({ id: 2 }), file(3)];
  renderQueue();
  feed(2);
  const body = $('#dl-body');
  const top = body.getBoundingClientRect().top;
  const at = (gap) => { dl.drag = { id: 1, y: top + gap * DL_ROW - body.scrollTop + 2, before: undefined }; dragTarget(); const b = dl.drag.before; dl.drag = null; return b; };
  // rows: d1, d2, f2:0..3, d3
  check('drag rows', keys(), ['d1', 'd2', 'f2:0', 'f2:1', 'f2:2', 'f2:3', 'd3']);
  check('drop above itself: stays', at(0), undefined);
  check('drop just below itself: stays', at(1), undefined);
  check('drop among folder files -> before d3', at(4), 3);
  check('drop at the end', at(9), 0);
  dl.drag = { id: 3, y: top + 1 * DL_ROW + 2, before: undefined }; dragTarget();
  check('drag 3 above folder', dl.drag.before, 2); dl.drag = null;
  $('#dl-drop').hidden = true;

  // Narrow steps by width.
  const steps = {};
  for (const w of [716, 900, 1000, 1250]) {
    panel.style.width = `${w}px`;
    panel.style.flex = 'none';
    updateDlStep();
    renderDlBody();
    steps[w] = [panel.dataset.step, body.scrollWidth === body.clientWidth, $('.dl-head').scrollWidth === $('.dl-head').clientWidth,
      ['c-eta', 'c-speed', 'c-prio'].filter((c) => getComputedStyle(document.querySelector(`.dl-head .${c}`)).display === 'none')];
  }
  out.push(`INFO widths: ${JSON.stringify(steps)} clientWidth@1250=${body.clientWidth}`);
  // The same 40 ticks at each width: cells and buttons keep their x and width.
  state.queue = [file(7, { status: 'running', bytes: 5, total: 100, speed: 9, eta: 5, connections: 2 }), folder({ id: 8 })];
  renderQueue();
  feed(8, folder().children.map((c, k) => (k ? c : { ...c, size: 5000000000 })));
  for (const w of [716, 900, 1000, 1250]) {
    panel.style.width = `${w}px`;
    state.queue = [file(7, { status: 'running', bytes: 5, total: 100, speed: 9, eta: 5, connections: 2 }), folder({ id: 8 })];
    renderQueue();
    const cells = () => JSON.stringify([...document.querySelectorAll('.dl-row[data-key="d8"] > span, .dl-row[data-key="d8"] button, .dl-row[data-key="d8"] select, .dl-row[data-key="d8"] .c-status > span, .dl-row[data-key="d8"] .c-progress > span, .dl-row[data-key="f8:0"] > span, .dl-row[data-key="f8:0"] button')]
      .map((n) => { const r = n.getBoundingClientRect(); return [r.x, r.width]; }));
    const first = cells();
    let moves = 0;
    for (let k = 0; k < 40; ++k) {
      const f = folder({ id: 8, bytes: 1000 + k * 977777, total: 9000000000, speed: k * 1234567, eta: k * 3700 + 1, connections: k % 9,
        files: { total: 177191, queued: 10, running: 2, paused: 0, done: 84 + k * 4321, failed: 0, skipped: 2 } });
      delta(8, [[0, FS.running, k * 9999, k * 99999, k * 77, 2]]);
      state.queue = [file(7, { status: 'running', bytes: 5 + k * 1000, total: 1000000, speed: k * 1000, eta: k, connections: 2 }), f];
      renderQueue();
      if (cells() !== first) moves++;
    }
    check(`nothing moves at ${w}`, moves, 0);
  }
  check('716 compact', steps[716][0], '4');
  check('1250 all', steps[1250][0], '0');
  check('no sideways scroll', Object.values(steps).every((s) => s[1] && s[2]), true);

  // ---- B2: sorting (5.6) --------------------------------------------------------------------
  panel.style.width = '1250px';
  updateDlStep();
  const grid = $('#queue');
  const order = () => dl.top.filter((e) => e.kind === 'dl').map((e) => e.id);
  const lastPrefs = () => { const p = sent.filter((m) => m.type === 'prefs'); return p.length ? p[p.length - 1].downloads : undefined; };
  const head = (k) => document.querySelector(`.dl-head [data-sort="${k}"]`);
  const six = [
    file(1, { name: 'b10.bin', status: 'running', bytes: 50, total: 100, speed: 300, eta: 20, priority: 'low' }),
    file(2, { name: 'b9.bin', status: 'queued', position: 1, total: 400, priority: 'high' }),
    file(3, { name: 'A1.bin', status: 'done', bytes: 900, total: 900 }),
    file(4, { name: 'c.bin', status: 'running', bytes: 10, total: 0, speed: 100, eta: 5 }), // size not known
    file(5, { name: 'b9.bin', status: 'failed', bytes: 0, total: 400, error: 'x' }),       // ties with 2 by name
    file(6, { name: 'd.bin', status: 'running', phase: 'verifying', bytes: 10, total: 100, speed: 50, eta: 1 }),
  ];
  state.queue = six;
  renderQueue();
  check('default order is the queue order', order(), [1, 2, 3, 4, 5, 6]);
  sent.length = 0;
  const cycle = {};
  for (const k of ['name', 'status', 'progress', 'size', 'speed', 'eta', 'priority']) {
    head(k).click();
    const first = [dlSort.dir, order()];
    head(k).click();
    const second = [dlSort.dir, order()];
    head(k).click();
    cycle[k] = [first, second, dlSort.key, order()];
  }
  // Ties (and rows without a value, last both ways) keep the queue order.
  check('name: A to Z naturally, then Z to A', cycle.name, [['asc', [3, 2, 5, 1, 4, 6]], ['desc', [6, 4, 1, 2, 5, 3]], '', [1, 2, 3, 4, 5, 6]]);
  check('status: running first', cycle.status, [['asc', [1, 4, 6, 2, 5, 3]], ['desc', [3, 5, 2, 1, 4, 6]], '', [1, 2, 3, 4, 5, 6]]);
  check('progress: most done first, unknown last', cycle.progress, [['desc', [3, 6, 1, 2, 5, 4]], ['asc', [2, 5, 1, 3, 6, 4]], '', [1, 2, 3, 4, 5, 6]]);
  check('size: largest first, unknown last', cycle.size, [['desc', [3, 2, 5, 1, 6, 4]], ['asc', [1, 6, 2, 5, 3, 4]], '', [1, 2, 3, 4, 5, 6]]);
  check('speed: fastest first, only fetching ones', cycle.speed, [['desc', [1, 4, 2, 3, 5, 6]], ['asc', [4, 1, 2, 3, 5, 6]], '', [1, 2, 3, 4, 5, 6]]);
  check('eta: least first, only fetching ones', cycle.eta, [['asc', [4, 1, 2, 3, 5, 6]], ['desc', [1, 4, 2, 3, 5, 6]], '', [1, 2, 3, 4, 5, 6]]);
  check('priority: High first, finished last', cycle.priority, [['desc', [2, 4, 5, 6, 1, 3]], ['asc', [1, 4, 5, 6, 2, 3]], '', [1, 2, 3, 4, 5, 6]]);
  check('each click saved the whole view', [sent.filter((m) => m.type === 'prefs').length, sent[0].downloads, lastPrefs()],
    [21, { sort: { key: 'name', dir: 'asc' }, widths: {} }, { sort: null, widths: {} }]);
  head('size').click();
  check('arrow, aria-sort, grips', [head('size').dataset.dir, head('size').parentElement.getAttribute('aria-sort'),
    head('size').querySelector('.arrow').textContent, head('name').parentElement.hasAttribute('aria-sort'), panel.classList.contains('sorted')],
  ['desc', 'descending', '▾', false, true]);
  check('grip title while sorted', el('d1').querySelector('.dl-grip').title.startsWith('Dragging works in the queue order'), true);
  const mv = downloadsMenu([dl.byId.get(2)]).filter((m) => m.label && m.label.startsWith('Move'));
  check('moves disabled while sorted', [mv.length, mv.every((m) => m.disabled)], [4, true]);
  sent.length = 0;
  selectDl(rowOf('d2'));
  grid.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowUp', altKey: true, bubbles: true, cancelable: true }));
  check('Alt+Up moves nothing while sorted', sent.filter((m) => m.type === 'moveItem').length, 0);
  check('Alt+Up sorted: a note in the Downloads head names the column', [!$('#dl-note').hidden,
    $('#dl-note').textContent.startsWith('Moves follow the queue order: click the Size title'), panel.classList.contains('noting')], [true, true, true]);
  dl.keyAt = -1e9; // the key above would hold a new order for 2 s (checked below)
  head('size').click();
  head('size').click(); // back to the queue order

  // Changed values: a new order waits while the pointer is over the table, comes once it left,
  // at most once a second, and not while a menu is open.
  clickDlSort('speed'); // [1, 4, ...]
  const y4 = el('d4').style.transform;
  dlSort.over = true;
  dlSort.at = -1e9;
  const speeds = (o) => { state.queue = state.queue.map((q) => (o[q.id] !== undefined ? { ...q, speed: o[q.id] } : q)); renderQueue(); };
  speeds({ 4: 999 });
  check('held while the pointer is over the table', [order(), dlSort.pending, head('speed').hasAttribute('data-pending'), el('d4').style.transform],
    [[1, 4, 2, 3, 5, 6], true, true, y4]);
  check('the title says a new order waits', head('speed').title.includes('waits'), true);
  dlSort.over = false;
  dlRecheck();
  check('comes once the pointer left', [order(), dlSort.pending, head('speed').hasAttribute('data-pending')], [[4, 1, 2, 3, 5, 6], false, false]);
  speeds({ 1: 5000 });
  check('at most once a second', [order(), dlSort.pending], [[4, 1, 2, 3, 5, 6], true]);
  dlSort.at -= DL_RESORT_MS;
  renderQueue();
  check('after the second', [order(), dlSort.pending], [[1, 4, 2, 3, 5, 6], false]);
  dlSort.at = -1e9;
  showMenu(10, 10, [{ label: 'x', act() {} }]);
  speeds({ 4: 99999 });
  check('held while a menu is open', [order(), dlSort.pending], [[1, 4, 2, 3, 5, 6], true]);
  closeMenu();
  dlRecheck();
  check('then comes', order(), [4, 1, 2, 3, 5, 6]);
  dlSort.at = -1e9;
  grid.focus();
  dl.keyAt = performance.now(); // a key in the table just now
  speeds({ 1: 1e10 });
  check('held for a while after a key in the table', [order(), dlSort.pending], [[4, 1, 2, 3, 5, 6], true]);
  dl.keyAt = -1e9;
  dlRecheck();
  check('then comes too', order(), [1, 4, 2, 3, 5, 6]);
  speeds({ 1: 5000 });
  dlSort.at = -1e9;
  renderQueue();
  check('back as before', order(), [4, 1, 2, 3, 5, 6]);
  dlSort.over = true;
  dlSort.at = -1e9;
  speeds({ 1: 1e9 }); // would go first: waits
  state.queue = [...state.queue, file(7, { status: 'running', speed: 5e8, bytes: 1, total: 10 })];
  renderQueue();
  check('a download added goes in its place, the rest stays', [order(), dlSort.pending], [[7, 4, 1, 2, 3, 5, 6], true]);
  state.queue = state.queue.filter((q) => q.id !== 3);
  renderQueue();
  check('a download removed: the rest stays', [order(), dlSort.pending], [[7, 4, 1, 2, 5, 6], true]);
  dlSort.over = false;
  dlRecheck();
  check('the new order', order(), [1, 7, 4, 2, 5, 6]);
  clickDlSort('speed'); // asc
  clickDlSort('speed'); // the queue order
  check('back to the queue order', [order(), dlSort.order, panel.classList.contains('sorted')], [[1, 2, 4, 5, 6, 7], null, false]);

  // gui.json's view from init: a known sort, widths within each column's least and 4000 px.
  sent.length = 0;
  applyDlView({ sort: { key: 'size', dir: 'asc' }, widths: { status: 10, progress: 90, name: 400, eta: 50, speed: 'x', size: -5, handle: 50 } });
  check('init: sort and widths', [dlSort.key, dlSort.dir, order(), dl.widths], ['size', 'asc', [7, 1, 6, 2, 5, 4], { status: 174, progress: 90, size: 124, eta: 50 }]);
  check('init sends nothing', sent.length, 0);
  const hw = (c) => Math.round(document.querySelector(`.dl-head .c-${c}`).getBoundingClientRect().width * 100) / 100;
  check('widths on the grid', [hw('status'), hw('progress'), hw('size'), hw('eta'), hw('speed')], [174, 90, 124, 50, 84]);
  check('rows follow the header', Math.round(el('d1').querySelector('.c-progress').getBoundingClientRect().width), 90);
  check('the bar takes the Progress column', Math.round(el('d1').querySelector('.dl-bar').getBoundingClientRect().width), 42);
  check('narrow steps from the widths', dl.need, DL_NEED.map((n, step) => n - 6 - 22 - 32 - (step === 0 ? 14 : 0)));
  applyDlView({ sort: { key: 'modified', dir: 'asc' }, widths: [] });
  check('a sort Browse has is kept out', [dlSort.key, dlSort.dir], ['size', 'asc']); // unchanged
  dl.widths = {};
  applyDlWidths();
  clickDlSort('size'); // from asc (its second direction): the queue order
  check('from the second direction back to the queue order', dlSort.key, '');
  sent.length = 0;

  // ---- B2: column widths (5.7) --------------------------------------------------------------
  const handle = (c) => document.querySelector(`.dl-resize[data-col="${c}"]`);
  const handles = () => [...document.querySelectorAll('.dl-head .dl-resize')].filter((h) => !h.hidden).map((h) => h.dataset.col);
  check('edges at full width', [dl.step, handles()], [0, ['name', 'status', 'progress', 'size', 'speed', 'eta']]);
  const name0 = hw('name');
  check('drag starts', startDlResize(handle('status'), 500, 1), true);
  moveDlResize(530);
  check('status +30, progress gives it', [hw('status'), hw('progress'), hw('name')], [210, 82, name0]);
  moveDlResize(600);
  check('progress keeps its least', [hw('status'), hw('progress')], [228, 64]);
  moveDlResize(300);
  check('status keeps its least', [hw('status'), hw('progress')], [174, 118]);
  check('nothing saved while dragging', sent.length, 0);
  endDlResize();
  check('saved on release', lastPrefs(), { sort: null, widths: { status: 174, progress: 118 } });
  resetDlWidths('status');
  check('double-click: both back to default', [dl.widths, hw('status'), hw('progress'), lastPrefs().widths], [{}, 180, 112, {}]);
  startDlResize(handle('name'), 400, 1);
  moveDlResize(380);
  check('name edge: name -20, status takes it', [Math.round(hw('name') - name0), hw('status')], [-20, 200]);
  moveDlResize(420);
  check('name edge: name +20 stops at status’s least', [Math.round(hw('name') - name0), hw('status')], [6, 174]);
  moveDlResize(-5000);
  check('name keeps its least', [hw('name'), hw('status')], [160, 180 + Math.round(name0 - 160)]);
  endDlResize();
  check('name has no width of its own', Object.keys(dl.widths), ['status']);
  resetDlWidths('name');
  check('double-click name edge: status back', [dl.widths, dl.step, hw('status')], [{}, 0, 180]);
  setDlWidth('status', 380);
  applyDlWidths();
  check('a wide column moves the narrow steps', [dl.step, body.scrollWidth === body.clientWidth, $('.dl-head').scrollWidth === $('.dl-head').clientWidth], [1, true, true]);
  check('ETA hidden: speed edge trades with priority', handles(), ['name', 'status', 'progress', 'size', 'speed']);
  dl.widths = {};
  applyDlWidths();
  panel.style.width = '900px';
  updateDlStep();
  check('step 3 edges', [dl.step, handles()], [3, ['name', 'status', 'progress']]);
  panel.style.width = '716px';
  updateDlStep();
  check('compact: no edges', [dl.step, handles()], [4, []]);
  check('compact ignores dragged widths', (() => { setDlWidth('status', 300); applyDlWidths(); const w = hw('status'); dl.widths = {}; applyDlWidths(); return w; })(), 120);

  // Review fixes: a sort on a hidden column shows on Name's title, whose click goes back to the
  // queue order; the titles' menu (Queue order, Reset column widths) works in compact columns.
  panel.style.width = '900px';
  updateDlStep();
  clickDlSort('speed');
  const nameHead = () => [head('name').querySelector('.lbl').textContent, head('name').querySelector('.arrow').textContent,
    head('name').parentElement.getAttribute('aria-sort'), head('name').title.startsWith('Sorted by Speed (its column is hidden)')];
  check('step 3, sorted by Speed: Name’s title shows it', [dl.step, ...nameHead()], [3, 'Name · by Speed', '▾', 'other', true]);
  check('grip names the way back', el('d1').querySelector('.dl-grip').title.includes('click Name’s title'), true);
  panel.style.width = '1250px';
  updateDlStep();
  check('Speed shown again: its own arrow, Name plain', [dl.step, ...nameHead(), head('speed').querySelector('.arrow').textContent], [0, 'Name', '', null, false, '▾']);
  panel.style.width = '900px';
  updateDlStep();
  head('name').click();
  check('a click on Name’s title: the queue order', [dlSort.key, lastPrefs().sort, ...nameHead()], ['', null, 'Name', '', null, false]);
  clickDlSort('eta');
  setDlWidth('status', 1000);
  applyDlWidths();
  panel.style.width = '1250px';
  updateDlStep();
  check('wide widths: compact with no edges', [dl.step, handles()], [4, []]);
  $('.dl-head .c-status').dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, cancelable: true, clientX: 300, clientY: 100 }));
  check('the titles’ menu', [...$('#menu').querySelectorAll('button')].map((b) => [b.textContent, b.disabled]),
    [['Queue order', false], ['Reset column widths', false]]);
  $('#menu').querySelectorAll('button')[1].click();
  check('Reset column widths', [dl.widths, dl.step, lastPrefs().widths, $('#menu').hidden], [{}, 0, {}, true]);
  $('.dl-head .c-status').dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, cancelable: true, clientX: 300, clientY: 100 }));
  $('#menu').querySelectorAll('button')[0].click();
  check('Queue order', [dlSort.key, lastPrefs().sort], ['', null]);
  // One shaded entry at most (the focused one): none when the mouse opens a menu, then the one
  // under the pointer, none over a greyed entry or once the pointer leaves; the keys go on from
  // the shaded one (or from the ends with none). Opened by the keys, the first starts shaded.
  {
    const act = () => {};
    const entries = [{ label: 'One', act }, { label: 'Two', act }, { label: 'Off', act, disabled: true }, '-', { label: 'Three', act }];
    const btn = (label) => [...$('#menu').querySelectorAll('button')].find((b) => b.querySelector('.text').textContent === label);
    const shaded = () => (document.activeElement === $('#menu') ? null
      : document.activeElement.closest('#menu') ? document.activeElement.querySelector('.text').textContent : 'outside');
    const move = (el) => el.dispatchEvent(new PointerEvent('pointermove', { bubbles: true }));
    const menuKey = (k) => document.activeElement.dispatchEvent(new KeyboardEvent('keydown', { key: k, bubbles: true, cancelable: true }));
    showMenu(10, 10, entries);
    check('menu by the mouse: nothing shaded, the menu has the keys', shaded(), null);
    move(btn('Two'));
    check('menu: the entry under the pointer is shaded', shaded(), 'Two');
    move(btn('Off'));
    check('menu: none over a greyed entry', shaded(), null);
    move(btn('One'));
    $('#menu').dispatchEvent(new PointerEvent('pointerleave'));
    check('menu: none once the pointer leaves', shaded(), null);
    menuKey('ArrowUp');
    check('menu: Up with none shaded: the last', shaded(), 'Three');
    menuKey('ArrowDown');
    check('menu: Down goes round past the greyed one', shaded(), 'One');
    move(btn('Two'));
    menuKey('ArrowDown');
    check('menu: the keys go on from the hovered entry', shaded(), 'Three');
    move(btn('Off'));
    let ran = 0;
    entries[0].act = () => { ran++; };
    menuKey('Enter');
    check('menu: Enter with none shaded does nothing', [ran, $('#menu').hidden], [0, false]);
    closeMenu();
    showMenu(10, 10, entries, true);
    check('menu by the keys: the first starts shaded', shaded(), 'One');
    closeMenu();
  }
  // The dialog gives the focus back where it was (the table after Delete's question).
  grid.focus();
  showModal({ title: 't', html: '', ok: 'OK', onOk() {} });
  check('dialog takes the focus', document.activeElement.id, 'modal-cancel');
  closeModal(false);
  check('the focus back on the table', document.activeElement === grid, true);
  panel.style.width = '1250px';
  updateDlStep();
  renderDlBody();
  sent.length = 0;

  // ---- B2: keys, the cursor and the treegrid (5.8) ----------------------------------------
  state.queue = [file(1), folder({ id: 2, unreadable: '1 folder couldn\'t be read: z: permission denied' }), file(3, { status: 'done', total: 5, bytes: 5 })];
  folderOpen.clear();
  renderQueue();
  feed(2);
  sent.length = 0; // its watchFiles
  // rows: d1, d2, m2:unreadable, f2:0 .. f2:3, d3
  const key = (k, mods = {}) => grid.dispatchEvent(new KeyboardEvent('keydown', { key: k, bubbles: true, cancelable: true, ...mods }));
  const active = () => { const id = grid.getAttribute('aria-activedescendant'); const e = id && document.getElementById(id); return e ? e.dataset.key : null; };
  check('treegrid', [grid.getAttribute('role'), grid.getAttribute('aria-multiselectable'), grid.getAttribute('aria-rowcount'), grid.tabIndex, body.tabIndex],
    ['treegrid', 'true', String(dl.n + 1), 0, -1]);
  check('rows have no Tab stop', [...body.querySelectorAll('*')].every((n) => n.tabIndex < 0), true);
  grid.blur();
  dl.cursor = '';
  clearSel();
  grid.focus();
  renderDlBody();
  check('focus puts the cursor on the first row in view', [document.activeElement === grid, dl.cursor, active(), selKeys().length], [true, 'd1', 'd1', 0]);
  key('ArrowDown');
  check('down: cursor and selection', [dl.cursor, selKeys(), active()], ['d2', ['d2'], 'd2']);
  const aria = (k, n) => el(k).getAttribute(n);
  check('row aria', [aria('d2', 'aria-rowindex'), aria('d2', 'aria-level'), aria('d2', 'aria-expanded'), aria('d2', 'aria-selected'),
    aria('f2:0', 'aria-level'), aria('f2:0', 'aria-expanded'), aria('d1', 'aria-expanded'), aria('d1', 'aria-selected'), aria('m2:unreadable', 'aria-level')],
  ['3', '1', 'true', 'true', '2', null, null, 'false', '2']);
  check('cursor class', [el('d2').classList.contains('cursor'), el('d1').classList.contains('cursor')], [true, false]);
  key('ArrowDown');
  check('a message row selects its download', [dl.cursor, selKeys()], ['m2:unreadable', ['d2']]);
  key('ArrowDown');
  key('ArrowDown', { shiftKey: true });
  check('shift extends within the folder', [dl.cursor, selKeys()], ['f2:1', ['f2:0', 'f2:1']]);
  key('ArrowDown', { ctrlKey: true });
  check('ctrl moves the cursor alone', [dl.cursor, selKeys()], ['f2:2', ['f2:0', 'f2:1']]);
  key(' ', { ctrlKey: true });
  check('ctrl+space toggles', selKeys(), ['f2:0', 'f2:1', 'f2:2']);
  key(' ');
  check('space is left free', [sent.length, selKeys().length], [0, 3]);
  key('a', { ctrlKey: true });
  check('ctrl+a: the files of that folder', selKeys(), ['f2:0', 'f2:1', 'f2:2', 'f2:3']);
  key('ArrowDown', { shiftKey: true });
  key('ArrowDown', { shiftKey: true }); // past the files: d3
  check('shift past the folder keeps to its files', [dl.cursor, selKeys()], ['d3', ['f2:2', 'f2:3']]);
  key('ArrowUp');
  key('ArrowUp');
  key('ArrowLeft');
  check('left goes up to the folder', [dl.cursor, selKeys()], ['d2', ['d2']]);
  key('ArrowLeft');
  check('left folds it', [keys(), dl.cursor, aria('d2', 'aria-expanded')], [['d1', 'd2', 'm2:unreadable', 'd3'], 'd2', 'false']);
  key('ArrowRight');
  feed(2);
  check('right opens it', [dl.n, dl.cursor], [8, 'd2']);
  key('ArrowRight');
  check('right goes to its first file', [dl.cursor, selKeys()], ['f2:0', ['f2:0']]);
  el('d2').querySelector('.dl-toggle').click(); // folded by the mouse: the cursor goes up to it
  check('a folded file\'s cursor goes to its folder', [dl.cursor, active()], ['d2', 'd2']);
  el('d2').querySelector('.dl-toggle').click();
  feed(2);
  key('End');
  check('end', [dl.cursor, selKeys()], ['d3', ['d3']]);
  sent.length = 0;
  key('Enter');
  check('enter shows a finished download', sent.map((m) => `${m.type}:${m.id}`), ['openFolder:3']);
  key('Home', { shiftKey: true });
  check('shift+home: the downloads up to the top', [dl.cursor, selKeys()], ['d1', ['d1', 'd2', 'd3']]);
  key('ArrowDown');
  sent.length = 0;
  key('Enter');
  check('enter on an unfinished download does nothing', sent.length, 0);
  key('a', { ctrlKey: true });
  check('ctrl+a on a download: every download', selKeys(), ['d1', 'd2', 'd3']);
  key('ArrowDown');
  key('F10', { shiftKey: true });
  check('shift+f10 opens the menu at the cursor row', [$('#menu').hidden, dl.cursor, selKeys()], [false, 'm2:unreadable', ['d2']]);
  closeMenu();
  clearSel();
  key('ContextMenu');
  check('the menu key with nothing selected takes the cursor row', [$('#menu').hidden, selKeys()], [false, ['d2']]);
  closeMenu();
  key('Delete');
  check('delete on a started download asks first', [$('#modal').hidden, sent.length], [false, 0]);
  closeModal(false);
  sent.length = 0;
  {
    const many = [];
    for (let k = 0; k < 400; ++k) many.push(file(100 + k));
    state.queue = many;
    renderQueue();
    key('Home');
    const page = Math.max(1, Math.floor(body.clientHeight / DL_ROW) - 1);
    key('PageDown');
    check('page down', dl.cursor, `d${100 + page}`);
    key('PageUp');
    check('page up', dl.cursor, 'd100');
    key('ArrowDown');
    const cur = dl.cursor;
    body.scrollTop = 300 * DL_ROW;
    renderDlBody();
    check('the cursor row keeps its element out of view', [active(), dl.bound.get(cur).hidden, document.querySelectorAll(`.dl-row[data-key="${cur}"]`).length], [cur, false, 1]);
    check('the pool stays small', dl.pool.length <= DL_POOL, true);
    body.scrollTop = 0;
    renderDlBody();
    check('back in view: still the right row', active(), cur);
    body.scrollTop = 300 * DL_ROW;
    renderDlBody();
    key('ArrowDown');
    check('a key brings the cursor back in view', [dl.cursor, active(), body.scrollTop <= 2 * DL_ROW], [`d${102}`, 'd102', true]);
    key('End');
    check('end scrolls to the last', [dl.cursor, active(), body.scrollTop + body.clientHeight >= 400 * DL_ROW - 1], ['d499', 'd499', true]);
    body.scrollTop = 0;
  }
  dl.cursor = '';
  clearSel();
  sent.length = 0;
  out.push(`INFO edges: ${JSON.stringify([...document.querySelectorAll('.dl-resize')].map((h) => { const r = h.getBoundingClientRect(); return [h.dataset.col, h.hidden, Math.round(r.x * 10) / 10, r.width, getComputedStyle(h).display]; }))}`);


  // ---- C: every file listed (6.3), actions by index, the filter, Go to running, file sorts ----
  panel.style.width = '1250px';
  panel.style.height = '420px'; // a body that scrolls, as the splitter gives it
  updateDlStep();
  dlSort.key = '';
  showDlOrder();
  folderOpen.clear();
  clearSel();
  dl.cursor = '';
  dl.keyAt = -1e9;
  const counts = (over) => ({ total: 10, queued: 10, running: 0, paused: 0, done: 0, failed: 0, skipped: 0, ...over });
  const ten = Array.from({ length: 10 }, (_, k) => ({ path: `d/f${k}.bin`, size: (k + 1) * 1000, status: 'queued', bytes: 0 }));
  state.queue = [folder({ id: 21, files: counts() })];
  sent.length = 0;
  renderQueue();
  feed(21, ten);
  check('C: every file is a row', keys(), ['d21', ...ten.map((_, k) => `f21:${k}`)]);
  delta(21, [[2, FS.done, 3000, 0, null, 2], [5, FS.done, 6000, 0, null, 2, null, 1], [7, FS.failed, 0, 0, null, 2, 'permission denied']]);
  check('C: finished files stay in place', keys(), ['d21', ...ten.map((_, k) => `f21:${k}`)]);
  check('C: their words', ['f21:2', 'f21:5', 'f21:7'].map((k) => el(k).querySelector('.dl-word').textContent), ['Done', 'Done', 'Failed']);
  check('C: verified mark', [el('f21:5').querySelector('.dl-mark').textContent, el('f21:2').querySelector('.dl-mark').textContent], ['✓', '']);
  check('C: a failed file says why', el('f21:7').querySelector('.c-status').title, 'permission denied');
  check('C: slots', ['f21:0', 'f21:2', 'f21:7'].map((k) => [...el(k).querySelectorAll('.dl-slot button')].map((b) => b.dataset.act)),
    [['pause', 'skip'], ['show'], ['retry', 'skip']]);
  sent.length = 0;
  el('f21:7').querySelector('[data-act="retry"]').click();
  el('f21:2').querySelector('[data-act="show"]').click();
  el('f21:0').querySelector('[data-act="pause"]').click();
  check('C: row buttons act by index', sent.map((m) => JSON.stringify(m)), [
    JSON.stringify({ type: 'fileAction', id: 21, gen: 1, act: 'retry', ranges: [[7, 7]] }),
    JSON.stringify({ type: 'showFile', id: 21, gen: 1, index: 2 }),
    JSON.stringify({ type: 'fileAction', id: 21, gen: 1, act: 'pause', ranges: [[0, 0]] })]);
  selectDl(rowOf('f21:0'));
  selectDl(rowOf('f21:9'), { extend: true });
  check('C: the menu of a range', dlMenu().filter((m) => m.label).map((m) => m.label),
    ['Pause 7', 'Retry 1', 'Skip 8', 'High', 'Normal', 'Low', 'Show in folder', 'Copy 10 paths', 'Copy error']);
  sent.length = 0;
  dlMenu().find((m) => m.label === 'Pause 7').act();
  check('C: one fileAction for the range, in ranges', sent.map((m) => `${m.act}:${JSON.stringify(m.ranges)}`), ['pause:[[0,1],[3,4],[6,6],[8,9]]']);
  sent.length = 0;
  deleteSelection();
  check('C: Delete on files without bytes: Skip at once', [$('#modal').hidden, sent.map((m) => `${m.act}:${JSON.stringify(m.ranges)}`)],
    [true, ['skip:[[0,1],[3,4],[6,9]]']]);
  sent.length = 0;
  key('Enter'); // not on a finished file: nothing
  selectDl(rowOf('f21:5'));
  dl.cursor = 'f21:5';
  key('Enter');
  check('C: Enter shows a finished file', sent.map((m) => `${m.type}:${m.index}`), ['showFile:5']);
  check('C: a file sorts and selects by its index', [dlIndexOf('f21:5'), rowOf('f21:5').index, isSel(rowOf('f21:5'))], [6, 5, true]);

  // A file saved under another name: the arrow, and Copy path's local path (5.10).
  state.queue = [folder({ id: 21, files: counts() }), folder({ id: 22, name: 'g', local: 'C:\\D\\g', files: counts({ total: 2 }) })];
  renderQueue();
  feed(22, [{ path: 'a:b.txt', size: 5, status: 'done', bytes: 5, local: 'a：b.txt' },
    { path: 'e01.mkv', size: 9, status: 'queued', bytes: 0, local: 'e01 (2).mkv', named: true }]);
  check('C: named and safe names', [el('f22:1').querySelector('.dl-text').textContent, el('f22:0').querySelector('.dl-text').textContent,
    localPath(dl.byId.get(22), dl.stores.get(22), 0), localPath(dl.byId.get(22), dl.stores.get(22), 1), localPath(dl.byId.get(21), dl.stores.get(21), 3)],
  ['e01.mkv → e01 (2).mkv', 'a:b.txt', 'C:\\D\\g\\a：b.txt', 'C:\\D\\g\\e01 (2).mkv', 'C:\\D\\f\\d\\f3.bin']);

  // The filter: finished rows dim, then leave once their second passed and the pointer left.
  state.queue = [folder({ id: 21, files: counts({ done: 2, failed: 1, queued: 7 }) }), file(40, { status: 'done', total: 5, bytes: 5 })];
  renderQueue();
  setDlShow('unfinished');
  check('C: Unfinished shows what still comes', keys(), ['d21', 'f21:0', 'f21:1', 'f21:3', 'f21:4', 'f21:6', 'f21:7', 'f21:8', 'f21:9']);
  dlSort.over = true;
  dl.stores.get(21).at = -1e9;
  delta(21, [[0, FS.done, 1000, 0, null, 2]]);
  check('C: a file done meanwhile dims and waits', [keys().includes('f21:0'), el('f21:0').classList.contains('leave'), dlSort.pending], [true, true, true]);
  dlSort.over = false;
  dl.keyAt = -1e9;
  dlRecheck();
  check('C: then it leaves', [keys().includes('f21:0'), dlSort.pending], [false, false]);
  setDlShow('failed');
  check('C: Failed: the failed file and its folder', keys(), ['d21', 'f21:7']);
  setDlShow('all');
  check('C: All again', keys().length, 12);
  check('C: the filter is never saved', sent.some((m) => m.type === 'prefs' && JSON.stringify(m).includes('unfinished')), false);

  // Go to running: the files downloading now, the next at each click, around to the first.
  const run400 = Array.from({ length: 400 }, (_, k) => ({ path: `m/${k}.bin`, size: 1e6, status: k === 150 || k === 300 ? 'running' : 'done', bytes: k === 150 || k === 300 ? 5e5 : 1e6 }));
  state.queue = [folder({ id: 23, files: counts({ total: 400 }) })];
  renderQueue();
  feed(23, run400);
  body.scrollTop = 0;
  renderDlBody();
  const topRow = () => Math.floor(body.scrollTop / DL_ROW);
  $('#dl-go').click();
  check('C: Go to running', [topRow(), dlRow(topRow() + 1).key], [150, 'f23:150']);
  $('#dl-go').click();
  check('C: and the next', dlRow(topRow() + 1).key, 'f23:300');
  $('#dl-go').click();
  check('C: around to the first', dlRow(topRow() + 1).key, 'f23:150');
  body.scrollTop = 0;

  // Sorting files under their folder (5.6): names naturally, sizes, ties in folder order; a live
  // column re-sorts a small folder as the numbers change (held while the pointer is over the table).
  const sortKids = [{ path: 'b10.bin', size: 5, status: 'queued', bytes: 0 }, { path: 'b9.bin', size: 5, status: 'queued', bytes: 0 },
    { path: 'a.bin', size: 7, status: 'done', bytes: 8 }, { path: 'C.bin', size: 1, status: 'queued', bytes: 0 }];
  state.queue = [folder({ id: 24, files: counts({ total: 4 }) })];
  renderQueue();
  feed(24, sortKids);
  const filesNow = () => keys().filter((k) => k.startsWith('f24:')).map((k) => +k.split(':')[1]);
  head('name').click();
  check('C: files by name', filesNow(), [2, 1, 0, 3]);
  head('name').click();
  check('C: files by name, Z to A', filesNow(), [3, 0, 1, 2]);
  head('name').click();
  check('C: back to the folder order', filesNow(), [0, 1, 2, 3]);
  head('size').click();
  check('C: by size: a done file by its bytes, ties in folder order', filesNow(), [2, 0, 1, 3]);
  head('size').click();
  head('size').click();
  head('progress').click();
  check('C: by progress', filesNow(), [2, 0, 1, 3]);
  dlSort.over = true;
  dl.stores.get(24).at = -1e9;
  delta(24, [[3, FS.running, 1, 10, 0, 2]]);
  check('C: a live re-sort waits while the pointer is over the table', [filesNow(), dlSort.pending], [[2, 0, 1, 3], true]);
  dlSort.over = false;
  dl.keyAt = -1e9;
  dlRecheck();
  check('C: then comes', [filesNow(), dlSort.pending], [[2, 3, 0, 1], false]);
  head('progress').click();
  head('progress').click();
  check('C: the queue order again', [dlSort.key, filesNow()], ['', [0, 1, 2, 3]]);

  // A folder of more than 5,000 files keeps the order of the click on a live column (hollow arrow).
  const big = Array.from({ length: 6000 }, (_, k) => ({ path: `b/${k}.bin`, size: 1e6, status: k < 3 ? 'running' : 'queued', bytes: 0 }));
  state.queue = [folder({ id: 25, files: counts({ total: 6000 }) })];
  renderQueue();
  feed(25, big);
  delta(25, [[0, FS.running, 10, 100, 9, 2], [1, FS.running, 10, 300, 9, 2], [2, FS.running, 10, 200, 9, 2]]);
  const bigNow = () => keys().filter((k) => k.startsWith('f25:')).slice(0, 3).map((k) => +k.split(':')[1]);
  head('speed').click();
  check('C: a big folder sorts on the click', [bigNow(), head('speed').querySelector('.arrow').textContent], [[1, 2, 0], '▿']);
  dl.stores.get(25).at = -1e9;
  delta(25, [[0, FS.running, 20, 900, 9, 2]]);
  dlRecheck();
  check('C: and keeps that order as the numbers change', bigNow(), [1, 2, 0]);
  head('speed').click();
  head('speed').click();

  // The user's ▸/▾ comes back from queue.json ('expanded'); a done folder the user opened stays open.
  sent.length = 0;
  state.queue = [folder({ id: 26, expanded: false, files: counts() }), folder({ id: 27, status: 'done', expanded: true, files: counts() })];
  folderOpen.clear();
  renderQueue();
  check('C: expanded from the backend', [keys(), sent.map((m) => `${m.type}:${m.id}`)], [['d26', 'd27', 'm27:loading'], ['watchFiles:27']]);

  // Generations and chunks: rows of another generation are dropped, a chunk not following the last
  // asks for the list again, a list in chunks shows its rows as they come.
  state.queue = [folder({ id: 24, files: counts({ total: 4 }) })];
  renderQueue();
  feed(24, sortKids);
  sent.length = 0;
  onFiles({ type: 'files', id: 24, gen: 1, from: 7, total: 4, rows: [] });
  check('C: a chunk out of order asks again', sent.map((m) => `${m.type}:${m.id}`), ['watchFiles:24']);
  delta(24, [[0, FS.done, 5, 0, null, 2]], 9);
  check('C: a delta of another generation is dropped', dl.stores.get(24).status[0], FS.queued);
  onFiles({ type: 'files', id: 24, gen: 2, from: 0, total: 8, rows: [...sortKids, ...sortKids].slice(0, 4).map(kidRow) });
  check('C: a list in chunks: its rows as they come', keys(), ['d24', 'm24:loading', 'f24:0', 'f24:1', 'f24:2', 'f24:3']);
  onFiles({ type: 'files', id: 24, gen: 2, from: 4, total: 8, rows: sortKids.map(kidRow) });
  check('C: then all of them', keys().length, 9);
  check('C: loading row text', msgView(dl.byId.get(24), { what: 'loading' }).msg, 'Loading the list of 8 files… (8)');

  // Review fix: rows coming or going above the view by themselves don't move the rows in view.
  const many = (n) => Array.from({ length: n }, (_, k) => ({ path: `v/${k}.bin`, size: 9, status: 'queued', bytes: 0 }));
  folderOpen.clear();
  state.queue = [folder({ id: 60, files: counts({ total: 300 }) }), folder({ id: 61, files: counts({ total: 300 }) })];
  renderQueue();
  feed(60, many(300));
  feed(61, many(300));
  body.scrollTop = (dlIndexOf('f61:100') ?? 0) * DL_ROW + 5;
  renderDlBody();
  const firstInView = () => dlRow(Math.floor(body.scrollTop / DL_ROW)).key;
  check('C: anchored at a file of the folder below', [firstInView(), body.scrollTop % DL_ROW], ['f61:100', 5]);
  state.queue = [folder({ id: 60, status: 'done', files: counts({ total: 300, done: 300, queued: 0 }) }), folder({ id: 61, files: counts({ total: 300 }) })];
  renderQueue();
  check('C: a folder above folding by itself leaves the view where it was', [keys().includes('f60:0'), firstInView(), body.scrollTop % DL_ROW], [false, 'f61:100', 5]);
  state.queue = [folder({ id: 60, files: counts({ total: 300 }) }), folder({ id: 61, files: counts({ total: 300 }) })];
  renderQueue();
  onFiles({ type: 'files', id: 60, gen: 1, from: 0, total: 300, rows: many(150).map(kidRow) });
  check('C: a list growing above leaves it too', firstInView(), 'f61:100');
  onFiles({ type: 'files', id: 60, gen: 1, from: 150, total: 300, rows: many(150).map(kidRow) });
  check('C: and when it is complete', firstInView(), 'f61:100');
  body.scrollTop = 0;
  folderOpen.clear();

  // Review fix: under Show: Unfinished in the queue order, a move shows at once, the pointer on the table.
  state.queue = [file(70), file(71), file(72)];
  renderQueue();
  setDlShow('unfinished');
  dlSort.over = true;
  sent.length = 0;
  moveDownload(72, 'top');
  check('C: a move under a filter is sent', sent.filter((m) => m.type === 'moveItem').map((m) => [m.id, m.before]), [[72, 70]]);
  state.queue = [file(72), file(70), file(71)];
  renderQueue();
  check('C: and shows at once', [keys(), dlSort.pending], [['d72', 'd70', 'd71'], false]);
  state.queue = [file(72, { status: 'done' }), file(70), file(71)];
  renderQueue();
  check('C: a row that stopped matching still waits', [keys(), dlSort.pending], [['d72', 'd70', 'd71'], true]);
  dlSort.over = false;
  dl.keyAt = -1e9;
  dlSort.at = -1e9;
  dlRecheck();
  check('C: then leaves', keys(), ['d70', 'd71']);
  setDlShow('all');

  // ---- D: a file's priority in its folder (DOWNLOADS-UI.md 7) ---------------------------------
  folderOpen.clear();
  clearSel();
  dl.cursor = '';
  dlSort.key = '';
  showDlOrder();
  const sixKids = [
    { path: 'p0.bin', size: 1e6, status: 'queued', bytes: 0, prio: 4 },
    { path: 'p1.bin', size: 1e6, status: 'running', bytes: 10 },
    { path: 'p2.bin', size: 1e6, status: 'done', bytes: 1e6 },
    { path: 'p3.bin', size: 1e6, status: 'failed', bytes: 0, error: 'x' },
    { path: 'p4.bin', size: 1e6, status: 'skipped', bytes: 0 },
    { path: 'p5.bin', size: 1e6, status: 'paused', bytes: 0 },
  ];
  const prioRow = (c) => { const r = kidRow(c); r[4] = c.prio || 2; return r; };
  state.queue = [folder({ id: 80, gen: 3, files: counts({ total: 6 }) }),
    folder({ id: 81, gen: 4, status: 'cancelled', expanded: true, files: counts({ total: 2 }) })];
  renderQueue();
  onFiles({ type: 'files', id: 80, gen: 3, from: 0, total: 6, rows: sixKids.map(prioRow) });
  onFiles({ type: 'files', id: 81, gen: 4, from: 0, total: 2, rows: sixKids.slice(0, 2).map(prioRow) });
  const prioOf = (k) => { const s = el(k).querySelector('.dl-prio'); return s.style.visibility === 'hidden' ? '' : s.value; };
  check('D: a priority on unfinished files only', [0, 1, 2, 3, 4, 5].map((i) => prioOf(`f80:${i}`)), ['high', 'normal', '', 'normal', '', 'normal']);
  check('D: none under a cancelled folder', [prioOf('f81:0'), prioOf('f81:1')], ['', '']);
  check('D: High is marked', el('f80:0').querySelector('.dl-prio').classList.contains('prio-high'), true);
  check('D: the file row\'s select says what it does', el('f80:0').querySelector('.dl-prio').title.startsWith('Priority in the folder'), true);
  check('D: the download row\'s select keeps its own', el('d80').querySelector('.dl-prio').title.startsWith('Bandwidth priority'), true);
  sent.length = 0;
  const sel5 = el('f80:5').querySelector('.dl-prio');
  sel5.value = 'low';
  sel5.dispatchEvent(new Event('change', { bubbles: true }));
  check('D: a row\'s select sets that file', sent.map((m) => JSON.stringify(m)),
    [JSON.stringify({ type: 'setFilePriority', id: 80, gen: 3, ranges: [[5, 5]], priority: 'low' })]);
  check('D: and gives the focus back to the table', document.activeElement === $('#queue'), true);
  // The selection's menu: the unfinished ones, each priority checked when all have it; only the
  // files it changes are sent.
  selectDl(rowOf('f80:0'));
  selectDl(rowOf('f80:5'), { extend: true });
  const prioItems = () => dlMenu().filter((m) => ['High', 'Normal', 'Low'].includes(m.label));
  check('D: the files\' menu has the priorities', prioItems().map((m) => [m.label, m.checked]), [['High', false], ['Normal', false], ['Low', false]]);
  sent.length = 0;
  prioItems().find((m) => m.label === 'High').act();
  check('D: High for the selection: the unfinished ones not High yet', sent.map((m) => `${m.type}:${m.priority}:${JSON.stringify(m.ranges)}`),
    ['setFilePriority:high:[[1,1],[3,3],[5,5]]']);
  selectDl(rowOf('f80:0'));
  check('D: one High file: High checked', prioItems().map((m) => m.checked), [true, false, false]);
  selectDl(rowOf('f80:2'));
  check('D: a done file has none', prioItems().length, 0);
  // The folder's menu: all its files at once, by their generation.
  selectDl(rowOf('d80'));
  const allItems = () => dlMenu().filter((m) => m.label && m.label.startsWith('Set all files to'));
  check('D: the folder\'s menu sets all files', allItems().map((m) => m.label), ['Set all files to High', 'Set all files to Normal', 'Set all files to Low']);
  sent.length = 0;
  allItems()[2].act();
  check('D: in one message for the whole folder', sent.map((m) => JSON.stringify(m)),
    [JSON.stringify({ type: 'setFilePriority', id: 80, gen: 3, ranges: [[0, 5]], priority: 'low' })]);
  selectDl(rowOf('d81'));
  check('D: not on a cancelled folder', allItems().length, 0);
  // The default order is the start order: a priority that changes moves the file (held like any
  // change from the data while the pointer is over the table).
  const order80 = () => keys().filter((k) => k.startsWith('f80:')).map((k) => +k.split(':')[1]);
  check('D: High first in the default order', order80(), [0, 1, 2, 3, 4, 5]);
  dlSort.over = true;
  dl.stores.get(80).at = -1e9;
  delta(80, [[5, FS.paused, 0, 0, null, 4], [1, FS.running, 20, 5, 9, 1]], 3);
  check('D: a new priority waits while the pointer is over the table', [order80(), dlSort.pending], [[0, 1, 2, 3, 4, 5], true]);
  dlSort.over = false;
  dl.keyAt = -1e9;
  dlRecheck();
  check('D: then the file moves: High, Normal, Low', order80(), [0, 5, 2, 3, 4, 1]);
  check('D: its select follows', [prioOf('f80:5'), prioOf('f80:1')], ['high', 'low']);
  // A narrow panel hides the column: the Status tooltip says it.
  const step = dl.step;
  dl.step = 3;
  check('D: in the Status tooltip when the column hides', fileView(dl.byId.get(80), dl.stores.get(80), 5).statusTip.includes('Priority: High'), true);
  dl.step = step;
  clearSel();
  folderOpen.clear();

  // ---- G: the total speed, its graph, Waiting files (DOWNLOADS-UI.md, chunk G) --------------------
  // The status bar's total is the backend's 'speed' (what arrives, all downloads together), not the
  // rows' speeds added up.
  dlSort.key = '';
  showDlOrder();
  state.limits.down.on = false;
  state.limits.up.on = false;
  state.queue = [folder({ id: 95, speed: 2 * 1048576, files: counts({ total: 4 }) }), file(96, { status: 'running', speed: 1048576 })];
  renderQueue();
  onSpeed({ type: 'speed', total: 5 * 1048576 });
  check('G: the total is the backend\'s, not the rows\' sum', $('#total-speed').textContent, '↓ 5.0 MiB/s total · 2 running');
  onSpeed({ type: 'speed', total: 512.4 });
  check('G: the total in the fixed format', $('#total-speed').textContent, '↓ 512 B/s total · 2 running');
  // The graph: the last 60 samples, scaled to their peak, before the text in a fixed box.
  speedHist.length = 0;
  upHist.length = 0;
  drawSpeedGraph();
  const graph = $('#speed-graph');
  const boxOf = (sel) => { const r = $(sel).getBoundingClientRect(); return [r.x, r.y, r.width, r.height].map((v) => Math.round(v * 10) / 10); };
  const idleBox = boxOf('#speed-graph');
  check('G: no samples: blank, its place kept', [graph.classList.contains('idle'), getComputedStyle(graph).visibility, idleBox[2], idleBox[3]], [true, 'hidden', 72, 16]);
  for (let k = 0; k < 70; ++k) { state.totalSpeed = (k % 10) * 100000; sampleSpeed(); }
  const pathOf = (cls) => graph.querySelector(cls).getAttribute('d');
  const ys = () => pathOf('.line').split(/[ML]/).filter(Boolean).map((p) => Number(p.split(' ')[1]));
  check('G: 60 samples, one point each', [speedHist.length, (pathOf('.line').match(/[ML]/g) || []).length], [60, 60]);
  check('G: shown, the newest on the right', [graph.classList.contains('idle'), pathOf('.line').startsWith('M0 '), /L59 [\d.]+$/.test(pathOf('.line'))], [false, true, true]);
  check('G: the peak at the top, nothing at the bottom', [Math.min(...ys()), Math.max(...ys())], [1, 15]);
  check('G: the peak in its tooltip', graph.title, 'Total speed over the last minute (peak 878.9 KiB/s)');
  check('G: drawn in the accent colour', getComputedStyle(graph.querySelector('.line')).stroke !== 'none', true);
  check('G: no uploads: no upload line, the tooltip as before', [pathOf('.up-line') || '', graph.title.startsWith('Total speed over')], ['', true]);
  const barBefore = [boxOf('#speed-graph'), boxOf('#limit-value'), boxOf('#limit-on')];
  onSpeed({ type: 'speed', total: 999.9 * 1073741824 });
  state.queue = Array.from({ length: 12 }, (_, k) => file(200 + k, { status: 'running' }));
  renderQueue();
  state.limits.down.on = true;
  renderTotalSpeed();
  const barAfter = [boxOf('#speed-graph'), boxOf('#limit-value'), boxOf('#limit-on')];
  check('G: a longer total moves nothing in the status bar', [$('#total-speed').textContent, $('#total-limit').textContent,
    JSON.stringify(barAfter) === JSON.stringify(barBefore)], ['↓ 999.9 GiB/s total · 12 running', `· ${fmtMiBps(state.limits.down.mibps)} limit`, true]);
  check('G: and the graph kept its box from when it was blank', JSON.stringify(idleBox) === JSON.stringify(barBefore[0]), true);
  state.limits.down.on = false;
  state.queue = [];
  renderQueue();
  check('G: none running', $('#total-speed').textContent, 'No downloads running');

  // ---- chunk 6 (UPLOADS.md 6.2): the totals of both directions, the graph's upload line ------------
  {
    const up = (id, more = {}) => file(id, { dir: 'up', status: 'running', path: `/srv/tv/u${id}.mkv`, name: `u${id}.mkv`,
      into: '/srv/tv', source: `E:\\Videos\\u${id}.mkv`, dest: '', local: `E:\\Videos\\u${id}.mkv`, ...more });
    const tot = $('#total-speed');
    const lim = $('#total-limit');
    const parts = () => [...tot.querySelectorAll('span')].map((s) => [s.textContent, s.classList.contains('limited')]);
    state.queue = [file(301, { status: 'running' }), up(302)];
    renderQueue();
    onSpeed({ type: 'speed', total: 11.9 * 1048576, up: 3.9 * 1048576 });
    check('6: an upload running: both totals in one unit, both counts', [tot.textContent, lim.hidden],
      ['↓ 11.9 · ↑ 3.9 MiB/s · 1 running · 1 uploading', true]);
    check('6: ... no limit: neither part marked', parts(), [['↓ 11.9', false], ['↑ 3.9', false]]);
    check('6: ... the tooltip in words', tot.title, 'Downloads: 11.9 MiB/s · Uploads: 3.9 MiB/s');
    state.limits.down = { on: true, mibps: 12, source: 'own' };
    state.limits.up = { on: true, mibps: 4, source: 'own' };
    renderTotalSpeed();
    check('6: each limit beside its total while on', [tot.textContent, parts(), lim.hidden],
      ['↓ 11.9 of 12 · ↑ 3.9 of 4 MiB/s · 1 running · 1 uploading', [['↓ 11.9 of 12', true], ['↑ 3.9 of 4', true]], true]);
    check('6: ... and in the tooltip', tot.title, 'Downloads: 11.9 MiB/s (limit 12 MiB/s) · Uploads: 3.9 MiB/s (limit 4 MiB/s)');
    state.limits.down = { on: false, mibps: 12, source: 'own' };
    renderTotalSpeed();
    check('6: only the upload limit: only its part marked', [tot.textContent, parts()],
      ['↓ 11.9 · ↑ 3.9 of 4 MiB/s · 1 running · 1 uploading', [['↓ 11.9', false], ['↑ 3.9 of 4', true]]]);
    onSpeed({ type: 'speed', total: 300, up: 2.5 * 1048576 });
    check('6: under a limit both in MiB/s (the limits\' unit)', tot.textContent, '↓ 0.0 · ↑ 2.5 of 4 MiB/s · 1 running · 1 uploading');
    state.limits.up = { on: false, mibps: 4, source: 'own' };
    state.queue = [up(302)];
    renderQueue();
    onSpeed({ type: 'speed', total: 0, up: 512 * 1024 });
    check('6: no limit: the larger one\'s unit; only uploads running', tot.textContent, '↓ 0.0 · ↑ 512.0 KiB/s · 1 uploading');
    state.queue = [];
    renderQueue();
    onSpeed({ type: 'speed', total: 0, up: 0 });
    check('6: nothing uploads, no upload limit: as before uploads came', [tot.textContent, lim.hidden, tot.hasAttribute('title')],
      ['No downloads running', true, false]);
    state.limits.up = { on: true, mibps: 4, source: 'own' };
    renderTotalSpeed();
    check('6: the upload limit on, nothing running: said after it', [tot.textContent, lim.hidden, lim.textContent],
      ['No downloads or uploads running', false, '· ↑ 4 MiB/s upload limit']);
    state.limits.down = { on: true, mibps: 12, source: 'own' };
    renderTotalSpeed();
    check('6: ... both limits on', lim.textContent, '· ↓ 12 · ↑ 4 MiB/s limits');
    state.limits.down = { on: false, mibps: 5 };
    state.limits.up = { on: false, mibps: 5 };
    renderTotalSpeed();
    check('6: both off again: as before', [tot.textContent, lim.hidden], ['No downloads running', true]);

    // The graph: an upload line beside the downloads' area, on one scale (the peak of both).
    speedHist.length = 0;
    upHist.length = 0;
    for (let k = 0; k < 60; ++k) {
      state.totalSpeed = 4 * 1048576;
      state.upSpeed = k === 30 ? 8 * 1048576 : 2 * 1048576;
      sampleSpeed();
    }
    const yOf = (cls) => pathOf(cls).split(/[ML]/).filter(Boolean).map((p) => Number(p.split(' ')[1]));
    check('6: the upload line: 60 points', [(pathOf('.up-line').match(/[ML]/g) || []).length, pathOf('.up-line').startsWith('M0 ')], [60, true]);
    check('6: one scale: the uploads\' peak at the top, the downloads at half', [Math.min(...yOf('.up-line')), Math.min(...yOf('.line'))], [1, 8]);
    check('6: the tooltip names both peaks', graph.title,
      'Total speeds over the last minute: ↓ downloads (peak 4.0 MiB/s), ↑ uploads (peak 8.0 MiB/s)');
    const stroke = (cls) => getComputedStyle(graph.querySelector(cls)).stroke;
    check('6: the upload line in a colour of its own', [stroke('.up-line') !== stroke('.line'), stroke('.up-line') !== 'none'], [true, true]);
    state.upSpeed = 0;
  }
  onSpeed({ type: 'speed', total: 3 * 1048576, up: 0 }); // for the screenshots

  // ---- the MiB/s boxes (↓ downloads', ↑ uploads': UPLOADS.md 6.2): typing never sets one; Enter or
  // leaving it does, confirmed. The same checks for each, then the two side by side.
  check('J: a speed read from the box', [limitValue('2').mibps, limitValue(' 2,5 ').mibps, limitValue('10000').mibps], [2, 2.5, 10000]);
  check('J: why one is refused', [limitValue('').why, limitValue('abc').why, limitValue('0.05').why, limitValue('20000').why, limitValue('800K').why],
    ['Type a speed in MiB/s, from 0.1 to 10000', '“abc” isn’t a number: type MiB/s, from 0.1 to 10000', 'At least 0.1 MiB/s',
      'At most 10000 MiB/s', '“800K” isn’t a number: type MiB/s, from 0.1 to 10000']);
  const limitBoxChecks = async (dir) => {
    const wait = (ms) => new Promise((r) => setTimeout(r, ms));
    const c = limitCtl[dir];
    const other = dir === 'up' ? 'down' : 'up';
    const K = dir === 'up' ? 'K ↑' : 'K ↓';
    const checkK = (name, got, want) => check(`${K}: ${name}`, got, want);
    const W = c.word; // Download | Upload
    const setText = (n) => `✓ ${W} limit · ${n} MiB/s`;
    const setSaid = (n) => `${W} speed limit set to ${n} MiB/s`;
    // The status bar's word for the limit when nothing runs: the download limit's as before
    // uploads came; the upload limit's marked.
    const barLimit = (n) => (dir === 'up' ? `· ↑ ${n} MiB/s upload limit` : `· ${n} MiB/s limit`);
    const box = c.box;
    const why = c.why;
    const tip = c.tip;
    const wrap = c.wrap;
    const badge = c.badge;
    const sets = () => sent.filter((m) => m.type === 'setLimit' && m.dir === dir);
    const limits = () => sets().map((m) => [m.on, m.mibps]);
    const type = (text) => { box.value = text; box.dispatchEvent(new Event('input', { bubbles: true })); };
    const key = (k) => box.dispatchEvent(new KeyboardEvent('keydown', { key: k, bubbles: true, cancelable: true }));
    // A "limit" message with this direction's `l`, the other's as it is.
    const limitMsg = (l, more = {}) => ({ type: 'limit', [dir]: l, [other]: state.limits[other], rules: state.schedule, ...more });
    // The backend's answer to the last set: the limit asked for, or what `got` says applies instead.
    const echo = (got = {}) => {
      const m = sets().pop();
      onLimit(limitMsg({ on: m.on, mibps: m.mibps, source: 'own', ...got }, { token: m.token, dir }));
    };
    const badgeView = () => [badge.classList.contains('show'), badge.querySelector('.shown').textContent,
      badge.querySelector('.said').textContent, wrap.classList.contains('set'), badge.classList.contains('off')];
    const idle = [false, '', '', false, false];
    const noBadge = () => endLimitBadge(c); // as at the end of its fade, for the next check
    const bar = () => JSON.stringify([boxOf(`#${box.id}`), boxOf(`#${c.sw.id}`), boxOf('#total-speed')]);
    state.limits[dir] = { on: true, mibps: 1, source: 'own' };
    renderLimit();
    box.focus();
    sent.length = 0;
    const barBefore = bar();
    checkK('idle: neither pending nor set, no badge', [wrap.classList.contains('pending'), tip.hidden, badgeView()], [false, true, idle]);

    // Typing only edits: pending, nothing sent, however long it waits.
    type('2');
    checkK('typed: nothing sent', limits(), []);
    checkK('pending: a dashed amber box, "Enter to set" above it',
      [wrap.classList.contains('pending'), tip.hidden, tip.textContent, getComputedStyle(box).borderTopStyle], [true, false, 'Enter to set', 'dashed']);
    const tipBox = tip.getBoundingClientRect();
    checkK('the hint above the box, within the window',
      [tipBox.bottom <= box.getBoundingClientRect().top, tipBox.right <= innerWidth, tipBox.left >= 0], [true, true, true]);
    checkK('the hint moves nothing in the status bar', bar() === barBefore, true);
    await wait(1700);
    checkK('nothing sent after a pause either (chunk J set it after 0.4 s, a lower one after 1.5 s)', [limits(), state.limits[dir].mibps, box.value],
      [[], 1, '2']);
    type('1');
    checkK('the speed that applies typed back: not pending', [wrap.classList.contains('pending'), tip.hidden], [false, true]);

    // Enter sets it, once; the badge comes with the backend's answer.
    type('12');
    key('Enter');
    checkK('Enter sends it, once', limits(), [[true, 12]]);
    checkK('... with a token', sets()[0].token > 0, true);
    checkK('the box shows it, left, not pending', [box.value, document.activeElement === box, wrap.classList.contains('pending'), tip.hidden],
      ['12', false, false, true]);
    checkK('no badge before the backend answers', badgeView(), idle);
    echo();
    checkK('the answer: the box flashes green and the badge says it', badgeView(),
      [true, setText(12), setSaid(12), true, false]);
    checkK('the badge is a polite live region', [badge.getAttribute('role'), badge.getAttribute('aria-live')], ['status', 'polite']);
    checkK('the status bar shows the limit', [$('#total-limit').hidden, $('#total-limit').textContent], [false, barLimit(12)]);
    checkK('the other box neither sends nor shows anything', [sent.some((m) => m.type === 'setLimit' && m.dir === other),
      limitCtl[other].badge.classList.contains('show'), limitCtl[other].wrap.classList.contains('set')], [false, false, false]);
    const badgeBox = badge.getBoundingClientRect();
    const valueBox = box.getBoundingClientRect();
    checkK('the badge above the box, not over it, within the window',
      [badgeBox.bottom <= valueBox.top, badgeBox.top >= 0, badgeBox.left >= 0, badgeBox.right <= innerWidth, badgeBox.width > 60],
      [true, true, true, true, true]);
    checkK('the badge and the flash move nothing in the status bar', bar() === barBefore, true);
    sent.length = 0;
    await wait(1000);
    checkK('a second later: still shown, the check mark in the box', [badgeView()[0], getComputedStyle(wrap.querySelector('.limit-check')).opacity], [true, '1']);
    await wait(1400);
    checkK('gone after about two seconds, the limit kept in the status bar', [...badgeView(), $('#total-limit').textContent],
      [...idle, barLimit(12)]);
    checkK('nothing more sent', limits(), []);

    // An answer with another limit than the one asked for, or to a set overtaken by a later one:
    // no badge.
    box.focus();
    type('7');
    key('Enter');
    echo({ mibps: 12 });
    checkK('the backend answered with another limit: no badge, the one that applies in the box', [badgeView(), box.value], [idle, '12']);
    box.focus();
    type('7');
    key('Enter');
    const first = sets().pop();
    box.focus();
    type('8');
    key('Enter');
    onLimit(limitMsg({ on: true, mibps: 7, source: 'own' }, { token: first.token, dir }));
    checkK('the answer to an earlier set, overtaken: no badge', badgeView(), idle);
    echo();
    checkK('the later one\'s: the badge', badgeView()[1], setText(8));

    // Another set while the badge shows: it says the new one, for its whole time again.
    await wait(1000);
    box.focus();
    type('9');
    key('Enter');
    echo();
    checkK('a set while the badge shows: the new one', badgeView(), [true, setText(9), setSaid(9), true, false]);
    await wait(1200);
    checkK('... shown its whole time again, not gone with the first one\'s', badgeView()[0], true);
    await wait(1200);
    checkK('... then gone', badgeView(), idle);

    // Leaving the box sets what it holds; unchanged, nothing. Enter on the speed that applies sets
    // it again, and confirms it.
    sent.length = 0;
    box.focus();
    type('4');
    box.blur();
    checkK('leaving the box sends it', [limits(), box.value, wrap.classList.contains('pending')], [[[true, 4]], '4', false]);
    echo();
    checkK('... confirmed the same way', badgeView()[1], setText(4));
    noBadge();
    sent.length = 0;
    box.focus();
    box.blur();
    box.focus();
    type('4.0');
    box.blur();
    checkK('leaving the box unchanged (or the same speed typed another way) sends nothing, shows nothing',
      [limits(), box.value, badgeView()], [[], '4', idle]);
    box.focus();
    key('Enter');
    checkK('Enter on the speed that applies sends it again', limits(), [[true, 4]]);
    echo();
    checkK('... and confirms it', badgeView(), [true, setText(4), setSaid(4), true, false]);
    noBadge();

    // Escape: nothing sent, the speed that applies back.
    sent.length = 0;
    box.focus();
    type('9');
    key('Escape');
    checkK('Escape sends nothing and puts back the speed that applies',
      [limits(), box.value, state.limits[dir].mibps, wrap.classList.contains('pending'), tip.hidden, document.activeElement === box, badgeView()],
      [[], '4', 4, false, true, false, idle]);

    // A value that isn't a speed: red and why, from a moment after typing stops; nothing sent.
    box.focus();
    type('abc');
    checkK('not a speed: not pending, nothing said at once', [wrap.classList.contains('pending'), why.hidden, tip.hidden], [false, true, true]);
    await wait(450);
    checkK('... said once typing stops: red, why above the box', [limits(), why.hidden, why.textContent, box.classList.contains('bad'),
      box.getAttribute('aria-invalid'), tip.hidden], [[], false, '“abc” isn’t a number: type MiB/s, from 0.1 to 10000', true, 'true', true]);
    checkK('the note moves nothing in the status bar', bar() === barBefore, true);
    checkK('the note above the box', why.getBoundingClientRect().bottom <= box.getBoundingClientRect().top, true);
    type('0');
    checkK('saying why already: the new reason at once', why.textContent, 'At least 0.1 MiB/s');
    type('6');
    checkK('a speed again: the note goes, pending instead',
      [why.hidden, box.classList.contains('bad'), wrap.classList.contains('pending'), tip.hidden], [true, false, true, false]);
    type('0');
    key('Enter');
    checkK('Enter with one refused: nothing sent, the speed that applies back, the note stays a while',
      [limits(), box.value, why.hidden, why.textContent, box.classList.contains('bad'), document.activeElement === box, badgeView()],
      [[], '4', false, 'At least 0.1 MiB/s', false, false, idle]);
    await wait(450);
    checkK('... and nothing sent later', limits(), []);
    showLimitWhy(c, '');

    // The switch: off, "Download limit off" (neutral, no flash); on, the speed kept, its badge.
    const sw = c.sw;
    sw.click();
    checkK('the switch off sends it', limits(), [[false, 4]]);
    checkK('... no badge before the answer', badgeView(), idle);
    echo();
    checkK('"... limit off", neutral, no flash', badgeView(), [true, `${W} limit off`, `${W} speed limit off`, false, true]);
    checkK('the status bar without a limit', $('#total-limit').hidden, true);
    sw.click();
    checkK('the switch on sends the speed kept', limits(), [[false, 4], [true, 4]]);
    echo();
    checkK('set, the box flashing', badgeView(), [true, setText(4), setSaid(4), true, false]);
    checkK('... the box focused by the switch', document.activeElement === box, true);
    checkK('... its text selected, so a speed typed replaces it', [box.value, box.selectionStart, box.selectionEnd], ['4', 0, 1]);
    type('6');
    checkK('typing then: the badge gives way to "Enter to set", nothing sent', [badgeView(), wrap.classList.contains('pending'), tip.hidden, limits()],
      [idle, true, false, [[false, 4], [true, 4]]]);
    key('Escape');
    sw.click();
    echo();
    sw.click(); // on again, the box focused; a value typed before the answer comes
    type('7');
    echo();
    checkK('the answer comes while a value typed waits: no badge over "Enter to set"', [badgeView(), tip.hidden, limits().length],
      [idle, false, 4]);
    key('Escape');

    // A change by the speed schedule: its own hint, no badge. In the box focused with nothing
    // typed (as by the switch) it shows too, and leaving the box doesn't set the speed shown before.
    box.focus();
    sent.length = 0;
    onLimit(limitMsg({ on: true, mibps: 3, source: 'schedule', until: '24:00' }));
    checkK('focused, nothing typed: a change by the schedule shows in the box', [document.activeElement === box, box.value], [true, '3']);
    box.blur();
    checkK('... and leaving the box sets nothing', limits(), []);
    checkK('a change by the schedule: no badge, its hint, the limit in the status bar',
      [badgeView(), $('#limit-hint').hidden, $('#limit-hint').textContent, $('#total-limit').textContent],
      [idle, false, dir === 'up' ? '↑ Scheduled until 24:00' : 'Scheduled until 24:00', barLimit(3)]);

    // A speed the box shows rounded (2.55 as '2.6'): Enter, nothing typed, sets it exactly (no
    // override of the schedule by '2.6'); '2.6' typed is another speed, set on leaving.
    state.limits[dir].mibps = 2.55;
    renderLimit();
    sent.length = 0;
    box.focus();
    checkK('2.55 shown rounded', box.value, '2.6');
    key('Enter');
    checkK('Enter, nothing typed: the speed that applies, exactly', limits(), [[true, 2.55]]);
    echo();
    noBadge();
    box.focus();
    type('2.6');
    checkK('2.6 typed over 2.55: pending', wrap.classList.contains('pending'), true);
    box.blur();
    checkK('... and set on leaving', limits(), [[true, 2.55], [true, 2.6]]);
    echo();
    noBadge();

    // The window losing focus (Alt+Tab) blurs the box but leaves it the active element: nothing
    // set, still pending; leaving it then sets it.
    sent.length = 0;
    box.focus();
    type('1');
    box.dispatchEvent(new FocusEvent('blur'));
    checkK('the window left while typing: nothing sent, still pending', [limits(), document.activeElement === box,
      wrap.classList.contains('pending'), box.value], [[], true, true, '1']);
    box.blur();
    checkK('... leaving the box then sets it', limits(), [[true, 1]]);
    echo();
    noBadge();

    // Another limit by the schedule while the badge shows: the badge goes (it would say a limit
    // that no longer applies); the same limit again (only its 'until' changing) keeps it.
    box.focus();
    type('12');
    key('Enter');
    echo();
    onLimit(limitMsg({ on: true, mibps: 12, source: 'own', until: '23:00' }));
    checkK('the same limit again, no token: the badge stays', badgeView()[1], setText(12));
    onLimit(limitMsg({ on: true, mibps: 12, source: 'own', until: '22:00' }, { token: 999, dir: other }));
    checkK('the other direction\'s answer: the badge stays', badgeView()[1], setText(12));
    onLimit(limitMsg({ on: true, mibps: 3, source: 'schedule', until: '24:00' }));
    checkK('another limit by the schedule: the badge gone at once', badgeView(), idle);
    sent.length = 0;
    showLimitWhy(c, '');
    state.limits[dir] = { on: false, mibps: 5 };
    renderLimit();
  };
  await limitBoxChecks('down');
  await limitBoxChecks('up');

  // The two side by side: each its own set, token and badge; an answer for one direction never
  // confirms the other's set, even with the same token.
  {
    const d = limitCtl.down;
    const u = limitCtl.up;
    const badgeOn = (c) => c.badge.classList.contains('show');
    const msg = (more) => ({ type: 'limit', down: state.limits.down, up: state.limits.up, rules: state.schedule, ...more });
    state.limits.down = { on: true, mibps: 12, source: 'own' };
    state.limits.up = { on: true, mibps: 4, source: 'own' };
    renderLimit();
    check('6: both boxes show their own limit', [d.box.value, u.box.value, d.sw.checked, u.sw.checked], ['12', '4', true, true]);
    sent.length = 0;
    d.token = 41;
    u.token = 41;
    d.box.focus();
    d.box.value = '10';
    d.box.dispatchEvent(new Event('input', { bubbles: true }));
    d.box.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true, cancelable: true }));
    u.box.focus();
    u.box.value = '3';
    u.box.dispatchEvent(new Event('input', { bubbles: true }));
    u.box.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true, cancelable: true }));
    check('6: each box sends its own direction', sent.filter((m) => m.type === 'setLimit').map((m) => [m.dir, m.on, m.mibps, m.token]),
      [['down', true, 10, 42], ['up', true, 3, 42]]);
    state.limits.up = { on: true, mibps: 3, source: 'own' };
    onLimit(msg({ down: { on: true, mibps: 12, source: 'own' }, token: 42, dir: 'up' }));
    check('6: the upload answer (the same token): only the upload badge', [badgeOn(d), badgeOn(u), u.badge.querySelector('.shown').textContent],
      [false, true, '✓ Upload limit · 3 MiB/s']);
    onLimit(msg({ down: { on: true, mibps: 10, source: 'own' }, token: 42, dir: 'down' }));
    check('6: then the download answer: its own badge, the upload one stays', [badgeOn(d), badgeOn(u), d.badge.querySelector('.shown').textContent],
      [true, true, '✓ Download limit · 10 MiB/s']);
    // The schedule: one hint for both, marked when they differ.
    onLimit(msg({ down: { on: true, mibps: 5, source: 'schedule', until: '24:00' }, up: { on: true, mibps: 2, source: 'schedule', until: '24:00' } }));
    check('6: both scheduled alike: one hint', $('#limit-hint').textContent, 'Scheduled until 24:00');
    onLimit(msg({ down: { on: true, mibps: 5, source: 'schedule', until: '24:00' }, up: { on: false, mibps: 2, source: 'change', until: '21:30' } }));
    check('6: apart: each marked', $('#limit-hint').textContent, '↓ Scheduled until 24:00 · ↑ Schedule resumes at 21:30');
    check('6: ... its tooltip in words', $('#limit-hint').title,
      'Speed schedule (Settings)\nDownloads: Scheduled until 24:00\nUploads: Schedule resumes at 21:30');
    onLimit(msg({ down: { on: false, mibps: 12, source: 'own' }, up: { on: false, mibps: 4, source: 'own' } }));
    check('6: no schedule: no hint', $('#limit-hint').hidden, true);
    endLimitBadge(d);
    endLimitBadge(u);

    // At the narrowest window (760 DIP, about 744 CSS px inside; at 150 % the same CSS px) with both
    // limits on, both directions moving, a long total and the schedule's hint for both: the bar on
    // one line, nothing past its edge, the controls where they were whatever the totals say.
    const footer = document.querySelector('footer.bar.status');
    const widthWas = footer.style.width;
    footer.style.width = '740px';
    state.limits.down = { on: true, mibps: 10000, source: 'change', until: 'Sat 06:00' };
    state.limits.up = { on: true, mibps: 2.5, source: 'schedule', until: '24:00' };
    state.queue = [file(311, { status: 'running' }), file(312, { status: 'running', dir: 'up', into: '/srv', source: 'E:\\u', dest: '', local: 'E:\\u' })];
    renderQueue();
    renderLimit();
    onSpeed({ type: 'speed', total: 9999.9 * 1048576, up: 2.5 * 1048576 });
    const kids = () => [...footer.children].filter((e) => !e.hidden && getComputedStyle(e).display !== 'none');
    const fr = footer.getBoundingClientRect();
    const rects = () => kids().map((e) => e.getBoundingClientRect());
    const mid = (r) => r.top + r.height / 2;
    const ctlBoxes = () => JSON.stringify([d.box, u.box, d.sw, u.sw].map((e) => boxOf(`#${e.id}`)));
    check('6 narrow: nothing past the bar', [footer.scrollWidth <= footer.clientWidth, rects().every((r) => r.left >= fr.left - 0.5 && r.right <= fr.right + 0.5)],
      [true, true]);
    const mids = rects().filter((r) => r.height > 0).map(mid);
    check('6 narrow: one line', Math.max(...mids) - Math.min(...mids) <= 1, true);
    check('6 narrow: the hint shows', [$('#limit-hint').hidden, $('#limit-hint').getBoundingClientRect().width > 20], [false, true]);
    out.push(`INFO status bar at 740: ${JSON.stringify(kids().map((e) => [e.id || e.className, Math.round(e.getBoundingClientRect().width)]))}`);
    const before = ctlBoxes();
    onSpeed({ type: 'speed', total: 1, up: 2 });
    check('6 narrow: the totals changing move no control', ctlBoxes() === before, true);
    check('6 narrow: the badge within the window', (() => {
      showLimitSet(u, 2.5);
      const b = u.badge.getBoundingClientRect();
      endLimitBadge(u);
      return [b.right <= fr.right + 0.5, b.left >= 0];
    })(), [true, true]);
    // Downloads only (the form from before uploads), its limit after the total: the same.
    state.limits.up = { on: false, mibps: 2.5, source: 'own' };
    state.queue = Array.from({ length: 12 }, (_, k) => file(320 + k, { status: 'running' }));
    renderQueue();
    renderLimit();
    onSpeed({ type: 'speed', total: 999.9 * 1073741824, up: 0 });
    const narrowDown = ctlBoxes();
    check('6 narrow, downloads only: nothing past the bar, the total in its tooltip',
      [footer.scrollWidth <= footer.clientWidth, rects().every((r) => r.left >= fr.left - 0.5 && r.right <= fr.right + 0.5),
        $('#total-speed').title], [true, true, 'Downloads: 999.9 GiB/s (limit 10000 MiB/s) · 12 running']);
    onSpeed({ type: 'speed', total: 5, up: 0 });
    check('6 narrow, downloads only: the total changing moves no control', ctlBoxes() === narrowDown, true);
    out.push(`INFO status bar at 740, downloads only: ${JSON.stringify(kids().map((e) => [e.id || e.className, Math.round(e.getBoundingClientRect().width)]))}`);
    // Nothing running, both limits on and the hint marked for each: the idle text with both limits
    // gives way too (review, chunk 6: it ran 24 px past the window), and the controls stay put
    // whatever the values.
    const fits = () => [footer.scrollWidth <= footer.clientWidth, rects().every((r) => r.left >= fr.left - 0.5 && r.right <= fr.right + 0.5)];
    const idle = (down, up) => {
      state.limits.down = { on: true, mibps: down, source: 'change', until: 'Sat 06:00' };
      state.limits.up = { on: true, mibps: up, source: 'schedule', until: 'Sun 23:00' };
      renderLimit();
      onSpeed({ type: 'speed', total: 0, up: 0 });
    };
    state.queue = [];
    renderQueue();
    idle(2.5, 1000);
    check('6 narrow, nothing running, both limits: nothing past the bar', fits(), [true, true]);
    const idleBoxes = ctlBoxes();
    idle(5, 2);
    check('6 narrow, nothing running, both limits: other values move no control', [fits(), ctlBoxes() === idleBoxes], [[true, true], true]);
    // The upload limit alone, changed during its schedule.
    state.limits.down = { on: false, mibps: 5, source: 'own' };
    state.limits.up = { on: true, mibps: 10, source: 'change', until: 'Sat 06:00' };
    renderLimit();
    onSpeed({ type: 'speed', total: 0, up: 0 });
    check('6 narrow, nothing running, the upload limit alone: nothing past the bar', [fits(), $('#limit-hint').hidden], [[true, true], false]);
    out.push(`INFO status bar at 740, nothing running: ${JSON.stringify(kids().map((e) => [e.id || e.className, Math.round(e.getBoundingClientRect().width)]))}`);
    footer.style.width = widthWas;
    state.queue = [];
    renderQueue();
    state.limits.down = { on: false, mibps: 5 };
    state.limits.up = { on: false, mibps: 5 };
    renderLimit();
    sent.length = 0;
  }

  // ---- chunk 6 (UPLOADS.md 6.3): the speed schedule's ↓ and ↑ boxes -------------------------------
  {
    const all = ['mon', 'tue', 'wed', 'thu', 'fri', 'sat', 'sun'];
    const own = { on: false, mibps: 5, source: 'own' };
    const schedules = () => sent.filter((m) => m.type === 'setSchedule').map((m) => JSON.parse(JSON.stringify(m.rules)));
    const rows = () => [...document.querySelectorAll('#schedule-rules .rule')];
    const speeds = () => rows().map((r) => [r.querySelector('[data-field=mibps]').value, r.querySelector('[data-field=upMiBps]').value]);
    const whyOf = (i) => [rows()[i].querySelector('.rule-why').hidden, rows()[i].querySelector('.rule-why').textContent];
    const edit = (i, field, text, how = 'change') => {
      const input = rows()[i].querySelector(`[data-field=${field}]`);
      input.value = text;
      if (how === 'change') input.dispatchEvent(new Event('change', { bubbles: true }));
      else input.dispatchEvent(new KeyboardEvent('keydown', { key: how, bubbles: true, cancelable: true }));
      return input;
    };
    sent.length = 0;
    onLimit({ type: 'limit', down: own, up: own, rules: [
      { days: all, from: '18:00', to: '00:00', mibps: 5 },
      { days: ['sat', 'sun'], from: '00:00', to: '00:00', upMiBps: 2 },
      { days: ['mon'], from: '20:00', to: '22:00', mibps: 3, upMiBps: 1 },
    ] });
    check('6 schedule: a ↓ and an ↑ box per time, blank where it doesn\'t limit', speeds(), [['5', ''], ['', '2'], ['3', '1']]);
    check('6 schedule: the boxes marked ↓ and ↑', [...rows()[0].querySelectorAll('.rule-dir')].map((e) => e.textContent), ['↓', '↑']);
    check('6 schedule: ... and named', [...rows()[0].querySelectorAll('input.num')].map((e) => e.getAttribute('aria-label')),
      ['Download speed in MiB/s', 'Upload speed in MiB/s']);
    edit(0, 'upMiBps', '4');
    check('6 schedule: an ↑ speed typed: sent beside the ↓ one', schedules().pop()[0], { days: all, from: '18:00', to: '00:00', mibps: 5, upMiBps: 4 });
    edit(0, 'mibps', '');
    check('6 schedule: ↓ blanked: downloads not limited by it (no speed sent)', schedules().pop()[0], { days: all, from: '18:00', to: '00:00', upMiBps: 4 });
    check('6 schedule: ... its box blank', speeds()[0], ['', '4']);
    sent.length = 0;
    const lone = edit(1, 'upMiBps', '');
    check('6 schedule: the only speed blanked: refused, why said, nothing sent', [lone.value, schedules().length, ...whyOf(1)],
      ['2', 0, false, 'A time needs a speed: ↓ for downloads, ↑ for uploads, or both']);
    const bad = edit(2, 'upMiBps', '0');
    check('6 schedule: not a speed: refused with why', [bad.value, schedules().length, ...whyOf(2)], ['1', 0, false, 'At least 0.1 MiB/s']);
    edit(2, 'upMiBps', '9', 'Escape');
    check('6 schedule: Escape puts it back, sends nothing', [speeds()[2], schedules().length], [['3', '1'], 0]);
    edit(1, 'mibps', '2,5', 'Enter');
    check('6 schedule: Enter sets ↓ on an ↑-only time', [schedules().pop()[1], whyOf(1)],
      [{ days: ['sat', 'sun'], from: '00:00', to: '00:00', upMiBps: 2, mibps: 2.5 }, [true, '']]);
    sent.length = 0;
    $('#schedule-add').click();
    check('6 schedule: a new time limits downloads at 5 MiB/s, as before', [speeds().pop(), schedules().pop().pop()],
      [['5', ''], { days: all, from: '18:00', to: '00:00', mibps: 5 }]);
    // The backend's echo: the same rules aren't drawn again (a field being edited keeps what it
    // holds); rules that differ only in an upload speed are.
    const one = (up) => [{ days: all, from: '18:00', to: '00:00', mibps: 5, upMiBps: up }];
    onLimit({ type: 'limit', down: own, up: own, rules: one(1) });
    rows()[0].querySelector('[data-field=upMiBps]').value = '1.5'; // typed, not set yet
    onLimit({ type: 'limit', down: own, up: own, rules: one(1) });
    check('6 schedule: the same rules again: not drawn again', speeds(), [['5', '1.5']]);
    onLimit({ type: 'limit', down: own, up: own, rules: one(7) });
    check('6 schedule: only an ↑ speed differs: drawn again', speeds(), [['5', '7']]);

    // "Now": with rules for downloads only, as before; otherwise each direction marked.
    const now = (rules, down, up) => {
      onLimit({ type: 'limit', down, up, rules });
      return $('#schedule-now').textContent;
    };
    const downRule = { days: all, from: '18:00', to: '00:00', mibps: 5 };
    const upRule = { days: ['sat'], from: '21:00', to: '23:00', upMiBps: 2 };
    check('6 schedule: "Now", downloads only: as before', now([downRule], { on: true, mibps: 5, source: 'schedule', until: '24:00' }, own),
      'Now: 5 MiB/s until 24:00');
    check('6 schedule: "Now" with upload speeds: each direction',
      now([downRule, upRule], { on: true, mibps: 5, source: 'schedule', until: '24:00' }, { ...own, until: '21:00', nextMiBps: 2 }),
      '↓ Now: 5 MiB/s until 24:00 · ↑ No scheduled limit now; next: 21:00 at 2 MiB/s');
    check('6 schedule: "Now", uploads only', now([upRule], own, { on: true, mibps: 2, source: 'schedule', until: '23:00' }),
      '↑ Now: 2 MiB/s until 23:00');
    onLimit({ type: 'limit', down: own, up: own, rules: [] });
    check('6 schedule: no rules, no "Now"', [rows().length, $('#schedule-now').textContent], [0, '']);
    state.limits.down = { on: false, mibps: 5 };
    state.limits.up = { on: false, mibps: 5 };
    renderLimit();
    sent.length = 0;
  }

  // A big file that has its place but waits for the folder's small files: Waiting, muted, why in
  // its tooltip; it can be paused or skipped as a queued one; it sorts after running files.
  const waitKids = [{ path: 'big1.bin', size: 5e7, status: 'waiting', bytes: 0 }, { path: 'big2.bin', size: 6e7, status: 'queued', bytes: 0 },
    { path: 's.txt', size: 900, status: 'running', bytes: 100 }];
  state.queue = [folder({ id: 97, files: counts({ total: 3, queued: 2, running: 1, held: 1 }) })];
  renderQueue();
  feed(97, waitKids);
  const v97 = (i) => fileView(dl.byId.get(97), dl.stores.get(97), i);
  const why = 'Waits for the folder’s small files: they use its connections; big files follow';
  check('G: Waiting, muted, why in its tooltip', [v97(0).word, v97(0).wordCls, v97(0).statusTip], ['Waiting', 'muted', why]);
  check('G: on its row', [el('f97:0').querySelector('.dl-word').textContent, el('f97:0').querySelector('.dl-word').className], ['Waiting', 'dl-word muted']);
  check('G: no speed or time left while it waits', [v97(0).speed, v97(0).eta], ['', '']);
  check('G: Pause and Skip, as a queued file', fileActs(dl.byId.get(97), dl.stores.get(97), 0).map((s) => s && s[0]), ['pause', 'skip']);
  check('G: one queued for At once stays Queued', v97(1).word, 'Queued');
  dl.show = 'unfinished';
  check('G: Unfinished shows it', fileMatches(dl.stores.get(97), 0), true);
  dl.show = 'all';
  head('status').click();
  check('G: by Status: running, then waiting, then queued', keys().filter((k) => k.startsWith('f97:')), ['f97:2', 'f97:0', 'f97:1']);
  head('status').click();
  head('status').click();
  check('G: the folder\'s Status tooltip says why while it is so',
    dlView(dl.byId.get(97)).statusTip.includes('A big file waits for the folder’s small files: they use its connections; big files follow'), true);
  check('G: two of them', dlView(folder({ files: counts({ held: 2 }) })).statusTip.includes('2 big files wait for the folder’s small files'), true);
  check('G: not while the folder is paused', dlView(folder({ status: 'paused', files: counts({ held: 2 }) })).statusTip.includes('small files'), false);
  delta(97, [[0, FS.running, 1048576, 2097152, 23, 2]]);
  check('G: reading: Downloading, with its speed', [v97(0).word, v97(0).speed], ['Downloading', '2.0 MiB/s']);
  folderOpen.clear();

  // ---- H: opening finished files (DOWNLOADS-UI.md, chunk H) ----------------------------------------
  // A double-click, the menu's Open and Ctrl+Enter open a finished file in its app, by the row's id
  // (and its folder's generation and index); nothing unfinished opens. A finished folder download's
  // menu has Open folder; a double-click on a folder's row does what a click does. A program is
  // asked about first: Run or Cancel, the focus on Cancel, Esc cancels.
  dlSort.key = '';
  showDlOrder();
  clearSel();
  dl.cursor = '';
  const openKids = [
    { path: 'notes.txt', size: 5, status: 'done', bytes: 5 },
    { path: 'photo.jpg', size: 9, status: 'running', bytes: 3 },
    { path: 'run.bat', size: 7, status: 'done', bytes: 7 },
    { path: 'gone.bin', size: 9, status: 'failed', bytes: 0, error: 'permission denied' },
    { path: 'big.bin', size: 9, status: 'queued', bytes: 0 },
    { path: 'skip.bin', size: 9, status: 'skipped', bytes: 0 },
  ];
  state.queue = [
    folder({ id: 70, gen: 5, files: counts({ total: 6, queued: 1, running: 1, done: 2, failed: 1, skipped: 1 }) }),
    file(71, { status: 'done', total: 5, bytes: 5 }),
    file(72, { status: 'running', total: 50, bytes: 5 }),
    folder({ id: 73, gen: 2, status: 'done', expanded: false, files: counts({ total: 1, queued: 0, done: 1 }) }),
    file(74, { status: 'failed', total: 5, bytes: 0, error: 'boom' }),
    file(75, { status: 'cancelled' }),
    file(76, { status: 'skipped', total: 5, bytes: 0 }),
  ];
  renderQueue();
  onFiles({ type: 'files', id: 70, gen: 5, from: 0, total: 6, rows: openKids.map(kidRow) });
  // A press, its release and click, as the mouse makes them; `detail` is the click count.
  const press = (target, detail, mods = {}) => {
    const r = target.getBoundingClientRect();
    const at = { bubbles: true, cancelable: true, button: 0, detail, clientX: r.x + 4, clientY: r.y + 4, ...mods };
    target.dispatchEvent(new MouseEvent('mousedown', at));
    target.dispatchEvent(new MouseEvent('mouseup', at));
    target.dispatchEvent(new MouseEvent('click', at));
  };
  const dblclick = (k, sel = '.dl-text', mods = {}) => {
    const target = el(k).querySelector(sel);
    press(target, 1, mods);
    press(target, 2, mods);
  };
  const opens = () => sent.filter((m) => m.type === 'openFile' || m.type === 'openDir').map((m) => JSON.stringify(m));
  const openFile70 = (index) => JSON.stringify({ type: 'openFile', id: 70, gen: 5, index });
  check('H: the rows', keys(), ['d70', 'f70:0', 'f70:1', 'f70:2', 'f70:3', 'f70:4', 'f70:5', 'd71', 'd72', 'd73', 'd74', 'm74:error', 'd75', 'd76']);
  sent.length = 0;
  dblclick('f70:0');
  check('H: a double-click opens a finished file of a folder, by its index', opens(), [openFile70(0)]);
  check('H: and selects it, as a click', selKeys(), ['f70:0']);
  sent.length = 0;
  dblclick('d71');
  check('H: a finished file download, by its id', opens(), [JSON.stringify({ type: 'openFile', id: 71 })]);
  sent.length = 0;
  for (const k of ['f70:1', 'f70:3', 'f70:4', 'f70:5', 'd70', 'd72', 'd73', 'd74', 'd75', 'd76']) dblclick(k);
  check('H: nothing else opens: running, failed, queued, skipped, cancelled, a folder (finished too)', opens(), []);
  check('H: a double-click there is a click', selKeys(), ['d76']);
  sent.length = 0;
  dblclick('f70:0', '.dl-text', { ctrlKey: true });
  dblclick('f70:0', '.dl-text', { shiftKey: true });
  check('H: not with Ctrl or Shift (they select)', opens(), []);
  sent.length = 0;
  dblclick('f70:0', '[data-act="show"]');
  check('H: on a row\'s button, its button acts', [opens(), sent.filter((m) => m.type === 'showFile').length], [[], 2]);
  // A single click opens nothing.
  sent.length = 0;
  press(el('f70:2').querySelector('.dl-text'), 1);
  check('H: a click opens nothing', opens(), []);

  // The menu: Open first on one finished file, with its key; Open folder on a finished folder.
  const labels = () => dlMenu().filter((m) => m.label).map((m) => m.label);
  selectDl(rowOf('f70:0'));
  check('H: a finished file\'s menu: Open first', [labels()[0], dlMenu()[0].key, labels().includes('Show in folder')], ['Open', 'Ctrl+Enter', true]);
  sent.length = 0;
  dlMenu()[0].act();
  check('H: Open opens it', opens(), [openFile70(0)]);
  for (const k of ['f70:1', 'f70:3', 'f70:4', 'f70:5']) {
    selectDl(rowOf(k));
    check(`H: no Open on an unfinished file (${k})`, labels().includes('Open'), false);
  }
  selectDl(rowOf('f70:0'));
  selectDl(rowOf('f70:2'), { extend: true });
  check('H: no Open with several selected', labels().includes('Open'), false);
  selectDl(rowOf('d71'));
  check('H: a finished file download: Open first', [labels()[0], dlMenu()[0].key], ['Open', 'Ctrl+Enter']);
  sent.length = 0;
  dlMenu()[0].act();
  check('H: by its id', opens(), [JSON.stringify({ type: 'openFile', id: 71 })]);
  selectDl(rowOf('d73'));
  check('H: a finished folder download: Open folder first', labels()[0], 'Open folder');
  sent.length = 0;
  dlMenu()[0].act();
  check('H: its folder in Explorer', opens(), [JSON.stringify({ type: 'openDir', id: 73 })]);
  for (const k of ['d70', 'd72', 'd74', 'd75', 'd76']) {
    selectDl(rowOf(k));
    check(`H: no Open on an unfinished download (${k})`, labels().filter((l) => l.startsWith('Open')), []);
  }
  selectDl(rowOf('d71'));
  selectDl(rowOf('d73'), { extend: true });
  check('H: no Open with several downloads selected', labels().filter((l) => l.startsWith('Open')), []);

  // The keys: Ctrl+Enter opens the cursor's finished row; Enter still shows it in its folder; the
  // menu key's menu has Open.
  clearSel();
  grid.focus();
  setDlCursor(rowOf('f70:0'));
  sent.length = 0;
  key('Enter', { ctrlKey: true });
  check('H: Ctrl+Enter opens the cursor\'s finished file', opens(), [openFile70(0)]);
  sent.length = 0;
  key('Enter');
  check('H: Enter still shows it in its folder', sent.map((m) => `${m.type}:${m.index}`), ['showFile:0']);
  sent.length = 0;
  for (const k of ['f70:1', 'f70:3', 'd72', 'd74']) {
    setDlCursor(rowOf(k));
    key('Enter', { ctrlKey: true });
  }
  check('H: Ctrl+Enter on unfinished rows: nothing', sent.length, 0);
  setDlCursor(rowOf('d73'));
  key('Enter', { ctrlKey: true });
  check('H: Ctrl+Enter on a finished folder opens its folder', opens(), [JSON.stringify({ type: 'openDir', id: 73 })]);
  clearSel();
  setDlCursor(rowOf('f70:2'));
  key('ContextMenu');
  const firstItem = () => $('#menu').querySelector('button .text').textContent;
  check('H: the menu key: Open first', [$('#menu').hidden, firstItem()], [false, 'Open']);
  closeMenu();
  clearSel();
  setDlCursor(rowOf('f70:1'));
  key('F10', { shiftKey: true });
  check('H: Shift+F10 on an unfinished file: no Open', [$('#menu').hidden, [...$('#menu').querySelectorAll('button .text')].some((t) => t.textContent === 'Open')], [false, false]);
  closeMenu();

  // A program is asked about first (the backend's 'openAsk'): Run or Cancel, the focus on Cancel.
  const ask70 = { type: 'openAsk', id: 70, gen: 5, index: 2, name: 'run.bat', server: 'Main' };
  sent.length = 0;
  onOpenAsk(ask70);
  check('H: the question', [$('#modal').hidden, $('#modal-title').textContent, $('#modal-body').textContent, $('#modal-ok').textContent,
    $('#modal-cancel').textContent], [false, 'Run “run.bat”?', 'It came from Main.', 'Run', 'Cancel']);
  check('H: the focus on Cancel', document.activeElement === $('#modal-cancel'), true);
  key('Enter', { ctrlKey: true }); // the table's keys wait while it is open
  document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }));
  check('H: Esc cancels: nothing runs', [$('#modal').hidden, sent.length], [true, 0]);
  onOpenAsk(ask70);
  $('#modal-cancel').click();
  check('H: Cancel: nothing runs', [$('#modal').hidden, sent.length], [true, 0]);
  onOpenAsk(ask70);
  $('#modal-ok').click();
  check('H: Run: the same row again, to run', sent.map((m) => JSON.stringify(m)), [JSON.stringify({ type: 'openFile', id: 70, gen: 5, index: 2, run: true })]);
  sent.length = 0;
  onOpenAsk({ type: 'openAsk', id: 71, name: 'setup.exe', server: 'Box' });
  check('H: a file download\'s question', $('#modal-body').textContent, 'It came from Box.');
  $('#modal-ok').click();
  check('H: run by its id', sent.map((m) => JSON.stringify(m)), [JSON.stringify({ type: 'openFile', id: 71, run: true })]);
  onOpenAsk({ type: 'openAsk', id: 71, name: 'invoice‮fdp.exe', server: '<b>x</b>' });
  check('H: a name that would read backwards shows its mark; the server is text',
    [$('#modal-title').textContent, $('#modal-body').textContent], ['Run “invoice⟨U+202E⟩fdp.exe”?', 'It came from <b>x</b>.']);
  closeModal(false);
  showModal({ title: 'Another question', html: '', ok: 'OK' });
  onOpenAsk(ask70);
  check('H: never over another question', $('#modal-title').textContent, 'Another question');
  closeModal(false);
  // A file download Skip ended by keeping the user's own file: done, but grab wrote nothing there.
  state.queue.push(file(77, { status: 'done', total: 5, bytes: 0, note: 'skipped: your file is kept', kept: true }));
  renderQueue();
  sent.length = 0;
  dblclick('d77');
  selectDl(rowOf('d77'));
  check('H: a file Skip kept: no Open, a double-click sends nothing', [labels().filter((l) => l.startsWith('Open')), opens()], [[], []]);
  grid.focus();
  setDlCursor(rowOf('d77'));
  key('Enter', { ctrlKey: true });
  check('H: nor Ctrl+Enter', opens(), []);
  clearSel();
  dl.cursor = '';
  folderOpen.clear();

  // ---- C: performance (DOWNLOADS-UI.md 14, C) ----------------------------------------------------
  const synth = (n, from, count) => Array.from({ length: count }, (_, j) => {
    const k = from + j;
    return [`dir${String(Math.floor(k / 1000)).padStart(3, '0')}/file${String(k).padStart(6, '0')}.bin`, 4096 + ((k * 7919) % 1048576), 0, 0, 2];
  });
  const median = (a) => a.slice().sort((x, y) => x - y)[Math.floor(a.length / 2)];
  const p95 = (a) => a.slice().sort((x, y) => x - y)[Math.min(a.length - 1, Math.floor(a.length * 0.95))];
  const ms = (x) => Math.round(x * 10) / 10;
  const perf = {};
  // 10,000 files: opening the folder to its first paint, in two chunks as the backend sends them.
  state.queue = [folder({ id: 30, expanded: false, files: counts({ total: 10000 }) })];
  folderOpen.clear();
  renderQueue();
  await frame();
  let t0 = performance.now();
  setFolderOpen(dl.byId.get(30), true);
  onFiles({ type: 'files', id: 30, gen: 1, from: 0, total: 10000, rows: synth(10000, 0, 5000) });
  onFiles({ type: 'files', id: 30, gen: 1, from: 5000, total: 10000, rows: synth(10000, 5000, 5000) });
  await frame();
  perf.open10k = ms(performance.now() - t0);
  check('C perf: 10,000 files open to the first paint <= 150 ms', perf.open10k <= 150, true);
  check('C perf: DOM rows <= 150', dl.pool.length <= DL_POOL, true);
  // A scroll from the top to the bottom, a step per frame: the work per frame and the frames.
  const work = [];
  const gaps = [];
  let last = performance.now();
  for (let y = 0; y <= body.scrollHeight; y += 100 * DL_ROW) {
    const w0 = performance.now();
    body.scrollTop = y;
    renderDlBody();
    work.push(performance.now() - w0);
    await frame();
    const now = performance.now();
    gaps.push(now - last);
    last = now;
  }
  perf.scrollWorkP95 = ms(p95(work));
  perf.scrollFrameP95 = ms(p95(gaps.slice(1)));
  check('C perf: scroll work per frame p95 <= 16 ms', perf.scrollWorkP95 <= 16, true);
  body.scrollTop = 0;
  t0 = performance.now();
  head('size').click();
  perf.sortSize10k = ms(performance.now() - t0);
  check('C perf: a sort of 10,000 by size <= 100 ms', perf.sortSize10k <= 100, true);
  head('size').click();
  head('size').click();
  // 8 files running and 300 landing a second, 4 ticks a second for 5 s: the page's time in them.
  let busy = 0;
  let biggest = 0;
  const run = Array.from({ length: 8 }, (_, k) => 9000 + k);
  t0 = performance.now();
  for (let tick = 0; tick < 20; ++tick) {
    const rows = run.map((i) => [i, FS.running, tick * 100000, 4194304, 30 - tick, 2]);
    for (let k = 0; k < 75; ++k) rows.push([tick * 75 + k, FS.done, 5000, 0, null, 2]);
    const msg = { type: 'fileDelta', id: 30, gen: 1, rows };
    biggest = Math.max(biggest, JSON.stringify(msg).length);
    const q = folder({ id: 30, bytes: tick * 1e6, total: 1e9, speed: 4e7, files: counts({ total: 10000, done: tick * 75, running: 8 }) });
    const w0 = performance.now();
    onQueueDelta({ type: 'queueDelta', items: [q], parallel: 4 });
    onFileDelta(msg);
    refreshFiles();
    busy += performance.now() - w0;
    await new Promise((r) => setTimeout(r, 250));
  }
  perf.cpuPct = ms((100 * busy) / (performance.now() - t0));
  perf.deltaBytes = biggest;
  check('C perf: page time with 8 running and 300 landing a second <= 5% of a core', perf.cpuPct <= 5, true);
  check('C perf: a tick\'s fileDelta <= 20 KB', perf.deltaBytes <= 20000, true);
  // 177,191 files: the whole list in chunks of 5,000, one per turn, input answered between them.
  state.queue = [folder({ id: 31, expanded: false, files: counts({ total: 177191 }) })];
  folderOpen.clear();
  renderQueue();
  await frame();
  const heap0 = performance.memory ? performance.memory.usedJSHeapSize : 0;
  const chunkTimes = [];
  t0 = performance.now();
  setFolderOpen(dl.byId.get(31), true);
  for (let from = 0; from < 177191; from += 5000) {
    const rows = synth(177191, from, Math.min(5000, 177191 - from));
    const c0 = performance.now();
    onFiles({ type: 'files', id: 31, gen: 1, from, total: 177191, rows });
    chunkTimes.push(performance.now() - c0);
    await tick();
  }
  await frame();
  perf.list177k = ms(performance.now() - t0);
  perf.chunkMax = ms(Math.max(...chunkTimes));
  perf.chunkMedian = ms(median(chunkTimes));
  perf.heapMB = performance.memory ? ms((performance.memory.usedJSHeapSize - heap0) / 1048576) : null;
  check('C perf: 177,191 files listed <= 2 s', perf.list177k <= 2000, true);
  check('C perf: each chunk leaves the input free (<= 50 ms)', perf.chunkMax <= 50, true);
  check('C perf: JS heap <= +60 MB', perf.heapMB === null || perf.heapMB <= 60, true);
  check('C perf: every file a row', dl.n, 177192);
  t0 = performance.now();
  head('name').click();
  perf.sortName177k = ms(performance.now() - t0);
  check('C perf: 177,191 by name on a click <= 1 s', perf.sortName177k <= 1000, true);
  t0 = performance.now();
  head('size').click();
  perf.sortSize177k = ms(performance.now() - t0);
  check('C perf: 177,191 by size on a click <= 1 s', perf.sortSize177k <= 1000, true);
  head('size').click();
  head('size').click();
  t0 = performance.now();
  setDlShow('unfinished');
  setDlShow('all');
  perf.filter177kTwice = ms(performance.now() - t0);
  out.push(`INFO perf: ${JSON.stringify(perf)}`);
  // The head keeps one line at every width, Show and Go to running too (decision 2).
  state.queue = [file(50, { status: 'running', bytes: 5, total: 100, speed: 9, eta: 5 })];
  renderQueue();
  const headAt = {};
  for (const w of [716, 900, 1250]) {
    panel.style.width = `${w}px`;
    updateDlStep();
    const h = $('.downloads-head');
    const tops = [...h.children].filter((c) => !c.hidden && getComputedStyle(c).display !== 'none').map((c) => Math.round(c.getBoundingClientRect().top + c.getBoundingClientRect().height / 2));
    headAt[w] = [h.scrollWidth <= h.clientWidth, Math.max(...tops) - Math.min(...tops) <= 2, Math.round(h.getBoundingClientRect().height),
      [...h.children].map((c) => Math.round(c.getBoundingClientRect().width))];
  }
  out.push(`INFO head: ${JSON.stringify(headAt)}`);
  check('C: the head keeps one line at 716, 900 and 1250 px', Object.values(headAt).map((v) => v[0] && v[1]), [true, true, true]);
  panel.style.width = '1250px';
  updateDlStep();
  panel.style.height = '';
  folderOpen.clear();
  state.queue = [];
  renderQueue();

  // ---- I: the split moves by the grip on the panel's top edge and by the head's free space ----
  {
    const grip = $('#split-grip');
    const dhead = $('.downloads-head');
    const title = dhead.querySelector('.title');
    const spacer = dhead.querySelector('.spacer');
    const flexWas = panel.style.flex;
    panel.style.flex = ''; // the panel's height from the split again (--dl-h), as in the window
    panel.style.width = '';
    panel.style.height = '';
    state.queue = [];
    renderQueue();
    check('I: hidden with an empty queue, the grip with it', [panel.hidden, grip.getClientRects().length], [true, 0]);
    state.queue = [file(60, { status: 'running', bytes: 5, total: 100, speed: 9, eta: 5 }), file(61), file(62, { status: 'paused', bytes: 5, total: 100 })];
    renderQueue();
    clearTimeout(split.keyTimer);
    split.share = split.saved = SPLIT.def;
    applySplit();
    await frame();
    updateDlStep();
    renderDlBody();
    const H = () => splitLimits().H;
    check('I: no band of its own any more', document.getElementById('split'), null);
    check('I: the grip is the separator, in the panel', [grip.parentElement === panel, grip.getAttribute('role'), grip.getAttribute('aria-orientation'),
      grip.tabIndex, grip.getAttribute('aria-label'), grip.getClientRects().length > 0], [true, 'separator', 'horizontal', 0, 'Resize the list and Downloads', true]);
    const stops = [...panel.querySelectorAll('button, select, [tabindex]')].filter((e) => e.tabIndex >= 0 && !e.disabled && e.getClientRects().length);
    check('I: Tab reaches the grip first, before the head\'s controls and the table', [stops[0] === grip, stops.indexOf($('#queue')) > stops.indexOf(grip)], [true, true]);
    check('I: the panel takes 45% of H', [Math.abs(split.px - Math.round(SPLIT.def * H())) <= 1, Math.round(panel.getBoundingClientRect().height)], [true, split.px]);
    const pct = (h) => String(Math.round(100 * (1 - h / H())));
    const aria = () => [grip.getAttribute('aria-valuenow'), grip.getAttribute('aria-valuemin'), grip.getAttribute('aria-valuemax')];
    check('I: its values as chunk A\'s (the view above\'s share, in %)', aria(), [pct(split.px), pct(splitLimits().hi), pct(splitLimits().lo)]);

    // Where it sits: a 40 x 4 px bar centred on the top edge, inside the border, clear of the
    // rounded corners, over no control; at the narrow steps too.
    const place = {};
    for (const w of [716, 900, 1250, 0]) {
      panel.style.width = w ? `${w}px` : '';
      updateDlStep();
      const p = panel.getBoundingClientRect();
      const ps = getComputedStyle(panel);
      const g = grip.getBoundingClientRect();
      const after = getComputedStyle(grip, '::after');
      const bar = { x: g.left + parseFloat(after.left), y: g.top + parseFloat(after.top), w: parseFloat(after.width), h: parseFloat(after.height) };
      const radius = parseFloat(ps.borderTopLeftRadius);
      const inner = p.top + parseFloat(ps.borderTopWidth);
      const controls = [...dhead.children].filter((c) => !c.matches('.title, .spacer') && c.getClientRects().length);
      const over = controls.filter((c) => { const r = c.getBoundingClientRect(); return r.top < g.bottom && r.bottom > g.top && r.left < g.right && r.right > g.left; });
      place[w || 'full'] = [[bar.w, bar.h], Math.abs(bar.x + bar.w / 2 - (p.left + p.width / 2)) <= 0.5, bar.y >= inner && g.top >= inner,
        bar.x >= p.left + radius + 8 && bar.x + bar.w <= p.right - radius - 8, over.map((c) => c.id || c.className),
        Math.round(dhead.getBoundingClientRect().height)];
    }
    out.push(`INFO grip: ${JSON.stringify(place)}`);
    check('I: a 40 x 4 px bar, centred, inside the border, clear of the corners, over no control (716, 900, 1250, full)',
      Object.values(place).map((v) => v.slice(0, 5)), Object.values(place).map(() => [[40, 4], true, true, true, []]));

    // Nothing in the head or the rows moves when the grip shows, lights up or takes the focus.
    const rect = (n) => { const r = n.getBoundingClientRect(); return [r.x, r.y, r.width, r.height].map((v) => Math.round(v * 100) / 100); };
    const look = () => JSON.stringify([dhead, ...dhead.children, $('.dl-head'), ...document.querySelectorAll('.dl-row[data-key]')].map(rect));
    const plain = look();
    grip.style.display = 'none';
    const without = look();
    grip.style.display = '';
    grip.classList.add('dragging');
    const lit = look();
    grip.classList.remove('dragging');
    grip.focus();
    const focused = look();
    grip.blur();
    check('I: no layout shift for the grip (hidden, lit, focused)', [without === plain, lit === plain, focused === plain], [true, true, true]);

    // Pointer input, as the mouse gives it; the events go to `el` itself (with a real mouse the
    // press captures it, so the moves and the release come to it too).
    const ptr = (el, type, y) => el.dispatchEvent(new PointerEvent(type, { pointerId: 1, pointerType: 'mouse', isPrimary: true, bubbles: true, cancelable: true,
      clientX: 10, clientY: y, button: type === 'pointermove' ? -1 : 0, buttons: type === 'pointerup' ? 0 : 1 }));
    const drag = (el, dy, steps = 10) => {
      const y = 300;
      ptr(el, 'pointerdown', y);
      for (let k = 1; k <= steps; ++k) ptr(el, 'pointermove', y + Math.round((dy * k) / steps));
      ptr(el, 'pointerup', y + dy);
    };
    const prefsSplit = () => sent.filter((m) => m.type === 'prefs' && 'split' in m).map((m) => m.split);
    // Each head child's place within the panel, and the head's height: a drag moves the panel's
    // edge, never anything in the head.
    const inHead = () => { const p = panel.getBoundingClientRect(); return JSON.stringify([dhead, ...dhead.children].filter((n) => n.getClientRects().length).map((n) => { const r = n.getBoundingClientRect(); return [r.x - p.x, r.y - p.y, r.width, r.height].map((v) => Math.round(v * 100) / 100); })); };
    const headBefore = inHead();
    sent.length = 0;
    let px0 = split.px;
    ptr(grip, 'pointerdown', 300);
    ptr(grip, 'pointermove', 290);
    check('I: while dragged the grip is lit and the cursor row-resize everywhere',
      [grip.classList.contains('dragging'), document.documentElement.classList.contains('split-drag'), getComputedStyle($('#pause-all')).cursor], [true, true, 'row-resize']);
    ptr(grip, 'pointerup', 290);
    check('I: a drag up on the grip grows Downloads by the dragged px, saved on release', [split.px - px0, prefsSplit(), grip.classList.contains('dragging'),
      document.documentElement.classList.contains('split-drag')], [10, [split.share], false, false]);
    check('I: the share saved is the height over H', split.share, Math.round((10000 * split.px) / H()) / 10000);
    sent.length = 0;
    px0 = split.px;
    drag(grip, -60);
    check('I: a 10-step drag on the grip follows the pointer', [split.px - px0, prefsSplit().length], [60, 1]);
    for (const [name, el] of [['the head', dhead], ['its title', title], ['its spacer', spacer]]) {
      sent.length = 0;
      px0 = split.px;
      drag(el, 40);
      check(`I: a drag down on ${name} shrinks Downloads by the dragged px, saved once`, [split.px - px0, prefsSplit().length], [-40, 1]);
    }
    check('I: nothing in the head moved with the drags', inHead(), headBefore);
    check('I: the aria values follow', aria(), [pct(split.px), pct(splitLimits().hi), pct(splitLimits().lo)]);

    // Not from a control or a text of the head: it does what it does, the split stays.
    const notZones = ['#dl-show', '.dl-show', '.dl-show .label', '#dl-go', '.stepper', '.stepper .label', '#parallel', '#parallel-down', '#parallel-up',
      '#pause-all', '#cancel-all', '#clear-finished', '#dl-summary', '#dl-note'];
    const moved = [];
    for (const sel of notZones) {
      sent.length = 0;
      px0 = split.px;
      drag($(sel), -50);
      if (split.px !== px0 || prefsSplit().length || split.drag) moved.push(sel);
      split.drag = null;
    }
    check('I: no drag from a control or a text of the head', moved, []);
    const md = (el) => { const e = new MouseEvent('mousedown', { bubbles: true, cancelable: true, button: 0 }); el.dispatchEvent(e); return e.defaultPrevented; };
    check('I: a press keeps the focus where it was on the grip and the free space only',
      [md(grip), md(dhead), md(title), md(spacer), md($('#pause-all')), md($('#dl-show')), md($('#dl-summary')), md($('#parallel-up'))],
      [true, true, true, true, false, false, false, false]);
    const cur = (el) => getComputedStyle(el).cursor;
    check('I: the row-resize cursor over the grip and the free space, not over the controls',
      [[grip, dhead, title, spacer].map(cur), ['#pause-all', '#dl-go', '#parallel-up', '#dl-show', '.stepper', '#dl-summary'].map((s) => cur($(s)) === 'row-resize')],
      [['row-resize', 'row-resize', 'row-resize', 'row-resize'], [false, false, false, false, false, false]]);
    check('I: buttons keep their pointer', [cur($('#pause-all')), cur($('#parallel-up'))], ['pointer', 'pointer']);

    // A click without moving (or under the 3 px slop) changes nothing and saves nothing.
    for (const [name, el] of [['the grip', grip], ['the head', dhead], ['the title', title]]) {
      sent.length = 0;
      px0 = split.px;
      const share = split.share;
      ptr(el, 'pointerdown', 300);
      ptr(el, 'pointermove', 302);
      ptr(el, 'pointerup', 302);
      check(`I: a click on ${name} does nothing`, [split.px, split.share, prefsSplit(), split.drag, grip.classList.contains('dragging')], [px0, share, [], null, false]);
    }

    // The limits: a drag past either end stops there, within 0.15..0.85 of H.
    const lim = splitLimits();
    drag(grip, -5000);
    check('I: up to the top limit', split.px, lim.hi);
    drag(dhead, 5000);
    check('I: down to the bottom limit', split.px, lim.lo);
    check('I: the limits keep the share within 0.15..0.85', [lim.hi <= Math.floor(SPLIT.max * lim.H), lim.lo >= Math.ceil(SPLIT.min * lim.H) || lim.lo === lim.hi], [true, true]);

    // Double-click: the grip and the free space go back to 45%, a control doesn't, nor the second
    // click of a drag that just ended, nor one whose first press was on a control (the second
    // drifting a pixel or two off it onto the free space). Each press as the mouse gives it.
    const press = (el) => { ptr(el, 'pointerdown', 300); ptr(el, 'pointerup', 300); };
    const dblOnly = (el) => el.dispatchEvent(new MouseEvent('dblclick', { bubbles: true, cancelable: true, button: 0, detail: 2 }));
    const dbl = (el, first = el) => { press(first); press(el); dblOnly(el); };
    const resets = [];
    for (const el of [grip, dhead, title, spacer, $('#pause-all'), $('#dl-summary'), $('.stepper')]) {
      drag(grip, -50);
      split.movedAt = -1e9;
      sent.length = 0;
      dbl(el);
      resets.push(split.share === SPLIT.def && prefsSplit().length === 1);
    }
    check('I: a double-click resets on the grip and the free space only', resets, [true, true, true, true, false, false, false]);
    const resetAfter = [];
    for (const first of [$('#pause-all'), $('.dl-head .sort'), dhead]) {
      drag(grip, -50);
      split.movedAt = -1e9;
      sent.length = 0;
      dbl(dhead, first);
      resetAfter.push(split.share === SPLIT.def && prefsSplit().length === 1);
    }
    check('I: no reset when the first press was on Pause all or a sort title, a reset after two on the head', resetAfter, [false, false, true]);
    drag(grip, -50);
    const share1 = split.share;
    dbl(dhead);
    check('I: not right after a drag', split.share, share1);
    split.movedAt = -1e9;

    // A size change during a press (a banner shows while the button is held) takes effect at the
    // release: the observer waits for the press, the release applies the limits.
    drag(grip, -5000);
    const banner = $('#update-banner');
    ptr(dhead, 'pointerdown', 300);
    banner.hidden = false;
    banner.textContent = 'A newer grab is out.';
    await frame();
    await frame();
    ptr(dhead, 'pointerup', 300);
    check('I: a banner shown during a press: the top limit and the list\'s least height at the release',
      [split.px, $('#results').getBoundingClientRect().height >= LIST_MIN - 0.5], [splitLimits().hi, true]);
    banner.hidden = true;
    banner.textContent = '';
    await frame();
    await frame();

    // Keys on the grip, as chunk A's: arrows 16 px, PgUp/PgDn a tenth of H, Home/End the ends,
    // Enter the default; saved 500 ms after the last key.
    const gripKey = (k) => grip.dispatchEvent(new KeyboardEvent('keydown', { key: k, bubbles: true, cancelable: true }));
    resetSplit();
    sent.length = 0;
    px0 = split.px;
    const page = Math.round(H() / 10);
    const steps = [];
    for (const k of ['ArrowUp', 'ArrowDown', 'ArrowDown', 'PageUp', 'PageDown']) { gripKey(k); steps.push(split.px - px0); }
    check('I: arrows 16 px, PgUp/PgDn a tenth of H', steps, [16, 0, -16, page - 16, -16]);
    gripKey('Home');
    const atHome = split.px;
    gripKey('End');
    check('I: Home and End go to the ends', [atHome, split.px], [splitLimits().hi, splitLimits().lo]);
    check('I: keys save once they stop', prefsSplit(), []);
    await new Promise((r) => setTimeout(r, 650));
    check('I: ... 500 ms after the last key', prefsSplit(), [split.share]);
    gripKey('Enter');
    check('I: Enter goes back to 45%', [split.share, prefsSplit().slice(-1)[0], Math.abs(split.px - Math.round(SPLIT.def * H())) <= 1], [SPLIT.def, SPLIT.def, true]);

    // The saved share gives the same height back (gui.json at the next start).
    drag(grip, -70);
    const saved = prefsSplit().slice(-1)[0];
    const pxSaved = split.px;
    split.share = split.saved = SPLIT.def;
    applySplit();
    split.share = split.saved = saved;
    applySplit();
    check('I: the saved share gives the dragged height back', Math.abs(split.px - pxSaved) <= 1, true);
    resetSplit();
    sent.length = 0;
    state.queue = [];
    renderQueue();
    panel.style.flex = flexWas;
  }

  // The update banner while `grab update` runs: a try GitHub failed shows in place of the step until
  // one gets through (the backend then clears the note), as text.
  {
    const ub = $('#update-banner');
    state.update = { state: 'running', version: '0.23.0', phase: 'downloading', note: '' };
    renderUpdate();
    check('update: the step while nothing failed', ub.textContent, 'Updating to 0.23.0: downloading…');
    const note = 'GitHub answered 504 (Gateway Time-out) for SHA256SUMS.txt; trying again in 3 s (try 2 of 6)';
    state.update = { ...state.update, note };
    renderUpdate();
    check('update: a failed try in place of the step', ub.textContent, `Updating to 0.23.0: ${note}`);
    state.update = { ...state.update, note: '<img src=x>' };
    renderUpdate();
    check('update: the note is text', [ub.querySelector('img'), ub.textContent], [null, 'Updating to 0.23.0: <img src=x>']);
    state.update = { state: '' };
    renderUpdate();
    check('update: no banner after', ub.hidden, true);
  }

  // ---- uploads in the window (UPLOADS.md 5.10, chunk 3) ------------------------------------------
  // Upload rows in the same table: "↑ name → Server:/folder", Uploading, the Show filter's
  // Downloads / Uploads, the head's "N uploading"; every row action on an upload row acts on the
  // upload (the server side, its source), never on a downloaded file; Browse's upload entries and
  // their refusals; the confirm dialog's states.
  {
    dlSort.key = '';
    showDlOrder();
    clearSel();
    dl.cursor = '';
    setDlShow('all');
    const remotesWas = state.remotes;
    const remoteWas = state.remote;
    state.remotes = [{ name: 'Main', host: 'h', user: 'u' }, { name: 'Box', host: 'b', user: 'u' }];
    state.remote = 'Main';
    const upFile = (id, over) => file(id, { dir: 'up', path: `/srv/tv/u${id}.mkv`, name: `u${id}.mkv`, into: '/srv/tv',
      source: `E:\\Videos\\u${id}.mkv`, dest: '', local: `E:\\Videos\\u${id}.mkv`, ...over });
    const upFolder = (id, over) => folder({ id, dir: 'up', path: '/srv/tv/holiday', name: 'holiday', into: '/srv/tv',
      source: 'E:\\Pictures\\holiday', dest: '', local: 'E:\\Pictures\\holiday', ...over });

    // The rows.
    const v = dlView(upFile(1, { status: 'running', speed: 1048576, connections: 2 }));
    check('up: a running upload: Uploading, ↑ name → Server:/folder',
      [v.word, v.text, v.dest, v.cls.split(' ').slice(0, 2), v.speed], ['Uploading', '↑ u1.mkv', '→ Main:/srv/tv', ['st-uploading', 'up'], '1.0 MiB/s']);
    check('up: its name tip: from where, to where', v.nameTip, 'E:\\Videos\\u1.mkv\n→ Main:/srv/tv/u1.mkv');
    check('up: a folder', [dlView(upFolder(2)).text, dlView(upFolder(2)).word], ['↑ holiday/', 'Uploading']);
    check('up: into the login folder', dlView(upFile(3, { into: '', path: 'u3.mkv' })).dest, '→ Main:~');
    check('up: queued and paused say so', [dlView(upFile(4, { position: 2 })).word, dlView(upFile(4, { status: 'paused' })).word], ['Queued #2', 'Paused']);
    check('up: a folder upload listing its folder, looking at the server', [dlView(upFolder(2, { phase: 'listing' })).statusTip,
      dlView(upFolder(2, { phase: 'checking' })).statusTip.split('\n')[0]], [dlView(upFolder(2, { phase: 'listing' })).statusTip, 'Looking at the folder on the server: the files already there']);
    check('up: slots: Pause and Cancel the upload', dlSlots(upFile(1, { status: 'running' })).map((s) => s && (s[2] || s[1])), ['Pause', 'Cancel the upload']);
    check('up: slots once done: Show on server', dlSlots(upFile(1, { status: 'done' })).map((s) => s && [s[0], s[2]]), [['show', 'Show on server'], null]);
    check('up: slots after a failure with bytes: Resume from what was sent', dlSlots(upFile(1, { status: 'failed', bytes: 5 }))[0][2], 'Resume from what was sent');
    check('up: a file of an upload folder: Uploading, Show on server once done',
      [fileStatus(upFolder(2), { status: 'running' }).word, fileSlots({ status: 'done' }, fileStatus(upFolder(2), { status: 'done' }), true)[0][2]],
      ['Uploading', 'Show on server']);

    // The filter and the head.
    state.queue = [file(10, { status: 'running', total: 100, bytes: 5 }), upFile(11, { status: 'running', total: 100, bytes: 5 }),
      upFile(12, { status: 'queued', position: 1 }), upFolder(13, { status: 'done', gen: 4, expanded: true,
        files: { total: 2, queued: 0, running: 0, paused: 0, done: 2, failed: 0, skipped: 0 } }),
      upFile(14, { status: 'paused', bytes: 7, total: 100 }), upFile(15, { status: 'failed', error: 'Main doesn’t let this account write in /srv/tv' })];
    sent.length = 0;
    renderQueue();
    onFiles({ type: 'files', id: 13, gen: 4, from: 0, total: 2, rows: [
      kidRow({ path: 'S01/a.jpg', size: 3, status: 'done', bytes: 3 }), kidRow({ path: 'b.jpg', size: 4, status: 'done', bytes: 4 })] });
    check('up: the head counts running uploads apart', $('#dl-summary').textContent, '1 running · 1 queued · 1 paused · 1 uploading');
    onSpeed({ type: 'speed', total: 3 * 1048576, up: 1048576 });
    check('up: the status bar: both totals, uploads counted', $('#total-speed').textContent, '↓ 3.0 · ↑ 1.0 MiB/s · 1 running · 1 uploading');
    const shows = (show) => {
      setDlShow(show);
      return state.queue.filter(dlMatches).map((q) => q.id);
    };
    check('up: Show: Downloads, Uploads', [shows('downloads'), shows('uploads'), shows('all')], [[10], [11, 12, 13, 14, 15], [10, 11, 12, 13, 14, 15]]);
    check('up: Show has Downloads and Uploads', [...$('#dl-show').options].map((o) => o.value), ['all', 'unfinished', 'failed', 'downloads', 'uploads']);
    setDlShow('uploads');
    check('up: a folder upload\'s files show under Uploads', fileMatches(dl.stores.get(13), 0), true);
    setDlShow('all');

    // Every row action on an upload row.
    const labels = (qs) => downloadsMenu(qs).filter((m) => m.label).map((m) => m.label);
    const actOn = (qs, label) => {
      sent.length = 0;
      downloadsMenu(qs).find((m) => m.label === label).act();
      return sent.map((m) => JSON.stringify(m));
    };
    const q = (id) => dl.byId.get(id);
    check('up: a running upload\'s menu', labels([q(11)]), ['Pause', 'Cancel', 'High', 'Normal', 'Low', 'Show on server', 'Show in Explorer', 'Copy server path']);
    check('up: Pause', actOn([q(11)], 'Pause'), [JSON.stringify({ type: 'pauseItem', id: 11 })]);
    check('up: Cancel (nothing sent yet: at once)', actOn([q(12)], 'Cancel'), [JSON.stringify({ type: 'cancelItem', id: 12 })]);
    check('up: Resume', actOn([q(14)], 'Resume'), [JSON.stringify({ type: 'resumeItem', id: 14 })]);
    check('up: Retry', actOn([q(15)], 'Retry'), [JSON.stringify({ type: 'retryItem', id: 15 })]);
    check('up: High', actOn([q(11)], 'High'), [JSON.stringify({ type: 'setPriority', id: 11, priority: 'high' })]);
    check('up: Show in Explorer: its source', actOn([q(11)], 'Show in Explorer'), [JSON.stringify({ type: 'openFolder', id: 11 })]);
    check('up: Copy server path', actOn([q(11), q(12)], 'Copy 2 server paths'), [JSON.stringify({ type: 'copyText', text: '/srv/tv/u11.mkv\n/srv/tv/u12.mkv' })]);
    check('up: Copy error', actOn([q(15)], 'Copy error'), [JSON.stringify({ type: 'copyText', text: 'Main doesn’t let this account write in /srv/tv' })]);
    const browsed = () => sent.filter((m) => m.type === 'browse').map((m) => [m.remote, m.path, m.top, state.browse.pendingSelect]);
    actOn([q(11)], 'Show on server');
    check('up: Show on server: Browse at its folder, the upload picked', browsed(), [['Main', '/srv/tv', false, '/srv/tv/u11.mkv']]);
    check('up: a waiting upload moves like a download', labels([q(12)]).filter((l) => l.startsWith('Move')).length, 4);
    check('up: a finished upload: nothing opens, nothing local', labels([q(13)]), ['Show on server', 'Show in Explorer', 'Copy server path']);
    check('up: Open is never an upload\'s', [dlOpenKind({ kind: 'dl', id: 13 }), dlOpenKind({ kind: 'file', id: 13, index: 0 }), dlOpen({ kind: 'dl', id: 13 })], ['', '', false]);
    check('up: downloads and uploads together: each their own entries', labels([q(10), q(11)]),
      ['Pause 2', 'Cancel 2', 'High', 'Normal', 'Low', 'Show in folder', 'Copy path', 'Show on server', 'Show in Explorer', 'Copy server path']);
    // Cancel from the menu on several, with bytes sent: asks, saying what goes.
    sent.length = 0;
    downloadsMenu([q(11), q(14)]).find((m) => m.label === 'Cancel 2').act();
    check('up: cancelling several uploads asks first', [$('#modal').hidden, $('#modal-title').textContent, $('#modal-cancel').textContent,
      $('#modal-body').textContent.includes('removed from the server'), sent.length], [false, 'Cancel 2 uploads?', 'Keep uploading', true, 0]);
    closeModal(true);
    check('up: then both', sent.map((m) => `${m.type}:${m.id}`), ['cancelItem:11', 'cancelItem:14']);
    // A file of a finished folder upload.
    selectDl(rowOf('f13:0'));
    const fl = dlMenu().filter((m) => m.label).map((m) => m.label);
    check('up: a file of a folder upload: no Open, its server side', fl, ['Show on server', 'Show in Explorer', 'Copy server path']);
    sent.length = 0;
    dlMenu().find((m) => m.label === 'Show in Explorer').act();
    check('up: its Show in Explorer: the local file, by its index', sent.map((m) => JSON.stringify(m)), [JSON.stringify({ type: 'showFile', id: 13, gen: 4, index: 0 })]);
    sent.length = 0;
    dlMenu().find((m) => m.label === 'Copy server path').act();
    check('up: its server path', sent.map((m) => m.text), ['/srv/tv/holiday/S01/a.jpg']);
    sent.length = 0;
    dlMenu().find((m) => m.label === 'Show on server').act();
    check('up: its Show on server: its folder there', browsed(), [['Main', '/srv/tv/holiday/S01', false, '/srv/tv/holiday/S01/a.jpg']]);
    // The Show button, Enter and a double-click open Browse there; nothing opens a file.
    const el = (key) => document.querySelector(`.dl-row[data-key="${CSS.escape(key)}"]`);
    sent.length = 0;
    el('d13').querySelector('[data-act="show"]').click();
    check('up: the Show button', browsed(), [['Main', '/srv/tv', false, '/srv/tv/holiday']]);
    const press = (target, detail) => {
      const r = target.getBoundingClientRect();
      const at = { bubbles: true, cancelable: true, button: 0, detail, clientX: r.x + 4, clientY: r.y + 4 };
      target.dispatchEvent(new MouseEvent('mousedown', at));
      target.dispatchEvent(new MouseEvent('mouseup', at));
      target.dispatchEvent(new MouseEvent('click', at));
    };
    sent.length = 0;
    press(el('d11').querySelector('.dl-text'), 1);
    press(el('d11').querySelector('.dl-text'), 2);
    check('up: a double-click on an upload: Browse there, nothing opened', [browsed(), sent.filter((m) => m.type === 'openFile').length],
      [[['Main', '/srv/tv', false, '/srv/tv/u11.mkv']], 0]);
    sent.length = 0;
    press(el('f13:1').querySelector('.dl-text'), 1);
    press(el('f13:1').querySelector('.dl-text'), 2);
    check('up: a double-click on its file', browsed(), [['Main', '/srv/tv/holiday', false, '/srv/tv/holiday/b.jpg']]);
    const grid = $('#queue');
    const key = (k, mods = {}) => grid.dispatchEvent(new KeyboardEvent('keydown', { key: k, bubbles: true, cancelable: true, ...mods }));
    selectDl(rowOf('d13'));
    setDlCursor(rowOf('d13'));
    sent.length = 0;
    key('Enter');
    check('up: Enter on a finished upload: Browse there, not a folder here', [browsed().length, sent.filter((m) => m.type === 'openFolder').length], [1, 0]);
    sent.length = 0;
    key('Enter', { ctrlKey: true });
    check('up: Ctrl+Enter opens nothing', sent.filter((m) => m.type === 'openFile' || m.type === 'openDir').length, 0);
    // A server no longer configured: said, nothing browsed.
    sent.length = 0;
    showOnServer(upFile(20, { remote: 'Gone' }));
    check('up: a server gone: said in the head', [browsed(), $('#dl-note').textContent], [[], 'Gone isn’t one of your servers any more.']);
    // Cancel all names the uploads.
    sent.length = 0;
    cancelAll();
    check('up: Cancel all asks about both', [$('#modal-title').textContent, $('#modal-cancel').textContent], ['Cancel all downloads and uploads?', 'Keep going']);
    closeModal(false);

    // Browse: Upload files here… / Upload folder here… on one folder row, and the folder's own on the
    // list's empty part; refusals say why.
    const b = state.browse;
    const browseWas = { ...b };
    Object.assign(b, { server: 'Main', path: '/srv', top: false, under: null, loading: false, error: '', uploadable: true, severalRoots: false,
      crumbs: [{ name: 'srv', path: '/srv' }] });
    state.hits = [{ name: 'tv', path: '/srv/tv', dir: true, remote: 'Main' }, { name: 'a.txt', path: '/srv/a.txt', remote: 'Main', size: 3 }];
    const pick = (i) => {
      state.selected = new Set([i]);
      state.cursor = i;
    };
    const entries = () => resultMenu().filter((m) => m.label && m.label.startsWith('Upload')).map((m) => m.label);
    pick(0);
    check('up: a folder row offers both', entries(), ['Upload files here…', 'Upload folder here…']);
    sent.length = 0;
    resultMenu().find((m) => m.label === 'Upload files here…').act();
    resultMenu().find((m) => m.label === 'Upload folder here…').act();
    check('up: each asks for the file dialog, into that folder', sent.map((m) => [m.type, m.server, m.dir, m.folders, m.token > 0]),
      [['uploadPick', 'Main', '/srv/tv', false, true], ['uploadPick', 'Main', '/srv/tv', true, true]]);
    // A file row, or several rows: the open folder's own, named for it (a folder whose rows fill
    // the list has no empty part to right-click).
    pick(1);
    check('up: a file row: the open folder\'s', entries(), ['Upload files to srv…', 'Upload folder to srv…']);
    state.selected = new Set([0, 1]);
    check('up: several rows: the open folder\'s', entries(), ['Upload files to srv…', 'Upload folder to srv…']);
    pick(1);
    sent.length = 0;
    resultMenu().find((m) => m.label === 'Upload folder to srv…').act();
    check('up: a file row\'s entry: into the open folder', sent.map((m) => [m.type, m.server, m.dir, m.folders]),
      [['uploadPick', 'Main', '/srv', true]]);
    upload.pending = null;
    // Shift+F10 or the menu key with no row under the cursor (an empty folder): the open folder's menu.
    const hitsWas = state.hits;
    state.hits = [];
    state.selected = new Set();
    state.cursor = -1;
    $('#results').dispatchEvent(new KeyboardEvent('keydown', { key: 'F10', shiftKey: true, bubbles: true, cancelable: true }));
    check('up: Shift+F10 in an empty folder opens its menu', [$('#menu').hidden,
      [...$('#menu').querySelectorAll('button')].map((x) => x.textContent).filter((t) => t.startsWith('Upload'))], [false, ['Upload files here…', 'Upload folder here…']]);
    closeMenu();
    state.hits = hitsWas;
    sent.length = 0;
    const bg = () => folderMenu().map((m) => m.label).filter((l) => l && l.startsWith('Upload'));
    const bgEntry = (label) => folderMenu().find((m) => m.label === label);
    check('up: the empty part: the open folder\'s', bg(), ['Upload files here…', 'Upload folder here…']);
    bgEntry('Upload folder here…').act();
    check('up: into the open folder', sent.map((m) => [m.type, m.server, m.dir, m.folders]), [['uploadPick', 'Main', '/srv', true]]);
    const refused = (over) => {
      Object.assign(b, over);
      sent.length = 0;
      state.flash = null;
      bgEntry('Upload files here…').act();
      const said = state.flash ? state.flash.text : '';
      Object.assign(b, { server: 'Main', path: '/srv', top: false, under: null, loading: false, error: '', uploadable: true, severalRoots: false });
      return [sent.length, said];
    };
    check('up: All servers\' top refuses, saying why', refused({ server: '' }), [0, 'Open a folder of a server first: uploads go into one folder of one server.']);
    check('up: the made-up top of several roots refuses', refused({ top: true, severalRoots: true, uploadable: false, path: '' }),
      [0, 'This is the list of the server’s search roots, not a folder: open one of them first.']);
    check('up: search results refuse', refused({ under: { path: '/srv', name: 'srv' } }),
      [0, 'Search results aren’t a folder: close the search (Esc) to upload into the folder.']);
    check('up: a folder still opening refuses', refused({ loading: true }), [0, 'srv is still opening.']);
    // A folder row among search results: its entries say why.
    b.under = { path: '/srv', name: 'srv' };
    pick(0);
    sent.length = 0;
    state.flash = null;
    resultMenu().find((m) => m.label === 'Upload files here…').act();
    check('up: a folder found by a search refuses', [sent.length, state.flash && state.flash.text],
      [0, 'Uploads go into a folder you have open, not into search results: open this folder first.']);
    b.under = null;
    // The backend's word on the folder shown.
    onBrowseResult({ type: 'browseResult', id: b.id, remote: 'Main', path: '/srv', top: false, items: [], uploadable: true });
    check('up: browseResult: uploadable', b.uploadable, true);
    onBrowseResult({ type: 'browseResult', id: b.id, remote: 'Main', path: '', top: true, severalRoots: true, items: [], uploadable: false });
    check('up: browseResult: the made-up top is not', b.uploadable, false);

    // The confirm dialog: opens with the backend's first check, Upload once the server takes it.
    const check0 = { type: 'uploadCheck', server: 'Main', dir: '/srv/tv', items: [], count: 0, errors: 0, files: 0, bytes: 0,
      skippedLinks: 0, longNames: 0, unreadable: 0, writable: null, existing: { count: 0, same: 0, newer: 0, older: 0, differs: 0, folders: 0 },
      setting: 'ask' };
    sent.length = 0;
    pickUpload('Main', '/srv/tv', true);
    const token = sent[0].token;
    const okBtn = $('#modal-ok');
    const body = () => $('#modal-body').textContent;
    onUploadCheck({ ...check0, token: token + 7, phase: 'local' });
    check('up: another pick\'s check is ignored', $('#modal').hidden, true);
    onUploadCheck({ ...check0, token, phase: 'local' });
    check('up: the dialog opens with the first, Upload waits', [$('#modal').hidden, $('#modal-title').textContent, okBtn.disabled, okBtn.textContent],
      [false, 'Upload to Main:/srv/tv', true, 'Upload']);
    const items = [{ name: 'holiday', dir: true, files: 1204, bytes: 3435973837 }, { name: 'notes.txt', dir: false, files: 1, bytes: 2048 }];
    onUploadCheck({ ...check0, token, phase: 'server', items, count: 2, files: 1205, bytes: 3435975885, skippedLinks: 2, longNames: 1 });
    check('up: what goes, and the server being checked', [body().includes('holiday/1,204 files, 3.2 GiB'), body().includes('notes.txt2.0 KiB'),
      body().includes('1,205 files, 3.2 GiB in all'), body().includes('2 links left out'), body().includes('1 name longer than a server takes'),
      body().includes('Checking Main…'), okBtn.disabled], [true, true, true, true, true, true, true]);
    onUploadCheck({ ...check0, token, phase: 'ready', items: [{ ...items[0], merging: true }, items[1]], count: 2, files: 1205, bytes: 3435975885,
      writable: true, free: 128849018880, existing: { count: 37, same: 30, newer: 5, older: 2, differs: 0, folders: 0 } });
    check('up: ready: you can write, the free space, the folder merged, the files there and the question, on Skip',
      [body().includes('✓ You can write here · 120.0 GiB free'), body().includes('merged into the folder “holiday” there'),
        body().includes('37 already there: 30 the same, 5 newer here, 2 older here'), $('#modal-body .up-all input:checked').value,
        body().includes('…and 37 more, which go as for all.'), okBtn.disabled, okBtn.textContent, okBtn.classList.contains('danger')],
      [true, true, true, 'skip', true, false, 'Upload', false]);
    sent.length = 0;
    okBtn.click();
    check('up: Upload starts it, Skip for all', [sent.map((m) => JSON.stringify(m)), $('#modal').hidden],
      [[JSON.stringify({ type: 'uploadStart', token, choice: 'skip', overrides: {}, always: false })], true]);
    onUploadCheck({ ...check0, token, phase: 'ready', writable: true });
    check('up: a late message of a started upload opens nothing', $('#modal').hidden, true);
    // Box.Guest: read-only, said plainly, Upload disabled; Esc cancels the check.
    sent.length = 0;
    pickUpload('Box.Guest', 'software', false);
    const t2 = sent[0].token;
    const why = 'Box.Guest is read-only for this account: nothing can be uploaded to software';
    onUploadCheck({ ...check0, token: t2, server: 'Box.Guest', dir: 'software', phase: 'refused', items: [items[1]], count: 1, files: 1, bytes: 2048,
      writable: false, why });
    check('up: refused: the reason, Upload disabled', [$('#modal').hidden, body().includes(why), okBtn.disabled], [false, true, true]);
    sent.length = 0;
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }));
    check('up: Esc cancels the check', [$('#modal').hidden, sent.map((m) => JSON.stringify(m))], [true, [JSON.stringify({ type: 'uploadCheckCancel', token: t2 })]]);
    // Not reachable: said, Upload disabled.
    sent.length = 0;
    pickUpload('Main', '/srv/tv', false);
    const t3 = sent[0].token;
    onUploadCheck({ ...check0, token: t3, phase: 'refused', items: [items[1]], count: 1, files: 1, bytes: 2048, writable: null,
      why: 'Couldn\'t reach Main: connection refused' });
    check('up: no login: the reason, Upload disabled', [body().includes('Couldn\'t reach Main: connection refused'), okBtn.disabled], [true, true]);
    closeModal(false);
    // Too little room: Upload anyway.
    sent.length = 0;
    pickUpload('Main', '/srv/tv', false);
    const t4 = sent[0].token;
    onUploadCheck({ ...check0, token: t4, phase: 'ready', items: [items[1]], count: 1, files: 1, bytes: 2048, writable: true, free: 1024, short: true });
    check('up: not enough space: Upload anyway', [body().includes('Not enough space: 1.0 KiB free, 2.0 KiB needed'), okBtn.disabled, okBtn.textContent,
      okBtn.classList.contains('danger'), body().includes('Nothing of it is there yet.')], [true, false, 'Upload anyway', true, true]);
    closeModal(false);
    check('up: another dialog after it is as it was', (() => {
      showModal({ title: 't', html: '', ok: 'OK' });
      const r = [okBtn.disabled, okBtn.classList.contains('danger')];
      closeModal(false);
      return r;
    })(), [false, false]);
    // The file dialog closed: nothing opens.
    sent.length = 0;
    pickUpload('Main', '/srv/tv', false);
    const t5 = sent[0].token;
    onUploadCheck({ type: 'uploadCheck', token: t5, phase: 'cancelled' });
    onUploadCheck({ ...check0, token: t5, phase: 'local' });
    check('up: the file dialog closed: no dialog', [$('#modal').hidden, upload.pending], [true, null]);
    // Another question open: this one isn't asked over it.
    sent.length = 0;
    pickUpload('Main', '/srv/tv', false);
    const t6 = sent[0].token;
    showModal({ title: 'other', html: '', ok: 'OK' });
    sent.length = 0;
    onUploadCheck({ ...check0, token: t6, phase: 'local' });
    check('up: over another question: cancelled', [$('#modal-title').textContent, sent.map((m) => m.type)], ['other', ['uploadCheckCancel']]);
    closeModal(false);
    // Names are text.
    sent.length = 0;
    pickUpload('Main', '/srv/tv', false);
    const t7 = sent[0].token;
    onUploadCheck({ ...check0, token: t7, phase: 'local', items: [{ name: '<img src=x>', dir: false, files: 1, bytes: 1 }], count: 1 });
    check('up: a name is text', [$('#modal-body').querySelector('img'), body().includes('<img src=x>')], [null, true]);
    closeModal(false);

    // The files already on the server (chunk 5): the question, on Skip under Ask; a choice for all
    // and one per file, what each does, Always do this; the setting with Change; the late question.
    const ex37 = { count: 37, same: 30, newer: 5, older: 1, differs: 0, folders: 1, files: [
      { name: 'holiday/a.jpg', size: 100, modified: 1763800000, mineSize: 100, mineModified: 1763800000, kind: 'same' },
      { name: 'holiday/b.jpg', size: 100, modified: 1763700000, mineSize: 120, mineModified: 1763800000, kind: 'newer' },
      { name: 'holiday/c.jpg', size: 100, modified: 1763900000, mineSize: 90, mineModified: 1763800000, kind: 'older' },
      { name: 'holiday/d', size: 0, modified: 1763800000, mineSize: 5, mineModified: 1763800000, kind: 'folder' },
      { name: 'notes.txt', size: 2048, modified: 1763800000, mineSize: 2048, mineModified: 1763800000, kind: 'same' }] };
    const ready = (t, over = {}) => onUploadCheck({ ...check0, token: t, phase: 'ready', items, count: 2, files: 1205, bytes: 3435975885,
      writable: true, existing: ex37, ...over });
    const radio = (value) => $(`#modal-body .up-all input[value="${value}"]`);
    const pickAll = (value) => { radio(value).checked = true; radio(value).dispatchEvent(new Event('change', { bubbles: true })); };
    const rowSel = (i) => $(`#modal-body select[data-i="${i}"]`);
    const pickRow = (i, value) => { rowSel(i).value = value; rowSel(i).dispatchEvent(new Event('change', { bubbles: true })); };
    const effects = () => [...document.querySelectorAll('#modal-body .up-clashes td.up-effect')].map((td) => td.textContent);
    const outcome = () => $('#modal-body .up-outcome').textContent;
    const wide = () => $('#modal .modal').classList.contains('wide');
    state.uploadExisting = 'ask';
    sent.length = 0;
    pickUpload('Main', '/srv/tv', true);
    const c1 = sent[0].token;
    onUploadCheck({ ...check0, token: c1, phase: 'local' });
    check('up ex: before the server was looked at: no question, the dialog narrow', [$('#modal-body .up-clash'), wide()], [null, false]);
    ready(c1);
    check('up ex: under Ask the question opens on Skip, each choice saying what it replaces',
      [body().includes('37 already there: 30 the same, 5 newer here, 1 older here, 1 a folder there'), radio('skip').checked,
        [...document.querySelectorAll('#modal-body .up-all label')].map((l) => l.textContent.trim()), wide()],
      [true, true, ['Replace 36 replaced', 'Replace if different 6 replaced', 'Replace if newer 5 replaced', 'Skip nothing replaced',
        'Keep both yours as “name (2)”'], true]);
    const rows = [...document.querySelectorAll('#modal-body .up-clashes tbody tr')];
    check('up ex: the first files listed: name, how they compare, yours, the server\'s, a choice each', [rows.length,
      rows[1].cells[0].textContent, rows[1].cells[1].textContent, rows[1].cells[2].textContent, rows[3].cells[2].textContent,
      [...rowSel(0).options].map((o) => o.value), [...rowSel(3).options].map((o) => o.textContent)],
      [5, 'holiday/b.jpg newer here', `120 B${fmtDate(1763800000)}`, `100 B${fmtDate(1763700000)}`, 'a folder',
        ['', 'replace', 'different', 'newer', 'skip', 'both'], ['As for all', 'Skip', 'Keep both']]);
    check('up ex: on Skip nothing is replaced; the rest go as for all; a folder is never replaced',
      [effects(), outcome(), body().includes('…and 32 more, which go as for all.'),
        body().includes('A folder of that name there is never replaced: Replace skips it.'), $('#up-always').checked,
        $('#modal-body .up-always').textContent],
      [['skipped', 'skipped', 'skipped', 'skipped', 'skipped'], '37 skipped: nothing on Main is replaced', true, true, false,
        ' Always do this: Skip without asking (Settings → Uploads)']);
    check('up ex: ARIA: the section named by its summary, the choices a group, the table and each choice named, the outcome live',
      [$('#modal-body .up-clash').getAttribute('aria-labelledby'), !!$('#up-clash-sum'), $('#modal-body .up-all legend').textContent,
        $('#modal-body .up-clashes').getAttribute('aria-label'), document.querySelectorAll('#modal-body .up-clashes th[scope="col"]').length,
        rowSel(0).getAttribute('aria-label'), $('#modal-body .up-outcome').getAttribute('aria-live'),
        new Set([...document.querySelectorAll('#modal-body .up-all input')].map((r) => r.name)).size, document.activeElement.id],
      ['up-clash-sum', true, 'For all of them', 'Files already on Main', 5, 'What to do with holiday/a.jpg', 'polite', 1, 'modal-cancel']);
    pickAll('newer');
    check('up ex: Replace if newer for all: those newer here replaced, the rest skipped', [effects(), outcome(),
      $('#modal-body .up-always-what').textContent], [['skipped', 'replaced', 'skipped', 'skipped', 'skipped'], '5 replaced, 32 skipped',
      'Replace if newer']);
    pickRow(2, 'replace');
    pickRow(3, 'both');
    pickRow(1, 'skip');
    check('up ex: a file\'s own choice beats the one for all', [effects(), outcome()],
      [['skipped', 'skipped', 'replaced', 'second copy', 'skipped'], '5 replaced, 1 sent as a second copy (“name (2)”), 31 skipped']);
    pickRow(1, '');
    check('up ex: As for all again', [effects()[1], outcome()], ['replaced', '6 replaced, 1 sent as a second copy (“name (2)”), 30 skipped']);
    pickRow(1, 'skip');
    pickAll('both');
    check('up ex: Keep both for all: every one beside the server\'s, the folder too', [effects(), outcome()],
      [['second copy', 'skipped', 'replaced', 'second copy', 'second copy'], '1 replaced, 35 sent as a second copy (“name (2)”), 1 skipped']);
    pickAll('newer');
    $('#up-always').checked = true;
    $('#up-always').dispatchEvent(new Event('change', { bubbles: true }));
    sent.length = 0;
    okBtn.click();
    check('up ex: Upload sends the choice for all, the files\' own by their place, Always do this; the setting follows',
      [sent.map((m) => JSON.stringify(m)), state.uploadExisting, $('#up-existing-set').value],
      [[JSON.stringify({ type: 'uploadStart', token: c1, choice: 'newer', overrides: { 1: 'skip', 2: 'replace', 3: 'both' }, always: true })],
        'newer', 'newer']);
    // With a setting: said, with Change; nothing asked.
    sent.length = 0;
    pickUpload('Main', '/srv/tv', true);
    const c2 = sent[0].token;
    ready(c2, { setting: 'newer' });
    check('up ex: a setting: the files there, the setting and Change, what it does; no question',
      [body().includes('Files already there: Replace if newer (your setting) · Change'), $('#modal-body .up-all'), outcome(), wide(),
        $('#modal-body [data-act="up-change"]').tagName],
      [true, null, '5 replaced, 32 skipped', false, 'BUTTON']);
    sent.length = 0;
    okBtn.click();
    check('up ex: Upload goes by the setting', sent.map((m) => JSON.stringify(m)),
      [JSON.stringify({ type: 'uploadStart', token: c2, choice: 'newer', overrides: {}, always: false })]);
    sent.length = 0;
    pickUpload('Main', '/srv/tv', true);
    const c3 = sent[0].token;
    ready(c3, { setting: 'replace' });
    $('#modal-body [data-act="up-change"]').click();
    check('up ex: Change opens the question for this upload at the setting\'s choice, the focus on it',
      [radio('replace').checked, document.activeElement === radio('replace'), wide(), effects()],
      [true, true, true, ['replaced', 'replaced', 'replaced', 'skipped', 'replaced']]);
    pickRow(0, 'skip');
    sent.length = 0;
    okBtn.click();
    check('up ex: ...and Upload sends what was chosen there', sent.map((m) => JSON.stringify(m)),
      [JSON.stringify({ type: 'uploadStart', token: c3, choice: 'replace', overrides: { 0: 'skip' }, always: false })]);
    // Nothing there: the setting goes with it (Ask: a file that turns up later is asked about).
    for (const setting of ['ask', 'replace']) {
      sent.length = 0;
      pickUpload('Main', '/srv/tv', true);
      const t = sent[0].token;
      ready(t, { setting, existing: { count: 0, same: 0, newer: 0, older: 0, differs: 0, folders: 0 } });
      const shown = body().includes('Nothing of it is there yet.');
      sent.length = 0;
      okBtn.click();
      check(`up ex: nothing there under ${setting}: the setting goes with it`, [shown, sent.map((m) => JSON.stringify(m))],
        [true, [JSON.stringify({ type: 'uploadStart', token: t, choice: setting, overrides: {}, always: false })]]);
    }
    // Esc cancels, the question too; names in the table are text.
    sent.length = 0;
    pickUpload('Main', '/srv/tv', true);
    const c4 = sent[0].token;
    ready(c4, { existing: { ...ex37, files: [{ ...ex37.files[0], name: '<img src=x>' }] } });
    check('up ex: a listed name is text', [$('#modal-body').querySelector('img'), body().includes('<img src=x>')], [null, true]);
    sent.length = 0;
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }));
    check('up ex: Esc cancels the dialog with the question', [$('#modal').hidden, sent.map((m) => JSON.stringify(m))],
      [true, [JSON.stringify({ type: 'uploadCheckCancel', token: c4 })]]);
    check('up ex: the next dialog is narrow again', (() => {
      showModal({ title: 't', html: '', ok: 'OK' });
      const r = wide();
      closeModal(false);
      return r;
    })(), false);

    // The late question: an upload waiting because files turned up there after its check.
    setUploadExisting('ask', false);
    const lateOne = upFile(40, { status: 'paused', waiting: 'A file of that name is already on Main. Resume to choose: replace it, skip it or keep both.',
      ask: { count: 1, same: 0, newer: 1, older: 0, differs: 0, folders: 0,
        files: [{ name: 'u40.mkv', size: 5, modified: 1763700000, mineSize: 9, mineModified: 1763800000, kind: 'newer' }] } });
    sent.length = 0;
    resumeItem(lateOne);
    check('up late: one file: its question, on Skip, Resume', [$('#modal-title').textContent, okBtn.textContent, radio('skip').checked,
      document.querySelectorAll('#modal-body .up-clashes tbody tr').length, outcome(), wide(), sent.length],
      ['“u40.mkv” is already on Main', 'Resume', true, 1, '1 skipped: nothing on Main is replaced', true, 0]);
    okBtn.click();
    check('up late: Resume answers it', sent.map((m) => JSON.stringify(m)),
      [JSON.stringify({ type: 'resolveExisting', id: 40, choice: 'skip', overrides: {}, always: false })]);
    const lateFiles = [{ name: 'holiday/x.jpg', size: 1, modified: 1763800000, mineSize: 1, mineModified: 1763800000, kind: 'same' },
      { name: 'holiday/y.jpg', size: 1, modified: 1763700000, mineSize: 2, mineModified: 1763800000, kind: 'newer' },
      { name: 'holiday/z.jpg', size: 1, modified: 1763900000, mineSize: 2, mineModified: 1763800000, kind: 'older' }];
    const lateMany = upFolder(41, { status: 'paused', waiting: '4 files of those names are already on Main. Resume to choose: replace, skip or keep both.',
      ask: { count: 4, same: 1, newer: 2, older: 1, differs: 0, folders: 0, files: lateFiles } });
    sent.length = 0;
    resumeItem(lateMany);
    check('up late: several: the first listed, the rest as for all', [$('#modal-title').textContent,
      body().includes('4 already there: 1 the same, 2 newer here, 1 older here'), body().includes('…and 1 more, which go as for all.')],
      ['4 files are already on Main', true, true]);
    pickRow(2, 'replace');
    pickAll('newer');
    $('#up-always').checked = true;
    $('#up-always').dispatchEvent(new Event('change', { bubbles: true }));
    check('up late: what happens', outcome(), '3 replaced, 1 skipped');
    okBtn.click();
    check('up late: the answer, per file by place; Always do this the setting now', [sent.map((m) => JSON.stringify(m)),
      state.uploadExisting, $('#up-existing-set').value],
      [[JSON.stringify({ type: 'resolveExisting', id: 41, choice: 'newer', overrides: { 2: 'replace' }, always: true })], 'newer', 'newer']);
    sent.length = 0;
    resumeItem(lateMany);
    closeModal(false);
    check('up late: Cancel leaves it waiting', sent.length, 0);
    check('up late: a download\'s question stays its own', (() => {
      resumeItem(file(42, { status: 'paused', ask: { count: 1, files: [{ name: 'x', size: 1, modified: null, mineSize: 2, mineModified: null }] } }));
      const r = [$('#modal-ok').textContent, !!$('#modal-body .up-clash'), wide()];
      closeModal(false);
      return r;
    })(), ['Replace', false, false]);

    // Settings → Uploads: the six choices; a change goes to the backend; the backend's word shows.
    check('up set: Settings → Uploads offers Ask and the five choices', [...$('#up-existing-set').options].map((o) => [o.value, o.textContent]),
      [['ask', 'Ask'], ['replace', 'Replace'], ['different', 'Replace if different'], ['newer', 'Replace if newer'], ['skip', 'Skip'],
        ['both', 'Keep both']]);
    sent.length = 0;
    $('#up-existing-set').value = 'ask';
    $('#up-existing-set').dispatchEvent(new Event('change', { bubbles: true }));
    check('up set: back to Ask', [sent.map((m) => JSON.stringify(m)), state.uploadExisting],
      [[JSON.stringify({ type: 'setUploadExisting', choice: 'ask' })], 'ask']);
    sent.length = 0;
    setUploadExisting('different', false); // the backend's "uploadExisting"
    check('up set: the backend\'s word shows, nothing sent back', [$('#up-existing-set').value, state.uploadExisting, sent.length],
      ['different', 'different', 0]);
    setUploadExisting('ask', false);
    check('up set: the panel hides with a server form open', (() => {
      const was = settings.form;
      settings.form = { editing: false, name: '' };
      renderServers();
      const hidden = $('#uploads-panel').hidden;
      settings.form = was;
      renderServers();
      return [hidden, $('#uploads-panel').hidden];
    })(), [true, false]);

    Object.assign(b, browseWas);
    state.hits = [];
    state.selected = new Set();
    state.remotes = remotesWas;
    state.remote = remoteWas;
    clearSel();
    dl.cursor = '';
    sent.length = 0;
  }

  // ---- Settings → Appearance: Auto (follow Windows), Light, Dark ---------------------------------
  // A radio group: one Tab stop (the chosen one), the arrows and Home/End choose as a click does; a
  // choice goes to the backend (setTheme), which applies it and says it back; the backend's word
  // (init, "theme") shows and sends nothing; anything else is Auto.
  {
    const viewWas = $('#settings-view').hidden;
    const openWas = settings.open;
    $('#settings-view').hidden = false; // shown, so its buttons take focus
    settings.open = true; // the window's keys as in Settings (Alt+← there isn't Back)
    const group = $('#theme-set');
    const radios = () => [...group.querySelectorAll('button')];
    const look = () => radios().map((r) => `${r.dataset.mode}:${r.getAttribute('aria-checked')}:${r.tabIndex}`);
    const key = (k, mods = {}) => {
      const ev = new KeyboardEvent('keydown', { key: k, bubbles: true, cancelable: true, ...mods });
      document.activeElement.dispatchEvent(ev);
      return ev.defaultPrevented;
    };
    const focused = () => (group.contains(document.activeElement) ? document.activeElement.dataset.mode : null);
    const sentModes = () => sent.filter((m) => m.type === 'setTheme').map((m) => m.mode);
    setTheme('auto', false);
    check('theme: a radio group named Appearance with its three choices',
      [group.getAttribute('role'), $(`#${group.getAttribute('aria-labelledby')}`).textContent,
        radios().map((r) => [r.getAttribute('role'), r.dataset.mode, r.textContent, r.type])],
      ['radiogroup', 'Appearance', [['radio', 'auto', 'Auto (follow Windows)', 'button'], ['radio', 'light', 'Light', 'button'],
        ['radio', 'dark', 'Dark', 'button']]]);
    check('theme: Auto at first, the one Tab stop', look(), ['auto:true:0', 'light:false:-1', 'dark:false:-1']);
    check('theme: the hint says Windows\' own dialogs keep Windows\' look', /Windows' own dialogs.*keep Windows' look/s.test($('#appearance-panel .hint').textContent), true);
    sent.length = 0;
    radios()[1].click();
    check('theme: a click on Light sends it and checks it', [sent.map((m) => JSON.stringify(m)), state.theme, look()],
      [[JSON.stringify({ type: 'setTheme', mode: 'light' })], 'light', ['auto:false:-1', 'light:true:0', 'dark:false:-1']]);
    sent.length = 0;
    radios()[1].click();
    check('theme: a click on the chosen one sends nothing', sent.length, 0);
    radios()[1].focus();
    check('theme: the chosen one takes focus', focused(), 'light');
    const keys = [];
    for (const [k, mods] of [['ArrowRight'], ['ArrowRight'], ['ArrowLeft'], ['ArrowUp'], ['ArrowDown'], ['Home'], ['End'], ['ArrowDown']]) {
      const prevented = key(k, mods);
      keys.push(`${k}:${prevented}:${state.theme}:${focused()}`);
    }
    check('theme: arrows go around, Home and End to the ends; each chooses and moves the focus', [keys, sentModes()],
      [['ArrowRight:true:dark:dark', 'ArrowRight:true:auto:auto', 'ArrowLeft:true:dark:dark', 'ArrowUp:true:light:light',
        'ArrowDown:true:dark:dark', 'Home:true:auto:auto', 'End:true:dark:dark', 'ArrowDown:true:auto:auto'],
      ['dark', 'auto', 'dark', 'light', 'dark', 'auto', 'dark', 'auto']]);
    sent.length = 0;
    check('theme: Home on the chosen first one sends nothing', [key('Home'), sent.length, focused()], [true, 0, 'auto']);
    check('theme: other keys, and arrows with Ctrl or Alt, are left alone',
      [key('Enter'), key('a'), key('ArrowRight', { ctrlKey: true }), key('ArrowLeft', { altKey: true }), sent.length, state.theme],
      [false, false, false, false, 0, 'auto']);
    // The backend's word: init's "theme", and its "theme" after a setTheme.
    setTheme('dark', false);
    check('theme: the backend\'s word shows, nothing sent back', [look(), state.theme, sent.length],
      [['auto:false:-1', 'light:false:-1', 'dark:true:0'], 'dark', 0]);
    for (const odd of [undefined, '', 'sepia', 7]) setTheme(odd, false);
    check('theme: none or an unknown one is Auto', [look(), state.theme, sent.length], [['auto:true:0', 'light:false:-1', 'dark:false:-1'], 'auto', 0]);
    check('theme: the panel hides with a server form open', (() => {
      const was = settings.form;
      settings.form = { editing: false, name: '' };
      renderServers();
      const hidden = $('#appearance-panel').hidden;
      settings.form = was;
      renderServers();
      return [hidden, $('#appearance-panel').hidden];
    })(), [true, false]);
    document.activeElement.blur();
    $('#settings-view').hidden = viewWas;
    settings.open = openWas;
    sent.length = 0;
  }

  // ---- drops from Explorer (UPLOADS.md 5.10, chunk 4) ---------------------------------------------
  // Every drag is kept from navigating the window; over Browse the overlay says where a drop goes
  // (the folder row under the pointer, else the folder open), Browse's refusals and the places
  // outside it say why; text shows nothing, a link is refused; the overlay goes with a drop, the
  // drag leaving or cancelled (dragleave at 0,0), Escape and dragovers stopping; a drop posts its
  // Files beside "uploadDrop" for the same dialog as Upload files here…, never a file WebView2
  // couldn't get (it ends the page for one).
  {
    const b = state.browse;
    const browseWas = { ...b };
    const hitsWas = state.hits;
    const remotesWas = state.remotes;
    const remoteWas = state.remote;
    const reset = () => {
      Object.assign(b, { server: 'Main', path: '/srv', top: false, under: null, loading: false, error: '', uploadable: true,
        severalRoots: false, crumbs: [{ name: 'srv', path: '/srv' }] });
      state.hits = [{ name: 'tv', path: '/srv/tv', dir: true, remote: 'Main' }, { name: 'a.txt', path: '/srv/a.txt', remote: 'Main', size: 3 }];
      renderRows();
    };
    state.remotes = [{ name: 'Main', host: 'h', user: 'u' }];
    state.remote = 'Main';
    state.selected = new Set();
    state.cursor = -1;
    reset();
    // A drag made by a page has no entries behind its files; Explorer's files and folders have.
    const realEntry = DataTransferItem.prototype.webkitGetAsEntry;
    DataTransferItem.prototype.webkitGetAsEntry = function entry() {
      if (this.kind !== 'file') return null;
      return this.getAsFile().name.startsWith('zipped') ? null : { isFile: true, isDirectory: false };
    };
    const posted = [];
    const realPost = postWithObjects;
    // eslint-disable-next-line no-global-assign
    postWithObjects = (m, o) => { posted.push([m, [...o].map((f) => f.name)]); return true; };
    const dragOf = (...names) => {
      const dt = new DataTransfer();
      for (const n of names) dt.items.add(new File(['abc'], n));
      return dt;
    };
    const textDrag = () => {
      const dt = new DataTransfer();
      dt.items.add('some words', 'text/plain');
      return dt;
    };
    // A link from a page in Edge, as WebView2 shows it (seen live, UPLOADS.md chunk 4 fix): text,
    // its URL and its HTML, no file (Edge marks the .url it offers for Chromium to ignore).
    const linkDrag = () => {
      const dt = new DataTransfer();
      dt.items.add('https://example.com/', 'text/plain');
      dt.items.add('https://example.com/', 'text/uri-list');
      dt.items.add('<a href="https://example.com/">up-link</a>', 'text/html');
      return dt;
    };
    // A page's address dragged from Edge's address bar: text, the URL, and the .url shortcut
    // WebView2 saved for the drop (in its temp folder), named .download.
    const addressDrag = () => {
      const dt = new DataTransfer();
      dt.items.add('file:///C:/x/up-link.html', 'text/plain');
      dt.items.add('file:///C:/x/up-link.html', 'text/uri-list');
      dt.items.add(new File(['[InternetShortcut]\r\nURL=file:///C:/x/up-link.html\r\n'], 'up-link.download'));
      return dt;
    };
    // An Internet shortcut of the user's own dragged from Explorer (first of `more` files): WebView2
    // reads its URL into the drag, as text and a text/uri-list.
    const shortcutDrag = (...more) => {
      const dt = new DataTransfer();
      dt.items.add('https://example.org/', 'text/plain');
      dt.items.add('https://example.org/', 'text/uri-list');
      for (const n of ['kept.url', ...more]) dt.items.add(new File(['abc'], n));
      return dt;
    };
    const inside = (el) => {
      const r = el.getBoundingClientRect();
      return { clientX: r.x + Math.min(20, r.width / 2), clientY: r.y + Math.min(10, r.height / 2) };
    };
    const fire = (el, type, dt, where = inside(el)) => {
      const ev = new DragEvent(type, { dataTransfer: dt, bubbles: true, cancelable: true, ...where });
      el.dispatchEvent(ev);
      return ev;
    };
    const overlay = () => ({ shown: !$('#drop-overlay').hidden, text: $('#drop-overlay').textContent,
      refused: $('#drop-overlay').classList.contains('refused'), marked: [...document.querySelectorAll('#rows .row.drop-target')].map((r) => Number(r.dataset.i)) });
    const rowEl = (i) => document.querySelector(`#rows .row[data-i="${i}"]`);
    const said = () => (state.flash && state.flash.until > Date.now() ? state.flash.text : '');
    const quiet = () => { state.flash = null; posted.length = 0; sent.length = 0; endDropOverlay(); };

    // Never the browser's own handling: every dragover and drop is taken, whatever is dragged and
    // wherever it is (the drops here go where none is taken: nothing is posted), but text or a link
    // over a text field, which goes in (or moves) as it always did.
    for (const [what, drag] of [['files', () => dragOf('a.txt')], ['text', textDrag], ['a link', linkDrag]]) {
      quiet();
      const evs = [fire($('#results'), 'dragenter', drag()), fire(document.body, 'dragover', drag()), fire($('#queue'), 'dragover', drag()),
        fire($('#query'), 'dragover', drag()), fire($('#queue'), 'drop', drag())];
      check(`drops: ${what}: dragenter, dragover and drop never left to the browser (text, a link: but over the search box)`, evs.map((e) => e.defaultPrevented),
        [true, true, true, what === 'files', true]);
      check(`drops: ${what}: nothing posted or sent`, [posted.length, sent.length], [0, 0]);
    }
    quiet();
    check('drops: text into the search box or path bar: the field’s own (dragover, drop)',
      [fire($('#query'), 'dragover', textDrag()), fire($('#query'), 'drop', textDrag()), fire($('#address'), 'drop', textDrag())].map((e) => e.defaultPrevented),
      [false, false, false]);
    check('drops: text or a link on a checkbox: still taken',
      [fire($('#select-all'), 'dragover', textDrag()), fire($('#select-all'), 'drop', textDrag()), fire($('#select-all'), 'drop', linkDrag())].map((e) => e.defaultPrevented),
      [true, true, true]);
    check('drops: files over the search box: still taken', fire($('#query'), 'dragover', dragOf('a.txt')).defaultPrevented, true);
    quiet();
    check('drops: a link over the search box: goes in as its URL (dragover, drop)',
      [fire($('#query'), 'dragover', linkDrag()), fire($('#query'), 'drop', linkDrag())].map((e) => e.defaultPrevented), [false, false]);
    // Text that reads as a URL: Chromium adds a text/uri-list to it (seen live, Edge 158), so it
    // looks like a link; a text field still takes it.
    const urlText = (text, uri) => {
      const dt = new DataTransfer();
      dt.items.add(text, 'text/plain');
      dt.items.add(uri, 'text/uri-list');
      return dt;
    };
    check('drops: a path read as a URL into the path bar or the key field: the field’s own (dragover, drop)',
      [fire($('#address'), 'dragover', urlText('Main:/srv/tv', 'main:/srv/tv')), fire($('#address'), 'drop', urlText('Main:/srv/tv', 'main:/srv/tv')),
        fire($('#f-key'), 'dragover', urlText('C:\\Users\\you\\.ssh\\id_ed25519', 'file:///C:/Users/you/.ssh/id_ed25519')),
        fire($('#f-key'), 'drop', urlText('C:\\Users\\you\\.ssh\\id_ed25519', 'file:///C:/Users/you/.ssh/id_ed25519'))].map((e) => e.defaultPrevented),
      [false, false, false, false]);
    check('drops: ... no overlay, nothing posted, sent or said', [overlay().shown, posted.length, sent.length, said()], [false, 0, 0, '']);
    quiet();

    // Where a drop goes.
    fire($('#results'), 'dragover', dragOf('a.txt'));
    check('drops: over Browse: the folder open', overlay(), { shown: true, text: 'Drop to upload to Main:/srv', refused: false, marked: [] });
    fire(rowEl(0), 'dragover', dragOf('a.txt'));
    check('drops: over a folder row: that folder, the row highlighted', overlay(), { shown: true, text: 'Drop to upload to Main:/srv/tv', refused: false, marked: [0] });
    fire(rowEl(1), 'dragover', dragOf('a.txt'));
    check('drops: over a file row: the folder open', overlay(), { shown: true, text: 'Drop to upload to Main:/srv', refused: false, marked: [] });
    const r = $('#drop-overlay').getBoundingClientRect();
    const list = $('#results').getBoundingClientRect();
    check('drops: the overlay covers the list', [r.x, r.y, r.width, r.height].map(Math.round), [list.x, list.y, list.width, list.height].map(Math.round));
    fire($('#results'), 'dragover', textDrag());
    check('drops: text: no overlay', overlay().shown, false);
    fire($('#results'), 'dragover', linkDrag());
    check('drops: a link: refused', overlay(), { shown: true, text: 'Drop files or folders from Explorer: grab uploads files, not links.', refused: true, marked: [] });
    // Files with a URL: an Internet shortcut from Explorer, or a page's address from Edge's address
    // bar; the names tell them apart, and they can't be read before the drop.
    for (const [what, drag] of [['an Internet shortcut from Explorer', shortcutDrag], ['a page’s address from Edge’s address bar', addressDrag]]) {
      fire($('#results'), 'dragover', drag());
      check(`drops: ${what}: told at the drop`, overlay(), { shown: true, text: 'Drop to upload to Main:/srv', refused: false, marked: [] });
    }
    fire($('#queue'), 'dragover', dragOf('a.txt'));
    check('drops: over Downloads: where to drop', [overlay().text, overlay().refused],
      ['Drop on Browse, above, to upload: into the folder it shows, or onto a folder in it.', true]);
    fire($('.bar.status'), 'dragover', dragOf('a.txt'));
    const sr = $('#drop-overlay').getBoundingClientRect();
    const bar = $('.bar.status').getBoundingClientRect();
    check('drops: over the status bar: where to drop, over the bar', [overlay().text, overlay().refused, [sr.y, sr.height].map(Math.round)],
      ['Drop on Browse, above, to upload: into the folder it shows, or onto a folder in it.', true, [bar.y, bar.height].map(Math.round)]);
    const refusedAt = (over, el = $('#results')) => {
      reset();
      Object.assign(b, over);
      renderRows();
      fire(typeof el === 'function' ? el() : el, 'dragover', dragOf('a.txt'));
      const o = overlay();
      reset();
      return [o.refused, o.text, o.marked];
    };
    check('drops: search results refuse', refusedAt({ under: { path: '/srv', name: 'srv' } }),
      [true, 'Search results aren’t a folder: close the search (Esc) to upload into the folder.', []]);
    check('drops: a folder found by a search refuses', refusedAt({ under: { path: '/srv', name: 'srv' } }, () => rowEl(0)),
      [true, 'Uploads go into a folder you have open, not into search results: open this folder first.', []]);
    check('drops: All servers refuses', refusedAt({ server: '' }),
      [true, 'Open a folder of a server first: uploads go into one folder of one server.', []]);
    check('drops: the made-up top of several roots refuses', refusedAt({ top: true, severalRoots: true, uploadable: false, path: '' }),
      [true, 'This is the list of the server’s search roots, not a folder: open one of them first.', []]);
    check('drops: ... but one of its roots takes a drop', refusedAt({ top: true, severalRoots: true, uploadable: false, path: '' }, () => rowEl(0)),
      [false, 'Drop to upload to Main:/srv/tv', [0]]);
    check('drops: a folder still opening refuses, its rows too', [refusedAt({ loading: true }), refusedAt({ loading: true }, () => rowEl(0))],
      [[true, 'srv is still opening.', []], [true, 'srv is still opening.', []]]);
    showModal({ title: 'other', html: '', ok: 'OK' });
    fire($('#results'), 'dragover', dragOf('a.txt'));
    check('drops: a dialog open: finish it first', [overlay().refused, overlay().text], [true, 'Finish the open dialog first, then drop again.']);
    closeModal(false);
    settings.open = true;
    fire($('#results'), 'dragover', dragOf('a.txt'));
    check('drops: Settings open: close it first', overlay().text, 'Close Settings first, then drop on a folder in Browse.');
    settings.open = false;
    // A PDF preview is a frame its drags never leave: while a drag is on, the frame lets them through
    // to the page (hit-testing skips it), so the preview's refusal shows over it too.
    quiet();
    viewer.open = true;
    $('#viewer').hidden = false;
    const pdf = document.createElement('iframe');
    pdf.className = 'viewer-pdf';
    $('#viewer-body').className = 'viewer-body pdf';
    $('#viewer-body').replaceChildren(pdf);
    const pdfHit = () => {
      const fr = pdf.getBoundingClientRect();
      return [document.elementFromPoint(fr.x + fr.width / 2, fr.y + fr.height / 2) === pdf, getComputedStyle(pdf).pointerEvents];
    };
    check('drops: a PDF preview, no drag: the frame takes the pointer', pdfHit(), [true, 'auto']);
    fire($('.viewer-bar'), 'dragover', dragOf('a.txt'));
    check('drops: a file drag over the preview’s bar: refused, the PDF frame lets drags through',
      [overlay().text, ...pdfHit()], ['Close the preview first, then drop on a folder in Browse.', false, 'none']);
    endDropOverlay();
    check('drops: ... and takes the pointer again when the drag is over', pdfHit(), [true, 'auto']);
    viewer.open = false;
    $('#viewer').hidden = true;
    $('#viewer-body').replaceChildren();

    // The overlay never stays.
    quiet();
    fire($('#results'), 'dragover', dragOf('a.txt'));
    fire(rowEl(0), 'dragleave', dragOf('a.txt'));
    check('drops: leaving a row for another: still shown', overlay().shown, true);
    fire($('#results'), 'dragleave', dragOf('a.txt'), { clientX: 0, clientY: 0 });
    check('drops: out of the window, or cancelled (dragleave at 0,0): gone, nothing said', [overlay().shown, said()], [false, '']);
    fire($('#queue'), 'dragover', dragOf('a.txt'));
    fire($('#queue'), 'dragleave', dragOf('a.txt'), { clientX: 0, clientY: 0 });
    check('drops: let go where no drop is taken: gone, the reason stays in the footer',
      [overlay().shown, said()], [false, 'Drop on Browse, above, to upload: into the folder it shows, or onto a folder in it.']);
    quiet();
    fire($('#results'), 'dragover', dragOf('a.txt'));
    fire($('#queue'), 'drop', dragOf('a.txt'));
    check('drops: a drop elsewhere: gone, nothing posted, said why', [overlay().shown, posted.length, said()],
      [false, 0, 'Drop on Browse, above, to upload: into the folder it shows, or onto a folder in it.']);
    fire($('#results'), 'dragover', dragOf('a.txt'));
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }));
    check('drops: Escape: gone', overlay(), { shown: false, text: 'Drop to upload to Main:/srv', refused: false, marked: [] });
    const idleWas = dropIdleMs;
    // eslint-disable-next-line no-global-assign
    dropIdleMs = 30;
    fire(rowEl(0), 'dragover', dragOf('a.txt'));
    check('drops: shown while dragovers come', overlay().shown, true);
    for (let i = 0; i < 400 && overlay().shown; i++) await new Promise((done) => setTimeout(done, 5));
    check('drops: dragovers stopped (the drag ended outside): gone, the row too', [overlay().shown, overlay().marked], [false, []]);
    // eslint-disable-next-line no-global-assign
    dropIdleMs = idleWas;

    // Drops: the Files beside "uploadDrop", the folder named; nothing through send().
    quiet();
    fire(rowEl(0), 'dragover', dragOf('a.txt', 'b.txt', 'c.txt'));
    const d1 = fire(rowEl(0), 'drop', dragOf('a.txt', 'b.txt', 'c.txt'));
    const [m1, o1] = posted[0] || [{}, []];
    check('drops: 3 files on a folder row: into that folder', [d1.defaultPrevented, m1.type, m1.server, m1.dir, m1.token === upload.token && m1.token > 0, o1, sent.length, overlay().shown],
      [true, 'uploadDrop', 'Main', '/srv/tv', true, ['a.txt', 'b.txt', 'c.txt'], 0, false]);
    check('drops: the dialog waits for it', upload.pending, { token: m1.token, server: 'Main', dir: '/srv/tv' });
    onUploadCheck({ type: 'uploadCheck', token: m1.token, phase: 'local', server: 'Main', dir: '/srv/tv', items: [], count: 0, files: 0, bytes: 0,
      existing: { count: 0 } });
    check('drops: ... the same dialog as Upload files here…', [$('#modal').hidden, $('#modal-title').textContent, $('#modal-ok').disabled],
      [false, 'Upload to Main:/srv/tv', true]);
    closeModal(false);
    quiet();
    fire($('#results'), 'drop', dragOf('holiday', 'notes.txt', 'b.png'));
    check('drops: a folder and files on the list: into the folder open', posted.map(([m, o]) => [m.dir, o]), [['/srv', ['holiday', 'notes.txt', 'b.png']]]);
    quiet();
    fire($('#results'), 'drop', dragOf('zipped.png'));
    check('drops: a file WebView2 couldn’t get (from inside a .zip): never posted, said', [posted.length, said()], [0, EXTRACT_FIRST]);
    quiet();
    fire($('#results'), 'drop', dragOf('a.txt', 'zipped.png'));
    check('drops: ... not even beside others', [posted.length, said()], [0, EXTRACT_FIRST]);
    quiet();
    fire($('#results'), 'drop', linkDrag());
    check('drops: a link: refused', [posted.length, said()], [0, 'Drop files or folders from Explorer: grab uploads files, not links.']);
    // Edge's link let go on Browse: WebView2 has no drop for it (the page took none) but a dragleave
    // at 0,0: said all the same.
    quiet();
    fire($('#results'), 'dragenter', linkDrag());
    fire($('#results'), 'dragover', linkDrag());
    fire($('#results'), 'dragleave', linkDrag(), { clientX: 0, clientY: 0 });
    check('drops: a link from Edge let go on Browse (no drop, dragleave at 0,0): refused, said', [overlay().shown, posted.length, sent.length, said()],
      [false, 0, 0, DROP_FILES]);
    quiet();
    fire($('#results'), 'dragover', addressDrag());
    fire($('#results'), 'drop', addressDrag());
    check('drops: a page’s address from Edge’s address bar: refused at the drop, nothing posted', [overlay().shown, posted.length, sent.length, said()],
      [false, 0, 0, DROP_FILES]);
    // Over a place that refuses drops the page takes none (dropEffect none), so WebView2 has no drop
    // there, only a dragleave at 0,0: the place's words (the names that tell a link can't be read
    // before a drop).
    quiet();
    fire($('#queue'), 'dragover', addressDrag());
    fire($('#queue'), 'dragleave', addressDrag(), { clientX: 0, clientY: 0 });
    check('drops: ... let go over Downloads (no drop, dragleave at 0,0): the place’s words', [overlay().shown, posted.length, sent.length, said()],
      [false, 0, 0, 'Drop on Browse, above, to upload: into the folder it shows, or onto a folder in it.']);
    quiet();
    fire($('#results'), 'drop', shortcutDrag());
    check('drops: an Internet shortcut (.url) from Explorer: a file, posted, its URL noted for the host',
      posted.map(([m, o]) => [m.dir, m.url, o]), [['/srv', true, ['kept.url']]]);
    quiet();
    fire($('#results'), 'drop', shortcutDrag('a.txt', 'holiday'));
    check('drops: ... beside other files and folders', posted.map(([m, o]) => [m.url, o]), [[true, ['kept.url', 'a.txt', 'holiday']]]);
    quiet();
    fire($('#results'), 'drop', dragOf('a.txt'));
    check('drops: files alone: no URL noted', posted.map(([m]) => m.url), [false]);
    quiet();
    fire($('#results'), 'drop', shortcutDrag());
    onUploadCheck({ type: 'uploadCheck', token: posted[0][0].token, phase: 'refused', server: 'Main', dir: '/srv', items: [], count: 0, why: DROP_FILES });
    check('drops: a browser’s shortcut the host finds is WebView2’s copy: refused as a link, no dialog', [$('#modal').hidden, said(), upload.pending],
      [true, DROP_FILES, null]);
    quiet();
    fire($('#results'), 'drop', textDrag());
    check('drops: text: nothing', [posted.length, said()], [0, '']);
    // The host refuses before looking (no paths, or only WebView2's copies of zipped files): said,
    // no dialog.
    quiet();
    fire($('#results'), 'drop', dragOf('mail.png'));
    const t2 = posted[0][0].token;
    onUploadCheck({ type: 'uploadCheck', token: t2, phase: 'refused', server: 'Main', dir: '/srv', items: [], count: 0, why: 'Extract files from a .zip first: ...' });
    check('drops: the host refuses: said, no dialog', [$('#modal').hidden, said(), upload.pending], [true, 'Extract files from a .zip first: ...', null]);
    // A WebView2 that can't post objects.
    quiet();
    // eslint-disable-next-line no-global-assign
    postWithObjects = () => false;
    fire($('#results'), 'drop', dragOf('a.txt'));
    check('drops: no way to hand files over: said', [upload.pending, said()], [null, 'This window can’t take dropped files: use Upload files here… instead.']);
    // eslint-disable-next-line no-global-assign
    postWithObjects = (m, o) => { posted.push([m, [...o].map((f) => f.name)]); return true; };

    // 2,000 files at once: one post, quickly.
    quiet();
    const many = dragOf(...Array.from({ length: 2000 }, (_, i) => `f${i}.jpg`));
    const t0 = performance.now();
    fire($('#results'), 'dragover', many);
    fire($('#results'), 'drop', many);
    const ms = performance.now() - t0;
    out.push(`INFO drops: 2,000 files dropped and posted in ${ms.toFixed(1)} ms`);
    check('drops: 2,000 files: one post with all of them, under 500 ms', [posted.length, posted[0] && posted[0][1].length, ms < 500], [1, 2000, true]);

    // The window opened the page again (its renderer ended): what happened, and during a drop why.
    remember();
    let kept = null;
    try { kept = JSON.parse(sessionStorage.getItem('grab.paths')); } catch (e) { kept = 'no storage'; }
    check('drops: Browse\'s folders kept for a page opened again', kept, state.browsePaths);
    pageWas.drag = Date.now();
    onPageRestored({ type: 'pageRestored', reason: 'ended' });
    check('drops: a page opened again during a drop says why', [$('#banner').hidden, $('#banner').textContent.startsWith('grab’s window was ended by WebView2 and was opened again'),
      $('#banner').textContent.includes(EXTRACT_FIRST), pageWas.drag], [false, true, true, 0]);
    onPageRestored({ type: 'pageRestored', reason: 'error page' });
    check('drops: ... and without a drop only what happened', [$('#banner').textContent.includes('was replaced by an error page'), $('#banner').textContent.includes('.zip')], [true, false]);
    showBanner('');

    DataTransferItem.prototype.webkitGetAsEntry = realEntry;
    // eslint-disable-next-line no-global-assign
    postWithObjects = realPost;
    upload.pending = null;
    quiet();
    Object.assign(b, browseWas);
    state.hits = hitsWas;
    state.remotes = remotesWas;
    state.remote = remoteWas;
    renderRows();
  }

  // ---- Browse's changes on a server (SERVER-OPS.md, chunk 2) --------------------------------------
  // The menus' entries on rows, folder rows, search results and the list's empty part, with their
  // keys; a read-only server greying every change with the reason; the clipboard (cut rows faded,
  // the hint, Esc, another server's items); Paste's look, question (relabelled, on Skip) and run;
  // Rename's and New folder's name box; Delete's confirmation and counts; Properties with its
  // folder size and sums; the footer's line with Cancel and the results; the keys only in the list.
  {
    const b = state.browse;
    const browseWas = { ...b };
    const hitsWas = state.hits;
    const remotesWas = state.remotes;
    const remoteWas = state.remote;
    const list = $('#results');
    const key = (k, mods = {}, el = list) => {
      const ev = new KeyboardEvent('keydown', { key: k, bubbles: true, cancelable: true, ...mods });
      el.dispatchEvent(ev);
      return ev.defaultPrevented;
    };
    const sentOf = (type) => sent.filter((m) => m.type === type);
    const lastOf = (type) => sentOf(type).pop();
    const reply = (msg) => onFilesDone({ type: 'filesDone', ...msg });
    const said = () => (ops.result && ops.result.until > Date.now() ? ops.result.text : '');
    const line = () => ({ shown: !$('#ops').hidden, text: $('#ops-text').textContent, cancel: !$('#ops-cancel').hidden, bad: $('#ops').classList.contains('bad') });
    const labels = (items) => items.map((m) => (m === '-' ? '-' : m.heading ? `# ${m.heading}` : `${m.label}${m.disabled ? ' (off)' : ''}`));
    const entry = (items, label) => items.find((m) => m.label === label);
    const pick = (...i) => {
      state.selected = new Set(i);
      state.cursor = i[0];
      renderRows();
    };
    const items = [{ name: 'tv', path: '/srv/tv', dir: true, remote: 'Main' }, { name: 'a.txt', path: '/srv/a.txt', remote: 'Main', size: 3 },
      { name: 'b.mkv', path: '/srv/b.mkv', remote: 'Main', size: 3435973837 }];
    const reset = (server = 'Main') => {
      Object.assign(b, { server, path: '/srv', top: false, under: null, loading: false, error: '', uploadable: true, severalRoots: false,
        crumbs: [{ name: 'srv', path: '/srv' }], items: items.map((it) => ({ ...it, remote: server })), pendingSelect: null });
      state.hits = b.items.slice();
      state.selected = new Set();
      state.cursor = 0;
      renderRows();
    };
    const quiet = () => {
      sent.length = 0;
      ops.result = null;
      ops.running = null;
      ops.refusal = null;
      ops.waits.clear();
      clearClip();
      if (modal) closeModal(false);
      closeMenu();
      renderOps();
    };
    state.remotes = [{ name: 'Main', host: 'h', user: 'u' }, { name: 'Box', host: 'b', user: 'u' }, { name: 'Box.Guest', host: 'b', user: 'g' }];
    state.remote = 'Main';
    ops.accounts.clear();
    ops.asking.clear();
    quiet();
    reset();

    // The account: asked once a session when Browse opens a server, again on Refresh; a failed ask
    // is asked again the next time.
    sent.length = 0;
    b.id = 41;
    onBrowseResult({ type: 'browseResult', id: 41, remote: 'Main', path: '/srv', top: false, items: [], uploadable: true });
    onBrowseResult({ type: 'browseResult', id: 41, remote: 'Main', path: '/srv', top: false, items: [], uploadable: true });
    check('ops: Browse opening a server asks its account once', sentOf('filesAccount'), [{ type: 'filesAccount', server: 'Main', again: false }]);
    onFilesAccount({ type: 'filesAccount', server: 'Main', readOnly: false, free: 1000 });
    onBrowseResult({ type: 'browseResult', id: 41, remote: 'Main', path: '/srv', top: false, items: [], uploadable: true });
    check('ops: ... and not again once known', [sentOf('filesAccount').length, ops.accounts.get('Main')],
      [1, { readOnly: false, free: 1000, home: '', copyData: true }]);
    sent.length = 0;
    $('#browse-refresh').click();
    check('ops: Refresh asks it again', sentOf('filesAccount'), [{ type: 'filesAccount', server: 'Main', again: true }]);
    onFilesAccount({ type: 'filesAccount', server: 'Main', readOnly: false });
    sent.length = 0;
    b.id = 42;
    b.server = 'Box.Guest';
    onBrowseResult({ type: 'browseResult', id: 42, remote: 'Box.Guest', path: '/srv', top: false, items: [], uploadable: true });
    onFilesAccount({ type: 'filesAccount', server: 'Box.Guest', error: 'no route' });
    onBrowseResult({ type: 'browseResult', id: 42, remote: 'Box.Guest', path: '/srv', top: false, items: [], uploadable: true });
    check('ops: a failed ask is asked again the next time', sentOf('filesAccount').length, 2);
    onFilesAccount({ type: 'filesAccount', server: 'Box.Guest', readOnly: true });
    // An All servers search: the hits' servers Browse hasn't opened are asked too (once), a
    // server no longer configured never.
    sent.length = 0;
    const searchWas = { results: state.results, lists: state.lists, searchId: state.searchId, filterTotal: state.filterTotal, resultNote: state.resultNote };
    b.under = { query: 'a' };
    state.searchId = 77;
    const searchHits = () => [{ name: 'a.txt', path: '/x/a.txt', remote: 'Box' }, { name: 'a.mkv', path: '/y/a.mkv', remote: 'Main' },
      { name: 'a.iso', path: '/z/a.iso', remote: 'Box' }, { name: 'a.bin', path: '/w/a.bin', remote: 'Gone' }];
    onSearchResult({ type: 'searchResult', id: 77, hits: searchHits(), lists: [], elapsedMs: 10 });
    onSearchResult({ type: 'searchResult', id: 77, hits: searchHits(), lists: [], elapsedMs: 10, update: true });
    check('ops: search hits of a server not opened: its account asked once', sentOf('filesAccount'),
      [{ type: 'filesAccount', server: 'Box', again: false }]);
    b.under = null;
    Object.assign(state, searchWas);
    onFilesAccount({ type: 'filesAccount', server: 'Box', readOnly: false });
    quiet();
    reset();

    // The menus: a file row, a folder row, several rows, the empty part.
    pick(1);
    check('ops: a file row: Cut, Copy, Paste into the open folder, Rename, Delete, Properties last', labels(resultMenu()),
      ['Preview', 'Download', 'Download to…', '-', 'Cut', 'Copy', 'Paste into srv (off)', '-', 'Rename…', 'Delete…', '-',
        'Upload files to srv…', 'Upload folder to srv…', '-', 'Copy path', 'Copy name', '-', 'Properties']);
    check('ops: the keys beside them', ['Cut', 'Copy', 'Paste into srv', 'Rename…', 'Delete…', 'Properties'].map((l) => entry(resultMenu(), l).key),
      ['Ctrl+X', 'Ctrl+C', 'Ctrl+V', 'F2', 'Del', 'Alt+Enter']);
    check('ops: Paste greyed with nothing held, saying so', [entry(resultMenu(), 'Paste into srv').note, entry(resultMenu(), 'Paste into srv').why],
      ['nothing cut or copied', 'Nothing to paste: Cut or Copy files or folders first.']);
    pick(0);
    check('ops: a folder row: Paste into that folder', labels(resultMenu()).filter((l) => l.startsWith('Paste')), ['Paste (off)']);
    pick(1, 2);
    const several = resultMenu();
    check('ops: several rows: Rename and Properties greyed, saying why; Delete names how many',
      [entry(several, 'Rename…').disabled, entry(several, 'Rename…').why, entry(several, 'Properties').why, !!entry(several, 'Delete 2…')],
      [true, 'Rename one item at a time.', 'Properties shows one item at a time: pick one.', true]);
    check('ops: the empty part: Paste, New folder, uploads, Properties of the open folder', labels(folderMenu()),
      ['Paste (off)', 'New folder…', '-', 'Upload files here…', 'Upload folder here…', '-', 'Properties']);
    // Shown: a greyed entry's reason on hover, its few words beside it.
    closeMenu();
    showMenu(10, 10, folderMenu());
    const pasteBtn = $('#menu button[data-i="0"]');
    check('ops: a greyed entry shows its few words and its reason on hover', [pasteBtn.disabled, pasteBtn.querySelector('.note').textContent,
      pasteBtn.title, pasteBtn.querySelector('.key').textContent], [true, 'nothing cut or copied', 'Nothing to paste: Cut or Copy files or folders first.', 'Ctrl+V']);
    closeMenu();

    // A read-only server (Box.Guest): every change greyed with the reason; Copy and Properties work.
    reset('Box.Guest');
    pick(1);
    const ro = resultMenu();
    check('ops: read-only: the reason heads the entries, every change greyed', [labels(ro).filter((l) => !l.startsWith('Upload')),
      entry(ro, 'Cut').why, entry(ro, 'Rename…').why, entry(ro, 'Delete…').why],
    [['Preview', 'Download', 'Download to…', '-', '# Box.Guest is read-only for this account', 'Cut (off)', 'Copy', 'Paste into srv (off)', '-',
      'Rename… (off)', 'Delete… (off)', '-', '-', 'Copy path', 'Copy name', '-', 'Properties'],
    'Box.Guest is read-only for this account', 'Box.Guest is read-only for this account', 'Box.Guest is read-only for this account']);
    check('ops: read-only: the empty part too, Properties left', labels(folderMenu()),
      ['# Box.Guest is read-only for this account', 'Paste (off)', 'New folder… (off)', '-', 'Upload files here…', 'Upload folder here…', '-', 'Properties']);
    check('ops: read-only: New folder says why', entry(folderMenu(), 'New folder…').why, 'Box.Guest is read-only for this account');
    showMenu(10, 10, ro);
    const cutBtn = [...$('#menu').querySelectorAll('button')].find((x) => x.querySelector('.text').textContent === 'Cut');
    check('ops: read-only: a screen reader hears the reason (the heading describes the menu, a greyed entry its own)',
      [$('#menu').getAttribute('aria-describedby'), $('#menu-heading').textContent, cutBtn.getAttribute('aria-description')],
      ['menu-heading', 'Box.Guest is read-only for this account', 'Box.Guest is read-only for this account']);
    closeMenu();
    showMenu(10, 10, folderMenu().filter((m) => !m.heading));
    check('ops: ... a menu without a heading has no description', $('#menu').getAttribute('aria-describedby'), null);
    closeMenu();
    sent.length = 0;
    const refusedKeys = [['F2'], ['Delete'], ['x', { ctrlKey: true }], ['v', { ctrlKey: true }], ['N', { ctrlKey: true, shiftKey: true }]].map(([k, m]) => {
      ops.result = null;
      key(k, m);
      return said();
    });
    check('ops: read-only: F2, Del, Ctrl+X, Ctrl+V, Ctrl+Shift+N say the reason in the footer, nothing sent',
      [refusedKeys, sent.length, !!modal], [Array(5).fill('Box.Guest is read-only for this account'), 0, false]);
    check('ops: the reason in red in the footer', line(), { shown: true, text: 'Box.Guest is read-only for this account', cancel: false, bad: true });
    quiet();
    reset();

    // The login folder, "/" and a folder holding the login folder, however each is spelled: Cut,
    // Rename and Delete greyed with the reason (the backend refuses them too), F2, Del and Ctrl+X
    // say it; Copy works. A folder inside the login folder is an ordinary row.
    onFilesAccount({ type: 'filesAccount', server: 'Main', readOnly: false, free: 1000, home: '/home/u', copyData: true });
    check('ops: the account keeps the login folder and copy-data', ops.accounts.get('Main'),
      { readOnly: false, free: 1000, home: '/home/u', copyData: true });
    const LOGIN = 'the login folder can’t be renamed, moved or deleted';
    const TOP = 'the server’s top folder can’t be renamed, moved or deleted';
    const tops = [['/home/u', LOGIN], ['/home/./u/', LOGIN], ['/home/u/d/..', LOGIN], ['', LOGIN], ['.', LOGIN], ['x/..', LOGIN], ['/', TOP],
      ['//', TOP], ['/home/..', TOP], ['/home', 'home holds the login folder, which can’t be renamed, moved or deleted'],
      ['/home/u/d', ''], ['d', ''], ['/home/uu', ''], ['/srv/tv', '']];
    const topRows = () => tops.map(([path]) => ({ name: path.split('/').filter(Boolean).pop() || path || '~', path, dir: true, remote: 'Main' }));
    const whys = () => tops.map((_, i) => {
      pick(i);
      const m = resultMenu();
      return [entry(m, 'Cut').why, entry(m, 'Rename…').why, entry(m, 'Delete…').why, entry(m, 'Copy').disabled];
    });
    state.hits = topRows();
    renderRows();
    check('ops: the login folder, / and a folder holding it greyed however spelled, with the reason; Copy left; one inside ordinary', whys(),
      tops.map(([, why]) => [why, why, why, false]));
    sent.length = 0;
    pick(0);
    const loginKeys = [['F2'], ['Delete'], ['x', { ctrlKey: true }]].map(([k, m]) => {
      ops.result = null;
      key(k, m);
      return said();
    });
    check('ops: ... F2, Del and Ctrl+X on the login folder by its full path say why, nothing sent, nothing held',
      [loginKeys, sent.length, !!modal, ops.clip], [Array(3).fill(LOGIN), 0, false, null]);
    pick(0, 10);
    check('ops: ... several rows with it among them: refused the same', entry(resultMenu(), 'Delete 2…').why, LOGIN);
    // The login folder not known yet: only its spellings from itself and "/" are greyed (the backend
    // still refuses the rest, with the real path it asks for).
    onFilesAccount({ type: 'filesAccount', server: 'Main', readOnly: false });
    check('ops: ... the login folder not known: "", ".", "/" still greyed', whys().map(([cut]) => cut),
      ['', '', '', LOGIN, LOGIN, LOGIN, TOP, TOP, TOP, '', '', '', '', '']);
    quiet();
    reset();

    // A server without copy-data (OpenSSH before 9.0): Paste of a copy greyed there with the reason,
    // Ctrl+V says it and sends nothing; a cut pastes there (a move is a rename).
    onFilesAccount({ type: 'filesAccount', server: 'Box', readOnly: false, home: '/home/b', copyData: false });
    reset('Box');
    pick(1);
    key('c', { ctrlKey: true });
    const NO_COPY = 'Box doesn’t copy files itself (its SSH server is older than OpenSSH 9.0)';
    check('ops: no copy-data: Paste of a copy greyed, saying why', [entry(resultMenu(), 'Paste into srv').disabled, entry(resultMenu(), 'Paste into srv').why,
      entry(folderMenu(), 'Paste').why, entry(folderMenu(), 'Paste').note], [true, NO_COPY, NO_COPY, 'the server can’t copy']);
    pick(0);
    check('ops: ... into a folder row too', entry(resultMenu(), 'Paste').why, NO_COPY);
    sent.length = 0;
    ops.result = null;
    key('v', { ctrlKey: true });
    check('ops: ... Ctrl+V says it, nothing looked at', [said(), sentOf('filesLook').length, !!modal], [NO_COPY, 0, false]);
    pick(1);
    key('x', { ctrlKey: true });
    check('ops: ... a cut pastes there', [entry(folderMenu(), 'Paste').disabled, entry(resultMenu(), 'Paste into srv').disabled], [false, false]);
    key('v', { ctrlKey: true });
    check('ops: ... Ctrl+V looks at the move', [lastOf('filesLook') && lastOf('filesLook').op, lastOf('filesLook') && lastOf('filesLook').server],
      ['move', 'Box']);
    onFilesAccount({ type: 'filesAccount', server: 'Box', readOnly: false });
    check('ops: ... copy-data taken as there when the backend doesn’t say', ops.accounts.get('Box').copyData, true);
    quiet();
    reset();

    // The clipboard: Ctrl+X on two rows fades them and says so; Esc clears; Ctrl+C fades nothing.
    pick(1, 2);
    check('ops: Ctrl+X taken', key('x', { ctrlKey: true }), true);
    const faded = () => [...document.querySelectorAll('#rows .row.cut')].map((r) => Number(r.dataset.i));
    check('ops: cut: held, the rows faded, the hint', [ops.clip.op, ops.clip.server, ops.clip.items.map((i) => i.path), faded(), $('#clip').hidden, $('#clip').textContent],
      ['cut', 'Main', ['/srv/a.txt', '/srv/b.mkv'], [1, 2], false, '2 items ready to paste · Esc clears']);
    check('ops: the hint names them and what Paste does', $('#clip').title, 'Cut on Main: a.txt, b.mkv. Paste (Ctrl+V) in a folder of Main moves them there.');
    check('ops: Esc clears it', [key('Escape'), ops.clip, faded(), $('#clip').hidden], [true, null, [], true]);
    pick(1);
    key('c', { ctrlKey: true });
    check('ops: Ctrl+C: held for a copy, nothing faded', [ops.clip.op, faded(), $('#clip').textContent], ['copy', [], '1 item ready to paste · Esc clears']);
    // Another server's items: not yet.
    reset('Box');
    pick(1);
    check('ops: another server\'s items: Paste greyed, not yet', [entry(resultMenu(), 'Paste into srv').note, entry(folderMenu(), 'Paste').note],
      ['not yet: copies between servers', 'not yet: copies between servers']);
    sent.length = 0;
    key('v', { ctrlKey: true });
    check('ops: ... Ctrl+V says it, nothing sent', [said(), sent.length],
      ['Not yet: copies between servers. What you cut or copied is on Main: paste it in a folder of Main.', 0]);
    quiet();
    reset();

    // Paste: the look, nothing taken there: the copy at once, Skip for anything taken meanwhile;
    // its progress with Cancel; the result; a copy keeps the clipboard.
    pick(1);
    key('c', { ctrlKey: true });
    sent.length = 0;
    key('v', { ctrlKey: true });
    const look = lastOf('filesLook');
    check('ops: Ctrl+V looks first: into the open folder', [look.server, look.op, look.paths, look.dir, line()],
      ['Main', 'copy', ['/srv/a.txt'], '/srv', { shown: true, text: 'Looking at a.txt and srv…', cancel: true, bad: false }]);
    sent.length = 0;
    key('F2');
    check('ops: one change at a time: another says so in the footer, over the line, in red', [line(), sentOf('filesRename').length, !!modal],
      [{ shown: true, text: 'Looking at a.txt is still going: wait until it ends, or cancel it first', cancel: true, bad: true }, 0, false]);
    reply({ token: look.token, op: 'look', files: 1, folders: 0, bytes: 3, existing: { count: 0, same: 0, newer: 0, older: 0, differs: 0, folders: 0, files: [] } });
    const paste = lastOf('filesPaste');
    check('ops: nothing taken: pasted at once, Skip for a name taken meanwhile', [paste.op, paste.paths, paste.dir, paste.choice, paste.own, !!modal],
      ['copy', ['/srv/a.txt'], '/srv', 'skip', [], false]);
    onFilesProgress({ type: 'filesProgress', token: paste.token, bytes: 1288490189, totalBytes: 3650722202, files: 0, totalFiles: 1 });
    check('ops: the progress line with Cancel', line(), { shown: true, text: 'Copying a.txt to Main:/srv · 1.2 of 3.4 GiB', cancel: true, bad: false });
    // Del, F2 or Ctrl+Shift+N while it runs: the refusal over the progress for a while, then the
    // progress again; the menu's entries say the same.
    sent.length = 0;
    key('Delete');
    check('ops: a change asked for while one runs: refused in the footer, Cancel kept', [line(), sent.length, !!modal],
      [{ shown: true, text: 'Copying a.txt is still going: wait until it ends, or cancel it first', cancel: true, bad: true }, 0, false]);
    check('ops: ... the progress still on hover', $('#ops').title, 'Copying a.txt is still going: wait until it ends, or cancel it first\nCopying a.txt to Main:/srv · 1.2 of 3.4 GiB');
    entry(resultMenu(), 'Rename…').act();
    check('ops: ... Rename from the menu too', [line().text, !!modal], ['Copying a.txt is still going: wait until it ends, or cancel it first', false]);
    ops.refusal.until = 0;
    renderOps();
    check('ops: ... then the progress again', line(), { shown: true, text: 'Copying a.txt to Main:/srv · 1.2 of 3.4 GiB', cancel: true, bad: false });
    sent.length = 0;
    $('#ops-cancel').click();
    check('ops: Cancel stops it', sent, [{ type: 'filesCancel', token: paste.token }]);
    reply({ token: paste.token, op: 'copy', ok: false, stopped: true, files: 0, folders: 0, bytes: 0, left: 0, placed: [''], entries: [], skipped: 0, failed: 0 });
    check('ops: cancelled: said, the line ends', line(), { shown: true, text: 'Copy cancelled: nothing copied', cancel: false, bad: false });
    check('ops: a copy keeps the clipboard for more pastes', ops.clip && ops.clip.op, 'copy');
    // Again, done.
    sent.length = 0;
    pasteInto('Main', '/srv/tv', 'tv');
    reply({ token: lastOf('filesLook').token, op: 'look', files: 1, folders: 0, bytes: 3, existing: { count: 0 } });
    const second = lastOf('filesPaste');
    reply({ token: second.token, op: 'copy', ok: true, stopped: false, files: 1, folders: 0, bytes: 3, left: 0, placed: ['/srv/tv/a.txt'],
      entries: [], skipped: 0, failed: 0 });
    check('ops: done: a short result', line(), { shown: true, text: 'Copied 1 file (3 B) to Main:/srv/tv', cancel: false, bad: false });

    // Names taken: the uploads' question, relabelled, on Skip, without "Always"; own choices sent.
    quiet();
    reset();
    pick(0, 1);
    key('x', { ctrlKey: true });
    pasteInto('Main', '/dst', 'dst');
    const look2 = lastOf('filesLook');
    check('ops: a cut pastes as a move', look2.op, 'move');
    const clashes = { count: 2, same: 0, newer: 1, older: 0, differs: 1, folders: 0,
      foldersOntoFiles: { count: 1, same: 0, newer: 0, older: 0, differs: 1, folders: 0 }, files: [
      { name: 'tv', from: '/srv/tv', to: '/dst/tv', size: 3, modified: 1700000000, mineSize: 4096, mineModified: 1700000000, kind: 'differs' },
      { name: 'a.txt', from: '/srv/a.txt', to: '/dst/a.txt', size: 2, modified: 1700000000, mineSize: 3, mineModified: 1800000000, kind: 'newer' }] };
    reply({ token: look2.token, op: 'look', files: 1, folders: 1, bytes: 3, existing: clashes });
    const heads = [...document.querySelectorAll('#modal-body thead th')].map((th) => th.textContent);
    check('ops: the question: Moving and Already there, on Skip, no Always, OK Move', [$('#modal').hidden, $('#modal-title').textContent, heads.slice(0, 3),
      $('#modal-body .up-all input:checked').value, !!$('#up-always'), $('#modal-ok').textContent, $('#modal-body .up-existing').textContent],
    [false, 'Move 2 items to Main:/dst', ['Name', 'Moving', 'Already there'], 'skip', false, 'Move', '2 already there: 1 newer, 1 of another size']);
    check('ops: each choice counts what it replaces: never the folder onto a file of its name',
      [...document.querySelectorAll('#modal-body .up-all label')].map((l) => l.textContent.trim()).slice(0, 4),
      ['Replace 1 replaced', 'Replace if different 1 replaced', 'Replace if newer 1 replaced', 'Skip nothing replaced']);
    const forAll = (v) => {
      const r = $(`#modal-body .up-all input[value="${v}"]`);
      r.checked = true;
      r.dispatchEvent(new Event('change', { bubbles: true }));
      return $('#modal-body .up-outcome').textContent;
    };
    check('ops: ... and the outcome under the table', [forAll('replace'), forAll('different'), forAll('skip')],
      ['1 replaced, 1 skipped', '1 replaced, 1 skipped', '2 skipped: nothing on Main is replaced']);
    const rows = [...document.querySelectorAll('#modal-body .up-clashes tbody tr')];
    check('ops: a folder moved onto a file: "a folder", never replaced (Skip or Keep both only)',
      [rows[0].children[0].textContent, rows[0].children[1].textContent, [...rows[0].querySelectorAll('option')].map((o) => o.value)],
      ['tv/ another size', 'a folder', ['', 'skip', 'both']]);
    const own = rows[1].querySelector('select');
    own.value = 'replace';
    own.dispatchEvent(new Event('change', { bubbles: true }));
    check('ops: a file\'s own choice: what it does', [rows[1].querySelector('.up-effect').textContent, $('#modal-body .up-outcome').textContent],
      ['replaced', '1 replaced, 1 skipped']);
    sent.length = 0;
    $('#modal-ok').click();
    const move = lastOf('filesPaste');
    check('ops: Move sends Skip for all, the file\'s own choice by its paths', [move.op, move.choice, move.own, $('#modal').hidden, line().text],
      ['move', 'skip', [{ from: '/srv/a.txt', to: '/dst/a.txt', choice: 'replace' }], true, 'Moving 2 items to Main:/dst…']);
    reply({ token: move.token, op: 'move', ok: true, stopped: false, files: 1, folders: 0, bytes: 0, left: 0, placed: ['', '/dst/a.txt'],
      entries: [{ state: 'skipped', from: '/srv/tv', to: '/dst/tv', why: 'a file of that name is there, and a folder doesn\'t replace it' }],
      skipped: 1, failed: 0, moved: ['/srv/a.txt'] });
    check('ops: moved: the result with what was skipped and why; the clipboard emptied',
      [line().text, ops.clip, $('#ops').title.includes('/srv/tv: skipped, a file of that name is there')],
      ['Moved 1 file to Main:/dst; 1 item skipped: tv: a file of that name is there, and a folder doesn\'t replace it', null, true]);
    // The backend refusing (a path a download uses; another change running) is said in red.
    reset();
    pick(1);
    key('x', { ctrlKey: true });
    sent.length = 0;
    pasteInto('Main', '/dst', 'dst');
    reply({ token: lastOf('filesLook').token, op: 'look', error: 'Main:/srv/a.txt is being downloaded: wait until it ends, or cancel it first' });
    check('ops: a refusal of the look in red, nothing pasted', [line(), sentOf('filesPaste').length],
      [{ shown: true, text: 'Main:/srv/a.txt is being downloaded: wait until it ends, or cancel it first', cancel: false, bad: true }, 0]);
    // The look cancelled from the footer: said, nothing pasted.
    pasteInto('Main', '/dst', 'dst');
    const look3 = lastOf('filesLook');
    $('#ops-cancel').click();
    check('ops: Cancel during the look stops it', lastOf('filesCancel'), { type: 'filesCancel', token: look3.token });
    reply({ token: look3.token, op: 'look', stopped: true });
    check('ops: ... said, nothing pasted', [line(), sentOf('filesPaste').length],
      [{ shown: true, text: 'Paste cancelled: nothing changed', cancel: false, bad: false }, 0]);
    // A full look answer crossing the Cancel: dropped, nothing pasted, no question.
    pasteInto('Main', '/dst', 'dst');
    const look4 = lastOf('filesLook');
    sent.length = 0;
    $('#ops-cancel').click();
    check('ops: Cancel of a look says it at once', [line(), sent], [{ shown: true, text: 'Paste cancelled: nothing changed', cancel: false, bad: false },
      [{ type: 'filesCancel', token: look4.token }]]);
    reply({ token: look4.token, op: 'look', files: 1, folders: 0, bytes: 3, existing: { count: 0 } });
    check('ops: ... a full answer after it starts nothing', [sentOf('filesPaste').length, !!modal, ops.running, line().text],
      [0, false, null, 'Paste cancelled: nothing changed']);
    // The question cancelled (its Cancel or Esc): the footer says so, nothing pasted.
    pasteInto('Main', '/dst', 'dst');
    reply({ token: lastOf('filesLook').token, op: 'look', files: 1, folders: 0, bytes: 3, existing: { count: 1, same: 1, newer: 0, older: 0, differs: 0,
      folders: 0, files: [{ name: 'a.txt', from: '/srv/a.txt', to: '/dst/a.txt', size: 3, modified: 1700000000, mineSize: 3, mineModified: 1700000000, kind: 'same' }] } });
    check('ops: names taken: the question, the look\'s line gone', [!!modal, line().shown], [true, false]);
    $('#modal-cancel').click();
    check('ops: the question cancelled: said, nothing pasted', [line(), sentOf('filesPaste').length, !!modal],
      [{ shown: true, text: 'Paste cancelled: nothing changed', cancel: false, bad: false }, 0, false]);
    quiet();
    reset();

    // Rename: F2; the name without its extension picked; bad names said as typed; Enter; a name
    // taken keeps the box open; done: closed, said, the new name picked once Browse shows it again.
    pick(1);
    check('ops: F2 opens Rename', key('F2'), true);
    const input = $('#fo-name');
    check('ops: Rename: its name, without the extension picked, OK Rename', [$('#modal-title').textContent, input.value, input.selectionStart,
      input.selectionEnd, document.activeElement === input, $('#modal-ok').textContent, $('#modal-cancel').hidden], ['Rename a.txt', 'a.txt', 0, 1, true, 'Rename', false]);
    const typeName = (v) => {
      const box = $('#fo-name');
      box.value = v;
      box.dispatchEvent(new Event('input', { bubbles: true }));
      return [$('#fo-why').textContent, $('#modal-ok').disabled];
    };
    check('ops: bad names said as typed, OK off', [typeName(''), typeName('x/y'), typeName('．'), typeName('a／b'), typeName('a‛／b'), typeName('b.txt')],
      [['A name can’t be empty.', true], ['A name can’t hold a /.', true], ['“.” can’t be a name.', true], ['A name can’t hold a /.', true], ['', false], ['', false]]);
    sent.length = 0;
    key('Enter', {}, input);
    const ren = lastOf('filesRename');
    check('ops: Enter renames', [ren.server, ren.path, ren.name, $('#modal-ok').disabled, input.disabled], ['Main', '/srv/a.txt', 'b.txt', true, true]);
    reply({ token: ren.token, op: 'rename', taken: 'b.txt is already taken in /srv' });
    check('ops: a name taken keeps the box open, saying so', [$('#modal').hidden, $('#fo-why').textContent, input.disabled, document.activeElement === input],
      [false, 'b.txt is already taken in /srv', false, true]);
    typeName('c.txt');
    $('#modal-ok').click();
    const ren2 = lastOf('filesRename');
    reply({ token: ren2.token, op: 'rename', ok: true, path: '/srv/c.txt' });
    check('ops: renamed: closed, said, the new name picked next', [$('#modal').hidden, line().text, b.pendingSelect], [true, 'Renamed a.txt to c.txt', '/srv/c.txt']);
    b.pendingSelect = null;
    // A refusal keeps it open too; Esc cancels with nothing sent.
    pick(0);
    key('F2');
    check('ops: a folder\'s whole name picked', [$('#fo-name').selectionStart, $('#fo-name').selectionEnd], [0, 2]);
    $('#fo-name').value = 'films';
    $('#modal-ok').click();
    reply({ token: lastOf('filesRename').token, op: 'rename', error: 'Main doesn’t let this account write in /srv' });
    check('ops: a refusal: said in the box, still open', [$('#modal').hidden, $('#fo-why').textContent], [false, 'Main doesn’t let this account write in /srv']);
    sent.length = 0;
    key('Escape', {}, document);
    check('ops: Esc cancels', [$('#modal').hidden, sent.length], [true, 0]);

    // New folder: Ctrl+Shift+N; a name no row has; OK Create.
    b.items = [...b.items, { name: 'New folder', path: '/srv/New folder', dir: true, remote: 'Main' }];
    sent.length = 0;
    key('N', { ctrlKey: true, shiftKey: true });
    check('ops: New folder: a free name, OK Create', [$('#modal-title').textContent, $('#fo-name').value, $('#modal-ok').textContent],
      ['New folder in srv', 'New folder (2)', 'Create']);
    $('#modal-ok').click();
    const mk = lastOf('filesMkdir');
    check('ops: ... it makes it in the open folder', [mk.server, mk.dir, mk.name], ['Main', '/srv', 'New folder (2)']);
    reply({ token: mk.token, op: 'mkdir', ok: true, path: '/srv/New folder (2)' });
    check('ops: made: said, picked next', [$('#modal').hidden, line().text, b.pendingSelect], [true, 'Made the folder New folder (2) in Main:/srv', '/srv/New folder (2)']);
    quiet();
    reset();

    // Delete: files: the confirmation at once; a red Delete, the focus on Cancel; its progress; the result.
    pick(1, 2);
    check('ops: Del asks first', key('Delete'), true);
    check('ops: the confirmation: what goes, a red Delete, Cancel focused', [$('#modal-title').textContent, $('#modal-ok').textContent,
      $('#modal-ok').classList.contains('danger'), $('#modal-ok').disabled, document.activeElement === $('#modal-cancel'),
      $('#modal-body .fo-counts').textContent, [...document.querySelectorAll('#modal-body .up-name')].map((e) => e.textContent),
      $('#modal-body').textContent.includes('From Main, for good')],
    ['Delete 2 items?', 'Delete', true, false, true, '2 files · 3.2 GiB', ['a.txt', 'b.mkv'], true]);
    sent.length = 0;
    $('#modal-ok').click();
    const del = lastOf('filesDelete');
    check('ops: Delete sends the paths', [del.server, del.paths], ['Main', ['/srv/a.txt', '/srv/b.mkv']]);
    onFilesProgress({ type: 'filesProgress', token: del.token, bytes: 0, totalBytes: 0, files: 1204, totalFiles: 2000 });
    check('ops: deleting: how far, with Cancel', line(), { shown: true, text: 'Deleting 1,204 of 2,000 files', cancel: true, bad: false });
    reply({ token: del.token, op: 'delete', ok: true, stopped: false, files: 2000, folders: 25, bytes: 9, left: 0, placed: [], entries: [], skipped: 0, failed: 0 });
    check('ops: deleted: the result', line(), { shown: true, text: 'Deleted 2,000 files and 25 folders', cancel: false, bad: false });
    // A folder: counted first (Delete off meanwhile, Cancel stops the count), refusals said.
    pick(0);
    sent.length = 0;
    key('Delete');
    const count = lastOf('filesSize');
    check('ops: a folder: counted first, the busy paths refused, Delete off', [count.paths, count.check, $('#modal-ok').disabled,
      $('#modal-body .fo-counts').textContent.startsWith('Counting what is in the folder')], [['/srv/tv'], true, true, true]);
    onFilesProgress({ type: 'filesProgress', token: count.token, bytes: 5, totalBytes: 0, files: 1204, totalFiles: 0 });
    check('ops: ... how far the count is', $('#modal-body .fo-counts').textContent, 'Counting what is in the folder… 1,204 files so far');
    reply({ token: count.token, op: 'size', files: 1204, folders: 25, links: 3, bytes: 3435973837, unreadable: [], unreadableCount: 0 });
    check('ops: counted: files, links, folders and the size; Delete on', [$('#modal-body .fo-counts').textContent, $('#modal-ok').disabled],
      ['1,204 files, 3 links and 25 folders · 3.2 GiB', false]);
    closeModal(false);
    sent.length = 0;
    key('Delete');
    const count2 = lastOf('filesSize');
    $('#modal-cancel').click();
    check('ops: Cancel while counting stops the count', sentOf('filesCancel'), [{ type: 'filesCancel', token: count2.token }]);
    key('Delete');
    reply({ token: lastOf('filesSize').token, op: 'size', error: 'Main:/srv/tv holds /srv/tv/x.mkv, which is being downloaded: wait until it ends, or cancel it first' });
    check('ops: a busy path: said, Delete off', [$('#modal-body .up-bad').textContent, $('#modal-ok').disabled],
      ['Main:/srv/tv holds /srv/tv/x.mkv, which is being downloaded: wait until it ends, or cancel it first', true]);
    closeModal(false);
    key('Delete');
    reply({ token: lastOf('filesSize').token, op: 'size', files: 3, folders: 2, links: 0, bytes: 9, unreadable: ['/srv/tv/x: permission denied'], unreadableCount: 1 });
    check('ops: a folder below that can\'t be read: nothing would go, Delete off', [$('#modal-body .up-warn').textContent, $('#modal-ok').disabled],
      ['1 folder below can’t be read, so nothing would be deleted: /srv/tv/x: permission denied', true]);
    closeModal(false);
    // A delete that failed partway: the engine's words in red.
    pick(1);
    key('Delete');
    $('#modal-ok').click();
    reply({ token: lastOf('filesDelete').token, op: 'delete', ok: false, stopped: false, files: 0, folders: 0, bytes: 0, left: 1, placed: [],
      error: 'Main doesn’t let this account write in /srv; nothing was deleted', entries: [], skipped: 0, failed: 0 });
    check('ops: a refused delete in red', line(), { shown: true, text: 'Main doesn’t let this account write in /srv; nothing was deleted', cancel: false, bad: true });
    quiet();

    // Search results: each hit's own server; Paste on a folder hit asks to open it; renames show
    // the new name, deletes drop the rows at once.
    reset();
    b.under = { query: 'a', path: '/srv', all: false, whole: false, name: 'srv' };
    state.results = [{ name: 'tv', path: '/srv/tv', dir: true, remote: 'Main', ord: 0 }, { name: 'a.txt', path: '/srv/tv/a.txt', remote: 'Main', size: 3, ord: 1 },
      { name: 'b.txt', path: '/srv/b.txt', remote: 'Main', size: 3, ord: 2 }];
    state.hits = state.results.slice();
    pick(0);
    check('ops: a folder hit: Paste asks to open it first', [entry(resultMenu(), 'Paste').note, entry(resultMenu(), 'Paste').why],
      ['open it first', 'Paste goes into a folder you have open, not into search results: open this folder first.']);
    pick(2);
    check('ops: a file hit: no Paste into search results', labels(resultMenu()).filter((l) => l.startsWith('Paste')), []);
    key('F2');
    $('#fo-name').value = 'c.txt';
    $('#modal-ok').click();
    reply({ token: lastOf('filesRename').token, op: 'rename', ok: true, path: '/srv/c.txt' });
    check('ops: a hit renamed shows its new name', state.results.map((h) => h.path), ['/srv/tv', '/srv/tv/a.txt', '/srv/c.txt']);
    pick(0);
    key('Delete');
    reply({ token: lastOf('filesSize').token, op: 'size', files: 1, folders: 1, links: 0, bytes: 3, unreadable: [], unreadableCount: 0 });
    $('#modal-ok').click();
    reply({ token: lastOf('filesDelete').token, op: 'delete', ok: true, stopped: false, files: 1, folders: 1, bytes: 3, left: 0, placed: [], entries: [], skipped: 0, failed: 0 });
    check('ops: a folder deleted drops it and its hits at once', state.hits.map((h) => h.path), ['/srv/c.txt']);
    check('ops: Esc in results closes them first, as before', [key('Escape'), b.under], [true, null]);
    quiet();
    reset();

    // Properties: one row; the dialog's only button Close; the server's answer; MD5 and SHA-256 on
    // buttons with Cancel; a folder's size; a link's target; closing stops what runs.
    pick(1);
    sent.length = 0;
    check('ops: Alt+Enter opens Properties', key('Enter', { altKey: true }), true);
    const props = lastOf('filesProps');
    check('ops: Properties: asked, one button Close, the focus on it', [props.path, $('#modal-title').textContent, $('#modal-cancel').hidden,
      $('#modal-ok').textContent, document.activeElement === $('#modal-ok')], ['/srv/a.txt', 'Properties of a.txt', true, 'Close', true]);
    reply({ token: props.token, op: 'props', kind: 'file', size: 3, modified: 1700000000, accessed: 1700000100, mode: 0o640, owner: 'giorgi',
      group: 'staff', free: 1073741824, readOnly: false });
    const cells = () => Object.fromEntries([...document.querySelectorAll('#modal-body .fo-props tr')].map((tr) => [tr.children[0].textContent, tr.children[1].textContent]));
    const c0 = cells();
    check('ops: Properties of a file', [c0.Where, c0.Type, c0.Size, c0.Modified, c0.Accessed, c0.Permissions, c0.Owner, c0.Group, c0['Free on Main'], c0.Checksums],
      ['Main:/srv/a.txt', 'File', '3 B (3 bytes)', fmtDate(1700000000, true), fmtDate(1700000100, true), 'rw-r----- 640', 'giorgi', 'staff', '1.0 GiB', 'MD5SHA-256']);
    sent.length = 0;
    $('#modal-body [data-act="fo-md5"]').click();
    const md5 = lastOf('filesHash');
    check('ops: MD5: asked of the server with the size, computing with Cancel, SHA-256 off meanwhile',
      [md5.sum, md5.size, md5.path, cells().Checksums.startsWith('MD5: computing on Main… Cancel'), $('#modal-body [data-act="fo-sha256"]').disabled],
      ['md5', 3, '/srv/a.txt', true, true]);
    reply({ token: md5.token, op: 'hash', sum: 'md5', hash: 'd41d8cd98f00b204e9800998ecf8427e' });
    check('ops: the MD5 shown, selectable', [$('#modal-body .fo-hash').textContent, getComputedStyle($('#modal-body .fo-hash')).userSelect],
      ['d41d8cd98f00b204e9800998ecf8427e', 'text']);
    $('#modal-body [data-act="fo-sha256"]').click();
    const sha = lastOf('filesHash');
    sent.length = 0;
    $('#modal-body [data-act="fo-cancel"]').click();
    check('ops: Cancel stops the sum', [sent, !!$('#modal-body [data-act="fo-sha256"]')], [[{ type: 'filesCancel', token: sha.token }], true]);
    $('#modal-body [data-act="fo-sha256"]').click();
    const sha2 = lastOf('filesHash');
    sent.length = 0;
    closeModal(false);
    check('ops: closing stops what runs', sent, [{ type: 'filesCancel', token: sha2.token }]);
    // A folder: Folder size.
    pick(0);
    key('Enter', { altKey: true });
    reply({ token: lastOf('filesProps').token, op: 'props', kind: 'folder', modified: 1700000000, mode: 0o755, owner: '1000', group: '1000' });
    check('ops: a folder: Folder size on a button, no sums', [cells().Type, cells().Size, cells().Permissions, 'Checksums' in cells()], ['Folder', 'Folder size', 'rwxr-xr-x 755', false]);
    $('#modal-body [data-act="fo-size"]').click();
    const fsize = lastOf('filesSize');
    onFilesProgress({ type: 'filesProgress', token: fsize.token, bytes: 2048, totalBytes: 0, files: 12, totalFiles: 0 });
    check('ops: counting, with Cancel', [fsize.paths, fsize.check, cells().Size], [['/srv/tv'], undefined, 'Counting… 12 files, 2.0 KiB so far Cancel']);
    reply({ token: fsize.token, op: 'size', files: 1204, folders: 25, links: 3, bytes: 3435973837, unreadable: [], unreadableCount: 0 });
    check('ops: the folder size', cells().Size, '3.2 GiB (3,435,973,837 bytes) · 1,204 files, 3 links, 25 folders');
    closeModal(false);
    // A link, and the open folder's own.
    pick(1);
    key('Enter', { altKey: true });
    reply({ token: lastOf('filesProps').token, op: 'props', kind: 'link', link: '../x.txt', target: { kind: 'file', size: 5, modified: 1 }, mode: 0o777 });
    check('ops: a link: what it leads to, its target\'s size, sums of it', [cells().Type, cells()['Points to'], cells().Size, 'Checksums' in cells()],
      ['Link to a file', '../x.txt', '5 B (5 bytes)', true]);
    closeModal(false);
    key('Enter', { altKey: true });
    reply({ token: lastOf('filesProps').token, op: 'props', kind: 'link', link: 'nowhere', mode: 0o777 });
    check('ops: a link leading nowhere says so', cells().Type, 'Link to nothing (its target is gone)');
    closeModal(false);
    sent.length = 0;
    entry(folderMenu(), 'Properties').act();
    check('ops: the open folder\'s Properties', [lastOf('filesProps').path, $('#modal-title').textContent], ['/srv', 'Properties of srv']);
    closeModal(false);
    pick(1, 2);
    ops.result = null;
    key('Enter', { altKey: true });
    check('ops: Alt+Enter on several says one at a time', [said(), !!modal], ['Properties shows one item at a time: pick one.', false]);

    // The keys only in the list: in a text field, the Downloads table and the recent searches they
    // keep their own meaning.
    quiet();
    reset();
    pick(1);
    for (const [k, m] of [['Delete', {}], ['F2', {}], ['x', { ctrlKey: true }], ['v', { ctrlKey: true }], ['N', { ctrlKey: true, shiftKey: true }]]) {
      $('#query').dispatchEvent(new KeyboardEvent('keydown', { key: k, bubbles: true, cancelable: true, ...m }));
      $('#queue').dispatchEvent(new KeyboardEvent('keydown', { key: k, bubbles: true, cancelable: true, ...m }));
    }
    check('ops: no change from keys in the search box or the Downloads table', [sent.filter((m) => m.type.startsWith('files')), !!modal && $('#modal-title').textContent.startsWith('Delete 1')],
      [[], false]);
    if (modal) closeModal(false);

    // The words: permissions, sizes, results.
    check('ops: rwx with set-id and sticky bits', [rwx(0o755), rwx(0o4755), rwx(0o2750), rwx(0o1777), rwx(0o4644)],
      ['rwxr-xr-x', 'rwsr-xr-x', 'rwxr-s---', 'rwxrwxrwt', 'rwSr--r--']);
    check('ops: sizes in their total\'s unit', [fmtSizeOf(1288490189, 3650722202), fmtSizeOf(5, 900), fmtSizeOf(0, 2048)], ['1.2 of 3.4 GiB', '5 of 900 B', '0.0 of 2.0 KiB']);
    const rep = (op, msg, job) => reportText(op, { files: 0, folders: 0, bytes: 0, left: 0, entries: [], skipped: 0, failed: 0, ...msg }, job).text;
    const job1 = { server: 'Main', dir: '/srv', paths: ['/a'], clip: { items: [{ name: 'holiday' }] } };
    check('ops: results in words', [rep('move', { files: 0, folders: 1 }, job1), rep('copy', { files: 1204, folders: 25, bytes: 3435973837 }, job1),
      rep('delete', { stopped: true, files: 12, left: 30 }), rep('copy', { error: 'the connection to Main was lost: reset', files: 3 }, job1),
      rep('copy', { files: 2, failed: 1, entries: [{ state: 'failed', from: '/a/x', to: '/b/x', why: 'refused' }] }, job1)],
    ['Moved holiday to Main:/srv', 'Copied 1,204 files and 25 folders (3.2 GiB) to Main:/srv', 'Delete cancelled: 12 files deleted, 30 still there',
      'the connection to Main was lost: reset (3 files copied first)', 'Copied 2 files to Main:/srv; 1 item failed: x: refused']);

    // Light and dark: every colour of these is a theme token's (app.css redefines them for dark), so
    // both themes hold: each part's colour against a probe of the token it should use.
    const token = (name) => {
      const probe = document.createElement('span');
      probe.style.color = `var(--${name})`;
      document.body.append(probe);
      const c = getComputedStyle(probe).color;
      probe.remove();
      return c;
    };
    const colorOf = (el) => getComputedStyle(el).color;
    reset();
    pick(1);
    key('c', { ctrlKey: true });
    opsSay('x', true);
    const colours = [colorOf($('#ops-text')) === token('danger'), colorOf($('#clip')) === token('muted'), colorOf($('#ops-cancel')) === token('accent')];
    ops.result = null;
    setRunning({ token: 1, op: 'copy', what: 'x', text: 'x' });
    colours.push(colorOf($('#ops-text')) === token('text'));
    ops.running = null;
    clearClip();
    showMenu(10, 10, folderMenu());
    colours.push(colorOf($('#menu .note')) === token('muted'));
    closeMenu();
    key('F2');
    typeName('');
    colours.push(colorOf($('#fo-why')) === token('danger'));
    closeModal(false);
    key('Enter', { altKey: true });
    colours.push(colorOf($('#modal-body .fo-props th')) === token('muted'));
    closeModal(false);
    check('ops: light and dark: the new parts take the theme\'s colours', colours, Array(colours.length).fill(true));

    quiet();
    ops.accounts.clear();
    Object.assign(b, browseWas);
    state.hits = hitsWas;
    state.remotes = remotesWas;
    state.remote = remoteWas;
    state.results = [];
    state.selected = new Set();
    state.cursor = -1;
    renderRows();
  }

  panel.style.width = '1250px';
  state.queue = [
    folder({ id: 11, name: 'season', phase: '', files: { total: 304, queued: 290, running: 4, paused: 0, done: 8, failed: 1, skipped: 1 }, bytes: 31457280, total: 104857600 }),
    file(12, { status: 'queued', position: 1, total: 20971520 }),
    file(13, { status: 'paused', bytes: 5242880, total: 20971520, waiting: '1 file already here differs from the server’s. Resume to choose: replace, keep both, or skip.', ask: { count: 1, files: [] } }),
    file(14, { status: 'failed', bytes: 0, total: 20971520, error: 'the server keeps refusing connections' }),
    file(15, { status: 'done', bytes: 20971520, total: 20971520, check: 'ok', note: 'checksum verified (MD5)' }),
    folder({ id: 16, name: 'other', status: 'paused', speed: 0, eta: null }),
  ];
  renderQueue();
  feed(11, [
    { path: 'S01/e01.mkv', size: 31457280, status: 'done', bytes: 31457280, verified: true },
    { path: 'S01/e02.mkv', size: 31457280, status: 'running', bytes: 10485760 },
    { path: 'S01/e03.mkv', size: 31457280, status: 'failed', bytes: 0, error: 'permission denied' },
    { path: 'S01/e04.mkv', size: 31457280, status: 'queued', bytes: 0, local: 'S01\\e04 (2).mkv', named: true },
    { path: 'S01/notes.txt', size: 2048, status: 'skipped', bytes: 0 },
  ]);
  feed(16);
  delta(11, [[1, FS.running, 10485760, 2097152, 10, 2]]);
  window.shotReady = true;
} catch (e) {
  failed++;
  out.push(`EXCEPTION ${e && e.stack}`);
}
const pre = document.createElement('pre');
pre.id = 'test-out';
pre.textContent = `${failed ? 'FAILED' : 'ALL PASS'} (${failed})\n${out.join('\n')}`;
document.body.append(pre);
})();
