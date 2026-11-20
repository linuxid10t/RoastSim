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
let snd = null;           // { ctx, gain, rate, next } while sound is on
let preheatC = 200;       // preheat target, kept in C

// ---------------------------------------------------------------- units
// The API always speaks C / grams / metres; this shared setting (also used for
// what Artisan receives, and saved by the simulator) only changes display/input.
let units = { temperature: 'C', weight: 'g', altitude: 'm' };
let unitKey = '';
const isF = () => units.temperature === 'F';
const T = (c) => isF() ? c * 1.8 + 32 : c;              // temperature
const fromT = (v) => isF() ? (v - 32) / 1.8 : v;
const R = (r) => isF() ? r * 1.8 : r;                    // rate of rise per minute
const TU = () => isF() ? '°F' : '°C';
const WF = { g: 1, kg: 1000, oz: 28.349523125, lb: 453.59237 };
const W = (g) => g / WF[units.weight];
const fromW = (v) => v * WF[units.weight];
const fmtW = (g) => W(g).toFixed(units.weight === 'g' ? 0 : units.weight === 'oz' ? 1 : 2);
const A = (m) => units.altitude === 'ft' ? m / 0.3048 : m;
const fromA = (v) => units.altitude === 'ft' ? v * 0.3048 : v;
const t1 = (c) => c === -1 ? 'open' : T(c).toFixed(1);   // probe reading (-1 = open thermocouple)

function applyUnits(s) {
  for (const el of document.querySelectorAll('.tu')) el.textContent = TU();
  $('batchUnit').textContent = units.weight;
  $('altUnit').textContent = units.altitude;
  $('uTemp').value = units.temperature; $('uWeight').value = units.weight; $('uAlt').value = units.altitude;
  $('batch').value = fmtW(s.next_batch_g);
  $('preheatT').value = T(preheatC).toFixed(0);
  $('env_ambient_C').value = T(s.env.ambient_C).toFixed(0);
  $('env_altitude_m').value = A(s.env.altitude_m).toFixed(0);
}

