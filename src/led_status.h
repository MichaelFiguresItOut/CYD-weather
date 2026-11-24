#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
//  Onboard RGB LED as the device status indicator (README.md "LED status"):
//
//    GREEN – Ready:      idle, Wi-Fi connected, NTP synced, an NWS station
//                        is set for the location.
//    BLUE  – Busy:       an activity is in progress (boot/initial load,
//                        Wi-Fi scan/connect/reconnect, weather fetch +
//                        processing, location geocoding + NWS update,
//                        station scan, profile save/restore/clear, SD card
//                        init/read/decode, screen-tap refresh).
//    RED   – Attention:  an activity finished and a condition persists —
//                        Wi-Fi disconnected (or radio off), NTP not synced,
//                        or no NWS station for the location — or an explicit
//                        error warning is on screen (ledAdvisoryOn), e.g.
//                        the "NO FREE SLOTS" line on SAVE LOCATIONS.
//
//  Exactly one color is on at a time; priority is always
//  BLUE > RED > GREEN (a running activity is never reported as green or
//  red). The LED never blinks, pulses or animates.
//
//  ESP32-2432S032C-I ("CYD") RGB LED (board schematic): all three channels
//  are driven active-LOW through current-limiting resistors — driving a
//  pin LOW lights that color. Pin map: R = GPIO4, G = GPIO16, B = GPIO17.
//  None of the three is used by the TFT (15/12/2/14/13, backlight 27),
//  the GT911 touch (33/32/25) or the SD slot (5/18/19/23).
//
//  Each channel runs hardware PWM (ESP32 LEDC, 5 kHz, 8-bit) at a fixed
//  brightness of ~1.2% LED-on time: the duty cycle is inverted for the
//  active-low wiring (lit = pin LOW for 3/256 of the period, off = pin
//  fully HIGH).
// ---------------------------------------------------------------------------

// State currently presented on the LED (for logging/diagnostics).
enum class LedState : uint8_t { OFF = 0, GREEN, BLUE, RED };

// Safe initialization: latches every channel OFF (HIGH) before the pins
// start driving, so no color flashes at power-up. Call once at the very
// start of setup(), before any other code touches these pins.
void ledInit();

// Mark an activity as starting (ledActivityBegin) or finished
// (ledActivityEnd). Calls may nest and overlap: the LED only leaves BLUE
// when every begin has its matching end (the counter reaches zero), so a
// finishing sub-step can never turn the LED green while a larger operation
// is still running. Each call recomputes the LED immediately.
void ledActivityBegin();
void ledActivityEnd();

// While an explicit user-visible error warning is displayed (e.g. the
// "NO FREE SLOTS" line on the SAVE LOCATIONS page, "LOOKUP FAILED" on the
// location-lookup page), keep the LED red unless an activity is running.
// Clear it when the message is dismissed or replaced — ledRecompute() then
// re-evaluates the persistent conditions.
void ledAdvisoryOn();
void ledAdvisoryOff();

// Recompute the LED from the current conditions:
//   BLUE  – at least one activity in progress;
//   RED   – no activity and an attention condition is true: advisory on,
//           Wi-Fi not connected (radio off included), NTP not synced, or
//           no NWS station set for the location;
//   GREEN – no activity and no attention condition.
// Cheap and idempotent — call it from the main loop and it only writes the
// pins when the state actually changes.
void ledRecompute();

// State currently presented on the LED.
LedState ledState();
