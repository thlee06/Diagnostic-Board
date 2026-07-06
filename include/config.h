#pragma once

// ── WiFi ──────────────────────────────────────────────────────────────────────
#define WIFI_SSID                  "NewlabMember 2.4GHz Only"
#define WIFI_PASSWORD              "!Welcome2NewLab!"
#define WIFI_CONNECT_TIMEOUT_MS    10000   // ms per connection attempt
#define WIFI_WATCHDOG_INTERVAL_MS   5000   // ms between drop checks

// ── Web Server ────────────────────────────────────────────────────────────────
#define HTTP_PORT        80

// ── Time (NTP) ────────────────────────────────────────────────────────────────
#define NTP_UTC_OFFSET_SEC  (-18000)    // UTC-5 (EST); -21600 CST, -25200 MST, -28800 PST

// ── Data Logging ─────────────────────────────────────────────────────────────
#define MAX_LOG_ENTRIES           2000000  // hard cap on rows per session
#define LOG_INTERVAL_DEFAULT_SEC       1   // seconds between SD writes (user-adjustable via UI)

// ── Thermistor (10k NTC) ─────────────────────────────────────────────────────
#define THERMISTOR_B     3950.0f        // Beta coefficient
#define THERMISTOR_R0    10000.0f       // Resistance at T0 (ohms)
#define THERMISTOR_T0    298.15f        // Reference temperature (Kelvin = 25°C)
// Per-sensor series resistors and offsets are defined in main.cpp

// ── ADC ───────────────────────────────────────────────────────────────────────
#define ADC_SAMPLES      16             // Readings to average per measurement
#define ADC_MAX_VALUE    4095           // 12-bit ADC maximum

// ── Sampling ──────────────────────────────────────────────────────────────────
#define SEND_INTERVAL_MS 100            // live display refresh — independent of log rate
