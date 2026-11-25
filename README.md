# CYD Weather Station Dashboard — Reference

Standalone ESP32 weather/clock dashboard.

**Repo:** [MichaelFiguresItOut/CYD-weather](https://github.com/MichaelFiguresItOut/CYD-weather)
— `git clone https://github.com/MichaelFiguresItOut/CYD-weather.git`

Fresh-clone setup: copy `src/local_config.h.example` to `src/local_config.h` and fill in
your WiFi SSID/password and site lat/lon (never commit the real file); copy the 17
outfit PNGs from `dogs/` to the SD card root (see Dog outfit images).

Companion reference: `docs/CYD-HARDWARE.md` holds the CYD hardware / display / font / SD
material split out of this file.

This file is the shared reference for future work: read it first in any new thread on this project.
## Files
- `src/main.cpp` — app shell: `setup()`/`loop()`, boot sequence, refresh cadence
- `src/config.{h,cpp}` — centralized configuration: `UserSettings` (all user-adjustable settings + site defaults), app timing/NTP constants, NVS persistence for every setting (`loadSettings()` + per-setting `save*()`), automatic/manual/unconfigured modes for the location (the timezone is automatic only — it follows the location); see the Configuration section
- firmware modules: `src/display.{h,cpp}` (TFT init), `src/wifi_conn.{h,cpp}` (WiFi credential seam + connect/auto-reconnect + scan), `src/weather.{h,cpp}` (fetch/parse, SD card + dog image), `src/render.{h,cpp}` (themes + per-second partial updates), `src/picker.{h,cpp}` (theme picker + swipe animation), `src/touch.{h,cpp}` (touch zones + gestures), `src/settings.{h,cpp}` (Settings app; see "Settings app (touchscreen)"), `src/keyboard.{h,cpp}` (blocking QWERTY modal), `src/geocode.{h,cpp}` (location lookup (postal code / city) for the LOCATION page), `src/time_util.{h,cpp}` (NTP + timezone), `src/metar.{h,cpp}` / `src/rain_timeline.{h,cpp}` (rain analysis), `src/nws_station.{h,cpp}` (NWS station discovery + latest observations + automatic backup/fallback logic for the STATIONS page), `src/nws_alerts.{h,cpp}` (NWS active alerts: polling + RAM cache + HTML→text, drives the WEATHER ALERT pill and screen — see the NWS active alerts section), `src/alerts_screen.{h,cpp}` (full-screen WEATHER ALERTS detail modal), `src/theme_ids.h` (theme indices)
- `src/nunito_bff.h` — generated Nunito BFF font blobs (masters em9/10/12/13/20/48) shared by themes 1 (SKY), 2 (SPLIT) and 3 (BENTO); regenerate with `tools/make_nunito_bff.py`, verify with `tools/check_bff_space.py`
- `src/dog_image.{h,cpp}` — SD-card PNG decoder; keeps palette indices (+PLTE/tRNS) for palette images, natural RGB565 + alpha for the rest, composites per row at draw time (see Dog outfit images section)
- `lib/puff/` — vendored Mark Adler `puff.c/h` (public-domain minimal DEFLATE inflater; the core's zlib is not linked into the app)
- `test/test_decoder.py` — host-side validation of the decode + composite pipeline against PIL for all 17 outfit PNGs (`python test/test_decoder.py`; ~206k px checked)
- `test/tz_verify.py` — host-side timezone verification: mirrors the C++ catalog + `localOffsetSec()` DST math and hourly-sweeps 2024–2033 for all 18 catalog zones against Python `zoneinfo` (IANA tzdb) (`python test/tz_verify.py`; ~1.58M samples, all must match)
- `test/nws/` — host-side NWS tests, each a 1:1 Python mirror of the pure logic in `src/nws_station.cpp` (`python test/nws/<script>.py`): `test_rank.py` (Phase 6.5 distance/freshness quality ranking, incl. the spec's worked example), `test_fallback.py` (Phase 4 backup-list building, observation usability and preferred→backup resolution), `test_feels.py` (Phase 5 NWS feels-like — wind chill / heat index — unit-aware observation parsing and dashboard source acceptance), `test_chunked.py` (the chunked `nwsGetCapped()` decoder against real wire bytes pulled from api.weather.gov), `test_trim.py` (the `trimFeatures()` character scanner); captured NWS point/station responses live in the same directory
- `test/capture_boot.py` / `test/enter_download_mode.py` — pyserial helpers over COM4: reset + capture boot log / force download mode (the force-download-mode path does NOT work on this board — use the manual button dance, see Build & flash)
- `tools/make_nunito_bff.py` — rasterizes Nunito (weight 700) at em sizes 10/13/20/48 into BFF blobs → rewrites `src/nunito_bff.h` (needs Pillow; font source `tools/fonts/Nunito-wght.ttf`)
- `tools/check_bff_space.py` — host-side verification: mimics `BFFfont` parsing exactly, decodes every glyph of every master against re-rasterized ground truth (`bad glyphs: 0/95` per master = good; exit code 1 on any mismatch)
- `tools/preview_theme1.py` — renders theme 1 day/night to `preview_theme1_{day,night}.png`, replicating the firmware's BFF parsing + placement math exactly
- `tools/preview_theme3.py` — same for theme 2 (SPLIT; the 3 in the filename is not the theme number), including LGFX's fixed-point fractional-scale glyph drawing (`python tools/preview_theme3.py [dog.png]`)
- `tools/preview_theme4.py` — same for theme 3 (BENTO; the 4 in the filename is not the theme number): mirrors `renderTheme3Nunito()` line for line (RGB565-exact palette, same fit math, procedural sun/moon/cloud/umbrella icons) → `preview_theme4_{day,night}.png`. Its `PAL` dicts duplicate `bentoDay`/`bentoNight` — update both together
- `platformio.ini` — build config (board, partitions, deps)
- SD card root: 17 outfit PNGs from `dogs/` (all 95x128, 256-color palette with tRNS transparency — copy them to the card root, see Dog outfit images section); they live on the card, not in flash

## Dog outfit images (SD card PNGs)
Golden retriever in the bottom-right corner, wearing an outfit that matches the
current weather. One of 17 palette PNGs on the SD card root is selected by
`selectDogImage()` in `src/weather.cpp` and reloaded whenever the selection changes.

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
- **Pipeline details** — PNG decode (puff inflate, scanline unfilter; palette
  sources kept as 1 byte/pixel indices + PLTE/tRNS, other sources as natural
  RGB565 + alpha), row-wise alpha compositing, SD wiring, and the byte-swap/
  inversion quirks: see `docs/CYD-HARDWARE.md` ("SD card PNG pipeline").
  `test/test_decoder.py` verifies the pipeline is pixel-exact against PIL.
- **Chunk-type comparison trap (hard-learned, 2026-11-20)**: PNG chunk names are
  fixed 4-byte fields, **not null-terminated strings** — the decoder must compare them
  with `memcmp(type, "IHDR", 4)`, never `strcmp()`. The old `strcmp()` read past the
  8-byte header buffer (undefined behavior) and on the 2026-11-20 build every
  comparison failed, so IEND was never recognized and *every* load died with
  `[DOG] no IEND` — with valid files on a working card. If a dog image suddenly fails
  with `no IEND`, suspect the chunk comparison before the card contact.

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
- **Load-failure diagnostics** (2026-11-22): a failed PNG load now prints the
  specific reason instead of staying silent — `[DOG] /<path>: read error walking
  chunks (truncated file or flaky SD read?)`, `read error in chunk CRC (flaky SD
  read?)`, `inflate size mismatch`, `bad scanline filter byte`, `bad palette index`,
  or per-buffer `out of memory (<buffer>)` (see **Decoder memory** below), plus
  the named checks (`bad IHDR`, `inflate failed rc=...`,
  `Adler-32 mismatch`, `chunk too large`). The SD-contact flake matters: a single
  bad read can leave the card in a bad state for the rest of the boot (in-boot
  retries keep failing until a full reset re-inits it — observed right after
  flashing, when `rain_later.png` and the `error.png` fallback failed across
  several fetches until a reboot). The 3× per-load retry in `refreshDogImage()`
  self-heals later in a clean boot, so a transient boot-time failure usually
  clears itself on the next fetch.
- **Decoder memory (2026-11-22): PSRAM for the transient buffers** — the
  captured "dog vanishes after the first fetch" incident was diagnosed as
  internal-heap exhaustion, *not* an SD read failure (the card initialized
  and the image data was read and inflated — the failure was the allocation
  of the decode buffers):
  - At boot the `error.png` load succeeded because the decoder ran *before*
    Wi-Fi initialization: the once-per-boot heap report showed internal free
    198 KB (largest block 107 KB) just before the decode, and the full
    ~104-109 KB working set fit, leaving 100 KB free / largest block 37 KB
    at the decode-peak probe. Once Wi-Fi, HTTPS/mbedTLS and JSON processing
    held their share, the post-fetch heap still held ~106-150 KB free
    (108,704–154,196 B across the captured fetches) but was fragmented: the
    47.6 KB `raw` buffer allocation failed during the fetches (in one
    cluster even the 24 KB `img` buffer), while a heap snapshot ~150 ms
    after one OOM cluster still showed 122 KB free / largest block 67 KB —
    a largest-contiguous-block problem, not a total-capacity one. The
    `error.png` fallback failed the same way — it needs essentially the same
    working set. (This documents the *captured* incident; earlier
    missing-dog incidents may have had other causes — see the SD card
    section below.)
  - Peak working set (the 95×128 RGBA outfits as they were on the card then;
    in the part-1 code all five allocations overlap — `idat` was only freed
    at the end of the load):

    | Buffer | Size |
    |---|---:|
    | Compressed IDAT (`idat`) | 16,186–22,973 B per file (varies) |
    | Raw/unfiltered image data (`raw`) | 48,768 B |
    | RGB565 output image (`img`) | 24,320 B |
    | Alpha buffer (`alpha`) | 12,160 B |
    | Row buffers (×2) | 760 B total |

    ~104 KB at a typical IDAT size, up to ~109 KB for the largest file's.

  - **Fix** (`src/dog_image.cpp`): the two *transient* buffers now request
    PSRAM first — `idat` grows via PSRAM-aware reallocation as compressed
    IDAT data arrives, and `raw` is allocated in PSRAM for the inflater's
    uncompressed image data. `img`, `alpha`, the row buffers and the
    downscale buffers stay in internal RAM (they are retained between
    renders or tiny). If PSRAM is unavailable or a PSRAM allocation fails,
    the helpers (`psMallocFirst()`/`psReallocFirst()`) fall back to ordinary
    internal allocation, so boards without PSRAM behave as before. Effect on
    a PSRAM-equipped board: the internal-heap peak drops from ~104 KB to
    ~37 KB — the large temporary buffers use the board's PSRAM instead of
    competing with Wi-Fi and the other internal-RAM users (configuration
    details: `docs/CYD-HARDWARE.md`, "Memory"). On the board in use this
    helped nothing — there is no usable PSRAM (see part 2 below), the
    fallback kept the peak in internal RAM and the post-Wi-Fi OOMs
    continued. Decoding behavior, dimensions, output quality, SD operations
    and cleanup are unchanged.
  - **Diagnostics:** once per boot, before the first decode:
    `[DOG] heap: PSRAM detected (free … KB, largest block … KB), internal
    free … KB (largest block … KB)` (`PSRAM NOT detected` if absent), and
    `[DOG] heap at decode peak: internal free … KB (largest block … KB)`
    while all decode buffers are live. Every OOM now names the buffer:
    `out of memory (idat buffer, … KB)`, `(raw buffer, … KB)`, `(rgb
    buffer)`, `(alpha buffer)`, `(row buffer A/B)`, `(downscale buffers)`.
    These separate allocation failures from SD communication failures
    (which show up as `[SD]` / `read error …` lines).
  - **Verified (2026-11-22):** `pio run` succeeds with no warnings;
    `test/test_decoder.py` passes all 17 dog PNGs (206,720 pixels checked);
    the existing NWS tests pass. The firmware flashed, but on the actual
    board the post-Wi-Fi OOMs continued — there is no usable PSRAM to fall
    onto (see part 2 below).
- **Decoder memory (2026-11-22, part 2): no usable PSRAM — palette assets,
  internal RAM only** — a later boot log the same day (`logs/
  device-monitor-261122-193056.log`) settled the PSRAM question for the board
  in use: `quad_psram: PSRAM ID read error: 0xffffffff, PSRAM chip not found
  or not supported` → `PSRAM NOT detected`. With no PSRAM to fall onto, every
  post-Wi-Fi dog load OOMed (`out of memory (raw buffer, 47 KB)` / `(rgb
  buffer)`) — the heap still held ~106-150 KB free (108,704–154,196 B across
  the captured fetches), but Wi-Fi/TLS/JSON allocations fragmented it so no
  contiguous 47.6 KB block existed during a fetch (a snapshot ~150 ms after
  one OOM cluster still showed 122 KB free / largest block 67 KB). The one
  decode that reached its peak (boot, pre-Wi-Fi, 198 KB free / largest block
  107 KB) loaded fine and left 100 KB free / largest block 37 KB at the
  decode-peak probe.
  - **Fix 1 — assets are 256-color palettes now.** All 17 `dogs/*.png` were
    converted to color type 3 (PLTE 256 entries + full 256-entry tRNS,
    dimensions and anti-aliased edges unchanged): ~18 KB → ~4.5 KB per file
    (IDAT ~2.6–3.7 KB). Quality approved side-by-side
    (`scratch-folder/dog-palette-compare/`); rendered-composite diff vs the
    original reference: p99 ≤ 17/255, max ≤ 33/255, mean ≤ 2.8 on the day
    (white) and night backgrounds (`scratch-folder/verify_vs_original.py`).
  - **Fix 2 — the decoder keeps palette images as indices** (`src/
    dog_image.cpp`): 1 byte/pixel index buffer + static PLTE/tRNS tables
    (13,184 B retained for 95×128, vs 36,480 B for RGB565+alpha). Per-pixel
    alpha is the tRNS lookup (index at/beyond the tRNS table length =
    opaque, per spec); `compositeRow()`/`dogRowSpan()` do the PLTE/tRNS
    lookup + blend at draw time. Peak decode working set drops from ~86-109
    KB (non-palette RGBA: 48,768 B `raw` + 24,320 B `img` + 12,160 B `alpha`
    + 760 B rows, plus the compressed IDAT, up to 22,973 B, while it is still
    alive) to 24,638 B (~24 KiB) — 12,288 B `raw` + 12,160 B indices live
    during the conversion pass, +2×95 B rows; the ≤3,756 B compressed IDAT
    is released right after inflate + Adler-32 so it never coexists with the
    others. The largest single allocation is 12,288 B, and the set fits the
    post-Wi-Fi internal heap (~106-150 KB free in the captured log); loads
    verified working on the board after the palette files were copied to the
    card. Non-palette sources (grey/RGB/RGBA) still decode to RGB565+alpha
    for robustness; a palette source larger than the 96x128 corner zone is
    expanded to RGB565+alpha first, since the area-average scalers work on
    that layout.
  - `-DBOARD_HAS_PSRAM` was removed from `platformio.ini` and the
    `psMallocFirst()`/`psReallocFirst()` helpers deleted (plain
    `malloc()`/`realloc()`; this board has no usable PSRAM).
  - **Verified (2026-11-22):** `pio run` builds clean (RAM 20.0%, Flash
    56.4%); `test/test_decoder.py` passes all 17 images (206,720 px; index →
    PLTE/tRNS resolution exact vs PIL, composite rows exact, opaque pixels
    untouched) plus a synthetic short-tRNS image proving the
    index-beyond-tRNS → alpha 255 default matches PIL; host-side render
    comparison vs the original references passes as above.
- **Transient SD failures**: if the dog is missing but everything else renders,
  check serial for `[SD]`/`[DOG]` lines first (card seating/contact was one such
  case — a plain reboot fixed it; `test/capture_boot.py` resets + captures).
  Boot now retries `SD.begin()` and `loadDogImage()` up to 3 times each (~1.4 s
  worst case), so a marginal contact usually recovers without a manual reboot.
- **SD card asset deployment — flashing never updates the card's PNGs.** The
  outfit PNGs are runtime data read from the SD card root; esptool writes only
  ESP32 flash, so after flashing a firmware that expects different assets
  (like the 2026-11-22 palette conversion) the old files stay on the card
  until the converted `dogs/*.png` are manually copied to the SD root. A
  missing dog right after a flash is either the transient boot failure above
  or stale assets on the card — the `[DOG]` lines tell which: `out of memory
  (…) buffer` = the on-card asset is too big for the decode pipeline (2026-
  11-22: the old 16-23 KB RGBA files OOMed on this no-PSRAM board; copying
  the new ~4 KB palette files brought the dog back), while `read error …` /
  `not found on SD card` = card contact or file problem.

## Firmware: display items → where they live (drawing in `src/render.cpp`; data in the `wx` global)
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
| **Data-age line** (replaces the old "Updated H:MM:SS AM/PM" stamp) | `drawBasicAgeLine()`: elapsed age of the source data, e.g. `NWS KLGA 24m - OM 15m` while an NWS station supplies the temperature (NWS age = observation time, OM age = Open-Meteo model-step time) or `OM 15m` when Open-Meteo does; `--` per source when the epoch is unknown (pre-NTP / unparseable). Grows with wall-clock time, so the per-second tick repaints the line once a minute (no network involved). Centered in text zone. SPLIT shows the compact two-source form `KLGA 24m · OM 15m` (no `NWS ` prefix) right-aligned at x=213 in row 1 — the full form does not fit next to the HUM/H/L text (`buildDataAgeLine()`'s `shortNws`) |
| **Dog outfit image bottom-right** | `drawDogImage()` composites the loaded outfit row-by-row against the current background and blits it (`src/dog_image.cpp`); selection in `selectDogImage()`, reloads via `refreshDogImage()`; drawn once per full render, never touched by partial updates. See Dog outfit images section |
| "touch: refresh" bottom hint | static label only – actual behavior per the Touch targets section below |
| **WEATHER ALERT pill** (only while ≥1 NWS alert is cached) | `drawAlertButton()` (render.cpp): drawn as the **last element of every theme** so it sits above all theme content; background/border colored by the highest cached severity (EXTREME deep red, SEVERE red-orange, MODERATE amber, MINOR blue). Per-theme rect from `alertBtnRect()` (render.h) — the single source of truth, `touch.cpp` hit-tests the same rectangle: BASIC open bottom-left, SKY in the free left part of the 140..204 band (sun sits right), SPLIT lower-left of the open stage (above the refresh hint), BENTO over the detail card's info row (redrawn after the per-minute info-line wipe) |

### Touch targets (action fires on touch-down; per-action debounce: 300 ms zone taps, 2 s refresh)
Generous fixed rectangles in landscape canvas coords (320x240), checked in this order in `handleTouch()`:

| Tap target | Zone | Action |
|---|---|---|
| WEATHER ALERT pill | the per-theme pill rect from `alertBtnRect()` (render.h) — hit and draw share one rectangle; only live while an alert is cached | opens the **WEATHER ALERTS** screen (see the NWS active alerts section) |
| Dog corner zone | x ≥ width−96, y ≥ height−128 (the reserved 96×128 bottom-right corner); in BENTO theme the whole right column (x ≥ 206, y ≥ 66) is the dog card and opens it too | opens the **THEME picker**: swipe left/right to browse themes, tap to select (see Themes below) |
| Clock band | y ∈ [12,50), full width | toggles clock between US 12-hour ("H:MM AM/PM") and 24-hour ("HH:MM") – `userSettings.use24Hour` |
| Date band | y ∈ [50,72), full width | cycles date line "Thursday 12 November 2026" ↔ "Thursday November 12, 2026" (weekday kept in place) – `userSettings.dateFormat`. Longest case ("Wednesday September 30, 2026", 336 px at size 2 on this fixed-pitch font) is scaled down slightly by `dateLineFit()` so the line always fits |
| Temperature band | x < width−96, y ∈ [92,138) | toggles all displayed temperatures (current / feels-like / today H-L) between °F and °C – `userSettings.useCelsius`. wx values stay in °F internally; conversion happens at draw time only (`tempForDisplay()`) |
| Anywhere else (excl. the pill) | — | immediate weather refetch + full render (original behavior) |

- Per-action debounce: zone taps (dog/clock/date/temp) are debounced 300 ms (double-tap protection — a tighter window would just toggle the value back); a manual refresh tap is debounced 2 s (the fetch is blocking, so this only guards the tail after a fast fetch completes — it prevents back-to-back manual fetches on a fast network). Both clocks are independent: a toggle does not delay a refresh and vice versa.
- Toggles update `userSettings.use24Hour` / `dateFormat` / `useCelsius` and are persisted via `saveDisplaySettings()` (explicit user change — see Configuration).
- Each toggle does a full `renderScreen()`; the per-second partial updates share the same format helpers (`formatClock()`, `formatDateLine()`) and `drawClockAndDate()` keeps `lastClockBuf/lastDateBuf` in sync, so no ghosting or redundant repaints after a toggle.
- While the THEME picker is up, `loop()` suppresses all main-display redraws (status changes, periodic refresh render, per-second partial updates) so it isn't clobbered; data fetching and dog-image reload still run normally. Picker gestures are evaluated on finger lift (`SwipeState` in `handleTouch()`): horizontal movement > 24 px (and more than twice the vertical) = **swipe** → next/previous theme (wraps around); a short (< 600 ms), small-movement press = **tap** → selects the previewed theme, saves it to NVS and returns to the main display. Ambiguous gestures are ignored.
- The clock/date/temp bands above follow the active theme (`touchBands()`): theme 0 derives from `gLayout`, themes 1–3 use fixed bands around their item positions – so tapping where an item is drawn toggles it on every skin.

### Themes (display skins)
Four themes (`THEME_COUNT` = 4), each with automatic day/night variants driven by `nightMode()`:

| # | Name | Layout |
|---|------|--------|
| 0 | BASIC | the original layout – all positions from `gLayout`, background via `fillBackground()` |
| 1 | SKY | full-screen sky gradient (3 stops), sun + clouds / moon + stars, status row, big centered clock, temperature left, description with mini cloud, FEELS and RAIN chips, dog in the corner (`renderTheme1()`); text is generated Nunito BFF fonts (see Theme 1 fonts section) with bitmap-font fallback |
| 2 | SPLIT | solid data zone on the left [0,214); right sky panel x ≥ 214 with flat sun / crescent moon + stars, two stationary clouds, DAY/NIGHT label and ground ellipse; text in the left zone, FEELS/RAIN chips span x 12..225 (213 px, so they cross the 214 seam by design — nothing must overpaint the 221..225 corner zone, see the dog-margin gotcha in Display gotchas), dog in the corner; Nunito BFF fonts like theme 1 (`renderTheme2()`) |
| 3 | BENTO | rounded card grid: clock card on top (time + date + WIFI/DAY row), temperature card with accent bar + FEELS chip + description row, RAIN card, detail card (HUMIDITY / TODAY H-L / data age + refresh hint) and a tall dog card on the right (`renderTheme3()`); text is Nunito BFF with bitmap-font fallback (see Theme 3 text below) |

- All renderers draw through one `struct T` transform (scale + offset): the live display uses `T::ID`, and the picker renders the previewed theme at half scale into a 160×120 window via `setClipRect()` – so previews are true scaled-down versions of what a selection will show. The picker's swipe animation bakes the same half-scale render into sprites – see the Theme picker section below.
- Colors: all palettes store *displayed*-space colors pre-inverted (this panel inverts every pixel) — `C(r,g,b)` = ~rgb565(displayed). Per-theme palettes: `SkyPal`, `CardsPal`, `SplitPal`, `BentoPal` (day/night instances) plus per-row background gradients via `bgDisplayed()` / `bgCode()` and `drawThemeBg()`.
- **Persistence**: the selected theme is saved to NVS (namespace `cydwx`, key `theme`) on selection (`saveTheme()`) and restored at boot before the first render (`loadTheme()` in `setup()`); both live in `src/config.cpp` and operate on `userSettings.theme`. `saveTheme()` also records `themeVer=2` with every save, so the legacy migration (which runs when the stored `themeVer` < 2) can never re-fire on a freshly selected theme: the removed CARDS theme (old index 2) falls back to BASIC (0); old SPLIT (3) → 2 and old BENTO (4) → 3. **NVS gotcha (cost a full debugging session, fixed 2026-11-27):** `Preferences::begin(name, true)` means *read-only*, not "create if missing" — in read-only mode every `putUInt` is a silent no-op returning 0, and `putUInt`'s return value is bytes written (4) or 0 on failure, so `if (err)`-style checks read a failed write as success. The namespace is now opened read-write and every write's return value is checked; the theme had never actually persisted before that fix.
- Per-second partial updates are theme-aware: BASIC repaints only its clock/date bands; SKY clears + redraws the status/clock/date band (including sky decorations); SPLIT clears + redraws only the left-zone clock/date band [10..214]×[22..78) via `drawSplitClockDate()`; BENTO clears + redraws only the clock card (8,8,304,52) via `drawBentoClockCard()` — the card's own background is part of the redraw, so the clock tick never needs a full re-render (no flicker, no flash). The data-age lines (see each theme's bottom line) repaint once a minute: BASIC clears its age band, SKY the tiny line [138..157), SPLIT the rain-chip/row-1 area [184..223) (per-row gradient at the 214 seam), BENTO the detail-card line band.
- **Theme 2 (SPLIT) FEELS chip "second F" (seen on glass, 2026-11-15).** `buildTempStr()` = `"%.1f %s"` — the unit char is **already appended** to temp strings (`"32.7 F"`, `"--.-"` when invalid). The SPLIT FEELS chip then appended `tempUnitChar()` a second time, drawing `"32.7 FF"` on glass. Every other theme draws the `tempOrDash()`/`buildTempStr()` result as-is. Rule: never append `tempUnitChar()` to a string produced by those helpers. Note the app-wide convention is value + space + unit char (no degree sign), so glass shows `32.7 F` where the design says `32.7°F` — intentional, and the host previews mirror the firmware string (`"32.7 F"`). The one exception is theme 3 (BENTO)'s TODAY H-L row, which appends `\xB0` (°) on purpose because the BENTO design specifies degree signs — `°` is in the BFF cmap (verified present before use).

**Theme 3 (BENTO) text: Nunito BFF (rebuilt 2026-11-26).** `renderTheme3()` dispatches on `nunitoOK`: Nunito path `renderTheme3Nunito()`, fallback `renderTheme3Legacy()` (bitmap font, boot log `[FONT] ...`). Geometry: clock card (8,8,304,52, r10) = 27 px clock (em48 @ ≤27/48, 320 px cap) + 10 px date (em10) + em10 status row (2.5 px dot + WIFI NTP left; sun/moon + DAY-NIGHT right); temp card (8,66,192,72) = 3.5 px accent bar, 34 px temp (em48 @ 34/48) + em20 @ 0.75 unit, 10 px FEELS chip (label + em10 value + crossed-arrows icon = three `drawLine` lw1 in a 9 px box), description row (10 px cloud + text, or "--" + "no data"); rain card (8,144,192,34) = 13 px umbrella/dot + 10 px label, whole group centered; detail card (8,184,192,48) = three 10 px rows HUMIDITY / TODAY H-L / data age + refresh hint (`drawBentoInfoLine()`: Nunito `"<age> \xB7 TOUCH TO REFRESH"` at em9, dropping to `"<age> \xB7 REFRESH"` when the two-source form misses the 174 px card; bitmap `"<age> - REFRESH"` at fitSize scale); dog card (206,66,106,166) = outfit centered in the 4/8 px margins + em10 caption. All procedural icons: day sun = fillCircle r3 + 8 lw1 rays at r5; crescent = fillCircle r4 with an offset r3 "bite" filled with the **card's** background color — the bite must match the surface the icon sits on (the white card, NOT the screen background) or a ring of the wrong color shows on the card. Palettes `bentoDay`/`bentoNight` are the displayed hexes (same `C()` 565 quantization as everything else — glass shows the quantized values). Preview: `tools/preview_theme4.py` mirrors the renderer line for line. Per-minute update: `clearThemed(THEME_BENTO, t, 8, 8, 304, 52)` + `drawBentoClockCard()` — nothing else repaints.

### Theme 1 (SKY) text: generated Nunito BFF fonts
Themes 1 (SKY), 2 (SPLIT) and 3 (BENTO) draw their text with generated Nunito BFF
fonts. Masters, per-theme sizing rules, and the four BFF gotchas that cost real
debugging time (byte padding, binary coverage, the float scale argument, no sub-1.0
scaling) are in `docs/CYD-HARDWARE.md` — "Nunito BFF fonts" and "Fonts (BFF)" sections.

Where a line may run long (the RAIN chip/card in all three themes), the text is
**never** scaled below 1.0 — sub-1.0 runs LGFX's nearest-neighbor scaler, which
drops 1 px source columns and garbles thin strokes (`Rain expected 10:45 AM
tomorrow` is 212 px at em13 and used to land at em13 @ 0.85 scale as broken
hairlines). Instead `bffRainFont()` steps the em size down (em13 → em12 → em10 →
em9) until the line fits the chip at scale 1.0; the longest rain line is 142 px
at em9, inside every chip's budget (161–180 px). Short lines keep em13, so only
the formerly-garbled long lines change. String widths for the shipped masters can
be checked offline with `tools/bff_width.py` (reuses the BFF parser from
`tools/inspect_bff.py`).

### Theme picker: static preview + swipe animation
Tap the dog corner to open the picker (`themeSelectScreen`). It shows a true half-scale preview (160×120 window at (80,60)) of `previewTheme` inside a yellow border on the normal background, plus chrome (`drawPickerChrome()`): yellow top bar "THEME n/4: NAME" (n is 1-based for the user; both numbers come from the format string, so they track `THEME_COUNT` automatically), a "SWIPE LEFT/RIGHT - TAP TO SELECT" hint, and a yellow bottom bar with the neighbours' names ("< PREV" / "NEXT >"). Swipe left/right = next/prev theme (wraps); tap = select, save to NVS, return to the main display. `previewTheme` starts at `userSettings.theme` when the picker opens and is updated at the *start* of each swipe (so a follow-up tap or swipe acts on the new theme immediately). Picker UI state (`themeSelectScreen`, `previewTheme`) is owned by `src/picker.{h,cpp}`.

**Swipe animation (`animatePickerSwipe()`): full-screen slide, sprite-baked cards.** The old card slides completely off the left edge while the new card enters from the right (mirrored for prev) — `kTravel` = 240 px, 8 frames, easeOutQuad, ~25 ms blit + 15 ms delay per frame ≈ 320 ms total. Redrawing both themes in immediate mode every frame takes ~50–120 ms/frame (visible stutter), so each theme is baked **once** into a 160×120 RGB565 sprite (38,400 B each) via `renderThemeIntoSprite()` (half-scale `T{0.5f, 0.5f, 0, 0}` through `renderTheme()` — includes the theme's background + the dog). Every animation frame then:

1. repaints the whole preview band (rows 60..180, full width) with `clearRegion()` — day = solid, night = the per-row grey gradient, so no seam against the dashboard;
2. blits each card's visible intersection with the screen (independently);
3. redraws the two vertical border columns the band fill wiped (`redrawBorderSides()`; the top/bottom border rows are outside the band).

Every pixel of the band is written by every frame — no ghosting, no stale pixels, no flicker.

LGFX gotchas hit while building this animation (independent blits, `writeImage` with a
manual pixelcopy, DMA vs 8-bit buffer pools, keeping the dog buffers loaded during the
bake) are documented in `docs/CYD-HARDWARE.md` — "Theme picker swipe animation: LGFX
gotchas".

**Boot diagnostics still in the tree (TEMP-DEBUG — remove when the animation is final):** `ramProbe()` prints free/min-free heap + DMA vs 8-bit pool sizes (incl. largest block) before/after allocating two card sprites; `gTimingOn` would add per-frame `[ANIM] f=.. off=.. blit: N ms` lines if enabled (currently never set — the boot auto-swipe demo that used it was removed when animation testing ended). Related serial lines: `[TOUCH] theme swipe -> NAME (n)`, `[ANIM] pre-render A+B: N ms`, `[ANIM] A -> B: 8 frames (sprites|immediate) in N ms`, `[RAM] ...`.

### Layout (single source of truth for item positions/sizes)
`struct Layout gLayout` is the single source of truth for **theme 0 (BASIC)**: every BASIC draw function, the per-second partial updates' clear bands and the theme-0 touch zones read from it – so re-tuning the original layout means assigning different values to `gLayout` and calling `renderScreen()`. Themes 1–3 have their own coordinates inside each `renderThemeN()` and fixed touch bands (`touchBands()`). `glyphH(size)` = 8×size is the built-in Font0 glyph height (fixed-pitch 6×8 px per size unit, verified in the LovyanGFX source).

Background: `fillBackground()` — day = solid white (write TFT_BLACK); night =
vertical grey gradient via `nightBgColor(y)` (values pre-inverted for the panel).
`clearRegion(x,y,w,h)` repaints a region with matching background rows.

Data flow: `fetchWeather()` → HTTPS GET Open-Meteo (+ conditional NBM cross-check) + METAR
(KJRB/KLGA/KEWR) → ArduinoJson parse → global `wx` struct (`WeatherData`) → `renderScreen()`.
(Fresh KJRB METAR overrides the description and RAIN NOW; KLGA/KEWR drive RAIN NEARBY;
see Data sources & timing.)

## Data sources & timing
- **Weather**: Open-Meteo `https://api.open-meteo.com/v1/forecast`,
  your site's lat/lon (`userSettings.lat`/`userSettings.lon`; defaults set in `src/local_config.h`, gitignored). Used for temperature, feels-like,
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
- **NWS active alerts** (`src/nws_alerts.cpp`): `https://api.weather.gov/alerts/active?point={lat},{lon}` (geo+json FeatureCollection). One scoped HTTPS GET per poll (identifying User-Agent, 32 KB body cap — NWS pretty-prints at ~3 KB raw per alert, and a cap hit mid-document just fails the parse); an ArduinoJson `DeserializationFilter` keeps only `features[].properties` (the geometry polygons are what make the raw body big). The RAM cache keeps up to `NWS_ALERTS_MAX` (8) alerts, most-severe-first (EXTREME/SEVERE/MODERATE/MINOR; ties keep feed order): uppercased event/severity/area, HTML-stripped description (≤ 1280 chars — NWS watches run ~1.2 KB) and instructions (≤ 120), plus the ISO-8601 `ends` (UTC epoch; 0 = unknown). Cadence (`ALERTS_REFRESH_ACTIVE_MS` 5 min while any alert is active, `ALERTS_REFRESH_CLEAR_MS` 15 min when clear, `ALERTS_MIN_GAP_MS` 60 s hard floor in config.h): a Wi-Fi (re)connect forces a fresh check, and a location change clears the cache first (the old location's alerts must not keep showing, even if the refetch never succeeds). **Every failure path — no Wi-Fi, HTTP error, unparseable or truncated response — keeps the previous cache**: stale alerts are shown rather than silently disappearing. The cache is rewritten in place and committed only after a fully parsed response. Chunked transfer encoding is decoded by hand off the HTTPClient stream (only `getString()` decodes it — same pattern and reason as `nws_station.cpp`). `htmlToText()` (nws_alerts.cpp) strips the tags, unescapes the entities NWS emits (named + numeric, `&#160;`/`&#x27;` style), collapses whitespace, and converts UTF-8 to 7-bit for the bitmap-font fallback path (curly quotes → straight, en/em dashes/bullet/middle dot → `-`, ellipsis → `...`, degree → `deg`; malformed/overlong sequences degrade to a space; a one-shot `[ALERTS] non-ASCII` line pinpoints a field that carried 8-bit bytes). Line structure: NWS wraps its own text at ~60 columns, so a `<br>`/`</li>`/`</p>`/newline mid-item is just source formatting and reflows as a space — only the break that *introduces* a `"* LABEL"` item is structural and kept as `\n`; a run of 3+ dots running into the next word renders as a spaced dash ("WHAT - ..." for readability in the GLCD font). `ends` is parsed by hand (newlib's `strptime %z` is inconsistent with the colon offset form NWS sends, and `timegm()` is not declared on the toolchain).
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
- **Time**: NTP (`configTzTime` + `time()`) with the servers from `config.h` (the system clock is kept in UTC). The local offset is computed in software by `localOffsetSec()` from the DST rules in `userSettings.timezone` — default US Eastern (UTC-5/UTC-4, 2nd Sun Mar → 1st Sun Nov). Do not rely on TZ-string parsing.
- **Refresh**: weather every 15 min (`WEATHER_REFRESH_MS`); at boot, wait up to
  `NTP_BOOT_WAIT_MS` (10 s) for NTP *before* the initial fetch so the rain line is
  valid from first paint; if NTP syncs later anyway and weather was already fetched,
  `loop()` refetches immediately on the sync transition. Clock display is H:MM AM/PM
  and repaints only when the minute changes (checked once per second in `loop()`);
  touch = immediate refetch.

## Configuration
All user-adjustable settings live in `src/config.{h,cpp}` — one `UserSettings`
instance (`userSettings`), initialized with the defaults in `config.cpp`:

| Field | Meaning | Default | Persisted |
|---|---|---|---|
| `lat` / `lon` | weather location (Open-Meteo requests) | from `src/local_config.h` (gitignored) | yes (`lat`/`lon` f32) |
| `locationMode` | `SettingMode` for lat/lon: UNCONFIGURED / AUTOMATIC / MANUAL | MANUAL (existing installs keep their built-for-site values) | yes (`locMode` u8) |
| `postalCode` | postal code or city entered on the LOCATION page (US ZIP, ZIP+4, international code, or city name) | `""` | yes (`postcode` str) |
| `placeName` | resolved location display ("City, Region, CountryCode") from geocoding | `""` | yes (`placename` str) |
| `timezone` | `TimezoneRules`: std/DST offsets + DST transition (month, nth Sunday, wall-clock hour). **Automatic only** — resolved from the active location on every weather fetch; the stored value is the last auto-resolved rules (a cache for boots before the first fetch) | US Eastern (UTC-5/UTC-4, 2nd Sun Mar → 1st Sun Nov), until the first fetch resolves the site zone | yes (`tzName` str, `tzStd`/`tzDst` i32, `tzDstS_M/W/H` + `tzDstE_M/W/H` u8) |
| `theme` | selected theme index (0–3) | BASIC (0), restored from NVS at boot | yes (`theme` u32) |
| `useCelsius` | temperature display unit (F/C) | F | yes (`celsius` u8) |
| `use24Hour` | clock format (12h/24h) | US 12-hour | yes (`clock24` u8) |
| `dateFormat` | date line format (0/1) | "Thursday 12 November 2026" | yes (`datefmt` u8) |

- App timing constants (`WEATHER_REFRESH_MS` 15 min, `WX_MIN_RETRY_MS` 30 s,
  `WIFI_RETRY_MS` 10 s, `NTP_BOOT_WAIT_MS` 10 s) and the NTP policy (UTC
  clock, `NTP_TZ`, `NTP_SERVERS`) are `constexpr` in `config.h`.
- The timezone is data, not code: `localOffsetSec()` applies whatever rules
  `userSettings.timezone` holds. To change the site timezone, edit the
  default in `config.cpp` (the DST-transition computation is generic over the
  rules struct).
- Deliberately **not** in the config struct (implementation-specific, not
  user settings): WiFi credentials (stay in `src/local_config.h` today, read
  through the `wifiCredentials()` seam in `wifi_conn.cpp`; see WiFi), METAR/NBM/HTTP
  timeout and rain-algorithm constants (`weather.cpp`), SD card pins
  (`weather.cpp`), and touch debounce/swipe-threshold constants (`touch.cpp`
  / `picker.cpp`).
- **Persistence** (NVS namespace `cydwx`): `loadSettings()` in `setup()`
  restores every setting before the first render; a missing key means "never
  saved" → the default above applies. Loaded values are validated (lat/lon
  ranges, timezone rules ranges, mode 0–2, theme clamped to 0–3) and invalid
  or out-of-range data falls back to the default. Saves are explicit —
  `saveTheme()` (picker selection), `saveDisplaySettings()` (clock/date/temp
  touch toggles), `saveLocation()`/`saveTimezone()` (after a successful
  automatic detection) — each writes only its own keys, so one setting is
  never clobbered by another, and nothing is written continuously. If the NVS
  namespace fails to open, all settings stay at their defaults and the saves
  no-op.
- **Automatic vs manual** (location only): mode UNCONFIGURED = no mode
  chosen yet; first successful auto-detection fills in the value and leaves
  the mode AUTOMATIC. AUTOMATIC = firmware-determined, refreshable whenever
  Wi-Fi is available. MANUAL = user-chosen, and automatic detection never
  overwrites it: `applyAutoLocation()` is the seam the future geolocation
  service calls, and it returns `false` without touching the value when the
  mode is MANUAL (or the value is invalid). `locationAutoAllowed()` gates the
  refresh behavior. The detection service and the settings UI that lets the
  user choose modes are not implemented yet. The timezone has no mode of its
  own: it is automatic only, and `applyAutoTimezone()` always applies a valid
  rule set (it is called only by the weather fetch).
- **Existing-install migration**: an install that predates this persistence
  has no `locMode` key. Location loads as MANUAL with the site default (the
  value it was always running with), so a working install keeps its location
  and is never handed to automatic detection. The timezone (automatic only)
  has no mode to migrate: the stored rules load as-is as the last
  auto-resolved cache, and the first weather fetch replaces them with the
  zone of the active location — a manual pick stored by older firmware is
  simply superseded by the first fetch, and a fresh install keeps the
  built-for-site default until then. The legacy `tzMode` key is erased on
  load. The legacy 5-theme numbering migration (`themeVer` < 2 → remap +
  write 2) is unchanged and still guarded by `themeVer=2` on every save.

## Settings app (touchscreen)
Full-screen modal UI (`src/settings.{h,cpp}`), entered from the **SETTINGS** button (top-right corner) of the THEME picker; `< BACK` walks one level back at a time, the main page's `< BACK` returns to the picker and repaints the dashboard. While active, `touch.cpp` routes every touch to the settings app (`settingsTouch()`) and `main.cpp` suppresses dashboard redraws. Every change is written to NVS (`cydwx`) immediately — there is no Apply step (spec requirement).

Main page: four section tiles.

| Section | Rows | Behavior |
|---|---|---|
| APPEARANCE | THEME, TEMPERATURE, CLOCK, DATE | all four rows tap-to-cycle (theme cycles the 4 themes; the others toggle) |
| LOCATION | CURRENT LOCATION (display only — "City, Region, CC" plus the postal code when known; a `* NAME` marker on the label line shows the saved profile matching the current location, Phase 7), LATITUDE / LONGITUDE / TIME ZONE (plain-text info rows), ADD LOCATION, SAVE LOCATIONS (Phase 7 — "n SAVED"), red CLEAR / RESET button | ADD LOCATION opens the QWERTY keyboard ("POSTAL CODE OR CITY") — accepts a postal code (US ZIP, ZIP+4, international) or a city name ("NEW YORK, NY", "WELLINGTON, NZ"; letters and the comma come from the keyboard's 123 page); the entered text is uppercased, trimmed, saved immediately, then geocoded via the Open-Meteo Geocoding API with a single Nominatim/OSM fallback request per action for codes or city+state phrases Open-Meteo doesn't index (identifying User-Agent, 1 req/s limit respected); the ADD LOCATION page stays up for the whole lookup (shows `LOOKING UP ON OPEN-METEO...`, then `FALLING BACK TO NOMINATIM...` when Open-Meteo has no match), then lists up to 20 matches on the same screen — one "City, Region, CC" row per place (no coordinates — not useful for choosing), 5 per page with `<` / `>` header arrows and a `PAGE n/N` footer when the results fill more than one page — and tapping one applies it: coordinates saved, `locationMode` → MANUAL, resolved name stored (CURRENT LOCATION row), the saved-profile association is dropped (Phase 7 — the new location is not a saved one), then the NWS station list is updated automatically on the same page before returning to LOCATION (Phase 6 — status lines and outcome messages in the STATIONS section below), and the weather is refetched with the new station chain; 0 matches or a network failure keeps the entered text and shows `NO MATCH FOR <entry>` / `LOOKUP FAILED - RETRY` on the same page (the header's `< BACK` returns to LOCATION) without touching the coordinates. SAVE LOCATIONS (Phase 7) opens the saved-profiles page — see the **Location profiles** bullet below. There is no coordinate entry — LATITUDE / LONGITUDE / TIME ZONE are display only: the location is changed via ADD LOCATION (postal code or city name), and the timezone is automatic (no manual selection, no DST toggle) — it follows the location and is resolved on every weather fetch (IANA id from Open-Meteo → catalog zone, `IANA_MAP` in `src/timezones.h`); the TIME ZONE row shows the resolved zone name and the footer shows the IANA id (`IANA: <id>`; `IANA: - (SET ON FIRST WEATHER UPDATE)` before the first successful fetch, `(UNSUPPORTED)` appended when the id has no catalog mapping — the last valid rules keep running). The red CLEAR / RESET button at the bottom clears the saved location (confirmation page, back keeps it); the association with any saved profile is dropped, the profiles themselves are kept |
| WI-FI | CURRENT NETWORK (status), SCAN NETWORKS (scan/join), ADD NETWORK (manual entry), WIFI ON/OFF (power), RESTORE SAVED NETWORK (build-time network) | see below |
| STATIONS | data source (info), preferred station (info), automatic backups (info), SCAN FOR LOCAL WEATHER STATION | NWS observation-station discovery + automatic fallback list — see below |

- **Wi-Fi flow** (see also `src/wifi_conn.{h,cpp}` and the scan requirements in
  `docs/CYD-HARDWARE.md`): the WI-FI page shows the connected SSID as plain
  **status text** (row 0 — display-only, deliberately not a button) and a
  **SCAN NETWORKS** button (row 1). **Entering the page never scans**; a
  scan runs only when the user presses SCAN NETWORKS (first time) or the
  RESCAN row. The SCAN page lists up to 40 results, **4 per page, strongest first**,
  with `<` / `>` arrows in the header (the top-right
  corner belongs to the arrows) and **RESCAN as the 5th row — shown even
  when the list is empty or the scan found nothing, so the page is never a
  dead end**; the footer shows `PAGE n/N`. Tapping a network joins it: WPA
  networks open the QWERTY keyboard (open networks join directly), ENTER
  commits, `< BACK` discards and the previously saved password stays in NVS;
  an empty password is accepted (some WPA networks genuinely use none). A
  scan needs the radio idle, so it disconnects the current link first and —
  once the list is shown — **reconnects to the previous network itself**
  (footer: `RECONNECTING...`), because the main loop's reconnect is paused
  while Settings is open. The
  **ADD NETWORK** row (page title: **WI-FI (2.4GHz only)** — the radio can
  only see 2.4 GHz, so the label is on the page and every Wi-Fi sub-page)
  enters an SSID and a password on the QWERTY keyboard (ENTER with an empty
  password field joins with none) and connects without scanning. A
  successful join (scanned or manual) saves the credentials to NVS
  (`wifiSaveCredentials()`) — they then take priority over `local_config.h`
  on the next boot. **WIFI ON/OFF** turns the radio physically off/on
  (`WiFi.mode(WIFI_OFF)` — RF off, not just disconnected): off also pauses
  the main loop's 10 s auto-reconnect, and pressing SCAN NETWORKS while it
  is off does nothing but prompt to turn the radio on (a scan must not
  silently re-enable it; a join does — explicit intent to connect). Off is
  a session setting — a reboot always comes back with Wi-Fi on. On
  reconnects with the current credentials (NVS, else build-time) after a
  short RF settle (a connect issued immediately after the OFF→STA switch is
  dropped by the driver).
  **RESTORE SAVED NETWORK** erases user-saved credentials from NVS and
  connects to the build-time network from `local_config.h` (the row's value
  shows that SSID). While the Settings modal is open the main loop neither auto-
  reconnects nor runs its periodic weather fetch — both are multi-second
  blocking operations and would swallow touch events (the board feels
  frozen); reconnecting resumes automatically once the user leaves Settings.
- **Keyboard** (`src/keyboard.{h,cpp}`, blocking modal): same header chrome
  as the settings pages — `[ < BACK ]  TITLE  [ ENTER ]`, with `< BACK` in the
  same top-left corner everywhere. ENTER commits, `< BACK` discards; the
  entry lives in a draft buffer until commit, so the caller's value is only
  replaced on KEY_OK. The header pills are hit-tested manually in the key
  poll (they are drawn text, not widgets). No inactivity timeout — the
  keyboard waits until the user chooses.
  QWERTY (passwords, manual SSIDs, location entry) shows a static `|` cursor
  (no blink): tapping the field moves it, keys insert at it, the BAK key
  (backspace) removes the character to its left and the DEL key removes the
  character to its right; long
  entries (31-char SSIDs, 64-char passwords) scroll within the field to
  keep the cursor visible. Its bottom row is `123` (letter/symbol pages),
  SPACE and DEL; the shift toggle (`abc`/`ABC`) and BAK sit in the Z-row —
  no duplicated labels in the layout.
- **STATIONS** (Phase 3/4/6/6.5, `src/nws_station.{h,cpp}`; US coverage only —
  footer: `NWS STATIONS (US ONLY) - TAP SCAN TO LOOK UP`): the info block
  shows `WEATHER DATA BY OPEN-METEO.COM`, then `PREFERRED STATION` and the
  selected station — `KABC - AMARILLO, GILLETTE FIELD` (size 2, truncated
  to fit) with `x.x KM AWAY - ASOS` below (provider, e.g. `ASOS`,
  `ASOS-HFM`) and, after the first successful fetch, `NOW USING: KABC`
  (white) or `NOW USING: KABC (BACKUP)` / `NOW USING: OPEN-METEO` (dim) —
  the source that actually supplied the last dashboard update (a backup id
  when the preferred's observation was stale or unavailable) — or, when the
  location has no NWS station (outside NWS coverage),
  `NO NWS STATION FOR THIS LOCATION` / `USING OPEN-METEO`;
  below the SCAN button it shows the firmware-managed automatic backup
  list (`BACKUPS: KLGA KTEB KEWR`, or `BACKUPS: NONE`). The only button is
  `SCAN FOR LOCAL WEATHER STATION`. The scan (blocking; a message page with `PLEASE WAIT` footer
  shows each step: `CONNECTING TO WIFI...`, `LOOKING UP NWS POINT...`,
  `FETCHING STATION LIST...`, then `CHECKING KXXX ...` per station while
  the Phase 6.5 quality selection runs) hits
  `api.weather.gov/points/{lat},{lon}` for the gridpoint (404 = outside US
  coverage → `NO STATIONS FOUND`, selection cleared), the gridpoint's
  station list, then the quality selection checks each of the up-to-8
  nearest suitable stations' `observations/latest` (one HTTPS request per
  station) and persists the quality-ranked preferred + automatic backups
  before the results page is shown; observations not checked that way are
  fetched lazily, for the stations on the current results page only (up to
  4 per page turn). The station list arrives
  chunked (`Transfer-Encoding: chunked`) and is de-framed stream-by-stream
  in `nwsGetCapped()` (plain vs chunked decided from the first byte;
  offline decoder tests in `test/nws/`). The results page (4 per page,
  row 5 = `REFRESH` re-runs discovery) shows one row per station:
  `ID - NAME` (size 2 if it fits 180 px else size 1) + right-aligned
  `x.x KM`, and below it the latest observation `59.0 F CLEAR - 32M AGO`
  — temperature unit follows the TEMPERATURE setting, no `°` glyph (the
  GLCD bitmap font has no degree sign: index 0xB0 is a checkerboard
  placeholder, and the main screen likewise writes plain `F`/`C`). The age
  comes from `nwsAgeText()` (NTP-synced obs timestamp vs device clock):
  `JUST NOW` (< 90 s), `32M AGO`, `2H 05M AGO`, `1D 4H AGO`, or
  `AGE UNKNOWN` without NTP. Stale rows (obs older than
  `NWS_OBS_FRESH_MAX_S`, 90 min) print in the warn color; a failed or not
  yet fetched observation shows `OBSERVATION FETCH FAILED` /
  `OBSERVATION NOT CHECKED` instead. Tapping a station row selects it —
  id, name, distance and provider are persisted to NVS (`cydwx`) — and the
  firmware automatically records up to 3 backup stations from the same
  scan's check set (Phase 6.5: the same quality ranking as the auto
  selection, excluding the pick; without NTP that is the plain
  nearest-first suitable list within 100 km — the former
  `nwsBuildFallbackList()` result) and persists them in NVS
  (`cydwx/nwsStnBk`); all of it survives reboot. The backup list is
  managed internally (never user-configurable) and is rebuilt on every
  selection. Phase 5 wires the selection into the dashboard: the temperature
  and feels-like come from the first usable observation in the order
  preferred → backups, then Open-Meteo (the main screen's data-age line
  shows which source supplied it — `NWS KLGA 24m - OM 15m`, see the
  dashboard table above).
- **Location profiles** (Phase 7, `src/profile.{h,cpp}`; NVS namespace
  `cydprof` — deliberately separate from `cydwx` so a corrupted or
  interrupted profile save can never touch the main settings, and vice
  versa): the LOCATION page's **SAVE LOCATIONS** button opens the profiles
  page. **SAVE CURRENT** asks for a profile name on the QWERTY keyboard
  (up to 12 characters, trimmed; names are unique case-insensitively) and
  stores the current location (postal code as entered, resolved display
  name, lat/lon) plus the full station configuration (preferred id/name/
  distance/provider + the ordered automatic backups) into the first empty
  slot — the new slot immediately becomes the **active** profile. When all
  three slots are used, SAVE CURRENT does not open the keyboard — the page
  shows `NO FREE SLOTS - CLEAR A SAVED LOCATION` in large (size 2, two
  lines) warn-colored text. Each of the three slots shows as a row: the
  profile **name + saved place** (`City, Region, CC`) on the left 75 %
  (tap to restore) + a red `CLEAR` (right 25 %); unused slots show
  `EMPTY` — tapping an empty slot acts like SAVE CURRENT (the current
  location is saved into that slot, which is the first empty one). The
  active profile's row is drawn on the yellow accent background (the
  name/place side — the red `CLEAR` box is unchanged — and the LOCATION
  page shows `* NAME` on the CURRENT LOCATION line). Tapping a saved
  profile's row opens a brief **RESTORE LOCATION** page that reports each
  step with a centered status line (same pattern as ADD LOCATION):
  `LOADING SAVED LOCATION...`, the saved station configuration (e.g.
  `NWS: KLGA + 3 BACKUPS`), `FETCHING WEATHER...`, then the outcome — the
  screen never looks frozen while the settings load and the weather is
  fetched. **RESTORE** copies the slot's location + station
  configuration back into the settings (no re-geocoding, no station
  rediscovery), makes it active, and refetches the weather — the
  automatic timezone re-resolves from the restored coordinates during
  that fetch (the timezone is never stored in a profile). **CLEAR** asks
  on the confirmation page (`ERASES THE SAVED PROFILE: NAME ... THE
  CURRENT LOCATION IS NOT CHANGED.`) and then deletes the slot — clearing
  the active profile drops the association. The active (yellow) profile
  is derived from the location itself: it is the profile whose stored
  location (coordinates + place name) equals the current location, so the
  marker survives a reboot and reappears whenever the current location is
  a saved one, even if the persisted association was dropped; SAVE and
  RESTORE record the association (it only breaks a tie between two
  profiles holding the same location), and ADD LOCATION / the location
  reset drop it (the saved profiles are kept). A station SCAN updates the
  active profile's station configuration in place (select or clear). Storage: one `|`-separated 12-field blob per slot (`p0`..`p2`)
  + the active index — on load a blob that does not parse exactly (an
  interrupted save) is treated as an empty slot, and a loaded active index
  whose stored location no longer matches the loaded location is dropped.
  There is no in-place rename (the spec lists one — the practical
  replacement is CLEAR + SAVE CURRENT with the new name), and no
  per-profile timezone (spec: the timezone always follows the location).
- **Phase 6 — automatic station update on a location change:** the SCAN
  button above is the manual refresh (with the step-by-step status messages
  and the `REFRESH` row on the results page). On top of that, confirming a
  new location on the ADD LOCATION page (tapping a search result) updates
  the station list automatically on the same page before returning to
  LOCATION: the previous selection is invalidated first (it belongs to the
  old location and must not be reused), the discovery runs with the centered
  status lines (`UPDATING NWS STATIONS...`, `CONNECTING TO WIFI...`,
  `LOOKING UP NWS POINT...`, `FETCHING STATION LIST...`, `CHECKING KXXX ...`
  per checked station — same pattern as the Open-Meteo location lookup),
  and the Phase 6.5 quality selection runs: the quality-ranked preferred +
  automatic backups are persisted, exactly what the SCAN auto-selection
  persists (the user can still change the pick afterwards via SCAN). The
  outcome is shown briefly before returning to LOCATION:
  `NWS: KLGA + 3 BACKUPS` (neutral color), `NO NWS STATIONS - USING
  OPEN-METEO` outside NWS coverage / with an empty list, `NO SUITABLE
  STATION - OPEN-METEO` when no station meets the suitability bar, or
  `STATION UPDATE FAILED - RETRY` on a transient network/API failure — the
  SCAN button on this page re-runs the discovery, and a failure never
  touches the coordinates or any unrelated setting (Wi-Fi, timezone).
- **Phase 6.5 — quality-based preferred + backup selection:** replaces
  "first suitable station (nearest first) wins" with a balance of distance
  and observation freshness, applied to both the SCAN auto-selection and
  the automatic post-location-change update (one shared code path). The
  search grows from a 10 km radius in 10 km steps to 100 km; within it, the
  observations of up to 8 nearest suitable stations (CoCoRaHS excluded)
  are checked, one HTTPS request each with a `CHECKING KXXX ...` progress
  line (the page never looks frozen). The preferred is the nearest station
  with a usable observation (fresh ≤ 90 min, NTP-synced) unless a station
  at most 10 km farther has an observation at least 20 min fresher (equal
  ages or a smaller difference keep the nearer one); up to 3 backups are
  then ranked the same way on the remaining candidates. A stale or failed
  observation never excludes a station — it is ranked after all usable
  ones, nearest first, so a temporary API failure only costs one selection
  cycle (the runtime fallback, Phase 5, keeps working unchanged: preferred
  → backups → Open-Meteo). Without NTP the selection falls back to plain
  nearest-first (no reliable age comparison) — the Phase 6 behavior. In
  sparse areas (no suitable station within 100 km) the nearest suitable
  station overall is selected, as in Phase 6. The ranking logic lives in
  `nwsRankStations()` / `nwsRankPrepare()` (`src/nws_station.cpp`) and is
  host-tested 1:1 in `test/nws/test_rank.py`.

## NWS active alerts (WEATHER ALERT pill + screen)
The pill is documented in the display-items table above; this section covers the detail screen and the text handling.

**Screen** (`src/alerts_screen.cpp`): full-screen modal in the settings-screen family (blue header bar with the same fixed "< BACK" / "<" / ">" pill slots, dark page background, dim footer hint). One alert per view; a long alert spans several pages (8 body lines each). The content stream for an alert is the wrapped description, then (when the alert has instructions) a blank line, the dim "WHAT TO DO" label (nudged to a fresh page when it would otherwise land on a page's bottom row) and the wrapped instructions. Header: "ALERT n OF m" (+ "P x/y" when the alert has > 1 page); arrows dim at the edges of the whole list. Rows below the header: event line (Nunito **em12**, cut from the end to fit the 312 px body width), severity (em9, colored) + `UNTIL <WWD> H:MM AM/PM` (em9, local wall clock from the cached UTC epoch; the weekday is included because NWS watches often end days out), affected area (em9, dim). Body text is Nunito **em13** at 15 px pitch (rows 98..216, footer hint at 226).

Navigation: body tap = next page → next alert → back to the dashboard on the last page; "<" / ">" go the other way (page, then alert). Taps fire on the **down edge** (re-armed when all fingers lift) so one hold pages once, and the opening/closing press is drained with the picker pattern (the tap that opened the screen is usually still on the glass when the page is drawn). `touch.cpp` routes every tap to the screen while it is up and swallows the open/close presses so they can't fall through to the dashboard zones underneath. If the Nunito BFF fonts failed to load (`nunitoOK` false, boot log `[FONT] ...`), the same layout falls back to the built-in GLCD font (size 1.5, 34 chars per line, char-width wrap).

**Wrapping** (`wrapBff()` in alerts_screen.cpp): greedy **word** wrap on *pixel* width (Nunito has variable advances) against `BFF_MAX_W` 312 px, with a 64-char-per-line cap that bounds the line buffer for pathological narrow-glyph runs. A word that does not fit the remaining line space **moves whole to the next line** when it fits on a fresh line (width ≤ 312 px and ≤ 64 chars) — words are never split mid-character; only a word that cannot fit a full line at all is hard-split. `\n` (kept by `htmlToText` for structural `"* LABEL"` items) forces a line break, a bullet block gets a blank separator line, and text left over after the page budget is marked with `...`. The GLCD fallback wraps on the same structure with a char count instead of pixels.

**Dashboard wiring** (main.cpp): the poll runs in `loop()` when `alertsDue()` and no modal is up (a changed cache triggers the full `renderScreen()` that shows/hides/recolors the pill; a location change clears the cache and redraws). The poll is deliberately **not** an LED activity (routine background work, see LED status). Per-second clock band repaints are suppressed while the screen is open (it is a full-screen modal like the picker and Settings).

**Serial**: `[ALERTS] n active` on each poll (with `(changed)` when the cache set changed — that is what triggers the dashboard redraw), `fetch failed (HTTP n), cache kept` / `parse failed: ..., cache kept` on the failure paths, and a one-shot `[ALERTS] non-ASCII 0x.. in <field> ...` when a raw alert field carried 8-bit bytes that `htmlToText()` had to convert.

## WiFi
- Build-time defaults + site location live in `src/local_config.h` (gitignored — copy `src/local_config.h.example` to `src/local_config.h` and fill in your WiFi SSID/password and your site's lat/lon; never commit real values). Runtime credentials are read through one seam, `wifiCredentials()` (`wifi_conn.cpp`), which prefers **user-saved NVS credentials** (`cydwx/ssid` + `cydwx/pass`, written by the Settings > WI-FI page on a successful join — `wifiSaveCredentials()`) and falls back to the build-time values. NVS is plaintext (no encryption; acceptable for this device). Auto-reconnect in `loop()` (`WIFI_RETRY_MS` 10 s check).
- **Scanning** (Settings > WI-FI): reliability requirements and the load-bearing scan parameters are documented in `docs/CYD-HARDWARE.md` ("WiFi radio: scanning"). The short version: passive 400 ms/channel scan (~6 s), only when the radio is fully idle, modem sleep off.
- HTTPS via `WiFiClientSecure` + `setInsecure()` (no CA cert). All three clients (main Open-Meteo,
  NBM cross-check, METAR) set `setConnectionTimeout(5000)` (TCP connect + socket read /
  SO_RCVTIMEO) and `setHandshakeTimeout(10)` — the core's default TLS handshake timeout is 120 s,
  which could freeze the loop on a stalled server. A timed-out fetch just fails and retries in 30 s.
- **Each request owns its client and frees its JSON before the next handshake** (cost real debugging
  time): keeping the main Open-Meteo client alive across the NBM/METAR requests left mbedTLS session/
  heap pressure that made those follow-up handshakes fail with HTTP -1 — silently degrading RAIN NOW
  to the model fallback and skipping the NBM cross-check. The scoping is load-bearing; don't
  "simplify" it away.

## LED status (onboard RGB)
The onboard RGB LED is a device status light, driven by `src/led_status.cpp` (`ledInit()` at the start of `setup()`; `loop()` keeps it current with `ledRecompute()`):
- **BLUE — busy**: the board is actively working — boot init + initial load, Wi-Fi scan/join (including the 10 s loop reconnect attempts), POSTAL lookup, STATION SCAN + observation checks, profile save/restore/clear, weather fetch, dog-outfit reload. An operation stays blue from start to finish even when it spans several network calls (e.g. the Open-Meteo → Nominatim fallback, a Wi-Fi reconnect mid-fetch): nested sub-steps never flash green/red in between.
- **RED — attention**: idle, but something needs looking at — Wi-Fi disconnected or radio off, NTP not synced, no NWS station configured, or a persistent failure line on screen ("NO FREE SLOTS - CLEAR A SAVED LOCATION", "LOOKUP FAILED - RETRY", "NO MATCH FOR …", "SAVE FAILED - RETRY", "FAILED" after a join). Red is suppressed while blue is active, so a blue operation that ends in failure settles on red.
- **GREEN — ready**: idle and healthy — connected, clock synced, NWS station configured.
Priority: blue > red > green. The 10 s Wi-Fi reconnect *check* in `loop()` is not an activity: while disconnected the LED stays red between attempts and goes blue only during the actual `wifiEnsureConnected()` call. Neither is the NWS alerts poll (the WEATHER ALERT feature) — it is routine background work and never moves the LED.

## Build & flash
- PlatformIO, board `esp32dev`, partition `huge_app.csv` (app > 1.25 MB),
  deps LovyanGFX ^1.1.0 + ArduinoJson ^7.0.0 + vendored `lib/puff`.
- Build: `pio run` (~RAM 20%, Flash ~56%)

### Flash procedure — ALWAYS these two steps, same every time
There is no shorter or longer variant; always do both, in this order:
1. **Download mode FIRST:** on the back of the CYD, **hold BOOT** (the button
   further in), **hit RESET** (button closer to the edge), **release BOOT**.
   Screen freezes/goes blank — normal; the ROM bootloader waits for data
   indefinitely, so there is no rush between steps 1 and 2.
2. In the project root: `pio run -t upload`

## Serial console
COM4 @ 115200. **Boot logging goes through `test/capture_boot.py`** — it runs
the CH340 auto-reset sequence that reliably re-boots the chip and captures the
log from t=0 (a plain port open does NOT reliably reset this board, verified
2026-11-27 — `docs/CYD-HARDWARE.md`, Board section).
- Boot: `[FONT] nunito bff: ok|FAILED - using bitmap fallback` reports the
  theme-1 font load (details in "Nunito BFF fonts", `docs/CYD-HARDWARE.md`).
- Boot also logs `[THEME] NVS: stored theme=.. ver=..` (what was read from NVS,
  ` (migrated)` if the legacy numbering migration ran) and `[THEME] active: NAME (n)`
  (the theme actually in use after migration/clamping). The NVS settings load
  logs `[CFG] location: mode=.. lat=.. lon=..`, `[CFG] timezone: NAME
  (std ..s dst ..s)` (the last auto-resolved rules; no mode — the timezone is
  automatic only) and `[CFG] display: temp=.. clock=.. datefmt=..` (values
  after NVS load; location mode 0=unconfigured 1=automatic 2=manual).
- Each refresh starts with `[WX] rssi=.. freeHeap=..`; each fetch logs
  `[WX] ... day=0|1|? rain=... next=...`.
- NBM and METAR log their own `[NBM]` / `[METAR]` lines (freeHeap at request
  time, `DNS failed`, HTTP code) — an explicit `DNS failed` distinguishes DNS
  from TCP/TLS when a fetch returns -1.
- `[ALERTS] n active[( changed)]` on each active-alerts poll (see the NWS
  active alerts section); the failure paths log `fetch failed (HTTP n),
  cache kept` / `parse failed: ..., cache kept`.
- Boot-time `[SD]` / `[DOG]` lines: see the Dog outfit images section.

## SD card: intermittent read failures (investigated; root cause not proven)
The board intermittently loses the dog image mid-run: boot SD init succeeds,
then PNG reads fail (dog falls back to `error.png`) until a chip RESET. This
is a **separate issue** from the PNG chunk-type `strcmp()`/`memcmp()` bug
(fixed 2026-11-20 — see "Chunk-type comparison trap" in the Dog outfit images
section): that was a decoder bug, not an SD communication problem. The failure
signature here is different — init succeeds and the files on the card are
valid, but individual sector reads fail. The earlier 2026-11-19 failure
history is in `docs/CYD-HARDWARE.md` ("SD card PNG pipeline", SD wiring item).

**Setup (as built / verified):**
- The SD card is driven by Arduino `SD.h` over a dedicated `HSPI` (SPI2)
  instance (`SPIClass sdSpi(HSPI)`; the TFT owns VSPI/SPI3).
- Pins (CYD schematic; `docs/CYD-HARDWARE.md`): CS 5, SCK 18, MISO 19, MOSI 23.
- Initialization runs at 400 kHz (forced by the driver); normal data transfer
  runs at 16 MHz (`SD.begin(SD_CS, sdSpi, 16000000)` in `src/weather.cpp`).
- Stack: PlatformIO `espressif32` 8.0.0, Arduino-ESP32 3.3.12, bundled
  ESP-IDF v5.5.5.

**Confirmed driver behavior** (verified against the installed framework
source, `libraries/SD/src/sd_diskio.cpp` + prebuilt FatFs):
- Initialization and normal reads use Arduino's own SPI implementation, not
  the ESP-IDF SDMMC driver.
- CMD17 handles single-block reads; CMD18 handles multi-block reads; CMD12
  (STOP_TRANSMISSION) terminates a multi-block transfer.
- FatFs batches contiguous reads, so PNG IDAT data can involve multi-block
  (CMD18 + CMD12) transfers; small reads (chunk headers, skips) use CMD17.
- The driver verifies the per-sector CRC16 and discards mismatched data.
- The existing retries re-issue clean transactions and can recover from some
  individual transfer failures, but they cannot clear a persistent
  `STA_NOINIT` state or reliably recover a card left in multi-block mode
  after an unsuccessful CMD12.
- Once `STA_NOINIT` is set, subsequent reads are rejected without
  communicating with the card. A complete re-initialization is needed to clear
  that state.
- The current firmware does not automatically re-initialize the SD card after
  a failed read (only the 3× boot-time `SD.begin()` / 3× load retries in
  `weatherInitSdAndDog()`).
- Pressing RESET restarts the ESP32 and runs SD initialization again. This can
  restore operation, but does not establish the original cause.

**Previously investigated Espressif issue (not applicable):** issue
[#14000](https://github.com/espressif/esp-idf/issues/14000) (SDSPI CMD52 CRC)
was investigated and found not directly applicable — our Arduino SPI driver
does not use CMD52, `sdmmc_io_reset()`, or `sdmmc_card_init()`, so its CMD52
CRC fix does not address our read path.

**What the evidence suggests, but does not prove:**
- Successful initialization at 400 kHz followed by failed reads at 16 MHz is
  consistent with an electrical/signal-integrity problem.
- Marginal SD-slot contact is a possibility, not a confirmed diagnosis.
- A failed CMD12 could leave the card in multi-block state and cause
  subsequent reads to fail for the rest of the boot.
- A persistent `STA_NOINIT` flag could independently prevent all further
  reads during that boot.

**What remains unknown:**
- The exact cause of the intermittent failures.
- Whether failures are caused by CRC errors, missing data tokens,
  command-response failures, CMD12 failures, or electrical interference.
- Whether the card is physically losing communication or entering a protocol
  state from which the current driver cannot recover.
- The actual cause of earlier incidents whose serial output was not captured.
  The most recent incident **was** captured and diagnosed (2026-11-22) as an
  internal-heap out-of-memory failure in the PNG decoder — not an SD
  communication failure; see the Dog outfit images section ("Decoder
  memory"). This section and its diagnostics remain valid for future
  genuine SD failures.

**Diagnostic logging gap (closed 2026-11-22):** with `CORE_DEBUG_LEVEL=0`
the driver's own error messages are compiled out, and the two data-phase
failures (missing data start token, CRC16 mismatch) print nothing at any log
level. To close this gap the project carries a local copy of the SD driver at
`lib/SD/` — a verbatim copy of the framework's `libraries/SD` (Arduino SD
library 3.3.12, from framework-arduinoespressif32 4.30312.261024, copied
2026-11-22) which PlatformIO prefers over the framework copy. Only one file
was modified, `lib/SD/src/sd_diskio.cpp` (the marked `CYD diagnostic` blocks):
it adds final-failure diagnostics that print directly to `Serial`, so they
are visible at `CORE_DEBUG_LEVEL=0`:

- **Data start token**: if the 0xFE token is not received within 500 ms the
  read logs the received value (0xFF = timeout, no data at all; any other
  value = wrong token/garbage) with the operation and sector, e.g.
  `[SD] data token fail: got 0xFF, expected 0xFE (CMD18 sector 32775)`.
- **CRC16 mismatch**: the received and expected CRC values are logged with
  the operation and sector, e.g.
  `[SD] CRC16 fail: got 0x1234, expected 0xABCD (CMD17 sector 32775, len 512)`.
- **Command failures**: `sdCommand()` logs the final failure with the command
  number/mnemonic, the raw response byte, and a decoded reason (R1 error
  bits), e.g. `[SD] CMD fail: cmd 12 CMD12 STOP_TRANSMISSION rsp=0x05
  (illegal command)`. This includes CMD12 failures that leave the card in
  multi-block state **without** setting `STA_NOINIT`, and the no-response path
  **that does** set `STA_NOINIT` (`... (STA_NOINIT set)`).

The additions are logging-only: protocol behavior, retry counts, timing,
pins, and SPI configuration are unchanged. The firmware binary built on
2026-11-22 was verified to compile `lib/SD/src/sd_diskio.cpp` (not the
framework copy) and to contain all diagnostic strings; it needs to be flashed
(`pio run -t upload`, see the "Flash procedure" section above) before the next
failure capture is useful. If the problem recurs, capture the failing boot with
`test/capture_boot.py` — the `[SD]` lines above now directly separate CRC
errors from token/protocol failures and identify which command failed.
