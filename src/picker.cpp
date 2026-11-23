#include "picker.h"
#include "display.h"
#include "render.h"
#include "theme_ids.h"

// Picker UI state (declared in picker.h).
bool     themeSelectScreen = false;   // true while the THEME picker is shown
int      previewTheme      = 0;       // card highlighted in the picker

// ---------------------------------------------------------------------------
//  THEME picker screen
//
//  Half-scale preview of the selected theme (yellow border) on the normal
//  background, with yellow bars top and bottom:
//    top bar:    "THEME n/4: NAME"
//    bottom bar: "< PREV" ... "NEXT >"
//  Swipe left/right to change themes; tap (no swipe) selects.
// ---------------------------------------------------------------------------
// Picker layout (shared by the static render and the swipe animation so both
// stay in sync). The window is a true half-scale of the 320x240 dashboard.
static const int kPrevW = 160, kPrevH = 120;
static const int kPrevX = (320 - kPrevW) / 2, kPrevY = 60;
static const uint16_t kBarC    = C(0xff,0xd1,0x66);   // displayed yellow (matches the border)
static const uint16_t kBorderC = C(0xff,0xd1,0x66);   // displayed yellow
static const uint16_t kBgC     = C(0xe8,0xed,0xf3);   // solid picker background
// TEMP-DEBUG: per-frame timing print + boot auto-demo (remove after diagnosis)
static bool gTimingOn = false;

// ---- Opening-press drain ---------------------------------------------------
// The tap that opens the picker (dog-zone tap, or Settings "< BACK") is
// usually still on the glass when the picker is drawn; without a drain the
// picker would evaluate that lingering press as its own tap/swipe. Arm the
// drain on every entry point; touch.cpp keeps feeding it until the finger
// lifts.
static bool gAwaitRelease = false;

void pickerEnter() { gAwaitRelease = true; }

void pickerPollRelease() {
  if (!gAwaitRelease) return;
  int16_t x, y;
  if (!tft.getTouch(&x, &y)) gAwaitRelease = false;   // finger lifted
}

bool pickerAwaitingRelease() { return gAwaitRelease; }

// Everything on the picker screen EXCEPT the preview window: background,
// yellow border, yellow bars with labels, hint line. idx is the theme whose
// name goes in the top bar (its neighbours fill the bottom bar).
static void drawPickerChrome(int idx) {
  tft.fillScreen(kBgC);   // full-screen solid background (no stale pixels)

  tft.drawRect(kPrevX - 1, kPrevY - 1, kPrevW + 2, kPrevH + 2, kBorderC);

  // Top bar: theme number + name (centered), corner buttons at both ends.
  // Hit zones (touch.cpp): x < 56 = "< BACK" (dashboard), x >= 264 =
  // "SETTINGS" (settings screen), the rest of the 20 px bar is ignored.
  tft.fillRect(0, 0, 320, 20, kBarC);
  char top[48];
  snprintf(top, sizeof(top), "THEME %d/%d: %s", idx + 1, THEME_COUNT, themeName(idx));  // 1-based for the user
  tft.setTextDatum(TL_DATUM);   // datum is inherited from the previous caller;
                                // leaving it MC_DATUM shifted the label into "< BACK"
  tft.setTextSize(1.5f);
  int tw = tft.textWidth(top);
  tft.setTextColor(TFT_WHITE);   // displays black on the yellow bar
  tft.drawString(top, (320 - tw) / 2, 4);
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(1.0f);
  tft.setTextColor(C(0x3d, 0x5f, 0x8f));   // blue on yellow (matches the hint)
  tft.drawString("< BACK", 28, 10);
  tft.drawString("SETTINGS", 292, 10);
  tft.setTextDatum(TL_DATUM);

  // Hint in the gap above the preview
  const char* hint = "SWIPE OR TAP CORNER: PREV/NEXT - TAP: SELECT";
  tft.setTextSize(1.0f);
  int hw = tft.textWidth(hint);
  tft.setTextColor(C(0x3d,0x5f,0x8f));
  tft.drawString(hint, (320 - hw) / 2, 40);

  // Bottom bar: previous name (left) + next name (right), with arrows
  int prevT = (idx + THEME_COUNT - 1) % THEME_COUNT;
  int nextT = (idx + 1) % THEME_COUNT;
  tft.fillRect(0, 220, 320, 20, kBarC);
  char l[48], r[48];
  snprintf(l, sizeof(l), "< %s", themeName(prevT));
  snprintf(r, sizeof(r), "%s >", themeName(nextT));
  tft.setTextSize(1.5f);
  int rw = tft.textWidth(r);
  tft.drawString(l, 8, 224);
  tft.drawString(r, 320 - 8 - rw, 224);
}

