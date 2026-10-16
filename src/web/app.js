let chartHistory = [];
let currentRec = null;

function switchTab(tabId) {
  document.querySelectorAll('.tab-content').forEach(el => el.classList.remove('active'));
  document.querySelectorAll('.tab-btn').forEach(el => el.classList.remove('active'));
  document.getElementById('tab-' + tabId).classList.add('active');
  if (event && event.target) {
    event.target.classList.add('active');
  }
}

function updatePumpSlider(val) {
  document.getElementById('lblPumpPct').innerText = val + '%';
}

function sendPumpCmd(pct) {
  document.getElementById('rngPump').value = pct;
  document.getElementById('lblPumpPct').innerText = pct + '%';
  fetch('/api/control', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ pump_pct: parseInt(pct) })
  });
}

function sendHeaterManual(state) {
  fetch('/api/control', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ heater_manual: state })
  });
}

function sendHeaterConfig() {
  const mode = parseInt(document.getElementById('selHeaterMode').value);
  const target = parseFloat(document.getElementById('rngTargetTemp').value);
  fetch('/api/control', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ heater_mode: mode, target_temp: target })
  });
}

function triggerEStop() {
  fetch('/api/control', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ estop: true })
  }).then(() => pollTelemetry());
}

function resetSafetyTrip() {
  fetch('/api/control', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ reset_fault: true })
  }).then(() => pollTelemetry());
}

function saveGeminiKey() {
  const key = document.getElementById('txtGeminiKey').value;
  if (!key) return;
  fetch('/api/ai/config', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ api_key: key })
  }).then(() => {
    alert('Gemini API Key Saved to Flash!');
    document.getElementById('txtGeminiKey').value = '';
    pollTelemetry();
  });
}

function saveAiModel() {
  const model = document.getElementById('selAiModel').value;
  fetch('/api/ai/config', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ model: model })
  }).then(() => alert('Gemini Model updated to: ' + model));
}

function saveAiInterval() {
  const interval = parseInt(document.getElementById('selAiInterval').value);
  fetch('/api/ai/config', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ auto_interval_ms: interval })
  });
}

function toggleAiClosedLoop() {
  const checked = document.getElementById('chkAiClosedLoop').checked;
  fetch('/api/ai/config', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ closed_loop: checked })
  });
}

function triggerGeminiQuery() {
  document.getElementById('badgeAi').innerText = 'ANALYZING...';
  fetch('/api/ai/query', { method: 'POST' })
    .then(r => r.json())
    .then(() => {
      pollTelemetry();
    });
}

function applyAiRecommendation() {
  if (!currentRec) return;
  sendPumpCmd(currentRec.recommended_pump_pwm);
  document.getElementById('selHeaterMode').value = "3"; // TIME-PWM
  fetch('/api/control', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ heater_mode: 3, heater_duty: currentRec.recommended_heater_duty })
  });
  alert('Applied Gemini setpoints: Pump ' + currentRec.recommended_pump_pwm + '%, Heater ' + currentRec.recommended_heater_duty + '%');
}

function saveWifiCredentials() {
  const ssid = document.getElementById('txtWifiSsid').value;
  const pass = document.getElementById('txtWifiPass').value;
  fetch('/api/config', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ wifi_ssid: ssid, wifi_pass: pass })
  }).then(() => alert('Wi-Fi credentials saved. ESP32 connecting...'));
}

function saveCalibration() {
  const k = parseFloat(document.getElementById('txtKFactor').value);
  fetch('/api/config', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
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
      if (d.ai) {
        if (d.ai.model && document.activeElement !== document.getElementById('selAiModel')) {
          document.getElementById('selAiModel').value = d.ai.model;
        }
        const lblKey = document.getElementById('lblKeyStatus');
        if (lblKey) {
          if (d.ai.has_key) {
            lblKey.innerText = '✓ Key Configured';
            lblKey.style.color = 'var(--safe)';
          } else {
            lblKey.innerText = '⚠ Key Not Set';
            lblKey.style.color = 'var(--danger)';
          }
        }

        if (d.ai.has_rec) {
          currentRec = d.ai;
          document.getElementById('badgeAi').innerText = 'AI: ' + d.ai.suggested_action;
          document.getElementById('lblAiScore').innerText = 'Score: ' + Math.round(d.ai.efficiency_score) + '/100';
          document.getElementById('aiAnalysisText').innerText = d.ai.analysis;
          document.getElementById('aiRecText').innerText = 'Recommendation: Pump ' + d.ai.recommended_pump_pwm + '%, Heater ' + d.ai.recommended_heater_duty + '% (' + d.ai.suggested_action + ')';
          document.getElementById('aiWarningText').innerText = (d.ai.safety_warning && d.ai.safety_warning !== 'NONE') ? ('⚠️ ' + d.ai.safety_warning) : '';
          document.getElementById('btnApplyAi').style.display = 'inline-block';
        }
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
  if (!c) return;
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
