// ---------------------------------------------------------------------------
//  NWS active alerts - see nws_alerts.h.
//
//  One HTTPS GET per fetch, scoped HTTP client, byte-capped body (NWS
//  pretty-prints, ~3 KB raw per alert) and a DeserializationFilter that
//  keeps only features[].properties. The cache is rewritten in place and
//  committed only after a fully parsed response, so every failure path
//  (network, HTTP error, parse error, truncated body) keeps whatever was
//  there before.
// ---------------------------------------------------------------------------

#include "nws_alerts.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <cstdlib>   // strtol (numeric HTML entities)

#include "config.h"      // userSettings, ALERTS_REFRESH_*_MS
#include "wifi_conn.h"   // wifiEnsureConnected()
#include "time_util.h"   // makeUtcTime()

static const char* NWS_UA =
    "CYD-Weather/1.0 (personal ESP32 weather dashboard)";
static const uint16_t NWS_HTTP_TIMEOUT_MS = 5000;   // connect + read timeout
static const unsigned long NWS_HTTP_HANDSHAKE_S = 10;

// Byte cap on the download. 32 KB covers a heavy multi-alert storm for one
// point; when the cap is hit mid-document the parse fails and the previous
// cache is kept (safe). The parsed JsonDocument stays small because the
// deserialization filter keeps only features[].properties, and it is freed
// when alertsFetch() returns (same pattern as weather.cpp).
static const size_t ALERTS_BODY_CAP = 32768;

// ---------------------------------------------------------------------------
//  Cache + cadence state
// ---------------------------------------------------------------------------

static NwsAlert   gAlerts[NWS_ALERTS_MAX];
static int        gCount = 0;
static uint32_t   gLastAttemptMs = 0;   // millis() of the last fetch attempt
static uint32_t   gLastSuccessMs = 0;   // millis() of the last successful fetch
static bool       gForce = true;        // boot: fetch as soon as Wi-Fi is up
static uint32_t   gSig = 0;             // fingerprint of the committed cache

// FNV-1a over count + (severity, event, ends) of each cached alert: cheap
// enough to answer "did the set change" for the redraw decision.
static uint32_t signatureOf(const NwsAlert* list, int n) {
  uint32_t s = 2166136261u;
  for (int i = 0; i < n; i++) {
    for (int j = 0; j < (int)sizeof(list[i].severity); j++)
      s = (s ^ (uint8_t)list[i].severity[j]) * 16777619u;
    for (int j = 0; j < (int)sizeof(list[i].event); j++)
      s = (s ^ (uint8_t)list[i].event[j]) * 16777619u;
    s = (s ^ (uint32_t)list[i].ends) * 16777619u;
  }
  s = (s ^ (uint32_t)n) * 16777619u;
  return s;
}

// ---------------------------------------------------------------------------
//  Small text helpers
// ---------------------------------------------------------------------------

// Copy `in` to `out` uppercased, truncated to n-1 (event / severity / area
// are shown in caps on the alert screen).
static void copyUpper(const char* in, char* out, size_t n) {
  out[0] = 0;
  if (!in || n == 0) return;
  size_t o = 0;
  for (const char* p = in; *p && o < n - 1; p++) {
    char c = *p;
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    out[o++] = c;
  }
  out[o] = 0;
}

// True when the tag text between < and > (inclusive) is a line break:
// <br> and the block closers </li> / </p>, case-insensitive, with an
// optional '/' and stray spaces (NWS emits <br>, <BR/>, </li>, ...).
static bool isBreakTag(const char* tag, size_t len) {
  char b[8];
  int k = 0;
  for (size_t i = 0; i < len && tag[i] != '>' && k < 8; i++) {
    char c = tag[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c != ' ' && c != '<') b[k++] = c;   // the leading '<' is part of the span
  }
  b[k] = 0;
  return !strcmp(b, "br") || !strcmp(b, "br/") || !strcmp(b, "/br") ||
         !strcmp(b, "li") || !strcmp(b, "/li") ||
         !strcmp(b, "p")  || !strcmp(b, "/p");
}

