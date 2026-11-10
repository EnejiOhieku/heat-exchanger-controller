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
      ai: { model: 'not available (hosted simulator)', has_key: false, key_hint: '',
            auto_enabled: false, interval_ms: 60000, closed_loop: false, chat_auto_exec: false,
            busy: false, phase: '', last_error: 'OK', latency_ms: 0, requests: 0, errors: 0,
            chat_rev: chatRev, unavailable: 'AI is disabled in the hosted simulator', has_rec: false }
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
  // Chat (no AI over the network)
  // =====================================================================
  function chatGet() { return { rev: chatRev, busy: false, phase: '', auto_exec: false, messages: chatMsgs }; }
  function chatPost(text) {
    chatMsgs.push({ id: ++chatRev, role: 'user', text: text, commands: [], executed: false, results: '', timeSec: 0 });
    chatMsgs.push({ id: ++chatRev, role: 'system', text: 'Hosted simulator: no AI or hardware link. Drive the virtual rig from the Control tab and inject faults under Settings.', commands: [], executed: false, results: '', timeSec: 0 });
    if (chatMsgs.length > 40) chatMsgs = chatMsgs.slice(-40);
    return { ok: true, message: 'Received' };
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
    if (u.indexOf('/api/ai/config') >= 0) return R({ ok: true, message: 'AI is disabled in the hosted simulator' });
    if (u.indexOf('/api/ai/query') >= 0) return R({ ok: false, message: 'AI is disabled in the hosted simulator' }, 409);
    if (u.indexOf('/api/chat/execute') >= 0) return R({ ok: false, message: 'AI is disabled in the hosted simulator' }, 409);
    if (u.indexOf('/api/chat/clear') >= 0) { chatMsgs = []; chatRev++; return R({ ok: true, message: 'Chat cleared' }); }
    if (u.indexOf('/api/chat') >= 0) {
      if (method === 'POST') return R(chatPost(parseBody(opts).message || ''));
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