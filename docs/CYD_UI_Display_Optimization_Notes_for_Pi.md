# CYD Weather — UI, Display, Animation, Keyboard, and Font Improvement Notes

## Purpose

Review the existing CYD Weather firmware for practical improvements to its on-screen keyboard, theme transitions, refresh flicker, and font clarity. Use the information below as project-specific engineering context. The goal is to improve the current implementation without introducing unnecessary libraries, memory use, or broad architectural changes.

This is a review brief, not permission to rewrite the display subsystem. First inspect the current implementation and report specific, evidence-based improvements. Preserve weather logic, touch behavior, settings semantics, persistent settings, and the existing hardware configuration unless a proposed change demonstrably requires otherwise.

## 1. Hardware and software constraints — use these as the source of truth

The board used by this project is the **ESP32-2432S032C-I**, not the original 2.8-inch ESP32-2432S028R board used by many examples in the general CYD community repository.

The project's board reference and current firmware configuration identify:

- Display: 1.9-inch ST7789, native 240 x 320 portrait glass, used as 320 x 240 landscape.
- Touch: GT911 capacitive touch controller.
- Graphics library: LovyanGFX, not TFT_eSPI.
- Processor: ESP32; 4 MB flash.
- This particular unit has **no usable PSRAM**. Do not assume that a CYD board has PSRAM just because another CYD variant does.
- Display SPI write clock is configured at 40 MHz.
- The TFT display's MISO/read connection is not wired. The SD card has its own MISO connection.
- The firmware uses LovyanGFX immediate-mode drawing: drawing calls write to the display over SPI rather than drawing into a permanent full-screen framebuffer.

The current display configuration correctly sets the panel as non-readable (`readable = false`). Preserve this. The project's BFF font rendering relies on the non-readable path because the TFT cannot return existing pixels. Setting the panel to readable can make anti-aliased glyph blending read invalid data and create pale or corrupted font edges.

Do not copy the generic CYD pin map, resistive-touch setup, XPT2046 code, TFT_eSPI configuration, rotation settings, or display assumptions from the community examples into this project. They target different hardware.

## 2. What the community CYD repository contributes

The community repository is useful as a catalogue of display techniques and UI-library examples. Its main examples are for the original 2.8-inch, 320 x 240, resistive-touch CYD and primarily use TFT_eSPI. Its LVGL examples use TFT_eSPI plus an XPT2046 resistive-touch input path; the LVGL 8 example is pinned to LVGL 8.3.11 and the LVGL 9 example to LVGL 9.0.0.

The most relevant transferable idea is that an embedded display UI can use a widget system such as LVGL to provide polished keyboard widgets, controls, and animated transitions. However, the community LVGL example is **not a drop-in solution** for this firmware. To use LVGL here, the firmware would need a LovyanGFX display-flush integration and a GT911 input integration. It would also add a UI framework and its memory/configuration overhead to a firmware that already has a working display stack and no usable PSRAM.

The community LVGL 9 configuration enables a broad set of features and examples, including the keyboard widget. Its example configuration reserves a 64 KB LVGL memory pool and enables examples/demo widgets. That is a demo configuration, not a suitable target configuration to copy into this application. A production LVGL build would need a deliberately reduced feature set and measured heap/stack use.

**Recommendation:** Do not migrate the project to LVGL or add TFT_eSPI merely to obtain a better keyboard or smoother swipe. First improve the existing LovyanGFX implementation. Treat LVGL as a possible future UI-architecture option only if a broader redesign is deliberately chosen and its memory use is measured on this exact board.

## 3. Current implementation already has several good optimizations

Do not duplicate work that is already present or replace these techniques without a measured reason:

