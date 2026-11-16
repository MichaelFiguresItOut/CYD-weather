#!/usr/bin/env python3
"""Histogram every 4bpp alpha value in src/nunito_bff.h.

The CYD panel is bit-inverted and LGFX's draw_alpha_bitmap_common() blends
intermediate-alpha pixels against getBaseColor(). With binary {0,15} coverage
no blending ever happens (a=0 -> skip, a>=max -> pure fg). Any intermediate
value would render as a halo on glass. This script proves whether the blob is
actually binary."""
import re, struct, sys

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

total = {}
for em in (10, 13, 20, 48):
    recs = parse_records(get_array(f"nunito_em{em}_data"))
    head, loca_p, glyf = recs["head"], recs["loca"], recs["glyf"]
    bpp = head[29]
    xyb, whb, awb = head[30], head[31], head[32]
    hdr_bits = awb + 2 * xyb + 2 * whb
    nloc = struct.unpack_from("<I", loca_p, 0)[0]
    lt = struct.unpack_from(f"<{nloc}I", loca_p, 4)

    hist = {}
    npix = 0
    for gid in range(nloc - 1):
        o = lt[gid]
        ln = (lt[gid + 1] if gid + 1 < nloc else len(glyf)) - o
        bs = BS(glyf, o * 8)
        adv = bs.bits(awb); bx = bs.bits(xyb); by = bs.bits(xyb)
        w = bs.bits(whb); h = bs.bits(whb)
        for _ in range(w * h):
            a = bs.bits(bpp)
            hist[a] = hist.get(a, 0) + 1
            npix += 1
    total[em] = (bpp, hist, npix)
    print(f"em{em}: bpp={bpp} pixels={npix} alpha histogram: "
          f"{ {k: v for k, v in sorted(hist.items())} }")

bad = any(v not in (0, 15) for _, h, _ in total.values() for v in h)
print("RESULT:", "NOT BINARY - intermediate alphas present!" if bad else "binary {0,15} only -> no blending possible")
sys.exit(1 if bad else 0)
