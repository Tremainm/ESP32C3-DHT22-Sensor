# capture_dht.py
import serial
import re

PORT = "/dev/ttyUSB0"   # change to match your COM port
BAUD = 115200
OUTPUT = "dht_readings.txt"

print(f"Opening {PORT} at {BAUD} baud. Ctrl+C to stop.")

with serial.Serial(PORT, BAUD, timeout=1) as ser, open(OUTPUT, "w") as out:
    count = 0
    while True:
        try:
            line = ser.readline().decode("utf-8", errors="ignore").strip()
        except KeyboardInterrupt:
            break

        if "DHT22:" in line:
            match = re.search(r"T=([\d.]+)C H=([\d.]+)%", line)
            if match:
                row = f"{match.group(1)},{match.group(2)}\n"
                out.write(row)
                out.flush()   # write immediately, don't buffer
                count += 1
                print(f"[{count}] T={match.group(1)}C  H={match.group(2)}%")

print(f"\nDone. {count} readings saved to {OUTPUT}.")