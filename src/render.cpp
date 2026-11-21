#include "render.h"
#include "weather.h"
#include "config.h"
#include "time_util.h"
#include "dog_image.h"
#include "nunito_bff.h"

static int textZoneRight() { return dogLeft(tft.width()); }

// ---------------------------------------------------------------------------
//  Display helpers – theme 0 (BASIC). All take a T transform so they can also
//  be rendered into the picker's half-scale preview window.
// ---------------------------------------------------------------------------

// Clock + date strings for the current time/format settings (shared by all
// themes and the partial-update cache).
static void buildClockDateStrings(char* clockBuf, size_t n1, char* dateBuf, size_t n2) {
  if (ntpSynced) {
    struct tm lt;
    nowLocal(&lt);
    formatClock(clockBuf, n1, &lt);   // respects use24Hour
    formatDateLine(dateBuf, n2, &lt);  // respects dateFormat
  } else {
    snprintf(clockBuf, n1, "--:--");
    snprintf(dateBuf, n2, "Waiting for NTP sync ...");
  }
}

// Full-space clock text size for a theme (shrinks so the widest string in the
// current time format fits its zone). Used by renderers AND partial updates.
static float themeClockFit(int idx) {
  const char* mx = clockMaxString();
  float s; int avail;
  switch (idx) {
    case 0:  return gLayout.clockSize;
    case THEME_SKY:   s = 5.25f; avail = 316; break;
    case THEME_SPLIT: s = 4.6f;  avail = 198; break;   // left zone [12,210)
    default:          s = 3.4f;  avail = 288; break;   // bento clock card
  }
  gT->setTextSize(s);
  int w = gT->textWidth(mx);
  return (w > avail && w > 0) ? s * (float)avail / (float)w : s;
}

static void drawStatusBar(const T& t) {
  // Single-arg setTextColor: glyph cells are NOT filled (back == fore), so the
  // text sits directly on whatever background is underneath (white or gradient).
  gT->setTextSize(1 * t.sx);
  gT->setTextColor(TFT_CYAN);

  char status[24];
  buildWifiStatus(status, sizeof(status));
  gT->drawString(status, t.X(4), t.Y(gLayout.statusBarY));

  // Persistent top-right day/night status (independent verification of is_day):
  //   "Day"             -> valid is_day == 1
  //   "Night"           -> valid is_day == 0
  //   "Day/Night ERROR" -> missing / invalid / not yet available
  const char* dn = !wx.isDayKnown ? "Day/Night ERROR"
                                  : (wx.isDay ? "Day" : "Night");
  gT->setTextSize(1);
  int dw = gT->textWidth(dn);   // full-space width at size 1
  gT->setTextSize(t.sx);
  gT->drawString(dn, t.X(320 - dw - 4), t.Y(gLayout.statusBarY));
}

// Cache of what is currently on screen for the clock/date lines. Kept in
// sync by every full render (drawClockAndDate) and used by updateDynamicTexts()
// to repaint only when the text actually changed.
static char lastClockBuf[16] = {0};
static char lastDateBuf[32] = {0};

// Choose a text size for the date line so the whole string always fits on
// screen. Normal strings render at exactly gLayout.dateSize; only very long
// ones (e.g. "Wednesday September 30, 2026" = 336 px at size 2 on this fixed-
// pitch font) are scaled down slightly. Returns the chosen FULL-space size and
// leaves tft's text size set to it.
static float dateLineFit(const char* buf) {
  const int avail = 320 - 4;   // small margin each side
  float s = (float)gLayout.dateSize;
  gT->setTextSize(s);
  int w = gT->textWidth(buf);
  if (w > avail && w > 0) {
    s *= (float)avail / (float)w;
    gT->setTextSize(s);
  }
  return s;
}

static void drawClockAndDate(const T& t) {
  char clockBuf[16];
  char dateBuf[32];

  // No comma after the weekday (keeps the line shorter; dateLineFit() scales
  // down any line that would still run off the screen).
  buildClockDateStrings(clockBuf, sizeof(clockBuf), dateBuf, sizeof(dateBuf));

  // Keep the partial-update cache in sync with what was just drawn so a full
  // render (e.g. right after a format toggle) causes no redundant repaint on
  // the next per-second tick.
  strncpy(lastClockBuf, clockBuf, sizeof(lastClockBuf) - 1);
  lastClockBuf[sizeof(lastClockBuf) - 1] = '\0';
  strncpy(lastDateBuf, dateBuf, sizeof(lastDateBuf) - 1);
  lastDateBuf[sizeof(lastDateBuf) - 1] = '\0';

  // Big digital clock, centred in the upper third
  gT->setTextSize(gLayout.clockSize);      // ~24 px tall digits at size 4
  int cw = gT->textWidth(clockBuf);
  int cx = (320 - cw) / 2;

  gT->setTextSize(gLayout.clockSize * t.sx);
  gT->setTextColor(TFT_WHITE);
  gT->drawString(clockBuf, t.X(cx), t.Y(gLayout.clockY));

  // Full date line under the clock (scaled down slightly if too long – see dateLineFit)
  float ds = dateLineFit(dateBuf);         // chosen full-space size (normally exactly gLayout.dateSize)
  gT->setTextSize(ds * t.sx);
  gT->setTextColor(TFT_YELLOW);
  int dw = gT->textWidth(dateBuf);         // width at the chosen size
  gT->drawString(dateBuf, t.X((320 - dw) / 2), t.Y(gLayout.dateY));
}

static void drawWeatherPanel(const T& t) {
  int y = gLayout.tempY;   // top of the weather block (layout)

  if (!wx.valid) {
    // Placed above the image top (y=108 for a full-size picture), so these can
    // stay full-width centred without touching it.
    gT->setTextSize(2 * t.sx);
    gT->setTextColor(TFT_GRAY);
    const char* msg1 = "No weather data yet";
    gT->drawString(msg1, t.X((320 - gT->textWidth(msg1)) / 2), t.Y(y - 22));

    const char* msg2 = "Fetching from Open-Meteo ...";
    gT->setTextSize(1 * t.sx);
    gT->drawString(msg2, t.X((320 - gT->textWidth(msg2)) / 2), t.Y(y - 2));
    return;
  }

  // Everything in the weather block is centered in the text zone
  // [0, textZoneRight()) so it never collides with the dog image in the
  // bottom-right corner. The two size-2 rows of the original layout were too
  // wide for that zone and are drawn at size 1 instead.
  int zoneRight = textZoneRight();

  // Current temperature (large). Unit follows the tap-toggled useCelsius flag;
  // feels-like and today H/L below convert the same way so the screen is
  // consistent.
  char tmp[16];
  snprintf(tmp, sizeof(tmp), "%.1f %s", tempForDisplay(wx.temp), tempUnitChar());
  int fs = gLayout.tempSize;   // big digits
  gT->setTextSize(fs);
  int tw = gT->textWidth(tmp);
  gT->setTextSize(fs * t.sx);
  gT->setTextColor(TFT_WHITE);
  gT->drawString(tmp, t.X((zoneRight - tw) / 2), t.Y(y));

  // Weather description
  gT->setTextSize(2);
  String desc = wx.description;
  int dw = gT->textWidth(desc.c_str());
  gT->setTextSize(2 * t.sx);
  gT->setTextColor(TFT_CYAN);
  gT->drawString(desc, t.X((zoneRight - dw) / 2), t.Y(y + gLayout.descOffset));

  // Feels-like & humidity row
  char line[64];
  snprintf(line, sizeof(line), "Feels %.1f %s   Hum %d%%",
           tempForDisplay(wx.feelsLike), tempUnitChar(), wx.humidity);
  gT->setTextSize(1);
  int lw = gT->textWidth(line);
  gT->setTextSize(t.sx);
  gT->setTextColor(TFT_GREENYELLOW);
  gT->drawString(line, t.X((zoneRight - lw) / 2), t.Y(y + gLayout.feelsOffset));

  // Today high / low
  snprintf(line, sizeof(line), "Today  H %.1f   L %.1f",
           tempForDisplay(wx.tMax), tempForDisplay(wx.tMin));
  gT->setTextSize(1);
  lw = gT->textWidth(line);
  gT->setTextColor(TFT_SKYBLUE);
  gT->drawString(line, t.X((zoneRight - lw) / 2), t.Y(y + gLayout.todayOffset));

  // Rain status line (size 1): RAIN NOW (+ forecast end) / RAIN NEARBY / next period / none.
  // While it is raining now or nearby, future-only messages are suppressed so the
  // line never says "Rain expected ..." while rain is actually falling. The shared
  // builder (buildRainLine) also serves themes 1-3.
  char rbuf[48];
  buildRainLine(rbuf, sizeof(rbuf));
  gT->setTextSize(1);
  lw = gT->textWidth(rbuf);
  gT->setTextSize(t.sx);
  gT->setTextColor(TFT_GREENYELLOW);   // displays dark blue on this panel
  gT->drawString(rbuf, t.X((zoneRight - lw) / 2), t.Y(y + gLayout.rainOffset));

  // Last update time ("Updated H:MM:SS AM/PM", local time)
  if (wx.lastUpdateEpoch > 1700000000UL) {
    char tbuf[16];
    struct tm lt;
    time_t local = wx.lastUpdateEpoch + localOffsetSec(wx.lastUpdateEpoch);
    localtime_r(&local, &lt);
    formatUpdTime(tbuf, sizeof(tbuf), &lt, true);
    snprintf(line, sizeof(line), "Updated %s", tbuf);
    gT->setTextSize(1);
    lw = gT->textWidth(line);
    gT->setTextSize(t.sx);
    gT->setTextColor(TFT_DARKGRAY);
    gT->drawString(line, t.X((zoneRight - lw) / 2), t.Y(y + gLayout.updatedOffset));
  }
}

