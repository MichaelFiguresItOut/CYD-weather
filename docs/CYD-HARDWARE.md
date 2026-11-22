# CYD (ESP32-2432S032C-I) — Hardware & Library Programming Reference

Board, display, font, SD-card and flashing knowledge for programming the LILYGO CYD,
split out of the original monolithic `README.md`. Companion to `README.md`,
which documents the weather app itself.

## Board: ESP32-2432S032C-I Cheap Yellow Display ("CYD")
- ESP32 dual-core, 4 MB flash, ~4 MB external PSRAM (see "Memory" section
  below), CH340 USB serial → **COM4** @ 115200 baud
  (**USB-C cable into the PC's front USB-C port** — this is the working setup).
  The old USB-A cable into a rear USB-A port enumerated as COM3 but gave flaky
  "port busy / access denied" errors; don't use it. A stale COM3 entry may
  linger in Device Manager (status Unknown) — ignore it.
- **Resetting the chip from the PC — verified behavior (2026-11-27).** The
  CH340's DTR/RTS lines are wired to the auto-reset circuit, but "any port
  open resets the chip" is NOT true on this setup: a plain `serial.Serial()`
  open left the chip running (two 9–12 s captures returned 0 bytes — a reset
  would have produced the boot log). What reliably works is the sequence in
  `test/capture_boot.py`: **DTR low, RTS high (hold ≥ 300 ms), then RTS low**
  → clean NORMAL boot captured from t=0. The older variant (DTR high, RTS
  high → RTS low) produced no boot at all — while DTR is high the chip appears
  held in reset, so a tool that opens the port with DTR asserted could leave
  the board frozen. Download mode is NOT reachable from the PC: DTR is not
  observed to reach BOOT/GPIO0 (cf. Flash procedure below) — the manual
  button dance is the only deterministic way in.
  Consequences: don't casually open the port to peek at logs (it won't reset
  the board, and a half-reset can freeze it); deliberate boot logging goes
  through `test/capture_boot.py`; otherwise inspect a serial log the user
  captures.
- 1.9" ST7789 TFT: native glass **240x320 portrait**, used as **320x240 landscape** (LGFX rotation 1 = rotation 3 flipped 180°)
- GT911 capacitive touch

| Function | GPIO |
|---|---|
| Display CS / RST / DC / SCK / MOSI | 15 / 12 / 2 / 14 / 13 |
| Backlight (PWM) | 27 |
| Touch SDA / SCL / RST | 33 / 32 / 25 |

## Memory: internal RAM vs PSRAM (~4 MB; verify per board)
- This board (ESP32-2432S032C-I) has **~4 MB of external PSRAM** in addition
  to the ESP32's internal SRAM. The verified firmware SDK configuration
  (arduino-esp32 3.3.12 / ESP-IDF v5.5.5, package
  `framework-arduinoespressif32-esp32-libs`, `sdkconfig`) enables it:
  `CONFIG_SPIRAM=y`, `CONFIG_SPIRAM_MODE_QUAD` (Quad SPI mode),
  `CONFIG_SPIRAM_SPEED_80M`, `CONFIG_SPIRAM_USE_MALLOC=y` — so PSRAM is
  available through the ESP-IDF heap-capability allocation APIs. Also
  relevant: `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096` (small allocations
  stay in internal RAM), `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` **not set**
  (Wi-Fi/lwIP buffers come from internal RAM), and
  `CONFIG_SPIRAM_OCCUPY_HSPI_HOST=y` (see "SPI2/HSPI occupancy" below —
  conditional, and NOT active on this board).
  **Capability must be verified for the specific board and build** (hardware
  PSRAM chip + `CONFIG_SPIRAM` + runtime `psramFound()`); do not assume every
  CYD variant has identical memory.
