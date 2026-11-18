#include "display.h"
#include "time_util.h"
#include "dog_image.h"
#include <WiFi.h>
#include "theme_ids.h"
#include "weather.h"

CYDDisplay tft;

// Bring up display + backlight + touch, set landscape orientation
// (moved out of setup(); driver class + pin map live in display.h).
void displayInit() {
  tft.init();
  tft.setRotation(TFT_ROTATION);
  tft.setBrightness(255);
  // Nunito BFF fonts for theme 1 (SKY). Static flash data; on load failure the
  // renderer falls back to the original bitmap-font layout.
  loadNunitoFonts();
  Serial.printf("[FONT] nunito bff: %s\n", nunitoOK ? "ok" : "FAILED - using bitmap fallback");
}

// ---------------------------------------------------------------------------
//  Background (day / night)
//
//  This ST7789 panel displays INVERTED: a value V written to the frame buffer
//  appears as ~V on the glass (that is why filling with TFT_BLACK looks white).
//  Day keeps the classic look (write black -> see white). Night draws a bright
//  grey vertical gradient; the values below are in CODE space, i.e. bit-inverted
//  from what you actually see:
//    top row    written ~0xE72C -> displays ≈ #E5E5E5 (near-white light grey)
//    bottom row written ~0xBEFF -> displays ≈ #BCBCBC (bright grey)
// ---------------------------------------------------------------------------
bool nightMode() { return wx.valid && !wx.isDay; }

uint16_t rgb565Gray(uint8_t v) {            // 0..255 -> RGB565 grey
  uint8_t r = (v * 31 + 127) / 255;
  uint8_t g = (v * 63 + 127) / 255;
  return (uint16_t)((r << 11) | (g << 5) | r);
}

uint16_t nightBgColor(int y) {              // code-space colour for row y
  int t = (tft.height() > 1) ? (y * 255 / (tft.height() - 1)) : 0;
  uint8_t v = (uint8_t)(232 - (232 - 192) * t / 255);   // displayed grey 232 -> 192
  return ~rgb565Gray(v) & 0xFFFF;
}

void fillBackground() {
  if (!nightMode()) { tft.fillScreen(TFT_BLACK); return; }
  for (int y = 0; y < tft.height(); y++)
    tft.fillRect(0, y, tft.width(), 1, nightBgColor(y));
}

// Repaint a region with the current background (solid or per-row gradient) so
// partial updates leave no seams.
void clearRegion(int x, int y, int w, int h) {
  if (!nightMode()) { tft.fillRect(x, y, w, h, TFT_BLACK); return; }
  for (int i = 0; i < h; i++)
    tft.fillRect(x, y + i, w, 1, nightBgColor(y + i));
}

Layout gLayout;   // defaults above = current layout

// ---- Coordinate transform ---------------------------------------------------
const T T::ID = {1.0f, 1.0f, 0, 0};

// ---- Colors -------------------------------------------------------------------
// Colors are specified in DISPLAYED space; this panel inverts every pixel, so all
// palette values are stored pre-inverted: C(r,g,b) = ~rgb565(displayed).
uint16_t D(uint8_t r, uint8_t g, uint8_t b) {
  return tft.color24to16(((uint32_t)r << 16) | ((uint32_t)g << 8) | b);   // displayed space
}
uint16_t C(uint8_t r, uint8_t g, uint8_t b) {
  return ~D(r, g, b) & 0xFFFF;   // code value that DISPLAYS as (r,g,b) on this inverted panel
}

// Theme 1 - SKY (gradient stops in displayed hex)
const SkyPal skyDay = {   // dark text on light sky
  C(0x16,0x32,0x5c), C(0x3d,0x6a,0x9e), C(0x16,0x32,0x5c), C(0x3d,0x5f,0x8f),
  C(0x40,0x58,0x7a), C(0x5b,0x7a,0x9e), C(0x7d,0x92,0xad),
  C(0x2b,0x6c,0xb0), C(0x2f,0x9e,0x6b), C(0xc7,0x7e,0x14),
  C(0xdf,0xf0,0xff), C(0x8f,0xc3,0xf2), C(0x2b,0x6c,0xb0), C(0x1d,0x4f,0x8c),
  C(0xe9,0xf8,0xef), C(0x7c,0xcf,0x9f), C(0x23,0x7a,0x52), C(0xff,0xc8,0x57)
};
const SkyPal skyNight = {
  C(0xff,0xff,0xff), C(0x9f,0xc3,0xe8), C(0xff,0xff,0xff), C(0xcf,0xe3,0xff),
  C(0xd7,0xe3,0xf4), C(0x8a,0xa3,0xc0), C(0x6f,0x87,0xa8),
  C(0x6f,0xb3,0xff), C(0x3e,0xcf,0x8e), C(0xff,0xc8,0x57),
  C(0x25,0x63,0xab), C(0x7c,0xc0,0xff), C(0xbc,0xdc,0xff), C(0xff,0xff,0xff),
  C(0x12,0x3f,0x2c), C(0x3e,0xcf,0x8e), C(0x7e,0xe2,0xb8), C(0xff,0xc8,0x57)
};

