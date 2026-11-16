#!/usr/bin/env python3
"""Enter USB download mode on the CYD (ESP32-2432S032C-I).

The board's only button is BOOT (GPIO0); the CH340 USB chip can reset the
chip (RTS -> EN) but cannot drive GPIO0. So:
  1. Hold the BOOT button (small button on the board, lower-left, next to
     the SD card slot) for ~5-8 seconds.
  2. While holding it, this script pulses the reset line (RTS) every
     ~600 ms. One reset pulse will land while GPIO0 is held low -> the chip
     boots into ROM download mode.
  3. The script confirms ROM mode with an esptool sync handshake, prints
     "ROM MODE CONFIRMED", and closes the port.
  4. The script then flashes .pio/build/esp32dev/firmware.bin with PlatformIO's
     esptool (which keeps EN released on Windows, so the chip stays in ROM).

Run this script and hold the button when it tells you to.
"""
import os
import serial
import subprocess
import sys
import time

PORT = "COM4"
PULSES = 66           # ~40 s of reset pulses
PULSE_PERIOD_S = 0.6

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIRMWARE = os.path.join(ROOT, ".pio", "build", "esp32dev", "firmware.bin")
ESPTOOL = os.path.expanduser(r"~/.platformio/packages/tool-esptool/esptool.exe")

def open_port():
    for attempt in range(5):
        try:
            p = serial.Serial(PORT, 115200, timeout=0.3)
            time.sleep(0.2)
            return p
        except serial.SerialException as e:
            print(f"  (port busy, retrying: {e})")
            time.sleep(1.0)
    raise SystemExit("could not open the port - close any other program using it")

s = open_port()
print(f"Pulsing reset on {PORT} for {PULSES * PULSE_PERIOD_S:.0f}s ...")
print(">>> NOW HOLD THE BOOT BUTTON (lower-left, next to the SD slot) <<<")
print("    hold for ~6-8 seconds, then release. Do NOT run any other scripts.")

ok = False
for i in range(PULSES):
    try:
        # pulse EN low
        s.dtr = False
        s.rts = True
        time.sleep(0.08)
        s.rts = False
        # wait for boot to settle, then try the ROM sync handshake
        time.sleep(0.35)
        s.reset_input_buffer()
        s.write(b"\x55" * 8)
        time.sleep(0.15)
        resp = s.read(16)
    except serial.SerialException:
        print("  (port hiccup - reopening)")
        try:
            s.close()
        except Exception:
            pass
        s = open_port()
        continue
    if resp[:8] == b"\x55" * 8:
        ok = True
        print(f"ROM sync detected on pulse {i + 1}/{PULSES}")
        break
    if i % 6 == 5:
        print(f"  ... still waiting ({i + 1}/{PULSES})")

s.dtr = False
s.rts = False
s.close()

if not ok:
    print("no ROM sync seen - try again, holding the button the whole time")
    sys.exit(1)

print("ROM MODE CONFIRMED - flashing now...")
if not os.path.exists(FIRMWARE):
    print(f"firmware not built: {FIRMWARE}")
    sys.exit(1)
r = subprocess.run([
    ESPTOOL, "--port", PORT, "--before", "no-reset",
    "write_flash", "0x0", FIRMWARE,
])
if r.returncode == 0:
    print("FLASH OK - the chip will boot the new firmware")
    sys.exit(0)
print("flash failed (chip may have left ROM mode) - try again")
sys.exit(1)
