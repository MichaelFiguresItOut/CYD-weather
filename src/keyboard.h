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
  KEY_SHIFT   = 130,
  KEY_123     = 131,
  KEY_DEL     = 132,
};

// QWERTY entry (Wi-Fi password). title is shown in the header (the selected
// SSID), label is shown under the field (e.g. "WIFI PASSWORD"). Returns
// KEY_OK when ENTER is pressed (out[] then holds the entry) or KEY_CANCEL
// for < BACK (out[] is to be ignored — the previous password is kept).
int keyboardQwerty(const char* title, const char* label, char* out, int outLen);

// Numeric entry (coordinates). hint is shown under the field until a
// validation error replaces it. Returns KEY_OK when ENTER is pressed with a
// value inside [minVal, maxVal] (out[] then holds the text), or KEY_BACK
// for < BACK (out[] is to be ignored — the original coordinate is kept).
int keypadNumeric(const char* title, const char* hint, char* out, int outLen,
                  int maxLen, float minVal, float maxVal);
