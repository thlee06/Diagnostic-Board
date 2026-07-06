#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <SPI.h>
#include <SD.h>
#include <math.h>
#include <time.h>
#include "config.h"

// ── SD Card SPI Pins ──────────────────────────────────────────────────────────
// Wiring: MOSI(D10)->CMD, MISO(D8)->DAT0, SCK(D9)->CLK, CS(D11)->CD/DAT3
const int SD_CS_PIN   = D11;
const int SD_MOSI_PIN = D10;
const int SD_SCK_PIN  = D9;
const int SD_MISO_PIN = D8;

// ── Sensor config ─────────────────────────────────────────────────────────────
const int NUM_SENSORS = 6;
const int thermistorPins[NUM_SENSORS] = {A0, A1, A2, A3, A4, A5};

// Exact series resistor values (ohms)
const float ACTUAL_SERIES_RESISTORS[NUM_SENSORS] = {
    927.0,  // A0
    928.0,  // A1
    929.0,  // A2
    933.0,  // A3
    928.0,  // A4
    929.0   // A5
};

// Final temperature offsets (°C)
const float TEMP_OFFSETS_C[NUM_SENSORS] = {
     -0.7,    // A0
     0.170,   // A1
     0.047,   // A2
     0.217,   // A3
    -0.110,   // A4
     0.351    // A5
};

// ── Session state ─────────────────────────────────────────────────────────────
enum SessionState { SESSION_IDLE = 0, SESSION_RECORDING = 1, SESSION_STOPPED = 2 };
static SessionState sessionState = SESSION_IDLE;
static bool sdAvailable = false;

// ── Server & cached state ─────────────────────────────────────────────────────
AsyncWebServer server(HTTP_PORT);
AsyncEventSource events("/events");
static float lastTemps[NUM_SENSORS] = {};
static int logCount = 0;

// ── Session file & markers ────────────────────────────────────────────────────
static char currentLogFile[64] = "";
static char markerLabels[6][64] = {
    "Marker 1", "Marker 2", "Marker 3",
    "Marker 4", "Marker 5", "Marker 6"
};

// ── Web page ──────────────────────────────────────────────────────────────────
static const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>AVOL — Diagnostic Board</title>
<style>
:root {
  --orange: #F05A00;
  --black:  #0D0D0D;
  --gray-d: #3A3A3A;
  --gray-m: #7A7A7A;
  --gray-l: #E2E2E2;
  --bg:     #FFFFFF;
}
* { box-sizing: border-box; margin: 0; padding: 0; }
body {
  background: var(--bg);
  color: var(--black);
  font-family: 'Helvetica Neue', Arial, sans-serif;
  padding: 36px 40px;
  max-width: 960px;
  margin: 0 auto;
}

/* ── Header ── */
header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 20px;
}
.brand { display: flex; align-items: center; gap: 16px; }
.logo  { height: 36px; width: auto; display: block; }
.brand-text { display: flex; flex-direction: column; gap: 1px; }
.company-name {
  font-size: 1.3rem;
  font-weight: 800;
  letter-spacing: 0.18em;
  color: var(--black);
  line-height: 1;
}
.board-name {
  font-size: 0.6rem;
  font-weight: 300;
  letter-spacing: 0.28em;
  color: var(--gray-m);
  text-transform: uppercase;
}
#badge {
  font-size: 0.62rem;
  font-weight: 700;
  letter-spacing: 0.2em;
  text-transform: uppercase;
  padding: 5px 12px;
  border: 1.5px solid var(--gray-l);
  color: var(--gray-m);
  display: flex;
  align-items: center;
  gap: 7px;
}
#badge.recording { border-color: var(--orange); color: var(--orange); }
#badge.stopped   { border-color: var(--black);  color: var(--black); }
#pulse {
  width: 7px; height: 7px;
  background: var(--orange);
  border-radius: 50%;
  display: none;
}
#badge.recording #pulse {
  display: block;
  animation: blink 1.1s ease-in-out infinite;
}
@keyframes blink { 0%,100%{opacity:1} 50%{opacity:0.15} }

/* ── Divider ── */
.rule { height: 2px; background: var(--orange); margin-bottom: 28px; }

