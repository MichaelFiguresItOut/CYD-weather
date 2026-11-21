// ---------------------------------------------------------------------------
//  NWS weather-station discovery - see nws_station.h.
//
//  The points and stations requests follow links handed back by NWS itself;
//  the latest-observation request is the one documented per-station endpoint
//  built from the ICAO identifier (the features carry no obs link).
//
//  Heap discipline: each stage allocates a scoped HTTP client, one response
//  String, and one JsonDocument, and all of it is freed before the next
//  stage. The station list is the one large document (a metro area returns
//  ~40 stations of pretty-printed geo+json, ~60 KB), so it is downloaded
//  with a byte cap and trimmed to whole features before parsing.
// ---------------------------------------------------------------------------

#include "nws_station.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "wifi_conn.h"   // wifiEnsureConnected()
#include "time_util.h"   // ntpIsSynced()

static const char* NWS_UA =
    "CYD-Weather/1.0 (personal ESP32 weather dashboard)";

static const uint16_t NWS_HTTP_TIMEOUT_MS = 5000;   // connect + read timeout
static const unsigned long NWS_HTTP_HANDSHAKE_S = 10;

// Byte cap on the station-list download. NWS orders the features nearest
// first; 15 stations covers any realistic candidate (in NYC the 15th is
// ~80 km out). The full metro list would be ~60 KB - too much for the free
// heap while the picker sprites and dog buffers are resident.
static const size_t NWS_LIST_BODY_CAP = 24576;
static const int NWS_STATION_PARSE_MAX = 15;

// ---------------------------------------------------------------------------
//  HTTP plumbing (geocode.cpp pattern: scoped client, bounded timeouts)
// ---------------------------------------------------------------------------

// One HTTPS GET against a full NWS URL. Returns the HTTP status code (0 =
// transport failure); `body` is written only on 200.
static int nwsGet(const String& url, String& body) {
  String host, path;
  if (url.indexOf("https://") != 0) return 0;
  const int sep = url.indexOf("://");
  const int slash = url.indexOf('/', sep + 3);   // -1 when the host has no path
  host = (slash < 0) ? url.substring(sep + 3) : url.substring(sep + 3, slash);
  path = (slash < 0) ? String("/") : url.substring(slash);
  if (host.length() == 0) return 0;

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
  if (code == HTTP_CODE_OK) body = http.getString();
  http.end();
  return code;
}

