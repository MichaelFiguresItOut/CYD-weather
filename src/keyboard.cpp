// ---------------------------------------------------------------------------
//  keyboard.cpp — modal blocking keyboards for the settings pages.
//
//  The keyboard shares the SAME header as the settings pages (fixed
//  geometry, 320x240):
//
//      +-----------------------------------------------+
//      | [ < BACK ]          TITLE             [ ENTER ] |   y 0..26
//      |  [ field ................................. ]    |   y 34..58
//      |  status / label line                      |   y 62
//      |  keys ...                                  |
//      +-----------------------------------------------+
//
//  Semantics:
//      ENTER (top right) commits the buffer.
//      < BACK (top left) DISCARDS it — the caller keeps the previous
//      value, so a wrong entry never leaks into the settings.
//  There is no inactivity timeout: the keyboard waits until the user
//  presses ENTER or < BACK.
//
//  Labels are COPIED into the static key table (each Key owns its label
//  string). Pointing at the middle of a longer literal (e.g. &"QWERTYUIOP"[1])
//  would make drawString() print the rest of that string — the garbled
//  QWERTY bug this table design exists to prevent.
// ---------------------------------------------------------------------------
#include "keyboard.h"

#include <Arduino.h>
#include <ctype.h>
#include <string.h>

#include "display.h"   // tft, C(), fillBackground()
#include "time_util.h" // strtof()

namespace {

constexpr int KEY_H = 30;

// Colors — same palette as settings.cpp (displayed values through C()).
const uint16_t kHdr     = C(0x2f, 0x7d, 0xc9);
const uint16_t kFieldBg = C(0xFF, 0xFF, 0xFF);
const uint16_t kFieldBd = C(0xc9, 0xd2, 0xdc);
const uint16_t kFieldTx = C(0x17, 0x22, 0x2e);
const uint16_t kKeyBg   = C(0xd9, 0xec, 0xff);
const uint16_t kKeyBd   = C(0x66, 0x75, 0x86);
const uint16_t kKeyTx   = C(0x17, 0x22, 0x2e);
const uint16_t kBackBg  = C(0xff, 0xd1, 0x66);   // "< BACK" pill (settings accent)
const uint16_t kBackBd  = C(0x9a, 0x74, 0x14);
const uint16_t kBackTx  = C(0x14, 0x14, 0x14);
const uint16_t kActBg   = C(0xff, 0xd1, 0x66);   // "ENTER" pill
const uint16_t kActBd   = C(0x9a, 0x74, 0x14);
const uint16_t kActTx   = C(0x14, 0x14, 0x14);
const uint16_t kErrBg   = C(0xff, 0x9a, 0x99);
const uint16_t kErrTx   = C(0x70, 0x00, 0x00);
const uint16_t kHint    = C(0xa9, 0xb9, 0xcc);

// --- Key table (static; labels owned by each key) ---------------------------

struct Key {
  char lbl[8];
  int  x, y, w;
  int  code;
};

constexpr int MAX_KEYS = 40;
Key keys[MAX_KEYS];
int keyCount = 0;

// --- Shared touch handling -------------------------------------------------

// Wait until every finger is up. The keyboard is entered from a touch-DOWN
// in the settings tap handler, so the opening tap is still on the glass:
// without this drain it would register as the first key press.
void waitTouchUp() {
  lgfx::touch_point_t p[5];
  while (tft.getTouch(p, 5) > 0) {
    delay(10);
  }
}

// Where the last field tap landed (x), for cursor placement.
int fieldTapX = -1;

// One blocking tap: waits for a press, tracks it, returns the pressed key
// code, or -1 if the finger lifted before reaching a key.
int pollTap() {
  lgfx::touch_point_t p[5];
  while (tft.getTouch(p, 5) == 0) {
    delay(10);
  }
  int x = p[0].x, y = p[0].y;
  while (tft.getTouch(p, 5) > 0) {
    x = p[0].x;
    y = p[0].y;
    delay(10);
  }
  for (int i = 0; i < keyCount; i++) {
    if (x >= keys[i].x && x < keys[i].x + keys[i].w &&
        y >= keys[i].y && y < keys[i].y + KEY_H) {
      return keys[i].code;
    }
  }
  // Header pills (not in keys[]): left half = "< BACK", right pill = ENTER.
  if (y < 26) {
    if (x < 160) return KEY_BACK;
    if (x >= 260) return KEY_OK;
  }
  // The field box (drawField: 8,34,304,24) — cursor placement (QWERTY).
  if (x >= 8 && x < 312 && y >= 34 && y < 58) {
    fieldTapX = x;
    return KEY_FIELDTAP;
  }
  return -1;
}

int txtW(const char* s, float size) {
  tft.setTextSize(size);
  return (int)tft.textWidth(s);
}

// --- Shared header (identical layout to settings.cpp) ----------------------

void drawHeader(const char* title, const char* action) {
  tft.fillRect(0, 0, 320, 26, kHdr);
  // "< BACK" — top-left, same pill as the settings pages.
  tft.fillRoundRect(2, 2, 58, 22, 5, kBackBg);
  tft.drawRoundRect(2, 2, 58, 22, 5, kBackBd);
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(kBackTx);
  tft.drawString("< BACK", 31, 13);
  // Action pill (ENTER) — top-right.
  if (action && action[0]) {
    tft.fillRoundRect(260, 2, 58, 22, 5, kActBg);
    tft.drawRoundRect(260, 2, 58, 22, 5, kActBd);
    tft.setTextColor(kActTx);
    tft.drawString(action, 289, 13);
  }
  // Title, auto-shrunk so long ones (an SSID) stay clear of both pills.
  tft.setTextDatum(MC_DATUM);
  float ts = 2.0f;
  int tw = txtW(title, ts);
  if (tw > 190) ts *= 190.0f / tw;
  tft.setTextSize(ts);
  tft.setTextColor(C(0xFF, 0xFF, 0xFF));
  tft.drawString(title, 160, 13);
}

// Field box + status/label line below it. Does NOT fill the screen — the
// caller does fillBackground() FIRST, then drawHeader(), then this (the old
// order of header-then-fill wiped the whole header bar).
void drawField(const char* text, const char* status, bool err, int inset = 14) {
  tft.fillRoundRect(8, 34, 304, 24, 5, err ? kErrBg : kFieldBg);
  tft.drawRoundRect(8, 34, 304, 24, 5, err ? kKeyBd : kFieldBd);
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(2);
  tft.setTextColor(err ? kErrTx : kFieldTx);
  tft.setClipRect(10, 35, 300, 22);   // long entries clip inside the box
  tft.drawString(text, inset, 38);   // inset < 14 = scrolled long entry
  tft.clearClipRect();
  tft.setTextSize(1);
  tft.setTextColor(kHint);
  tft.drawString(status, 10, 62);
}

void addCharKey(char c, int x, int y, int w, int code) {
  if (keyCount >= MAX_KEYS) return;
  Key& k = keys[keyCount++];
  k.lbl[0] = c;
  k.lbl[1] = 0;
  k.x = x; k.y = y; k.w = w; k.code = code;
}

void addWordKey(const char* s, int x, int y, int w, int code) {
  if (keyCount >= MAX_KEYS) return;
  Key& k = keys[keyCount++];
  strncpy(k.lbl, s, sizeof(k.lbl) - 1);
  k.lbl[sizeof(k.lbl) - 1] = 0;
  k.x = x; k.y = y; k.w = w; k.code = code;
}

// label overrides the stored label (the SHIFT key shows "abc"/"ABC").
void drawKey(const Key& k, const char* label) {
  const char* L = (label != nullptr) ? label : k.lbl;
  int fs = (L[1] == 0) ? 2 : 1;   // single character vs word
  tft.fillRoundRect(k.x, k.y, k.w, KEY_H, 5, kKeyBg);
  tft.drawRoundRect(k.x, k.y, k.w, KEY_H, 5, kKeyBd);
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize((float)fs);
  tft.setTextColor(kKeyTx);
  tft.drawString(L, k.x + k.w / 2, k.y + KEY_H / 2 + 1);
}

}  // namespace

