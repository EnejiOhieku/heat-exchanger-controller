'use strict';

// ======================================================================
// Helpers
// ======================================================================
const $ = (id) => document.getElementById(id);
const qsa = (sel, root = document) => Array.from(root.querySelectorAll(sel));
const fmt = (v, d = 1) => (typeof v === 'number' && isFinite(v) ? v.toFixed(d) : '--');
const isTemp = (t) => typeof t === 'number' && t > -100;
const HEATER_MODES = ['Off', 'Manual', 'Thermostat', 'Time-PWM', 'PID'];
const SIM_MODES = ['Off', 'On', 'Auto (fault-tolerant)'];
const PROBE_NAMES = ['Th,in', 'Th,out', 'Tc,in', 'Tc,out'];
const FAULT_NAMES = [
  [1, 'hot flow blocked'],
  [2, 'Th,in probe lost'],
  [4, 'relay welded'],
  [8, 'cold flow blocked'],
];

function toast(msg, kind = '') {
  const el = document.createElement('div');
  el.className = 'toast ' + kind;
  el.textContent = msg;
  $('toasts').appendChild(el);
  setTimeout(() => el.remove(), kind === 'err' ? 6000 : 3500);
}

async function api(path, body, { method, timeout = 6000, quiet = false } = {}) {
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), timeout);
  try {
    const res = await fetch(path, {
      method: method || (body !== undefined ? 'POST' : 'GET'),
      headers: body !== undefined ? { 'Content-Type': 'application/json' } : undefined,
      body: body !== undefined ? JSON.stringify(body) : undefined,
      signal: ctrl.signal,
    });
    const data = await res.json().catch(() => ({}));
    if (!res.ok || data.ok === false) {
      if (!quiet) toast(data.message || ('Request failed (' + res.status + ')'), 'err');
      return null;
    }
    return data;
  } catch (e) {
    if (!quiet) toast('Controller not reachable', 'err');
    return null;
  } finally {
    clearTimeout(timer);
  }
}

// Controls the user touched recently are not overwritten by telemetry.
const touched = {};
const touch = (key) => { touched[key] = Date.now(); };
const isTouched = (key, ms = 4000) => touched[key] && Date.now() - touched[key] < ms;

function faultList(mask) {
  return FAULT_NAMES.filter(([bit]) => mask & bit).map(([, name]) => name).join(', ');
}

// ======================================================================
// Tabs
// ======================================================================
let activeTab = 'overview';
qsa('.tab').forEach((b) => b.addEventListener('click', () => {
  activeTab = b.dataset.tab;
  qsa('.tab').forEach((x) => x.classList.toggle('active', x === b));
  qsa('.tab-panel').forEach((p) => p.classList.toggle('active', p.id === 'tab-' + activeTab));
  if (activeTab === 'assistant') { $('chatDot').classList.add('hidden'); scrollChat(); }
  if (activeTab === 'status') $('statusDot').classList.add('hidden');
  if (activeTab === 'overview') drawChart();
}));

// ======================================================================
// Commands
// ======================================================================
async function control(body, okMsg) {
  const r = await api('/api/control', body);
  if (r) { if (okMsg !== false) toast(okMsg || r.message, 'ok'); poll(); }
  return r;
}
async function config(body, okMsg) {
  const r = await api('/api/config', body);
  if (r) { toast(okMsg || r.message, 'ok'); poll(); }
  return r;
}

$('btnEstop').addEventListener('click', () => control({ estop: true }));
document.addEventListener('click', (e) => {
  if (e.target.closest('[data-action="reset"]')) control({ reset_fault: true });
});

// ---- Hot pump -------------------------------------------------------
qsa('#segPumpMode button').forEach((b) => b.addEventListener('click', () => {
  touch('pmode');
  setPumpModeUi(+b.dataset.pmode);
  if (+b.dataset.pmode === 1) control({ flow_setpoint: +$('numFlowSp').value });
  else control({ pump_pct: +$('rngPump').value });
}));
$('rngPump').addEventListener('input', (e) => { touch('pump'); $('lblPumpCmd').textContent = e.target.value; });
$('rngPump').addEventListener('change', (e) => { touch('pump'); control({ pump_pct: +e.target.value }, false); });
qsa('[data-pump]').forEach((b) => b.addEventListener('click', () => {
  touch('pump'); touch('pmode');
  const v = +b.dataset.pump;
  $('rngPump').value = v; $('lblPumpCmd').textContent = v;
  setPumpModeUi(0);
  control(v === 0 ? { pump_stop: true } : { pump_pct: v }, false);
}));
$('btnFlowSp').addEventListener('click', () => { touch('pmode'); control({ flow_setpoint: +$('numFlowSp').value }); });
$('numFlowSp').addEventListener('input', () => touch('flowsp'));

function setPumpModeUi(mode) {
  qsa('#segPumpMode button').forEach((b) => b.classList.toggle('active', +b.dataset.pmode === mode));
  $('pumpManual').classList.toggle('hidden', mode !== 0);
  $('pumpFlow').classList.toggle('hidden', mode !== 1);
}