static void drawTouchHint(const T& t) {
  const char* buf = "touch: refresh";
  gT->setTextSize(1);
  int w = gT->textWidth(buf);
  gT->setTextSize(t.sx);
  gT->setTextColor(TFT_DARKGRAY);
  gT->drawString(buf, t.X((320 - w) / 2), t.Y(240 - gLayout.hintBottomGap));
}

// ---------------------------------------------------------------------------
//  Theme renderers – one per theme. T::ID for live display; a half-scale T
//  plus clip rect inside the picker's preview window.
// ---------------------------------------------------------------------------

static void tempOrDash(char* buf, size_t n, bool valid, float v) {
  if (!valid || isnan(v)) snprintf(buf, n, "--.-");
  else                    buildTempStr(buf, n, v);
}

// Number-only temperature string for layouts that draw the unit separately as a
// small glyph (buildTempStr already includes it - using it there printed "F"/"C" twice).
static void tempNumOnly(char* buf, size_t n, bool valid, float v) {
  if (!valid || isnan(v)) snprintf(buf, n, "--.-");
  else                    snprintf(buf, n, "%.1f", tempForDisplay(v));
}

// Sky decorations (sun/moon + clouds/stars) for a theme; shared by the full
// renderer and the per-second partial update so they never drift apart.
static void drawSkyDecor(int idx, const T& t) {
  if (idx == 1) {   // SKY: sun top-right / moon+stars at night
    if (!nightMode()) {
      drawSun(t, 284, 48, C(0xff,0xe9,0xa8), C(0xff,0xcf,0x4d), C(0xf0,0xb4,0x29));
      drawCloud(t, 170, 34, 1.0f, C(0xff,0xff,0xff));
      drawCloud(t, 44, 52, 1.0f, C(0xff,0xff,0xff));   // second cloud: one cloud-width (52px) further left + lower, behind clock
    } else {
      static const int stars[][2] = {{24,34},{52,48},{76,28},{244,30},{268,44},{296,26},
                                     {308,52},{238,56},{232,74},{258,90},{288,70},{310,94}};
      for (auto& s : stars) drawStar(t, s[0], s[1], C(0xbc,0xd7,0xff));
    }
  } else if (idx == THEME_SPLIT) {   // SPLIT: flat sun/moon + panel clouds on the right
    if (!nightMode()) {
      drawFlatSun(t, 267, 23);                       // design skyicon: top:14, 18px
      drawSplitCloud(t, 238, 64, false, C(0xff,0xff,0xff));
      drawSplitCloud(t, 270, 86, true, blendWhite(bgCode(THEME_SPLIT, 290, 88), 75));   // pc2 @ .75 opacity
    } else {
      gT->fillCircle(t.X(267), t.Y(22), (int)(6 * t.sx + 0.5f), C(0xff,0xc8,0x57));
      gT->fillCircle(t.X(267) + (int)(3 * t.sx), t.Y(22) - (int)(2 * t.sx), (int)(5 * t.sx + 0.5f), bgCode(THEME_SPLIT, 267, 22));
      static const int stars[][2] = {{226,58},{300,52},{270,74},{292,88}};
      for (auto& s : stars) drawStar(t, s[0], s[1], C(0xbc,0xd7,0xff));
    }
  }
}

static void renderTheme0(const T& t) {   // BASIC (original layout)
  drawThemeBg(0, t);                     // == fillBackground()
  drawDogImage(t, 0);
  drawStatusBar(t);
  drawClockAndDate(t);
  drawWeatherPanel(t);
  drawTouchHint(t);
}

// ---- Nunito BFF fonts (generated by tools/make_nunito_bff.py) -------------------------
// Four fixed em sizes as compact 4bpp-alpha BFF blobs in flash. LGFX's runtime
// font slot holds one font at a time, so each master gets its own BFFfont and we
// draw with the explicit-font overloads (drawString/drawCenterString + IFont*).
static lgfx::BFFfont nunitoF9, nunitoF10, nunitoF12, nunitoF13, nunitoF20, nunitoF48;
static lgfx::PointerWrapper nunitoW9(nunito_em9_data, sizeof(nunito_em9_data));
static lgfx::PointerWrapper nunitoW10(nunito_em10_data, sizeof(nunito_em10_data));
static lgfx::PointerWrapper nunitoW12(nunito_em12_data, sizeof(nunito_em12_data));
static lgfx::PointerWrapper nunitoW13(nunito_em13_data, sizeof(nunito_em13_data));
static lgfx::PointerWrapper nunitoW20(nunito_em20_data, sizeof(nunito_em20_data));
static lgfx::PointerWrapper nunitoW48(nunito_em48_data, sizeof(nunito_em48_data));

bool nunitoOK = false;
void loadNunitoFonts() {
  nunitoOK = nunitoF9.loadFont(&nunitoW9) && nunitoF10.loadFont(&nunitoW10)
          && nunitoF12.loadFont(&nunitoW12) && nunitoF13.loadFont(&nunitoW13)
          && nunitoF20.loadFont(&nunitoW20) && nunitoF48.loadFont(&nunitoW48);
}

// Pixel width of a BFF string at scale 1 (sum of advances; default advance on miss).
static int bffWidth(const lgfx::IFont& f, const char* s) {
  lgfx::FontMetrics m;
  f.getDefaultMetric(&m);
  int w = 0;
  for (; *s; ++s) { (void)f.updateFontMetric(&m, (uint16_t)(unsigned char)*s); w += m.x_advance; }
  return w;
}

