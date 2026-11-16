# CYD Weather Station Dashboard — Reference

Standalone ESP32 weather/clock dashboard. This file is the shared reference for
future work: read it first in any new thread on this project.

## Files
- `src/main.cpp` — firmware: UI layout, weather fetch/parse, rendering pipeline
- `src/nunito_bff.h` — generated Nunito BFF font blobs (masters em9/10/12/13/20/48) shared by themes 1 (SKY), 2 (SPLIT) and 3 (BENTO); regenerate with `tools/make_nunito_bff.py`, verify with `tools/check_bff_space.py`
- `src/dog_image.{h,cpp}` — SD-card PNG decoder; keeps natural RGB565 + alpha, composites per row at draw time (see Dog outfit images section)
- `lib/puff/` — vendored Mark Adler `puff.c/h` (public-domain minimal DEFLATE inflater; the core's zlib is not linked into the app)
- `test/test_decoder.py` — host-side validation of the decode + composite pipeline against PIL for all 17 outfit PNGs (`python test/test_decoder.py`; ~206k px checked)
- `test/capture_boot.py` / `test/enter_download_mode.py` — pyserial helpers over COM4: reset + capture boot log / force download mode (the force-download-mode path does NOT work on this board — use the manual button dance, see Build & flash)
- `tools/make_nunito_bff.py` — rasterizes Nunito (weight 700) at em sizes 10/13/20/48 into BFF blobs → rewrites `src/nunito_bff.h` (needs Pillow; font source `tools/fonts/Nunito-wght.ttf`)
- `tools/check_bff_space.py` — host-side verification: mimics `BFFfont` parsing exactly, decodes every glyph of every master against re-rasterized ground truth (`bad glyphs: 0/95` per master = good; exit code 1 on any mismatch)
- `tools/preview_theme1.py` — renders theme 1 day/night mockups to `preview_theme1_{day,night}.png`, replicating the firmware's BFF parsing + placement math exactly
- `tools/preview_theme3.py` — same for theme 2 (SPLIT; file named after mockup3), including LGFX's fixed-point fractional-scale glyph drawing (`python tools/preview_theme3.py [dog.png]`)
- `tools/preview_theme4.py` — same for theme 3 (BENTO; file named after mockup4): mirrors `renderTheme3Nunito()` line for line (RGB565-exact palette, same fit math, procedural sun/moon/cloud/umbrella icons) → `preview_theme4_{day,night}.png`. Its `PAL` dicts duplicate `bentoDay`/`bentoNight` — update both together
- `mockups/` — HTML design targets per theme (day + night each); see Mockups → glass fidelity section before trusting them pixel-for-pixel
- `platformio.ini` — build config (board, partitions, deps)
- SD card root: 17 outfit PNGs from `dogs/` (all 95x128 RGBA with transparency — copy them to the card root, see Dog outfit images section); they live on the card, not in flash

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

## Dog outfit images (SD card PNGs)
Golden retriever in the bottom-right corner, wearing an outfit that matches the
current weather. One of 17 RGBA PNGs on the SD card root is selected by
`selectDogImage()` in main.cpp and reloaded whenever the selection changes.

**Selection chart** (feels-like temperature in °F; rain overrides everything):
| Condition | File |
|---|---|
| Raining now (`wx.rainingNow`) | `rain_now.png` |
| Rain expected later / RAIN NEARBY, not raining now | `rain_later.png` |
| Feels < 30 F | `extreme_cold[_sun].png` |
| 31–45 F | `very_cold[_sun].png` |
| 46–55 F | `cold[_sun].png` |
| 56–65 F | `cool[_sun].png` |
| 66–70 F | `mild_cool[_sun].png` |
| 71–75 F | `mild_warm[_sun].png` |
| ≥ 76 F | `hot[_sun].png` |
| No usable weather data / load failure | `error.png` |

- **`_sun` variants** are used when it is known daytime AND clear or mainly
  clear: a fresh KJRB METAR sky group wins (`SKC`/`CLR` = clear, `FEW` =
  mainly clear; anything denser → no sun), otherwise the Open-Meteo WMO code
  (0/1 = sunny). Night or unknown day/night never gets `_sun`. The determination
  is logged in the `[WX] ... sun=0|1` line.
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
- **Layout zone**: image drawn at `(width - w - 4, height - h - 4)`; full-size
  95x128 → corner (221,108)-(315,235). `textZoneRight()` = left edge of the image
  (full width if no image loaded); **every line of the weather block is centered
  within [0, textZoneRight())**, not full screen. Per-second partial updates only
  repaint rows [16,70), so they never touch the image; an outfit swap rides along
  with the next full render (fetch/touch/NTP-sync).
- **Reload policy** (`refreshDogImage()`): called after boot SD init (no weather
  yet → `error.png`), after every successful fetch, and on touch refresh. It only
  reloads when the selected filename changed; loads retry up to 3× (marginal SD
  contact) and fall back to `error.png` if the wanted file fails, so a bad state
  is visible instead of silently showing an old outfit.
- **Boot log**: `[SD] card ready` then `[DOG] loaded /<name>.png: WxH px, N KB buffers`
  on every (re)load. Missing card prints `[SD] ...` and the UI falls back to
  full-width layout (no crash); a missing file prints `[DOG] <file> not found on SD card`.
- **Transient SD failures**: if the dog is missing but everything else renders,
  check serial for `[SD]`/`[DOG]` lines first (card seating/contact was one such
  case — a plain reboot fixed it; `test/capture_boot.py` resets + captures).
  Boot now retries `SD.begin()` and `loadDogImage()` up to 3 times each (~1.4 s
  worst case), so a marginal contact usually recovers without a manual reboot.
  Flashing itself never touches the card (esptool writes only ESP32 flash; the
  firmware is read-only on SD) — a missing dog after flashing is always this
  transient boot failure, not file loss.

## Firmware: display items → where they live (all in src/main.cpp)
| Item | Function / notes |
|---|---|
| "WIFI NTP" top-left | `drawStatusBar()`; shows `--OFF--` / ` --NTP` when down |
| **Day/Night indicator** top-right | `drawStatusBar()`: `Day` (is_day=1 valid), `Night` (is_day=0 valid), `Day/Night ERROR` (missing/invalid/unavailable). Independent verification of is_day parsing. |
| Big clock H:MM AM/PM (no seconds) | `drawClockAndDate()` size 4, y=18; partial update in `updateDynamicTexts()` repaints only on minute change, box rows [16,50). Times default to **US 12-hour** via `formatUS12()` (no leading zero) – tap toggles 12h/24h (`use24Hour`, see Touch targets); the clock's repaint max-width string is `"12:59 PM"` / `"23:59"` per mode (`clockMaxString()`) |
| Date line "Thursday 12 November 2026" | size 2, y≈52. **No comma after weekday** in format 0 (keeps it short). Band rows [50,70) wiped only when date string changes. **Tap cycles** to US-style with the weekday kept: "Thursday November 12, 2026" – lines longer than the screen at size 2 are scaled down slightly by `dateLineFit()` so they always fit (see Touch targets) |
| Temperature "50.1 F" | `drawWeatherPanel()` size 5, y=96, **centered in text zone [0, textZoneRight())** (not full width — see Dog image). Fahrenheit from API (`temperature_unit=fahrenheit`). **Tap toggles F/C** – all displayed temps convert together via `tempForDisplay()`, wx values stay in °F (see Touch targets) |
| Description ("Overcast") | KJRB METAR-derived short text (`metarText()`, phrasing mirrors `wmoText()`); Open-Meteo WMO fallback when METAR is missing/stale |
| **Rain status line** | `drawWeatherPanel()` size 1, y≈204: `RAIN NOW - MAY CONTINUE` (NBM cross-check sees precip past the RRFS end — see NBM cross-check) / `RAIN NOW - ends ~H:MM AM/PM` / `RAIN NOW` (end unknown) / `RAIN NEARBY (LGA/EWR)` / `Rain expected H:MM [tomorrow]` (RRFS 15-min source) / `Rain possible H:MM [tomorrow]` (hourly-model-only source) / `No rain expected`. `RAIN NOW` from fresh KJRB METAR precip codes (`metarHasRain()`, obs ≤ 90 min; Open-Meteo `current.rain`/WMO fallback when no fresh KJRB obs). The end time comes from the RRFS 15-min timeline and is shown **only** when the data shows a subsequent dry stretch — rain running to the edge of the forecast window leaves it unknown (plain `RAIN NOW`); if the NBM cross-check still sees precip after that end, the definitive time is suppressed in favor of `MAY CONTINUE`. `RAIN NEARBY` = fresh precip at KLGA/KEWR while KJRB reports dry. Next-rain from `analyzeRainTimeline()`: RRFS 15-min periods first, hourly model beyond that window; **needs NTP** — if time is unavailable at fetch the line shows "No rain expected" until the next refresh (boot waits for sync first; see Refresh). While raining now/nearby, future-only messages are suppressed. |
| "Feels X F   Hum Y%" / "Today H .. L .." | `drawWeatherPanel()`, size 1 lines (size 2 was too wide for the text zone), centered in text zone |
| "Updated H:MM:SS AM/PM" | static timestamp of last successful fetch (local time), not a countdown; centered in text zone |
| **Dog outfit image bottom-right** | `drawDogImage()` composites the loaded outfit row-by-row against the current background and blits it (`src/dog_image.cpp`); selection in `selectDogImage()`, reloads via `refreshDogImage()`; drawn once per full render, never touched by partial updates. See Dog outfit images section |
| "touch: refresh" bottom hint | static label only – actual behavior per the Touch targets section below |

### Touch targets (tap = touch release; 2 s debounce between actions)
Generous fixed rectangles in landscape canvas coords (320x240), checked in this order in `handleTouch()`:

| Tap target | Zone | Action |
|---|---|---|
| Dog corner zone | x ≥ width−96, y ≥ height−128 (the reserved 96×128 bottom-right corner); in BENTO theme the whole right column (x ≥ 206, y ≥ 66) is the dog card and opens it too | opens the **THEME picker**: swipe left/right to browse themes, tap to select (see Themes below) |
| Clock band | y ∈ [12,50), full width | toggles clock between US 12-hour ("H:MM AM/PM") and 24-hour ("HH:MM") – `use24Hour` |
| Date band | y ∈ [50,72), full width | cycles date line "Thursday 12 November 2026" ↔ "Thursday November 12, 2026" (weekday kept in place) – `dateFormat`. Longest case ("Wednesday September 30, 2026", 336 px at size 2 on this fixed-pitch font) is scaled down slightly by `dateLineFit()` so the line always fits |
| Temperature band | x < width−96, y ∈ [92,138) | toggles all displayed temperatures (current / feels-like / today H-L) between °F and °C – `useCelsius`. wx values stay in °F internally; conversion happens at draw time only (`tempForDisplay()`) |
| Anywhere else | — | immediate weather refetch + full render (original behavior) |

- Toggles are runtime state only (`use24Hour`, `dateFormat`, `useCelsius`); not persisted across reboot.
- Each toggle does a full `renderScreen()`; the per-second partial updates share the same format helpers (`formatClock()`, `formatDateLine()`) and `drawClockAndDate()` keeps `lastClockBuf/lastDateBuf` in sync, so no ghosting or redundant repaints after a toggle.
- While the THEME picker is up, `loop()` suppresses all main-display redraws (status changes, periodic refresh render, per-second partial updates) so it isn't clobbered; data fetching and dog-image reload still run normally. Picker gestures are evaluated on finger lift (`SwipeState` in `handleTouch()`): horizontal movement > 24 px (and more than twice the vertical) = **swipe** → next/previous theme (wraps around); a short (< 600 ms), small-movement press = **tap** → selects the previewed theme, saves it to NVS and returns to the main display. Ambiguous gestures are ignored.
- The clock/date/temp bands above follow the active theme (`touchBands()`): theme 0 derives from `gLayout`, themes 1–3 use fixed bands around their mockup positions – so tapping where an item is drawn toggles it on every skin.

### Themes (display skins)
Four themes (`THEME_COUNT` = 4), each with automatic day/night variants driven by `nightMode()`:

| # | Name | Layout |
|---|------|--------|
| 0 | BASIC | the original layout – all positions from `gLayout`, background via `fillBackground()` |
| 1 | SKY | full-screen sky gradient (3 stops), sun + clouds / moon + stars, status row, big centered clock, temperature left, description with mini cloud, FEELS and RAIN chips, dog in the corner (`renderTheme1()`); text is generated Nunito BFF fonts (see Theme 1 fonts section) with bitmap-font fallback |
| 2 | SPLIT | solid data zone on the left [0,214); right sky panel x ≥ 214 with flat sun / crescent moon + stars, two stationary mockup-style clouds, DAY/NIGHT label and ground ellipse; text in the left zone, FEELS/RAIN chips span x 12..225 (213 px mockup content-box, so they cross the 214 seam by design — nothing must overpaint the 221..225 corner zone, see the dog-margin gotcha in Display gotchas), dog in the corner; Nunito BFF fonts like theme 1 (`renderTheme2()`) |
| 3 | BENTO | rounded card grid: clock card on top (time + date + WIFI/DAY row), temperature card with accent bar + FEELS chip + description row, RAIN card, detail card (HUMIDITY / TODAY H-L / UPD-REFRESH) and a tall dog card on the right (`renderTheme3()`); text is Nunito BFF with bitmap-font fallback (see Theme 3 text below) |

- All renderers draw through one `struct T` transform (scale + offset): the live display uses `T::ID`, and the picker renders the previewed theme at half scale into a 160×120 window via `setClipRect()` – so previews are true scaled-down versions of what a selection will show. The picker's swipe animation bakes the same half-scale render into sprites – see the Theme picker section below.
- Colors: the mockups specify *displayed* colors; this panel inverts every pixel, so all palette values are stored pre-inverted (`C(r,g,b)` = ~rgb565(displayed)). Per-theme palettes: `SkyPal`, `CardsPal`, `SplitPal`, `BentoPal` (day/night instances) plus per-row background gradients via `bgDisplayed()` / `bgCode()` and `drawThemeBg()`.
- **Persistence**: the selected theme is saved to NVS (namespace `cydwx`, key `theme`) on selection (`saveTheme()`) and restored at boot before the first render (`loadTheme()` in `setup()`). On load, legacy/out-of-range indices are migrated one time (NVS key `themeVer` = 2 marks a migrated write): the removed CARDS theme (old index 2) falls back to BASIC (0); old SPLIT (3) → 2 and old BENTO (4) → 3.
- Per-second partial updates are theme-aware: BASIC repaints only its clock/date bands; SKY clears + redraws the status/clock/date band (including sky decorations); SPLIT clears + redraws only the left-zone clock/date band [10..214]×[22..78) via `drawSplitClockDate()`; BENTO clears + redraws only the clock card (8,8,304,52) via `drawBentoClockCard()` — the card's own background is part of the redraw, so the clock tick never needs a full re-render (no flicker, no flash).
- **Theme 2 (SPLIT) FEELS chip "second F" (seen on glass, 2026-11-15).** `buildTempStr()` = `"%.1f %s"` — the unit char is **already appended** to temp strings (`"32.7 F"`, `"--.-"` when invalid). The SPLIT FEELS chip then appended `tempUnitChar()` a second time, drawing `"32.7 FF"` on glass. Every other theme draws the `tempOrDash()`/`buildTempStr()` result as-is. Rule: never append `tempUnitChar()` to a string produced by those helpers. Note the app-wide convention is value + space + unit char (no degree sign), so glass shows `32.7 F` where the mockups say `32.7°F` — intentional, and the host previews mirror the firmware string (`"32.7 F"`). The one exception is theme 3 (BENTO)'s TODAY H-L row, which appends `\xB0` (°) on purpose because mockup4 specifies degree signs — `°` is in the BFF cmap (verified present before use).

**Theme 3 (BENTO) text: Nunito BFF (rebuilt 2026-11-26).** `renderTheme3()` dispatches on `nunitoOK`: Nunito path `renderTheme3Nunito()`, fallback `renderTheme3Legacy()` (bitmap font, boot log `[FONT] ...`). Geometry is taken 1:1 from `mockup4-bento-day.html` (the mockup CSS class names are the layout source of truth — read the CSS, not the screenshot): clock card (8,8,304,52, r10) = 27 px clock (em48 @ ≤27/48, 320 px cap) + 10 px date (em10) + em10 status row (2.5 px dot + WIFI NTP left; sun/moon + DAY-NIGHT right); temp card (8,66,192,72) = 3.5 px accent bar, 34 px temp (em48 @ 34/48) + em20 @ 0.75 unit, 10 px FEELS chip (label + em10 value + crossed-arrows icon = three `drawLine` lw1 in a 9 px box), description row (10 px cloud + text, or "--" + "no data"); rain card (8,144,192,34) = 13 px umbrella/dot + 10 px label, whole group centered; detail card (8,184,192,48) = three 10 px rows HUMIDITY / TODAY H-L / UPDATED; dog card (206,66,106,166) = outfit centered in the 4/8 px margins + em10 caption. All procedural icons: day sun = fillCircle r3 + 8 lw1 rays at r5; crescent = fillCircle r4 with an offset r3 "bite" filled with the **card's** background color — the bite must match the surface the icon sits on (the white card, NOT the screen background) or a ring of the wrong color shows on the card. Palettes `bentoDay`/`bentoNight` mirror the mockup's displayed hexes (same `C()` 565 quantization as everything else — glass shows the quantized values). Preview: `tools/preview_theme4.py` mirrors the renderer line for line and its output matched the mockup4 screenshots (day + night). Per-minute update: `clearThemed(THEME_BENTO, t, 8, 8, 304, 52)` + `drawBentoClockCard()` — nothing else repaints.

### Theme 1 (SKY) text: generated Nunito BFF fonts
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

### Theme picker: static preview + swipe animation
Tap the dog corner to open the picker (`themeSelectScreen`). It shows a true half-scale preview (160×120 window at (80,60)) of `previewTheme` inside a yellow border on the normal background, plus chrome (`drawPickerChrome()`): green top bar "THEME n/4: NAME" (n is 1-based for the user; both numbers come from the format string, so they track `THEME_COUNT` automatically), a "SWIPE LEFT/RIGHT - TAP TO SELECT" hint, and a green bottom bar with the neighbours' names ("< PREV" / "NEXT >"). Swipe left/right = next/prev theme (wraps); tap = select, save to NVS, return to the main display. `previewTheme` starts at `activeTheme` when the picker opens and is updated at the *start* of each swipe (so a follow-up tap or swipe acts on the new theme immediately).

**Swipe animation (`animatePickerSwipe()`): full-screen slide, sprite-baked cards.** The old card slides completely off the left edge while the new card enters from the right (mirrored for prev) — `kTravel` = 240 px, 8 frames, easeOutQuad, ~25 ms blit + 15 ms delay per frame ≈ 320 ms total. Redrawing both themes in immediate mode every frame takes ~50–120 ms/frame (visible stutter), so each theme is baked **once** into a 160×120 RGB565 sprite (38,400 B each) via `renderThemeIntoSprite()` (half-scale `T{0.5f, 0.5f, 0, 0}` through `renderTheme()` — includes the theme's background + the dog). Every animation frame then:

1. repaints the whole preview band (rows 60..180, full width) with `clearRegion()` — day = solid, night = the per-row grey gradient, so no seam against the dashboard;
2. blits each card's visible intersection with the screen (independently);
3. redraws the two vertical border columns the band fill wiped (`redrawBorderSides()`; the top/bottom border rows are outside the band).

Every pixel of the band is written by every frame — no ghosting, no stale pixels, no flicker.

**Gotchas that cost real debugging time (all hit in this feature):**

- **Blit each card independently.** On the final frame the old card is fully off-screen (`w == 0`); `if (A.w > 0 && B.w > 0)` around the two blits then also skips the new card and leaves a white box in the preview. Use two separate `if (w > 0)` guards.
- **Final frame uses the exact travel distance** (`off = kTravel`), not the eased value — the easing undershoots by 1–2 px and the new card lands off the preview position, leaving a seam.
- **Partial blits must go through `panel()->writeImage()` with a manual `pixelcopy_t`** (`src_bitwidth = 160` = full sprite row width, `src_x` = first visible column). `LGFXBase::pushImage()` overwrites `src_bitwidth` with the sub-rect width, so each row is read with the wrong stride and adjacent pixels shift — visible as garbled pixels along the moving edge. A 16→16-bit copy is a raw row transfer whose byte order matches `push_sprite()` exactly.
- **`writeImage(..., use_dma)` must match the buffer's pool.** Allocation ladder per card: `createSprite()` (DMA-capable pool → row DMA, `use_dma=true`) → `heap_caps_malloc(MALLOC_CAP_8BIT)` + `setBuffer()` (general heap, **not DMA-accessible** → `use_dma=false`, rows CPU-pumped, still smooth). LGFX treats external buffers as "Preallocated", so `deleteSprite()` never frees them — `freeSwipeSprites()` owns their lifecycle. Passing `use_dma=true` for an 8-bit-pool buffer sends garbage (the panel's DMA cannot read that pool).
- **Keep the dog buffers loaded during the bake — no free/reload dance.** Free heap after WiFi/NTP is ~134 KB; two sprites (76.8 KB) + the dog (~36 KB) fit. The original approach freed the dog, allocated the sprites, then reloaded the dog before baking — but that reload peaks at ~100 KB against only ~93 KB of free heap and **always fails**, so every card was baked without the dog. Symptom: "dog missing in every theme" in the picker only (main screen fine, because it renders live). If a sprite can't be allocated at all, the immediate-mode fallback (re-render both themes clipped per frame) still runs, dog included.

**Boot diagnostics still in the tree (TEMP-DEBUG — remove when the animation is final):** `ramProbe()` prints free/min-free heap + DMA vs 8-bit pool sizes (incl. largest block) before/after allocating two card sprites; `gTimingOn` would add per-frame `[ANIM] f=.. off=.. blit: N ms` lines if enabled (currently never set — the boot auto-swipe demo that used it was removed when animation testing ended). Related serial lines: `[TOUCH] theme swipe -> NAME (n)`, `[ANIM] pre-render A+B: N ms`, `[ANIM] A -> B: 8 frames (sprites|immediate) in N ms`, `[RAM] ...`.

### Layout (single source of truth for item positions/sizes)
`struct Layout gLayout` is the single source of truth for **theme 0 (BASIC)**: every BASIC draw function, the per-second partial updates' clear bands and the theme-0 touch zones read from it – so re-tuning the original layout means assigning different values to `gLayout` and calling `renderScreen()`. Themes 1–3 have their own coordinates inside each `renderThemeN()` (matching the mockups in `mockups/`) and fixed touch bands (`touchBands()`). `glyphH(size)` = 8×size is the built-in Font0 glyph height (fixed-pitch 6×8 px per size unit, verified in the LovyanGFX source).

Background: `fillBackground()` — day = solid white (write TFT_BLACK); night =
vertical grey gradient via `nightBgColor(y)` (values pre-inverted for the panel).
`clearRegion(x,y,w,h)` repaints a region with matching background rows.

Data flow: `fetchWeather()` → HTTPS GET Open-Meteo (+ conditional NBM cross-check) + METAR
(KJRB/KLGA/KEWR) → ArduinoJson parse → global `wx` struct (`WeatherData`) → `renderScreen()`.
(Fresh KJRB METAR overrides the description and RAIN NOW; KLGA/KEWR drive RAIN NEARBY;
see Data sources & timing.)

## Data sources & timing
- **Weather**: Open-Meteo `https://api.open-meteo.com/v1/forecast`,
  lat 40.7517 / lon -73.9707 (ZIP 10017, NYC). Used for temperature, feels-like,
  humidity, today's high/low, the hourly rain forecast (next-rain scan) and day/night.
  Params:
  `current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,is_day,rain`
  `daily=temperature_2m_max,temperature_2m_min` `hourly=rain` `minutely_15=precipitation`
  `timezone=auto&forecast_days=2&temperature_unit=fahrenheit&precipitation_unit=inch`
  (all times are local wall-clock; rain analysis needs NTP synced)
  - **Rain timeline** (`analyzeRainTimeline()`): `minutely_15.precipitation` is NOAA RRFS,
    "preceding 15 minutes sum" in inches (~0.004 in quantization), covering ~48 h from local
    midnight. A step stamped T covers (T−15 min, T]. Wet = ≥ `RAIN_15MIN_THRESH_IN` (0.01 in/15 min;
    skips trace amounts — RRFS quantizes to ~0.004 in, so the threshold must stay above that floor);
    consecutive wet steps form a period, bridging ≤2 dry steps (30-min lulls); periods with fewer
    than `RAIN_MIN_WET_STEPS` (2) wet steps are ignored as model noise.
    Period onset = first wet stamp − 15 min, end = last wet stamp; an end time is only reported when
    the data shows a subsequent dry stretch. `hourly.rain` (≥ `RAIN_HOURLY_THRESH_IN`, 0.02 in/hr)
    extends the timeline beyond the last 15-min step. Display wording: RRFS-sourced next rain =
    "expected", hourly-only = "possible".
- **Live observation**: METAR via the NOAA Aviation Weather Center
  `https://aviationweather.gov/api/data/metar?ids=KJRB,KLGA,KEWR&format=json`. Response is a
  top-level JSON array (one object per station); parsed fields: `icaoId`, `rawOb` (full METAR/SPECI
  string) and `obsTime` (UTC epoch seconds). KJRB (Wall Street, Manhattan) is primary: its fresh obs
  drives the current-condition description (`metarText()`) and RAIN NOW detection
  (`metarHasRain()` — precip groups RA/-RA/+RA/SHRA/TSRA, plus DZ/FZ variants). KLGA and KEWR are
  nearby airports: a fresh obs with precip there while KJRB reports dry shows `RAIN NEARBY (LGA/EWR)`
  instead of a future-only "Rain expected ..." message.
  SPECIs (special reports issued between hourly updates when conditions change) count as
  fresh observations. An observation older than
  `METAR_MAX_AGE_S` (90 min), or a missing/unparseable response, falls back to Open-Meteo's
  WMO code for both (previous behavior); **RAIN NOW is never shown from a stale obs**.
  Checking the observation age needs NTP. Both sources are fetched on every refresh cycle.
  **Known limitation — model data lags reality:** Open-Meteo values come from the nearest
  grid point of a forecast model (updates every few hours, ~1 h lag), not live observations.
  This no longer affects RAIN NOW or the description (those come from KJRB METAR) but still
  applies to temperature, feels-like, humidity and today's high/low — don't debug a stale
  temp as a firmware bug.
- **NBM cross-check** (`nbmIndicatesContinuation()`): independent NOAA blend product that sanity-
  checks the RRFS rain end time. Separate small GET on `https://api.open-meteo.com/v1/forecast`
  with `models=ncep_nbm_conus&hourly=precipitation` (~1.5 KB body) — the main request stays
  byte-for-byte unchanged (a combined multi-model request is impossible: `models=auto,ncep_nbm_conus`
  → HTTP 400, and combining would add +37 % to the body; see
  `docs/nbm-crosscheck-investigation.md`). NBM hourly value at stamp T is a trailing sum over
  (T−60 min, T], so stamp E+1 h covers exactly the hour after an RRFS end time E — directly
  comparable. Rule: if any of the next `RAIN_NBM_CHECK_HOURS` (6) hourly values strictly past the
  RRFS end is ≥ `RAIN_NBM_HOURLY_THRESH_IN` (0.004 in/hr ≈ 0.1 mm/hr), rain "may continue": the
  definitive end time is suppressed and the line shows `RAIN NOW - MAY CONTINUE`. NBM never derives
  a new end time. Runs only when RRFS declared a concrete end for the current period; every failure
  path (no WiFi, HTTP error, timeout, bad JSON) returns false → RRFS-only behavior retained.
- **Time**: NTP (`configTzTime` + `time()`). US Eastern offset (UTC-5/UTC-4) is
  computed in software by `localOffsetSec()` with DST rules (2nd Sun Mar →
  1st Sun Nov). Do not rely on TZ-string parsing.
- **Refresh**: weather every 15 min (`WEATHER_REFRESH_MS`); at boot, wait up to
  `NTP_BOOT_WAIT_MS` (10 s) for NTP *before* the initial fetch so the rain line is
  valid from first paint; if NTP syncs later anyway and weather was already fetched,
  `loop()` refetches immediately on the sync transition. Clock display is H:MM AM/PM
  and repaints only when the minute changes (checked once per second in `loop()`);
  touch = immediate refetch.

## WiFi
- Credentials live in `src/wifi_config.h` (gitignored — copy `src/wifi_config.h.example` to `src/wifi_config.h` and fill in your SSID/password; never commit real values); auto-reconnect in `loop()`.
- HTTPS via `WiFiClientSecure` + `setInsecure()` (no CA cert). All three clients (main Open-Meteo,
  NBM cross-check, METAR) set `setConnectionTimeout(5000)` (TCP connect + socket read /
  SO_RCVTIMEO) and `setHandshakeTimeout(10)` — the core's default TLS handshake timeout is 120 s,
  which could freeze the loop on a stalled server. A timed-out fetch just fails and retries in 30 s.
- **Each request owns its client and frees its JSON before the next handshake** (cost real debugging
  time): keeping the main Open-Meteo client alive across the NBM/METAR requests left mbedTLS session/
  heap pressure that made those follow-up handshakes fail with HTTP -1 — silently degrading RAIN NOW
  to the model fallback and skipping the NBM cross-check. The scoping is load-bearing; don't
  "simplify" it away.

## ArduinoJson gotcha (caused the "night never triggers" bug)
- `variant | true` only accepts JSON **boolean literals**. Open-Meteo returns
  `is_day` as a **number** (0/1), so it always fell back to the default.
  Parse numerics explicitly: `!v.isNull() && (v == 0 || v == 1)`.

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

## Build & flash
- PlatformIO, board `esp32dev`, partition `huge_app.csv` (app > 1.25 MB),
  deps LovyanGFX ^1.1.0 + ArduinoJson ^7.0.0 + vendored `lib/puff`.
- Build: `pio run` (~RAM 17%, Flash ~52%)

### Flash procedure — ALWAYS these two steps, same every time
There is no shorter or longer variant; always do both, in this order:
1. **Download mode FIRST:** on the back of the CYD, **hold BOOT** (the button
   further in), **hit RESET** (button closer to the edge), **release BOOT**.
   Screen freezes/goes blank — normal; the ROM bootloader waits for data
   indefinitely, so there is no rush between steps 1 and 2.
2. In `C:\PIWorkspace\CYD`, CMD: `pio run -t upload`

Why both every time: `upload_port = COM4` and
`board_upload.before_reset = no-reset` are set in platformio.ini, so esptool
does NOT put the chip into download mode itself. Plain `pio run -t upload`
without step 1 is unreliable — it has sometimes connected (the DTR/RTS
transitions when the port opens happened to reset the chip into download mode)
and other times failed with "No serial data received" or "Wrong boot mode
detected (0x13)". The button dance makes it deterministic.

`test/enter_download_mode.py` was tried as a no-buttons alternative (2026-11-26):
after its DTR/RTS sequence, `pio run -t upload` still failed with "No serial
data received" — the script does NOT put this board into download mode (the auto-reset
circuit reliably provides reset, but DTR is not observed to reach BOOT/GPIO0).
Don't rely on it; the button dance is the only deterministic way in.