// Theme 2 - SPLIT (left data zone + right sky panel at x >= 214)
// SPLIT colors (pre-inverted).
const SplitPal splitDay = {
  C(0xff,0xff,0xff),
  C(0xff,0xff,0xff), C(0x6f,0xb1,0xe2),
  C(0x35,0x61,0x8f), C(0x2f,0x9e,0x6b),
  C(0x16,0x32,0x5c), C(0x4a,0x6f,0xa5), C(0x16,0x32,0x5c), C(0x3d,0x5f,0x8f),
  C(0x5a,0x70,0x89),
  C(0xe3,0xf1,0xff), C(0x9c,0xc7,0xf5), C(0x2b,0x6c,0xb0), C(0x1d,0x4f,0x8c), C(0x2f,0x7f,0xd0),
  C(0xe6,0xf7,0xec), C(0x8f,0xd6,0xae), C(0x23,0x7a,0x52), C(0x2f,0x9e,0x6b),
  C(0x7d,0x92,0xad), C(0xa8,0xb8,0xcc)
};
const SplitPal splitNight = {
  C(0x15,0x1f,0x2e),
  C(0xff,0xc8,0x57), C(0x27,0x4b,0x7d),
  C(0x6f,0xb3,0xff), C(0x3e,0xcf,0x8e),
  C(0xff,0xff,0xff), C(0x9f,0xc3,0xe8), C(0xff,0xff,0xff), C(0xcf,0xe3,0xff),
  C(0xc3,0xd0,0xe4),
  C(0x17,0x3a,0x5f), C(0x5a,0xa9,0xff), C(0x9f,0xd0,0xff), C(0xff,0xff,0xff), C(0x9f,0xd0,0xff),
  C(0x12,0x35,0x2a), C(0x2f,0x9e,0x6b), C(0x7e,0xe2,0xb8), C(0x7e,0xe2,0xb8),
  C(0x8a,0xa3,0xc0), C(0x5f,0x73,0x92)
};

// Theme 3 - BENTO (rounded cards on a solid screen background)
// BENTO colors (design class names noted).
const BentoPal bentoDay = {
  C(0xff,0xff,0xff), C(0xd7,0xe3,0xf4),            // .card bg / border
  C(0x16,0x32,0x5c), C(0x4a,0x6f,0xa5),            // .time / .date
  C(0x2b,0x6c,0xb0), C(0x2f,0x9e,0x6b), C(0xc7,0x7e,0x14),  // .stl / .dot / .str
  C(0x5a,0xa9,0xff),                                // .accent
  C(0x16,0x32,0x5c), C(0x3d,0x5f,0x8f),             // .tempv / .tempu
  C(0xe3,0xf1,0xff), C(0x9c,0xc7,0xf5),             // .fchip bg / border
  C(0x2b,0x6c,0xb0), C(0x1d,0x4f,0x8c),             // .flab / .fval
  C(0x5a,0x70,0x89),                                // .desc
  C(0xe6,0xf7,0xec), C(0x8f,0xd6,0xae),             // .raincard bg / border
  C(0x23,0x7a,0x52), C(0x2f,0x9e,0x6b),             // .rtxt / .rdot
  C(0x7d,0x92,0xad), C(0x9f,0xb0,0xc4),             // .dline / .upd
  C(0xf4,0xf9,0xff), C(0x7d,0x92,0xad)              // .dcard bg / .dcap
};
const BentoPal bentoNight = {
  C(0x1c,0x25,0x34), C(0x2c,0x3a,0x52),
  C(0xff,0xff,0xff), C(0x8f,0xa3,0xc0),
  C(0x6f,0xb3,0xff), C(0x3e,0xcf,0x8e), C(0xff,0xc8,0x57),
  C(0x5a,0xa9,0xff),
  C(0xff,0xff,0xff), C(0x9f,0xc3,0xe8),
  C(0x17,0x3a,0x5f), C(0x5a,0xa9,0xff),
  C(0x9f,0xd0,0xff), C(0xff,0xff,0xff),
  C(0xc3,0xd0,0xe4),
  C(0x12,0x35,0x2a), C(0x2f,0x9e,0x6b),
  C(0x7e,0xe2,0xb8), C(0x3e,0xcf,0x8e),
  C(0x8f,0xa3,0xc0), C(0x5f,0x73,0x92),
  C(0x1a,0x23,0x33), C(0x6f,0x87,0xa8)
};