// Shrink-to-fit scale for a BFF string: min(1.0, avail / width).
static float bffFit(const lgfx::IFont& f, const char* s, int avail) {
  int w = bffWidth(f, s);
  return (w > avail && w > 0) ? (float)avail / (float)w : 1.0f;
}

// BFF text at full-space line-box top-left (x,y), scale sc, color c.
// `back` = RAW code of the background under the glyphs. This fork's alpha renderer
// blends glyph edge pixels toward getBaseColor(); on this inverted panel the default
// base 0x0000 displays as WHITE, which put a soft white halo around every non-white
// glyph (the "blurry" lines). Passing the real background makes edges anti-alias
// correctly. Default 0 keeps legacy behavior for callers that don't pass it.
static void bffText(const T& t, const lgfx::IFont& f, const char* s, int x, int y, uint16_t c, float sc = 1.0f, uint16_t back = 0) {
  gT->setBaseColor(back);
  gT->setTextSize(sc * t.sx);
  gT->setTextColor(c);
  gT->drawString(s, t.X(x), t.Y(y), &f);
}

// BFF text centered on full-space x=cx (line-box top at y).
static void bffCenter(const T& t, const lgfx::IFont& f, const char* s, int cx, int y, uint16_t c, float sc = 1.0f, uint16_t back = 0) {
  gT->setBaseColor(back);
  gT->setTextSize(sc * t.sx);
  gT->setTextColor(c);
  gT->drawCenterString(s, t.X(cx), t.Y(y), &f);
}

// Theme-1 dynamic band (status row + big clock + date line), full-space coords.
// Shared by the full render and updateDynamicTexts() so they never drift apart.
static void drawSkyDynamicBand(const T& t) {
  const SkyPal& p = SKY();
  if (!nunitoOK) {   // original bitmap-font band (legacy behavior)
    drawStatusRow(t, 10, 7, 1.2f, p.stL, p.dot, dayNightLabel(), p.stR, 310, true);
    char clockBuf[16], dateBuf[32];
    buildClockDateStrings(clockBuf, sizeof(clockBuf), dateBuf, sizeof(dateBuf));
    drawCenter(t, 160, 19, clockBuf, themeClockFit(1), p.clock, 316);
    drawCenter(t, 160, 62, dateBuf, 1.5f, p.date, 300);
    return;
  }
  // Status row: dot + "WIFI NTP" (em10) left; sun/moon + DAY/NIGHT right of X(310).
  char status[24]; buildWifiStatus(status, sizeof(status));
  int dr = max(1, (int)(2.5f * t.sx + 0.5f));
  gT->fillCircle(t.X(10) + dr, t.Y(7 + 4), dr, p.dot);
  bffText(t, nunitoF10, status, (int)(10 + 8 * 1.2f), 7, p.stL, 1.0f, bgCode(1, 0, 7));
  const char* dn = dayNightLabel();
  int rw = bffWidth(nunitoF10, dn);
  bffText(t, nunitoF10, dn, 310 - rw, 7, p.stR, 1.0f, bgCode(1, 0, 7));
  {
    int icx = 310 - rw - (int)(9 * 1.2f);
    drawDnIcon(t, icx, 7 + 4 * 1.2f, p.stR, bgCode(1, icx, 7 + 4 * 1.2f));
  }

  // Clock: centered at x=160 (em48). Date: centered at x=160 (em13).
  char clockBuf[16], dateBuf[32];
  buildClockDateStrings(clockBuf, sizeof(clockBuf), dateBuf, sizeof(dateBuf));
  bffCenter(t, nunitoF48, clockBuf, 160, 19, p.clock, bffFit(nunitoF48, clockBuf, 316), bgCode(1, 0, 19));
  bffCenter(t, nunitoF13, dateBuf, 160, 62, p.date, bffFit(nunitoF13, dateBuf, 300), bgCode(1, 0, 62));
}

static void renderTheme1Legacy(const T& t) {   // SKY - original bitmap-font layout
  const SkyPal& p = SKY();
  drawThemeBg(1, t);
  drawSkyDecor(1, t);

  drawStatusRow(t, 10, 7, 1.2f, p.stL, p.dot, dayNightLabel(), p.stR, 310, true);

  char clockBuf[16], dateBuf[32];
  buildClockDateStrings(clockBuf, sizeof(clockBuf), dateBuf, sizeof(dateBuf));
  float cs = themeClockFit(1);
  gT->setTextSize(cs); int cw = gT->textWidth(clockBuf);
  gT->setTextSize(cs * t.sx); gT->setTextColor(p.clock);
  gT->drawString(clockBuf, t.X((320 - cw) / 2.0f), t.Y(19));
  drawCenter(t, 160, 62, dateBuf, 1.5f, p.date, 300);

  char tmp[16]; tempNumOnly(tmp, sizeof(tmp), wx.valid, wx.temp);
  gT->setTextSize(2.5f); int uw = gT->textWidth(tempUnitChar());
  float ts = fitSize(tmp, 6.0f, max(40, 204 - uw - 10));
  gT->setTextSize(ts); int tw = gT->textWidth(tmp);
  gT->setTextSize(ts * t.sx); gT->setTextColor(p.temp);
  gT->drawString(tmp, t.X(14), t.Y(76));
  gT->setTextSize(2.5f * t.sx); gT->setTextColor(p.unit);
  gT->drawString(tempUnitChar(), t.X(14 + tw + 6), t.Y(76 + 8 * ts - 8 * 2.5f));

  const char* desc = wx.valid ? wx.description.c_str() : "No weather data";
  drawCloud(t, 16, 132, 0.38f, p.desc);   // was white - invisible on the day sky
  float dsc = fitSize(desc, 1.75f, 218 - 40);
  gT->setTextSize(dsc * t.sx); gT->setTextColor(p.desc);
  gT->drawString(desc, t.X(40), t.Y(129));

  char tiny[64], upd[24];
  buildUpdShort(upd, sizeof(upd));
  if (wx.valid)
    // Single spaces: with triple spaces the Fahrenheit string (two-digit temps)
    // overflowed the 204px zone and fitSize shrank it to an unreadable size.
    snprintf(tiny, sizeof(tiny), "HUM %d%% H %.0f L %.0f%s%s",
             wx.humidity, tempForDisplay(wx.tMax), tempForDisplay(wx.tMin), upd[0] ? " " : "", upd);
  else
    snprintf(tiny, sizeof(tiny), "NO WEATHER DATA");
  drawCenter(t, 116, 148, tiny, 1.0f, p.tiny, 204);

  char feels[16]; tempOrDash(feels, sizeof(feels), wx.valid, wx.feelsLike);
  drawFeelsChip(t, 14, 162, 204, 30, p.fBg, p.fBd, 1.25f, p.fLbl, feels, 2.25f, p.fVal);

  char rain[48]; buildRainLine(rain, sizeof(rain));
  // Same predicate as buildRainLine(): umbrella shows iff the text reports rain.
  bool rainy = wx.valid && (wx.rainingNow || (wx.rainNearby && wx.nearbyStns[0]) || wx.nextRain[0]);
  uint16_t iconC = rainy ? (nightMode() ? C(0x7c,0xc0,0xff) : C(0x2f,0x7f,0xd0))   // umbrella
                         : (wx.valid    ? p.rDot : C(0x9d,0xb4,0xc8));             // yellow circle / neutral dot
  drawRainChip(t, 14, 198, 204, 28, p.rBg, p.rBd, rain, 1.6f, p.rTxt, iconC, rainy);

  drawCenter(t, 115, 230, "TOUCH TO REFRESH", 1.2f, p.hint, 204);
  drawDogImage(t, 1);
}