/* ── Section labels ── */
.sec-label {
  font-size: 0.6rem;
  font-weight: 700;
  letter-spacing: 0.22em;
  color: var(--gray-m);
  text-transform: uppercase;
  margin-bottom: 10px;
  display: flex;
  align-items: center;
  gap: 8px;
}
.sec-label::before {
  content: '';
  display: inline-block;
  width: 0; height: 0;
  border-left: 5px solid var(--orange);
  border-top: 4px solid transparent;
  border-bottom: 4px solid transparent;
}

/* ── Sensor cards ── */
#grid {
  display: grid;
  grid-template-columns: repeat(3, 1fr);
  gap: 1px;
  background: var(--gray-l);
  border: 1px solid var(--gray-l);
  margin-bottom: 28px;
}
.card {
  background: var(--bg);
  padding: 18px 16px 16px;
  border-top: 3px solid var(--orange);
  text-align: left;
}
.lbl {
  font-size: 0.6rem;
  font-weight: 700;
  letter-spacing: 0.22em;
  color: var(--gray-m);
  text-transform: uppercase;
  margin-bottom: 8px;
}
.val {
  font-size: 1.7rem;
  font-weight: 800;
  color: var(--black);
  font-variant-numeric: tabular-nums;
  letter-spacing: -0.01em;
}
.val.err { font-size: 0.9rem; color: #CC1100; font-weight: 700; letter-spacing: 0.08em; }

/* ── Chart ── */
.chart-wrap { position: relative; margin-bottom: 28px; }
#leg {
  display: flex;
  flex-wrap: wrap;
  gap: 14px;
  margin-bottom: 10px;
}
.li {
  font-size: 0.62rem;
  font-weight: 500;
  letter-spacing: 0.12em;
  color: var(--gray-d);
  text-transform: uppercase;
  display: flex;
  align-items: center;
  gap: 6px;
}
.dot { width: 8px; height: 8px; flex-shrink: 0; }
canvas {
  display: block;
  width: 100%;
  border: 1px solid var(--gray-l);
  border-top: 2px solid var(--black);
}

/* ── Controls ── */
#bar {
  display: flex;
  gap: 0;
  align-items: stretch;
  flex-wrap: wrap;
  margin-bottom: 10px;
}
button.btn {
  padding: 11px 22px;
  font-family: inherit;
  font-size: 0.7rem;
  font-weight: 700;
  letter-spacing: 0.18em;
  text-transform: uppercase;
  cursor: pointer;
  border: 1.5px solid var(--black);
  border-right: none;
  background: var(--bg);
  color: var(--black);
  transition: background 0.12s, color 0.12s;
}
button.btn:last-of-type { border-right: 1.5px solid var(--black); }
button.btn:hover:not(:disabled) { background: var(--black); color: var(--bg); }
#startbtn:hover:not(:disabled) { background: var(--orange); border-color: var(--orange); color: #fff; }
#startbtn:not(:disabled)       { border-color: var(--orange); color: var(--orange); }
button.btn:disabled { background: #F5F5F5; color: var(--gray-l); border-color: var(--gray-l); cursor: not-allowed; }
#st {
  font-size: 0.65rem;
  font-weight: 400;
  letter-spacing: 0.1em;
  color: var(--gray-m);
  margin-left: 20px;
  align-self: center;
}
#sdwarn {
  font-size: 0.65rem;
  font-weight: 600;
  letter-spacing: 0.1em;
  color: #CC1100;
  margin-top: 8px;
  text-transform: uppercase;
}
#irow {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-top: 12px;
  font-size: 0.65rem;
  font-weight: 700;
  letter-spacing: 0.14em;
  color: var(--gray-d);
  text-transform: uppercase;
}
#ival {
  width: 58px;
  padding: 9px 6px;
  font-family: inherit;
  font-size: 0.7rem;
  font-weight: 700;
  border: 1.5px solid var(--black);
  color: var(--black);
  text-align: center;
}

