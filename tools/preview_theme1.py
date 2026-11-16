#!/usr/bin/env python3
"""Preview Theme 1 (SKY) with the generated Nunito BFF fonts.

Parses src/nunito_bff.h and replicates the firmware's BFF parsing + placement
math EXACTLY (BFFfont::loadFont / mapCodepointToGlyph / decodeGlyphBitmap /
drawChar in lgfx_fonts.cpp, plus LGFXBase::draw_string datum handling), so a
correct preview here guarantees correct rendering on the CYD.

Placement (datum top_left, scale 1):
    abs_top  = user_y + baseline - (bbox_y + h)     # bbox_y: bottom from baseline, up-positive
    abs_left = user_x + bbox_x
    advance  = adv (raw px)

The generated glyphs use BINARY coverage (alpha thresholded to {0,15} in the
generator), so no intermediate-alpha blending ever happens on glass - this is
what kills the bright halo that AA edges produced on the inverted panel. The
blend branch below therefore only sees a==0 or a==15 for these fonts.

Writes preview_theme1_day.png / preview_theme1_night.png (320x240).
"""
import re
import struct
from PIL import Image, ImageDraw

HDR_PATH = "src/nunito_bff.h"


# ---------------------------------------------------------------- header parse
def load_masters(path):
    src = open(path, encoding="utf-8").read()
    masters = {}
    for m in re.finditer(r"static const uint8_t (nunito_em(\d+))_data\[\] = \{(.*?)\};",
                         src, re.S):
        name, em, body = m.group(1), int(m.group(2)), m.group(3)
        blob = bytes(int(t, 16) for t in re.findall(r"0x[0-9a-f]{2}", body))
        masters[em] = BFFFont(blob)
    return masters


# ---------------------------------------------------------------- BFF parser
class Glyph:
    __slots__ = ("adv", "bx", "by", "w", "h", "px")

    def __init__(self, adv, bx, by, w, h, px):
        self.adv, self.bx, self.by, self.w, self.h, self.px = adv, bx, by, w, h, px


