#pragma once

// ---------------------------------------------------------------------------
//  Blocking on-screen keyboards (320x240, settings sub-pages only).
//
//  Both functions paint the full screen, block until the user finishes, and
//  return a result code — the caller then redraws its page. Every keyboard
//  page carries the SAME header chrome as the settings pages:
//      [ < BACK ]  TITLE  [ ENTER ]
//  with "< BACK" in the top-left corner (2,2,58,22) and ENTER in the
//  top-right (260,2,58,22), so the back button is in one fixed position on
//  every settings screen.
//
//  Semantics (both layouts):
//      ENTER commits the entry; < BACK discards it — the caller must keep
//      its previous value when anything but KEY_OK is returned.
//  There is no inactivity timeout: the keyboard waits until the user
//  chooses. (The keyboard also drains the still-pressed opening tap
//  itself, so touch.cpp's down-edge routing cannot double-fire it.)
// ---------------------------------------------------------------------------

// Result codes (return values).
enum {
  KEY_OK      = 0,   // ENTER pressed (keypad: entry validated)
  KEY_CANCEL  = 1,   // QWERTY "< BACK": discard
  KEY_BACK    = 2,   // keypad "< BACK": discard (out[] must be ignored)
};

// Internal key codes (distinct from ASCII so a code can never be inserted
// as a text character):
enum {
  KEY_SHIFT    = 130,
  KEY_123      = 131,
  KEY_DEL      = 132,   // both keyboards: delete character RIGHT of the cursor
  KEY_FIELDTAP = 133,   // tap on the input field (moves the | cursor, both keyboards)
  KEY_BAK      = 134,   // both keyboards: backspace, character LEFT of the cursor
};

// QWERTY entry (Wi-Fi password / manual SSID). title is shown in the
// header (the selected SSID), label is shown under the field (e.g.
// "WIFI PASSWORD"). Returns KEY_OK when ENTER is pressed (out[] then
// holds the entry) or KEY_CANCEL for < BACK (out[] is to be ignored —
// the previous password is kept).
//
// Same cursor model as the numeric keypad: the field shows a static "|"
// cursor, tapping the field moves it to the tapped position, printable
// keys insert at the cursor, BAK deletes the character to the LEFT of the
// cursor and DEL the character to the RIGHT. Long entries (SSIDs up to
// 31 chars, passwords up to 64) overflow the field, which then scrolls to
// keep the cursor visible.
int keyboardQwerty(const char* title, const char* label, char* out, int outLen);

// Numeric entry (coordinates). hint is shown under the field until a
// validation error replaces it. Returns KEY_OK when ENTER is pressed with a
// value inside [minVal, maxVal] (out[] then holds the text), or KEY_BACK
// for < BACK (out[] is to be ignored — the original coordinate is kept).
//
// The field shows the buffer with a static "|" cursor (no blink). Tapping
// the field moves the cursor to the tapped position; digits and "-" insert
// at the cursor. BAK (backspace) deletes the character to the LEFT of the
// cursor, DEL deletes the character to the RIGHT — so a coordinate can be
// edited in place instead of retyped. initCursor is the starting index
// (the digit the finger tapped on the settings row) or -1 for the end of
// the number.
int keypadNumeric(const char* title, const char* hint, char* out, int outLen,
                  int maxLen, float minVal, float maxVal, int initCursor = -1);

// Map a tap's x to a character index of s, assuming s is drawn left to
// right (built-in font, size `size`) with the first character's left edge
// at x0. Taps left of the text return 0, taps past the end return
// strlen(s); within a character, the nearer boundary wins.
int textCharIndexAt(const char* s, int x0, int x, float size);