- `src/display.h` / `src/display.cpp` configure LovyanGFX for the project's ST7789 and GT911 hardware.
- `src/render.cpp` has per-second partial-update logic. The clock/date and other changing text regions are repainted selectively instead of redrawing the entire dashboard every second.
- `src/picker.cpp` already pre-renders the old and new theme previews into two 160 x 120, 16-bit sprites before animating. Each sprite is about 38,400 bytes (37.5 KiB); both together use about 75 KiB before allocator overhead.
- The theme transition tries to allocate DMA-capable sprite memory first and has a general-heap fallback. It frees its temporary buffers after the animation.
- The picker has a slower immediate-mode fallback if the sprites cannot be allocated. That fallback redraws the themes on each animation frame.
- `src/keyboard.cpp` already uses a fixed-size static key table (`MAX_KEYS` and fixed-size label arrays) rather than dynamically allocating one object per key. This is a sensible low-overhead design for the device.
- The keyboard already supports upper/lowercase, a numeric/symbol layout, cursor placement, insertion, backspace, delete, and commit/cancel behavior. Preserve these behaviors.
- Themes 1–3 already use generated Nunito BFF font masters. The font generator and host-side font verification tools are already part of the project.

## 4. Keyboard: improve appearance and reduce redraw work without adding a UI framework

Relevant code: `src/keyboard.cpp`, `src/keyboard.h`, and the settings code that invokes the keyboard.

### Current behavior

The keyboard is a blocking modal interface. It waits for the opening touch to be released, polls for taps, then redraws itself when the input changes. The fixed key table is memory-efficient and should be retained unless a measured improvement justifies a different representation.

The main inefficiency is the `redraw` lambda in `keyboardQwerty()`. On ordinary character insertion, deletion, or cursor movement it currently:

1. Calls `fillBackground()`.
2. Draws the header again.
3. Rebuilds and redraws the input field.
4. Redraws every key.

This means a single keypress causes a full-screen repaint and then a repaint of the keyboard. On the night background, `fillBackground()` also paints the screen a row at a time. This is unnecessary work and can make key entry appear to flash.

### Recommended direction

Keep the modal keyboard and existing key table, but split the initial screen draw from incremental updates:

- Draw the full keyboard background, header, input field, and keycaps once when opening the keyboard.
- For normal text edits, repaint only the input field's bounding rectangle and redraw its text/cursor. Ensure the input-field clear uses the correct background and does not leave old characters behind.
- For cursor movement, repaint only the input field; do not repaint the keys or header.
- For a normal key press, optionally show a brief pressed-state colour on the touched key, then restore that single key. Do not redraw all keys to show press feedback.
- When Shift or the numeric/symbol layout changes, redraw only the keycaps whose labels or positions change. A complete keyboard-area redraw is acceptable for a layout switch; a full-screen redraw is not necessary.
- Keep the existing fixed key table and bounded input buffers. Avoid `String`, per-key heap allocations, or a large widget framework for this simple keyboard.
- Keep the current cursor, scrolling, maximum-length, backspace/delete, and Enter/Back semantics unchanged.

### Appearance improvements worth evaluating

- Make the key rows look more like a deliberate mobile keyboard: consistent spacing, alignment, corner radius, and key widths.
- Give special keys (Shift, `123`, Backspace, Space, and Enter) clearer visual hierarchy and enough touch area.
- Add a pressed state with a contrast change, not a costly animation.
- Use a consistent font family for labels if it remains legible at the small key sizes. The existing Nunito BFF font data may be reusable, but avoid downscaling small glyphs below their native raster size. If the current small built-in font is clearer at a given size, keep it.
- Keep all key labels and touch hitboxes aligned. Do not improve appearance by shrinking touch targets excessively.

Do not replace the keyboard solely because it is custom-built. The current fixed-size implementation is already cheap in memory; the avoidable cost is repeated full-screen drawing, not the static key table.

## 5. Theme swipe animation: determine which existing path is running before changing it

Relevant code: `src/picker.cpp` and the theme rendering functions in `src/render.cpp`.

### Current implementation

