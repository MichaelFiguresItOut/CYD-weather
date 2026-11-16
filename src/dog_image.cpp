// ============================================================================
//  Dog outfit image loader / PNG decoder
//
//  Reads an explicitly named file from the SD card root, decodes it (PNG chunks
//  + DEFLATE via Mark Adler's public-domain puff.c in lib/puff) and keeps the
//  result as natural RGB565 + per-pixel alpha in heap buffers. main.cpp picks
//  one of the weather-outfit files (see selectDogImage() there) and reloads on
//  every change; compositeRow() does the alpha blend + panel transforms at draw time.
//
//  PNG support: non-interlaced, 8-bit samples, color types 0/2/3/6.
//  IDAT integrity is checked with the Adler-32 of the zlib wrapper; per-chunk
//  CRCs are not verified (IHDR sanity checks + Adler-32 cover the rest).
// ============================================================================

#include <Arduino.h>
#include <SD.h>
#include "dog_image.h"

// puff() inflates a raw DEFLATE stream (no zlib header handling, no checksum).
extern "C" int puff(unsigned char *dest, unsigned long *destlen,
                    const unsigned char *source, unsigned long *sourcelen);

// The dashboard reserves the bottom-right corner for this image: everything
// below the temperature row is centered to its left (see main.cpp), so it must
// fit in a box of at most kMaxW x kMaxH with kDogMargin around it.
static const int kMaxW = 96;    // keeps a >=220 px text zone for size-2 lines
static const int kMaxH = 128;   // full native height (rows [108,236))

static uint16_t* g_rgb   = nullptr;   // natural RGB565 pixels (heap)
static uint8_t*  g_alpha = nullptr;   // per-pixel alpha 0..255 (heap)
static int       g_w     = 0;
static int       g_h     = 0;
static char      g_name[48] = "";

const char* dogCurrentName() { return g_name; }
int dogWidth()              { return g_w; }
int dogHeight()             { return g_h; }
int dogLeft(int screenWidth) { return (g_w > 0) ? screenWidth - g_w - kDogMargin : screenWidth; }

