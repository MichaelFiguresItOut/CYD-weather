#!/usr/bin/env python3
"""Host-side validation of the dog_image.cpp decode + composite pipeline.

Mirrors the C logic exactly (chunk walk incl. PLTE/tRNS, zlib-header strip,
Adler-32 check, unfiltering, storage as palette indices or natural-RGB565
+ alpha, and compositeRow()'s blend + panel inversion + byte swap for
LovyanGFX's swap565_t pushImage) and compares against PIL's reference
decode of every outfit PNG in dogs/.

The dog assets are 256-color palette PNGs (color type 3, PLTE + tRNS); the
decoder keeps them as 1 byte/pixel palette indices (this board has no
usable PSRAM). The mirror does the same, so memory layout is verified by
construction (see the memory note in src/dog_image.cpp).

Checks per image:
  A. palette: decoded indices resolve through PLTE/tRNS to exactly PIL's
     color + alpha, including indices beyond the tRNS table length
     (spec: opaque, alpha 255);
     rgb565: decoded alpha matches PIL exactly and stored RGB565 is within
     quantization tolerance of PIL's color;
  B. compositeRow() output, run through the library swap + panel inversion,
     equals the C integer blend of (stored color, alpha, background);
  C. opaque pixels are untouched by compositing; transparent pixels actually
     exist and change with the background (transparency is exercised).

Plus: a synthetic small palette PNG with a *short* tRNS table is decoded to
prove the "index beyond tRNS -> alpha 255" default end-to-end against PIL
(the shipped assets all happen to carry full 256-entry tRNS tables).
"""
import glob, io, os, struct, sys, zlib
from PIL import Image

DOGS_DIR = os.path.join(os.path.dirname(__file__), "..", "dogs")
FILES = sorted(glob.glob(os.path.join(DOGS_DIR, "*.png")))

def be32(b): return struct.unpack(">I", b)[0]

def pal_alpha(i, trns):
    """C rule: an index at or beyond the tRNS table length is opaque."""
    return trns[i] if i < len(trns) else 255

