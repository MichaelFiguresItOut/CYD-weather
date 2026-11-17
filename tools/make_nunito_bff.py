#!/usr/bin/env python3
"""Generate LovyanGFX BFF (4bpp alpha) font masters from Nunito for the CYD Sky theme.

Reads:  tools/fonts/Nunito-wght.ttf   (Nunito variable font, weight axis 200..1000)
Writes: src/nunito_bff.h              (C arrays + per-master metrics)

BFF layout implemented here (matches BFFfont::loadFont in lgfx_fonts.cpp):
  file = records; record = u32 rec_size | marker[4] | payload(rec_size-8)
  head: version(4) extra_tables(2) font_size(2) ascent(2) descent(2s) typo_asc(2)
        typo_desc(2s) line_gap(2) min_y(2s) max_y(2s) default_advance_width(2)
        kerning_scale(2) index_to_loc_format(1) glyph_id_format(1)
        advance_width_format(1) bits_per_pixel(1)=4 bbox_xy_bits(1) bbox_wh_bits(1)
        advance_width_bits(1) compression_algorithm(1)=0 subpixel_rendering(1)=0
  cmap: u32 count; per subtable (16B): data_offset(u32, payload-relative),
        range_start(u32), range_length(u16), glyph_id_offset(u16), entries_count(u16),
        format_type(u8)=1 sparse (+1 pad); data = [u16 delta x N][u16 gid x N] sorted by delta
  loca: u32 count; offsets (u32, index_to_loc_format=1) relative to glyf PAYLOAD start
  glyf: per glyph (region starts at a byte boundary; loca = payload-relative byte offsets):
    ONE continuous bitstream, MSB-first:
      advance(awb) | bbox_x signed(xyb) | bbox_y signed(xyb) | w(whb) | h(whb)
      then w*h 4bpp values packed immediately after the header bits (bit position
      header_bits = awb+2*xyb+2*whb, NOT byte-aligned - decodeGlyphBitmap sets
      bs.bit_pos = header_bits). Region length = ceil((header_bits + w*h*4)/8) bytes.
  bbox placement (verified against LGFXBase::draw_string + BFFfont::drawChar in lgfx_fonts.cpp):
    draw_string does y -= y_offset (= +ascent) before the char loop; drawChar then does
    y += y_offset (= -ascent), so they cancel, and bitmap row 0 lands at:
      abs_top = user_y + baseline - (bbox_y + h)
    with bbox_y stored as bottom position from baseline (up-positive). For datum top_left
    the line box is [user_y, user_y+em] with the baseline B rows down, so this places ink
    correctly: e.g. 'A' (by=0,h=7,B=9) -> abs_top = user_y + 2.
    In generator terms: by = -(ink bottom below baseline) == bottom-from-baseline (up-positive).
"""
import math
import struct
from PIL import Image, ImageDraw, ImageFont

FONT_PATH = "tools/fonts/Nunito-wght.ttf"
WEIGHT = 700                      # Nunito Bold (design uses 600-800)
# ASCII printable + degree sign (0xB0) + middle dot (0xB7). Raw high-bit bytes
# work as codepoints because LGFX utf8 mode is off in main.cpp.
CODEPOINTS = list(range(32, 127)) + [0xB0, 0xB7]
# em sizes covering the Sky theme's text sizes. 9/12 were added because the
# the design's fractional sizes (10.5/12/11.5/9.5/9px) were being reproduced with
# sub-1.0 BFF scaling, and LGFX's nearest-neighbor scaler DROPS 1px source
# columns when downscaling (see docs/CYD-HARDWARE.md post-mortem) - thin strokes vanish and
# small text comes out garbled. Exact-size masters at scale 1.0 are always crisp.
MASTERS = [9, 10, 12, 13, 20, 48]


class BitWriter:
    def __init__(self):
        self.buf = bytearray()
        self.pos = 0

    def write(self, val, bits):
        for i in range(bits - 1, -1, -1):          # MSB first
            if (val >> i) & 1:
                bi = self.pos // 8
                while len(self.buf) <= bi:
                    self.buf.append(0)
                self.buf[bi] |= 0x80 >> (self.pos % 8)
            self.pos += 1

    def to_bytes(self):
        # Pad to a full byte boundary. CRITICAL: BFFfont::loadGlyphInfo requires
        # region length >= ceil(header_bits/8); dropping trailing zero bytes would
        # truncate the header of pixel-less glyphs (space, .notdef) and make the
        # firmware fall back to drawCharDummy (box outline at every space).
        n = (self.pos + 7) // 8
        if len(self.buf) < n:
            self.buf.extend(b"\x00" * (n - len(self.buf)))
        return bytes(self.buf[:n])