- **SPI2/HSPI occupancy — verified NOT active on this board (2026-11-22):**
  The `OCCUPY_HSPI_HOST` setting is *conditional* in ESP-IDF v5.5.5
  (`components/esp_psram/esp32/esp_psram_impl_quad.c`): the "claim HSPI/SPI2
  to output a solid 80 MHz clock" path runs **only** when the detected PSRAM
  chip is the legacy 32-Mbit "ver0" part (JEDEC EID byte `0x20`) *and* flash
  + PSRAM both run at 80 MHz. Any other chip runs in normal mode, clocked
  from the flash host (SPI1) SCLK — SPI2 is never touched. This board's 4 MB
  PSRAM is **not** that chip, proven empirically: our SD driver
  (`SPIClass sdSpi(HSPI)`) hard-resets the SPI2 peripheral and reroutes its
  SCK in `begin()` (`cores/esp32/esp32-hal-spi.c`) — if the clock hack were
  active, PSRAM would lose its 80 MHz clock and the first decode-buffer
  write (PSRAM, ~18/48 KB, *after* `SD.begin()`) would hang into a watchdog
  reset. The board instead decodes and renders the dog images normally.
  (A PSRAM SPI2 claim would not even block the SD driver — Arduino's SPI
  bypasses `spi_bus_initialize` and the periph-claim check — it would only
  break PSRAM.) Consequences:
  - The SD card on HSPI/SPI2 does **not** conflict with PSRAM on this
    board; `SPIRAM_OCCUPY_HSPI_HOST` is inert here.
  - Latent risk on *other* CYD units only: CYD comes in WROOM (no PSRAM) and
    WROVER (4 MB PSRAM) variants, and the PSRAM part can differ per batch.
    On a unit with the legacy ver0 4 MB chip, this firmware would claim SPI2
    at boot and the SD driver would kill the PSRAM clock after `SD.begin()`
    → the first image load hangs (watchdog reset loop). Fix for such a unit:
    set `SPIRAM_SPEED_40M` in the build (at 40 MHz even ver0 chips share the
    flash host clock, so no SPI2 claim).
- **Allocation approach** (as implemented in `src/dog_image.cpp`):
  - Fresh buffers: `heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`.
  - Dynamically growing buffers: `heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM |
    MALLOC_CAP_8BIT)` — preserves contents, may move the block between the
    PSRAM and internal heaps (use the returned pointer); on failure the
    original block is left unchanged.
  - Safe fallback: a PSRAM-first helper falls back to ordinary
    `malloc()`/`realloc()` when PSRAM is unavailable or the allocation
    fails, so the code also runs on boards without PSRAM.
  - Ordinary `free()` remains valid for buffers from either pool
    (`free()` ≡ `heap_caps_free()`).
- **Current usage:** the PNG decoder's large *transient* buffers — compressed
  IDAT (~18 KB) and inflated raw image data (~48 KB), see "SD card PNG
  pipeline" — are allocated PSRAM-first with internal fallback
  (`psMallocFirst()` / `psReallocFirst()`). The *retained* decoded buffers
  (RGB565 ~24 KB + alpha ~12 KB) and the small row/downscale buffers stay in
  internal RAM. Reason: after Wi-Fi + mbedTLS initialization the internal
  DMA heap's largest contiguous block is only ~63 KB, so the decoder's ~104
  KB peak working set no longer fit there (the 2026-11-22 "dog out of
  memory" diagnosis — README, "Dog outfit images"). PSRAM absorbs the
  transient buffers and the internal peak drops to ~37 KB.
- **Guideline for future code:** internal RAM is the scarce, fast resource —
  Wi-Fi, networking, DMA and other time-sensitive allocations must keep
  working there; PSRAM is slower than internal RAM, so allocation placement
  should reflect the workload and hardware requirements: put large,
  short-lived, sequentially accessed buffers in PSRAM when that relieves
  internal-heap headroom, and keep latency-sensitive or DMA-touching buffers
  in internal RAM.

## WiFi radio: scanning (cost real debugging time — read before touching `wifi_conn.cpp`)
- **The ESP32 is 2.4 GHz only.** Phones enumerate 2.4 + 5 (+6) GHz and merge
  duplicates, so a phone's "15 networks" is not a number this chip can ever
  match — part of any scan-count gap is physics, not a bug.
- **The default active scan is unreliable.** Bare `WiFi.scanNetworks()` is an
  *active* probe pass of only ~100–300 ms per channel (framework `WiFiScan.cpp`,
  arduino-esp32 3.x): the radio sends probe requests and listens briefly on
  each channel; APs that miss that window (weak signal, busy, or ignoring
  probes) never appear — the classic "0–3 networks, never even the current
  one" symptom (verified on this board 2026-11-18; a phone in the same spot
  showed 15). A reliable scan is *passive* (listen for beacons):
  `WiFi.scanNetworks(false, true, true, 400, 0)` = blocking, show-hidden,
  passive, **400 ms dwell per channel, all channels (~6 s total)**. That is
  what `wifiScanNetworks()` does; the parameter set is load-bearing — don't
  "simplify" it back to the no-arg call.