// Map a tap's x to a character index of s, drawn left to right (built-in
// font) with the first character's left edge at x0. Per-character widths
// come out of consecutive textWidth() measurements of the remaining tail.
// Taps left of the text return 0, taps past the end return strlen(s).
// (External linkage: settings.cpp uses it for the coordinate rows.)
int textCharIndexAt(const char* s, int x0, int x, float size) {
  if (x <= x0) return 0;
  int n = (int)strnlen(s, 40);
  int cx = x0;
  for (int i = 0; i < n; i++) {
    int w = txtW(s + i, size) - txtW(s + i + 1, size);
    if (x < cx + w / 2) return i;
    cx += w;
  }
  return n;
}

// --- QWERTY (Wi-Fi password entry) ------------------------------------------

namespace {

void buildQwertyKeys(bool shift, bool digits) {
  keyCount = 0;
  if (!digits) {
    // row0: 10 keys w25 (x = 21 + i*28); row1: 9 keys w32 (x = 4 + i*35);
    // row2: 7 letters + abc + BAK, all w32 (x = 4 + i*35).
    const char* r0 = "QWERTYUIOP";
    const char* r1 = "ASDFGHJKL";
    const char* r2 = "ZXCVBNM";
    for (int i = 0; i < 10; i++) {
      char c = shift ? r0[i] : (char)tolower(r0[i]);
      addCharKey(c, 21 + i * 28, 82, 25, (int)c);
    }
    for (int i = 0; i < 9; i++) {
      char c = shift ? r1[i] : (char)tolower(r1[i]);
      addCharKey(c, 4 + i * 35, 116, 32, (int)c);
    }
    for (int i = 0; i < 7; i++) {
      char c = shift ? r2[i] : (char)tolower(r2[i]);
      addCharKey(c, 4 + i * 35, 150, 32, (int)c);
    }
    addWordKey("abc", 4 + 7 * 35, 150, 32, KEY_SHIFT);   // x249, label drawn dynamically
    addWordKey("BAK", 4 + 8 * 35, 150, 32, KEY_BAK);     // x284, backspace (left of |)
  } else {
    // row0: digits w25; rows 1-2: 8 symbols + ABC + BAK, all w28 (x = 6 + i*31).
    const char* d0 = "1234567890";
    const char* s1 = "-.@#$%&!?";
    const char* s2 = ":;,()/'\"";
    for (int i = 0; i < 10; i++)
      addCharKey(d0[i], 21 + i * 28, 82, 25, (int)d0[i]);
    for (int i = 0; i < 8; i++)
      addCharKey(s1[i], 6 + i * 31, 116, 28, (int)s1[i]);
    for (int i = 0; i < 8; i++)
      addCharKey(s2[i], 6 + i * 31, 150, 28, (int)s2[i]);
    addWordKey("ABC", 6 + 8 * 31, 150, 28, KEY_123);     // x254: back to letters
    addWordKey("BAK", 6 + 9 * 31, 150, 28, KEY_BAK);     // x285, ends 313
  }
  // Bottom row, identical in both modes (no duplicated labels): 123 swaps
  // the letter/symbol pages, SPACE inserts a space, DEL deletes right of
  // the cursor. BAK (backspace, left of the cursor) lives in row 2.
  addWordKey("123",   21,  184, 54,  KEY_123);
  addWordKey("SPACE", 78,  184, 90,  ' ');
  addWordKey("DEL",   171, 184, 128, KEY_DEL);
}

}  // namespace

