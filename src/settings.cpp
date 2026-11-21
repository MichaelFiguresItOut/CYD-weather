// ---------------------------------------------------------------------------
//  SETTINGS screen — modal, full-screen. Entry: the "SETTINGS" button on
//  the THEME picker's top bar (touch.cpp routes the tap here).
//
//  Pages (row-tap navigation, every page has "< BACK"):
//    MAIN         – APPEARANCE / LOCATION / TIME ZONE / WI-FI
//    APPEARANCE   – theme (cycles 4), °F/°C, 12/24 h, date format
//    LOCATION     – current location (display), ADD LOCATION (QWERTY +
//                   geocode lookup), latitude / longitude (keypadNumeric)
//    POSTAL       – location lookup (postal code or city): searching
//                   status (OPEN-METEO / NOMINATIM), then results (tap one
//                   to apply) or a no-match / failure message
//    TIME         – time zone (paged list, TZLIST), DST on/off
//    WIFI         – current network, network scan, manual SSID entry,
//                   radio on/off, restore build-time network
//    SCAN         – scan results (tap to join, keyboardQwerty password)
//    STATIONS     – NWS observation-station discovery (Phase 3): nearest
//                   stations with distance + latest-observation freshness
//    CONFIRM      – CLEAR / RESET confirmation (LOCATION / TIME ZONE /
//                   WI-FI): < BACK cancels, the red button performs the
//                   reset and returns to the settings page
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
#include "touch.h"

#include <WiFi.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>

#include "config.h"
#include "display.h"
#include "geocode.h"
#include "picker.h"
#include "keyboard.h"
#include "local_config.h"
#include "theme_ids.h"
#include "timezones.h"
#include "wifi_conn.h"
#include "nws_station.h"
#include "weather.h"