- **Scan only when the radio is fully idle.** While the station is connected
  — or still tearing the link down — the scan gets truncated. So
  `wifiScanNetworks()`: `setAutoReconnect(false)` + `disconnect()`, then
  **poll until `WL_DISCONNECTED`** (a blind delay is not enough), 300 ms RF
  settle, then scan. The device is offline for the ~6 s scan; the scan flow
  in `settings.cpp` restores the link itself once the list is shown (footer
  `RECONNECTING...`) — the `loop()` 10 s check is paused while Settings is
  open, so the scan flow owns the reconnect. This is by design: it happens
  only inside the Wi-Fi settings flow.
- **A just-deauthenticated AP can be a passive-scan miss** (sometimes absent
  from the raw results) — so the current network may not appear in the SCAN
  list right after a scan; it is connected regardless (the scan flow
  reconnects to the previous credentials). Scan diagnostics go to Serial:
  `[WIFI] Scan: N raw, M below -90 dBm dropped, K stored`.
- **Modem sleep off around the scan.** `WiFi.setSleep(false)` — power-save
  delays scan events. It stays off afterwards (plugged-in display; the mA
  cost is negligible).

## Display gotchas (cost real debugging time — read before touching UI code)

- **The panel displays INVERTED colors: displayed = ~written.** Verified against
  photos (write cyan 0x07FF → see red; write yellow 0xFFE0 → see blue).
  To display white, write `TFT_BLACK`; to display black, write `TFT_WHITE`.
  All palette choices in the firmware (per-theme palettes in `src/render.cpp`) are made for the *displayed* result.

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
- **An edge that looks "cut off" on glass but correct in preview is being
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
  it appeared only when the chips were widened to 213 px (ending at x=225). Fix: `dogRowSpan()` (first/last alpha>0 column
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

Theme 1's text is drawn with **Nunito** (weight 700) at six fixed em sizes — 9/10/12/13/20/48 px — stored in flash as compact BFF blobs (`src/nunito_bff.h`, ~60 KB total). Coverage is **binary** (alpha thresholded to {0,15} at generation time — see gotcha #2 below). Theme 2 (SPLIT) reuses the same masters, but draws **small text at scale 1.0 in the master closest to the design pixel size** (9px → em9, 9.5px → em10, 10.5px → em10, 11.5px → em12, 12px → em12) — see gotcha #4, why sub-1.0 scaling is forbidden for thin-stroke fonts. Only the large thick-stroke glyphs keep fractional scaling (clock em48 @ ≤37/48, temp em48 @ ≤43/48, unit em20 @ 0.95, FEELS value em20 @ ≤0.9). Theme 3 (BENTO) likewise reuses the masters at 1.0 for its small text (status/detail em10, date em13, temp unit em20 @ 0.75) and keeps fractional scaling only on the thick em48 clock (≤27/48) and temp (34/48). Theme 0 (BASIC) still uses LGFX's built-in bitmap font.

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
Theme 2 (SPLIT) was originally drawn at the design's exact fractional sizes (date em13 @ 10.5/13 ≈ 0.808, status em10 @ 0.95, row1/hint em10 @ 0.85, desc em13 @ 12/13, rain em13 @ 11.5/13). On glass the small lines came out garbled: 'Sunday 15 November 2026' read like "Snday I5 Mvember 2026", 'HUM 65%...' like "HM 65...", with strokes vanishing and neighbors merging. The font blobs were proven byte-correct (host decode matches Pillow rasterization), and the host preview at the same scales was crisp — so it was the runtime scaler. In `lgfx_fonts.cpp`, `BFFfont::drawChar()` maps every source pixel through `draw_alpha_bitmap_common()` even at scale 1.0, and that function is a **nearest-neighbor scaler with integer floor mapping**: dest pixel x samples source column `⌊x/scale⌋`. When scale < 1, consecutive destination columns share source columns, and some source columns map to *zero* destination columns and are simply dropped. At 0.808, 1-px-wide strokes (the right stem of 'H', the 'l' in 'November', the '1' in '15') land on a dropped column and vanish. Reproduced exactly on the host by simulating the same floor mapping (thin strokes disappear at the same positions as the photo). The reason 1.0 looks perfect and 0.808 garbles is that at sx = 65536 the mapping is the identity. Note the same decimation happens for *any* BFF text at a fractional scale — even the large clock (em48 @ 37/48) is slightly degraded, it's just invisible on 3-4 px thick strokes.
Fix (2026-11-15): added em9/em12 masters to `make_nunito_bff.py`'s `MASTERS` and regenerated `src/nunito_bff.h`; Theme 2 now draws every thin-stroke line at **scale 1.0 in the nearest master** (status/date → em10, row1/hint/FEELS label → em9, desc/rain → em12) and only the big thick glyphs keep fractional scaling. Sizes land within 0.5 px of the design sizes. Rule: for BFF fonts, either use a native-size master at 1.0, or only accept fractional scales on ≥20 px fonts where stroke width survives the decimation.

## Fonts (BFF): adding a new one
All custom text on the CYD goes through LovyanGFX's **BFF** format (4bpp alpha, no compression). Themes 1, 2 and 3 use generated Nunito masters; theme 0 (BASIC) uses LGFX's built-in bitmap font (`glyphH(size)` = 8×size, fixed-pitch 6×8 px per size unit). To add another custom font, follow the exact workflow that produced `src/nunito_bff.h` — every step below exists because something broke when it was skipped.

**Pipeline**: TTF (variable or static) → Pillow rasterize at chosen em sizes → BFF blob per master → `static const uint8_t` arrays in a generated header → one `lgfx::BFFfont` + `PointerWrapper` per master (in the firmware, `src/render.cpp`) → draw with the explicit-font overloads.

1. **Pick the font file and weight.** Variable fonts: load once, then `font.set_variation_by_axes([700])` before any rasterization or metric call (weight 700). Keep the TTF under `tools/fonts/`.
2. **Pick em sizes per layout band** (the set is 9/10/12/13/20/48 px — one master per design text size; theme 2 draws at 1.0 in the nearest master, see gotcha #4). Blob size scales with em² — em48 is ~36.9 KB of the ~59.6 KB total for six masters. Rasterize each glyph with an explicit anchor — `d.text((PX,PY), ch, font=f, fill=255, anchor="ls")` (left-baseline) at a fixed offset inside a padded canvas; ink bbox = `img.getbbox()`. Check top/bottom overflow against the FreeType line box (`asc/desc`) so glyphs fit their bands — the generator prints these stats.
3. **BFF layout** (what `tools/make_nunito_bff.py` emits; mirrors what `lgfx_fonts.cpp` parses):
   - Records: each = u32 total size + 4-char marker + payload, concatenated: `head`, `cmap`, `loca`, `glyf`.
   - `head`: 36-byte payload — font_size, ascent/descent (from FreeType metrics), bits_per_pixel=4, compression_algorithm=0, and per-master bit-widths for advance / bbox-x / bbox-y / w-h (`awb/xyb/whb`), sized to the max value across glyphs.
   - `cmap`: sparse format 1; subtable header is `<IIHHHBx` (16 bytes); data = **u16** deltas + **u16** gids sorted by delta. u8 packing silently corrupts lookups — this was a real bug.
   - `loca`: nloc+1 u32 offsets, relative to the glyf payload start.
   - `glyf`: one bitstream per glyph: header bits (advance, bbox_x, bbox_y, w, h) followed by 4bpp alpha pixels packed MSB-first **in the same stream** — no byte alignment between header and pixels. `bbox_y` is "distance from bottom of baseline, positive up" (Y-axis convention; sign errors flip glyphs vertically). Keep coverage binary ({0,15}): intermediate alpha gets blended against the black base color in code space and shows as a bright halo on this inverted panel (see Theme 1 fonts section, gotcha #2).
   - **Every glyph region must be padded to a full byte boundary** (`ceil(total_bits/8)`). Dropping trailing zero bytes truncates pixel-less glyphs (space, .notdef) below the header size → `loadGlyphInfo()` rejects them → `drawCharDummy()` draws a box outline in foreground color at every space. Glyphs with pixels survive only because `bit_stream_t::read_bits` clamps out-of-range reads to 0 (transparent). See Theme 1 fonts section for the on-glass symptom ("WIFI|NTP").
4. **Integrate into the render module** (firmware pattern: the `Nunito BFF fonts` block in `src/render.cpp`, `static lgfx::BFFfont nunitoF*`): LGFX's runtime font slot holds only one font (`_runtime_font`), so create one `BFFfont` + `PointerWrapper` per master, `loadFont()` all of them in setup, and draw with the explicit-font overloads: `tft.drawString(s, x, y, &f)` / `drawCenterString`. Measure width with `getDefaultMetric` + `updateFontMetric` (sum of advances); shrink-to-fit = min(1.0, avail/width) via a `bffFit()`-style helper so long strings never overflow their band.
5. **Always keep a fallback**: if any master fails `loadFont()`, render the original bitmap-font layout (`nunitoOK` pattern → `renderTheme1Legacy()`), and log at boot (`[FONT] nunito bff: ok|FAILED...`) so a bad font is visible, not silent.
6. **Verify on host before flashing** (both steps are cheap; flashing is not):
   - `python tools/check_bff_space.py` — mimics `BFFfont` parsing exactly (including the out-of-range→0 clamp) and decodes every glyph of every master against re-rasterized ground truth. Must print `bad glyphs: 0/95`. **Ground truth = rendered ink bbox** (`d.text(...)` + `img.getbbox()`), *not* `font.getbbox()`: for variable fonts the two disagree (a first checker version flagged ~80 false mismatches on em48).
   - `python tools/preview_theme1.py` — renders day/night PNGs replicating firmware placement math exactly: datum top-left, `abs_top = user_y + baseline − (bbox_y + h)`, `abs_left = user_x + bbox_x`, advance = raw px. A correct preview guarantees correct glass rendering.

## SD card PNG pipeline (decode + composite + wiring)

The weather app's dog outfits are 17 RGBA PNGs on the SD card root (selection chart in
`README.md`). The machinery below is what any CYD project blitting SD-card
PNGs needs (`src/dog_image.cpp`; verified pixel-exact by `test/test_decoder.py`):

- **Decode pipeline** (`src/dog_image.cpp`): PNG chunk walk (IHDR/PLTE/IDAT/IEND)
  → strip 2-byte zlib header → `puff()` inflate → verify Adler-32 → scanline
  unfilter (None/Sub/Up/Average/Paeth) → **natural RGB565 + per-pixel alpha**
  buffers. Color types 0, 2, 3, 6 supported; interlaced PNGs rejected. Images
  larger than the 96x128 corner zone are area-averaged down to fit (all outfits
  are exactly 95x128 → no scaling). Memory placement (2026-11-22): retained
  buffers (natural RGB565 ~24 KB + alpha ~12 KB) live in internal RAM; the
  transient decode buffers (compressed `idat` ~18 KB, inflated `raw` ~48 KB)
  are allocated PSRAM-first with internal fallback — see "Memory".
- **Chunk-type comparison — hard-learned (2026-11-20, cost a whole debug session)**:
  PNG chunk types are fixed 4-byte fields, NOT C strings. The decoder must use
  `memcmp(type, "IHDR", 4)` etc. The decoder had carried
  `strcmp((const char*)&hdr[4], ...)` since the code was ported from the GitHub
  project: that reads PAST the 8-byte `hdr`
  buffer (undefined behavior) and only stops at a coincidental NUL. On the
  2026-11-20 build every comparison returned non-zero, so IEND was never
  recognized and every load failed with "[DOG] no IEND" — while the files were
  valid and the card worked. The failure was misread as marginal SD contact and
  then as compiler codegen: the walk's exit check `if (!ok || !iend)` was
  disassembled twice and was correct both times (the compiler was innocent). A
  one-line switch to `memcmp` fixed it. Two takeaways: (1) fixed-length binary
  fields are never compared as C strings; (2) before blaming the compiler, look
  at the actual instructions in `firmware.elf` — a misdiagnosed codegen bug
  burned a day, the real bug was a textbook UB read.
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
  the global VSPI and fight the display). SPI clock is 16 MHz; verified on
  2026-11-20 (SD init + all dog image loads succeed). History: on 2026-11-19
  mid-file reads failed systematically at 16 MHz (init OK, first ~1KB OK, then
  EOF on every file); a 4 MHz "extra margin" experiment did not fix it and
  correlated with failing boots, so 16 MHz was kept. The failures look like
  marginal card contact rather than clock rate — the 3× `SD.begin()` / 3× load
  retries in `weatherInitSdAndDog()` are the mitigation. SPI-host
  compatibility verified 2026-11-22: PSRAM does **not** occupy SPI2 on this
  board (it is not the legacy EID-`0x20` 32-Mbit chip that would trigger the
  80+80 MHz occupancy), so the SD card's use of HSPI is conflict-free — see
  "Memory" ("SPI2/HSPI occupancy").
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
(`animatePickerSwipe()`; feature description in `README.md`):

- **Blit each card independently.** On the final frame the old card is fully off-screen (`w == 0`); `if (A.w > 0 && B.w > 0)` around the two blits then also skips the new card and leaves a white box in the preview. Use two separate `if (w > 0)` guards.
- **Final frame uses the exact travel distance** (`off = kTravel`), not the eased value — the easing undershoots by 1–2 px and the new card lands off the preview position, leaving a seam.
- **Partial blits must go through `panel()->writeImage()` with a manual `pixelcopy_t`** (`src_bitwidth = 160` = full sprite row width, `src_x` = first visible column). `LGFXBase::pushImage()` overwrites `src_bitwidth` with the sub-rect width, so each row is read with the wrong stride and adjacent pixels shift — visible as garbled pixels along the moving edge. A 16→16-bit copy is a raw row transfer whose byte order matches `push_sprite()` exactly.
- **`writeImage(..., use_dma)` must match the buffer's pool.** Allocation ladder per card: `createSprite()` (DMA-capable pool → row DMA, `use_dma=true`) → `heap_caps_malloc(MALLOC_CAP_8BIT)` + `setBuffer()` (general heap, **not DMA-accessible** → `use_dma=false`, rows CPU-pumped, still smooth). LGFX treats external buffers as "Preallocated", so `deleteSprite()` never frees them — `freeSwipeSprites()` owns their lifecycle. Passing `use_dma=true` for an 8-bit-pool buffer sends garbage (the panel's DMA cannot read that pool).
- **Keep the dog buffers loaded during the bake — no free/reload dance.** Free heap after WiFi/NTP is ~134 KB; two sprites (76.8 KB) + the dog (~36 KB) fit. The original approach freed the dog, allocated the sprites, then reloaded the dog before baking — but that reload peaks at ~100 KB against only ~93 KB of free heap and **always fails**, so every card was baked without the dog. Symptom: "dog missing in every theme" in the picker only (main screen fine, because it renders live). If a sprite can't be allocated at all, the immediate-mode fallback (re-render both themes clipped per frame) still runs, dog included.