/* ── Markers ── */
#markers {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 8px;
  margin-bottom: 28px;
}
.marker-row { display: flex; align-items: center; gap: 8px; }
.mbtn {
  flex-shrink: 0;
  width: 90px;
  padding: 9px 0;
  border-right: 1.5px solid var(--black) !important;
}
.mbtn:disabled { background: #F5F5F5; color: var(--gray-l); border-color: var(--gray-l) !important; cursor: not-allowed; }
.minput {
  flex: 1;
  padding: 9px 8px;
  font-family: inherit;
  font-size: 0.68rem;
  font-weight: 500;
  border: 1.5px solid var(--gray-l);
  color: var(--black);
  min-width: 0;
}
.minput:focus { outline: none; border-color: var(--black); }
</style>
</head>
<body>

<header>
  <div class="brand">
    <img class="logo" src="data:image/jpeg;base64,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" alt="AVOL">
    <div class="brand-text">
      <span class="company-name">AVOL</span>
      <span class="board-name">Diagnostic Board</span>
    </div>
  </div>
  <div style="display:flex;align-items:center;gap:20px;">
    <div id="sdstat" style="font-size:0.62rem;font-weight:600;letter-spacing:0.1em;color:var(--gray-m);text-transform:uppercase;"></div>
    <div id="badge"><span id="pulse"></span><span id="badge-text">CONNECTING</span></div>
  </div>
</header>

<div class="rule"></div>

<div class="sec-label">Live Sensors</div>
<div id="grid">
  <div class="card"><div class="lbl">Channel A0</div><div class="val" id="v0">--</div></div>
  <div class="card"><div class="lbl">Channel A1</div><div class="val" id="v1">--</div></div>
  <div class="card"><div class="lbl">Channel A2</div><div class="val" id="v2">--</div></div>
  <div class="card"><div class="lbl">Channel A3</div><div class="val" id="v3">--</div></div>
  <div class="card"><div class="lbl">Channel A4</div><div class="val" id="v4">--</div></div>
  <div class="card"><div class="lbl">Channel A5</div><div class="val" id="v5">--</div></div>
</div>

<div class="sec-label">Temperature History</div>
<div class="chart-wrap">
  <div id="leg"></div>
  <canvas id="cv" height="200"></canvas>
</div>

<div class="sec-label">Markers</div>
<div id="markers">
  <div class="marker-row"><button class="btn mbtn" id="mb1" onclick="mark(1)" disabled>Mark 1</button><input class="minput" id="ml1" type="text" value="Marker 1" onchange="setLabel(1,this.value)" placeholder="Label for mark 1"></div>
  <div class="marker-row"><button class="btn mbtn" id="mb2" onclick="mark(2)" disabled>Mark 2</button><input class="minput" id="ml2" type="text" value="Marker 2" onchange="setLabel(2,this.value)" placeholder="Label for mark 2"></div>
  <div class="marker-row"><button class="btn mbtn" id="mb3" onclick="mark(3)" disabled>Mark 3</button><input class="minput" id="ml3" type="text" value="Marker 3" onchange="setLabel(3,this.value)" placeholder="Label for mark 3"></div>
  <div class="marker-row"><button class="btn mbtn" id="mb4" onclick="mark(4)" disabled>Mark 4</button><input class="minput" id="ml4" type="text" value="Marker 4" onchange="setLabel(4,this.value)" placeholder="Label for mark 4"></div>
  <div class="marker-row"><button class="btn mbtn" id="mb5" onclick="mark(5)" disabled>Mark 5</button><input class="minput" id="ml5" type="text" value="Marker 5" onchange="setLabel(5,this.value)" placeholder="Label for mark 5"></div>
  <div class="marker-row"><button class="btn mbtn" id="mb6" onclick="mark(6)" disabled>Mark 6</button><input class="minput" id="ml6" type="text" value="Marker 6" onchange="setLabel(6,this.value)" placeholder="Label for mark 6"></div>
</div>

<div id="bar">
  <button class="btn" id="startbtn" onclick="startLog()" disabled>Start Session</button>
  <button class="btn" id="stopbtn"  onclick="stopLog()"  disabled>Stop Session</button>
  <button class="btn" id="savebtn"  onclick="saveSession()" disabled>Save &amp; Download</button>
  <span id="st"></span>
</div>
<div id="sdwarn"></div>
<div id="irow">
  Log every&nbsp;<input id="ival" type="number" min="1" max="3600" value="1">&nbsp;s
  <button class="btn" style="border:1.5px solid var(--black)" onclick="setLogInterval()">Set</button>
</div>

<div id="modal-bg" style="display:none;position:fixed;inset:0;background:rgba(0,0,0,0.45);z-index:100;align-items:center;justify-content:center;">
  <div style="background:#fff;padding:32px 36px;max-width:420px;width:90%;border-top:3px solid var(--orange);">
    <div style="font-size:0.7rem;font-weight:800;letter-spacing:0.2em;text-transform:uppercase;margin-bottom:20px;">Save Session</div>
    <div style="font-size:0.62rem;font-weight:700;letter-spacing:0.14em;text-transform:uppercase;color:var(--gray-m);margin-bottom:6px;">Filename</div>
    <input id="save-name" type="text" style="width:100%;padding:10px 8px;font-family:inherit;font-size:0.8rem;border:1.5px solid var(--black);margin-bottom:16px;">
    <div style="font-size:0.62rem;font-weight:700;letter-spacing:0.14em;text-transform:uppercase;color:var(--gray-m);margin-bottom:6px;">Description <span style="font-weight:400;text-transform:none;">(optional)</span></div>
    <textarea id="save-desc" rows="3" style="width:100%;padding:10px 8px;font-family:inherit;font-size:0.8rem;border:1.5px solid var(--black);resize:vertical;margin-bottom:20px;"></textarea>
    <div id="overwrite-warn" style="display:none;font-size:0.62rem;font-weight:700;color:#CC1100;letter-spacing:0.1em;text-transform:uppercase;margin-bottom:14px;">&#9650; A file with this name already exists and will be overwritten.</div>
    <div style="display:flex;gap:0;">
      <button class="btn" onclick="confirmSave()" style="border-right:none;">Save &amp; Download</button>
      <button class="btn" onclick="closeModal()">Cancel</button>
    </div>
  </div>
</div>

<script>
const COLORS = ['#F05A00','#0D0D0D','#5C6BC0','#2E7D32','#8D6E63','#7A7A7A'];
const LABELS = ['A0','A1','A2','A3','A4','A5'];
const MAX_PTS = 300;
const hist = { t: [], v: LABELS.map(() => []) };

const leg = document.getElementById('leg');
LABELS.forEach((l, i) => {
  const s = document.createElement('span');
  s.className = 'li';
  s.innerHTML = '<span class="dot" style="background:' + COLORS[i] + '"></span>' + l;
  leg.appendChild(s);
});

function push(t, vals) {
  hist.t.push(t);
  vals.forEach((v, i) => hist.v[i].push(v));
  if (hist.t.length > MAX_PTS) { hist.t.shift(); hist.v.forEach(a => a.shift()); }
}

function draw() {
  const cv = document.getElementById('cv');
  const W = cv.offsetWidth || 600;
  cv.width = W; cv.height = 200;
  const ctx = cv.getContext('2d');
  ctx.fillStyle = '#FAFAFA'; ctx.fillRect(0, 0, W, 200);
  const n = hist.t.length;
  if (n < 2) return;
  let mn = Infinity, mx = -Infinity;
  hist.v.forEach(a => a.forEach(v => {
    if (Math.abs(v) < 500) { mn = Math.min(mn, v); mx = Math.max(mx, v); }
  }));
  if (!isFinite(mn)) return;
  mn -= 0.3; mx += 0.3;
  const rng = Math.max(mx - mn, 0.5);
  const toX = i => 38 + (i / (n - 1)) * (W - 50);
  const toY = v => 185 - ((v - mn) / rng) * 165;
  ctx.strokeStyle = '#E2E2E2'; ctx.lineWidth = 1;
  for (let k = 0; k <= 4; k++) {
    const yp = toY(mn + rng * k / 4);
    ctx.beginPath(); ctx.moveTo(38, yp); ctx.lineTo(W - 12, yp); ctx.stroke();
    ctx.fillStyle = '#9A9A9A'; ctx.font = '700 9px Arial, sans-serif'; ctx.textAlign = 'right';
    ctx.fillText((mn + rng * k / 4).toFixed(1) + '\xb0', 36, yp + 3);
  }
  hist.v.forEach((arr, si) => {
    ctx.strokeStyle = COLORS[si]; ctx.lineWidth = 2; ctx.beginPath(); let first = true;
    arr.forEach((v, i) => {
      if (Math.abs(v) > 500) { first = true; return; }
      first ? ctx.moveTo(toX(i), toY(v)) : ctx.lineTo(toX(i), toY(v));
      first = false;
    });
    ctx.stroke();
  });
}

// ── SSE ───────────────────────────────────────────────────────────────────────
let es;
function connectSSE() {
  if (es) es.close();
  es = new EventSource('/events');
  es.addEventListener('temp', function(e) {
    const p     = e.data.split(',');
    const vals  = p.slice(0, 6).map(Number);
    const ms    = parseInt(p[6]);
    const logC  = parseInt(p[7]);
    const state = parseInt(p[8]);
    const sdOk  = parseInt(p[9]);

    vals.forEach((v, i) => {
      const el = document.getElementById('v' + i);
      if (v > 500)       { el.textContent = 'OPEN';  el.className = 'val err'; }
      else if (v < -500) { el.textContent = 'SHORT'; el.className = 'val err'; }
      else               { el.textContent = v.toFixed(2) + '\xb0C'; el.className = 'val'; }
    });

    document.getElementById('sdwarn').textContent =
      sdOk ? '' : '\u25b2  SD CARD NOT DETECTED \u2014 INSERT CARD AND RESTART';

    const badge = document.getElementById('badge');
    const btext = document.getElementById('badge-text');
    badge.className = (state === 1) ? 'recording' : (state === 2) ? 'stopped' : '';
    btext.textContent = (state === 1) ? 'Recording' : (state === 2) ? 'Session Stopped' : 'Idle';

    const recording = (state === 1);
    const stopped   = (state === 2);
    document.getElementById('startbtn').disabled = recording || !sdOk;
    document.getElementById('stopbtn').disabled  = !recording;
    document.getElementById('savebtn').disabled  = !stopped;
    [1,2,3,4,5,6].forEach(n => {
      const b = document.getElementById('mb' + n);
      if (b) b.disabled = !recording;
    });

    const fmt = n => n.toLocaleString();
    const stEl = document.getElementById('st');
    if      (state === 0) stEl.textContent = 'Ready to record';
    else if (state === 1) stEl.textContent = fmt(logC) + ' rows captured';
    else if (state === 2) stEl.textContent = fmt(logC) + ' rows \u2014 save your session';

    push(ms, vals);
    draw();
  });
  es.onerror = function() {
    document.getElementById('badge-text').textContent = 'Reconnecting';
    document.getElementById('badge').className = '';
    es.close();
    setTimeout(connectSSE, 1000);
  };
}
connectSSE();

// ── SD status ─────────────────────────────────────────────────────────────────
function pollStatus() {
  fetch('/status').then(r => r.json()).then(d => {
    const el = document.getElementById('sdstat');
    if (!d.sd) { el.textContent = 'No SD'; el.style.color = '#CC1100'; return; }
    const gb = (d.free / 1073741824).toFixed(1);
    el.style.color = (d.free < 200 * 1048576) ? '#CC1100' : 'var(--gray-m)';
    el.textContent = gb + ' GB free';
  }).catch(() => {});
}
pollStatus();
setInterval(pollStatus, 5000);

// ── Controls ──────────────────────────────────────────────────────────────────
function setLogInterval() {
  const v = parseInt(document.getElementById('ival').value);
  if (v >= 1 && v <= 3600) fetch('/log/interval?s=' + v);
}
function startLog() {
  fetch('/log/start').then(r => { if (!r.ok) r.text().then(t => alert('Error: ' + t)); });
}
function stopLog() { fetch('/log/stop'); }

function mark(n) { fetch('/mark?n=' + n).catch(() => {}); }
function setLabel(n, val) {
  const label = val.trim() || ('Marker ' + n);
  fetch('/marker/set?n=' + n + '&label=' + encodeURIComponent(label)).catch(() => {});
}

// ── Save modal ────────────────────────────────────────────────────────────────
function saveSession() {
  document.getElementById('save-name').value = 'session_' + new Date().toISOString().slice(0,10).replace(/-/g,'_');
  document.getElementById('save-desc').value = '';
  document.getElementById('overwrite-warn').style.display = 'none';
  document.getElementById('modal-bg').style.display = 'flex';
  document.getElementById('save-name').focus();
}
function closeModal() { document.getElementById('modal-bg').style.display = 'none'; }
function confirmSave() {
  const name = document.getElementById('save-name').value.trim();
  const desc = document.getElementById('save-desc').value.trim();
  if (!name) { document.getElementById('save-name').focus(); return; }
  const url = '/log/save?name=' + encodeURIComponent(name) + '&desc=' + encodeURIComponent(desc);
  fetch(url).then(r => {
    const overwritten = r.headers.get('X-Overwrite') === 'true';
    if (overwritten) document.getElementById('overwrite-warn').style.display = 'block';
    if (r.ok) {
      return r.blob().then(b => {
        const a = document.createElement('a');
        a.href = URL.createObjectURL(b);
        a.download = name.replace(/[^a-zA-Z0-9_\-]/g,'_') + '.csv';
        a.click();
        closeModal();
      });
    } else {
      return r.text().then(t => alert('Save failed: ' + t));
    }
  }).catch(e => alert('Network error: ' + e));
}
document.getElementById('modal-bg').addEventListener('click', function(e) {
  if (e.target === this) closeModal();
});
</script>
</body>
</html>
)rawliteral";

