#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "theme_ids.h"

// User UI options + selection screen state.
extern bool use24Hour;             // true: "14:35:09", false: "2:35:09 PM"
extern int  dateFormat;            // 0: "Saturday, May 2, 2026"  1: "Sat, May 2, 2026"  2: "05/02/26"
extern bool useCelsius;            // true: display Celsius, false: Fahrenheit
extern bool     themeSelectScreen; // true: picker up, false: main dashboard
extern int      activeTheme;       // currently displayed theme (0-3)
extern int      previewTheme;      // card highlighted in the picker

// NVS persistence (namespace "cydwx").
void loadTheme();
void saveTheme();