static void renderTheme1(const T& t) {   // SKY - Nunito BFF fonts
  if (!nunitoOK) { renderTheme1Legacy(t); return; }
  const SkyPal& p = SKY();
  drawThemeBg(1, t);
  drawSkyDecor(1, t);
  drawSkyDynamicBand(t);   // status row + clock + date

  // Temperature + unit: left x=14 (em48 digits / em20 unit), baselines aligned.
  char tmp[16]; tempNumOnly(tmp, sizeof(tmp), wx.valid, wx.temp);
  float ts = bffFit(nunitoF48, tmp, max(40, 204 - bffWidth(nunitoF20, "C") - 10));
  int tw = (int)(bffWidth(nunitoF48, tmp) * ts);
  bffText(t, nunitoF48, tmp, 14, 76, p.temp, ts, bgCode(1, 0, 76));
  bffText(t, nunitoF20, tempUnitChar(), 14 + tw + 6,
          (int)(76 + (nunitoF48.ascent - nunitoF20.ascent) * ts), p.unit, 1.0f, bgCode(1, 0, 95));

  // Cloud icon + description text (em20).
  const char* desc = wx.valid ? wx.description.c_str() : "No weather data";
  drawCloud(t, 16, 132, 0.38f, p.desc);
  bffText(t, nunitoF20, desc, 40, 129, p.desc, bffFit(nunitoF20, desc, 218 - 40), bgCode(1, 0, 129));

  // Tiny row: centered x=116 (em10).
  char tiny[64], upd[24];
  buildUpdShort(upd, sizeof(upd));
  if (wx.valid)
    // Single spaces: with triple spaces the Fahrenheit string (two-digit temps)
    // overflowed the 204px zone and fitSize shrank it to an unreadable size.
    snprintf(tiny, sizeof(tiny), "HUM %d%% H %.0f L %.0f%s%s",
             wx.humidity, tempForDisplay(wx.tMax), tempForDisplay(wx.tMin), upd[0] ? " " : "", upd);
  else
    snprintf(tiny, sizeof(tiny), "NO WEATHER DATA");
  bffCenter(t, nunitoF10, tiny, 116, 148, p.tiny, bffFit(nunitoF10, tiny, 204), bgCode(1, 0, 148));

  // Feels-like chip (em10 label / em20 value).
  char feels[16]; tempOrDash(feels, sizeof(feels), wx.valid, wx.feelsLike);
  gT->fillRoundRect(t.X(14), t.Y(162), max(2, t.W(204)), max(2, t.H(30)), (int)(9 * t.sx + 0.5f), p.fBg);
  gT->drawRoundRect(t.X(14), t.Y(162), max(1, t.W(204) - 1), max(1, t.H(30) - 1), (int)(8 * t.sx + 0.5f), p.fBd);
  bffText(t, nunitoF10, "FEELS LIKE", 14 + 12, 172, p.fLbl, 1.0f, p.fBg);
  float fvs = bffFit(nunitoF20, feels, max(8, 204 - 24 - bffWidth(nunitoF10, "FEELS LIKE")));
  int vw = (int)(bffWidth(nunitoF20, feels) * fvs);
  bffText(t, nunitoF20, feels, 14 + 204 - 12 - vw, 168, p.fVal, fvs, p.fBg);

  // Rain chip (em13).
  char rain[48]; buildRainLine(rain, sizeof(rain));
  // Same predicate as buildRainLine(): umbrella shows iff the text reports rain.
  bool rainy = wx.valid && (wx.rainingNow || (wx.rainNearby && wx.nearbyStns[0]) || wx.nextRain[0]);
  uint16_t iconC = rainy ? (nightMode() ? C(0x7c,0xc0,0xff) : C(0x2f,0x7f,0xd0))   // umbrella
                         : (wx.valid    ? p.rDot : C(0x9d,0xb4,0xc8));             // yellow circle / neutral dot
  gT->fillRoundRect(t.X(14), t.Y(198), max(2, t.W(204)), max(2, t.H(28)), (int)(8 * t.sx + 0.5f), p.rBg);
  gT->drawRoundRect(t.X(14), t.Y(198), max(1, t.W(204) - 1), max(1, t.H(28) - 1), (int)(7 * t.sx + 0.5f), p.rBd);
  float rsc = bffFit(nunitoF13, rain, 204 - 24);
  int rw2 = (int)(bffWidth(nunitoF13, rain) * rsc);
  int cxFull = 14 + 204 / 2;
  if (rainy)
    drawUmbrella(t, (int)(cxFull - rw2 / 2.0f - 9), 198 + 14, 1.0f, iconC, p.rBg);
  else {
    int dr2 = max(1, (int)(3 * 1.2f * t.sx + 0.5f));
    gT->fillCircle(t.X((int)(cxFull - rw2 / 2.0f - 9)), t.Y(198 + 14), dr2, iconC);
  }
  bffText(t, nunitoF13, rain, (int)(cxFull - rw2 / 2.0f), 206, p.rTxt, rsc, p.rBg);

  // Hint: centered (em10).
  bffCenter(t, nunitoF10, "TOUCH TO REFRESH", 115, 230, p.hint, 1.0f, bgCode(1, 0, 230));
  drawDogImage(t, 1);
}

// Theme-2 dynamic band (clock + date), full-space coords. Shared by the full
// render and updateDynamicTexts() so they never drift apart; the per-second path
// clears only this band -> no full-screen flash like theme 1 (SKY).
static float splitClockScale(const char* s) {   // design: font-size 37px -> em48 @ 37/48
  float sc = bffFit(nunitoF48, s, 198);
  return (sc < 37.0f / 48.0f) ? sc : 37.0f / 48.0f;
}
// Design date is 10.5px: rendered with the em10 master at scale 1.0 (10px).
// Deliberately NOT em13@0.81 - any sub-1.0 BFF scale runs LGFX's nearest-
// neighbor scaler, which drops 1px source columns and garbles thin strokes.
static float splitDateScale(const char* s) {
  return bffFit(nunitoF10, s, 201);
}
static void drawSplitClockDate(const T& t) {
  const SplitPal& p = SPLIT();
  char clockBuf[16], dateBuf[32];
  buildClockDateStrings(clockBuf, sizeof(clockBuf), dateBuf, sizeof(dateBuf));
  if (!nunitoOK) {   // original bitmap-font band (legacy behavior)
    float cs = themeClockFit(THEME_SPLIT);
    gT->setTextSize(cs * t.sx); gT->setTextColor(p.clock);
    gT->drawString(clockBuf, t.X(12), t.Y(24));
    float ds = fitSize(dateBuf, 1.3f, 201);
    gT->setTextSize(ds * t.sx); gT->setTextColor(p.date);
    gT->drawString(dateBuf, t.X(13), t.Y(64));
    return;
  }
  bffText(t, nunitoF48, clockBuf, 12, 24, p.clock, splitClockScale(clockBuf), p.left);
  bffText(t, nunitoF10, dateBuf, 13, 64, p.date, splitDateScale(dateBuf), p.left);
}

