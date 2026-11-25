// ---------------------------------------------------------------------------
//  Full-screen "WEATHER ALERTS" detail screen — see alerts_screen.h.
//
//  One alert per view; a long alert spans several pages (BODY_LINES body
//  lines each), one full redraw per page (the screen is small and taps are
//  the only motion). Drawing follows the settings screens: blue header bar
//  with "< BACK" + "<"/">" pills at the same fixed slots, dim footer hint,
//  dark page background. Text: Nunito BFF (em13 body / em12 event / em9
//  meta lines), GLCD
//  fallback when the BFF fonts failed to load.
// ---------------------------------------------------------------------------

#include "alerts_screen.h"

#include <Arduino.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "display.h"      // tft, C()
#include "render.h"       // renderScreen()
#include "nws_alerts.h"
#include "time_util.h"    // localOffsetSec()

// Header / pill geometry — same fixed slots as the settings screens.
constexpr int HDR_H      = 26;    // blue header bar
constexpr int BTN_BACK_X = 2,  BTN_BACK_W = 58, BTN_H = 22;   // "< BACK"
constexpr int BTN_NEXT_X = 260, BTN_NEXT_W = 58;              // ">"
constexpr int BTN_PREV_X = BTN_NEXT_X - BTN_NEXT_W - 4;       // "<"

// Same palette as the settings screens (settings.cpp) so the modal family
// looks consistent.
const uint16_t colBg     = C(0x24, 0x31, 0x42);
const uint16_t colHeader = C(0x2f, 0x7d, 0xc9);
const uint16_t colBtnBg  = C(0xff, 0xd1, 0x66);   // "< BACK" accent (picker yellow)
const uint16_t colBtnBd  = C(0x9a, 0x74, 0x14);
const uint16_t colBtnTxt = C(0x14, 0x14, 0x14);
const uint16_t colNextBg = C(0xFF, 0xFF, 0xFF);
const uint16_t colNextBd = C(0xc9, 0xd2, 0xdc);
const uint16_t colBgTxt  = C(0xf0, 0xf4, 0xf8);   // light text on dark page bg
const uint16_t colHint   = C(0xa9, 0xb9, 0xcc);   // dim text on dark bg

// Body layout (see drawAlertPage).
constexpr int COUNTER_Y = 33;
constexpr int EVENT_Y   = 46;
constexpr int SEV_Y     = 70;
constexpr int AREA_Y    = 84;
constexpr int BODY_Y    = 98;    // description lines start
constexpr int BODY_PITCH = 15;   // Nunito em13 line box is 13 px + 2 gap
constexpr int BODY_LINES = 8;    // 98 .. 216 (footer hint at 226)
constexpr int FOOTER_Y  = 226;

// Alert text is drawn in the Nunito BFF face (scale 1.0) - a real font,
// much more readable than the built-in GLCD bitmap font: em13 for the
// body, em12 for the event line, em9 for the small counter / severity /
// area lines. Body lines are wrapped on PIXEL width (BFF_MAX_W) because
// Nunito has variable advances; the char cap bounds the line buffer for
// pathological narrow-glyph runs. If the BFF fonts fail to load (nunitoOK
// false, see render.cpp) the screen falls back to the built-in font at size
// 1.5 (9 px/char), 34 chars per line.
constexpr float BODY_FONT = 1.5f;   // GLCD fallback size
constexpr int WRAP_CHARS = 34;      // GLCD fallback: chars per line
constexpr int BFF_MAX_W = 312;      // Nunito: body width in px
constexpr int BFF_MAX_CHARS = 64;   // Nunito: char cap per line
constexpr int LINE_BUF = BFF_MAX_CHARS + 1;

// Flat content stream for the alert on screen: description lines, then
// (when present) a blank line, the dim "WHAT TO DO" label, and the
// instruction lines. drawAlertPage() renders one page (BODY_LINES of it);
// buildStream() fills it and returns the page count.
static const int STREAM_MAX = 56;
static char gStream[STREAM_MAX][LINE_BUF];
static bool gStreamLabel[STREAM_MAX];
static int  gStreamN = 0;