def signed_bits_needed(m):
    """Smallest n such that all ints in [-m, m] fit in n-bit two's complement."""
    n = 1
    while (1 << (n - 1)) <= m:
        n += 1
    return max(1, n)


def nibble(v):
    """Map a 0..255 coverage pixel to a 4bpp alpha value.

    Thresholded to binary {0,15} on purpose: the CYD glass displays bit-inverted
    pixels and LGFX blends AA glyph edges against the base color (black) in code
    space, so any intermediate alpha shows up as a bright halo around letters on
    this panel. Binary coverage is never blended -> no halo, identical everywhere.
    """
    return 15 if v >= 128 else 0


def load_font(em):
    f = ImageFont.truetype(FONT_PATH, size=em)
    f.set_variation_by_axes([WEIGHT])
    return f


def glyph_data(f, ch):
    """Rasterize one char; pen origin (baseline start) at (PX, PY)."""
    PX, PY = 64, 96
    img = Image.new("L", (256, 256), 0)
    d = ImageDraw.Draw(img)
    d.text((PX, PY), ch, font=f, fill=255, anchor="ls")  # 'ls' = left-baseline pen origin
    adv = int(round(f.getlength(ch)))
    bbox = img.getbbox()                            # ink bbox or None
    if bbox is None:
        return {"adv": adv, "w": 0, "h": 0, "bx": 0, "by": 0, "bmp": b""}
    l, t, r, b = bbox
    w, h = r - l, b - t
    bx = l - PX                                     # left bearing (right +)
    by = -(b - PY)                                  # bottom from baseline (up +)
    bmp = img.crop((l, t, r, b))                    # 'L' AA bitmap
    return {"adv": adv, "w": w, "h": h, "bx": bx, "by": by, "bmp": bmp}


def build_master(em):
    f = load_font(em)
    ft_asc, ft_desc = f.getmetrics()                # FreeType ascent/descent at em
    T = ft_asc + ft_desc

    glyphs = [glyph_data(f, chr(cp)) for cp in CODEPOINTS]
    space_adv = glyphs[0]["adv"]                    # ' ' is cp 32

    # Line box of height E=em with the FreeType content area centered (CSS line-height:1).
    # Small masters may overflow the box by a pixel or two (no clipping in BFF; rows have gaps).
    B = int(math.floor(ft_asc - (T - em) / 2.0 + 0.5))   # baseline from top of line box
    tops = [g["by"] + g["h"] for g in glyphs if g["h"]]   # ink top above baseline (by+h)
    bots = [-g["by"] for g in glyphs if g["h"]]           # ink bottom below baseline
    print(f"  em{em}: FT asc/desc={ft_asc}/{ft_desc} line-box B={B} "
          f"top_overflow={max(0, max(tops) - B)}px bottom_overflow={max(0, B + max(bots) - em)}px")

    awb = max(1, max(g["adv"] for g in glyphs).bit_length())
    lo = min([g["bx"] for g in glyphs] + [g["by"] for g in glyphs])
    hi = max([g["bx"] for g in glyphs] + [g["by"] for g in glyphs])
    xyb = signed_bits_needed(max(abs(lo), abs(hi)))
    whb = max(1, max(g["w"] for g in glyphs).bit_length(),
              max(g["h"] for g in glyphs).bit_length())

    # ---- glyf payload (gid 0 = .notdef empty, then cp 32..126) ----
    glyf = bytearray()
    loca = [0] * (len(CODEPOINTS) + 1)
    bw = BitWriter()
    bw.write(space_adv, awb); bw.write(0, xyb); bw.write(0, xyb); bw.write(0, whb); bw.write(0, whb)
    glyf += bw.to_bytes()                            # gid 0: header only
    for i, g in enumerate(glyphs):
        loca[i + 1] = len(glyf)
        bw = BitWriter()
        bw.write(g["adv"], awb); bw.write(g["bx"] & 0xFFFFFFFF, xyb)
        bw.write(g["by"] & 0xFFFFFFFF, xyb)   # bottom from baseline (up-positive), see docstring
        bw.write(g["w"], whb); bw.write(g["h"], whb)
        if g["bmp"]:
            for v in g["bmp"].getdata():       # nibble bits continue the same bitstream
                bw.write(nibble(v), 4)
        glyf += bw.to_bytes()

    # ---- head payload (parser reads buf[0..34]; pad to 36 so record size >= 44) ----
    head = struct.pack(
        "<IHHHhHhHhhHHBBBBBBBBB",
        1,                    # version
        0,                    # extra tables
        em,                   # font_size (line box height at scale 1.0)
        B,                    # ascent  (baseline from top of line box)
        -(em - B),            # descent (negative; ascent+|descent| == em)
        B,                    # typo_ascent
        -(em - B),            # typo_descent
        0,                    # typo_line_gap
        -(em - B),            # min_y
        B,                    # max_y
        space_adv,            # default_advance_width
        0,                    # kerning_scale
        1,                    # index_to_loc_format (u32 offsets)
        0,                    # glyph_id_format
        0,                    # advance_width_format (raw px)
        4,                    # bits_per_pixel
        xyb,                  # bbox_xy_bits
        whb,                  # bbox_wh_bits
        awb,                  # advance_width_bits
        0,                    # compression_algorithm (none)
        0,                    # subpixel_rendering
    )
    head += b"\x00"
    assert len(head) == 36, len(head)

    # ---- cmap payload: one sparse subtable per contiguous run of CODEPOINTS ----
    # format_type=1 (sparse): data = [u16 delta x N][u16 gid x N], sorted by delta.
    runs = []
    for cp in sorted(set(CODEPOINTS)):
        if runs and cp == runs[-1][1] + 1:
            runs[-1][1] = cp
        else:
            runs.append([cp, cp])
    gid_of = {cp: i + 1 for i, cp in enumerate(CODEPOINTS)}   # gid 0 reserved (.notdef)
    blocks = []
    for (lo, hi) in runs:
        cps = sorted(cp for cp in CODEPOINTS if lo <= cp <= hi)
        deltas = struct.pack(f"<{len(cps)}H", *[cp - lo for cp in cps])   # ascending
        gids = struct.pack(f"<{len(cps)}H", *(gid_of[cp] for cp in cps))
        blocks.append((lo, hi - lo + 1, len(cps), deltas + gids))
    hdr_off = 4                                              # payload-relative
    data_off = hdr_off + 16 * len(blocks)
    cmap = struct.pack("<I", len(blocks))                    # subtable count (u32!)
    for (lo, ln, nent, data) in blocks:
        cmap += struct.pack("<IIHHHBx", data_off, lo, ln, 0, nent, 1)
        data_off += len(data)
    for (_, _, _, data) in blocks:
        cmap += data

    # ---- records ----
    def record(marker, payload):
        return struct.pack("<I", len(payload) + 8) + marker + payload

    blob = b"".join([record(b"head", head), record(b"cmap", cmap),
                     record(b"loca", struct.pack("<I", len(loca)) +
                               struct.pack(f"<{len(loca)}I", *loca)),
                     record(b"glyf", bytes(glyf))])

    return {
        "em": em, "ascent": B, "descent": em - B, "space_adv": space_adv,
        "ft_asc": ft_asc, "ft_desc": ft_desc, "blob": blob,
        "awb": awb, "xyb": xyb, "whb": whb,
    }


