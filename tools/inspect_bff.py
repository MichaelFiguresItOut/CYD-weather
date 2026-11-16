#!/usr/bin/env python3
"""Independent minimal BFF parser — dump glyphs as ASCII art.

Parses the exact blob in src/nunito_bff.h the way LGFX BFFfont does
(head bits -> per-glyph bitstream), so any corruption seen here is IN THE BLOB
(generator bug), not in the decoder.
"""
import re, struct, sys

def parse_bff(path="src/nunito_bff.h"):
    src = open(path).read()
    # all arrays: static const uint8_t NAME_data[] = { ... };
    arrays = {}
    for m in re.finditer(r"static const uint8_t (\w+)_data\[\] = \{(.*?)\n\};", src, re.S):
        name = m.group(1)
        data = bytes(int(t, 16) for t in re.findall(r"0x([0-9a-fA-F]{2})", m.group(2)))
        arrays[name] = data
    masters = {}
    for m in re.finditer(
        r"\{\s*(\w+_data),\s*sizeof\(\1\),\s*(\d+),\s*(-?\d+),\s*(-?\d+),\s*(\d+)\s*\}", src):
        name, em, asc, desc, sp = m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5))
        masters[em] = {"name": name, "asc": asc, "desc": desc, "sp": sp,
                       "data": arrays[name[:-len("_data")]]}
    return masters

def records(data):
    out, off = {}, 0
    while off + 8 <= len(data):
        size = struct.unpack_from("<I", data, off)[0]
        marker = data[off+4:off+8].decode()
        out[marker] = data[off+8:off+size]
        off += size
    return out

def parse_master(em, data):
    rec = records(data)
    h = rec["head"]
    font_size = struct.unpack_from("<H", h, 6)[0]
    ascent, descent = struct.unpack_from("<Hh", h, 8)
    dadv = struct.unpack_from("<H", h, 22)[0]
    idx_fmt, gid_fmt, adv_fmt, bpp = h[26], h[27], h[28], h[29]
    xyb, whb, awb, comp, subpx = h[30], h[31], h[32], h[33], h[34]

    cmap = rec["cmap"]
    nsub = struct.unpack_from("<I", cmap, 0)[0]
    subs = []
    for i in range(nsub):
        off, start, ln, gido, nent, fmt = struct.unpack_from("<IIHHHB", cmap, 4 + 16*i)
        subs.append(dict(off=off, start=start, ln=ln, gido=gido, nent=nent, fmt=fmt))

    loca = rec["loca"]
    n = struct.unpack_from("<I", loca, 0)[0]
    loc = struct.unpack_from(f"<{n}I", loca, 4)
    glyf = rec["glyf"]

    hdr_bits = awb + 2*xyb + 2*whb
    hdr_bytes = (hdr_bits + 7)//8

    def rd_bits(buf, bitpos, bits, signed=False):
        v = 0
        for i in range(bits-1, -1, -1):
            b = (buf[(bitpos)//8] >> (7 - bitpos%8)) & 1
            v = (v << 1) | b
            bitpos += 1
        if signed and v & (1 << (bits-1)):
            v -= 1 << bits
        return v, bitpos

    glyphs = {}
    for gid in range(1, n):
        off = loc[gid]
        end = loc[gid+1] if gid+1 < len(loc) else len(glyf)
        ln = end - off
        buf = glyf[off:off+ln]
        adv = dadv if awb == 0 else rd_bits(buf, 0, awb)[0]
        p = awb
        bx, p = rd_bits(buf, p, xyb, True)
        by, p = rd_bits(buf, p, xyb, True)
        w, p = rd_bits(buf, p, whb)
        hh, p = rd_bits(buf, p, whb)
        pixels = []
        for i in range(w*hh):
            v, p = rd_bits(buf, p, bpp)
            pixels.append(v)
        glyphs[gid] = dict(adv=adv, bx=bx, by=by, w=w, h=hh, px=pixels)
    # verify + map codepoints via the sparse subtables
    def cp_of(gid):
        for s in subs:
            if s["fmt"] != 1:
                return None
            d = cmap[s["off"]:]
            deltas = struct.unpack_from(f"<{s['nent']}H", d, 0)
            gids = struct.unpack_from(f"<{s['nent']}H", d, s["nent"]*2)
            for k, g in enumerate(gids):
                if g == gid:
                    return s["start"] + deltas[k]
        return None
    return dict(font_size=font_size, ascent=ascent, descent=descent, dadv=dadv,
                xyb=xyb, whb=whb, awb=awb, comp=comp, subpx=subpx,
                hdr_bytes=hdr_bytes, glyphs=glyphs, cp_of=cp_of, n=n)

def render(g, scale=1):
    lines = []
    for y in range(g["h"]):
        row = "".join("#" if g["px"][y*g["w"]+x] >= 8 else ("+" if g["px"][y*g["w"]+x] else ".") for x in range(g["w"]))
        lines.append(row)
    return "\n".join(lines)

if __name__ == "__main__":
    em = int(sys.argv[1]) if len(sys.argv) > 1 else 13
    which = sys.argv[2] if len(sys.argv) > 2 else None
    masters = parse_bff()
    M = parse_master(em, masters[em]["data"])
    print(f"em{em}: asc={M['ascent']} desc={M['descent']} dadv={M['dadv']} bits xy/wh/aw={M['xyb']}/{M['whb']}/{M['awb']} comp={M['comp']} subpx={M['subpx']} hdr_bytes={M['hdr_bytes']} glyphs={M['n']}")
    items = []
    for gid in range(1, M["n"]):
        cp = M["cp_of"](gid)
        if cp is None: continue
        items.append((cp, gid))
    if which:
        items = [(cp, gid) for cp, gid in items if chr(cp) in which or (cp in (0xB0,0xB7) and ("°" in which or "·" in which))]
    for cp, gid in items:
        g = M["glyphs"][gid]
        ch = chr(cp)
        if cp == 0xB0: ch = "°"
        if cp == 0xB7: ch = "·"
        print(f"--- {ch!r} cp={cp} gid={gid} adv={g['adv']} bx={g['bx']} by={g['by']} {g['w']}x{g['h']}")
        print(render(g))