void drawThemePicker() {
  drawPickerChrome(previewTheme);
  tft.setClipRect(kPrevX, kPrevY, kPrevW, kPrevH);
  T t{0.5f, 0.5f, kPrevX, kPrevY};
  renderTheme(previewTheme, t);                 // includes its own background fill
  tft.clearClipRect();
}

// ---------------------------------------------------------------------------
//  Swipe animation: full-screen-width slide.
//
//  The old preview card (half-scale theme, 160x120) slides ALL THE WAY to
//  the left edge (completely off-screen) while the new card slides in from
//  the right edge - or mirrored for "prev" - so the motion spans the entire
//  320 px screen width, not just the central preview box.  Only the preview
//  band (rows kPrevY..kPrevY+120, full width) is touched; the yellow border
//  frame, yellow bars and hint stay put (the left/right border columns are
//  redrawn after each frame so the card slides behind the fixed frame).
//
//  Both cards are pre-rendered ONCE into two 160x120 sprites (38.4 KB each);
//  every animation frame then:
//      1. repaints the whole band with the picker background (clearRegion)
//      2. blits the visible part of the old card (shrinking away)
//      3. blits the visible part of the new card (growing in)
//  so every pixel of the band is written by every frame - no stale pixels,
//  no ghosting, no flicker.
//
//  Card positions (off = 0..kTravel, kTravel = 240):
//      dir > 0 (next):  old x0 = kPrevX - off      (80  -> -160, exits left)
//                       new x0 = 320 - off         (320 -> 80,  enters right)
//      dir < 0 (prev):  old x0 = kPrevX + off      (80  -> 320, exits right)
//                       new x0 = off - kPrevW      (-160-> 80, enters left)
//  Each blit copies the visible intersection of card [x0, x0+160) with the
//  screen [0, 320): see the per-frame math below.
//
//  Sub-rect blits go through panel()->writeImage() with a manual pixelcopy_t
//  (src_bitwidth = full sprite width = 160, src_x = first visible column),
//  because LGFXBase::pushImage() would overwrite src_bitwidth with the sub-
//  rect width.  A 16-bit -> 16-bit same-depth copy is a raw row DMA transfer
//  (for DMA-capable buffers) - byte order matches push_sprite() exactly.
//
//  RAM: the ~13.2 KB retained dog buffers (palette indices + PLTE/tRNS)
//  stay loaded during the animation - two card sprites (38.4 KB each) +
//  the dog fit in the ~134 KB free heap after WiFi, and keeping the dog
//  loaded is what puts the outfit into the baked cards (a reload between
//  the sprite alloc and the bake would fit too - it peaks at ~24.6 KB -
//  but the free/reload dance is avoided anyway).
//
//  Fallbacks (in order): each sprite is allocated from the DMA-capable pool
//  first, then from general heap (rows then pumped by CPU - still smooth);
//  if a sprite can't be allocated at all, the classic immediate-mode
//  animation (re-render both themes each frame inside a clip window, also
//  full-width) runs instead.  Buffers are freed when the animation ends.
// ---------------------------------------------------------------------------
static lgfx::LGFX_Sprite sprA, sprB;
static bool     sprUp = false;
static bool     sprADma = false;      // buffer is in the DMA-capable pool?
static bool     sprBDma = false;
static uint8_t* extBufA = nullptr;    // non-DMA fallback buffer for sprA
static uint8_t* extBufB = nullptr;    // non-DMA fallback buffer for sprB
static const int kTravel = 320 - kPrevX;   // 240: full card width + margin -> whole screen

// Allocate one 160x120 card sprite: DMA-capable pool first (fast row DMA),
// 8-bit pool second (CPU-pumped rows - that pool is NOT DMA-accessible, so
// *dmaOk reports which blit mode the buffer needs).  External buffers are
// tracked as "Preallocated" by LGFX, so deleteSprite() never frees them -
// we do.
static bool allocCardSprite(lgfx::LGFX_Sprite* s, uint8_t** extBuf, bool* dmaOk) {
  if (s->createSprite(kPrevW, kPrevH) != nullptr) { *dmaOk = true; return true; }
  *extBuf = (uint8_t*)heap_caps_malloc((size_t)kPrevW * kPrevH * 2 + 16, MALLOC_CAP_8BIT);
  if (*extBuf) {
    s->setBuffer(*extBuf, kPrevW, kPrevH);
    *dmaOk = false;   // panel DMA cannot read the 8-bit pool -> CPU-pumped blit
    return true;
  }
  heap_caps_free(*extBuf); *extBuf = nullptr;
  return false;
}