// Remove HTML tags, unescape the entities NWS actually emits, collapse
// whitespace runs, and truncate at n-1. NWS descriptions/instructions are
// HTML with <br>, <b>, <ul>/<li> and &nbsp;.
//
// Line structure: NWS wraps its own text at ~60 columns, so a <br>, </li>,
// </p> or raw newline mid-item is just source formatting and becomes a
// space (the words reflow). Only the break that introduces a "* LABEL" item
// is structural and kept as '\n'; runs collapse to one break and a space
// right after a break is dropped.
//
// NWS's "* WHAT..." label style: a run of 3+ dots running straight into the
// next word is rendered as a spaced dash ("WHAT - ...") — the "..." is
// hard to read in the GLCD font.
//
// The output is 7-bit text for the GLCD font. Multi-byte UTF-8 is decoded
// and each code point mapped to its 7-bit equivalent (NWS text is supposed
// to be plain ASCII, but some revisions carry U+00A0 no-break spaces,
// curly quotes, dashes and degrees; raw UTF-8 bytes have no glyphs in the
// font and would draw one wrong glyph per byte). Malformed, overlong or
// unmapped code points degrade to a space.

// Replacement text for a Unicode code point with no 7-bit glyph in the 5x7
// font: whitespace variants -> space, curly quotes -> straight, dashes,
// minus, bullet and middle dot -> '-', ellipsis -> "...", degree -> "deg"
// (the font has no degree glyph). Everything else -> space.
static void mapCodePoint(unsigned cp, const char** out, int* len) {
  static const char ellipsis[] = "...";
  static const char deg[] = "deg";
  static char ascii[2];
  const char* s = " ";
  int l = 1;
  if (cp < 0x80) {
    if (cp >= 0x20 && cp <= 0x7E) {        // printable ASCII (e.g. &#x27; -> ')
      ascii[0] = (char)cp;
      ascii[1] = 0;
      s = ascii;
    } else s = " ";                         // C0 controls
  }
  else if (cp == 0x2018 || cp == 0x2019 || cp == 0x201A)  s = "'";
  else if (cp == 0x201C || cp == 0x201D || cp == 0x201E)  s = "\"";
  else if (cp == 0x2013 || cp == 0x2014 || cp == 0x2012 || cp == 0x2015 ||
           cp == 0x2212 || cp == 0x2022 || cp == 0x00B7)  s = "-";
  else if (cp == 0x2026) { s = ellipsis; l = 3; }
  else if (cp == 0x00B0) { s = deg;      l = 3; }
  *out = s;
  *len = l;
}

