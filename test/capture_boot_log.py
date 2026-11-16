#!/usr/bin/env python3
"""Reset the CYD (normal app boot) and capture serial output for N seconds."""
import serial, sys, time

PORT = "COM4"
BAUD = 115200
SECS = int(sys.argv[1]) if len(sys.argv) > 1 else 75

s = serial.Serial(PORT, BAUD, timeout=1)
time.sleep(0.2)
# Normal reset: pulse EN (RTS) only, keep GPIO0 (DTR) HIGH -> boots the app
s.dtr = True    # GPIO0 high (app-boot state)
s.rts = True    # EN low -> chip in reset
time.sleep(0.15)
s.rts = False   # release reset -> app boot

end = time.time() + SECS
buf = []
while time.time() < end:
    data = s.read(4096)
    if data:
        buf.append(data.decode("utf-8", "replace"))
s.close()
sys.stdout.write("".join(buf))