static void renderTheme2(const T& t) {   // SPLIT
  const SplitPal& p = SPLIT();
  drawThemeBg(THEME_SPLIT, t);
  drawSkyDecor(THEME_SPLIT, t);

  // DAY/NIGHT label + ground ellipse on the panel
  if (!nunitoOK) drawCenter(t, 267, 38, dayNightLabel(), 1.2f, p.skyLbl, 100);
  else           bffCenter(t, nunitoF10, dayNightLabel(), 267, 38, p.skyLbl, 1.0f, bgCode(THEME_SPLIT, 267, 43));
  gT->fillEllipse(t.X(267), t.Y(231), max(1, (int)(43 * t.sx + 0.5f)),
                  max(1, (int)(5 * t.sy + 0.5f)), p.ground);

  if (!nunitoOK) {   // original bitmap-font layout (legacy behavior)
    drawStatusRow(t, 12, 9, 1.2f, p.stL, p.dot, nullptr, 0, 0);
    drawSplitClockDate(t);
    char tmp[16]; tempNumOnly(tmp, sizeof(tmp), wx.valid, wx.temp);
    gT->setTextSize(2.4f); int uw = gT->textWidth(tempUnitChar());
    float ts = fitSize(tmp, 5.4f, max(40, 202 - uw - 10));
    gT->setTextSize(ts); int tw = gT->textWidth(tmp);
    gT->setTextSize(ts * t.sx); gT->setTextColor(p.temp);
    gT->drawString(tmp, t.X(12), t.Y(80));
    gT->setTextSize(2.4f * t.sx); gT->setTextColor(p.unit);
    gT->drawString(tempUnitChar(), t.X(12 + tw + 6), t.Y(80 + 8 * ts - 8 * 2.4f));

    const char* desc = wx.valid ? wx.description.c_str() : "No weather data";
    drawCloud(t, 13, 131, 0.5f, nightMode() ? C(0xff,0xff,0xff) : C(0x9b,0xc4,0xe8));
    float dsc = fitSize(desc, 1.5f, 214 - 36);
    gT->setTextSize(dsc * t.sx); gT->setTextColor(p.descTxt);
    gT->drawString(desc, t.X(36), t.Y(128));

    char feels[16]; tempOrDash(feels, sizeof(feels), wx.valid, wx.feelsLike);
    // Chips span 12..225 (design .tile is content-box: 190 + 20 padding + border),
    // so they extend ~10px past the 214 panel line.
    drawFeelsChip(t, 12, 146, 213, 32, p.fBg, p.fBd, 1.1f, p.fLbl, feels, 2.2f, p.fVal);

    char rain[48]; buildRainLine(rain, sizeof(rain));
    drawRainChip(t, 12, 184, 213, 27, p.rBg, p.rBd, rain, 1.4f, p.rTxt, p.rDot);

    char left[48], right[24];   // GLCD font has no \xB7 glyph: use spaces
    if (wx.valid) snprintf(left, sizeof(left), "HUM %d%%  H %.1f L %.1f",
                           wx.humidity, tempForDisplay(wx.tMax), tempForDisplay(wx.tMin));
    else          snprintf(left, sizeof(left), "NO WEATHER DATA");
    buildUpdShort(right, sizeof(right));
    drawSplitRow(t, 12, 217, 202, left, right, 1.0f, p.row1C);

    drawCenter(t, 107, 229, "TOUCH TO REFRESH", 1.0f, p.hint, 196);
    drawDogImage(t, THEME_SPLIT);
    return;
  }

  // Status row: green dot (5px) + "WIFI NTP" (em10 @10px; design 9.5px).
  char status[24]; buildWifiStatus(status, sizeof(status));
  int dr = max(1, (int)(2 * t.sx + 0.5f));
  gT->fillCircle(t.X(12) + dr, t.Y(9 + 5), dr, p.dot);
  bffText(t, nunitoF10, status, 22, 9, p.stL, 1.0f, p.left);

  drawSplitClockDate(t);

  // Temperature (em48 @43px) + unit "\xB0F" (em20 @19px), baselines aligned.
  char tmp[16]; tempNumOnly(tmp, sizeof(tmp), wx.valid, wx.temp);
  float ts = bffFit(nunitoF48, tmp, 170);
  if (ts > 43.0f / 48.0f) ts = 43.0f / 48.0f;
  bffText(t, nunitoF48, tmp, 12, 80, p.temp, ts, p.left);
  int tw = (int)(bffWidth(nunitoF48, tmp) * ts + 0.5f);
  bffText(t, nunitoF20, tempUnitChar(), 12 + tw + 6,
          (int)(80 + nunitoF48.ascent * ts - nunitoF20.ascent * 0.95f), p.unit, 0.95f, p.left);

  // Description row: cloud icon (14px) + text (em12 @12px = design size), design left:13 top:128.
  const char* desc = wx.valid ? wx.description.c_str() : "No weather data";
  drawMiniCloud(t, 13, 129, p.descTxt);
  float dsc = bffFit(nunitoF12, desc, 214 - 32);
  bffText(t, nunitoF12, desc, 32, 128, p.descTxt, dsc, p.left);

  // FEELS chip: design .tile{left:12;width:190px;padding:0 10px;border:1.5px} is
  // content-box -> outer box 12..225 (213 wide), so the 214 panel line runs ~10px
  // BEHIND the chip's right edge (chip is painted after the bg, covering the line).
  // Icon @x=24 (12+1.5+10=23.5), label @x=46 (icon box 14 + gap 8), value
  // em20@18px right-aligned at x=213 (225-1.5border-10padding=213.5).
  char feels[16]; tempOrDash(feels, sizeof(feels), wx.valid, wx.feelsLike);
  gT->fillRoundRect(t.X(12), t.Y(146), max(2, t.W(213)), max(2, t.H(32)), (int)(9 * t.sx + 0.5f), p.fBg);
  gT->drawRoundRect(t.X(12), t.Y(146), max(1, t.W(213) - 1), max(1, t.H(32) - 1), (int)(8 * t.sx + 0.5f), p.fBd);
  int ix = 24, iy = 146 + (32 - 14) / 2;   // icon box 14px, vertically centred
  gT->drawLine(t.X(ix + 5), t.Y(iy + 1), t.X(ix + 5), t.Y(iy + 9), p.fIcon);
  gT->drawLine(t.X(ix + 2), t.Y(iy + 2), t.X(ix + 8), t.Y(iy + 7), p.fIcon);
  gT->drawLine(t.X(ix + 8), t.Y(iy + 2), t.X(ix + 2), t.Y(iy + 7), p.fIcon);
  bffText(t, nunitoF9, "FEELS LIKE", 46, 146 + (32 - 9) / 2, p.fLbl, 1.0f, p.fBg);
  // feels already ends with the unit char ("32.7 F") — do NOT append tempUnitChar() again.
  float fvs = bffFit(nunitoF20, feels, 213 - 84);
  if (fvs > 0.9f) fvs = 0.9f;
  int vw = (int)(bffWidth(nunitoF20, feels) * fvs + 0.5f);
  bffText(t, nunitoF20, feels, 213 - vw, 146 + (32 - (int)(20 * fvs)) / 2, p.fVal, fvs, p.fBg);

  // RAIN chip [12,184] 213 wide: same outer box as the feels chip (content-box).
  // Umbrella icon + text (em13 @11.5px) centered as a group.
  char rain[48]; buildRainLine(rain, sizeof(rain));
  gT->fillRoundRect(t.X(12), t.Y(184), max(2, t.W(213)), max(2, t.H(27)), (int)(8 * t.sx + 0.5f), p.rBg);
  gT->drawRoundRect(t.X(12), t.Y(184), max(1, t.W(213) - 1), max(1, t.H(27) - 1), (int)(7 * t.sx + 0.5f), p.rBd);
  // em12 @1.0 (design 11.5px): sub-1.0 BFF scaling decimates 1px strokes.
  float rsc = bffFit(nunitoF12, rain, 213 - 40);
  int rw = (int)(bffWidth(nunitoF12, rain) * rsc + 0.5f);
  int gx = 12 + (213 - (13 + 6 + rw)) / 2;     // group: umbrella(13px) + gap(6px) + text
  drawUmbrella(t, gx + 6, 184 + 13, 1.0f, p.rDot, p.rBg);
  bffText(t, nunitoF12, rain, gx + 19, 184 + (27 - (int)(12 * rsc)) / 2, p.rTxt, rsc, p.rBg);

  // row1 (design top:217 width:190): left text @x=12; UPD right-aligned at x=202.
  // em9 @1.0 = design's 9px (em10@0.85 decimated and garbled thin strokes).
  char left[48], right[24];
  if (wx.valid) snprintf(left, sizeof(left), "HUM %d%% \xB7 H %.1f\xB0 L %.1f\xB0",
                         wx.humidity, tempForDisplay(wx.tMax), tempForDisplay(wx.tMin));
  else          snprintf(left, sizeof(left), "NO WEATHER DATA");
  buildUpdShort(right, sizeof(right));
  float lsc = bffFit(nunitoF9, left, 202 - 12);
  bffText(t, nunitoF9, left, 12, 217, p.row1C, lsc, p.left);
  int rwid = (int)(bffWidth(nunitoF9, right) + 0.5f);
  bffText(t, nunitoF9, right, 202 - rwid, 217, p.row1C, 1.0f, p.left);

  bffCenter(t, nunitoF9, "TOUCH TO REFRESH", 107, 229, p.hint, 1.0f, p.left);
  drawDogImage(t, THEME_SPLIT);
}