int keyboardQwerty(const char* title, const char* label, char* buf, int bufSize) {
  waitTouchUp();
  bool shift = true;
  bool digits = false;
  buildQwertyKeys(shift, digits);
  // The field shows a static "|" cursor; tapping the field moves it
  // (KEY_FIELDTAP), printable keys insert AT the cursor, BAK deletes the
  // character LEFT of it, DEL the character RIGHT of it.
  int cur = (int)strnlen(buf, bufSize);   // start at the end (buffers open empty)
  int scroll = 0;                          // px scrolled left, used for field taps

  auto redraw = [&]() {
    fillBackground();
    drawHeader(title, "ENTER");
    char disp[80];
    int n = (int)strnlen(buf, bufSize);
    if (cur > n) cur = n;
    memcpy(disp, buf, cur);
    disp[cur] = '|';
    memcpy(disp + cur + 1, buf + cur, n - cur + 1);   // rest incl. NUL
    // Scroll only when the entry does not fit the 300 px field:
    // 31-char SSIDs and 64-char passwords otherwise hide the cursor.
    int scrollMax = txtW(disp, 2.0f) - 280;
    if (scrollMax > 0) {
      char pre[80];
      memcpy(pre, disp, cur);
      pre[cur] = 0;
      scroll = 14 + txtW(pre, 2.0f) - 160;   // center the cursor in the field
      if (scroll < 0) scroll = 0;
      if (scroll > scrollMax) scroll = scrollMax;
    } else {
      scroll = 0;
    }
    drawField(disp, label, false, 14 - scroll);
    for (int i = 0; i < keyCount; i++) {
      const char* L = (keys[i].code == KEY_SHIFT) ? (shift ? "abc" : "ABC") : nullptr;
      drawKey(keys[i], L);
    }
  };
  redraw();

  int r = KEY_CANCEL;
  while (true) {
    int code = pollTap();
    if (code < 0) continue;
    if (code == KEY_BACK) break;                 // discard
    if (code == KEY_OK) { r = KEY_OK; break; }   // commit
    if (code == KEY_SHIFT) {
      shift = !shift;
      buildQwertyKeys(shift, digits);
      redraw();
      continue;
    }
    if (code == KEY_123) {
      digits = !digits;
      shift = false;
      buildQwertyKeys(shift, digits);
      redraw();
      continue;
    }
    if (code == KEY_FIELDTAP) {
      cur = textCharIndexAt(buf, 14 - scroll, fieldTapX, 2.0f);   // 14 = field text inset
      redraw();
      continue;
    }
    size_t len = strnlen(buf, bufSize);
    bool changed = false;
    if (code == KEY_BAK) {            // backspace: character LEFT of the cursor
      if (cur > 0) {
        memmove(buf + cur - 1, buf + cur, len - cur + 1);
        buf[len - 1] = 0;
        cur--;
        changed = true;
      }
    } else if (code == KEY_DEL) {     // delete: character RIGHT of the cursor
      if (cur < (int)len) {
        memmove(buf + cur, buf + cur + 1, len - cur);
        buf[len - 1] = 0;
        changed = true;
      }
    } else if ((code >= '0' && code <= '9') || (code >= 32 && code < 128)) {
      // Printable key (digit, space or symbol) — inserted AT the cursor.
      if (len + 1 < bufSize) {
        memmove(buf + cur + 1, buf + cur, len - cur + 1);
        buf[cur] = (char)code;
        cur++;
        changed = true;
      }
    }
    if (changed) redraw();
  }
  return r;
}