static bool gOpen = false;
static int  gAlert = 0;
static int  gPage = 0;

// Opening/closing-press drain (picker pattern, see picker.cpp): the tap that
// opens the screen is still on the glass when the page is drawn, and the
// press that closes it is still down when the dashboard is redrawn
// underneath. Armed on every entry/exit path; touch.cpp keeps feeding
// alertsScreenPollRelease() until the finger lifts and skips all input while
// the drain is armed.
static bool gAwaitRelease = false;

void alertsScreenPollRelease() {
  if (!gAwaitRelease) return;
  int16_t x, y;
  if (!tft.getTouch(&x, &y)) gAwaitRelease = false;   // finger lifted
}

bool alertsScreenAwaitingRelease() { return gAwaitRelease; }

// ---------------------------------------------------------------------------
//  Small helpers
// ---------------------------------------------------------------------------

static int txtW(const char* s, float size) {
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(size);
  return (int)tft.textWidth(s);
}

static void closeAlerts() {
  gOpen = false;
  gAwaitRelease = true;   // the closing press is still on the glass; swallow it
  renderScreen();
}

static uint16_t sevColor(const char* sev) {
  if      (!strcmp(sev, "EXTREME"))  return C(0xc8, 0x2a, 0x2a);
  else if (!strcmp(sev, "SEVERE"))   return C(0xd4, 0x55, 0x00);
  else if (!strcmp(sev, "MODERATE")) return C(0xff, 0xd1, 0x66);
  return colHint;                              // MINOR / unknown
}

// "UNTIL FRI 1:00 AM" from the cached UTC epoch (local wall clock); false
// when the alert has no expiration. The weekday is included because NWS
// watches often end days out and a bare time would read as tonight.
static bool fmtEnds(const NwsAlert* a, char* buf, size_t n) {
  if (a->ends <= 0) return false;
  const time_t utc = (time_t)a->ends;
  const time_t loc = utc + (time_t)localOffsetSec(utc);
  struct tm lt;
  gmtime_r(&loc, &lt);
  int h = lt.tm_hour % 12;
  if (h == 0) h = 12;
  static const char* const wd[7] = {
    "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"
  };
  snprintf(buf, n, "UNTIL %s %d:%02d %s",
           wd[lt.tm_wday], h, lt.tm_min, lt.tm_hour < 12 ? "AM" : "PM");
  return true;
}

// Event line (size 2): cut from the end to fit the 312 px body width.
static void fitEvent(const char* in, char* out, size_t n) {
  snprintf(out, n, "%s", in);
  while (txtW(out, 2) > 312 && strlen(out) > 1) {
    out[strlen(out) - 1] = 0;
  }
}

