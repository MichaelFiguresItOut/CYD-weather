#!/usr/bin/env python3
"""Host-side validation of the dog_image.cpp decode + composite pipeline.

Mirrors the C logic exactly (chunk walk, zlib-header strip, Adler-32 check,
unfiltering, natural-RGB565 + alpha storage, and compositeRow()'s blend +
panel inversion + byte swap for LovyanGFX's swap565_t pushImage) and compares
against PIL's reference decode of every outfit PNG in dogs/.

Checks per image:
  A. decoded alpha matches PIL exactly; stored RGB565 (expanded back to 8-bit)
     is within the quantization tolerance of PIL's natural color;
  B. compositeRow() output, run through the library swap + panel inversion,
     equals the C integer blend of (stored color, alpha, background);
  C. opaque pixels are untouched by compositing; transparent pixels actually
     exist and change with the background (transparency is exercised).
"""
import glob, os, struct, sys, zlib
from PIL import Image

DOGS_DIR = os.path.join(os.path.dirname(__file__), "..", "dogs")
FILES = sorted(glob.glob(os.path.join(DOGS_DIR, "*.png")))

def be32(b): return struct.unpack(">I", b)[0]

def decode_png(data):
    """Mirror of loadDogImage(): returns (w, h, rgb565 list, alpha bytearray)."""
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"

    # ---- chunk walk (same as C) ---------------------------------------------
    pos = 8
    info_w = info_h = None
    color_type = None
    palette = None
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
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            iend = True
            break
    assert iend and color_type in (0, 2, 3, 6), f"bad state: {iend} {color_type}"

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

    # ---- unfilter (same as C) -------------------------------------------------
    img  = [0] * (info_w * info_h)      # natural RGB565 (16-bit values)
    alph = bytearray(info_w * info_h)   # per-pixel alpha
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
        for x in range(info_w):
            al = 255
            if color_type == 0:   r = g = bl = cur[x]
            elif color_type == 3:
                i = cur[x]; assert i < 256
                r, g, bl = palette[i*3], palette[i*3+1], palette[i*3+2]
            elif color_type == 2: r, g, bl = cur[x*3], cur[x*3+1], cur[x*3+2]
            else:                 al = cur[x*4+3]; r, g, bl = cur[x*4], cur[x*4+1], cur[x*4+2]
            img[y*info_w + x]  = ((r >> 3) << 11) | ((g >> 2) << 5) | (bl >> 3)   # natural RGB565
            alph[y*info_w + x] = al
        t = cur; cur = prev if prev is not None else bytearray(row_bytes - 1); prev = t   # swap (mirrors C)

    return info_w, info_h, img, alph

def bswap(v): return ((v & 0xFF) << 8) | (v >> 8)
def expand565(p):
    r = (((p >> 11) & 31) << 3) | (((p >> 11) & 31) >> 2)
    g = (((p >> 5) & 63) << 2) | (((p >> 5) & 63) >> 4)
    b = ((p & 31) << 3) | ((p & 31) >> 2)
    return r, g, b

def composite_row(img, alph, w, y, bg):
    """Mirror of compositeRow(): returns the panel-ready row buffer."""
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

def main():
    assert FILES, f"no PNGs found in {DOGS_DIR}"
    total_px = 0
    failures = []

    for path in FILES:
        name = os.path.basename(path)
        data = open(path, "rb").read()
        w, h, img, alph = decode_png(data)
        assert (w, h) == (95, 128), f"{name}: expected 95x128, got {w}x{h}"

        ref = Image.open(path).convert("RGBA")
        n_transparent = 0
        worst = 0
        for y in range(h):
            for x in range(w):
                R, G, B, A = ref.getpixel((x, y))
                i = y*w + x
                if alph[i] != A:
                    failures.append(f"{name} ({x},{y}): alpha {alph[i]} != PIL {A}")
                if A < 255: n_transparent += 1
                sr, sg, sb = expand565(img[i])   # stored color back to 8-bit
                worst = max(worst, abs(sr-R), abs(sg-G), abs(sb-B))
                # RGB565 quantization tolerance after bit-replication expansion:
                # r/b (5-bit) can be off by up to 7, g (6-bit) by up to 3.
                if abs(sr-R) > 8 or abs(sg-G) > 4 or abs(sb-B) > 8:
                    failures.append(f"{name} ({x},{y}): stored {sr},{sg},{sb} vs PIL {R},{G},{B}")

        # ---- composite checks (white day bg + a night-gradient grey row) ------
        for bg in ((255, 255, 255), (196, 196, 196)):   # top/bottom of the night gradient
            buf = composite_row(img, alph, w, h//2, bg)
            for x in range(w):
                got = struct.unpack("<H", buf[x*2:x*2+2])[0]
                disp = ~bswap(got) & 0xFFFF             # library swap + panel inversion
                p = img[(h//2)*w + x]; a = alph[(h//2)*w + x]
                r, g, b = expand565(p)                   # expected: C integer blend of stored color
                er = (r*a + bg[0]*(255-a)) // 255
                eg = (g*a + bg[1]*(255-a)) // 255
                eb = (b*a + bg[2]*(255-a)) // 255
                exp = ((er >> 3) << 11) | ((eg >> 2) << 5) | (eb >> 3)
                if disp != exp:
                    failures.append(f"{name} ({x},{h//2}): composite {disp:#06x} != expected {exp:#06x}")
                if a == 255 and disp != p:
                    failures.append(f"{name} ({x},{h//2}): opaque pixel changed by compositing")

        total_px += w*h
        print(f"ok  {name:<24} {w}x{h}  transparent={n_transparent}/{w*h}  worst RGB565-expand diff={worst}")
        if n_transparent == 0:
            failures.append(f"{name}: no transparent pixels - compositing path not exercised")

    print(f"\n{len(FILES)} images, {total_px} px checked")
    if failures:
        print(f"FAILURES ({len(failures)}):")
        for f in failures[:20]:
            print("  " + f)
        sys.exit(1)
    print("all checks passed")

if __name__ == "__main__":
    main()
