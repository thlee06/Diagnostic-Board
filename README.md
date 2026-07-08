# AVOL Diagnostic Board

A 6-channel NTC thermistor temperature monitor built on the Arduino Nano ESP32. The board connects to your WiFi network and pushes live sensor readings at 5 Hz to a central dashboard server over WebSocket. All display and data retention is handled by the server.

---

## Hardware

| Component | Part |
|-----------|------|
| Microcontroller | Arduino Nano ESP32 (ESP32-S3, 16 MB flash) |
| Sensors | 6× 10 kΩ NTC thermistors |
| Network | 2.4 GHz WiFi (station mode — joins existing network) |

---

## Wiring

### Thermistors (voltage divider per channel)

Each channel uses a series resistor (~930 Ω) between the analog pin and GND, with the thermistor between 3.3 V and the analog pin:

```
3.3V ──── [Thermistor] ──── Ax ──── [Series R ~930Ω] ──── GND
```

Channels: **A0, A1, A2, A3, A4, A5**

---

## Configuration

All user settings are in `include/config.h`. Edit before flashing:

```cpp
// WiFi
WIFI_SSID              // Network name
WIFI_PASSWORD          // Network password

// Central dashboard server (get IP from `npm start` output on the laptop)
CENTRAL_SERVER_IP      // e.g. "10.105.44.153"
CENTRAL_SERVER_PORT    // 8080
MODULE_ID              // Stable slug for this board, e.g. "diag_board_1"
MODULE_NAME            // Human-readable name shown in the dashboard

// Sampling
SEND_INTERVAL_MS       // Push interval in ms — default 200 (5 Hz)
ADC_SAMPLES            // ADC reads averaged per measurement — default 16
```

---

## Sensor Calibration

Two per-sensor arrays are defined at the top of `src/main.cpp`:

### `ACTUAL_SERIES_RESISTORS[6]`
The exact measured resistance (ohms) of the voltage-divider resistor on each channel. Measure each with a multimeter before soldering.

```cpp
const float ACTUAL_SERIES_RESISTORS[NUM_SENSORS] = {
    927.0,  // A0
    928.0,  // A1
    929.0,  // A2
    933.0,  // A3
    928.0,  // A4
    929.0   // A5
};
```

### `TEMP_OFFSETS_C[6]`
Fixed °C offset applied after the Steinhart-Hart calculation. Trim against a reference thermometer to remove sensor-to-sensor variation.

```cpp
const float TEMP_OFFSETS_C[NUM_SENSORS] = {
     -0.700,  // A0
      0.170,  // A1
      0.047,  // A2
      0.217,  // A3
     -0.110,  // A4
      0.351   // A5
};
```

### Temperature Calculation

```
1/T = 1/T₀ + (1/B) × ln(R_thermistor / R₀)
```

`B = 3950`, `R₀ = 10 kΩ`, `T₀ = 298.15 K (25°C)` — defined in `config.h`.

---

## Build & Flash

This project uses [PlatformIO](https://platformio.org/).

1. Install the PlatformIO extension for VS Code (or the CLI)
2. Open this project folder
3. Edit `include/config.h` — WiFi credentials and `CENTRAL_SERVER_IP`
4. Connect the Arduino Nano ESP32 via USB
5. Click **Upload** in VS Code, or run:
   ```
   pio run --target upload
   ```
6. Open the Serial Monitor at **115200 baud** to confirm boot and WebSocket connection

Expected Serial output on successful boot:
```
Connecting to "YourNetwork"...
WiFi connected — IP: 10.105.x.x
WS task: connecting to ws://10.105.44.153:8080/ingest
WS: connected to 10.105.44.153:8080
WS: sent hello as module "diag_board_1"
```

---

## How It Works

### Architecture

The board is a pure sensor pusher — no local web server, no SD card. All display and storage is handled by the central dashboard server (`unified-web-server`).

```
[Thermistors] → [ESP32 ADC] → [WS push @ 5 Hz] → [Central Server] → [Browser Dashboard]
```

### Firmware (`src/main.cpp`)

**Boot (`setup`)**
1. Configures ADC pins and resolution
2. Starts WiFi (auto-reconnect enabled)
3. Creates a FreeRTOS queue and spawns the WS task on Core 0

**Main loop (`loop`) — Core 1**
Every 200 ms: reads all 6 thermistors and posts the readings to the queue. Never touches the socket — no blocking possible.

**WS task (`wsTaskFunc`) — Core 0**
- Waits for WiFi, then opens a WebSocket connection to `ws://CENTRAL_SERVER_IP:8080/ingest`
- On connect: sends a `hello` message with module ID, name, and channel metadata
- Drains the queue at ~100 Hz: for each reading, sends a `sample` message
- Reconnects automatically (5 s backoff) on drop; handles WiFi loss transparently

The WS task is pinned to Core 0 (the WiFi/TCP core) so any blocking socket I/O never stalls sensor sampling on Core 1.

### Wire Protocol

See the `unified-web-server` README for full protocol details. In brief:

**hello** (sent once on connect):
```json
{
  "type": "hello",
  "module": "diag_board_1",
  "name": "Diagnostic Board 1",
  "channels": {
    "a0_c": { "label": "Temp A0", "unit": "°C" },
    ...
  }
}
```

**sample** (sent at 5 Hz):
```json
{
  "type": "sample",
  "module": "diag_board_1",
  "values": { "a0_c": 24.57, "a1_c": 24.61, ... }
}
```

Sensors reading `OPEN` (>900 °C) or `SHORT` (<−900 °C) are omitted from `values`.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| "Connecting to…" never resolves | Wrong SSID/password | Edit `config.h`, reflash |
| "WS: disconnected — will retry" loops | Server not running or wrong IP | Run `npm start` on laptop; confirm `CENTRAL_SERVER_IP` |
| Module panel missing from dashboard | Wrong `MODULE_ID` or server not receiving | Check Serial for "sent hello"; verify server console |
| Temperatures offset | Wrong series resistor value or sensor variation | Measure resistors; adjust `TEMP_OFFSETS_C` |
| No data after power-cycle | Normal — board reconnects and resumes within ~5 s | Wait for reconnect |

---

## Project Structure

```
Diagnostic-Board/
├── src/
│   └── main.cpp          # All firmware logic
├── include/
│   └── config.h          # User configuration (WiFi, server IP, module ID)
├── Assets/
│   └── avol_favicon.jpg  # Logo
├── partitions.csv        # ESP32 flash partition table (16 MB)
├── platformio.ini        # PlatformIO build configuration
└── README.md             # This file
```
