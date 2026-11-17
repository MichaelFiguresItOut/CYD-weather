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
  themePrefs.begin("cydwx", true);
  uint32_t t = themePrefs.getUInt("theme", 0);
  uint32_t v = themePrefs.getUInt("themeVer", 1);
  if (v < 2) {   // one-time migration to the 4-theme numbering (CARDS was 2)
    if (t == 3)                            t = THEME_SPLIT;   // SPLIT: 3 -> 2
    else if (t == 4)                       t = THEME_BENTO;   // BENTO: 4 -> 3
    else if (t == 2 || t >= THEME_COUNT)   t = THEME_BASIC;   // old CARDS / unknown
    themePrefs.putUInt("themeVer", 2);
  }
  userSettings.theme = (t < THEME_COUNT) ? (int)t : 0;
}
void saveTheme() { themePrefs.putUInt("theme", (uint32_t)userSettings.theme); }