namespace {

enum Page { PAGE_MAIN, PAGE_APPEARANCE, PAGE_WEATHER, PAGE_TIME,
            PAGE_TZLIST, PAGE_WIFI, PAGE_SCAN, PAGE_POSTAL, PAGE_STATIONS,
            PAGE_STN_SCAN, PAGE_CONFIRM };

// --- Geometry (320x240) ------------------------------------------------
constexpr int HDR_H      = 26;    // blue header bar
constexpr int BTN_BACK_X = 2,  BTN_BACK_W = 58, BTN_H = 22;   // "< BACK"
constexpr int BTN_NEXT_X = 260, BTN_NEXT_W = 58;              // ">"  (TZ list, fixed slot)
constexpr int BTN_PREV_X = BTN_NEXT_X - BTN_NEXT_W - 4;       // "<"  (TZ list, just left of ">")
constexpr int ROW_X      = 4, ROW_W = 312;
constexpr int ROW_Y0     = 34, ROW_H = 36, ROW_GAP = 3;        // up to 5 rows
// Red CLEAR / RESET button (bottom of the LOCATION / TIME ZONE / WI-FI
// pages and the confirmation page): full width, fixed height.
constexpr int RESET_BTN_H  = 28;
// The WI-FI page uses compact rows (no footer) so the red reset button
// fits at the bottom of the 240 px screen.
constexpr int WIFI_ROW_Y0  = 66;
constexpr int WIFI_ROW_H   = 32;
constexpr int WIFI_ROW_GAP = 2;

constexpr int TZ_PER_PAGE   = 5;    // TIMEZONES per TIMEZONE list page
constexpr int SCAN_PER_PAGE = 4;    // networks per SCAN page (row 5 = RESCAN)
constexpr int STN_PER_PAGE  = 4;    // stations per STATIONS page (row 5 = REFRESH)
constexpr int STN_MAX       = 15;   // matches the nws_station parse cap
constexpr int GEO_PER_PAGE  = 5;    // location search results per page
constexpr int GEO_LIST_MAX  = 20;   // search results fetched (paged at GEO_PER_PAGE)
// Stored scan results (paginated). The ESP32 keeps every AP the passive
// pass sees; this is how many we copy out of it — 20 was not enough in a
// busy area and the user's own (weaker or later-channel) network was cut
// off the list.
constexpr int SCAN_LIST_MAX = 40;
constexpr int WIFI_SSID_MAX = 31;
constexpr int WIFI_MIN_RSSI = -90;  // weak-but-present APs still show (phones list to ~-90); below is noise

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
const uint16_t colDanger   = C(0xc8, 0x2a, 0x2a);   // red CLEAR / RESET buttons
const uint16_t colDangerBd = C(0x74, 0x15, 0x15);

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
// What the confirmation page (PAGE_CONFIRM) is asking about: which section
// the red CLEAR / RESET button would clear.
enum ResetTarget { RT_NONE, RT_LOCATION, RT_TIMEZONE, RT_WIFI };
int  resetTarget = RT_NONE;
GeoResult geoResults[GEO_LIST_MAX];  // location search results (all pages)
int  geoCount = 0;
int  geoPage = 0;                    // location results list page
// POSTAL page phase: the page stays up through the whole lookup —
// searching (service named), results, or a terminal message.
enum PostalPhase { PP_SEARCHING, PP_RESULTS, PP_MSG };
PostalPhase postalPhase = PP_MSG;
char postalMsg[48] = "";
// STATIONS page state (Phase 3, NWS observation-station discovery). Same
// pattern as POSTAL: the page stays up through the lookup. Latest
// observations are fetched per page (up to STN_PER_PAGE requests) when the
// page is shown, so paging stays fast and request counts stay bounded.
enum StnPhase { STN_SEARCHING, STN_RESULTS, STN_MSG };
StnPhase stnPhase = STN_MSG;
char stnMsg[48] = "";
NwsStation stn[STN_MAX];
int  stnCount = 0;
int  stnPage = 0;
void drawStnScan();   // used by draw() before its definition below

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
// The loop must measure the shrinking PREFIX, not the original string
// (measuring `s` keeps the condition constant and truncates to "").
void truncText(const char* s, int size, int maxPx, char* out, int outLen) {
  if (txtW(s, size) <= maxPx) { snprintf(out, outLen, "%s", s); return; }
  char tmp[81];
  size_t n = strnlen(s, 80);
  while (n > 0) {
    snprintf(tmp, sizeof(tmp), "%.*s", (int)n, s);
    if (txtW(tmp, size) <= maxPx) break;
    n--;
  }
  snprintf(out, outLen, "%.*s", (int)n, s);
}

// Map a tap's x to a character index of the number in a coordinate row
// ("LATITUDE: -41.2862"), mirroring drawListRow's rendering: the whole
// label at size 2 if it fits 180 px, else size 1 (these 17-20 char labels
// always fit the 205 px truncation budget, so nothing is cut), left-
// aligned at x = ROW_X + 10. Taps on the "LATITUDE:" part map to 0.
int coordCursorAt(const char* prefix, const char* num, int x) {
  char label[48];
  snprintf(label, sizeof(label), "%s%s", prefix, num);
  int sz = (txtW(label, 2) <= 180) ? 2 : 1;
  int x0 = ROW_X + 10 + txtW(prefix, sz);
  return textCharIndexAt(num, x0, x, (float)sz);
}

void setStatus(const char* s) { snprintf(wifiMsg, sizeof(wifiMsg), "%s", s); }

// Wi-Fi status line: transient message wins while set — paired with the
// live SSID while connected ("MyNet · CONNECTED") so a successful join or
// restore names the network it landed on; else the live state.
void statusText(char* out, int n) {
  if (wifiMsg[0]) {
    if (WiFi.status() == WL_CONNECTED)
      snprintf(out, n, "%s \xC2\xB7 %s", WiFi.SSID().c_str(), wifiMsg);
    else
      snprintf(out, n, "%s", wifiMsg);
    return;
  }
  if (scanning)   { snprintf(out, n, "SCANNING..."); return; }
  if (WiFi.status() == WL_CONNECTED) { snprintf(out, n, "%s", WiFi.SSID().c_str()); return; }
  snprintf(out, n, "OFFLINE");
}

int tzPageCount() { return (TIMEZONE_COUNT + TZ_PER_PAGE - 1) / TZ_PER_PAGE; }
int scanPageCount() {
  int p = (netCount + SCAN_PER_PAGE - 1) / SCAN_PER_PAGE;
  return (p < 1) ? 1 : p;
}
int geoPageCount() {
  int p = (geoCount + GEO_PER_PAGE - 1) / GEO_PER_PAGE;
  return (p < 1) ? 1 : p;
}

// --- Drawing -----------------------------------------------------------

// Top-right pills: hasNext = ">" (TZ/SCAN page next), hasPrev = "<" just to
// its left (page previous); action = an accent pill in the ">" slot.
// hasNext/action are never both true. The pill slots stay at fixed x
// positions on every page — only their presence varies.
void drawHeader(const char* title, bool hasNext, const char* action = nullptr,
                bool hasPrev = false) {
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
  // Title: centered in the pill-free gap, auto-shrunk (the keyboard header
  // does the same) so long titles like "WI-FI SCAN (2.4GHz only)" stay clear
  // of "< BACK" and the page arrows.
  int l = BTN_BACK_X + BTN_BACK_W + 4;
  int rr = hasPrev ? (BTN_PREV_X - 4)
           : (hasNext || (action && action[0])) ? (BTN_NEXT_X - 4)
           : 316;
  int tw = txtW(title, 2);
  float ts = (tw > rr - l) ? (float)(rr - l) * 2.0f / (float)tw : 2.0f;
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(ts);
  tft.setTextColor(C(0xFF, 0xFF, 0xFF));
  tft.drawString(title, (l + rr) / 2, 13);
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

// y < 0 / default h: the standard row i geometry (ROW_Y0 stride). The
// compact WI-FI rows pass explicit y + WIFI_ROW_H.
void drawListRow(int i, const char* l, const char* value, bool sel, bool warn = false,
                 int y = -1, int h = ROW_H) {
  if (y < 0) y = ROW_Y0 + i * (ROW_H + ROW_GAP);
  tft.fillRoundRect(ROW_X, y, ROW_W, h, 6, sel ? colRowSel : colRowBg);
  tft.drawRoundRect(ROW_X, y, ROW_W, h, 6, colRowBd);
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
  tft.drawString(lab, ROW_X + 10, y + (h == ROW_H ? (sz == 2 ? 7 : 10)
                                                  : (sz == 2 ? 6 : 9)));
  if (value && value[0]) {
    char val[48];
    int vMax = 308 - (ROW_X + 10 + txtW(lab, sz)) - 8;
    if (vMax < 40) vMax = 40;
    truncText(value, 1, vMax, val, sizeof(val));
    tft.setTextDatum(TR_DATUM);
    tft.setTextSize(1);
    tft.setTextColor(colDim);
    tft.drawString(val, 308, y + (h == ROW_H ? 10 : 9));
  }
}

// WI-FI page compact rows: top edge of row i (1..4) and the row under y.
int wifiRowY(int i) { return WIFI_ROW_Y0 + (i - 1) * (WIFI_ROW_H + WIFI_ROW_GAP); }
int wifiRowAt(int16_t y) {
  if (y < WIFI_ROW_Y0) return -1;
  int d   = y - WIFI_ROW_Y0;
  int i   = d / (WIFI_ROW_H + WIFI_ROW_GAP);
  int rem = d % (WIFI_ROW_H + WIFI_ROW_GAP);
  if (i < 0 || i > 3 || rem >= WIFI_ROW_H) return -1;
  return i + 1;
}

// Top edge of the red CLEAR / RESET button on the current page (-1: none).
int resetBtnY() {
  switch (page) {
    case PAGE_WEATHER:   // LOCATION: standard rows end at 187, footer below
    case PAGE_TIME:      // TIME ZONE: rows end at 109
      return 192;
    case PAGE_WIFI:      // compact rows end at 200 (no footer on this page)
      return 204;
    case PAGE_CONFIRM:
      return 180;
    default:
      return -1;
  }
}

// Red CLEAR / RESET button: full-width red pill with a centred label. Same
// look on the three settings pages and the confirmation page (where it is
// labelled "RESET / CLEAR").
void drawResetButton(const char* label) {
  int y = resetBtnY();
  if (y < 0) return;
  tft.fillRoundRect(ROW_X, y, ROW_W, RESET_BTN_H, 6, colDanger);
  tft.drawRoundRect(ROW_X, y, ROW_W, RESET_BTN_H, 6, colDangerBd);
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(2);
  tft.setTextColor(C(0xFF, 0xFF, 0xFF));
  tft.drawString(label, ROW_X + ROW_W / 2, y + RESET_BTN_H / 2);
}

// --- Pages ---------------------------------------------------------------

void drawMain() {
  tft.fillRect(0, HDR_H, 320, 240 - HDR_H, colBg);
  drawHeader("SETTINGS", false);
  const char* items[5][2] = {
    { "APPEARANCE",  "theme - units - clock - date" },
    { "LOCATION",    "add location - lat/lon" },
    { "TIME ZONE",   "time zone - daylight saving" },
    { "WI-FI",       "networks - join - add" },
    { "STATIONS",    "data source - local NWS station" },
  };
  for (int i = 0; i < 5; i++)
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
  drawHeader("LOCATION", false);
  // Row 0: the resolved location as plain display text — deliberately
  // without the white button styling (see the WI-FI page's CURRENT
  // NETWORK row) and not tappable (handleTap ignores row 0). Shows
  // "City, Region, CC" plus the postal code when known; "--" when the
  // location is not resolved yet.
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(colDim);
  tft.drawString("CURRENT LOCATION", ROW_X + 10, ROW_Y0 + 8);
  char loc[72], locFit[72];
  if (userSettings.placeName[0])
    snprintf(loc, sizeof(loc), "%s%s%s", userSettings.placeName,
             userSettings.postalCode[0] ? " - " : "", userSettings.postalCode);
  else
    snprintf(loc, sizeof(loc), "--");
  int locSz = (txtW(loc, 2) <= 296) ? 2 : 1;
  truncText(loc, locSz, 296, locFit, sizeof(locFit));
  tft.setTextSize(locSz);
  tft.setTextColor(userSettings.placeName[0] ? C(0xFF, 0xFF, 0xFF) : colHint);
  tft.drawString(locFit, ROW_X + 10, ROW_Y0 + 22);
  char pc[32];
  snprintf(pc, sizeof(pc), "%s",
           userSettings.postalCode[0] ? userSettings.postalCode : "TAP TO ENTER");
  drawListRow(1, "ADD LOCATION", pc, sGrid == 1);
  char lat[24], lon[24];
  snprintf(lat, sizeof(lat), "LATITUDE: %.4f", userSettings.lat);
  snprintf(lon, sizeof(lon), "LONGITUDE: %.4f", userSettings.lon);
  const char* mode = userSettings.locationMode == SettingMode::AUTOMATIC ? "AUTO" : "";
  drawListRow(2, lat, mode, sGrid == 2);
  drawListRow(3, lon, "", sGrid == 3);
  drawResetButton("CLEAR / RESET");
  drawFooter("TAP A ROW TO EDIT");
}

// ADD LOCATION search results: one row per candidate place — the display
// name "City, Region, CC" (city, state, country; no coordinates — they are
// not useful for choosing). Paged 5 per page with the same "<"/">" header
// arrows as the TIME ZONE list when the results fill more than one page.
// Tapping a row applies that result (see applyLocation).
void drawPostal() {
  tft.fillRect(0, HDR_H, 320, 240 - HDR_H, colBg);
  drawHeader("ADD LOCATION", geoPage < geoPageCount() - 1, nullptr, geoPage > 0);
  if (postalPhase != PP_RESULTS) {
    // Searching / terminal message: centered status, same pattern as the
    // SCAN page's "Scanning for networks...".
    tft.setTextDatum(MC_DATUM);
    int msgSz = (txtW(postalMsg, 2) <= 300) ? 2 : 1;   // long messages fit size 1
    tft.setTextSize(msgSz);
    tft.setTextColor(postalPhase == PP_SEARCHING ? colBgTxt : colWarn);
    tft.drawString(postalMsg, 160, 108);
    drawFooter(postalPhase == PP_SEARCHING ? "" : "TAP < BACK TO GO BACK");
    return;
  }
  for (int i = 0; i < GEO_PER_PAGE; i++) {
    int idx = geoPage * GEO_PER_PAGE + i;
    if (idx >= geoCount) break;
    drawListRow(i, geoResults[idx].place, "", i == sGrid);
  }
  char hint[40];
  if (geoPageCount() > 1)
    snprintf(hint, sizeof(hint), "PAGE %d/%d - TAP TO APPLY", geoPage + 1, geoPageCount());
  else
    snprintf(hint, sizeof(hint), "%d RESULT%s - TAP TO APPLY", geoCount,
             geoCount == 1 ? "" : "S");
  drawFooter(hint);
}

void drawTime() {
  tft.fillRect(0, HDR_H, 320, 240 - HDR_H, colBg);
  drawHeader("TIME ZONE", false);
  // Hand the full name to drawListRow (value column is size 1, right-aligned,
  // trailing-trimmed); the longest zone name ("Australia East (AEST)",
  // 21 chars) fits untrimmed. The old fitText() pre-pass trimmed from the
  // START, so "Eastern Europe (EET)" showed up as "ern Europe (EET)".
  drawListRow(0, "TIME ZONE", userSettings.timezone.name, sGrid == 0);
  bool dst = (userSettings.timezone.dstOffsetSec != userSettings.timezone.stdOffsetSec);
  drawListRow(1, "DAYLIGHT SAVING", dst ? "DST ON" : "DST OFF", sGrid == 1);
  drawResetButton("CLEAR / RESET");
  // Footer shows how the zone was chosen. In automatic mode an IANA id with
  // no catalog mapping is surfaced here instead of silently falling back to
  // the last valid rules (the id itself goes to the serial log).
  char footer[44];
  if (userSettings.timezoneMode == SettingMode::MANUAL)
    snprintf(footer, sizeof(footer), "MANUAL - TAP TO CHANGE");
  else if (autoTimezoneUnsupported())
    snprintf(footer, sizeof(footer), "AUTO - UNSUPPORTED ZONE");
  else
    snprintf(footer, sizeof(footer), "AUTO - TAP TO CHANGE");
  drawFooter(footer);
}

void drawTzList() {
  tft.fillRect(0, HDR_H, 320, 240 - HDR_H, colBg);
  // "<" (when not first page) + ">" (when not last page); the title is
  // centered in the space left of the "<" slot so it never collides.
  drawHeader("TIME ZONE", tzPage < tzPageCount() - 1, nullptr, tzPage > 0);
  int cur = timezoneIndexByRules(userSettings.timezone);
  for (int i = 0; i < TZ_PER_PAGE; i++) {
    int idx = tzPage * TZ_PER_PAGE + i;
    if (idx >= (int)TIMEZONE_COUNT) break;
    char val[12];
    snprintf(val, sizeof(val), "%s", idx == cur ? "SELECTED" : "");
    drawListRow(i, TIMEZONES[idx].name, val, i == sGrid);
  }
  char hint[40];
  snprintf(hint, sizeof(hint), "PAGE %d/%d - TAP TO SELECT", tzPage + 1, tzPageCount());
  drawFooter(hint);
}

void drawWifi() {
  tft.fillRect(0, HDR_H, 320, 240 - HDR_H, colBg);
  drawHeader("WI-FI (2.4GHz only)", false);
  // Compact rows (see WIFI_ROW_*) leave room for the red reset button at
  // the bottom; the page has no footer, so the status line (transient
  // message first, then the network state) is shown in the row 0 block.
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(colDim);
  tft.drawString("CURRENT NETWORK", ROW_X + 10, 36);
  char st[64];
  statusText(st, sizeof(st));
  bool on = (WiFi.status() == WL_CONNECTED);
  char cur[48];
  truncText(st, 2, 280, cur, sizeof(cur));
  tft.setTextSize(2);
  tft.setTextColor(wifiMsg[0] || scanning ? colWarn : (on ? C(0xFF, 0xFF, 0xFF) : colHint));
  tft.drawString(cur, ROW_X + 10, 48);
  char nval[16];
  if (scanning)            snprintf(nval, sizeof(nval), "SCANNING...");
  else if (netCount > 0)   snprintf(nval, sizeof(nval), "%d FOUND", netCount);
  else                     snprintf(nval, sizeof(nval), "TAP TO SCAN");
  drawListRow(1, "SCAN NETWORKS", nval, sGrid == 1, false, wifiRowY(1), WIFI_ROW_H);
  drawListRow(2, "ADD NETWORK", "TYPE SSID / PASS", sGrid == 2, false, wifiRowY(2), WIFI_ROW_H);
  // Row 3: radio power (off is a session setting, not persisted).
  drawListRow(3, "WIFI ON/OFF", wifiEnabled() ? "ON" : "OFF", sGrid == 3, false, wifiRowY(3), WIFI_ROW_H);
  // Row 4: restore the build-time network (erases user-saved credentials).
  char def[40];
  truncText(WIFI_SSID, 1, 130, def, sizeof(def));
  drawListRow(4, "RESTORE SAVED NETWORK", def, sGrid == 4, false, wifiRowY(4), WIFI_ROW_H);
  drawResetButton("CLEAR / RESET");
}

// Confirmation page behind the red CLEAR / RESET buttons. < BACK (header)
// returns to the settings page without changing anything; only the red
// button performs the reset.
void drawConfirm() {
  tft.fillRect(0, HDR_H, 320, 240 - HDR_H, colBg);
  const char* title =
      (resetTarget == RT_LOCATION) ? "CLEAR LOCATION"
      : (resetTarget == RT_TIMEZONE) ? "RESET TIME ZONE"
                                     : "RESET WI-FI";
  drawHeader(title, false);

  const char* lines[5];
  switch (resetTarget) {
    case RT_LOCATION:
      lines[0] = "ERASES: postal code,";
      lines[1] = "latitude, longitude and";
      lines[2] = "place name. The location";
      lines[3] = "becomes UNCONFIGURED.";
      lines[4] = "Timezone is not changed.";
      break;
    case RT_TIMEZONE:
      lines[0] = "THE TIME ZONE BECOMES";
      lines[1] = "AUTOMATIC AND FOLLOWS";
      lines[2] = "YOUR LOCATION, SET ON";
      lines[3] = "THE NEXT WEATHER UPDATE.";
      lines[4] = "Your location is kept.";
      break;
    default:  // RT_WIFI
      lines[0] = "ERASES THE SAVED WI-FI";
      lines[1] = "CREDENTIALS AND JOINS";
      lines[2] = "THE BUILT-IN DEFAULT";
      lines[3] = "NETWORK.";
      lines[4] = "Radio on/off unchanged.";
      break;
  }
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(2);
  for (int i = 0; i < 5; i++) {
    tft.setTextColor(i < 4 ? C(0xFF, 0xFF, 0xFF) : colHint);
    tft.drawString(lines[i], 10, 46 + i * 20);
  }
  drawResetButton("RESET / CLEAR");
  drawFooter("PRESS < BACK TO CANCEL");
}

void drawScan() {
  tft.fillRect(0, HDR_H, 320, 240 - HDR_H, colBg);
  // The top-right corner is the page arrows now (same as the TZ page), so
  // RESCAN is the last row of the list instead of a header pill.
  drawHeader("WI-FI SCAN (2.4GHz only)", scanPage < scanPageCount() - 1, nullptr,
             scanPage > 0);
  if (scanning || netCount == 0) {
    tft.setTextColor(colBgTxt);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(2);
    tft.drawString(scanning ? "Scanning for networks..." : "No networks found.", 160, 108);
    // The RESCAN row stays visible even with an empty list, so the page is
    // never a dead end.
    drawListRow(SCAN_PER_PAGE, "RESCAN", "scan again", false);
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
    snprintf(hint, sizeof(hint), "PAGE %d/%d - TAP A NETWORK TO JOIN",
             scanPage + 1, scanPageCount());
  else
    snprintf(hint, sizeof(hint), "TAP A NETWORK TO JOIN");
  drawFooter(hint);
}

// STATIONS page (Phase 3): where the weather data comes from (information
// only, not a button), "and", the selected local NWS station with its
// details (or "NO LOCAL WEATHER STATION"), and the SCAN button that runs
// the Local Weather Station lookup. Only the SCAN button (row slot 2) is
// tappable; the text above it is display-only.
void drawStations() {
  tft.fillRect(0, HDR_H, 320, 240 - HDR_H, colBg);
  drawHeader("STATIONS", false);
  drawFooter("NWS STATIONS (US ONLY) - TAP SCAN TO LOOK UP");

  const int x = ROW_X + 10;
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(colBgTxt);
  tft.drawString("WEATHER DATA BY OPEN-METEO.COM", x, 40);
  tft.setTextColor(colDim);
  tft.drawString("AND", x, 56);
  if (userSettings.nwsStationId[0]) {
    char line[52], fit[52];
    snprintf(line, sizeof(line), "%.7s - %.39s",
             userSettings.nwsStationId, userSettings.nwsStationName);
    for (char* c = line; *c; c++) *c = (char)toupper((unsigned char)*c);
    int sz = (txtW(line, 2) <= 296) ? 2 : 1;
    truncText(line, sz, 296, fit, sizeof(fit));
    tft.setTextSize(sz);
    tft.setTextColor(C(0xFF, 0xFF, 0xFF));
    tft.drawString(fit, x, 72);
    char det[40];
    snprintf(det, sizeof(det), "%.1f KM AWAY - %s",
             userSettings.nwsStationDistKm, userSettings.nwsStationProvider);
    tft.setTextSize(1);
    tft.setTextColor(colDim);
    tft.drawString(det, x, 92);
  } else {
    tft.setTextSize(2);
    tft.setTextColor(C(0xFF, 0xFF, 0xFF));
    tft.drawString("NO LOCAL WEATHER STATION", x, 72);
  }
  drawListRow(2, "SCAN FOR LOCAL WEATHER STATION", "", sGrid == 2, false,
              112, ROW_H);
}

void draw() {
  // Every long blocking operation in the settings flow (keyboard modal,
  // geocoding, weather fetch, Wi-Fi scan/join) ends in a redraw; re-arm the
  // tap edge so the first tap afterwards is not swallowed. No-op while a
  // finger is still down (a tap that switched the page keeps its suppression).
  settingsTouchRearm();
  switch (page) {
    case PAGE_MAIN:       drawMain();       break;
    case PAGE_APPEARANCE: drawAppearance(); break;
    case PAGE_WEATHER:    drawWeather();    break;
    case PAGE_TIME:       drawTime();       break;
    case PAGE_TZLIST:     drawTzList();     break;
    case PAGE_WIFI:       drawWifi();       break;
    case PAGE_CONFIRM:    drawConfirm();    break;
    case PAGE_SCAN:       drawScan();       break;
    case PAGE_POSTAL:     drawPostal();     break;
    case PAGE_STATIONS:   drawStations();   break;
    case PAGE_STN_SCAN:   drawStnScan();    break;
  }
}

// ---------------------------------------------------------------------------
//  NWS observation-station discovery (Phase 3) — the "Local Weather
//  Station" page (PAGE_STN_SCAN), reached from the SCAN button on the
//  STATIONS page.
//
//  Flow (the page stays up through the whole lookup, same pattern as the
//  ADD LOCATION search): tap SCAN -> stnDiscover():
//    wifi check -> points lookup -> station list -> per-page observation
//    checks (stnCheckPage, up to STN_PER_PAGE requests) -> results.
//  A row tap selects that station (stnSelect: persist + back to the
//  STATIONS page); a REFRESH row re-runs the discovery. A definite
//  "no stations" outcome (outside NWS coverage / empty list) shows the
//  message briefly, clears the selection, and returns to the STATIONS page.
// ---------------------------------------------------------------------------

void stnClearSelection();   // defined below stnDiscover, which calls it
void stnSelect(int i);

void stnReset() {
  stnPhase = STN_MSG;
  stnMsg[0] = 0;
  stnCount = 0;
  stnPage = 0;
}

int stnPageCount() {
  int p = (stnCount + STN_PER_PAGE - 1) / STN_PER_PAGE;
  return (p < 1) ? 1 : p;
}

// Fetch the latest observation for the current page's stations that
// haven't been checked yet (up to STN_PER_PAGE requests), then show the
// results. Runs on entry to the page and on every page turn.
void stnCheckPage() {
  stnPhase = STN_SEARCHING;
  const int first = stnPage * STN_PER_PAGE;
  const int last  = (first + STN_PER_PAGE < stnCount) ? first + STN_PER_PAGE : stnCount;
  for (int i = first; i < last; i++) {
    if (stn[i].hasObs || stn[i].obsFail) continue;
    snprintf(stnMsg, sizeof(stnMsg), "CHECKING %s ...", stn[i].id);
    draw();
    delay(50);
    nwsFetchLatestObs(&stn[i]);   // on failure sets obsFail; the row shows it
  }
  stnPhase = STN_RESULTS;
  draw();
}

void stnDiscover() {
  stnReset();
  stnPhase = STN_SEARCHING;
  if (!wifiEnabled()) {
    stnPhase = STN_MSG;
    snprintf(stnMsg, sizeof(stnMsg), "TURN WIFI ON");
    draw();
    return;
  }
  snprintf(stnMsg, sizeof(stnMsg), "CONNECTING TO WIFI...");
  draw();
  delay(50);
  if (!wifiEnsureConnected()) {
    stnPhase = STN_MSG;
    snprintf(stnMsg, sizeof(stnMsg), "WIFI NOT CONNECTED");
    draw();
    return;
  }
  snprintf(stnMsg, sizeof(stnMsg), "LOOKING UP NWS POINT...");
  draw();
  delay(50);
  char stationsUrl[64];
  const int r = nwsPointsLookup(userSettings.lat, userSettings.lon, stationsUrl,
                                sizeof(stationsUrl));
  if (r == -2) {                        // 404: outside NWS coverage
    stnPhase = STN_MSG;
    snprintf(stnMsg, sizeof(stnMsg), "NOT IN NWS COVERAGE");
    draw();
    delay(2000);                        // show it, then back to STATIONS page
    stnClearSelection();
    return;
  }
  if (r != 0) {
    stnPhase = STN_MSG;
    snprintf(stnMsg, sizeof(stnMsg), "NWS LOOKUP FAILED - RETRY");
    draw();
    return;
  }
  snprintf(stnMsg, sizeof(stnMsg), "FETCHING STATION LIST...");
  draw();
  delay(50);
  // The station list (and with it the observations) may have changed, so
  // drop all cached per-station observation state before parsing.
  for (int i = 0; i < STN_MAX; i++) {
    stn[i].hasObs = false;
    stn[i].obsFail = false;
  }
  stnCount = nwsFetchStations(stationsUrl, stn, STN_MAX);
  if (stnCount < 0) {
    stnPhase = STN_MSG;
    stnCount = 0;
    snprintf(stnMsg, sizeof(stnMsg), "STATION LIST FAILED - RETRY");
    draw();
    return;
  }
  if (stnCount == 0) {
    stnPhase = STN_MSG;
    snprintf(stnMsg, sizeof(stnMsg), "NO STATIONS FOUND");
    draw();
    delay(2000);                        // show it, then back to STATIONS page
    stnClearSelection();
    return;
  }
  stnPage = 0;
  stnCheckPage();                       // -> results
}

// The scan found no stations for the current location (outside NWS coverage
// or empty list): clear the persisted selection so the STATIONS page shows
// "NO LOCAL WEATHER STATION", and return there.
void stnClearSelection() {
  userSettings.nwsStationId[0] = 0;
  userSettings.nwsStationName[0] = 0;
  userSettings.nwsStationDistKm = 0.0f;
  userSettings.nwsStationProvider[0] = 0;
  saveNwsStation();
  page = PAGE_STATIONS;
  sGrid = 2;
  draw();
}

// Row tap on the Local Weather Station page: select that station (persisted
// to NVS) and return to the STATIONS page with the local station populated.
void stnSelect(int i) {
  if (i < 0 || i >= stnCount) return;
  const NwsStation* s = &stn[i];
  snprintf(userSettings.nwsStationId, sizeof(userSettings.nwsStationId), "%s", s->id);
  snprintf(userSettings.nwsStationName, sizeof(userSettings.nwsStationName), "%s", s->name);
  userSettings.nwsStationDistKm = s->distKm;
  snprintf(userSettings.nwsStationProvider, sizeof(userSettings.nwsStationProvider),
           "%s", s->provider);
  saveNwsStation();
  Serial.printf("[STN] selected: %s (%s) %.1f km\n", s->id, s->name, (double)s->distKm);
  page = PAGE_STATIONS;
  sGrid = 2;
  draw();
}

// One station row, two lines:
//   "KNYC - NEW YORK CITY, CENTRAL PARK     8.7 KM"
//   "60.1 F CLEAR - 5M AGO"    (warn colour when the observation is stale)
void drawStationRow(int i, const NwsStation* s) {
  int y = ROW_Y0 + i * (ROW_H + ROW_GAP);
  tft.fillRoundRect(ROW_X, y, ROW_W, ROW_H, 6, colRowBg);
  tft.drawRoundRect(ROW_X, y, ROW_W, ROW_H, 6, colRowBd);
  tft.setTextDatum(TL_DATUM);

  char label[50];
  snprintf(label, sizeof(label), "%.4s - %.39s", s->id, s->name);
  for (char* c = label; *c; c++) *c = (char)toupper((unsigned char)*c);

  char dist[12];
  if (s->distKm < 100.0f)
    snprintf(dist, sizeof(dist), "%.1f KM", s->distKm);
  else
    snprintf(dist, sizeof(dist), "%d KM", (int)(s->distKm + 0.5f));

  // Line 1: label (trimmed to fit) + right-aligned distance.
  const int lx = ROW_X + 10;
  const int budget = (ROW_X + ROW_W - 10) - lx - txtW(dist, 1) - 8;
  if (txtW(label, 2) <= 180) {
    tft.setTextSize(2);
    tft.setTextColor(colText);
    tft.drawString(label, lx, y + 7);
  } else {
    char t[50];
    snprintf(t, sizeof(t), "%s", label);
    size_t n = strlen(t);
    while (n > 3 && txtW(t, 1) > budget) { t[n - 1] = 0; n--; }
    tft.setTextSize(1);
    tft.setTextColor(colText);
    tft.drawString(t, lx, y + 13);
  }
  tft.setTextDatum(TR_DATUM);   // right-align the distance at the row edge
  tft.setTextSize(1);
  tft.setTextColor(colDim);
  tft.drawString(dist, 308, y + 12);

  // Line 2: latest observation + age (warn when stale).
  char obs[60];
  uint16_t obsCol = colDim;
  if (s->hasObs) {
    char age[16];
    nwsAgeText(s->obsAgeSec, age, sizeof(age));
    // No \xB0 degree sign: the GLCD bitmap font has no such glyph (draws a
    // checkered box); the main screen likewise omits it ("57.0 F").
    if (isnan(s->obsTempC))
      snprintf(obs, sizeof(obs), "%s - %s", s->obsText[0] ? s->obsText : "NO DATA", age);
    else if (userSettings.useCelsius)
      snprintf(obs, sizeof(obs), "%.1f C %s - %s", s->obsTempC, s->obsText, age);
    else
      snprintf(obs, sizeof(obs), "%.1f F %s - %s",
               s->obsTempC * 9.0f / 5.0f + 32.0f, s->obsText, age);
    if (s->obsAgeSec >= NWS_OBS_FRESH_MAX_S) obsCol = colWarn;
  } else if (s->obsFail) {
    snprintf(obs, sizeof(obs), "OBSERVATION FETCH FAILED");
  } else {
    snprintf(obs, sizeof(obs), "OBSERVATION NOT CHECKED");
  }
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(obsCol);
  tft.drawString(obs, lx, y + ROW_H - 8);
}

void drawStnScan() {
  tft.fillRect(0, HDR_H, 320, 240 - HDR_H, colBg);
  if (stnPhase != STN_RESULTS) {
    drawHeader("LOCAL WEATHER STATION", false);
    tft.setTextDatum(MC_DATUM);
    const int msgSz = (txtW(stnMsg, 2) <= 300) ? 2 : 1;
    tft.setTextSize(msgSz);
    tft.setTextColor(stnPhase == STN_SEARCHING ? colBgTxt : colWarn);
    tft.drawString(stnMsg, 160, 108);
    drawListRow(STN_PER_PAGE, "REFRESH", "look up again", sGrid == STN_PER_PAGE);
    drawFooter(stnPhase == STN_SEARCHING ? "PLEASE WAIT" : "TAP REFRESH TO RETRY");
    return;
  }
  drawHeader("LOCAL WEATHER STATION", stnPage < stnPageCount() - 1, nullptr,
             stnPage > 0);
  const int first = stnPage * STN_PER_PAGE;
  for (int i = 0; i < STN_PER_PAGE; i++) {
    const int idx = first + i;
    if (idx >= stnCount) break;
    drawStationRow(i, &stn[idx]);
  }
  drawListRow(STN_PER_PAGE, "REFRESH", "look up again", sGrid == STN_PER_PAGE);
  char hint[48];
  if (stnPageCount() > 1)
    snprintf(hint, sizeof(hint), "PAGE %d/%d - TAP A STATION TO SELECT",
             stnPage + 1, stnPageCount());
  else
    snprintf(hint, sizeof(hint), "%d STATION(S) - TAP A STATION TO SELECT",
             stnCount);
  drawFooter(hint);
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
  draw();   // show the results while the link comes back
  // The scan needed the link down for a reliable result, and the main
  // loop's 10 s reconnect is paused while Settings is open — so the scan
  // flow restores the connection itself, with a visible status (otherwise
  // the user sees OFFLINE for the rest of the session).
  WifiCreds c = wifiCredentials();
  setStatus("RECONNECTING...");
  draw();
  delay(50);
  bool up = wifiConnectTo(c.ssid, c.pass);
  setStatus(up ? "CONNECTED" : "OFFLINE");
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
    sGrid = 1;      // highlight the SCAN NETWORKS row (row 0 is status only)
    draw();
  } else {
    setStatus("FAILED");
    draw();
  }
}

// Manual entry (ADD NETWORK row): type an SSID, then a password. ENTER
// with an empty password field joins with none, like an open network. On
// success the credentials are saved to NVS — exactly like joinNet().
void addNet() {
  char ssid[WIFI_SSID_MAX + 1] = "";
  if (keyboardQwerty("ADD NETWORK", "SSID (UP TO 31 CHARS)", ssid,
                     sizeof(ssid)) != KEY_OK) {
    draw();
    return;
  }
  if (ssid[0] == 0) { draw(); return; }   // an empty SSID has nothing to join
  char pass[65] = "";
  if (keyboardQwerty(ssid, "WIFI PASSWORD (ENTER = BLANK)", pass,
                     sizeof(pass)) != KEY_OK) {
    draw();
    return;
  }
  setStatus("JOINING...");
  draw();
  delay(50);
  if (wifiConnectTo(ssid, pass)) {
    wifiSaveCredentials(ssid, pass);
    snprintf(savedSsid, sizeof(savedSsid), "%.31s", ssid);
    setStatus("CONNECTED");
    sGrid = 1;
    draw();
  } else {
    setStatus("FAILED");
    draw();
  }
}

// WIFI ON/OFF row: off kills the radio (the main loop's auto-reconnect is
// paused while it is off); on reconnects with the current credentials.
void toggleRadio() {
  if (wifiEnabled()) {
    wifiSetRadio(false);
    setStatus("");
    draw();
  } else {
    WifiCreds c = wifiCredentials();
    setStatus("JOINING...");
    draw();
    delay(50);
    setStatus(wifiConnectTo(c.ssid, c.pass) ? "CONNECTED" : "FAILED");
    draw();
  }
}

// RESTORE SAVED NETWORK row: erase user-saved credentials from NVS and connect to
// the build-time network (local_config.h). A wrong tap costs at most one
// re-join: the build-time network is exactly what the device boots with.
void restoreDefault() {
  wifiForgetCredentials();
  snprintf(savedSsid, sizeof(savedSsid), "%.31s", WIFI_SSID);
  setStatus("JOINING...");
  draw();
  delay(50);
  setStatus(wifiConnectTo(WIFI_SSID, WIFI_PASS) ? "CONNECTED" : "FAILED");
  sGrid = 4;
  draw();
}

// WI-FI reset (confirmation page): erase the user-saved credentials from
// NVS and return to the built-in defaults (local_config.h). The radio
// on/off state is preserved: while ON it reconnects to the built-in network
// now; while OFF the defaults simply take over on the next manual "on".
void doWifiReset() {
  wifiForgetCredentials();
  if (wifiEnabled()) {
    setStatus("JOINING...");
    draw();
    delay(50);
    setStatus(wifiConnectTo(WIFI_SSID, WIFI_PASS) ? "CONNECTED" : "FAILED");
  }
  page = PAGE_WIFI;
  sGrid = 1;
  draw();
}

// tapX is the row tap's x: the keypad opens with the cursor under the digit
// the finger landed on (coordCursorAt) so the coordinate can be edited in
// place instead of retyped from scratch.
void editCoord(bool isLat, int tapX) {
  char num[24];
  snprintf(num, sizeof(num), "%.4f", isLat ? userSettings.lat : userSettings.lon);
  int cursor = coordCursorAt(isLat ? "LATITUDE: " : "LONGITUDE: ", num, tapX);
  int r = keypadNumeric(isLat ? "LATITUDE" : "LONGITUDE",
                        isLat ? "-90..90 - TAP FIELD TO MOVE |" : "-180..180 - TAP FIELD TO MOVE |",
                        num, (int)sizeof(num), 12,
                        isLat ? -90.0f : -180.0f, isLat ? 90.0f : 180.0f, cursor);
  if (r != KEY_OK) { draw(); return; }   // < BACK: original coordinate kept
  float v = strtof(num, nullptr);
  if (v < (isLat ? -90.0f : -180.0f) || v > (isLat ? 90.0f : 180.0f)) { draw(); return; }
  if (isLat) userSettings.lat = v; else userSettings.lon = v;
  // The keypad range-checks each field, so the pair is complete and in
  // range here; re-validate the whole pair defensively before committing.
  if (userSettings.lat < -90.0f || userSettings.lat > 90.0f ||
      userSettings.lon < -180.0f || userSettings.lon > 180.0f) {
    draw();
    return;
  }
  userSettings.locationMode = SettingMode::MANUAL;
  saveLocation();
  // Refresh the resolved place name (and the postal code, when the service
  // provides one) so CURRENT LOCATION stays in sync with the coordinates.
  // A failed reverse lookup keeps the coordinates — they remain valid and
  // in use, the location just stays unnamed.
  setStatus("RESOLVING...");
  draw();
  delay(50);
  GeoResult g;
  if (geocodeReverse(userSettings.lat, userSettings.lon, &g)) {
    snprintf(userSettings.placeName, sizeof(userSettings.placeName), "%s", g.place);
    if (g.postcode[0])
      snprintf(userSettings.postalCode, sizeof(userSettings.postalCode), "%s", g.postcode);
    saveLocation();
  }
  setStatus("");
  fetchWeather();
  draw();
}

// A US ZIP+4 is "#####-####"; the lookup query uses the base 5 digits
// (Open-Meteo's index is keyed on 5-digit ZIPs), but the full code the
// user typed is what gets stored and displayed.
static bool isZipPlus4(const char* c) {
  for (int i = 0; i < 4; i++) if (!isdigit((unsigned char)c[i])) return false;
  if (c[4] != '-') return false;
  for (int i = 5; i < 9; i++) if (!isdigit((unsigned char)c[i])) return false;
  return c[9] == 0;
}

// ADD LOCATION row: QWERTY entry — a postal code (US ZIP, ZIP+4, or
// international code) or a city name ("NEW YORK, NY", "WELLINGTON, NZ").
// Letters and the "," symbol come from the 123 page of the same keyboard.
// The entered text is saved on entry; coordinates and the place name are
// applied only when the user taps a result (0 results / network failure
// -> the entry is kept, a status line is shown, and the coordinates stay
// put).
void editLocation() {
  char code[32];
  snprintf(code, sizeof(code), "%s", userSettings.postalCode);
  if (keyboardQwerty("ADD LOCATION", "POSTAL CODE OR CITY", code,
                     sizeof(code)) != KEY_OK) {
    draw();
    return;
  }
  // Sanitize: uppercase (postal codes and city names are both
  // case-insensitive for these lookups) + trim.
  for (char* p = code; *p; p++)
    if (*p >= 'a' && *p <= 'z') *p += (int)('A' - 'a');
  char* s = code;
  while (*s == ' ') s++;
  memmove(code, s, strlen(s) + 1);
  size_t n = strlen(code);
  while (n > 0 && code[n - 1] == ' ') code[--n] = 0;
  if (code[0] == 0) { draw(); return; }   // empty entry: discard

  // Store the text the user actually entered (ZIP+4 stays ZIP+4); the
  // lookup query is the base 5 digits for a ZIP+4, everything else
  // (codes and city names) goes through unchanged.
  snprintf(userSettings.postalCode, sizeof(userSettings.postalCode), "%s", code);
  saveLocation();

  char query[32];
  if (isZipPlus4(code)) snprintf(query, sizeof(query), "%.5s", code);
  else                  snprintf(query, sizeof(query), "%s", code);

  // The ADD LOCATION page stays up through the whole lookup: searching
  // status first, then the results (or a terminal message) on the same
  // screen. The header's "< BACK" is the escape hatch at any point.
  setStatus("");
  page = PAGE_POSTAL;
  sGrid = 0;
  geoPage = 0;
  postalPhase = PP_SEARCHING;
  snprintf(postalMsg, sizeof(postalMsg), "LOOKING UP ON OPEN-METEO...");
  draw();
  delay(50);
  int nRes = geocodePostalOpenMeteo(query, geoResults, GEO_LIST_MAX);
  if (nRes == 0) {
    // Definite no-match — usually an international code or a city+state
    // phrase Open-Meteo's name index doesn't cover; Nominatim gets one
    // look. A network failure (-1) is not retried against a second service.
    snprintf(postalMsg, sizeof(postalMsg), "FALLING BACK TO NOMINATIM...");
    draw();
    delay(50);
    nRes = geocodePostalNominatim(query, geoResults, GEO_LIST_MAX);
  }
  if (nRes > 0) {
    geoCount = nRes;
    postalPhase = PP_RESULTS;
  } else if (nRes < 0) {
    postalPhase = PP_MSG;
    snprintf(postalMsg, sizeof(postalMsg), "LOOKUP FAILED - RETRY");
  } else {
    postalPhase = PP_MSG;
    snprintf(postalMsg, sizeof(postalMsg), "NO MATCH FOR %.23s", code);
  }
  draw();
}

// Tap on a location search result: apply the coordinates (mode ->
// MANUAL), keep the entered code/city, store the resolved place name,
// and refetch the weather for the new location.
void applyLocation(int i) {
  if (i < 0 || i >= geoCount) return;
  const GeoResult& g = geoResults[i];
  userSettings.lat = g.lat;
  userSettings.lon = g.lon;
  userSettings.locationMode = SettingMode::MANUAL;
  // g.place is the full display string; fall back to the primary name so
  // a result can never be applied with an empty CURRENT LOCATION.
  snprintf(userSettings.placeName, sizeof(userSettings.placeName), "%s",
           g.place[0] ? g.place : g.name);
  saveLocation();
  Serial.printf("[GEO] applied: place=\"%s\" entry=\"%s\"\n",
                userSettings.placeName, userSettings.postalCode);
  fetchWeather();
  page = PAGE_WEATHER;
  sGrid = 0;
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
// (Only the paged lists draw the arrows; other pages guard by page.)
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
      case PAGE_POSTAL:     page = PAGE_WEATHER; sGrid = 1; draw(); break;
      case PAGE_STATIONS:   page = PAGE_MAIN;  sGrid = 4; draw(); break;
      case PAGE_STN_SCAN:   page = PAGE_STATIONS; sGrid = 2; draw(); break;
      case PAGE_SCAN:       page = PAGE_WIFI;  sGrid = 1; draw(); break;
      case PAGE_WIFI:       page = PAGE_MAIN;  sGrid = 3; draw(); break;
      case PAGE_CONFIRM: {   // BACK: nothing has been changed yet.
        int t = resetTarget;
        resetTarget = RT_NONE;
        page = (t == RT_TIMEZONE) ? PAGE_TIME
             : (t == RT_WIFI)     ? PAGE_WIFI : PAGE_WEATHER;
        sGrid = 0;
        draw();
        break;
      }
    }
    return;
  }
  if (hb == 2) {   // ">" (TZ page / SCAN page / location results)
    if (page == PAGE_TZLIST && tzPage < tzPageCount() - 1) {
      tzPage++;
      sGrid = 0;
      draw();
    } else if (page == PAGE_SCAN && scanPage < scanPageCount() - 1) {
      scanPage++;
      draw();
    } else if (page == PAGE_POSTAL && postalPhase == PP_RESULTS &&
               geoPage < geoPageCount() - 1) {
      geoPage++;
      sGrid = 0;
      draw();
    } else if (page == PAGE_STN_SCAN && stnPhase == STN_RESULTS &&
               stnPage < stnPageCount() - 1) {
      stnPage++;
      stnCheckPage();      // checks the new page's observations, draws
    }
    return;
  }
  if (hb == 3) {   // "<" (TZ page / SCAN page / location results)
    if (page == PAGE_TZLIST && tzPage > 0) {
      tzPage--;
      sGrid = 0;
      draw();
    } else if (page == PAGE_SCAN && scanPage > 0) {
      scanPage--;
      draw();
    } else if (page == PAGE_POSTAL && postalPhase == PP_RESULTS &&
               geoPage > 0) {
      geoPage--;
      sGrid = 0;
      draw();
    } else if (page == PAGE_STN_SCAN && stnPhase == STN_RESULTS &&
               stnPage > 0) {
      stnPage--;
      stnCheckPage();
    }
    return;
  }
  if (y < HDR_H) return;