def decode_png(data):
    """Mirror of loadDogImage().

    Returns (w, h, ct, img, alph, pal, trns) where for color type 3
    img = list of palette indices and alph is None (color + per-pixel
    alpha live in pal/trns); otherwise img = natural RGB565 list and
    alph = per-pixel alpha bytearray, pal/trns unused (None).
    """
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"

    # ---- chunk walk (same as C) ---------------------------------------------
    pos = 8
    info_w = info_h = None
    color_type = None
    palette = None
    trns = None
    idat = bytearray()
    iend = False
    while pos < len(data):
        ln, typ = be32(data[pos:pos+4]), data[pos+4:pos+8]
        body = data[pos+8:pos+8+ln]
        crc_stored = be32(data[pos+8+ln:pos+12+ln])
        pos += 12 + ln
        if typ == b"IHDR":
            info_w, info_h, depth, ct, comp, filt, interlace = struct.unpack(">IIBBBBB", body)
            assert depth == 8 and interlace == 0 and comp == 0 and filt == 0
            color_type = ct
        elif typ == b"PLTE":
            palette = bytes(body)
        elif typ == b"tRNS":
            trns = list(body)
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            iend = True
            break
    assert iend and color_type in (0, 2, 3, 6), f"bad state: {iend} {color_type}"
    if color_type == 3:
        assert palette, "palette PNG without PLTE"
    pal = list(palette) if palette is not None else None
    trns = trns if trns is not None else []

    # ---- zlib header strip + inflate + adler (same as C) ---------------------
    has_adler = False
    def_start = 0
    if len(idat) >= 6 and (idat[0] & 0x0F) == 8 and ((idat[0]*256 + idat[1]) % 31) == 0 and (idat[1] & 0x20) == 0:
        def_start = 2
        has_adler = True

    bpp = {0:1, 2:3, 3:1, 6:4}[color_type]
    row_bytes = info_w * bpp + 1
    raw_size = row_bytes * info_h

    if has_adler:
        # Python's zlib consumes the wrapper (header + Adler-32) itself and
        # verifies the checksum internally; also verify manually to mirror C.
        raw = zlib.decompress(bytes(idat))
        expect = be32(bytes(idat[-4:]))
        a, b_ = 1, 0
        for i in range(0, len(raw), 5552):
            for byte in raw[i:i+5552]:
                a += byte; b_ += a
            a %= 65521; b_ %= 65521
        adler = (b_ << 16) | a
        assert adler == expect, f"adler mismatch {adler:#x} != {expect:#x}"
    else:
        d = zlib.decompressobj(-15)
        raw = d.decompress(bytes(idat))
        assert d.unused_data == b"", "raw deflate did not consume all input"

    assert len(raw) == raw_size, f"size {len(raw)} != {raw_size}"

    # ---- unfilter + store (same as C) ----------------------------------------
    # Palette sources: 1 byte/pixel index (alph is None). Others: RGB565 +
    # per-pixel alpha.
    img  = [0] * (info_w * info_h)
    alph = None if color_type == 3 else bytearray(info_w * info_h)
    cur = bytearray(row_bytes - 1)
    prev = None
    for y in range(info_h):
        srow = raw[y*row_bytes:(y+1)*row_bytes]
        f = srow[0]
        assert f <= 4, f"bad filter {f} at row {y}"
        for x in range(info_w * bpp):
            a = cur[x-bpp] if x >= bpp else 0
            b = prev[x] if prev else 0
            c = (prev[x-bpp] if prev and x >= bpp else 0)
            v = srow[1+x]
            if f == 0:   out = v
            elif f == 1: out = (v + a) & 0xFF
            elif f == 2: out = (v + b) & 0xFF
            elif f == 3: out = (v + (a+b)//2) & 0xFF
            else:
                p = a + b - c
                pa, pb, pc = abs(p-a), abs(p-b), abs(p-c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                out = (v + pr) & 0xFF
            cur[x] = out
        if color_type == 3:
            for x in range(info_w):
                i = cur[x]
                assert i < len(pal) // 3, f"bad palette index {i} at row {y}"
                img[y*info_w + x] = i
        else:
            for x in range(info_w):
                al = 255
                if color_type == 0:   r = g = bl = cur[x]
                elif color_type == 2: r, g, bl = cur[x*3], cur[x*3+1], cur[x*3+2]
                else:                 al = cur[x*4+3]; r, g, bl = cur[x*4], cur[x*4+1], cur[x*4+2]
                img[y*info_w + x]  = ((r >> 3) << 11) | ((g >> 2) << 5) | (bl >> 3)   # natural RGB565
                alph[y*info_w + x] = al
        t = cur; cur = prev if prev is not None else bytearray(row_bytes - 1); prev = t   # swap (mirrors C)

    return info_w, info_h, color_type, img, alph, pal, trns

def bswap(v): return ((v & 0xFF) << 8) | (v >> 8)
def expand565(p):
    r = (((p >> 11) & 31) << 3) | (((p >> 11) & 31) >> 2)
    g = (((p >> 5) & 63) << 2) | (((p >> 5) & 63) >> 4)
    b = ((p & 31) << 3) | ((p & 31) >> 2)
    return r, g, b

def composite_row(img, alph, w, y, bg):
    """Mirror of compositeRow()'s RGB565 branch: panel-ready row buffer."""
    br, bgg, bb = bg
    out = bytearray(w * 2)
    for x in range(w):
        p = img[y*w + x]
        r, g, b = expand565(p)
        a = alph[y*w + x]
        r = (r*a + br*(255-a)) // 255
        g = (g*a + bgg*(255-a)) // 255
        b = (b*a + bb*(255-a)) // 255
        v = ~(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)) & 0xFFFF
        out[x*2:x*2+2] = struct.pack("<H", bswap(v))
    return out

def composite_row_pal(ix, pal, trns, w, y, bg):
    """Mirror of compositeRow()'s palette branch: PLTE/tRNS lookup + blend."""
    br, bgg, bb = bg
    out = bytearray(w * 2)
    for x in range(w):
        i = ix[y*w + x]
        r, g, b = pal[i*3], pal[i*3+1], pal[i*3+2]
        a = pal_alpha(i, trns)
        r = (r*a + br*(255-a)) // 255
        g = (g*a + bgg*(255-a)) // 255
        b = (b*a + bb*(255-a)) // 255
        v = ~(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)) & 0xFFFF
        out[x*2:x*2+2] = struct.pack("<H", bswap(v))
    return out

def check_composite(name, w, y, bg, buf, is_pal, pal, trns, img, alph, failures):
    """Checks B + C for one composite row (both layout branches share this)."""
    for x in range(w):
        got = struct.unpack("<H", buf[x*2:x*2+2])[0]
        disp = ~bswap(got) & 0xFFFF             # library swap + panel inversion
        if is_pal:
            i = img[y*w + x]
            pr, pg, pb = pal[i*3], pal[i*3+1], pal[i*3+2]
            a = pal_alpha(i, trns)
        else:
            p = img[y*w + x]; a = alph[y*w + x]
            pr, pg, pb = expand565(p)           # C blends the expanded 8-bit color
        er = (pr*a + bg[0]*(255-a)) // 255
        eg = (pg*a + bg[1]*(255-a)) // 255
        eb = (pb*a + bg[2]*(255-a)) // 255
        exp = ((er >> 3) << 11) | ((eg >> 2) << 5) | (eb >> 3)
        if disp != exp:
            failures.append(f"{name} ({x},{y}): composite {disp:#06x} != expected {exp:#06x}")
        if a == 255 and disp != ((pr >> 3) << 11) | ((pg >> 2) << 5) | (pb >> 3):
            failures.append(f"{name} ({x},{y}): opaque pixel changed by compositing")

