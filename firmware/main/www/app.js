'use strict';
const $ = (s) => document.querySelector(s);
const H = { 'X-Requested-With': 'gw' };

async function api(path, opts = {}) {
  const r = await fetch(path, { credentials: 'same-origin', ...opts, headers: { ...H, ...(opts.headers || {}) } });
  if (r.status === 401) { show(false); throw new Error('login required'); }
  if (!r.ok) throw new Error((await r.text()).trim() || r.statusText);
  return r;
}

function show(loggedIn, role) {
  $('#login').hidden = loggedIn;
  $('#app').hidden = !loggedIn;
  $('#logout').hidden = !loggedIn;
  $('#who').textContent = loggedIn ? role : '';
  document.body.classList.toggle('viewer', role === 'viewer');
  if (loggedIn) { sessionStorage.setItem('role', role); refreshStatus(); } else { sessionStorage.removeItem('role'); }
}

function kv(line) {
  const out = [];
  const re = /(\w+)=("[^"]*"|\S+)/g;
  let m;
  while ((m = re.exec(line)) !== null) out.push([m[1], m[2].replace(/^"|"$/g, '')]);
  return out;
}

const LABELS = {
  ota: 'Firmware', faults: 'Resets and faults', stack_free: 'Least free stack (bytes)', radio: 'Radio and system',
  version: 'Version', slot: 'Slot', state: 'Image state', wifi: 'Wi-Fi IP', eth: 'Ethernet IP', upstream: 'Upstream (TTN)',
  last_check: 'Last update check', boots: 'Boots', panic: 'Panics', wdt: 'Watchdog resets', brownout: 'Brownouts',
  sw: 'Software restarts', run: 'Abnormal resets in a row', rx: 'Packets received', crc_ok: 'CRC OK', dropped: 'Dropped (queue full)',
  starts: 'Concentrator starts', led: 'Status LED', restarts: 'Concentrator restarts', uptime_s: 'Uptime', heap_internal: 'Internal RAM free',
  heap_internal_min: 'Internal RAM free (lowest)', heap_psram: 'PSRAM free',
};
const fmtUptime = (s) => { s = +s; const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
  return (d ? d + ' d ' : '') + (d || h ? h + ' h ' : '') + m + ' min'; };
const fmtKB = (b) => (+b >= 1048576 ? (b / 1048576).toFixed(1) + ' MB' : Math.round(b / 1024) + ' KB');
function fmt(k, v) {
  if (k === 'uptime_s') return fmtUptime(v);
  if (k.startsWith('heap_')) return fmtKB(v);
  return v;
}
function tile(k, v, sub, cls) {
  const t = document.createElement('div');
  t.className = 'tile';
  for (const [c, x] of [['k', k], ['v', v], ['s', sub || '']]) {
    const e = document.createElement('div');
    e.className = c + (c === 'v' && cls ? ' ' + cls : '');
    e.textContent = x;
    if (c === 'v') e.title = x; /* full text when the tile cuts it */
    t.append(e);
  }
  return t;
}

