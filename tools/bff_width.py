#!/usr/bin/env python3
"""Measure BFF string widths (scale 1.0) for the em sizes in src/nunito_bff.h.

Uses the same parser as inspect_bff.py; advance logic mirrors LGFX's
BFFfont::updateFontMetric (glyph x_advance, default advance on miss).
"""
import re, struct, sys
sys.path.insert(0, "tools")
from inspect_bff import parse_bff, parse_master

def width_of(M, s):
    # build cp -> gid map
    cp2gid = {}
    for gid in range(1, M["n"]):
        cp = M["cp_of"](gid)
        if cp is not None:
            cp2gid[cp] = gid
    w = 0
    for ch in s:
        cp = ord(ch)
        gid = cp2gid.get(cp)
        if gid is None or gid not in M["glyphs"]:
            w += M["dadv"]
        else:
            w += M["glyphs"][gid]["adv"]
    return w

if __name__ == "__main__":
    strings = [
        "Rain expected 10:45 AM tomorrow",
        "Rain possible 10:45 AM tomorrow",
        "RAIN NOW - ends ~10:45 PM",
        "RAIN NOW - MAY CONTINUE",
        "RAIN NEARBY (LGA/EWR)",
        "No rain expected",
        "NO WEATHER DATA",
    ]
    masters = parse_bff()
    for em in (9, 10, 12, 13):
        M = parse_master(em, masters[em]["data"])
        print(f"em{em} (asc={M['ascent']} desc={M['descent']} dadv={M['dadv']}):")
        for s in strings:
            print(f"  {width_of(M, s):4d}  {s}")
