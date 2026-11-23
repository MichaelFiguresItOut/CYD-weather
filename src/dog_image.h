// ============================================================================
//  Dog outfit image (bottom-right corner of the dashboard)
//
//  Decodes a PNG from the SD card and keeps it in natural-RGB565 + alpha
//  buffers (palette images: 1 byte/pixel palette indices + PLTE/tRNS tables).
//  main.cpp picks one of the weather-outfit files at the root of the
//  card (see selectDogImage() there) and calls loadDogImage() with an explicit
//  filename whenever the selection changes.
//
//  Supported: non-interlaced, 8-bit PNGs with color type 0 (grey), 2 (RGB),
//  3 (palette) or 6 (RGBA). Images larger than the reserved corner zone are
//  area-averaged down to fit it.
//
//  The panel displays bit-inverted colors and LovyanGFX v1 pushImage() swaps
//  bytes again, so compositeRow() emits pixels pre-transformed for this panel;
//  transparent areas are blended against the background color passed in (the
//  screen background behind the image is white by day, a grey gradient at night).
// ============================================================================
#pragma once

#include <stdint.h>

// Margin between the image and the screen edges (shared with main.cpp).
static const int kDogMargin = 4;

bool loadDogImage(const char* path);   // root-relative filename; call after SD.begin(); false => no usable image
const char* dogCurrentName();          // name of the loaded file ("") if none
int  dogWidth();                       // 0 if not loaded
int  dogHeight();                      // 0 if not loaded

// X coordinate where the image is drawn (bottom-right corner). Returns
// screenWidth when no image is loaded, so callers can use it as the right edge
// of a "text zone" that shrinks only while the image is present.
int dogLeft(int screenWidth);

// Composite row y of the image against a solid background color (displayed-space
// 8-bit RGB) and write panel-ready pixels (alpha-blended, inverted + byte-swapped;
// see file header) into out (must hold at least dogWidth() uint16_t).
void compositeRow(int y, uint8_t bgR, uint8_t bgG, uint8_t bgB, uint16_t* out);

// Opaque span of row y: first/last columns with alpha > 0. Returns false when
// the row is fully transparent (callers must then skip the push entirely -
// compositeRow() would emit the background color for alpha=0 pixels, which
// would erase whatever is drawn under the image's transparent margins, e.g.
// the theme-3 chips whose right edge reaches into the dog box).
bool dogRowSpan(int y, int& x0, int& x1);
