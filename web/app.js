// RoastSim web control panel. Talks to the simulator over the same WebSocket
// JSON API as every other front end (docs/API.md).
'use strict';

const $ = (id) => document.getElementById(id);
const COLS = ['t', 't_roast', 'bt', 'et', 'ror_bt', 'ror_et', 'bean_true', 'heat', 'fan', 'drum'];

let ws = null, reqId = 0;
const pending = new Map();
let state = null, def = null;
let history = [];          // rows: arrays in COLS order
let events = [];
let lastPhase = null;
let dragging = new Set();
let lastPops = { fc: 0, sc: 0 };
let audio = null;

// ---------------------------------------------------------------- transport
function connect() {
  const url = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws';
  ws = new WebSocket(url);
  ws.onopen = async () => {
    setPill('conn', true, 'online');
    history = []; events = [];
    await loadLists();
    await refreshDef();
    send({ cmd: 'subscribe', history_from: 0, event_seq: 0 });
  };
  ws.onclose = () => { setPill('conn', false, 'offline'); setTimeout(connect, 1500); };
  ws.onmessage = (m) => {
    let msg;
    try { msg = JSON.parse(m.data); } catch { return; }
    if (msg.req !== undefined && pending.has(msg.req)) { pending.get(msg.req)(msg); pending.delete(msg.req); return; }
    if (msg.type === 'state') onState(msg);
    else if (msg.type === 'event') onEvent(msg);
  };
}

function send(cmd) {
  return new Promise((resolve) => {
    if (!ws || ws.readyState !== 1) return resolve({ ok: false, error: 'not connected' });
    const req = ++reqId;
    pending.set(req, resolve);
    ws.send(JSON.stringify({ ...cmd, req, origin: 'web' }));
  });
}

async function act(cmd) {
  const r = await send(cmd);
  if (r && r.ok === false) alert(r.error || 'failed');
  return r;
}

function setPill(id, on, text) { const el = $(id); el.className = 'pill ' + (on ? 'on' : 'off'); el.textContent = text; }

// ---------------------------------------------------------------- setup
async function loadLists() {
  const l = await send({ cmd: 'list' });
  const fill = (sel, arr) => { sel.innerHTML = ''; for (const x of arr) sel.add(new Option(x.name, x.id)); };
  fill($('roaster'), l.roasters || []);
  fill($('bean'), l.beans || []);
}

async function refreshDef() {
  const s = await send({ cmd: 'state', full: true });
  def = s.roaster_def;
  $('roaster').value = s.roaster.id;
  $('bean').value = s.bean.id;
  $('desc').textContent = def.description || '';
  $('batch').value = s.next_batch_g;
  $('batch').min = def.batch.min_g; $('batch').max = def.batch.max_g;
  buildSliders();
  const fp = $('faultProbe'); fp.innerHTML = '';
  for (const p of def.probes) fp.add(new Option(p.label + ' (' + p.id + ')', p.id));
  for (const k of Object.keys(s.env)) { const el = $('env_' + k); if (el) el.value = s.env[k]; }
}

function buildSliders() {
  const box = $('sliders'); box.innerHTML = '';
  for (const c of def.controls) {
    if (c.toggle) {
      const row = document.createElement('label'); row.className = 'check';
      row.innerHTML = `<input type="checkbox" data-ctl="${c.id}"> ${c.label}`;
      row.querySelector('input').onchange = (e) => send({ cmd: 'set', control: c.id, value: e.target.checked ? 100 : 0 });
      box.appendChild(row); continue;
    }
    const row = document.createElement('div'); row.className = 'slider';
    row.innerHTML = `<span>${c.label}</span><input type="range" min="${c.min}" max="${c.max}" step="${c.step}" data-ctl="${c.id}"><output>0%</output>`;
    const input = row.querySelector('input'), out = row.querySelector('output');
    input.oninput = () => { dragging.add(c.id); out.textContent = input.value + '%'; send({ cmd: 'set', control: c.id, value: +input.value }); };
    input.onchange = () => dragging.delete(c.id);
    box.appendChild(row);
  }
}

