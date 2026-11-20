#include "config.h"

#include <Preferences.h>
#include <cstring>
#include "local_config.h"
#include "timezones.h"

// ---------------------------------------------------------------------------
//  User settings: defaults = the previously hard-coded behavior.
//  lat/lon come from local_config.h (gitignored site configuration).
//  Every setting is restored from NVS at boot (loadSettings()); the defaults
//  above apply to anything never saved, and the modes default to MANUAL so
//  the site the firmware was built for is never handed to auto-detection.
// ---------------------------------------------------------------------------
UserSettings userSettings = {
  LOC_LAT,                  // site latitude (local_config.h)
  LOC_LON,                  // site longitude (local_config.h)
  SettingMode::MANUAL,      // existing behavior: built-for-site values
  "",                       // postalCode: set on the LOCATION page; may be blank
  "",                       // placeName: resolved by geocoding; may be blank
  TIMEZONE_US_EASTERN,      // US Eastern, US DST rules
  SettingMode::MANUAL,      // existing behavior: built-for-site timezone
  THEME_BASIC,              // default; loadSettings() overwrites with the persisted value
  false,                    // Fahrenheit
  false,                    // US 12-hour clock
  0                         // "Thursday 12 November 2026"
};

// ---- Persistence (NVS namespace "cydwx") ----------------------------------
// Keys (all per-key writes; a missing key = "never saved" -> default):
//   "theme"   u32   selected theme index (current numbering: 0 BASIC .. 3 BENTO)
//   "themeVer"u32   numbering version; 1 = pre-CARDS-removal (5-theme) indices
//   "locMode" u8    SettingMode of the weather location
//   "lat"     f32   weather latitude
//   "lon"     f32   weather longitude
//   "postcode" str  postal code entered on the LOCATION page ("" = none)
//   "placename" str resolved location display name ("" = not resolved)
//   "tzMode"  u8    SettingMode of the timezone
//   "tzName"  str   timezone display name (informational)
//   "tzStd"   i32   standard-time offset, seconds
//   "tzDst"   i32   daylight-saving offset, seconds
//   "tzDstS_M/W/H" u8  DST start: month / which Sunday / wall-clock hour
//   "tzDstE_M/W/H" u8  DST end:   month / which Sunday / wall-clock hour
//   "celsius" u8    0 = Fahrenheit, 1 = Celsius
//   "clock24" u8    0 = US 12-hour, 1 = 24-hour
//   "datefmt" u8    date-line format (0/1)
Preferences prefs;   // namespace "cydwx" (also used by wifi_conn.cpp)
static bool nvsOpen = false;   // set by loadSettings(); save*() no-op without it
static char tzNameBuf[64];     // home for the loaded/stored timezone name (TimezoneRules.name is a pointer)

// ---- Value validation -------------------------------------------------------
static bool validLatLon(float lat, float lon) {
  return lat >= -90.0f && lat <= 90.0f && lon >= -180.0f && lon <= 180.0f;
}
static bool validTzOffset(int s) {
  return s >= -15 * 3600 && s <= 15 * 3600;   // no real zone is beyond +/-15 h
}
static bool validTzRange(int v, int lo, int hi) { return v >= lo && v <= hi; }
static bool validTimezone(const TimezoneRules& t) {
  return validTzOffset(t.stdOffsetSec) && validTzOffset(t.dstOffsetSec)
      && validTzRange(t.dstStartMonth, 1, 12) && validTzRange(t.dstStartWeek, 1, 5)
      && validTzRange(t.dstStartHourLocal, 0, 23)
      && validTzRange(t.dstEndMonth, 1, 12) && validTzRange(t.dstEndWeek, 1, 5)
      && validTzRange(t.dstEndHourLocal, 0, 23);
}
// Memberwise equality (no synthesized operator!= for this struct in this
// toolchain); the name participates, so a renamed zone is re-saved.
static bool tzEqual(const TimezoneRules& a, const TimezoneRules& b) {
  return a.stdOffsetSec == b.stdOffsetSec && a.dstOffsetSec == b.dstOffsetSec
      && a.dstStartMonth == b.dstStartMonth && a.dstStartWeek == b.dstStartWeek
      && a.dstStartHourLocal == b.dstStartHourLocal
      && a.dstEndMonth == b.dstEndMonth && a.dstEndWeek == b.dstEndWeek
      && a.dstEndHourLocal == b.dstEndHourLocal
      && strcmp(a.name ? a.name : "", b.name ? b.name : "") == 0;
}
// A stored mode byte outside 0..2 is corrupt -> MANUAL (the safe default:
// it can never be clobbered by auto-detection).
static SettingMode loadMode(const char* key) {
  uint8_t m = prefs.getUChar(key, (uint8_t)SettingMode::MANUAL);
  return (m <= (uint8_t)SettingMode::MANUAL) ? (SettingMode)m : SettingMode::MANUAL;
}

