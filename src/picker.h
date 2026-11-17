#pragma once

// Theme selection screen (dog-zone entry).
void drawThemePicker();
void ramProbe();
// Swipe animation between themes (driven by the touch handler).
void animatePickerSwipe(int stage);

// Picker UI state (defined in picker.cpp): set by the touch handler, read by
// the main loop so it doesn't clobber the picker while it is up.
extern bool themeSelectScreen;   // true while the THEME picker is shown
extern int  previewTheme;        // card highlighted in the picker