// ---------------------------------------------------------------- state
function fmtTime(s) {
  if (s === null || s === undefined || s < 0) return '--:--';
  const t = Math.floor(s); return String(Math.floor(t / 60)).padStart(2, '0') + ':' + String(t % 60).padStart(2, '0');
}

function onState(s) {
  // On CHARGE the server keeps one minute of pre-charge history re-timed to
  // negative roast time; refetch it so the graph shows the drop into TP.
  if (lastPhase !== null && lastPhase !== 'roasting' && s.phase === 'roasting') {
    lastPhase = s.phase;
    history = [];
    send({ cmd: 'subscribe', history_from: 0, event_seq: s.event_seq });
    return;
  }
  lastPhase = s.phase;
  if (s.history) {
    for (const r of s.history) history.push(r);
    if (history.length > 20000) history.splice(0, history.length - 20000);
  }
  state = s;
  $('bt').textContent = s.bt.toFixed(1);
  $('et').textContent = s.et.toFixed(1);
  $('ror').textContent = s.t_roast > 0 ? s.ror_bt.toFixed(1) : '--';
  $('time').textContent = fmtTime(s.t_roast);
  $('phase').textContent = s.phase;
  setPill('logger', s.logger_connected, s.logger_connected ? 'logger connected' : 'no logger');
  for (const el of document.querySelectorAll('[data-ctl]')) {
    const c = s.controls[el.dataset.ctl]; if (!c) continue;
    if (el.type === 'checkbox') el.checked = c.value >= 50;
    else if (!dragging.has(el.dataset.ctl) && document.activeElement !== el) {
      el.value = c.value;
      el.nextElementSibling.textContent = Math.abs(c.actual - c.value) > 0.5 ? `${c.actual.toFixed(0)}→${c.value.toFixed(0)}` : c.value.toFixed(0) + '%';
    }
  }
  $('speed').value = s.paused ? '0' : String(s.time_scale);
  $('alarms').innerHTML = s.alarms.map(a => `<span>${a.replace(/_/g, ' ')}</span>`).join('');
  renderTables(s);
  pops(s);
  drawChart();
}

function onEvent(e) {
  if (e.event === 'CHARGE') { events = []; }
  events.push(e);
  const li = document.createElement('li');
  li.textContent = `${fmtTime(e.t_roast)}  ${e.event}${e.detail ? ' ' + e.detail : ''}  (BT ${e.bt.toFixed(1)}, ${e.origin})`;
  const ol = $('events'); ol.appendChild(li); ol.scrollTop = ol.scrollHeight;
  if (e.event === 'COOL') showReport();
}

function renderTables(s) {
  const b = s.beans, m = s.machine;
  const rows = (pairs) => pairs.map(([k, v]) => `<tr><td>${k}</td><td>${v}</td></tr>`).join('');
  $('beanTable').innerHTML = b.present ? rows([
    ['Mean / surface / core', `${b.T_mean} / ${b.T_surface} / ${b.T_core} °C`],
    ['Spread (min–max)', `${b.T_min}–${b.T_max} °C`],
    ['Moisture', b.moisture_pct.toFixed(2) + ' %'],
    ['Weight loss', b.weight_loss_pct.toFixed(1) + ' %'],
    ['Agtron whole / ground', `${b.agtron_whole} / ${b.agtron_ground} (±${b.agtron_sd})`],
    ['Level', b.level],
    ['First / second crack', `${(b.fc_frac * 100).toFixed(0)} % / ${(b.sc_frac * 100).toFixed(0)} %`],
    ['Expansion', '×' + b.expansion.toFixed(2)],
  ]) : rows([['Beans', 'none — set a batch size and Charge']]);
  $('machineTable').innerHTML = rows([
    ['Heater', m.heater_W + ' W' + (m.voltage_V ? ` @ ${m.voltage_V} V` : '')],
    ['Airflow', m.airflow_g_s + ' g/s'],
    ['Inlet / exhaust', `${m.inlet_C} / ${m.exhaust_C} °C`],
    ['Wall / element', `${m.wall_C} / ${m.element_C} °C`],
    ['Bed', `${m.bed_state} (agitation ${m.agitation}, mixing ${(m.mix_quality * 100).toFixed(0)} %)`],
    ['Smoke', (s.smoke * 100).toFixed(0) + ' %'],
  ]);
  drawBeans();
}

