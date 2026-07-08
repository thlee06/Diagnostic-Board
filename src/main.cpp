#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include "config.h"

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

// ── Central dashboard WebSocket client ───────────────────────────────────────
static bool wifiReady          = false;
static bool wsConnected        = false;
static volatile bool wsReady   = false;  // set when WiFi is up; unblocks wsTaskFunc

static const char* const WS_CHAN_KEYS[NUM_SENSORS]   = {"a0_c","a1_c","a2_c","a3_c","a4_c","a5_c"};
static const char* const WS_CHAN_LABELS[NUM_SENSORS] = {"Temp A0","Temp A1","Temp A2","Temp A3","Temp A4","Temp A5"};

static WebSocketsClient wsClient;

struct WsPayload { float temps[NUM_SENSORS]; };
static QueueHandle_t wsQueue = NULL;

static void sendWsHello() {
    JsonDocument doc;
    doc["type"]   = "hello";
    doc["module"] = MODULE_ID;
    doc["name"]   = MODULE_NAME;
    JsonObject channels = doc["channels"].to<JsonObject>();
    for (int i = 0; i < NUM_SENSORS; i++) {
        JsonObject ch = channels[WS_CHAN_KEYS[i]].to<JsonObject>();
        ch["label"] = WS_CHAN_LABELS[i];
        ch["unit"]  = "\xC2\xB0" "C";   // °C in UTF-8
    }
    String msg;
    serializeJson(doc, msg);
    wsClient.sendTXT(msg);
    Serial.printf("WS: sent hello as module \"%s\"\n", MODULE_ID);
}

static void onWsEvent(WStype_t type, uint8_t* /*payload*/, size_t /*length*/) {
    switch (type) {
        case WStype_CONNECTED:
            wsConnected = true;
            Serial.printf("WS: connected to %s:%d\n", CENTRAL_SERVER_IP, CENTRAL_SERVER_PORT);
            sendWsHello();
            break;
        case WStype_DISCONNECTED:
            wsConnected = false;
            Serial.println("WS: disconnected — will retry");
            break;
        default:
            break;
    }
}

// WS task: owns wsClient exclusively so its blocking socket I/O never stalls
// the main task's sensor sampling. Pinned to Core 0 (WiFi core).
static void wsTaskFunc(void*) {
    while (!wsReady) vTaskDelay(pdMS_TO_TICKS(100));

    wsClient.onEvent(onWsEvent);
    wsClient.begin(CENTRAL_SERVER_IP, CENTRAL_SERVER_PORT, "/ingest");
    wsClient.setReconnectInterval(5000);
    Serial.printf("WS task: connecting to ws://%s:%d/ingest\n",
                  CENTRAL_SERVER_IP, CENTRAL_SERVER_PORT);

    WsPayload pkt;
    for (;;) {
        wsClient.loop();   // drives reconnect, ping/pong, incoming frames

        if (xQueueReceive(wsQueue, &pkt, 0) == pdTRUE && wsConnected) {
            JsonDocument doc;
            doc["type"]   = "sample";
            doc["module"] = MODULE_ID;
            JsonObject values = doc["values"].to<JsonObject>();
            for (int i = 0; i < NUM_SENSORS; i++) {
                if (pkt.temps[i] > 900.0f || pkt.temps[i] < -900.0f) continue;
                values[WS_CHAN_KEYS[i]] = pkt.temps[i];
            }
            String msg;
            serializeJson(doc, msg);
            wsClient.sendTXT(msg);
        }

        vTaskDelay(pdMS_TO_TICKS(10));  // yield; wsClient.loop() runs at ~100 Hz
    }
}

static void onWifiConnected() {
    wifiReady = true;
    wsReady   = true;   // unblock WS task
    Serial.print("WiFi connected — IP: ");
    Serial.println(WiFi.localIP());
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

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("Connecting to \"%s\"...\n", WIFI_SSID);

    wsQueue = xQueueCreate(4, sizeof(WsPayload));
    xTaskCreatePinnedToCore(wsTaskFunc, "wsTask", 8192, NULL, 2, NULL, 0);
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    // WiFi watchdog — react to connect/disconnect, let the stack handle reconnection
    static unsigned long lastWiFiCheck = 0;
    if (millis() - lastWiFiCheck >= WIFI_WATCHDOG_INTERVAL_MS) {
        lastWiFiCheck = millis();
        bool connected = (WiFi.status() == WL_CONNECTED);
        if (connected && !wifiReady) {
            onWifiConnected();
        } else if (!connected && wifiReady) {
            wifiReady = false;
            Serial.println("WiFi lost — reconnecting...");
        }
    }

    // Sample sensors and hand to WS task
    static unsigned long lastSample = 0;
    if (millis() - lastSample >= SEND_INTERVAL_MS) {
        lastSample = millis();

        WsPayload pkt;
        for (int i = 0; i < NUM_SENSORS; i++) {
            pkt.temps[i] = readTemperatureCelsius(i);
        }

        if (wsQueue) xQueueSend(wsQueue, &pkt, 0);  // non-blocking; drop if task is behind
    }
}
