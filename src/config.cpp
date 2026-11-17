#include "config.h"

#include <Preferences.h>
#include "local_config.h"

// ---------------------------------------------------------------------------
//  User settings: defaults = the previously hard-coded behavior.
//  lat/lon come from local_config.h (gitignored site configuration).
//  theme is restored from NVS at boot (loadTheme()); the other fields are
//  runtime state, not persisted yet.
// ---------------------------------------------------------------------------
UserSettings userSettings = {
  LOC_LAT,                // site latitude (local_config.h)
  LOC_LON,                // site longitude (local_config.h)
  TIMEZONE_US_EASTERN,    // US Eastern, US DST rules
  THEME_BASIC,            // default; loadTheme() overwrites with the persisted value
  false,                  // Fahrenheit
  false,                  // US 12-hour clock
  0                       // "Thursday 12 November 2026"
};

// ---- Persistence (NVS namespace "cydwx") ----------------------------------
// "theme"    = selected theme index (current numbering: 0 BASIC .. 3 BENTO)
// "themeVer" = numbering version; 1 = pre-CARDS-removal (5-theme) indices
static Preferences themePrefs;
void loadTheme() {
  // NOTE: the second argument of Preferences::begin() is `readOnly`, not
  // "create if missing" — opening the namespace read-only turns every putUInt
  // into a silent no-op (it returns 0, which looks like "success").
  if (!themePrefs.begin("cydwx")) {
    Serial.println("[THEME] NVS open failed, theme persistence disabled");
    return;
  }
  uint32_t rawT = themePrefs.getUInt("theme", 0);
  uint32_t rawV = themePrefs.getUInt("themeVer", 1);
  uint32_t t = rawT;
  bool migrated = false;
  if (rawV < 2) {   // one-time migration to the 4-theme numbering (CARDS was 2)
    if (t == 3)                            t = THEME_SPLIT;   // SPLIT: 3 -> 2
    else if (t == 4)                       t = THEME_BENTO;   // BENTO: 4 -> 3
    else if (t == 2 || t >= THEME_COUNT)   t = THEME_BASIC;   // old CARDS / unknown
    size_t wmig = themePrefs.putUInt("themeVer", 2);
    if (wmig == 0) Serial.println("[THEME] migration themeVer write failed");
    migrated = true;
  }
  userSettings.theme = (t < THEME_COUNT) ? (int)t : 0;
  Serial.printf("[THEME] NVS: stored theme=%lu ver=%lu%s\n",
                (unsigned long)rawT, (unsigned long)rawV, migrated ? " (migrated)" : "");
}
void saveTheme() {
  // putUInt() returns the bytes written (4) or 0 on failure.
  size_t ok = themePrefs.putUInt("theme", (uint32_t)userSettings.theme);
  // Record the current numbering with every save, so the legacy migration in
  // loadTheme() can never re-fire after a fresh selection.
  if (ok) ok = themePrefs.putUInt("themeVer", 2);
  if (!ok) Serial.println("[THEME] NVS save failed");
}