// ---------------------------------------------------------------- sounds
function pops(s) {
  const fc = s.pops_fc - lastPops.fc, sc = s.pops_sc - lastPops.sc;
  lastPops = { fc: s.pops_fc, sc: s.pops_sc };
  if (!$('sound').checked || (fc <= 0 && sc <= 0)) return;
  if (!audio) { try { audio = new AudioContext(); } catch { return; } }
  const n = Math.min(6, Math.max(0, fc) + Math.max(0, sc));
  for (let i = 0; i < n; i++) click(i < fc ? 'fc' : 'sc', Math.random() * 0.24);
}

function click(kind, delay) {
  const ctx = audio, len = Math.floor(ctx.sampleRate * (kind === 'fc' ? 0.025 : 0.012));
  const buf = ctx.createBuffer(1, len, ctx.sampleRate), d = buf.getChannelData(0);
  let prev = 0;
  for (let i = 0; i < len; i++) {
    const n = Math.random() * 2 - 1, env = Math.exp(-i / (len * 0.22));
    d[i] = (kind === 'fc' ? (n + prev) / 2 : (n - prev) / 2) * env * (kind === 'fc' ? 0.8 : 0.45);
    prev = n;
  }
  const src = ctx.createBufferSource(); src.buffer = buf; src.connect(ctx.destination);
  src.start(ctx.currentTime + delay);
}

// ---------------------------------------------------------------- drawing
function css(name) { return getComputedStyle(document.documentElement).getPropertyValue(name).trim(); }