// Greedy word wrap into up to `maxLines` lines of WRAP_CHARS chars; overlong
// words are hard-split. '\n' (line breaks kept by htmlToText from <br>/<li>/<p>
// and NWS's own newlines) forces a line break, and a blank line separates a
// bullet block ("* LABEL..." lines) from the text above it. Returns the
// number of lines used and sets *truncated (last line ends with "...") when
// text was left over.
static int wrapText(const char* s, char lines[][LINE_BUF], int maxLines, bool* truncated) {
  int li = 0, o = 0;
  lines[0][0] = 0;
  if (truncated) *truncated = false;   // truncated may be null
  while (*s) {
    bool hadSpace = false;
    while (*s == ' ') { s++; hadSpace = true; }
    if (!*s) break;
    if (*s == '\n') {                          // hard break (kept by htmlToText)
      lines[li][o] = 0;
      li++; o = 0;
      if (li >= maxLines) { if (truncated) *truncated = true; break; }
      s++;
      continue;
    }
    // Blank line before a bullet block ("* LABEL..."): only at the start of
    // a line, never before the first line, and only when the previous line
    // is not already a bullet (so a run of bullets stays compact).
    if (o == 0 && *s == '*' && li > 0 && lines[li - 1][0] != 0 && lines[li - 1][0] != '*') {
      lines[li][0] = 0;                        // current (empty) line = the blank
      li++; o = 0;
      if (li >= maxLines) { if (truncated) *truncated = true; break; }
    }
    int w = 0;
    while (s[w] && s[w] != ' ' && s[w] != '\n') w++;
    if (w > WRAP_CHARS) w = WRAP_CHARS;
    // One separator space before the word, except at the start of a line
    // (and except for hard-split chunks of an overlong word, which stay
    // flush as before).
    int pad = (o > 0 && hadSpace) ? 1 : 0;
    if (o + pad + w > WRAP_CHARS) {            // word overflows the line
      lines[li][o] = 0;
      li++; o = 0; pad = 0;
      if (li >= maxLines) { if (truncated) *truncated = true; break; }
    }
    if (pad) lines[li][o++] = ' ';
    memcpy(lines[li] + o, s, w);
    o += w;
    s += w;
  }
  if (li >= maxLines) li = maxLines - 1;       // overflow: last full line
  if (!truncated || !*truncated) lines[li][o] = 0;
  if (truncated && *truncated) {
    int L = (int)strlen(lines[li]);
    if (L >= 3) {
      lines[li][L - 3] = '.';
      lines[li][L - 2] = '.';
      lines[li][L - 1] = '.';
    } else {
      lines[li][0] = '.'; lines[li][1] = '.'; lines[li][2] = '.'; lines[li][3] = 0;
    }
  }
  return li + 1;
}

// Width-based greedy wrap for the Nunito BFF face (variable advances): same
// structure as wrapText() (hard breaks stay breaks, "*" bullets get a blank
// line, "..." marks truncation), but the fit test is the pixel width
// (bffWidth) against BFF_MAX_W instead of a char count, and lines are
// additionally capped at BFF_MAX_CHARS so narrow-glyph runs can't overflow
// the line buffer.
static int wrapBff(const char* s, const lgfx::IFont* f, char lines[][LINE_BUF],
                   int maxLines, bool* truncated) {
  const int maxW = BFF_MAX_W, maxC = BFF_MAX_CHARS;
  int li = 0, o = 0;
  lines[0][0] = 0;
  if (truncated) *truncated = false;   // truncated may be null
  while (*s) {
    bool hadSpace = false;
    while (*s == ' ') { s++; hadSpace = true; }
    if (!*s) break;
    if (*s == '\n') {                          // hard break (kept by htmlToText)
      lines[li][o] = 0;
      li++; o = 0;
      if (li >= maxLines) { if (truncated) *truncated = true; break; }
      s++;
      continue;
    }
    if (o == 0 && *s == '*' && li > 0 &&
        lines[li - 1][0] != 0 && lines[li - 1][0] != '*') {
      lines[li][0] = 0;                        // current (empty) line = the blank
      li++; o = 0;
      if (li >= maxLines) { if (truncated) *truncated = true; break; }
    }
    int wlen = 0;
    while (s[wlen] && s[wlen] != ' ' && s[wlen] != '\n') wlen++;
    int pos = 0;   // progress within the current word
    bool stop = false;
    while (pos < wlen) {
      int pad = (o > 0 && pos == 0 && hadSpace) ? 1 : 0;
      int take = wlen - pos;
      int cap = maxC - (o + pad);
      if (take > cap) take = cap;              // char cap (buffer bound)
      while (take > 0) {
        // Candidate line = current line + pad + word chunk; shrink to fit.
        char cand[LINE_BUF];
        memcpy(cand, lines[li], (size_t)o);
        if (pad) cand[o] = ' ';
        memcpy(cand + o + pad, s + pos, (size_t)take);
        cand[o + pad + take] = 0;
        if (bffWidth(*f, cand) <= maxW) break;
        take--;
      }
      if (pos == 0 && take > 0 && take < wlen && wlen <= maxC) {
        // A word prefix fit, but not the whole word. If the word fits on
        // a fresh line, move it there whole instead of breaking it
        // (only hard-split words that cannot fit a full line either).
        char cand[LINE_BUF];
        memcpy(cand, s, (size_t)wlen);
        cand[wlen] = 0;
        if (bffWidth(*f, cand) <= maxW) take = 0;
      }
      if (take <= 0) {                          // nothing fits: next line
        lines[li][o] = 0;
        li++; o = 0;
        if (li >= maxLines) { if (truncated) *truncated = true; stop = true; break; }
        continue;
      }
      if (pad) lines[li][o++] = ' ';
      memcpy(lines[li] + o, s + pos, (size_t)take);
      o += take;
      pos += take;
    }
    if (stop) break;
    s += wlen;
  }
  if (li >= maxLines) li = maxLines - 1;       // overflow: last full line
  if (!truncated || !*truncated) lines[li][o] = 0;
  if (truncated && *truncated) {
    int L = (int)strlen(lines[li]);
    if (L >= 3) {
      lines[li][L - 3] = '.';
      lines[li][L - 2] = '.';
      lines[li][L - 1] = '.';
    } else {
      lines[li][0] = '.'; lines[li][1] = '.'; lines[li][2] = '.'; lines[li][3] = 0;
    }
  }
  return li + 1;
}

