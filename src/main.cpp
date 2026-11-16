// ============================================================================
//  CYD Weather Station Dashboard
//  Board : ESP32-2432S032C-I (CYD) — ST7789 320x240 + GT911 capacitive touch
//  Stack : PlatformIO / Arduino / LovyanGFX / ArduinoJson
//
//  Features:
//   - Large digital clock (H:MM AM/PM, US style), full date line
//   - Current temperature, weather description, feels-like, humidity
//   - Rain status: RAIN NOW (+ forecast end time) / RAIN NEARBY / next rain period
//   - Today's high/low, last-update indicator
//   - Open-Meteo API (temps/humidity/high-low + NOAA RRFS 15-min precip via
//     minutely_15) + KJRB/KLGA/KEWR METAR live obs for current conditions;
//     refreshed every 15 minutes
//   - NTP time sync, automatic WiFi reconnection
//   - Weather-outfit dog image in the bottom-right corner (one of 17 PNGs on
//     the SD card, selected by feels-like temp + condition; see selectDogImage())
//   - Four display themes (BASIC / SKY / SPLIT / BENTO), each with
//     automatic day/night variants; selection persisted in NVS
//   - Touch targets: clock = 12h/24h toggle, date line = format cycle,
//     temperature = F/C toggle, dog corner zone = THEME picker (swipe left/right
//     to browse the 4 display themes, tap to select; choice persists in NVS),
//     anywhere else = force a weather refresh
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LovyanGFX.hpp>
#include <esp_heap_caps.h>   // heap_caps_malloc(MALLOC_CAP_DMA) for sprite buffers
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>
#include "dog_image.h"
#include "nunito_bff.h"   // Nunito BFF font blobs for theme 1 (tools/make_nunito_bff.py)

// ---------------------------------------------------------------------------
//  Pin map (confirmed from the board's ESP-IDF BSP)
// ---------------------------------------------------------------------------
#define TFT_CS        15
#define TFT_SCK       14
#define TFT_MOSI      13
#define TFT_DC         2
#define TFT_RST       12
#define TFT_BL        27

#define TOUCH_SDA     33
#define TOUCH_SCL     32
#define TOUCH_INT    -1   // INT is not wired on this board -> polling mode
#define TOUCH_RST     25

// ---------------------------------------------------------------------------
//  SD card slot (from the CYD MCU schematic ESP32-2432S032-MCU-V1.0):
//    TF_CS = GPIO5   TF_CLK = GPIO18   MCU_MISO = GPIO19   MCU_MOSI = GPIO23
//  The TFT owns VSPI (SPI3); the SD card gets a dedicated HSPI (SPI2) instance
//  so the two buses never share hardware.
// ---------------------------------------------------------------------------
#define SD_CS         5
#define SD_SCK        18
#define SD_MISO       19
#define SD_MOSI       23

// ---------------------------------------------------------------------------
//  Display orientation
//  The glass is natively PORTRAIT (RAM: 240 cols x 320 rows) but mounted in
//  landscape on the board. Rotation 1 yields a full-screen 320x240 landscape
//  canvas; it is rotation 3 flipped by 180 degrees (rot + 2 = upside down),
//  matching how the CYD sits on the desk.
// ---------------------------------------------------------------------------
#define TFT_ROTATION  1

// ---------------------------------------------------------------------------
//  WiFi credentials
//  Kept out of the public repo: real values live in src/wifi_config.h, which
//  is gitignored. On a fresh clone, copy src/wifi_config.h.example to
//  src/wifi_config.h and fill it in.
// ---------------------------------------------------------------------------
#include "wifi_config.h"

// ---------------------------------------------------------------------------
//  Location & timezone (edit to match your site)
// ---------------------------------------------------------------------------
static const float LOC_LAT = 40.7517;    // ZIP 10017, New York City (Manhattan)
static const float LOC_LON = -73.9707;

// The toolchain's tzset() parser is unreliable for TZ strings with DST rules,
// so the system clock is kept on UTC and the local offset is computed in
// software instead.
//
// US Eastern Time (America/New_York):
//   EST = UTC-5  (standard time)
//   EDT = UTC-4  (daylight saving time, since 2007: starts 02:00 local on the
//                second Sunday of March, ends 02:00 local on the first Sunday
//                of November)
static const char* NTP_TZ = "UTC";

// Day of week (0=Sunday .. 6=Saturday) – Tomohiko Ito's algorithm.
static int dayOfWeek(int y, int m, int d) {
  static const int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
  if (m < 3) y--;
  return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
}

// Days since 1970-01-01 for a civil date – Howard Hinnant's algorithm.
static long daysFromCivil(int y, int m, int d) {
  y -= (m <= 2);
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);                          // [0,399]
  unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)d - 1u; // [0,365]
  unsigned doe = yoe * 365u + yoe/4u - yoe/100u + doy;               // [0,146096]
  return era * 146097L + (long)doe - 719468;
}

static long makeUtcTime(int y, int mo, int d, int h, int mi, int s) {
  return daysFromCivil(y, mo, d) * 86400L + h * 3600L + mi * 60L + s;
}

// Current US Eastern offset in seconds (negative = west of UTC).
static long localOffsetSec(time_t utc) {
  struct tm t;
  gmtime_r(&utc, &t);
  int y = t.tm_year + 1900;

  // DST starts: second Sunday of March at 02:00 EST (= 07:00 UTC)
  int marFirstSun = 1 + ((7 - dayOfWeek(y, 3, 1)) % 7);
  time_t dstStartUtc = makeUtcTime(y, 3, marFirstSun + 7, 7, 0, 0);

  // DST ends: first Sunday of November at 02:00 EDT (= 06:00 UTC)
  int novFirstSun = 1 + ((7 - dayOfWeek(y, 11, 1)) % 7);
  time_t dstEndUtc   = makeUtcTime(y, 11, novFirstSun, 6, 0, 0);

  return (utc >= dstStartUtc && utc < dstEndUtc) ? -4L * 3600 : -5L * 3600;
}

// ---------------------------------------------------------------------------
//  Refresh cadence
// ---------------------------------------------------------------------------
static const uint32_t WEATHER_REFRESH_MS = 15UL * 60UL * 1000UL;   // 15 min
static const uint32_t WIFI_RETRY_MS      = 10UL * 1000UL;           // 10 s
static const uint32_t WX_MIN_RETRY_MS    = 30UL * 1000UL;           // min gap between fetch attempts
static const uint32_t NTP_BOOT_WAIT_MS   = 10UL * 1000UL;           // max wait for time before initial fetch

// ---------------------------------------------------------------------------
//  Live observation sources (METAR via NOAA Aviation Weather Center)
//  KJRB (Wall Street / Manhattan) is primary; KLGA and KEWR are nearby airports
//  used for "RAIN NEARBY" when KJRB itself reports dry.
// ---------------------------------------------------------------------------
static const char*    METAR_URL         = "https://aviationweather.gov/api/data/metar?ids=KJRB,KLGA,KEWR&format=json";
static const uint32_t METAR_MAX_AGE_S   = 90UL * 60;                // obs older than this -> fall back to model data

// HTTPS protection: bound every phase of a fetch so a stalled server can never
// freeze the loop. (Core defaults are connect 30 s and TLS handshake 120 s!)
static const uint16_t        HTTP_TIMEOUT_MS   = 5000;              // TCP connect + socket read (SO_RCVTIMEO)
static const unsigned long   HTTP_HANDSHAKE_S  = 10;                // TLS handshake

// Rain thresholds, inches. Open-Meteo quantizes RRFS output to ~0.004 in steps
// (~0.1 mm); the 15-min threshold sits above that floor so trace amounts are
// skipped, and a period needs >= RAIN_MIN_WET_STEPS wet steps before it is
// announced (an isolated single-step spike is model noise). The hourly
// threshold likewise skips trace amounts (drizzle / model noise):
static const float RAIN_15MIN_THRESH_IN  = 0.01f;   // per 15-min step (~0.25 mm, ~1 mm/hr)
static const float RAIN_HOURLY_THRESH_IN = 0.02f;   // per hour    (~0.5 mm/hr)
static const int   RAIN_MAX_DRY_STEPS    = 2;       // bridge <=30 min lulls inside one rain event
static const int   RAIN_MIN_WET_STEPS    = 2;       // min wet steps for a period to be announced

// NBM cross-check (independent NOAA blend, hourly trailing sums in inches):
// if RRFS declares the current rain period ends at E but NBM still shows >=
// this amount in any of the next RAIN_NBM_CHECK_HOURS hours after E, the
// definitive end time is suppressed and "RAIN NOW - MAY CONTINUE" is shown.
static const float RAIN_NBM_HOURLY_THRESH_IN = 0.004f;   // per hour (~0.1 mm/hr)
static const int   RAIN_NBM_CHECK_HOURS      = 6;        // hours past the RRFS end to check

// ---------------------------------------------------------------------------
//  Custom LGFX device for the CYD board
// ---------------------------------------------------------------------------
class CYDDisplay : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789  _panel_instance;
  lgfx::Bus_SPI       _bus_instance;
  lgfx::Light_PWM     _light_instance;
  lgfx::Touch_GT911   _touch_instance;

public:
  CYDDisplay() {
    // ---- Panel (ST7789) ----
    // The glass is natively portrait: RAM is 240 columns x 320 rows.
    // panel_* = native glass size; setRotation(TFT_ROTATION) swaps them into
    // the landscape 320x240 drawing canvas (see TFT_ROTATION note above).
    auto cfg = _panel_instance.config();
    cfg.pin_cs          = TFT_CS;
    cfg.pin_rst         = TFT_RST;
    cfg.panel_width     = 240;   // native glass width
    cfg.panel_height    = 320;   // native glass height
    cfg.memory_width    = 240;   // RAM columns
    cfg.memory_height   = 320;   // RAM rows
    cfg.readable        = false; // THIS BOARD: the TFT's MISO is not wired (only the
                                 // SD card has MISO, GPIO19). LGFX defaults to
                                 // readable=true, which makes the BFF font renderer
                                 // take its "readable alpha" path: it calls
                                 // readRectRGB() per glyph row and blends edge pixels
                                 // against whatever garbage comes back on the unconnected
                                 // MISO line - corrupting every anti-aliased text pixel
                                 // (blurry edges + pale-coloured patches). With
                                 // readable=false the renderer instead blends against
                                 // getBaseColor(), which bffText()/bffCenter() set to
                                 // the exact background behind each text run.
    _panel_instance.config(cfg);

    // ---- SPI bus (attached to the panel) ----
    auto bcfg = _bus_instance.config();
    bcfg.spi_host       = VSPI_HOST;
    bcfg.freq_write     = 40000000;      // 40 MHz
    bcfg.pin_sclk       = TFT_SCK;
    bcfg.pin_mosi       = TFT_MOSI;
    bcfg.pin_dc         = TFT_DC;
    _bus_instance.config(bcfg);
    _panel_instance.setBus(&_bus_instance);

    // ---- Backlight (PWM). If the screen stays dark, set invert = true. ----
    auto lcfg = _light_instance.config();
    lcfg.pin_bl          = TFT_BL;
    lcfg.freq           = 5000;
    lcfg.invert         = false;
    _light_instance.config(lcfg);
    _panel_instance.setLight(&_light_instance);

    // ---- Touch (GT911 over I2C, polling – INT pin not wired) ----
    auto tcfg = _touch_instance.config();
    tcfg.i2c_port        = 0;            // I2C_NUM_0
    tcfg.pin_sda         = TOUCH_SDA;
    tcfg.pin_scl         = TOUCH_SCL;
    tcfg.pin_int         = TOUCH_INT;    // -1 => polling mode
    tcfg.pin_rst         = TOUCH_RST;
    tcfg.i2c_addr        = 0x14;         // default GT911 address (driver also tries 0x5D)
    // Raw GT911 coordinates are in the glass's native portrait space
    // (x: 0..239, y: 0..319). LGFX's convertRawXY() applies the same
    // rotation as the display automatically, so getTouch() returns
    // landscape canvas coordinates – no manual mapping needed.
    tcfg.x_min           = 0;
    tcfg.x_max           = 240;
    tcfg.y_min           = 0;
    tcfg.y_max           = 320;
    _touch_instance.config(tcfg);
    _panel_instance.setTouch(&_touch_instance);

    setPanel(&_panel_instance);
  }
};

CYDDisplay tft;

// ---------------------------------------------------------------------------
//  Weather data model
// ---------------------------------------------------------------------------
struct WeatherData {
  bool     valid        = false;
  float    temp         = NAN;   // current °F (API returns fahrenheit)
  float    feelsLike    = NAN;   // apparent temperature °F
  int      humidity     = -1;    // %
  float    tMax         = NAN;   // today high °F
  float    tMin         = NAN;   // today low °F
  String   description  = "";
  bool     isDay        = true;  // Open-Meteo current.is_day (1 = daytime)
  bool     isDayKnown   = false; // true once a usable is_day value has been received
  int      wmoCode      = -1;    // Open-Meteo current.weather_code (-1 if missing)
  bool     sunny        = false; // known daytime + clear/mainly-clear (drives "_sun" outfits)
  bool     rainingNow   = false; // currently raining at KJRB (METAR precip codes, Open-Meteo fallback)
  float    rainNowIn    = NAN;   // current-hour rain amount, inches (model)
  char     rainEnd[16] = "";     // "H:MM AM/PM" end of the current wet period per RRFS ("" = unknown)
  bool     rainMayContinue = false; // NBM cross-check: precip may continue past the RRFS end time
  bool     rainNearby   = false; // fresh precip at KLGA/KEWR while KJRB is not reporting rain now
  char     nearbyStns[8] = "";   // which stations report: "LGA", "EWR" or "LGA/EWR"
  char     nextRain[16] = "";    // "H:MM AM/PM" local time of the next rain period ("" = none in window)
  bool     nextRainTomorrow = false;
  bool     nextRainFrom15min = false;   // nextRain came from RRFS 15-min data (vs hourly model)
  uint32_t lastUpdateMs = 0;     // millis() of last successful fetch (refresh cadence)
  time_t   lastUpdateEpoch = 0;  // UTC epoch of last successful fetch (displayed)
};

static WeatherData wx;
static bool ntpSynced = false;

// ---------------------------------------------------------------------------
//  User-selectable display options (toggled by tapping the screen; runtime
//  state only - not persisted across reboot)
// ---------------------------------------------------------------------------
static bool use24Hour      = false;   // tap clock: US 12-hour <-> 24-hour
static int  dateFormat     = 0;       // tap date line: 0 = "Thursday 12 November 2026", 1 = "Thursday November 12, 2026"
static bool useCelsius     = false;   // tap temperature: F <-> C (all displayed temps)
// THEME picker: shown when the dog image area is tapped. A half-scale preview
// of a theme sits in a yellow-bordered window between two green bars (name on
// top, prev/next names + swipe arrows below). Swipe left/right to change the
// previewed theme; tap anywhere to select it and return to the main display.
static bool     themeSelectScreen = false;  // true while the THEME picker is shown
static int      activeTheme  = 0;           // displayed theme (persisted in NVS)
static int      previewTheme = 0;           // theme shown in the picker's window

// ---------------------------------------------------------------------------
//  WMO weather-code → short human-readable text
// ---------------------------------------------------------------------------
static const char* wmoText(int code) {
  switch (code) {
    case 0:  return "Clear sky";
    case 1:  return "Mainly clear";
    case 2:  return "Partly cloudy";
    case 3:  return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Moderate rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Moderate snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: return "Rain showers";
    case 81: return "Mod. rain showers";
    case 82: return "Violent rain shw.";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Thunderstorm hail";
    default: return "Unknown";
  }
}

