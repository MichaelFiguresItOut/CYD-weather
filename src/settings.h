#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
//  Settings screen (modal, full-screen)
//
//  Entry: the SETTINGS button (top-right corner) on the THEME picker's top
//  bar; the picker's < BACK corner returns to the dashboard. The main page
//  shows five tiles:
//
//    APPEARANCE   - display theme (4 choices; day/night is automatic),
//                   temp unit, clock format, date format
//    LOCATION     - add location (postal code or city), saved location
//                   profiles (save / restore / clear, up to 3), time zone
//    WI-FI        - network scan + join (QWERTY keyboard for credentials),
//                   manual SSID entry, radio on/off, restore the build-time
//                   network (erases user-saved credentials)
//    STATIONS     - local NWS observation-station discovery + fallback list
//    TEMP RANGES  - the six dog-outfit temperature-band limits (sliders,
//                   band-name tap = full-screen outfit preview)
//
//  Navigation: every screen has < BACK; deeper screens (theme list, timezone
//  pages, scan results) go back to their section page; the sections go back
//  to the main page; the main page goes back to the THEME picker.
//
//  Persistence: every change is saved to NVS ("cydwx") immediately, as
//  required by the spec — no Apply step. Keyboard/keypad modals are
//  blocking (keyboard.cpp) and the page behind them is redrawn on return.
//
//  While settingsActive(), touch.cpp routes all touch events to
//  settingsTouch() and main.cpp suppresses dashboard redraws.
// ---------------------------------------------------------------------------

// Enter Settings from the THEME picker.
void settingsEnterFromPicker();

// Enter Settings directly on the WI-FI page (fresh-install onboarding at
// boot: no saved credentials and the build-time network unreachable).
void settingsEnterWifi();

// True while the Settings screen (incl. keyboards) is on screen.
bool settingsActive();

// One completed touch at (x, y); called from touch.cpp only when active.
void settingsTouch(int16_t x, int16_t y);

// TEMP RANGES slider drag: touch.cpp calls settingsDragMove() for every
// new finger position while settingsDragActive() is true (a slider is
// being dragged) and settingsDragEnd() when the finger lifts. No other
// settings page drags, so the routing is inert elsewhere.
bool settingsDragActive();
void settingsDragMove(int16_t x, int16_t y);
void settingsDragEnd();