static void htmlToText(const char* in, char* out, size_t n) {
  out[0] = 0;
  if (!in || n == 0) return;
  size_t o = 0;
  bool inTag = false, prevSpace = true;
  const char* tagStart = nullptr;
  // One output char with the space-collapse rules (no double spaces, no
  // space right after a line break).
  auto emitChar = [&](char c) {
    if (o >= n - 1) return;
    if (c == ' ') {
      if (!prevSpace && !(o && out[o - 1] == '\n')) {   // no leading space on a new line
        out[o++] = ' ';
        prevSpace = true;
      }
      return;
    }
    prevSpace = false;
    out[o++] = c;
  };
  // First real text char from pos: skips spaces, newlines and whole tags.
  auto nextReal = [&](const char* q) -> char {
    while (*q) {
      if (*q == '<') { while (*q && *q != '>') q++; if (*q) q++; continue; }
      if (*q == ' ' || *q == '\n' || *q == '\r') { q++; continue; }
      return *q;
    }
    return 0;
  };
  // Line break. Kept only when it introduces a "* LABEL" item (nextReal ==
  // '*'); every other break is NWS's ~60-column source wrapping and is
  // reflowed as a space.
  auto emitBreak = [&](const char* after) {
    if (nextReal(after) != '*') { emitChar(' '); return; }
    if (o == 0) { prevSpace = true; return; }    // break at the very start: nothing above
    if (out[o - 1] == '\n') return;              // collapse runs to one break
    if (o < n - 1) { out[o++] = '\n'; prevSpace = true; }
  };
  for (const char* p = in; *p && o < n - 1; p++) {
    char c = *p;
    if (c == '<') { inTag = true; tagStart = p; continue; }
    if (c == '>') {
      if (inTag && isBreakTag(tagStart, (size_t)(p - tagStart) + 1)) emitBreak(p + 1);
      inTag = false;
      continue;
    }
    if (inTag) continue;
    if (c == '&') {
      const char* rep = nullptr;
      int rlen = 1;
      if      (!strncmp(p, "&nbsp;", 6))   { rep = " ";   p += 5; }
      else if (!strncmp(p, "&amp;", 5))    { rep = "&";   p += 4; }
      else if (!strncmp(p, "&lt;", 4))     { rep = "<";   p += 3; }
      else if (!strncmp(p, "&gt;", 4))     { rep = ">";   p += 3; }
      else if (!strncmp(p, "&quot;", 6))   { rep = "\"";  p += 5; }
      else if (!strncmp(p, "&#39;", 5))    { rep = "'";   p += 4; }
      else if (!strncmp(p, "&apos;", 6))   { rep = "'";   p += 5; }
      else if (!strncmp(p, "&ldquo;", 7))  { rep = "\"";  p += 6; }
      else if (!strncmp(p, "&rdquo;", 7))  { rep = "\"";  p += 6; }
      else if (!strncmp(p, "&lsquo;", 7))  { rep = "'";   p += 6; }
      else if (!strncmp(p, "&rsquo;", 7))  { rep = "'";   p += 6; }
      else if (!strncmp(p, "&mdash;", 7))  { rep = "-";   p += 6; }
      else if (!strncmp(p, "&ndash;", 7))  { rep = "-";   p += 6; }
      else if (!strncmp(p, "&bull;", 6))   { rep = "-";   p += 5; }
      else if (!strncmp(p, "&hellip;", 7)) { rep = "..."; p += 6; rlen = 3; }
      else if (!strncmp(p, "&deg;", 5))    { rep = "deg"; p += 4; rlen = 3; }
      else if (p[1] == '#') {            // numeric ref: &#160; / &#x27; / &#8217;
        const bool hex = (p[2] == 'x' || p[2] == 'X');
        char* end;
        const long v = strtol(p + 2 + (hex ? 1 : 0), &end, hex ? 16 : 10);
        if (end > p + 2 + (hex ? 1 : 0) && *end == ';') {
          p = end;
          mapCodePoint(v >= 0 ? (unsigned)v : 0u, &rep, &rlen);
        }
      }
      if (rep) {
        for (int k = 0; k < rlen; k++) emitChar(rep[k]);
        continue;
      }
      // Unknown or malformed reference: drop the whole "&name;" when the
      // name is alphanumeric and short enough (NWS entities are 5 chars);
      // otherwise keep the literal '&' ("fish & chips").
      size_t k = 1;
      while (k < 16 && p[k] && p[k] != ';' &&
             ((p[k] >= '0' && p[k] <= '9') || (p[k] >= 'a' && p[k] <= 'z') ||
              (p[k] >= 'A' && p[k] <= 'Z')))
        k++;
      if (p[k] == ';') p += k;            // the loop's p++ steps past the ';'
      else emitChar('&');
      continue;
    }
    if ((unsigned char)c >= 0x80) {
      // UTF-8 lead byte: decode the sequence, then emit the code point's
      // 7-bit equivalent.
      unsigned char b0 = (unsigned char)c;
      unsigned cp = 0;
      int ncont = -1;   // continuation bytes expected; -1 = invalid lead byte
      if      (b0 >= 0xC2 && b0 <= 0xDF) { cp = b0 & 0x1Fu; ncont = 1; }
      else if (b0 >= 0xE0 && b0 <= 0xEF) { cp = b0 & 0x0Fu; ncont = 2; }
      else if (b0 >= 0xF0 && b0 <= 0xF4) { cp = b0 & 0x07u; ncont = 3; }
      bool ok = (ncont > 0);
      for (int k = 0; ok && k < ncont; k++) {
        unsigned char b = (unsigned char)p[k + 1];
        if ((b & 0xC0) != 0x80) { ok = false; break; }   // not a continuation byte
        cp = (cp << 6) | (b & 0x3Fu);
      }
      if (ok && ((ncont == 1 && cp < 0x80) || (ncont == 2 && cp < 0x800) ||
                 (ncont == 3 && cp < 0x10000)))
        ok = false;   // overlong encoding
      p += (ok ? ncont : 0);   // the loop's p++ covers the lead byte
      const char* rep;
      int rlen;
      mapCodePoint(ok ? cp : 0u, &rep, &rlen);
      for (int k = 0; k < rlen; k++) emitChar(rep[k]);
      continue;
    }
    if ((unsigned char)c < 32) {
      if (c == '\n' || c == '\r') { emitBreak(p + 1); continue; }
      c = ' ';   // other C0 controls: no glyph
    }
    emitChar(c);
  }
  while (o > 0 && (out[o - 1] == '\n' || out[o - 1] == ' ')) o--;   // no dangling break/space
  // NWS's "* WHAT..." label style: a run of 3+ dots running straight into
  // the next word becomes a spaced dash.
  {
    size_t w = 0;
    for (size_t i = 0; i < o;) {
      if (i + 2 < o && out[i] == '.' && out[i + 1] == '.' && out[i + 2] == '.') {
        size_t j = i;
        while (j < o && out[j] == '.') j++;
        const char next = (j < o) ? out[j] : 0;
        if (next != ' ' && next != '\n' && next != 0) {
          const char prev = (i > 0) ? out[i - 1] : 0;
          if (prev && prev != ' ' && prev != '\n') out[w++] = ' ';
          out[w++] = '-';
          out[w++] = ' ';
          i = j;
          continue;
        }
      }
      out[w++] = out[i++];
    }
    o = w;
  }
  out[o] = 0;
}

