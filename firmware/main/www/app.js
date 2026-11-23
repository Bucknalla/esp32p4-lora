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

const MODES = { ttn: 'The Things Network', hybrid: 'Hybrid', standalone: 'Standalone' };

function ago(s) {
  if (s < 0) return 'not since boot';
  if (s < 90) return s + ' s ago';
  if (s < 5400) return Math.round(s / 60) + ' min ago';
  return Math.round(s / 3600) + ' h ago';
}

const MHZ = (hz) => (hz / 1e6).toFixed(3) + ' MHz';
const PLAN_SRC = { default: 'Built-in EU868 (TTN)', local: 'Set on this page', server: 'From the network server' };

async function refreshRadio() {
  try {
    const p = await (await api('/api/radio')).json();
    $('#radio-src').textContent = PLAN_SRC[p.source] || p.source;
    $('#radio-centres').textContent = 'radio 0 ' + MHZ(p.radios[0]) + ' · radio 1 ' + MHZ(p.radios[1]);
    const cell = (text, cls) => { const td = document.createElement('td'); td.textContent = text; if (cls) td.className = cls; return td; };
    const row = (name, c, mod) => {
      const tr = document.createElement('tr');
      if (!c.enable) tr.className = 'off';
      tr.append(cell(name), cell(c.enable ? MHZ(c.hz) : 'off', 'mono'), cell(String(c.radio) + (c.if ? ' (' + (c.if > 0 ? '+' : '') + c.if / 1000 + ' kHz)' : '')), cell(mod));
      return tr;
    };
    $('#radio-tx').textContent = (p.tx ? 'on' : 'off') + ' · local server ' + (p.lns_tx ? 'on' : 'off (dry run)')
      + ' · cap ' + p.cap_dbm + ' dBm at the connector (ceiling ' + p.ceiling_dbm + ', antenna ' + (p.gain_db10 / 10).toFixed(1) + ' dBi)'
      + (p.tx_built ? '' : ' · transmit not built in');
    $('#radio-rx').textContent = 'RX1 ' + p.rx1_delay_s + ' s · RX2 ' + MHZ(p.rx2_hz) + ' DR' + p.rx2_dr + ' (new joins)';
    if (!$('#rx-form').dataset.dirty) { $('#rx-d').value = p.rx1_delay_s; $('#rx-f').value = (p.rx2_hz / 1e6).toFixed(3); $('#rx-dr').value = String(p.rx2_dr); }
    const f = $('#tx-form');
    if (!f.dataset.dirty) { $('#tx-gain').value = (p.gain_db10 / 10).toFixed(1); $('#tx-on').checked = p.tx; $('#tx-lns').checked = p.lns_tx; }
    if (!$('#plan-form').dataset.dirty) fillPlanEditor(p);
    $('#radio-rows').replaceChildren(
      ...p.multisf.map((c, i) => row('Multi-SF ' + i, c, 'LoRa SF7–12, 125 kHz')),
      row('LoRa std', p.std, 'LoRa SF' + p.std_sf + ', ' + p.std_bw_khz + ' kHz'),
      row('FSK', p.fsk, 'FSK 50 kbit/s'),
    );
  } catch (e) { $('#radio-src').textContent = e.message; }
}