// Same as nwsGet(), but reads at most `cap` bytes of the body and closes
// the connection. Used for the station list (see NWS_LIST_BODY_CAP).
//
// NWS serves the station list with chunked transfer encoding: the raw
// client stream carries the chunk framing ("<hex>\r\n<data>\r\n ... 0\r\n\r\n"),
// which HTTPClient::getString() would decode but has no byte cap. The
// framing is detected from the first non-whitespace body byte - NWS JSON
// starts with '{' (plain, content-length), a hex digit starts a chunk size
// line - and decoded by hand as it is read.
static int nwsGetCapped(const String& url, String& body, size_t cap) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setConnectionTimeout(NWS_HTTP_TIMEOUT_MS);
  client.setHandshakeTimeout(NWS_HTTP_HANDSHAKE_S);
  client.setTimeout(NWS_HTTP_TIMEOUT_MS);   // read timeout: NWS keep-alive
                                            // must not hang the read loop

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
  const bool plain = (first == '{');
  if (plain) {
    body += (char)first;
    total++;
    while (total < cap) {
      const int n = stream->readBytes(buf, sizeof(buf));
      if (n <= 0) break;
      body.concat(buf, (unsigned int)n);   // exact-length append; not NUL-terminated
      total += (size_t)n;
    }
  } else {
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
//  JSON helpers
// ---------------------------------------------------------------------------

// Cut a NWS station-list body so it ends after the nth COMPLETE feature
// (or at the natural end of the features array, whichever comes first) and
// is therefore still valid JSON. String-aware: station names may contain
// braces. Returns the number of complete features kept (0 = none found).
// The tail after the cut (NWS pagination fields) is dropped.
static int trimFeatures(String& body, int n) {
  const char* b = body.c_str();
  const size_t len = body.length();
  const char* fs = strstr(b, "\"features\":");
  if (!fs) return 0;
  size_t p = (size_t)(fs - b) + strlen("\"features\":");
  while (p < len && (b[p] == ' ' || b[p] == '\t')) p++;   // ": [" (pretty-printed)
  if (p >= len || b[p] != '[') return 0;
  p++;

  int depth = 0;            // brace depth inside the features array
  bool inStr = false, esc = false;
  int kept = 0;
  size_t cut = 0;
  bool naturalEnd = false;
  for (size_t i = p; i < len; i++) {
    const char c = b[i];
    if (inStr) {
      if (esc) esc = false;
      else if (c == '\\') esc = true;
      else if (c == '"') inStr = false;
      continue;
    }
    if (c == '"') inStr = true;
    else if (c == '{') depth++;
    else if (c == '}') {
      depth--;
      if (depth == 0) {
        kept++;
        cut = i + 1;
        if (kept >= n) break;
      }
    } else if (c == ']' && depth == 0) {
      cut = i + 1;          // features array closed on its own
      naturalEnd = true;
      break;
    }
  }
  if (cut == 0) return 0;
  String t = body.substring(0, cut);
  t += naturalEnd ? "}" : "]}";       // close features array + root object
  body = t;
  return kept;
}

// Days from 1970-01-01 for a civil date (Howard Hinnant's
// days_from_civil algorithm) - timegm() is not declared on the ESP32
// toolchain, so the date is reduced arithmetically instead.
static int64_t civilEpochDays(int y, int m, int d) {
  const int64_t yy = (int64_t)y - (m <= 2 ? 1 : 0);
  const int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
  const int64_t yoe = yy - era * 400;                                  // [0, 399]
  const int64_t doy = (153 * (int64_t)(m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  // [0, 365]
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           // [0, 146096]
  return era * 146097 + doe - 719468;
}

// "2026-11-21T02:51:00+00:00" (fractional seconds or 'Z' tolerated) ->
// UTC epoch seconds. Returns 0 when unparseable (epoch 0 is not a valid
// NWS timestamp, so callers can treat it as "no observation").
static int64_t iso8601ToEpoch(const char* s) {
  int y, mo, d, h, mi, se;
  if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
  int64_t e = civilEpochDays(y, mo, d) * 86400 + h * 3600 + mi * 60 + se;
  // Offset (or 'Z') follows the seconds, possibly after fractional digits.
  for (const char* p = s + 19; *p; p++) {
    if (*p == 'Z') break;   // UTC
    if (*p == '+' || *p == '-') {
      int oh, om;
      if (sscanf(p + 1, "%d:%d", &oh, &om) == 2)
        e += (*p == '-') ? (int64_t)(oh * 3600 + om * 60)
                         : -(int64_t)(oh * 3600 + om * 60);
      break;
    }
  }
  return e;
}

// ---------------------------------------------------------------------------
//  Stage 1: /points/{lat},{lon}
// ---------------------------------------------------------------------------

int nwsPointsLookup(float lat, float lon, char* stationsUrl, size_t urlLen) {
  if (!stationsUrl || urlLen < 2) return -1;
  if (!wifiEnsureConnected()) return -1;

  String path = String("/points/") + String(lat, 4) + "," + String(lon, 4);
  String body;
  const int code = nwsGet(String("https://api.weather.gov") + path, body);
  if (code != HTTP_CODE_OK) {
    if (code == HTTP_CODE_NOT_FOUND) {   // 404 InvalidPoint: outside NWS coverage
      Serial.printf("[NWS] point %.4f,%.4f not covered by NWS\n", lat, lon);
      return -2;
    }
    Serial.printf("[NWS] points lookup HTTP %d\n", code);
    return -1;
  }

  JsonDocument doc;
  // ArduinoJson v7: a non-Ok DeserializationError converts to true.
  if (ArduinoJson::deserializeJson(doc, body)) {
    Serial.println("[NWS] points response unparseable");
    return -3;
  }
  const char* url = doc["properties"]["observationStations"].as<const char*>();
  if (!url || !*url) {
    Serial.println("[NWS] points response has no observationStations link");
    return -3;
  }
  snprintf(stationsUrl, urlLen, "%s", url);
  Serial.printf("[NWS] points ok: grid %s, stations url %s\n",
                doc["properties"]["gridId"].as<const char*>(), url);
  return 0;
}

// ---------------------------------------------------------------------------
//  Stage 2: station list
// ---------------------------------------------------------------------------

int nwsFetchStations(const char* stationsUrl, NwsStation* out, int maxStations) {
  if (!stationsUrl || !out || maxStations < 1) return -1;
  if (!wifiEnsureConnected()) return -1;

  String body;
  const int code = nwsGetCapped(stationsUrl, body, NWS_LIST_BODY_CAP);
  if (code != HTTP_CODE_OK) {
    Serial.printf("[NWS] station list HTTP %d\n", code);
    return -1;
  }

  const int kept = trimFeatures(body, NWS_STATION_PARSE_MAX);
  JsonDocument doc;
  if (ArduinoJson::deserializeJson(doc, body)) {   // non-Ok error is true
    Serial.printf("[NWS] station list unparseable (kept %d features)\n", kept);
    return -3;
  }
  JsonVariant feats = doc["features"];
  if (!feats.is<JsonArray>()) {
    Serial.println("[NWS] station list has no features array");
    return -3;
  }

  int n = 0;
  for (JsonVariant fv : feats.as<JsonArray>()) {
    if (n >= maxStations) break;
    JsonVariant props = fv["properties"];
    const char* id = props["stationIdentifier"].as<const char*>();
    if (!id || !*id) continue;

    NwsStation* s = &out[n];
    memset(s, 0, sizeof(*s));
    snprintf(s->id, sizeof(s->id), "%.7s", id);
    snprintf(s->name, sizeof(s->name), "%.39s",
             props["name"].as<const char*>() ? props["name"].as<const char*>() : "");
    JsonVariant coords = fv["geometry"]["coordinates"];
    s->lon = coords[0].as<float>();   // geojson order is [lon, lat]
    s->lat = coords[1].as<float>();
    s->distKm = props["distance"]["value"].as<float>() / 1000.0f;  // NWS: metres
    snprintf(s->provider, sizeof(s->provider), "%.15s",
             props["provider"].as<const char*>() ? props["provider"].as<const char*>() : "");
    s->hasObs = false;
    s->obsFail = false;
    s->obsAgeSec = -1;
    s->obsTempC = NAN;
    s->obsText[0] = 0;
    n++;
  }
  Serial.printf("[NWS] station list: %d stored (features kept %d)\n", n, kept);
  return n;
}

// ---------------------------------------------------------------------------
//  Stage 3: per-station latest observation
// ---------------------------------------------------------------------------

int nwsFetchLatestObs(NwsStation* st) {
  if (!st || !st->id[0]) return -3;
  if (!wifiEnsureConnected()) { st->obsFail = true; return -1; }

  String url = String("https://api.weather.gov/stations/") + st->id +
               "/observations/latest";
  String body;
  const int code = nwsGet(url, body);
  if (code != HTTP_CODE_OK) {
    Serial.printf("[NWS] %s observation HTTP %d\n", st->id, code);
    st->obsFail = true;
    return -1;
  }

  JsonDocument doc;
  if (ArduinoJson::deserializeJson(doc, body)) {   // non-Ok error is true
    Serial.printf("[NWS] %s observation unparseable\n", st->id);
    st->obsFail = true;
    return -3;
  }
  JsonVariant props = doc["properties"];
  const char* ts = props["timestamp"].as<const char*>();
  const int64_t epoch = ts ? iso8601ToEpoch(ts) : 0;
  if (epoch == 0) {
    Serial.printf("[NWS] %s observation has no usable timestamp\n", st->id);
    st->obsFail = true;
    return -3;
  }

  st->hasObs = true;
  st->obsFail = false;
  JsonVariant tv = props["temperature"]["value"];
  st->obsTempC = tv.isNull() ? NAN : tv.as<float>();
  snprintf(st->obsText, sizeof(st->obsText), "%.23s",
           props["textDescription"].as<const char*>() ? props["textDescription"].as<const char*>() : "");
  if (ntpIsSynced()) {
    int64_t now = (int64_t)time(nullptr);
    long age = (long)(now - epoch);
    st->obsAgeSec = (age < 0) ? 0 : age;   // clock skew: treat as fresh
  } else {
    st->obsAgeSec = -1;                    // no NTP: age unknown
  }
  Serial.printf("[NWS] %s obs: age %lds, %.1fC, %s\n",
                st->id, st->obsAgeSec, st->obsTempC, st->obsText);
  return 0;
}

// ---------------------------------------------------------------------------
//  Display helper
// ---------------------------------------------------------------------------

void nwsAgeText(long ageSec, char* out, size_t outLen) {
  if (outLen == 0) return;
  if (ageSec < 0) { snprintf(out, outLen, "AGE UNKNOWN"); return; }
  if (ageSec < 90) { snprintf(out, outLen, "JUST NOW"); return; }
  // "AGO" suffix: the row otherwise just says "... - 32M", which reads like
  // a distance, not an observation age.
  if (ageSec < 3600) snprintf(out, outLen, "%ldM AGO", ageSec / 60);
  else if (ageSec < 86400)
    snprintf(out, outLen, "%ldH %02ldM AGO", ageSec / 3600, (ageSec % 3600) / 60);
  else
    snprintf(out, outLen, "%ldD %ldH AGO", ageSec / 86400, (ageSec % 86400) / 3600);
}