def to_c_array(name, data):
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        lines.append("  " + ", ".join(f"0x{b:02x}" for b in chunk) + ("," if i + 16 < len(data) else ""))
    return f"static const uint8_t {name}[] = {{\n" + "\n".join(lines) + "\n};\n"


def main():
    masters = [build_master(em) for em in MASTERS]

    out = []
    out.append("// AUTO-GENERATED by tools/make_nunito_bff.py - do not edit.")
    out.append(f"// Nunito (weight {WEIGHT}) rasterized to LovyanGFX BFF masters (4bpp, binary coverage).")
    out.append("#pragma once\n#include <cstdint>\n")
    for m in masters:
        name = f"nunito_em{m['em']}"
        out.append(f"// em={m['em']}  line-box ascent={m['ascent']} descent={m['descent']} "
                   f"(FT asc/desc {m['ft_asc']}/{m['ft_desc']}) space adv={m['space_adv']} "
                   f"bits aw/xy/wh={m['awb']}/{m['xyb']}/{m['whb']}  size={len(m['blob'])}B")
        out.append(to_c_array(f"{name}_data", m["blob"]))

    out.append("struct NunitoMaster { const uint8_t* data; int size, em, ascent, descent, spaceAdv; };")
    rows = []
    for i, m in enumerate(masters):
        rows.append(f"  {{ nunito_em{m['em']}_data, sizeof(nunito_em{m['em']}_data), "
                    f"{m['em']}, {m['ascent']}, {m['descent']}, {m['space_adv']} }}")
    out.append(f"static const NunitoMaster NUNITO_MASTERS[{len(masters)}] = {{\n" +
               ",\n".join(rows) + "\n};\n")

    with open("src/nunito_bff.h", "w", newline="\r\n") as fh:
        fh.write("\n".join(out))

    total = sum(len(m["blob"]) for m in masters)
    print(f"wrote src/nunito_bff.h  ({len(masters)} masters, {total} bytes of font data)")
    for m in masters:
        print(f"  em={m['em']:>2}: blob={len(m['blob']):>6}B asc={m['ascent']} desc={m['descent']} "
              f"space={m['space_adv']} bits aw/xy/wh={m['awb']}/{m['xyb']}/{m['whb']}")


if __name__ == "__main__":
    main()