async function refreshStatus() {
  try {
    const s = await (await api('/api/status')).json();
    const all = {};
    for (const [g, line] of Object.entries(s)) all[g] = Object.fromEntries(kv(line));
    const o = all.ota || {}, r = all.radio || {}, f = all.faults || {}, g = all.ttn || {};
    const ttnOn = g.report !== 'off';
    const rx = +r.rx || 0, ok = +r.crc_ok || 0;
    const restarts = +r.restarts || 0, run = +f.run || 0;
    const notices = [];
    if (ttnOn && r.upstream !== 'up') notices.push(['warn', 'Upstream (TTN) is down: uplinks are not forwarded.']);
    if (o.state && o.state !== 'valid') notices.push(['warn', 'Firmware ' + o.state + ': it rolls back unless its health check passes.']);
    if (run > 0) notices.push(['warn', run + ' crash or watchdog reset(s) in a row. See Logs → Previous boot.']);
    if (restarts > 0) notices.push(['warn', 'The concentrator restarted ' + restarts + ' time(s) since boot.']);
    if ((o.last_check || '').startsWith('update available')) notices.push(['', 'A newer release is available: Firmware → Install latest release.']);
    $('#notices').replaceChildren(...notices.map(([cls, text]) => {
      const li = document.createElement('li');
      li.className = cls;
      li.textContent = text;
      return li;
    }));
    $('#tiles').replaceChildren(
      ttnOn ? tile('Upstream', r.upstream === 'up' ? 'Connected' : 'Down', 'TTN, Semtech UDP', r.upstream === 'up' ? 'ok' : 'warn')
            : tile('Upstream', 'Off', 'TTN reporting is off'),
      tile('Packets', String(rx), ok + ' with valid CRC'),
      tile('Uptime', fmtUptime(r.uptime_s || 0), (f.boots || '?') + ' boots, ' + (f.panic || 0) + ' panics'),
      tile('Internal RAM', fmtKB(r.heap_internal || 0), 'lowest ' + fmtKB(r.heap_internal_min || 0)),
    );
    $('#ttn').hidden = !ttnOn;
    $('#ttn-eui').textContent = g.eui || '';
    $('#ttn-server').textContent = (g.server || '') + (g.port ? ' : ' + g.port + ' (UDP)' : '');
    if (ttnOn && !$('#ttn-qr').src) $('#ttn-qr').src = '/api/ttn/qr.svg';
    $('#ttn-on').checked = ttnOn;
    const fw = $('#fw');
    fw.replaceChildren();
    for (const k of ['version', 'slot', 'state', 'last_check', 'wifi', 'eth']) {
      const dt = document.createElement('dt');
      dt.textContent = LABELS[k];
      const dd = document.createElement('dd');
      dd.textContent = o[k] || '–';
      fw.append(dt, dd);
    }
    const groups = $('#groups');
    groups.replaceChildren();
    for (const [g, vals] of Object.entries(all)) {
      const h = document.createElement('h3');
      h.textContent = LABELS[g] || g;
      const dl = document.createElement('dl');
      for (const [k, v] of Object.entries(vals)) {
        const dt = document.createElement('dt');
        dt.textContent = g === 'stack_free' ? k : (LABELS[k] || k);
        const dd = document.createElement('dd');
        dd.textContent = fmt(k, v);
        dl.append(dt, dd);
      }
      groups.append(h, dl);
    }
  } catch (e) { /* shown on next login */ }
}

async function refreshLogs() {
  const pre = $('#logs');
  try {
    let lines = (await (await api('/api/logs' + ($('#prev').checked ? '?prev=1' : ''))).text()).split('\n');
    if ($('#warn-only').checked) lines = lines.filter((l) => /^[WE] /.test(l));
    pre.replaceChildren(...lines.map((l) => {
      const span = document.createElement('span');
      span.className = /^[WE] /.test(l) ? l[0] : '';
      span.textContent = l + '\n';
      return span;
    }));
    if (!lines.join('').trim()) pre.textContent = '(empty)';
    pre.scrollTop = pre.scrollHeight; /* newest at the bottom */
  } catch (e) { pre.textContent = String(e.message); }
}

async function action(path, method, msg) {
  const el = $('#fw-msg');
  el.classList.remove('bad');
  try {
    el.textContent = (await (await api(path, { method })).text()).trim() || msg;
  } catch (e) { el.textContent = e.message; el.classList.add('bad'); }
}

