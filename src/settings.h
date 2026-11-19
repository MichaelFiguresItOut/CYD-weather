#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
//  Settings screen (modal, full-screen)
//
//  Entry: the SETTINGS button (top-right corner) on the THEME picker's top
//  bar; the picker's < BACK corner returns to the dashboard. The main page
//  shows four tiles:
//
//    APPEARANCE   - display theme (4 choices; day/night is automatic)
//    LOCATION     - manual latitude/longitude (numeric keypad)
//    TIME ZONE    - timezone list, daylight saving on/off
//    WI-FI        - network scan + join (QWERTY keyboard for credentials),
//                   manual SSID entry, radio on/off, restore the build-time
//                   network (erases user-saved credentials)
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

// True while the Settings screen (incl. keyboards) is on screen.
bool settingsActive();

// One completed touch at (x, y); called from touch.cpp only when active.
void settingsTouch(int16_t x, int16_t y);
