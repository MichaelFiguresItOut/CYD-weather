#include "touch.h"
#include "display.h"
#include "config.h"
#include "weather.h"
#include "wifi_conn.h"
#include "render.h"
#include "picker.h"
#include "settings.h"
#include "led_status.h"
#include "nws_alerts.h"
#include "alerts_screen.h"

// ---------------------------------------------------------------------------
//  Touch targets (per-action debounce: 300 ms zone taps, 2 s refresh)
//
//  Generous hit rectangles. Checked in this order (BASIC layout values shown):
//    dog corner zone  x >= width-96, y >= height-128   -> THEME picker;
//                     swipe left/right to browse themes, tap to select
//    clock band       y in [12,50)                      -> toggle 12h / 24h
//    date band        y in [50,72)                       -> cycle "day month year"
//                                                          <-> "weekday month, day, year"
//    temperature band x < width-96, y in [92,138)        -> toggle F / C
//    anywhere else                                                -> weather refresh
//  Each band contains nothing but its item (see the layout comments above),
//  so wide hit areas are safe. Bands follow the ACTIVE THEME: theme 0 derives
//  from gLayout; themes 1-3 use fixed bands around their design positions.
// ---------------------------------------------------------------------------
struct TouchBands {
  int clkTop, clkBot, dateTop, dateBot, tempTop, tempBot, tempMaxX;
  int alX, alY, alW, alH;   // "WEATHER ALERT" pill (alertBtnRect, render.h)
};
static TouchBands touchBands() {
  switch (userSettings.theme) {
    case 0: {   // BASIC – original gLayout-derived zones
      TouchBands b{ gLayout.statusBarY + glyphH(1) + 2, gLayout.clockY + glyphH(gLayout.clockSize),
                    gLayout.clockY + glyphH(gLayout.clockSize), gLayout.dateY + glyphH(gLayout.dateSize) + 4,
                    gLayout.tempY - 4, gLayout.tempY + glyphH(gLayout.tempSize) + 2,
                    tft.width() - gLayout.dogZoneW, 0, 0, 0, 0 };
      const AlertBtnRect r = alertBtnRect(userSettings.theme);
      b.alX = r.x; b.alY = r.y; b.alW = r.w; b.alH = r.h;
      return b;
    }
    case 1: {   // SKY – status y7, clock y19 (≤5.25), date y62, temp y76 (≤6)
      TouchBands b{ 18, 61, 61, 75, 75, 125, 220, 0, 0, 0, 0 };
      const AlertBtnRect r = alertBtnRect(userSettings.theme);
      b.alX = r.x; b.alY = r.y; b.alW = r.w; b.alH = r.h;
      return b;
    }
    case 2: {   // SPLIT – status y9, clock y24 (≤4.6), date y64, temp y80; left zone only
      TouchBands b{ 20, 62, 62, 77, 77, 125, 210, 0, 0, 0, 0 };
      const AlertBtnRect r = alertBtnRect(userSettings.theme);
      b.alX = r.x; b.alY = r.y; b.alW = r.w; b.alH = r.h;
      return b;
    }
    default: {  // BENTO – clock card [8,60) split at the date line; temp card [66,138]
      TouchBands b{ 8, 40, 40, 60, 66, 138, 202, 0, 0, 0, 0 };
      const AlertBtnRect r = alertBtnRect(userSettings.theme);
      b.alX = r.x; b.alY = r.y; b.alW = r.w; b.alH = r.h;
      return b;
    }
  }
}

static bool inDogZone(int x, int y) {   // reserved dog corner zone (bottom-right)
  if (userSettings.theme == THEME_BENTO) return x >= 206 && y >= 66;   // BENTO: whole right column (FEELS box + dog card)
  return x >= tft.width() - gLayout.dogZoneW && y >= tft.height() - gLayout.dogZoneH;
}

static bool inClockZone(int x, int y) {  // band between the status row and the date line
  TouchBands b = touchBands();
  return y >= b.clkTop && y < b.clkBot;
}

static bool inDateZone(int x, int y) {   // band just under the clock, around the date line
  TouchBands b = touchBands();
  return y >= b.dateTop && y < b.dateBot;
}

static bool inTempZone(int x, int y) {   // big temperature digits (left of the dog zone)
  TouchBands b = touchBands();
  return x >= 0 && x < b.tempMaxX && y >= b.tempTop && y < b.tempBot;
}

static bool inAlertZone(int x, int y) {  // "WEATHER ALERT" pill (when active)
  if (!alertsActive()) return false;
  TouchBands b = touchBands();
  // 4 px of slack around the drawn pill (BENTO's is only 14 px tall).
  return x >= b.alX && x < b.alX + b.alW && y >= b.alY - 4 && y < b.alY + b.alH + 4;
}

