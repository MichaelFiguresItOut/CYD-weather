#!/usr/bin/env python3
"""Render the BENTO theme (index 3 in the 4-theme numbering) day + night previews that mirror src/main.cpp.

Mirrors renderTheme3Nunito() (and drawBentoClockCard / drawBentoDogCard) line
for line: card rectangles, Nunito BFF text at the design sizes, accent bar,
FEELS chip with crossed-arrows icon, rain card with umbrella/dot, detail rows
and the centered dog. Colors are passed through RGB565 exactly like the
firmware's C() macro so the preview matches the panel.

Usage: python tools/preview_theme4.py
Outputs preview_theme4_day.png / preview_theme4_night.png in the repo root.
"""
import math
import os
import sys
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
from tools.preview_theme3 import Font, F9, F10, F12, F13, F20, F48, hx, cround  # noqa: E402

W, H = 320, 240


# ------------------------------------------------------------------ colors ---
def c565(rgb):
    r, g, b = rgb
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def e565(code):   # expand RGB565 -> 888 (firmware color space)
    r = (code >> 11) & 31
    g = (code >> 5) & 63
    b = code & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def C(r, g, b):
    return e565(c565((r, g, b)))


def HX(s):
    return C(*hx(s))


# Palettes: exact copies of bentoDay / bentoNight in src/main.cpp.
PAL = {
    "day": dict(
        bg=HX("eef4fa"),
        cardBg=C(0xff, 0xff, 0xff), cardBd=HX("d7e3f4"),
        clock=HX("16325c"), date=HX("4a6fa5"),
        stL=HX("2b6cb0"), dot=HX("2f9e6b"), stR=HX("c77e14"),
        accent=HX("5aa9ff"), temp=HX("16325c"), unit=HX("3d5f8f"),
        chipBg=HX("e3f1ff"), chipBd=HX("9cc7f5"), chipLbl=HX("2b6cb0"), chipVal=HX("1d4f8c"),
        descTxt=HX("5a7089"),
        rBg=HX("e6f7ec"), rBd=HX("8fd6ae"), rTxt=HX("237a52"), rDot=HX("2f9e6b"),
        detC=HX("7d92ad"), updC=HX("9fb0c4"),
        dogCardBg=HX("f4f9ff"), capC=HX("7d92ad"),
    ),
    "night": dict(
        bg=HX("131a26"),
        cardBg=HX("1c2534"), cardBd=HX("2c3a52"),
        clock=(255, 255, 255), date=HX("8fa3c0"),
        stL=HX("6fb3ff"), dot=HX("3ecf8e"), stR=HX("ffc857"),
        accent=HX("5aa9ff"), temp=(255, 255, 255), unit=HX("9fc3e8"),
        chipBg=HX("173a5f"), chipBd=HX("5aa9ff"), chipLbl=HX("9fd0ff"), chipVal=(255, 255, 255),
        descTxt=HX("c3d0e4"),
        rBg=HX("12352a"), rBd=HX("2f9e6b"), rTxt=HX("7ee2b8"), rDot=HX("3ecf8e"),
        detC=HX("8fa3c0"), updC=HX("5f7392"),
        dogCardBg=HX("1a2333"), capC=HX("6f87a8"),
    ),
}

# Sample data: the design's own strings (worst-case widths checked against avail).
STATUS = "WIFI NTP"
CLOCK = "1:03 AM"
DATE = "Saturday 14 November 2026"
TEMP = "38.2"
UNIT = "F"
FEELS = "32.7 F"
DESC = "Overcast"
RAIN = "NO RAIN EXPECTED"
D1 = "HUMIDITY 98%"
D2 = "TODAY H 43.7\u00b0 L 34.5\u00b0"
REF = "UPD 12:59 AM \u00b7 TOUCH TO REFRESH"
CAP = "OUTFIT \u00b7 VERY COLD"
DOG = {"day": "very_cold_sun.png", "night": "very_cold.png"}


