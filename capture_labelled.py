# capture_labelled.py
import serial
import re
import sys

PORT = "/dev/ttyUSB0"
BAUD = 115200
OUTPUT = "labelled_readings.csv"

VALID_LABELS = ["NORMAL", "WINDOW_OPEN", "HEATING_ON"]

# ── Validate command line argument ────────────────────────────────────────────
# sys.argv is the list of command line arguments.
# sys.argv[0] is always the script name itself, so sys.argv[1] is the first
# argument you pass — in this case the label.
if len(sys.argv) != 2 or sys.argv[1] not in VALID_LABELS:
    print(f"Usage: python3 capture_labelled.py <label>")
    print(f"Valid labels: {', '.join(VALID_LABELS)}")
    sys.exit(1)

label = sys.argv[1]

print(f"Label:  {label}")
print(f"Output: {OUTPUT}  (appending)")
print(f"Port:   {PORT} at {BAUD} baud")
print(f"Ctrl+C to stop.\n")

# ── Capture loop ──────────────────────────────────────────────────────────────
# Open serial port for reading and CSV file in append mode ("a").
# Append mode means running the script multiple times for different labels
# all accumulates into the same file rather than overwriting it.
with serial.Serial(PORT, BAUD, timeout=1) as ser, open(OUTPUT, "a") as out:
    count = 0
    while True:
        try:
            line = ser.readline().decode("utf-8", errors="ignore").strip()
        except KeyboardInterrupt:
            break

        if "DHT22:" not in line:
            continue

        match = re.search(r"T=([\d.]+)C H=([\d.]+)%", line)
        if not match:
            continue

        temp = match.group(1)
        hum  = match.group(2)
        row  = f"{temp},{hum},{label}\n"

        out.write(row)
        out.flush()  # write immediately so nothing is lost on Ctrl+C
        count += 1
        print(f"[{count:>4}]  T={temp}°C  H={hum}%  →  {label}")

print(f"\nDone. {count} readings appended to {OUTPUT}.")