// One-shot boot-log hint when a raw alert field carries bytes outside
// 7-bit ASCII. htmlToText decodes and converts them to supported 7-bit
// equivalents, but the line pinpoints which revision of NWS text carried
// them.
static void logNonAscii(const char* field, const char* s) {
  if (!s) return;
  for (const char* p = s; *p; p++) {
    if ((unsigned char)*p >= 0x80) {
      const char* ctx = p - 16 < s ? s : p - 16;
      Serial.printf("[ALERTS] non-ASCII 0x%02X in %s at offset %d, after: ...%.*s\n",
                    (unsigned)*p, field, (int)(p - s),
                    (int)(p - ctx), ctx);
      return;
    }
  }
}

// "2026-11-27T01:00:00-05:00" / "2026-11-27T01:00:00Z" -> UTC epoch
// seconds; 0 on failure. Parsed by hand: newlib's strptime %z is
// inconsistent with the colon offset form NWS sends, and timegm() is not
// declared on the ESP32 toolchain (the date part uses makeUtcTime from
// time_util.h, which reduces it arithmetically).
static long parseIso8601(const char* s) {
  if (!s) return 0;
  int Y, M, D, h, mi, se;
  if (sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &mi, &se) != 6) return 0;
  long e = makeUtcTime(Y, M, D, h, mi, se);
  const char* off = s + 19;
  if (*off == 'Z' || *off == '\0') return e;
  char sign = 0;
  int oh = 0, om = 0;
  if (sscanf(off, "%c%d:%d", &sign, &oh, &om) < 1) return e;
  const long secs = ((long)oh * 60 + om) * 60L;
  return (sign == '+') ? e - secs : e + secs;
}

// ---------------------------------------------------------------------------
//  HTTP plumbing (nws_station.cpp pattern: scoped client, bounded timeouts)
// ---------------------------------------------------------------------------

