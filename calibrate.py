"""
Thermistor Calibration Tool
Usage: python calibrate.py <ESP32_IP> [port]
       python calibrate.py              (auto-discover)

Collects temperature samples from all 6 sensors, then either:
  - Computes offsets relative to one another (pick a reference sensor), or
  - Computes offsets relative to a known external reference temperature.

Paste the suggested offsets into TEMP_OFFSETS_C[] in main.cpp.
"""

import socket
import sys
import time

# ── Config ────────────────────────────────────────────────────────────────────

DISCOVERY_PORT   = 8889
DISCOVERY_MSG    = b"DIAGNOSTIC_BOARD_HERE"
NUM_SENSORS      = 6
SENSOR_LABELS    = ["A0", "A1", "A2", "A3", "A4", "A5"]
SAMPLE_DURATION  = 30   # seconds to collect data (increase for noisy environments)

# ── Discovery ─────────────────────────────────────────────────────────────────

def discover_esp32(timeout=15):
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
                    print(f"Found ESP32 at {addr[0]}\n")
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

# ── Data collection ───────────────────────────────────────────────────────────

print(f"Connecting to {HOST}:{PORT} ...")

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
    s.settimeout(5.0)
    s.connect((HOST, PORT))
    print(f"Connected. Collecting samples for {SAMPLE_DURATION} seconds...\n")
    s.settimeout(2.0)

    totals  = [0.0] * NUM_SENSORS
    counts  = [0]   * NUM_SENSORS  # shared count (all update together per line)
    n       = 0
    buf     = ""
    deadline = time.time() + SAMPLE_DURATION

    while time.time() < deadline:
        remaining = deadline - time.time()
        try:
            chunk = s.recv(256).decode("utf-8")
        except socket.timeout:
            continue
        if not chunk:
            print("Connection closed by ESP32.")
            break
        buf += chunk

        while "\n" in buf:
            line, buf = buf.split("\n", 1)
            parts = line.strip().split(",")
            if len(parts) == NUM_SENSORS + 1:
                try:
                    vals = [float(parts[i]) for i in range(NUM_SENSORS)]
                except ValueError:
                    continue

                # Skip fault readings
                if any(abs(v) > 900 for v in vals):
                    continue

                for i in range(NUM_SENSORS):
                    totals[i] += vals[i]
                n += 1

                # Live progress line
                avg_str = "  ".join(f"{SENSOR_LABELS[i]}={totals[i]/n:6.2f}°C" for i in range(NUM_SENSORS))
                print(f"\r[{n:4d} samples | {remaining:.0f}s left]  {avg_str}", end="", flush=True)

print()  # newline after progress

if n == 0:
    print("No valid samples received. Check wiring and thermistor connections.")
    sys.exit(1)

# ── Results ───────────────────────────────────────────────────────────────────

averages = [totals[i] / n for i in range(NUM_SENSORS)]

print(f"\n{'─'*55}")
print(f"  Results after {n} samples")
print(f"{'─'*55}")
for i in range(NUM_SENSORS):
    print(f"  {SENSOR_LABELS[i]}: avg = {averages[i]:.3f} °C")
print(f"{'─'*55}\n")

# ── Offset calculation ────────────────────────────────────────────────────────

print("How would you like to calculate offsets?\n")
print("  1) Relative to a reference sensor (e.g. your most trusted one)")
print("  2) Relative to a known external temperature (e.g. thermometer reading)")
print("  3) Just show the averages — I'll calculate offsets myself")
choice = input("\nEnter 1, 2, or 3: ").strip()

suggested_offsets = None

if choice == "1":
    print(f"\nAvailable sensors: {', '.join(SENSOR_LABELS)}")
    ref = input("Enter reference sensor label (e.g. A2): ").strip().upper()
    if ref not in SENSOR_LABELS:
        print(f"Unknown sensor '{ref}'. Exiting.")
        sys.exit(1)
    ref_idx = SENSOR_LABELS.index(ref)
    ref_avg = averages[ref_idx]
    print(f"\nReference {ref} average: {ref_avg:.3f} °C")
    suggested_offsets = [ref_avg - averages[i] for i in range(NUM_SENSORS)]

elif choice == "2":
    try:
        known = float(input("Enter known reference temperature (°C): ").strip())
    except ValueError:
        print("Invalid temperature. Exiting.")
        sys.exit(1)
    suggested_offsets = [known - averages[i] for i in range(NUM_SENSORS)]

elif choice == "3":
    print("\nAverages (no offsets calculated):")
    for i in range(NUM_SENSORS):
        print(f"  {SENSOR_LABELS[i]}: {averages[i]:.3f} °C")
    sys.exit(0)

else:
    print("Invalid choice. Exiting.")
    sys.exit(1)

# ── Print suggested offsets ───────────────────────────────────────────────────

print(f"\n{'─'*55}")
print("  Suggested TEMP_OFFSETS_C[] for main.cpp")
print(f"{'─'*55}")
print("\nconst float TEMP_OFFSETS_C[NUM_SENSORS] = {")
for i, (label, avg, offset) in enumerate(zip(SENSOR_LABELS, averages, suggested_offsets)):
    comma = "," if i < NUM_SENSORS - 1 else " "
    print(f"  {offset:+.2f}{comma}  // {label}: avg was {avg:.3f} °C")
print("};")
print(f"\n{'─'*55}")
print("Paste the block above into main.cpp, then rebuild and flash.\n")