// WMO codes that mean liquid precipitation (drizzle / rain / showers).
static bool wmoIsRain(int code) {
  switch (code) {
    case 51: case 53: case 55:   // drizzle
    case 56: case 57:            // freezing drizzle
    case 61: case 63: case 65:   // rain
    case 66: case 67:            // freezing rain
    case 80: case 81: case 82:   // showers
      return true;
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
//  KJRB METAR parsing (live observation, aviationweather.gov)
//
//  Open-Meteo's "current" block is model-derived and can lag reality by up to
//  an hour, so current conditions + RAIN NOW come from the KJRB (Wall Street)
//  METAR instead. Temperature/humidity/high-low/forecast stay on Open-Meteo.
// ---------------------------------------------------------------------------

// True if a whitespace-separated METAR token is liquid precipitation:
// RA / -RA / +RA, SHRA, TSRA, DZ variants, FZRA/FZDZ (same semantics as wmoIsRain).
static bool metarTokenIsRain(const char* tok) {
  const char* p = tok;
  if (*p == '-' || *p == '+') p++;        // intensity prefix
  if (strncmp(p, "VC", 2) == 0) p += 2;   // vicinity prefix
  return strcmp(p, "RA") == 0    || strcmp(p, "DZ") == 0   ||
         strcmp(p, "FZRA") == 0  || strcmp(p, "FZDZ") == 0 ||
         strcmp(p, "SHRA") == 0  || strcmp(p, "TSRA") == 0;
}

static bool metarHasRain(const char* raw) {
  if (!raw || !*raw) return false;
  const char* s = raw;
  while (*s) {
    while (*s == ' ') s++;
    if (!*s) break;
    const char* e = s;
    while (*e && *e != ' ') e++;
    size_t len = (size_t)(e - s);
    if (len < 12) {                       // all precip groups are short
      char tok[12];
      memcpy(tok, s, len);
      tok[len] = '\0';
      if (metarTokenIsRain(tok)) return true;
    }
    s = e;
  }
  return false;
}

// Short display text from a raw METAR string (phrasing mirrors wmoText()).
static String metarText(const char* raw) {
  if (!raw || !*raw) return "Unknown";
  // Pass 1: precipitation / fog groups take priority over sky cover.
  const char* s = raw;
  while (*s) {
    while (*s == ' ') s++;
    if (!*s) break;
    const char* e = s;
    while (*e && *e != ' ') e++;
    size_t len = (size_t)(e - s);
    if (len < 12) {
      char tok[12];
      memcpy(tok, s, len);
      tok[len] = '\0';
      const char* p = tok;
      if (*p == '-' || *p == '+') p++;
      if (strncmp(p, "VC", 2) == 0) p += 2;
      if (strcmp(p, "TSRA") == 0) return "Thunderstorms";
      if (strcmp(p, "SHRA") == 0) return "Rain showers";
      if (strcmp(p, "FZRA") == 0 || strcmp(p, "FZDZ") == 0) return "Freezing rain";
      if (strcmp(p, "RA") == 0) {
        if (*tok == '-') return "Light rain";
        if (*tok == '+') return "Heavy rain";
        return "Rain";
      }
      if (strcmp(p, "DZ") == 0) return "Drizzle";
      if (strcmp(p, "FG") == 0 || strcmp(p, "BR") == 0 ||
          strcmp(p, "UP") == 0 || strcmp(p, "PU") == 0) return "Fog";
    }
    s = e;
  }
  // Pass 2: sky cover (densest group wins).
  int cover = 0;   // 0 clear .. 3 overcast
  s = raw;
  while (*s) {
    while (*s == ' ') s++;
    if (!*s) break;
    const char* e = s;
    while (*e && *e != ' ') e++;
    size_t len = (size_t)(e - s);
    if (len >= 3 && len <= 6) {
      char tok[12];
      memcpy(tok, s, len);
      tok[len] = '\0';
      int c = -1;
      if (strcmp(tok, "SKC") == 0 || strcmp(tok, "CLR") == 0) c = 0;
      else if (strncmp(tok, "FEW", 3) == 0 || strncmp(tok, "SCT", 3) == 0) c = 1;
      else if (strncmp(tok, "BKN", 3) == 0) c = 2;
      else if (strncmp(tok, "OVC", 3) == 0) c = 3;
      if (c > cover) cover = c;
    }
    s = e;
  }
  switch (cover) {
    case 1: return "Partly cloudy";
    case 2: return "Mostly cloudy";
    case 3: return "Overcast";
    default: return "Clear";
  }
}

// Sky cover from a raw METAR string for the dog's "sunny" determination:
// 0 = clear (SKC/CLR), 1 = mainly clear (FEW), >=2 = too cloudy, -1 = no sky group.
static int metarSkyCover(const char* raw) {
  if (!raw || !*raw) return -1;
  int cover = -1;   // densest seen (-1 = none yet)
  const char* s = raw;
  while (*s) {
    while (*s == ' ') s++;
    if (!*s) break;
    const char* e = s;
    while (*e && *e != ' ') e++;
    size_t len = (size_t)(e - s);
    if (len >= 3 && len <= 6) {
      int c = -1;
      if (strcmp(s, "SKC") == 0 || strcmp(s, "CLR") == 0) c = 0;
      else if (strncmp(s, "FEW", 3) == 0) c = 1;   // SCT/BKN/OVC all count as cloudy
      else if (strncmp(s, "SCT", 3) == 0 || strncmp(s, "BKN", 3) == 0 ||
               strncmp(s, "OVC", 3) == 0) c = 2;
      if (c >= 0 && (cover < 0 || c > cover)) cover = c;
    }
    s = e;
  }
  return cover;
}

// ---------------------------------------------------------------------------
//  US-style time formatting: "3:47 PM" / "12:05:09 AM"
//  (strftime %I zero-pads the hour; US style drops the leading zero)
// ---------------------------------------------------------------------------
static void formatUS12(char* out, size_t n, const struct tm* lt, bool withSeconds) {
  if (withSeconds) strftime(out, n, "%I:%M:%S %p", lt);
  else             strftime(out, n, "%I:%M %p", lt);
  if (out[0] == '0') memmove(out, out + 1, strlen(out));
}

// Clock string in the currently selected mode (US 12-hour or 24-hour).
static void formatClock(char* out, size_t n, const struct tm* lt) {
  if (use24Hour) snprintf(out, n, "%02d:%02d", lt->tm_hour, lt->tm_min);
  else           formatUS12(out, n, lt, false);
}

// Widest possible clock string in the current mode (partial-update box size).
static const char* clockMaxString() { return use24Hour ? "23:59" : "12:59 PM"; }

// Last-update time in the currently selected clock mode (US 12h or 24h), so the
// "Updated ..." / "UPD ..." lines follow the same tap toggle as the main clock.
static void formatUpdTime(char* out, size_t n, const struct tm* lt, bool withSeconds) {
  if (use24Hour) snprintf(out, n, withSeconds ? "%02d:%02d:%02d" : "%02d:%02d",
                          lt->tm_hour, lt->tm_min, lt->tm_sec);
  else           formatUS12(out, n, lt, withSeconds);
}

// Date line in the currently selected format. Format 0 keeps the original
// "Thursday 12 November 2026"; format 1 is US-style with the weekday kept in
// place, "Thursday November 12, 2026". Longest case ("Wednesday September 30,
// 2026") is 336 px at size 2 – dateLineFit() scales such lines down slightly.
static void formatDateLine(char* out, size_t n, const struct tm* lt) {
  if (dateFormat == 0) { strftime(out, n, "%A %d %B %Y", lt); return; }
  strftime(out, n, "%A %B %d, %Y", lt);
}

// Temperature conversion for display only (wx values stay in °F internally).
static float tempForDisplay(float f) { return useCelsius ? (f - 32.0f) * 5.0f / 9.0f : f; }
static const char* tempUnitChar()    { return useCelsius ? "C" : "F"; }

// ---------------------------------------------------------------------------
//  NTP (configTzTime is provided by the ESP32 Arduino core itself)
// ---------------------------------------------------------------------------
static void configureTime() {
  configTzTime(NTP_TZ, "pool.ntp.org", "time.google.com", "time.windows.com");
}

// The core has no timeStatus(); treat a post-2023 epoch value as "synced".
static bool ntpIsSynced() {
  return time(nullptr) > 1700000000UL;
}

// Break the current local time (UTC + DST-aware offset) into a struct tm.
static bool nowLocal(struct tm* out) {
  if (!ntpIsSynced()) return false;
  time_t utc   = time(nullptr);
  time_t local = utc + localOffsetSec(utc);
  localtime_r(&local, out);   // TZ is UTC, so this just decomposes the epoch
  return true;
}

// ---------------------------------------------------------------------------
//  WiFi – connect / reconnect
// ---------------------------------------------------------------------------
static bool wifiEnsureConnected() {
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.printf("[WIFI] Connecting to %s ...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WIFI] Connected. IP = %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("[WIFI] Connection failed.");
  return false;
}

// ---------------------------------------------------------------------------
//  Rain timeline: NOAA RRFS 15-min precip (Open-Meteo "minutely_15") for the
//  near term, extended by the hourly model rain beyond that window.
//
//  A step stamped T covers the interval (T - 15 min, T] ("preceding 15 minutes
//  sum"). Wet = >= RAIN_15MIN_THRESH_IN. Consecutive wet steps form a period;
//  runs separated by <= RAIN_MAX_DRY_STEPS dry steps are bridged into one
//  period (a short lull does not end the event). Periods with fewer than
//  RAIN_MIN_WET_STEPS wet steps are ignored (isolated spikes = model noise).
//  Period onset = first wet
//  stamp - 15 min, cessation = last wet stamp. An end time is only reported
//  when the data actually shows a subsequent dry stretch -- if rain runs to
//  the edge of the forecast window the end stays unknown (""). Hourly values are
//  trailing sums over (T - 60 min, T]; their onset is reported at the start of
//  that interval (T - 60 min).
// ---------------------------------------------------------------------------

struct StepTime { int y, mo, d, hh, mm; };   // local wall clock from "YYYY-MM-DDTHH:MM"

static bool parseStepTime(const char* ts, StepTime& out) {
  if (!ts || strlen(ts) < 16) return false;
  out.y = atoi(ts); out.mo = atoi(ts + 5); out.d = atoi(ts + 8);
  out.hh = atoi(ts + 11); out.mm = atoi(ts + 14);
  return (out.y >= 2020 && out.mo >= 1 && out.mo <= 12 && out.d >= 1 && out.d <= 31
          && out.hh <= 23 && out.mm <= 59);
}

static bool stepBefore(const StepTime& a, const StepTime& b) {   // strict <
  if (a.y != b.y) return a.y < b.y;
  if (a.mo != b.mo) return a.mo < b.mo;
  if (a.d != b.d) return a.d < b.d;
  if (a.hh != b.hh) return a.hh < b.hh;
  return a.mm < b.mm;
}

static bool stepLE(const StepTime& a, const StepTime& b) { return !stepBefore(b, a); }

// t -= 15 minutes, with day/month/year borrow for the rare midnight crossing.
static void stepMinus15min(StepTime& t) {
  if (t.mm >= 15) { t.mm -= 15; return; }
  t.mm += 45;
  if (t.hh > 0) { t.hh--; return; }
  t.hh = 23;
  if (t.d > 1) { t.d--; return; }
  static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (--t.mo < 1) { t.mo = 12; t.y--; }
  t.d = mdays[t.mo - 1];
  if (t.mo == 2 && ((t.y % 4 == 0 && t.y % 100 != 0) || t.y % 400 == 0)) t.d = 29;
}

static void formatStepUS12(const StepTime& t, char* buf, size_t n) {   // "H:MM AM/PM"
  int h = t.hh % 12; if (h == 0) h = 12;
  snprintf(buf, n, "%d:%02d %s", h, t.mm, (t.hh < 12) ? "AM" : "PM");
}

struct RainTimeline {
  bool have15min = false;      // got a usable minutely_15 series
  char rainEnd[16] = "";       // cessation of the period containing now ("")
  StepTime endTime{};          // machine-readable form of rainEnd (for the NBM cross-check)
  bool hasEndTime = false;
  char nextRain[16] = "";      // onset of the next period ("")
  bool nextTomorrow = false;   // onset falls on another local day than now
  bool nextFrom15min = false;  // source: RRFS 15-min (true) vs hourly model (false)
};

static void analyzeRainTimeline(JsonDocument& doc, const struct tm& lt, RainTimeline& out) {
  StepTime now{ lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min };

  JsonVariant vT = doc["minutely_15"]["time"];
  JsonVariant vP = doc["minutely_15"]["precipitation"];
  size_t n15 = 0;
  if (vT.is<JsonArray>() && vP.is<JsonArray>()) {
    size_t a = vT.size(), b = vP.size();
    n15 = (a < b) ? a : b;
  }
  out.have15min = (n15 > 4);   // need a few steps for this to mean anything

  StepTime after = now;        // hourly scan starts strictly after this time
  if (out.have15min) {
    const JsonArray tA = vT.as<JsonArray>();
    const JsonArray pA = vP.as<JsonArray>();

    int startIdx = -1, lastWetIdx = -1;   // open period
    int wetCount = 0;                     // wet steps in the open period
    int dryRun = 0;                       // consecutive sub-threshold steps inside it

    auto closePeriod = [&](int s, int e, bool closedByDry) {
      if (wetCount < RAIN_MIN_WET_STEPS) return;   // isolated spike: model noise
      StepTime ts, te;
      if (!parseStepTime(tA[s].as<const char*>(), ts) ||
          !parseStepTime(tA[e].as<const char*>(), te)) return;
      StepTime onset = ts;
      stepMinus15min(onset);
      // "Contains now" only when the data shows the rain actually stopping (te).
      if (closedByDry && stepLE(onset, now) && stepBefore(now, te)) {
        formatStepUS12(te, out.rainEnd, sizeof(out.rainEnd));
        out.endTime = te;          // machine-readable end time for the NBM cross-check
        out.hasEndTime = true;
      } else if (stepBefore(now, onset) && !out.nextRain[0]) {   // first future period wins
        formatStepUS12(onset, out.nextRain, sizeof(out.nextRain));
        out.nextTomorrow  = !(onset.y == now.y && onset.mo == now.mo && onset.d == now.d);
        out.nextFrom15min = true;
      }
    };

    for (size_t i = 0; i < n15; i++) {
      float p = (float)pA[i];
      bool wet = !pA[i].isNull() && !isnan(p) && p >= RAIN_15MIN_THRESH_IN;
      if (wet) {
        if (startIdx < 0) startIdx = (int)i;
        lastWetIdx = (int)i;
        wetCount++;
        dryRun = 0;
      } else if (startIdx >= 0 && ++dryRun > RAIN_MAX_DRY_STEPS) {
        closePeriod(startIdx, lastWetIdx, true);
        startIdx = -1;
        wetCount = 0;
        dryRun = 0;
      }
    }
    if (startIdx >= 0) closePeriod(startIdx, lastWetIdx, false);   // runs to end of data: no known end

    parseStepTime(tA[n15 - 1].as<const char*>(), after);   // hourly picks up beyond this
  }

  // Hourly extension: first hour with >= threshold strictly after `after`.
  JsonVariant hT = doc["hourly"]["time"];
  JsonVariant hR = doc["hourly"]["rain"];
  if (!out.nextRain[0] && hT.is<JsonArray>() && hR.is<JsonArray>()) {
    const JsonArray tH = hT.as<JsonArray>();
    const JsonArray rH = hR.as<JsonArray>();
    size_t nh = (tH.size() < rH.size()) ? tH.size() : rH.size();
    for (size_t i = 0; i < nh && !out.nextRain[0]; i++) {
      StepTime th;
      if (!parseStepTime(tH[i].as<const char*>(), th)) continue;
      if (!stepBefore(after, th)) continue;   // past, or already covered by the 15-min window
      float r = (float)rH[i];
      if (!rH[i].isNull() && !isnan(r) && r >= RAIN_HOURLY_THRESH_IN) {
        // Hourly value stamped T accumulates over (T - 60 min, T] (trailing sum);
        // report onset at the start of that interval.
        StepTime onset = th;
        if (onset.hh > 0) {
          onset.hh--;
        } else {
          onset.hh = 23;
          if (onset.d > 1) { onset.d--; }
          else {
            static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
            if (--onset.mo < 1) { onset.mo = 12; onset.y--; }
            onset.d = mdays[onset.mo - 1];
            if (onset.mo == 2 && ((onset.y % 4 == 0 && onset.y % 100 != 0) || onset.y % 400 == 0)) onset.d = 29;
          }
        }
        if (!stepBefore(now, onset)) continue;   // interval already started: METAR covers "now"
        formatStepUS12(onset, out.nextRain, sizeof(out.nextRain));
        out.nextTomorrow  = !(onset.y == now.y && onset.mo == now.mo && onset.d == now.d);
        out.nextFrom15min = false;
      }
    }
  }
}

// ---------------------------------------------------------------------------
//  NBM cross-check (second precipitation source, hourly only)
//
//  Independent NOAA blend product fetched from a SEPARATE request so the main
//  best-match RRFS response stays exactly what it is today. NBM's hourly value
//  stamped T is a trailing sum over (T-60 min, T], i.e. stamp E+1 h covers
//  exactly the hour after an RRFS end time E - directly comparable to RRFS.
//
//  If any of the next RAIN_NBM_CHECK_HOURS hourly values past the RRFS end is
//  >= RAIN_NBM_HOURLY_THRESH_IN, rain "may continue" and the definitive end
//  time is suppressed on display ("RAIN NOW - MAY CONTINUE"). No new end time
//  is ever derived from NBM.
//
//  Every failure path (no WiFi, HTTP error, timeout, bad/missing JSON) returns
//  false -> existing RRFS-only behavior is retained and the normal update is
//  unaffected. The JsonDocument is local to this function, so its heap memory
//  (~3-5 KB peak on a ~1.5 KB body) is released when it returns.
// ---------------------------------------------------------------------------

// t += h hours with day/month/year rollover (h < 24). Mirrors the month-length
// handling in stepMinus15min() without touching that function.
static void stepAddHours(StepTime& t, int h) {
  t.hh += h;
  while (t.hh >= 24) {
    t.hh -= 24;
    static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (++t.d > mdays[t.mo - 1]) {
      t.d = 1;
      if (++t.mo > 12) { t.mo = 1; t.y++; }
    }
  }
}

// Pre-resolve a host so that a subsequent HTTP error -1 can be attributed to
// TCP/TLS rather than DNS (DNS failures are reported separately by the caller).
static bool dnsResolve(const char* host) {
  IPAddress ip;
  return WiFi.hostByName(host, ip);
}

static bool nbmIndicatesContinuation(const StepTime& end) {
  WiFiClientSecure nClient;
  nClient.setInsecure();   // same self-contained tradeoff as the other clients
  nClient.setConnectionTimeout(HTTP_TIMEOUT_MS);
  nClient.setHandshakeTimeout(HTTP_HANDSHAKE_S);

  HTTPClient nHttp;
  String url = String("https://api.open-meteo.com/v1/forecast?latitude=")
             + String(LOC_LAT, 4) + "&longitude=" + String(LOC_LON, 4)
             + "&models=ncep_nbm_conus&hourly=precipitation"   // NBM hourly trailing sums (inches)
             + "&timezone=auto&forecast_days=2"
             + "&temperature_unit=fahrenheit&precipitation_unit=inch";
  if (!nHttp.begin(nClient, url)) {
    Serial.println("[NBM] http.begin failed");
    return false;
  }
  nHttp.addHeader("User-Agent", "CYD-WeatherStation/1.0");

  Serial.printf("[NBM] freeHeap=%lu\n", ESP.getFreeHeap());   // diagnostic: heap at request time
  if (!dnsResolve("api.open-meteo.com")) {
    Serial.println("[NBM] DNS failed");
    return false;
  }

  int code = nHttp.GET();
  String body = (code == HTTP_CODE_OK) ? nHttp.getString() : "";
  nHttp.end();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[NBM] HTTP error %d\n", code);
    return false;
  }

  JsonDocument nbmDoc;   // ~1.5 KB response; freed when this function returns
  ArduinoJson::DeserializationError err = ArduinoJson::deserializeJson(nbmDoc, body);
  if (err) {
    Serial.printf("[NBM] JSON parse error: %s\n", err.c_str());
    return false;
  }

  JsonVariant vT = nbmDoc["hourly"]["time"];          // flat root-level arrays (single model)
  JsonVariant vP = nbmDoc["hourly"]["precipitation"];
  if (!vT.is<JsonArray>() || !vP.is<JsonArray>()) {
    Serial.println("[NBM] missing hourly time/precipitation arrays");
    return false;
  }
  size_t n = vT.as<JsonArray>().size();
  if (n > vP.as<JsonArray>().size()) n = vP.as<JsonArray>().size();

  // Window: stamps strictly after the RRFS end, up to and including end + RAIN_NBM_CHECK_HOURS.
  StepTime winEnd = end;
  stepAddHours(winEnd, RAIN_NBM_CHECK_HOURS);

  bool mayContinue = false;
  for (size_t i = 0; i < n && !mayContinue; i++) {
    const char* ts = vT.as<JsonArray>()[i].as<const char*>();
    if (!ts) continue;
    StepTime st;
    if (!parseStepTime(ts, st)) continue;
    if (!stepBefore(end, st)) continue;        // must be strictly after the RRFS end time
    if (stepBefore(winEnd, st)) break;         // hourly stamps are ascending -> past window
    JsonVariant pv = vP.as<JsonArray>()[i];
    if (pv.isNull()) continue;
    float p = pv.as<float>();
    mayContinue = !isnan(p) && p >= RAIN_NBM_HOURLY_THRESH_IN;
  }

  Serial.printf("[NBM] %d h after RRFS end: %s\n", RAIN_NBM_CHECK_HOURS,
                mayContinue ? "MAY CONTINUE" : "dry");
  return mayContinue;
}

// ---------------------------------------------------------------------------
//  Open-Meteo fetch (HTTPS)
// ---------------------------------------------------------------------------
static bool fetchWeather() {
  if (!wifiEnsureConnected()) return false;

  Serial.printf("[WX] rssi=%d dBm freeHeap=%lu\n", WiFi.RSSI(), ESP.getFreeHeap());   // diagnostic

  // ---- Fetch JSON body over HTTPS ----
  // The TLS client is scoped to this block so its socket and mbedTLS context are
  // released before the NBM/METAR requests start (each of which opens its own).
  String body;
  {
    WiFiClientSecure secureClient;
    // NOTE: server certificate verification is disabled to keep the firmware
    // self-contained (no embedded CA bundle). The traffic is still encrypted.
    // For full verification, embed the ISRG Root X1 cert and call setCACert().
    secureClient.setInsecure();
    secureClient.setConnectionTimeout(HTTP_TIMEOUT_MS);   // TCP connect + socket read <= 5 s
    secureClient.setHandshakeTimeout(HTTP_HANDSHAKE_S);   // TLS handshake <= 10 s (core default: 120 s!)

    HTTPClient http;
    String url = String("https://api.open-meteo.com/v1/forecast?latitude=")
               + String(LOC_LAT, 4) + "&longitude=" + String(LOC_LON, 4)
               + "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,is_day,rain"
               + "&daily=temperature_2m_max,temperature_2m_min"
               + "&hourly=rain"                    // rain beyond the 15-min window (local wall-clock hours)
               + "&minutely_15=precipitation"      // NOAA RRFS 15-min precip sums, local time (~48 h)
               + "&timezone=auto&forecast_days=2"
               + "&temperature_unit=fahrenheit&precipitation_unit=inch";   // temps °F, rain in inches

    Serial.printf("[WX] GET %s\n", url.c_str());
    if (!dnsResolve("api.open-meteo.com")) {
      Serial.println("[WX] DNS failed");
      return false;
    }
    if (!http.begin(secureClient, url)) {
      Serial.println("[WX] http.begin failed");
      return false;
    }
    http.addHeader("User-Agent", "CYD-WeatherStation/1.0");

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
      Serial.printf("[WX] HTTP error %d\n", code);
      http.end();
      return false;
    }

    body = http.getString();
    http.end();
  }   // secureClient destroyed here: socket + TLS context freed before NBM/METAR

  // ---- Parse JSON ----
  JsonDocument doc;   // ArduinoJson v7
  ArduinoJson::DeserializationError err = ArduinoJson::deserializeJson(doc, body);
  if (err) {
    Serial.printf("[WX] JSON parse error: %s\n", err.c_str());
    return false;
  }

  float curTemp   = doc["current"]["temperature_2m"];
  float curFeels  = doc["current"]["apparent_temperature"];
  int   curHum    = doc["current"]["relative_humidity_2m"];
  int   wCode     = doc["current"]["weather_code"];
  float curRain   = doc["current"]["rain"];      // inches this hour (null if unavailable)
  // is_day arrives as a JSON NUMBER (0/1). `variant | true` would reject it:
  // ArduinoJson's bool check only accepts JSON boolean literals, so test the value.
  JsonVariant vIsDay = doc["current"]["is_day"];
  bool  isDayOk   = !vIsDay.isNull() && (vIsDay == 0 || vIsDay == 1);
  float dMax      = doc["daily"]["temperature_2m_max"][0];
  float dMin      = doc["daily"]["temperature_2m_min"][0];

  if (isnan(curTemp)) {
    Serial.println("[WX] Missing current temperature in response");
    return false;
  }

  wx.valid        = true;
  wx.temp         = curTemp;
  wx.feelsLike    = curFeels;
  wx.humidity     = curHum;
  wx.isDay        = isDayOk ? ((int)vIsDay != 0) : true;   // unknown -> assume day
  wx.isDayKnown   = isDayOk;
  wx.tMax         = dMax;
  wx.tMin         = dMin;
  wx.description  = wmoText(wCode);
  wx.wmoCode      = wCode;
  wx.lastUpdateMs = millis();
  wx.lastUpdateEpoch = time(nullptr);   // 0/garbage until NTP has synced

  // ---- Rain status: RRFS 15-min periods + hourly extension (needs NTP for "now") ----
  bool rainingNow = (!isnan(curRain) && curRain > 0.0f) || wmoIsRain(wCode);   // model fallback

  char nextRain[16] = {0};
  bool nextTomorrow = false;
  bool nextFrom15min = false;
  char rainEnd[16] = {0};
  bool rainMayContinue = false;   // NBM cross-check result (false on any NBM failure)
  if (ntpIsSynced()) {
    struct tm lt;
    nowLocal(&lt);
    RainTimeline tl;
    analyzeRainTimeline(doc, lt, tl);
    snprintf(nextRain, sizeof(nextRain), "%s", tl.nextRain);
    snprintf(rainEnd, sizeof(rainEnd), "%s", tl.rainEnd);
    nextTomorrow  = tl.nextTomorrow;
    nextFrom15min = tl.nextFrom15min;

    // doc/body are no longer needed: free them before the NBM TLS handshake.
    doc.clear();
    body = "";

    // NBM cross-check: only when RRFS declared a concrete end time for the
    // current period. A failed/missing NBM fetch leaves rainMayContinue false,
    // i.e. the existing RRFS-only behavior is retained (see nbmIndicatesContinuation).
    if (tl.hasEndTime) {
      rainMayContinue = nbmIndicatesContinuation(tl.endTime);
    }
  } else {
    Serial.println("[WX] rain timeline skipped (no NTP)");
    doc.clear();   // no longer needed; free before the METAR TLS handshake
    body = "";
  }

  // ---- METAR observations: KJRB (primary) + KLGA/KEWR for "RAIN NEARBY" ----
  bool metarOk = false;      // fresh KJRB obs parsed
  int  kjrbCover = -1;       // sky cover from a fresh KJRB obs (-1 = none)
  String metarDesc;
  bool kjrbRain = false, lgaRain = false, ewrRain = false;   // precip in fresh obs
  {
    WiFiClientSecure mClient;
    mClient.setInsecure();   // same self-contained tradeoff as the Open-Meteo client
    mClient.setConnectionTimeout(HTTP_TIMEOUT_MS);
    mClient.setHandshakeTimeout(HTTP_HANDSHAKE_S);
    HTTPClient mHttp;
    Serial.printf("[METAR] freeHeap=%lu\n", ESP.getFreeHeap());   // diagnostic: heap at request time
    if (!dnsResolve("aviationweather.gov")) {
      Serial.println("[METAR] DNS failed");
    } else if (mHttp.begin(mClient, METAR_URL)) {
      mHttp.addHeader("User-Agent", "CYD-WeatherStation/1.0");
      int mCode = mHttp.GET();
      String mBody = (mCode == HTTP_CODE_OK) ? mHttp.getString() : "";
      mHttp.end();
      if (mCode != HTTP_CODE_OK) {
        Serial.printf("[METAR] HTTP error %d\n", mCode);
      } else {
        JsonDocument mDoc;   // small response (~3 KB); top level is a JSON ARRAY
        ArduinoJson::DeserializationError mErr = ArduinoJson::deserializeJson(mDoc, mBody);
        if (mErr) {
          Serial.printf("[METAR] JSON parse error: %s\n", mErr.c_str());
        } else if (!mDoc.is<JsonArray>() || mDoc.as<JsonArray>().size() == 0) {
          Serial.println("[METAR] No observations in response");
        } else {
          int64_t nowEpoch = ntpIsSynced() ? (int64_t)time(nullptr) : 0;
          const JsonArray arr = mDoc.as<JsonArray>();
          for (size_t i = 0; i < arr.size(); i++) {
            JsonObject obs = arr[i];
            const char* id     = obs["icaoId"];              // "KJRB" / "KLGA" / "KEWR"
            const char* raw    = obs["rawOb"];               // full METAR/SPECI string
            int64_t   obsEpoch = (int64_t)obs["obsTime"];    // UTC epoch seconds
            if (!id || !raw || !*raw) continue;
            long age = (nowEpoch > 0 && obsEpoch > 1700000000LL) ? (long)(nowEpoch - obsEpoch) : -1;
            Serial.printf("[METAR] %s (%ld s old): %s\n", id, age, raw);
            bool fresh = age >= 0 && age <= (long)METAR_MAX_AGE_S;
            if (!fresh) continue;   // stale: never drives RAIN NOW / NEARBY
            bool rain = metarHasRain(raw);
            if (strcmp(id, "KJRB") == 0) {
              metarOk   = true;
              metarDesc = metarText(raw);
              kjrbRain  = rain;
              kjrbCover = metarSkyCover(raw);
            } else if (rain && strcmp(id, "KLGA") == 0) {
              lgaRain = true;
            } else if (rain && strcmp(id, "KEWR") == 0) {
              ewrRain = true;
            }
          }
        }
      }
    } else {
      Serial.println("[METAR] http.begin failed");
    }
  }

  if (metarOk) {
    wx.description = metarDesc;   // live observation replaces WMO model text
    rainingNow     = kjrbRain;    // never report RAIN NOW from a stale obs
  }

  // Sun determination for the dog outfit: a fresh KJRB sky group wins over the
  // WMO model code. Sunny = known daytime + clear (SKC/CLR) or mainly clear (FEW);
  // with no usable METAR sky group, fall back to WMO codes 0/1.
  if (isDayOk && wx.isDay) {
    int cover = kjrbCover;        // -1 when no fresh KJRB obs / no sky group
    wx.sunny = (cover >= 0) ? (cover <= 1) : ((wCode == 0 || wCode == 1));
  } else {
    wx.sunny = false;             // night or unknown day/night -> never "_sun"
  }

  // RAIN NEARBY: fresh precip at KLGA/KEWR while KJRB is not reporting rain now.
  bool rainNearby = !rainingNow && (lgaRain || ewrRain);

  wx.rainingNow   = rainingNow;
  wx.rainNowIn    = curRain;
  strncpy(wx.nextRain, nextRain, sizeof(wx.nextRain) - 1);
  wx.nextRain[sizeof(wx.nextRain) - 1] = '\0';
  wx.nextRainTomorrow = nextTomorrow;
  wx.nextRainFrom15min = nextFrom15min;
  strncpy(wx.rainEnd, rainEnd, sizeof(wx.rainEnd) - 1);
  wx.rainEnd[sizeof(wx.rainEnd) - 1] = '\0';
  wx.rainMayContinue = rainMayContinue;
  wx.rainNearby   = rainNearby;
  if (rainNearby) {
    snprintf(wx.nearbyStns, sizeof(wx.nearbyStns), "%s",
             (lgaRain && ewrRain) ? "LGA/EWR" : lgaRain ? "LGA" : "EWR");
  } else {
    wx.nearbyStns[0] = '\0';
  }

  Serial.printf("[WX] %.1f F feels %.1f F hum %d%% %s (H:%.1f L:%.1f) day=%s sun=%s rain=%s next=%s%s end=%s metar=%s\n",
                curTemp, curFeels, curHum, wx.description.c_str(), dMax, dMin,
                isDayOk ? (wx.isDay ? "1" : "0") : "?", wx.sunny ? "1" : "0",
                rainingNow ? "NOW" : "-",
                wx.nextRain[0] ? wx.nextRain : "-", nextFrom15min ? "(15m)" : "",
                rainEnd[0] ? rainEnd : "-", metarOk ? "ok" : "fallback");
  return true;
}

// ---------------------------------------------------------------------------
//  Background (day / night)
//
//  This ST7789 panel displays INVERTED: a value V written to the frame buffer
//  appears as ~V on the glass (that is why filling with TFT_BLACK looks white).
//  Day keeps the classic look (write black -> see white). Night draws a bright
//  grey vertical gradient; the values below are in CODE space, i.e. bit-inverted
//  from what you actually see:
//    top row    written ~0xE72C -> displays ≈ #E5E5E5 (near-white light grey)
//    bottom row written ~0xBEFF -> displays ≈ #BCBCBC (bright grey)
// ---------------------------------------------------------------------------
static bool nightMode() { return wx.valid && !wx.isDay; }

static uint16_t rgb565Gray(uint8_t v) {            // 0..255 -> RGB565 grey
  uint8_t r = (v * 31 + 127) / 255;
  uint8_t g = (v * 63 + 127) / 255;
  return (uint16_t)((r << 11) | (g << 5) | r);
}

static uint16_t nightBgColor(int y) {              // code-space colour for row y
  int t = (tft.height() > 1) ? (y * 255 / (tft.height() - 1)) : 0;
  uint8_t v = (uint8_t)(232 - (232 - 192) * t / 255);   // displayed grey 232 -> 192
  return ~rgb565Gray(v) & 0xFFFF;
}

static void fillBackground() {
  if (!nightMode()) { tft.fillScreen(TFT_BLACK); return; }
  for (int y = 0; y < tft.height(); y++)
    tft.fillRect(0, y, tft.width(), 1, nightBgColor(y));
}

// Repaint a region with the current background (solid or per-row gradient) so
// partial updates leave no seams.
static void clearRegion(int x, int y, int w, int h) {
  if (!nightMode()) { tft.fillRect(x, y, w, h, TFT_BLACK); return; }
  for (int i = 0; i < h; i++)
    tft.fillRect(x, y + i, w, 1, nightBgColor(y + i));
}

// ---------------------------------------------------------------------------
//  Dog outfit image (bottom-right corner)
//
//  One of 17 PNGs on the SD card, selected by current weather (see
//  selectDogImage()). Decoded in src/dog_image.cpp; drawn once per full render.
//  The per-second partial updates (current layout) only touch rows [16,70), so
//  they never disturb it. Every line of the weather block is centered within
//  [0, textZoneRight()) to keep clear of the image.
// ---------------------------------------------------------------------------
static int textZoneRight() { return dogLeft(tft.width()); }

// NOTE: the actual dog blitting (drawDogBox / drawDogImage) lives in the theme
// system section below - it needs per-theme background colors for compositing.

// ---------------------------------------------------------------------------
//  Outfit selection (chart in README.md)
//
//  Rain overrides every other condition: raining now -> rain_now.png; otherwise
//  any expected rain (nextRain set) or RAIN NEARBY -> rain_later.png. Otherwise
//  the band follows the FEELS-LIKE temperature and "_sun" variants are used for
//  clear / mainly-clear daytime (wx.sunny). No usable weather data -> error.png
//  so a broken state is visible on screen instead of a stale outfit.
// ---------------------------------------------------------------------------
static const char* selectDogImage() {
  if (!wx.valid || isnan(wx.feelsLike)) return "error.png";

  if (wx.rainingNow)                          return "rain_now.png";
  if (wx.nextRain[0] != '\0' || wx.rainNearby) return "rain_later.png";

  float f = wx.feelsLike;   // °F
  const char* band;
  if (f < 30.0f)      band = "extreme_cold";   // below 30 F
  else if (f < 46.0f) band = "very_cold";      // 31-45 F
  else if (f < 56.0f) band = "cold";           // 46-55 F
  else if (f < 66.0f) band = "cool";           // 56-65 F
  else if (f < 71.0f) band = "mild_cool";      // 66-70 F
  else if (f < 76.0f) band = "mild_warm";      // 71-75 F
  else                band = "hot";            // 76 F and above

  static char name[24];
  snprintf(name, sizeof(name), "%s%s.png", band, wx.sunny ? "_sun" : "");
  return name;
}

// Load the outfit for the current weather state if it changed since the last
// load. Retries like at boot (marginal SD contact); on failure falls back to
// error.png so a bad state is visible instead of silently showing an old dog.
static bool sdCardReady = false;   // set in setup() once SD.begin() succeeds

static void refreshDogImage() {
  if (!sdCardReady) return;
  const char* want = selectDogImage();
  if (strcmp(want, dogCurrentName()) == 0) return;   // already showing it

  bool ok = false;
  for (int attempt = 1; attempt <= 3 && !ok; attempt++) {
    if (attempt > 1) delay(200);   // transient read error - try again
    ok = loadDogImage(want);
  }
  if (!ok && strcmp(want, "error.png") != 0) {
    Serial.printf("[DOG] %s failed to load - falling back to error.png\n", want);
    for (int attempt = 1; attempt <= 3 && !ok; attempt++) {
      if (attempt > 1) delay(200);
      ok = loadDogImage("error.png");
    }
  }
}

// ---------------------------------------------------------------------------
//  Main-screen layout – single source of truth for item positions/sizes.
//
//  Every draw function AND every touch hit zone reads from gLayout, so a new
//  layout added later (items resized/moved) only needs different values here:
//  assign them to gLayout and call renderScreen() – rendering, the per-second
//  partial updates and the tap areas all follow automatically. The defaults
//  below are the current layout; they match the original hardcoded coordinates
//  exactly (no visual change).
// ---------------------------------------------------------------------------
static inline int glyphH(int size) { return 8 * size; }   // built-in Font0: 8 px tall per size unit

struct Layout {
  int statusBarY = 2;        // status bar line (size 1), top-left / right-aligned
  int clockY     = 18;       // big clock digits, centred full width
  int clockSize  = 4;
  int dateY      = 52;       // date line under the clock (drawn at dateSize or scaled – see dateLineFit)
  int dateSize   = 2;
  int tempY      = 96;       // weather block base y = big temperature line
  int tempSize   = 5;
  int descOffset    = 44;    // description line (size 2), relative to tempY
  int feelsOffset   = 66;    // "Feels .. Hum .." (size 1)
  int todayOffset   = 80;    // "Today H .. L .." (size 1)
  int rainOffset    = 94;    // rain status line (size 1)
  int updatedOffset = 108;   // "Updated .." (size 1)
  int hintBottomGap = 14;    // bottom hint baseline, distance from screen bottom (size 1)
  int dogZoneW     = 96;     // reserved dog corner zone width (bottom-right) – also the dog tap area
  int dogZoneH     = 128;    // reserved dog corner zone height
};

static Layout gLayout;   // defaults above = current layout

// ---------------------------------------------------------------------------
//  Theme system
//
//  Four display themes (each with day and night variants) following the HTML
//  mockups in /mockups:
//    0 BASIC - original layout (gLayout coordinates, white/grey background)
//    1 SKY   - full-screen sky gradient, sun/moon + clouds/stars
//    2 SPLIT - data zone left, sky "stage" panel right of the dog
//    3 BENTO - bento grid of rounded cards
//
//  The THEME picker shows a half-scale preview: every renderer takes a T
//  transform (identity for live display) and draws in full-screen coordinates;
//  the clip rect confines it to the preview window.
// ---------------------------------------------------------------------------

#define THEME_COUNT 4
#define THEME_BASIC 0
#define THEME_SKY   1
#define THEME_SPLIT 2
#define THEME_BENTO 3

static const char* themeName(int i) {
  static const char* names[THEME_COUNT] = {"BASIC", "SKY", "SPLIT", "BENTO"};
  return (i >= 0 && i < THEME_COUNT) ? names[i] : "?";
}

// ---- Persistence (NVS namespace "cydwx") ----------------------------------
// "theme"    = selected theme index (current numbering: 0 BASIC .. 3 BENTO)
// "themeVer" = numbering version; 1 = pre-CARDS-removal (5-theme) indices
static Preferences themePrefs;
static void loadTheme() {
  themePrefs.begin("cydwx", true);
  uint32_t t = themePrefs.getUInt("theme", 0);
  uint32_t v = themePrefs.getUInt("themeVer", 1);
  if (v < 2) {   // one-time migration to the 4-theme numbering (CARDS was 2)
    if (t == 3)                            t = THEME_SPLIT;   // SPLIT: 3 -> 2
    else if (t == 4)                       t = THEME_BENTO;   // BENTO: 4 -> 3
    else if (t == 2 || t >= THEME_COUNT)   t = THEME_BASIC;   // old CARDS / unknown
    themePrefs.putUInt("themeVer", 2);
  }
  activeTheme = (t < THEME_COUNT) ? (int)t : 0;
}
static void saveTheme() { themePrefs.putUInt("theme", (uint32_t)activeTheme); }

// ---- Coordinate transform ---------------------------------------------------
struct T {
  float sx, sy;   // scale (1.0 live, 0.5 preview)
  int ox, oy;     // offset in panel pixels
  static const T ID;
  int   X(float v) const { return ox + (int)(v * sx); }
  int   Y(float v) const { return oy + (int)(v * sy); }
  int   W(float v) const { return (int)(v * sx); }
  int   H(float v) const { return (int)(v * sy); }
};
const T T::ID = {1.0f, 1.0f, 0, 0};

// ---- Colors -------------------------------------------------------------------
// The mockups specify DISPLAYED colors; this panel inverts every pixel, so all
// palette values are stored pre-inverted: C(r,g,b) = ~rgb565(displayed).
static uint16_t D(uint8_t r, uint8_t g, uint8_t b) {
  return tft.color24to16(((uint32_t)r << 16) | ((uint32_t)g << 8) | b);   // displayed space
}
static uint16_t C(uint8_t r, uint8_t g, uint8_t b) {
  return ~D(r, g, b) & 0xFFFF;   // code value that DISPLAYS as (r,g,b) on this inverted panel
}

// Theme 1 - SKY (gradient stops in displayed hex)
struct SkyPal {
  uint16_t clock, date, temp, unit, desc, tiny, hint;
  uint16_t stL, dot, stR;            // status row left text / dot / right label
  uint16_t fBg, fBd, fLbl, fVal;     // feels chip
  uint16_t rBg, rBd, rTxt, rDot;     // rain chip
};
static const SkyPal skyDay = {   // mockup1-sky-day.html (dark text on light sky)
  C(0x16,0x32,0x5c), C(0x3d,0x6a,0x9e), C(0x16,0x32,0x5c), C(0x3d,0x5f,0x8f),
  C(0x40,0x58,0x7a), C(0x5b,0x7a,0x9e), C(0x7d,0x92,0xad),
  C(0x2b,0x6c,0xb0), C(0x2f,0x9e,0x6b), C(0xc7,0x7e,0x14),
  C(0xdf,0xf0,0xff), C(0x8f,0xc3,0xf2), C(0x2b,0x6c,0xb0), C(0x1d,0x4f,0x8c),
  C(0xe9,0xf8,0xef), C(0x7c,0xcf,0x9f), C(0x23,0x7a,0x52), C(0xff,0xc8,0x57)
};
static const SkyPal skyNight = {   // mockup1-sky-night.html
  C(0xff,0xff,0xff), C(0x9f,0xc3,0xe8), C(0xff,0xff,0xff), C(0xcf,0xe3,0xff),
  C(0xd7,0xe3,0xf4), C(0x8a,0xa3,0xc0), C(0x6f,0x87,0xa8),
  C(0x6f,0xb3,0xff), C(0x3e,0xcf,0x8e), C(0xff,0xc8,0x57),
  C(0x25,0x63,0xab), C(0x7c,0xc0,0xff), C(0xbc,0xdc,0xff), C(0xff,0xff,0xff),
  C(0x12,0x3f,0x2c), C(0x3e,0xcf,0x8e), C(0x7e,0xe2,0xb8), C(0xff,0xc8,0x57)
};

// Theme 2 - SPLIT (left data zone + right sky panel at x >= 214)
struct SplitPal {
  uint16_t left;                      // solid data-zone background
  uint16_t skyLbl, ground;            // DAY/NIGHT label + ground ellipse
  uint16_t stL, dot;                  // status row (no right label here)
  uint16_t clock, date, temp, unit, descTxt;
  uint16_t fBg, fBd, fLbl, fVal, fIcon;   // FEELS chip + crossed-arrows icon
  uint16_t rBg, rBd, rTxt, rDot;      // RAIN chip (rDot = umbrella icon)
  uint16_t row1C, hint;
};
// Mockup3 colors (pre-inverted), taken from mockups/mockup3-split-{day,night}.html.
static const SplitPal splitDay = {
  C(0xff,0xff,0xff),
  C(0xff,0xff,0xff), C(0x6f,0xb1,0xe2),
  C(0x35,0x61,0x8f), C(0x2f,0x9e,0x6b),
  C(0x16,0x32,0x5c), C(0x4a,0x6f,0xa5), C(0x16,0x32,0x5c), C(0x3d,0x5f,0x8f),
  C(0x5a,0x70,0x89),
  C(0xe3,0xf1,0xff), C(0x9c,0xc7,0xf5), C(0x2b,0x6c,0xb0), C(0x1d,0x4f,0x8c), C(0x2f,0x7f,0xd0),
  C(0xe6,0xf7,0xec), C(0x8f,0xd6,0xae), C(0x23,0x7a,0x52), C(0x2f,0x9e,0x6b),
  C(0x7d,0x92,0xad), C(0xa8,0xb8,0xcc)
};
static const SplitPal splitNight = {
  C(0x15,0x1f,0x2e),
  C(0xff,0xc8,0x57), C(0x27,0x4b,0x7d),
  C(0x6f,0xb3,0xff), C(0x3e,0xcf,0x8e),
  C(0xff,0xff,0xff), C(0x9f,0xc3,0xe8), C(0xff,0xff,0xff), C(0xcf,0xe3,0xff),
  C(0xc3,0xd0,0xe4),
  C(0x17,0x3a,0x5f), C(0x5a,0xa9,0xff), C(0x9f,0xd0,0xff), C(0xff,0xff,0xff), C(0x9f,0xd0,0xff),
  C(0x12,0x35,0x2a), C(0x2f,0x9e,0x6b), C(0x7e,0xe2,0xb8), C(0x7e,0xe2,0xb8),
  C(0x8a,0xa3,0xc0), C(0x5f,0x73,0x92)
};

// Theme 3 - BENTO (rounded cards on a solid screen background)
struct BentoPal {
  uint16_t cardBg, cardBd;            // standard cards
  uint16_t clock, date, stL, dot, stR;
  uint16_t accent;                    // orange bar on the temp card
  uint16_t temp, unit;
  uint16_t chipBg, chipBd, chipLbl, chipVal;   // FEELS chip
  uint16_t descTxt;
  uint16_t rBg, rBd, rTxt, rDot;      // rain card
  uint16_t detC, updC;                // detail rows / updated row
  uint16_t dogCardBg, capC;           // dog card + caption
};
// Colors match mockups/mockup4-bento-day|night.html exactly (class names noted).
static const BentoPal bentoDay = {
  C(0xff,0xff,0xff), C(0xd7,0xe3,0xf4),            // .card bg / border
  C(0x16,0x32,0x5c), C(0x4a,0x6f,0xa5),            // .time / .date
  C(0x2b,0x6c,0xb0), C(0x2f,0x9e,0x6b), C(0xc7,0x7e,0x14),  // .stl / .dot / .str
  C(0x5a,0xa9,0xff),                                // .accent
  C(0x16,0x32,0x5c), C(0x3d,0x5f,0x8f),             // .tempv / .tempu
  C(0xe3,0xf1,0xff), C(0x9c,0xc7,0xf5),             // .fchip bg / border
  C(0x2b,0x6c,0xb0), C(0x1d,0x4f,0x8c),             // .flab / .fval
  C(0x5a,0x70,0x89),                                // .desc
  C(0xe6,0xf7,0xec), C(0x8f,0xd6,0xae),             // .raincard bg / border
  C(0x23,0x7a,0x52), C(0x2f,0x9e,0x6b),             // .rtxt / .rdot
  C(0x7d,0x92,0xad), C(0x9f,0xb0,0xc4),             // .dline / .upd
  C(0xf4,0xf9,0xff), C(0x7d,0x92,0xad)              // .dcard bg / .dcap
};
static const BentoPal bentoNight = {
  C(0x1c,0x25,0x34), C(0x2c,0x3a,0x52),
  C(0xff,0xff,0xff), C(0x8f,0xa3,0xc0),
  C(0x6f,0xb3,0xff), C(0x3e,0xcf,0x8e), C(0xff,0xc8,0x57),
  C(0x5a,0xa9,0xff),
  C(0xff,0xff,0xff), C(0x9f,0xc3,0xe8),
  C(0x17,0x3a,0x5f), C(0x5a,0xa9,0xff),
  C(0x9f,0xd0,0xff), C(0xff,0xff,0xff),
  C(0xc3,0xd0,0xe4),
  C(0x12,0x35,0x2a), C(0x2f,0x9e,0x6b),
  C(0x7e,0xe2,0xb8), C(0x3e,0xcf,0x8e),
  C(0x8f,0xa3,0xc0), C(0x5f,0x73,0x92),
  C(0x1a,0x23,0x33), C(0x6f,0x87,0xa8)
};

// Palette accessors (day/night follows the live weather state; the picker's
// preview uses the same variant so it matches what a selection would show).
static const SkyPal&   SKY()   { return nightMode() ? skyNight : skyDay; }
static const SplitPal& SPLIT() { return nightMode() ? splitNight : splitDay; }
static const BentoPal& BENTO() { return nightMode() ? bentoNight : bentoDay; }

// ---- Backgrounds ---------------------------------------------------------------
static void lerpRgb(uint8_t ar, uint8_t ag, uint8_t ab,
                    uint8_t br, uint8_t bg, uint8_t bb,
                    float t, uint8_t& r, uint8_t& g, uint8_t& b) {
  if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
  r = (uint8_t)(ar + (br - ar) * t + 0.5f);
  g = (uint8_t)(ag + (bg - ag) * t + 0.5f);
  b = (uint8_t)(ab + (bb - ab) * t + 0.5f);
}

// DISPLAYED RGB565 of the screen background at full-screen position (x,y).
static uint16_t bgDisplayed(int idx, int xFull, int yFull) {
  float t = (float)yFull / 239.0f;
  uint8_t r, g, b;
  switch (idx) {
    case 0:   // BASIC: white or the original night grey gradient
      if (!nightMode()) return D(255, 255, 255);
      return ~nightBgColor(yFull) & 0xFFFF;
    case 1: { // SKY: 3-stop vertical gradient (mid at 55%)
      uint8_t tr, tg, tb, mr, mg, mb, br, bgc, bb;
      if (!nightMode()) { tr=0x8f;tg=0xc9;tb=0xef; mr=0xcf;mg=0xe9;mb=0xfb; br=0xea;bgc=0xf6;bb=0xff; }
      else             { tr=0x0a;tg=0x1c;tb=0x38; mr=0x17;mg=0x39;mb=0x6b; br=0x1d;bgc=0x47;bb=0x7f; }
      if (t < 0.55f) lerpRgb(tr,tg,tb, mr,mg,mb, t / 0.55f, r,g,b);
      else           lerpRgb(mr,mg,mb, br,bgc,bb, (t - 0.55f) / 0.45f, r,g,b);
      return D(r,g,b);
    }
    case THEME_SPLIT:   // SPLIT: solid left zone; right panel is a top->bottom gradient
      if (xFull < 214) return nightMode() ? D(0x15,0x1f,0x2e) : D(0xff,0xff,0xff);
      if (!nightMode()) lerpRgb(0x7c,0xc0,0xec, 0xb5,0xdc,0xf6, t, r,g,b);
      else              lerpRgb(0x1c,0x3f,0x74, 0x0e,0x25,0x49, t, r,g,b);
      return D(r,g,b);
    default:  // BENTO: solid
      return nightMode() ? D(0x13,0x1a,0x26) : D(0xee,0xf4,0xfa);
  }
}
static uint16_t bgCode(int idx, int xFull, int yFull) { return ~bgDisplayed(idx, xFull, yFull) & 0xFFFF; }

// Draw target for the theme renderers below: &tft normally, or one of the
// pre-render sprites while the swipe animation bakes themes offscreen (see
// animatePickerSwipe / renderThemeIntoSprite).
static lgfx::LGFXBase* gT = &tft;

// Full background fill for a theme (per-row gradient sampling).
static void drawThemeBg(int idx, const T& t) {
  int rows = max(1, t.H(240));
  for (int i = 0; i < rows; i++) {
    int yFull = (240 * i + rows / 2) / rows;
    if (idx == THEME_SPLIT) {   // split: left solid + right per-row gradient
      gT->fillRect(t.X(0),   t.Y(i), max(1, t.W(214)),     1, bgCode(THEME_SPLIT, 0, yFull));
      gT->fillRect(t.X(214), t.Y(i), max(1, t.W(320 - 214)), 1, bgCode(THEME_SPLIT, 267, yFull));
    } else {
      gT->fillRect(t.X(0), t.Y(i), max(1, t.W(320)), 1, bgCode(idx, 0, yFull));
    }
  }
}

// Clear a band back to the theme background (partial updates for themes 1-3).
// Coordinates are full-space; T::ID for live display.
static void clearThemed(int idx, const T& t, int xFull, int yFull, int wFull, int hFull) {
  for (int i = 0; i < hFull; i++)
    gT->fillRect(t.X(xFull), t.Y(yFull + i), max(1, t.W(wFull)), 1, bgCode(idx, 0, yFull + i));
}

// ---- Text helpers ------------------------------------------------------------------
// Shrink-to-fit: returns the (possibly reduced) full-space text size for buf.
static float fitSize(const char* buf, float sBase, int avail) {
  gT->setTextSize(sBase);
  int w = gT->textWidth(buf);
  if (w > avail && w > 0) return sBase * (float)avail / (float)w;
  return sBase;
}

// Centered text at full-space center cx, row yFull.
static void drawCenter(const T& t, int cx, int yFull, const char* buf, float s, uint16_t c, int avail) {
  float fs = fitSize(buf, s, avail);
  gT->setTextSize(fs);
  int w = gT->textWidth(buf);          // full-space width at fs
  gT->setTextSize(fs * t.sx);
  gT->setTextColor(c);
  gT->drawString(buf, t.X(cx - w / 2.0f), t.Y(yFull));
}

// Right-aligned text ending at xEnd (full space).
static void drawRight(const T& t, int xEnd, int yFull, const char* buf, float s, uint16_t c) {
  gT->setTextSize(s);
  int w = gT->textWidth(buf);
  gT->setTextSize(s * t.sx);
  gT->setTextColor(c);
  gT->drawString(buf, t.X(xEnd - w), t.Y(yFull));
}

// Two strings on one row: left at xL, right ending at xR; both shrink together.
static void drawSplitRow(const T& t, int xL, int xR, int yFull, const char* l, const char* r, float s, uint16_t c) {
  gT->setTextSize(s);
  int wl = gT->textWidth(l), wr = gT->textWidth(r);
  int gap = (int)(8 * s + 0.5f);
  float fs = s;
  if (wl + wr + gap > xR - xL && wl + wr > 0) fs = s * (float)(xR - xL - gap) / (float)(wl + wr);
  gT->setTextSize(fs);
  wl = gT->textWidth(l); wr = gT->textWidth(r);
  gT->setTextSize(fs * t.sx);
  gT->setTextColor(c);
  gT->drawString(l, t.X(xL), t.Y(yFull));
  if (wr > 0) gT->drawString(r, t.X(xR - wr), t.Y(yFull));
}

// ---- Decorations ---------------------------------------------------------------------
// Thick line (v1's drawLine has no width argument): Bresenham path of filled dots.
static void thickLine(const T& t, int x0f, int y0f, int x1f, int y1f, float wFull, uint16_t c) {
  int x0 = t.X(x0f), y0 = t.Y(y0f), x1 = t.X(x1f), y1 = t.Y(y1f);
  int r = max(1, (int)(wFull * t.sx / 2.0f + 0.5f));
  int dx = abs(x1 - x0), dy = -abs(y1 - y0);
  int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    gT->fillCircle(x0, y0, r, c);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

// Flat sun (mockup1): pale halo disc + core + 8 short rays at the rim.
static void drawSun(const T& t, int cx, int cy, uint16_t halo, uint16_t core, uint16_t ray) {
  float s = t.sx;
  int X = t.X(cx), Y = t.Y(cy);
  gT->fillCircle(X, Y, (int)(17 * s + 0.5f), halo);
  const int d[8][2] = {{0,-1},{1,0},{0,1},{-1,0},{1,-1},{1,1},{-1,1},{-1,-1}};
  for (int i = 0; i < 8; i++) {
    float m = (d[i][0] != 0 && d[i][1] != 0) ? 0.707f : 1.0f;   // normalize diagonals
    thickLine(t, cx + (int)(16 * d[i][0] * m), cy + (int)(16 * d[i][1] * m),
              cx + (int)(12 * d[i][0] * m), cy + (int)(12 * d[i][1] * m), 2.5f, ray);
  }
  gT->fillCircle(X, Y, (int)(11.5f * s + 0.5f), core);
}

// Mockup3 flat sun: core r6 + short rays, no halo (18px icon in the panel).
static void drawFlatSun(const T& t, int cx, int cy) {
  float s = t.sx;
  int X = t.X(cx), Y = t.Y(cy);
  const uint16_t core = C(0xff,0xd7,0x5e), ray = C(0xf0,0xb4,0x29);
  const int d[8][2] = {{0,-1},{1,0},{0,1},{-1,0},{1,-1},{1,1},{-1,1},{-1,-1}};
  for (int i = 0; i < 8; i++) {
    float m = (d[i][0] != 0 && d[i][1] != 0) ? 0.707f : 1.0f;
    thickLine(t, cx + (int)(6.5 * d[i][0] * m), cy + (int)(6.5 * d[i][1] * m),
              cx + (int)(8.5 * d[i][0] * m), cy + (int)(8.5 * d[i][1] * m), 1.4f, ray);
  }
  gT->fillCircle(X, Y, (int)(6 * s + 0.5f), core);
}

// Mockup3 panel cloud (stationary): rounded bar with two bumps above it.
static void drawSplitCloud(const T& t, int x, int y, bool small, uint16_t c) {
  float s = t.sx;
  if (!small) {   // pc1: 52x13 bar; bumps r9 @(+18,-7), r6 @(+33,+2)
    gT->fillRoundRect(t.X(x), t.Y(y), max(2, (int)(52 * s + 0.5f)), max(2, (int)(13 * s + 0.5f)),
                      (int)(8 * s + 0.5f), c);
    gT->fillCircle(t.X(x + 18), t.Y(y - 7), (int)(9 * s + 0.5f), c);
    gT->fillCircle(t.X(x + 33), t.Y(y + 2), (int)(6 * s + 0.5f), c);
  } else {        // pc2: 40x11 bar; bumps r7 @(+14,+1), r5 @(+25,+1)
    gT->fillRoundRect(t.X(x), t.Y(y), max(2, (int)(40 * s + 0.5f)), max(2, (int)(11 * s + 0.5f)),
                      (int)(6 * s + 0.5f), c);
    gT->fillCircle(t.X(x + 14), t.Y(y + 1), (int)(7 * s + 0.5f), c);
    gT->fillCircle(t.X(x + 25), t.Y(y + 1), (int)(5 * s + 0.5f), c);
  }
}

// Blend a pre-inverted RGB565 color toward white by pct/100.
static uint16_t blendWhite(uint16_t c, int pct) {
  int r = ((c >> 11) & 31u), g = ((c >> 5) & 63u), b = (c & 31u);
  auto mix = [pct](int v, int maxv) { return (v * (100 - pct) + maxv * pct) / 100; };
  return D(mix(r, 31), mix(g, 63), mix(b, 31));
}

// Mockup3 description cloud icon (~14px): three bumps over a flat bottom.
static void drawMiniCloud(const T& t, int x, int y, uint16_t c) {
  float s = t.sx;
  gT->fillCircle(t.X(x + 4), t.Y(y + 7), max(1, (int)(3 * s + 0.5f)), c);
  gT->fillCircle(t.X(x + 8), t.Y(y + 5), max(1, (int)(4 * s + 0.5f)), c);
  gT->fillCircle(t.X(x + 11), t.Y(y + 7), max(1, (int)(3 * s + 0.5f)), c);
  gT->fillRect(t.X(x + 2), t.Y(y + 7), max(1, (int)(11 * s)), max(1, (int)(4 * s)), c);
}

// Flat cloud: rounded bar with two bumps on top; k scales the whole shape.
static void drawCloud(const T& t, int x, int y, float k, uint16_t c) {
  float s = t.sx * k;
  int X = t.X(x), Y = t.Y(y);
  int W = max(2, (int)(52 * s + 0.5f));
  int Hh = max(3, (int)(16 * s + 0.5f));
  gT->fillRoundRect(X, Y, W, Hh, min(Hh / 2, (int)(8 * s + 0.5f)), c);
  gT->fillCircle(X + (int)(23 * s), Y + (int)(2 * s), max(1, (int)(11 * s + 0.5f)), c);
  gT->fillCircle(X + (int)(42 * s), Y + (int)(2 * s), max(1, (int)(8 * s + 0.5f)), c);
}

// Tiny star dot (mockup .star).
static void drawStar(const T& t, int x, int y, uint16_t c) {
  int d = max(1, (int)(2 * t.sx));
  gT->fillRect(t.X(x), t.Y(y), d, d, c);
}

// Small umbrella (mockup1 rain chip): half-disc canopy + straight shaft.
// bgC flattens the lower half of the canopy circle; only used where the icon
// sits on a solid chip background. k scales the whole shape (~13px tall at 1).
static void drawUmbrella(const T& t, int cx, int cy, float k, uint16_t c, uint16_t bgC) {
  float s = t.sx * k;
  int X = t.X(cx), Y = t.Y(cy);
  int r = max(2, (int)(5.0f * s + 0.5f));
  int cyC = Y - (int)(3.5f * s);                    // canopy center line
  gT->fillCircle(X, cyC, r, c);
  gT->fillRect(X - r, cyC + 1, 2 * r, r, bgC);     // keep only the top half
  int sw = max(1, (int)(1.6f * s));                 // shaft
  gT->fillRect(X - sw / 2, cyC + 1, sw, (int)(7.5f * s), c);
}

// Tiny sun / crescent for the status-row DAY/NIGHT label (mockup1 st-r icon).
// biteC: color cut out of the crescent (the surface the icon sits on).
static void drawDnIcon(const T& t, int cx, int cy, uint16_t c, uint16_t biteC) {
  float s = t.sx;
  int X = t.X(cx), Y = t.Y(cy);
  if (!nightMode()) {   // sun: disc + 8 short rays
    gT->fillCircle(X, Y, max(2, (int)(3 * s + 0.5f)), c);
    const int d[8][2] = {{0,-1},{1,0},{0,1},{-1,0},{1,-1},{1,1},{-1,1},{-1,-1}};
    for (int i = 0; i < 8; i++) {
      float m = (d[i][0] != 0 && d[i][1] != 0) ? 0.707f : 1.0f;   // normalize diagonals
      thickLine(t, cx + (int)(5 * d[i][0] * m), cy + (int)(5 * d[i][1] * m),
                cx + (int)(6.5f * d[i][0] * m), cy + (int)(6.5f * d[i][1] * m), 1.2f, c);
    }
  } else {   // crescent: disc with a background-colored bite
    gT->fillCircle(X, Y, max(2, (int)(4 * s + 0.5f)), c);
    gT->fillCircle(X + (int)(1.8f * s), Y - (int)(1.6f * s),
                   max(2, (int)(3.4f * s + 0.5f)), biteC);
  }
}

// ---- Widgets ---------------------------------------------------------------------------
// "DAY"/"NIGHT" (or "--" before the first fetch) for themed layouts.
static const char* dayNightLabel() {
  return !wx.isDayKnown ? "--" : (wx.isDay ? "DAY" : "NIGHT");
}

// Left status: dot + "WIFI NTP"/"--OFF-- --NTP"; optional right label.
static void buildWifiStatus(char* buf, size_t n) {
  snprintf(buf, n, "%s%s", (WiFi.status() == WL_CONNECTED) ? "WIFI" : "--OFF--",
           ntpSynced ? " NTP" : " --NTP");
}

static void drawStatusRow(const T& t, int xL, int yFull, float s, uint16_t cL, uint16_t dotC,
                          const char* right, uint16_t cR, int xREnd, bool dnIcon = false) {
  char status[24];
  buildWifiStatus(status, sizeof(status));
  gT->setTextSize(s * t.sx);
  int dr = max(1, (int)(2.5f * s * t.sx + 0.5f));
  gT->fillCircle(t.X(xL) + dr, t.Y(yFull + 4 * s), dr, dotC);
  gT->setTextColor(cL);
  gT->drawString(status, t.X(xL + 8 * s), t.Y(yFull));
  if (right && right[0]) {
    gT->setTextSize(s);
    int rw = gT->textWidth(right);   // full-space width
    gT->setTextSize(s * t.sx);
    gT->setTextColor(cR);
    gT->drawString(right, t.X(xREnd - rw), t.Y(yFull));
    if (dnIcon) {   // small sun/moon left of the label (mockup1)
      int icx = xREnd - rw - (int)(9 * s);
      drawDnIcon(t, icx, yFull + 4 * s, cR, bgCode(1, icx, yFull + 4 * s));
    }
  }
}

// Rounded "FEELS LIKE" chip: label left, value right.
static void drawFeelsChip(const T& t, int x, int yF, int w, int h,
                          uint16_t bgC, uint16_t bdC, float sLbl, uint16_t cLbl,
                          const char* val, float sVal, uint16_t cVal) {
  gT->fillRoundRect(t.X(x), t.Y(yF), max(2, t.W(w)), max(2, t.H(h)), (int)(9 * t.sx + 0.5f), bgC);
  gT->drawRoundRect(t.X(x), t.Y(yF), max(1, t.W(w) - 1), max(1, t.H(h) - 1), (int)(8 * t.sx + 0.5f), bdC);
  gT->setTextSize(sLbl);
  int lw = gT->textWidth("FEELS LIKE");   // full-space width
  gT->setTextSize(sLbl * t.sx);
  gT->setTextColor(cLbl);
  gT->drawString("FEELS LIKE", t.X(x + (int)(10 * sLbl)), t.Y(yF + (h - 8 * sLbl) / 2.0f));
  float fs = fitSize(val, sVal, max(8, w - (int)(24 * sVal) - lw));
  gT->setTextSize(fs);
  int vw = gT->textWidth(val);
  gT->setTextSize(fs * t.sx);
  gT->setTextColor(cVal);
  gT->drawString(val, t.X(x + w - (int)(10 * fs) - vw), t.Y(yF + (h - 8 * fs) / 2.0f));
}

// Rounded rain-status banner with a small icon left of the centered text:
// plain dot by default; umbrella when rain is falling/nearby/forecast (theme 1).
static void drawRainChip(const T& t, int x, int yF, int w, int h,
                         uint16_t bgC, uint16_t bdC, const char* txt, float s,
                         uint16_t cTxt, uint16_t dotC, bool umbrella = false) {
  gT->fillRoundRect(t.X(x), t.Y(yF), max(2, t.W(w)), max(2, t.H(h)), (int)(8 * t.sx + 0.5f), bgC);
  gT->drawRoundRect(t.X(x), t.Y(yF), max(1, t.W(w) - 1), max(1, t.H(h) - 1), (int)(7 * t.sx + 0.5f), bdC);
  float fs = fitSize(txt, s, w - (int)(24 * s));
  gT->setTextSize(fs);
  int tw = gT->textWidth(txt);   // full-space width
  int cxFull = x + w / 2;
  if (umbrella)
    drawUmbrella(t, cxFull - tw / 2.0f - (int)(9 * fs), yF + h / 2.0f, fs / 1.6f, dotC, bgC);
  else {
    int dr = max(1, (int)(3 * fs * t.sx + 0.5f));
    gT->fillCircle(t.X(cxFull - tw / 2.0f - (int)(9 * fs)), t.Y(yF + h / 2.0f), dr, dotC);
  }
  gT->setTextSize(fs * t.sx);
  gT->setTextColor(cTxt);
  gT->drawString(txt, t.X(cxFull - tw / 2.0f), t.Y(yF + (h - 8 * fs) / 2.0f));
}

// Rounded card fill + border.
static void drawCard(const T& t, int x, int y, int w, int h, uint16_t bgC, uint16_t bdC) {
  gT->fillRoundRect(t.X(x), t.Y(y), max(2, t.W(w)), max(2, t.H(h)), (int)(10 * t.sx + 0.5f), bgC);
  gT->drawRoundRect(t.X(x), t.Y(y), max(1, t.W(w) - 1), max(1, t.H(h) - 1), (int)(9 * t.sx + 0.5f), bdC);
}

// ---- Shared string builders ---------------------------------------------------------------
// Rain status line shared by all themes (same logic as the original layout).
static void buildRainLine(char* buf, size_t n) {
  if (!wx.valid) { snprintf(buf, n, "NO WEATHER DATA"); return; }
  if (wx.rainingNow) {
    if (wx.rainEnd[0] && wx.rainMayContinue) snprintf(buf, n, "RAIN NOW - MAY CONTINUE");
    else if (wx.rainEnd[0])                  snprintf(buf, n, "RAIN NOW - ends ~%s", wx.rainEnd);
    else                                     snprintf(buf, n, "RAIN NOW");
  } else if (wx.rainNearby && wx.nearbyStns[0]) {
    snprintf(buf, n, "RAIN NEARBY (%s)", wx.nearbyStns);
  } else if (wx.nextRain[0]) {
    const char* verb = wx.nextRainFrom15min ? "expected" : "possible";
    snprintf(buf, n, "Rain %s %s%s", verb, wx.nextRain, wx.nextRainTomorrow ? " tomorrow" : "");
  } else {
    snprintf(buf, n, "No rain expected");
  }
}

// Short "UPD h:mm AM/PM" / "UPD HH:MM" in the current clock mode (empty if no update yet).
static void buildUpdShort(char* buf, size_t n) {
  if (wx.lastUpdateEpoch <= 1700000000UL) { buf[0] = '\0'; return; }
  char tbuf[16];
  struct tm lt;
  time_t local = wx.lastUpdateEpoch + localOffsetSec(wx.lastUpdateEpoch);
  localtime_r(&local, &lt);
  formatUpdTime(tbuf, sizeof(tbuf), &lt, false);
  snprintf(buf, n, "UPD %s", tbuf);
}

// Temperature string with unit ("--.-" when unknown).
static void buildTempStr(char* buf, size_t n, float v) {
  if (isnan(v)) snprintf(buf, n, "--.-");
  else          snprintf(buf, n, "%.1f %s", tempForDisplay(v), tempUnitChar());
}

// Human label for the dog's current outfit band (bento caption).
static const char* dogBandLabel() {
  const char* name = dogCurrentName();
  if (!name || !name[0]) return "NO DATA";
  if (strncmp(name, "error", 5) == 0)        return "NO DATA";
  if (strncmp(name, "rain_now", 8) == 0)     return "RAINING";
  if (strncmp(name, "rain_later", 10) == 0)  return "RAIN SOON";
  static const struct { const char* band; const char* label; } bands[] = {
    {"extreme_cold", "EXTREME COLD"}, {"very_cold", "VERY COLD"}, {"cold", "COLD"},
    {"cool", "COOL"}, {"mild_cool", "MILD COOL"}, {"mild_warm", "MILD WARM"}, {"hot", "HOT"}
  };
  for (auto& b : bands) if (strncmp(name, b.band, strlen(b.band)) == 0) return b.label;
  return "?";
}

// ---- Dog compositing ------------------------------------------------------------------------
// Displayed background color behind the dog for a theme at full-screen row y.
static void dogBgDisplayed(int theme, int yFull, uint8_t& r, uint8_t& g, uint8_t& b) {
  if (theme == THEME_SPLIT) {   // dog sits on the right panel: its gradient
    float t = (float)yFull / 239.0f;
    if (!nightMode()) lerpRgb(0x7c,0xc0,0xec, 0xb5,0xdc,0xf6, t, r,g,b);
    else              lerpRgb(0x1c,0x3f,0x74, 0x0e,0x25,0x49, t, r,g,b);
    return;
  }
  if (theme == THEME_BENTO) {   // solid dog card
    uint16_t d = nightMode() ? D(0x1a,0x23,0x33) : D(0xf4,0xf9,0xff);
    r = (uint8_t)(((d >> 11) & 31u) * 255 / 31);
    g = (uint8_t)(((d >> 5) & 63u) * 255 / 63);
    b = (uint8_t)((d & 31u) * 255 / 31);
    return;
  }
  uint16_t d = bgDisplayed(theme, 0, yFull);   // themes 0-1: screen background
  r = (uint8_t)(((d >> 11) & 31u) * 255 / 31);
  g = (uint8_t)(((d >> 5) & 63u) * 255 / 63);
  b = (uint8_t)((d & 31u) * 255 / 31);
}

// Composite + blit the current dog image. The full-res region is
// (xFull,yTopFull,fw,fh) in screen space; t scales it for previews (nearest
// neighbour). Transparent pixels show the theme's background behind them.
static void drawDogBox(const T& t, int xFull, int yTopFull, int fw, int fh, int theme) {
  if (fw <= 0 || fh <= 0) return;
  int px = t.X(xFull), py = t.Y(yTopFull);
  int pw = max(1, t.W(fw)), ph = max(1, t.H(fh));
  static uint16_t row[128];    // >= kMaxW (96) full-res pixels
  for (int i = 0; i < ph; i++) {
    int imgRow = (i * fh) / ph;                       // nearest-neighbour source row
    int x0, x1;
    if (!dogRowSpan(imgRow, x0, x1)) continue;   // fully transparent row: leave what's underneath
    uint8_t br, bgc, bb;
    dogBgDisplayed(theme, yTopFull + imgRow, br, bgc, bb);
    compositeRow(imgRow, br, bgc, bb, row);
    if (pw == fw) { gT->pushImage(px + x0, py + i, x1 - x0 + 1, 1, row + x0); continue; }
    static uint16_t hrow[64];   // >= kMaxW/2
    int sx0 = (x0 * pw) / fw, sx1 = (x1 * pw) / fw;
    for (int j = sx0; j <= sx1 && j < pw; j++) hrow[j - sx0] = row[(j * fw) / pw];
    gT->pushImage(px + sx0, py + i, sx1 - sx0 + 1, 1, hrow);
  }
}

// Standard bottom-right corner placement (themes 0-2).
static void drawDogImage(const T& t, int theme) {
  int fw = dogWidth();
  if (fw <= 0) return;
  int fh = dogHeight();
  drawDogBox(t, dogLeft(320), 240 - fh - kDogMargin, fw, fh, theme);
}

// ---------------------------------------------------------------------------
//  Display helpers – theme 0 (BASIC). All take a T transform so they can also
//  be rendered into the picker's half-scale preview window.
// ---------------------------------------------------------------------------

// Clock + date strings for the current time/format settings (shared by all
// themes and the partial-update cache).
static void buildClockDateStrings(char* clockBuf, size_t n1, char* dateBuf, size_t n2) {
  if (ntpSynced) {
    struct tm lt;
    nowLocal(&lt);
    formatClock(clockBuf, n1, &lt);   // respects use24Hour
    formatDateLine(dateBuf, n2, &lt);  // respects dateFormat
  } else {
    snprintf(clockBuf, n1, "--:--");
    snprintf(dateBuf, n2, "Waiting for NTP sync ...");
  }
}

// Full-space clock text size for a theme (shrinks so the widest string in the
// current time format fits its zone). Used by renderers AND partial updates.
static float themeClockFit(int idx) {
  const char* mx = clockMaxString();
  float s; int avail;
  switch (idx) {
    case 0:  return gLayout.clockSize;
    case THEME_SKY:   s = 5.25f; avail = 316; break;
    case THEME_SPLIT: s = 4.6f;  avail = 198; break;   // left zone [12,210)
    default:          s = 3.4f;  avail = 288; break;   // bento clock card
  }
  gT->setTextSize(s);
  int w = gT->textWidth(mx);
  return (w > avail && w > 0) ? s * (float)avail / (float)w : s;
}

static void drawStatusBar(const T& t) {
  // Single-arg setTextColor: glyph cells are NOT filled (back == fore), so the
  // text sits directly on whatever background is underneath (white or gradient).
  gT->setTextSize(1 * t.sx);
  gT->setTextColor(TFT_CYAN);

  char status[24];
  buildWifiStatus(status, sizeof(status));
  gT->drawString(status, t.X(4), t.Y(gLayout.statusBarY));

  // Persistent top-right day/night status (independent verification of is_day):
  //   "Day"             -> valid is_day == 1
  //   "Night"           -> valid is_day == 0
  //   "Day/Night ERROR" -> missing / invalid / not yet available
  const char* dn = !wx.isDayKnown ? "Day/Night ERROR"
                                  : (wx.isDay ? "Day" : "Night");
  gT->setTextSize(1);
  int dw = gT->textWidth(dn);   // full-space width at size 1
  gT->setTextSize(t.sx);
  gT->drawString(dn, t.X(320 - dw - 4), t.Y(gLayout.statusBarY));
}

// Cache of what is currently on screen for the clock/date lines. Kept in
// sync by every full render (drawClockAndDate) and used by updateDynamicTexts()
// to repaint only when the text actually changed.
static char lastClockBuf[16] = {0};
static char lastDateBuf[32] = {0};

// Choose a text size for the date line so the whole string always fits on
// screen. Normal strings render at exactly gLayout.dateSize; only very long
// ones (e.g. "Wednesday September 30, 2026" = 336 px at size 2 on this fixed-
// pitch font) are scaled down slightly. Returns the chosen FULL-space size and
// leaves tft's text size set to it.
static float dateLineFit(const char* buf) {
  const int avail = 320 - 4;   // small margin each side
  float s = (float)gLayout.dateSize;
  gT->setTextSize(s);
  int w = gT->textWidth(buf);
  if (w > avail && w > 0) {
    s *= (float)avail / (float)w;
    gT->setTextSize(s);
  }
  return s;
}

static void drawClockAndDate(const T& t) {
  char clockBuf[16];
  char dateBuf[32];

  // No comma after the weekday (keeps the line shorter; dateLineFit() scales
  // down any line that would still run off the screen).
  buildClockDateStrings(clockBuf, sizeof(clockBuf), dateBuf, sizeof(dateBuf));

  // Keep the partial-update cache in sync with what was just drawn so a full
  // render (e.g. right after a format toggle) causes no redundant repaint on
  // the next per-second tick.
  strncpy(lastClockBuf, clockBuf, sizeof(lastClockBuf) - 1);
  lastClockBuf[sizeof(lastClockBuf) - 1] = '\0';
  strncpy(lastDateBuf, dateBuf, sizeof(lastDateBuf) - 1);
  lastDateBuf[sizeof(lastDateBuf) - 1] = '\0';

  // Big digital clock, centred in the upper third
  gT->setTextSize(gLayout.clockSize);      // ~24 px tall digits at size 4
  int cw = gT->textWidth(clockBuf);
  int cx = (320 - cw) / 2;

  gT->setTextSize(gLayout.clockSize * t.sx);
  gT->setTextColor(TFT_WHITE);
  gT->drawString(clockBuf, t.X(cx), t.Y(gLayout.clockY));

  // Full date line under the clock (scaled down slightly if too long – see dateLineFit)
  float ds = dateLineFit(dateBuf);         // chosen full-space size (normally exactly gLayout.dateSize)
  gT->setTextSize(ds * t.sx);
  gT->setTextColor(TFT_YELLOW);
  int dw = gT->textWidth(dateBuf);         // width at the chosen size
  gT->drawString(dateBuf, t.X((320 - dw) / 2), t.Y(gLayout.dateY));
}

static void drawWeatherPanel(const T& t) {
  int y = gLayout.tempY;   // top of the weather block (layout)

  if (!wx.valid) {
    // Placed above the image top (y=108 for a full-size picture), so these can
    // stay full-width centred without touching it.
    gT->setTextSize(2 * t.sx);
    gT->setTextColor(TFT_GRAY);
    const char* msg1 = "No weather data yet";
    gT->drawString(msg1, t.X((320 - gT->textWidth(msg1)) / 2), t.Y(y - 22));

    const char* msg2 = "Fetching from Open-Meteo ...";
    gT->setTextSize(1 * t.sx);
    gT->drawString(msg2, t.X((320 - gT->textWidth(msg2)) / 2), t.Y(y - 2));
    return;
  }

  // Everything in the weather block is centered in the text zone
  // [0, textZoneRight()) so it never collides with the dog image in the
  // bottom-right corner. The two size-2 rows of the original layout were too
  // wide for that zone and are drawn at size 1 instead.
  int zoneRight = textZoneRight();

  // Current temperature (large). Unit follows the tap-toggled useCelsius flag;
  // feels-like and today H/L below convert the same way so the screen is
  // consistent.
  char tmp[16];
  snprintf(tmp, sizeof(tmp), "%.1f %s", tempForDisplay(wx.temp), tempUnitChar());
  int fs = gLayout.tempSize;   // big digits
  gT->setTextSize(fs);
  int tw = gT->textWidth(tmp);
  gT->setTextSize(fs * t.sx);
  gT->setTextColor(TFT_WHITE);
  gT->drawString(tmp, t.X((zoneRight - tw) / 2), t.Y(y));

  // Weather description
  gT->setTextSize(2);
  String desc = wx.description;
  int dw = gT->textWidth(desc.c_str());
  gT->setTextSize(2 * t.sx);
  gT->setTextColor(TFT_CYAN);
  gT->drawString(desc, t.X((zoneRight - dw) / 2), t.Y(y + gLayout.descOffset));

  // Feels-like & humidity row
  char line[64];
  snprintf(line, sizeof(line), "Feels %.1f %s   Hum %d%%",
           tempForDisplay(wx.feelsLike), tempUnitChar(), wx.humidity);
  gT->setTextSize(1);
  int lw = gT->textWidth(line);
  gT->setTextSize(t.sx);
  gT->setTextColor(TFT_GREENYELLOW);
  gT->drawString(line, t.X((zoneRight - lw) / 2), t.Y(y + gLayout.feelsOffset));

  // Today high / low
  snprintf(line, sizeof(line), "Today  H %.1f   L %.1f",
           tempForDisplay(wx.tMax), tempForDisplay(wx.tMin));
  gT->setTextSize(1);
  lw = gT->textWidth(line);
  gT->setTextColor(TFT_SKYBLUE);
  gT->drawString(line, t.X((zoneRight - lw) / 2), t.Y(y + gLayout.todayOffset));

  // Rain status line (size 1): RAIN NOW (+ forecast end) / RAIN NEARBY / next period / none.
  // While it is raining now or nearby, future-only messages are suppressed so the
  // line never says "Rain expected ..." while rain is actually falling. The shared
  // builder (buildRainLine) also serves themes 1-3.
  char rbuf[48];
  buildRainLine(rbuf, sizeof(rbuf));
  gT->setTextSize(1);
  lw = gT->textWidth(rbuf);
  gT->setTextSize(t.sx);
  gT->setTextColor(TFT_GREENYELLOW);   // displays dark blue on this panel
  gT->drawString(rbuf, t.X((zoneRight - lw) / 2), t.Y(y + gLayout.rainOffset));

  // Last update time ("Updated H:MM:SS AM/PM", local time)
  if (wx.lastUpdateEpoch > 1700000000UL) {
    char tbuf[16];
    struct tm lt;
    time_t local = wx.lastUpdateEpoch + localOffsetSec(wx.lastUpdateEpoch);
    localtime_r(&local, &lt);
    formatUpdTime(tbuf, sizeof(tbuf), &lt, true);
    snprintf(line, sizeof(line), "Updated %s", tbuf);
    gT->setTextSize(1);
    lw = gT->textWidth(line);
    gT->setTextSize(t.sx);
    gT->setTextColor(TFT_DARKGRAY);
    gT->drawString(line, t.X((zoneRight - lw) / 2), t.Y(y + gLayout.updatedOffset));
  }
}

static void drawTouchHint(const T& t) {
  const char* buf = "touch: refresh";
  gT->setTextSize(1);
  int w = gT->textWidth(buf);
  gT->setTextSize(t.sx);
  gT->setTextColor(TFT_DARKGRAY);
  gT->drawString(buf, t.X((320 - w) / 2), t.Y(240 - gLayout.hintBottomGap));
}

// ---------------------------------------------------------------------------
//  Theme renderers – one per theme. T::ID for live display; a half-scale T
//  plus clip rect inside the picker's preview window.
// ---------------------------------------------------------------------------

static void tempOrDash(char* buf, size_t n, bool valid, float v) {
  if (!valid || isnan(v)) snprintf(buf, n, "--.-");
  else                    buildTempStr(buf, n, v);
}

// Number-only temperature string for layouts that draw the unit separately as a
// small glyph (buildTempStr already includes it - using it there printed "F"/"C" twice).
static void tempNumOnly(char* buf, size_t n, bool valid, float v) {
  if (!valid || isnan(v)) snprintf(buf, n, "--.-");
  else                    snprintf(buf, n, "%.1f", tempForDisplay(v));
}

// Sky decorations (sun/moon + clouds/stars) for a theme; shared by the full
// renderer and the per-second partial update so they never drift apart.
static void drawSkyDecor(int idx, const T& t) {
  if (idx == 1) {   // SKY: sun top-right / moon+stars at night
    if (!nightMode()) {
      drawSun(t, 284, 48, C(0xff,0xe9,0xa8), C(0xff,0xcf,0x4d), C(0xf0,0xb4,0x29));
      drawCloud(t, 170, 34, 1.0f, C(0xff,0xff,0xff));
      drawCloud(t, 44, 52, 1.0f, C(0xff,0xff,0xff));   // second cloud: one cloud-width (52px) further left + lower, behind clock
    } else {
      static const int stars[][2] = {{24,34},{52,48},{76,28},{244,30},{268,44},{296,26},
                                     {308,52},{238,56},{232,74},{258,90},{288,70},{310,94}};
      for (auto& s : stars) drawStar(t, s[0], s[1], C(0xbc,0xd7,0xff));
    }
  } else if (idx == THEME_SPLIT) {   // SPLIT: flat sun/moon + panel clouds on the right
    if (!nightMode()) {
      drawFlatSun(t, 267, 23);                       // mockup skyicon: top:14, 18px
      drawSplitCloud(t, 238, 64, false, C(0xff,0xff,0xff));
      drawSplitCloud(t, 270, 86, true, blendWhite(bgCode(THEME_SPLIT, 290, 88), 75));   // pc2 @ .75 opacity
    } else {
      gT->fillCircle(t.X(267), t.Y(22), (int)(6 * t.sx + 0.5f), C(0xff,0xc8,0x57));
      gT->fillCircle(t.X(267) + (int)(3 * t.sx), t.Y(22) - (int)(2 * t.sx), (int)(5 * t.sx + 0.5f), bgCode(THEME_SPLIT, 267, 22));
      static const int stars[][2] = {{226,58},{300,52},{270,74},{292,88}};
      for (auto& s : stars) drawStar(t, s[0], s[1], C(0xbc,0xd7,0xff));
    }
  }
}

static void renderTheme0(const T& t) {   // BASIC (original layout)
  drawThemeBg(0, t);                     // == fillBackground()
  drawDogImage(t, 0);
  drawStatusBar(t);
  drawClockAndDate(t);
  drawWeatherPanel(t);
  drawTouchHint(t);
}

// ---- Nunito BFF fonts (generated by tools/make_nunito_bff.py) -------------------------
// Four fixed em sizes as compact 4bpp-alpha BFF blobs in flash. LGFX's runtime
// font slot holds one font at a time, so each master gets its own BFFfont and we
// draw with the explicit-font overloads (drawString/drawCenterString + IFont*).
static lgfx::BFFfont nunitoF9, nunitoF10, nunitoF12, nunitoF13, nunitoF20, nunitoF48;
static lgfx::PointerWrapper nunitoW9(nunito_em9_data, sizeof(nunito_em9_data));
static lgfx::PointerWrapper nunitoW10(nunito_em10_data, sizeof(nunito_em10_data));
static lgfx::PointerWrapper nunitoW12(nunito_em12_data, sizeof(nunito_em12_data));
static lgfx::PointerWrapper nunitoW13(nunito_em13_data, sizeof(nunito_em13_data));
static lgfx::PointerWrapper nunitoW20(nunito_em20_data, sizeof(nunito_em20_data));
static lgfx::PointerWrapper nunitoW48(nunito_em48_data, sizeof(nunito_em48_data));

static bool nunitoOK = false;
static void loadNunitoFonts() {
  nunitoOK = nunitoF9.loadFont(&nunitoW9) && nunitoF10.loadFont(&nunitoW10)
          && nunitoF12.loadFont(&nunitoW12) && nunitoF13.loadFont(&nunitoW13)
          && nunitoF20.loadFont(&nunitoW20) && nunitoF48.loadFont(&nunitoW48);
}

// Pixel width of a BFF string at scale 1 (sum of advances; default advance on miss).
static int bffWidth(const lgfx::IFont& f, const char* s) {
  lgfx::FontMetrics m;
  f.getDefaultMetric(&m);
  int w = 0;
  for (; *s; ++s) { (void)f.updateFontMetric(&m, (uint16_t)(unsigned char)*s); w += m.x_advance; }
  return w;
}

// Shrink-to-fit scale for a BFF string: min(1.0, avail / width).
static float bffFit(const lgfx::IFont& f, const char* s, int avail) {
  int w = bffWidth(f, s);
  return (w > avail && w > 0) ? (float)avail / (float)w : 1.0f;
}

// BFF text at full-space line-box top-left (x,y), scale sc, color c.
// `back` = RAW code of the background under the glyphs. This fork's alpha renderer
// blends glyph edge pixels toward getBaseColor(); on this inverted panel the default
// base 0x0000 displays as WHITE, which put a soft white halo around every non-white
// glyph (the "blurry" lines). Passing the real background makes edges anti-alias
// correctly. Default 0 keeps legacy behavior for callers that don't pass it.
static void bffText(const T& t, const lgfx::IFont& f, const char* s, int x, int y, uint16_t c, float sc = 1.0f, uint16_t back = 0) {
  gT->setBaseColor(back);
  gT->setTextSize(sc * t.sx);
  gT->setTextColor(c);
  gT->drawString(s, t.X(x), t.Y(y), &f);
}

// BFF text centered on full-space x=cx (line-box top at y).
static void bffCenter(const T& t, const lgfx::IFont& f, const char* s, int cx, int y, uint16_t c, float sc = 1.0f, uint16_t back = 0) {
  gT->setBaseColor(back);
  gT->setTextSize(sc * t.sx);
  gT->setTextColor(c);
  gT->drawCenterString(s, t.X(cx), t.Y(y), &f);
}

// Theme-1 dynamic band (status row + big clock + date line), full-space coords.
// Shared by the full render and updateDynamicTexts() so they never drift apart.
static void drawSkyDynamicBand(const T& t) {
  const SkyPal& p = SKY();
  if (!nunitoOK) {   // original bitmap-font band (legacy behavior)
    drawStatusRow(t, 10, 7, 1.2f, p.stL, p.dot, dayNightLabel(), p.stR, 310, true);
    char clockBuf[16], dateBuf[32];
    buildClockDateStrings(clockBuf, sizeof(clockBuf), dateBuf, sizeof(dateBuf));
    drawCenter(t, 160, 19, clockBuf, themeClockFit(1), p.clock, 316);
    drawCenter(t, 160, 62, dateBuf, 1.5f, p.date, 300);
    return;
  }
  // Status row: dot + "WIFI NTP" (em10) left; sun/moon + DAY/NIGHT right of X(310).
  char status[24]; buildWifiStatus(status, sizeof(status));
  int dr = max(1, (int)(2.5f * t.sx + 0.5f));
  gT->fillCircle(t.X(10) + dr, t.Y(7 + 4), dr, p.dot);
  bffText(t, nunitoF10, status, (int)(10 + 8 * 1.2f), 7, p.stL, 1.0f, bgCode(1, 0, 7));
  const char* dn = dayNightLabel();
  int rw = bffWidth(nunitoF10, dn);
  bffText(t, nunitoF10, dn, 310 - rw, 7, p.stR, 1.0f, bgCode(1, 0, 7));
  {
    int icx = 310 - rw - (int)(9 * 1.2f);
    drawDnIcon(t, icx, 7 + 4 * 1.2f, p.stR, bgCode(1, icx, 7 + 4 * 1.2f));
  }

  // Clock: centered at x=160 (em48). Date: centered at x=160 (em13).
  char clockBuf[16], dateBuf[32];
  buildClockDateStrings(clockBuf, sizeof(clockBuf), dateBuf, sizeof(dateBuf));
  bffCenter(t, nunitoF48, clockBuf, 160, 19, p.clock, bffFit(nunitoF48, clockBuf, 316), bgCode(1, 0, 19));
  bffCenter(t, nunitoF13, dateBuf, 160, 62, p.date, bffFit(nunitoF13, dateBuf, 300), bgCode(1, 0, 62));
}

static void renderTheme1Legacy(const T& t) {   // SKY - original bitmap-font layout
  const SkyPal& p = SKY();
  drawThemeBg(1, t);
  drawSkyDecor(1, t);

  drawStatusRow(t, 10, 7, 1.2f, p.stL, p.dot, dayNightLabel(), p.stR, 310, true);

  char clockBuf[16], dateBuf[32];
  buildClockDateStrings(clockBuf, sizeof(clockBuf), dateBuf, sizeof(dateBuf));
  float cs = themeClockFit(1);
  gT->setTextSize(cs); int cw = gT->textWidth(clockBuf);
  gT->setTextSize(cs * t.sx); gT->setTextColor(p.clock);
  gT->drawString(clockBuf, t.X((320 - cw) / 2.0f), t.Y(19));
  drawCenter(t, 160, 62, dateBuf, 1.5f, p.date, 300);

  char tmp[16]; tempNumOnly(tmp, sizeof(tmp), wx.valid, wx.temp);
  gT->setTextSize(2.5f); int uw = gT->textWidth(tempUnitChar());
  float ts = fitSize(tmp, 6.0f, max(40, 204 - uw - 10));
  gT->setTextSize(ts); int tw = gT->textWidth(tmp);
  gT->setTextSize(ts * t.sx); gT->setTextColor(p.temp);
  gT->drawString(tmp, t.X(14), t.Y(76));
  gT->setTextSize(2.5f * t.sx); gT->setTextColor(p.unit);
  gT->drawString(tempUnitChar(), t.X(14 + tw + 6), t.Y(76 + 8 * ts - 8 * 2.5f));

  const char* desc = wx.valid ? wx.description.c_str() : "No weather data";
  drawCloud(t, 16, 132, 0.38f, p.desc);   // was white - invisible on the day sky
  float dsc = fitSize(desc, 1.75f, 218 - 40);
  gT->setTextSize(dsc * t.sx); gT->setTextColor(p.desc);
  gT->drawString(desc, t.X(40), t.Y(129));

  char tiny[64], upd[24];
  buildUpdShort(upd, sizeof(upd));
  if (wx.valid)
    // Single spaces: with triple spaces the Fahrenheit string (two-digit temps)
    // overflowed the 204px zone and fitSize shrank it to an unreadable size.
    snprintf(tiny, sizeof(tiny), "HUM %d%% H %.0f L %.0f%s%s",
             wx.humidity, tempForDisplay(wx.tMax), tempForDisplay(wx.tMin), upd[0] ? " " : "", upd);
  else
    snprintf(tiny, sizeof(tiny), "NO WEATHER DATA");
  drawCenter(t, 116, 148, tiny, 1.0f, p.tiny, 204);

  char feels[16]; tempOrDash(feels, sizeof(feels), wx.valid, wx.feelsLike);
  drawFeelsChip(t, 14, 162, 204, 30, p.fBg, p.fBd, 1.25f, p.fLbl, feels, 2.25f, p.fVal);

  char rain[48]; buildRainLine(rain, sizeof(rain));
  bool rainy = wx.valid && (wx.rainingNow || wx.rainNearby || wx.nextRain[0]);
  uint16_t iconC = rainy ? (nightMode() ? C(0x7c,0xc0,0xff) : C(0x2f,0x7f,0xd0))   // umbrella
                         : (wx.valid    ? p.rDot : C(0x9d,0xb4,0xc8));             // yellow circle / neutral dot
  drawRainChip(t, 14, 198, 204, 28, p.rBg, p.rBd, rain, 1.6f, p.rTxt, iconC, rainy);

  drawCenter(t, 115, 230, "TOUCH TO REFRESH", 1.2f, p.hint, 204);
  drawDogImage(t, 1);
}

static void renderTheme1(const T& t) {   // SKY (mockup1) - Nunito BFF fonts
  if (!nunitoOK) { renderTheme1Legacy(t); return; }
  const SkyPal& p = SKY();
  drawThemeBg(1, t);
  drawSkyDecor(1, t);
  drawSkyDynamicBand(t);   // status row + clock + date

  // Temperature + unit: left x=14 (em48 digits / em20 unit), baselines aligned.
  char tmp[16]; tempNumOnly(tmp, sizeof(tmp), wx.valid, wx.temp);
  float ts = bffFit(nunitoF48, tmp, max(40, 204 - bffWidth(nunitoF20, "C") - 10));
  int tw = (int)(bffWidth(nunitoF48, tmp) * ts);
  bffText(t, nunitoF48, tmp, 14, 76, p.temp, ts, bgCode(1, 0, 76));
  bffText(t, nunitoF20, tempUnitChar(), 14 + tw + 6,
          (int)(76 + (nunitoF48.ascent - nunitoF20.ascent) * ts), p.unit, 1.0f, bgCode(1, 0, 95));

  // Cloud icon + description text (em20).
  const char* desc = wx.valid ? wx.description.c_str() : "No weather data";
  drawCloud(t, 16, 132, 0.38f, p.desc);
  bffText(t, nunitoF20, desc, 40, 129, p.desc, bffFit(nunitoF20, desc, 218 - 40), bgCode(1, 0, 129));

  // Tiny row: centered x=116 (em10).
  char tiny[64], upd[24];
  buildUpdShort(upd, sizeof(upd));
  if (wx.valid)
    // Single spaces: with triple spaces the Fahrenheit string (two-digit temps)
    // overflowed the 204px zone and fitSize shrank it to an unreadable size.
    snprintf(tiny, sizeof(tiny), "HUM %d%% H %.0f L %.0f%s%s",
             wx.humidity, tempForDisplay(wx.tMax), tempForDisplay(wx.tMin), upd[0] ? " " : "", upd);
  else
    snprintf(tiny, sizeof(tiny), "NO WEATHER DATA");
  bffCenter(t, nunitoF10, tiny, 116, 148, p.tiny, bffFit(nunitoF10, tiny, 204), bgCode(1, 0, 148));

  // Feels-like chip (em10 label / em20 value).
  char feels[16]; tempOrDash(feels, sizeof(feels), wx.valid, wx.feelsLike);
  gT->fillRoundRect(t.X(14), t.Y(162), max(2, t.W(204)), max(2, t.H(30)), (int)(9 * t.sx + 0.5f), p.fBg);
  gT->drawRoundRect(t.X(14), t.Y(162), max(1, t.W(204) - 1), max(1, t.H(30) - 1), (int)(8 * t.sx + 0.5f), p.fBd);
  bffText(t, nunitoF10, "FEELS LIKE", 14 + 12, 172, p.fLbl, 1.0f, p.fBg);
  float fvs = bffFit(nunitoF20, feels, max(8, 204 - 24 - bffWidth(nunitoF10, "FEELS LIKE")));
  int vw = (int)(bffWidth(nunitoF20, feels) * fvs);
  bffText(t, nunitoF20, feels, 14 + 204 - 12 - vw, 168, p.fVal, fvs, p.fBg);

  // Rain chip (em13).
  char rain[48]; buildRainLine(rain, sizeof(rain));
  bool rainy = wx.valid && (wx.rainingNow || wx.rainNearby || wx.nextRain[0]);
  uint16_t iconC = rainy ? (nightMode() ? C(0x7c,0xc0,0xff) : C(0x2f,0x7f,0xd0))   // umbrella
                         : (wx.valid    ? p.rDot : C(0x9d,0xb4,0xc8));             // yellow circle / neutral dot
  gT->fillRoundRect(t.X(14), t.Y(198), max(2, t.W(204)), max(2, t.H(28)), (int)(8 * t.sx + 0.5f), p.rBg);
  gT->drawRoundRect(t.X(14), t.Y(198), max(1, t.W(204) - 1), max(1, t.H(28) - 1), (int)(7 * t.sx + 0.5f), p.rBd);
  float rsc = bffFit(nunitoF13, rain, 204 - 24);
  int rw2 = (int)(bffWidth(nunitoF13, rain) * rsc);
  int cxFull = 14 + 204 / 2;
  if (rainy)
    drawUmbrella(t, (int)(cxFull - rw2 / 2.0f - 9), 198 + 14, 1.0f, iconC, p.rBg);
  else {
    int dr2 = max(1, (int)(3 * 1.2f * t.sx + 0.5f));
    gT->fillCircle(t.X((int)(cxFull - rw2 / 2.0f - 9)), t.Y(198 + 14), dr2, iconC);
  }
  bffText(t, nunitoF13, rain, (int)(cxFull - rw2 / 2.0f), 206, p.rTxt, rsc, p.rBg);

  // Hint: centered (em10).
  bffCenter(t, nunitoF10, "TOUCH TO REFRESH", 115, 230, p.hint, 1.0f, bgCode(1, 0, 230));
  drawDogImage(t, 1);
}

// Theme-2 dynamic band (clock + date), full-space coords. Shared by the full
// render and updateDynamicTexts() so they never drift apart; the per-second path
// clears only this band -> no full-screen flash like theme 1 (SKY).
static float splitClockScale(const char* s) {   // mockup: font-size 37px -> em48 @ 37/48
  float sc = bffFit(nunitoF48, s, 198);
  return (sc < 37.0f / 48.0f) ? sc : 37.0f / 48.0f;
}
// Mockup date is 10.5px: rendered with the em10 master at scale 1.0 (10px).
// Deliberately NOT em13@0.81 - any sub-1.0 BFF scale runs LGFX's nearest-
// neighbor scaler, which drops 1px source columns and garbles thin strokes.
static float splitDateScale(const char* s) {
  return bffFit(nunitoF10, s, 201);
}
static void drawSplitClockDate(const T& t) {
  const SplitPal& p = SPLIT();
  char clockBuf[16], dateBuf[32];
  buildClockDateStrings(clockBuf, sizeof(clockBuf), dateBuf, sizeof(dateBuf));
  if (!nunitoOK) {   // original bitmap-font band (legacy behavior)
    float cs = themeClockFit(THEME_SPLIT);
    gT->setTextSize(cs * t.sx); gT->setTextColor(p.clock);
    gT->drawString(clockBuf, t.X(12), t.Y(24));
    float ds = fitSize(dateBuf, 1.3f, 201);
    gT->setTextSize(ds * t.sx); gT->setTextColor(p.date);
    gT->drawString(dateBuf, t.X(13), t.Y(64));
    return;
  }
  bffText(t, nunitoF48, clockBuf, 12, 24, p.clock, splitClockScale(clockBuf), p.left);
  bffText(t, nunitoF10, dateBuf, 13, 64, p.date, splitDateScale(dateBuf), p.left);
}

static void renderTheme2(const T& t) {   // SPLIT (mockup3)
  const SplitPal& p = SPLIT();
  drawThemeBg(THEME_SPLIT, t);
  drawSkyDecor(THEME_SPLIT, t);

  // DAY/NIGHT label + ground ellipse on the panel
  if (!nunitoOK) drawCenter(t, 267, 38, dayNightLabel(), 1.2f, p.skyLbl, 100);
  else           bffCenter(t, nunitoF10, dayNightLabel(), 267, 38, p.skyLbl, 1.0f, bgCode(THEME_SPLIT, 267, 43));
  gT->fillEllipse(t.X(267), t.Y(231), max(1, (int)(43 * t.sx + 0.5f)),
                  max(1, (int)(5 * t.sy + 0.5f)), p.ground);

  if (!nunitoOK) {   // original bitmap-font layout (legacy behavior)
    drawStatusRow(t, 12, 9, 1.2f, p.stL, p.dot, nullptr, 0, 0);
    drawSplitClockDate(t);
    char tmp[16]; tempNumOnly(tmp, sizeof(tmp), wx.valid, wx.temp);
    gT->setTextSize(2.4f); int uw = gT->textWidth(tempUnitChar());
    float ts = fitSize(tmp, 5.4f, max(40, 202 - uw - 10));
    gT->setTextSize(ts); int tw = gT->textWidth(tmp);
    gT->setTextSize(ts * t.sx); gT->setTextColor(p.temp);
    gT->drawString(tmp, t.X(12), t.Y(80));
    gT->setTextSize(2.4f * t.sx); gT->setTextColor(p.unit);
    gT->drawString(tempUnitChar(), t.X(12 + tw + 6), t.Y(80 + 8 * ts - 8 * 2.4f));

    const char* desc = wx.valid ? wx.description.c_str() : "No weather data";
    drawCloud(t, 13, 131, 0.5f, nightMode() ? C(0xff,0xff,0xff) : C(0x9b,0xc4,0xe8));
    float dsc = fitSize(desc, 1.5f, 214 - 36);
    gT->setTextSize(dsc * t.sx); gT->setTextColor(p.descTxt);
    gT->drawString(desc, t.X(36), t.Y(128));

    char feels[16]; tempOrDash(feels, sizeof(feels), wx.valid, wx.feelsLike);
    // Chips span 12..225 (mockup .tile is content-box: 190 + 20 padding + border),
    // so they extend ~10px past the 214 panel line.
    drawFeelsChip(t, 12, 146, 213, 32, p.fBg, p.fBd, 1.1f, p.fLbl, feels, 2.2f, p.fVal);

    char rain[48]; buildRainLine(rain, sizeof(rain));
    drawRainChip(t, 12, 184, 213, 27, p.rBg, p.rBd, rain, 1.4f, p.rTxt, p.rDot);

    char left[48], right[24];   // GLCD font has no \xB7 glyph: use spaces
    if (wx.valid) snprintf(left, sizeof(left), "HUM %d%%  H %.1f L %.1f",
                           wx.humidity, tempForDisplay(wx.tMax), tempForDisplay(wx.tMin));
    else          snprintf(left, sizeof(left), "NO WEATHER DATA");
    buildUpdShort(right, sizeof(right));
    drawSplitRow(t, 12, 217, 202, left, right, 1.0f, p.row1C);

    drawCenter(t, 107, 229, "TOUCH TO REFRESH", 1.0f, p.hint, 196);
    drawDogImage(t, THEME_SPLIT);
    return;
  }

  // Status row: green dot (5px) + "WIFI NTP" (em10 @10px; mockup 9.5px).
  char status[24]; buildWifiStatus(status, sizeof(status));
  int dr = max(1, (int)(2 * t.sx + 0.5f));
  gT->fillCircle(t.X(12) + dr, t.Y(9 + 5), dr, p.dot);
  bffText(t, nunitoF10, status, 22, 9, p.stL, 1.0f, p.left);

  drawSplitClockDate(t);

  // Temperature (em48 @43px) + unit "\xB0F" (em20 @19px), baselines aligned.
  char tmp[16]; tempNumOnly(tmp, sizeof(tmp), wx.valid, wx.temp);
  float ts = bffFit(nunitoF48, tmp, 170);
  if (ts > 43.0f / 48.0f) ts = 43.0f / 48.0f;
  bffText(t, nunitoF48, tmp, 12, 80, p.temp, ts, p.left);
  int tw = (int)(bffWidth(nunitoF48, tmp) * ts + 0.5f);
  bffText(t, nunitoF20, tempUnitChar(), 12 + tw + 6,
          (int)(80 + nunitoF48.ascent * ts - nunitoF20.ascent * 0.95f), p.unit, 0.95f, p.left);

  // Description row: cloud icon (14px) + text (em12 @12px = mockup size), mockup left:13 top:128.
  const char* desc = wx.valid ? wx.description.c_str() : "No weather data";
  drawMiniCloud(t, 13, 129, p.descTxt);
  float dsc = bffFit(nunitoF12, desc, 214 - 32);
  bffText(t, nunitoF12, desc, 32, 128, p.descTxt, dsc, p.left);

  // FEELS chip: mockup .tile{left:12;width:190px;padding:0 10px;border:1.5px} is
  // content-box -> outer box 12..225 (213 wide), so the 214 panel line runs ~10px
  // BEHIND the chip's right edge (chip is painted after the bg, covering the line).
  // Icon @x=24 (12+1.5+10=23.5), label @x=46 (icon box 14 + gap 8), value
  // em20@18px right-aligned at x=213 (225-1.5border-10padding=213.5).
  char feels[16]; tempOrDash(feels, sizeof(feels), wx.valid, wx.feelsLike);
  gT->fillRoundRect(t.X(12), t.Y(146), max(2, t.W(213)), max(2, t.H(32)), (int)(9 * t.sx + 0.5f), p.fBg);
  gT->drawRoundRect(t.X(12), t.Y(146), max(1, t.W(213) - 1), max(1, t.H(32) - 1), (int)(8 * t.sx + 0.5f), p.fBd);
  int ix = 24, iy = 146 + (32 - 14) / 2;   // icon box 14px, vertically centred
  gT->drawLine(t.X(ix + 5), t.Y(iy + 1), t.X(ix + 5), t.Y(iy + 9), p.fIcon);
  gT->drawLine(t.X(ix + 2), t.Y(iy + 2), t.X(ix + 8), t.Y(iy + 7), p.fIcon);
  gT->drawLine(t.X(ix + 8), t.Y(iy + 2), t.X(ix + 2), t.Y(iy + 7), p.fIcon);
  bffText(t, nunitoF9, "FEELS LIKE", 46, 146 + (32 - 9) / 2, p.fLbl, 1.0f, p.fBg);
  // feels already ends with the unit char ("32.7 F") — do NOT append tempUnitChar() again.
  float fvs = bffFit(nunitoF20, feels, 213 - 84);
  if (fvs > 0.9f) fvs = 0.9f;
  int vw = (int)(bffWidth(nunitoF20, feels) * fvs + 0.5f);
  bffText(t, nunitoF20, feels, 213 - vw, 146 + (32 - (int)(20 * fvs)) / 2, p.fVal, fvs, p.fBg);

  // RAIN chip [12,184] 213 wide: same outer box as the feels chip (content-box).
  // Umbrella icon + text (em13 @11.5px) centered as a group.
  char rain[48]; buildRainLine(rain, sizeof(rain));
  gT->fillRoundRect(t.X(12), t.Y(184), max(2, t.W(213)), max(2, t.H(27)), (int)(8 * t.sx + 0.5f), p.rBg);
  gT->drawRoundRect(t.X(12), t.Y(184), max(1, t.W(213) - 1), max(1, t.H(27) - 1), (int)(7 * t.sx + 0.5f), p.rBd);
  // em12 @1.0 (mockup 11.5px): sub-1.0 BFF scaling decimates 1px strokes.
  float rsc = bffFit(nunitoF12, rain, 213 - 40);
  int rw = (int)(bffWidth(nunitoF12, rain) * rsc + 0.5f);
  int gx = 12 + (213 - (13 + 6 + rw)) / 2;     // group: umbrella(13px) + gap(6px) + text
  drawUmbrella(t, gx + 6, 184 + 13, 1.0f, p.rDot, p.rBg);
  bffText(t, nunitoF12, rain, gx + 19, 184 + (27 - (int)(12 * rsc)) / 2, p.rTxt, rsc, p.rBg);

  // row1 (mockup top:217 width:190): left text @x=12; UPD right-aligned at x=202.
  // em9 @1.0 = mockup's 9px (em10@0.85 decimated and garbled thin strokes).
  char left[48], right[24];
  if (wx.valid) snprintf(left, sizeof(left), "HUM %d%% \xB7 H %.1f\xB0 L %.1f\xB0",
                         wx.humidity, tempForDisplay(wx.tMax), tempForDisplay(wx.tMin));
  else          snprintf(left, sizeof(left), "NO WEATHER DATA");
  buildUpdShort(right, sizeof(right));
  float lsc = bffFit(nunitoF9, left, 202 - 12);
  bffText(t, nunitoF9, left, 12, 217, p.row1C, lsc, p.left);
  int rwid = (int)(bffWidth(nunitoF9, right) + 0.5f);
  bffText(t, nunitoF9, right, 202 - rwid, 217, p.row1C, 1.0f, p.left);

  bffCenter(t, nunitoF9, "TOUCH TO REFRESH", 107, 229, p.hint, 1.0f, p.left);
  drawDogImage(t, THEME_SPLIT);
}

// ---------------------------------------------------------------------------
//  Theme 3 - BENTO (mockup4).
//
//  The clock card is its own function: updateDynamicTexts() repaints just that
//  card once per minute instead of re-rendering the whole screen every second
//  (the full rewrite is what the panel showed as a visible flash).
// ---------------------------------------------------------------------------

// Clock card (8,8,304,52): status row + big clock + date. Nunito BFF front
// (the SKY theme's font) when available, original bitmap fonts otherwise.
static void drawBentoClockCard(const T& t) {
  const BentoPal& p = BENTO();
  drawCard(t, 8, 8, 304, 52, p.cardBg, p.cardBd);

  char clockBuf[16], dateBuf[32];
  buildClockDateStrings(clockBuf, sizeof(clockBuf), dateBuf, sizeof(dateBuf));

  if (nunitoOK) {
    // Status row (mockup y=25): dot 5px + "WIFI NTP" (em9); sun/moon +
    // DAY/NIGHT right-aligned to x=304
    char wifi[24]; buildWifiStatus(wifi, sizeof(wifi));
    gT->fillCircle(t.X(18), t.Y(27), max(1, (int)(2.5f * t.sx + 0.5f)), p.dot);
    bffText(t, nunitoF9, wifi, 24, 25, p.stL, 1.0f, p.cardBg);
    const char* dn = dayNightLabel();
    int dw = bffWidth(nunitoF9, dn);
    bffText(t, nunitoF9, dn, 304 - dw, 25, p.stR, 1.0f, p.cardBg);
    drawDnIcon(t, 304 - dw - 9, 29, p.stR, p.cardBg);

    // Clock (em48 at the mockup's 27px) and date (em10, mockup ~9.5px)
    float cs = bffFit(nunitoF48, clockBuf, 290);
    if (cs > 27.0f / 48.0f) cs = 27.0f / 48.0f;
    bffCenter(t, nunitoF48, clockBuf, 160, 11, p.clock, cs, p.cardBg);
    bffCenter(t, nunitoF10, dateBuf, 160, 41, p.date, bffFit(nunitoF10, dateBuf, 288), p.cardBg);
  } else {   // original bitmap-font card
    float cs = themeClockFit(THEME_BENTO);
    gT->setTextSize(cs); int cw = gT->textWidth(clockBuf);
    gT->setTextSize(cs * t.sx); gT->setTextColor(p.clock);
    gT->drawString(clockBuf, t.X((160 - cw) / 2.0f), t.Y(11));   // centered in [8,312]
    drawCenter(t, 160, 41, dateBuf, 1.0f, p.date, 288);

    char wifi[24]; buildWifiStatus(wifi, sizeof(wifi));
    gT->setTextSize(1 * t.sx);
    int dr = max(1, (int)(2.5f * t.sx + 0.5f));
    gT->fillCircle(t.X(16) + dr, t.Y(29), dr, p.dot);
    gT->setTextColor(p.stL);
    gT->drawString(wifi, t.X(24), t.Y(25));
    drawRight(t, 304, 25, dayNightLabel(), 1.0f, p.stR);
  }

  // Keep the partial-update cache in sync so a per-second tick right after a
  // full render does not cause a redundant repaint.
  strncpy(lastClockBuf, clockBuf, sizeof(lastClockBuf) - 1);
  lastClockBuf[sizeof(lastClockBuf) - 1] = '\0';
  strncpy(lastDateBuf, dateBuf, sizeof(lastDateBuf) - 1);
  lastDateBuf[sizeof(lastDateBuf) - 1] = '\0';
}

// Dog card (206,66,106,166): caption + dog centered in the card above it.
static void drawBentoDogCard(const T& t) {
  const BentoPal& p = BENTO();
  drawCard(t, 206, 66, 106, 166, p.dogCardBg, p.cardBd);
  if (nunitoOK) {   // "OUTFIT · BAND" (mockup); plain band label if it does not fit
    char cap[32];
    snprintf(cap, sizeof(cap), "OUTFIT \xB7 %s", dogBandLabel());
    if (bffWidth(nunitoF9, cap) > 96) snprintf(cap, sizeof(cap), "%s", dogBandLabel());
    bffCenter(t, nunitoF9, cap, 259, 207, p.capC, 1.0f, p.dogCardBg);
  } else {
    drawCenter(t, 259, 207, dogBandLabel(), 1.0f, p.capC, 96);
  }
  int fw = dogWidth();
  if (fw > 0) {
    int fh = dogHeight();
    int xFull = 206 + (106 - fw) / 2;
    int yTopFull = 70 + (134 - fh) / 2;   // centered in [70,204], above the caption
    drawDogBox(t, xFull, yTopFull, fw, fh, THEME_BENTO);
  }
}

// BENTO with the original bitmap fonts (fallback when the Nunito BFF load fails).
static void renderTheme3Legacy(const T& t) {
  const BentoPal& p = BENTO();
  drawThemeBg(THEME_BENTO, t);

  drawBentoClockCard(t);                        // clock card
  drawCard(t, 8, 66, 192, 72, p.cardBg, p.cardBd);   // temp card
  drawCard(t, 8, 184, 192, 48, p.cardBg, p.cardBd);  // detail card
  drawBentoDogCard(t);                          // dog card
  gT->fillRoundRect(t.X(7), t.Y(74), max(1, t.W(4)), max(1, t.H(56)),
                    (int)(2 * t.sx + 0.5f), p.accent);   // bar on temp card (mockup .accent)

  char tmp[16]; tempNumOnly(tmp, sizeof(tmp), wx.valid, wx.temp);
  gT->setTextSize(2.0f); int uw = gT->textWidth(tempUnitChar());
  float ts = fitSize(tmp, 4.25f, max(30, 108 - uw - 8));
  gT->setTextSize(ts); int tw = gT->textWidth(tmp);
  gT->setTextSize(ts * t.sx); gT->setTextColor(p.temp);
  gT->drawString(tmp, t.X(24), t.Y(74));
  gT->setTextSize(2.0f * t.sx); gT->setTextColor(p.unit);
  gT->drawString(tempUnitChar(), t.X(24 + tw + 5), t.Y(74 + 8 * ts - 16));

  // FEELS chip [132,74,60,40]
  char feels[16]; tempOrDash(feels, sizeof(feels), wx.valid, wx.feelsLike);
  gT->fillRoundRect(t.X(132), t.Y(74), max(2, t.W(60)), max(2, t.H(40)), (int)(8 * t.sx + 0.5f), p.chipBg);
  gT->drawRoundRect(t.X(132), t.Y(74), max(1, t.W(60) - 1), max(1, t.H(40) - 1), (int)(7 * t.sx + 0.5f), p.chipBd);
  drawCenter(t, 162, 82, "FEELS", 1.0f, p.chipLbl, 52);
  float fv = fitSize(feels, 2.0f, 52);
  gT->setTextSize(fv); int fw2 = gT->textWidth(feels);
  gT->setTextSize(fv * t.sx); gT->setTextColor(p.chipVal);
  gT->drawString(feels, t.X(162 - fw2 / 2.0f), t.Y(96));

  // Description row inside the temp card: mini cloud + text
  const char* desc = wx.valid ? wx.description.c_str() : "No weather data";
  drawCloud(t, 25, 114, 0.38f, nightMode() ? C(0xff,0xff,0xff) : C(0x9b,0xc4,0xe8));
  float dsc = fitSize(desc, 1.5f, 196 - 43);
  gT->setTextSize(dsc * t.sx); gT->setTextColor(p.descTxt);
  gT->drawString(desc, t.X(43), t.Y(112));

  // Rain card [8,144,192,34]
  char rain[48]; buildRainLine(rain, sizeof(rain));
  drawRainChip(t, 8, 144, 192, 34, p.rBg, p.rBd, rain, 1.6f, p.rTxt, p.rDot);

  // Detail card rows
  char d1[32], d2[32];
  snprintf(d1, sizeof(d1), wx.valid ? "HUMIDITY %d%%" : "HUMIDITY --", wx.humidity);
  if (wx.valid) snprintf(d2, sizeof(d2), "TODAY H %.0f L %.0f",
                         tempForDisplay(wx.tMax), tempForDisplay(wx.tMin));
  else          snprintf(d2, sizeof(d2), "NO WEATHER DATA");
  gT->setTextSize(1 * t.sx); gT->setTextColor(p.detC);
  gT->drawString(d1, t.X(22), t.Y(194));
  gT->drawString(d2, t.X(22), t.Y(207));
  char upd[32]; buildUpdShort(upd, sizeof(upd));
  String updl = String(upd); if (updl.length()) updl += " - REFRESH";
  float us = fitSize(updl.c_str(), 0.85f, 174);
  gT->setTextSize(us * t.sx); gT->setTextColor(p.updC);
  gT->drawString(updl.c_str(), t.X(22), t.Y(221));
}

// BENTO with the Nunito BFF front (the SKY theme's font): mockup typography -
// 27px clock, ~9.5px date, 34px temp value, em9/em10/em12/em13/em20 for the rest.
static void renderTheme3Nunito(const T& t) {
  const BentoPal& p = BENTO();
  drawThemeBg(THEME_BENTO, t);

  drawBentoClockCard(t);                        // clock card

  // ---- Temp card (mockup 192x72): accent bar + 34px number + FEELS chip ----
  drawCard(t, 8, 66, 192, 72, p.cardBg, p.cardBd);
  gT->fillRoundRect(t.X(7), t.Y(74), max(1, t.W(4)), max(1, t.H(56)),
                    (int)(2 * t.sx + 0.5f), p.accent);   // mockup .accent
  char tmp[16]; tempNumOnly(tmp, sizeof(tmp), wx.valid, wx.temp);
  float ts = bffFit(nunitoF48, tmp, 140);
  if (ts > 34.0f / 48.0f) ts = 34.0f / 48.0f;              // mockup .tempv = 34px
  bffText(t, nunitoF48, tmp, 24, 74, p.temp, ts, p.cardBg);
  float us = 15.0f / 20.0f;                                // mockup .tempu = 15px
  int t48w = (int)(bffWidth(nunitoF48, tmp) * ts + 0.5f);
  bffText(t, nunitoF20, tempUnitChar(), 24 + t48w + 6,
          (int)(74 + nunitoF48.ascent * ts - nunitoF20.ascent * us),
          p.unit, us, p.cardBg);

  // FEELS chip (mockup 60x40 at (132,74), r10): crossed-arrows icon (9px) +
  // "FEELS" (em9) over the value (em20 @ 14px).
  gT->fillRoundRect(t.X(132), t.Y(74), max(2, t.W(60)), max(2, t.H(40)),
                    (int)(10 * t.sx + 0.5f), p.chipBg);
  gT->drawRoundRect(t.X(132), t.Y(74), max(1, t.W(59)), max(1, t.H(39)),
                    (int)(9 * t.sx + 0.5f), p.chipBd);
  int ix = 162 - (9 + 3 + bffWidth(nunitoF9, "FEELS")) / 2;   // icon(9) + gap(3) + label
  int iy = 81;
  gT->drawLine(t.X(ix + 4), t.Y(iy + 1), t.X(ix + 4), t.Y(iy + 8), p.chipLbl);
  gT->drawLine(t.X(ix + 1), t.Y(iy + 2), t.X(ix + 8), t.Y(iy + 7), p.chipLbl);
  gT->drawLine(t.X(ix + 8), t.Y(iy + 2), t.X(ix + 1), t.Y(iy + 7), p.chipLbl);
  bffText(t, nunitoF9, "FEELS", ix + 9 + 3, 81, p.chipLbl, 1.0f, p.chipBg);
  char feels[16]; tempOrDash(feels, sizeof(feels), wx.valid, wx.feelsLike);
  float fvs = bffFit(nunitoF20, feels, 44);
  if (fvs > 14.0f / 20.0f) fvs = 14.0f / 20.0f;            // mockup .fval = 14px
  bffCenter(t, nunitoF20, feels, 162, 95, p.chipVal, fvs, p.chipBg);

  // Description row inside the temp card (mockup y=112): cloud + em12 text
  const char* desc = wx.valid ? wx.description.c_str() : "No weather data";
  drawMiniCloud(t, 25, 113, p.descTxt);
  bffText(t, nunitoF12, desc, 42, 112, p.descTxt, bffFit(nunitoF12, desc, 154), p.cardBg);

  // ---- Rain card (mockup 192x34, r10): dot/umbrella + em13 text, centered group ----
  gT->fillRoundRect(t.X(8), t.Y(144), t.W(192), t.H(34), (int)(10 * t.sx + 0.5f), p.rBg);
  gT->drawRoundRect(t.X(8), t.Y(144), max(1, t.W(191)), max(1, t.H(33)),
                    (int)(9 * t.sx + 0.5f), p.rBd);
  char rain[48]; buildRainLine(rain, sizeof(rain));
  float rsc = bffFit(nunitoF13, rain, 161);
  int rw = (int)(bffWidth(nunitoF13, rain) * rsc + 0.5f);
  bool wet = wx.rainingNow || (wx.rainNearby && wx.nearbyStns[0]) || wx.nextRain[0];
  int iw = wet ? 13 : 6;
  int gx = 8 + (192 - (iw + 7 + rw)) / 2;                  // mockup gap: 7px
  if (wet) drawUmbrella(t, gx + 6, 163, 1.0f, p.rDot, p.rBg);
  else gT->fillCircle(t.X(gx + 3), t.Y(161), max(1, (int)(3 * t.sx + 0.5f)), p.rDot);
  bffText(t, nunitoF13, rain, gx + iw + 7, 154, p.rTxt, rsc, p.rBg);

  // ---- Detail card rows (mockup: x=22, y=194/207/221) ----
  drawCard(t, 8, 184, 192, 48, p.cardBg, p.cardBd);
  char d1[32], d2[32];
  snprintf(d1, sizeof(d1), wx.valid ? "HUMIDITY %d%%" : "HUMIDITY --", wx.humidity);
  bffText(t, nunitoF10, d1, 22, 194, p.detC, 1.0f, p.cardBg);
  if (wx.valid)
    snprintf(d2, sizeof(d2), "TODAY H %.1f\xB0 L %.1f\xB0",   // mockup: "TODAY H 43.7° L 34.5°"
             tempForDisplay(wx.tMax), tempForDisplay(wx.tMin));
  else
    snprintf(d2, sizeof(d2), "NO WEATHER DATA");
  bffText(t, nunitoF10, d2, 22, 207, p.detC, 1.0f, p.cardBg);
  char upd[32], ref[64];
  buildUpdShort(upd, sizeof(upd));
  if (upd[0]) snprintf(ref, sizeof(ref), "%s \xB7 TOUCH TO REFRESH", upd);
  else        snprintf(ref, sizeof(ref), "TOUCH TO REFRESH");
  bffText(t, nunitoF9, ref, 22, 221, p.updC, 1.0f, p.cardBg);

  drawBentoDogCard(t);
}

static void renderTheme3(const T& t) {
  if (nunitoOK) renderTheme3Nunito(t);
  else          renderTheme3Legacy(t);
}

static void renderTheme(int idx, const T& t) {
  switch (idx) {
    case 0: renderTheme0(t); break;
    case 1: renderTheme1(t); break;
    case 2: renderTheme2(t); break;   // SPLIT
    default: renderTheme3(t); break;  // BENTO
  }
}

// LovyanGFX v1 draws in immediate mode – every call goes straight to the
// panel over SPI, so there is no separate framebuffer flush step.
static void renderScreen() {
  renderTheme(activeTheme, T::ID);
}

// ---------------------------------------------------------------------------
//  THEME picker screen
//
//  Half-scale preview of the selected theme (yellow border) on the normal
//  background, with green bars top and bottom:
//    top bar:    "THEME n/4: NAME"
//    bottom bar: "< PREV" ... "NEXT >"
//  Swipe left/right to change themes; tap (no swipe) selects.
// ---------------------------------------------------------------------------
// Picker layout (shared by the static render and the swipe animation so both
// stay in sync). The window is a true half-scale of the 320x240 dashboard.
static const int kPrevW = 160, kPrevH = 120;
static const int kPrevX = (320 - kPrevW) / 2, kPrevY = 60;
static const uint16_t kBarC    = C(0x2e,0x7d,0x32);   // displayed green (mockup topbar)
static const uint16_t kBorderC = C(0xff,0xd1,0x66);   // displayed yellow
// TEMP-DEBUG: per-frame timing print + boot auto-demo (remove after diagnosis)
static bool gTimingOn = false;

// Everything on the picker screen EXCEPT the preview window: background,
// yellow border, green bars with labels, hint line. idx is the theme whose
// name goes in the top bar (its neighbours fill the bottom bar).
static void drawPickerChrome(int idx) {
  fillBackground();   // normal day/night background behind the preview

  tft.drawRect(kPrevX - 1, kPrevY - 1, kPrevW + 2, kPrevH + 2, kBorderC);

  // Top bar: theme number + name
  tft.fillRect(0, 0, 320, 20, kBarC);
  char top[48];
  snprintf(top, sizeof(top), "THEME %d/%d: %s", idx + 1, THEME_COUNT, themeName(idx));  // 1-based for the user
  tft.setTextSize(1.5f);
  int tw = tft.textWidth(top);
  tft.setTextColor(TFT_BLACK);   // displays white on the green bar
  tft.drawString(top, (320 - tw) / 2, 4);

  // Hint in the gap above the preview
  const char* hint = "SWIPE LEFT/RIGHT - TAP TO SELECT";
  tft.setTextSize(1.0f);
  int hw = tft.textWidth(hint);
  tft.setTextColor(C(0x3d,0x5f,0x8f));
  tft.drawString(hint, (320 - hw) / 2, 40);

  // Bottom bar: previous name (left) + next name (right), with arrows
  int prevT = (idx + THEME_COUNT - 1) % THEME_COUNT;
  int nextT = (idx + 1) % THEME_COUNT;
  tft.fillRect(0, 220, 320, 20, kBarC);
  char l[48], r[48];
  snprintf(l, sizeof(l), "< %s", themeName(prevT));
  snprintf(r, sizeof(r), "%s >", themeName(nextT));
  tft.setTextSize(1.5f);
  int rw = tft.textWidth(r);
  tft.drawString(l, 8, 224);
  tft.drawString(r, 320 - 8 - rw, 224);
}

static void drawThemePicker() {
  drawPickerChrome(previewTheme);
  tft.setClipRect(kPrevX, kPrevY, kPrevW, kPrevH);
  T t{0.5f, 0.5f, kPrevX, kPrevY};
  renderTheme(previewTheme, t);                 // includes its own background fill
  tft.clearClipRect();
}

// ---------------------------------------------------------------------------
//  Swipe animation: full-screen-width slide.
//
//  The old preview card (half-scale theme, 160x120) slides ALL THE WAY to
//  the left edge (completely off-screen) while the new card slides in from
//  the right edge - or mirrored for "prev" - so the motion spans the entire
//  320 px screen width, not just the central preview box.  Only the preview
//  band (rows kPrevY..kPrevY+120, full width) is touched; the yellow border
//  frame, green bars and hint stay put (the left/right border columns are
//  redrawn after each frame so the card slides behind the fixed frame).
//
//  Both cards are pre-rendered ONCE into two 160x120 sprites (38.4 KB each);
//  every animation frame then:
//      1. repaints the whole band with the picker background (clearRegion)
//      2. blits the visible part of the old card (shrinking away)
//      3. blits the visible part of the new card (growing in)
//  so every pixel of the band is written by every frame - no stale pixels,
//  no ghosting, no flicker.
//
//  Card positions (off = 0..kTravel, kTravel = 240):
//      dir > 0 (next):  old x0 = kPrevX - off      (80  -> -160, exits left)
//                       new x0 = 320 - off         (320 -> 80,  enters right)
//      dir < 0 (prev):  old x0 = kPrevX + off      (80  -> 320, exits right)
//                       new x0 = off - kPrevW      (-160-> 80, enters left)
//  Each blit copies the visible intersection of card [x0, x0+160) with the
//  screen [0, 320): see the per-frame math below.
//
//  Sub-rect blits go through panel()->writeImage() with a manual pixelcopy_t
//  (src_bitwidth = full sprite width = 160, src_x = first visible column),
//  because LGFXBase::pushImage() would overwrite src_bitwidth with the sub-
//  rect width.  A 16-bit -> 16-bit same-depth copy is a raw row DMA transfer
//  (for DMA-capable buffers) - byte order matches push_sprite() exactly.
//
//  RAM: the ~36 KB dog PNG buffers stay loaded during the animation -
//  two card sprites (38.4 KB each) + the dog fit in the ~134 KB free heap
//  after WiFi, and keeping the dog loaded is what puts the outfit into the
//  baked cards (a reload between the sprite alloc and the bake does not
//  fit: it peaks at ~100 KB against ~93 KB of free heap).
//
//  Fallbacks (in order): each sprite is allocated from the DMA-capable pool
//  first, then from general heap (rows then pumped by CPU - still smooth);
//  if a sprite can't be allocated at all, the classic immediate-mode
//  animation (re-render both themes each frame inside a clip window, also
//  full-width) runs instead.  Buffers are freed when the animation ends.
// ---------------------------------------------------------------------------
static lgfx::LGFX_Sprite sprA, sprB;
static bool     sprUp = false;
static bool     sprADma = false;      // buffer is in the DMA-capable pool?
static bool     sprBDma = false;
static uint8_t* extBufA = nullptr;    // non-DMA fallback buffer for sprA
static uint8_t* extBufB = nullptr;    // non-DMA fallback buffer for sprB
static const int kTravel = 320 - kPrevX;   // 240: full card width + margin -> whole screen

// Allocate one 160x120 card sprite: DMA-capable pool first (fast row DMA),
// 8-bit pool second (CPU-pumped rows - that pool is NOT DMA-accessible, so
// *dmaOk reports which blit mode the buffer needs).  External buffers are
// tracked as "Preallocated" by LGFX, so deleteSprite() never frees them -
// we do.
static bool allocCardSprite(lgfx::LGFX_Sprite* s, uint8_t** extBuf, bool* dmaOk) {
  if (s->createSprite(kPrevW, kPrevH) != nullptr) { *dmaOk = true; return true; }
  *extBuf = (uint8_t*)heap_caps_malloc((size_t)kPrevW * kPrevH * 2 + 16, MALLOC_CAP_8BIT);
  if (*extBuf) {
    s->setBuffer(*extBuf, kPrevW, kPrevH);
    *dmaOk = false;   // panel DMA cannot read the 8-bit pool -> CPU-pumped blit
    return true;
  }
  heap_caps_free(*extBuf); *extBuf = nullptr;
  return false;
}

static bool allocSwipeSprites() {
  if (sprUp) return true;
  if (!allocCardSprite(&sprA, &extBufA, &sprADma)) return false;
  if (!allocCardSprite(&sprB, &extBufB, &sprBDma)) {
    sprA.deleteSprite();
    if (extBufA) { heap_caps_free(extBufA); extBufA = nullptr; }
    return false;
  }
  sprUp = true;
  return true;
}

static void freeSwipeSprites() {
  if (!sprUp) return;
  sprA.deleteSprite();
  sprB.deleteSprite();
  if (extBufA) { heap_caps_free(extBufA); extBufA = nullptr; }
  if (extBufB) { heap_caps_free(extBufB); extBufB = nullptr; }
  sprUp = false;
}

// Render a full theme at half scale into a 160x120 sprite (card = theme's
// full 320x240 screen squeezed into 160x120, includes background + dog).
static void renderThemeIntoSprite(lgfx::LGFX_Sprite* s, int idx) {
  s->clearClipRect();
  lgfx::LGFXBase* saved = gT;
  gT = s;
  renderTheme(idx, T{0.5f, 0.5f, 0, 0});
  gT = saved;
}

// Redraw the left/right columns of the static yellow border frame (the band
// fill + card blits wipe them; top/bottom border rows are outside the band
// and stay untouched).
static inline void redrawBorderSides() {
  tft.fillRect(kPrevX - 1, kPrevY, 1, kPrevH, kBorderC);
  tft.fillRect(kPrevX + kPrevW, kPrevY, 1, kPrevH, kBorderC);
}

// Blit sub-rect [srcX..srcX+w) x [0..h) of a 160-px-wide 16-bit sprite buffer
// to panel (dstX,dstY).  Replicates LGFX_Sprite::push_sprite()'s pixelcopy
// setup so byte order matches the normal sprite push path exactly.
static inline void blitRegion(const void* buf, bool dma, int srcX, int dstX, int dstY, int w, int h) {
  if (w <= 0 || h <= 0) return;
  lgfx::pixelcopy_t p(buf, tft.getColorDepth(), tft.getColorDepth(), false);
  p.src_bitwidth = kPrevW;      // stride of the full sprite row (in pixels)
  p.src_x = srcX;               // first visible column
  tft.startWrite();
  tft.panel()->writeImage((uint_fast16_t)dstX, (uint_fast16_t)dstY,
                          (uint_fast16_t)w, (uint_fast16_t)h, &p, dma);
  tft.endWrite();
}

static void animatePickerSwipe(int dir) {   // +1 = next (enters from right), -1 = prev
  const uint32_t t0 = millis();
  const int oldIdx = previewTheme;
  const int newIdx = (dir > 0) ? (oldIdx + 1) % THEME_COUNT
                               : (oldIdx + THEME_COUNT - 1) % THEME_COUNT;
  previewTheme = newIdx;   // now: tap-to-select and the next swipe work immediately
  Serial.printf("[TOUCH] theme swipe -> %s (%d)\n", themeName(newIdx), newIdx);

  drawPickerChrome(newIdx);   // background, border, bars with NEW labels, hint

  // The ~36 KB dog buffers stay loaded for the whole animation: the two
  // 38.4 KB sprite buffers + the dog fit in the ~134 KB free heap after
  // WiFi, and keeping the dog loaded is what puts the outfit into the
  // baked cards (a reload between sprite alloc and bake peaks at ~100 KB
  // against only ~93 KB of free heap and always fails).
  const bool sprites = allocSwipeSprites();
  if (sprites) {
    const uint32_t tr = millis();
    renderThemeIntoSprite(&sprA, oldIdx);   // old theme baked at half scale (with dog)
    renderThemeIntoSprite(&sprB, newIdx);   // new theme baked at half scale (with dog)
    Serial.printf("[ANIM] pre-render %s+%s: %lu ms\n", themeName(oldIdx),
                  themeName(newIdx), (unsigned long)(millis() - tr));
  } else {
    Serial.println("[ANIM] sprite alloc failed -> immediate-mode fallback (dog included)");
  }

  const int FRAMES = 8;
  for (int f = 1; f <= FRAMES; ++f) {
    const uint32_t f0 = millis();
    const float p = (float)f / (float)FRAMES;
    const float e = 1.0f - (1.0f - p) * (1.0f - p);   // easeOutQuad
    const int off = (f == FRAMES) ? kTravel : (int)(e * (float)kTravel);

    // Card positions for this offset (see header comment):
    const int oldX = (dir > 0) ? (kPrevX - off) : (kPrevX + off);
    const int newX = (dir > 0) ? (320 - off)    : (off - kPrevW);
    // Visible intersection of a card [x0, x0+160) with the screen [0, 320):
    struct Vis { int srcX, dstX, w; };
    auto vis = [](int x0) -> Vis {
      const int srcX = x0 < 0 ? -x0 : 0;          // first visible card column
      const int dstX = x0 < 0 ? 0 : x0;           // matching screen column
      int w = kPrevW - srcX;                      // card columns to the right
      const int right = 320 - dstX;               // screen columns to the right
      if (right < w) w = right;
      return Vis{srcX, dstX, w > 0 ? w : 0};
    };
    const Vis A = vis(oldX);
    const Vis B = vis(newX);

    if (sprites) {
      const void* a = sprA.getBuffer();
      const void* b = sprB.getBuffer();
      clearRegion(0, kPrevY, 320, kPrevH);   // repaint whole band (background)
      // Each card blits independently: on the final frame the old card is
      // fully off-screen (A.w == 0) while the new one still needs to land
      // at the preview position (an && here left the preview white).
      if (A.w > 0) blitRegion(a, sprADma, A.srcX, A.dstX, kPrevY, A.w, kPrevH);
      if (B.w > 0) blitRegion(b, sprBDma, B.srcX, B.dstX, kPrevY, B.w, kPrevH);
      redrawBorderSides();
    } else {
      // Classic immediate-mode path (slow but always works): repaint the
      // band, then redraw each theme clipped to its visible card rect.
      clearRegion(0, kPrevY, 320, kPrevH);
      if (A.w > 0) {
        tft.setClipRect(A.dstX, kPrevY, A.w, kPrevH);
        renderTheme(oldIdx, T{0.5f, 0.5f, oldX, kPrevY});
        tft.clearClipRect();
      }
      if (B.w > 0) {
        tft.setClipRect(B.dstX, kPrevY, B.w, kPrevH);
        renderTheme(newIdx, T{0.5f, 0.5f, newX, kPrevY});
        tft.clearClipRect();
      }
      redrawBorderSides();
    }
    if (gTimingOn)
      Serial.printf("[ANIM] f=%d off=%d %s: %lu ms\n", f, off,
                    sprites ? "blit" : "render", (unsigned long)(millis() - f0));

    delay(15);   // blit ~25 ms + 15 ms -> ~40 ms/frame, 8 frames ~320 ms
  }
  if (sprites) freeSwipeSprites();
  // (the dog buffers were never freed - nothing to restore)
  Serial.printf("[ANIM] %s -> %s: %d frames (%s) in %lu ms\n",
                themeName(oldIdx), themeName(newIdx), FRAMES,
                sprites ? "sprites" : "immediate", (unsigned long)(millis() - t0));
}

// TEMP-DEBUG: probe whether two half-scale card sprites fit in RAM at runtime.
static void ramProbe() {
  Serial.printf("[RAM] before: free=%lu minFree=%lu\n",
                (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap());
  // Pool detail: DMA-capable vs general heap, incl. largest contiguous block
  multi_heap_info_t infDMA, infGEN;
  heap_caps_get_info(&infDMA, MALLOC_CAP_DMA);
  heap_caps_get_info(&infGEN, MALLOC_CAP_8BIT);
  Serial.printf("[RAM] DMA heap: free=%u KB, largest block=%u KB\n",
                (unsigned)(infDMA.total_free_bytes / 1024), (unsigned)(infDMA.largest_free_block / 1024));
  Serial.printf("[RAM] GEN heap: free=%u KB, largest block=%u KB\n",
                (unsigned)(infGEN.total_free_bytes / 1024), (unsigned)(infGEN.largest_free_block / 1024));
  Serial.printf("[RAM] panel depth=%d bits\n", (int)tft.getColorDepth());
  auto* s1 = new lgfx::LGFX_Sprite();
  bool ok1 = (s1->createSprite(160, 120) != nullptr);
  auto* s2 = new lgfx::LGFX_Sprite();
  bool ok2 = (s2->createSprite(160, 120) != nullptr);
  Serial.printf("[RAM] after 2x(160x120): free=%lu minFree=%lu (s1=%d s2=%d)\n",
                (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(),
                ok1 ? 1 : 0, ok2 ? 1 : 0);
  if (ok1) { s1->deleteSprite(); delete s1; }
  if (ok2) { s2->deleteSprite(); delete s2; }
}

// ---------------------------------------------------------------------------
//  Flicker-free per-second updates
//
//  The panel has no framebuffer – every pixel write goes straight over SPI,
//  so redrawing the whole screen once per second is visible as flicker.
//  Instead, only what actually changed is repainted:
//    theme 0 (BASIC): band-clear + redraw of clock/date lines (original approach)
//    theme 1 (SKY):   clearThemed() over the status/clock/date bands + redraw
//                     those elements (sky decorations included – they sit in
//                     the cleared band and are cheap to repaint)
//    theme 2 (SPLIT): clearThemed() over the clock/date band + redraw it
//    theme 3 (BENTO): clearThemed() over the clock card + redraw it. Its text
//                     is minute-granular, so the per-second tick is a no-op
//                     (no SPI traffic at all -> no flash)
// ---------------------------------------------------------------------------
static void updateDynamicTexts() {
  if (!ntpSynced) return;
  struct tm lt;
  nowLocal(&lt);

  char clockBuf[16], dateBuf[32];
  formatClock(clockBuf, sizeof(clockBuf), &lt);   // respects use24Hour
  formatDateLine(dateBuf, sizeof(dateBuf), &lt);  // respects dateFormat

  if (activeTheme == 0) {
    // ---- Big clock (gLayout.clockY/clockSize) – repaint only on minute change ----
    if (strcmp(clockBuf, lastClockBuf) != 0) {
      strncpy(lastClockBuf, clockBuf, sizeof(lastClockBuf) - 1);
      lastClockBuf[sizeof(lastClockBuf) - 1] = '\0';
      tft.setTextSize(gLayout.clockSize);
      int maxW = tft.textWidth(clockMaxString());   // widest in the current mode (12h/24h)
      int boxX = (320 - maxW) / 2;
      clearRegion(boxX, gLayout.clockY - 2, maxW + 4, glyphH(gLayout.clockSize) + 2);   // clock band, matches background
      tft.setTextSize(gLayout.clockSize * T::ID.sx);
      int cw = tft.textWidth(clockBuf);
      tft.setTextColor(TFT_WHITE);
      tft.drawString(clockBuf, boxX + (maxW - cw) / 2, gLayout.clockY);
    }

    // ---- Date line – repaint only on change ----
    if (strcmp(dateBuf, lastDateBuf) != 0) {
      strncpy(lastDateBuf, dateBuf, sizeof(lastDateBuf) - 1);
      lastDateBuf[sizeof(lastDateBuf) - 1] = '\0';
      int bandTop = gLayout.clockY + glyphH(gLayout.clockSize);   // just under the clock
      clearRegion(0, bandTop, 320, (gLayout.dateY + glyphH(gLayout.dateSize)) - bandTop + 2);
      float ds = dateLineFit(dateBuf);               // chosen full-space size (normally exactly gLayout.dateSize)
      tft.setTextSize(ds * T::ID.sx);
      int dw = tft.textWidth(dateBuf);
      tft.setTextColor(TFT_YELLOW);
      tft.drawString(dateBuf, (320 - dw) / 2, gLayout.dateY);
    }
    return;
  }

  if (activeTheme == THEME_SKY) {
    bool clockChanged = strcmp(clockBuf, lastClockBuf) != 0;
    bool dateChanged  = strcmp(dateBuf, lastDateBuf) != 0;
    if (!clockChanged && !dateChanged) return;

    T t = T::ID;
    // Clear the dynamic band (status row y~7 + clock + date line ending ~y73),
    // then redraw it. Sky decorations live inside this band and are repainted
    // right after, so nothing is lost.
    clearThemed(activeTheme, t, 4, 4, 312, 72);
    if (clockChanged) {
      strncpy(lastClockBuf, clockBuf, sizeof(lastClockBuf) - 1);
      lastClockBuf[sizeof(lastClockBuf) - 1] = '\0';
    }
    if (dateChanged) {
      strncpy(lastDateBuf, dateBuf, sizeof(lastDateBuf) - 1);
      lastDateBuf[sizeof(lastDateBuf) - 1] = '\0';
    }

    drawSkyDecor(activeTheme, t);   // sun/moon/clouds/stars live in this band
    drawSkyDynamicBand(t);   // Nunito band (bitmap fallback inside)
    return;
  }

  if (activeTheme == THEME_SPLIT) {   // SPLIT: repaint only the clock/date band.
    bool clockChanged = strcmp(clockBuf, lastClockBuf) != 0;
    bool dateChanged  = strcmp(dateBuf, lastDateBuf) != 0;
    if (!clockChanged && !dateChanged) return;   // no blinking colon: nothing moves per second
    const T& t = T::ID;     // screen is exactly 320x240 -> identity transform
    clearThemed(THEME_SPLIT, t, 10, 22, 204, 56);   // left-zone rows [22..78): solid bg
    drawSplitClockDate(t);
    if (clockChanged) {
      strncpy(lastClockBuf, clockBuf, sizeof(lastClockBuf) - 1);
      lastClockBuf[sizeof(lastClockBuf) - 1] = '\0';
    }
    if (dateChanged) {
      strncpy(lastDateBuf, dateBuf, sizeof(lastDateBuf) - 1);
      lastDateBuf[sizeof(lastDateBuf) - 1] = '\0';
    }
    return;
  }

  if (activeTheme == THEME_BENTO) {   // BENTO: repaint only the clock card. Its text is
                            // minute-granular, so a per-second tick is a no-op
                            // (zero SPI traffic -> no flash); once a minute
                            // only the 304x52 card band is cleared + redrawn.
    bool clockChanged = strcmp(clockBuf, lastClockBuf) != 0;
    bool dateChanged  = strcmp(dateBuf, lastDateBuf) != 0;
    if (!clockChanged && !dateChanged) return;
    T t = T::ID;
    clearThemed(THEME_BENTO, t, 8, 8, 304, 52);   // clock card rows [8..60)
    drawBentoClockCard(t);               // redraws the card + re-syncs the cache
    return;
  }

  // Safety net: unknown theme -> full re-render.
  renderScreen();
}

// ---------------------------------------------------------------------------
//  Touch targets (2 s debounce between actions)
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
//  from gLayout; themes 1-3 use fixed bands around their mockup positions.
// ---------------------------------------------------------------------------
struct TouchBands { int clkTop, clkBot, dateTop, dateBot, tempTop, tempBot, tempMaxX; };
static TouchBands touchBands() {
  switch (activeTheme) {
    case 0:   // BASIC – original gLayout-derived zones
      return { gLayout.statusBarY + glyphH(1) + 2, gLayout.clockY + glyphH(gLayout.clockSize),
               gLayout.clockY + glyphH(gLayout.clockSize), gLayout.dateY + glyphH(gLayout.dateSize) + 4,
               gLayout.tempY - 4, gLayout.tempY + glyphH(gLayout.tempSize) + 2,
               tft.width() - gLayout.dogZoneW };
    case 1:   // SKY – status y7, clock y19 (≤5.25), date y62, temp y76 (≤6)
      return { 18, 61, 61, 75, 75, 125, 220 };
    case 2:   // SPLIT – status y9, clock y24 (≤4.6), date y64, temp y80; left zone only
      return { 20, 62, 62, 77, 77, 125, 210 };
    default:  // BENTO – clock card [8,60) split at the date line; temp card [66,138]
      return { 8, 40, 40, 60, 66, 138, 202 };
  }
}

static bool inDogZone(int x, int y) {   // reserved dog corner zone (bottom-right)
  if (activeTheme == THEME_BENTO) return x >= 206 && y >= 66;   // BENTO: whole right column = dog card
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

static void handleTouch() {
  lgfx::touch_point_t pts[5];
  int n = tft.getTouch(pts, 5);

  // ---- THEME picker: gesture tracking (swipe left/right, tap to select) ----
  if (themeSelectScreen) {
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
        } else if (dt < 600 && adx <= 12 && ady <= 12) {   // tap: select this theme
          activeTheme = previewTheme;
          saveTheme();
          themeSelectScreen = false;
          Serial.printf("[TOUCH] theme selected -> %s (%d)\n", themeName(activeTheme), activeTheme);
          renderScreen();
        } else {
          Serial.println("[TOUCH] theme gesture ignored (long/ambiguous)");
        }
      }
    }
    return;   // main-display zones are inactive while the picker is up
  }

  if (n == 0) return;
  static uint32_t lastAction = 0;
  if (millis() - lastAction < 2000) return;   // debounce
  lastAction = millis();

  int x = pts[0].x, y = pts[0].y;
  Serial.printf("[TOUCH] (%d,%d)\n", x, y);

  if (inDogZone(x, y)) {                // dog corner zone -> THEME picker
    themeSelectScreen = true;
    previewTheme = activeTheme;
    swipe.active = false;
    Serial.println("[TOUCH] -> theme picker");
    drawThemePicker();
    return;
  }

  if (inClockZone(x, y)) {              // clock -> 12h <-> 24h
    use24Hour = !use24Hour;
    Serial.printf("[TOUCH] clock -> %s\n", use24Hour ? "24h" : "12h");
    renderScreen();                           // full redraw in the new format
    return;
  }

  if (inDateZone(x, y)) {               // date line -> cycle format
    dateFormat = (dateFormat + 1) % 2;
    Serial.printf("[TOUCH] date -> format %d\n", dateFormat);
    renderScreen();
    return;
  }

  if (inTempZone(x, y)) {               // temperature -> F <-> C
    useCelsius = !useCelsius;
    Serial.printf("[TOUCH] temp -> %s\n", useCelsius ? "C" : "F");
    renderScreen();                           // all displayed temps convert together
    return;
  }

  // Anywhere else: force a weather refresh (original behavior)
  if (!wifiEnsureConnected()) {
    renderScreen();   // show offline status
    return;
  }
  bool got = fetchWeather();
  if (got) refreshDogImage();
  renderScreen();
}

// ---------------------------------------------------------------------------
//  Display hard reset — see README "Display gotchas" (ST7789).
//  The ST7789 is a separate chip with its own volatile state: resetting the
//  ESP32 (RST button / flashing) does NOT reset it, and LGFX's Panel_ST7789
//  init list contains no SWRESET (0x01). Kept as defense against stale panel
//  state (e.g. a brownout glitch during boot). NOTE: the persistent pale/
//  yellow patches this was originally added for turned out to be a software
//  rendering bug (the bffCenter scale-argument bug — see README), not stale
//  panel state; no reset could ever fix that.
//  Belt and suspenders:
//    1) hardware pulse on TFT_RST (datasheet min ~5 ms low / 10 ms after
//       release; we use 100/100 for margin),
//    2) SWRESET (0x01) over SPI — recovers the panel even if its RST line is
//       not reachable from this GPIO, as long as the SPI command interface
//       still latches.
// ---------------------------------------------------------------------------
static void displayHardReset() {
  // 1) Hardware reset pulse.
  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_RST, LOW);
  delay(100);
  digitalWrite(TFT_RST, HIGH);
  delay(100);

  // 2) Software reset over SPI (same VSPI host LGFX uses; tft.init()
  //    re-initializes the peripheral afterwards).
  {
    static SPIClass tftSpi(VSPI);   // Arduino constant for the SPI3 peripheral
    pinMode(TFT_CS, OUTPUT);
    digitalWrite(TFT_CS, HIGH);
    pinMode(TFT_DC, OUTPUT);
    digitalWrite(TFT_DC, LOW);      // command mode (DC/CS are not managed by Arduino SPI)
    tftSpi.begin(TFT_SCK, -1, TFT_MOSI, TFT_CS);
    tftSpi.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    digitalWrite(TFT_CS, LOW);      // HW CS is off -> toggle manually
    tftSpi.transfer(0x01);          // SWRESET
    digitalWrite(TFT_CS, HIGH);
    tftSpi.endTransaction();
  }
  delay(120);   // ST7789 needs up to 120 ms after SWRESET before it accepts commands
}

// ---------------------------------------------------------------------------
//  setup / loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== CYD Weather Station ===");

  // Display first so the user gets immediate feedback. Force a full panel
  // reset before init (defensive — see displayHardReset() header comment).
  displayHardReset();
  tft.init();
  tft.setRotation(TFT_ROTATION);
  tft.setBrightness(255);

  // Nunito BFF fonts for theme 1 (SKY). Static flash data; on load failure the
  // renderer falls back to the original bitmap-font layout.
  loadNunitoFonts();
  Serial.printf("[FONT] nunito bff: %s\n", nunitoOK ? "ok" : "FAILED - using bitmap fallback");

  // SD card: dedicated HSPI instance (the TFT owns VSPI). The outfit images are
  // optional - without a card the dashboard simply runs in the full-width layout.
  {
    static SPIClass sdSpi(HSPI);   // SPI2 peripheral, separate from the TFT's SPI3
    sdSpi.begin(SD_SCK, SD_MISO, SD_MOSI);

    // The slot has marginal contact: a single init/read can fail on any boot
    // (most often noticed right after flashing, when you're watching the boot).
    // Retry instead of giving up for the whole session.
    bool cardOk = false;
    for (int attempt = 1; attempt <= 3 && !cardOk; attempt++) {
      if (attempt > 1) delay(500);
      cardOk = SD.begin(SD_CS, sdSpi, 16000000);
    }
    if (cardOk) {
      sdCardReady = true;
      Serial.println("[SD] card ready");
      // No weather data yet -> error.png until the first fetch picks a real outfit.
      refreshDogImage();
    } else {
      Serial.println("[SD] no card / init failed after 3 tries - continuing without dog image");
    }
  }

  // Restore the last selected display theme (NVS), then show it.
  loadTheme();
  Serial.printf("[THEME] active: %s (%d)\n", themeName(activeTheme), activeTheme);
  renderScreen();

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  wifiEnsureConnected();

  // NTP
  configureTime();

  // Wait briefly for the clock: without it, fetchWeather()'s rain scan is
  // skipped and boot would show "No rain expected" even when rain is forecast.
  uint32_t t0 = millis();
  while (!ntpIsSynced() && millis() - t0 < NTP_BOOT_WAIT_MS) delay(100);

  // Initial weather fetch (best-effort)
  if (WiFi.status() == WL_CONNECTED) {
    bool got = fetchWeather();
    if (got) refreshDogImage();   // swap error.png for the real outfit
    renderScreen();
  }

  // TEMP-DEBUG: RAM probe (remove when the animation is final).
  ramProbe();
}

