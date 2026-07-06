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
  <div id="badge"><span id="pulse"></span><span id="badge-text">CONNECTING</span></div>
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

<div id="bar">
  <button class="btn" id="startbtn" onclick="startLog()" disabled>Start Log</button>
  <button class="btn" id="stopbtn"  onclick="stopLog()"  disabled>Stop Log</button>
  <button class="btn" id="dlbtn"    onclick="downloadLog()" disabled>Download Log</button>
  <span id="st"></span>
</div>
<div id="sdwarn"></div>
<div id="irow">
  Log every&nbsp;<input id="ival" type="number" min="1" max="3600" value="1">&nbsp;s
  <button class="btn" style="border:1.5px solid var(--black)" onclick="setLogInterval()">Set</button>
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
    btext.textContent = (state === 1) ? 'Recording' : (state === 2) ? 'Session Saved' : 'Idle';

    document.getElementById('startbtn').disabled = (state === 1) || !sdOk;
    document.getElementById('stopbtn').disabled  = (state !== 1);
    document.getElementById('dlbtn').disabled    = (state !== 2);

    const fmt = n => n.toLocaleString();
    const stEl = document.getElementById('st');
    if      (state === 0) stEl.textContent = 'Ready to record';
    else if (state === 1) stEl.textContent = fmt(logC) + ' rows captured';
    else if (state === 2) stEl.textContent = fmt(logC) + ' rows \u2014 name and download your session';

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

function setLogInterval() {
  const v = parseInt(document.getElementById('ival').value);
  if (v >= 1 && v <= 3600) fetch('/log/interval?s=' + v);
}
function startLog()    { fetch('/log/start'); }
function stopLog()     { fetch('/log/stop'); }
function downloadLog() {
  const today = new Date().toISOString().slice(0,10).replace(/-/g,'_');
  const name  = prompt('Enter a name for this session:', 'session_' + today);
  if (!name || !name.trim()) return;
  window.location.href = '/download?name=' + encodeURIComponent(name.trim());
}
</script>
</body>
</html>
)rawliteral";

// ── WiFi helpers ──────────────────────────────────────────────────────────────
static bool wifiReady = false;   // true once connected and services started

static void startNetworkServices() {
    wifiReady = true;
    configTime(NTP_UTC_OFFSET_SEC, 0, "pool.ntp.org");
    MDNS.end();
    server.begin();
    if (MDNS.begin("diagboard"))
        Serial.print("Web server up — http://diagboard.local  or  http://");
    else
        Serial.print("Web server up — http://");
    Serial.println(WiFi.localIP());
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
static const char LOG_FILE[]   = "/session_temp.csv";
static const char LOG_HEADER[] = "timestamp,A0_C,A1_C,A2_C,A3_C,A4_C,A5_C\n";
static unsigned int logIntervalSec = LOG_INTERVAL_DEFAULT_SEC;

void initLog() {
    if (SD.exists(LOG_FILE)) {
        // Session file from before reboot — resume it rather than wiping
        File f = SD.open(LOG_FILE, FILE_READ);
        size_t sz = f ? f.size() : 0;
        if (f) f.close();
        // Estimate existing row count (header + ~58 bytes/row)
        const size_t headerLen = strlen(LOG_HEADER);
        logCount = sz > headerLen ? (sz - headerLen) / 58 : 0;
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
        sessionState = SESSION_STOPPED;  // auto-stop if cap reached
        return;
    }
    File f = SD.open(LOG_FILE, FILE_APPEND);
    if (!f) return;

    char ts[24] = "unknown";
    struct tm t;
    if (getLocalTime(&t, 0)) {
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &t);
    }

    char row[100];
    snprintf(row, sizeof(row), "%s,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
             ts,
             temps[0], temps[1], temps[2], temps[3], temps[4], temps[5]);
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
    if (!SD.begin(SD_CS_PIN, SPI, 1000000U)) {
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

    // Start a new session — wipes any previous temp file and begins logging
    server.on("/log/start", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!sdAvailable) { req->send(503, "text/plain", "No SD card"); return; }
        if (SD.exists(LOG_FILE)) SD.remove(LOG_FILE);
        File f = SD.open(LOG_FILE, FILE_WRITE);
        if (f) { f.print(LOG_HEADER); f.close(); }
        logCount = 0;
        sessionState = SESSION_RECORDING;
        req->send(200, "text/plain", "OK");
    });

    // Stop logging — data stays on SD, waiting for download
    server.on("/log/stop", HTTP_GET, [](AsyncWebServerRequest *req) {
        sessionState = SESSION_STOPPED;
        req->send(200, "text/plain", "OK");
    });

    // Set log interval — ?s=N where N is seconds between SD writes
    server.on("/log/interval", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (req->hasParam("s")) {
            int v = req->getParam("s")->value().toInt();
            if (v >= 1 && v <= 3600) logIntervalSec = (unsigned int)v;
        }
        req->send(200, "text/plain", String(logIntervalSec).c_str());
    });

    // Download — prompt came from the browser; rename temp file, serve it, go idle
    server.on("/download", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!sdAvailable) { req->send(503, "text/plain", "No SD card"); return; }
        if (sessionState != SESSION_STOPPED) {
            req->send(400, "text/plain", "No completed session to download");
            return;
        }
        if (!SD.exists(LOG_FILE)) {
            req->send(404, "text/plain", "Session file missing");
            return;
        }

        String raw  = req->hasParam("name") ? req->getParam("name")->value() : "session";
        String safe = sanitizeName(raw);
        String path = "/" + safe + ".csv";

        // If a file with this name already exists on the SD, remove it first
        if (SD.exists(path)) SD.remove(path);
        SD.rename(LOG_FILE, path.c_str());

        sessionState = SESSION_IDLE;
        logCount = 0;

        String disp = "attachment; filename=\"" + safe + ".csv\"";
        AsyncWebServerResponse *resp = req->beginResponse(SD, path.c_str(), "text/csv");
        resp->addHeader("Content-Disposition", disp.c_str());
        req->send(resp);
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