// ── WiFi helpers ──────────────────────────────────────────────────────────────
static bool wifiReady = false;   // true once connected and services started

static void startNetworkServices() {
    static bool serverStarted = false;
    wifiReady = true;
    configTime(NTP_UTC_OFFSET_SEC, 0, "pool.ntp.org");
    if (!serverStarted) {
        MDNS.end();
        server.begin();
        serverStarted = true;
        if (MDNS.begin("diagboard"))
            Serial.print("Web server up — http://diagboard.local  or  http://");
        else
            Serial.print("Web server up — http://");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("WiFi reconnected — server already running");
    }
}

// ── Thermistor reading ─────────────────────────────────────────────────────────
float readTemperatureCelsius(int idx) {
    long sum = 0;
    for (int i = 0; i < ADC_SAMPLES; i++) {
        sum += analogRead(thermistorPins[idx]);
    }
    int avg = sum / ADC_SAMPLES;

    if (avg <= 0)             return 999.0f;
    if (avg >= ADC_MAX_VALUE) return -999.0f;

    float r_therm = ACTUAL_SERIES_RESISTORS[idx] * (float)avg / (float)(ADC_MAX_VALUE - avg);
    float inv_T = (1.0f / THERMISTOR_T0) + (1.0f / THERMISTOR_B) * logf(r_therm / THERMISTOR_R0);
    return (1.0f / inv_T) - 273.15f + TEMP_OFFSETS_C[idx];
}