def check_synthetic_short_trns():
    """Decodes a small palette PNG whose tRNS table is shorter than the
    palette and confirms the mirror matches PIL - i.e. indices beyond the
    tRNS length come out opaque (alpha 255), per the PNG spec and the C."""
    im = Image.new("P", (4, 1))
    pal = [0, 0, 0] * 20             # 20 entries; color idx 17 as blue
    pal[0:3] = [255, 0, 0]; pal[1:3] = [0, 255, 0]; pal[17:20] = [0, 0, 255]
    im.putpalette(pal)
    im.putdata([0, 1, 17, 0])        # idx 17 is beyond the 2-entry tRNS below
    im.info["transparency"] = b"\x00\x80"
    buf = io.BytesIO(); im.save(buf, format="PNG")

    w, h, ct, img, alph, pal, trns = decode_png(buf.getvalue())
    assert (w, h, ct) == (4, 1, 3), f"synthetic: bad state {(w, h, ct)}"
    failures = []
    ref = Image.open(io.BytesIO(buf.getvalue())).convert("RGBA")
    for x in range(w):
        R, G, B, A = ref.getpixel((x, 0))
        i = img[x]
        assert pal_alpha(i, trns) == A, \
            f"synthetic ({x}): alpha rule {pal_alpha(i, trns)} != PIL {A}"
        assert (pal[i*3], pal[i*3+1], pal[i*3+2]) == (R, G, B), "synthetic: color mismatch"
    # composite at row 0 against a grey bg: beyond-tRNS pixel (x=2) must be
    # opaque -> equals the raw palette color.
    bg = (196, 196, 196)
    cbuf = composite_row_pal(img, pal, trns, w, 0, bg)
    check_composite("synthetic", w, 0, bg, cbuf, True, pal, trns, img, None, failures)
    assert not failures, failures
    print("ok  synthetic short-tRNS      4x1  idx beyond tRNS -> alpha 255 (matches PIL)")

def main():
    assert FILES, f"no PNGs found in {DOGS_DIR}"
    total_px = 0
    failures = []

    for path in FILES:
        name = os.path.basename(path)
        data = open(path, "rb").read()
        w, h, ct, img, alph, pal, trns = decode_png(data)
        assert (w, h) == (95, 128), f"{name}: expected 95x128, got {w}x{h}"

        ref = Image.open(path).convert("RGBA")
        n_transparent = 0
        worst = 0
        for y in range(h):
            for x in range(w):
                R, G, B, A = ref.getpixel((x, y))
                i = y*w + x
                if A < 255: n_transparent += 1
                if ct == 3:
                    # Palette decode is exact: index -> PLTE RGB + tRNS alpha.
                    idx = img[i]
                    a = pal_alpha(idx, trns)
                    if a != A:
                        failures.append(f"{name} ({x},{y}): alpha {a} != PIL {A}")
                    pr, pg, pb = pal[idx*3], pal[idx*3+1], pal[idx*3+2]
                    if (pr, pg, pb) != (R, G, B):
                        failures.append(f"{name} ({x},{y}): palette {pr},{pg},{pb} != PIL {R},{G},{B}")
                else:
                    if alph[i] != A:
                        failures.append(f"{name} ({x},{y}): alpha {alph[i]} != PIL {A}")
                    sr, sg, sb = expand565(img[i])   # stored color back to 8-bit
                    worst = max(worst, abs(sr-R), abs(sg-G), abs(sb-B))
                    # RGB565 quantization tolerance after bit-replication expansion:
                    # r/b (5-bit) can be off by up to 7, g (6-bit) by up to 3.
                    if abs(sr-R) > 8 or abs(sg-G) > 4 or abs(sb-B) > 8:
                        failures.append(f"{name} ({x},{y}): stored {sr},{sg},{sb} vs PIL {R},{G},{B}")

        # ---- composite checks (white day bg + a night-gradient grey row) ------
        for bg in ((255, 255, 255), (196, 196, 196)):   # top/bottom of the night gradient
            if ct == 3:
                buf = composite_row_pal(img, pal, trns, w, h//2, bg)
                check_composite(name, w, h//2, bg, buf, True, pal, trns, img, None, failures)
            else:
                buf = composite_row(img, alph, w, h//2, bg)
                check_composite(name, w, h//2, bg, buf, False, None, None, img, alph, failures)

        total_px += w*h
        kind = "palette" if ct == 3 else "rgb565"
        extra = "" if ct == 3 else f"  worst RGB565-expand diff={worst}"
        print(f"ok  {name:<24} {w}x{h}  {kind}  transparent={n_transparent}/{w*h}{extra}")
        if n_transparent == 0:
            failures.append(f"{name}: no transparent pixels - compositing path not exercised")

    check_synthetic_short_trns()

    print(f"\n{len(FILES)} images, {total_px} px checked")
    if failures:
        print(f"FAILURES ({len(failures)}):")
        for f in failures[:20]:
            print("  " + f)
        sys.exit(1)
    print("all checks passed")

if __name__ == "__main__":
    main()
