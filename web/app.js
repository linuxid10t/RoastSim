// RoastSim web control panel. Talks to the simulator over the same WebSocket
// JSON API as every other front end (docs/API.md), and mirrors the native
// Win32 app: menus for chores and settings, a roast progress panel, a graph
// that runs from power-on to the roast report, and a status bar.
'use strict';

const $ = (id) => document.getElementById(id);
const COLS = ['t', 't_roast', 'bt', 'et', 'ror_bt', 'ror_et', 'bean_true', 'heat', 'fan', 'drum'];
const PHASES = ['Drying', 'Maillard', 'Development'];
const MARKS = [['DRY', 'Mark DRY'], ['FCs', 'Mark FC start'], ['FCe', 'Mark FC end'], ['SCs', 'Mark SC start'], ['SCe', 'Mark SC end']];
const SPEEDS = [0, 1, 2, 5, 10, 30, 60];
const VOLUMES = [25, 50, 75, 100];
const ENV_DEFAULTS = { ambient_C: 22, line_voltage: 120, source_ohms: 0.25, gas_pressure: 1, altitude_m: 0, humidity_rh: 45 };

let ws = null, reqId = 0, serverPollTimer;
const pending = new Map();
let state = null, def = null, defLoading = false;
let history = [];          // rows: arrays in COLS order, kept since power-on
let events = [];           // events of the current graph's roast
let offRoast = [];         // roast charged while the machine is off (no graph yet)
let dragging = new Set();
let snd = null;            // { ctx, gain, rate, next } while sound is on
let volume = 75;           // percent
let preheatC = 200;        // preheat target, kept in C
let serverInfo = null;

// The graph covers one session: it starts when the machine is switched on and
// ends with the roast report (COOL), when the machine switches off again.
// Beans charged into a switched-off machine still count as the roast once it
// is switched on.
// Session-clock seconds; start < 0 = not started, end < 0 = still recording.
let graph = { start: -1, end: -1 };
// Events up to this sequence number were replayed on connect: they rebuild the
// graph but must not pop up a report or switch the machine off again.
let liveFrom = Infinity;
let eventSeq = 0;

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
const isFt = () => units.altitude === 'ft';
const A = (m) => isFt() ? m / 0.3048 : m;
const fromA = (v) => isFt() ? v * 0.3048 : v;
const AU = () => isFt() ? 'ft' : 'm';
const t1 = (c) => c === -1 ? 'open' : T(c).toFixed(1);   // probe reading (-1 = open thermocouple)
const num = (v, step) => String(Math.round(v / step) * step).replace(/(\.\d*?)0+$/, '$1').replace(/\.$/, '');

function applyUnits(s) {
  for (const el of document.querySelectorAll('.tu')) el.textContent = TU();
  $('batchUnit').textContent = units.weight;
  $('altUnit').textContent = AU();
  $('batch').value = fmtW(s.next_batch_g);
}