The picker draws each theme into a 160 x 120 sprite, then slides the two cached images across the preview band. It uses eight animation frames, an ease-out quadratic position curve, and a 15 ms delay per frame. The code comments estimate roughly 25 ms of drawing/blitting plus 15 ms delay per frame, or approximately 40 ms per frame and 320 ms for the eight-frame transition.

If sprite allocation fails, the fallback redraws the old and new themes through LovyanGFX on every frame. That is substantially more work and is expected to look less smooth.

The two 16-bit sprites require about 75 KiB in total, excluding allocator overhead. The project reports roughly 134 KB free heap after Wi-Fi in the conditions used when this path was designed. That is not a permanent guarantee: heap fragmentation, Wi-Fi state, image buffers, and other runtime allocations can change what is available. The code must continue to handle allocation failure safely.

### Recommended investigation

Before tuning the animation, establish which path is actually used on the device:

- Check the existing `[ANIM]` serial messages to confirm whether the sprite path or immediate-mode fallback is selected.
- The per-frame timing output is controlled by `gTimingOn`, which is currently initialized to `false`. Use a temporary diagnostic build or another low-overhead measurement to record each frame's actual duration and distinguish sprite blits from theme re-rendering.
- Record the sprite allocation result, DMA-capable memory availability, general heap availability, largest free blocks, pre-render duration, and total transition duration.
- Measure after Wi-Fi and the dog image have been loaded, because those are part of the real operating conditions.
- Do not leave noisy per-frame serial logging enabled in the normal build if it changes the animation timing.

### Improvements to consider after measurement

- If the immediate-mode fallback is running, investigate why the two existing sprites cannot be allocated. Do not simply add larger buffers. Preserve the fallback so the UI still works when memory is tight.
- If the sprite path is running but the slide still looks coarse, evaluate a modest increase from eight frames only after measuring transfer time. Eight frames move the card in comparatively large position steps; more frames can reduce the visible jump between frames, but each frame also causes additional display traffic and increases total transition time unless the drawing work is optimized.
- Consider a short A/B test of 10–12 frames versus the current eight, with the overall duration measured rather than assumed. Keep the transition responsive; do not add a long animation just to increase the frame count.
- Investigate whether the current full-width preview-band clear is more work than necessary. Any dirty-region optimisation must correctly erase the old card position, draw the new card position, and preserve the fixed border without leaving trails or stale pixels.
- Keep the pre-rendered sprite approach. Rendering complete themes repeatedly during a swipe should remain a fallback, not the preferred path.
- Retain DMA-capable transfers when available, but keep the general-heap CPU-pumped path correct. Do not assume every allocated sprite buffer is DMA-capable.

## 6. Full-screen refresh flashes: what is and is not achievable on this board

LovyanGFX is being used in immediate mode. There is no application-owned full-screen framebuffer that lets the firmware compose a new complete screen invisibly and then swap it in. A full `renderScreen()` writes the replacement screen directly to the panel, so a clear-then-redraw sequence can be visible as a flash or partial frame.

A full 320 x 240 RGB565 image needs:

`320 x 240 x 2 = 153,600 bytes` (150 KiB) for one buffer.

That is larger than the approximately 134 KB free-heap figure documented for the normal Wi-Fi-loaded operating condition, before accounting for allocation overhead or fragmentation. This board has no usable PSRAM. A full-screen off-screen buffer is therefore not a realistic default solution, and double buffering would require twice that storage.

### Recommended direction

- Preserve the existing partial-update approach for clocks, dates, status lines, and other small changes.
- Audit all callers of `renderScreen()` and other full-screen clear functions. Full redraws should be limited to genuine screen/state changes, not routine updates.
- For a screen transition, consider composing only the affected region in a temporary sprite and pushing that region once. Choose region dimensions from measured free heap, not guesswork.
- Batch related SPI drawing operations where LovyanGFX supports it, while maintaining correct transaction boundaries.
- Avoid clearing an area and then slowly drawing many objects over it when the same result can be composed off-screen in a smaller sprite and pushed as a single region.
- Do not introduce a full-screen framebuffer, second full-screen sprite, or continuous video-style frame buffer unless a real memory measurement proves it can fit with adequate headroom. On this board, the likely path to a better perceived result is fewer pixels written and more region-level updates, not a full-screen double buffer.