namespace {

// ---------------------------------------------------------------------------
//  Small helpers
// ---------------------------------------------------------------------------

uint32_t be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// Adler-32 over the decompressed IDAT payload (PNG's zlib wrapper ends with it).
uint32_t adler32(const uint8_t* data, size_t len) {
  uint32_t a = 1, b = 0;
  while (len > 0) {
    int n = (int)(len > 5552 ? 5552 : len);
    len -= (size_t)n;
    for (; n-- > 0; data++) { a += *data; b += a; }
    a %= 65521u; b %= 65521u;
  }
  return (b << 16) | a;
}

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// Area-average downscale of an 8-bit alpha mask (same box math as scaleAreaAvg).
uint8_t* scaleAlpha(const uint8_t* src, int sw, int sh, int dw, int dh) {
  uint8_t* dst = (uint8_t*)malloc((size_t)dw * dh);
  if (!dst) return nullptr;

  for (int dy = 0; dy < dh; dy++) {
    int y0 = (dy * sh) / dh, y1 = ((dy + 1) * sh) / dh;
    if (y1 <= y0) y1 = y0 + 1;
    for (int dx = 0; dx < dw; dx++) {
      int x0 = (dx * sw) / dw, x1 = ((dx + 1) * sw) / dw;
      if (x1 <= x0) x1 = x0 + 1;
      uint32_t s = 0, n = 0;
      for (int y = y0; y < y1; y++) {
        const uint8_t* row = src + (size_t)y * sw;
        for (int x = x0; x < x1; x++) { s += row[x]; n++; }
      }
      dst[(size_t)dy * dw + dx] = (uint8_t)((s + n / 2) / n);
    }
  }
  return dst;
}

// Area-average downscale RGB565 -> RGB565 (natural colors).
uint16_t* scaleAreaAvg(const uint16_t* src, int sw, int sh, int dw, int dh) {
  uint16_t* dst = (uint16_t*)malloc((size_t)dw * dh * sizeof(uint16_t));
  if (!dst) return nullptr;

  for (int dy = 0; dy < dh; dy++) {
    int y0 = (dy * sh) / dh, y1 = ((dy + 1) * sh) / dh;
    if (y1 <= y0) y1 = y0 + 1;
    for (int dx = 0; dx < dw; dx++) {
      int x0 = (dx * sw) / dw, x1 = ((dx + 1) * sw) / dw;
      if (x1 <= x0) x1 = x0 + 1;
      uint32_t r = 0, g = 0, b = 0, n = 0;
      for (int y = y0; y < y1; y++) {
        const uint16_t* row = src + (size_t)y * sw;
        for (int x = x0; x < x1; x++) {
          uint16_t p = row[x];
          r += (((p >> 11) & 31u) << 3) | (((p >> 11) & 31u) >> 2);   // 5 -> 8 bit
          g += (((p >> 5) & 63u) << 2) | (((p >> 5) & 63u) >> 4);     // 6 -> 8 bit
          b += ((p & 31u) << 3) | ((p & 31u) >> 2);                   // 5 -> 8 bit
          n++;
        }
      }
      dst[(size_t)dy * dw + dx] = rgb565((uint8_t)((r + n / 2) / n),
                                         (uint8_t)((g + n / 2) / n),
                                         (uint8_t)((b + n / 2) / n));
    }
  }
  return dst;
}

struct PngInfo {
  int     w = 0, h = 0;
  uint8_t colorType = 0;      // 0 grey, 2 rgb, 3 palette, 6 rgba
};

// Read exactly n bytes (File::read may return short). Returns false on EOF.
bool readFull(File& f, void* buf, size_t n) {
  uint8_t* p = (uint8_t*)buf;
  while (n > 0) {
    int got = (int)f.read(p, n);
    if (got <= 0) return false;
    p += got; n -= (size_t)got;
  }
  return true;
}

// Skip n bytes.
bool skip(File& f, size_t n) {
  uint8_t tmp[256];
  while (n > 0) {
    size_t c = n < sizeof(tmp) ? n : sizeof(tmp);
    if (!readFull(f, tmp, c)) return false;
    n -= c;
  }
  return true;
}

}   // namespace