// ---- Load (called once at boot, before the first render) -------------------
static void loadThemePart() {
  uint32_t rawT = prefs.getUInt("theme", 0);
  uint32_t rawV = prefs.getUInt("themeVer", 1);
  uint32_t t = rawT;
  bool migrated = false;
  if (rawV < 2) {   // one-time migration to the 4-theme numbering (CARDS was 2)
    if (t == 3)                            t = THEME_SPLIT;   // SPLIT: 3 -> 2
    else if (t == 4)                       t = THEME_BENTO;   // BENTO: 4 -> 3
    else if (t == 2 || t >= THEME_COUNT)   t = THEME_BASIC;   // old CARDS / unknown
    size_t wmig = prefs.putUInt("themeVer", 2);
    if (wmig == 0) Serial.println("[THEME] migration themeVer write failed");
    migrated = true;
  }
  userSettings.theme = (t < THEME_COUNT) ? (int)t : 0;
  Serial.printf("[THEME] NVS: stored theme=%lu ver=%lu%s\n",
                (unsigned long)rawT, (unsigned long)rawV, migrated ? " (migrated)" : "");
}

static void loadLocationPart() {
  userSettings.locationMode = loadMode("locMode");
  float lat = prefs.getFloat("lat", LOC_LAT);
  float lon = prefs.getFloat("lon", LOC_LON);
  if (!validLatLon(lat, lon)) { lat = LOC_LAT; lon = LOC_LON; }
  userSettings.lat = lat;
  userSettings.lon = lon;
  // Postal code / resolved name: "" if never saved (isKey avoids the NVS
  // not-found log, same pattern as the timezone name).
  userSettings.postalCode[0] = 0;
  if (prefs.isKey("postcode"))
    prefs.getString("postcode", userSettings.postalCode, sizeof(userSettings.postalCode));
  userSettings.placeName[0] = 0;
  if (prefs.isKey("placename"))
    prefs.getString("placename", userSettings.placeName, sizeof(userSettings.placeName));
  Serial.printf("[CFG] location: mode=%d lat=%.4f lon=%.4f code=\"%s\" place=\"%s\"\n",
                (int)userSettings.locationMode, lat, lon,
                userSettings.postalCode, userSettings.placeName);
}

static void loadTimezonePart() {
  userSettings.timezoneMode = loadMode("tzMode");
  TimezoneRules tz = TIMEZONE_US_EASTERN;
  tz.stdOffsetSec      = prefs.getInt("tzStd", tz.stdOffsetSec);
  tz.dstOffsetSec      = prefs.getInt("tzDst", tz.dstOffsetSec);
  tz.dstStartMonth     = prefs.getUChar("tzDstS_M", tz.dstStartMonth);
  tz.dstStartWeek      = prefs.getUChar("tzDstS_W", tz.dstStartWeek);
  tz.dstStartHourLocal = prefs.getUChar("tzDstS_H", tz.dstStartHourLocal);
  tz.dstEndMonth       = prefs.getUChar("tzDstE_M", tz.dstEndMonth);
  tz.dstEndWeek        = prefs.getUChar("tzDstE_W", tz.dstEndWeek);
  tz.dstEndHourLocal   = prefs.getUChar("tzDstE_H", tz.dstEndHourLocal);
  if (!validTimezone(tz)) {                       // corrupt -> default rules
    tz = TIMEZONE_US_EASTERN;
  }
  // Name is informational: keep whatever is stored; default if never saved.
  snprintf(tzNameBuf, sizeof(tzNameBuf), "%s", tz.name);
  if (prefs.isKey("tzName")) {                     // isKey avoids the NVS not-found log
    size_t n = prefs.getString("tzName", tzNameBuf, sizeof(tzNameBuf));
    if (n == 0) snprintf(tzNameBuf, sizeof(tzNameBuf), "%s", tz.name);
  }
  tz.name = tzNameBuf;
  userSettings.timezone = tz;
  Serial.printf("[CFG] timezone: mode=%d %s (std %+ds dst %+ds)\n",
                (int)userSettings.timezoneMode, tz.name, tz.stdOffsetSec, tz.dstOffsetSec);
}