It is possible to improve smoothness substantially, especially for small transitions and partial updates. However, the same firmware should not be expected to achieve unrestricted full-screen video playback or tear-free 60 fps animation with its current memory and SPI constraints. The smooth video demonstrations seen on other CYD setups may use different hardware variants, different display libraries, DMA strategies, or much simpler frame content.

## 7. Fonts: preserve the existing BFF work and remove avoidable downscaling

Relevant code: `src/nunito_bff.h`, `src/render.cpp`, `src/display.h`, `src/display.cpp`, and `tools/make_nunito_bff.py`.

### Current font implementation

- Themes 1 (SKY), 2 (SPLIT), and 3 (BENTO) use generated Nunito BFF font masters.
- The available masters are em sizes 9, 10, 12, 13, 20, and 48.
- BFF glyphs are stored as compact static data in firmware flash; the font arrays are not a reason to allocate a full-screen RAM buffer.
- Theme 0 (BASIC) and some settings/keyboard/picker UI text use LovyanGFX's built-in bitmap font path.
- The generator rasterizes the Nunito variable font at chosen sizes and creates BFF data. The project includes host-side font verification.

### Important clarity issue: fractional downscaling

The project's own font investigation identified that LovyanGFX's BFF scaling path can drop one-pixel-wide source strokes when a small font is rendered below its native size. This makes thin letters look broken or uneven. The project already added smaller font masters to avoid scaling small text down.

Use this rule:

- For small text, select the closest available native BFF master and draw it at scale 1.0.
- Avoid BFF scales below 1.0 for small fonts. Prefer a slightly different text size to missing strokes.
- Fractional scaling is more tolerable for larger fonts (roughly 20 px and above) when their strokes are thick enough to survive.
- Review the actual effective scale, including the theme transform. The theme picker currently renders the full theme at half scale (`T` scale 0.5), so small text in the 160 x 120 previews is necessarily reduced and can lose detail. This is a likely reason preview text looks less crisp than the full-size theme.
- For the picker, consider simplified, purpose-built 160 x 120 theme thumbnails. Show the major layout, colours, clock, and a few representative labels; omit tiny detail text rather than shrinking every full-size glyph to half size. Another option is to draw a small amount of preview text using a preview-appropriate font master and layout.

### Preserve the panel/font blending configuration

- Keep `readable = false` for the ST7789 panel because the display MISO path is not connected.
- Keep the project's tested BFF background-colour handling. The BFF helper needs the correct background colour behind each text run.
- The current font generator deliberately uses binary glyph coverage values (0 or 15) rather than intermediate antialias values. This avoids a visible halo caused by blending against an incorrect base colour on this inverted, non-readable panel. Do not switch back to intermediate alpha coverage without testing the result on the physical display.
- Keep an explicit scale argument on every BFF drawing call. The helper's argument order is text colour, scale, then background colour; omitting the scale can silently pass a colour value as the scale.
- Keep a bitmap-font fallback if custom BFF loading fails.
- Avoid including the generated font arrays independently in multiple source files if that causes duplicate flash data. Prefer one shared font owner/helper that exposes the loaded font objects and drawing functions to the keyboard/settings/picker code.

### Where to look for practical improvements

- Audit every small-text BFF call for an effective scale below 1.0.
- Check theme 2 (SPLIT) and the picker previews especially, because compact labels and half-scale rendering are most vulnerable.
- Decide whether the remaining built-in bitmap text in settings and keyboard would benefit from the same Nunito look. Test legibility at the exact physical pixel size before converting it; do not force a custom font where the built-in font is clearer.
- Do not add runtime TrueType rendering or FreeType just to sharpen the UI. Pre-rasterized BFF masters are better suited to this fixed layout and constrained device.