// ---------------------------------------------------------------- transport
function connect() {
  const url = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws';
  const socket = new WebSocket(url);
  ws = socket;
  socket.binaryType = 'arraybuffer';
  socket.onopen = async () => {
    history = []; events = []; offRoast = []; graph = { start: -1, end: -1 };
    eventSeq = 0;
    renderEvents();
    if (!await loadLists()) return;
    const s0 = await refreshDef();
    if (!s0 || ws !== socket) return;
    liveFrom = s0.event_seq || 0;
    send({ cmd: 'subscribe', history_from: 0, event_seq: 0 });
    if (snd) send({ cmd: 'audio', value: true, rate: snd.rate });  // resume sound after a reconnect
    pollServer(socket);
  };
  socket.onclose = () => {
    ws = null;
    state = null;
    serverInfo = null;
    clearTimeout(serverPollTimer);
    for (const resolve of pending.values()) resolve({ ok: false, error: 'connection closed' });
    pending.clear();
    $('sbState').textContent = 'offline';
    setTimeout(connect, 1500);
  };
  socket.onmessage = (m) => {
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

async function pollServer(socket) {
  if (ws !== socket || socket.readyState !== 1) return;
  const info = await send({ cmd: 'server' });
  if (ws !== socket) return;
  serverInfo = info;
  serverPollTimer = setTimeout(() => pollServer(socket), 5000);
}

// ---------------------------------------------------------------- setup
async function loadLists() {
  const l = await send({ cmd: 'list' });
  if (!l.ok) return false;
  const fill = (sel, arr) => { sel.innerHTML = ''; for (const x of arr) sel.add(new Option(x.name, x.id)); };
  fill($('roaster'), l.roasters || []);
  fill($('bean'), l.beans || []);
  return true;
}

async function refreshDef() {
  defLoading = true;
  const s = await send({ cmd: 'state', full: true });
  defLoading = false;
  if (!s.roaster_def) return;
  def = s.roaster_def;
  $('roaster').value = s.roaster.id;
  $('bean').value = s.bean.id;
  $('desc').textContent = def.description || '';
  buildSliders();
  const fp = $('faultProbe'); fp.innerHTML = '';
  for (const p of def.probes) fp.add(new Option(p.label + ' (' + p.id + ')', p.id));
  if (s.units) units = s.units;
  unitKey = JSON.stringify(units);
  applyUnits(s);
  return s;
}

function buildSliders() {
  const box = $('sliders'), toggles = $('toggles');
  box.innerHTML = ''; toggles.innerHTML = '';
  for (const c of def.controls) {
    if (c.toggle) {  // e.g. the cooling tray fan: a checkbox next to Power, as in the native app
      const row = document.createElement('label'); row.className = 'check';
      row.innerHTML = `<input type="checkbox" data-ctl="${c.id}"> ${c.label}`;
      row.querySelector('input').onchange = (e) => send({ cmd: 'set', control: c.id, value: e.target.checked ? 100 : 0 });
      toggles.appendChild(row); continue;
    }
    const row = document.createElement('div'); row.className = 'slider';
    row.innerHTML = `<span>${c.label}</span><input type="range" min="${c.min}" max="${c.max}" step="${c.step}" data-ctl="${c.id}"><output>0%</output>`;
    const input = row.querySelector('input'), out = row.querySelector('output');
    input.oninput = () => { dragging.add(c.id); out.textContent = input.value + '%'; send({ cmd: 'set', control: c.id, value: +input.value }); };
    input.onchange = () => dragging.delete(c.id);
    box.appendChild(row);
  }
  $('controlsHead').hidden = box.children.length === 0;
}

// ---------------------------------------------------------------- graph session + events
function fmtTime(s) {
  if (s === null || s === undefined || s < 0) return '--:--';
  const t = Math.floor(s); return String(Math.floor(t / 60)).padStart(2, '0') + ':' + String(t % 60).padStart(2, '0');
}
const fmtSigned = (s) => s < 0 ? '-' + fmtTime(-s) : fmtTime(s);

function startGraph(t) {
  graph = { start: t, end: -1 };
  events = [];
  if (t >= 0) {
    // Beans charged while the machine was off: that roast carries on here
    // (its CHARGE stays off the graph, which only starts now).
    events = offRoast;
    const i = history.findIndex(r => r[0] >= t - 1); history.splice(0, i < 0 ? history.length : i);
  }
  offRoast = [];
  renderEvents();
}

const eventTime = (type) => { const e = events.find(x => x.event === type); return e ? e.t_roast : -1; };

function onEvent(e) {
  eventSeq = e.seq;
  const type = e.event;
  // A cold reset or roaster load (from any front end) wipes the simulator's
  // history: start a new graph if the machine is on.
  if (type === 'NOTE' && e.detail === 'cold reset' && e.origin === 'sim') {
    offRoast = [];
    startGraph((e.power ?? state?.power) ? e.t : -1);
    return;
  }
  // Switching on starts a new graph, unless a roast is in progress on the
  // current one (a mid-roast power cut keeps the graph going).
  if (type === 'NOTE' && e.detail === 'power on' && e.power !== false &&
      (graph.start < 0 || graph.end >= 0 || offRoast.length || eventTime('CHARGE') < 0)) startGraph(e.t);
  // Hold a roast charged with the power off for the next power-on.
  if (type === 'CHARGE' && (e.power === false || graph.start < 0)) {
    offRoast = [e];
    return;
  }
  if (offRoast.length) {
    if (type === 'COOL') offRoast = [];
    else offRoast.push(e);
    return;
  }
  if (graph.start < 0 || graph.end >= 0) return;
  if (type === 'CHARGE') events = [];
  events.push(e);
  renderEvents();
  if (type === 'COOL') {
    // The roast is over: freeze the graph here, then power off and report.
    graph.end = e.t;
    if (e.seq > liveFrom) roastDone();
  }
}

async function roastDone() {
  await send({ cmd: 'power', value: false });
  showReport();
}

function renderEvents() {
  const ol = $('events');
  ol.innerHTML = events.map(e => `<li>${fmtTime(e.t_roast)}  ${e.event}${e.detail ? ' ' + e.detail : ''}  (BT ${t1(e.bt)}${TU()}, ${e.origin})</li>`).join('');
  ol.scrollTop = ol.scrollHeight;
}

// ---------------------------------------------------------------- roast progress
// Everything the progress panel shows, worked out from the roast's events.
// After DROP the numbers freeze at the drop.
function phaseView() {
  const v = { roast: false, dropped: false, end: 0, start: [-1, -1, -1], dur: [-1, -1, -1], active: -1,
              sinceName: 'dry end', since: -1, sinceIdx: -1 };
  const drop = eventTime('DROP');
  if (eventTime('CHARGE') < 0 || (drop < 0 && (!state || state.t_roast < 0))) return v;
  v.roast = true;
  v.dropped = drop >= 0;
  v.end = v.dropped ? drop : state.t_roast;
  const dry = eventTime('DRY'), fcs = eventTime('FCs');
  v.start = [0, dry, fcs >= 0 && dry >= 0 ? fcs : -1];
  for (let i = 0; i < 3; i++) {
    if (v.start[i] < 0) continue;
    const next = i + 1 < 3 && v.start[i + 1] >= 0 ? v.start[i + 1] : v.end;
    v.dur[i] = Math.max(0, next - v.start[i]);
    if (!v.dropped) v.active = i;
  }
  // The newest event wins: second crack replaces first crack, and so on.
  for (const [type, name, phase] of [['TP', 'turning point', 0], ['DRY', 'dry end', 1], ['FCs', 'first crack', 2], ['SCs', 'second crack', 2]]) {
    const t = eventTime(type);
    if (t < 0 || t > v.end) continue;
    v.since = v.end - t; v.sinceName = name; v.sinceIdx = phase;
  }
  return v;
}

function renderProgress() {
  const v = phaseView();
  $('roastCap').textContent = v.dropped ? 'Roast time (dropped)' : 'Roast time';
  $('roastTime').textContent = v.roast ? fmtTime(v.end) : '--:--';
  $('roastTime').parentElement.classList.toggle('idle', !v.roast);
  $('sinceCap').textContent = `Since ${v.sinceName}${v.dropped && v.since >= 0 ? ' (to drop)' : ''}`;
  $('since').textContent = v.since >= 0 ? fmtTime(v.since) : '--:--';
  $('since').style.color = v.since >= 0 ? `var(--ph${v.sinceIdx})` : 'var(--muted)';
  for (const cell of document.querySelectorAll('.phase')) {
    const i = +cell.dataset.i, reached = v.dur[i] >= 0;
    cell.classList.toggle('off', !reached);
    cell.classList.toggle('active', i === v.active);
    cell.querySelector('b').textContent = reached ? fmtTime(v.dur[i]) : '--:--';
    cell.querySelector('em').textContent = reached && v.end > 0 ? (v.dur[i] / v.end * 100).toFixed(0) + '%' : '';
  }
  // The Mark button offers the next event of the roast not yet marked (by
  // you, Artisan or the simulator's own detection).
  const next = MARKS.findIndex(([type]) => eventTime(type) < 0);
  const roasting = state && state.phase === 'roasting';
  const b = $('markNext');
  b.textContent = next >= 0 ? MARKS[next][1] : 'All marked';
  b.disabled = !(roasting && next >= 0);
  b.dataset.mark = next >= 0 ? MARKS[next][0] : '';
}

// ---------------------------------------------------------------- state
function onState(s) {
  if (s.history) {
    for (const r of s.history) history.push(r);
    if (history.length > 20000) history.splice(0, history.length - 20000);
  }
  state = s;
  // Machine already on with no power-on in the event log (e.g. switched on
  // before the last cold reset): the graph starts with the oldest sample.
  if (s.power && graph.start < 0 && eventSeq >= s.event_seq) startGraph(history.length ? history[0][0] : s.t);
  if (def?.id !== s.roaster.id && !defLoading) refreshDef();
  $('bean').value = s.bean.id;
  if (s.units && JSON.stringify(s.units) !== unitKey) {  // changed here or in another front end
    units = s.units;
    unitKey = JSON.stringify(units);
    applyUnits(s);
    renderEvents();
  }
  $('bt').textContent = t1(s.bt);
  $('et').textContent = t1(s.et);
  $('ror').textContent = s.t_roast > 0 ? R(s.ror_bt).toFixed(1) : '--';
  // Estimated time to DRY / first crack (the simulator's projection at the
  // current heat), counting down; "stall" when the beans won't get there.
  for (const [id, v] of [['etaDry', s.eta_dry_s], ['etaFc', s.eta_fc_s]]) {
    const ok = typeof v === 'number' && v >= 0, stall = v === -2;
    $(id).textContent = ok ? fmtTime(Math.ceil(v)) : stall ? 'stall' : '--:--';
    $(id).classList.toggle('na', !ok && !stall);
    $(id).classList.toggle('stall', stall);
  }
  for (const el of document.querySelectorAll('[data-ctl]')) {
    const c = s.controls[el.dataset.ctl]; if (!c) continue;
    if (el.type === 'checkbox') el.checked = c.value >= 50;
    else if (!dragging.has(el.dataset.ctl) && document.activeElement !== el) {
      el.value = c.value;
      el.nextElementSibling.textContent = Math.abs(c.actual - c.value) > 0.5 ? `${c.actual.toFixed(0)}→${c.value.toFixed(0)}` : c.value.toFixed(0) + '%';
    }
  }
  $('power').checked = !!s.power;
  $('controlsPanel').classList.toggle('off', !s.power);
  $('alarms').innerHTML = s.alarms.map(a => `<span>${a.replace(/_/g, ' ')}</span>`).join('');
  renderStatus(s);
  renderProgress();
  renderTables(s);
  drawChart();
}

function speedText(s) {
  if (s.paused) return 'Paused';
  if (s.logger_connected) return 'Real time (logger)';
  return s.time_scale === 1 ? 'Real time' : `Speed ${s.time_scale}×`;
}
const envSummary = (e) => `${T(e.ambient_C).toFixed(0)} ${TU()} · ${e.line_voltage.toFixed(0)} V · ${A(e.altitude_m).toFixed(0)} ${AU()}`;

function renderStatus(s) {
  const phases = { idle: 'empty', roasting: 'roasting', cooling: 'cooling', done: 'done' };
  $('sbState').textContent = s.power ? `Power on · ${phases[s.phase] || s.phase}` : 'POWER OFF';
  $('sbState').classList.toggle('off', !s.power);
  $('sbSpeed').textContent = speedText(s) + ' ▾';
  $('sbEnv').textContent = envSummary(s.env);
  $('sbLogger').textContent = s.logger_connected ? 'Logger connected' : 'No logger';
  if (serverInfo && serverInfo.net) {
    $('sbModbus').textContent = 'Modbus ' + (serverInfo.net.modbus || 'off');
    $('sbWeb').textContent = 'Web/WS ' + (serverInfo.net.http || 'off');
  }
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
    gain.gain.value = volume / 100;
    gain.connect(ctx.destination);
    snd = { ctx, gain, rate: 24000, next: 0 };
    const r = await send({ cmd: 'audio', value: true, rate: snd.rate });
    if (!r.ok) { alert(r.error || 'sound unavailable'); setSound(false); }
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

  // The graph runs from power-on (graph.start) to the roast report (graph.end).
  // Before CHARGE the axis is time since power-on; from CHARGE on it is roast
  // time, with the pre-charge part (preheat) at negative times.
  const started = graph.start >= 0;
  const tEnd = graph.end >= 0 ? graph.end : state.t;
  const charge = events.find(e => e.event === 'CHARGE');
  const roast = started && !!charge;
  const origin = roast ? charge.t : graph.start;
  const inGraph = (ts) => started && ts >= graph.start - 0.5 && ts <= tEnd + 0.5;
  let x0 = 0, x1 = 300;
  if (roast) {
    // Negative when the machine was switched on before CHARGE (preheat);
    // positive when the beans went into a cold machine before power-on.
    x0 = Math.floor((graph.start - charge.t) / 30) * 30;
    x1 = Math.max(720, Math.ceil((tEnd - charge.t + 60) / 120) * 120);
  } else if (started) {
    x1 = Math.max(x1, Math.ceil((tEnd - graph.start + 30) / 60) * 60);
  }
  // Axes in display units: C 0-250 by 50 / F 0-500 by 100; RoR 0-50 C/min / 0-90 F/min.
  const tStep = isF() ? 100 : 50, rorMax = isF() ? 90 : 50, rorStep = isF() ? 15 : 10;
  let tMax = isF() ? 500 : 250;
  for (const r of history) if (inGraph(r[0])) tMax = Math.max(tMax, T(r[2]) + 10, T(r[3]) + 10);
  tMax = Math.ceil(tMax / tStep) * tStep;
  const X = (x) => L + (x - x0) / (x1 - x0) * (Rx - L);
  const Y = (c) => B - T(c) / tMax * (B - Tp);
  const YR = (r) => B - Math.max(-5, Math.min(rorMax, R(r))) / rorMax * (B - Tp);
  g.font = '11px system-ui'; g.fillStyle = css('--muted'); g.strokeStyle = css('--line'); g.lineWidth = 1;
  for (let v = 0; v <= tMax; v += tStep) { const y = B - v / tMax * (B - Tp); g.beginPath(); g.moveTo(L, y); g.lineTo(Rx, y); g.stroke(); g.fillText(v, 8, y + 4); }
  for (let v = 0; v <= rorMax; v += rorStep) g.fillText(v, Rx + 6, B - v / rorMax * (B - Tp) + 4);
  g.fillText(TU(), 8, Tp - 8); g.fillText(TU() + '/min', Rx - 24, Tp - 8);
  // Time grid: the smallest step that keeps the labels from crowding.
  let step = 600;
  for (const s of [30, 60, 120, 300]) if ((x1 - x0) / s <= (Rx - L) / 50) { step = s; break; }
  for (let x = Math.ceil(x0 / step) * step; x <= x1; x += step) {
    g.beginPath(); g.moveTo(X(x), Tp); g.lineTo(X(x), B); g.stroke(); g.fillText(fmtSigned(x), X(x) - 14, H - 6);
  }
  if (!started) {
    const msg = 'Switch the machine on (Power) to start the graph';
    g.font = '600 14px system-ui'; g.fillStyle = css('--muted');
    g.fillText(msg, (L + Rx - g.measureText(msg).width) / 2, (Tp + B) / 2);
    return;
  }
  // heat % band
  g.fillStyle = 'rgba(224,150,42,0.18)';
  for (const r of history) {
    if (!inGraph(r[0])) continue;
    const x = r[0] - origin; if (x < x0 || x > x1) continue;
    const h = r[7] / 100 * (B - Tp) * 0.18; g.fillRect(X(x), B - h, Math.max(1, (Rx - L) / (x1 - x0)), h);
  }
  const line = (idx, color, width, ror, filter) => {
    g.strokeStyle = color; g.lineWidth = width; g.beginPath(); let on = false;
    for (const r of history) {
      const x = r[0] - origin;
      if (!inGraph(r[0]) || x < x0 || x > x1 || (filter && !filter(r))) { on = false; continue; }
      if (!ror && r[idx] <= -0.5) { on = false; continue; }
      const y = ror ? YR(r[idx]) : Y(r[idx]);
      if (!on) { g.moveTo(X(x), y); on = true; } else g.lineTo(X(x), y);
    }
    g.stroke();
  };
  line(3, css('--et'), 2, false);
  line(2, css('--bt'), 2, false);
  if (roast) {
    line(6, css('--true'), 1, false, r => r[1] >= 0);   // no beans before CHARGE
    line(4, css('--ror'), 1.5, true, r => r[1] > 20);
  }
  // Phase band along the top of the plot: drying, Maillard, development.
  const pv = phaseView();
  if (roast && pv.roast) {
    for (let i = 0; i < 3; i++) {
      if (pv.dur[i] < 0) continue;
      const a = Math.max(L, X(pv.start[i])), z = Math.min(Rx, X(pv.start[i] + pv.dur[i]));
      if (z > a) { g.fillStyle = css('--ph' + i); g.fillRect(a, Tp, z - a, 5); }
    }
  }
  // Event lines, each labelled with its time and BT so nothing has to be read
  // off the axes. Labels go in the first row where they don't overlap.
  const rowEnd = [-1e9, -1e9, -1e9, -1e9];
  for (const e of events) {
    if (['ALARM', 'NOTE'].includes(e.event) || (e.event === 'CHARGE' && e.power === false)) continue;
    const x = e.t - origin;
    if (!inGraph(e.t) || x < x0 || x > x1) continue;
    const px = X(x);
    g.strokeStyle = css('--muted'); g.lineWidth = 1; g.setLineDash([3, 3]);
    g.beginPath(); g.moveTo(px, Tp + 5); g.lineTo(px, B); g.stroke(); g.setLineDash([]);
    let detail = fmtSigned(x);
    if (e.bt !== -1 && e.event !== 'COOL') detail += `  ${T(e.bt).toFixed(0)}${TU()}`;
    g.font = '600 12px system-ui'; const nameW = g.measureText(e.event).width;
    g.font = '11px system-ui'; const w = nameW + 4 + g.measureText(detail).width;
    let row = 0;
    for (let i = 0; i < rowEnd.length; i++) { if (rowEnd[i] < px) { row = i; break; } if (rowEnd[i] < rowEnd[row]) row = i; }
    const lx = Math.min(px + 4, Rx - w - 2), ly = Tp + 9 + row * 16;
    g.fillStyle = css('--panel'); g.fillRect(lx - 2, ly, w + 4, 15);
    g.fillStyle = css('--ink'); g.font = '600 12px system-ui'; g.fillText(e.event, lx, ly + 11);
    g.fillStyle = css('--muted'); g.font = '11px system-ui'; g.fillText(detail, lx + nameW + 4, ly + 11);
    rowEnd[row] = lx + w + 6;
  }
  // legend
  g.font = '11px system-ui';
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
  if (!$('reportDlg').open) $('reportDlg').showModal();
}

// ---------------------------------------------------------------- menus
const menus = [...document.querySelectorAll('details.menu')];
for (const d of menus) {
  d.addEventListener('toggle', () => {
    if (!d.open) return;
    for (const o of menus) if (o !== d) o.open = false;
    updateMenus();
  });
  d.querySelector('.items').addEventListener('click', (e) => { if (e.target.closest('button')) d.open = false; });
}
document.addEventListener('click', (e) => { for (const d of menus) if (!d.contains(e.target)) d.open = false; });
document.addEventListener('keydown', (e) => { if (e.key === 'Escape') for (const d of menus) d.open = false; });

for (const [i, v] of SPEEDS.entries()) {
  const b = document.createElement('button'); b.className = 'radio'; b.dataset.speed = v;
  b.textContent = v === 0 ? 'Pause' : v === 1 ? 'Real time' : v + '×';
  b.onclick = () => setSpeed(v);
  $('speeds').appendChild(b);
  if (i === 0) $('speeds').appendChild(document.createElement('hr'));
}
for (const v of VOLUMES) {
  const b = document.createElement('button'); b.className = 'radio'; b.dataset.volume = v; b.textContent = v + '%';
  b.onclick = () => { volume = v; if (snd) snd.gain.gain.value = v / 100; };
  $('volumes').appendChild(b);
}

// Menus show live state (checks, radio marks, current environment values).
function updateMenus() {
  const s = state;
  const mark = (el, on) => el.classList.toggle('checked', !!on);
  mark($('mPower'), s && s.power);
  mark($('mSound'), snd);
  for (const b of document.querySelectorAll('[data-speed]')) mark(b, s && (s.paused ? +b.dataset.speed === 0 : !s.paused && +b.dataset.speed === s.time_scale));
  for (const b of document.querySelectorAll('[data-volume]')) mark(b, +b.dataset.volume === volume);
  for (const b of document.querySelectorAll('[data-unit]')) mark(b, units[b.dataset.unit] === b.dataset.val);
  const roasting = s && s.phase === 'roasting';
  for (const b of document.querySelectorAll('.menu [data-mark]')) {
    const done = eventTime(b.dataset.mark) >= 0;
    mark(b, done);
    b.disabled = done || !roasting;
  }
  if (s) {
    $('mEnvAmbient').textContent = `Room temperature:  ${T(s.env.ambient_C).toFixed(0)} ${TU()}`;
    $('mEnvVolts').textContent = `Mains voltage:  ${s.env.line_voltage.toFixed(0)} V`;
    $('mEnvAlt').textContent = `Altitude:  ${A(s.env.altitude_m).toFixed(0)} ${AU()}`;
  }
}

async function setSpeed(v) {
  if (v === 0) await act({ cmd: 'pause', value: true });
  else { await act({ cmd: 'pause', value: false }); await act({ cmd: 'time_scale', value: v }); }
}

// ---------------------------------------------------------------- dialogs
function fillEnv(e) {
  $('env_ambient').value = num(T(e.ambient_C), 0.1);
  $('env_volts').value = num(e.line_voltage, 0.1);
  $('env_ohms').value = num(e.source_ohms, 0.001);
  $('env_gas').value = num(e.gas_pressure * 100, 1);
  $('env_alt').value = num(A(e.altitude_m), 1);
  $('env_humidity').value = num(e.humidity_rh, 0.1);
}

function openEnv(focus) {
  if (!state) return;
  fillEnv(state.env);
  $('envError').textContent = '';
  $('envDlg').showModal();
  const el = $('env_' + (focus || 'ambient'));
  el.focus(); el.select();
}

// Same limits as the native app; returns [field, message] for the first bad value.
function envProblem() {
  const v = (id) => $(id).value === '' ? NaN : +$(id).value;
  const amb = fromT(v('env_ambient')), volts = v('env_volts'), ohms = v('env_ohms'), gas = v('env_gas'), alt = fromA(v('env_alt'));
  if (!(amb >= -40 && amb <= 60)) return ['env_ambient', isF() ? 'Room temperature must be between -40 and 140 °F.' : 'Room temperature must be between -40 and 60 °C.'];
  if (!(volts >= 50 && volts <= 260)) return ['env_volts', 'Mains voltage must be between 50 and 260 V.'];
  if (!(ohms >= 0 && ohms <= 5)) return ['env_ohms', 'Wiring resistance must be between 0 and 5 ohms.'];
  if (!(gas >= 0 && gas <= 200)) return ['env_gas', 'Gas supply pressure must be between 0 and 200 %.'];
  if (!(alt >= -500 && alt <= 5000)) return ['env_alt', isFt() ? 'Altitude must be between -1640 and 16400 ft.' : 'Altitude must be between -500 and 5000 m.'];
  const rh = v('env_humidity');
  if (!(rh >= 0 && rh <= 100)) return ['env_humidity', 'Relative humidity must be between 0 and 100 %.'];
  return null;
}

$('envForm').addEventListener('submit', async (ev) => {
  if (!ev.submitter || ev.submitter.value !== 'ok') return;
  const bad = envProblem();
  if (bad) { ev.preventDefault(); $('envError').textContent = bad[1]; $(bad[0]).focus(); $(bad[0]).select(); return; }
  await act({ cmd: 'env', ambient_C: fromT(+$('env_ambient').value), line_voltage: +$('env_volts').value,
              source_ohms: +$('env_ohms').value, gas_pressure: +$('env_gas').value / 100, altitude_m: fromA(+$('env_alt').value),
              humidity_rh: +$('env_humidity').value });
});
$('envDefaults').onclick = () => fillEnv({ ...ENV_DEFAULTS, line_voltage: state?.roaster?.nominal_V ?? ENV_DEFAULTS.line_voltage });
for (const b of document.querySelectorAll('[data-env]')) b.onclick = () => openEnv(b.dataset.env);

$('preheatDlg').addEventListener('close', async () => {
  if ($('preheatDlg').returnValue !== 'ok' || $('preheatT').value === '') return;
  preheatC = fromT(+$('preheatT').value);
  await act({ cmd: 'preheat', target_C: preheatC });
});
$('faultDlg').addEventListener('close', () => {
  if ($('faultDlg').returnValue === 'ok') act({ cmd: 'fault', probe: $('faultProbe').value, type: $('faultType').value });
});
$('faultClear').onclick = async () => { for (const p of def.probes) await act({ cmd: 'fault', probe: p.id, type: 'none' }); };

// ---------------------------------------------------------------- wiring
async function loadRoaster(confirmText) {
  if (state && state.bean_location === 'machine' && !confirm(confirmText)) { await refreshDef(); return; }  // puts the picker back
  await act({ cmd: 'load', roaster: $('roaster').value, bean: $('bean').value });
  await refreshDef();  // the cold reset that comes with a load restarts the graph
}
$('roaster').onchange = () => loadRoaster('Beans are roasting. Switch roaster anyway and discard them?');
$('mReload').onclick = () => loadRoaster('Beans are roasting. Reload the roaster anyway and discard them?');
$('bean').onchange = () => act({ cmd: 'bean', id: $('bean').value });
$('power').onchange = (e) => act({ cmd: 'power', value: e.target.checked });
$('mPower').onclick = () => act({ cmd: 'power', value: !(state && state.power) });
$('charge').onclick = () => act({ cmd: 'charge', batch_g: fromW(+$('batch').value) });
$('drop').onclick = () => act({ cmd: 'drop' });
$('markNext').onclick = () => { const m = $('markNext').dataset.mark; if (m) act({ cmd: 'mark', event: m }); };
for (const b of document.querySelectorAll('.menu [data-mark]')) b.onclick = () => act({ cmd: 'mark', event: b.dataset.mark });
for (const b of document.querySelectorAll('[data-cmd]')) {
  b.onclick = () => b.dataset.cmd === 'charge' ? $('charge').click() : act({ cmd: b.dataset.cmd });
}
$('mReport').onclick = showReport;
$('mPreheat').onclick = () => { $('preheatT').value = num(T(preheatC), 1); $('preheatDlg').showModal(); $('preheatT').select(); };
$('mReset').onclick = async () => { if (confirm('Cool the machine to room temperature and discard any beans?')) await act({ cmd: 'reset' }); };
$('mRepair').onclick = () => act({ cmd: 'repair' });
$('mFaults').onclick = () => $('faultDlg').showModal();
$('mSound').onclick = () => setSound(!snd);
// Units are one shared, saved setting: changing them here also changes the native
// app and what Artisan receives. The state stream then calls applyUnits().
for (const b of document.querySelectorAll('[data-unit]')) b.onclick = () => act({ cmd: 'units', [b.dataset.unit]: b.dataset.val });
$('sbSpeed').onclick = (e) => { e.stopPropagation(); $('simMenu').open = true; };
$('sbEnv').onclick = () => openEnv('');
window.addEventListener('resize', drawChart);

connect();