class BFFFont:
    """Mirrors BFFfont (lgfx_fonts.cpp) for the subset this generator emits."""

    def __init__(self, blob):
        self.blob = blob
        self.pos = 0
        self.head = None
        self.cmap = {}          # codepoint -> gid
        self.loca = []
        self.glyf_off = 0       # absolute offset of glyf PAYLOAD start in blob
        self._parse_records()

    def _u32(self, off):
        return struct.unpack_from("<I", self.blob, off)[0]

    def _parse_records(self):
        n = len(self.blob)
        while self.pos < n:
            rec_size = self._u32(self.pos)
            marker = self.blob[self.pos + 4:self.pos + 8]
            payload = self.pos + 8
            if marker == b"head":
                (version, extra, font_size, ascent, descent, typo_a, typo_d, gap,
                 min_y, max_y, def_adv, kern_scale, itl, gid_fmt, aw_fmt, bpp,
                 xyb, whb, awb, comp, subpx) = struct.unpack_from(
                    "<IHHHhHhHhhHHBBBBBBBBB", self.blob, payload)
                self.head = dict(font_size=font_size, ascent=ascent, descent=descent,
                                 def_adv=def_adv, bpp=bpp, xyb=xyb, whb=whb, awb=awb,
                                 comp=comp)
            elif marker == b"cmap":
                count = self._u32(payload)
                for i in range(count):
                    so = payload + 4 + i * 16
                    data_off, range_start, range_len, gid_off, entries, fmt = \
                        struct.unpack_from("<IIHHHB", self.blob, so)
                    assert fmt == 1, f"unexpected cmap format {fmt}"
                    do = payload + data_off
                    for j in range(entries):
                        delta = struct.unpack_from("<H", self.blob, do + j * 2)[0]
                        gid = struct.unpack_from("<H", self.blob, do + entries * 2 + j * 2)[0]
                        self.cmap[range_start + delta] = gid
            elif marker == b"loca":
                count = self._u32(payload)
                assert count == self.head is not None or True
                self.loca = [self._u32(payload + 4 + i * 4) for i in range(count)]
            elif marker == b"glyf":
                self.glyf_off = payload
            self.pos += rec_size
        assert self.head, "no head record parsed"

    # -- glyph decode: mirrors loadGlyphInfo + decodeGlyphBitmap --------------
    def _glyph(self, gid):
        h = self.head
        off = self.loca[gid]
        nxt = self.loca[gid + 1] if gid + 1 < len(self.loca) else (len(self.blob) - self.glyf_off)
        region = self.blob[self.glyf_off + off:self.glyf_off + nxt]

        header_bits = h["awb"] + 2 * h["xyb"] + 2 * h["whb"]
        bitpos = 0

        def rbits(count):
            nonlocal bitpos
            v = 0
            for _ in range(count):
                byte_i, bit_i = bitpos >> 3, 7 - (bitpos & 7)
                if byte_i >= len(region):
                    break
                v = (v << 1) | ((region[byte_i] >> bit_i) & 1)
                bitpos += 1
            return v

        def rsbits(count):
            v = rbits(count)
            sign = 1 << (count - 1)
            if v & sign:
                v |= ~0 << count
            return v

        adv = rbits(h["awb"]) or h["def_adv"]
        bx, by = rsbits(h["xyb"]), rsbits(h["xyb"])
        w, hh = rbits(h["whb"]), rbits(h["whb"])
        px = [rbits(4) for _ in range(w * hh)] if w and hh else []
        return Glyph(adv, bx, by, w, hh, px)

    # -- text metrics / drawing -------------------------------------------------
    def width(self, s):
        return sum(self._glyph(self.cmap.get(ord(c), 0)).adv for c in s)

    def draw(self, img, x, y, s, color):
        """Draw string at (x,y) = line-box top-left (datum top_left, scale 1)."""
        B = self.head["ascent"]
        px = img.load()
        W, H = img.size
        cr, cg, cb = color
        for c in s:
            g = self._glyph(self.cmap.get(ord(c), 0))
            if not (g.w and g.h):
                x += g.adv
                continue
            top = y + B - g.by - g.h          # verified firmware math
            left = x + g.bx
            for j in range(g.h):
                iy = top + j
                if 0 <= iy < H:
                    row = (j * g.w)
                    for i in range(g.w):
                        ix = left + i
                        if not (0 <= ix < W):
                            continue
                        a = g.px[row + i]
                        if a == 0:
                            continue
                        r, gg, b = px[ix, iy][:3]
                        if a >= 15:
                            px[ix, iy] = (cr, cg, cb)
                        else:
                            m = 15 // 2
                            nr = r + ((cr - r) * a + m) // 15
                            ng = gg + ((cg - gg) * a + m) // 15
                            nb = b + ((cb - b) * a + m) // 15
                            px[ix, iy] = (nr, ng, nb)
            x += g.adv


# ---------------------------------------------------------------- theme 1 layout
SKY_DAY = dict(
    clock=(0x16, 0x32, 0x5c), date=(0x3d, 0x6a, 0x9e), temp=(0x16, 0x32, 0x5c),
    unit=(0x3d, 0x5f, 0x8f), desc=(0x40, 0x58, 0x7a), tiny=(0x5b, 0x7a, 0x9e),
    hint=(0x7d, 0x92, 0xad), stL=(0x2b, 0x6c, 0xb0), dot=(0x2f, 0x9e, 0x6b),
    stR=(0xc7, 0x7e, 0x14), fBg=(0xdf, 0xf0, 0xff), fBd=(0x8f, 0xc3, 0xf2),
    fLbl=(0x2b, 0x6c, 0xb0), fVal=(0x1d, 0x4f, 0x8c), rBg=(0xe9, 0xf8, 0xef),
    rBd=(0x7c, 0xcf, 0x9f), rTxt=(0x23, 0x7a, 0x52), rDot=(0xff, 0xc8, 0x57))
SKY_NIGHT = dict(
    clock=(0xff, 0xff, 0xff), date=(0x9f, 0xc3, 0xe8), temp=(0xff, 0xff, 0xff),
    unit=(0xcf, 0xe3, 0xff), desc=(0xd7, 0xe3, 0xf4), tiny=(0x8a, 0xa3, 0xc0),
    hint=(0x6f, 0x87, 0xa8), stL=(0x6f, 0xb3, 0xff), dot=(0x3e, 0xcf, 0x8e),
    stR=(0xff, 0xc8, 0x57), fBg=(0x25, 0x63, 0xab), fBd=(0x7c, 0xc0, 0xff),
    fLbl=(0xbc, 0xdc, 0xff), fVal=(0xff, 0xff, 0xff), rBg=(0x12, 0x3f, 0x2c),
    rBd=(0x3e, 0xcf, 0x8e), rTxt=(0x7e, 0xe2, 0xb8), rDot=(0xff, 0xc8, 0x57))