static bool allocSwipeSprites() {
  if (sprUp) return true;
  if (!allocCardSprite(&sprA, &extBufA, &sprADma)) return false;
  if (!allocCardSprite(&sprB, &extBufB, &sprBDma)) {
    sprA.deleteSprite();
    if (extBufA) { heap_caps_free(extBufA); extBufA = nullptr; }
    return false;
  }
  sprUp = true;
  return true;
}

static void freeSwipeSprites() {
  if (!sprUp) return;
  sprA.deleteSprite();
  sprB.deleteSprite();
  if (extBufA) { heap_caps_free(extBufA); extBufA = nullptr; }
  if (extBufB) { heap_caps_free(extBufB); extBufB = nullptr; }
  sprUp = false;
}

// Render a full theme at half scale into a 160x120 sprite (card = theme's
// full 320x240 screen squeezed into 160x120, includes background + dog).
static void renderThemeIntoSprite(lgfx::LGFX_Sprite* s, int idx) {
  s->clearClipRect();
  lgfx::LGFXBase* saved = gT;
  gT = s;
  renderTheme(idx, T{0.5f, 0.5f, 0, 0});
  gT = saved;
}

// Redraw the left/right columns of the static yellow border frame (the band
// fill + card blits wipe them; top/bottom border rows are outside the band
// and stay untouched).
static inline void redrawBorderSides() {
  tft.fillRect(kPrevX - 1, kPrevY, 1, kPrevH, kBorderC);
  tft.fillRect(kPrevX + kPrevW, kPrevY, 1, kPrevH, kBorderC);
}

// Blit sub-rect [srcX..srcX+w) x [0..h) of a 160-px-wide 16-bit sprite buffer
// to panel (dstX,dstY).  Replicates LGFX_Sprite::push_sprite()'s pixelcopy
// setup so byte order matches the normal sprite push path exactly.
static inline void blitRegion(const void* buf, bool dma, int srcX, int dstX, int dstY, int w, int h) {
  if (w <= 0 || h <= 0) return;
  lgfx::pixelcopy_t p(buf, tft.getColorDepth(), tft.getColorDepth(), false);
  p.src_bitwidth = kPrevW;      // stride of the full sprite row (in pixels)
  p.src_x = srcX;               // first visible column
  tft.startWrite();
  tft.panel()->writeImage((uint_fast16_t)dstX, (uint_fast16_t)dstY,
                          (uint_fast16_t)w, (uint_fast16_t)h, &p, dma);
  tft.endWrite();
}

