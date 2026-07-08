#pragma once

// ── WiFi ──────────────────────────────────────────────────────────────────────
#define WIFI_SSID                  "NewlabMember 2.4GHz Only"
#define WIFI_PASSWORD              "!Welcome2NewLab!"
#define WIFI_WATCHDOG_INTERVAL_MS   2000   // ms between connection state checks

// ── Thermistor (10k NTC) ─────────────────────────────────────────────────────
#define THERMISTOR_B     3950.0f        // Beta coefficient
#define THERMISTOR_R0    10000.0f       // Resistance at T0 (ohms)
#define THERMISTOR_T0    298.15f        // Reference temperature (Kelvin = 25°C)
// Per-sensor series resistors and offsets are defined in main.cpp

// ── ADC ───────────────────────────────────────────────────────────────────────
#define ADC_SAMPLES      16             // Readings to average per measurement
#define ADC_MAX_VALUE    4095           // 12-bit ADC maximum

// ── Sampling ──────────────────────────────────────────────────────────────────
#define SEND_INTERVAL_MS 200            // Push rate to central server (5 Hz)

// ── Central Dashboard Push ────────────────────────────────────────────────────
// Set CENTRAL_SERVER_IP to the LAN IP printed by `npm start` on the laptop.
// MODULE_ID must be stable per physical board: ^[A-Za-z0-9_-]+$
#define CENTRAL_SERVER_IP    "10.105.44.153"   // <-- replace with laptop's LAN IP
#define CENTRAL_SERVER_PORT  8080
#define MODULE_ID            "diag_board_1"
#define MODULE_NAME          "Diagnostic Board 1"