// ---- Cold pump ------------------------------------------------------
qsa('#segColdPumpMode button').forEach((b) => b.addEventListener('click', () => {
  touch('cpmode');
  setColdPumpModeUi(+b.dataset.cpmode);
  if (+b.dataset.cpmode === 1) control({ cold_flow_setpoint: +$('numColdFlowSp').value });
  else control({ cold_pump_pct: +$('rngColdPump').value });
}));
$('rngColdPump').addEventListener('input', (e) => { touch('cpump'); $('lblColdPumpCmd').textContent = e.target.value; });
$('rngColdPump').addEventListener('change', (e) => { touch('cpump'); control({ cold_pump_pct: +e.target.value }, false); });
qsa('[data-cpump]').forEach((b) => b.addEventListener('click', () => {
  touch('cpump'); touch('cpmode');
  const v = +b.dataset.cpump;
  $('rngColdPump').value = v; $('lblColdPumpCmd').textContent = v;
  setColdPumpModeUi(0);
  control(v === 0 ? { cold_pump_stop: true } : { cold_pump_pct: v }, false);
}));
$('btnColdFlowSp').addEventListener('click', () => { touch('cpmode'); control({ cold_flow_setpoint: +$('numColdFlowSp').value }); });
$('numColdFlowSp').addEventListener('input', () => touch('cflowsp'));

function setColdPumpModeUi(mode) {
  qsa('#segColdPumpMode button').forEach((b) => b.classList.toggle('active', +b.dataset.cpmode === mode));
  $('coldPumpManual').classList.toggle('hidden', mode !== 0);
  $('coldPumpFlow').classList.toggle('hidden', mode !== 1);
}

// ---- Heater ---------------------------------------------------------
qsa('#segHeaterMode button').forEach((b) => b.addEventListener('click', () => {
  touch('hmode');
  setHeaterModeUi(+b.dataset.hmode);
  control({ heater_mode: +b.dataset.hmode }, false);
}));
qsa('[data-hman]').forEach((b) => b.addEventListener('click', () => control({ heater_manual: b.dataset.hman === '1' }, false)));
$('rngSp').addEventListener('input', (e) => { touch('sp'); $('lblSp').textContent = (+e.target.value).toFixed(1); });
$('rngSp').addEventListener('change', (e) => { touch('sp'); control({ target_temp: +e.target.value }, false); });
$('rngDuty').addEventListener('input', (e) => { touch('duty'); $('lblDutyCmd').textContent = e.target.value; });
$('rngDuty').addEventListener('change', (e) => { touch('duty'); control({ heater_duty: +e.target.value }, false); });

function setHeaterModeUi(mode) {
  qsa('#segHeaterMode button').forEach((b) => b.classList.toggle('active', +b.dataset.hmode === mode));
  qsa('.hm').forEach((el) => el.classList.toggle('hidden', !el.classList.contains('hm-' + mode)));
}

// ---- Tuning / calibration ------------------------------------------
const numVal = (id) => { const v = parseFloat($(id).value); return isFinite(v) ? v : undefined; };
$('btnSaveTuning').addEventListener('click', () => config({
  kp: numVal('cfgKp'), ki: numVal('cfgKi'), kd: numVal('cfgKd'), hysteresis: numVal('cfgHyst'),
  control_probe: +$('cfgProbe').value, flow_kp: numVal('cfgFkp'), flow_ki: numVal('cfgFki'),
  cold_flow_kp: numVal('cfgFkp2'), cold_flow_ki: numVal('cfgFki2'),
}));
$('btnSaveCal').addEventListener('click', () => config({
  flow_k_factor: numVal('cfgK'),
  cold_flow_k_factor: numVal('cfgColdK'),
  pump_min_duty: Math.round(numVal('cfgPmin') ?? 25),
  cold_pump_min_duty: Math.round(numVal('cfgColdPmin') ?? 25),
}));
let cfgLoaded = false;
qsa('.form-grid input, .form-grid select').forEach((el) => el.addEventListener('input', () => touch('cfg')));

// ---- Simulation -----------------------------------------------------
qsa('#segSim button').forEach((b) => b.addEventListener('click', () => {
  const m = +b.dataset.sim;
  if (T && T.sys.sim_mode === m) return;
  if (!confirm('Changing simulation mode stops both pumps and turns the heater off. Continue?')) return;
  config({ sim_mode: m });
}));
qsa('[data-fault]').forEach((c) => c.addEventListener('change', () => {
  touch('faults');
  const mask = qsa('[data-fault]').reduce((m, x) => m | (x.checked ? +x.dataset.fault : 0), 0);
  config({ sim_faults: mask }, 'Simulated faults updated');
}));

// ---- Wi-Fi ----------------------------------------------------------
$('btnWifi').addEventListener('click', () => {
  const ssid = $('txtSsid').value.trim();
  if (!ssid) return toast('Enter an SSID', 'err');
  config({ wifi_ssid: ssid, wifi_pass: $('txtPass').value }, 'Saved, connecting to ' + ssid);
  $('txtPass').value = '';
});
$('btnScan').addEventListener('click', async () => {
  $('btnScan').disabled = true;
  $('btnScan').textContent = 'Scanning';
  for (let i = 0; i < 20; i++) {
    const r = await api('/api/wifi/scan', undefined, { quiet: true });
    if (r && !r.scanning) {
      const nets = (r.networks || []).sort((a, b) => b.rssi - a.rssi);
      $('ssidList').innerHTML = '';
      nets.forEach((n) => { const o = document.createElement('option'); o.value = n.ssid; o.label = n.rssi + ' dBm' + (n.open ? ' (open)' : ''); $('ssidList').appendChild(o); });
      toast(nets.length + ' networks found - pick one in the SSID box', 'ok');
      break;
    }
    await new Promise((r2) => setTimeout(r2, 700));
  }
  $('btnScan').disabled = false;
  $('btnScan').textContent = 'Scan';
});

// ---- Data -----------------------------------------------------------
$('btnLogClear').addEventListener('click', async () => {
  if (confirm('Clear all logged samples?')) { const r = await api('/api/logging/clear', {}); if (r) toast('Log cleared', 'ok'); }
});