document.addEventListener('DOMContentLoaded', () => {
  $('#login-form').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const body = new URLSearchParams(new FormData(ev.target));
    const msg = $('#login-msg');
    msg.textContent = 'Checking…';
    msg.classList.remove('bad');
    try {
      const r = await fetch('/api/login', { method: 'POST', headers: { ...H, 'Content-Type': 'application/x-www-form-urlencoded' }, body });
      if (!r.ok) throw new Error((await r.text()).trim());
      ev.target.reset();
      msg.textContent = '';
      show(true, (await r.json()).role);
    } catch (e) { msg.textContent = e.message; msg.classList.add('bad'); }
  });
  $('#logout').addEventListener('click', async () => { try { await api('/api/logout', { method: 'POST' }); } catch (e) {} show(false); });
  for (const b of document.querySelectorAll('nav button')) {
    b.addEventListener('click', () => {
      for (const x of document.querySelectorAll('nav button')) x.classList.toggle('on', x === b);
      for (const t of document.querySelectorAll('.tab')) t.hidden = t.id !== 'tab-' + b.dataset.tab;
      if (b.dataset.tab === 'logs') refreshLogs();
      if (b.dataset.tab === 'status' || b.dataset.tab === 'firmware') refreshStatus();
    });
  }
  $('#logs-refresh').addEventListener('click', refreshLogs);
  $('#prev').addEventListener('change', refreshLogs);
  $('#warn-only').addEventListener('change', refreshLogs);
  $('#ota-check').addEventListener('click', () => { if (confirm('Download and install the latest release if it differs from this one? The gateway restarts.')) action('/api/ota/check', 'POST'); });
  $('#reboot').addEventListener('click', () => { if (confirm('Reboot the gateway?')) action('/api/reboot', 'POST'); });
  $('#ttn-copy').addEventListener('click', async (ev) => {
    try { await navigator.clipboard.writeText($('#ttn-eui').textContent); ev.target.textContent = 'Copied'; }
    catch (e) { getSelection().selectAllChildren($('#ttn-eui')); }
  });
  $('#ttn-save').addEventListener('click', async () => {
    if (!confirm('Save and restart the gateway?')) return;
    const el = $('#fw-msg');
    try {
      const r = await api('/api/settings', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: 'ttn=' + ($('#ttn-on').checked ? 'on' : 'off') });
      el.textContent = (await r.text()).trim();
    } catch (e) { el.textContent = e.message; }
  });
  $('#cd-del').addEventListener('click', () => { if (confirm('Erase the stored coredump?')) action('/api/coredump', 'DELETE'); });
  /* Claim: with no admin password yet, the first LAN visit sets it (ADR-0014). */
  fetch('/api/claim', { headers: H }).then((r) => (r.ok ? r.json() : null)).then((st) => {
    if (!st || st.admin_set) return;
    if (st.open) {
      $('#setup').hidden = false;
      $('#login').hidden = true; /* nothing to log in to yet */
    } else {
      $('#login-msg').textContent = 'No admin password yet and the claim window has closed: restart the gateway, or hold its button 5 s.';
    }
  }).catch(() => {});
  $('#setup-form').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const msg = $('#setup-msg');
    msg.classList.remove('bad');
    if (ev.target.admin.value !== $('#admin2').value) {
      msg.textContent = 'The admin passwords differ.';
      msg.classList.add('bad');
      return;
    }
    msg.textContent = 'Saving…';
    try {
      const r = await fetch('/api/claim', { method: 'POST', headers: { ...H, 'Content-Type': 'application/x-www-form-urlencoded' }, body: new URLSearchParams(new FormData(ev.target)) });
      const t = (await r.text()).trim();
      if (!r.ok) throw new Error(t);
      ev.target.reset();
      $('#setup').hidden = true;
      $('#login').hidden = false;
      $('#login-msg').textContent = t.includes('restarts') ? 'Admin password set. The gateway restarts; log in when it is back.' : 'Admin password set. Log in as admin.';
    } catch (e) { msg.textContent = e.message; msg.classList.add('bad'); }
  });
  const role = sessionStorage.getItem('role');
  if (role) { show(true, role); } else { show(false); }
  setInterval(() => { if (!$('#app').hidden && $('#tab-logs').hidden) refreshStatus(); }, 10000);
});