// Palette accessors (day/night follows the live weather state; the picker's
// preview uses the same variant so it matches what a selection would show).
const SkyPal&   SKY()   { return nightMode() ? skyNight : skyDay; }
const SplitPal& SPLIT() { return nightMode() ? splitNight : splitDay; }
const BentoPal& BENTO() { return nightMode() ? bentoNight : bentoDay; }

// ---- Backgrounds ---------------------------------------------------------------
void lerpRgb(uint8_t ar, uint8_t ag, uint8_t ab,
                    uint8_t br, uint8_t bg, uint8_t bb,
                    float t, uint8_t& r, uint8_t& g, uint8_t& b) {
  if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
  r = (uint8_t)(ar + (br - ar) * t + 0.5f);
  g = (uint8_t)(ag + (bg - ag) * t + 0.5f);
  b = (uint8_t)(ab + (bb - ab) * t + 0.5f);
}

// DISPLAYED RGB565 of the screen background at full-screen position (x,y).
uint16_t bgDisplayed(int idx, int xFull, int yFull) {
  float t = (float)yFull / 239.0f;
  uint8_t r, g, b;
  switch (idx) {
    case 0:   // BASIC: white or the original night grey gradient
      if (!nightMode()) return D(255, 255, 255);
      return ~nightBgColor(yFull) & 0xFFFF;
    case 1: { // SKY: 3-stop vertical gradient (mid at 55%)
      uint8_t tr, tg, tb, mr, mg, mb, br, bgc, bb;
      if (!nightMode()) { tr=0x8f;tg=0xc9;tb=0xef; mr=0xcf;mg=0xe9;mb=0xfb; br=0xea;bgc=0xf6;bb=0xff; }
      else             { tr=0x0a;tg=0x1c;tb=0x38; mr=0x17;mg=0x39;mb=0x6b; br=0x1d;bgc=0x47;bb=0x7f; }
      if (t < 0.55f) lerpRgb(tr,tg,tb, mr,mg,mb, t / 0.55f, r,g,b);
      else           lerpRgb(mr,mg,mb, br,bgc,bb, (t - 0.55f) / 0.45f, r,g,b);
      return D(r,g,b);
    }
    case THEME_SPLIT:   // SPLIT: solid left zone; right panel is a top->bottom gradient
      if (xFull < 214) return nightMode() ? D(0x15,0x1f,0x2e) : D(0xff,0xff,0xff);
      if (!nightMode()) lerpRgb(0x7c,0xc0,0xec, 0xb5,0xdc,0xf6, t, r,g,b);
      else              lerpRgb(0x1c,0x3f,0x74, 0x0e,0x25,0x49, t, r,g,b);
      return D(r,g,b);
    default:  // BENTO: solid
      return nightMode() ? D(0x13,0x1a,0x26) : D(0xee,0xf4,0xfa);
  }
}
uint16_t bgCode(int idx, int xFull, int yFull) { return ~bgDisplayed(idx, xFull, yFull) & 0xFFFF; }

// Draw target for the theme renderers below: &tft normally, or one of the
// pre-render sprites while the swipe animation bakes themes offscreen (see
// animatePickerSwipe / renderThemeIntoSprite).
lgfx::LGFXBase* gT = &tft;

// Full background fill for a theme (per-row gradient sampling).
void drawThemeBg(int idx, const T& t) {
  int rows = max(1, t.H(240));
  for (int i = 0; i < rows; i++) {
    // i is a PREVIEW-space row: fill the actual panel row `t.oy + i` (NOT
    // t.Y(i) — that applies the scale a second time, so at the 0.5x picker
    // preview the 120 rows collapse onto 60 pixels and the bottom half of
    // the window is never painted).
    int yFull = (240 * i + rows / 2) / rows;
    if (idx == THEME_SPLIT) {   // split: left solid + right per-row gradient
      gT->fillRect(t.X(0),   t.oy + i, max(1, t.W(214)),     1, bgCode(THEME_SPLIT, 0, yFull));
      gT->fillRect(t.X(214), t.oy + i, max(1, t.W(320 - 214)), 1, bgCode(THEME_SPLIT, 267, yFull));
    } else {
      gT->fillRect(t.X(0), t.oy + i, max(1, t.W(320)), 1, bgCode(idx, 0, yFull));
    }
  }
}

// Clear a band back to the theme background (partial updates for themes 1-3).
// Coordinates are full-space; T::ID for live display.
void clearThemed(int idx, const T& t, int xFull, int yFull, int wFull, int hFull) {
  for (int i = 0; i < hFull; i++)
    gT->fillRect(t.X(xFull), t.Y(yFull + i), max(1, t.W(wFull)), 1, bgCode(idx, 0, yFull + i));
}

