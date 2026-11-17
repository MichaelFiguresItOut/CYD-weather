#pragma once

#include <Arduino.h>
#include <LovyanGFX.hpp>

//  Pin map (confirmed from the board's ESP-IDF BSP)
// ---------------------------------------------------------------------------
#define TFT_CS        15
#define TFT_SCK       14
#define TFT_MOSI      13
#define TFT_DC         2
#define TFT_RST       12
#define TFT_BL        27

#define TOUCH_SDA     33
#define TOUCH_SCL     32
#define TOUCH_INT    -1   // INT is not wired on this board -> polling mode
#define TOUCH_RST     25

// ---------------------------------------------------------------------------
//  Display orientation
//  The glass is natively PORTRAIT (RAM: 240 cols x 320 rows) but mounted in
//  landscape on the board. Rotation 1 yields a full-screen 320x240 landscape
//  canvas; it is rotation 3 flipped by 180 degrees (rot + 2 = upside down),
//  matching how the CYD sits on the desk.
// ---------------------------------------------------------------------------
#define TFT_ROTATION  1

// ---------------------------------------------------------------------------
//  Custom LGFX device for the CYD board
// ---------------------------------------------------------------------------
class CYDDisplay : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789  _panel_instance;
  lgfx::Bus_SPI       _bus_instance;
  lgfx::Light_PWM     _light_instance;
  lgfx::Touch_GT911   _touch_instance;

public:
  CYDDisplay() {
    // ---- Panel (ST7789) ----
    // The glass is natively portrait: RAM is 240 columns x 320 rows.
    // panel_* = native glass size; setRotation(TFT_ROTATION) swaps them into
    // the landscape 320x240 drawing canvas (see TFT_ROTATION note above).
    auto cfg = _panel_instance.config();
    cfg.pin_cs          = TFT_CS;
    cfg.pin_rst         = TFT_RST;
    cfg.panel_width     = 240;   // native glass width
    cfg.panel_height    = 320;   // native glass height
    cfg.memory_width    = 240;   // RAM columns
    cfg.memory_height   = 320;   // RAM rows
    cfg.readable        = false; // THIS BOARD: the TFT's MISO is not wired (only the
                                 // SD card has MISO, GPIO19). LGFX defaults to
                                 // readable=true, which makes the BFF font renderer
                                 // take its "readable alpha" path: it calls
                                 // readRectRGB() per glyph row and blends edge pixels
                                 // against whatever garbage comes back on the unconnected
                                 // MISO line - corrupting every anti-aliased text pixel
                                 // (blurry edges + pale-coloured patches). With
                                 // readable=false the renderer instead blends against
                                 // getBaseColor(), which bffText()/bffCenter() set to
                                 // the exact background behind each text run.
    _panel_instance.config(cfg);

    // ---- SPI bus (attached to the panel) ----
    auto bcfg = _bus_instance.config();
    bcfg.spi_host       = VSPI_HOST;
    bcfg.freq_write     = 40000000;      // 40 MHz
    bcfg.pin_sclk       = TFT_SCK;
    bcfg.pin_mosi       = TFT_MOSI;
    bcfg.pin_dc         = TFT_DC;
    _bus_instance.config(bcfg);
    _panel_instance.setBus(&_bus_instance);

    // ---- Backlight (PWM). If the screen stays dark, set invert = true. ----
    auto lcfg = _light_instance.config();
    lcfg.pin_bl          = TFT_BL;
    lcfg.freq           = 5000;
    lcfg.invert         = false;
    _light_instance.config(lcfg);
    _panel_instance.setLight(&_light_instance);

    // ---- Touch (GT911 over I2C, polling – INT pin not wired) ----
    auto tcfg = _touch_instance.config();
    tcfg.i2c_port        = 0;            // I2C_NUM_0
    tcfg.pin_sda         = TOUCH_SDA;
    tcfg.pin_scl         = TOUCH_SCL;
    tcfg.pin_int         = TOUCH_INT;    // -1 => polling mode
    tcfg.pin_rst         = TOUCH_RST;
    tcfg.i2c_addr        = 0x14;         // default GT911 address (driver also tries 0x5D)
    // Raw GT911 coordinates are in the glass's native portrait space
    // (x: 0..239, y: 0..319). LGFX's convertRawXY() applies the same
    // rotation as the display automatically, so getTouch() returns
    // landscape canvas coordinates – no manual mapping needed.
    tcfg.x_min           = 0;
    tcfg.x_max           = 240;
    tcfg.y_min           = 0;
    tcfg.y_max           = 320;
    _touch_instance.config(tcfg);
    _panel_instance.setTouch(&_touch_instance);

    setPanel(&_panel_instance);
  }
};