GRAD_DAY = [(0x8f, 0xc9, 0xef), (0xcf, 0xe9, 0xfb), (0xea, 0xf6, 0xff)]
GRAD_NIGHT = [(0x0a, 0x1c, 0x38), (0x17, 0x39, 0x6b), (0x1d, 0x47, 0x7f)]


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t + 0.5) for i in range(3))


def sky_bg(night):
    stops = GRAD_NIGHT if night else GRAD_DAY
    rows = []
    for y in range(240):
        t = y / 239.0
        if t < 0.55:
            c = lerp(stops[0], stops[1], t / 0.55)
        else:
            c = lerp(stops[1], stops[2], (t - 0.55) / 0.45)
        rows.append(c)
    return rows


def fit_scale(font, s, avail):
    w = font.width(s)
    return min(1.0, avail / w) if w > avail else 1.0


def draw_cloud(d, x, y, k, color):
    """Mirror of firmware drawCloud(): rounded bar + two bumps."""
    s = k
    W = max(2, int(52 * s + 0.5))
    Hh = max(3, int(16 * s + 0.5))
    d.rounded_rectangle([x, y, x + W - 1, y + Hh - 1], radius=min(Hh // 2, int(8 * s + 0.5)), fill=color)
    r1 = max(1, int(11 * s + 0.5))
    d.ellipse([x + int(23 * s) - r1, y + int(2 * s) - r1, x + int(23 * s) + r1, y + int(2 * s) + r1], fill=color)
    r2 = max(1, int(8 * s + 0.5))
    d.ellipse([x + int(42 * s) - r2, y + int(2 * s) - r2, x + int(42 * s) + r2, y + int(2 * s) + r2], fill=color)


def render(night, out_path, data):
    F = load_masters(HDR_PATH)
    f10, f13, f20, f48 = F[10], F[13], F[20], F[48]
    p = SKY_NIGHT if night else SKY_DAY

    img = Image.new("RGB", (320, 240))
    d = ImageDraw.Draw(img)
    for y, c in enumerate(sky_bg(night)):
        d.line([(0, y), (319, y)], fill=c)

    # sky clouds (day only; night has stars) - drawn before text so text overlaps them
    if not night:
        draw_cloud(d, 170, 34, 1.0, (255, 255, 255))
        draw_cloud(d, 44, 52, 1.0, (255, 255, 255))

    # status row: dot + WIFI text left; sun/moon + DAY/NIGHT right of 310
    dr = max(1, int(2.5 * 1.2 + 0.5))
    d.ellipse([10 + dr - dr, 7 + int(4 * 1.2) - dr, 10 + dr + dr, 7 + int(4 * 1.2) + dr], fill=p["dot"])
    f10.draw(img, 19, 7, data["wifi"], p["stL"])
    rw = f10.width(data["dn"])
    ix = 310 - rw - int(9 * 1.2)
    if night:
        d.ellipse([ix - 4, 12 - 4, ix + 4, 12 + 4], fill=p["stR"])
        d.ellipse([ix - 4 + 2, 12 - 4 - 2, ix + 4 + 2, 12 + 2], fill=sky_bg(night)[12])
    else:
        d.ellipse([ix - 3, 9, ix + 3, 15], fill=p["stR"])
    f10.draw(img, 310 - rw, 7, data["dn"], p["stR"])

    # clock (em48) centered y=19; date (em13) centered y=62
    cw = f48.width(data["clock"])
    f48.draw(img, int((320 - cw) / 2), 19, data["clock"], p["clock"])
    dw = f13.width(data["date"])
    f13.draw(img, int(160 - dw / 2), 62, data["date"], p["date"])

    # temp (em48) at (14,76); unit (em20) baseline-aligned to the digits' baseline
    ts = fit_scale(f48, data["temp"], max(40, 204 - f20.width(data["unit"]) - 10))
    tw = int(f48.width(data["temp"]) * ts) if ts < 1 else f48.width(data["temp"])
    f48.draw(img, 14, 76, data["temp"], p["temp"])          # scale-1 path (ts==1 expected)
    base = 76 + f48.head["ascent"]                            # digits' baseline row
    uy = base - f20.head["ascent"]                            # unit line-box top -> same baseline
    f20.draw(img, 14 + tw + 6, uy, data["unit"], p["unit"])

    # cloud icon (drawCloud @ k=0.38) + description (em20) at (40,129)
    s = 0.38
    Wc = max(2, int(52 * s + 0.5))
    Hc = max(3, int(16 * s + 0.5))
    d.rounded_rectangle([16, 132, 16 + Wc - 1, 132 + Hc - 1], radius=min(Hc // 2, int(8 * s + 0.5)), fill=p["desc"])
    r1 = max(1, int(11 * s + 0.5))
    d.ellipse([16 + int(23 * s) - r1, 132 + int(2 * s) - r1, 16 + int(23 * s) + r1, 132 + int(2 * s) + r1], fill=p["desc"])
    r2 = max(1, int(8 * s + 0.5))
    d.ellipse([16 + int(42 * s) - r2, 132 + int(2 * s) - r2, 16 + int(42 * s) + r2, 132 + int(2 * s) + r2], fill=p["desc"])
    f20.draw(img, 40, 129, data["desc"], p["desc"])

    # tiny row (em10) centered x=116 y=148
    twd = f10.width(data["tiny"])
    f10.draw(img, int(116 - twd / 2), 148, data["tiny"], p["tiny"])

    # feels chip (14,162,204,30)
    d.rounded_rectangle([14, 162, 14 + 204 - 1, 162 + 30 - 1], radius=9, fill=p["fBg"])
    d.rounded_rectangle([14, 162, 14 + 204 - 2, 162 + 30 - 2], radius=8, outline=p["fBd"])
    f10.draw(img, 14 + int(10 * 1.25), 172, "FEELS LIKE", p["fLbl"])
    vw = f20.width(data["feels"])
    f20.draw(img, 14 + 204 - 12 - vw, 168, data["feels"], p["fVal"])

    # rain chip (14,198,204,28)
    d.rounded_rectangle([14, 198, 14 + 204 - 1, 198 + 28 - 1], radius=8, fill=p["rBg"])
    d.rounded_rectangle([14, 198, 14 + 204 - 2, 198 + 28 - 2], radius=7, outline=p["rBd"])
    rtw = f13.width(data["rain"])
    cx = 14 + 204 // 2
    icon_cx = int(cx - rtw / 2) - 9
    if data.get("umbrella"):
        uc = (0x7c, 0xc0, 0xff) if night else (0x2f, 0x7f, 0xd0)
        d.ellipse([icon_cx - 4, 212 - 4 - 3, icon_cx + 4, 212 + 4 - 3], fill=uc)   # canopy (top half)
        d.rectangle([icon_cx - 1, 212 - 3, icon_cx + 1, 212 + 5], fill=uc)          # shaft
    else:
        d.ellipse([icon_cx - 4, 208, icon_cx + 4, 216], fill=p["rDot"])
    f13.draw(img, int(cx - rtw / 2), 206, data["rain"], p["rTxt"])

    # hint (em10) centered y=230
    hw = f10.width("TOUCH TO REFRESH")
    f10.draw(img, int(115 - hw / 2), 230, "TOUCH TO REFRESH", p["hint"])

    img.save(out_path)
    print(f"wrote {out_path}")


if __name__ == "__main__":
    render(False, "preview_theme1_day.png", dict(
        wifi="WIFI NTP", dn="DAY", clock="12:59 PM", date="Wed Jul 16",
        temp="72", unit="C", desc="Light rain",
        tiny="HUM 62% H 81 L 64 UPD 12:05", feels="75",
        rain="RAIN NOW - ends ~2:00 PM", umbrella=True))
    render(True, "preview_theme1_night.png", dict(
        wifi="WIFI NTP", dn="NIGHT", clock="9:41 PM", date="Wed Jul 16",
        temp="64", unit="C", desc="Clear skies",
        tiny="HUM 58% H 72 L 55 UPD 9:30 PM", feels="66",
        rain="No rain expected", umbrella=False))
