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
    s->obsWindMph = NAN;
    s->obsRelHum = NAN;
    s->obsEpochSec = 0;
    s->obsText[0] = 0;
    n++;
  }
  Serial.printf("[NWS] station list: %d stored (features kept %d)\n", n, kept);
  return n;
}

// ---------------------------------------------------------------------------
//  Observation property parsing (unit-aware)
//
//  NWS reports observation values with a wmoUnit code and the unit is not
//  guaranteed to be the "expected" one (the KMDW capture in test/nws/ is
//  degC + km/h, many US stations report degF + mph), so every value is
//  converted from its own unitCode rather than assumed.
// ---------------------------------------------------------------------------

// Generic numeric property (e.g. relativeHumidity, already in %): the
// "value" member of a {unitCode, value} object, or NaN when absent/null.
static float nwsObsNum(JsonVariant obj) {
  JsonVariant v = obj["value"];
  return v.isNull() ? NAN : v.as<float>();
}

// Temperature {unitCode, value} -> °C (degF converted; degC and anything
// unrecognized passed through as °C - NWS only sends degC/degF).
static float nwsObsValueF2C(JsonVariant obj) {
  float v = nwsObsNum(obj);
  if (isnan(v)) return v;
  const char* u = obj["unitCode"].as<const char*>();
  if (u && strstr(u, "degF")) v = (v - 32.0f) * 5.0f / 9.0f;
  return v;
}