extern CYDDisplay tft;

// Bring up display + backlight + touch and set landscape orientation
// (moved out of setup(); the driver class and pin map live here).
void displayInit();
void displayHardReset();

// ---------------------------------------------------------------------------
//  Main-screen layout – single source of truth for item positions/sizes.
//
//  Every draw function AND every touch hit zone reads from gLayout, so a new
//  layout added later (items resized/moved) only needs different values here:
//  assign them to gLayout and call renderScreen() – rendering, the per-second
//  partial updates and the tap areas all follow automatically. The defaults
//  below are the current layout; they match the original hardcoded coordinates
//  exactly (no visual change).
// ---------------------------------------------------------------------------
static inline int glyphH(int size) { return 8 * size; }   // built-in Font0: 8 px tall per size unit

struct Layout {
  int statusBarY = 2;        // status bar line (size 1), top-left / right-aligned
  int clockY     = 18;       // big clock digits, centred full width
  int clockSize  = 4;
  int dateY      = 52;       // date line under the clock (drawn at dateSize or scaled – see dateLineFit)
  int dateSize   = 2;
  int tempY      = 96;       // weather block base y = big temperature line
  int tempSize   = 5;
  int descOffset    = 44;    // description line (size 2), relative to tempY
  int feelsOffset   = 66;    // "Feels .. Hum .." (size 1)
  int todayOffset   = 80;    // "Today H .. L .." (size 1)
  int rainOffset    = 94;    // rain status line (size 1)
  int updatedOffset = 108;   // "Updated .." (size 1)
  int hintBottomGap = 14;    // bottom hint baseline, distance from screen bottom (size 1)
  int dogZoneW     = 96;     // reserved dog corner zone width (bottom-right) – also the dog tap area
  int dogZoneH     = 128;    // reserved dog corner zone height
};
extern Layout gLayout;

// ---- Coordinate transform ---------------------------------------------------
struct T {
  float sx, sy;   // scale (1.0 live, 0.5 preview)
  int ox, oy;     // offset in panel pixels
  static const T ID;
  int   X(float v) const { return ox + (int)(v * sx); }
  int   Y(float v) const { return oy + (int)(v * sy); }
  int   W(float v) const { return (int)(v * sx); }
  int   H(float v) const { return (int)(v * sy); }
};

struct SkyPal {
  uint16_t clock, date, temp, unit, desc, tiny, hint;
  uint16_t stL, dot, stR;            // status row left text / dot / right label
  uint16_t fBg, fBd, fLbl, fVal;     // feels chip
  uint16_t rBg, rBd, rTxt, rDot;     // rain chip
};

struct SplitPal {
  uint16_t left;                      // solid data-zone background
  uint16_t skyLbl, ground;            // DAY/NIGHT label + ground ellipse
  uint16_t stL, dot;                  // status row (no right label here)
  uint16_t clock, date, temp, unit, descTxt;
  uint16_t fBg, fBd, fLbl, fVal, fIcon;   // FEELS chip + crossed-arrows icon
  uint16_t rBg, rBd, rTxt, rDot;      // RAIN chip (rDot = umbrella icon)
  uint16_t row1C, hint;
};

struct BentoPal {
  uint16_t cardBg, cardBd;            // standard cards
  uint16_t clock, date, stL, dot, stR;
  uint16_t accent;                    // orange bar on the temp card
  uint16_t temp, unit;
  uint16_t chipBg, chipBd, chipLbl, chipVal;   // FEELS chip
  uint16_t descTxt;
  uint16_t rBg, rBd, rTxt, rDot;      // rain card
  uint16_t detC, updC;                // detail rows / updated row
  uint16_t dogCardBg, capC;           // dog card + caption
};

