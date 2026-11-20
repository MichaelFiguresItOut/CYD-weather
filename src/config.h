#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "theme_ids.h"

// ---------------------------------------------------------------------------
//  Centralized configuration: all user-adjustable settings (UserSettings),
//  the site defaults, and the app-wide timing/service constants.
//
//  The initial values of userSettings (set in config.cpp) are the defaults.
//  Every setting is persisted in NVS (namespace "cydwx"): loadSettings()
//  restores them at boot (defaults fill anything never saved, invalid values
//  fall back to defaults), and the save*() functions persist explicit
//  changes — never continuously.
// ---------------------------------------------------------------------------

// How a location/timezone value was chosen. Persisted PER SETTING, so the two
// stay independent (automatic location + manual timezone, or vice versa):
//   UNCONFIGURED – no mode chosen yet; first successful auto-detection fills
//                  it in (and leaves the mode AUTOMATIC)
//   AUTOMATIC    – firmware determines it from network-based services;
//                  refreshable whenever Wi-Fi is available
//   MANUAL       – user-chosen; automatic detection never overwrites it
enum class SettingMode : uint8_t { UNCONFIGURED = 0, AUTOMATIC = 1, MANUAL = 2 };

// Daylight-saving rules: the transition happens at the (week-th) Sunday of
// (month) at (hourLocal) local wall-clock time. Offsets are in seconds,
// negative = west of UTC.
struct TimezoneRules {
  const char* name;               // e.g. "US Eastern (America/New_York)"
  int stdOffsetSec;               // standard-time offset
  int dstOffsetSec;               // daylight-saving offset
  int dstStartMonth;              // 1-12
  int dstStartWeek;               // which Sunday (1-5) of dstStartMonth
  int dstStartHourLocal;          // wall-clock hour of the spring transition
  int dstEndMonth;
  int dstEndWeek;
  int dstEndHourLocal;
};

// Default site timezone: US Eastern (America/New_York).
// EST = UTC-5, EDT = UTC-4, DST from 02:00 local on the second Sunday of
// March until 02:00 local on the first Sunday of November (US rules since 2007).
constexpr TimezoneRules TIMEZONE_US_EASTERN = {
  "US Eastern (America/New_York)",
  -5 * 3600, -4 * 3600,
  3, 2, 2,
  11, 1, 2
};

// All runtime user-adjustable settings in one place.
struct UserSettings {
  float lat;                      // weather location (Open-Meteo); default from local_config.h
  float lon;                      // weather location (Open-Meteo); default from local_config.h
  SettingMode locationMode;       // how lat/lon were chosen
  char postalCode[12];            // postal code entered on the LOCATION page
                                  // (US ZIP, ZIP+4, or international code); "" = none
  char placeName[56];             // resolved location display ("City, Region,
                                  // Country") from geocoding; "" = not resolved
  TimezoneRules timezone;         // local timezone + DST rules; default US Eastern
  SettingMode timezoneMode;       // how the timezone was chosen
  int  theme;                     // selected theme (0..THEME_COUNT-1)
  bool useCelsius;                // temperature display: false = Fahrenheit
  bool use24Hour;                 // clock: false = US 12-hour ("2:35 PM")
  int  dateFormat;                // 0: "Thursday 12 November 2026"  1: "Thursday November 12, 2026"
};

extern UserSettings userSettings;

// Shared NVS handle (namespace "cydwx"); owned by config.cpp, also used by
// wifi_conn.cpp for the user-saved Wi-Fi credentials ("ssid"/"pass").
extern Preferences prefs;

// Boot: open the "cydwx" NVS namespace and restore every persisted setting.
// Called in setup() before the first render and before NTP/weather use the
// values. Existing-install migration: a setting whose mode was never saved
// loads as MANUAL (its default value is the value it was always running
// with), so previously working locations/timezones are never handed to
// automatic detection.
void loadSettings();

// Explicit per-setting saves — each writes only its own keys, so saving one
// setting never touches the others. Called on user change (touch toggles,
// picker selection) and after a successful automatic detection.
void saveTheme();            // theme index (also records the numbering version)
void saveLocation();         // lat/lon + location mode
void saveTimezone();         // timezone rules + timezone mode
void saveDisplaySettings();  // temp unit, clock format, date format

// Automatic-detection seam for the future geolocation/timezone services:
// true while the setting may be (re)determined automatically — UNCONFIGURED
// (first time) or AUTOMATIC (refresh). MANUAL is never overwritten.
bool locationAutoAllowed();
bool timezoneAutoAllowed();
// Apply an auto-detected value. Returns false (value unchanged) when the
// setting is MANUAL or the value is invalid. On success the mode becomes
// AUTOMATIC (UNCONFIGURED transitions on first detection) and the change is
// persisted. tz.name must point to persistent storage (it is kept as a
// pointer in userSettings and copied into NVS on save).
bool applyAutoLocation(float lat, float lon);
bool applyAutoTimezone(const TimezoneRules& tz);

// ---------------------------------------------------------------------------
//  App timing + service constants
// ---------------------------------------------------------------------------
constexpr uint32_t WEATHER_REFRESH_MS = 15UL * 60UL * 1000UL;   // periodic weather fetch
constexpr uint32_t WX_MIN_RETRY_MS    = 30UL * 1000UL;          // min gap between fetch attempts
constexpr uint32_t WIFI_RETRY_MS      = 10UL * 1000UL;          // WiFi reconnect check interval
constexpr uint32_t NTP_BOOT_WAIT_MS   = 10UL * 1000UL;          // max wait for NTP before initial fetch

// NTP: the system clock is kept in UTC and local time is computed in software
// from userSettings.timezone (see time_util.cpp).
constexpr const char* NTP_TZ = "UTC";
constexpr const char* NTP_SERVERS[3] = { "pool.ntp.org", "time.google.com", "time.windows.com" };