### AI instruction: do NOT flash the firmware yourself; always tell the user how.
After every successful build / whenever new firmware is provided, include the
two flash steps above verbatim in the reply to the user — don't make them look
them up.

- Serial monitor: COM4 @ 115200. At boot, `[FONT] nunito bff: ok|FAILED - using bitmap fallback`
  reports the theme-1 font load (see Theme 1 fonts section). Each refresh starts with
  `[WX] rssi=.. freeHeap=..`; each fetch logs `[WX] ... day=0|1|? rain=... next=...`, and NBM/METAR
  log their own `[NBM]`/`[METAR]` lines (freeHeap at request time, `DNS failed`, HTTP code) — an
  explicit `DNS failed` line distinguishes DNS from TCP/TLS when a fetch returns -1.
- **Opening COM4 resets the chip.** The CH340's DTR/RTS lines are wired to the auto-reset
  circuit (the same path esptool uses to reach download mode). Any plain open of the port —
  e.g. a pyserial log capture with no esptool sequence — toggles DTR and resets the ESP32.
  Observed: screen goes blank, then a full normal boot follows (NOT download mode). Don't
  casually open COM4 to peek at logs; use `test/capture_boot.py` (intentional reset +
  capture) or ask the user to share the log.

## Open items
- (Resolved 2026-11-15: Theme 2 (SPLIT) FEELS/RAIN chip right edges looked "cut off"
  on glass — the dog PNG's transparent left margin (device x 221..237) was
  pushed as solid background color over the chip corner arc (chips now end at
  x=225). LGFX's rounded-rect primitives were innocent (hand-traced for the
  exact size). Fix: `dogRowSpan()` + opaque-span-only push in `drawDogBox()`.
  See Display gotchas + Dog outfit images.)