  // Red CLEAR / RESET button (bottom of the LOCATION / TIME ZONE / WI-FI
  // pages; on the confirmation page it performs the reset and returns to
  // the settings page).
  {
    int by = resetBtnY();
    if (by >= 0 && x >= ROW_X && x < ROW_X + ROW_W && y >= by && y < by + RESET_BTN_H) {
      if (page == PAGE_CONFIRM) {
        switch (resetTarget) {
          case RT_LOCATION:
            resetLocation();
            page = PAGE_WEATHER; sGrid = 0;
            break;
          case RT_TIMEZONE:
            resetTimezone();
            page = PAGE_TIME; sGrid = 0;
            break;
          case RT_WIFI:
            doWifiReset();    // sets page + sGrid, draws itself
            break;
        }
        resetTarget = RT_NONE;
        draw();
      } else {
        resetTarget = (page == PAGE_WEATHER) ? RT_LOCATION
                     : (page == PAGE_TIME)   ? RT_TIMEZONE : RT_WIFI;
        page = PAGE_CONFIRM;
        draw();
      }
      return;
    }
  }

  int r = rowAt(y);
  switch (page) {
    case PAGE_MAIN:
      if (r < 0 || r > 4) return;
      sGrid = r;
      switch (r) {
        case 0: page = PAGE_APPEARANCE; sGrid = 0; draw(); break;
        case 1: page = PAGE_WEATHER;    sGrid = 0; draw(); break;
        case 2: page = PAGE_TIME;       sGrid = 0; draw(); break;
        case 3: page = PAGE_WIFI;       sGrid = 1; draw();
              // No scan on entry: it runs from the SCAN page (the
              // SCAN NETWORKS row or the RESCAN row) — on demand only.
              break;
        case 4: page = PAGE_STATIONS;   sGrid = 2;
              draw();                   // the lookup starts from the SCAN button
              break;
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
      if (r < 0 || r > 3) return;
      if (r == 0) return;   // Row 0 is display-only: no action, no redraw.
      sGrid = r;
      if (r == 1) {
        editLocation();
        break;
      }
      editCoord(r == 2, x);
      break;
    case PAGE_POSTAL:
      if (postalPhase != PP_RESULTS) return;   // searching / message: no rows
      if (r < 0 || r >= GEO_PER_PAGE) return;
      {
        int idx = geoPage * GEO_PER_PAGE + r;
        if (idx < geoCount) applyLocation(idx);
      }
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
      // Compact rows 1..4 (row 0 is display-only status).
      r = wifiRowAt(y);
      if (r < 0 || r > 4) return;
      sGrid = r;
      switch (r) {
        case 1:
          // The radio is off by the user's choice: a scan must not silently
          // turn it back on (a join must — explicit intent to connect).
          if (!wifiEnabled()) {
            setStatus("TURN WIFI ON");
            draw();
            break;
          }
          page = PAGE_SCAN;
          scanNetworks();
          break;
        case 2: addNet();             break;
        case 3: toggleRadio();        break;
        case 4: restoreDefault();     break;
      }
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
    case PAGE_STATIONS:
      // Info page: only the SCAN button (row slot 2) acts; the source line
      // and the selected-station lines are display-only.
      if (r != 2) return;
      sGrid = 2;
      page = PAGE_STN_SCAN;
      sGrid = 0;
      stnDiscover();   // runs the whole lookup, draws itself
      break;
    case PAGE_STN_SCAN:
      if (r == STN_PER_PAGE) {         // REFRESH row
        sGrid = r;
        stnDiscover();
        return;
      }
      if (stnPhase != STN_RESULTS) return;   // searching / message: no rows
      if (r < 0 || r >= STN_PER_PAGE) return;
      {
        const int idx = stnPage * STN_PER_PAGE + r;
        if (idx < stnCount) stnSelect(idx);   // persist + back to STATIONS page
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
  geoCount = 0;
  geoPage = 0;
  stnReset();
  resetTarget = RT_NONE;
  wifiMsg[0] = 0;
  if (!prefs.getString("ssid", savedSsid, sizeof(savedSsid)))
    savedSsid[0] = 0;
  draw();
}

bool settingsActive() { return sActive; }

void settingsTouch(int16_t x, int16_t y) { handleTap(x, y); }
