#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "theme_ids.h"
#include "nws_station.h"   // NWS_BACKUP_MAX (Phase 4 fallback list size)

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

// How a location value was chosen (the timezone is automatic only — it
// always follows the location and has no mode of its own):
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
  char postalCode[64];            // postal code or city entered on the
                                  // LOCATION page (US ZIP, ZIP+4, international
                                  // code, or a city name); "" = none
  char placeName[56];             // resolved location display ("City, Region,
                                  // Country") from geocoding; "" = not resolved
  // Timezone: automatic only (Phase 2A/2B) — resolved from the active
  // location on every weather fetch (IANA id -> catalog rules, timezones.h).
  // There is no manual selection: whatever is stored here is the last
  // auto-resolved rules, kept as a cache so the clock stays correct until
  // the next fetch replaces it.
  TimezoneRules timezone;         // last auto-resolved rules; default US Eastern
  int  theme;                     // selected theme (0..THEME_COUNT-1)
  bool useCelsius;                // temperature display: false = Fahrenheit
  bool use24Hour;                 // clock: false = US 12-hour ("2:35 PM")
  int  dateFormat;                // 0: "Thursday 12 November 2026"  1: "Thursday November 12, 2026"
  // TEMP RANGES (Settings -> TEMP RANGES): the max °F of each dog-outfit
  // temperature band. The outfit follows the feels-like temperature; a
  // band covers "previous limit + 1 .. this limit" (feels <= limit). The
  // six limits stay ascending with at least 1° apart (slider constraint),
  // and everything above tempMildWarmMaxF is HOT.
  uint8_t tempExtremeColdMaxF;    // EXTREME COLD (default 30 F)
  uint8_t tempVeryColdMaxF;       // VERY COLD    (default 45 F)
  uint8_t tempColdMaxF;           // COLD         (default 55 F)
  uint8_t tempCoolMaxF;           // COOL         (default 65 F)
  uint8_t tempMildCoolMaxF;       // MILD COOL    (default 70 F)
  uint8_t tempMildWarmMaxF;       // MILD WARM    (default 75 F; above = HOT)
  // Selected NWS observation station (chosen on the STATIONS settings page).
  // Phase 5: fetchWeather() reads nwsStationId + nwsStationBackup[] as the
  // dashboard temperature/feels-like source (preferred -> backups -> Open-Meteo).
  // "" = no station selected / none found near the location.
  char nwsStationId[8];           // ICAO identifier, e.g. "KNYC"
  char nwsStationName[40];        // NWS display name of the selected station
  float nwsStationDistKm;         // distance from the location at selection time
  char nwsStationProvider[16];    // e.g. "ASOS", "ASOS-HFM"
  // Phase 4: automatic NWS backup stations. Firmware-selected when the user
  // picks the preferred station (the nearest suitable stations from the same
  // NWS list, within NWS_BACKUP_MAX_KM - see nwsBuildFallbackList());
  // never user-configurable, rebuilt on every selection. The ordered
  // fallback list is [nwsStationId, nwsStationBackup[0..count-1]].
  char nwsStationBackup[NWS_BACKUP_MAX][8];  // ICAO ids, "" = empty slot
  // Last completed NWS points lookup: true = the location is inside NWS
  // coverage (a station selection is possible), false = outside coverage
  // (non-US site; an empty selection is the healthy state and must not
  // raise the red LED). Default true = "never looked up yet", which keeps
  // the red nudge for built-for-site US installs until the first lookup.
  bool nwsCovered;
};

// TEMP RANGES: slider range and the values first boot / the RESTORE
// DEFAULTS button use. The defaults keep the original hardwired outfit
// chart (README, Dog outfit images).
constexpr int TEMP_MIN_F = 0;
constexpr int TEMP_MAX_F = 110;
constexpr uint8_t TEMP_DEF_EXTREME_COLD = 30;
constexpr uint8_t TEMP_DEF_VERY_COLD    = 45;
constexpr uint8_t TEMP_DEF_COLD         = 55;
constexpr uint8_t TEMP_DEF_COOL         = 65;
constexpr uint8_t TEMP_DEF_MILD_COOL    = 70;
constexpr uint8_t TEMP_DEF_MILD_WARM    = 75;

extern UserSettings userSettings;

// Shared NVS handle (namespace "cydwx"); owned by config.cpp, also used by
// wifi_conn.cpp for the user-saved Wi-Fi credentials ("ssid"/"pass").
extern Preferences prefs;