// ---- AI settings ----------------------------------------------------
$('btnKey').addEventListener('click', async () => {
  const key = $('txtKey').value.trim();
  if (key.length < 20) return toast('That does not look like a Gemini API key', 'err');
  if (await api('/api/ai/config', { api_key: key })) { $('txtKey').value = ''; toast('API key saved to flash', 'ok'); poll(); }
});
$('selModel').addEventListener('change', async (e) => {
  touch('model');
  const custom = e.target.value === '__custom';
  $('customModelRow').classList.toggle('hidden', !custom);
  if (!custom && await api('/api/ai/config', { model: e.target.value })) toast('Model: ' + e.target.value, 'ok');
});
$('btnModel').addEventListener('click', async () => {
  touch('model');
  const m = $('txtModel').value.trim();
  if (await api('/api/ai/config', { model: m })) toast('Model: ' + m, 'ok');
});
$('selInterval').addEventListener('change', (e) => { touch('intv'); api('/api/ai/config', { auto_interval_ms: +e.target.value }); });
$('chkClosedLoop').addEventListener('change', (e) => {
  touch('cl');
  if (e.target.checked && !confirm('Let Gemini adjust both pump speeds (and Time-PWM duty) automatically? Changes are slew-limited and interlocks stay active.')) {
    e.target.checked = false; return;
  }
  api('/api/ai/config', { closed_loop: e.target.checked });
});
$('chkAutoExec').addEventListener('change', (e) => {
  touch('ax');
  if (e.target.checked && !confirm('Run commands proposed by Gemini without asking first?')) { e.target.checked = false; return; }
  api('/api/ai/config', { chat_auto_exec: e.target.checked });
});
$('btnAiRun').addEventListener('click', async () => {
  const r = await api('/api/ai/query', {});
  if (r) toast('Analysis requested', 'ok');
});
$('btnAiApply').addEventListener('click', () => {
  const ai = T && T.ai;
  if (!ai || !ai.has_rec) return;
  if (!confirm(`Apply hot pump ${ai.recommended_pump_pct}%, cold pump ${ai.recommended_cold_pump_pct}% and heater Time-PWM ${ai.recommended_heater_duty}%?`)) return;
  control({
    pump_pct: ai.recommended_pump_pct,
    cold_pump_pct: ai.recommended_cold_pump_pct,
    heater_mode: 3,
    heater_duty: ai.recommended_heater_duty,
  }, 'AI suggestion applied');
});

// ======================================================================
// Chat
// ======================================================================
let chatRev = -1;
let chatBusy = false;

$('chatForm').addEventListener('submit', (e) => { e.preventDefault(); sendChat($('chatInput').value); });
qsa('#quickPrompts button').forEach((b) => b.addEventListener('click', () => sendChat(b.dataset.q)));
$('btnChatClear').addEventListener('click', async () => { if (await api('/api/chat/clear', {})) loadChat(); });

async function sendChat(text) {
  text = (text || '').trim();
  if (!text) return;
  $('chatInput').value = '';
  if (await api('/api/chat', { message: text })) loadChat();
}

function scrollChat() { const log = $('chatLog'); log.scrollTop = log.scrollHeight; }

function renderChat(msgs) {
  const log = $('chatLog');
  const atBottom = log.scrollHeight - log.scrollTop - log.clientHeight < 40;
  log.innerHTML = '';
  if (!msgs.length) {
    const d = document.createElement('div');
    d.className = 'msg system';
    d.textContent = "Ask about the rig or give an instruction. Offline commands (type 'help') work without internet; everything else goes to Gemini.";
    log.appendChild(d);
  }
  msgs.forEach((m) => {
    const d = document.createElement('div');
    d.className = 'msg ' + m.role;
    d.textContent = m.text;
    if (m.commands && m.commands.length) {
      const c = document.createElement('div');
      c.className = 'cmds';
      m.commands.forEach((cmd) => {
        const s = document.createElement('span');
        s.className = 'cmd';
        s.textContent = cmd.action + (cmd.arg ? ' ' + cmd.arg : (cmd.action.startsWith('set_') ? ' ' + cmd.value : ''));
        c.appendChild(s);
      });
      if (!m.executed) {
        const b = document.createElement('button');
        b.className = 'btn btn-sm btn-primary';
        b.textContent = 'Execute';
        b.addEventListener('click', async () => {
          b.disabled = true;
          const r = await api('/api/chat/execute', { id: m.id });
          if (r) toast(r.message, 'ok');
          loadChat(); poll();
        });
        c.appendChild(b);
      }
      d.appendChild(c);
      if (m.executed && m.results) {
        const r = document.createElement('div');
        r.className = 'res';
        r.textContent = '→ ' + m.results;
        d.appendChild(r);
      }
    }
    log.appendChild(d);
  });
  if (atBottom || activeTab === 'assistant') scrollChat();
}

async function loadChat() {
  const r = await api('/api/chat', undefined, { quiet: true });
  if (!r) return;
  if (r.rev !== chatRev && chatRev >= 0 && activeTab !== 'assistant') $('chatDot').classList.remove('hidden');
  chatRev = r.rev;
  chatBusy = r.busy;
  renderChat(r.messages || []);
  $('chatTyping').classList.toggle('hidden', !r.busy);
  $('chatPhase').textContent = r.phase || 'thinking';
  if (!isTouched('ax')) $('chkAutoExec').checked = !!r.auto_exec;
}

// ======================================================================
// Chart
// ======================================================================
const SERIES = [
  { key: 'thIn', label: 'Th,in', color: '--hot', axis: 't', on: true },
  { key: 'thOut', label: 'Th,out', color: '--hot-2', axis: 't', on: true },
  { key: 'tcIn', label: 'Tc,in', color: '--cold', axis: 't', on: true },
  { key: 'tcOut', label: 'Tc,out', color: '--cold-2', axis: 't', on: true },
  { key: 'sp', label: 'Setpoint', color: '--muted', axis: 't', on: true, dash: [5, 4] },
  { key: 'flow', label: 'Flow hot', color: '--flow', axis: 'f', on: true },
  { key: 'flowCold', label: 'Flow cold', color: '--flow-cold', axis: 'f', on: true },
];
const history = [];
const MAX_HISTORY = 600;
let windowSec = 60;
let hoverX = null;