// HTTPS GET with a byte cap on the body. Returns the HTTP status code
// (0 = transport failure); `body` is written only on 200. Chunked transfer
// encoding is decoded by hand — the HTTPClient stream carries the raw chunk
// framing, only getString() decodes it (see the note in nws_station.cpp).
static int alertsGetCapped(const char* url, String& body, size_t cap) {
  WiFiClientSecure client;
  client.setInsecure();   // same self-contained tradeoff as the other clients
  client.setConnectionTimeout(NWS_HTTP_TIMEOUT_MS);
  client.setHandshakeTimeout(NWS_HTTP_HANDSHAKE_S);
  client.setTimeout(NWS_HTTP_TIMEOUT_MS);   // read timeout (NWS keep-alive)

  HTTPClient http;
  if (!http.begin(client, url)) { http.end(); return 0; }
  http.addHeader("User-Agent", NWS_UA);
  http.addHeader("Accept", "application/geo+json");
  const int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return code; }

  WiFiClient* stream = http.getStreamPtr();
  if (!stream) { http.end(); return 0; }
  body = "";
  size_t total = 0;
  char buf[256];

  int first = -1;   // first non-whitespace body byte
  for (;;) {
    const int c = stream->read();
    if (c < 0) break;
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') { first = c; break; }
  }
  if (first == '{') {
    // Plain body (Content-Length): just read it.
    body += (char)first;
    total++;
    while (total < cap) {
      const int n = stream->readBytes(buf, sizeof(buf));
      if (n <= 0) break;
      body.concat(buf, (unsigned int)n);   // exact-length append; not NUL-terminated
      total += (size_t)n;
    }
  } else {
    // Chunked framing: "<hex-size>\r\n<data>\r\n ... 0\r\n\r\n".
    bool done = (first < 0);
    size_t chunkLeft = 0;
    bool inData = false;
    while (!done && total < cap) {
      if (!inData) {
        // Chunk size line (the first digit may already have been read).
        char line[32];
        int ln = 0;
        if (first >= 0) { line[ln++] = (char)first; first = -1; }
        bool eol = false;
        while (ln < (int)sizeof(line) - 1) {
          const int c = stream->read();
          if (c < 0) { done = true; break; }
          line[ln++] = (char)c;
          if (c == '\n') { eol = true; break; }
        }
        if (!eol) { done = true; break; }
        line[ln] = 0;
        for (int i = ln - 1; i >= 0 && (line[i] == '\r' || line[i] == '\n'); i--)
          line[i] = 0;
        char* semi = strchr(line, ';');   // chunk extensions (NWS sends none)
        if (semi) *semi = 0;
        char* endp = nullptr;
        const unsigned long size = strtoul(line, &endp, 16);
        if (endp == line || *endp != 0) { done = true; break; }  // not a size
        if (size == 0) break;                                    // terminating
        chunkLeft = (size_t)size;
        inData = true;
        continue;
      }
      size_t want = (chunkLeft < cap - total) ? chunkLeft : cap - total;
      size_t got = 0;
      while (got < want) {
        const size_t room = want - got;
        const int n = stream->readBytes(buf, room < sizeof(buf) ? room : sizeof(buf));
        if (n <= 0) { done = true; break; }
        body.concat(buf, (unsigned int)n);
        got += (size_t)n;
        total += (size_t)n;
      }
      if (done) break;
      chunkLeft -= got;
      if (chunkLeft == 0) {
        stream->read();   // swallow the CRLF after the chunk data
        stream->read();
        inData = false;
      }
    }
  }
  http.end();
  return code;
}

// ---------------------------------------------------------------------------
//  Public API
// ---------------------------------------------------------------------------

// Severity rank, most severe first: 0 = EXTREME, 1 = SEVERE, 2 = MODERATE,
// 3 = MINOR / unknown. The cache is kept sorted by this (ties preserve the
// NWS feed order) so the alert list is most-severe-first.
static int sevRank(const NwsAlert* a) {
  if      (!strcmp(a->severity, "EXTREME"))  return 0;
  else if (!strcmp(a->severity, "SEVERE"))   return 1;
  else if (!strcmp(a->severity, "MODERATE")) return 2;
  return 3;
}

int alertsCount() { return gCount; }
const NwsAlert* alertsAt(int i) { return (i >= 0 && i < gCount) ? &gAlerts[i] : nullptr; }
bool alertsActive() { return gCount > 0; }

int alertsHighestSeverity() {
  int worst = -1;
  for (int i = 0; i < gCount; i++) {
    const int s = sevRank(&gAlerts[i]);
    if (worst < 0 || s < worst) worst = s;
  }
  return worst;
}