// ---- Text helpers ------------------------------------------------------------------
// Shrink-to-fit: returns the (possibly reduced) full-space text size for buf.
float fitSize(const char* buf, float sBase, int avail) {
  gT->setTextSize(sBase);
  int w = gT->textWidth(buf);
  if (w > avail && w > 0) return sBase * (float)avail / (float)w;
  return sBase;
}

// Centered text at full-space center cx, row yFull.
void drawCenter(const T& t, int cx, int yFull, const char* buf, float s, uint16_t c, int avail) {
  float fs = fitSize(buf, s, avail);
  gT->setTextSize(fs);
  int w = gT->textWidth(buf);          // full-space width at fs
  gT->setTextSize(fs * t.sx);
  gT->setTextColor(c);
  gT->drawString(buf, t.X(cx - w / 2.0f), t.Y(yFull));
}

// Right-aligned text ending at xEnd (full space).
void drawRight(const T& t, int xEnd, int yFull, const char* buf, float s, uint16_t c) {
  gT->setTextSize(s);
  int w = gT->textWidth(buf);
  gT->setTextSize(s * t.sx);
  gT->setTextColor(c);
  gT->drawString(buf, t.X(xEnd - w), t.Y(yFull));
}

// Two strings on one row: left at xL, right ending at xR; both shrink together.
void drawSplitRow(const T& t, int xL, int xR, int yFull, const char* l, const char* r, float s, uint16_t c) {
  gT->setTextSize(s);
  int wl = gT->textWidth(l), wr = gT->textWidth(r);
  int gap = (int)(8 * s + 0.5f);
  float fs = s;
  if (wl + wr + gap > xR - xL && wl + wr > 0) fs = s * (float)(xR - xL - gap) / (float)(wl + wr);
  gT->setTextSize(fs);
  wl = gT->textWidth(l); wr = gT->textWidth(r);
  gT->setTextSize(fs * t.sx);
  gT->setTextColor(c);
  gT->drawString(l, t.X(xL), t.Y(yFull));
  if (wr > 0) gT->drawString(r, t.X(xR - wr), t.Y(yFull));
}

