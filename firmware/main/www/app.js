'use strict';
/* LoRaWAN gateway console. Same API as before; the UI is built from small
 * DOM helpers (no framework, no inline styles: CSP default-src 'self'). */

const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
const H = { 'X-Requested-With': 'gw' };
const FORM = { 'Content-Type': 'application/x-www-form-urlencoded' };
const SVGNS = 'http://www.w3.org/2000/svg';

function el(tag, attrs, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v === false || v == null) continue;
    if (k === 'class') e.className = v; else if (k === 'text') e.textContent = v; else e.setAttribute(k, v === true ? '' : v);
  }
  for (const k of kids.flat()) if (k != null && k !== false) e.append(k);
  return e;
}
function sv(tag, attrs, ...kids) {
  const e = document.createElementNS(SVGNS, tag);
  for (const [k, v] of Object.entries(attrs || {})) if (v != null) e.setAttribute(k, v);
  for (const k of kids.flat()) if (k != null) e.append(typeof k === 'string' ? document.createTextNode(k) : k);
  return e;
}
const icon = (d, vb = '0 0 24 24') => sv('svg', { viewBox: vb, 'aria-hidden': 'true' }, ...d.map((p) => sv('path', { d: p })));
const ICONS = {
  cloud: ['M17.5 19H8a5 5 0 1 1 .9-9.9A6 6 0 0 1 20 11.5a3.8 3.8 0 0 1-2.5 7.5z'],
  antenna: ['M12 22V10', 'M8.5 6.5a5 5 0 0 1 7 0', 'M5.6 3.6a9 9 0 0 1 12.8 0', 'M12 10l-4 12M12 10l4 12'],
  chip: ['M7 7h10v10H7z', 'M9 2v3M15 2v3M9 19v3M15 19v3M2 9h3M2 15h3M19 9h3M19 15h3'],
  clock: ['M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18z', 'M12 7v5l3 2'],
  alert: ['M12 3 2 20h20L12 3z', 'M12 10v4M12 17.5v.5'],
  info: ['M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18z', 'M12 11v5M12 8v.5'],
};

async function api(path, opts = {}) {
  const r = await fetch(path, { credentials: 'same-origin', ...opts, headers: { ...H, ...(opts.headers || {}) } });
  if (r.status === 401) { show(false); throw new Error('Log in again: the session ended.'); }
  if (!r.ok) throw new Error((await r.text()).trim() || r.statusText);
  return r;
}
const post = async (path, body) => (await (await api(path, { method: 'POST', headers: FORM, body })).text()).trim();

let toastTimer;
function toast(text, bad) {
  const t = $('#toast');
  t.textContent = text;
  t.classList.toggle('bad', !!bad);
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.hidden = true; }, bad ? 9000 : 5000);
}

/* ---- theme: light, dark or the system's; remembered in this browser -------- */
function store(k, v) { try { if (v === undefined) return localStorage.getItem(k); localStorage.setItem(k, v); } catch (e) { /* private mode */ } return null; }
function applyTheme(t) {
  if (t === 'light' || t === 'dark') document.documentElement.dataset.theme = t; else delete document.documentElement.dataset.theme;
  for (const b of $$('[data-theme-set]')) b.setAttribute('aria-checked', String(b.dataset.themeSet === (t || 'auto')));
}
applyTheme(store('theme'));

/* ---- session --------------------------------------------------------------- */
function show(loggedIn, role) {
  $('#login').hidden = loggedIn;
  $('#app').hidden = !loggedIn;
  $('#logout').hidden = !loggedIn;
  $('#link-pill').hidden = !loggedIn;
  $('#who').textContent = loggedIn ? (role === 'admin' ? 'Admin' : 'Viewer') : '';
  document.body.classList.toggle('viewer', role === 'viewer');
  if (loggedIn) { sessionStorage.setItem('role', role); openTab(location.hash.slice(1) || 'overview'); } else { sessionStorage.removeItem('role'); }
}

function kv(line) {
  const out = {};
  const re = /(\w+)=("[^"]*"|\S+)/g;
  let m;
  while ((m = re.exec(line)) !== null) out[m[1]] = m[2].replace(/^"|"$/g, '');
  return out;
}

/* "ttn" is the stored name of the forward-everything mode (kept for saved settings) */
const MODES = { ttn: 'Network server', hybrid: 'Hybrid', standalone: 'Standalone' };
const isTTN = (host) => /\.cloud\.thethings\.network$/.test(host || '');
/* short name of the server: "TTN eu1" or the host itself */
const srvName = (host) => (isTTN(host) ? 'TTN ' + host.split('.')[0] : host || 'the network server');
const fmtUptime = (s) => { s = +s; const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
  return (d ? d + ' d ' : '') + (d || h ? h + ' h ' : '') + m + ' min'; };
const fmtKB = (b) => (+b >= 1048576 ? (b / 1048576).toFixed(1) + ' MB' : Math.round(b / 1024) + ' KB');
const MHZ = (hz) => (hz / 1e6).toFixed(3) + ' MHz';
const ago = (s) => (s < 0 ? 'Not seen since the gateway started' : s < 90 ? s + ' s ago' : s < 5400 ? Math.round(s / 60) + ' min ago' : Math.round(s / 3600) + ' h ago');

let S = {}; /* last /api/status, parsed */

/* ---- overview ---------------------------------------------------------------- */
function gauge(ic, cls, title, detail) {
  return el('div', { class: 'gauge ' + cls }, icon(ICONS[ic]), el('b', { text: title }), el('span', { text: detail }));
}
/* One labelled value; `extra` is a meter fraction (number) or a chip ([class, text]). */
function stat(label, value, extra, cls) {
  let tail = null;
  if (typeof extra === 'number') {
    tail = el('div', { class: 'meter' }, el('span', { class: cls || '' }));
    tail.firstChild.style.width = Math.max(0, Math.min(100, extra * 100)) + '%';
  }
  const v = Array.isArray(extra) ? el('span', { class: 'chip dot ' + extra[0], text: extra[1] }) : el('b', { text: value });
  return el('div', { class: 'stat' }, el('span', { text: label }), v, tail);
}
function notice(cls, ic, text, action) {
  return el('li', { class: cls }, icon(ICONS[ic]), el('span', { text }), action || null);
}
const gotoBtn = (tab, text) => { const b = el('button', { type: 'button', class: 'ghost small', text }); b.addEventListener('click', () => openTab(tab)); return b; };
const n = (v) => +v || 0;