/* Channel plan editor: one row per channel (multi-SF 0-7, std, FSK). */
const PE_CHANS = ['m0', 'm1', 'm2', 'm3', 'm4', 'm5', 'm6', 'm7', 'std', 'fsk'];
function fillPlanEditor(p) {
  $('#pe-r0').value = (p.radios[0] / 1e6).toFixed(1);
  $('#pe-r1').value = (p.radios[1] / 1e6).toFixed(1);
  $('#pe-sf').value = String(p.std_sf);
  $('#pe-bw').value = String(p.std_bw_khz);
  const chans = [...p.multisf, p.std, p.fsk];
  $('#pe-rows').replaceChildren(...chans.map((c, i) => {
    const tr = document.createElement('tr');
    tr.dataset.k = PE_CHANS[i];
    const td = (el) => { const d = document.createElement('td'); d.append(el); return d; };
    const name = document.createElement('span');
    name.textContent = i < 8 ? 'Multi-SF ' + i : i === 8 ? 'LoRa std' : 'FSK';
    const on = document.createElement('input');
    on.type = 'checkbox'; on.checked = c.enable; on.className = 'pe-on';
    const radio = document.createElement('select');
    radio.className = 'pe-radio';
    for (const r of ['0', '1']) { const o = document.createElement('option'); o.value = o.textContent = r; radio.append(o); }
    radio.value = String(c.radio);
    const ifk = document.createElement('input');
    ifk.type = 'number'; ifk.step = '25'; ifk.min = '-500'; ifk.max = '500'; ifk.className = 'pe-if';
    ifk.value = String(c.if / 1000);
    const f = document.createElement('span');
    f.className = 'mono pe-hz';
    tr.append(td(name), td(on), td(radio), td(ifk), td(f));
    return tr;
  }));
  updatePlanFreqs();
}
function updatePlanFreqs() {
  const r = [parseFloat($('#pe-r0').value) * 1e6, parseFloat($('#pe-r1').value) * 1e6];
  for (const tr of document.querySelectorAll('#pe-rows tr')) {
    const on = tr.querySelector('.pe-on').checked;
    const hz = r[+tr.querySelector('.pe-radio').value] + parseFloat(tr.querySelector('.pe-if').value) * 1000;
    tr.querySelector('.pe-hz').textContent = on && isFinite(hz) ? MHZ(hz) : 'off';
  }
}
function planBody() {
  const b = new URLSearchParams();
  b.set('r0', String(Math.round(parseFloat($('#pe-r0').value) * 1e6)));
  b.set('r1', String(Math.round(parseFloat($('#pe-r1').value) * 1e6)));
  for (const tr of document.querySelectorAll('#pe-rows tr')) {
    const v = [tr.querySelector('.pe-on').checked ? 1 : 0, tr.querySelector('.pe-radio').value,
      Math.round(parseFloat(tr.querySelector('.pe-if').value) * 1000)];
    if (tr.dataset.k === 'std') v.push($('#pe-sf').value, $('#pe-bw').value);
    b.set(tr.dataset.k, v.join(':'));
  }
  return b;
}

async function refreshDevices() {
  try {
    const list = await (await api('/api/devices')).json();
    const cell = (text, cls) => { const td = document.createElement('td'); td.textContent = text; if (cls) td.className = cls; return td; };
    const rows = list.map((d) => {
      const tr = document.createElement('tr');
      const who = cell(d.name || d.dev_eui);
      const sub = document.createElement('div');
      sub.className = 'hint mono';
      sub.textContent = d.dev_eui + ' · ' + d.mac;
      who.append(sub);
      const del = document.createElement('button');
      del.type = 'button';
      del.textContent = 'Remove';
      del.addEventListener('click', async () => {
        if (!confirm('Remove ' + (d.name || d.dev_eui) + '? It must join again after you add it back.')) return;
        try {
          await api('/api/devices/delete', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: 'dev_eui=' + d.dev_eui });
          refreshDevices();
        } catch (e) { $('#cfg-msg').textContent = e.message; }
      });
      const act = cell('', 'admin');
      act.append(del);
      tr.append(
        who,
        cell(!d.answered ? 'Join request seen, not answered (transmit not approved)'
          : d.joined ? 'Joined · ' + d.dev_addr + ' · FCnt ' + d.fcnt_up : 'Not joined'),
        cell(ago(d.seen_s) + (d.uplinks ? ' · ' + d.uplinks + ' up' : '')),
        cell((d.seen_s >= 0 ? d.rssi + ' dBm · ' + d.snr + ' dB · SF' + d.sf : '–')
          + (d.tx_power > 0 ? ' · power −' + (2 * d.tx_power) + ' dB (ADR)' : '')
          + (d.adr === 'pending' ? ' · ADR change sent' : '')
          + (d.battery >= 0 ? ' · battery ' + (d.battery === 0 ? 'external' : d.battery === 255 ? 'unknown' : Math.round(d.battery * 100 / 254) + '%') + ' · margin ' + d.margin + ' dB' : '')),
        cell(d.port >= 0 ? 'port ' + d.port + ': ' + (d.payload || '(empty)') : '–', 'mono'),
        act,
      );
      return tr;
    });
    if (!rows.length) {
      const tr = document.createElement('tr');
      const td = cell('No devices yet.');
      td.colSpan = 6;
      tr.append(td);
      rows.push(tr);
    }
    $('#dev-rows').replaceChildren(...rows);
  } catch (e) { /* the status refresh reports errors */ }
}

