
#pragma once

// ---------------------------------------------------------------------------

//  Theme system

//

//  Four display themes (each with day and night variants):

//    0 BASIC - original layout (gLayout coordinates, white/grey background)

//    1 SKY   - full-screen sky gradient, sun/moon + clouds/stars

//    2 SPLIT - data zone left, sky "stage" panel right of the dog

//    3 BENTO - bento grid of rounded cards

//

//  The THEME picker shows a half-scale preview: every renderer takes a T

//  transform (identity for live display) and draws in full-screen coordinates;

//  the clip rect confines it to the preview window.

// ---------------------------------------------------------------------------


#define THEME_COUNT 4

#define THEME_BASIC 0

#define THEME_SKY   1

#define THEME_SPLIT 2

#define THEME_BENTO 3


inline const char* themeName(int i) {

  static const char* names[THEME_COUNT] = {"BASIC", "SKY", "SPLIT", "BENTO"};

  return (i >= 0 && i < THEME_COUNT) ? names[i] : "?";

}