void animatePickerSwipe(int dir) {   // +1 = next (enters from right), -1 = prev
  const uint32_t t0 = millis();
  const int oldIdx = previewTheme;
  const int newIdx = (dir > 0) ? (oldIdx + 1) % THEME_COUNT
                               : (oldIdx + THEME_COUNT - 1) % THEME_COUNT;
  previewTheme = newIdx;   // now: tap-to-select and the next swipe work immediately
  Serial.printf("[TOUCH] theme swipe -> %s (%d)\n", themeName(newIdx), newIdx);

  drawPickerChrome(newIdx);   // background, border, bars with NEW labels, hint

  // The ~13.2 KB dog buffers (palette indices + PLTE/tRNS) stay loaded for
  // the whole animation: the two 38.4 KB sprite buffers + the dog fit in
  // the ~134 KB free heap after WiFi, and keeping the dog loaded is what
  // puts the outfit into the baked cards (a reload between sprite alloc
  // and bake peaks at ~24.6 KB - it would fit, but we avoid the dance).
  const bool sprites = allocSwipeSprites();
  if (sprites) {
    const uint32_t tr = millis();
    renderThemeIntoSprite(&sprA, oldIdx);   // old theme baked at half scale (with dog)
    renderThemeIntoSprite(&sprB, newIdx);   // new theme baked at half scale (with dog)
    Serial.printf("[ANIM] pre-render %s+%s: %lu ms\n", themeName(oldIdx),
                  themeName(newIdx), (unsigned long)(millis() - tr));
  } else {
    Serial.println("[ANIM] sprite alloc failed -> immediate-mode fallback (dog included)");
  }

  const int FRAMES = 8;
  for (int f = 1; f <= FRAMES; ++f) {
    const uint32_t f0 = millis();
    const float p = (float)f / (float)FRAMES;
    const float e = 1.0f - (1.0f - p) * (1.0f - p);   // easeOutQuad
    const int off = (f == FRAMES) ? kTravel : (int)(e * (float)kTravel);

    // Card positions for this offset (see header comment):
    const int oldX = (dir > 0) ? (kPrevX - off) : (kPrevX + off);
    const int newX = (dir > 0) ? (320 - off)    : (off - kPrevW);
    // Visible intersection of a card [x0, x0+160) with the screen [0, 320):
    struct Vis { int srcX, dstX, w; };
    auto vis = [](int x0) -> Vis {
      const int srcX = x0 < 0 ? -x0 : 0;          // first visible card column
      const int dstX = x0 < 0 ? 0 : x0;           // matching screen column
      int w = kPrevW - srcX;                      // card columns to the right
      const int right = 320 - dstX;               // screen columns to the right
      if (right < w) w = right;
      return Vis{srcX, dstX, w > 0 ? w : 0};
    };
    const Vis A = vis(oldX);
    const Vis B = vis(newX);

    if (sprites) {
      const void* a = sprA.getBuffer();
      const void* b = sprB.getBuffer();
      tft.fillRect(0, kPrevY, 320, kPrevH, kBgC);   // repaint whole band (picker bg)
      // Each card blits independently: on the final frame the old card is
      // fully off-screen (A.w == 0) while the new one still needs to land
      // at the preview position (an && here left the preview white).
      if (A.w > 0) blitRegion(a, sprADma, A.srcX, A.dstX, kPrevY, A.w, kPrevH);
      if (B.w > 0) blitRegion(b, sprBDma, B.srcX, B.dstX, kPrevY, B.w, kPrevH);
      redrawBorderSides();
    } else {
      // Classic immediate-mode path (slow but always works): repaint the
      // band, then redraw each theme clipped to its visible card rect.
      tft.fillRect(0, kPrevY, 320, kPrevH, kBgC);
      if (A.w > 0) {
        tft.setClipRect(A.dstX, kPrevY, A.w, kPrevH);
        renderTheme(oldIdx, T{0.5f, 0.5f, oldX, kPrevY});
        tft.clearClipRect();
      }
      if (B.w > 0) {
        tft.setClipRect(B.dstX, kPrevY, B.w, kPrevH);
        renderTheme(newIdx, T{0.5f, 0.5f, newX, kPrevY});
        tft.clearClipRect();
      }
      redrawBorderSides();
    }
    if (gTimingOn)
      Serial.printf("[ANIM] f=%d off=%d %s: %lu ms\n", f, off,
                    sprites ? "blit" : "render", (unsigned long)(millis() - f0));

    delay(15);   // blit ~25 ms + 15 ms -> ~40 ms/frame, 8 frames ~320 ms
  }
  if (sprites) freeSwipeSprites();
  // (the dog buffers were never freed - nothing to restore)
  Serial.printf("[ANIM] %s -> %s: %d frames (%s) in %lu ms\n",
                themeName(oldIdx), themeName(newIdx), FRAMES,
                sprites ? "sprites" : "immediate", (unsigned long)(millis() - t0));
}

// TEMP-DEBUG: probe whether two half-scale card sprites fit in RAM at runtime.
void ramProbe() {
  Serial.printf("[RAM] before: free=%lu minFree=%lu\n",
                (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap());
  // Pool detail: DMA-capable vs general heap, incl. largest contiguous block
  multi_heap_info_t infDMA, infGEN;
  heap_caps_get_info(&infDMA, MALLOC_CAP_DMA);
  heap_caps_get_info(&infGEN, MALLOC_CAP_8BIT);
  Serial.printf("[RAM] DMA heap: free=%u KB, largest block=%u KB\n",
                (unsigned)(infDMA.total_free_bytes / 1024), (unsigned)(infDMA.largest_free_block / 1024));
  Serial.printf("[RAM] GEN heap: free=%u KB, largest block=%u KB\n",
                (unsigned)(infGEN.total_free_bytes / 1024), (unsigned)(infGEN.largest_free_block / 1024));
  Serial.printf("[RAM] panel depth=%d bits\n", (int)tft.getColorDepth());
  auto* s1 = new lgfx::LGFX_Sprite();
  bool ok1 = (s1->createSprite(160, 120) != nullptr);
  auto* s2 = new lgfx::LGFX_Sprite();
  bool ok2 = (s2->createSprite(160, 120) != nullptr);
  Serial.printf("[RAM] after 2x(160x120): free=%lu minFree=%lu (s1=%d s2=%d)\n",
                (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(),
                ok1 ? 1 : 0, ok2 ? 1 : 0);
  if (ok1) { s1->deleteSprite(); delete s1; }
  if (ok2) { s2->deleteSprite(); delete s2; }
}