// Load one outfit file from the SD card root (e.g. "rain_now.png"). Frees any
// previously decoded image first, so a failed load leaves a clean empty state.
bool loadDogImage(const char* path) {
  free(g_rgb);   g_rgb = nullptr;
  free(g_alpha); g_alpha = nullptr;
  g_w = g_h = 0;
  g_name[0] = '\0';

  if (!path || !*path) return false;
  char full[48];
  snprintf(full, sizeof(full), "/%s", path);
  File f = SD.open(full);
  if (!f) {
    Serial.printf("[DOG] %s not found on SD card\n", full);
    return false;
  }

  uint8_t sig[8];
  bool ok = readFull(f, sig, 8) && memcmp(sig, "\x89PNG\r\n\x1a\n", 8) == 0;
  if (!ok) { f.close(); Serial.println("[DOG] not a PNG file"); return false; }

  PngInfo info;
  uint8_t palette[768]; bool havePalette = false;
  uint8_t* idat = nullptr; size_t idatLen = 0, idatCap = 0;
  bool iend = false;

  // ---------------------------------------------------------------------------
  //  Chunk walk: IHDR / PLTE / IDAT / IEND (everything else is skipped)
  // ---------------------------------------------------------------------------
  for (;;) {
    uint8_t hdr[8];   // length (4 BE) + type (4 ASCII)
    if (!readFull(f, hdr, 8)) break;
    uint32_t len = be32(&hdr[0]);
    const char* type = (const char*)&hdr[4];

    if (len > 1u * 1024u * 1024u) { ok = false; Serial.println("[DOG] chunk too large"); break; }

    if (strcmp(type, "IHDR") == 0) {
      uint8_t d[13];
      if (!readFull(f, d, 13) || len < 13) { ok = false; Serial.println("[DOG] bad IHDR"); break; }
      info.w = (int)be32(&d[0]);
      info.h = (int)be32(&d[4]);
      bool ctOk = (d[9] == 0) || (d[9] == 2) || (d[9] == 3) || (d[9] == 6);
      if (!ctOk || d[8] != 8 || d[10] != 0 || d[11] != 0 || d[12] != 0) {
        ok = false; Serial.println("[DOG] unsupported PNG (need 8-bit, non-interlaced)"); break;
      }
      info.colorType = d[9];
      if (info.w < 1 || info.h < 1 || info.w > 512 || info.h > 512) { ok = false; Serial.println("[DOG] bad image size"); break; }
    } else if (strcmp(type, "PLTE") == 0) {
      if (len > sizeof(palette)) { ok = false; Serial.println("[DOG] palette too large"); break; }
      if (!readFull(f, palette, len)) { ok = false; Serial.println("[DOG] read error in PLTE"); break; }
      havePalette = true;
    } else if (strcmp(type, "IDAT") == 0) {
      if (idatLen + len > idatCap) {
        uint8_t* nb = (uint8_t*)realloc(idat, idatLen + len);   // exact growth
        if (!nb) { ok = false; Serial.println("[DOG] out of memory"); break; }
        idat = nb; idatCap = idatLen + len;
      }
      if (!readFull(f, idat + idatLen, len)) { ok = false; Serial.println("[DOG] read error in IDAT"); break; }
      idatLen += len;
    } else if (strcmp(type, "IEND") == 0) {
      iend = true;   // done after consuming the CRC below
    } else {
      if (!skip(f, len)) { ok = false; Serial.println("[DOG] read error skipping chunk"); break; }
    }

    // Every chunk ends with a 4-byte CRC32 (not verified: pixel data is covered
    // by the Adler-32 check below and IHDR sanity checks).
    if (!skip(f, 4)) ok = false;

    if (!ok || iend) break;
  }

  f.close();

  if (!ok || !iend) { free(idat); return false; }
  if (info.colorType == 3 && !havePalette) { free(idat); Serial.println("[DOG] palette PNG without PLTE"); return false; }

  int bpp = (info.colorType == 2) ? 3 : (info.colorType == 6) ? 4 : 1;
  size_t rowBytes = (size_t)info.w * bpp + 1;      // + filter byte per scanline
  size_t rawSize  = rowBytes * info.h;

  // ---------------------------------------------------------------------------
  //  Decompress IDAT. Per RFC 2083 the payload is zlib-wrapped:
  //    [CMF][FLG][deflate blocks...][Adler-32 (4, big endian)]
  //  puff() inflates raw DEFLATE only, so strip a valid zlib header first; also
  //  accept a non-standard file that stores raw deflate directly.
  // ---------------------------------------------------------------------------
  const uint8_t* defStart = idat;
  bool hasAdler = false;
  if (idatLen >= 6 && (idat[0] & 0x0F) == 8 && ((uint32_t)idat[0] * 256u + idat[1]) % 31u == 0 && (idat[1] & 0x20) == 0) {
    defStart = idat + 2;   // valid zlib header -> skip CMF/FLG, expect Adler-32 tail
    hasAdler = true;
  }

  uint8_t* raw = (uint8_t*)malloc(rawSize);
  if (!raw) { free(idat); Serial.println("[DOG] out of memory"); return false; }

  unsigned long destLen = (unsigned long)rawSize;
  unsigned long srcAvail = (unsigned long)(idat + idatLen - defStart);
  int rc = puff(raw, &destLen, defStart, &srcAvail);

  bool good = (rc == 0) && (destLen == rawSize);
  if (good && hasAdler) {
    size_t tail = (size_t)(idat + idatLen - defStart - srcAvail);
    uint32_t expect = be32(idat + idatLen - 4);
    good = (tail == 4) && (adler32(raw, rawSize) == expect);
    if (!good) Serial.println("[DOG] Adler-32 mismatch (corrupt IDAT?)");
  } else if (good) {
    // Raw-deflate file: the stream must have consumed everything.
    good = ((size_t)(idat + idatLen - defStart - srcAvail) == 0);
  }
  if (!good && rc != 0) Serial.printf("[DOG] inflate failed (rc=%d)\n", rc);

  // ---------------------------------------------------------------------------
  //  Unfilter scanlines and convert to RGB565 (natural colors for now).
  //  Two row buffers: cur is written in place (Sub/Paeth only read positions
  //  already written this row), prev holds the previous unfiltered row.
  // ---------------------------------------------------------------------------
  uint16_t* img  = nullptr;   // natural RGB565
  uint8_t*  alph = nullptr;   // per-pixel alpha (255 for non-RGBA images)
  if (good) {
    img  = (uint16_t*)malloc((size_t)info.w * info.h * sizeof(uint16_t));
    alph = (uint8_t*)malloc((size_t)info.w * info.h);
    if (!img || !alph) good = false;
  }

  uint8_t* rowA = nullptr, *rowB = nullptr;
  if (good) {
    rowA = (uint8_t*)malloc(rowBytes - 1);
    rowB = (uint8_t*)malloc(rowBytes - 1);
    if (!rowA || !rowB) good = false;
  }

  if (good) {
    uint8_t* cur  = rowA;
    uint8_t* prev = nullptr;
    for (int y = 0; y < info.h && good; y++) {
      const uint8_t* srow = raw + (size_t)y * rowBytes;
      uint8_t filter = srow[0];
      if (filter > 4) { good = false; break; }

      for (int x = 0; x < info.w * bpp; x++) {
        uint8_t a = (x >= bpp) ? cur[x - bpp] : 0;
        uint8_t b = prev ? prev[x] : 0;
        uint8_t c = (prev && x >= bpp) ? prev[x - bpp] : 0;
        switch (filter) {
          case 0: cur[x] = srow[1 + x]; break;                          // None
          case 1: cur[x] = (uint8_t)(srow[1 + x] + a); break;           // Sub
          case 2: cur[x] = (uint8_t)(srow[1 + x] + b); break;           // Up
          case 3: cur[x] = (uint8_t)(srow[1 + x] + ((int)a + (int)b) / 2); break;   // Average
          default: {                                                    // Paeth
            int p = (int)a + (int)b - (int)c;
            int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
            uint8_t pr = (pa <= pb && pa <= pc) ? a : ((pb <= pc) ? b : c);
            cur[x] = (uint8_t)(srow[1 + x] + pr);
          }
        }
      }

      for (int x = 0; x < info.w && good; x++) {
        uint8_t r, g, b2, al = 255;
        if (info.colorType == 0) {
          r = g = b2 = cur[x];
        } else if (info.colorType == 3) {
          int idx = cur[x];
          if (idx >= 256) { good = false; break; }
          r = palette[idx * 3]; g = palette[idx * 3 + 1]; b2 = palette[idx * 3 + 2];
        } else if (info.colorType == 2) {
          r = cur[x * 3]; g = cur[x * 3 + 1]; b2 = cur[x * 3 + 2];
        } else {   // RGBA: keep natural color + alpha; composited at draw time
          al = cur[x * 4 + 3];
          r = cur[x * 4]; g = cur[x * 4 + 1]; b2 = cur[x * 4 + 2];
        }
        img[(size_t)y * info.w + x]  = rgb565(r, g, b2);
        alph[(size_t)y * info.w + x] = al;
      }

      uint8_t* t = cur; cur = prev ? prev : rowB; prev = t;   // swap (first row: cur was A)
    }
  }
  free(rowA); free(rowB);

  if (!good) {
    free(raw); free(idat); free(img); free(alph);
    return false;
  }

  // ---------------------------------------------------------------------------
  //  Fit into the reserved corner zone. No panel transform here - compositeRow()
  //  applies inversion + byte swap per row at draw time, after blending alpha.
  // ---------------------------------------------------------------------------
  int fw = info.w, fh = info.h;
  if (fw > kMaxW || fh > kMaxH) {
    float scale = fminf((float)kMaxW / fw, (float)kMaxH / fh);
    int dw = (int)(fw * scale + 0.5f); if (dw < 1) dw = 1;
    int dh = (int)(fh * scale + 0.5f); if (dh < 1) dh = 1;
    uint16_t* small  = scaleAreaAvg(img, fw, fh, dw, dh);
    uint8_t*  aSmall = scaleAlpha(alph, fw, fh, dw, dh);
    if (!small || !aSmall) {
      free(small); free(aSmall); free(raw); free(idat); free(img); free(alph);
      Serial.println("[DOG] out of memory"); return false;
    }
    free(img);  img = small;
    free(alph); alph = aSmall;
    fw = dw; fh = dh;
  }

  free(raw); free(idat);
  g_rgb   = img;
  g_alpha = alph;
  g_w = fw; g_h = fh;
  snprintf(g_name, sizeof(g_name), "%s", path);
  Serial.printf("[DOG] loaded /%s: %dx%d px, %u KB buffers\n",
                path, fw, fh, (unsigned)(fw * fh * 3 / 1024));
  return true;
}