function renderOverview() {
  const o = S.ota || {}, r = S.radio || {}, f = S.faults || {}, g = S.ttn || {}, ln = S.lns || {}, lb = S.lbs || null;
  const mode = g.mode || 'ttn';
  const upNeeded = mode !== 'standalone';
  const up = r.upstream === 'up';
  const rx = n(r.rx), ok = n(r.crc_ok), restarts = n(r.restarts), run = n(f.run);

  const notes = [];
  if (upNeeded && g.lns_key === 'none') notes.push(notice('warn', 'alert', 'No API key: the gateway cannot connect to ' + srvName(g.server) + '.', gotoBtn('setup', 'Add the key')));
  else if (upNeeded && !up) notes.push(notice('warn', 'alert', 'Not connected to ' + srvName(g.server) + ': frames are not forwarded.', gotoBtn('setup', 'Check setup')));
  const mq = S.mqtt || {};
  if (mq.state === 'refused') notes.push(notice('warn', 'alert', 'The MQTT broker refused the user or password: nothing is published.', gotoBtn('setup', 'Check setup')));
  else if (mq.state === 'tls_error') notes.push(notice('warn', 'alert', 'TLS to the MQTT broker failed (certificate or host name): nothing is published.', gotoBtn('setup', 'Check setup')));
  else if (mq.state === 'connecting' || mq.state === 'no_host') notes.push(notice('warn', 'alert', 'Not connected to the MQTT broker: uplinks are not published.', gotoBtn('setup', 'Check setup')));
  if (lb && lb.plan === 'differs') notes.push(notice('warn', 'alert', 'The network server sent a channel plan this radio cannot use: the radio keeps its own.', gotoBtn('radio', 'Open Radio')));
  if (o.state && o.state !== 'valid') notes.push(notice('warn', 'alert', 'New firmware on trial: it rolls back unless its health check passes.'));
  if (run > 0) notes.push(notice('warn', 'alert', run + ' crash or watchdog reset(s) in a row.', gotoBtn('system', 'Open System')));
  if (restarts > 0) notes.push(notice('warn', 'alert', 'The radio restarted ' + restarts + ' time(s) since the gateway started.'));
  if ((o.last_check || '').startsWith('update available')) notes.push(notice('', 'info', 'A newer firmware release is available.', gotoBtn('system', 'Open System')));
  $('#notices').replaceChildren(...notes);

  $('#health').replaceChildren(
    !upNeeded ? gauge('cloud', 'off', 'Upstream off', 'Standalone mode: nothing leaves the gateway')
      : g.lns_key === 'none' ? gauge('cloud', 'warn', 'No API key', 'Add one in Setup')
      : gauge('cloud', up ? 'ok' : 'warn', up ? 'Connected' : 'Not connected', srvName(g.server) + ' · Basics Station'),
    gauge('antenna', n(r.starts) > 0 ? (restarts ? 'warn' : 'ok') : 'warn', n(r.starts) > 0 ? 'Receiving' : 'Radio not started', ok + ' valid frames'),
    mode === 'ttn' ? gauge('chip', 'off', 'Local server off', 'The network server serves every device')
      : gauge('chip', ln.tx === 'dry-run' ? 'warn' : 'ok', n(ln.joined) + ' of ' + n(ln.devices) + ' joined', ln.tx === 'dry-run' ? 'Local transmit is off' : n(ln.uplinks) + ' uplinks'),
    gauge('clock', 'ok', 'Up ' + fmtUptime(r.uptime_s || 0), o.eth && o.eth !== '-' ? 'On Ethernet' : o.wifi && o.wifi !== '-' ? 'On Wi-Fi' : 'No network'),
  );

  $('#ov-radio').replaceChildren(
    stat('Frames received', String(rx)),
    stat('With a valid CRC', ok + (rx ? ' (' + Math.round(ok * 100 / rx) + '%)' : ''), rx ? ok / rx : 0),
    stat('Dropped, queue full', String(n(r.dropped))),
    stat('Radio starts', String(n(r.starts))),
    stat('Radio restarts', String(restarts), restarts ? ['warn', restarts + ' since boot'] : null),
  );

  if (!upNeeded) $('#ov-link').replaceChildren(el('p', { class: 'empty-note', text: 'Not used in standalone mode.' }));
  else $('#ov-link').replaceChildren(
    stat('Server', '', up ? ['ok', srvName(g.server)] : ['warn', srvName(g.server) + ': ' + (lb ? lb.link : 'down')]),
    stat('Channel plan', '', !lb ? null : lb.plan === 'ok' ? ['ok', 'Matches this radio'] : ['warn', 'Not usable here']),
    stat('Sent upstream', String(lb ? n(lb.uplinks) : 0)),
    stat('Downlinks from the server', String(lb ? n(lb.downlinks) : 0)),
    stat('Confirmed sent', String(lb ? n(lb.dntxed) : 0), lb && n(lb.downlinks) ? n(lb.dntxed) / n(lb.downlinks) : null),
    stat('Refused (radio or format)', String(lb ? n(lb.refused) : 0)),
    stat('Dropped while offline', String(lb ? n(lb.dropped) : 0)),
  );

  if (mode === 'ttn') $('#ov-local').replaceChildren(el('p', { class: 'empty-note', text: 'Off: the network server serves every device. Choose hybrid or standalone in Setup to serve your own.' }));
  else $('#ov-local').replaceChildren(
    stat('Devices joined', n(ln.joined) + ' of ' + n(ln.devices), n(ln.devices) ? n(ln.joined) / n(ln.devices) : 0),
    stat('Joins', String(n(ln.joins))),
    stat('Uplinks', String(n(ln.uplinks))),
    stat('Downlinks', String(n(ln.downlinks))),
    ...(mode === 'hybrid' ? [stat('Passed to the network server', String(n(ln.forwarded))), stat('Not passed on, queue full', String(n(ln.fwd_dropped)))] : []),
    stat('Local transmit', '', ln.tx === 'on' ? ['ok', 'On'] : ['warn', 'Off (dry run)']),
  );
}

/* ---- system ------------------------------------------------------------------- */
/* esp_ota image states as ota.c reports them */
const IMAGE_STATE = { valid: ['ok', 'Confirmed'], 'pending-verify': ['warn', 'On trial'], new: ['warn', 'New, not booted yet'],
  invalid: ['bad', 'Invalid'], aborted: ['bad', 'Aborted'], undefined: ['', 'Not tracked (USB flash)'] };
/* ota.c's last_check text as a state: "update available (...)", "up to date", "error: ...", ... */
function updateChip(s) {
  s = s || '';
  if (s.startsWith('update available')) return ['warn', 'Newer release available'];
  if (s.startsWith('up to date')) return ['ok', 'This is the latest'];
  if (s.startsWith('error')) return ['bad', 'Check failed: ' + s.replace(/^error:\s*/, '')];
  if (s.startsWith('install failed') || s === 'failed before' || s === 'rolled back before') return ['bad', s.charAt(0).toUpperCase() + s.slice(1)];
  if (!s || s === 'never') return ['', 'Not checked yet'];
  return ['', s.charAt(0).toUpperCase() + s.slice(1)];
}
function renderSystem() {
  const o = S.ota || {}, r = S.radio || {}, f = S.faults || {}, g = S.ttn || {};
  $('#sys-fw').replaceChildren(
    stat('Version', o.version || '–'),
    stat('Image', '', IMAGE_STATE[o.state] || ['', o.state || '–']),
    stat('Slot', o.slot || '–'),
    stat('Latest release', '', updateChip(o.last_check)),
  );
  const ip = (v) => (v && v !== '-' ? v : 'Not connected');
  $('#sys-net').replaceChildren(
    el('div', { class: 'stat' }, el('span', { text: 'Gateway EUI' }), el('code', { text: g.eui || '–' })),
    stat('Wi-Fi', ip(o.wifi)),
    stat('Ethernet', ip(o.eth)),
    stat('Status LED', r.led || '–'),
  );
  const crashes = n(f.panic) + n(f.wdt) + n(f.brownout);
  $('#sys-rel').replaceChildren(
    stat('Uptime', fmtUptime(r.uptime_s || 0)),
    stat('Boots', String(n(f.boots))),
    stat('Crashes (panic)', String(n(f.panic)), n(f.panic) ? ['warn', String(n(f.panic))] : null),
    stat('Watchdog resets', String(n(f.wdt)), n(f.wdt) ? ['warn', String(n(f.wdt))] : null),
    stat('Brownouts', String(n(f.brownout)), n(f.brownout) ? ['warn', String(n(f.brownout))] : null),
    stat('Software restarts', String(n(f.sw))),
    stat('Abnormal resets in a row', '', n(f.run) ? ['bad', String(n(f.run))] : ['ok', crashes ? 'None now' : 'None']),
  );
  const ih = n(r.heap_internal), imin = n(r.heap_internal_min);
  $('#sys-mem').replaceChildren(
    stat('Internal RAM free', fmtKB(ih)),
    stat('Lowest since boot', fmtKB(imin), null),
    stat('PSRAM free', fmtKB(n(r.heap_psram)) + ' of 32 MB', n(r.heap_psram) / PSRAM_TOTAL),
  );
}

/* ---- system: every task, its stack and CPU (/api/tasks) ---------------------- */
let tasksPrev = null, tasksSort = 'cpu', tasksAll = false;
const TASKS_SHORT = 8; /* rows before "Show all" */
/* Headroom from the share of its own stack a task has used, with a floor in
 * bytes (an interrupt on top of the deepest call still needs some). */