## 8. Licensing and attribution

Use only the following licensing assumptions when considering the techniques described here:

### Community CYD repository

The community `ESP32-Cheap-Yellow-Display` repository is under the MIT License, with this notice:

> Copyright (c) 2023 Brian Lough
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

MIT permits commercial and non-commercial use, modification, and redistribution. If Pi copies source code or substantial portions of source code from that repository into this project, preserve the copyright notice above and include the permission/warranty notice with the redistributed source or accompanying license notices. A separate credit in the on-screen UI is not required by MIT.

This brief does not require copying any community source code. The recommendation is to reuse general techniques and keep the current hardware-specific implementation.

### Nunito font

Nunito is licensed under the **SIL Open Font License 1.1 (OFL)**. The font may be embedded and distributed with software, including converted or rasterized font data, provided the required copyright and OFL notice accompany the font software. The font software itself must remain under the OFL; it must not be sold by itself. The OFL does not require a visible credit in the application's UI.

The project README identifies `tools/fonts/Nunito-wght.ttf` as the source for the generated BFF files. Ensure the Nunito copyright notice and complete OFL 1.1 text are included in the repository/distribution alongside the source font and its generated BFF derivative. The expected `tools/fonts/OFL.txt` path was not present when checked, so Pi should confirm that the notice is included elsewhere or add it. Do not change the font's licensing when distributing the converted font data.

### LVGL (only if a future migration is considered)

LVGL is MIT-licensed. If it is ever adopted, preserve its MIT license notice with the distributed library. No LVGL code or library needs to be added for the recommended first-pass improvements in this brief.

### Existing LovyanGFX dependency

The project already uses LovyanGFX. Its package metadata identifies the library as `MIT AND BSD-2-Clause`; retain the relevant upstream license notices when redistributing the library with firmware/source. No library replacement is recommended here.

## 9. Recommended review order

1. **Keyboard redraw:** confirm that each normal text edit currently triggers a full-screen repaint, then propose a minimal incremental-redraw change. This is the clearest avoidable source of visual flashing and extra SPI traffic.
2. **Swipe path and timing:** confirm sprite allocation succeeds on the actual device. Measure frame times and determine whether the slower immediate-mode fallback is being used. Only then propose frame-count, dirty-region, or DMA changes.
3. **Full-screen refresh audit:** identify every path that invokes a full dashboard redraw and separate necessary screen changes from routine data updates. Preserve partial updates.
4. **Font audit:** identify small BFF text rendered below scale 1.0, including the half-size picker previews. Prefer native-size masters or preview-specific typography/thumbnails.
5. **Keyboard styling:** after the redraw path is efficient, propose a visual refinement using the current key table and existing graphics library.
6. **Licensing:** confirm that the Nunito OFL notice accompanies the font source and generated font data. Preserve MIT/BSD notices for any library or code actually redistributed.

For each proposed improvement, state the exact files/functions involved, expected visual or memory benefit, risks, and how to verify it. Prefer one small, reversible change at a time. Do not make unrelated changes to weather parsing, networking, time-zone logic, SD decoding, touch zones, or settings persistence as part of this UI review.

## 10. Additional findings from TFT_eSPI and its Sprite examples

These examples are useful for understanding sprite memory, composition, and animation. They are written for TFT_eSPI, however. This project uses LovyanGFX, so copy the design principles only; do not copy TFT_eSPI calls or introduce a second display library.

### 10.1 Sprite colour depth is a possible memory trade-off

TFT_eSPI documents that an 8-bit sprite uses one byte per pixel instead of two bytes per pixel for a 16-bit RGB565 sprite. Its 8-bit format is RGB332, so it halves memory but has fewer available colours and can show banding or less accurate gradients.