// ---- Background + shared drawing helpers (defined in display.cpp) ---
bool nightMode();
uint16_t rgb565Gray(uint8_t v);
uint16_t nightBgColor(int y);
void fillBackground();
void clearRegion(int x, int y, int w, int h);
uint16_t D(uint8_t r, uint8_t g, uint8_t b);
uint16_t C(uint8_t r, uint8_t g, uint8_t b);
extern const SkyPal skyDay;
extern const SkyPal skyNight;
extern const SplitPal splitDay;
extern const SplitPal splitNight;
extern const BentoPal bentoDay;
extern const BentoPal bentoNight;
const SkyPal& SKY();
const SplitPal& SPLIT();
const BentoPal& BENTO();
void lerpRgb(uint8_t ar, uint8_t ag, uint8_t ab, uint8_t br, uint8_t bg, uint8_t bb, float t, uint8_t& r, uint8_t& g, uint8_t& b);
uint16_t bgDisplayed(int idx, int xFull, int yFull);
uint16_t bgCode(int idx, int xFull, int yFull);
extern lgfx::LGFXBase* gT;
void drawThemeBg(int idx, const T& t);
void clearThemed(int idx, const T& t, int xFull, int yFull, int wFull, int hFull);
float fitSize(const char* buf, float sBase, int avail);
void drawCenter(const T& t, int cx, int yFull, const char* buf, float s, uint16_t c, int avail);
void drawRight(const T& t, int xEnd, int yFull, const char* buf, float s, uint16_t c);
void drawSplitRow(const T& t, int xL, int xR, int yFull, const char* l, const char* r, float s, uint16_t c);
void thickLine(const T& t, int x0f, int y0f, int x1f, int y1f, float wFull, uint16_t c);
void drawSun(const T& t, int cx, int cy, uint16_t halo, uint16_t core, uint16_t ray);
void drawFlatSun(const T& t, int cx, int cy);
void drawSplitCloud(const T& t, int x, int y, bool small, uint16_t c);
uint16_t blendWhite(uint16_t c, int pct);
void drawMiniCloud(const T& t, int x, int y, uint16_t c);
void drawCloud(const T& t, int x, int y, float k, uint16_t c);
void drawStar(const T& t, int x, int y, uint16_t c);
void drawUmbrella(const T& t, int cx, int cy, float k, uint16_t c, uint16_t bgC);
void drawDnIcon(const T& t, int cx, int cy, uint16_t c, uint16_t biteC);
const char* dayNightLabel();
void buildWifiStatus(char* buf, size_t n);
void drawStatusRow(const T& t, int xL, int yFull, float s, uint16_t cL, uint16_t dotC, const char* right, uint16_t cR, int xREnd, bool dnIcon = false);
void drawFeelsChip(const T& t, int x, int yF, int w, int h, uint16_t bgC, uint16_t bdC, float sLbl, uint16_t cLbl, const char* val, float sVal, uint16_t cVal);
void drawRainChip(const T& t, int x, int yF, int w, int h, uint16_t bgC, uint16_t bdC, const char* txt, float s, uint16_t cTxt, uint16_t dotC, bool umbrella = false);
void drawCard(const T& t, int x, int y, int w, int h, uint16_t bgC, uint16_t bdC);
void buildRainLine(char* buf, size_t n);
void buildUpdShort(char* buf, size_t n);
void buildTempStr(char* buf, size_t n, float v);
const char* dogBandLabel();
void dogBgDisplayed(int theme, int yFull, uint8_t& r, uint8_t& g, uint8_t& b);
void drawDogBox(const T& t, int xFull, int yTopFull, int fw, int fh, int theme);
void drawDogImage(const T& t, int theme);

// Nunito BFF font bring-up (defined in render.cpp; called by displayInit).
void loadNunitoFonts();
extern bool nunitoOK;
