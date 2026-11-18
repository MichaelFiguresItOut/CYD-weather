// ---------------------------------------------------------------------------
//  SETTINGS screen — modal, full-screen. Entry: the "SETTINGS" button on
//  the THEME picker's top bar (touch.cpp routes the tap here).
//
//  Pages (row-tap navigation, every page has "< BACK"):
//    MAIN         – APPEARANCE / WEATHER / TIME & DATE / WI-FI
//    APPEARANCE   – theme (cycles 4), °F/°C, 12/24 h, date format
//    WEATHER      – latitude / longitude (keypadNumeric)
//    TIME         – time zone (paged list, TZLIST), DST on/off
//    WIFI         – current network (connect), network scan
//    SCAN         – scan results (tap to join, keyboardQwerty password)
//
//  "< BACK" on MAIN returns to the THEME picker (the picker's own "< BACK"
//  then goes to the dashboard). All changes persist immediately via the
//  save*() functions in config.cpp — no Apply step.
//
//  While settingsActive() is true the main loop does not repaint the
//  dashboard (main.cpp guard), so nothing here touches the main display;
//  the dashboard is repainted by the picker's BACK handler when the user
//  leaves.
//
//  Geometry: 320x240 landscape, built-in fonts 1-3, LovyanGFX 1.3 API
//  (setTextSize + textWidth without the built-in-font argument).
// ---------------------------------------------------------------------------
#include "settings.h"

#include <WiFi.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "display.h"
#include "picker.h"
#include "keyboard.h"
#include "theme_ids.h"
#include "timezones.h"
#include "wifi_conn.h"
#include "weather.h"

