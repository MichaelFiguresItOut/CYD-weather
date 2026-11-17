#!/usr/bin/env python3
"""Render the SPLIT theme (index 2 in the 4-theme numbering) day + night previews that mirror src/main.cpp.

Reuses the verified BFF parser from tools/preview_theme1.py and adds a
draw_scaled() that replicates LGFX's BFFfont::drawChar fixed-point math
exactly (see lgfx_fonts.cpp):

    sx = sy = int(65536 * scale)
    xAdvance  = (adv * sx) >> 16
    xoffset   = (bbox_x * sx) >> 16          # draw_x = user_x + xoffset
    yoffset   = ascent - (bbox_y + h)        # UNSCALED (datum top_left: the
                                             # scaled y_offset terms cancel)
    per pixel row py / col px:
        y0 = ((yoffset + py) * sy) >> 16 ; y1 = ((yoffset + py + 1) * sy) >> 16
        x0 = (px * sx) >> 16           ; x1 = ((px + 1) * sx) >> 16
        fillRect(draw_x + x0, user_y + y0, x1 - x0, y1 - y0)

Layout mirrors renderTheme2(): left solid data zone [0,214), right sky panel
x>=214 with vertical gradient (#7cc0ec->#b5dcf6 day / #1c3f74->#0e2549 night);
flat sun or crescent moon + stars; two stationary panel clouds;
DAY/NIGHT label (white/gold) + ground ellipse under the dog; status row,
left-aligned clock/date, temp+unit with aligned baselines, description with
mini cloud icon, FEELS chip with crossed-arrows icon, RAIN chip with umbrella
icon, bottom data row @ y=217 and hint. Dog PNG blitted bottom-right (stationary).

Usage: python tools/preview_theme3.py [dog.png]
Outputs preview_theme3_day.png / preview_theme3_night.png in the repo root.
"""
import math
import os
import sys
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
from tools.preview_theme1 import BFFFont, load_masters  # noqa: E402

W, H = 320, 240
SPLIT_X = 214


# ----------------------------------------------------------------- colors ---
def hx(s):
    s = s.lstrip("#")
    return tuple(int(s[i:i + 2], 16) for i in (0, 2, 4))


PAL = {
    "day": dict(
        left=(255, 255, 255), grad_top=hx("7cc0ec"), grad_bot=hx("b5dcf6"),
        sky_lbl=(255, 255, 255), ground=hx("6fb1e2"),
        st_l=hx("35618f"), dot=hx("2f9e6b"),
        clock=hx("16325c"), date=hx("4a6fa5"), temp=hx("16325c"), unit=hx("3d5f8f"),
        desc_txt=hx("5a7089"),
        f_bg=hx("e3f1ff"), f_bd=hx("9cc7f5"), f_lbl=hx("2b6cb0"),
        f_val=hx("1d4f8c"), f_icon=hx("2f7fd0"),
        r_bg=hx("e6f7ec"), r_bd=hx("8fd6ae"), r_txt=hx("237a52"), r_dot=hx("2f9e6b"),
        row1_c=hx("7d92ad"), hint=hx("a8b8cc"),
    ),
    "night": dict(
        left=(21, 31, 46), grad_top=hx("1c3f74"), grad_bot=hx("0e2549"),
        sky_lbl=hx("ffc857"), ground=hx("274b7d"),
        st_l=hx("6fb3ff"), dot=hx("3ecf8e"),
        clock=(255, 255, 255), date=hx("9fc3e8"), temp=(255, 255, 255), unit=hx("cfe3ff"),
        desc_txt=hx("c3d0e4"),
        f_bg=hx("173a5f"), f_bd=hx("5aa9ff"), f_lbl=hx("9fd0ff"),
        f_val=(255, 255, 255), f_icon=hx("9fd0ff"),
        r_bg=hx("12352a"), r_bd=hx("2f9e6b"), r_txt=hx("7ee2b8"), r_dot=hx("7ee2b8"),
        row1_c=hx("8aa3c0"), hint=hx("5f7392"),
    ),
}


