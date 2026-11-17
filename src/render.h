#pragma once

#include "display.h"

// Full-screen dashboard rendering.
void renderScreen();              // full redraw (after any persistent setting change)
void renderTheme(int idx, const T& t);   // one theme into a transformed region
void updateDynamicTexts();        // clock tick + dynamic bands only (no full redraw)