function drawChart() {
  const cv = $('chart'), dpr = window.devicePixelRatio || 1;
  const W = cv.clientWidth, H = cv.clientHeight;
  if (cv.width !== W * dpr || cv.height !== H * dpr) { cv.width = W * dpr; cv.height = H * dpr; }
  const g = cv.getContext('2d'); g.setTransform(dpr, 0, 0, dpr, 0, 0); g.clearRect(0, 0, W, H);
  if (!state) return;
  const L = 44, R = W - 40, T = 12, B = H - 24;
  const roast = state.t_roast >= 0;
  const xi = roast ? 1 : 0;
  let x0, x1;
  if (roast) { x0 = -60; x1 = Math.max(720, Math.ceil((state.t_roast + 60) / 120) * 120); }
  else { x1 = Math.max(60, state.t); x0 = Math.max(0, x1 - 600); }
  let tMax = 250;
  for (const r of history) if (r[xi] >= x0) tMax = Math.max(tMax, r[2] + 10, r[3] + 10);
  tMax = Math.ceil(tMax / 50) * 50;
  const rorMax = 50;
  const X = (x) => L + (x - x0) / (x1 - x0) * (R - L);
  const Y = (v) => B - v / tMax * (B - T);
  const YR = (v) => B - Math.max(-5, Math.min(rorMax, v)) / rorMax * (B - T);
  g.font = '11px system-ui'; g.fillStyle = css('--muted'); g.strokeStyle = css('--line'); g.lineWidth = 1;
  for (let v = 0; v <= tMax; v += 50) { g.beginPath(); g.moveTo(L, Y(v)); g.lineTo(R, Y(v)); g.stroke(); g.fillText(v, 8, Y(v) + 4); }
  for (let v = 0; v <= rorMax; v += 10) g.fillText(v, R + 6, YR(v) + 4);
  const step = (x1 - x0) > 900 ? 120 : 60;
  for (let x = Math.ceil(x0 / step) * step; x <= x1; x += step) {
    g.beginPath(); g.moveTo(X(x), T); g.lineTo(X(x), B); g.stroke(); g.fillText(fmtTime(x), X(x) - 14, H - 6);
  }
  // heat % band
  g.fillStyle = 'rgba(224,150,42,0.18)';
  for (const r of history) { if (r[xi] < x0) continue; const h = r[7] / 100 * (B - T) * 0.18; g.fillRect(X(r[xi]), B - h, Math.max(1, (R - L) / (x1 - x0)), h); }
  const line = (col, idx, color, width, ror, filter) => {
    g.strokeStyle = color; g.lineWidth = width; g.beginPath(); let started = false;
    for (const r of history) {
      const x = r[xi]; if (x < x0 || x > x1 || (filter && !filter(r))) { started = false; continue; }
      if (!ror && r[idx] <= -0.5) { started = false; continue; }
      const y = ror ? YR(r[idx]) : Y(r[idx]);
      if (!started) { g.moveTo(X(x), y); started = true; } else g.lineTo(X(x), y);
    }
    g.stroke();
  };
  line('et', 3, css('--et'), 2, false);
  line('bt', 2, css('--bt'), 2, false);
  if (roast) {
    line('true', 6, css('--true'), 1, false, r => r[1] >= 0);
    line('ror', 4, css('--ror'), 1.5, true, r => r[1] > 20);
  }
  g.fillStyle = css('--ink'); g.strokeStyle = css('--muted'); g.setLineDash([3, 3]);
  let row = 0;
  for (const e of events) {
    if (['ALARM', 'NOTE'].includes(e.event)) continue;
    const x = roast ? e.t_roast : e.t; if (x < x0 || x > x1) continue;
    g.beginPath(); g.moveTo(X(x), T); g.lineTo(X(x), B); g.stroke();
    g.fillText(e.event, X(x) + 3, T + 12 + (row++ % 2) * 13);
  }
  g.setLineDash([]);
  // legend
  const leg = [['BT', '--bt'], ['ET', '--et'], ['RoR', '--ror'], ['true bean', '--true']];
  const legW = leg.reduce((w, [n]) => w + 22 + g.measureText(n).width, 0);
  let lx = R - legW - 4;
  g.fillStyle = css('--panel'); g.globalAlpha = 0.85; g.fillRect(lx - 6, B - 24, legW + 8, 18); g.globalAlpha = 1;
  for (const [n, c] of leg) { g.fillStyle = css(c); g.fillRect(lx, B - 16, 10, 3); g.fillStyle = css('--muted'); g.fillText(n, lx + 14, B - 11); lx += 22 + g.measureText(n).width; }
}

let beanSeeds = null;
async function drawBeans() {
  const v = await send({ cmd: 'visual', max: 300 });
  const cv = $('beans'), dpr = window.devicePixelRatio || 1;
  const W = cv.clientWidth, H = cv.clientHeight;
  if (cv.width !== W * dpr) { cv.width = W * dpr; cv.height = H * dpr; }
  const g = cv.getContext('2d'); g.setTransform(dpr, 0, 0, dpr, 0, 0); g.clearRect(0, 0, W, H);
  if (!v.ok || !v.beans || v.beans.length === 0) { g.fillStyle = css('--muted'); g.fillText('No beans', 10, 20); return; }
  if (!beanSeeds || beanSeeds.length !== v.beans.length) beanSeeds = v.beans.map(() => [Math.random(), Math.random(), Math.random() * 6.28]);
  const t = performance.now() / 1000, agit = Math.min(2, v.agitation) * (v.bean_location === 1 ? 1 : 0.1);
  v.beans.forEach((b, i) => {
    const [sx, sy, ph] = beanSeeds[i];
    const x = 8 + (sx + Math.sin(t * 3.1 + ph) * 0.02 * agit) * (W - 16);
    const y = 8 + (sy + Math.cos(t * 2.3 + ph) * 0.05 * agit) * (H - 16);
    const r = b[3] * 0.7;
    g.fillStyle = `rgb(${b[0] * 255 | 0},${b[1] * 255 | 0},${b[2] * 255 | 0})`;
    g.beginPath(); g.ellipse(x, y, r, r * 0.68, ph, 0, 6.283); g.fill();
    if (b[4] > 0.2) { g.fillStyle = 'rgba(255,245,230,0.6)'; g.beginPath(); g.ellipse(x - r * 0.25, y - r * 0.2, r * 0.3, r * 0.15, ph, 0, 6.283); g.fill(); }
  });
}