// ---------------------------------------------------------------------------
//  Theme 3 - BENTO.
//
//  The clock card is its own function: updateDynamicTexts() repaints just that
//  card once per minute instead of re-rendering the whole screen every second
//  (the full rewrite is what the panel showed as a visible flash).
// ---------------------------------------------------------------------------

// Clock card (8,8,304,52): status row + big clock + date. Nunito BFF front
// (the SKY theme's font) when available, original bitmap fonts otherwise.
static void drawBentoClockCard(const T& t) {
  const BentoPal& p = BENTO();
  drawCard(t, 8, 8, 304, 52, p.cardBg, p.cardBd);

  char clockBuf[16], dateBuf[32];
  buildClockDateStrings(clockBuf, sizeof(clockBuf), dateBuf, sizeof(dateBuf));

  if (nunitoOK) {
    // Status row (design y=25): dot 5px + "WIFI NTP" (em9); sun/moon +
    // DAY/NIGHT right-aligned to x=304
    char wifi[24]; buildWifiStatus(wifi, sizeof(wifi));
    gT->fillCircle(t.X(18), t.Y(27), max(1, (int)(2.5f * t.sx + 0.5f)), p.dot);
    bffText(t, nunitoF9, wifi, 24, 25, p.stL, 1.0f, p.cardBg);
    const char* dn = dayNightLabel();
    int dw = bffWidth(nunitoF9, dn);
    bffText(t, nunitoF9, dn, 304 - dw, 25, p.stR, 1.0f, p.cardBg);
    drawDnIcon(t, 304 - dw - 9, 29, p.stR, p.cardBg);

    // Clock (em48 at the design's 27px) and date (em10, design ~9.5px)
    float cs = bffFit(nunitoF48, clockBuf, 290);
    if (cs > 27.0f / 48.0f) cs = 27.0f / 48.0f;
    bffCenter(t, nunitoF48, clockBuf, 160, 11, p.clock, cs, p.cardBg);
    bffCenter(t, nunitoF10, dateBuf, 160, 41, p.date, bffFit(nunitoF10, dateBuf, 288), p.cardBg);
  } else {   // original bitmap-font card
    float cs = themeClockFit(THEME_BENTO);
    gT->setTextSize(cs); int cw = gT->textWidth(clockBuf);
    gT->setTextSize(cs * t.sx); gT->setTextColor(p.clock);
    gT->drawString(clockBuf, t.X((160 - cw) / 2.0f), t.Y(11));   // centered in [8,312]
    drawCenter(t, 160, 41, dateBuf, 1.0f, p.date, 288);

    char wifi[24]; buildWifiStatus(wifi, sizeof(wifi));
    gT->setTextSize(1 * t.sx);
    int dr = max(1, (int)(2.5f * t.sx + 0.5f));
    gT->fillCircle(t.X(16) + dr, t.Y(29), dr, p.dot);
    gT->setTextColor(p.stL);
    gT->drawString(wifi, t.X(24), t.Y(25));
    drawRight(t, 304, 25, dayNightLabel(), 1.0f, p.stR);
  }

  // Keep the partial-update cache in sync so a per-second tick right after a
  // full render does not cause a redundant repaint.
  strncpy(lastClockBuf, clockBuf, sizeof(lastClockBuf) - 1);
  lastClockBuf[sizeof(lastClockBuf) - 1] = '\0';
  strncpy(lastDateBuf, dateBuf, sizeof(lastDateBuf) - 1);
  lastDateBuf[sizeof(lastDateBuf) - 1] = '\0';
}

// Dog card (206,66,106,166): caption + dog centered in the card above it.
static void drawBentoDogCard(const T& t) {
  const BentoPal& p = BENTO();
  drawCard(t, 206, 66, 106, 166, p.dogCardBg, p.cardBd);
  if (nunitoOK) {   // "OUTFIT · BAND" (design); plain band label if it does not fit
    char cap[32];
    snprintf(cap, sizeof(cap), "OUTFIT \xB7 %s", dogBandLabel());
    if (bffWidth(nunitoF9, cap) > 96) snprintf(cap, sizeof(cap), "%s", dogBandLabel());
    bffCenter(t, nunitoF9, cap, 259, 207, p.capC, 1.0f, p.dogCardBg);
  } else {
    drawCenter(t, 259, 207, dogBandLabel(), 1.0f, p.capC, 96);
  }
  int fw = dogWidth();
  if (fw > 0) {
    int fh = dogHeight();
    int xFull = 206 + (106 - fw) / 2;
    int yTopFull = 70 + (134 - fh) / 2;   // centered in [70,204], above the caption
    drawDogBox(t, xFull, yTopFull, fw, fh, THEME_BENTO);
  }
}

