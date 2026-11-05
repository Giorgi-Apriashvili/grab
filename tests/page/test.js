'use strict';
// Page-logic checks for the Downloads table (chunks B1, B2, C, D and G: the table, sorting, column
// widths, keys and the treegrid ARIA, every file of a folder by subscription, the filter, Go to
// running, the performance numbers, a file's priority in its folder, the status bar's total and its
// graph, Waiting files), run in headless Edge by run_page_test.ps1.
const out = [];
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
  state.limitOn = false;
  state.queue = [folder({ id: 95, speed: 2 * 1048576, files: counts({ total: 4 }) }), file(96, { status: 'running', speed: 1048576 })];
  renderQueue();
  onSpeed({ type: 'speed', total: 5 * 1048576 });
  check('G: the total is the backend\'s, not the rows\' sum', $('#total-speed').textContent, '↓ 5.0 MiB/s total · 2 running');
  onSpeed({ type: 'speed', total: 512.4 });
  check('G: the total in the fixed format', $('#total-speed').textContent, '↓ 512 B/s total · 2 running');
  // The graph: the last 60 samples, scaled to their peak, before the text in a fixed box.
  speedHist.length = 0;
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
  const barBefore = [boxOf('#speed-graph'), boxOf('#limit-value'), boxOf('#limit-on')];
  onSpeed({ type: 'speed', total: 999.9 * 1073741824 });
  state.queue = Array.from({ length: 12 }, (_, k) => file(200 + k, { status: 'running' }));
  renderQueue();
  state.limitOn = true;
  renderTotalSpeed();
  const barAfter = [boxOf('#speed-graph'), boxOf('#limit-value'), boxOf('#limit-on')];
  check('G: a longer total moves nothing in the status bar', [$('#total-speed').textContent, JSON.stringify(barAfter) === JSON.stringify(barBefore)],
    ['↓ 999.9 GiB/s total · 12 running (limited)', true]);
  check('G: and the graph kept its box from when it was blank', JSON.stringify(idleBox) === JSON.stringify(barBefore[0]), true);
  state.limitOn = false;
  state.queue = [];
  renderQueue();
  check('G: none running', $('#total-speed').textContent, 'No downloads running');
  onSpeed({ type: 'speed', total: 3 * 1048576 }); // for the screenshots

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
