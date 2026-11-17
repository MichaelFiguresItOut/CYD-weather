#pragma once

#include <Arduino.h>
#include "theme_ids.h"

// ---------------------------------------------------------------------------
//  Centralized configuration: all user-adjustable settings (UserSettings),
//  the site defaults, and the app-wide timing/service constants.
//
//  The initial values of userSettings (set in config.cpp) are the defaults.
//  Only `theme` is persisted today (NVS, loadTheme/saveTheme); the rest are
//  runtime state toggled by taps, persisted later.
// ---------------------------------------------------------------------------

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
  TimezoneRules timezone;         // local timezone + DST rules; default US Eastern
  int  theme;                     // selected theme (0..THEME_COUNT-1); persisted in NVS
  bool useCelsius;                // temperature display: false = Fahrenheit
  bool use24Hour;                 // clock: false = US 12-hour ("2:35 PM")
  int  dateFormat;                // 0: "Thursday 12 November 2026"  1: "Thursday November 12, 2026"
};

extern UserSettings userSettings;

// Restore the last selected theme from NVS (namespace "cydwx"); called in
// setup() before the first render. Migrates the legacy 5-theme numbering.
void loadTheme();
// Persist the currently selected theme to NVS; called on picker selection.
void saveTheme();

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