// Nunito BFF text at scale 1.0 in plain screen coordinates. The BFF alpha
// renderer blends edge pixels toward getBaseColor(); on this inverted panel
// that must be the page's actual background code or every glyph gets a white
// halo (same requirement as bffText() in render.cpp).
static void textBff(const lgfx::IFont* f, const char* s, int x, int y, uint16_t c) {
  tft.setBaseColor(colBg);
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1.0f);
  tft.setTextColor(c);
  tft.drawString(s, x, y, f);
}

// ---------------------------------------------------------------------------
//  Drawing
// ---------------------------------------------------------------------------

static void drawHeader(bool canPrev, bool canNext) {
  tft.fillRect(0, 0, 320, HDR_H, colHeader);
  tft.fillRoundRect(BTN_BACK_X, 2, BTN_BACK_W, BTN_H, 5, colBtnBg);
  tft.drawRoundRect(BTN_BACK_X, 2, BTN_BACK_W, BTN_H, 5, colBtnBd);
  tft.fillRoundRect(BTN_PREV_X, 2, BTN_NEXT_W, BTN_H, 5, colNextBg);
  tft.drawRoundRect(BTN_PREV_X, 2, BTN_NEXT_W, BTN_H, 5, colNextBd);
  tft.fillRoundRect(BTN_NEXT_X, 2, BTN_NEXT_W, BTN_H, 5, colNextBg);
  tft.drawRoundRect(BTN_NEXT_X, 2, BTN_NEXT_W, BTN_H, 5, colNextBd);

  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(colBtnTxt);
  tft.drawString("< BACK", BTN_BACK_X + BTN_BACK_W / 2, 13);
  // Page arrows stay in their fixed slots; a glyph is dimmed when it has
  // nothing to navigate to (first/last page of the whole list).
  tft.setTextSize(2);
  tft.setTextColor(canPrev ? colHeader : C(0xb8, 0xc8, 0xd8));
  tft.drawString("<", BTN_PREV_X + BTN_NEXT_W / 2, 12);
  tft.setTextColor(canNext ? colHeader : C(0xb8, 0xc8, 0xd8));
  tft.drawString(">", BTN_NEXT_X + BTN_NEXT_W / 2, 12);

  // Title: centered in the pill-free gap, auto-shrunk.
  const char* title = "WEATHER ALERTS";
  int l = BTN_BACK_X + BTN_BACK_W + 4;
  int rr = BTN_PREV_X - 4;
  int tw = txtW(title, 2);
  float ts = (tw > rr - l) ? (float)(rr - l) * 2.0f / (float)tw : 2.0f;
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(ts);
  tft.setTextColor(C(0xFF, 0xFF, 0xFF));
  tft.drawString(title, (l + rr) / 2, 13);
}