## ArduinoJson gotcha (caused the "night never triggers" bug)
- `variant | true` only accepts JSON **boolean literals**. Open-Meteo returns
  `is_day` as a **number** (0/1), so it always fell back to the default.
  Parse numerics explicitly: `!v.isNull() && (v == 0 || v == 1)`.

## Flash procedure — ALWAYS these two steps, same every time
There is no shorter or longer variant; always do both, in this order:
1. **Download mode FIRST:** on the back of the CYD, **hold BOOT** (the button
   further in), **hit RESET** (button closer to the edge), **release BOOT**.
   Screen freezes/goes blank — normal; the ROM bootloader waits for data
   indefinitely, so there is no rush between steps 1 and 2.
2. In the project root: `pio run -t upload`

Why both every time: `upload_port = COM4` and
`board_upload.before_reset = no-reset` are set in platformio.ini, so esptool
does NOT put the chip into download mode itself. Plain `pio run -t upload`
without step 1 is unreliable — it has sometimes connected and other times
failed with "No serial data received" or "Wrong boot mode detected (0x13)".
The button dance makes it deterministic.

`test/enter_download_mode.py` was tried as a no-buttons alternative (2026-11-26):
after its DTR/RTS sequence, `pio run -t upload` still failed with "No serial
data received" — the script does NOT put this board into download mode (the auto-reset
circuit reliably provides reset, but DTR is not observed to reach BOOT/GPIO0).
Don't rely on it; the button dance is the only deterministic way in.