- **(In progress 2026-11-26) Theme 3 (BENTO) rebuilt on Nunito BFF + mockup4-exact
  geometry** — builds clean (Flash ~52%), host preview (`tools/preview_theme4.py`)
  matches the mockups; **not yet on glass** (never flash-verified yet). After
  flashing: check the minute tick repaints only the clock card, the crescent's
  bite has no halo ring on the white card, and the picker's half-scale preview
  of BENTO renders cleanly (bffText @ 0.5× scale).
- **TEMP-DEBUG hooks in the swipe animation**: `ramProbe()` at boot and the dormant `gTimingOn`
  per-frame timing flag — remove once the animation is considered final (see Theme picker section).
  The boot auto-swipe demo in `setup()` was removed when animation testing ended; don't re-add it.
- (Resolved earlier: the night grey gradient was verified working on the glass;
  the "looks white" impression was wrong.)
- (Resolved 2026-11-15: the Theme 2 (SPLIT) night "yellow band" was the `bffText`/`bffCenter`
  scale-argument bug — the "NIGHT" label drawn at ~49,000× scale. A software bug
  misdiagnosed for a while as ST7789 stale state; the `displayHardReset()` sequence
  was never the fix. See Display gotchas + Theme 1 fonts Gotcha #3.)
- (Resolved 2026-11-15: Theme 2 (SPLIT) small text came out **garbled on glass** —
  `draw_alpha_bitmap_common()` is a nearest-neighbor scaler whose integer floor
  mapping drops whole 1-px source columns whenever scale < 1.0, so thin strokes
  vanished (date read like "Snday I5 Mvember"). Fix: exact-size em9/em12 masters
  added; every thin-stroke line now draws at scale 1.0 in the nearest master.
  See Theme 1 fonts Gotcha #4.)
- (Resolved 2026-11-15: Theme 2 (SPLIT) FEELS chip showed `"32.7 FF"` — `buildTempStr()`
  already carries the unit char and the chip appended it again. See Themes section.)