// Fill the content stream for one alert: the description wrapped on the
// body face (pixel width), then (when the alert has instructions) a blank
// line, the dim "WHAT TO DO" label, and the wrapped instructions. Returns
// the page count. fBody is the Nunito body face (nunitoEm13()); nullptr
// selects the GLCD char-width fallback.
static int buildStream(const NwsAlert* a, const lgfx::IFont* fBody) {
  bool dTrunc = false;
  int dn = fBody ? wrapBff(a->description, fBody, gStream, 50, &dTrunc)
                 : wrapText(a->description, gStream, 50, &dTrunc);
  gStreamN = dn;
  for (int i = 0; i < dn; i++) gStreamLabel[i] = false;
  if (a->instructions[0] && gStreamN < STREAM_MAX - 6) {
    gStream[gStreamN][0] = 0; gStreamLabel[gStreamN] = false; gStreamN++;   // blank
    // Keep the label off the bottom row of a page (the instructions would
    // continue on the next one) by starting it on a fresh page.
    if (gStreamN % BODY_LINES == BODY_LINES - 2) {
      gStream[gStreamN][0] = 0; gStreamLabel[gStreamN] = false; gStreamN++;
    }
    snprintf(gStream[gStreamN], LINE_BUF, "WHAT TO DO");
    gStreamLabel[gStreamN] = true;
    gStreamN++;
    char il[4][LINE_BUF];
    bool iTrunc = false;
    const int in = fBody ? wrapBff(a->instructions, fBody, il, 4, &iTrunc)
                      : wrapText(a->instructions, il, 4, &iTrunc);
    for (int i = 0; i < in && gStreamN < STREAM_MAX; i++) {
      memcpy(gStream[gStreamN], il[i], LINE_BUF);
      gStreamLabel[gStreamN] = false;
      gStreamN++;
    }
  }
  return (gStreamN + BODY_LINES - 1) / BODY_LINES;
}

static void drawAlertPage(int idx, int page) {
  tft.fillScreen(colBg);
  tft.setTextDatum(TL_DATUM);   // datum is inherited from the previous caller
  const int count = alertsCount();
  const NwsAlert* a = alertsAt(idx);
  const lgfx::IFont* f9 = nunitoEm9();    // nullptr: BFF fonts failed -> GLCD
  const lgfx::IFont* f12 = nunitoEm12();
  const lgfx::IFont* f13 = nunitoEm13();  // body face (also used for wrapping)
  const int pages = a ? buildStream(a, f13) : 1;
  if (page >= pages) page = pages - 1;
  if (page < 0) page = 0;
  drawHeader(idx > 0 || page > 0, idx < count - 1 || page < pages - 1);
  if (!a) return;   // cache cleared while the screen was up — header only

  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);

  char line[58];   // worst case of four %d fields (values are <= 8 / <= 6)
  if (pages > 1)
    snprintf(line, sizeof(line), "ALERT %d OF %d  P%d/%d", idx + 1, count, page + 1, pages);
  else
    snprintf(line, sizeof(line), "ALERT %d OF %d", idx + 1, count);
  if (f9) {
    textBff(f9, line, 4, COUNTER_Y, colHint);
  } else {
    tft.setTextColor(colHint);
    tft.drawString(line, 4, COUNTER_Y);
  }

  // Event line (Nunito em12, or GLCD size 2), cut from the end to fit the
  // 312 px body width.
  char ev[48];
  if (f12) {
    snprintf(ev, sizeof(ev), "%s", a->event);
    while (bffWidth(*f12, ev) > BFF_MAX_W && strlen(ev) > 1) ev[strlen(ev) - 1] = 0;
    textBff(f12, ev, 4, EVENT_Y, colBgTxt);
  } else {
    fitEvent(a->event, ev, sizeof(ev));
    tft.setTextSize(2);
    tft.setTextColor(colBgTxt);
    tft.drawString(ev, 4, EVENT_Y);
  }

  // Severity (colored) + expiration, one line.
  if (f9) {
    textBff(f9, a->severity, 4, SEV_Y, sevColor(a->severity));
    int sx = 4 + bffWidth(*f9, a->severity) + 4;
    char ends[24];
    if (fmtEnds(a, ends, sizeof(ends))) textBff(f9, ends, sx, SEV_Y, colHint);
  } else {
    tft.setTextSize(1);
    tft.setTextColor(sevColor(a->severity));
    tft.drawString(a->severity, 4, SEV_Y);
    int sx = 4 + txtW(a->severity, 1) + 4;
    char ends[24];
    if (fmtEnds(a, ends, sizeof(ends))) {
      tft.setTextColor(colHint);
      tft.drawString(ends, sx, SEV_Y);
    }
  }

  if (f9) {
    textBff(f9, a->area, 4, AREA_Y, colHint);
  } else {
    tft.setTextSize(1);
    tft.setTextColor(colHint);
    tft.drawString(a->area, 4, AREA_Y);
  }

  // Body: one page of the content stream (description, then instructions).
  const int off = page * BODY_LINES;
  for (int k = 0; k < BODY_LINES; k++) {
    const int i = off + k;
    if (i >= gStreamN || !gStream[i][0]) continue;   // blank/used-up slot
    if (f13) {
      textBff(f13, gStream[i], 4, BODY_Y + k * BODY_PITCH,
              gStreamLabel[i] ? colHint : colBgTxt);
    } else {
      tft.setTextSize(BODY_FONT);
      tft.setTextColor(gStreamLabel[i] ? colHint : colBgTxt);
      tft.drawString(gStream[i], 4, BODY_Y + k * BODY_PITCH);
    }
  }

  // Footer hint (left) + source credit (right), like the settings pages.
  const char* hint = "TAP SCREEN: NEXT PAGE";
  if (page >= pages - 1)
    hint = (idx < count - 1) ? "TAP SCREEN: NEXT ALERT" : "TAP SCREEN: BACK";
  tft.setTextSize(1);
  tft.setTextColor(colHint);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(hint, 8, FOOTER_Y);
  tft.setTextDatum(TR_DATUM);
  tft.drawString("NWS", 312, FOOTER_Y);
  tft.setTextDatum(TL_DATUM);   // leave the shared tft the way the dashboard expects it
}