For the current picker dimensions:

| Buffer | 16-bit RGB565 | 8-bit RGB332 |
|---|---:|---:|
| One 160 x 120 preview | 38,400 bytes (37.5 KiB) | 19,200 bytes (18.75 KiB) |
| Two previews | 76,800 bytes (75 KiB) | 38,400 bytes (37.5 KiB) |
| Full 320 x 240 screen | 153,600 bytes (150 KiB) | 76,800 bytes (75 KiB) |

This is relevant because the current picker keeps two 16-bit preview sprites at once. An 8-bit version could potentially save about 37.5 KiB while both previews are allocated. That is an experiment, not an automatic recommendation: the themes contain gradients, dark backgrounds, and carefully chosen colours that may look worse after RGB332 quantization.

If evaluating this:

- First confirm that the installed LovyanGFX `LGFX_Sprite` version supports the required 8-bit colour depth and determine exactly how it encodes/expands those pixels.
- Keep the current 16-bit implementation as the known-good baseline and compare physical-screen photos of all themes, especially gradients and night palettes.
- Measure both total free heap and the largest contiguous allocation in the required capability pools while Wi-Fi and the dog-image buffers are active. A smaller buffer is only useful if its allocation and transfer path are reliable.
- Do not assume that TFT_eSPI's 8-bit sprite behaviour or API is identical to LovyanGFX.
- **Important:** the current `blitRegion()` path explicitly describes and constructs a same-depth 16-bit source-to-display transfer. It uses the display colour depth for both sides of `pixelcopy_t` and relies on raw same-depth DMA. Changing sprite depth without also changing this conversion path would be incorrect. An 8-bit sprite would need a valid 8-to-16-bit conversion path and a measured transfer-time comparison; it must not be passed through the existing 16-bit raw-blit path.

TFT_eSPI also documents that non-PSRAM ESP32 boards have tight sprite limits. This supports the existing decision not to add a full 320 x 240 16-bit framebuffer to this device. The library's quoted limits are general guidance, not a substitute for measurements on this unit.

### 10.2 Pre-compose before showing an animation frame

Bodmer's Sprite documentation and the reply in the supplied flicker discussion make the same general point: flicker occurs when the display pixels are changed rapidly in visible stages; the ideal solution is to finish composing a frame in memory and then transfer the completed image. The discussion also notes that the time needed to transfer a large display area limits the maximum frame rate.

Apply that principle carefully:

- The current picker already does an important part correctly: it pre-renders both themes once, before the animation, rather than re-rendering every theme on every frame.
- The current animation then clears the preview band and blits the visible parts of the old and new cards in separate operations. This is a region-limited approach that avoids a full-screen refresh, but the screen still sees a clear followed by one or two blits during each frame.
- If the sprite path is confirmed active and the animation still visibly flashes, Pi may evaluate whether the band can be transferred in a more completely composed form, or whether the transfers can be grouped into one uninterrupted display write. Any new composite buffer must be included in the RAM budget; do not add another 76.8 KB 16-bit band buffer alongside the two existing cards without proving that the largest allocatable block and runtime heap headroom support it.
- Do not trade the current reliable DMA-capable transfer path for a slower CPU conversion or redraw path merely to reduce the number of drawing calls. Measure total frame time and visual result on the actual display.
- Do not increase the number of frames until per-frame transfer time is known. More animation frames can make motion appear smoother only when the firmware can deliver them at a steady rate.

The supplied discussion is not a tested implementation recipe for this firmware. It describes a different ESP32-S3 board with an 8-bit parallel display interface. Treat the maintainer's full-frame-buffer comment as a general rendering principle, not evidence that a full-screen buffer is feasible here.

### 10.3 Readback-based sprite example is incompatible with this panel

The TFT_eSPI animated-dial example stores the background pixels under a moving needle, restores them when the needle moves, and then saves the new background. Its implementation reads pixels and rectangles back from the TFT controller.

