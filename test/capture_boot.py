#!/usr/bin/env python3
"""Reset the CYD over COM4 (DTR/RTS auto-reset circuit) and capture boot log."""
import serial, sys, time

PORT = "COM4"
BAUD = 115200
CAPTURE_S = 9.0

s = serial.Serial(PORT, BAUD, timeout=1)
time.sleep(0.2)

# Hard reset via the CH340 auto-reset circuit (polarity verified on CYD):
s.dtr = False   # keep GPIO0 released -> normal boot (not download mode)
s.rts = True    # pull EN low -> chip in reset
time.sleep(0.3)
t0 = time.time()
print(f"--- reset released at t=0, capturing {CAPTURE_S}s ---")
s.rts = False   # release reset -> chip boots

buf = b""
while time.time() - t0 < CAPTURE_S:
    chunk = s.read(4096)
    if chunk:
        buf += chunk
s.close()

text = buf.decode("utf-8", errors="replace")
print(text)
# Highlight the lines that matter
for line in text.splitlines():
    if "[SD]" in line or "[DOG]" in line or "SD:" in line or "card" in line.lower():
        print(">>", line.strip())