SERIES.forEach((s) => {
  const b = document.createElement('button');
  b.innerHTML = `<i style="background:var(${s.color})"></i>${s.label}`;
  b.addEventListener('click', () => { s.on = !s.on; b.classList.toggle('off', !s.on); drawChart(); });
  $('legend').appendChild(b);
});
qsa('#segWindow button').forEach((b) => b.addEventListener('click', () => {
  windowSec = +b.dataset.win;
  qsa('#segWindow button').forEach((x) => x.classList.toggle('active', x === b));
  drawChart();
}));
const canvas = $('chart');
canvas.addEventListener('mousemove', (e) => { hoverX = e.offsetX; drawChart(); });
canvas.addEventListener('mouseleave', () => { hoverX = null; $('chartTip').classList.add('hidden'); drawChart(); });
window.addEventListener('resize', () => drawChart());

function cssVar(name) { return getComputedStyle(document.documentElement).getPropertyValue(name).trim(); }

function niceRange(min, max, minSpan) {
  if (!isFinite(min)) { min = 0; max = minSpan; }
  if (max - min < minSpan) { const c = (max + min) / 2; min = c - minSpan / 2; max = c + minSpan / 2; }
  const step = Math.pow(10, Math.floor(Math.log10((max - min) / 4)));
  const s = [1, 2, 5, 10].map((m) => m * step).find((x) => (max - min) / x <= 6) || step * 10;
  return { min: Math.floor(min / s) * s, max: Math.ceil(max / s) * s, step: s };
}

function drawChart() {
  if (activeTab !== 'overview') return;
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth, h = canvas.clientHeight;
  if (!w) return;
  canvas.width = w * dpr; canvas.height = h * dpr;
  const ctx = canvas.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);

  const pad = { l: 40, r: 44, t: 10, b: 22 };
  const pw = w - pad.l - pad.r, ph = h - pad.t - pad.b;
  const now = history.length ? history[history.length - 1].t : 0;
  const pts = history.filter((p) => p.t >= now - windowSec);
  const grid = cssVar('--border'), muted = cssVar('--muted');
  ctx.font = '11px system-ui, sans-serif';

  let tmin = Infinity, tmax = -Infinity, fmax = 1;
  pts.forEach((p) => {
    SERIES.forEach((s) => {
      if (!s.on || s.axis !== 't' || !isTemp(p[s.key])) return;
      tmin = Math.min(tmin, p[s.key]); tmax = Math.max(tmax, p[s.key]);
    });
    fmax = Math.max(fmax, p.flow || 0, p.flowCold || 0);
  });
  const ty = niceRange(tmin, tmax, 5);
  const fy = niceRange(0, fmax * 1.1, 1);
  fy.min = 0;

  const xOf = (t) => pad.l + pw * (1 - (now - t) / windowSec);
  const yT = (v) => pad.t + ph * (1 - (v - ty.min) / (ty.max - ty.min));
  const yF = (v) => pad.t + ph * (1 - (v - fy.min) / (fy.max - fy.min));

  ctx.strokeStyle = grid; ctx.fillStyle = muted; ctx.lineWidth = 1;
  ctx.textAlign = 'right'; ctx.textBaseline = 'middle';
  for (let v = ty.min; v <= ty.max + 1e-9; v += ty.step) {
    const y = Math.round(yT(v)) + 0.5;
    ctx.beginPath(); ctx.moveTo(pad.l, y); ctx.lineTo(w - pad.r, y); ctx.stroke();
    ctx.fillText(v.toFixed(ty.step < 1 ? 1 : 0) + '°', pad.l - 6, y);
  }
  ctx.textAlign = 'left';
  for (let v = fy.min; v <= fy.max + 1e-9; v += fy.step) ctx.fillText(v.toFixed(fy.step < 1 ? 1 : 0), w - pad.r + 6, yF(v));
  ctx.save(); ctx.fillStyle = cssVar('--flow'); ctx.fillText('L/min', w - pad.r + 6, pad.t + 2); ctx.restore();
  ctx.textAlign = 'center'; ctx.textBaseline = 'top';
  const tickEvery = windowSec <= 60 ? 15 : windowSec <= 300 ? 60 : 120;
  for (let s = 0; s <= windowSec; s += tickEvery) ctx.fillText(s === 0 ? 'now' : '-' + (s >= 60 ? s / 60 + 'm' : s + 's'), xOf(now - s), h - pad.b + 6);

  if (pts.length < 2) {
    ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
    ctx.fillText('Collecting data...', pad.l + pw / 2, pad.t + ph / 2);
    return;
  }

  SERIES.forEach((s) => {
    if (!s.on) return;
    const yf = s.axis === 't' ? yT : yF;
    ctx.strokeStyle = cssVar(s.color); ctx.lineWidth = s.key === 'sp' ? 1.25 : 2;
    ctx.setLineDash(s.dash || []);
    ctx.beginPath();
    let pen = false;
    pts.forEach((p) => {
      const v = p[s.key];
      const okv = s.axis === 't' ? isTemp(v) : typeof v === 'number';
      if (!okv) { pen = false; return; }
      const x = xOf(p.t), y = yf(v);
      if (pen) ctx.lineTo(x, y); else ctx.moveTo(x, y);
      pen = true;
    });
    ctx.stroke();
  });
  ctx.setLineDash([]);

  const tip = $('chartTip');
  if (hoverX !== null && hoverX >= pad.l && hoverX <= w - pad.r) {
    const tAt = now - windowSec * (1 - (hoverX - pad.l) / pw);
    const p = pts.reduce((a, b) => (Math.abs(b.t - tAt) < Math.abs(a.t - tAt) ? b : a));
    const x = xOf(p.t);
    ctx.strokeStyle = muted; ctx.beginPath(); ctx.moveTo(x, pad.t); ctx.lineTo(x, pad.t + ph); ctx.stroke();
    tip.innerHTML = `<b>-${Math.round(now - p.t)} s</b><br>` + SERIES.filter((s) => s.on).map((s) =>
      `<span style="color:var(${s.color})">■</span> ${s.label}: ${s.axis === 't' ? (isTemp(p[s.key]) ? p[s.key].toFixed(2) + ' °C' : 'fault') : fmt(p[s.key], 2) + ' L/min'}`).join('<br>');
    tip.classList.remove('hidden');
    const tw = tip.offsetWidth;
    tip.style.left = (x + 12 + tw > w ? x - tw - 12 : x + 12) + 'px';
  } else {
    tip.classList.add('hidden');
  }
}