Do not copy that technique directly. This project's display MISO/read line is not connected and the panel is deliberately configured as non-readable. Any method that calls `readPixel()`, `readRect()`, or otherwise expects to fetch the current TFT pixels is incompatible with the current wiring/configuration. The current pre-rendered sprite approach avoids that requirement.

The transparent-sprite example is useful for understanding colour-key transparency and dynamically allocating/deleting small sprites. It is not a direct fix for the current opaque theme cards or the keyboard redraw problem. Transparency still requires pixels to be transferred and does not by itself make an animation flicker-free. The project already allocates its picker sprites temporarily and frees them after the swipe, so that lifecycle pattern is already present.

## 11. Findings from esp32-smartdisplay and the Sunton PlatformIO board definitions

These repositories show a more complete LVGL-based driver stack for Sunton boards. They are useful as architecture and memory references, but adopting them would be a display-stack migration, not a small keyboard enhancement.

### 11.1 LVGL can use a partial draw buffer rather than a full-screen framebuffer

The Sunton board definition for the ESP32-2432S032C configures an LVGL draw buffer for one quarter of the display's pixel count. At 240 x 320 and RGB565, that is 19,200 pixels or 38,400 bytes (37.5 KiB). This is an example of a partial-buffer architecture: the framework renders a portion of the screen and flushes it, rather than requiring a full-screen 150 KiB RGB565 framebuffer.

This is worth retaining as a future architectural option if the project ever deliberately moves to LVGL. It does not mean LVGL is free: the LVGL core, widget code, styles, fonts, input integration, draw buffer, and application objects all consume flash and RAM. The project would still need to reproduce its current themes, settings, touch semantics, and rendering behaviour.

The `esp32-smartdisplay` project is a driver/integration layer for LVGL, not a ready-made drop-in keyboard for this application. A keyboard widget would be built using LVGL's own widget/API and enabled/configured in its LVGL settings. Using it would require porting the UI to LVGL rather than merely replacing `keyboard.cpp`.

### 11.2 Why this is not a recommended immediate migration

The Smartdisplay stack uses LVGL together with Espressif's `esp_lcd` panel interfaces and the Sunton board-definition package. The current project is built around LovyanGFX, its custom ST7789/GT911 setup, BFF font renderer, direct drawing functions, and carefully controlled partial updates.

Moving to Smartdisplay would change the display and touch integration and introduce a second graphics architecture. It may be a reasonable future redesign if the application grows into a widget-heavy interface, but it is not justified just to improve the current on-screen keyboard, theme swipe, or font clarity. First implement and measure the smaller LovyanGFX improvements in this brief.

The Smartdisplay repository's current documentation also demonstrates useful LVGL diagnostics such as optional CPU/FPS and memory monitors. Those monitors are relevant only to a separate LVGL experiment; they are not usable by the current LovyanGFX application without adding LVGL.

### 11.3 The Sunton board definition must not be copied blindly

The Sunton board-definition repository includes an entry for `ESP32-2432S032C`. It agrees with important parts of this project's configuration: ESP32-WROOM-32 class hardware, 4 MB flash, 240 x 320 native display dimensions, ST7789 over SPI, GT911 over I2C, backlight on GPIO27, and the SD-card bus pins used by this project.

However, the external board JSON also contains settings that conflict with this project's hardware configuration and verified source:

- It assigns GPIO12 as the ST7789 bus MISO pin. This project assigns GPIO12 to TFT reset and explicitly configures the TFT as non-readable because the display MISO line is not connected.
- It assigns GPIO21 as the GT911 interrupt pin. This project configures `TOUCH_INT = -1` and polls the touch controller because its interrupt line is not wired.
- It specifies a 24 MHz display pixel clock, while this project currently uses a tested 40 MHz LovyanGFX write clock.