// ---------------------------------------------------------------- transport
function connect() {
  const url = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws';
  ws = new WebSocket(url);
  ws.binaryType = 'arraybuffer';
  ws.onopen = async () => {
    setPill('conn', true, 'online');
    history = []; events = [];
    await loadLists();
    await refreshDef();
    send({ cmd: 'subscribe', history_from: 0, event_seq: 0 });
    if (snd) send({ cmd: 'audio', value: true, rate: snd.rate });  // resume sound after a reconnect
  };
  ws.onclose = () => { setPill('conn', false, 'offline'); setTimeout(connect, 1500); };
  ws.onmessage = (m) => {
    if (m.data instanceof ArrayBuffer) { playPcm(m.data); return; }
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
  buildSliders();
  const fp = $('faultProbe'); fp.innerHTML = '';
  for (const p of def.probes) fp.add(new Option(p.label + ' (' + p.id + ')', p.id));
  for (const k of Object.keys(s.env)) { const el = $('env_' + k); if (el) el.value = s.env[k]; }
  if (s.units) units = s.units;
  unitKey = JSON.stringify(units);
  applyUnits(s);
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
  if (s.units && JSON.stringify(s.units) !== unitKey) {  // changed here or in another front end
    units = s.units;
    unitKey = JSON.stringify(units);
    applyUnits(s);
  }
  $('bt').textContent = t1(s.bt);
  $('et').textContent = t1(s.et);
  $('ror').textContent = s.t_roast > 0 ? R(s.ror_bt).toFixed(1) : '--';
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
  drawChart();
}

function onEvent(e) {
  if (e.event === 'CHARGE') { events = []; }
  events.push(e);
  const li = document.createElement('li');
  li.textContent = `${fmtTime(e.t_roast)}  ${e.event}${e.detail ? ' ' + e.detail : ''}  (BT ${t1(e.bt)}${TU()}, ${e.origin})`;
  const ol = $('events'); ol.appendChild(li); ol.scrollTop = ol.scrollHeight;
  if (e.event === 'COOL') showReport();
}

function renderTables(s) {
  const b = s.beans, m = s.machine;
  const rows = (pairs) => pairs.map(([k, v]) => `<tr><td>${k}</td><td>${v}</td></tr>`).join('');
  const f1 = (c) => T(c).toFixed(1), f0 = (c) => T(c).toFixed(0), tu = TU();
  $('beanTable').innerHTML = b.present ? rows([
    ['Mean / surface / core', `${f1(b.T_mean)} / ${f1(b.T_surface)} / ${f1(b.T_core)} ${tu}`],
    ['Spread (min–max)', `${f1(b.T_min)}–${f1(b.T_max)} ${tu}`],
    ['Weight', `${fmtW(b.mass_g)} ${units.weight}`],
    ['Moisture', b.moisture_pct.toFixed(2) + ' %'],
    ['Weight loss', b.weight_loss_pct.toFixed(1) + ' %'],
    ['Agtron whole / ground', `${b.agtron_whole} / ${b.agtron_ground} (±${b.agtron_sd})`],
    ['Level', b.level],
    ['First / second crack', `${(b.fc_frac * 100).toFixed(0)} % / ${(b.sc_frac * 100).toFixed(0)} %`],
    ['Expansion', '×' + b.expansion.toFixed(2)],
    ...(b.scorch_frac > 0.005 || b.tipping_frac > 0.005
      ? [['Scorched / tipped', `${(b.scorch_frac * 100).toFixed(0)} % / ${(b.tipping_frac * 100).toFixed(0)} %`]] : []),
  ]) : rows([['Beans', 'none — set a batch size and Charge']]);
  const inMachine = s.bean_location === 'machine';
  $('machineTable').innerHTML = rows([
    ['Bed', `${m.bed_state} (agitation ${m.agitation}, mixing ${(m.mix_quality * 100).toFixed(0)} %)`],
    ...(inMachine ? [['Fill / BT probe buried', `${(m.fill * 100).toFixed(0)} % / ${(m.bt_immersion * 100).toFixed(0)} %`]] : []),
    ...(m.drum_rpm > 0.1 ? [['Drum', m.drum_rpm.toFixed(0) + ' rpm']] : []),
    ...(m.stir_rpm > 0.1 ? [['Stir arm', m.stir_rpm.toFixed(1) + ' rpm']] : []),
    ['Heater', m.heater_W + ' W' + (m.voltage_V ? ` @ ${m.voltage_V} V` : '')],
    ['Airflow', m.airflow_g_s + ' g/s'],
    ['Inlet / exhaust', `${f0(m.inlet_C)} / ${f0(m.exhaust_C)} ${tu}`],
    ['Wall / element', `${f0(m.wall_C)} / ${f0(m.element_C)} ${tu}`],
    ['Smoke / chaff', `${(s.smoke * 100).toFixed(0)} % / ${s.chaff_g_s.toFixed(2)} g/s`],
  ]);
  drawBeans();
}

// ---------------------------------------------------------------- sound
// The simulator renders the roaster's sound (the same synthesizer the native
// app plays) and streams it as 16-bit mono PCM in binary WebSocket frames.
// Here we just schedule those chunks back to back with a small jitter buffer.
async function setSound(on) {
  if (on && !snd) {
    let ctx;
    try { ctx = new AudioContext({ sampleRate: 24000 }); } catch { ctx = new AudioContext(); }
    const gain = ctx.createGain();
    gain.gain.value = +$('volume').value / 100;
    gain.connect(ctx.destination);
    snd = { ctx, gain, rate: 24000, next: 0 };
    const r = await send({ cmd: 'audio', value: true, rate: snd.rate });
    if (!r.ok) { alert(r.error || 'sound unavailable'); setSound(false); $('sound').checked = false; }
  } else if (!on && snd) {
    send({ cmd: 'audio', value: false });
    snd.ctx.close();
    snd = null;
  }
}

function playPcm(data) {
  if (!snd) return;
  const pcm = new Int16Array(data);
  const buf = snd.ctx.createBuffer(1, pcm.length, snd.rate);
  const ch = buf.getChannelData(0);
  for (let i = 0; i < pcm.length; i++) ch[i] = pcm[i] / 32768;
  const now = snd.ctx.currentTime;
  // Keep ~0.1-0.3 s of audio queued: drop a chunk if we are too far ahead
  // (keeps cracks in sync with the screen), restart the queue after a gap.
  if (snd.next > now + 0.3) return;
  if (snd.next < now + 0.02) snd.next = now + 0.08;
  const src = snd.ctx.createBufferSource();
  src.buffer = buf;
  src.connect(snd.gain);
  src.start(snd.next);
  snd.next += buf.duration;
}

// ---------------------------------------------------------------- drawing
function css(name) { return getComputedStyle(document.documentElement).getPropertyValue(name).trim(); }

function drawChart() {
  const cv = $('chart'), dpr = window.devicePixelRatio || 1;
  const W = cv.clientWidth, H = cv.clientHeight;
  if (cv.width !== W * dpr || cv.height !== H * dpr) { cv.width = W * dpr; cv.height = H * dpr; }
  const g = cv.getContext('2d'); g.setTransform(dpr, 0, 0, dpr, 0, 0); g.clearRect(0, 0, W, H);
  if (!state) return;
  const L = 44, Rx = W - 40, Tp = 22, B = H - 24;
  const roast = state.t_roast >= 0;
  const xi = roast ? 1 : 0;
  let x0, x1;
  if (roast) { x0 = -60; x1 = Math.max(720, Math.ceil((state.t_roast + 60) / 120) * 120); }
  else { x1 = Math.max(60, state.t); x0 = Math.max(0, x1 - 600); }
  // Axes in display units: C 0-250 by 50 / F 0-500 by 100; RoR 0-50 C/min / 0-90 F/min.
  const tStep = isF() ? 100 : 50, rorMax = isF() ? 90 : 50, rorStep = isF() ? 15 : 10;
  let tMax = isF() ? 500 : 250;
  for (const r of history) if (r[xi] >= x0) tMax = Math.max(tMax, T(r[2]) + 10, T(r[3]) + 10);
  tMax = Math.ceil(tMax / tStep) * tStep;
  const X = (x) => L + (x - x0) / (x1 - x0) * (Rx - L);
  const Y = (c) => B - T(c) / tMax * (B - Tp);
  const YR = (r) => B - Math.max(-5, Math.min(rorMax, R(r))) / rorMax * (B - Tp);
  g.font = '11px system-ui'; g.fillStyle = css('--muted'); g.strokeStyle = css('--line'); g.lineWidth = 1;
  for (let v = 0; v <= tMax; v += tStep) { const y = B - v / tMax * (B - Tp); g.beginPath(); g.moveTo(L, y); g.lineTo(Rx, y); g.stroke(); g.fillText(v, 8, y + 4); }
  for (let v = 0; v <= rorMax; v += rorStep) g.fillText(v, Rx + 6, B - v / rorMax * (B - Tp) + 4);
  g.fillText(TU(), 8, Tp - 2); g.fillText(TU() + '/min', Rx - 24, Tp - 2);
  const step = (x1 - x0) > 900 ? 120 : 60;
  for (let x = Math.ceil(x0 / step) * step; x <= x1; x += step) {
    g.beginPath(); g.moveTo(X(x), Tp); g.lineTo(X(x), B); g.stroke(); g.fillText(fmtTime(x), X(x) - 14, H - 6);
  }
  // heat % band
  g.fillStyle = 'rgba(224,150,42,0.18)';
  for (const r of history) { if (r[xi] < x0) continue; const h = r[7] / 100 * (B - Tp) * 0.18; g.fillRect(X(r[xi]), B - h, Math.max(1, (Rx - L) / (x1 - x0)), h); }
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
    g.beginPath(); g.moveTo(X(x), Tp); g.lineTo(X(x), B); g.stroke();
    g.fillText(e.event, X(x) + 3, Tp + 12 + (row++ % 2) * 13);
  }
  g.setLineDash([]);
  // legend
  const leg = [['BT', '--bt'], ['ET', '--et'], ['RoR', '--ror'], ['true bean', '--true']];
  const legW = leg.reduce((w, [n]) => w + 22 + g.measureText(n).width, 0);
  let lx = Rx - legW - 4;
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
      <tr><td>Batch → out</td><td>${fmtW(r.batch_g)} → ${fmtW(r.out_g)} ${units.weight} (${r.weight_loss_pct}% loss)</td></tr>
      <tr><td>Moisture</td><td>${r.moisture_pct}%</td></tr>
      <tr><td>TP / DRY / FCs / DROP</td><td>${fmtTime(r.tp_s)} / ${fmtTime(r.dry_s)} / ${fmtTime(r.fcs_s)} / ${fmtTime(r.drop_s)}</td></tr>
      <tr><td>BT at TP / FCs / DROP</td><td>${T(r.tp_bt).toFixed(0)} / ${T(r.fcs_bt).toFixed(0)} / ${T(r.drop_bt).toFixed(0)} ${TU()}</td></tr>
      <tr><td>Development</td><td>${r.dev_time_s} s (DTR ${r.dtr_pct}%)</td></tr>
      <tr><td>Agtron whole / ground</td><td>${r.agtron_whole} / ${r.agtron_ground} (±${r.agtron_sd})</td></tr>
      <tr><td>Roast level</td><td>${r.roast_level}</td></tr>
      <tr><td>RoR max / FC / drop</td><td>${R(r.max_ror).toFixed(1)} / ${R(r.ror_at_fcs).toFixed(1)} / ${R(r.ror_at_drop).toFixed(1)} ${TU()}/min</td></tr>
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
$('charge').onclick = () => act({ cmd: 'charge', batch_g: fromW(+$('batch').value) });
$('drop').onclick = () => act({ cmd: 'drop' });
$('preheat').onclick = async () => {
  preheatC = fromT(+$('preheatT').value);
  await act({ cmd: 'preheat', target_C: preheatC });
  history = []; send({ cmd: 'subscribe', history_from: 0 });
};
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
  cmd.ambient_C = fromT(cmd.ambient_C);
  cmd.altitude_m = fromA(cmd.altitude_m);
  act(cmd);
};
// Units are one shared, saved setting: changing them here also changes the native
// app and what Artisan receives. The state stream then calls applyUnits().
$('uTemp').onchange = (e) => act({ cmd: 'units', temperature: e.target.value });
$('uWeight').onchange = (e) => act({ cmd: 'units', weight: e.target.value });
$('uAlt').onchange = (e) => act({ cmd: 'units', altitude: e.target.value });
$('faultApply').onclick = () => act({ cmd: 'fault', probe: $('faultProbe').value, type: $('faultType').value });
for (const b of document.querySelectorAll('[data-mark]')) b.onclick = () => act({ cmd: 'mark', event: b.dataset.mark });
window.addEventListener('resize', drawChart);
$('sound').onchange = (e) => setSound(e.target.checked);
$('volume').oninput = (e) => { if (snd) snd.gain.gain.value = +e.target.value / 100; };

connect();