def panel_color(mode, y):
    p = PAL[mode]
    t = min(1.0, max(0.0, y / (H - 1)))
    return tuple(int(a + (b - a) * t + 0.5) for a, b in zip(p["grad_top"], p["grad_bot"]))


def blend_white(rgb, pct):   # mirror firmware: blend in RGB565 space, then expand
    r = rgb[0] >> 3; g = rgb[1] >> 2; b = rgb[2] >> 3
    mix = lambda v, mx: (v * (100 - pct) + mx * pct) // 100
    return (mix(r, 31) << 3 | mix(r, 31) >> 2,
            mix(g, 63) << 2 | mix(g, 63) >> 4,
            mix(b, 31) << 3 | mix(b, 31) >> 2)


def cround(x):   # C-style (int)(x + 0.5f)
    return int(x + 0.5)


# ------------------------------------------------------------------ fonts ---
class Font:
    """BFFFont with firmware-exact scaled drawing (bffText / bffCenter)."""

    def __init__(self, em):
        self.f = load_masters(os.path.join(ROOT, "src", "nunito_bff.h"))[em]
        self.em = em
        self.ascent = self.f.head["ascent"]

    def width(self, s):   # bffWidth: unscaled advance sum
        return self.f.width(s)

    def fit(self, s, avail, cap=None):   # bffFit + optional design size cap
        w = self.width(s)
        sc = (avail / w) if (w > avail and w > 0) else 1.0
        if cap is not None:
            sc = min(sc, cap)
        return sc

    def draw(self, img, text, x, y, s, color):   # bffText: datum top_left
        B = self.ascent
        sx = int(65536 * s)
        sy = int(65536 * s)
        px = img.load()
        W_, H_ = img.size
        cr, cg, cb = color
        cx = x
        for c in text:
            g = self.f._glyph(self.f.cmap.get(ord(c), 0))
            adv_s = (g.adv * sx) >> 16
            if not (g.w and g.h):
                cx += adv_s
                continue
            draw_x = cx + ((g.bx * sx) >> 16)
            yoffset = B - (g.by + g.h)          # unscaled, see docstring
            for py in range(g.h):
                y0 = ((yoffset + py) * sy) >> 16
                y1 = ((yoffset + py + 1) * sy) >> 16
                if y1 <= y0:
                    continue
                yy = y + y0
                for i in range(g.w):
                    a = g.px[py * g.w + i]
                    if a == 0:
                        continue
                    x0 = (i * sx) >> 16
                    x1 = ((i + 1) * sx) >> 16
                    for ix in range(draw_x + x0, draw_x + x1):
                        if 0 <= ix < W_:
                            px[ix, yy] = (cr, cg, cb)   # binary alpha {0,15}
            cx += adv_s

    def center(self, img, text, cx, y, s, color):   # bffCenter: datum top_center
        w = cround(self.width(text) * s)
        self.draw(img, text, cx - w // 2, y, s, color)


F9 = Font(9)
F10 = Font(10)
F12 = Font(12)
F13 = Font(13)
F20 = Font(20)
F48 = Font(48)

# sample data (same as firmware demo strings)
STATUS = "WIFI NTP"
CLOCK = "12:00"
DATE = "Sunday 15 November 2026"   # worst-case (longest) date string
TEMP = "32.7"
UNIT = "\u00b0F"
DESC = "Overcast"
FEELS = "32.7 F"    # firmware format: buildTempStr = "%.1f " + unit char (no degree sign)
RAIN = "NO RAIN \u00b7 NEXT 1H"
ROW_L = "HUM 65% \u00b7 H 43.7\u00b0 L 34.5\u00b0"
ROW_R = "UPD 12:00"
HINT = "TOUCH TO REFRESH"


# ------------------------------------------------------------------ shapes ---
def draw_flat_sun(img, cx, cy):   # firmware drawFlatSun(267,23)
    d = ImageDraw.Draw(img)
    core, ray = hx("ffd75e"), hx("f0b429")
    dirs = [(0, -1), (1, 0), (0, 1), (-1, 0), (1, -1), (1, 1), (-1, 1), (-1, -1)]
    for dx, dy in dirs:
        m = math.sqrt(2) / 2 if dx and dy else 1.0
        d.line([(cx + int(round(6.5 * dx * m)), cy + int(round(6.5 * dy * m))),
                (cx + int(round(8.5 * dx * m)), cy + int(round(8.5 * dy * m)))], fill=ray, width=2)
    d.ellipse([cx - 6, cy - 6, cx + 6, cy + 6], fill=core)


def draw_split_cloud(img, x, y, small, color):   # firmware drawSplitCloud
    d = ImageDraw.Draw(img)
    if not small:   # pc1: 52x13 bar; bumps r9 @(+18,-7), r6 @(+33,+2)
        d.rounded_rectangle([x, y, x + 52 - 1, y + 13 - 1], radius=8, fill=color)
        d.ellipse([x + 18 - 9, y - 7 - 9, x + 18 + 9, y - 7 + 9], fill=color)
        d.ellipse([x + 33 - 6, y + 2 - 6, x + 33 + 6, y + 2 + 6], fill=color)
    else:           # pc2: 40x11 bar; bumps r7 @(+14,+1), r5 @(+25,+1)
        d.rounded_rectangle([x, y, x + 40 - 1, y + 11 - 1], radius=6, fill=color)
        d.ellipse([x + 14 - 7, y + 1 - 7, x + 14 + 7, y + 1 + 7], fill=color)
        d.ellipse([x + 25 - 5, y + 1 - 5, x + 25 + 5, y + 1 + 5], fill=color)


def draw_mini_cloud(img, x, y, color):   # firmware drawMiniCloud(13,129)
    d = ImageDraw.Draw(img)
    for cx, cy, r in ((x + 4, y + 7, 3), (x + 8, y + 5, 4), (x + 11, y + 7, 3)):
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=color)
    d.rectangle([x + 2, y + 7, x + 2 + 10, y + 7 + 3], fill=color)


