#pragma once

#include "display.h"
#include "nws_alerts.h"

// Full-screen dashboard rendering.
void renderScreen();              // full redraw (after any persistent setting change)
void renderTheme(int idx, const T& t);   // one theme into a transformed region
void updateDynamicTexts();        // clock tick + dynamic bands only (no full redraw)

// "WEATHER ALERT" button (NWS active alerts): per-theme hit/draw rectangle -
// the single source of truth shared by render.cpp (drawing) and touch.cpp
// (hit test). SKY/SPLIT/BENTO draw it as a 60x40 button with "WEATHER"/
// "ALERT" stacked; BASIC keeps the single-line pill. No-op from
// drawAlertButton() when no alert is cached.
struct AlertBtnRect { int x, y, w, h; };
AlertBtnRect alertBtnRect(int theme);
void drawAlertButton(int theme, const T& t);

// Nunito BFF masters (src/nunito_bff.h): nullptr when the fonts failed to
// load, in which case callers fall back to the built-in GLCD font. Used by
// the alert screen for its text.
const lgfx::IFont* nunitoEm9();
const lgfx::IFont* nunitoEm12();
const lgfx::IFont* nunitoEm13();
// Pixel width of an ASCII string at scale 1.0 (BFFfont::updateFontMetric).
int bffWidth(const lgfx::IFont& f, const char* s);
int bffWidth(const lgfx::IFont* f, const char* s);   // null -> 0