// ---------------------------------------------------------------- report
async function showReport() {
  const r = await send({ cmd: 'report' });
  if (!r.valid) { alert('No roast yet. Charge some beans first.'); return; }
  $('reportBody').innerHTML = `
    <p><b>${r.roaster}</b><br>${r.bean}</p>
    <table>
      <tr><td>Batch → out</td><td>${r.batch_g} g → ${r.out_g} g (${r.weight_loss_pct}% loss)</td></tr>
      <tr><td>Moisture</td><td>${r.moisture_pct}%</td></tr>
      <tr><td>TP / DRY / FCs / DROP</td><td>${fmtTime(r.tp_s)} / ${fmtTime(r.dry_s)} / ${fmtTime(r.fcs_s)} / ${fmtTime(r.drop_s)}</td></tr>
      <tr><td>Development</td><td>${r.dev_time_s} s (DTR ${r.dtr_pct}%)</td></tr>
      <tr><td>Agtron whole / ground</td><td>${r.agtron_whole} / ${r.agtron_ground} (±${r.agtron_sd})</td></tr>
      <tr><td>Roast level</td><td>${r.roast_level}</td></tr>
      <tr><td>RoR max / FC / drop</td><td>${r.max_ror} / ${r.ror_at_fcs} / ${r.ror_at_drop} °C/min</td></tr>
    </table>
    <ul>${r.notes.map(n => `<li>${n}</li>`).join('')}</ul>`;
  $('reportDlg').showModal();
}

// ---------------------------------------------------------------- wiring
$('load').onclick = async () => {
  if (state && state.bean_location === 'machine' && !confirm('Beans are roasting. Discard them?')) return;
  await act({ cmd: 'load', roaster: $('roaster').value, bean: $('bean').value });
  history = []; events = []; $('events').innerHTML = '';
  await refreshDef();
  send({ cmd: 'subscribe', history_from: 0, event_seq: 0 });
};
$('bean').onchange = () => act({ cmd: 'bean', id: $('bean').value });
$('charge').onclick = () => act({ cmd: 'charge', batch_g: +$('batch').value });
$('drop').onclick = () => act({ cmd: 'drop' });
$('preheat').onclick = async () => { await act({ cmd: 'preheat', target_C: +$('preheatT').value }); history = []; send({ cmd: 'subscribe', history_from: 0 }); };
$('reset').onclick = async () => { if (confirm('Cool the machine and discard beans?')) { await act({ cmd: 'reset' }); history = []; events = []; $('events').innerHTML = ''; send({ cmd: 'subscribe', history_from: 0 }); } };
$('repair').onclick = () => act({ cmd: 'repair' });
$('reportBtn').onclick = showReport;
$('speed').onchange = async (e) => {
  const v = +e.target.value;
  if (v === 0) await act({ cmd: 'pause', value: true });
  else { await act({ cmd: 'pause', value: false }); await act({ cmd: 'time_scale', value: v }); }
};
$('envApply').onclick = () => {
  const cmd = { cmd: 'env' };
  for (const k of ['ambient_C', 'humidity_rh', 'line_voltage', 'source_ohms', 'altitude_m', 'gas_pressure']) cmd[k] = +$('env_' + k).value;
  act(cmd);
};
$('faultApply').onclick = () => act({ cmd: 'fault', probe: $('faultProbe').value, type: $('faultType').value });
for (const b of document.querySelectorAll('[data-mark]')) b.onclick = () => act({ cmd: 'mark', event: b.dataset.mark });
window.addEventListener('resize', drawChart);
document.addEventListener('click', () => { if (audio && audio.state === 'suspended') audio.resume(); });

connect();
