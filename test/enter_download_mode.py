#!/usr/bin/env python3
"""Put the CYD into USB download mode via the DTR/RTS auto-reset circuit."""
import serial, time

s = serial.Serial("COM4", 115200)
time.sleep(0.1)
s.dtr = False   # pull GPIO0 low (download-mode select)
s.rts = True    # pull EN low -> chip in reset
time.sleep(0.2)
s.rts = False   # release reset; chip boots into ROM download mode
time.sleep(0.15)
s.close()
print("board should now be in download mode")