// ======================================================================
// Telemetry
// ======================================================================
let T = null;
let polling = false;
let failures = 0;
let offline = false;

function setChip(id, text, cls) { const el = $(id); el.textContent = text; el.className = 'chip ' + (cls || ''); }

function setOffline(on) {
  if (on === offline) return;
  offline = on;
  if (on) {
    setChip('chipSafety', 'OFFLINE', 'chip-warn');
    setChip('chipNet', 'OFFLINE', 'chip-warn');
    $('statusDot').classList.remove('hidden');
    if ($('statOverall')) { $('statOverall').textContent = 'OFFLINE'; $('statOverall').className = 'pill wait'; }
    if ($('statConn')) { $('statConn').textContent = 'OFFLINE'; $('statConn').className = 'pill wait'; }
  }
}

function renderStatus(d) {
  const ai = d.ai;
  // ---- system ----
  $('statOverall').textContent = d.simulated ? 'SIM' : 'HW';
  $('statOverall').className = 'pill ' + (d.simulated ? 'sim' : 'on');
  const found = d.probes.filter((p) => p.present).length;
  const sysRows = [
    ['Source', d.simulated ? (d.sys.sim_fault_fallback ? 'Simulation (hardware fault fallback)' : 'Simulation (virtual plant)') : 'Real hardware'],
    ['Simulation mode', SIM_MODES[d.sys.sim_mode] || '--'],
    ['Uptime', formatDuration(d.uptime_s)],
    ['Free heap', (d.sys.heap / 1024).toFixed(0) + ' kB'],
    ['LCD', d.sys.lcd ? 'online' : 'not detected'],
    ['Probes found', found + ' / 4'],
    ['Safety trips', d.safety.trips],
  ];
  if (d.simulated) sysRows.push(['Virtual tank', fmt(d.sys.sim_tank, 1) + ' °C']);
  $('statTable').innerHTML = sysRows.map(([k, v]) => `<tr><td>${k}</td><td>${v}</td></tr>`).join('');

  // ---- safety ----
  const tripped = d.safety.tripped;
  $('statSafety').textContent = tripped ? 'TRIPPED' : 'NORMAL';
  $('statSafety').className = 'pill ' + (tripped ? 'trip' : 'on');
  $('statTripBox').classList.toggle('hidden', !tripped);
  $('statTripText').textContent = tripped ? d.safety.error : '';
  $('statSafetyExtra').textContent = tripped
    ? 'The heater stays locked out until the cause is gone and the trip is reset.'
    : 'All latching interlocks are clear.';

  // ---- warnings / notices ----
  const items = [];
  if (d.simulated && d.sys.sim_fault_fallback) {
    items.push(['Auto-fallback: a hardware fault was detected, so the controller is running the virtual plant. It returns to real hardware automatically when the fault clears.', 'sim']);
  }
  if (d.simulated) items.push(['Simulation active: readings come from the virtual plant and physical outputs are held off.', 'sim']);
  if (d.safety.warnings & 1) items.push(['Heater is requesting heat but is held off: hot flow is below 0.3 L/min. Start the hot pump.', '']);
  if (d.safety.warnings & 2) items.push(['Hot pump is running but no hot flow is detected. Check for air, a closed valve or the flow sensor.', '']);
  if (d.safety.warnings & 4) items.push(['A cold-side probe is faulty; heat-balance numbers are unreliable.', '']);
  if (d.safety.warnings & 8) items.push(['The cold loop has no flow while the hot side is running. Start the cold pump.', '']);
  if (d.sys.sim_faults) items.push(['Injected simulation fault(s): ' + faultList(d.sys.sim_faults) + '.', 'sim']);
  if (!items.length) items.push(['No warnings. All monitored conditions are normal.', 'ok']);
  $('statWarnings').innerHTML = items.map(([t, c]) => `<li class="${c}">${t}</li>`).join('');

  // ---- probes ----
  const probeTemps = [d.th_in, d.th_out, d.tc_in, d.tc_out];
  $('statProbes').innerHTML = d.probes.map((p, i) => {
    const state = p.valid ? 'OK' : (p.present ? 'INVALID' : 'MISSING');
    const val = isTemp(probeTemps[i]) && p.valid ? probeTemps[i].toFixed(1) + ' °C' : '--';
    return `<span class="probe ${p.valid ? 'ok' : 'bad'}" title="${p.rom !== '0000000000000000' ? 'ROM ' + p.rom : 'not detected'}; ${p.errors} read errors">${PROBE_NAMES[i]} · ${val} · ${state}</span>`;
  }).join('');

  // ---- connection & AI ----
  $('statConn').textContent = d.net.sta_connected ? 'STA' : 'AP';
  $('statConn').className = 'pill ' + (d.net.sta_connected ? 'on' : '');
  const netRows = [
    ['Wi-Fi mode', d.net.sta_connected ? 'Station' : 'Access point'],
    ['IP address', d.net.sta_connected ? d.net.sta_ip : d.net.ap_ip],
    ['SSID', d.net.ssid || 'not set'],
    ['Signal', d.net.sta_connected ? d.net.rssi + ' dBm' : '—'],
    ['AI key', ai.has_key ? 'present (' + ai.key_hint + ')' : 'not set'],
    ['AI model', ai.model],
    ['AI state', ai.busy ? ai.phase : (ai.last_error && ai.last_error !== 'OK' ? ai.last_error : 'idle')],
    ['AI requests', ai.requests + ' requests, ' + ai.errors + ' errors'],
  ];
  $('statNet').innerHTML = netRows.map(([k, v]) => `<tr><td>${k}</td><td>${v}</td></tr>`).join('');
}