// BENTO with the original bitmap fonts (fallback when the Nunito BFF load fails).
static void renderTheme3Legacy(const T& t) {
  const BentoPal& p = BENTO();
  drawThemeBg(THEME_BENTO, t);

  drawBentoClockCard(t);                        // clock card
  drawCard(t, 8, 66, 192, 72, p.cardBg, p.cardBd);   // temp card
  drawCard(t, 8, 184, 192, 48, p.cardBg, p.cardBd);  // detail card
  drawBentoDogCard(t);                          // dog card
  gT->fillRoundRect(t.X(7), t.Y(74), max(1, t.W(4)), max(1, t.H(56)),
                    (int)(2 * t.sx + 0.5f), p.accent);   // bar on temp card (design .accent)

  char tmp[16]; tempNumOnly(tmp, sizeof(tmp), wx.valid, wx.temp);
  gT->setTextSize(2.0f); int uw = gT->textWidth(tempUnitChar());
  float ts = fitSize(tmp, 4.25f, max(30, 108 - uw - 8));
  gT->setTextSize(ts); int tw = gT->textWidth(tmp);
  gT->setTextSize(ts * t.sx); gT->setTextColor(p.temp);
  gT->drawString(tmp, t.X(24), t.Y(74));
  gT->setTextSize(2.0f * t.sx); gT->setTextColor(p.unit);
  gT->drawString(tempUnitChar(), t.X(24 + tw + 5), t.Y(74 + 8 * ts - 16));

  // FEELS chip [132,74,60,40]
  char feels[16]; tempOrDash(feels, sizeof(feels), wx.valid, wx.feelsLike);
  gT->fillRoundRect(t.X(132), t.Y(74), max(2, t.W(60)), max(2, t.H(40)), (int)(8 * t.sx + 0.5f), p.chipBg);
  gT->drawRoundRect(t.X(132), t.Y(74), max(1, t.W(60) - 1), max(1, t.H(40) - 1), (int)(7 * t.sx + 0.5f), p.chipBd);
  drawCenter(t, 162, 82, "FEELS", 1.0f, p.chipLbl, 52);
  float fv = fitSize(feels, 2.0f, 52);
  gT->setTextSize(fv); int fw2 = gT->textWidth(feels);
  gT->setTextSize(fv * t.sx); gT->setTextColor(p.chipVal);
  gT->drawString(feels, t.X(162 - fw2 / 2.0f), t.Y(96));

  // Description row inside the temp card: mini cloud + text
  const char* desc = wx.valid ? wx.description.c_str() : "No weather data";
  drawCloud(t, 25, 114, 0.38f, nightMode() ? C(0xff,0xff,0xff) : C(0x9b,0xc4,0xe8));
  float dsc = fitSize(desc, 1.5f, 196 - 43);
  gT->setTextSize(dsc * t.sx); gT->setTextColor(p.descTxt);
  gT->drawString(desc, t.X(43), t.Y(112));

  // Rain card [8,144,192,34]
  char rain[48]; buildRainLine(rain, sizeof(rain));
  drawRainChip(t, 8, 144, 192, 34, p.rBg, p.rBd, rain, 1.6f, p.rTxt, p.rDot);

  // Detail card rows
  char d1[32], d2[32];
  snprintf(d1, sizeof(d1), wx.valid ? "HUMIDITY %d%%" : "HUMIDITY --", wx.humidity);
  if (wx.valid) snprintf(d2, sizeof(d2), "TODAY H %.0f L %.0f",
                         tempForDisplay(wx.tMax), tempForDisplay(wx.tMin));
  else          snprintf(d2, sizeof(d2), "NO WEATHER DATA");
  gT->setTextSize(1 * t.sx); gT->setTextColor(p.detC);
  gT->drawString(d1, t.X(22), t.Y(194));
  gT->drawString(d2, t.X(22), t.Y(207));
  char upd[32]; buildUpdShort(upd, sizeof(upd));
  String updl = String(upd); if (updl.length()) updl += " - REFRESH";
  float us = fitSize(updl.c_str(), 0.85f, 174);
  gT->setTextSize(us * t.sx); gT->setTextColor(p.updC);
  gT->drawString(updl.c_str(), t.X(22), t.Y(221));
}

// BENTO with the Nunito BFF front (the SKY theme's font): design typography -
// 27px clock, ~9.5px date, 34px temp value, em9/em10/em12/em13/em20 for the rest.
static void renderTheme3Nunito(const T& t) {
  const BentoPal& p = BENTO();
  drawThemeBg(THEME_BENTO, t);

  drawBentoClockCard(t);                        // clock card

  // ---- Temp card (design 192x72): accent bar + 34px number + FEELS chip ----
  drawCard(t, 8, 66, 192, 72, p.cardBg, p.cardBd);
  gT->fillRoundRect(t.X(7), t.Y(74), max(1, t.W(4)), max(1, t.H(56)),
                    (int)(2 * t.sx + 0.5f), p.accent);   // design .accent
  char tmp[16]; tempNumOnly(tmp, sizeof(tmp), wx.valid, wx.temp);
  float ts = bffFit(nunitoF48, tmp, 140);
  if (ts > 34.0f / 48.0f) ts = 34.0f / 48.0f;              // design .tempv = 34px
  bffText(t, nunitoF48, tmp, 24, 74, p.temp, ts, p.cardBg);
  float us = 15.0f / 20.0f;                                // design .tempu = 15px
  int t48w = (int)(bffWidth(nunitoF48, tmp) * ts + 0.5f);
  bffText(t, nunitoF20, tempUnitChar(), 24 + t48w + 6,
          (int)(74 + nunitoF48.ascent * ts - nunitoF20.ascent * us),
          p.unit, us, p.cardBg);

  // FEELS chip (design 60x40 at (132,74), r10): crossed-arrows icon (9px) +
  // "FEELS" (em9) over the value (em20 @ 14px).
  gT->fillRoundRect(t.X(132), t.Y(74), max(2, t.W(60)), max(2, t.H(40)),
                    (int)(10 * t.sx + 0.5f), p.chipBg);
  gT->drawRoundRect(t.X(132), t.Y(74), max(1, t.W(59)), max(1, t.H(39)),
                    (int)(9 * t.sx + 0.5f), p.chipBd);
  int ix = 162 - (9 + 3 + bffWidth(nunitoF9, "FEELS")) / 2;   // icon(9) + gap(3) + label
  int iy = 81;
  gT->drawLine(t.X(ix + 4), t.Y(iy + 1), t.X(ix + 4), t.Y(iy + 8), p.chipLbl);
  gT->drawLine(t.X(ix + 1), t.Y(iy + 2), t.X(ix + 8), t.Y(iy + 7), p.chipLbl);
  gT->drawLine(t.X(ix + 8), t.Y(iy + 2), t.X(ix + 1), t.Y(iy + 7), p.chipLbl);
  bffText(t, nunitoF9, "FEELS", ix + 9 + 3, 81, p.chipLbl, 1.0f, p.chipBg);
  char feels[16]; tempOrDash(feels, sizeof(feels), wx.valid, wx.feelsLike);
  float fvs = bffFit(nunitoF20, feels, 44);
  if (fvs > 14.0f / 20.0f) fvs = 14.0f / 20.0f;            // design .fval = 14px
  bffCenter(t, nunitoF20, feels, 162, 95, p.chipVal, fvs, p.chipBg);

  // Description row inside the temp card (design y=112): cloud + em12 text
  const char* desc = wx.valid ? wx.description.c_str() : "No weather data";
  drawMiniCloud(t, 25, 113, p.descTxt);
  bffText(t, nunitoF12, desc, 42, 112, p.descTxt, bffFit(nunitoF12, desc, 154), p.cardBg);

  // ---- Rain card (design 192x34, r10): dot/umbrella + em13 text, centered group ----
  gT->fillRoundRect(t.X(8), t.Y(144), t.W(192), t.H(34), (int)(10 * t.sx + 0.5f), p.rBg);
  gT->drawRoundRect(t.X(8), t.Y(144), max(1, t.W(191)), max(1, t.H(33)),
                    (int)(9 * t.sx + 0.5f), p.rBd);
  char rain[48]; buildRainLine(rain, sizeof(rain));
  float rsc = bffFit(nunitoF13, rain, 161);
  int rw = (int)(bffWidth(nunitoF13, rain) * rsc + 0.5f);
  bool wet = wx.rainingNow || (wx.rainNearby && wx.nearbyStns[0]) || wx.nextRain[0];
  int iw = wet ? 13 : 6;
  int gx = 8 + (192 - (iw + 7 + rw)) / 2;                  // design gap: 7px
  if (wet) drawUmbrella(t, gx + 6, 163, 1.0f, p.rDot, p.rBg);
  else gT->fillCircle(t.X(gx + 3), t.Y(161), max(1, (int)(3 * t.sx + 0.5f)), p.rDot);
  bffText(t, nunitoF13, rain, gx + iw + 7, 154, p.rTxt, rsc, p.rBg);

  // ---- Detail card rows (design: x=22, y=194/207/221) ----
  drawCard(t, 8, 184, 192, 48, p.cardBg, p.cardBd);
  char d1[32], d2[32];
  snprintf(d1, sizeof(d1), wx.valid ? "HUMIDITY %d%%" : "HUMIDITY --", wx.humidity);
  bffText(t, nunitoF10, d1, 22, 194, p.detC, 1.0f, p.cardBg);
  if (wx.valid)
    snprintf(d2, sizeof(d2), "TODAY H %.1f\xB0 L %.1f\xB0",   // design: "TODAY H 43.7° L 34.5°"
             tempForDisplay(wx.tMax), tempForDisplay(wx.tMin));
  else
    snprintf(d2, sizeof(d2), "NO WEATHER DATA");
  bffText(t, nunitoF10, d2, 22, 207, p.detC, 1.0f, p.cardBg);
  char upd[32], ref[64];
  buildUpdShort(upd, sizeof(upd));
  if (upd[0]) snprintf(ref, sizeof(ref), "%s \xB7 TOUCH TO REFRESH", upd);
  else        snprintf(ref, sizeof(ref), "TOUCH TO REFRESH");
  bffText(t, nunitoF9, ref, 22, 221, p.updC, 1.0f, p.cardBg);

  drawBentoDogCard(t);
}