function headroom(t) {
  const used = t.stack ? 1 - t.free / t.stack : 0;
  return used >= 0.9 || t.free < 256 ? ['bad', 'Critical'] : used >= 0.75 || t.free < 512 ? ['warn', 'Low'] : ['ok', 'OK'];
}
const fmtCPU = (v) => (v == null ? '–' : v >= 9.95 ? Math.round(v) + ' %' : v >= 0.05 ? v.toFixed(1) + ' %' : '0 %');
const fmtKiB = (b) => (b / 1024).toFixed(b % 1024 ? 1 : 0);
async function refreshTasks() {
  let d;
  try { d = await (await api('/api/tasks')).json(); } catch (e) { return; }
  const prev = tasksPrev;
  tasksPrev = d;
  const dt = prev ? d.now - prev.now : 0;
  const before = new Map(prev ? prev.tasks.map((t) => [t.name, t.run]) : []);
  /* CPU: share of one core since the last read, so it needs two reads */
  for (const t of d.tasks) t.cpu = dt > 0 && before.has(t.name) ? Math.max(0, (t.run - before.get(t.name)) / dt * 100) : null;
  const cores = [];
  for (let c = 0; c < d.cores; c++) {
    const idle = d.tasks.find((t) => t.idle && t.core === c);
    const load = idle && idle.cpu != null ? Math.max(0, Math.min(100, 100 - idle.cpu)) : null;
    const m = el('div', { class: 'meter' }, el('span', { class: load >= 90 ? 'bad' : load >= 70 ? 'warn' : '' }));
    m.firstChild.style.width = (load || 0) + '%';
    cores.push(el('div', { class: 'core' }, el('span', { text: 'Core ' + c }), el('b', { text: load == null ? 'Measuring…' : Math.round(load) + ' % busy' }), m));
  }
  $('#sys-cores').replaceChildren(...cores);
  if (!prev) setTimeout(refreshTasks, 1500); /* a quick second read for the first CPU figures */
  renderTasks();
}
function renderTasks() {
  if (!tasksPrev) return;
  const used = (t) => (t.stack ? 1 - t.free / t.stack : 0);
  const by = {
    name: (a, b) => a.name.localeCompare(b.name),
    cpu: (a, b) => (b.cpu || 0) - (a.cpu || 0) || a.name.localeCompare(b.name),
    stack: (a, b) => used(b) - used(a) || a.free - b.free,
  };
  for (const b of $$('#sys-tasks .sort')) b.classList.toggle('on', b.dataset.sort === tasksSort);
  /* the idle tasks are the core meters */
  const all = tasksPrev.tasks.filter((t) => !t.idle).sort(by[tasksSort]);
  const more = $('#tasks-more');
  more.hidden = all.length <= TASKS_SHORT;
  more.textContent = tasksAll ? 'Show fewer' : 'Show all ' + all.length + ' tasks';
  const rows = (tasksAll ? all : all.slice(0, TASKS_SHORT)).map((t) => {
    const h = headroom(t);
    const m = el('div', { class: 'meter' }, el('span', { class: h[0] === 'ok' ? '' : h[0] }));
    m.firstChild.style.width = used(t) * 100 + '%';
    return el('tr', {},
      el('td', {}, el('b', { text: t.name })),
      el('td', { class: 'num', text: fmtCPU(t.cpu) }),
      el('td', { class: 'use' }, el('div', {}, t.stack ? m : null,
        el('span', { class: 'sub', text: t.stack ? fmtKiB(t.stack - t.free) + ' of ' + fmtKiB(t.stack) + ' KB' : t.free + ' B never used' }))),
      /* only a problem gets a chip */
      el('td', {}, h[0] === 'ok' ? el('span', { class: 'sub', text: h[1] }) : el('span', { class: 'chip dot ' + h[0], text: h[1] })),
      el('td', { class: 'num', text: t.core < 0 ? 'Any' : String(t.core) }),
      el('td', { class: 'num', text: String(t.prio) }));
  });
  $('#sys-tasks tbody').replaceChildren(...rows);
}

/* ---- overview: the gateway itself, and diagnostics to share ---------------- */
const PSRAM_TOTAL = 32 * 1048576; /* ESP32-P4 module: 32 MB PSRAM */
function copyable(text) {
  const b = el('button', { type: 'button', class: 'ghost', text: 'Copy' });
  b.addEventListener('click', async () => {
    try { await navigator.clipboard.writeText(text); b.textContent = 'Copied'; setTimeout(() => { b.textContent = 'Copy'; }, 2000); } catch (e) { toast('Copy failed: select the text instead.', true); }
  });
  return el('span', { class: 'copy' }, el('code', { text }), b);
}
function renderGateway() {
  /* what you need while operating it: its identity, how it is connected, its firmware */
  const o = S.ota || {}, g = S.ttn || {};
  const net = o.eth && o.eth !== '-' ? 'Ethernet · ' + o.eth : o.wifi && o.wifi !== '-' ? 'Wi-Fi · ' + o.wifi : 'Not connected';
  $('#ov-gw').replaceChildren(
    el('div', { class: 'stat' }, el('span', { text: 'Gateway EUI' }), g.eui ? copyable(g.eui) : el('b', { text: '–' })),
    stat('Connection', net),
    stat('Firmware', o.version || '–'), /* a newer release shows as a notice above */
  );
}

/* Everything the status, radio and device APIs report, as one JSON object. Numbers
 * are numbers; with `redact`, EUIs and IPv4 addresses are replaced. No keys: the
 * APIs never return them. */