static void loadDisplayPart() {
  userSettings.useCelsius = prefs.getUChar("celsius", 0) != 0;
  userSettings.use24Hour  = prefs.getUChar("clock24", 0) != 0;
  int df = prefs.getUChar("datefmt", 0);
  userSettings.dateFormat = (df == 1) ? 1 : 0;
  Serial.printf("[CFG] display: temp=%s clock=%s datefmt=%d\n",
                userSettings.useCelsius ? "C" : "F",
                userSettings.use24Hour ? "24h" : "12h", userSettings.dateFormat);
}

void loadSettings() {
  // NOTE: the second argument of Preferences::begin() is `readOnly`, not
  // "create if missing" — opening the namespace read-only turns every put*
  // into a silent no-op (it returns 0, which looks like "success").
  if (!prefs.begin("cydwx")) {
    Serial.println("[CFG] NVS open failed; all settings stay at defaults");
    nvsOpen = false;
    return;
  }
  nvsOpen = true;
  loadThemePart();
  loadLocationPart();
  loadTimezonePart();
  loadDisplayPart();
  // The namespace stays open for the app's lifetime: saves happen later,
  // from touch handlers, and reopening per save would be needless churn.
}

// ---- Save (explicit change only; each writes just its own keys) -----------
void saveTheme() {
  if (!nvsOpen) return;
  // put*() returns the bytes written or 0 on failure.
  size_t ok = prefs.putUInt("theme", (uint32_t)userSettings.theme);
  // Record the current numbering with every save, so the legacy migration in
  // loadSettings() can never re-fire after a fresh selection.
  if (ok) ok = prefs.putUInt("themeVer", 2);
  if (!ok) Serial.println("[THEME] NVS save failed");
}

void saveLocation() {
  if (!nvsOpen) return;
  size_t ok = prefs.putUChar("locMode", (uint8_t)userSettings.locationMode);
  if (ok) ok = prefs.putFloat("lat", userSettings.lat);
  if (ok) ok = prefs.putFloat("lon", userSettings.lon);
  if (ok) ok = prefs.putString("postcode", userSettings.postalCode);
  if (ok) ok = prefs.putString("placename", userSettings.placeName);
  if (!ok) Serial.println("[CFG] location save failed");
}

void saveTimezone() {
  if (!nvsOpen) return;
  const TimezoneRules& t = userSettings.timezone;
  size_t ok = prefs.putUChar("tzMode", (uint8_t)userSettings.timezoneMode);
  if (ok) ok = prefs.putString("tzName", t.name ? t.name : "");
  if (ok) ok = prefs.putInt("tzStd", t.stdOffsetSec);
  if (ok) ok = prefs.putInt("tzDst", t.dstOffsetSec);
  if (ok) ok = prefs.putUChar("tzDstS_M", (uint8_t)t.dstStartMonth);
  if (ok) ok = prefs.putUChar("tzDstS_W", (uint8_t)t.dstStartWeek);
  if (ok) ok = prefs.putUChar("tzDstS_H", (uint8_t)t.dstStartHourLocal);
  if (ok) ok = prefs.putUChar("tzDstE_M", (uint8_t)t.dstEndMonth);
  if (ok) ok = prefs.putUChar("tzDstE_W", (uint8_t)t.dstEndWeek);
  if (ok) ok = prefs.putUChar("tzDstE_H", (uint8_t)t.dstEndHourLocal);
  if (!ok) Serial.println("[CFG] timezone save failed");
}

