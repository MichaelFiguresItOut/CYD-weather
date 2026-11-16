# CYD (ESP32-2432S032C-I) — Hardware & Library Programming Reference

Board, display, font, SD-card and flashing knowledge for programming the LILYGO CYD,
split out of `README.md` (kept as-is for reference). Companion to
`README-WeatherApp.md`, which documents the weather app itself.

## Board: ESP32-2432S032C-I ("CYD")
- ESP32 dual-core, 4 MB flash, CH340 USB serial → **COM4** @ 115200 baud
  (**USB-C cable into the PC's front USB-C port** — this is the working setup).
  The old USB-A cable into a rear USB-A port enumerated as COM3 but gave flaky
  "port busy / access denied" errors; don't use it. A stale COM3 entry may
  linger in Device Manager (status Unknown) — ignore it.
- 1.9" ST7789 TFT: native glass **240x320 portrait**, used as **320x240 landscape** (LGFX rotation 1 = rotation 3 flipped 180°)
- GT911 capacitive touch

| Function | GPIO |
|---|---|
| Display CS / RST / DC / SCK / MOSI | 15 / 12 / 2 / 14 / 13 |
| Backlight (PWM) | 27 |
| Touch SDA / SCL / RST | 33 / 32 / 25 |

## Display gotchas (cost real debugging time — read before touching UI code)
- **The ST7789 is NOT reset when the ESP32 resets.** The RST button and
  flashing only reset the MCU; the display controller keeps its own volatile
  state (internal address/window registers, GRAM), and LGFX's `Panel_ST7789`
  init list does NOT include SWRESET (0x01) — its only reset is a pin pulse
  via `cfg.pin_rst`. So `setup()` starts with `displayHardReset()`: a
  100 ms hardware pulse on `TFT_RST` (GPIO 12) followed by an explicit
  SWRESET over SPI, then `tft.init()`. **Keep this** — it is cheap defense
  against genuinely stale panel state (e.g. a brownout glitch during boot).

  **Correction (2026-11-15): the "persistent pale/mottled patches" that
  originally motivated this sequence were NOT a panel-state problem.** They
  were a software rendering bug — the SPLIT theme's `bffCenter` scale-argument bug
  (post-mortem in the Theme 1 fonts section, Gotcha #3). A misdiagnosed
  software bug re-renders at every boot, so it *always* survives reset and
  power cycle — which is exactly what made the panel-reset hypothesis look
  plausible. The hard reset never fixed anything; the code fix did. Lesson:
  if a "display artifact" appears and survives reflash, audit the rendering
  code first (pixel-sampling a photo of the screen is decisive — see that
  post-mortem) before blaming panel state, brownouts or reset sequences.
- **The panel displays INVERTED colors: displayed = ~written.** Verified against
  photos (write cyan 0x07FF → see red; write yellow 0xFFE0 → see blue).
  To display white, write `TFT_BLACK`; to display black, write `TFT_WHITE`.
  All palette choices in main.cpp are made for the *displayed* result.

  | Write (code value) | Displays on glass |
  |---|---|
  | TFT_BLACK 0x0000 | white |
  | TFT_WHITE 0xFFFF | black |
  | TFT_YELLOW 0xFFE0 | blue |
  | TFT_CYAN 0x07FF | red |
  | TFT_GREENYELLOW 0xFFE7 | dark blue |
  | TFT_SKYBLUE | brown/olive |

- **LGFX v1 is immediate mode (no framebuffer).** Full-screen redraws flicker.
  Per-second updates must use partial repaints: `updateDynamicTexts()` +
  `clearRegion()`, which repaint only the exact bounding boxes with matching
  background rows. Full `renderScreen()` only on state changes (boot, fetch, touch).
- **An edge that looks "cut off" on glass but correct in preview/mockup is being
  overpainted by something drawn LATER — audit the draw order, not the shape
  primitive or the panel.** (2026-11-15, SPLIT chip corner.) Symptom: the
  FEELS/RAIN chip right edges looked flat/clipped in a glass photo while the
  host preview showed the rounded corner. Red herrings chased: ST7789 window
  clipping at the x=214 seam, then a suspected LGFX `fillRoundRect`/
  `drawRoundRect` bug — the LGFX v1 quadrant (midpoint-circle) algorithm was
  hand-traced for the exact 213×27 r=9 chip and it does emit the corner pixels
  (the arc reaches x=224). Pixel-sampling the photo (calibrating screen↔photo
  using the display's left/right glass edges on a clean row) localized the true
  edge: chip fill stayed green to device x≈223, panel blue after — the corner
  region (216..225) had been repainted with panel color. Root cause:
  `drawDogBox()` pushed the **full composited row** of the dog PNG and
  `compositeRow()` writes the *background color* for alpha=0 pixels. The dog
  PNGs have a 17 px transparent left margin (opaque bbox starts at image-x 17
  → device x 238; the box starts at x=221), so the margin band 221..237
  painted panel gradient over the chip corner arc on every render. It was
  latent for a long time: the old 190 px chips ended at x=202, under nothing;
  it appeared only when the chips were widened to the mockup content-box
  (213 px, ending at x=225). Fix: `dogRowSpan()` (first/last alpha>0 column
  per row) + `drawDogBox()` pushes only the opaque span and skips fully
  transparent rows — alpha>0 edge pixels still blend against
  `dogBgDisplayed()`. One shared routine, so every theme + the picker previews
  got the fix. Takeaways: for any "clipping" at a suspicious x/y, list what is
  drawn *after* the element in that region and check the **opaque extent** of
  each (image bboxes, clear bands, clip rects); and when a photo disagrees
  with the preview, sample its pixels to localize the real edge before
  theorizing (see the panel-reset correction above for the same lesson).
- **`setTextColor(fg)` single-arg = no glyph cell fill** (text sits directly on
  the background — required for text over gradients). Two-arg fills each glyph's
  box with the bg color.
- Panel config uses native glass dims (`panel_width=240, panel_height=320`,
  memory 240x320) + `setRotation(1)` (landscape, flipped 180° from rotation 3).
  Touch coords are auto-mapped by LGFX (`convertRawXY`) — no manual touch
  mapping needed; this keeps working when the rotation value changes.
- **The TFT's MISO pin is NOT wired on this board** (only SCK/MOSI/DC/CS/BL;
  see the pin map above). LGFX defaults to `readable=true`, which makes the BFF
  font renderer take its "readable alpha" path: it calls `readRectRGB()` to get
  the background under each glyph and blends intermediate coverage against that.
  With MISO unconnected those reads return garbage, so any AA (intermediate-
  coverage) glyphs render as corrupted/haloed pixels. Two rules follow:
    1. Keep `cfg.readable = false` in `CYDDisplay()` — the renderer then blends
       against `getBaseColor()` instead of doing SPI reads.
    2. Generated BFF fonts must use **binary coverage ({0,15})** so no blending
       happens at all (see `tools/make_nunito_bff.py`).
  Also: never trust `readPixel()`/`readRect()` on this board — GRAM readback is
  invalid and any "pixel sampling" debug output is garbage.

## Nunito BFF fonts (weather app themes 1–3: SKY, SPLIT, BENTO)
(For the general workflow of adding *another* font, see the Fonts (BFF) section below.)

Theme 1's text is drawn with **Nunito** (weight 700) at six fixed em sizes — 9/10/12/13/20/48 px — stored in flash as compact BFF blobs (`src/nunito_bff.h`, ~60 KB total). Coverage is **binary** (alpha thresholded to {0,15} at generation time — see gotcha #2 below). Theme 2 (SPLIT) reuses the same masters, but draws **small text at scale 1.0 in the master closest to the mockup's CSS pixel size** (mockup 9px → em9, 9.5px → em10, 10.5px → em10, 11.5px → em12, 12px → em12) — see gotcha #4, why sub-1.0 scaling is forbidden for thin-stroke fonts. Only the large thick-stroke glyphs keep fractional scaling (clock em48 @ ≤37/48, temp em48 @ ≤43/48, unit em20 @ 0.95, FEELS value em20 @ ≤0.9). Theme 3 (BENTO) likewise reuses the masters at 1.0 for its small text (status/detail em10, date em13, temp unit em20 @ 0.75) and keeps fractional scaling only on the thick em48 clock (≤27/48) and temp (34/48). Theme 0 (BASIC) still uses LGFX's built-in bitmap font.

- **One `BFFfont` per master** (`nunitoF9/F10/F12/F13/F20/F48`, loaded in `loadNunitoFonts()` at setup): LGFX's runtime font slot holds only one font, so six instances are created and drawn through the explicit-font overloads (`bffText`/`bffCenter` → `drawString(..., IFont*)`).
- **Shrink-to-fit**: `bffFit(f, s, avail)` = min(1.0, avail / measured width); applied to clock (≤316 px), date (≤300), temperature, description, FEELS value and rain line so long strings never overflow their band.
- **Fallback**: if any master fails `loadFont()`, `nunitoOK` stays false and theme 1 renders the original bitmap-font layout (`renderTheme1Legacy()` + legacy status/clock/date bands) — same positions as before the font work. Boot log: `[FONT] nunito bff: ok` or `... FAILED - using bitmap fallback`.
- **Regenerate**: `python tools/make_nunito_bff.py` (Pillow; reads `tools/fonts/Nunito-wght.ttf`, weight 700) rewrites `src/nunito_bff.h` for every size in its `MASTERS` list. Then verify with `python tools/check_bff_space.py` — it mimics `BFFfont` parsing exactly and must print `bad glyphs: 0/95` for all masters (it auto-discovers whichever `nunito_emN_data` arrays exist).
- **Preview**: `python tools/preview_theme1.py` writes `preview_theme1_day.png` / `_night.png`, replicating the firmware's BFF parsing + placement math exactly, so a correct preview guarantees correct on-glass rendering.

**Gotcha (cost real debugging time): glyph regions must be padded to full bytes.**
Each BFF glyf region is one bitstream: header bits (advance/bbox/w/h) followed by 4bpp pixels, MSB-first, with no byte alignment between them. `tools/make_nunito_bff.py`'s `BitWriter.to_bytes()` pads every region out to `ceil(total_bits/8)` bytes. If that padding is ever "optimized" away (dropping trailing zero bytes), pixel-less glyphs — space and .notdef — shrink below the header size (`ceil(header_bits/8)` = 3–4 B); `BFFfont::loadGlyphInfo()` then rejects them and LGFX's `drawCharDummy()` draws a box outline in foreground color at every space ("WIFI|NTP", "3:28|PM"). Glyphs with pixels survive the truncation only because `bit_stream_t::read_bits` clamps out-of-range reads to 0 (transparent) — which is why the bug was invisible until you looked at spaces.

**Gotcha #2 (visible on glass): keep glyph coverage binary.**
LGFX's `draw_alpha_bitmap_common()` blends intermediate-alpha pixels against the *base color* (`getBaseColor()`, default black — the app never calls `setBaseColor`) in code space, and this panel displays ~written. So every AA edge pixel renders as ~(blend-toward-black) = a bright/whitish halo around each letter (worst on em10 status text: "WIFI NTP", "DAY"). The generator therefore thresholds coverage to {0,15} (`nibble()` in `tools/make_nunito_bff.py`): binary pixels are never blended, so edges render as exact fg/bg colors everywhere. Don't "restore" smooth 4bpp alpha — the halo comes back.

**Gotcha #3 (cost a multi-day misdiagnosis): the 7th arg of `bffText`/`bffCenter` is the float scale — never omit it.**
Signature: `bffText/bffCenter(t, f, s, x|cx, y, color, float sc = 1.0f, uint16_t back = 0)`. 7th parameter = text scale, 8th = the background code under the glyphs (the fork's alpha renderer blends edge pixels toward `getBaseColor()`, which these helpers set from `back`). On 2026-11-15 the Theme 2 (SPLIT) night renderer called
`bffCenter(t, nunitoF10, dayNightLabel(), 267, 38, p.skyLbl, bgCode(THEME_SPLIT, 267, 43))` —
`bgCode(...)` (a 16-bit color, ≈ 24,000–49,000 as a float) was silently converted into the **scale** parameter, so `setTextSize(~49000)` drew the "NIGHT" label at ~49,000× scale: enormous amber glyph blobs (the label uses the same amber as the moon, `C(0xff,0xc8,0x57)`) sweeping across the left data zone and part of the right sky panel — the "yellow background" artifact on the night Split theme. The compiler can't catch this (uint16 → float is a legal implicit conversion), and it re-rendered at every boot, so it survived every reset/power cycle and was briefly misdiagnosed as ST7789 stale state (see Display gotchas). Photo diagnosis that identified it:
  (a) every "yellow" region in the photo sampled to RGB ≈ (210,210,95) — identical to the amber moon crescent on the same photo, and in night Split the label + moon are the only two things using that amber;
  (b) render order explains the picture exactly — the giant label is drawn right after the background fill + moon, so everything drawn afterwards (clock, temp, chips, dog box) appears on top of the yellow, while background/label zones don't;
  (c) the theme-picker preview showed the same blob at half scale, because the preview runs the same `renderTheme2` — proving it was code, not hardware.
Fix: pass the scale explicitly — `bffCenter(..., p.skyLbl, 1.0f, bgCode(THEME_SPLIT, 267, 43))` — so `bgCode` lands in the `back` slot it was meant for. Rule: every `bffText`/`bffCenter` call has an explicit float in the 7th position.

**Gotcha #4 (visible on glass): never scale a BFF master below 1.0 — small text garbles.**
Theme 2 (SPLIT) was originally drawn at the mockup's exact fractional sizes (date em13 @ 10.5/13 ≈ 0.808, status em10 @ 0.95, row1/hint em10 @ 0.85, desc em13 @ 12/13, rain em13 @ 11.5/13). On glass the small lines came out garbled: 'Sunday 15 November 2026' read like "Snday I5 Mvember 2026", 'HUM 65%...' like "HM 65...", with strokes vanishing and neighbors merging. The font blobs were proven byte-correct (host decode matches Pillow rasterization), and the host preview at the same scales was crisp — so it was the runtime scaler. In `lgfx_fonts.cpp`, `BFFfont::drawChar()` maps every source pixel through `draw_alpha_bitmap_common()` even at scale 1.0, and that function is a **nearest-neighbor scaler with integer floor mapping**: dest pixel x samples source column `⌊x/scale⌋`. When scale < 1, consecutive destination columns share source columns, and some source columns map to *zero* destination columns and are simply dropped. At 0.808, 1-px-wide strokes (the right stem of 'H', the 'l' in 'November', the '1' in '15') land on a dropped column and vanish. Reproduced exactly on the host by simulating the same floor mapping (thin strokes disappear at the same positions as the photo). The reason 1.0 looks perfect and 0.808 garbles is that at sx = 65536 the mapping is the identity. Note the same decimation happens for *any* BFF text at a fractional scale — even the large clock (em48 @ 37/48) is slightly degraded, it's just invisible on 3-4 px thick strokes.
Fix (2026-11-15): added em9/em12 masters to `make_nunito_bff.py`'s `MASTERS` and regenerated `src/nunito_bff.h`; Theme 2 now draws every thin-stroke line at **scale 1.0 in the nearest master** (status/date → em10, row1/hint/FEELS label → em9, desc/rain → em12) and only the big thick glyphs keep fractional scaling. Sizes land within 0.5 px of the mockup. Rule: for BFF fonts, either use a native-size master at 1.0, or only accept fractional scales on ≥20 px fonts where stroke width survives the decimation.

## Fonts (BFF): adding a new one
All custom text on the CYD goes through LovyanGFX's **BFF** format (4bpp alpha, no compression). Themes 1, 2 and 3 use generated Nunito masters; theme 0 (BASIC) uses LGFX's built-in bitmap font (`glyphH(size)` = 8×size, fixed-pitch 6×8 px per size unit). To add another custom font, follow the exact workflow that produced `src/nunito_bff.h` — every step below exists because something broke when it was skipped.

**Pipeline**: TTF (variable or static) → Pillow rasterize at chosen em sizes → BFF blob per master → `static const uint8_t` arrays in a generated header → one `lgfx::BFFfont` + `PointerWrapper` per master in main.cpp → draw with the explicit-font overloads.

1. **Pick the font file and weight.** Variable fonts: load once, then `font.set_variation_by_axes([700])` before any rasterization or metric call (weight 700 matched the mockups). Keep the TTF under `tools/fonts/`.
2. **Pick em sizes per layout band** (the set is 9/10/12/13/20/48 px — one master per mockup text size; theme 2 draws at 1.0 in the nearest master, see gotcha #4). Blob size scales with em² — em48 is ~36.9 KB of the ~59.6 KB total for six masters. Rasterize each glyph with an explicit anchor — `d.text((PX,PY), ch, font=f, fill=255, anchor="ls")` (left-baseline) at a fixed offset inside a padded canvas; ink bbox = `img.getbbox()`. Check top/bottom overflow against the FreeType line box (`asc/desc`) so glyphs fit their bands — the generator prints these stats.
3. **BFF layout** (what `tools/make_nunito_bff.py` emits; mirrors what `lgfx_fonts.cpp` parses):
   - Records: each = u32 total size + 4-char marker + payload, concatenated: `head`, `cmap`, `loca`, `glyf`.
   - `head`: 36-byte payload — font_size, ascent/descent (from FreeType metrics), bits_per_pixel=4, compression_algorithm=0, and per-master bit-widths for advance / bbox-x / bbox-y / w-h (`awb/xyb/whb`), sized to the max value across glyphs.
   - `cmap`: sparse format 1; subtable header is `<IIHHHBx` (16 bytes); data = **u16** deltas + **u16** gids sorted by delta. u8 packing silently corrupts lookups — this was a real bug.
   - `loca`: nloc+1 u32 offsets, relative to the glyf payload start.
   - `glyf`: one bitstream per glyph: header bits (advance, bbox_x, bbox_y, w, h) followed by 4bpp alpha pixels packed MSB-first **in the same stream** — no byte alignment between header and pixels. `bbox_y` is "distance from bottom of baseline, positive up" (Y-axis convention; sign errors flip glyphs vertically). Keep coverage binary ({0,15}): intermediate alpha gets blended against the black base color in code space and shows as a bright halo on this inverted panel (see Theme 1 fonts section, gotcha #2).
   - **Every glyph region must be padded to a full byte boundary** (`ceil(total_bits/8)`). Dropping trailing zero bytes truncates pixel-less glyphs (space, .notdef) below the header size → `loadGlyphInfo()` rejects them → `drawCharDummy()` draws a box outline in foreground color at every space. Glyphs with pixels survive only because `bit_stream_t::read_bits` clamps out-of-range reads to 0 (transparent). See Theme 1 fonts section for the on-glass symptom ("WIFI|NTP").
4. **Integrate into main.cpp** (pattern at lines ~2019–2075): LGFX's runtime font slot holds only one font (`_runtime_font`), so create one `BFFfont` + `PointerWrapper` per master, `loadFont()` all of them in setup, and draw with the explicit-font overloads: `tft.drawString(s, x, y, &f)` / `drawCenterString`. Measure width with `getDefaultMetric` + `updateFontMetric` (sum of advances); shrink-to-fit = min(1.0, avail/width) via a `bffFit()`-style helper so long strings never overflow their band.
5. **Always keep a fallback**: if any master fails `loadFont()`, render the original bitmap-font layout (`nunitoOK` pattern → `renderTheme1Legacy()`), and log at boot (`[FONT] nunito bff: ok|FAILED...`) so a bad font is visible, not silent.
6. **Verify on host before flashing** (both steps are cheap; flashing is not):
   - `python tools/check_bff_space.py` — mimics `BFFfont` parsing exactly (including the out-of-range→0 clamp) and decodes every glyph of every master against re-rasterized ground truth. Must print `bad glyphs: 0/95`. **Ground truth = rendered ink bbox** (`d.text(...)` + `img.getbbox()`), *not* `font.getbbox()`: for variable fonts the two disagree (a first checker version flagged ~80 false mismatches on em48).
   - `python tools/preview_theme1.py` — renders day/night PNGs replicating firmware placement math exactly: datum top-left, `abs_top = user_y + baseline − (bbox_y + h)`, `abs_left = user_x + bbox_x`, advance = raw px. A correct preview guarantees correct glass rendering.

## SD card PNG pipeline (decode + composite + wiring)

The weather app's dog outfits are 17 RGBA PNGs on the SD card root (selection chart in
`README-WeatherApp.md`). The machinery below is what any CYD project blitting SD-card
PNGs needs (`src/dog_image.cpp`; verified pixel-exact by `test/test_decoder.py`):

- **Decode pipeline** (`src/dog_image.cpp`): PNG chunk walk (IHDR/PLTE/IDAT/IEND)
  → strip 2-byte zlib header → `puff()` inflate → verify Adler-32 → scanline
  unfilter (None/Sub/Up/Average/Paeth) → **natural RGB565 + per-pixel alpha**
  buffers. Color types 0, 2, 3, 6 supported; interlaced PNGs rejected. Images
  larger than the 96x128 corner zone are area-averaged down to fit (all outfits
  are exactly 95x128 → no scaling). Decoded buffers: ~24 KB in RAM.
- **Alpha compositing at draw time** (`compositeRow()`): the outfits have
  transparent backgrounds, so pixels are NOT flattened onto white at decode
  time. `drawDogBox()` pushes one row at a time, and ONLY the opaque span of
  each row: `dogRowSpan(y,&x0,&x1)` returns the first/last column with
  alpha>0, fully transparent rows are skipped, and within a row only
  [x0..x1] is pushed. This is REQUIRED, not an optimization: `compositeRow()`
  emits the background color for alpha=0 pixels, so pushing the full row
  would paint the image's transparent margins (17 px left on the 95×128
  outfits) over whatever is underneath — which is exactly what erased the
  SPLIT theme's chip corners (post-mortem in Display gotchas, 2026-11-15). Pixels
  with alpha>0 are blended against the *expected* background behind the dog
  (`dogBgDisplayed(theme, y)`: the screen background gradient for themes 0–1,
  the right-panel gradient for theme 2 (SPLIT), the solid dog-card color for
  theme 3 (BENTO), all converted to displayed-space 8-bit RGB), then inverted + byte-swapped
  for the panel. The blend math matches PIL's (`(px*a + bg*(255-a))/255` on
  8-bit values expanded from RGB565). Opaque pixels (a=255) pass through
  untouched.
- **SD wiring** (from the CYD MCU schematic): CS=5, SCK=18, MISO=19, MOSI=23.
  The TFT owns VSPI (SPI3), so the SD card gets a dedicated
  `SPIClass sdSpi(HSPI)` instance: `sdSpi.begin(18, 19, 23);` then
  `SD.begin(5, sdSpi, 16000000);` — do NOT use plain `SD.begin()` (it would grab
  the global VSPI and fight the display).
- **Image pixels go through BOTH panel quirks** (this cost a debugging session):
  1. Panel inversion: displayed = ~written, same as all other drawing.
  2. LGFX v1 `pushImage(x,y,w,h,const uint16_t*)` with `_swapBytes=false`
     treats the buffer as **byte-swapped** RGB565 (`swap565_t`) and swaps each
     pixel again before SPI send. A native-RGB565 buffer therefore arrives at the
     panel byte-swapped → garbled pink/green/blue speckles (channels land in the
     wrong bit positions).
  Fix: emit `bswap16(~blended_rgb565)` per row from `compositeRow()` — the library
  swaps it back to native, the panel inverts it, result = the blended color.
  Verified pixel-exact against PIL via `test/test_decoder.py`.
## Theme picker swipe animation: LGFX gotchas

All hit while building the weather app's theme-picker swipe animation
(`animatePickerSwipe()`; feature description in `README-WeatherApp.md`):

- **Blit each card independently.** On the final frame the old card is fully off-screen (`w == 0`); `if (A.w > 0 && B.w > 0)` around the two blits then also skips the new card and leaves a white box in the preview. Use two separate `if (w > 0)` guards.
- **Final frame uses the exact travel distance** (`off = kTravel`), not the eased value — the easing undershoots by 1–2 px and the new card lands off the preview position, leaving a seam.
- **Partial blits must go through `panel()->writeImage()` with a manual `pixelcopy_t`** (`src_bitwidth = 160` = full sprite row width, `src_x` = first visible column). `LGFXBase::pushImage()` overwrites `src_bitwidth` with the sub-rect width, so each row is read with the wrong stride and adjacent pixels shift — visible as garbled pixels along the moving edge. A 16→16-bit copy is a raw row transfer whose byte order matches `push_sprite()` exactly.
- **`writeImage(..., use_dma)` must match the buffer's pool.** Allocation ladder per card: `createSprite()` (DMA-capable pool → row DMA, `use_dma=true`) → `heap_caps_malloc(MALLOC_CAP_8BIT)` + `setBuffer()` (general heap, **not DMA-accessible** → `use_dma=false`, rows CPU-pumped, still smooth). LGFX treats external buffers as "Preallocated", so `deleteSprite()` never frees them — `freeSwipeSprites()` owns their lifecycle. Passing `use_dma=true` for an 8-bit-pool buffer sends garbage (the panel's DMA cannot read that pool).
- **Keep the dog buffers loaded during the bake — no free/reload dance.** Free heap after WiFi/NTP is ~134 KB; two sprites (76.8 KB) + the dog (~36 KB) fit. The original approach freed the dog, allocated the sprites, then reloaded the dog before baking — but that reload peaks at ~100 KB against only ~93 KB of free heap and **always fails**, so every card was baked without the dog. Symptom: "dog missing in every theme" in the picker only (main screen fine, because it renders live). If a sprite can't be allocated at all, the immediate-mode fallback (re-render both themes clipped per frame) still runs, dog included.


## Mockups → glass fidelity
The HTML mockups in `mockups/` are design targets, not pixel promises. Every mismatch class below actually happened; the fixes make future mockups WYSIWYG.

**Why they didn't look right on the CYD:**
1. **Panel inversion (the big one).** The glass displays ~written: write cyan → see red, write white → see black. Porting a mockup's hex values straight into draw calls inverts the whole design. All palette structs store *pre-inverted* code-space values via `C(r,g,b)` = ~rgb565(displayed); the mockup hex is always the *displayed* color (see Display gotchas).
2. **RGB565 quantization.** Browsers render 8-bit sRGB; the panel has 6-5-5 bits per channel, so every non-trivial color shifts slightly (blues most). A mockup that looks right in a browser can look muddy on glass even with correct inversion.
3. **Font mismatch + metrics.** Mockup 1 specified VT323 (pixel font) and the firmware approximated it with LGFX's built-in bitmap font — different glyph shapes/widths, so text didn't sit where the mockup put it; the user then asked for smooth Nunito instead (see Theme 1 fonts section). Browser px sizes ≠ firmware em rasterization, CSS `letter-spacing` has no firmware equivalent (BFF advances are raw), and browser antialiasing ≠ 4bpp alpha. Note: `mockups/mockup1-sky-*.html` still specify VT323 while the implemented theme uses Nunito — update the HTML if it should stay the design source of truth.
4. **Rotation/flip.** Glass is natively 240×320 portrait; the app uses 320×240 landscape via rotation 1 (= rotation 3 flipped 180°). A wrong rotation value gives mirrored/rotated content, not a layout bug.
5. **No framebuffer (immediate mode).** Mockup animations (sun pulse, cloud drift, colon blink, dog bob) can't be full-screen redraws — they flicker. Only partial repaints of exact bounding boxes are possible; dynamic items must live in clearable bands (see Display gotchas + per-second update notes).
6. **AA halos on the inverted panel.** Browser antialiasing blends glyph edges against the *actual* background pixel; LGFX blends intermediate alpha against the base color (black) in code space, and the glass then inverts — so AA text gets a bright halo around every letter instead of smooth edges. Fix: binary-coverage glyphs thresholded at generation time, which never blend (see Theme 1 fonts section, gotcha #2).

**How to make a mockup that will look the same on glass:**
1. Author at exactly **320×240 landscape** — canvas coords = firmware `T::ID` coords, 1:1 with `tft` drawing space.
2. **Quantize the palette to RGB565 first**, then write the mockup CSS using the *expanded* 565 values (pick a color, round each channel to 6/5 bits, use that expanded hex in HTML). Then browser and glass show identical colors; firmware stores `C(r,g,b)` of the same displayed value.
3. Use **the exact font + weight + em sizes** the firmware will rasterize (theme 1: Nunito 700 at 10/13/20/48 px), and avoid CSS letter-spacing (or account for it) so line widths match `bffWidth()`.
4. **Validate with a pixel-exact host preview, not the browser**: replicate firmware parsing + placement math against the actual generated font blobs (`tools/preview_theme1.py` is the reference implementation). A correct preview guarantees correct glass rendering; a pretty browser mockup does not.
5. Keep dynamic items in **clearable bands** so per-second updates can partial-repaint without flicker (mockups are static and can't show this constraint — design for it anyway).

## ArduinoJson gotcha (caused the "night never triggers" bug)
- `variant | true` only accepts JSON **boolean literals**. Open-Meteo returns
  `is_day` as a **number** (0/1), so it always fell back to the default.
  Parse numerics explicitly: `!v.isNull() && (v == 0 || v == 1)`.