// ── Log helpers ───────────────────────────────────────────────────────────────
static const char LOG_HEADER[] = "timestamp,A0_C,A1_C,A2_C,A3_C,A4_C,A5_C,note\n";
static unsigned int logIntervalSec = LOG_INTERVAL_DEFAULT_SEC;

void initLog() {
    const char* tempPath = "/session_temp.csv";
    if (SD.exists(tempPath)) {
        strlcpy(currentLogFile, tempPath, sizeof(currentLogFile));
        File f = SD.open(tempPath, FILE_READ);
        size_t sz = f ? f.size() : 0;
        if (f) f.close();
        const size_t headerLen = strlen(LOG_HEADER);
        logCount = sz > headerLen ? (sz - headerLen) / 65 : 0;
        sessionState = SESSION_RECORDING;
        Serial.printf("Resumed session from SD — ~%d rows already logged\n", logCount);
    } else {
        logCount = 0;
        sessionState = SESSION_IDLE;
    }
}

void appendLog(float* temps) {
    if (sessionState != SESSION_RECORDING) return;
    if (logCount >= MAX_LOG_ENTRIES) {
        sessionState = SESSION_STOPPED;
        return;
    }
    if (currentLogFile[0] == '\0') return;
    File f = SD.open(currentLogFile, FILE_APPEND);
    if (!f) return;

    char ts[24] = "unknown";
    struct tm t;
    if (getLocalTime(&t, 0)) {
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &t);
    }

    char row[112];
    snprintf(row, sizeof(row), "%s,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,\n",
             ts,
             temps[0], temps[1], temps[2], temps[3], temps[4], temps[5]);
    f.print(row);
    f.close();
    logCount++;
}

