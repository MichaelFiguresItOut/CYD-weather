#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
//  Full-screen "WEATHER ALERTS" detail screen (modal). One alert per view;
//  a long alert spans several pages (10 body lines each).
//
//  Opened by tapping the dashboard's "WEATHER ALERT" button (touch.cpp).
//  Same modal contract as the Settings screen: while active, handleTouch()
//  routes every tap here and the dashboard zones / picker are inactive.
//  Layout follows the settings screens: "< BACK" + "<"/">" pills in the
//  blue header bar, a dim footer hint. Body taps go to the next page, then
//  the next alert; on the last alert's last page a body tap goes back to
//  the dashboard.
//
//  Text is drawn in the Nunito BFF face (em13 body, em12 event line, em9
//  meta lines) via
//  render.h's nunitoEm9()/nunitoEm12()/nunitoEm13(); if the fonts failed
//  screen falls back to the built-in GLCD font (same layout, 34-char lines).
// ---------------------------------------------------------------------------

void alertsScreenOpen();            // show the first alert (arms the press drain)
bool alertsScreenActive();
void alertsScreenTouch(int16_t x, int16_t y);

// Opening/closing-press drain (same contract as the picker, see picker.h):
// the tap that opened the screen is usually still on the glass when the
// page is drawn, and the press that closed it is still down when the
// dashboard is redrawn underneath. touch.cpp feeds alertsScreenPollRelease()
// each loop and skips all input while alertsScreenAwaitingRelease() is true.
void alertsScreenPollRelease();
bool alertsScreenAwaitingRelease();