// Gesture tracking for the THEME picker: a gesture starts on touch-down and is
// evaluated when all fingers lift. Horizontal movement > 24 px (and clearly
// more than vertical) = swipe; otherwise a short, small-movement press = tap.
struct SwipeState {
  bool active = false;
  int startX = 0, startY = 0;
  int lastX = 0, lastY = 0;
  uint32_t startMs = 0;
};
static SwipeState swipe;

// Settings tap edge state: set when a DOWN edge was consumed, cleared when
// the finger lifts. File scope so settingsTouchRearm() can reset it after a
// long blocking operation (see touch.h).
static bool settingsDown = false;
static bool alertsDown = false;     // touch-down edge for the alert detail page (set/reset in handleTouch)

// Re-arm the Settings tap edge (see touch.h for why).
void settingsTouchRearm() {
  lgfx::touch_point_t pts[5];
  if (tft.getTouch(pts, 5) == 0)
    settingsDown = false;
}

void handleTouch() {
  lgfx::touch_point_t pts[5];
  int n = tft.getTouch(pts, 5);

  // ---- WEATHER ALERTS screen: modal — owns every tap while it is up.
  //  It can only be reached from the main display (and only while an alert
  //  is cached), so no other modal can be active underneath it.
  //  The press that OPENED the screen is usually still on the glass when the
  //  page is drawn, so the picker-style release drain (alerts_screen) swallows
  //  it; taps then fire on the DOWN edge only (re-armed when all fingers lift)
  //  so one hold pages once, not once per loop.
  if (alertsScreenActive()) {
    alertsScreenPollRelease();
    if (alertsScreenAwaitingRelease()) return;   // swallow the press that opened us
    if (n > 0) {
      if (!alertsDown) {
        alertsDown = true;
        alertsScreenTouch(pts[0].x, pts[0].y);
      }
    } else {
      alertsDown = false;
    }
    return;
  }
  alertsDown = false;   // screen closed (closeAlerts) — clear the down-edge state

  // ---- SETTINGS screen: modal — route every tap, the main-display zones and
  // the THEME picker are inactive while it is up (also reached from the
  // picker's top-bar "SETTINGS" button).
  //  Fires on the DOWN edge only and re-arms on touch-up: a tap that switches
  //  the page must not re-fire on the new page while the finger is still down.
  // ----
  if (settingsActive()) {
    if (n > 0) {
      if (!settingsDown) {
        settingsDown = true;
        settingsTouch(pts[0].x, pts[0].y);
      } else if (settingsDragActive()) {
        // TEMP RANGES slider: continuous finger position while a slider is
        // being dragged (inert on every other settings page).
        settingsDragMove(pts[0].x, pts[0].y);
      }
    } else {
      if (settingsDragActive()) settingsDragEnd();   // finger lift: save
      settingsDown = false;
    }
    return;
  }
  settingsDown = false;

  // The press that just OPENED or CLOSED the alert screen may still be on
  // the glass: swallow the rest of it so it can't fall through to the
  // dashboard zones below (pill re-open / refresh), picker or theme dot.
  alertsScreenPollRelease();
  if (alertsScreenAwaitingRelease()) return;

  // ---- THEME picker: gesture tracking (swipe left/right, tap to select) ----
  if (themeSelectScreen) {
    pickerPollRelease();
    if (pickerAwaitingRelease()) return;   // swallow the press that opened us
    static uint32_t lastAction = 0;   // debounce between completed gestures
    if (!swipe.active && n > 0) {
      swipe.active = true;
      swipe.startX = swipe.lastX = pts[0].x;
      swipe.startY = swipe.lastY = pts[0].y;
      swipe.startMs = millis();
      return;
    }
    if (swipe.active) {
      if (n > 0) {                    // finger(s) still down: track the first point
        swipe.lastX = pts[0].x;
        swipe.lastY = pts[0].y;
      } else {                        // all fingers lifted -> evaluate
        swipe.active = false;
        int dx = swipe.lastX - swipe.startX;
        int dy = swipe.lastY - swipe.startY;
        int adx = abs(dx), ady = abs(dy);
        uint32_t dt = millis() - swipe.startMs;

        if (adx > 24 && adx > 2 * ady) {          // horizontal swipe -> next/prev theme
          if (millis() - lastAction < 300) return;   // debounce
          lastAction = millis();
          animatePickerSwipe(dx < 0 ? +1 : -1);   // slides old out / new in, updates previewTheme
        } else if (dt < 600 && adx <= 12 && ady <= 12) {   // tap
          int tx = swipe.startX, ty = swipe.startY;
          if (ty < 20) {                  // top bar: corner buttons only
            if (tx < 56) {                // "< BACK"
              themeSelectScreen = false;
              Serial.println("[TOUCH] picker -> dashboard");
              renderScreen();
              return;
            }
            if (tx >= 264) {              // "SETTINGS"
              themeSelectScreen = false;
              Serial.println("[TOUCH] picker -> settings");
              settingsEnterFromPicker();
              return;
            }
            Serial.println("[TOUCH] picker top bar ignored");
            return;
          }
          // Bottom bar (y>=220) already shows "< PREV" (left) and "NEXT >"
          // (right): tapping either corner jumps to that theme instantly,
          // no slide animation. The bar's empty middle is ignored.
          if (ty >= 220) {
            if (tx < 96) {
              if (millis() - lastAction < 300) return;   // debounce
              lastAction = millis();
              previewTheme = (previewTheme + THEME_COUNT - 1) % THEME_COUNT;
              drawThemePicker();
              Serial.printf("[TOUCH] picker corner -> prev %s (%d)\n", themeName(previewTheme), previewTheme);
              return;
            }
            if (tx > 224) {
              if (millis() - lastAction < 300) return;   // debounce
              lastAction = millis();
              previewTheme = (previewTheme + 1) % THEME_COUNT;
              drawThemePicker();
              Serial.printf("[TOUCH] picker corner -> next %s (%d)\n", themeName(previewTheme), previewTheme);
              return;
            }
            Serial.println("[TOUCH] picker bottom bar ignored");
            return;
          }
          userSettings.theme = previewTheme;   // tap elsewhere: select this theme
          saveTheme();
          themeSelectScreen = false;
          Serial.printf("[TOUCH] theme selected -> %s (%d)\n", themeName(userSettings.theme), userSettings.theme);
          renderScreen();
        } else {
          Serial.println("[TOUCH] theme gesture ignored (long/ambiguous)");
        }
      }
    }
    return;   // main-display zones are inactive while the picker is up
  }

  if (n == 0) return;
  int x = pts[0].x, y = pts[0].y;

  // Per-action debounce (the action fires on touch-down): zone taps get
  // 300 ms — just double-tap protection; a manual refresh keeps 2 s so a
  // fast network can't be spammed with back-to-back fetches (the fetch
  // itself is blocking, so the 2 s only guards the tail after it completes).
  static uint32_t lastToggle = 0;   // dog / clock / date / temp zone taps
  static uint32_t lastRefresh = 0;  // manual weather refresh
  bool refreshTap = !inDogZone(x, y) && !inClockZone(x, y) &&
                    !inDateZone(x, y) && !inTempZone(x, y) && !inAlertZone(x, y);
  if (refreshTap) {
    if (millis() - lastRefresh < 2000) return;   // debounce
    lastRefresh = millis();
  } else {
    if (millis() - lastToggle < 300) return;     // debounce
    lastToggle = millis();
  }

  Serial.printf("[TOUCH] (%d,%d)\n", x, y);

  if (inAlertZone(x, y)) {              // "WEATHER ALERT" pill -> alert list
    alertsScreenOpen();                 // arms its opening-press drain
    return;
  }

  if (inDogZone(x, y)) {                // dog corner zone -> THEME picker
    themeSelectScreen = true;
    previewTheme = userSettings.theme;
    swipe.active = false;
    pickerEnter();                       // this tap is still on the glass
    Serial.println("[TOUCH] -> theme picker");
    drawThemePicker();
    return;
  }

  if (inClockZone(x, y)) {              // clock -> 12h <-> 24h
    userSettings.use24Hour = !userSettings.use24Hour;
    Serial.printf("[TOUCH] clock -> %s\n", userSettings.use24Hour ? "24h" : "12h");
    saveDisplaySettings();                // explicit user change -> persist
    renderScreen();                           // full redraw in the new format
    return;
  }

  if (inDateZone(x, y)) {               // date line -> cycle format
    userSettings.dateFormat = (userSettings.dateFormat + 1) % 2;
    Serial.printf("[TOUCH] date -> format %d\n", userSettings.dateFormat);
    saveDisplaySettings();                // explicit user change -> persist
    renderScreen();
    return;
  }

  if (inTempZone(x, y)) {               // temperature -> F <-> C
    userSettings.useCelsius = !userSettings.useCelsius;
    Serial.printf("[TOUCH] temp -> %s\n", userSettings.useCelsius ? "C" : "F");
    saveDisplaySettings();                // explicit user change -> persist
    renderScreen();                           // all displayed temps convert together
    return;
  }

  // Anywhere else: force a weather refresh (original behavior). One activity
  // over connect + fetch + outfit, so the LED never flashes green between
  // the steps.
  ledActivityBegin();
  if (!wifiEnsureConnected()) {
    renderScreen();   // show offline status
    ledActivityEnd();
    return;
  }
  bool got = fetchWeather();
  if (got) refreshDogImage();
  ledActivityEnd();
  renderScreen();
}