void appendMarker(int n, float* temps) {
    if (sessionState != SESSION_RECORDING) return;
    if (currentLogFile[0] == '\0') return;
    File f = SD.open(currentLogFile, FILE_APPEND);
    if (!f) return;

    char ts[24] = "unknown";
    struct tm t;
    if (getLocalTime(&t, 0)) {
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &t);
    }

    char label[80];
    snprintf(label, sizeof(label), "MARK %d: %s", n, markerLabels[n - 1]);

    char row[180];
    snprintf(row, sizeof(row), "%s,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%s\n",
             ts,
             temps[0], temps[1], temps[2], temps[3], temps[4], temps[5],
             label);
    f.print(row);
    f.close();
    logCount++;
}

// Sanitize a user-provided session name into a safe filename (no path chars)
String sanitizeName(const String& raw) {
    String out;
    out.reserve(raw.length());
    for (unsigned int i = 0; i < raw.length() && out.length() < 40; i++) {
        char c = raw[i];
        if (isalnum(c) || c == '-' || c == '_') out += c;
        else if (c == ' ')                       out += '_';
        // all other characters are dropped
    }
    if (out.length() == 0) out = "session";
    return out;
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    unsigned long t0 = millis();
    while (!Serial && millis() - t0 < 3000) { delay(10); }

    for (int i = 0; i < NUM_SENSORS; i++) {
        pinMode(thermistorPins[i], INPUT);
    }
    analogReadResolution(12);

    Serial.println("Waiting 2 s for SD power to stabilize...");
    delay(2000);
    SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    if (!SD.begin(SD_CS_PIN, SPI, 4000000U)) {
        Serial.println("ERROR: SD mount failed — check wiring and power");
        Serial.println("  MOSI(D10)->CMD  MISO(D8)->DAT0  SCK(D9)->CLK  CS(D11)->CD/DAT3");
        sdAvailable = false;
    } else {
        Serial.printf("SD mounted — card size: %llu MB\n", SD.cardSize() / (1024 * 1024));
        sdAvailable = true;
        initLog();
    }

    // Hand off to the ESP32 WiFi stack — it will keep trying until it connects
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("Connecting to \"%s\" (will keep trying)...\n", WIFI_SSID);

    // Serve the dashboard
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "text/html", PAGE);
    });

    // Start a new session
    server.on("/log/start", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!sdAvailable) { req->send(503, "text/plain", "No SD card"); return; }
        if (sessionState == SESSION_RECORDING) { req->send(400, "text/plain", "Already recording"); return; }
        uint64_t freeBytes = SD.totalBytes() - SD.usedBytes();
        if (freeBytes < (uint64_t)SD_MIN_FREE_MB * 1024 * 1024) {
            req->send(507, "text/plain", "Insufficient storage"); return;
        }
        const char* tempPath = "/session_temp.csv";
        if (SD.exists(tempPath)) SD.remove(tempPath);
        File f = SD.open(tempPath, FILE_WRITE);
        if (!f) { req->send(500, "text/plain", "Failed to create file"); return; }
        f.print(LOG_HEADER);
        f.close();
        strlcpy(currentLogFile, tempPath, sizeof(currentLogFile));
        logCount = 0;
        sessionState = SESSION_RECORDING;
        req->send(200, "text/plain", "OK");
    });

    // Stop logging — data stays on SD, waiting for save
    server.on("/log/stop", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (sessionState != SESSION_RECORDING) { req->send(400, "text/plain", "Not recording"); return; }
        sessionState = SESSION_STOPPED;
        req->send(200, "text/plain", "OK");
    });

    // Save session — append metadata, rename, serve download
    server.on("/log/save", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!sdAvailable) { req->send(503, "text/plain", "No SD card"); return; }
        if (sessionState != SESSION_STOPPED) { req->send(400, "text/plain", "Not stopped"); return; }
        if (currentLogFile[0] == '\0' || !SD.exists(currentLogFile)) {
            req->send(404, "text/plain", "No session file"); return;
        }
        String raw  = req->hasParam("name") ? req->getParam("name")->value() : "session";
        String desc = req->hasParam("desc") ? req->getParam("desc")->value() : "";
        String safe = sanitizeName(raw);
        String dest = "/" + safe + ".csv";
        bool overwrite = SD.exists(dest);
        if (overwrite) SD.remove(dest);
        {
            File f = SD.open(currentLogFile, FILE_APPEND);
            if (f) {
                f.print("# ---\n");
                f.printf("# Name: %s\n", safe.c_str());
                if (desc.length() > 0) f.printf("# Description: %s\n", desc.c_str());
                for (int i = 0; i < 6; i++) {
                    f.printf("# Marker %d: %s\n", i + 1, markerLabels[i]);
                }
                f.close();
            }
        }
        SD.rename(currentLogFile, dest.c_str());
        sessionState = SESSION_IDLE;
        logCount = 0;
        currentLogFile[0] = '\0';
        String disp = "attachment; filename=\"" + safe + ".csv\"";
        AsyncWebServerResponse *resp = req->beginResponse(SD, dest.c_str(), "text/csv");
        resp->addHeader("Content-Disposition", disp.c_str());
        if (overwrite) resp->addHeader("X-Overwrite", "true");
        req->send(resp);
    });

    // Set log interval — ?s=N
    server.on("/log/interval", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (req->hasParam("s")) {
            int v = req->getParam("s")->value().toInt();
            if (v >= 1 && v <= 3600) logIntervalSec = (unsigned int)v;
        }
        req->send(200, "text/plain", String(logIntervalSec).c_str());
    });

    // Write a marker row for button n (1–6)
    server.on("/mark", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (sessionState != SESSION_RECORDING) { req->send(400, "text/plain", "Not recording"); return; }
        if (!req->hasParam("n")) { req->send(400, "text/plain", "Missing n"); return; }
        int n = req->getParam("n")->value().toInt();
        if (n < 1 || n > 6) { req->send(400, "text/plain", "n must be 1-6"); return; }
        appendMarker(n, lastTemps);
        req->send(200, "text/plain", "OK");
    });

    // Update a marker label in RAM — ?n=1&label=Fan+on
    server.on("/marker/set", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("n") || !req->hasParam("label")) {
            req->send(400, "text/plain", "Missing params"); return;
        }
        int n = req->getParam("n")->value().toInt();
        if (n < 1 || n > 6) { req->send(400, "text/plain", "n must be 1-6"); return; }
        String lbl = req->getParam("label")->value();
        lbl.trim();
        if (lbl.length() == 0) lbl = String("Marker ") + n;
        strlcpy(markerLabels[n - 1], lbl.c_str(), sizeof(markerLabels[0]));
        req->send(200, "text/plain", "OK");
    });

    // SD space and session state
    server.on("/status", HTTP_GET, [](AsyncWebServerRequest *req) {
        uint64_t total = sdAvailable ? SD.totalBytes() : 0;
        uint64_t used  = sdAvailable ? SD.usedBytes()  : 0;
        uint64_t free_ = total - used;
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "{\"free\":%llu,\"total\":%llu,\"state\":%d,\"rows\":%d,\"sd\":%d}",
                 free_, total, (int)sessionState, logCount, (int)sdAvailable);
        req->send(200, "application/json", buf);
    });

    // SSE: confirm connection on client connect
    events.onConnect([](AsyncEventSourceClient *client) {
        client->send("connected", "ping", millis());
    });
    server.addHandler(&events);

    // Routes registered — server.begin() is called in startNetworkServices() once WiFi is up
    Serial.println("Routes registered. Waiting for WiFi...\n");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    // WiFi watchdog — lets the ESP32 stack manage reconnection, we just react to state changes
    static unsigned long lastWiFiCheck = 0;
    if (millis() - lastWiFiCheck >= WIFI_WATCHDOG_INTERVAL_MS) {
        lastWiFiCheck = millis();
        bool connected = (WiFi.status() == WL_CONNECTED);
        if (connected && !wifiReady) {
            startNetworkServices();           // just connected
        } else if (!connected && wifiReady) {
            wifiReady = false;
            Serial.println("WiFi lost — reconnecting...");  // stack auto-reconnects
        }
    }

    // Keep-alive ping — forces the async server to flush dead SSE connections
    static unsigned long lastPing = 0;
    if (events.count() > 0 && millis() - lastPing >= 3000) {
        lastPing = millis();
        events.send("", "ping", millis());
    }

    // Sample sensors and push to live display at 100 ms
    static unsigned long lastSample = 0;
    if (millis() - lastSample >= SEND_INTERVAL_MS) {
        lastSample = millis();

        for (int i = 0; i < NUM_SENSORS; i++) {
            lastTemps[i] = readTemperatureCelsius(i);
        }

        char payload[128];
        snprintf(payload, sizeof(payload),
                 "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%lu,%d,%d,%d",
                 lastTemps[0], lastTemps[1], lastTemps[2],
                 lastTemps[3], lastTemps[4], lastTemps[5],
                 (unsigned long)millis(), logCount,
                 (int)sessionState,
                 (int)sdAvailable);
        if (events.count() > 0)
            events.send(payload, "temp", millis());
    }

    // Write to SD at user-configured interval — runs independently of display rate
    static unsigned long lastLog = 0;
    if (sessionState == SESSION_RECORDING &&
        millis() - lastLog >= (unsigned long)logIntervalSec * 1000UL) {
        lastLog = millis();
        appendLog(lastTemps);
    }
}