// ---- Decorations ---------------------------------------------------------------------
// Thick line (v1's drawLine has no width argument): Bresenham path of filled dots.
void thickLine(const T& t, int x0f, int y0f, int x1f, int y1f, float wFull, uint16_t c) {
  int x0 = t.X(x0f), y0 = t.Y(y0f), x1 = t.X(x1f), y1 = t.Y(y1f);
  int r = max(1, (int)(wFull * t.sx / 2.0f + 0.5f));
  int dx = abs(x1 - x0), dy = -abs(y1 - y0);
  int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    gT->fillCircle(x0, y0, r, c);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

// Flat sun (SKY): pale halo disc + core + 8 short rays at the rim.
void drawSun(const T& t, int cx, int cy, uint16_t halo, uint16_t core, uint16_t ray) {
  float s = t.sx;
  int X = t.X(cx), Y = t.Y(cy);
  gT->fillCircle(X, Y, (int)(17 * s + 0.5f), halo);
  const int d[8][2] = {{0,-1},{1,0},{0,1},{-1,0},{1,-1},{1,1},{-1,1},{-1,-1}};
  for (int i = 0; i < 8; i++) {
    float m = (d[i][0] != 0 && d[i][1] != 0) ? 0.707f : 1.0f;   // normalize diagonals
    thickLine(t, cx + (int)(16 * d[i][0] * m), cy + (int)(16 * d[i][1] * m),
              cx + (int)(12 * d[i][0] * m), cy + (int)(12 * d[i][1] * m), 2.5f, ray);
  }
  gT->fillCircle(X, Y, (int)(11.5f * s + 0.5f), core);
}

// SPLIT flat sun: core r6 + short rays, no halo (18px icon in the panel).
void drawFlatSun(const T& t, int cx, int cy) {
  float s = t.sx;
  int X = t.X(cx), Y = t.Y(cy);
  const uint16_t core = C(0xff,0xd7,0x5e), ray = C(0xf0,0xb4,0x29);
  const int d[8][2] = {{0,-1},{1,0},{0,1},{-1,0},{1,-1},{1,1},{-1,1},{-1,-1}};
  for (int i = 0; i < 8; i++) {
    float m = (d[i][0] != 0 && d[i][1] != 0) ? 0.707f : 1.0f;
    thickLine(t, cx + (int)(6.5 * d[i][0] * m), cy + (int)(6.5 * d[i][1] * m),
              cx + (int)(8.5 * d[i][0] * m), cy + (int)(8.5 * d[i][1] * m), 1.4f, ray);
  }
  gT->fillCircle(X, Y, (int)(6 * s + 0.5f), core);
}

// SPLIT panel cloud (stationary): rounded bar with two bumps above it.
void drawSplitCloud(const T& t, int x, int y, bool small, uint16_t c) {
  float s = t.sx;
  if (!small) {   // pc1: 52x13 bar; bumps r9 @(+18,-7), r6 @(+33,+2)
    gT->fillRoundRect(t.X(x), t.Y(y), max(2, (int)(52 * s + 0.5f)), max(2, (int)(13 * s + 0.5f)),
                      (int)(8 * s + 0.5f), c);
    gT->fillCircle(t.X(x + 18), t.Y(y - 7), (int)(9 * s + 0.5f), c);
    gT->fillCircle(t.X(x + 33), t.Y(y + 2), (int)(6 * s + 0.5f), c);
  } else {        // pc2: 40x11 bar; bumps r7 @(+14,+1), r5 @(+25,+1)
    gT->fillRoundRect(t.X(x), t.Y(y), max(2, (int)(40 * s + 0.5f)), max(2, (int)(11 * s + 0.5f)),
                      (int)(6 * s + 0.5f), c);
    gT->fillCircle(t.X(x + 14), t.Y(y + 1), (int)(7 * s + 0.5f), c);
    gT->fillCircle(t.X(x + 25), t.Y(y + 1), (int)(5 * s + 0.5f), c);
  }
}

// Blend a pre-inverted RGB565 color toward white by pct/100.
uint16_t blendWhite(uint16_t c, int pct) {
  int r = ((c >> 11) & 31u), g = ((c >> 5) & 63u), b = (c & 31u);
  auto mix = [pct](int v, int maxv) { return (v * (100 - pct) + maxv * pct) / 100; };
  return D(mix(r, 31), mix(g, 63), mix(b, 31));
}

// SPLIT description cloud icon (~14px): three bumps over a flat bottom.
void drawMiniCloud(const T& t, int x, int y, uint16_t c) {
  float s = t.sx;
  gT->fillCircle(t.X(x + 4), t.Y(y + 7), max(1, (int)(3 * s + 0.5f)), c);
  gT->fillCircle(t.X(x + 8), t.Y(y + 5), max(1, (int)(4 * s + 0.5f)), c);
  gT->fillCircle(t.X(x + 11), t.Y(y + 7), max(1, (int)(3 * s + 0.5f)), c);
  gT->fillRect(t.X(x + 2), t.Y(y + 7), max(1, (int)(11 * s)), max(1, (int)(4 * s)), c);
}

// Flat cloud: rounded bar with two bumps on top; k scales the whole shape.
void drawCloud(const T& t, int x, int y, float k, uint16_t c) {
  float s = t.sx * k;
  int X = t.X(x), Y = t.Y(y);
  int W = max(2, (int)(52 * s + 0.5f));
  int Hh = max(3, (int)(16 * s + 0.5f));
  gT->fillRoundRect(X, Y, W, Hh, min(Hh / 2, (int)(8 * s + 0.5f)), c);
  gT->fillCircle(X + (int)(23 * s), Y + (int)(2 * s), max(1, (int)(11 * s + 0.5f)), c);
  gT->fillCircle(X + (int)(42 * s), Y + (int)(2 * s), max(1, (int)(8 * s + 0.5f)), c);
}

// Tiny star dot (design .star).
void drawStar(const T& t, int x, int y, uint16_t c) {
  int d = max(1, (int)(2 * t.sx));
  gT->fillRect(t.X(x), t.Y(y), d, d, c);
}

// Small umbrella (SKY rain chip): half-disc canopy + straight shaft.
// bgC flattens the lower half of the canopy circle; only used where the icon
// sits on a solid chip background. k scales the whole shape (~13px tall at 1).
void drawUmbrella(const T& t, int cx, int cy, float k, uint16_t c, uint16_t bgC) {
  float s = t.sx * k;
  int X = t.X(cx), Y = t.Y(cy);
  int r = max(2, (int)(5.0f * s + 0.5f));
  int cyC = Y - (int)(3.5f * s);                    // canopy center line
  gT->fillCircle(X, cyC, r, c);
  gT->fillRect(X - r, cyC + 1, 2 * r, r, bgC);     // keep only the top half
  int sw = max(1, (int)(1.6f * s));                 // shaft
  gT->fillRect(X - sw / 2, cyC + 1, sw, (int)(7.5f * s), c);
}

// Tiny sun / crescent for the status-row DAY/NIGHT label (SKY st-r icon).
// biteC: color cut out of the crescent (the surface the icon sits on).
void drawDnIcon(const T& t, int cx, int cy, uint16_t c, uint16_t biteC) {
  float s = t.sx;
  int X = t.X(cx), Y = t.Y(cy);
  if (!nightMode()) {   // sun: disc + 8 short rays
    gT->fillCircle(X, Y, max(2, (int)(3 * s + 0.5f)), c);
    const int d[8][2] = {{0,-1},{1,0},{0,1},{-1,0},{1,-1},{1,1},{-1,1},{-1,-1}};
    for (int i = 0; i < 8; i++) {
      float m = (d[i][0] != 0 && d[i][1] != 0) ? 0.707f : 1.0f;   // normalize diagonals
      thickLine(t, cx + (int)(5 * d[i][0] * m), cy + (int)(5 * d[i][1] * m),
                cx + (int)(6.5f * d[i][0] * m), cy + (int)(6.5f * d[i][1] * m), 1.2f, c);
    }
  } else {   // crescent: disc with a background-colored bite
    gT->fillCircle(X, Y, max(2, (int)(4 * s + 0.5f)), c);
    gT->fillCircle(X + (int)(1.8f * s), Y - (int)(1.6f * s),
                   max(2, (int)(3.4f * s + 0.5f)), biteC);
  }
}

// ---- Widgets ---------------------------------------------------------------------------
// "DAY"/"NIGHT" (or "--" before the first fetch) for themed layouts.
const char* dayNightLabel() {
  return !wx.isDayKnown ? "--" : (wx.isDay ? "DAY" : "NIGHT");
}

// Left status: dot + "WIFI NTP"/"--OFF-- --NTP"; optional right label.
void buildWifiStatus(char* buf, size_t n) {
  snprintf(buf, n, "%s%s", (WiFi.status() == WL_CONNECTED) ? "WIFI" : "--OFF--",
           ntpSynced ? " NTP" : " --NTP");
}

void drawStatusRow(const T& t, int xL, int yFull, float s, uint16_t cL, uint16_t dotC,
                          const char* right, uint16_t cR, int xREnd, bool dnIcon) {
  char status[24];
  buildWifiStatus(status, sizeof(status));
  gT->setTextSize(s * t.sx);
  int dr = max(1, (int)(2.5f * s * t.sx + 0.5f));
  gT->fillCircle(t.X(xL) + dr, t.Y(yFull + 4 * s), dr, dotC);
  gT->setTextColor(cL);
  gT->drawString(status, t.X(xL + 8 * s), t.Y(yFull));
  if (right && right[0]) {
    gT->setTextSize(s);
    int rw = gT->textWidth(right);   // full-space width
    gT->setTextSize(s * t.sx);
    gT->setTextColor(cR);
    gT->drawString(right, t.X(xREnd - rw), t.Y(yFull));
    if (dnIcon) {   // small sun/moon left of the label (SKY)
      int icx = xREnd - rw - (int)(9 * s);
      drawDnIcon(t, icx, yFull + 4 * s, cR, bgCode(1, icx, yFull + 4 * s));
    }
  }
}

// Rounded "FEELS LIKE" chip: label left, value right.
void drawFeelsChip(const T& t, int x, int yF, int w, int h,
                          uint16_t bgC, uint16_t bdC, float sLbl, uint16_t cLbl,
                          const char* val, float sVal, uint16_t cVal) {
  gT->fillRoundRect(t.X(x), t.Y(yF), max(2, t.W(w)), max(2, t.H(h)), (int)(9 * t.sx + 0.5f), bgC);
  gT->drawRoundRect(t.X(x), t.Y(yF), max(1, t.W(w) - 1), max(1, t.H(h) - 1), (int)(8 * t.sx + 0.5f), bdC);
  gT->setTextSize(sLbl);
  int lw = gT->textWidth("FEELS LIKE");   // full-space width
  gT->setTextSize(sLbl * t.sx);
  gT->setTextColor(cLbl);
  gT->drawString("FEELS LIKE", t.X(x + (int)(10 * sLbl)), t.Y(yF + (h - 8 * sLbl) / 2.0f));
  float fs = fitSize(val, sVal, max(8, w - (int)(24 * sVal) - lw));
  gT->setTextSize(fs);
  int vw = gT->textWidth(val);
  gT->setTextSize(fs * t.sx);
  gT->setTextColor(cVal);
  gT->drawString(val, t.X(x + w - (int)(10 * fs) - vw), t.Y(yF + (h - 8 * fs) / 2.0f));
}

// Rounded rain-status banner with a small icon left of the centered text:
// plain dot by default; umbrella when rain is falling/nearby/forecast (theme 1).
void drawRainChip(const T& t, int x, int yF, int w, int h,
                         uint16_t bgC, uint16_t bdC, const char* txt, float s,
                         uint16_t cTxt, uint16_t dotC, bool umbrella) {
  gT->fillRoundRect(t.X(x), t.Y(yF), max(2, t.W(w)), max(2, t.H(h)), (int)(8 * t.sx + 0.5f), bgC);
  gT->drawRoundRect(t.X(x), t.Y(yF), max(1, t.W(w) - 1), max(1, t.H(h) - 1), (int)(7 * t.sx + 0.5f), bdC);
  float fs = fitSize(txt, s, w - (int)(24 * s));
  gT->setTextSize(fs);
  int tw = gT->textWidth(txt);   // full-space width
  int cxFull = x + w / 2;
  if (umbrella)
    drawUmbrella(t, cxFull - tw / 2.0f - (int)(9 * fs), yF + h / 2.0f, fs / 1.6f, dotC, bgC);
  else {
    int dr = max(1, (int)(3 * fs * t.sx + 0.5f));
    gT->fillCircle(t.X(cxFull - tw / 2.0f - (int)(9 * fs)), t.Y(yF + h / 2.0f), dr, dotC);
  }
  gT->setTextSize(fs * t.sx);
  gT->setTextColor(cTxt);
  gT->drawString(txt, t.X(cxFull - tw / 2.0f), t.Y(yF + (h - 8 * fs) / 2.0f));
}

// Rounded card fill + border.
void drawCard(const T& t, int x, int y, int w, int h, uint16_t bgC, uint16_t bdC) {
  gT->fillRoundRect(t.X(x), t.Y(y), max(2, t.W(w)), max(2, t.H(h)), (int)(10 * t.sx + 0.5f), bgC);
  gT->drawRoundRect(t.X(x), t.Y(y), max(1, t.W(w) - 1), max(1, t.H(h) - 1), (int)(9 * t.sx + 0.5f), bdC);
}

// ---- Shared string builders ---------------------------------------------------------------
// Rain status line shared by all themes (same logic as the original layout).
void buildRainLine(char* buf, size_t n) {
  if (!wx.valid) { snprintf(buf, n, "NO WEATHER DATA"); return; }
  if (wx.rainingNow) {
    if (wx.rainEnd[0] && wx.rainMayContinue) snprintf(buf, n, "RAIN NOW - MAY CONTINUE");
    else if (wx.rainEnd[0])                  snprintf(buf, n, "RAIN NOW - ends ~%s", wx.rainEnd);
    else                                     snprintf(buf, n, "RAIN NOW");
  } else if (wx.rainNearby && wx.nearbyStns[0]) {
    snprintf(buf, n, "RAIN NEARBY (%s)", wx.nearbyStns);
  } else if (wx.nextRain[0]) {
    const char* verb = wx.nextRainFrom15min ? "expected" : "possible";
    snprintf(buf, n, "Rain %s %s%s", verb, wx.nextRain, wx.nextRainTomorrow ? " tomorrow" : "");
  } else {
    snprintf(buf, n, "No rain expected");
  }
}

// Short "UPD h:mm AM/PM" / "UPD HH:MM" in the current clock mode (empty if no update yet).
void buildUpdShort(char* buf, size_t n) {
  if (wx.lastUpdateEpoch <= 1700000000UL) { buf[0] = '\0'; return; }
  char tbuf[16];
  struct tm lt;
  time_t local = wx.lastUpdateEpoch + localOffsetSec(wx.lastUpdateEpoch);
  localtime_r(&local, &lt);
  formatUpdTime(tbuf, sizeof(tbuf), &lt, false);
  snprintf(buf, n, "UPD %s", tbuf);
}

// Temperature string with unit ("--.-" when unknown).
void buildTempStr(char* buf, size_t n, float v) {
  if (isnan(v)) snprintf(buf, n, "--.-");
  else          snprintf(buf, n, "%.1f %s", tempForDisplay(v), tempUnitChar());
}

// Human label for the dog's current outfit band (bento caption).
const char* dogBandLabel() {
  const char* name = dogCurrentName();
  if (!name || !name[0]) return "NO DATA";
  if (strncmp(name, "error", 5) == 0)        return "NO DATA";
  if (strncmp(name, "rain_now", 8) == 0)     return "RAINING";
  if (strncmp(name, "rain_later", 10) == 0)  return "RAIN SOON";
  static const struct { const char* band; const char* label; } bands[] = {
    {"extreme_cold", "EXTREME COLD"}, {"very_cold", "VERY COLD"}, {"cold", "COLD"},
    {"cool", "COOL"}, {"mild_cool", "MILD COOL"}, {"mild_warm", "MILD WARM"}, {"hot", "HOT"}
  };
  for (auto& b : bands) if (strncmp(name, b.band, strlen(b.band)) == 0) return b.label;
  return "?";
}

// ---- Dog compositing ------------------------------------------------------------------------
// Displayed background color behind the dog for a theme at full-screen row y.
void dogBgDisplayed(int theme, int yFull, uint8_t& r, uint8_t& g, uint8_t& b) {
  if (theme == THEME_SPLIT) {   // dog sits on the right panel: its gradient
    float t = (float)yFull / 239.0f;
    if (!nightMode()) lerpRgb(0x7c,0xc0,0xec, 0xb5,0xdc,0xf6, t, r,g,b);
    else              lerpRgb(0x1c,0x3f,0x74, 0x0e,0x25,0x49, t, r,g,b);
    return;
  }
  if (theme == THEME_BENTO) {   // solid dog card
    uint16_t d = nightMode() ? D(0x1a,0x23,0x33) : D(0xf4,0xf9,0xff);
    r = (uint8_t)(((d >> 11) & 31u) * 255 / 31);
    g = (uint8_t)(((d >> 5) & 63u) * 255 / 63);
    b = (uint8_t)((d & 31u) * 255 / 31);
    return;
  }
  uint16_t d = bgDisplayed(theme, 0, yFull);   // themes 0-1: screen background
  r = (uint8_t)(((d >> 11) & 31u) * 255 / 31);
  g = (uint8_t)(((d >> 5) & 63u) * 255 / 63);
  b = (uint8_t)((d & 31u) * 255 / 31);
}

// Composite + blit the current dog image. The full-res region is
// (xFull,yTopFull,fw,fh) in screen space; t scales it for previews (nearest
// neighbour). Transparent pixels show the theme's background behind them.
void drawDogBox(const T& t, int xFull, int yTopFull, int fw, int fh, int theme) {
  if (fw <= 0 || fh <= 0) return;
  int px = t.X(xFull), py = t.Y(yTopFull);
  int pw = max(1, t.W(fw)), ph = max(1, t.H(fh));
  static uint16_t row[128];    // >= kMaxW (96) full-res pixels
  for (int i = 0; i < ph; i++) {
    int imgRow = (i * fh) / ph;                       // nearest-neighbour source row
    int x0, x1;
    if (!dogRowSpan(imgRow, x0, x1)) continue;   // fully transparent row: leave what's underneath
    uint8_t br, bgc, bb;
    dogBgDisplayed(theme, yTopFull + imgRow, br, bgc, bb);
    compositeRow(imgRow, br, bgc, bb, row);
    if (pw == fw) { gT->pushImage(px + x0, py + i, x1 - x0 + 1, 1, row + x0); continue; }
    static uint16_t hrow[64];   // >= kMaxW/2
    int sx0 = (x0 * pw) / fw, sx1 = (x1 * pw) / fw;
    for (int j = sx0; j <= sx1 && j < pw; j++) hrow[j - sx0] = row[(j * fw) / pw];
    gT->pushImage(px + sx0, py + i, sx1 - sx0 + 1, 1, hrow);
  }
}

// Standard bottom-right corner placement (themes 0-2).
void drawDogImage(const T& t, int theme) {
  int fw = dogWidth();
  if (fw <= 0) return;
  int fh = dogHeight();
  drawDogBox(t, dogLeft(320), 240 - fh - kDogMargin, fw, fh, theme);
}

// ---------------------------------------------------------------------------
//  Display hard reset — see docs/CYD-HARDWARE.md "Display gotchas" (ST7789).
//  The ST7789 is a separate chip with its own volatile state: resetting the
//  ESP32 (RST button / flashing) does NOT reset it, and LGFX's Panel_ST7789
//  init list contains no SWRESET (0x01). Kept as defense against stale panel
//  state (e.g. a brownout glitch during boot). NOTE: the persistent pale/
//  yellow patches this was originally added for turned out to be a software
//  rendering bug (the bffCenter scale-argument bug — see README), not stale
//  panel state; no reset could ever fix that.
//  Belt and suspenders:
//    1) hardware pulse on TFT_RST (datasheet min ~5 ms low / 10 ms after
//       release; we use 100/100 for margin),
//    2) SWRESET (0x01) over SPI — recovers the panel even if its RST line is
//       not reachable from this GPIO, as long as the SPI command interface
//       still latches.
// ---------------------------------------------------------------------------
void displayHardReset() {
  // 1) Hardware reset pulse.
  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_RST, LOW);
  delay(100);
  digitalWrite(TFT_RST, HIGH);
  delay(100);

  // 2) Software reset over SPI (same VSPI host LGFX uses; tft.init()
  //    re-initializes the peripheral afterwards).
  {
    static SPIClass tftSpi(VSPI);   // Arduino constant for the SPI3 peripheral
    pinMode(TFT_CS, OUTPUT);
    digitalWrite(TFT_CS, HIGH);
    pinMode(TFT_DC, OUTPUT);
    digitalWrite(TFT_DC, LOW);      // command mode (DC/CS are not managed by Arduino SPI)
    tftSpi.begin(TFT_SCK, -1, TFT_MOSI, TFT_CS);
    tftSpi.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    digitalWrite(TFT_CS, LOW);      // HW CS is off -> toggle manually
    tftSpi.transfer(0x01);          // SWRESET
    digitalWrite(TFT_CS, HIGH);
    tftSpi.endTransaction();
  }
  delay(120);   // ST7789 needs up to 120 ms after SWRESET before it accepts commands
}