namespace {

enum Page { PAGE_MAIN, PAGE_APPEARANCE, PAGE_WEATHER, PAGE_TIME,
            PAGE_TZLIST, PAGE_WIFI, PAGE_SCAN };

// --- Geometry (320x240) ------------------------------------------------
constexpr int HDR_H      = 26;    // blue header bar
constexpr int BTN_BACK_X = 2,  BTN_BACK_W = 58, BTN_H = 22;   // "< BACK"
constexpr int BTN_NEXT_X = 260, BTN_NEXT_W = 58;              // ">"  (TZ list, fixed slot)
constexpr int BTN_PREV_X = BTN_NEXT_X - BTN_NEXT_W - 4;       // "<"  (TZ list, just left of ">")
constexpr int ROW_X      = 4, ROW_W = 312;
constexpr int ROW_Y0     = 34, ROW_H = 36, ROW_GAP = 3;        // up to 5 rows

constexpr int TZ_PER_PAGE   = 5;    // TIMEZONES per TIMEZONE list page
constexpr int SCAN_PER_PAGE = 4;    // networks per SCAN page (row 5 = RESCAN)
constexpr int SCAN_LIST_MAX = 20;   // stored scan results (paginated)
constexpr int WIFI_SSID_MAX = 31;
constexpr int WIFI_MIN_RSSI = -85;  // ignore only truly weak links (noise floor)

// Colors (displayed values; C() maps them to this inverted panel's code
// space). Page background is dark so the white rows and the light footer
// text keep good contrast; text that sits directly on colBg uses colBgTxt.
const uint16_t colBg     = C(0x24, 0x31, 0x42);
const uint16_t colHeader = C(0x2f, 0x7d, 0xc9);
const uint16_t colRowBg  = C(0xFF, 0xFF, 0xFF);
const uint16_t colRowSel = C(0xd9, 0xec, 0xff);
const uint16_t colRowBd  = C(0xc9, 0xd2, 0xdc);
const uint16_t colBtnBg  = C(0xff, 0xd1, 0x66);   // "< BACK" – accent (picker yellow)
const uint16_t colBtnBd  = C(0x9a, 0x74, 0x14);
const uint16_t colBtnTxt = C(0x14, 0x14, 0x14);
const uint16_t colNextBg = C(0xFF, 0xFF, 0xFF);
const uint16_t colNextBd = C(0xc9, 0xd2, 0xdc);
const uint16_t colText   = C(0x17, 0x22, 0x2e);
const uint16_t colDim    = C(0x66, 0x75, 0x86);
const uint16_t colBgTxt  = C(0xf0, 0xf4, 0xf8);   // light text on dark page bg
const uint16_t colHint   = C(0xa9, 0xb9, 0xcc);   // footer hint on dark bg
const uint16_t colWarn   = C(0xd4, 0x55, 0x00);

// Screen state
int  page = PAGE_MAIN;
int  sGrid = 0;               // highlighted row (per page)
bool sActive = false;
int  tzPage = 0;              // TIMEZONE list page
int  scanPage = 0;            // SCAN list page
char savedSsid[WIFI_SSID_MAX + 1] = "";
int  netCount = 0;
WifiScanItem nets[SCAN_LIST_MAX];
bool scanning = false;
char wifiMsg[48] = "";        // transient status ("JOINING...", "FAILED", ...)

// LovyanGFX 1.3: textWidth() no longer takes a built-in font number —
// measure with the text style (same pattern as the rest of the codebase).
int txtW(const char* s, int size) {
  tft.setTextSize((float)size);
  return (int)tft.textWidth(s);
}

// Fit s into maxPx by trimming leading characters (keyboard-style).
void fitText(const char* s, int size, int maxPx, char* out, int outLen) {
  if (txtW(s, size) <= maxPx) { snprintf(out, outLen, "%s", s); return; }
  size_t start = 0, n = strlen(s);
  while (start < n && txtW(s + start, size) > maxPx) start++;
  snprintf(out, outLen, "%s", s + start);
}

// Cut s from the end so it fits maxPx at the given size (long SSIDs, etc.).
void truncText(const char* s, int size, int maxPx, char* out, int outLen) {
  if (txtW(s, size) <= maxPx) { snprintf(out, outLen, "%s", s); return; }
  size_t n = strnlen(s, 40);
  while (n > 0 && txtW(s, size) > maxPx) n--;
  snprintf(out, outLen, "%.*s", (int)n, s);
}

void setStatus(const char* s) { snprintf(wifiMsg, sizeof(wifiMsg), "%s", s); }

// Wi-Fi status line: transient message wins while set, else the live state.
void statusText(char* out, int n) {
  if (wifiMsg[0]) { snprintf(out, n, "%s", wifiMsg); return; }
  if (scanning)   { snprintf(out, n, "SCANNING..."); return; }
  if (WiFi.status() == WL_CONNECTED) { snprintf(out, n, "%s", WiFi.SSID().c_str()); return; }
  snprintf(out, n, "OFFLINE");
}

int tzPageCount() { return (TIMEZONE_COUNT + TZ_PER_PAGE - 1) / TZ_PER_PAGE; }
int scanPageCount() {
  int p = (netCount + SCAN_PER_PAGE - 1) / SCAN_PER_PAGE;
  return (p < 1) ? 1 : p;
}

// --- Drawing -----------------------------------------------------------

// Top-right pills: hasNext = ">" (TZ page next), hasPrev = "<" just to its
// left (TZ page previous); action = an accent pill in the ">" slot (the SCAN
// page's "RESCAN"). hasNext/action are never both true. The pill slots stay
// at fixed x positions on every page — only their presence varies.
void drawHeader(const char* title, bool hasNext, const char* action = nullptr,
                bool hasPrev = false, int titleCx = 160) {
  tft.fillRect(0, 0, 320, HDR_H, colHeader);
  tft.fillRoundRect(BTN_BACK_X, 2, BTN_BACK_W, BTN_H, 5, colBtnBg);
  tft.drawRoundRect(BTN_BACK_X, 2, BTN_BACK_W, BTN_H, 5, colBtnBd);
  if (hasNext) {
    tft.fillRoundRect(BTN_NEXT_X, 2, BTN_NEXT_W, BTN_H, 5, colNextBg);
    tft.drawRoundRect(BTN_NEXT_X, 2, BTN_NEXT_W, BTN_H, 5, colNextBd);
  } else if (action && action[0]) {
    tft.fillRoundRect(BTN_NEXT_X, 2, BTN_NEXT_W, BTN_H, 5, colBtnBg);
    tft.drawRoundRect(BTN_NEXT_X, 2, BTN_NEXT_W, BTN_H, 5, colBtnBd);
  }
  if (hasPrev) {
    tft.fillRoundRect(BTN_PREV_X, 2, BTN_NEXT_W, BTN_H, 5, colNextBg);
    tft.drawRoundRect(BTN_PREV_X, 2, BTN_NEXT_W, BTN_H, 5, colNextBd);
  }
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(colBtnTxt);
  tft.drawString("< BACK", BTN_BACK_X + BTN_BACK_W / 2, 13);
  if (hasNext) {
    tft.setTextSize(2);
    tft.setTextColor(colHeader);
    tft.drawString(">", BTN_NEXT_X + BTN_NEXT_W / 2, 12);
  } else if (action && action[0]) {
    tft.setTextSize(1);
    tft.setTextColor(colBtnTxt);
    tft.drawString(action, BTN_NEXT_X + BTN_NEXT_W / 2, 13);
  }
  if (hasPrev) {
    tft.setTextSize(2);
    tft.setTextColor(colHeader);
    tft.drawString("<", BTN_PREV_X + BTN_NEXT_W / 2, 12);
  }
  tft.setTextColor(C(0xFF, 0xFF, 0xFF));
  tft.setTextSize(2);
  tft.drawString(title, titleCx, 13);
}

// Bottom line: dim hint (left) + Wi-Fi status (right).
void drawFooter(const char* hint) {
  char st[64];
  statusText(st, sizeof(st));
  char fit[40];
  fitText(st, 1, 110, fit, sizeof(fit));
  tft.setTextSize(1);
  tft.setTextColor(colHint);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(hint, 8, 226);
  tft.setTextDatum(TR_DATUM);
  tft.drawString(fit, 312, 226);
}

void drawRow(int i, const char* a, const char* b, bool sel) {
  int y = ROW_Y0 + i * (ROW_H + ROW_GAP);
  tft.fillRoundRect(ROW_X, y, ROW_W, ROW_H, 6, sel ? colRowSel : colRowBg);
  tft.drawRoundRect(ROW_X, y, ROW_W, ROW_H, 6, colRowBd);
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(2);
  tft.setTextColor(colText);
  tft.drawString(a, ROW_X + 10, y + 6);
  tft.setTextSize(1);
  tft.setTextColor(colDim);
  tft.drawString(b, ROW_X + 10, y + 22);
  if (sel) {
    tft.setTextColor(colHeader);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(2);
    tft.drawString(">", 296, y + ROW_H / 2);
  }
}

void drawListRow(int i, const char* l, const char* value, bool sel, bool warn = false) {
  int y = ROW_Y0 + i * (ROW_H + ROW_GAP);
  tft.fillRoundRect(ROW_X, y, ROW_W, ROW_H, 6, sel ? colRowSel : colRowBg);
  tft.drawRoundRect(ROW_X, y, ROW_W, ROW_H, 6, colRowBd);
  // Left label: size 2 when it fits, else size 1, cut from the END to stay
  // clear of the right-side value (long SSIDs, tz names). TL_DATUM at a
  // fixed inset — an MC_DATUM at x=14 would center the text on the row's
  // left edge and clip half of every glyph.
  char lab[48];
  int sz = (txtW(l, 2) <= 180) ? 2 : 1;
  if (sz == 2) snprintf(lab, sizeof(lab), "%s", l);
  else         truncText(l, 1, 205, lab, sizeof(lab));
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(sz);
  tft.setTextColor(warn ? colWarn : colText);
  tft.drawString(lab, ROW_X + 10, y + (sz == 2 ? 7 : 10));
  if (value && value[0]) {
    char val[48];
    int vMax = 308 - (ROW_X + 10 + txtW(lab, sz)) - 8;
    if (vMax < 40) vMax = 40;
    truncText(value, 1, vMax, val, sizeof(val));
    tft.setTextDatum(TR_DATUM);
    tft.setTextSize(1);
    tft.setTextColor(colDim);
    tft.drawString(val, 308, y + 10);
  }
}

// --- Pages ---------------------------------------------------------------

void drawMain() {
  tft.fillRect(0, HDR_H, 320, 240 - HDR_H, colBg);
  drawHeader("SETTINGS", false);
  const char* items[4][2] = {
    { "APPEARANCE",  "theme · units · clock · date" },
    { "WEATHER",     "location (lat/lon)" },
    { "TIME & DATE", "time zone · daylight saving" },
    { "WI-FI",       "networks · join" },
  };
  for (int i = 0; i < 4; i++)
    drawRow(i, items[i][0], items[i][1], i == sGrid);
  drawFooter("< BACK RETURNS TO THEME PICKER");
}

void drawAppearance() {
  tft.fillRect(0, HDR_H, 320, 240 - HDR_H, colBg);
  drawHeader("APPEARANCE", false);
  char fmt[32];
  snprintf(fmt, sizeof(fmt), "%s",
           userSettings.dateFormat == 0 ? "THURSDAY 12 NOVEMBER 2026"
                                        : "THURSDAY NOVEMBER 12, 2026");
  drawListRow(0, "THEME",       themeName(userSettings.theme), sGrid == 0);
  drawListRow(1, "TEMPERATURE", userSettings.useCelsius ? "CELSIUS" : "FAHRENHEIT", sGrid == 1);
  drawListRow(2, "CLOCK",       userSettings.use24Hour ? "24 HOUR" : "12 HOUR",  sGrid == 2);
  drawListRow(3, "DATE",        fmt, sGrid == 3);
  drawFooter("TAP TO CHANGE");
}

void drawWeather() {
  tft.fillRect(0, HDR_H, 320, 240 - HDR_H, colBg);
  drawHeader("WEATHER", false);
  char lat[24], lon[24];
  snprintf(lat, sizeof(lat), "LATITUDE: %.4f", userSettings.lat);
  snprintf(lon, sizeof(lon), "LONGITUDE: %.4f", userSettings.lon);
  const char* mode = userSettings.locationMode == SettingMode::AUTOMATIC ? "AUTO" : "";
  drawListRow(0, lat, mode, sGrid == 0);
  drawListRow(1, lon, "", sGrid == 1);
  drawFooter("TAP A ROW TO EDIT");
}

void drawTime() {
  tft.fillRect(0, HDR_H, 320, 240 - HDR_H, colBg);
  drawHeader("TIME & DATE", false);
  char tz[40];
  fitText(userSettings.timezone.name, 2, 200, tz, sizeof(tz));
  drawListRow(0, "TIME ZONE", tz, sGrid == 0);
  bool dst = (userSettings.timezone.dstOffsetSec != userSettings.timezone.stdOffsetSec);
  drawListRow(1, "DAYLIGHT SAVING", dst ? "DST ON" : "DST OFF", sGrid == 1);
  drawFooter("TAP TO CHANGE");
}

void drawTzList() {
  tft.fillRect(0, HDR_H, 320, 240 - HDR_H, colBg);
  // "<" (when not first page) + ">" (when not last page); the title is
  // centered in the space left of the "<" slot so it never collides.
  drawHeader("TIME ZONE", tzPage < tzPageCount() - 1, nullptr,
             tzPage > 0, 128);
  int cur = timezoneIndexByRules(userSettings.timezone);
  for (int i = 0; i < TZ_PER_PAGE; i++) {
    int idx = tzPage * TZ_PER_PAGE + i;
    if (idx >= (int)TIMEZONE_COUNT) break;
    char val[12];
    snprintf(val, sizeof(val), "%s", idx == cur ? "SELECTED" : "");
    drawListRow(i, TIMEZONES[idx].name, val, i == sGrid);
  }
  char hint[40];
  snprintf(hint, sizeof(hint), "PAGE %d/%d · TAP TO SELECT", tzPage + 1, tzPageCount());
  drawFooter(hint);
}

void drawWifi() {
  tft.fillRect(0, HDR_H, 320, 240 - HDR_H, colBg);
  drawHeader("WI-FI", false);
  // Row 0: the connected network as plain status text — deliberately
  // without the white button styling, and not tappable.
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(colDim);
  tft.drawString("CURRENT NETWORK", ROW_X + 10, ROW_Y0 + 8);
  bool on = (WiFi.status() == WL_CONNECTED);
  char cur[48], curFit[48];
  snprintf(cur, sizeof(cur), "%s", on ? WiFi.SSID().c_str() : "OFFLINE");
  truncText(cur, 2, 280, curFit, sizeof(curFit));
  tft.setTextSize(2);
  tft.setTextColor(on ? C(0xFF, 0xFF, 0xFF) : colHint);
  tft.drawString(curFit, ROW_X + 10, ROW_Y0 + 22);
  // Row 1: the one actionable row.
  char nval[16];
  if (scanning)            snprintf(nval, sizeof(nval), "SCANNING...");
  else if (netCount > 0)   snprintf(nval, sizeof(nval), "%d FOUND", netCount);
  else                     snprintf(nval, sizeof(nval), "TAP TO SCAN");
  drawListRow(1, "NETWORKS", nval, sGrid == 1);
  drawFooter("TAP NETWORKS TO SCAN / JOIN");
}

void drawScan() {
  tft.fillRect(0, HDR_H, 320, 240 - HDR_H, colBg);
  // The top-right corner is the page arrows now (same as the TZ page), so
  // RESCAN is the last row of the list instead of a header pill.
  drawHeader("WI-FI SCAN", scanPage < scanPageCount() - 1, nullptr,
             scanPage > 0, 128);
  if (scanning || netCount == 0) {
    tft.setTextColor(colBgTxt);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(2);
    tft.drawString(scanning ? "Scanning for networks..." : "No networks found.", 160, 120);
    drawFooter("");
    return;
  }
  for (int i = 0; i < SCAN_PER_PAGE; i++) {
    int idx = scanPage * SCAN_PER_PAGE + i;
    if (idx >= netCount) break;
    char l[44], val[16];
    snprintf(l, sizeof(l), "%s%s", nets[idx].ssid, nets[idx].secure ? " [WPA]" : "");
    snprintf(val, sizeof(val), "%d dBm", nets[idx].rssi);
    drawListRow(i, l, val, false);
  }
  drawListRow(SCAN_PER_PAGE, "RESCAN", "scan again", false);
  char hint[48];
  if (scanPageCount() > 1)
    snprintf(hint, sizeof(hint), "PAGE %d/%d · TAP A NETWORK TO JOIN",
             scanPage + 1, scanPageCount());
  else
    snprintf(hint, sizeof(hint), "TAP A NETWORK TO JOIN");
  drawFooter(hint);
}

void draw() {
  switch (page) {
    case PAGE_MAIN:       drawMain();       break;
    case PAGE_APPEARANCE: drawAppearance(); break;
    case PAGE_WEATHER:    drawWeather();    break;
    case PAGE_TIME:       drawTime();       break;
    case PAGE_TZLIST:     drawTzList();     break;
    case PAGE_WIFI:       drawWifi();       break;
    case PAGE_SCAN:       drawScan();       break;
  }
}

// --- Actions -------------------------------------------------------------

// Return to the THEME picker (entry point); its "< BACK" goes to the
// dashboard and repaints it.
void backToPicker() {
  sActive = false;
  themeSelectScreen = true;
  previewTheme = userSettings.theme;   // preview follows the saved theme
  pickerEnter();   // the press that tapped "< BACK" is still down; let the
                   // picker swallow it instead of acting on the new screen
  drawThemePicker();
}

void scanNetworks() {
  scanPage = 0;
  scanning = true;
  setStatus("");
  draw();
  delay(50);
  netCount = wifiScanNetworks(nets, SCAN_LIST_MAX, WIFI_MIN_RSSI);
  scanning = false;
  draw();
}

// Join nets[i]; password via the QWERTY keyboard when the network is
// secured. ENTER commits, < BACK discards (the previously saved password,
// if any, stays in NVS). On success the new credentials are saved to NVS.
void joinNet(int i) {
  if (i < 0 || i >= netCount) return;
  char ssid[WIFI_SSID_MAX + 2] = "";                  // nets[] ssid is [33]: keep it whole
  snprintf(ssid, sizeof(ssid), "%s", nets[i].ssid);   // stable copy: a RESCAN could rewrite nets[]
  char pass[65] = "";
  if (nets[i].secure) {
    int r = keyboardQwerty(ssid, "WIFI PASSWORD", pass, sizeof(pass));
    if (r != KEY_OK) { draw(); return; }        // < BACK: previous password kept
    // An empty password is allowed: some WPA networks genuinely use none.
    // (Open networks never reach the keyboard at all.)
  }
  setStatus("JOINING...");
  draw();
  delay(50);
  if (wifiConnectTo(nets[i].ssid, pass)) {
    wifiSaveCredentials(nets[i].ssid, pass);
    snprintf(savedSsid, sizeof(savedSsid), "%.31s", nets[i].ssid);
    setStatus("CONNECTED");
    page = PAGE_WIFI;
    sGrid = 1;      // highlight the NETWORKS row (row 0 is status only)
    draw();
  } else {
    setStatus("FAILED");
    draw();
  }
}

void editCoord(bool isLat) {
  char num[24];
  snprintf(num, sizeof(num), "%.4f", isLat ? userSettings.lat : userSettings.lon);
  int r = keypadNumeric(isLat ? "LATITUDE" : "LONGITUDE",
                        isLat ? "-90..90" : "-180..180",
                        num, (int)sizeof(num), 12,
                        isLat ? -90.0f : -180.0f, isLat ? 90.0f : 180.0f);
  if (r != KEY_OK) { draw(); return; }   // < BACK: original coordinate kept
  float v = strtof(num, nullptr);
  if (v < (isLat ? -90.0f : -180.0f) || v > (isLat ? 90.0f : 180.0f)) { draw(); return; }
  if (isLat) userSettings.lat = v; else userSettings.lon = v;
  userSettings.locationMode = SettingMode::MANUAL;
  saveLocation();
  fetchWeather();
  draw();
}

void onTzTap(int i) {
  int idx = tzPage * TZ_PER_PAGE + i;
  if (idx >= (int)TIMEZONE_COUNT) return;
  userSettings.timezone = TIMEZONES[idx].rules;
  userSettings.timezoneMode = SettingMode::MANUAL;
  saveTimezone();
  page = PAGE_TIME;
  sGrid = 0;
  draw();
}

// --- Touch routing -------------------------------------------------------

// Row under y (0..4), -1 when outside the row band.
int rowAt(int16_t y) {
  if (y < ROW_Y0) return -1;
  int i = (y - ROW_Y0) / (ROW_H + ROW_GAP);
  int rem = (y - ROW_Y0) % (ROW_H + ROW_GAP);
  if (i < 0 || i > 4 || rem >= ROW_H) return -1;
  return i;
}

// Header button under (x,y): 1 = back, 2 = ">", 3 = "<", 0 = none.
// (Only the TZ page draws the arrows; other pages guard by page.)
int headerBtnAt(int16_t x, int16_t y) {
  if (y > HDR_H) return 0;
  if (x >= BTN_BACK_X && x < BTN_BACK_X + BTN_BACK_W) return 1;
  if (x >= BTN_PREV_X && x < BTN_PREV_X + BTN_NEXT_W) return 3;
  if (x >= BTN_NEXT_X && x < BTN_NEXT_X + BTN_NEXT_W) return 2;
  return 0;
}

void handleTap(int16_t x, int16_t y) {
  int hb = headerBtnAt(x, y);
  if (hb == 1) {
    switch (page) {
      case PAGE_MAIN:       backToPicker();      break;
      case PAGE_APPEARANCE:
      case PAGE_WEATHER:
      case PAGE_TIME:       page = PAGE_MAIN; sGrid = 0; draw(); break;
      case PAGE_TZLIST:     page = PAGE_TIME;  sGrid = 0; draw(); break;
      case PAGE_SCAN:       page = PAGE_WIFI;  sGrid = 1; draw(); break;
      case PAGE_WIFI:       page = PAGE_MAIN;  sGrid = 3; draw(); break;
    }
    return;
  }
  if (hb == 2) {   // ">" (TZ page / SCAN page)
    if (page == PAGE_TZLIST && tzPage < tzPageCount() - 1) {
      tzPage++;
      sGrid = 0;
      draw();
    } else if (page == PAGE_SCAN && scanPage < scanPageCount() - 1) {
      scanPage++;
      draw();
    }
    return;
  }
  if (hb == 3) {   // "<" (TZ page / SCAN page)
    if (page == PAGE_TZLIST && tzPage > 0) {
      tzPage--;
      sGrid = 0;
      draw();
    } else if (page == PAGE_SCAN && scanPage > 0) {
      scanPage--;
      draw();
    }
    return;
  }
  if (y < HDR_H) return;

  int r = rowAt(y);
  switch (page) {
    case PAGE_MAIN:
      if (r < 0 || r > 3) return;
      sGrid = r;
      switch (r) {
        case 0: page = PAGE_APPEARANCE; sGrid = 0; draw(); break;
        case 1: page = PAGE_WEATHER;    sGrid = 0; draw(); break;
        case 2: page = PAGE_TIME;       sGrid = 0; draw(); break;
        case 3: page = PAGE_WIFI;       sGrid = 1; draw();
              // No scan on entry: it runs from the SCAN page (the
              // NETWORKS button or the RESCAN row) — on demand only.
      }
      break;
    case PAGE_APPEARANCE:
      if (r < 0 || r > 3) return;
      sGrid = r;
      switch (r) {
        case 0: userSettings.theme = (userSettings.theme + 1) % THEME_COUNT;
                saveTheme();
                break;
        case 1: userSettings.useCelsius = !userSettings.useCelsius;
                saveDisplaySettings();
                break;
        case 2: userSettings.use24Hour = !userSettings.use24Hour;
                saveDisplaySettings();
                break;
        case 3: userSettings.dateFormat = (userSettings.dateFormat + 1) % 2;
                saveDisplaySettings();
                break;
      }
      draw();
      break;
    case PAGE_WEATHER:
      if (r < 0 || r > 1) return;
      sGrid = r;
      editCoord(r == 0);
      break;
    case PAGE_TIME:
      if (r < 0 || r > 1) return;
      sGrid = r;
      if (r == 0) {
        int cur = timezoneIndexByRules(userSettings.timezone);
        tzPage = cur / TZ_PER_PAGE;
        sGrid  = cur % TZ_PER_PAGE;
        page = PAGE_TZLIST;
        draw();
      } else {
        // DST on/off: the zones in the list shift +1 h, and "off" is
        // expressed as dstOffsetSec == stdOffsetSec (the DST window in
        // time_util.cpp then has no effect).
        if (userSettings.timezone.dstOffsetSec == userSettings.timezone.stdOffsetSec)
          userSettings.timezone.dstOffsetSec = userSettings.timezone.stdOffsetSec + 3600;
        else
          userSettings.timezone.dstOffsetSec = userSettings.timezone.stdOffsetSec;
        saveTimezone();
        draw();
      }
      break;
    case PAGE_TZLIST:
      if (r < 0) return;
      onTzTap(r);
      break;
    case PAGE_WIFI:
      // Only the NETWORKS row (r == 1) is tappable; the status row above
      // it (r == 0) is display-only.
      if (r != 1) return;
      page = PAGE_SCAN;
      scanNetworks();
      break;
    case PAGE_SCAN:
      if (r == SCAN_PER_PAGE) {        // RESCAN row
        scanNetworks();
        return;
      }
      if (r < 0 || r >= SCAN_PER_PAGE) return;
      {
        int idx = scanPage * SCAN_PER_PAGE + r;
        if (idx < netCount) joinNet(idx);
      }
      break;
  }
}

}  // namespace

// --- Public API ----------------------------------------------------------

void settingsEnterFromPicker() {
  sActive = true;
  page = PAGE_MAIN;
  sGrid = 0;
  tzPage = 0;
  scanPage = 0;
  netCount = 0;
  scanning = false;
  wifiMsg[0] = 0;
  if (!prefs.getString("ssid", savedSsid, sizeof(savedSsid)))
    savedSsid[0] = 0;
  draw();
}

bool settingsActive() { return sActive; }

void settingsTouch(int16_t x, int16_t y) { handleTap(x, y); }