// Composite row y of the image against a solid background color (displayed-space
// RGB) and emit panel-ready pixels. Two transformations are needed for this panel:
//   1. The panel displays the bitwise inverse of what is written
//      (displayed = ~written), so invert every pixel.
//   2. LovyanGFX v1 pushImage(const uint16_t*) with _swapBytes=false treats
//      the buffer as swap565_t - i.e. byte-swapped RGB565 - and swaps each
//      pixel again before sending it to the panel. Store bswap16(~natural):
//      the library swaps it back to native, the panel inverts it, and the
//      result is the natural color.
void compositeRow(int y, uint8_t bgR, uint8_t bgG, uint8_t bgB, uint16_t* out) {
  if (!g_rgb || !g_alpha || y < 0 || y >= g_h) return;
  const uint16_t* px = g_rgb + (size_t)y * g_w;
  const uint8_t* al = g_alpha + (size_t)y * g_w;
  for (int x = 0; x < g_w; x++) {
    // Expand RGB565 back to 8-bit so the alpha blend matches PIL's math.
    uint16_t p = px[x];
    uint8_t r = (((p >> 11) & 31u) << 3) | (((p >> 11) & 31u) >> 2);   // 5 -> 8 bit
    uint8_t g = (((p >> 5) & 63u) << 2) | (((p >> 5) & 63u) >> 4);     // 6 -> 8 bit
    uint8_t b = ((p & 31u) << 3) | ((p & 31u) >> 2);                   // 5 -> 8 bit
    int a = al[x];   // 255 (opaque) leaves the pixel untouched
    r = (uint8_t)((r * a + bgR * (255 - a)) / 255);
    g = (uint8_t)((g * a + bgG * (255 - a)) / 255);
    b = (uint8_t)((b * a + bgB * (255 - a)) / 255);
    uint16_t v = ~rgb565(r, g, b);
    out[x] = (uint16_t)((v << 8) | (v >> 8));
  }
}

bool dogRowSpan(int y, int& x0, int& x1) {
  if (!g_alpha || y < 0 || y >= g_h) return false;
  const uint8_t* al = g_alpha + (size_t)y * g_w;
  int a = 0;
  while (a < g_w && al[a] == 0) a++;
  if (a >= g_w) return false;
  int b = g_w - 1;
  while (b >= 0 && al[b] == 0) b--;
  x0 = a;
  x1 = b;
  return true;
}