static void renderTheme3(const T& t) {
  if (nunitoOK) renderTheme3Nunito(t);
  else          renderTheme3Legacy(t);
}

void renderTheme(int idx, const T& t) {
  switch (idx) {
    case 0: renderTheme0(t); break;
    case 1: renderTheme1(t); break;
    case 2: renderTheme2(t); break;   // SPLIT
    default: renderTheme3(t); break;  // BENTO
  }
}

// LovyanGFX v1 draws in immediate mode – every call goes straight to the
// panel over SPI, so there is no separate framebuffer flush step.
void renderScreen() {
  renderTheme(userSettings.theme, T::ID);
}

// ---------------------------------------------------------------------------
//  Flicker-free per-second updates
//
//  The panel has no framebuffer – every pixel write goes straight over SPI,
//  so redrawing the whole screen once per second is visible as flicker.
//  Instead, only what actually changed is repainted:
//    theme 0 (BASIC): band-clear + redraw of clock/date lines (original approach)
//    theme 1 (SKY):   clearThemed() over the status/clock/date bands + redraw
//                     those elements (sky decorations included – they sit in
//                     the cleared band and are cheap to repaint)
//    theme 2 (SPLIT): clearThemed() over the clock/date band + redraw it
//    theme 3 (BENTO): clearThemed() over the clock card + redraw it. Its text
//                     is minute-granular, so the per-second tick is a no-op
//                     (no SPI traffic at all -> no flash)
// ---------------------------------------------------------------------------
void updateDynamicTexts() {
  if (!ntpSynced) return;
  struct tm lt;
  nowLocal(&lt);

  char clockBuf[16], dateBuf[32];
  formatClock(clockBuf, sizeof(clockBuf), &lt);   // respects use24Hour
  formatDateLine(dateBuf, sizeof(dateBuf), &lt);  // respects dateFormat

  if (userSettings.theme == 0) {
    // ---- Big clock (gLayout.clockY/clockSize) – repaint only on minute change ----
    if (strcmp(clockBuf, lastClockBuf) != 0) {
      strncpy(lastClockBuf, clockBuf, sizeof(lastClockBuf) - 1);
      lastClockBuf[sizeof(lastClockBuf) - 1] = '\0';
      tft.setTextSize(gLayout.clockSize);
      int maxW = tft.textWidth(clockMaxString());   // widest in the current mode (12h/24h)
      int boxX = (320 - maxW) / 2;
      clearRegion(boxX, gLayout.clockY - 2, maxW + 4, glyphH(gLayout.clockSize) + 2);   // clock band, matches background
      tft.setTextSize(gLayout.clockSize * T::ID.sx);
      int cw = tft.textWidth(clockBuf);
      tft.setTextColor(TFT_WHITE);
      tft.drawString(clockBuf, boxX + (maxW - cw) / 2, gLayout.clockY);
    }

    // ---- Date line – repaint only on change ----
    if (strcmp(dateBuf, lastDateBuf) != 0) {
      strncpy(lastDateBuf, dateBuf, sizeof(lastDateBuf) - 1);
      lastDateBuf[sizeof(lastDateBuf) - 1] = '\0';
      int bandTop = gLayout.clockY + glyphH(gLayout.clockSize);   // just under the clock
      clearRegion(0, bandTop, 320, (gLayout.dateY + glyphH(gLayout.dateSize)) - bandTop + 2);
      float ds = dateLineFit(dateBuf);               // chosen full-space size (normally exactly gLayout.dateSize)
      tft.setTextSize(ds * T::ID.sx);
      int dw = tft.textWidth(dateBuf);
      tft.setTextColor(TFT_YELLOW);
      tft.drawString(dateBuf, (320 - dw) / 2, gLayout.dateY);
    }
    return;
  }

  if (userSettings.theme == THEME_SKY) {
    bool clockChanged = strcmp(clockBuf, lastClockBuf) != 0;
    bool dateChanged  = strcmp(dateBuf, lastDateBuf) != 0;
    if (!clockChanged && !dateChanged) return;

    T t = T::ID;
    // Clear the dynamic band (status row y~7 + clock + date line ending ~y73),
    // then redraw it. Sky decorations live inside this band and are repainted
    // right after, so nothing is lost.
    clearThemed(userSettings.theme, t, 4, 4, 312, 72);
    if (clockChanged) {
      strncpy(lastClockBuf, clockBuf, sizeof(lastClockBuf) - 1);
      lastClockBuf[sizeof(lastClockBuf) - 1] = '\0';
    }
    if (dateChanged) {
      strncpy(lastDateBuf, dateBuf, sizeof(lastDateBuf) - 1);
      lastDateBuf[sizeof(lastDateBuf) - 1] = '\0';
    }

    drawSkyDecor(userSettings.theme, t);   // sun/moon/clouds/stars live in this band
    drawSkyDynamicBand(t);   // Nunito band (bitmap fallback inside)
    return;
  }

  if (userSettings.theme == THEME_SPLIT) {   // SPLIT: repaint only the clock/date band.
    bool clockChanged = strcmp(clockBuf, lastClockBuf) != 0;
    bool dateChanged  = strcmp(dateBuf, lastDateBuf) != 0;
    if (!clockChanged && !dateChanged) return;   // no blinking colon: nothing moves per second
    const T& t = T::ID;     // screen is exactly 320x240 -> identity transform
    clearThemed(THEME_SPLIT, t, 10, 22, 204, 56);   // left-zone rows [22..78): solid bg
    drawSplitClockDate(t);
    if (clockChanged) {
      strncpy(lastClockBuf, clockBuf, sizeof(lastClockBuf) - 1);
      lastClockBuf[sizeof(lastClockBuf) - 1] = '\0';
    }
    if (dateChanged) {
      strncpy(lastDateBuf, dateBuf, sizeof(lastDateBuf) - 1);
      lastDateBuf[sizeof(lastDateBuf) - 1] = '\0';
    }
    return;
  }

  if (userSettings.theme == THEME_BENTO) {   // BENTO: repaint only the clock card. Its text is
                            // minute-granular, so a per-second tick is a no-op
                            // (zero SPI traffic -> no flash); once a minute
                            // only the 304x52 card band is cleared + redrawn.
    bool clockChanged = strcmp(clockBuf, lastClockBuf) != 0;
    bool dateChanged  = strcmp(dateBuf, lastDateBuf) != 0;
    if (!clockChanged && !dateChanged) return;
    T t = T::ID;
    clearThemed(THEME_BENTO, t, 8, 8, 304, 52);   // clock card rows [8..60)
    drawBentoClockCard(t);               // redraws the card + re-syncs the cache
    return;
  }

  // Safety net: unknown theme -> full re-render.
  renderScreen();
}