async function refreshStatus() {
  try {
    const s = await (await api('/api/status')).json();
    const all = {};
    for (const [g, line] of Object.entries(s)) all[g] = Object.fromEntries(kv(line));
    const o = all.ota || {}, r = all.radio || {}, f = all.faults || {}, g = all.ttn || {};
    const ttnOn = g.report !== 'off';
    const ln = all.lns || {};
    const mode = g.mode || (ttnOn ? 'ttn' : 'standalone');
    const rx = +r.rx || 0, ok = +r.crc_ok || 0;
    const restarts = +r.restarts || 0, run = +f.run || 0;
    const notices = [];
    if (ttnOn && g.lns_key === 'none') notices.push(['warn', 'No TTN API key: the gateway cannot connect to TTN. Add one under Config.']);
    else if (ttnOn && r.upstream !== 'up') notices.push(['warn', 'Upstream (TTN) is down: uplinks are not forwarded. Check the registration and API key under Config.']);
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
      ttnOn ? tile('Upstream', r.upstream === 'up' ? 'Connected' : 'Down', 'TTN, Basics Station', r.upstream === 'up' ? 'ok' : 'warn')
            : tile('Upstream', 'Off', 'Standalone mode'),
      ...(mode !== 'ttn' ? [tile('Local devices', (ln.joined || 0) + ' / ' + (ln.devices || 0), 'joined, ' + (ln.uplinks || 0) + ' uplinks since boot')] : []),
      tile('Packets', String(rx), ok + ' with valid CRC'),
      tile('Uptime', fmtUptime(r.uptime_s || 0), (f.boots || '?') + ' boots, ' + (f.panic || 0) + ' panics'),
      tile('Internal RAM', fmtKB(r.heap_internal || 0), 'lowest ' + fmtKB(r.heap_internal_min || 0)),
    );
    $('#ttn').hidden = !ttnOn;
    $('#mode-state').textContent = MODES[mode] + (mode !== 'ttn' && ln.tx === 'dry-run' ? ' (transmit not approved: joins are logged, not answered; in hybrid they still go to TTN)' : '');
    if (!$('#mode-pick').dataset.dirty) for (const x of document.querySelectorAll('#mode-pick input')) x.checked = x.value === mode;
    $('#dev-note').textContent = mode === 'ttn'
      ? 'These devices are used in hybrid and standalone modes. You can add them now and change the mode later.'
      : 'Up to 32 OTAA devices. EU868, class A.';
    if (!$('#tab-config').hidden) { refreshDevices(); refreshRadio(); }
    $('#ttn-eui').textContent = g.eui || '';
    $('#ttn-server').textContent = g.server ? 'wss://' + g.server + (g.port ? ':' + g.port : '') : '';
    $('#ttn-key').textContent = g.lns_key === 'set' ? 'Stored' : 'Not set';
    if (!$('#lns-host').value && g.server) $('#lns-host').value = g.server;
    if (ttnOn && !$('#ttn-qr').src) $('#ttn-qr').src = '/api/ttn/qr.svg';
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
      if (b.dataset.tab !== 'logs') refreshStatus();
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
  $('#mode-pick').addEventListener('change', (ev) => { ev.currentTarget.dataset.dirty = '1'; });
  $('#mode-save').addEventListener('click', async () => {
    const m = document.querySelector('#mode-pick input:checked');
    if (!m || !confirm('Change the mode to ' + MODES[m.value] + ' and restart the gateway?')) return;
    const el = $('#cfg-msg');
    try {
      const r = await api('/api/settings', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: 'mode=' + m.value });
      el.textContent = (await r.text()).trim();
    } catch (e) { el.textContent = e.message; }
  });
  $('#plan-form').addEventListener('input', (ev) => { ev.currentTarget.dataset.dirty = '1'; updatePlanFreqs(); });
  const savePlan = async (body, what) => {
    if (!confirm(what + ' The radio restarts (about 3 s); local devices must join again.')) return;
    const el = $('#cfg-msg');
    el.classList.remove('bad');
    try {
      const r = await api('/api/radio/plan', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body });
      el.textContent = (await r.text()).trim();
      delete $('#plan-form').dataset.dirty;
      refreshRadio();
    } catch (e) { el.textContent = e.message; el.classList.add('bad'); }
  };
  $('#plan-form').addEventListener('submit', (ev) => { ev.preventDefault(); savePlan(planBody(), 'Save this channel plan?'); });
  $('#pe-reset').addEventListener('click', () => savePlan(new URLSearchParams({ reset: '1' }), 'Go back to the built-in EU868 plan?'));
  $('#rx-form').addEventListener('input', (ev) => { ev.currentTarget.dataset.dirty = '1'; });
  $('#rx-form').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const body = new URLSearchParams({ rx1_delay: $('#rx-d').value, rx2_hz: String(Math.round(parseFloat($('#rx-f').value) * 1e6)), rx2_dr: $('#rx-dr').value });
    if (!confirm('Save the receive windows? They apply to devices that join from now on.')) return;
    const el = $('#cfg-msg');
    el.classList.remove('bad');
    try {
      const r = await api('/api/radio', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body });
      el.textContent = (await r.text()).trim();
      delete ev.target.dataset.dirty;
      refreshRadio();
    } catch (e) { el.textContent = e.message; el.classList.add('bad'); }
  });
  $('#tx-form').addEventListener('input', (ev) => { ev.currentTarget.dataset.dirty = '1'; });
  $('#tx-form').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const body = new URLSearchParams({ gain: $('#tx-gain').value, tx: $('#tx-on').checked ? 'on' : 'off', lns_tx: $('#tx-lns').checked ? 'on' : 'off' });
    if (!confirm('Save the transmit settings?')) return;
    const el = $('#cfg-msg');
    el.classList.remove('bad');
    try {
      const r = await api('/api/radio', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body });
      el.textContent = (await r.text()).trim();
      delete ev.target.dataset.dirty;
      refreshRadio();
    } catch (e) { el.textContent = e.message; el.classList.add('bad'); }
  });
  $('#lns-form').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const fd = new FormData(ev.target);
    const body = new URLSearchParams();
    if (fd.get('lns_host')) body.set('lns_host', fd.get('lns_host'));
    if (fd.get('lns_key')) body.set('lns_key', fd.get('lns_key'));
    if (![...body.keys()].length || !confirm('Save the TTN connection settings and restart the gateway?')) return;
    const el = $('#cfg-msg');
    el.classList.remove('bad');
    try {
      const r = await api('/api/settings', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body });
      el.textContent = (await r.text()).trim();
      ev.target.lns_key.value = '';
    } catch (e) { el.textContent = e.message; el.classList.add('bad'); }
  });
  $('#dev-add').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const el = $('#cfg-msg');
    el.classList.remove('bad');
    try {
      const r = await api('/api/devices', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: new URLSearchParams(new FormData(ev.target)) });
      el.textContent = 'Device ' + (await r.text()).trim() + '.';
      ev.target.reset();
      refreshDevices();
    } catch (e) { el.textContent = e.message; el.classList.add('bad'); }
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
      $('#login-msg').textContent = 'Admin password set. Log in as admin.';
    } catch (e) { msg.textContent = e.message; msg.classList.add('bad'); }
  });
  const role = sessionStorage.getItem('role');
  if (role) { show(true, role); } else { show(false); }
  setInterval(() => { if (!$('#app').hidden && $('#tab-logs').hidden) refreshStatus(); }, 10000);
});