async function diagnostics(redact) {
  const typed = (o) => Object.fromEntries(Object.entries(o).map(([k, v]) => [k, v !== '' && !isNaN(+v) && !/^0[0-9]/.test(v) && !/^[0-9A-Fa-f]{16}$/.test(v) ? +v : v]));
  const out = { generated: new Date().toISOString(), page: location.host, status: {} };
  for (const [k, v] of Object.entries(S)) out.status[k] = typed(v);
  try { out.tasks = await (await api('/api/tasks')).json(); } catch (e) { out.tasks = 'unavailable: ' + e.message; }
  try { out.radio = await (await api('/api/radio')).json(); } catch (e) { out.radio = 'unavailable: ' + e.message; }
  try {
    out.devices = (await (await api('/api/devices')).json()).map((d) => ({ name: d.name, dev_eui: d.dev_eui, mac: d.mac, joined: d.joined, answered: d.answered,
      fcnt_up: d.fcnt_up, uplinks: d.uplinks, seen_s: d.seen_s, rssi: d.rssi, snr: d.snr, sf: d.sf, battery: d.battery, margin: d.margin,
      tx_power: d.tx_power, adr: d.adr, rx1_delay_s: d.rx1_delay_s, rx2_hz: d.rx2_hz, rx2_dr: d.rx2_dr, class: d.class }));
  } catch (e) { out.devices = 'unavailable: ' + e.message; }
  let s = JSON.stringify(out, null, 2);
  if (redact) {
    s = s.replace(/\b[0-9A-Fa-f]{16}\b/g, 'XXXXXXXXXXXXXXXX')
      .replace(/\b(?:[0-9]{1,3}\.){3}[0-9]{1,3}\b/g, 'x.x.x.x')
      .replace(new RegExp(location.hostname.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'), 'g'), 'gateway.local');
  }
  return s;
}

/* ---- header pill ------------------------------------------------------------ */
function renderPill() {
  const g = S.ttn || {}, r = S.radio || {};
  const p = $('#link-pill');
  const mode = g.mode || 'ttn';
  const up = r.upstream === 'up';
  p.className = 'pill ' + (mode === 'standalone' ? '' : up ? 'ok' : 'warn');
  p.textContent = MODES[mode] + (mode === 'standalone' ? '' : up ? ' · connected' : ' · not connected');
}

/* ---- setup steps ------------------------------------------------------------ */
function stepState(id, cls, chipText, chipCls) {
  const li = $(id);
  li.classList.remove('done', 'todo', 'skip');
  li.classList.add(cls);
  /* the marker shows done / to do; only a skipped step says why, as plain text */
  $('.state', li).textContent = cls === 'skip' ? chipText : '';
}
function renderSetup() {
  const g = S.ttn || {}, r = S.radio || {}, ln = S.lns || {};
  const mode = g.mode || 'ttn';
  stepState('#step-mode', 'done', MODES[mode], 'ok');
  for (const c of $$('.mode-card')) c.classList.toggle('current', $('input', c).value === mode);
  const pick = $('#mode-pick');
  if (!pick.dataset.dirty) for (const x of $$('input', pick)) x.checked = x.value === mode;
  const sel = $('input:checked', pick);
  for (const c of $$('.mode-card')) c.classList.toggle('picked', $('input', c).checked);
  $('#mode-save').disabled = !sel || sel.value === mode;
  $('#mode-note').textContent = sel && sel.value !== mode ? 'The gateway restarts in the new mode.' : '';

  if (mode === 'standalone') stepState('#step-ttn', 'skip', 'Not needed in standalone mode');
  else if (g.lns_key === 'none') stepState('#step-ttn', 'todo', 'API key missing', 'warn');
  else if (r.upstream !== 'up') stepState('#step-ttn', 'todo', 'Key stored, not connected', 'warn');
  else stepState('#step-ttn', 'done', 'Connected to ' + srvName(g.server), 'ok');
  $('#ttn-eui').textContent = g.eui || '';
  $('#ttn-server').textContent = g.server ? 'wss://' + g.server + (g.port ? ':' + g.port : '') : '';
  const key = $('#ttn-key');
  key.className = 'chip dot ' + (g.lns_key === 'set' ? 'ok' : 'warn');
  key.textContent = g.lns_key === 'set' ? 'Stored' : 'Not set';
  const sp = $('#srv-pick');
  if (!sp.dataset.dirty && g.server) {
    const preset = $$('input', sp).find((i) => i.value === g.server && +g.port === 8887);
    for (const i of $$('input', sp)) i.checked = preset ? i === preset : i.value === 'other';
    $('#lns-host').value = g.server;
    $('#lns-port').value = g.port || '';
  }
  showServerHelp();
  if (mode !== 'standalone' && !$('#ttn-qr').getAttribute('src')) $('#ttn-qr').src = '/api/ttn/qr.svg';

  const n = +ln.devices || 0;
  if (mode === 'ttn') stepState('#step-dev', 'skip', 'Not used in network server mode');
  else if (n === 0) stepState('#step-dev', 'todo', 'No devices yet', 'warn');
  else stepState('#step-dev', 'done', n + (n === 1 ? ' device' : ' devices'), 'ok');
  $('#dev-summary').textContent = n === 0 ? 'Add the devices this gateway serves itself: DevEUI, JoinEUI and AppKey.'
    : (ln.joined || 0) + ' of ' + n + ' devices have joined.';
  $('#dev-note').textContent = mode === 'ttn'
    ? 'These devices are served in hybrid and standalone modes. You can add them now and change the mode later.'
    : 'Up to 32 OTAA devices, EU868, class A or C.';
  renderMqtt();
}

/* MQTT step (ADR-0018). The form shows the stored settings until edited. */
function renderMqtt() {
  const q = S.mqtt || {};
  const st = q.state || 'off';
  if (st === 'connected') stepState('#step-mqtt', 'done', 'Connected to ' + q.host, 'ok');
  else if (st === 'refused') stepState('#step-mqtt', 'todo', 'Broker refused the user or password', 'bad');
  else if (st === 'tls_error') stepState('#step-mqtt', 'todo', 'TLS failed: check the host name and the broker certificate', 'bad');
  else if (st === 'connecting') stepState('#step-mqtt', 'todo', 'Connecting to ' + q.host, 'warn');
  else if (st === 'no_host') stepState('#step-mqtt', 'todo', 'No broker set', 'warn');
  else if (q.on === 'on') stepState('#step-mqtt', 'todo', 'Restart to connect', 'warn');
  else stepState('#step-mqtt', 'skip', 'Optional');
  const chip = $('#mqtt-state');
  const C = { connected: ['ok', 'Connected to ' + q.host], refused: ['bad', 'Refused: check the user and password'],
    connecting: ['warn', 'Connecting to ' + q.host + '…'], no_host: ['warn', 'No broker set'] };
  const [cc, ct] = C[st] || (q.on === 'on' ? ['warn', 'Saved: restart to connect'] : ['', 'Off']);
  chip.className = 'chip dot ' + cc;
  chip.textContent = ct + (st === 'connected' ? ', ' + n(q.published) + ' published' + (n(q.dropped) ? ', ' + n(q.dropped) + ' dropped' : '') : '');
  const f = $('#mqtt-form');
  if (!f.dataset.dirty && q.state) {
    $('#mqtt-on').checked = q.on === 'on';
    $('#mqtt-host').value = q.host && q.host !== '-' ? q.host : '';
    $('#mqtt-port').value = q.port || 1883;
    $('#mqtt-user').value = q.user && q.user !== '-' ? q.user : '';
    $('#mqtt-gwid').value = q.gw_id || '';
    $('#mqtt-tls').checked = q.tls === 'on';
    f.mqtt_pass.placeholder = q.pass === 'set' ? 'Leave empty to keep the stored password' : 'Not set';
  }
  $('#mqtt-plain-note').hidden = $('#mqtt-tls').checked;
  const id = $('#mqtt-gwid').value || q.gw_id || '<gateway id>';
  $('#mqtt-topics').textContent = 'lora/' + id + '/status, lora/' + id + '/<DevEUI>/up, …/join, …/down/result; in: …/down';
}

/* The server picker: TTN presets (port 8887) or another host and port; the
 * TTN registration help only for TTN. */
function pickedServer() {
  const v = ($('#srv-pick input:checked') || {}).value;
  if (!v) return null;
  if (v === 'other') return { host: $('#lns-host').value.trim(), port: +$('#lns-port').value || 0 };
  return { host: v, port: 8887 };
}
function showServerHelp() {
  const v = ($('#srv-pick input:checked') || {}).value;
  for (const o of $$('.srv-opt')) o.classList.toggle('picked', $('input', o).checked);
  $('#srv-other').hidden = v !== 'other';
  const p = pickedServer();
  const ttn = p ? isTTN(p.host) : true;
  $('#help-ttn').hidden = !ttn;
  $('#help-other').hidden = ttn;
  if (p && ttn) $('#ttn-console').href = 'https://' + p.host + '/console/gateways/add';
}

async function refreshStatus() {
  try {
    const s = await (await api('/api/status')).json();
    S = {};
    for (const [g, line] of Object.entries(s)) S[g] = kv(line);
    renderPill();
    renderOverview();
    renderGateway();
    renderSetup();
    renderSystem();
  } catch (e) { /* shown at the next login */ }
}

/* ---- devices ---------------------------------------------------------------- */
function bars(snr, seen) {
  const n = !seen ? 0 : snr > 5 ? 4 : snr > 0 ? 3 : snr > -7 ? 2 : 1;
  return el('span', { class: 'bars', 'aria-hidden': 'true' }, [1, 2, 3, 4].map((i) => el('i', { class: i <= n ? 'on' : '' })));
}
function battery(level) {
  if (level < 0) return null;
  const txt = level === 0 ? 'External power' : level === 255 ? 'Battery unknown' : Math.round(level * 100 / 254) + '%';
  const frac = level > 0 && level < 255 ? level / 254 : level === 0 ? 1 : 0;
  const s = sv('svg', { viewBox: '0 0 22 12', 'aria-hidden': 'true' }, sv('rect', { class: 'shell', x: 0.75, y: 0.75, width: 18, height: 10.5, rx: 2 }),
    sv('rect', { class: 'fill', x: 2.5, y: 2.5, width: (15 * frac).toFixed(1), height: 7, rx: 1 }), sv('rect', { x: 19.5, y: 4, width: 2, height: 4, rx: 1, fill: 'currentColor' }));
  return el('span', { class: 'batt' }, s, txt);
}
async function refreshDevices() {
  try {
    const list = await (await api('/api/devices')).json();
    if (!list.length) {
      $('#dev-list').replaceChildren(el('div', { class: 'empty' }, el('b', { text: 'No devices yet' }), 'Add a device with its DevEUI, JoinEUI and AppKey.'));
      return;
    }
    $('#dev-list').replaceChildren(...list.map((d) => {
      const seen = d.seen_s >= 0;
      const status = !d.answered ? ['warn', 'Join seen, not answered'] : d.joined ? ['ok', 'Joined'] : ['', 'Not joined'];
      const del = el('button', { type: 'button', class: 'ghost small admin', text: 'Remove' });
      del.addEventListener('click', async () => {
        if (!confirm('Remove ' + (d.name || d.dev_eui) + '? It must join again after you add it back.')) return;
        try { await post('/api/devices/delete', 'dev_eui=' + d.dev_eui); toast('Device removed.'); refreshDevices(); } catch (e) { toast(e.message, true); }
      });
      /* class A or C (ADR-0020): C gets its downlinks at once, on RX2 */
      const cls = el('select', { class: 'admin small', 'aria-label': 'Class of ' + (d.name || d.dev_eui) },
        el('option', { value: 'A', text: 'Class A' }), el('option', { value: 'C', text: 'Class C' }));
      cls.value = d.class === 'C' ? 'C' : 'A';
      cls.addEventListener('change', async () => {
        try { await post('/api/devices/settings', 'dev_eui=' + d.dev_eui + '&class=' + cls.value); toast('Class ' + cls.value + ' set.'); refreshDevices(); } catch (e) { toast(e.message, true); refreshDevices(); }
      });
      /* class C: how long a confirmed downlink waits for the uplink with its ACK */
      const ack = el('input', { type: 'number', min: 5, max: 3600, step: 1, value: d.c_ack_s || 60, class: 'small',
        'aria-label': 'ACK wait in seconds', title: 'How long a confirmed downlink waits for the device to acknowledge it' });
      ack.addEventListener('change', async () => {
        try { await post('/api/devices/settings', 'dev_eui=' + d.dev_eui + '&c_ack_s=' + ack.value); toast('ACK wait ' + ack.value + ' s.'); } catch (e) { toast(e.message, true); refreshDevices(); }
      });
      const ackBox = d.class === 'C' ? el('label', { class: 'admin ack-wait' }, 'ACK wait ', ack, ' s') : null;
      return el('div', { class: 'dev' },
        el('div', { class: 'who' }, el('b', { text: d.name || 'Unnamed device' }), el('code', { text: d.dev_eui }),
          el('span', { class: 'chip viewer-only', text: 'Class ' + (d.class === 'C' ? 'C' : 'A') }), cls, ackBox,
          d.waiting ? el('span', { class: 'chip warn', text: d.waiting + ' downlink' + (d.waiting > 1 ? 's' : '') + ' waiting' }) : null),
        el('div', { class: 'sig' }, bars(d.snr, seen), el('span', { text: seen ? d.rssi + ' dBm · SNR ' + d.snr + ' dB · SF' + d.sf : 'No signal yet' })),
        el('div', { class: 'seen' }, el('span', { class: 'chip dot ' + status[0], text: status[1] }),
          el('small', { text: ago(d.seen_s) + (d.uplinks ? ' · ' + d.uplinks + ' uplinks' : '') + (d.joined ? ' · FCnt ' + d.fcnt_up : '') }),
          battery(d.battery)),
        del);
    }));
  } catch (e) { /* the status refresh reports errors */ }
}

/* ---- radio ------------------------------------------------------------------ */
const PLAN_SRC = { default: ['Built-in EU868 plan', ''], local: ['Plan set on this page', 'warn'], server: ['Plan from the network server', 'ok'] };
/* EU868 duty-cycle sub-bands, as the radio task enforces them (core/src/txpk.c) */
const SUBBANDS = [[863.0, 865.0, '0.1%'], [865.0, 868.0, '1%'], [868.0, 868.6, '1%'], [868.7, 869.2, '0.1%'], [869.4, 869.65, '10%'], [869.7, 870.0, '1%']];
let P = null; /* last /api/radio */

function drawSpectrum(p) {
  /* Lane plot (one row per radio, then the single-SF and FSK channels) on one
   * frequency axis; the EU868 duty-cycle sub-bands are a ruler under it. */
  const svg = $('#spectrum');
  const L = 112, R = 984, TOP = 30, LANE = 46, BAR = 16;
  const multi = p.multisf.map((c, i) => ({ ...c, i }));
  const lanes = [
    { name: 'Radio 0', sub: MHZ(p.radios[0]), radio: 0 },
    { name: 'Radio 1', sub: MHZ(p.radios[1]), radio: 1 },
    { name: 'Single-SF', sub: p.std.enable ? 'SF' + p.std_sf + ' · ' + p.std_bw_khz + ' kHz' : 'Off', ch: p.std, kind: 'std', bw: p.std_bw_khz },
    { name: 'FSK', sub: p.fsk.enable ? '50 kbit/s' : 'Off', ch: p.fsk, kind: 'fsk', bw: 125 },
  ];
  const hzOf = (c) => (c.hz ? c.hz : p.radios[c.radio] + c.if);
  /* one decimal on the 100 kHz grid, three when the frequency needs them */
  const fq = (mhz) => (Math.round(mhz * 1000) % 100 ? mhz.toFixed(3) : mhz.toFixed(1));
  const pts = [p.rx2_hz / 1e6];
  for (const r of p.radios) pts.push(r / 1e6 - 0.4625, r / 1e6 + 0.4625);
  for (const c of [...multi, p.std, p.fsk]) if (c.enable) pts.push(hzOf(c) / 1e6);
  let lo = Math.max(863, Math.min(...pts) - 0.1), hi = Math.min(870, Math.max(...pts) + 0.1);
  if (hi - lo < 1) { const mid = (lo + hi) / 2; lo = mid - 0.5; hi = mid + 0.5; }
  const x = (mhz) => L + (mhz - lo) / (hi - lo) * (R - L);
  const RULER = TOP + lanes.length * LANE + 6, AXIS = RULER + 24, H = AXIS + 30;
  const kids = [];
  const tip = (node, text) => { node.setAttribute('data-tip', text); return node; };
  /* gridlines first: recessive hairlines at the axis ticks */
  const step = hi - lo > 4 ? 1 : hi - lo > 2 ? 0.5 : 0.25;
  const ticks = [];
  for (let t = Math.ceil(lo / step - 1e-9) * step; t <= hi + 1e-9; t += step) ticks.push(t);
  for (const t of ticks) kids.push(sv('line', { class: 'grid', x1: x(t), x2: x(t), y1: TOP - 6, y2: RULER - 4 }));
  /* RX2 behind the rows, so channels and their labels stay on top */
  const xr = x(p.rx2_hz / 1e6);
  kids.push(tip(sv('line', { class: 'rx2', x1: xr, x2: xr, y1: TOP - 6, y2: RULER - 4 }),
    'RX2 for your devices: ' + MHZ(p.rx2_hz) + ', DR' + p.rx2_dr + '. The network server sets its own for its devices.'));
  kids.push(sv('text', { class: 'rx2-label', x: xr, y: TOP - 12, 'text-anchor': xr > R - 80 ? 'end' : xr < L + 80 ? 'start' : 'middle' },
    'RX2 (your devices) ' + (p.rx2_hz / 1e6).toFixed(3)));
  lanes.forEach((ln, li) => {
    const yc = TOP + li * LANE + LANE / 2;
    kids.push(sv('text', { class: 'lane', x: 0, y: yc - 2 }, ln.name), sv('text', { class: 'lane-sub', x: 0, y: yc + 13 }, ln.sub));
    if (ln.radio !== undefined) {
      const c = p.radios[ln.radio] / 1e6, x0 = x(c - 0.4625), x1 = x(c + 0.4625);
      kids.push(tip(sv('rect', { class: 'track', x: x0, y: yc - 15, width: x1 - x0, height: 30, rx: 6 }),
        'Radio ' + ln.radio + ' receive window: ' + (c - 0.4625).toFixed(3) + '–' + (c + 0.4625).toFixed(3) + ' MHz'));
      const mine = multi.filter((m) => m.radio === ln.radio);
      for (const m of mine) {
        const f = hzOf(m) / 1e6, w = Math.max(4, x(f + 0.0625) - x(f - 0.0625));
        kids.push(tip(sv('rect', { class: 'ch multi' + (m.enable ? '' : ' off'), x: x(f) - w / 2, y: yc - BAR / 2, width: w, height: BAR, rx: 4 }),
          'Multi-SF channel ' + m.i + ': ' + fq(f) + ' MHz, SF7–12, 125 kHz' + (m.enable ? '' : ' (off)')));
      }
      /* direct labels only where they fit side by side */
      let last = -1e9, lastW = 0;
      for (const m of [...mine].filter((q) => q.enable).sort((a, b) => hzOf(a) - hzOf(b))) {
        const xm = x(hzOf(m) / 1e6), label = fq(hzOf(m) / 1e6), w = label.length * 6.6;
        if (xm - last < (w + lastW) / 2 + 8) continue;
        kids.push(sv('text', { class: 'ch-label', x: xm, y: yc + 26, 'text-anchor': 'middle' }, label));
        last = xm;
        lastW = w;
      }
    } else {
      const c = ln.ch, f = hzOf(c) / 1e6, half = ln.bw / 2000;
      const w = Math.max(4, x(f + half) - x(f - half));
      kids.push(tip(sv('rect', { class: 'ch ' + ln.kind + (c.enable ? '' : ' off'), x: x(f) - w / 2, y: yc - BAR / 2, width: w, height: BAR, rx: 4 }),
        ln.name + ' channel: ' + MHZ(f * 1e6) + ' on radio ' + c.radio + (c.enable ? '' : ' (off)')));
      if (c.enable) kids.push(sv('text', { class: 'ch-label', x: x(f), y: yc + 26, 'text-anchor': 'middle' }, fq(f)));
    }
  });
  /* duty-cycle ruler: one segment per sub-band, 2px apart */
  for (const [a, b, duty] of SUBBANDS) {
    if (b <= lo || a >= hi) continue;
    const x0 = x(Math.max(a, lo)) + 1, x1 = x(Math.min(b, hi)) - 1;
    kids.push(tip(sv('rect', { class: 'band', x: x0, y: RULER, width: Math.max(1, x1 - x0), height: 18, rx: 4 }), a + '–' + b + ' MHz: ' + duty + ' transmit duty cycle (gateway and devices)'));
    if (x1 - x0 > 58) kids.push(sv('text', { class: 'band-label', x: (x0 + x1) / 2, y: RULER + 13, 'text-anchor': 'middle' }, duty + ' duty'));
  }
  kids.push(sv('text', { class: 'lane-sub', x: 0, y: RULER + 13 }, 'Transmit duty'));
  for (const t of ticks) kids.push(sv('text', { class: 'tick', x: x(t), y: AXIS + 12, 'text-anchor': 'middle' }, t.toFixed(step < 0.5 ? 2 : 1)));
  kids.push(sv('text', { class: 'lane-sub', x: 0, y: AXIS + 12 }, 'MHz'));
  svg.setAttribute('viewBox', '0 0 1000 ' + H);
  svg.replaceChildren(...kids);
}

/* One tooltip for every chart mark with data-tip; hit area is the mark plus the pointer's slack. */
function chartTips() {
  const svg = $('#spectrum'), box = $('#spec-tip');
  const hide = () => { box.hidden = true; };
  svg.addEventListener('pointermove', (ev) => {
    const t = ev.target.closest('[data-tip]');
    if (!t) { hide(); return; }
    box.textContent = t.getAttribute('data-tip');
    box.hidden = false;
    const fr = svg.parentElement.getBoundingClientRect();
    box.style.left = Math.min(ev.clientX - fr.left + 12, fr.width - box.offsetWidth - 4) + 'px';
    box.style.top = (ev.clientY - fr.top + 14) + 'px';
  });
  svg.addEventListener('pointerleave', hide);
}

function renderRadio(p) {
  const src = PLAN_SRC[p.source] || [p.source, ''];
  const chip = $('#plan-src');
  chip.className = 'chip dot ' + src[1];
  chip.textContent = src[0];
  drawSpectrum(p);
  const row = (name, c, mod) => el('tr', { class: c.enable ? '' : 'off' }, el('td', { text: name }), el('td', { text: c.enable ? MHZ(c.hz) : 'Off' }),
    el('td', { text: 'Radio ' + c.radio + (c.if ? ' (' + (c.if > 0 ? '+' : '') + c.if / 1000 + ' kHz)' : '') }), el('td', { text: mod }));
  $('#radio-rows').replaceChildren(...p.multisf.map((c, i) => row('Multi-SF ' + i, c, 'LoRa SF7-12, 125 kHz')),
    row('Single-SF', p.std, 'LoRa SF' + p.std_sf + ', ' + p.std_bw_khz + ' kHz'), row('FSK', p.fsk, 'FSK 50 kbit/s'));

  const tx = $('#tx-form');
  if (!tx.dataset.dirty) { $('#tx-gain').value = (p.gain_db10 / 10).toFixed(1); $('#tx-on').checked = p.tx; $('#tx-lns').checked = p.lns_tx; }
  const ro = document.body.classList.contains('viewer');
  for (const i of $$('#tx-form input, #rx-form input, #rx-form select')) i.disabled = ro;
  $('#tx-on').disabled = ro || !p.tx_built;
  updateCap();
  const rx = $('#rx-form');
  if (!rx.dataset.dirty) { $('#rx-d').value = p.rx1_delay_s; $('#rx-f').value = (p.rx2_hz / 1e6).toFixed(3); $('#rx-dr').value = String(p.rx2_dr); }
  if (!$('#plan-form').dataset.dirty) fillPlanEditor(p);
}
/* Same rule as the firmware: 16.1 dBm EIRP minus the gain, never above the ceiling. */
function updateCap() {
  if (!P) return;
  const g = parseFloat($('#tx-gain').value);
  $('#tx-gain-out').textContent = g.toFixed(1) + ' dBi';
  const d = Math.round(161 - g * 10), cap = Math.min(d >= 0 ? Math.floor(d / 10) : -Math.ceil(-d / 10), P.ceiling_dbm);
  const on = $('#tx-on').checked;
  $('#tx-cap').textContent = on ? cap + ' dBm' + (cap === P.ceiling_dbm ? ' (firmware ceiling)' : '') : 'Transmit off';
  const span = (v) => Math.max(0, Math.min(1, (v + 6) / 33)) * 100 + '%'; /* scale -6..27 dBm */
  const bar = $('#tx-cap-bar');
  bar.style.width = on ? span(cap) : '0%';
  $('#tx-ceiling-mark').style.left = span(P.ceiling_dbm);
}
async function refreshRadio() {
  try { P = await (await api('/api/radio')).json(); renderRadio(P); } catch (e) { toast(e.message, true); }
}

/* Channel plan editor: one row per channel (multi-SF 0-7, single-SF, FSK). */
const PE_CHANS = ['m0', 'm1', 'm2', 'm3', 'm4', 'm5', 'm6', 'm7', 'std', 'fsk'];
function fillPlanEditor(p) {
  $('#pe-r0').value = (p.radios[0] / 1e6).toFixed(1);
  $('#pe-r1').value = (p.radios[1] / 1e6).toFixed(1);
  $('#pe-sf').value = String(p.std_sf);
  $('#pe-bw').value = String(p.std_bw_khz);
  $('#pe-rows').replaceChildren(...[...p.multisf, p.std, p.fsk].map((c, i) => {
    const sw = el('label', { class: 'switch small' }, el('input', { type: 'checkbox', class: 'pe-on', checked: c.enable, 'aria-label': 'On' }), el('span', { class: 'track', 'aria-hidden': 'true' }));
    const radio = el('select', { class: 'pe-radio', 'aria-label': 'Radio' }, el('option', { value: '0', text: '0' }), el('option', { value: '1', text: '1' }));
    radio.value = String(c.radio);
    return el('tr', { 'data-k': PE_CHANS[i] }, el('td', { text: i < 8 ? 'Multi-SF ' + i : i === 8 ? 'Single-SF' : 'FSK' }), el('td', {}, sw), el('td', {}, radio),
      el('td', {}, el('input', { type: 'number', class: 'pe-if', step: '25', min: '-500', max: '500', value: String(c.if / 1000), 'aria-label': 'Offset in kHz' })),
      el('td', { class: 'pe-hz' }));
  }));
  updatePlanFreqs();
}
function updatePlanFreqs() {
  const r = [parseFloat($('#pe-r0').value) * 1e6, parseFloat($('#pe-r1').value) * 1e6];
  for (const tr of $$('#pe-rows tr')) {
    const on = $('.pe-on', tr).checked;
    const hz = r[+$('.pe-radio', tr).value] + parseFloat($('.pe-if', tr).value) * 1000;
    tr.classList.toggle('off', !on);
    $('.pe-hz', tr).textContent = on && isFinite(hz) ? MHZ(hz) : 'Off';
  }
}
function planBody() {
  const b = new URLSearchParams();
  b.set('r0', String(Math.round(parseFloat($('#pe-r0').value) * 1e6)));
  b.set('r1', String(Math.round(parseFloat($('#pe-r1').value) * 1e6)));
  for (const tr of $$('#pe-rows tr')) {
    const v = [$('.pe-on', tr).checked ? 1 : 0, $('.pe-radio', tr).value, Math.round(parseFloat($('.pe-if', tr).value) * 1000)];
    if (tr.dataset.k === 'std') v.push($('#pe-sf').value, $('#pe-bw').value);
    b.set(tr.dataset.k, v.join(':'));
  }
  return b;
}

/* ---- logs ------------------------------------------------------------------- */
async function refreshLogs() {
  const pre = $('#logs');
  try {
    let lines = (await (await api('/api/logs' + ($('#prev').checked ? '?prev=1' : ''))).text()).split('\n');
    if ($('#warn-only').checked) lines = lines.filter((l) => /^[WE] /.test(l));
    pre.replaceChildren(...lines.map((l) => el('span', { class: /^[WE] /.test(l) ? l[0] : '', text: l + '\n' })));
    if (!lines.join('').trim()) pre.textContent = $('#prev').checked ? 'Nothing kept from the previous boot.' : 'The log is empty.';
    pre.scrollTop = pre.scrollHeight;
  } catch (e) { pre.textContent = e.message; }
}

/* ---- restart: wait for the gateway to go down and come back, then reload ---- */
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
/* /api/claim needs no session, so it answers before anyone logs in again */
async function alive() {
  const c = new AbortController();
  const t = setTimeout(() => c.abort(), 2500);
  try {
    const r = await fetch('/api/claim', { headers: H, signal: c.signal, cache: 'no-store' });
    return r.ok;
  } catch (e) {
    return false;
  } finally {
    clearTimeout(t);
  }
}
let busyTimer;
function busy(title, status, done) {
  const box = $('#busy');
  $('#busy-title').textContent = title;
  $('#busy-status').textContent = status;
  box.classList.toggle('stopped', !!done);
  $('#busy-actions').hidden = !done;
  box.hidden = false;
}
/* opts.downWithin: how long a restart may take to start (an OTA install downloads first);
 * opts.notDown: what to say when it never stopped. */
async function waitRestart(title, opts = {}) {
  const downWithin = opts.downWithin || 30000, upWithin = opts.upWithin || 120000;
  const t0 = Date.now();
  clearInterval(busyTimer);
  busyTimer = setInterval(() => { $('#busy-time').textContent = Math.round((Date.now() - t0) / 1000) + ' s'; }, 1000);
  $('#busy-time').textContent = '0 s';
  busy(title, opts.first || 'Restarting…');
  let down = false;
  while (Date.now() - t0 < downWithin) {
    if (!(await alive())) { down = true; break; }
    await sleep(1000);
  }
  if (!down) {
    clearInterval(busyTimer);
    busy(title, opts.notDown || 'The gateway did not restart.', true);
    return;
  }
  busy(title, 'Restarting…');
  const t1 = Date.now();
  while (Date.now() - t1 < upWithin) {
    await sleep(1500);
    if (await alive()) {
      busy(title, 'Back online. Reloading the page…');
      await sleep(600);
      location.reload();
      return;
    }
  }
  clearInterval(busyTimer);
  busy(title, 'Not back after ' + Math.round(upWithin / 60000) + ' minutes. Check the gateway’s power and network, then reload.', true);
}

/* ---- tabs -------------------------------------------------------------------- */
const TABS = ['overview', 'setup', 'devices', 'radio', 'system'];
function openTab(name) {
  if (!TABS.includes(name)) name = 'overview';
  for (const b of $$('.tabs button')) b.classList.toggle('on', b.dataset.tab === name);
  for (const t of $$('.tab')) t.hidden = t.id !== 'tab-' + name;
  if (location.hash.slice(1) !== name) history.replaceState(null, '', '#' + name);
  refreshStatus();
  if (name === 'devices' || name === 'setup') refreshDevices();
  if (name === 'radio') refreshRadio();
  if (name === 'system') { refreshLogs(); tasksPrev = null; refreshTasks(); }
}
const current = () => (TABS.find((t) => !$('#tab-' + t).hidden) || 'overview');

/* A form whose Save stays off until something changes. */
function dirtyForm(form, onInput) {
  const save = $('button[type=submit]', form);
  form.addEventListener('input', () => { form.dataset.dirty = '1'; if (save) save.disabled = false; if (onInput) onInput(); });
  return () => { delete form.dataset.dirty; if (save) save.disabled = true; };
}

document.addEventListener('DOMContentLoaded', () => {
  for (const b of $$('[data-theme-set]')) {
    b.setAttribute('role', 'radio');
    b.addEventListener('click', () => { const t = b.dataset.themeSet; store('theme', t); applyTheme(t); });
  }
  applyTheme(store('theme'));
  chartTips();
  for (const b of $$('.tabs button')) b.addEventListener('click', () => openTab(b.dataset.tab));
  for (const b of $$('[data-goto]')) b.addEventListener('click', () => openTab(b.dataset.goto));

  $('#login-form').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const msg = $('#login-msg');
    msg.textContent = 'Checking…';
    msg.classList.remove('bad');
    try {
      const r = await fetch('/api/login', { method: 'POST', headers: { ...H, ...FORM }, body: new URLSearchParams(new FormData(ev.target)) });
      if (!r.ok) throw new Error((await r.text()).trim());
      ev.target.reset();
      msg.textContent = '';
      show(true, (await r.json()).role);
    } catch (e) { msg.textContent = e.message; msg.classList.add('bad'); }
  });
  $('#logout').addEventListener('click', async () => { try { await api('/api/logout', { method: 'POST' }); } catch (e) { /* gone anyway */ } show(false); });

  /* setup */
  $('#mode-pick').addEventListener('change', (ev) => { ev.currentTarget.dataset.dirty = '1'; renderSetup(); });
  $('#mode-save').addEventListener('click', async () => {
    const m = $('#mode-pick input:checked');
    if (!m || !confirm('Change to ' + MODES[m.value] + '? The gateway restarts (about 15 s).')) return;
    try {
      const msg = await post('/api/settings', 'mode=' + m.value);
      delete $('#mode-pick').dataset.dirty;
      if (/restart/.test(msg)) waitRestart('Switching to ' + MODES[m.value]); else toast(msg);
    } catch (e) { toast(e.message, true); }
  });
  $('#ttn-copy').addEventListener('click', async (ev) => {
    try { await navigator.clipboard.writeText($('#ttn-eui').textContent); ev.target.textContent = 'Copied'; setTimeout(() => { ev.target.textContent = 'Copy'; }, 2000); }
    catch (e) { getSelection().selectAllChildren($('#ttn-eui')); }
  });
  $('#srv-pick').addEventListener('change', (ev) => { ev.currentTarget.dataset.dirty = '1'; showServerHelp(); });
  $('#srv-other').addEventListener('input', showServerHelp);
  $('#lns-form').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const g = S.ttn || {};
    const p = pickedServer();
    const body = new URLSearchParams();
    if (p && (!p.host || !p.port)) { toast('Enter the server host and port.', true); return; }
    if (p && p.host !== g.server) body.set('lns_host', p.host);
    if (p && p.port !== +g.port) body.set('lns_port', String(p.port));
    if (ev.target.lns_key.value) body.set('lns_key', ev.target.lns_key.value);
    if (![...body.keys()].length) { toast('Nothing changed: pick another server or enter a new key.'); return; }
    if (!confirm('Save the network server settings and restart the gateway?')) return;
    try {
      const msg = await post('/api/settings', body);
      ev.target.lns_key.value = '';
      delete $('#srv-pick').dataset.dirty;
      if (/restart/.test(msg)) waitRestart('Connecting to ' + srvName(p ? p.host : (S.ttn || {}).server)); else toast(msg);
    } catch (e) { toast(e.message, true); }
  });

  $('#mqtt-form').addEventListener('input', (ev) => { ev.currentTarget.dataset.dirty = '1'; renderMqtt(); });
  /* the usual port follows the TLS switch, unless another port was typed */
  $('#mqtt-tls').addEventListener('change', (ev) => {
    const p = $('#mqtt-port');
    if (ev.target.checked && (p.value === '' || p.value === '1883')) p.value = '8883';
    else if (!ev.target.checked && p.value === '8883') p.value = '1883';
  });
  $('#mqtt-form').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const f = ev.target, q = S.mqtt || {};
    const body = new URLSearchParams();
    const on = $('#mqtt-on').checked;
    if (on && !f.mqtt_host.value.trim()) { toast('Enter the broker address.', true); return; }
    if (!f.checkValidity()) { f.reportValidity(); return; }
    body.set('mqtt', on ? 'on' : 'off');
    body.set('mqtt_host', f.mqtt_host.value.trim());
    body.set('mqtt_port', f.mqtt_port.value || '1883');
    body.set('mqtt_user', f.mqtt_user.value.trim());
    if (f.mqtt_pass.value) body.set('mqtt_pass', f.mqtt_pass.value);
    if (f.mqtt_gw_id.value !== q.gw_id) body.set('mqtt_gw_id', f.mqtt_gw_id.value);
    body.set('mqtt_tls', $('#mqtt-tls').checked ? 'on' : 'off');
    if (!confirm('Save the MQTT settings and restart the gateway?')) return;
    try {
      const msg = await post('/api/settings', body);
      f.mqtt_pass.value = '';
      delete f.dataset.dirty;
      if (/restart/.test(msg)) waitRestart(on ? 'Connecting to the MQTT broker' : 'Restarting'); else toast(msg);
    } catch (e) { toast(e.message, true); }
  });

  /* devices */
  const addBox = $('#dev-add-box');
  $('#dev-add-open').addEventListener('click', () => { addBox.hidden = false; $('input', addBox).focus(); });
  $('#dev-add-cancel').addEventListener('click', () => { addBox.hidden = true; $('#dev-add').reset(); });
  $('#dev-add').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    try {
      await post('/api/devices', new URLSearchParams(new FormData(ev.target)));
      toast('Device added. It can join now.');
      ev.target.reset();
      addBox.hidden = true;
      refreshDevices();
      refreshStatus();
    } catch (e) { toast(e.message, true); }
  });

  /* radio */
  const txClean = dirtyForm($('#tx-form'), updateCap);
  $('#tx-form').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const body = new URLSearchParams({ gain: $('#tx-gain').value, tx: $('#tx-on').checked ? 'on' : 'off', lns_tx: $('#tx-lns').checked ? 'on' : 'off' });
    try { toast(await post('/api/radio', body)); txClean(); refreshRadio(); } catch (e) { toast(e.message, true); }
  });
  const rxClean = dirtyForm($('#rx-form'));
  $('#rx-form').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const body = new URLSearchParams({ rx1_delay: $('#rx-d').value, rx2_hz: String(Math.round(parseFloat($('#rx-f').value) * 1e6)), rx2_dr: $('#rx-dr').value });
    try { toast(await post('/api/radio', body)); rxClean(); refreshRadio(); } catch (e) { toast(e.message, true); }
  });
  $('#plan-form').addEventListener('input', (ev) => { ev.currentTarget.dataset.dirty = '1'; updatePlanFreqs(); });
  const savePlan = async (body, what) => {
    if (!confirm(what + ' The radio restarts (about 3 s); your devices must join again.')) return;
    try { toast(await post('/api/radio/plan', body)); delete $('#plan-form').dataset.dirty; refreshRadio(); } catch (e) { toast(e.message, true); }
  };
  $('#plan-form').addEventListener('submit', (ev) => { ev.preventDefault(); savePlan(planBody(), 'Save this channel plan?'); });
  $('#pe-reset').addEventListener('click', () => savePlan(new URLSearchParams({ reset: '1' }), 'Go back to the built-in EU868 plan?'));

  /* system */
  $('#logs-refresh').addEventListener('click', refreshLogs);
  $('#prev').addEventListener('change', refreshLogs);
  $('#warn-only').addEventListener('change', refreshLogs);
  const act = async (path, method) => { try { toast((await (await api(path, { method })).text()).trim() || 'Done.'); } catch (e) { toast(e.message, true); } };
  $('#ota-check').addEventListener('click', async () => {
    if (!confirm('Download and install the latest release if it differs from this one? The gateway restarts.')) return;
    try {
      await api('/api/ota/check', { method: 'POST' });
      waitRestart('Installing the latest release', { downWithin: 180000, upWithin: 180000, first: 'Checking for a release and downloading it…',
        notDown: 'No restart: this gateway already runs the latest release, or the download failed. The log says which.' });
    } catch (e) { toast(e.message, true); }
  });
  $('#reboot').addEventListener('click', async () => {
    if (!confirm('Restart the gateway now?')) return;
    try { await api('/api/reboot', { method: 'POST' }); waitRestart('Restarting the gateway'); } catch (e) { toast(e.message, true); }
  });
  $('#busy-close').addEventListener('click', () => { $('#busy').hidden = true; });
  $('#diag-copy').addEventListener('click', async (ev) => {
    const b = ev.currentTarget;
    try {
      await navigator.clipboard.writeText(await diagnostics($('#diag-redact').checked));
      toast($('#diag-redact').checked ? 'Diagnostics copied, EUIs and IP addresses hidden.' : 'Diagnostics copied, including EUIs and IP addresses.');
      b.textContent = 'Copied';
      setTimeout(() => { b.textContent = 'Copy diagnostics'; }, 2000);
    } catch (e) { toast('Copy failed: ' + e.message, true); }
  });
  $('#busy-reload').addEventListener('click', () => location.reload());
  $('#cd-del').addEventListener('click', () => { if (confirm('Erase the stored crash dump?')) act('/api/coredump', 'DELETE'); });

  /* claim: with no admin password yet, the first LAN visit sets it (ADR-0014) */
  fetch('/api/claim', { headers: H }).then((r) => (r.ok ? r.json() : null)).then((st) => {
    if (!st || st.admin_set) return;
    if (st.open) { $('#setup').hidden = false; $('#login').hidden = true; }
    else $('#login-msg').textContent = 'No admin password yet and the claim window has closed: restart the gateway, or hold its button for 5 s.';
  }).catch(() => {});
  $('#setup-form').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const msg = $('#setup-msg');
    msg.classList.remove('bad');
    if (ev.target.admin.value !== $('#admin2').value) { msg.textContent = 'The two passwords differ.'; msg.classList.add('bad'); return; }
    msg.textContent = 'Saving…';
    try {
      const r = await fetch('/api/claim', { method: 'POST', headers: { ...H, ...FORM }, body: new URLSearchParams(new FormData(ev.target)) });
      const t = (await r.text()).trim();
      if (!r.ok) throw new Error(t);
      ev.target.reset();
      $('#setup').hidden = true;
      $('#login').hidden = false;
      $('#login-msg').textContent = 'Admin password set. Log in as admin.';
    } catch (e) { msg.textContent = e.message; msg.classList.add('bad'); }
  });

  window.addEventListener('hashchange', () => { if (!$('#app').hidden) openTab(location.hash.slice(1)); });
  const role = sessionStorage.getItem('role');
  if (role) show(true, role); else show(false);
  setInterval(() => {
    if ($('#app').hidden || !$('#busy').hidden) return; /* no polling while it restarts */
    const t = current();
    if (t !== 'system') refreshStatus();
    if (t === 'devices') refreshDevices();
  }, 10000);
  setInterval(() => { if (!$('#app').hidden && $('#busy').hidden && current() === 'system') refreshTasks(); }, 5000);
  for (const b of $$('#sys-tasks .sort')) b.addEventListener('click', () => { tasksSort = b.dataset.sort; renderTasks(); });
  $('#tasks-more').addEventListener('click', () => { tasksAll = !tasksAll; renderTasks(); });
});
