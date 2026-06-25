#pragma once

// ── WiFi (station mode — joins your existing network) ─────────────────────────
#define WIFI_SSID        "NewlabMember 2.4GHz Only"
#define WIFI_PASSWORD    "!Welcome2NewLab!"

// ── Web Server ────────────────────────────────────────────────────────────────
#define HTTP_PORT        80

// ── Time (NTP) ────────────────────────────────────────────────────────────────
#define NTP_UTC_OFFSET_SEC  (-18000)     // UTC-5 (EST); change to -21600 for CST, -25200 for MST, -28800 for PST

// ── Data Logging ─────────────────────────────────────────────────────────────
#define MAX_LOG_ENTRIES  2000000        // Max rows per session on SD (~55 hr at 100 ms)

// ── Thermistor (10k NTC) ─────────────────────────────────────────────────────
#define THERMISTOR_B     3950.0f        // Beta coefficient
#define THERMISTOR_R0    10000.0f       // Resistance at T0 (ohms)
#define THERMISTOR_T0    298.15f        // Reference temperature (Kelvin = 25°C)
// Per-sensor series resistors and offsets are defined in main.cpp

// ── ADC ───────────────────────────────────────────────────────────────────────
#define ADC_SAMPLES      16             // Readings to average per measurement
#define ADC_MAX_VALUE    4095           // 12-bit ADC maximum

// ── Sampling ──────────────────────────────────────────────────────────────────
#define SEND_INTERVAL_MS 100            // ms between temperature readings
