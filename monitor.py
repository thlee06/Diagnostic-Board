"""
Thermistor Temperature Monitor — 6-channel
Usage: python monitor.py <ESP32_IP> [port]

Connects to the ESP32 TCP server and displays a live temperature graph.
Requires: pip install matplotlib
"""

import socket
import sys
import time
import threading
from collections import deque
import matplotlib.pyplot as plt
import matplotlib.animation as animation

# ── Config ────────────────────────────────────────────────────────────────────

DISCOVERY_PORT = 8889
DISCOVERY_MSG  = b"DIAGNOSTIC_BOARD_HERE"
NUM_SENSORS    = 6
SENSOR_LABELS  = ["A0", "A1", "A2", "A3", "A4", "A5"]
SENSOR_COLORS  = ["red", "blue", "green", "orange", "purple", "brown"]

def discover_esp32(timeout=15):
    """Listen for the ESP32's UDP broadcast and return its IP address."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", DISCOVERY_PORT))
    sock.settimeout(1.0)
    print(f"Searching for ESP32 on the network (up to {timeout}s)...")
    deadline = time.time() + timeout
    try:
        while time.time() < deadline:
            try:
                data, addr = sock.recvfrom(1024)
                if data == DISCOVERY_MSG:
                    print(f"Found ESP32 at {addr[0]}")
                    return addr[0]
            except socket.timeout:
                print(".", end="", flush=True)
        print("\nESP32 not found. Is it on the same network?")
        sys.exit(1)
    finally:
        sock.close()

if len(sys.argv) >= 2:
    HOST = sys.argv[1]
    PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8888
else:
    HOST = discover_esp32()
    PORT = 8888

MAX_POINTS = 600    # 60 seconds of history at 100 ms intervals
RETRY_DELAY = 3.0   # seconds between reconnection attempts

# ── Shared state ──────────────────────────────────────────────────────────────

# One deque per sensor for temperatures, plus a shared time deque
temps = [deque(maxlen=MAX_POINTS) for _ in range(NUM_SENSORS)]
times = deque(maxlen=MAX_POINTS)
lock  = threading.Lock()

# ── Reader thread ─────────────────────────────────────────────────────────────

def reader_thread():
    """Connects to the ESP32, reads CSV lines (t0,t1,t2,t3,t4,t5,ts_ms),
    appends to shared deques. Automatically reconnects on any error."""
    while True:
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.settimeout(5.0)
                print(f"Connecting to {HOST}:{PORT} ...")
                s.connect((HOST, PORT))
                print("Connected.")
                s.settimeout(2.0)   # detect lost connection within 2 s

                buf = ""
                while True:
                    chunk = s.recv(256).decode("utf-8")
                    if not chunk:
                        raise ConnectionError("Server closed the connection")
                    buf += chunk
                    while "\n" in buf:
                        line, buf = buf.split("\n", 1)
                        parts = line.strip().split(",")
                        if len(parts) == NUM_SENSORS + 1:
                            try:
                                sensor_temps = [float(parts[i]) for i in range(NUM_SENSORS)]
                                ts_ms = int(parts[NUM_SENSORS])
                                with lock:
                                    for i in range(NUM_SENSORS):
                                        temps[i].append(sensor_temps[i])
                                    times.append(ts_ms / 1000.0)
                            except ValueError:
                                pass  # ignore malformed lines
        except Exception as exc:
            print(f"Connection error: {exc}  — retrying in {RETRY_DELAY:.0f}s ...")
            time.sleep(RETRY_DELAY)

# ── Matplotlib animation ──────────────────────────────────────────────────────

fig, ax = plt.subplots(figsize=(12, 5))
ax.set_title("NTC Thermistor Temperatures (6-channel)")
ax.set_xlabel("Time (s)")
ax.set_ylabel("Temperature (°C)")
ax.grid(True, linestyle="--", alpha=0.5)

line_plots = []
for i in range(NUM_SENSORS):
    lp, = ax.plot([], [], color=SENSOR_COLORS[i], linewidth=1.5, label=f"Temp {SENSOR_LABELS[i]}")
    line_plots.append(lp)

temp_text = ax.text(0.02, 0.97, "Waiting for data...",
                    transform=ax.transAxes, va="top", fontsize=10,
                    bbox=dict(boxstyle="round", facecolor="wheat", alpha=0.5))
ax.legend(loc="upper right")

def update(frame):
    with lock:
        if len(times) < 2:
            return
        t = list(times)
        ys = [list(temps[i]) for i in range(NUM_SENSORS)]

    # All sensor deques should be same length as times, but guard anyway
    n = min(len(t), *(len(y) for y in ys))
    t  = t[-n:]
    ys = [y[-n:] for y in ys]

    t0    = t[0]
    t_rel = [x - t0 for x in t]

    for i, lp in enumerate(line_plots):
        lp.set_data(t_rel, ys[i])

    ax.relim()
    ax.autoscale_view()

    lines = []
    for i in range(NUM_SENSORS):
        val = ys[i][-1]
        if val > 900:
            label = f"{SENSOR_LABELS[i]}: open circuit"
        elif val < -900:
            label = f"{SENSOR_LABELS[i]}: short circuit"
        else:
            label = f"{SENSOR_LABELS[i]}: {val:.1f} °C"
        lines.append(label)
    temp_text.set_text("\n".join(lines))

ani = animation.FuncAnimation(fig, update, interval=100, blit=False, cache_frame_data=False)

# Start the reader as a daemon so it exits when the window closes.
t = threading.Thread(target=reader_thread, daemon=True)
t.start()

plt.tight_layout()
plt.show()