function render(d) {
  // ---- header chips ----
  setChip('chipSource', d.simulated ? 'SIM' : 'HW', d.simulated ? 'chip-sim' : 'chip-ok');
  setChip('chipSafety', d.safety.tripped ? 'TRIPPED' : 'NORMAL', d.safety.tripped ? 'chip-trip' : 'chip-ok');
  setChip('chipNet', d.net.sta_connected ? 'STA ' + d.net.sta_ip : 'AP ' + d.net.ap_ip, d.net.sta_connected ? 'chip-on' : '');
  const ai = d.ai;
  setChip('chipAi', ai.busy ? 'AI ' + ai.phase : (ai.has_key ? 'AI ready' : 'AI no key'), ai.busy ? 'chip-on' : (ai.has_key ? '' : 'chip-warn'));
  if (activeTab !== 'status') {
    const issue = d.safety.tripped || d.safety.warnings || (d.simulated && d.sys.sim_fault_fallback);
    $('statusDot').classList.toggle('hidden', !issue);
  }

  // ---- schematic ----
  const temps = [d.th_in, d.th_out, d.tc_in, d.tc_out];
  ['sxThIn', 'sxThOut', 'sxTcIn', 'sxTcOut'].forEach((id, i) => {
    const ok = isTemp(temps[i]) && d.probes[i].valid;
    $(id).textContent = PROBE_NAMES[i] + ' ' + (ok ? temps[i].toFixed(1) + '°' : 'FAULT');
    $(id).parentNode.classList.toggle('bad', !ok);
  });
  const hotFlowing = d.flow_lpm > 0.05;
  const coldFlowing = d.flow_cold_lpm > 0.05;
  qsa('.flowline').forEach((p) => {
    const isCold = p.classList.contains('cold') || p.classList.contains('cold-ret');
    p.classList.toggle('moving', isCold ? coldFlowing : hotFlowing);
  });
  $('sxFlowHot').textContent = fmt(d.flow_lpm, 2) + ' L/min';
  $('sxFlowCold').textContent = fmt(d.flow_cold_lpm, 2) + ' L/min';
  $('sxPump').classList.toggle('on', d.pump.pct > 0);
  $('sxImpeller').classList.toggle('on', d.pump.pct > 0);
  $('sxPumpTxt').textContent = 'hot pump ' + d.pump.pct + '%';
  $('sxColdPump').classList.toggle('on', d.pump_cold.pct > 0);
  $('sxColdImpeller').classList.toggle('on', d.pump_cold.pct > 0);
  $('sxColdPumpTxt').textContent = 'cold pump ' + d.pump_cold.pct + '%';
  $('sxCoil').classList.toggle('on', d.heater.on);
  $('sxHeater').textContent = d.heater.on ? 'heater ON' : (d.heater.demand ? 'heater held off' : 'heater OFF');
  const tankT = isTemp(d.th_in) ? d.th_in : 25;
  const heat = Math.max(0, Math.min(1, (tankT - 20) / 55));
  $('sxTankFill').style.fill = `color-mix(in srgb, var(--hot) ${Math.round(15 + heat * 60)}%, var(--cold) ${Math.round((1 - heat) * 40)}%)`;
  $('lblUptime').textContent = 'uptime ' + formatDuration(d.uptime_s);

  // ---- KPIs ----
  $('kQh').textContent = fmt(d.q_hot_w, 0);
  $('kQc').textContent = fmt(d.q_cold_w, 0);
  $('kEff').textContent = fmt(d.effectiveness, 1);
  $('kLmtd').textContent = fmt(d.lmtd, 2);
  $('kUa').textContent = fmt(d.ua, 1);
  $('kBal').textContent = d.heat_balance > 0 ? fmt(d.heat_balance, 0) : '--';
  $('kFlow').textContent = fmt(d.flow_lpm, 2);
  $('kFlowCold').textContent = fmt(d.flow_cold_lpm, 2);
  $('kVol').textContent = fmt(d.total_liters, 1);
  $('kVolCold').textContent = fmt(d.total_liters_cold, 1);
  $('lblThermoValid').textContent = d.thermo_valid ? '' : 'needs all 4 probes';
  $('probeRow').innerHTML = d.probes.map((p, i) => {
    const hw = d.simulated ? 'sim' : 'GPIO ' + p.pin;
    return `<span class="probe ${p.valid ? 'ok' : 'bad'}" title="${p.rom !== '0000000000000000' ? 'ROM ' + p.rom : 'not detected'}; ${p.errors} read errors">${PROBE_NAMES[i]} · ${hw} · ${p.valid ? 'OK' : (p.present ? 'ERR' : 'MISSING')}</span>`;
  }).join('');

  // ---- hot pump ----
  if (!isTouched('pmode')) setPumpModeUi(d.pump.mode);
  if (!isTouched('pump') && d.pump.mode === 0) { $('rngPump').value = d.pump.target_pct; $('lblPumpCmd').textContent = d.pump.target_pct; }
  if (!isTouched('flowsp') && d.pump.mode === 1 && document.activeElement !== $('numFlowSp')) $('numFlowSp').value = d.pump.flow_setpoint.toFixed(1);
  $('barPump').style.width = d.pump.pct + '%';
  $('lblPumpAct').textContent = d.pump.pct + '%';
  $('barFlow').style.width = Math.min(100, d.flow_lpm / 10 * 100) + '%';
  $('lblFlowAct').textContent = fmt(d.flow_lpm, 2);
  const ps = $('lblPumpState');
  ps.textContent = d.pump.pct > 0 ? (d.pump.mode === 1 ? 'FLOW LOOP' : 'RUNNING') : 'STOPPED';
  ps.className = 'pill ' + (d.pump.pct > 0 ? 'on' : '');

  // ---- cold pump ----
  const cp = d.pump_cold;
  if (!isTouched('cpmode')) setColdPumpModeUi(cp.mode);
  if (!isTouched('cpump') && cp.mode === 0) { $('rngColdPump').value = cp.target_pct; $('lblColdPumpCmd').textContent = cp.target_pct; }
  if (!isTouched('cflowsp') && cp.mode === 1 && document.activeElement !== $('numColdFlowSp')) $('numColdFlowSp').value = cp.flow_setpoint.toFixed(1);
  $('barColdPump').style.width = cp.pct + '%';
  $('lblColdPumpAct').textContent = cp.pct + '%';
  $('barColdFlow').style.width = Math.min(100, d.flow_cold_lpm / 10 * 100) + '%';
  $('lblColdFlowAct').textContent = fmt(d.flow_cold_lpm, 2);
  const cps = $('lblColdPumpState');
  cps.textContent = cp.pct > 0 ? (cp.mode === 1 ? 'FLOW LOOP' : 'RUNNING') : 'STOPPED';
  cps.className = 'pill ' + (cp.pct > 0 ? 'on' : '');

  // ---- heater ----
  const h = d.heater;
  if (!isTouched('hmode')) setHeaterModeUi(h.mode);
  if (!isTouched('sp')) { $('rngSp').value = h.target; $('lblSp').textContent = h.target.toFixed(1); }
  if (!isTouched('duty')) { $('rngDuty').value = h.duty_setting; $('lblDutyCmd').textContent = Math.round(h.duty_setting); }
  $('lblCtrlProbe').textContent = 'on ' + PROBE_NAMES[d.cfg.control_probe];
  $('barDuty').style.width = h.duty + '%';
  $('lblDutyAct').textContent = Math.round(h.duty) + '%';
  const hs = $('lblHeaterState');
  hs.textContent = h.on ? 'RELAY ON' : (h.demand ? 'HELD OFF' : HEATER_MODES[h.mode].toUpperCase());
  hs.className = 'pill ' + (h.on ? 'hot' : (h.demand ? 'wait' : ''));
  const probeOk = d.probes[d.cfg.control_probe].valid;
  const checks = [
    [h.mode !== 0, 'Mode armed (' + HEATER_MODES[h.mode] + ')'],
    [d.flow_lpm >= 0.3, 'Hot flow permissive ≥ 0.30 L/min (now ' + fmt(d.flow_lpm, 2) + ')'],
    [!d.safety.tripped, d.safety.tripped ? 'Safety trip: ' + d.safety.error : 'No safety trip'],
    [probeOk, 'Control probe ' + PROBE_NAMES[d.cfg.control_probe] + (probeOk ? ' valid' : ' faulty')],
    [h.demand, h.demand ? 'Controller demanding heat' : 'Controller satisfied / no demand'],
  ];
  $('heaterChecks').innerHTML = checks.map(([ok, t], i) => `<li class="${i === 4 ? (ok ? 'ok' : 'na') : (ok ? 'ok' : 'no')}">${t}</li>`).join('');

  // ---- tuning / calibration (fill once, or when not being edited) ----
  if (!cfgLoaded || !isTouched('cfg', 15000)) {
    const c = d.cfg;
    const set = (id, v) => { if (document.activeElement !== $(id)) $(id).value = v; };
    set('cfgKp', c.kp); set('cfgKi', c.ki); set('cfgKd', c.kd); set('cfgHyst', c.hyst);
    set('cfgProbe', c.control_probe); set('cfgFkp', c.flow_kp); set('cfgFki', c.flow_ki);
    set('cfgFkp2', c.cold_flow_kp); set('cfgFki2', c.cold_flow_ki);
    set('cfgK', c.k_factor); set('cfgColdK', c.cold_k_factor);
    set('cfgPmin', c.pump_min_duty); set('cfgColdPmin', c.cold_pump_min_duty);
    $('rngSp').max = Math.floor(c.max_temp - 2);
    cfgLoaded = true;
  }

  // ---- simulation ----
  qsa('#segSim button').forEach((b) => b.classList.toggle('active', +b.dataset.sim === d.sys.sim_mode));
  const sl = $('lblSimState');
  sl.textContent = d.simulated ? (d.sys.sim_fault_fallback ? 'SIM (FAULT)' : 'SIMULATING') : 'HARDWARE';
  sl.className = 'pill ' + (d.simulated ? 'sim' : 'on');
  if (!isTouched('faults')) qsa('[data-fault]').forEach((c) => { c.checked = !!(d.sys.sim_faults & +c.dataset.fault); c.disabled = !d.simulated; });

  // ---- Wi-Fi ----
  const st = $('lblSta');
  st.textContent = d.net.sta_connected ? 'CONNECTED ' + d.net.rssi + ' dBm' : (d.net.ssid ? 'NOT CONNECTED' : 'NOT SET');
  st.className = 'pill ' + (d.net.sta_connected ? 'on' : 'wait');
  if (!$('txtSsid').value && d.net.ssid && document.activeElement !== $('txtSsid')) $('txtSsid').value = d.net.ssid;

  // ---- AI ----
  $('lblKey').textContent = ai.has_key ? '✓ key ' + ai.key_hint : '⚠ no key';
  $('lblKey').style.color = ai.has_key ? 'var(--ok)' : 'var(--warn)';
  if (!isTouched('model')) {
    const known = qsa('#selModel option').some((o) => o.value === ai.model);
    $('selModel').value = known ? ai.model : '__custom';
    $('customModelRow').classList.toggle('hidden', known);
    if (!known && document.activeElement !== $('txtModel')) $('txtModel').value = ai.model;
  }
  if (!isTouched('intv')) {
    const iv = ai.auto_enabled ? String(ai.interval_ms) : '0';
    if (qsa('#selInterval option').some((o) => o.value === iv)) $('selInterval').value = iv;
  }
  if (!isTouched('cl')) $('chkClosedLoop').checked = ai.closed_loop;
  $('btnAiRun').disabled = ai.busy || !!ai.unavailable;
  $('btnAiRun').textContent = ai.busy ? 'Working...' : 'Analyse now';
  let status = ai.unavailable ? ai.unavailable : (ai.busy ? 'Gemini: ' + ai.phase + '...' : '');
  if (!ai.busy && ai.last_error && ai.last_error !== 'OK') status = 'Last request: ' + ai.last_error;
  else if (!ai.busy && ai.requests) status = status || `Last reply in ${(ai.latency_ms / 1000).toFixed(1)} s · ${ai.requests} requests, ${ai.errors} errors`;
  $('aiStatus').textContent = status;
  if (ai.has_rec) {
    $('lblAiScore').textContent = Math.round(ai.efficiency_score) + '/100';
    const box = $('aiReport');
    box.classList.remove('muted');
    box.innerHTML = '';
    box.append(document.createTextNode(ai.analysis));
    const rec = document.createElement('div');
    rec.className = 'rec';
    rec.textContent = `${ai.suggested_action}: hot pump ${ai.recommended_pump_pct}%, cold pump ${ai.recommended_cold_pump_pct}%, heater ${ai.recommended_heater_duty}% · ${formatDuration(ai.age_s)} ago`;
    box.append(rec);
    if (ai.safety_warning && ai.safety_warning.toUpperCase() !== 'NONE') {
      const w = document.createElement('div'); w.className = 'warn'; w.textContent = '⚠ ' + ai.safety_warning; box.append(w);
    }
    $('btnAiApply').classList.remove('hidden');
  }
  if (ai.chat_rev !== chatRev || chatBusy || ai.busy) loadChat();

  // ---- data / system ----
  $('lblLog').textContent = d.sys.log_count + ' / ' + d.sys.log_capacity;
  $('barLog').style.width = (d.sys.log_count / d.sys.log_capacity * 100) + '%';
  const rows = [
    ['Source', d.simulated ? 'Simulation (tank ' + fmt(d.sys.sim_tank, 1) + ' °C)' : 'Hardware'],
    ['Uptime', formatDuration(d.uptime_s)],
    ['Free heap', (d.sys.heap / 1024).toFixed(0) + ' kB'],
    ['LCD', d.sys.lcd ? 'online' : 'not detected'],
    ['AP / STA IP', d.net.ap_ip + ' / ' + (d.net.sta_ip || '-')],
    ['Hot pump PWM raw', d.pump.duty_raw + ' / 1023'],
    ['Cold pump PWM raw', d.pump_cold.duty_raw + ' / 1023'],
    ['Safety trips', d.safety.trips],
    ...d.probes.map((p, i) => [PROBE_NAMES[i] + ' GPIO ' + p.pin, (p.present ? p.rom : 'not found') + (p.errors ? ' · ' + p.errors + ' err' : '')]),
  ];
  $('sysTable').innerHTML = rows.map(([k, v]) => `<tr><td>${k}</td><td>${v}</td></tr>`).join('');

  // ---- status tab ----
  renderStatus(d);
}

function formatDuration(s) {
  s = Math.max(0, Math.round(s || 0));
  if (s < 60) return s + ' s';
  if (s < 3600) return Math.floor(s / 60) + ' m ' + (s % 60) + ' s';
  return Math.floor(s / 3600) + ' h ' + Math.floor((s % 3600) / 60) + ' m';
}

async function poll() {
  if (polling) return;
  polling = true;
  const d = await api('/api/telemetry', undefined, { quiet: true, timeout: 4000 });
  polling = false;
  if (!d) {
    if (++failures >= 3) setOffline(true);
    return;
  }
  failures = 0;
  setOffline(false);
  T = d;
  if (!history.length || d.uptime_s !== history[history.length - 1].t) {
    history.push({ t: d.uptime_s, thIn: d.th_in, thOut: d.th_out, tcIn: d.tc_in, tcOut: d.tc_out,
      flow: d.flow_lpm, flowCold: d.flow_cold_lpm,
      sp: (d.heater.mode === 2 || d.heater.mode === 4) ? d.heater.target : null });
    if (history.length > MAX_HISTORY) history.shift();
  }
  render(d);
  drawChart();
}

poll();
loadChat();
setInterval(poll, 1000);