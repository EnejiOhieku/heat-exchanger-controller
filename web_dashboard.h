#pragma once

#include <Arduino.h>

static const char PAGE_DASHBOARD[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Heat Exchanger Control & Telemetry</title>
  <style>
    :root {
      --bg: #0d1117;
      --card-bg: #161b22;
      --border: #30363d;
      --text: #c9d1d9;
      --text-bright: #f0f6fc;
      --accent: #58a6ff;
      --hot: #f85149;
      --cold: #388bfd;
      --flow: #3fb950;
      --warn: #d29922;
      --danger: #da3633;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
    body { background: var(--bg); color: var(--text); padding: 12px; }
    .header { display: flex; justify-content: space-between; align-items: center; padding: 12px 16px; background: var(--card-bg); border: 1px solid var(--border); border-radius: 8px; margin-bottom: 12px; flex-wrap: wrap; gap: 8px; }
    .header h1 { font-size: 1.2rem; color: var(--text-bright); display: flex; align-items: center; gap: 8px; }
    .badges { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; }
    .badge { padding: 4px 10px; border-radius: 12px; font-size: 0.75rem; font-weight: bold; text-transform: uppercase; border: 1px solid var(--border); }
    .badge-ok { background: rgba(63, 185, 80, 0.15); color: #3fb950; border-color: #3fb950; }
    .badge-trip { background: rgba(248, 81, 73, 0.25); color: #f85149; border-color: #f85149; animation: pulse 1s infinite; }
    @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.4; } }
    .btn-estop { background: var(--danger); color: #fff; border: none; padding: 8px 16px; border-radius: 6px; font-weight: bold; cursor: pointer; transition: 0.2s; }
    .btn-estop:hover { background: #b62324; }
    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr)); gap: 12px; margin-bottom: 12px; }
    .card { background: var(--card-bg); border: 1px solid var(--border); border-radius: 8px; padding: 16px; }
    .card h2 { font-size: 0.95rem; color: var(--text-bright); margin-bottom: 12px; display: flex; justify-content: space-between; border-bottom: 1px solid var(--border); padding-bottom: 6px; }
    .stat-row { display: flex; justify-content: space-between; align-items: center; margin-bottom: 10px; }
    .stat-label { font-size: 0.85rem; color: #8b949e; }
    .stat-val { font-size: 1.15rem; font-weight: bold; color: var(--text-bright); }
    .val-hot { color: var(--hot); }
    .val-cold { color: var(--cold); }
    .val-flow { color: var(--flow); }
    .val-accent { color: var(--accent); }
    .val-warn { color: var(--warn); }
    .control-group { margin-bottom: 12px; }
    .control-group label { display: block; font-size: 0.8rem; color: #8b949e; margin-bottom: 4px; }
    .slider-row { display: flex; align-items: center; gap: 10px; }
    input[type=range] { flex: 1; accent-color: var(--accent); cursor: pointer; }
    input[type=number], input[type=text], select { background: #0d1117; border: 1px solid var(--border); color: #fff; padding: 6px 10px; border-radius: 4px; font-size: 0.85rem; width: 100%; }
    .btn { background: #21262d; border: 1px solid var(--border); color: #c9d1d9; padding: 6px 12px; border-radius: 6px; cursor: pointer; font-size: 0.85rem; transition: 0.2s; }
    .btn:hover { background: #30363d; color: #fff; }
    .btn-primary { background: #238636; border-color: #2ea043; color: #fff; }
    .btn-primary:hover { background: #2ea043; }
    .btn-warn { background: #9e6a03; border-color: #bb8009; color: #fff; }
    .btn-row { display: flex; gap: 8px; margin-top: 8px; }
    canvas { width: 100%; height: 180px; background: #090d13; border: 1px solid var(--border); border-radius: 6px; display: block; }
    .ai-box { background: rgba(88, 166, 255, 0.08); border: 1px solid rgba(88, 166, 255, 0.3); border-radius: 6px; padding: 12px; font-size: 0.85rem; line-height: 1.4; }
    .ai-rec { margin-top: 8px; font-weight: bold; color: var(--accent); }
    .tab-nav { display: flex; gap: 4px; border-bottom: 1px solid var(--border); margin-bottom: 12px; }
    .tab-btn { padding: 6px 12px; background: transparent; border: none; color: #8b949e; cursor: pointer; font-size: 0.85rem; border-bottom: 2px solid transparent; }
    .tab-btn.active { color: var(--text-bright); border-color: var(--accent); font-weight: bold; }
    .tab-content { display: none; }
    .tab-content.active { display: block; }
  </style>
</head>
<body>
  <div class="header">
    <h1>
      <span>♨️ Heat Exchanger Controller</span>
    </h1>
    <div class="badges">
      <span id="badgeSafety" class="badge badge-ok">NORMAL</span>
      <span id="badgeWifi" class="badge">AP: 192.168.4.1</span>
      <span id="badgeAi" class="badge">AI IDLE</span>
      <button class="btn-estop" onclick="triggerEStop()">E-STOP</button>
    </div>
  </div>

  <div class="tab-nav">
    <button class="tab-btn active" onclick="switchTab('dashboard')">Dashboard</button>
    <button class="tab-btn" onclick="switchTab('gemini')">Gemini AI Advisor</button>
    <button class="tab-btn" onclick="switchTab('logging')">CSV Data Logger</button>
    <button class="tab-btn" onclick="switchTab('settings')">Settings & WiFi</button>
  </div>

  <!-- TAB: DASHBOARD -->
  <div id="tab-dashboard" class="tab-content active">
    <div class="grid">
      <!-- Temperatures Card -->
      <div class="card">
        <h2><span>🌡️ Temperature Probes</span> <span id="lblDeltaTh" class="val-hot">ΔTh: --°C</span></h2>
        <div class="stat-row">
          <span class="stat-label">Hot Inlet (Th,in):</span>
          <span id="valThIn" class="stat-val val-hot">-- °C</span>
        </div>
        <div class="stat-row">
          <span class="stat-label">Hot Outlet (Th,out):</span>
          <span id="valThOut" class="stat-val val-hot">-- °C</span>
        </div>
        <div class="stat-row">
          <span class="stat-label">Cold Inlet (Tc,in):</span>
          <span id="valTcIn" class="stat-val val-cold">-- °C</span>
        </div>
        <div class="stat-row">
          <span class="stat-label">Cold Outlet (Tc,out):</span>
          <span id="valTcOut" class="stat-val val-cold">-- °C</span>
        </div>
        <div class="stat-row" style="margin-top: 12px; border-top: 1px dashed var(--border); padding-top: 8px;">
          <span class="stat-label">Cold Heat Uptake (ΔTc):</span>
          <span id="lblDeltaTc" class="stat-val val-cold">-- °C</span>
        </div>
      </div>

      <!-- Thermodynamics & Flow Card -->
      <div class="card">
        <h2><span>⚡ Thermodynamics & Flow</span> <span id="valEff" class="val-accent">ε: --%</span></h2>
        <div class="stat-row">
          <span class="stat-label">Flow Rate:</span>
          <span id="valFlow" class="stat-val val-flow">-- LPM</span>
        </div>
        <div class="stat-row">
          <span class="stat-label">Total Volume Pumped:</span>
          <span id="valVolume" class="stat-val">-- L</span>
        </div>
        <div class="stat-row">
          <span class="stat-label">Heat Exchanged (Q_hot):</span>
          <span id="valQHot" class="stat-val val-accent">-- kW</span>
        </div>
        <div class="stat-row">
          <span class="stat-label">Cold Absorption (Q_cold):</span>
          <span id="valQCold" class="stat-val val-cold">-- kW</span>
        </div>
        <div class="stat-row">
          <span class="stat-label">Log Mean Temp Diff (LMTD):</span>
          <span id="valLmtd" class="stat-val val-warn">-- °C</span>
        </div>
      </div>

      <!-- Pump & Heater Actuator Control -->
      <div class="card">
        <h2><span>⚙️ Actuator Controls</span></h2>
        <!-- Pump -->
        <div class="control-group">
          <label>WATER PUMP SPEED: <span id="lblPumpPct" style="color:#fff;font-weight:bold;">0%</span></label>
          <div class="slider-row">
            <input type="range" id="rngPump" min="0" max="100" value="0" oninput="updatePumpSlider(this.value)" onchange="sendPumpCmd(this.value)">
          </div>
          <div class="btn-row">
            <button class="btn" onclick="sendPumpCmd(0)">OFF</button>
            <button class="btn" onclick="sendPumpCmd(50)">50%</button>
            <button class="btn" onclick="sendPumpCmd(100)">100%</button>
          </div>
        </div>
        <!-- Heater -->
        <div class="control-group" style="margin-top: 16px; border-top: 1px dashed var(--border); padding-top: 12px;">
          <label>30A HEATER CONTROL: <span id="lblHeaterState" style="color:#fff;font-weight:bold;">OFF</span></label>
          <div class="stat-row">
            <select id="selHeaterMode" onchange="sendHeaterConfig()">
              <option value="0">MODE: OFF (Safe Lock)</option>
              <option value="1">MODE: MANUAL ON/OFF</option>
              <option value="2">MODE: THERMOSTATIC (Setpoint)</option>
              <option value="3">MODE: TIME-PWM (Slow Proportional)</option>
            </select>
          </div>
          <div class="btn-row" id="grpManualHeater" style="margin-top: 6px;">
            <button class="btn btn-primary" onclick="sendHeaterManual(true)">HEATER ON</button>
            <button class="btn" onclick="sendHeaterManual(false)">HEATER OFF</button>
          </div>
          <div id="grpThermostatic" style="margin-top: 8px;">
            <label>Target Temp (°C): <span id="lblTargetTemp" style="color:#fff;">50</span>°C</label>
            <input type="range" id="rngTargetTemp" min="20" max="72" value="50" oninput="document.getElementById('lblTargetTemp').innerText=this.value" onchange="sendHeaterConfig()">
          </div>
        </div>
      </div>
    </div>

    <!-- Live Chart -->
    <div class="card">
      <h2><span>📈 Real-Time Thermal Dynamics</span> <span style="font-size:0.75rem;color:#8b949e;">Rolling 60s Profile</span></h2>
      <canvas id="chartCanvas"></canvas>
      <div style="display:flex;gap:16px;margin-top:8px;font-size:0.75rem;justify-content:center;flex-wrap:wrap;">
        <span style="color:var(--hot);">■ Th,in</span>
        <span style="color:#ff8533;">■ Th,out</span>
        <span style="color:var(--cold);">■ Tc,in</span>
        <span style="color:#00ffff;">■ Tc,out</span>
        <span style="color:var(--flow);">■ Flow (LPM)</span>
      </div>
    </div>
  </div>

  <!-- TAB: GEMINI AI ADVISOR -->
  <div id="tab-gemini" class="tab-content">
    <div class="grid">
      <div class="card">
        <h2><span>🤖 Google Gemini AI Edge Integration</span></h2>
        <div class="control-group">
          <label>Google Gemini API Key:</label>
          <div style="display:flex;gap:8px;">
            <input type="text" id="txtGeminiKey" placeholder="AIzaSy...">
            <button class="btn btn-primary" onclick="saveGeminiKey()">Save</button>
          </div>
        </div>
        <div class="control-group">
          <label>Auto-Analysis Interval:</label>
          <select id="selAiInterval" onchange="saveAiInterval()">
            <option value="0">Disabled (Manual Only)</option>
            <option value="30000">Every 30 Seconds</option>
            <option value="60000">Every 1 Minute</option>
            <option value="120000">Every 2 Minutes</option>
          </select>
        </div>
        <div class="control-group">
          <label>
            <input type="checkbox" id="chkAiClosedLoop" onchange="toggleAiClosedLoop()">
            Enable Autonomous Closed-Loop Tuning (AI applies pump & heater setpoints within safety limits)
          </label>
        </div>
        <div class="btn-row">
          <button class="btn btn-primary" onclick="triggerGeminiQuery()">Run AI Thermodynamic Analysis Now</button>
        </div>
      </div>

      <div class="card">
        <h2><span>🧠 Latest AI Diagnostic Report</span> <span id="lblAiScore" class="val-accent">Score: --/100</span></h2>
        <div class="ai-box">
          <div id="aiAnalysisText">No AI evaluation performed yet. Connect to Station Wi-Fi with internet and click 'Run AI Analysis'.</div>
          <div class="ai-rec" id="aiRecText"></div>
          <div id="aiWarningText" style="color:var(--danger);margin-top:6px;"></div>
        </div>
        <div class="btn-row" style="margin-top:12px;">
          <button class="btn btn-warn" id="btnApplyAi" style="display:none;" onclick="applyAiRecommendation()">Apply Suggested Setpoints</button>
        </div>
      </div>
    </div>
  </div>

  <!-- TAB: LOGGING -->
  <div id="tab-logging" class="tab-content">
    <div class="card">
      <h2><span>💾 Experimental CSV Data Logging</span></h2>
      <p style="font-size:0.85rem;color:#8b949e;margin-bottom:12px;">The ESP32 logs live thermodynamic measurements into a circular RAM buffer at 1 Hz.</p>
      <div class="stat-row">
        <span class="stat-label">Logged Samples:</span>
        <span id="lblLogCount" class="stat-val">0 / 600</span>
      </div>
      <div class="btn-row" style="margin-top:16px;">
        <button class="btn btn-primary" onclick="window.open('/api/export.csv')">Download CSV Dataset</button>
        <button class="btn" onclick="clearLogData()">Reset Logger Buffer</button>
      </div>
    </div>
  </div>

  <!-- TAB: SETTINGS -->
  <div id="tab-settings" class="tab-content">
    <div class="grid">
      <div class="card">
        <h2><span>📶 Wi-Fi Station Configuration</span></h2>
        <div class="control-group">
          <label>Station Network SSID:</label>
          <input type="text" id="txtWifiSsid" placeholder="Home or Lab Network">
        </div>
        <div class="control-group">
          <label>Network Password:</label>
          <input type="password" id="txtWifiPass" placeholder="Password">
        </div>
        <div class="btn-row">
          <button class="btn btn-primary" onclick="saveWifiCredentials()">Connect & Save</button>
        </div>
      </div>

      <div class="card">
        <h2><span>⚙️ Calibration & Safety Overrides</span></h2>
        <div class="control-group">
          <label>Flow Meter K-Factor (Hz / LPM):</label>
          <input type="number" id="txtKFactor" step="0.1" value="7.5">
        </div>
        <div class="btn-row">
          <button class="btn btn-primary" onclick="saveCalibration()">Save Calibration</button>
          <button class="btn btn-warn" onclick="resetSafetyTrip()">Clear Safety Alarm</button>
        </div>
      </div>
    </div>
  </div>

  <script>
    let chartHistory = [];
    let currentRec = null;

    function switchTab(tabId) {
      document.querySelectorAll('.tab-content').forEach(el => el.classList.remove('active'));
      document.querySelectorAll('.tab-btn').forEach(el => el.classList.remove('active'));
      document.getElementById('tab-' + tabId).classList.add('active');
      event.target.classList.add('active');
    }

    function updatePumpSlider(val) {
      document.getElementById('lblPumpPct').innerText = val + '%';
    }

    function sendPumpCmd(pct) {
      document.getElementById('rngPump').value = pct;
      document.getElementById('lblPumpPct').innerText = pct + '%';
      fetch('/api/control', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({ pump_pct: parseInt(pct) })
      });
    }

    function sendHeaterManual(state) {
      fetch('/api/control', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({ heater_manual: state })
      });
    }

    function sendHeaterConfig() {
      const mode = parseInt(document.getElementById('selHeaterMode').value);
      const target = parseFloat(document.getElementById('rngTargetTemp').value);
      fetch('/api/control', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({ heater_mode: mode, target_temp: target })
      });
    }

    function triggerEStop() {
      fetch('/api/control', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({ estop: true })
      }).then(() => pollTelemetry());
    }

    function resetSafetyTrip() {
      fetch('/api/control', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({ reset_fault: true })
      }).then(() => pollTelemetry());
    }

    function saveGeminiKey() {
      const key = document.getElementById('txtGeminiKey').value;
      fetch('/api/ai/config', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({ api_key: key })
      }).then(r => alert('Gemini API Key Saved!'));
    }

    function saveAiInterval() {
      const interval = parseInt(document.getElementById('selAiInterval').value);
      fetch('/api/ai/config', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({ auto_interval_ms: interval })
      });
    }

    function toggleAiClosedLoop() {
      const checked = document.getElementById('chkAiClosedLoop').checked;
      fetch('/api/ai/config', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({ closed_loop: checked })
      });
    }

    function triggerGeminiQuery() {
      document.getElementById('badgeAi').innerText = 'ANALYZING...';
      fetch('/api/ai/query', { method: 'POST' })
        .then(r => r.json())
        .then(data => {
          pollTelemetry();
        });
    }

    function applyAiRecommendation() {
      if (!currentRec) return;
      sendPumpCmd(currentRec.recommended_pump_pwm);
      document.getElementById('selHeaterMode').value = "3"; // TIME-PWM
      fetch('/api/control', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({ heater_mode: 3, heater_duty: currentRec.recommended_heater_duty })
      });
      alert('Applied Gemini setpoints: Pump ' + currentRec.recommended_pump_pwm + '%, Heater ' + currentRec.recommended_heater_duty + '%');
    }

    function saveWifiCredentials() {
      const ssid = document.getElementById('txtWifiSsid').value;
      const pass = document.getElementById('txtWifiPass').value;
      fetch('/api/config', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({ wifi_ssid: ssid, wifi_pass: pass })
      }).then(() => alert('Wi-Fi credentials saved. ESP32 connecting...'));
    }

    function saveCalibration() {
      const k = parseFloat(document.getElementById('txtKFactor').value);
      fetch('/api/config', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({ flow_k_factor: k })
      }).then(() => alert('Calibration updated!'));
    }

    function clearLogData() {
      if (confirm('Clear telemetry log buffer?')) {
        fetch('/api/logging/clear', { method: 'POST' }).then(() => pollTelemetry());
      }
    }

    function pollTelemetry() {
      fetch('/api/telemetry')
        .then(r => r.json())
        .then(d => {
          // Temperatures
          document.getElementById('valThIn').innerText = d.th_in.toFixed(1) + ' °C';
          document.getElementById('valThOut').innerText = d.th_out.toFixed(1) + ' °C';
          document.getElementById('valTcIn').innerText = d.tc_in.toFixed(1) + ' °C';
          document.getElementById('valTcOut').innerText = d.tc_out.toFixed(1) + ' °C';
          document.getElementById('lblDeltaTh').innerText = 'ΔTh: ' + (d.th_in - d.th_out).toFixed(1) + '°C';
          document.getElementById('lblDeltaTc').innerText = (d.tc_out - d.tc_in).toFixed(1) + ' °C';

          // Flow & Power
          document.getElementById('valFlow').innerText = d.flow_lpm.toFixed(2) + ' LPM';
          document.getElementById('valVolume').innerText = d.total_liters.toFixed(1) + ' L';
          document.getElementById('valQHot').innerText = d.q_hot_kw.toFixed(2) + ' kW (' + Math.round(d.q_hot_w) + ' W)';
          document.getElementById('valQCold').innerText = d.q_cold_kw.toFixed(2) + ' kW';
          document.getElementById('valLmtd').innerText = d.lmtd.toFixed(1) + ' °C';
          document.getElementById('valEff').innerText = 'ε: ' + Math.round(d.effectiveness) + '%';

          // Actuators
          document.getElementById('lblPumpPct').innerText = d.pump_pct + '%';
          document.getElementById('lblHeaterState').innerText = d.heater_on ? 'ON (RELAY ACTIVE)' : 'OFF';
          document.getElementById('lblHeaterState').style.color = d.heater_on ? 'var(--hot)' : '#8b949e';

          // Badges
          const bSafety = document.getElementById('badgeSafety');
          if (d.safety_tripped) {
            bSafety.className = 'badge badge-trip';
            bSafety.innerText = d.safety_error;
          } else {
            bSafety.className = 'badge badge-ok';
            bSafety.innerText = 'NORMAL';
          }

          document.getElementById('badgeWifi').innerText = d.sta_connected ? ('STA: ' + d.sta_ip) : ('AP: ' + d.ap_ip);
          document.getElementById('lblLogCount').innerText = d.log_count + ' / ' + d.log_capacity;

          // AI Card
          if (d.ai && d.ai.has_rec) {
            currentRec = d.ai;
            document.getElementById('badgeAi').innerText = 'AI: ' + d.ai.suggested_action;
            document.getElementById('lblAiScore').innerText = 'Score: ' + Math.round(d.ai.efficiency_score) + '/100';
            document.getElementById('aiAnalysisText').innerText = d.ai.analysis;
            document.getElementById('aiRecText').innerText = 'Recommendation: Pump ' + d.ai.recommended_pump_pwm + '%, Heater ' + d.ai.recommended_heater_duty + '% (' + d.ai.suggested_action + ')';
            document.getElementById('aiWarningText').innerText = (d.ai.safety_warning && d.ai.safety_warning !== 'NONE') ? ('⚠️ ' + d.ai.safety_warning) : '';
            document.getElementById('btnApplyAi').style.display = 'inline-block';
          }

          // Append to chart history (keep 60 points)
          chartHistory.push({
            thIn: d.th_in,
            thOut: d.th_out,
            tcIn: d.tc_in,
            tcOut: d.tc_out,
            flow: d.flow_lpm
          });
          if (chartHistory.length > 60) chartHistory.shift();
          renderChart();
        })
        .catch(e => console.error(e));
    }

    function renderChart() {
      const c = document.getElementById('chartCanvas');
      const ctx = c.getContext('2d');
      const w = c.width = c.clientWidth;
      const h = c.height = c.clientHeight;

      ctx.clearRect(0, 0, w, h);
      if (chartHistory.length < 2) return;

      // Draw grid
      ctx.strokeStyle = '#1e242c';
      ctx.lineWidth = 1;
      for (let y = 0; y < h; y += 35) {
        ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
      }

      function drawLine(key, color, scaleMin, scaleMax) {
        ctx.strokeStyle = color;
        ctx.lineWidth = 2;
        ctx.beginPath();
        for (let i = 0; i < chartHistory.length; i++) {
          const x = (i / (chartHistory.length - 1)) * (w - 20) + 10;
          const val = chartHistory[i][key];
          const norm = Math.max(0, Math.min(1, (val - scaleMin) / (scaleMax - scaleMin)));
          const y = h - (norm * (h - 20)) - 10;
          if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
        }
        ctx.stroke();
      }

      // Temperatures scaled 10°C to 80°C
      drawLine('thIn', '#f85149', 10, 80);
      drawLine('thOut', '#ff8533', 10, 80);
      drawLine('tcIn', '#388bfd', 10, 80);
      drawLine('tcOut', '#00ffff', 10, 80);
      // Flow scaled 0 to 10 LPM
      drawLine('flow', '#3fb950', 0, 10);
    }

    // Initial load and fast 1s polling
    pollTelemetry();
    setInterval(pollTelemetry, 1000);
  </script>
</body>
</html>
)rawliteral";