// ---------------------------------------------------------------------------
//  Public API
// ---------------------------------------------------------------------------

void alertsScreenOpen() {
  if (alertsCount() == 0) return;
  gOpen = true;
  gAlert = 0;
  gPage = 0;
  gAwaitRelease = true;   // the opening tap is still on the glass
  drawAlertPage(0, 0);
}

bool alertsScreenActive() { return gOpen; }

void alertsScreenTouch(int16_t x, int16_t y) {
  const int count = alertsCount();
  if (count == 0) { closeAlerts(); return; }   // cache cleared underneath us
  if (gAlert >= count) { gAlert = count - 1; gPage = 0; }   // cache shrank
  const int pages = buildStream(alertsAt(gAlert), nunitoEm13());
  if (gPage >= pages) gPage = pages - 1;

  if (y <= HDR_H) {                             // header pills
    if (x < BTN_BACK_X + BTN_BACK_W) {          // "< BACK"
      closeAlerts();
      return;
    }
    if (x >= BTN_PREV_X && x < BTN_PREV_X + BTN_NEXT_W) {
      if (gPage > 0) { gPage--; drawAlertPage(gAlert, gPage); }
      else if (gAlert > 0) { gAlert--; gPage = 0; drawAlertPage(gAlert, gPage); }
      return;
    }
    if (x >= BTN_NEXT_X && x < BTN_NEXT_X + BTN_NEXT_W) {
      if (gPage < pages - 1) { gPage++; drawAlertPage(gAlert, gPage); }
      else if (gAlert < count - 1) { gAlert++; gPage = 0; drawAlertPage(gAlert, gPage); }
      return;
    }
    return;                                     // header gaps: nothing
  }

  // Body tap: next page, then next alert; on the last alert, back to the
  // dashboard.
  if (gPage < pages - 1) { gPage++; drawAlertPage(gAlert, gPage); }
  else if (gAlert < count - 1) { gAlert++; gPage = 0; drawAlertPage(gAlert, gPage); }
  else closeAlerts();
}
