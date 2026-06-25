# AVOL Diagnostic Board

A 6-channel NTC thermistor temperature monitor and data logger built on the Arduino Nano ESP32. The board connects to your WiFi network and hosts a live web dashboard. Sessions are recorded directly to an SD card and downloaded as named CSV files from the browser — no software installation required on the user's end.

---

## Hardware

| Component | Part |
|-----------|------|
| Microcontroller | Arduino Nano ESP32 (ESP32-S3, 16 MB flash) |
| Sensors | 6× 10 kΩ NTC thermistors |
| Storage | MicroSD card (FAT32, any size) |
| Network | 2.4 GHz WiFi (station mode — joins existing network) |

---

## Wiring

### SD Card (SPI)

| SD Card Pin | ESP32 Pin |
|-------------|-----------|
| CMD / MOSI  | D10       |
| DAT0 / MISO | D8        |
| CLK / SCK   | D9        |
| CD / CS     | D11       |
| VCC         | 3.3 V     |
| GND         | GND       |

### Thermistors (voltage divider per channel)

Each channel uses a series resistor between the analog pin and GND, with the thermistor between 3.3 V and the analog pin:

```
3.3V ──── [Thermistor] ──── Ax ──── [Series R] ──── GND
```

The ADC reads the voltage at `Ax`. As temperature rises the thermistor resistance drops, pulling the voltage lower.

Channels: A0, A1, A2, A3, A4, A5

---

## Configuration

All user-adjustable settings live in `include/config.h`. Edit this file before flashing.

```cpp
WIFI_SSID            // Your network name
WIFI_PASSWORD        // Your network password

NTP_UTC_OFFSET_SEC   // Timezone offset in seconds
                     // EST = -18000 | CST = -21600 | MST = -25200 | PST = -28800

MAX_LOG_ENTRIES      // Hard cap on rows per session (safety limit)
                     // Default: 2,000,000 rows ≈ 55 hours at 100 ms intervals

SEND_INTERVAL_MS     // Milliseconds between sensor reads and browser updates
                     // Default: 100 ms (10 readings/second)

ADC_SAMPLES          // ADC readings averaged per measurement (noise reduction)
                     // Default: 16
```

---

## Sensor Calibration

Two per-sensor arrays are defined at the top of `src/main.cpp`:

### `ACTUAL_SERIES_RESISTORS[6]`
The exact measured resistance (in ohms) of the voltage-divider resistor on each channel. Measure each resistor with a multimeter before soldering for best accuracy. Even small deviations matter — the default values reflect measured hardware:

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
A fixed °C offset applied after the Steinhart-Hart calculation. Use this to trim sensor-to-sensor variation (e.g. measured against a reference thermometer in a controlled bath). Positive values raise the reported temperature; negative values lower it.

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
The firmware uses the Steinhart-Hart beta equation:

```
1/T = 1/T₀ + (1/B) × ln(R_thermistor / R₀)
```

Where `B = 3950`, `R₀ = 10 kΩ`, `T₀ = 298.15 K (25°C)`. Constants are defined in `config.h`.

---

## Build & Flash

