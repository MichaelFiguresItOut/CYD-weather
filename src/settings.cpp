#include "settings.h"

// ---------------------------------------------------------------------------
//  User-selectable display options (toggled by tapping the screen; runtime
//  state only - not persisted across reboot)
// ---------------------------------------------------------------------------
bool use24Hour      = false;   // tap clock: US 12-hour <-> 24-hour
int  dateFormat     = 0;       // tap date line: 0 = "Thursday 12 November 2026", 1 = "Thursday November 12, 2026"
bool useCelsius     = false;   // tap temperature: F <-> C (all displayed temps)
// THEME picker: shown when the dog image area is tapped. A half-scale preview
// of a theme sits in a yellow-bordered window between two green bars (name on
// top, prev/next names + swipe arrows below). Swipe left/right to change the
// previewed theme; tap anywhere to select it and return to the main display.
bool     themeSelectScreen = false;  // true while the THEME picker is shown
int      activeTheme  = 0;           // displayed theme (persisted in NVS)
int      previewTheme = 0;           // theme shown in the picker's window

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
  activeTheme = (t < THEME_COUNT) ? (int)t : 0;
}
void saveTheme() { themePrefs.putUInt("theme", (uint32_t)activeTheme); }