// Wind speed {unitCode, value} -> mph (km/h converted; mi_h passed
// through; anything else -> NaN).
static float nwsObsWindMph(JsonVariant obj) {
  float v = nwsObsNum(obj);
  if (isnan(v)) return v;
  const char* u = obj["unitCode"].as<const char*>();
  if (u && strstr(u, "km_h")) v /= 1.609344f;
  else if (!u || !strstr(u, "mi_h")) return NAN;
  return v;
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
  st->obsEpochSec = (long)epoch;
  st->obsTempC = nwsObsValueF2C(props["temperature"]);   // unitCode-aware
  st->obsWindMph = nwsObsWindMph(props["windSpeed"]);    // unitCode-aware
  st->obsRelHum = nwsObsNum(props["relativeHumidity"]);  // already %
  snprintf(st->obsText, sizeof(st->obsText), "%.23s",
           props["textDescription"].as<const char*>() ? props["textDescription"].as<const char*>() : "");
  if (ntpIsSynced()) {
    int64_t now = (int64_t)time(nullptr);
    long age = (long)(now - epoch);
    st->obsAgeSec = (age < 0) ? 0 : age;   // clock skew: treat as fresh
  } else {
    st->obsAgeSec = -1;                    // no NTP: age unknown
  }
  Serial.printf("[NWS] %s obs: age %lds, %.1fC, wind %.1f mph, RH %.0f%%, %s\n",
                st->id, st->obsAgeSec, st->obsTempC, st->obsWindMph,
                st->obsRelHum, st->obsText);
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

// ---------------------------------------------------------------------------
//  Phase 4: automatic backup stations + observation availability.
//
//  Pure logic (no network, no NVS): the settings UI calls
//  nwsBuildFallbackList() with the station list it just fetched and the
//  user's pick; the dashboard source selection (Phase 5) will call
//  nwsFindUsable() with the fallback-ordered list after fetching
//  observations. Everything here is deterministic and host-testable
//  (test/nws/test_fallback.py mirrors it 1:1).
// ---------------------------------------------------------------------------

bool nwsStationSuitable(const NwsStation* st) {
  if (!st || !st->id[0]) return false;
  char p[16];
  snprintf(p, sizeof(p), "%s", st->provider);
  for (char* c = p; *c; c++) *c = (char)toupper((unsigned char)*c);
  return !strstr(p, "COCORAH");
}

int nwsBuildFallbackList(const NwsStation* candidates, int nCandidates,
                         const char* preferredId, NwsStation* out,
                         int maxBackups) {
  if (!candidates || !out || maxBackups < 0) return 0;
  if (maxBackups > NWS_BACKUP_MAX) maxBackups = NWS_BACKUP_MAX;
  int n = 0;
  for (int i = 0; i < nCandidates && n < maxBackups; i++) {
    const NwsStation* c = &candidates[i];
    if (!nwsStationSuitable(c)) continue;                       // CoCoRaHS
    if (c->distKm > NWS_BACKUP_MAX_KM) continue;                 // unrelated
    if (preferredId && strcmp(c->id, preferredId) == 0) continue;
    bool dup = false;
    for (int k = 0; k < n; k++)
      if (strcmp(out[k].id, c->id) == 0) { dup = true; break; }
    if (dup) continue;
    out[n] = *c;
    n++;
  }
  return n;
}

bool nwsObsUsable(const NwsStation* st) {
  return st && st->hasObs && st->obsAgeSec >= 0 &&
         st->obsAgeSec <= NWS_OBS_FRESH_MAX_S;
}

int nwsFindUsable(const NwsStation* list, int n) {
  for (int i = 0; i < n; i++)
    if (nwsObsUsable(&list[i])) return i;
  return -1;   // no usable NWS station -> Open-Meteo is the source
}

// ---------------------------------------------------------------------------
//  Phase 5: dashboard reading from NWS - acceptance + feels-like.
//  Pure logic, host-testable (test/nws/test_feels.py mirrors 1:1).
// ---------------------------------------------------------------------------

bool nwsObsHasValidTemp(const NwsStation* st) {
  return st && st->hasObs && !isnan(st->obsTempC);
}

float nwsWindChillF(float tempF, float windMph) {
  // NWS/Environment Canada wind-chill formula (F, mph; valid wind >= 3 mph).
  const float v = powf(windMph, 0.16f);
  return 35.74f + 0.6215f * tempF - 35.75f * v + 0.4275f * tempF * v;
}

float nwsHeatIndexF(float tempF, float relHumPct) {
  // NWS heat index: Rothfusz (2005) regression (F, RH %).
  float hi = -42.379f
           + 2.04901523f  * tempF
           + 10.14333127f * relHumPct
           - 0.22475541f  * tempF * relHumPct
           - 6.83783e-3f  * tempF * tempF
           - 5.481717e-2f * relHumPct * relHumPct
           + 1.22874e-3f  * tempF * tempF * relHumPct
           + 8.5282e-4f   * tempF * relHumPct * relHumPct
           - 1.99e-6f     * tempF * tempF * relHumPct * relHumPct;
  // Published low-humidity adjustment: RH < 13% and 80F <= T <= 112F
  // (the 14 below is part of the adjustment formula, not the threshold).
  if (relHumPct < 13.0f && tempF >= 80.0f && tempF <= 112.0f) {
    hi += sqrtf((14.0f - relHumPct) / 4.0f) * sqrtf((170.0f - tempF) / 17.0f);
  }
  // Published high-humidity adjustment: RH > 85% and 80F <= T <= 87F.
  if (relHumPct > 85.0f && tempF >= 80.0f && tempF <= 87.0f) {
    hi += ((relHumPct - 85.0f) / 10.0f) * ((87.0f - tempF) / 5.0f);
  }
  return hi;
}

float nwsFeelsLikeF(float tempF, float windMph, float relHumPct) {
  if (isnan(tempF)) return tempF;
  // Cold, windy -> wind chill (the published applicability limits).
  if (!isnan(windMph) && tempF <= 50.0f && windMph >= 3.0f)
    return nwsWindChillF(tempF, windMph);
  // Hot and humid enough -> heat index.
  if (!isnan(relHumPct) && tempF >= 80.0f && relHumPct >= 40.0f)
    return nwsHeatIndexF(tempF, relHumPct);
  return tempF;   // in between, or missing inputs: air temperature
}

// ---------------------------------------------------------------------------
//  Phase 6.5: quality-based preferred + backup selection - rules in
//  nws_station.h. Pure logic, host-testable (test/nws/test_rank.py
//  mirrors this 1:1).
// ---------------------------------------------------------------------------

float nwsHaversineKm(float lat1, float lon1, float lat2, float lon2) {
  const float R = 6371.0088f;   // mean Earth radius, km
  const float dLat = radians(lat2 - lat1);
  const float dLon = radians(lon2 - lon1);
  const float a = sinf(dLat * 0.5f) * sinf(dLat * 0.5f) +
                  cosf(radians(lat1)) * cosf(radians(lat2)) *
                  sinf(dLon * 0.5f) * sinf(dLon * 0.5f);
  return 2.0f * R * asinf(sqrtf(a > 1.0f ? 1.0f : a));
}

void nwsRankPrepare(NwsStation* st, int n, float lat, float lon) {
  if (!st || n < 1) return;
  for (int i = 0; i < n; i++)
    st[i].distKm = nwsHaversineKm(lat, lon, st[i].lat, st[i].lon);
  // Insertion sort, nearest first (n <= 15 in practice).
  for (int i = 1; i < n; i++) {
    NwsStation key = st[i];
    int j = i - 1;
    while (j >= 0 && st[j].distKm > key.distKm) { st[j + 1] = st[j]; j--; }
    st[j + 1] = key;
  }
}

int nwsRankStations(const NwsStation* cands, int nCands, bool ntpOK,
                    const char* excludeId, NwsStation* out, int maxOut) {
  if (!cands || !out || maxOut < 1 || nCands < 1) return 0;
  const int NMAX = 32;   // callers pass <= STN_MAX (15); clamp defensively
  if (nCands > NMAX) nCands = NMAX;
  int rem[NMAX];         // indices into cands, nearest first
  int nRem = 0;
  for (int i = 0; i < nCands; i++) {
    if (!nwsStationSuitable(&cands[i])) continue;              // CoCoRaHS
    if (excludeId && excludeId[0] &&
        strcmp(cands[i].id, excludeId) == 0) continue;
    rem[nRem++] = i;
  }
  int n = 0;
  while (n < maxOut && nRem > 0) {
    int best = -1;
    if (ntpOK) {
      // Anchor = nearest remaining candidate with a usable observation.
      int anchor = -1;
      for (int k = 0; k < nRem; k++)
        if (nwsObsUsable(&cands[rem[k]])) { anchor = k; break; }
      if (anchor >= 0) {
        best = anchor;
        const float win = cands[rem[anchor]].distKm + NWS_RANK_COMPETE_KM;
        long fresh = cands[rem[anchor]].obsAgeSec;
        for (int k = 0; k < nRem; k++) {        // freshest inside the window
          if (!nwsObsUsable(&cands[rem[k]]) || cands[rem[k]].distKm > win)
            continue;
          if (cands[rem[k]].obsAgeSec < fresh) fresh = cands[rem[k]].obsAgeSec;
        }
        if (cands[rem[anchor]].obsAgeSec - fresh >= NWS_RANK_MIN_NEWER_S) {
          for (int k = 0; k < nRem; k++) {      // pick it (nearest on a tie)
            if (!nwsObsUsable(&cands[rem[k]]) || cands[rem[k]].distKm > win)
              continue;
            if (cands[rem[k]].obsAgeSec == fresh) { best = k; break; }
          }
        }
      }
    }
    if (best < 0) best = 0;   // no usable (or no NTP): nearest remaining
    out[n++] = cands[rem[best]];
    rem[best] = rem[nRem - 1];
    nRem--;
  }
  return n;
}