# ------------------------------------------------------------------ shapes ---
def draw_card(img, x, y, w, h, bg, bd):   # firmware drawCard (r=10 / border r=9)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([x, y, x + w - 1, y + h - 1], radius=10, fill=bg)
    d.rounded_rectangle([x, y, x + w - 2, y + h - 2], radius=9, outline=bd)


def draw_mini_cloud(img, x, y, color):   # firmware drawMiniCloud
    d = ImageDraw.Draw(img)
    d.ellipse([x + 4 - 3, y + 7 - 3, x + 4 + 3, y + 7 + 3], fill=color)
    d.ellipse([x + 8 - 4, y + 5 - 4, x + 8 + 4, y + 5 + 4], fill=color)
    d.ellipse([x + 11 - 3, y + 7 - 3, x + 11 + 3, y + 7 + 3], fill=color)
    d.rectangle([x + 2, y + 7, x + 2 + 10, y + 7 + 3], fill=color)


def draw_umbrella(img, cx, cy, color, bg):   # firmware drawUmbrella k=1 (s=1)
    d = ImageDraw.Draw(img)
    r = max(2, int(5 * 1 + 0.5))
    ccy = cy - int(3.5 * 1)
    d.ellipse([cx - r, ccy - r, cx + r, ccy + r], fill=color)
    d.rectangle([cx - r, ccy + 1, cx + r - 1, ccy + r], fill=bg)   # keep top half
    sw = max(1, int(1.6 * 1))
    d.rectangle([cx - sw // 2, ccy + 1, cx - sw // 2 + sw - 1, ccy + 1 + int(7.5 * 1) - 1], fill=color)


def draw_dn_icon(img, cx, cy, color, bg, night):   # firmware drawDnIcon (sun / crescent)
    d = ImageDraw.Draw(img)
    if not night:
        d.ellipse([cx - 3, cy - 3, cx + 3, cy + 3], fill=color)
        dirs = [(0, -1), (1, 0), (0, 1), (-1, 0), (1, -1), (1, 1), (-1, 1), (-1, -1)]
        for dx, dy in dirs:
            m = math.sqrt(2) / 2 if dx and dy else 1.0
            d.line([(cx + int(round(5 * dx * m)), cy + int(round(5 * dy * m))),
                    (cx + int(round(6.5 * dx * m)), cy + int(round(6.5 * dy * m)))],
                   fill=color)
    else:
        d.ellipse([cx - 4, cy - 4, cx + 4, cy + 4], fill=color)
        d.ellipse([cx + 2 - 3, cy - 2 - 3, cx + 2 + 3, cy - 2 + 3], fill=bg)   # bite


def draw_feels_icon(img, ix, iy, color):   # firmware crossed-arrows (9px box)
    d = ImageDraw.Draw(img)
    d.line([(ix + 4, iy + 1), (ix + 4, iy + 8)], fill=color)
    d.line([(ix + 1, iy + 2), (ix + 8, iy + 7)], fill=color)
    d.line([(ix + 8, iy + 2), (ix + 1, iy + 7)], fill=color)


def draw_dog(img, mode):
    path = os.path.join(ROOT, "dogs", DOG[mode])
    if not os.path.exists(path):
        return
    dog = Image.open(path).convert("RGBA")
    fw, fh = dog.size
    x = 206 + (106 - fw) // 2
    y = 70 + (134 - fh) // 2
    img.paste(dog, (x, y), dog)


# -------------------------------------------------------------------- render ---
def render(mode):
    p = PAL[mode]
    img = Image.new("RGB", (W, H), p["bg"])
    d = ImageDraw.Draw(img)
    night = mode == "night"

    # ---- Clock card (8,8,304,52) -------------------------------------------
    draw_card(img, 8, 8, 304, 52, p["cardBg"], p["cardBd"])
    # status row: dot r3 @ (18,27) + WIFI NTP em9 @ (24,25); DAY/NIGHT + icon
    d.ellipse([18 - 3, 27 - 3, 18 + 3, 27 + 3], fill=p["dot"])
    F9.draw(img, STATUS, 24, 25, 1.0, p["stL"])
    dn = "NIGHT" if night else "DAY"
    dw = F9.width(dn)
    F9.draw(img, dn, 304 - dw, 25, 1.0, p["stR"])
    draw_dn_icon(img, 304 - dw - 9, 29, p["stR"], p["cardBg"], night)
    # clock em48 @ <=27/48 centered; date em10 @ <=288px
    cs = F48.fit(CLOCK, 290, cap=27 / 48)
    F48.center(img, CLOCK, 160, 11, cs, p["clock"])
    ds = F10.fit(DATE, 288)
    F10.center(img, DATE, 160, 41, ds, p["date"])

    # ---- Temp card (8,66,192,72) + accent bar --------------------------------
    draw_card(img, 8, 66, 192, 72, p["cardBg"], p["cardBd"])
    d.rounded_rectangle([7, 74, 7 + 3, 74 + 55], radius=2, fill=p["accent"])
    ts = F48.fit(TEMP, 140, cap=34 / 48)
    F48.draw(img, TEMP, 24, 74, ts, p["temp"])
    tw = cround(F48.width(TEMP) * ts)
    uy = int(74 + F48.ascent * ts - F20.ascent * 0.75)
    F20.draw(img, UNIT, 24 + tw + 6, uy, 0.75, p["unit"])

    # ---- FEELS chip (132,74,60,40) -------------------------------------------
    d.rounded_rectangle([132, 74, 191, 113], radius=10, fill=p["chipBg"])
    d.rounded_rectangle([132, 74, 190, 112], radius=9, outline=p["chipBd"])
    ix = 162 - (9 + 3 + F9.width("FEELS")) // 2
    iy = 81
    draw_feels_icon(img, ix, iy, p["chipLbl"])
    F9.draw(img, "FEELS", ix + 9 + 3, 81, 1.0, p["chipLbl"])
    fvs = F20.fit(FEELS, 44, cap=14 / 20)
    F20.center(img, FEELS, 162, 95, fvs, p["chipVal"])

    # ---- Description row: mini cloud @ (25,113) + em12 text @ (42,112) --------
    draw_mini_cloud(img, 25, 113, p["descTxt"])
    dsc = F12.fit(DESC, 154)
    F12.draw(img, DESC, 42, 112, dsc, p["descTxt"])

    # ---- Rain card (8,144,192,34) --------------------------------------------
    d.rounded_rectangle([8, 144, 199, 177], radius=10, fill=p["rBg"])
    d.rounded_rectangle([8, 144, 198, 176], radius=9, outline=p["rBd"])
    rsc = F13.fit(RAIN, 161)
    rw = cround(F13.width(RAIN) * rsc)
    wet = True   # preview the umbrella variant
    iw = 13 if wet else 6
    gx = 8 + (192 - (iw + 7 + rw)) // 2
    if wet:
        draw_umbrella(img, gx + 6, 163, p["rDot"], p["rBg"])
    else:
        d.ellipse([gx + 3 - 3, 161 - 3, gx + 3 + 3, 161 + 3], fill=p["rDot"])
    F13.draw(img, RAIN, gx + iw + 7, 154, rsc, p["rTxt"])

    # ---- Detail card (8,184,192,48): rows @ y=194/207/221 ---------------------
    draw_card(img, 8, 184, 192, 48, p["cardBg"], p["cardBd"])
    F10.draw(img, D1, 22, 194, 1.0, p["detC"])
    F10.draw(img, D2, 22, 207, 1.0, p["detC"])
    F9.draw(img, REF, 22, 221, 1.0, p["updC"])

    # ---- Dog card (206,66,106,166) --------------------------------------------
    draw_card(img, 206, 66, 106, 166, p["dogCardBg"], p["cardBd"])
    cap = CAP if cround(F9.width(CAP)) <= 96 else "VERY COLD"
    F9.center(img, cap, 259, 207, 1.0, p["capC"])
    draw_dog(img, mode)

    out = os.path.join(ROOT, f"preview_theme4_{mode}.png")
    img.save(out)
    print(f"wrote {out}")


if __name__ == "__main__":
    render("day")
    render("night")