// Boot: open the "cydwx" NVS namespace and restore every persisted setting.
// Called in setup() before the first render and before NTP/weather use the
// values. Existing-install migration: a location whose mode was never saved
// loads as MANUAL (its default value is the value it was always running
// with), so a previously working location is never handed to automatic
// detection. The timezone has no mode of its own: the stored rules load as
// the last auto-resolved cache, and the first weather fetch refreshes them
// from the active location.
void loadSettings();

// Explicit per-setting saves — each writes only its own keys, so saving one
// setting never touches the others. Called on user change (touch toggles,
// picker selection) and after a successful automatic detection.
void saveTheme();            // theme index (also records the numbering version)
void saveLocation();         // lat/lon + location mode
void saveTimezone();         // timezone rules (last auto-resolved cache)
void saveDisplaySettings();  // temp unit, clock format, date format
void saveTempRanges();       // the six TEMP RANGES band limits
void saveNwsStation();       // selected NWS station (id/name/distance/provider)

// Settings-page resets (confirmation pages): clear the saved NVS values of
// one section and persist the cleared state. Nothing outside the section
// is touched.
void resetLocation();   // lat/lon -> 0, postal code/place name -> "", mode -> UNCONFIGURED

// Automatic-detection seam for the future geolocation service: true while
// the location may be (re)determined automatically — UNCONFIGURED (first
// time) or AUTOMATIC (refresh). MANUAL is never overwritten.
bool locationAutoAllowed();
// Apply an auto-detected value. Location: returns false (value unchanged)
// when the setting is MANUAL or the value is invalid; on success the mode
// becomes AUTOMATIC (UNCONFIGURED transitions on first detection) and the
// change is persisted. Timezone: automatic only, so this always applies;
// the value must be a valid TimezoneRules. tz.name must point to persistent
// storage (it is kept as a pointer in userSettings and copied into NVS on
// save).
bool applyAutoLocation(float lat, float lon);
bool applyAutoTimezone(const TimezoneRules& tz);
// Phase 2A: IANA id from Open-Meteo (timezone=auto, e.g. "America/New_York")
// -> catalog zone -> applied as the automatic timezone. Automatic only:
// always applies; an unsupported id keeps the last valid rules.
bool applyAutoTimezoneFromIana(const char* iana);
// Phase 2B: last automatic IANA resolution (set by applyAutoTimezoneFromIana
// and read by the TIME ZONE settings page). autoTimezoneIana() is "" until
// the first resolution; autoTimezoneUnsupported() is true when that id has
// no catalog mapping and the last valid rules are still in effect.
const char* autoTimezoneIana();
bool autoTimezoneUnsupported();

// ---------------------------------------------------------------------------
//  App timing + service constants
// ---------------------------------------------------------------------------
constexpr uint32_t WEATHER_REFRESH_MS = 15UL * 60UL * 1000UL;   // periodic weather fetch
constexpr uint32_t WX_MIN_RETRY_MS    = 30UL * 1000UL;          // min gap between fetch attempts
// NWS active-alert polling (nws_alerts.cpp): tighter while any alert is
// active (it can end at any time), relaxed when clear, and a hard floor
// between attempts so repeated failures don't hammer the API. A failed
// attempt retries after ALERTS_RETRY_FAIL_MS instead of the full floor, so
// a transient truncation right after boot doesn't delay the pill ~a minute.
constexpr uint32_t ALERTS_REFRESH_ACTIVE_MS = 5UL * 60UL * 1000UL;
constexpr uint32_t ALERTS_REFRESH_CLEAR_MS  = 15UL * 60UL * 1000UL;
constexpr uint32_t ALERTS_MIN_GAP_MS        = 60UL * 1000UL;
constexpr uint32_t ALERTS_RETRY_FAIL_MS     = 10UL * 1000UL;
constexpr uint32_t WIFI_RETRY_MS      = 10UL * 1000UL;          // WiFi reconnect check interval
constexpr uint32_t NTP_BOOT_WAIT_MS   = 10UL * 1000UL;          // max wait for NTP before initial fetch

// NTP: the system clock is kept in UTC and local time is computed in software
// from userSettings.timezone (see time_util.cpp).
constexpr const char* NTP_TZ = "UTC";
constexpr const char* NTP_SERVERS[3] = { "pool.ntp.org", "time.google.com", "time.windows.com" };