def draw_umbrella(img, cx, cy, color, bg):   # firmware drawUmbrella k=1 (s=1)
    d = ImageDraw.Draw(img)
    r = max(2, int(5 * 1 + 0.5))
    ccy = cy - int(3.5 * 1)
    d.ellipse([cx - r, ccy - r, cx + r, ccy + r], fill=color)
    d.rectangle([cx - r, ccy + 1, cx + r - 1, ccy + r], fill=bg)   # keep top half
    sw = max(1, int(1.6 * 1))
    d.rectangle([cx - sw // 2, ccy + 1, cx - sw // 2 + sw - 1, ccy + 1 + int(7.5 * 1) - 1], fill=color)


def draw_feels_icon(img, ix, iy, color):   # firmware: vertical bar + two diagonals
    d = ImageDraw.Draw(img)
    d.line([(ix + 5, iy + 1), (ix + 5, iy + 9)], fill=color)
    d.line([(ix + 2, iy + 2), (ix + 8, iy + 7)], fill=color)
    d.line([(ix + 8, iy + 2), (ix + 2, iy + 7)], fill=color)


def draw_dog(img, path):   # firmware drawDogImage: bottom-right, kDogMargin=4
    if not path or not os.path.exists(path):
        return
    dog = Image.open(path).convert("RGBA")
    dw, dh = dog.size
    img.alpha_composite(dog, (W - dw - 4, H - dh - 4))


# ------------------------------------------------------------------- main ---
def render(mode):
    p = PAL[mode]
    img = Image.new("RGB", (W, H), p["left"])
    d = ImageDraw.Draw(img)

    # right panel gradient
    for y in range(H):
        c = panel_color(mode, y)
        d.line([(SPLIT_X, y), (W - 1, y)], fill=c)

    # sky decor on the panel
    if mode == "day":
        draw_flat_sun(img, 267, 23)
        draw_split_cloud(img, 238, 64, False, (255, 255, 255))
        draw_split_cloud(img, 270, 86, True, blend_white(panel_color(mode, 88), 75))
    else:
        d.ellipse([267 - 6, 22 - 6, 267 + 6, 22 + 6], fill=hx("ffc857"))
        d.ellipse([270 - 5, 20 - 5, 270 + 5, 20 + 5], fill=panel_color(mode, 22))
        for sx_, sy_ in ((226, 58), (300, 52), (270, 74), (292, 88)):
            d.rectangle([sx_, sy_, sx_ + 1, sy_ + 1], fill=hx("bcd7ff"))

    # DAY/NIGHT label (em10 @ scale 1) + ground ellipse under the dog
    F10.center(img, "DAY" if mode == "day" else "NIGHT", 267, 38, 1.0, p["sky_lbl"])
    d.ellipse([267 - 43, 231 - 5, 267 + 43, 231 + 5], fill=p["ground"])

    # status row: dot r2 @ (14,14) + text x=22 y=9 (em10 @ .95)
    d.ellipse([12, 12, 16, 16], fill=p["dot"])
    F10.draw(img, STATUS, 22, 9, 1.0, p["st_l"])

    # clock (em48 @ <=37/48) left x=12 y=24; date (em10 @1.0) x=13 y=64
    cs = F48.fit(CLOCK, 198, cap=37 / 48)
    F48.draw(img, CLOCK, 12, 24, cs, p["clock"])
    ds = F10.fit(DATE, 201)
    F10.draw(img, DATE, 13, 64, ds, p["date"])

    # temp (em48 @ <=43/48) + unit (em20 @ .95), baselines aligned at y=80
    ts = F48.fit(TEMP, 170, cap=43 / 48)
    F48.draw(img, TEMP, 12, 80, ts, p["temp"])
    tw = cround(F48.width(TEMP) * ts)
    uy = int(80 + F48.ascent * ts - F20.ascent * 0.95)   # firmware formula
    F20.draw(img, UNIT, 12 + tw + 6, uy, 0.95, p["unit"])

    # description row: mini cloud @ (13,129) + text x=32 y=128 (em13 @ <=12/13)
    draw_mini_cloud(img, 13, 129, p["desc_txt"])
    dsc = F12.fit(DESC, SPLIT_X - 32)
    F12.draw(img, DESC, 32, 128, dsc, p["desc_txt"])

    # FEELS chip [12,146,213,32] (design content-box: 190+20 pad+border -> 12..225)
    d.rounded_rectangle([12, 146, 224, 177], radius=9, fill=p["f_bg"], outline=p["f_bd"])
    draw_feels_icon(img, 24, 146 + (32 - 14) // 2, p["f_icon"])
    F9.draw(img, "FEELS LIKE", 46, 146 + (32 - 9) // 2, 1.0, p["f_lbl"])
    fvs = F20.fit(FEELS, 213 - 84, cap=0.9)
    vw = cround(F20.width(FEELS) * fvs)
    F20.draw(img, FEELS, 213 - vw, 146 + (32 - int(20 * fvs)) // 2, fvs, p["f_val"])

    # RAIN chip [12,184,213,27]: umbrella + text centered as a group
    d.rounded_rectangle([12, 184, 224, 210], radius=8, fill=p["r_bg"], outline=p["r_bd"])
    rsc = F12.fit(RAIN, 213 - 40)
    rw = cround(F12.width(RAIN) * rsc)
    gx = 12 + (213 - (13 + 6 + rw)) // 2
    draw_umbrella(img, gx + 6, 184 + 13, p["r_dot"], p["r_bg"])
    F12.draw(img, RAIN, gx + 19, 184 + (27 - int(12 * rsc)) // 2, rsc, p["r_txt"])

    # row1 @ y=217: left x=12; UPD right-aligned at x=202 (em9 @1.0)
    lsc = F9.fit(ROW_L, 202 - 12)
    F9.draw(img, ROW_L, 12, 217, lsc, p["row1_c"])
    rwid = cround(F9.width(ROW_R))
    F9.draw(img, ROW_R, 202 - rwid, 217, 1.0, p["row1_c"])

    # hint centered in [12..202] (em9 @1.0)
    F9.center(img, HINT, (12 + 202) // 2, 229, 1.0, p["hint"])

    draw_dog(img, sys.argv[1] if len(sys.argv) > 1 else None)
    out = os.path.join(ROOT, f"preview_theme3_{mode}.png")
    img.save(out)
    print(f"wrote {out}")


if __name__ == "__main__":
    render("day")
    render("night")
