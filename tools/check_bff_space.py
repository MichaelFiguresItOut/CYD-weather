#!/usr/bin/env python3
"""Verify src/nunito_bff.h by mimicking BFFfont parsing (lgfx_fonts.cpp) exactly:
decode every glyph header + pixel count per master and compare with Pillow.
Exit code 0 = all good."""
import re, struct, sys
from PIL import Image, ImageDraw, ImageFont

src = open("src/nunito_bff.h", "rb").read().decode()

def get_array(name):
    m = re.search(rf"static const uint8_t {name}\[\] = \{{(.*?)\}};", src, re.S)
    return bytes(int(n, 16) for n in re.findall(r"0x([0-9a-f]{2})", m.group(1)))

def parse_records(blob):
    recs, off = {}, 0
    while off < len(blob):
        rsz = struct.unpack_from("<I", blob, off)[0]
        recs[blob[off+4:off+8].decode()] = blob[off+8:off+rsz]
        off += rsz
    return recs

class BS:   # mirrors bit_stream_t (read_bits clamps out-of-range to 0)
    def __init__(self, data, bit_pos=0):
        self.d, self.p, self.n = data, bit_pos, len(data) * 8
    def bits(self, count):
        v = 0
        for _ in range(count):
            if self.p >= self.n: break
            v = (v << 1) | ((self.d[self.p >> 3] >> (7 - (self.p & 7))) & 1)
            self.p += 1
        return v

fails = 0
# every master present in the header (keep this in sync with MASTERS in make_nunito_bff.py)
for em in sorted(set(int(m) for m in re.findall(r"nunito_em(\d+)_data", src))):
    font = ImageFont.truetype("tools/fonts/Nunito-wght.ttf", size=em)
    font.set_variation_by_axes([700])
    recs = parse_records(get_array(f"nunito_em{em}_data"))
    head, cmap, loca_p, glyf = recs["head"], recs["cmap"], recs["loca"], recs["glyf"]

    xyb, whb, awb = head[30], head[31], head[32]
    hdr_bits = awb + 2 * xyb + 2 * whb
    nloc = struct.unpack_from("<I", loca_p, 0)[0]
    lt = struct.unpack_from(f"<{nloc}I", loca_p, 4)

    bad = []
    for cp in range(32, 127):
        gid = cp - 31                      # gid 0 = .notdef, cp32 -> gid1 ...
        o = lt[gid]
        ln = (lt[gid + 1] if gid + 1 < nloc else len(glyf)) - o
        bs = BS(glyf, o * 8)               # region starts at BYTE offset o
        adv = bs.bits(awb); bx = bs.bits(xyb); by = bs.bits(xyb)
        w = bs.bits(whb); h = bs.bits(whb)
        need_hdr = (hdr_bits + 7) // 8
        # Ground truth: re-rasterize exactly like the generator does.
        img = Image.new("L", (256, 256), 0)
        ImageDraw.Draw(img).text((64, 96), chr(cp), font=font, fill=255, anchor="ls")
        bb = img.getbbox()
        exp_w, exp_h = (bb[2] - bb[0], bb[3] - bb[1]) if bb else (0, 0)
        ok = (ln >= need_hdr and w == exp_w and h == exp_h
              and adv > 0 and ln >= (hdr_bits + w * h * 4 + 7) // 8)
        if not ok:
            bad.append(f"cp={cp} {chr(cp)!r}: len={ln}(need_hdr={need_hdr}, need_pix={(hdr_bits+w*h*4+7)//8}) "
                       f"adv={adv} bx={bx} by={by} w={w} h={h} | pillow w={exp_w} h={exp_h}")

    # space must decode as empty with a real advance
    bs = BS(glyf, lt[1] * 8)
    s_adv, s_bx, s_by, s_w, s_h = (bs.bits(awb), bs.bits(xyb), bs.bits(xyb), bs.bits(whb), bs.bits(whb))
    space_ok = (s_w == 0 and s_h == 0 and s_adv > 0)

    print(f"em{em}: awb={awb} xyb={xyb} whb={whb} hdr_bits={hdr_bits} "
          f"space(adv={s_adv},w={s_w},h={s_h}) {'OK' if space_ok else 'FAIL'} | bad glyphs: {len(bad)}/95")
    for line in bad[:10]:
        print("   ", line)
    fails += len(bad) + (0 if space_ok else 1)

sys.exit(1 if fails else 0)