void loop() {
  static uint32_t lastClockDraw = 0;
  static uint32_t lastWeatherTry = 0;
  static uint32_t lastWifiCheck  = 0;
  static bool     lastWifiState  = false;
  static bool     lastNtpState   = false;

  uint32_t nowMs = millis();

  // ---- Reconnect WiFi if dropped (check every 10 s) ----
  if (WiFi.status() != WL_CONNECTED && nowMs - lastWifiCheck >= WIFI_RETRY_MS) {
    lastWifiCheck = nowMs;
    wifiEnsureConnected();
  }

  // ---- NTP: once connected, wait for sync ----
  if (!ntpSynced && WiFi.status() == WL_CONNECTED && ntpIsSynced()) {
    ntpSynced = true;
    Serial.println("[NTP] Time synced.");
    // If weather was already fetched before the clock existed, its rain scan
    // was skipped — refetch now so the state-change redraw below shows the
    // correct rain line + outfit (no extra render: this block runs first).
    if (wx.valid && fetchWeather()) refreshDogImage();
  }

  // ---- Full redraw only when the status-bar indicators change ----
  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  if (wifiOk != lastWifiState || ntpSynced != lastNtpState) {
    lastWifiState = wifiOk;
    lastNtpState  = ntpSynced;
    if (!themeSelectScreen) renderScreen();   // don't clobber the THEME picker
  }

  // ---- Periodic weather refresh (15 min), retry on failure ----
  bool needWeather = !wx.valid || (nowMs - wx.lastUpdateMs >= WEATHER_REFRESH_MS);
  if (needWeather && wifiOk && nowMs - lastWeatherTry >= WX_MIN_RETRY_MS) {
    lastWeatherTry = nowMs;
    // Data + outfit update even while the THEME picker is up; the redraw
    // waits until the user returns to the main display.
    if (fetchWeather()) { refreshDogImage(); if (!themeSelectScreen) renderScreen(); }
  }

  // ---- Per-second tick: repaint clock/date only when their text changes ----
  bool timeChanged = ntpSynced && (nowMs / 1000 != lastClockDraw / 1000);
  if (timeChanged) {
    lastClockDraw = nowMs;
    if (!themeSelectScreen) updateDynamicTexts();   // partial updates target the main display only
  }


  // ---- Touch ----
  handleTouch();

  delay(50);   // keep the loop light; GT911 is polled inside getTouch()
}