// Settings-page reset (LOCATION page confirmation): clear the saved
// location - coordinates, postal code and place name - and return the
// location to the UNCONFIGURED state so automatic location (Phase 2A) may
// detect it later. Nothing is resolved here, and the timezone keys are not
// touched.
void resetLocation() {
  userSettings.lat = 0.0f;
  userSettings.lon = 0.0f;
  userSettings.postalCode[0] = 0;
  userSettings.placeName[0]  = 0;
  userSettings.locationMode = SettingMode::UNCONFIGURED;
  saveLocation();
}

// Settings-page reset (TIME ZONE page confirmation): return the mode to
// AUTOMATIC. The last valid rules are kept as-is - the next Open-Meteo
// resolution (Phase 2A, inside fetchWeather) replaces them with the zone
// for the saved location. The location keys are not touched.
void resetTimezone() {
  userSettings.timezoneMode = SettingMode::AUTOMATIC;
  saveTimezone();
}

void saveDisplaySettings() {
  if (!nvsOpen) return;
  size_t ok = prefs.putUChar("celsius", userSettings.useCelsius ? 1 : 0);
  if (ok) ok = prefs.putUChar("clock24", userSettings.use24Hour ? 1 : 0);
  if (ok) ok = prefs.putUChar("datefmt", userSettings.dateFormat ? 1 : 0);
  if (!ok) Serial.println("[CFG] display settings save failed");
}

// ---- Automatic-detection seam (future geolocation / timezone services) ----
bool locationAutoAllowed() {
  return userSettings.locationMode == SettingMode::UNCONFIGURED ||
         userSettings.locationMode == SettingMode::AUTOMATIC;
}
bool timezoneAutoAllowed() {
  return userSettings.timezoneMode == SettingMode::UNCONFIGURED ||
         userSettings.timezoneMode == SettingMode::AUTOMATIC;
}

bool applyAutoLocation(float lat, float lon) {
  if (!locationAutoAllowed()) return false;   // MANUAL is never overwritten
  if (!validLatLon(lat, lon)) return false;
  bool changed = (userSettings.locationMode != SettingMode::AUTOMATIC) ||
                 userSettings.lat != lat || userSettings.lon != lon;
  userSettings.lat = lat;
  userSettings.lon = lon;
  userSettings.locationMode = SettingMode::AUTOMATIC;   // UNCONFIGURED -> AUTOMATIC
  if (changed) saveLocation();   // no flash write when nothing actually changed
  Serial.printf("[CFG] auto location applied: %.4f, %.4f\n", lat, lon);
  return true;
}

bool applyAutoTimezone(const TimezoneRules& tz) {
  if (!timezoneAutoAllowed()) return false;   // MANUAL is never overwritten
  if (!validTimezone(tz)) return false;
  bool changed = (userSettings.timezoneMode != SettingMode::AUTOMATIC) ||
                 !tzEqual(userSettings.timezone, tz);
  userSettings.timezone = tz;
  userSettings.timezoneMode = SettingMode::AUTOMATIC;
  if (changed) saveTimezone();
  Serial.printf("[CFG] auto timezone applied: %s (std %+ds dst %+ds)\n",
                tz.name ? tz.name : "?", tz.stdOffsetSec, tz.dstOffsetSec);
  return true;
}

// Phase 2A: the IANA id Open-Meteo reports for the saved coordinates
// (timezone=auto, e.g. "America/New_York"). Maps it to the catalog zone with
// matching rules and applies it as the automatic timezone - a no-op under
// MANUAL mode; an unsupported id leaves the last valid rules in place.
bool applyAutoTimezoneFromIana(const char* iana) {
  int idx = timezoneIndexByIana(iana);
  if (idx < 0) {
    Serial.printf("[TZ] unsupported IANA id \"%s\" - keeping last valid rules\n",
                  iana ? iana : "?");
    return false;
  }
  return applyAutoTimezone(TIMEZONES[idx].rules);
}