bool alertsDue() {
  if (WiFi.status() != WL_CONNECTED) return false;   // nothing to fetch
  const uint32_t now = millis();
  // The floor between attempts must never block the very first attempt
  // (gLastAttemptMs == 0): otherwise a location set within the first 60 s
  // after boot waits out the whole floor before its first alert check.
  const bool gapOk = (gLastAttemptMs == 0) ||
                     (now - gLastAttemptMs >= ALERTS_MIN_GAP_MS);
  if (!gapOk) return false;
  if (gForce) return true;
  if (gLastSuccessMs == 0) return true;   // never succeeded: keep retrying
  const uint32_t cad = alertsActive() ? ALERTS_REFRESH_ACTIVE_MS
                                      : ALERTS_REFRESH_CLEAR_MS;
  return now - gLastSuccessMs >= cad;
}

void alertsForceDue() { gForce = true; }

void alertsLocationChanged() {
  gCount = 0;          // stale: the old location's alerts no longer apply
  gSig = 0;            // so the dashboard redraws (pill disappears) even if the
                       // refetch never succeeds
  gLastSuccessMs = 0;
  gForce = true;
}

bool alertsFetch() {
  gForce = false;
  gLastAttemptMs = millis();

  if (!wifiEnsureConnected()) {
    Serial.println("[ALERTS] no WiFi, skipped");
    return false;
  }

  char url[96];
  snprintf(url, sizeof(url),
           "https://api.weather.gov/alerts/active?point=%.*f,%.7f",
           6, (double)userSettings.lat, (double)userSettings.lon);

  String body;
  const int code = alertsGetCapped(url, body, ALERTS_BODY_CAP);
  if (code != HTTP_CODE_OK) {
    Serial.printf("[ALERTS] fetch failed (HTTP %d), cache kept\n", code);
    return false;
  }

  // Keep only features[].properties: geometry/polygon, @context and the id
  // links are skipped, which is what keeps the parsed document small.
  // ArduinoJson v7 filter shape: an array keeps all elements when filtered
  // by a JSON ARRAY of element filters, so [ { "properties": true } ]
  // applies that sub-filter to every feature.
  JsonDocument filter;
  JsonArray fa = filter["features"].to<JsonArray>();
  fa.add<JsonVariant>()["properties"] = true;

  JsonDocument doc;
  const ArduinoJson::DeserializationError err = ArduinoJson::deserializeJson(
      doc, body.c_str(), ArduinoJson::DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("[ALERTS] parse failed: %s, cache kept\n", err.c_str());
    return false;
  }

  int n = 0;
  const JsonArray features = doc["features"].as<JsonArray>();
  for (size_t i = 0; i < features.size() && n < NWS_ALERTS_MAX; i++) {
    const JsonVariant p = features[i]["properties"];
    NwsAlert* a = &gAlerts[n];
    copyUpper(p["event"].as<const char*>(), a->event, sizeof(a->event));
    copyUpper(p["severity"].as<const char*>(), a->severity, sizeof(a->severity));
    copyUpper(p["areaDesc"].as<const char*>(), a->area, sizeof(a->area));
    const char* descRaw = p["description"].as<const char*>();
    const char* instrRaw = p["instructions"].as<const char*>();
    logNonAscii("description", descRaw);
    logNonAscii("instructions", instrRaw);
    htmlToText(descRaw, a->description, sizeof(a->description));
    htmlToText(instrRaw, a->instructions, sizeof(a->instructions));
    a->ends = parseIso8601(p["ends"].as<const char*>());
    n++;
  }

  // Most severe first; equal severities keep the NWS feed order (stable).
  // Insertion sort: n <= 8, and the static temp keeps the ~1.5 KB struct
  // copies off the stack.
  static NwsAlert tmp;
  for (int i = 1; i < n; i++) {
    int j = i;
    while (j > 0 && sevRank(&gAlerts[j - 1]) > sevRank(&gAlerts[j])) {
      tmp = gAlerts[j - 1];
      gAlerts[j - 1] = gAlerts[j];
      gAlerts[j] = tmp;
      j--;
    }
  }

  gCount = n;
  gLastSuccessMs = millis();
  const uint32_t sig = signatureOf(gAlerts, n);
  const bool changed = (sig != gSig);
  gSig = sig;
  Serial.printf("[ALERTS] %d active%s\n", gCount, changed ? " (changed)" : "");
  return changed;
}
