#!/usr/bin/env python3
"""Compare a line of BFF text on the glass (from photo) against the expected
bitmap computed directly from the font blob.

Usage: python tools/glass_forensics.py <photo.jpg> <line> [scale]
  line: one of date | row1 | status | hint | desc | feels | rain
Prints per-column profiles (E=expected ink, A=actual ink, .=both clear) and
the first columns that differ.
"""
import sys, os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
from PIL import Image
from tools.preview_theme3 import F10, F13, F20, F48

# photo-space mapping (measured from 20261115_183458.jpg)
PHOTO_X0 = 740.0    # photo x of display x=0 (left zone)
PHOTO_PX = 6.24     # photo px per display px (horizontal)
PHOTO_Y0 = 202.0    # photo y of display y=0
PHOTO_PY = 6.35     # photo px per display px (vertical)

# display-space layout of theme 3 (from renderTheme3 in src/main.cpp)
LINES = {
    # name: (font, text, x_top_left, y_top_of_line, scale_in_firmware, avail)
    "status": (F10, "WIFI NTP", 22, 9, 0.95, None),
    "date":   (F13, "Sunday 15 November 2026", 13, 64, 1.0, 201),
    "desc":   (F13, "No rain expected", 32, 128, 1.0, 182),
    "feels":  (F20, "36.0°F", 108, 151, 1.0, None),
    "rain":   (F13, "No rain expected", 20, 189, 1.0, None),
    "row1":   (F10, "HUM 62% · H 50.9° L 36.9°", 12, 217, 1.0, 190),
    "hint":   (F10, "TOUCH TO REFRESH", 32, 229, 0.85, None),
}

def expected_bitmap(font, text, scale):
    """Binary matrix of expected ink, indexed [line_row][col]."""
    B = font.ascent
    rows, cols = {}, {}
    cells = set()
    cx = 0
    sx = int(65536 * scale)
    sy = int(65536 * scale)
    for ch in text:
        g = font.f._glyph(font.f.cmap.get(ord(ch), 0))
        adv_s = (g.adv * sx) >> 16
        if g.w and g.h:
            xbase = cx + ((g.bx * sx) >> 16)
            yoff = B - (g.by + g.h)
            for py in range(g.h):
                y0 = ((yoff + py) * sy) >> 16
                y1 = ((yoff + py + 1) * sy) >> 16
                if y1 <= y0:
                    continue
                for px in range(g.w):
                    x0 = xbase + ((px * sx) >> 16)
                    x1 = xbase + (((px + 1) * sx) >> 16)
                    if x1 <= x0:
                        continue
                    a = g.px[py * g.w + px]
                    if a > 0:
                        for dy in range(y0, y1):
                            for dxp in range(x0, x1):
                                cells.add((dy, dxp))
        cx += adv_s
    return cells

def photo_profile(im, x0_disp, y0_disp, ncol, line_h, scale):
    """For each display column: 1 if any ink found in the row band."""
    prof = []
    for dx in range(ncol):
        px = int(PHOTO_X0 + PHOTO_PX * (x0_disp + dx))
        # row band with slack for perspective tilt
        py_c = PHOTO_Y0 + PHOTO_PY * (y0_disp + line_h / 2)
        y_a = int(py_c - PHOTO_PY * (line_h / 2 + 3))
        y_b = int(py_c + PHOTO_PY * (line_h / 2 + 3))
        hit = 0
        for py in range(max(0, y_a), min(im.height, y_b)):
            for qx in range(px - 2, px + 3):
                if 0 <= qx < im.width:
                    r, g, b = im.getpixel((qx, py))[:3]
                    lum = (r + g + b) / 3
                    if lum > 150:
                        hit = 1
                        break
            if hit:
                break
        prof.append(hit)
    return prof

def main():
    photo_path, name = sys.argv[1], sys.argv[2]
    scale = float(sys.argv[3]) if len(sys.argv) > 3 else None
    font, text, x0, y0, sc_fw, avail = LINES[name]
    sc = sc_fw if scale is None else scale
    im = Image.open(photo_path).convert("RGB")

    exp = expected_bitmap(font, text, sc)
    ncol = max(x for (r, x) in exp) + 1 if exp else 200
    prof = photo_profile(im, x0, y0, ncol, 14 if font is F10 else 14, sc)

    # expected column profile
    exp_prof = [1 if any((r, x) in exp for r in range(16)) else 0 for x in range(ncol)]

    line1 = "".join("E" if e else "." for e in exp_prof)
    line2 = "".join("A" if a else "." for a in prof)
    line3 = "".join("^" if (e != a) else " " for e, a in zip(exp_prof, prof))
    print(f"{name}: {text!r} scale={sc:.3f}  cols={ncol}")
    print("expected:", line1)
    print("actual:  ", line2)
    print("diff:    ", line3)
    ndiff = sum(1 for e, a in zip(exp_prof, prof) if e != a)
    print(f"columns: {ncol}, mismatching: {ndiff}")
    if ndiff:
        # show first mismatch region with glyph boundaries
        b = 0
        print("char cells (start col):", end=" ")
        cx = 0; sx = int(65536 * sc)
        for ch in text:
            g = font.f._glyph(font.f.cmap.get(ord(ch), 0))
            print(f"{ch!r}@{cx}", end=" ")
            cx += (g.adv * sx) >> 16
        print()

if __name__ == "__main__":
    main()
