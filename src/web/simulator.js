/*
 * Hosted client-side simulator for the Heat Exchanger dashboard.
 *
 * This is a JavaScript port of the firmware's virtual rig
 * (src/sim/PlantSimulator.hpp, Thermodynamics.hpp, SafetyManager.hpp,
 * PumpController.hpp, HeaterController.hpp, Pid.hpp). It intercepts the
 * dashboard's /api/* requests and answers them locally, so the exact same UI
 * (KPIs, trend chart, controls, status tab, CSV export) runs on any static host
 * - GitHub Pages, a file:// open, wherever - with no ESP32 attached.
 *
 * Load it BEFORE app.js.
 */
(function () {
  'use strict';

  // =====================================================================
  // Constants (mirror src/config.h + PlantSimulator.hpp)
  // =====================================================================
  var K = {
    cp: 4184, rho: 1.0,
    heaterW: 2000, tankKg: 3.0, hxUA: 180, tankLoss: 4.0,
    ambient: 25, coldSupply: 20, maxFlow: 8.0, flowTau: 1.5, probeTau: 3.0,
    minFlowHeater: 0.3, minColdFlow: 0.3, zeroFlowTimeoutMs: 1500,
    maxSafeTemp: 75, minPlausible: -10, maxPlausible: 110, disconnect: -127,
    bootGraceMs: 4000, pumpDryRunMs: 15000,
    pumpMinDuty: 25, pumpRampPctPerS: 50, pumpKickstartMs: 250,
    heaterWindowMs: 10000, heaterMinPulseMs: 1000, heaterMinSwitchMs: 5000,
    pwmMax: 1023,
    logCapacity: 600,
    resolutionBits: 12
  };

  var PROBE_NAMES = ['Hot In', 'Hot Out', 'Cold In', 'Cold Out'];
  var PROBE_ROMS = ['28FF64A1B2C3D401', '28FF64A1B2C3D402', '28FF64A1B2C3D403', '28FF64A1B2C3D404'];

  // Fault bits (mirror SimFault)
  var F_HOT_BLOCKED = 1, F_THIN_LOST = 2, F_RELAY_WELD = 4, F_COLD_BLOCKED = 8;

  // =====================================================================
  // Helpers
  // =====================================================================
  var now = function () { return performance.now(); };
  function clamp(v, a, b) { return v < a ? a : (v > b ? b : v); }
  function round16(v) { return Math.round(v * 16) / 16; }

  // Discrete PID (mirror Pid.hpp): D on measurement, conditional-integration
  // anti-windup, bumpless reset.
  function Pid(kp, ki, kd, outMin, outMax) {
    this.kp = kp; this.ki = ki; this.kd = kd;
    this.outMin = outMin; this.outMax = outMax;
    this.integral = 0; this.prevMeas = 0; this.dFiltered = 0; this.output = 0; this.primed = false;
  }
  Pid.prototype.setGains = function (kp, ki, kd) {
    if (ki > 1e-6 && this.ki > 1e-6) this.integral *= this.ki / ki;
    this.kp = kp; this.ki = ki; this.kd = kd;
  };
  Pid.prototype.reset = function (init) {
    init = init || 0;
    this.integral = this.ki > 1e-6 ? clamp(init, this.outMin, this.outMax) / this.ki : 0;
    this.dFiltered = 0; this.primed = false; this.output = init;
  };
  Pid.prototype.update = function (sp, meas, dt) {
    if (dt <= 0) return this.output;
    if (!this.primed) { this.prevMeas = meas; this.primed = true; }
    var err = sp - meas;
    var dMeas = (meas - this.prevMeas) / dt; this.prevMeas = meas;
    this.dFiltered += 0.2 * (dMeas - this.dFiltered);
    var p = this.kp * err, d = -this.kd * this.dFiltered;
    var unsat = p + this.ki * this.integral + d;
    var pHi = unsat >= this.outMax && err > 0;
    var pLo = unsat <= this.outMin && err < 0;
    if (!pHi && !pLo) this.integral += err * dt;
    this.output = clamp(p + this.ki * this.integral + d, this.outMin, this.outMax);
    return this.output;
  };

  // =====================================================================
  // State
  // =====================================================================
  var plant = {
    tankC: K.ambient, flowHot: 0, flowCold: 0, totalHot: 0, totalCold: 0,
    probe: [K.ambient, K.ambient, K.coldSupply, K.coldSupply], heaterPower: 0
  };
  var thermo = { thIn: 0, thOut: 0, tcIn: 0, tcOut: 0, deltaTh: 0, deltaTc: 0, valid: false,
                 qHot: 0, qCold: 0, lmtd: 0, ua: 0, eff: 0, balance: 0 };

  function newPump(minDuty, kp, ki) {
    return { mode: 0, target: 0, cur: 0, flowSp: 0, minDuty: minDuty,
             pid: new Pid(kp, ki, 0, 0, 100), kickUntil: 0, driving: false, dutyRaw: 0 };
  }
  var pump = newPump(K.pumpMinDuty, 8, 4);
  var pumpCold = newPump(K.pumpMinDuty, 8, 4);

  var heater = { mode: 0, manual: false, target: 50, hyst: 1, dutySetting: 0, dutyOutput: 0,
                 demand: false, relay: false, kp: 8, ki: 0.05, kd: 0, controlProbe: 0,
                 lastSwitch: 0, windowStart: now(), lastPidT: now(),
                 pid: new Pid(8, 0.05, 0, 0, 100) };

  var safety = { err: 0, warnings: 0, trips: 0, lowFlowStart: 0, dryRunStart: 0,
                 boot: now(), permissive: false, tripDetail: 0, maxValidTemp: -999 };

  var simMode = 2, simFaults = 0;
  var kHot = 7.5, kCold = 7.5;
  var simStart = now();
  var lastTick = now(), lastLog = now();

  // Boot into a running example so the hosted page produces data straight away
  // (heater PID to 50 C, hot pump 60 %, cold pump 50 %). Change or stop it from
  // the UI exactly like the real rig.
  pump.target = 60;
  pumpCold.target = 50;
  heater.mode = 4;
  heater.target = 50;
  heater.windowStart = now();
  heater.lastPidT = now();

  var log = [];
  var chatRev = 0, chatMsgs = [];

  // =====================================================================
  // Gemini (browser -> Google directly; key stays in localStorage)
  // =====================================================================
  var AI_STORE = 'hx_sim_ai';
  var ai = {
    key: '', model: 'gemini-3.6-flash', auto: false, interval: 60000,
    closed: false, chatAuto: false,
    busy: false, phase: '', lastError: 'OK', latency: 0, requests: 0, errors: 0,
    lastAuto: now(), seq: 0, rec: null
  };
  var appliedSeq = 0;

  function loadAi() {
    try {
      var s = JSON.parse(localStorage.getItem(AI_STORE) || '{}');
      if (typeof s.key === 'string') ai.key = s.key;
      if (typeof s.model === 'string' && s.model) ai.model = s.model;
      ai.auto = !!s.auto;
      ai.closed = !!s.closed;
      ai.chatAuto = !!s.chatAuto;
      if (typeof s.interval === 'number' && s.interval > 0) ai.interval = s.interval;
    } catch (e) { /* localStorage unavailable */ }
  }
  function saveAi() {
    try {
      localStorage.setItem(AI_STORE, JSON.stringify({
        key: ai.key, model: ai.model, auto: ai.auto, interval: ai.interval,
        closed: ai.closed, chatAuto: ai.chatAuto
      }));
    } catch (e) { /* ignore */ }
  }
  loadAi();

  function aiHasKey() { return ai.key.length >= 20; }

  var ANALYSIS_PROMPT =
    'You are a thermal process engineer supervising a small liquid-to-liquid counter-flow heat exchanger ' +
    'test rig (browser simulation). A relay-switched electric heater warms a hot loop driven by one pump; a ' +
    'second pump drives the cold loop. Using only the telemetry given, assess heat transfer (Q, LMTD, UA, ' +
    'effectiveness, heat balance), spot anomalies, and recommend hot-pump %, cold-pump % and heater duty %. ' +
    'Keep analysis under 60 words. safety_warning is NONE unless there is a concrete risk.';

  var CHAT_PROMPT =
    'You are the operator assistant for an ESP32-style heat exchanger test rig running in a browser ' +
    'simulator. Answer briefly from the LIVE TELEMETRY block; never invent readings. If the operator asks ' +
    'to change the system, put the changes in `commands` using only these actions: set_pump_pct (value ' +
    '0-100), set_flow_setpoint (value L/min 0-30), pump_stop, set_cold_pump_pct (value 0-100), ' +
    'set_cold_flow_setpoint (value L/min 0-30), cold_pump_stop, set_heater_mode (arg: off, manual, ' +
    'thermostatic, time_pwm, pid), set_heater_manual (arg on/off; heater must already be in manual mode), ' +
    'set_target_temp (value C 10-73), set_heater_duty (value 0-100, time_pwm mode), estop, clear_log. To ' +
    'heat to a temperature, prefer pid mode with set_target_temp and make sure the pumps are running. ' +
    'Questions only => empty commands. Plain text, no markdown, under 100 words.';

  var ANALYSIS_SCHEMA = {
    type: 'OBJECT',
    properties: {
      analysis: { type: 'STRING' },
      efficiency_score: { type: 'NUMBER' },
      recommended_pump_pct: { type: 'INTEGER' },
      recommended_cold_pump_pct: { type: 'INTEGER' },
      recommended_heater_duty: { type: 'INTEGER' },
      safety_warning: { type: 'STRING' },
      action: { type: 'STRING', enum: ['MAINTAIN', 'INCREASE_FLOW', 'DECREASE_FLOW', 'INCREASE_HEAT', 'DECREASE_HEAT'] }
    },
    required: ['analysis', 'efficiency_score', 'recommended_pump_pct', 'recommended_heater_duty', 'safety_warning', 'action']
  };
  var CHAT_SCHEMA = {
    type: 'OBJECT',
    properties: {
      reply: { type: 'STRING' },
      commands: {
        type: 'ARRAY',
        items: {
          type: 'OBJECT',
          properties: {
            action: {
              type: 'STRING',
              enum: ['set_pump_pct', 'set_flow_setpoint', 'pump_stop', 'set_cold_pump_pct',
                     'set_cold_flow_setpoint', 'cold_pump_stop', 'set_heater_mode', 'set_heater_manual',
                     'set_target_temp', 'set_heater_duty', 'estop', 'clear_log']
            },
            value: { type: 'NUMBER' },
            arg: { type: 'STRING' }
          },
          required: ['action']
        }
      }
    },
    required: ['reply', 'commands']
  };

  function aiContext() {
    var d = thermo;
    var f = function (v, n) { return v.toFixed(n == null ? 2 : n); };
    return [
      'source=SIMULATION (browser virtual plant, no hardware)',
      'Th_in=' + f(d.thIn) + ' Th_out=' + f(d.thOut) + ' Tc_in=' + f(d.tcIn) + ' Tc_out=' + f(d.tcOut) + ' C',
      'flow_hot=' + f(plant.flowHot) + ' L/min  flow_cold=' + f(plant.flowCold) + ' L/min  Q_hot=' +
        f(d.qHot, 0) + ' W  Q_cold=' + f(d.qCold, 0) + ' W  heat_balance=' + f(d.balance, 0) + '%',
      'LMTD=' + f(d.lmtd) + ' C  UA=' + f(d.ua, 1) + ' W/K  effectiveness=' + f(d.eff, 1) + '%',
      'pump_hot: mode=' + (pump.mode ? 'flow_control' : 'manual') + ' speed=' + Math.round(pump.cur) +
        '% target=' + Math.round(pump.target) + '% flow_setpoint=' + f(pump.flowSp),
      'pump_cold: mode=' + (pumpCold.mode ? 'flow_control' : 'manual') + ' speed=' + Math.round(pumpCold.cur) +
        '% target=' + Math.round(pumpCold.target) + '% flow_setpoint=' + f(pumpCold.flowSp),
      'heater: mode=' + ['OFF', 'MAN', 'THR', 'TPW', 'PID'][heater.mode] + ' relay=' +
        (heater.relay ? 'ON' : 'OFF') + ' demand=' + (heater.demand ? 'yes' : 'no') + ' duty=' +
        currentDuty().toFixed(0) + '% setpoint=' + heater.target.toFixed(1) + ' C control_probe=' +
        PROBE_NAMES[heater.controlProbe],
      'safety: ' + (safety.err ? 'TRIPPED ' + errString(safety.err) : 'normal') + '  warnings=' + safety.warnings,
      'limits: over_temp_trip=' + K.maxSafeTemp + ' C, min_flow_for_heater=' + K.minFlowHeater + ' L/min'
    ].join('\n');
  }

  function addMsg(role, text, cmds) {
    var m = { id: ++chatRev, role: role, text: text, commands: cmds || [],
              executed: false, results: '', timeSec: Math.floor((now() - simStart) / 1000) };
    chatMsgs.push(m);
    if (chatMsgs.length > 40) chatMsgs = chatMsgs.slice(-40);
    return m;
  }

  function runAction(action, value, arg) {
    var a = String(action || '');
    var name = String(arg || '').toLowerCase();
    switch (a) {
      case 'set_pump_pct': return applyControl({ pump_pct: value });
      case 'set_flow_setpoint': return applyControl({ flow_setpoint: value });
      case 'pump_stop': return applyControl({ pump_stop: true });
      case 'set_cold_pump_pct': return applyControl({ cold_pump_pct: value });
      case 'set_cold_flow_setpoint': return applyControl({ cold_flow_setpoint: value });
      case 'cold_pump_stop': return applyControl({ cold_pump_stop: true });
      case 'set_heater_mode': {
        var modes = { off: 0, manual: 1, thermostatic: 2, thermo: 2, time_pwm: 3, pwm: 3, pid: 4 };
        var m = modes[name];
        if (m == null) m = value | 0;
        return applyControl({ heater_mode: m });
      }
      case 'set_heater_manual': return applyControl({ heater_manual: name === 'on' || value >= 0.5 });
      case 'set_target_temp': return applyControl({ target_temp: value });
      case 'set_heater_duty': return applyControl({ heater_duty: value });
      case 'estop': return applyControl({ estop: true });
      case 'clear_log': log = []; return { ok: true, message: 'Log buffer cleared' };
      default: return { ok: false, message: 'Unknown command: ' + a };
    }
  }

  function executeMsg(m) {
    if (!m || !m.commands || !m.commands.length || m.executed) {
      return { ok: false, message: 'Nothing to execute (already run or no commands)' };
    }
    var results = [], allOk = true;
    m.commands.forEach(function (c) {
      var r = runAction(c.action, c.value, c.arg);
      allOk = allOk && r.ok;
      results.push((r.ok ? 'OK ' : 'FAILED ') + r.message);
    });
    m.executed = true;
    m.results = results.join('; ');
    addMsg('system', (allOk ? 'Executed: ' : 'Executed with errors: ') + m.results, []);
    return { ok: allOk, message: m.results };
  }

  function handleChatResult(o) {
    var reply = o && o.reply ? String(o.reply) : '(no reply)';
    var cmds = [];
    if (o && Array.isArray(o.commands)) {
      o.commands.slice(0, 6).forEach(function (c) {
        if (c && c.action) cmds.push({ action: String(c.action), value: Number(c.value) || 0, arg: String(c.arg || '') });
      });
    }
    var m = addMsg('assistant', reply, cmds);
    if (ai.chatAuto && cmds.length) executeMsg(m);
  }

  function applyClosedLoop(rec, seq) {
    if (appliedSeq === seq) return;
    appliedSeq = seq;
    if (safety.err) return;
    var warn = String(rec.safety_warning || '').toUpperCase();
    if (warn && warn !== 'NONE') return;
    var apply = [];
    if (pump.mode === 0) {
      var cur = Math.round(pump.target);
      var tgt = clamp(rec.recommended_pump_pct, cur - 15, cur + 15);
      if (heater.mode !== 0) tgt = Math.max(tgt, 20);
      if (tgt !== cur) apply.push('hot pump ' + tgt + '%');
    }
    if (pumpCold.mode === 0) {
      var cur2 = Math.round(pumpCold.target);
      var tgt2 = clamp(rec.recommended_cold_pump_pct, cur2 - 15, cur2 + 15);
      if (heater.mode !== 0) tgt2 = Math.max(tgt2, 20);
      if (tgt2 !== cur2) apply.push('cold pump ' + tgt2 + '%');
    }
    if (heater.mode === 3) {
      var c = Math.round(heater.dutySetting);
      var t = clamp(rec.recommended_heater_duty, c - 20, c + 20);
      if (t !== c) apply.push('heater duty ' + t + '%');
    }
    if (!apply.length) return;
    if (pump.mode === 0) applyControl({ pump_pct: clamp(rec.recommended_pump_pct, Math.round(pump.target) - 15, Math.round(pump.target) + 15) });
    if (pumpCold.mode === 0) applyControl({ cold_pump_pct: clamp(rec.recommended_cold_pump_pct, Math.round(pumpCold.target) - 15, Math.round(pumpCold.target) + 15) });
    if (heater.mode === 3) applyControl({ heater_duty: clamp(rec.recommended_heater_duty, Math.round(heater.dutySetting) - 20, Math.round(heater.dutySetting) + 20) });
    addMsg('system', 'AI closed-loop applied: ' + apply.join(', '), []);
  }

  function handleAnalysisResult(o) {
    var hot = clamp(Math.round(Number(o.recommended_pump_pct) || 0), 0, 100);
    var coldRaw = (o.recommended_cold_pump_pct != null) ? Number(o.recommended_cold_pump_pct) : hot;
    ai.rec = {
      analysis: String(o.analysis || ''),
      efficiency_score: clamp(Number(o.efficiency_score) || 0, 0, 100),
      recommended_pump_pct: hot,
      recommended_cold_pump_pct: clamp(Math.round(coldRaw), 0, 100),
      recommended_heater_duty: clamp(Math.round(Number(o.recommended_heater_duty) || 0), 0, 100),
      safety_warning: String(o.safety_warning || 'NONE'),
      suggested_action: String(o.action || 'MAINTAIN'),
      time: now()
    };
    ai.seq++;
    if (ai.closed) applyClosedLoop(ai.rec, ai.seq);
  }

  function geminiRequest(job, userText) {
    if (!aiHasKey()) { ai.lastError = 'Gemini API key is not set (Assistant tab)'; return; }
    if (ai.busy) return;
    ai.busy = true; ai.phase = 'waiting for model'; ai.lastError = 'OK';
    var t0 = now();
    var user = job === 'chat'
      ? 'LIVE TELEMETRY:\n' + aiContext() + '\n\nOPERATOR MESSAGE:\n' + userText
      : 'LIVE TELEMETRY:\n' + aiContext() + '\n\nAssess the exchanger and recommend setpoints.';
    var payload = {
      systemInstruction: { parts: [{ text: job === 'chat' ? CHAT_PROMPT : ANALYSIS_PROMPT }] },
      contents: [{ role: 'user', parts: [{ text: user }] }],
      generationConfig: {
        temperature: 0.3,
        responseMimeType: 'application/json',
        responseSchema: job === 'chat' ? CHAT_SCHEMA : ANALYSIS_SCHEMA
      }
    };
    var url = 'https://generativelanguage.googleapis.com/v1beta/models/' +
              encodeURIComponent(ai.model) + ':generateContent';
    realFetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'x-goog-api-key': ai.key },
      body: JSON.stringify(payload)
    }).then(function (res) {
      ai.phase = 'reading';
      return res.text().then(function (txt) { return { ok: res.ok, status: res.status, txt: txt }; });
    }).then(function (r) {
      ai.latency = Math.round(now() - t0);
      ai.requests++;
      if (!r.ok) {
        var msg = 'HTTP ' + r.status;
        try {
          var e = JSON.parse(r.txt);
          if (e && e.error && e.error.message) msg += ' - ' + e.error.message;
        } catch (x) { /* not JSON */ }
        throw new Error(msg);
      }
      var data = JSON.parse(r.txt);
      var text = '';
      var parts = (data.candidates && data.candidates[0] && data.candidates[0].content &&
                   data.candidates[0].content.parts) || [];
      for (var i = 0; i < parts.length; i++) if (!parts[i].thought) text += (parts[i].text || '');
      var obj = JSON.parse(text);
      ai.lastError = 'OK';
      if (job === 'chat') handleChatResult(obj); else handleAnalysisResult(obj);
    }).catch(function (e) {
      ai.errors++;
      ai.lastError = String((e && e.message) || e);
    }).then(function () {
      ai.busy = false; ai.phase = '';
    });
  }

  function handleLocal(raw) {
    var s = raw.toLowerCase().trim();
    var m;
    if (s === 'help' || s === '?') {
      addMsg('system', 'Offline commands: status | pump <0-100> | pump off | flow <L/min> | coldpump <0-100> | coldpump off | coldflow <L/min> | heater off|manual|thermo|pwm|pid | heater on|off | setpoint <C> | duty <0-100> | estop | reset | sim on|off|auto | clear log. Anything else goes to Gemini.', []);
      return true;
    }
    if (s === 'status') { addMsg('system', aiContext(), []); return true; }
    if (s === 'estop' || s === 'stop all' || s === 'emergency stop') { addMsg('system', applyControl({ estop: true }).message, []); return true; }
    if (s === 'reset') { var ok = resetFault(); addMsg('system', ok ? 'Safety trip cleared' : 'Cannot reset: condition still present', []); return true; }
    if (s === 'clear log') { log = []; addMsg('system', 'Log buffer cleared', []); return true; }
    if ((m = s.match(/^pump\s+(off|stop|(\d+(?:\.\d+)?))$/))) {
      if (m[2] === undefined) applyControl({ pump_stop: true }); else applyControl({ pump_pct: +m[2] });
      addMsg('system', 'Pump command applied', []); return true;
    }
    if ((m = s.match(/^flow\s+(\d+(?:\.\d+)?)$/))) { applyControl({ flow_setpoint: +m[1] }); addMsg('system', 'Flow setpoint applied', []); return true; }
    if ((m = s.match(/^coldpump\s+(off|stop|(\d+(?:\.\d+)?))$/))) {
      if (m[2] === undefined) applyControl({ cold_pump_stop: true }); else applyControl({ cold_pump_pct: +m[2] });
      addMsg('system', 'Cold pump command applied', []); return true;
    }
    if ((m = s.match(/^coldflow\s+(\d+(?:\.\d+)?)$/))) { applyControl({ cold_flow_setpoint: +m[1] }); addMsg('system', 'Cold flow setpoint applied', []); return true; }
    if ((m = s.match(/^heater\s+(on|off|manual|thermo|thermostatic|pwm|time_pwm|pid)$/))) {
      var w = m[1];
      if (w === 'on') { applyControl({ heater_mode: 1 }); applyControl({ heater_manual: true }); }
      else if (w === 'off') applyControl({ heater_mode: 0 });
      else { var modes = { manual: 1, thermo: 2, thermostatic: 2, pwm: 3, time_pwm: 3, pid: 4 }; applyControl({ heater_mode: modes[w] }); }
      addMsg('system', 'Heater command applied', []); return true;
    }
    if ((m = s.match(/^(?:setpoint|target|sp)\s+(\d+(?:\.\d+)?)$/))) { applyControl({ target_temp: +m[1] }); addMsg('system', 'Setpoint applied', []); return true; }
    if ((m = s.match(/^duty\s+(\d+(?:\.\d+)?)$/))) { applyControl({ heater_duty: +m[1] }); addMsg('system', 'Duty applied', []); return true; }
    if ((m = s.match(/^sim\s+(on|off|auto)$/))) { simMode = m[1] === 'on' ? 1 : (m[1] === 'off' ? 0 : 2); addMsg('system', 'Simulation mode ' + m[1] + ' (hosted page always simulates)', []); return true; }
    return false;
  }

  function handleChat(text) {
    text = String(text || '').trim();
    if (!text) return { ok: false, message: 'Empty message' };
    addMsg('user', text, []);
    if (handleLocal(text)) return { ok: true, message: 'Received' };
    if (!aiHasKey()) {
      addMsg('system', 'AI unavailable: Gemini API key is not set. Offline commands still work - type help.', []);
      return { ok: true, message: 'Received' };
    }
    geminiRequest('chat', text);
    return { ok: true, message: 'Received' };
  }

  function applyAiConfig(b) {
    if (typeof b.api_key === 'string') { ai.key = b.api_key.trim(); ai.lastAuto = now(); }
    if (typeof b.model === 'string') {
      if (!/^gemini-[\w.\-]+$/.test(b.model)) return { ok: false, message: 'Invalid model id (expected e.g. gemini-2.5-flash)' };
      ai.model = b.model;
    }
    if (typeof b.auto_interval_ms === 'number') {
      ai.auto = b.auto_interval_ms > 0;
      if (ai.auto) ai.interval = b.auto_interval_ms;
      ai.lastAuto = now();
    }
    if (typeof b.closed_loop === 'boolean') ai.closed = b.closed_loop;
    if (typeof b.chat_auto_exec === 'boolean') ai.chatAuto = b.chat_auto_exec;
    saveAi();
    return { ok: true, message: 'AI config updated' };
  }

  function chatGet() {
    return { rev: chatRev, busy: ai.busy, phase: ai.phase, auto_exec: ai.chatAuto, messages: chatMsgs };
  }

  function aiTelemetry() {
    var o = {
      model: ai.model, has_key: aiHasKey(),
      key_hint: aiHasKey() ? '...' + ai.key.slice(-4) : '',
      auto_enabled: ai.auto, interval_ms: ai.interval, closed_loop: ai.closed,
      chat_auto_exec: ai.chatAuto, busy: ai.busy, phase: ai.phase,
      last_error: ai.lastError, latency_ms: ai.latency, requests: ai.requests,
      errors: ai.errors, chat_rev: chatRev,
      unavailable: aiHasKey() ? '' : 'Gemini API key is not set (Assistant tab)',
      has_rec: !!ai.rec
    };
    if (ai.rec) {
      o.analysis = ai.rec.analysis;
      o.efficiency_score = ai.rec.efficiency_score;
      o.recommended_pump_pct = ai.rec.recommended_pump_pct;
      o.recommended_cold_pump_pct = ai.rec.recommended_cold_pump_pct;
      o.recommended_heater_duty = ai.rec.recommended_heater_duty;
      o.safety_warning = ai.rec.safety_warning;
      o.suggested_action = ai.rec.suggested_action;
      o.age_s = Math.max(0, Math.round((now() - ai.rec.time) / 1000));
    }
    return o;
  }

  // =====================================================================
  // Plant (mirror PlantSimulator::update, two loops)
  // =====================================================================
  function pumpCurve(pct) { return pct < 10 ? 0 : K.maxFlow * (pct - 10) / 90; }

  function stepPlant(dtMs) {
    var dt = Math.min(dtMs / 1000, 1.0);
    if (dt <= 0) return;

    var ftHot = pumpCurve(pump.cur), ftCold = pumpCurve(pumpCold.cur);
    if (simFaults & F_HOT_BLOCKED) ftHot = 0;
    if (simFaults & F_COLD_BLOCKED) ftCold = 0;

    var a = Math.min(dt / K.flowTau, 1);
    plant.flowHot += (ftHot - plant.flowHot) * a;
    plant.flowCold += (ftCold - plant.flowCold) * a;
    if (plant.flowHot < 0.01) plant.flowHot = 0;
    if (plant.flowCold < 0.01) plant.flowCold = 0;
    plant.totalHot += plant.flowHot * dt / 60;
    plant.totalCold += plant.flowCold * dt / 60;

    var heating = heater.relay || !!(simFaults & F_RELAY_WELD);
    plant.heaterPower = heating ? K.heaterW : 0;

    var cHot = plant.flowHot / 60 * K.rho * K.cp;
    var cCold = plant.flowCold / 60 * K.rho * K.cp;
    var thOut, tcOut, q = 0;
    if (cHot > 1 && cCold > 1) {
      var cMin = Math.min(cHot, cCold), cMax = Math.max(cHot, cCold);
      var cr = cMin / cMax, ntu = K.hxUA / cMin;
      var eps = Math.abs(cr - 1) < 1e-3 ? ntu / (1 + ntu)
              : (1 - Math.exp(-ntu * (1 - cr))) / (1 - cr * Math.exp(-ntu * (1 - cr)));
      q = eps * cMin * (plant.tankC - K.coldSupply);
      thOut = plant.tankC - q / cHot;
      tcOut = K.coldSupply + q / cCold;
    } else {
      thOut = plant.probe[1] + (K.ambient - plant.probe[1]) * 0.02;
      tcOut = plant.probe[3] + (K.ambient - plant.probe[3]) * 0.02;
    }

    var loss = K.tankLoss * (plant.tankC - K.ambient);
    plant.tankC += (plant.heaterPower - q - loss) / (K.tankKg * K.cp) * dt;

    var ap = Math.min(dt / K.probeTau, 1);
    plant.probe[0] += (plant.tankC - plant.probe[0]) * ap;
    plant.probe[1] += (thOut - plant.probe[1]) * ap;
    plant.probe[2] += (K.coldSupply - plant.probe[2]) * ap;
    plant.probe[3] += (tcOut - plant.probe[3]) * ap;
  }

  function probeValid(i) { return !(i === 0 && (simFaults & F_THIN_LOST)); }
  function probeTemp(i) {
    if (!probeValid(i)) return K.disconnect;
    var noisy = plant.probe[i] + (Math.random() - 0.5) * 0.1;
    return round16(noisy);
  }
  function faultMask() { return probeValid(0) ? 0 : 0b0001; }

  // =====================================================================
  // Thermodynamics (mirror Thermodynamics.hpp)
  // =====================================================================
  function updateThermo() {
    var thIn = probeTemp(0), thOut = probeTemp(1), tcIn = probeTemp(2), tcOut = probeTemp(3);
    thermo.thIn = thIn; thermo.thOut = thOut; thermo.tcIn = tcIn; thermo.tcOut = tcOut;
    var ok = function (t) { return t > K.disconnect + 1; };
    thermo.valid = ok(thIn) && ok(thOut) && ok(tcIn) && ok(tcOut);
    if (!thermo.valid) {
      thermo.deltaTh = thermo.deltaTc = 0; thermo.qHot = thermo.qCold = 0;
      thermo.lmtd = thermo.ua = thermo.eff = thermo.balance = 0;
      return;
    }
    thermo.deltaTh = thIn - thOut;
    thermo.deltaTc = tcOut - tcIn;
    var mHot = plant.flowHot / 60 * K.rho;
    var mCold = plant.flowCold / 60 * K.rho;
    thermo.qHot = (mHot > 0.0001 && thermo.deltaTh > 0) ? mHot * K.cp * thermo.deltaTh : 0;
    thermo.qCold = (mCold > 0.0001 && thermo.deltaTc > 0) ? mCold * K.cp * thermo.deltaTc : 0;
    thermo.balance = thermo.qHot > 10 ? Math.min(thermo.qCold / thermo.qHot * 100, 150) : 0;
    var dt1 = thIn - tcOut, dt2 = thOut - tcIn;
    if (dt1 > 0.05 && dt2 > 0.05) {
      var diff = dt1 - dt2;
      thermo.lmtd = Math.abs(diff) < 0.1 ? (dt1 + dt2) / 2 : diff / Math.log(dt1 / dt2);
    } else thermo.lmtd = 0;
    thermo.ua = thermo.lmtd > 0.2 ? thermo.qHot / thermo.lmtd : 0;
    var cHot = mHot * K.cp, cCold = mCold * K.cp, cMin = Math.min(cHot, cCold);
    var maxDeltaT = thIn - tcIn;
    thermo.eff = (cMin > 0.0001 && maxDeltaT > 0.5)
      ? clamp(thermo.qHot / (cMin * maxDeltaT) * 100, 0, 100) : 0;
  }

  // =====================================================================
  // Pumps + heater (mirror PumpController.hpp / HeaterController.hpp)
  // =====================================================================
  function pctToDuty(p, pct) {
    if (pct < 0.5) return 0;
    var eff = p.minDuty + (100 - p.minDuty) * (pct / 100);
    return Math.round(eff / 100 * K.pwmMax);
  }
  function rampPump(p, dtMs) {
    var maxStep = K.pumpRampPctPerS * dtMs / 1000;
    var diff = p.target - p.cur;
    if (Math.abs(diff) <= maxStep) p.cur = p.target;
    else p.cur += (diff > 0 ? 1 : -1) * maxStep;
    p.dutyRaw = computeDuty(p);
  }
  function computeDuty(p) {
    var duty = pctToDuty(p, p.cur);
    if (duty === 0) { p.kickUntil = 0; p.driving = false; return 0; }
    if (!p.driving && K.pumpKickstartMs > 0) p.kickUntil = now() + K.pumpKickstartMs;
    p.driving = true;
    if (p.kickUntil && now() < p.kickUntil) duty = K.pwmMax;
    else p.kickUntil = 0;
    return duty;
  }
  function updateFlowLoop(p, flow, dtS) {
    if (p.mode !== 1 || dtS > 1) return;
    if (p.flowSp < 0.05) { p.target = 0; p.pid.reset(0); }
    else p.target = p.pid.update(p.flowSp, flow, dtS);
  }

  function timeProportion(dutyPct) {
    var t = now();
    while (t - heater.windowStart >= K.heaterWindowMs) heater.windowStart += K.heaterWindowMs;
    var onMs = K.heaterWindowMs * dutyPct / 100;
    if (onMs < K.heaterMinPulseMs) onMs = 0;
    if (K.heaterWindowMs - onMs < K.heaterMinPulseMs) onMs = K.heaterWindowMs;
    return (t - heater.windowStart) < onMs;
  }

  function updateHeater() {
    var controlTemp = probeTemp(heater.controlProbe);
    var tempValid = controlTemp > K.disconnect + 1;
    var permitted = safety.permissive;
    switch (heater.mode) {
      case 0: heater.demand = false; break;
      case 1: heater.demand = heater.manual; break;
      case 2:
        if (!tempValid || controlTemp > heater.target + heater.hyst) heater.demand = false;
        else if (controlTemp < heater.target - heater.hyst && now() - heater.lastSwitch >= K.heaterMinSwitchMs) heater.demand = true;
        break;
      case 3: heater.demand = timeProportion(heater.dutySetting); break;
      case 4:
        if (!tempValid) { heater.pid.reset(0); heater.demand = false; break; }
        if (now() - heater.lastPidT >= 1000) {
          var dt = (now() - heater.lastPidT) / 1000; heater.lastPidT = now();
          if (permitted) heater.pid.update(heater.target, controlTemp, dt);
          else heater.pid.reset(0);
        }
        heater.dutyOutput = heater.pid.output;
        heater.demand = timeProportion(heater.dutyOutput);
        break;
      default: heater.demand = false;
    }
    var energize = heater.demand && permitted;
    if (energize !== heater.relay) heater.lastSwitch = now();
    heater.relay = energize;
  }

  function currentDuty() {
    if (heater.mode === 4) return heater.pid.output;
    if (heater.mode === 3) return heater.dutySetting;
    return heater.relay ? 100 : 0;
  }

  // =====================================================================
  // Safety (mirror SafetyManager.hpp)
  // =====================================================================
  function errString(code) {
    switch (code) {
      case 0: return 'SYSTEM NORMAL';
      case 1: return 'DRY-FIRE: FLOW LOST WHILE HEATING';
      case 2: return 'OVER-TEMPERATURE SHUTDOWN';
      case 3: return 'HOT-SIDE SENSOR FAULT';
      case 4: return 'EMERGENCY STOP';
      default: return 'UNKNOWN FAULT';
    }
  }
  function trip(err, detail) {
    if (safety.err === 0) safety.trips++;
    safety.err = err; safety.tripDetail = detail; safety.permissive = false;
  }
  function resetFault() {
    var mask = faultMask();
    if (safety.err === 3 && (mask & 0b0011)) return false;
    if (safety.err === 2 && safety.maxValidTemp >= K.maxSafeTemp - 5) return false;
    safety.err = 0; safety.tripDetail = 0; safety.lowFlowStart = 0;
    return true;
  }
  function updateSafety() {
    var t = now();
    var flowOk = plant.flowHot >= K.minFlowHeater;
    safety.warnings = 0;

    var maxT = -999;
    for (var i = 0; i < 4; i++) if (probeValid(i)) maxT = Math.max(maxT, probeTemp(i));
    safety.maxValidTemp = maxT;

    if (!probeValid(2) || !probeValid(3)) safety.warnings |= 4;
    if (heater.demand && !flowOk) safety.warnings |= 1;
    if ((heater.demand || pump.cur > 0.5) && plant.flowCold < K.minColdFlow) safety.warnings |= 8;
    if (pump.cur > 0.5 && plant.flowHot < 0.05) {
      if (!safety.dryRunStart) safety.dryRunStart = t;
      if (t - safety.dryRunStart >= K.pumpDryRunMs) safety.warnings |= 2;
    } else safety.dryRunStart = 0;

    if (safety.err === 0) {
      var booting = (t - safety.boot) < K.bootGraceMs;
      var mask = faultMask();
      if (!booting && (mask & 0b0011)) trip(3, mask & 0b0011);
      else if (maxT >= K.maxSafeTemp) trip(2, 0);
      else if (heater.relay && !flowOk) {
        if (!safety.lowFlowStart) safety.lowFlowStart = t;
        else if (t - safety.lowFlowStart >= K.zeroFlowTimeoutMs) trip(1, 0);
      } else safety.lowFlowStart = 0;
    }
    var flowGrace = heater.relay && safety.lowFlowStart !== 0;
    safety.permissive = (safety.err === 0) && (flowOk || flowGrace);
  }

  // =====================================================================
  // Logging (mirror DataLogger.hpp, 1 Hz ring buffer)
  // =====================================================================
  function record() {
    log.push({
      t: Math.floor((now() - simStart) / 1000),
      thIn: thermo.thIn, thOut: thermo.thOut, tcIn: thermo.tcIn, tcOut: thermo.tcOut,
      flowHot: plant.flowHot, flowCold: plant.flowCold,
      qHot: thermo.qHot, qCold: thermo.qCold, lmtd: thermo.lmtd, ua: thermo.ua,
      eff: thermo.eff, pumpHot: Math.round(pump.cur), pumpCold: Math.round(pumpCold.cur),
      duty: Math.round(currentDuty()), relay: heater.relay, sim: true
    });
    if (log.length > K.logCapacity) log.shift();
  }

  // =====================================================================
  // Fixed-rate tick (10 Hz control, 1 Hz logging)
  // =====================================================================
  function tick() {
    var t = now();
    var dtMs = t - lastTick; lastTick = t;
    if (dtMs <= 0) return;

    rampPump(pump, dtMs);
    rampPump(pumpCold, dtMs);
    stepPlant(dtMs);
    updateFlowLoop(pump, plant.flowHot, dtMs / 1000);
    updateFlowLoop(pumpCold, plant.flowCold, dtMs / 1000);
    updateThermo();
    updateSafety();
    updateHeater();

    if (t - lastLog >= 1000) { lastLog = t; record(); }

    // Periodic Gemini analysis (only when a key is present and enabled).
    if (ai.auto && aiHasKey() && !ai.busy && t - ai.lastAuto >= ai.interval) {
      ai.lastAuto = t;
      geminiRequest('analysis');
    }
  }
  setInterval(tick, 100);

  // =====================================================================
  // Telemetry payload (same shape as WebServerManager.hpp)
  // =====================================================================
  function telemetry() {
    var probes = [0, 1, 2, 3].map(function (i) {
      return { name: PROBE_NAMES[i], pin: 4, present: true, valid: probeValid(i),
               errors: 0, rom: PROBE_ROMS[i] };
    });
    return {
      uptime_s: Math.floor((now() - simStart) / 1000),
      simulated: true,
      th_in: thermo.thIn, th_out: thermo.thOut, tc_in: thermo.tcIn, tc_out: thermo.tcOut,
      delta_th: thermo.deltaTh, delta_tc: thermo.deltaTc, thermo_valid: thermo.valid,
      flow_lpm: plant.flowHot, flow_cold_lpm: plant.flowCold,
      total_liters: plant.totalHot, total_liters_cold: plant.totalCold,
      q_hot_w: thermo.qHot, q_cold_w: thermo.qCold, lmtd: thermo.lmtd,
      ua: thermo.ua, effectiveness: thermo.eff, heat_balance: thermo.balance,
      probes: probes,
      pump: { pct: Math.round(pump.cur), target_pct: Math.round(pump.target), mode: pump.mode,
              flow_setpoint: pump.flowSp, duty_raw: pump.dutyRaw },
      pump_cold: { pct: Math.round(pumpCold.cur), target_pct: Math.round(pumpCold.target),
                   mode: pumpCold.mode, flow_setpoint: pumpCold.flowSp, duty_raw: pumpCold.dutyRaw },
      heater: { on: heater.relay, demand: heater.demand, mode: heater.mode, manual: heater.manual,
                duty: currentDuty(), duty_setting: heater.dutySetting, target: heater.target,
                permitted: safety.permissive },
      safety: { tripped: safety.err !== 0, error: errString(safety.err), code: safety.err,
                warnings: safety.warnings, trips: safety.trips },
      net: { sta_connected: false, sta_ip: '', ap_ip: 'simulator', ssid: '', rssi: 0 },
      sys: { heap: 180000, lcd: false, log_count: log.length, log_capacity: K.logCapacity,
             sim_mode: simMode, sim_faults: simFaults, sim_fault_fallback: false, sim_tank: plant.tankC },
      cfg: { k_factor: kHot, cold_k_factor: kCold, hyst: heater.hyst, kp: heater.kp,
             ki: heater.ki, kd: heater.kd, control_probe: heater.controlProbe,
             pump_min_duty: pump.minDuty, flow_kp: pump.pid.kp, flow_ki: pump.pid.ki,
             cold_pump_min_duty: pumpCold.minDuty, cold_flow_kp: pumpCold.pid.kp,
             cold_flow_ki: pumpCold.pid.ki, max_temp: K.maxSafeTemp },
      ai: aiTelemetry()
    };
  }

  // =====================================================================
  // Command / config handling (mirror CommandExec.hpp)
  // =====================================================================
  function applyControl(b) {
    var msgs = [];
    if (b.estop) {
      safety.err = 4; safety.permissive = false; safety.tripDetail = 0;
      pump.mode = 0; pump.target = 0; pump.cur = 0; pump.flowSp = 0;
      pumpCold.mode = 0; pumpCold.target = 0; pumpCold.cur = 0; pumpCold.flowSp = 0;
      heater.mode = 0; heater.manual = false; heater.demand = false; heater.relay = false;
      msgs.push('E-STOP: heater off, pumps stopped');
    }
    if (b.reset_fault) msgs.push(resetFault() ? 'Safety trip cleared' : 'Cannot reset: condition still present');
    if (b.pump_stop) { pump.mode = 0; pump.target = 0; msgs.push('Pump ramping to stop'); }
    if (typeof b.pump_pct === 'number') { pump.mode = 0; pump.target = clamp(b.pump_pct, 0, 100); msgs.push('Pump set to ' + Math.round(b.pump_pct) + '%'); }
    if (typeof b.flow_setpoint === 'number') { if (pump.mode !== 1) pump.pid.reset(pump.cur); pump.mode = 1; pump.flowSp = clamp(b.flow_setpoint, 0, 30); msgs.push('Pump flow control ' + b.flow_setpoint.toFixed(2) + ' L/min'); }
    if (b.cold_pump_stop) { pumpCold.mode = 0; pumpCold.target = 0; msgs.push('Cold pump ramping to stop'); }
    if (typeof b.cold_pump_pct === 'number') { pumpCold.mode = 0; pumpCold.target = clamp(b.cold_pump_pct, 0, 100); msgs.push('Cold pump set to ' + Math.round(b.cold_pump_pct) + '%'); }
    if (typeof b.cold_flow_setpoint === 'number') { if (pumpCold.mode !== 1) pumpCold.pid.reset(pumpCold.cur); pumpCold.mode = 1; pumpCold.flowSp = clamp(b.cold_flow_setpoint, 0, 30); msgs.push('Cold flow control ' + b.cold_flow_setpoint.toFixed(2) + ' L/min'); }
    if (typeof b.heater_mode === 'number' && b.heater_mode >= 0 && b.heater_mode <= 4) {
      heater.mode = b.heater_mode; heater.windowStart = now();
      if (heater.mode === 0) { heater.demand = false; heater.relay = false; }
      if (heater.mode === 1) heater.manual = false;
      if (heater.mode === 4) { heater.pid.reset(0); heater.lastPidT = now(); }
      msgs.push('Heater mode ' + ['OFF', 'MAN', 'THR', 'TPW', 'PID'][heater.mode]);
    }
    if (typeof b.heater_manual === 'boolean') { heater.manual = b.heater_manual; msgs.push('Manual heater ' + (heater.manual ? 'ON' : 'OFF')); }
    if (typeof b.heater_duty === 'number') { heater.dutySetting = clamp(b.heater_duty, 0, 100); msgs.push('Time-PWM duty ' + Math.round(b.heater_duty) + '%'); }
    if (typeof b.target_temp === 'number') { heater.target = clamp(b.target_temp, 10, K.maxSafeTemp - 2); msgs.push('Heater setpoint ' + heater.target.toFixed(1) + ' C'); }
    return { ok: true, message: msgs.length ? msgs.join('; ') : 'No change' };
  }

  function applyConfig(b) {
    if (typeof b.flow_k_factor === 'number' && b.flow_k_factor > 0.1) kHot = b.flow_k_factor;
    if (typeof b.cold_flow_k_factor === 'number' && b.cold_flow_k_factor > 0.1) kCold = b.cold_flow_k_factor;
    if (typeof b.hysteresis === 'number') heater.hyst = clamp(b.hysteresis, 0.2, 5);
    if (typeof b.kp === 'number' || typeof b.ki === 'number' || typeof b.kd === 'number') {
      heater.pid.setGains(b.kp != null ? b.kp : heater.kp, b.ki != null ? b.ki : heater.ki, b.kd != null ? b.kd : heater.kd);
      heater.kp = heater.pid.kp; heater.ki = heater.pid.ki; heater.kd = heater.pid.kd;
    }
    if (typeof b.control_probe === 'number') heater.controlProbe = clamp(b.control_probe, 0, 3);
    if (typeof b.pump_min_duty === 'number') pump.minDuty = clamp(b.pump_min_duty, 0, 80);
    if (typeof b.cold_pump_min_duty === 'number') pumpCold.minDuty = clamp(b.cold_pump_min_duty, 0, 80);
    if (typeof b.flow_kp === 'number' || typeof b.flow_ki === 'number') pump.pid.setGains(b.flow_kp != null ? b.flow_kp : pump.pid.kp, b.flow_ki != null ? b.flow_ki : pump.pid.ki, 0);
    if (typeof b.cold_flow_kp === 'number' || typeof b.cold_flow_ki === 'number') pumpCold.pid.setGains(b.cold_flow_kp != null ? b.cold_flow_kp : pumpCold.pid.kp, b.cold_flow_ki != null ? b.cold_flow_ki : pumpCold.pid.ki, 0);
    if (typeof b.sim_mode === 'number') simMode = clamp(b.sim_mode, 0, 2);
    if (typeof b.sim_faults === 'number') simFaults = b.sim_faults;
    if (b.wifi_ssid !== undefined) return { ok: true, message: 'Wi-Fi is not used in the hosted simulator' };
    return { ok: true, message: 'Settings saved' };
  }

  // =====================================================================
  // CSV export (client-side Blob)
  // =====================================================================
  function downloadCsv() {
    var head = 'Time_sec,Th_in_C,Th_out_C,Tc_in_C,Tc_out_C,Flow_hot_LPM,Flow_cold_LPM,Q_hot_W,Q_cold_W,LMTD_C,UA_W_per_K,Effectiveness_pct,Pump_hot_pct,Pump_cold_pct,Heater_Duty_pct,Heater_State,Source\n';
    var body = log.map(function (e) {
      return [e.t, e.thIn.toFixed(2), e.thOut.toFixed(2), e.tcIn.toFixed(2), e.tcOut.toFixed(2),
              e.flowHot.toFixed(2), e.flowCold.toFixed(2), e.qHot.toFixed(1), e.qCold.toFixed(1),
              e.lmtd.toFixed(2), e.ua.toFixed(1), e.eff.toFixed(1), e.pumpHot, e.pumpCold, e.duty,
              e.relay ? 'ON' : 'OFF', 'SIM'].join(',');
    }).join('\n');
    var blob = new Blob([head + body], { type: 'text/csv' });
    var a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'heatexchanger_telemetry_sim.csv';
    document.body.appendChild(a); a.click(); document.body.removeChild(a);
    setTimeout(function () { URL.revokeObjectURL(a.href); }, 2000);
  }
  // The Data tab uses an <a href="/api/export.csv"> download link; intercept it.
  document.addEventListener('click', function (e) {
    var a = e.target.closest && e.target.closest('a[href="/api/export.csv"]');
    if (!a) return;
    e.preventDefault();
    downloadCsv();
  }, true);

  // =====================================================================
  // fetch shim
  // =====================================================================
  var realFetch = window.fetch ? window.fetch.bind(window) : null;
  function R(obj, status) {
    status = status || 200;
    return Promise.resolve({ ok: status < 400, status: status, json: function () { return Promise.resolve(obj); } });
  }
  function parseBody(opts) {
    try { return opts && opts.body ? JSON.parse(opts.body) : {}; } catch (e) { return {}; }
  }

  window.fetch = function (url, opts) {
    var u = String(url);
    var method = (opts && opts.method) || 'GET';
    if (u.indexOf('/api/telemetry') >= 0) return R(telemetry());
    if (u.indexOf('/api/control') >= 0) return R(applyControl(parseBody(opts)));
    if (u.indexOf('/api/config') >= 0) return R(applyConfig(parseBody(opts)));
    if (u.indexOf('/api/ai/config') >= 0) return R(applyAiConfig(parseBody(opts)));
    if (u.indexOf('/api/ai/query') >= 0) {
      if (!aiHasKey()) return R({ ok: false, message: 'Gemini API key is not set (Assistant tab)' }, 409);
      if (ai.busy) return R({ ok: false, message: 'Gemini is busy' }, 409);
      geminiRequest('analysis');
      return R({ ok: true, message: 'Analysis queued' }, 202);
    }
    if (u.indexOf('/api/chat/execute') >= 0) {
      var cid = parseBody(opts).id;
      var target = null;
      for (var ci = 0; ci < chatMsgs.length; ci++) if (chatMsgs[ci].id === cid) { target = chatMsgs[ci]; break; }
      var er = executeMsg(target);
      return R(er, er.ok ? 200 : 409);
    }
    if (u.indexOf('/api/chat/clear') >= 0) { chatMsgs = []; chatRev++; return R({ ok: true, message: 'Chat cleared' }); }
    if (u.indexOf('/api/chat') >= 0) {
      if (method === 'POST') return R(handleChat(parseBody(opts).message || ''));
      return R(chatGet());
    }
    if (u.indexOf('/api/wifi/scan') >= 0) return R({ scanning: false, networks: [] });
    if (u.indexOf('/api/logging/clear') >= 0) { log = []; return R({ ok: true, message: 'Log cleared' }); }
    if (u.indexOf('/api/export.csv') >= 0) { downloadCsv(); return R({ ok: true, message: 'CSV' }); }
    return realFetch ? realFetch(url, opts) : R({ ok: false, message: 'offline' }, 404);
  };

  // A tiny console hint (harmless in production).
  console.log('[sim] hosted client-side simulator active; /api/* is served locally.');
})();