Do not import that board JSON, change these GPIO assignments, enable TFT readback, or change the SPI clock based on the external repository. The local pin configuration and the actual unit's wiring/log evidence take precedence. The external definition is a comparison reference only.

There is also a display-size naming discrepancy to flag for the project owner: the local `CYD-HARDWARE.md` calls the panel 1.9-inch, while the external board catalog identifies the `ESP32-2432S032C` family as a 3.2-inch 240 x 320 unit. The local firmware's 240 x 320 native panel configuration matches the pixel dimensions, but it does not by itself establish the physical diagonal. Pi should report this documentation/model discrepancy and leave it unchanged unless the owner confirms the physical panel size. The pixel-buffer calculations in this document are based on the configured 320 x 240 landscape resolution and are unaffected by the diagonal label.

## 12. Licensing notes for the additional resources

### TFT_eSPI and its Sprite examples

The TFT_eSPI repository contains multiple license origins; do not describe the entire repository as MIT-only. Its license file identifies:

- Original Adafruit ILI9341 material under the MIT license, with the required original notice retained on redistribution.
- Imported Adafruit_GFX functions under the BSD license, requiring the applicable copyright notice, conditions, and disclaimer to be retained for source and binary redistribution.
- Bodmer's original library code and example sketches under the FreeBSD license, which requires retaining the copyright notice, conditions, and disclaimer in source redistributions and reproducing the notice in binary-distribution documentation/materials.
- GFX Free Fonts have their own license file. Do not copy a font asset without checking its individual notice.

Using general programming ideas from the examples does not require copying their source. If Pi copies an example's source code or a substantial portion, it must preserve the applicable license and copyright notices. A visible on-screen credit is not required by these licenses; retain notices in the repository's third-party notices or other accompanying documentation as applicable. Do not copy fonts without checking their separate terms.

### esp32-smartdisplay and platformio-espressif32-sunton

Both repositories identify their project license as **GNU GPL version 3 (GPL-3.0)**. This is not equivalent to MIT/BSD. If the project copies their implementation code, uses their library as a dependency, or incorporates their board-definition files, GPL-3.0 obligations may apply to distribution of the combined work. Pi must not pull them into the current project as a casual helper or copy their board JSON without first identifying and accepting the applicable GPL-3.0 obligations.

For this review, use only the technical observations recorded above. No Smartdisplay source, board JSON, LVGL implementation, or TFT_eSPI library needs to be added to CYD Weather.

## 13. Updated review priorities

1. **Keyboard incremental redraw:** keep the fixed key table; repaint only the input field and/or changed keycaps instead of the full screen.
2. **Picker path verification:** confirm that both 16-bit sprites allocate successfully from the DMA-capable heap and that the animation never uses the immediate-mode fallback.
3. **Measure actual animation frame times:** use the existing timing instrumentation to measure each of the eight frames, including clear, card blits, and border repaint. Distinguish sprite rendering time from display transfer time.
4. **Consider sprite colour depth only as a measured experiment:** compare 16-bit and 8-bit buffers only after verifying LovyanGFX support and implementing a correct source-to-display conversion path. Check colour quality and animation timing.
5. **Avoid readback-based restoration:** the TFT is non-readable; do not copy animated-dial background capture/restore code.
6. **Keep full-screen buffering off the table for now:** a 320 x 240 RGB565 buffer is 150 KiB, which is too large for the known no-PSRAM runtime budget. A quarter-screen LVGL buffer is a useful future reference, not a reason to migrate now.
7. **Font audit:** identify small BFF text rendered below scale 1.0 and consider preview-specific thumbnails.
8. **Hardware metadata check:** report the 1.9-inch versus 3.2-inch naming discrepancy and the conflicting external GPIO12/GPIO21 definitions, but do not change the project's verified hardware configuration.
9. **Licensing:** preserve the appropriate TFT_eSPI/Adafruit notices if any code is copied; avoid GPL-3.0 Smartdisplay and board-definition code unless the project deliberately accepts those terms.