This project uses [PlatformIO](https://platformio.org/).

1. Install the PlatformIO extension for VS Code (or the CLI)
2. Open this project folder
3. Edit `include/config.h` with your WiFi credentials and timezone
4. Connect the Arduino Nano ESP32 via USB
5. Click **Upload** in VS Code, or run:
   ```
   pio run --target upload
   ```
6. Open the Serial Monitor at **115200 baud** to confirm boot and get the IP address

---

## Using the Dashboard

### Connecting
After boot (~5 seconds), open a browser on the same WiFi network and navigate to:

```
http://diagboard.local
```

If mDNS doesn't work on your network, use the IP address printed to the Serial Monitor instead (e.g. `http://192.168.1.42`).

### Live View
The dashboard displays real-time temperatures across all 6 channels, updating at 10 Hz via a persistent Server-Sent Events connection. A scrolling chart shows the last 300 readings (~30 seconds at default rate). No page refresh is needed.

| Display | Meaning |
|---------|---------|
| `24.57°C` | Normal reading |
| `OPEN` | Thermistor disconnected or wire broken |
| `SHORT` | Thermistor shorted |

### Recording a Session

The workflow is three buttons:

**1. Start Log**
Clears any previous undownloaded session and begins writing timestamped rows to the SD card. The button turns orange and the header badge shows `● Recording` with a pulsing dot and a live row count.

**2. Stop Log**
Stops writing. The data is safely stored on the SD card. The board holds it there until you either download it or start a new session.

**3. Download Log**
Prompts you to name the session (e.g. `Ice Bath Test June 25`). The board saves the file to the SD card under that name and simultaneously downloads it to your computer as a `.csv` file. Spaces become underscores; special characters are removed.

> **Important:** Pressing **Start Log** again permanently wipes the previous session. Always download before starting a new recording.

---

## CSV Format

Downloaded files are standard comma-separated values, openable directly in Excel or any data tool.

```
timestamp,A0_C,A1_C,A2_C,A3_C,A4_C,A5_C
2026-06-25 14:32:01,24.57,24.61,24.53,24.44,24.59,24.62
2026-06-25 14:32:01,24.58,24.61,24.54,24.45,24.59,24.63
...
```

- **timestamp** — wall-clock time from NTP (synced at boot)
- **A0_C through A5_C** — temperature in °C for each channel

Files are also retained on the SD card under the name you chose, so you always have a backup copy.

---

## How the Code Works

### Firmware (`src/main.cpp`)

**Boot sequence (`setup`)**
1. Waits 2 seconds for SD card power to stabilize
2. Initializes SPI and mounts the SD card; sets `sdAvailable` flag
3. Deletes any leftover `session_temp.csv` from a previous boot
4. Connects to WiFi; syncs time via NTP (`pool.ntp.org`)
5. Registers mDNS hostname (`diagboard.local`)
6. Starts the async web server with the routes below

**Main loop (`loop`)**
Every `SEND_INTERVAL_MS` milliseconds:
1. Reads all 6 thermistors (averaging `ADC_SAMPLES` readings each)
2. If a session is recording, appends one row to `session_temp.csv` on the SD
3. Pushes a comma-delimited SSE event to all connected browsers with the 6 temperatures, row count, session state, and SD status

**Session state machine**

| State | Value | Meaning |
|-------|-------|---------|
| `SESSION_IDLE` | 0 | No session in progress; Start button active |
| `SESSION_RECORDING` | 1 | Writing to SD; Stop button active |
| `SESSION_STOPPED` | 2 | Data held on SD; Download button active |

**ADC → Temperature**
Each channel: 16 ADC readings are averaged → voltage divider math gives thermistor resistance → Steinhart-Hart beta equation gives temperature in Kelvin → convert to °C → apply per-channel offset.

### Web Routes

| Route | Description |
|-------|-------------|
| `GET /` | Serves the dashboard HTML (stored in `PROGMEM`) |
| `GET /events` | SSE stream — browser subscribes once, receives data at 10 Hz |
| `GET /log/start` | Clears temp file, creates fresh header, sets state to RECORDING |
| `GET /log/stop` | Sets state to STOPPED |
| `GET /download?name=X` | Renames `session_temp.csv` → `X.csv`, serves it as a download, resets to IDLE |

### Web Dashboard (`PAGE[]` in `main.cpp`)

The entire dashboard is a self-contained HTML page embedded in firmware flash (`PROGMEM`). It requires no external files on the device.

- **Font**: Space Grotesk (loaded from Google Fonts — requires internet on the viewing device)
- **Live data**: A single `EventSource` connection receives SSE events and updates the DOM without polling
- **Chart**: Drawn on an HTML5 `<canvas>` element; last 300 data points, auto-scaling Y axis
- **Logo**: Base64-encoded JPEG embedded inline in the `<img>` tag

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| "WiFi failed" in Serial Monitor | Wrong SSID or password | Edit `config.h`, reflash |
| "SD mount failed" in Serial Monitor | Wiring error, no card, or wrong format | Check SPI wiring; format card as FAT32 |
| `diagboard.local` doesn't resolve | mDNS not supported on this network | Use the IP address from Serial Monitor |
| Start Log button is greyed out | SD not detected | Insert card and restart the board |
| Temperatures read `OPEN` | Thermistor disconnected | Check wiring at Ax pin and 3.3V |
| Temperatures read `SHORT` | Thermistor or wiring shorted | Inspect for solder bridges |
| Temperatures seem offset | Series resistor value incorrect, or sensor variation | Measure resistors with multimeter; adjust `TEMP_OFFSETS_C` |
| Chart shows flat line at boot | Normal — chart fills as data arrives | Wait a few seconds |

---

## Project Structure

```
Diagnostic-Board/
├── src/
│   ├── main.cpp          # All firmware logic and embedded HTML
│   └── main.c            # Empty — framework placeholder
├── include/
│   └── config.h          # User configuration (WiFi, timezone, sampling rate)
├── Assets/
│   └── avol_favicon.jpg  # Logo source (embedded as base64 in main.cpp)
├── partitions.csv        # ESP32 flash partition table (16 MB)
├── platformio.ini        # PlatformIO build configuration
└── README.md             # This file
```
