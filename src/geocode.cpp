#include "geocode.h"

#include <Arduino.h>
#include <ctype.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include "wifi_conn.h"   // wifiEnsureConnected()

// ---------------------------------------------------------------------------
//  Geocoding for the LOCATION settings page (see geocode.h for the
//  contract).
//
//  Forward (postal code or city name -> place): Open-Meteo Geocoding API
//  first — US ZIPs and many international codes resolve directly, and plain
//  city names match its name index. When Open-Meteo answers definitively
//  with zero results (typical for international alphanumeric codes and
//  "CITY, ST" phrases its index doesn't cover), Nominatim (OpenStreetMap)
//  gets one fallback request. A network failure on Open-Meteo is reported
//  as a failure without hammering a second service.
//
//  Reverse (coordinates -> place): Open-Meteo has no reverse-geocoding
//  endpoint, so this uses BigDataCloud's free keyless reverse-geocode
//  client API (designed for keyless client-side use, no account).
//
//  Each request owns a scoped WiFiClientSecure (socket + mbedTLS released
//  when it returns) with bounded timeouts — the same pattern and tradeoffs
//  as the weather clients (HTTPS with setInsecure(), no CA verification).
// ---------------------------------------------------------------------------

static const char* GEO_HOST_OPENMETEO  = "geocoding-api.open-meteo.com";
static const char* GEO_HOST_NOMINATIM  = "nominatim.openstreetmap.org";

static const uint16_t      GEO_HTTP_TIMEOUT_MS = 5000;   // TCP connect / socket read
static const unsigned long GEO_HTTP_HANDSHAKE_S = 10;    // TLS handshake

// Nominatim usage policy requires a User-Agent that identifies the app.
static const char* GEO_UA_NOMINATIM =
    "CYD-Weather/1.0 (personal ESP32 weather dashboard; "
    "https://github.com/MichaelFiguresItOut/CYD-weather)";

// ---- Small helpers --------------------------------------------------------

// Percent-encode a URL query component (postal codes may carry spaces).
static void urlEncode(const char* in, String& out) {
  static const char* hex = "0123456789ABCDEF";
  for (const char* p = in; *p; p++) {
    unsigned char c = (unsigned char)*p;
    if (isalnum(c) || c == '-' || c == '_' || c == '.') out += (char)c;
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
}

// Append text to a fixed buffer with an explicit offset; overflow-safe.
static size_t appStr(char* buf, size_t size, size_t off, const char* s) {
  if (!s || off >= size) return off;
  int w = snprintf(buf + off, size - off, "%s", s);
  return (w > 0) ? off + (size_t)w : off;
}

// Fill the shared display fields of a result from its parts. `place` =
// "name, region, country" (empty parts skipped); `meta` = "region, CC".
static void fillPlace(GeoResult* g, const char* name, const char* region,
                      const char* country, const char* countryCode) {
  snprintf(g->name, sizeof(g->name), "%.39s", name);
  size_t off = 0;
  off = appStr(g->place, sizeof(g->place), off, name);
  if (region && region[0]) {
    off = appStr(g->place, sizeof(g->place), off, ", ");
    off = appStr(g->place, sizeof(g->place), off, region);
  }
  // Country code first: "City, Region, CC" stays short enough to share
  // the display row with the postal code on the 320 px screen.
  const char* c = (countryCode && countryCode[0]) ? countryCode : country;
  if (c) {
    off = appStr(g->place, sizeof(g->place), off, ", ");
    off = appStr(g->place, sizeof(g->place), off, c);
  }
  off = 0;
  bool first = true;
  if (region && region[0]) { off = appStr(g->meta, sizeof(g->meta), off, region); first = false; }
  if (countryCode && countryCode[0]) {
    if (!first) off = appStr(g->meta, sizeof(g->meta), off, ", ");
    off = appStr(g->meta, sizeof(g->meta), off, countryCode);
  }
  g->postcode[0] = 0;
}

// One HTTPS GET. The client is scoped so the socket and mbedTLS context
// are released when this returns. Returns true on HTTP 200 (body in `out`).
static bool httpsGet(const char* host, const String& path, String& out,
                     const char* userAgent = nullptr) {
  WiFiClientSecure client;
  client.setInsecure();   // same self-contained tradeoff as the weather clients
  client.setConnectionTimeout(GEO_HTTP_TIMEOUT_MS);
  client.setHandshakeTimeout(GEO_HTTP_HANDSHAKE_S);

  HTTPClient http;
  String url = String("https://") + host + path;
  if (!http.begin(client, url)) { http.end(); return false; }
  http.addHeader("User-Agent", userAgent ? userAgent : "CYD-WeatherStation/1.0");
  int code = http.GET();
  if (code == HTTP_CODE_OK) out = http.getString();
  http.end();
  return code == HTTP_CODE_OK;
}

// ---- Forward: Open-Meteo Geocoding API ------------------------------------
// count bounds the payload; a big city's "postcodes" array can still be a
// few KB, so the document is capacity-capped to keep heap use bounded.
static int searchOpenMeteo(const char* query, GeoResult* res, int maxResults) {
  String enc, path;
  urlEncode(query, enc);
  path = String("/v1/search?name=") + enc + "&count=" + maxResults
       + "&format=json&language=en";

  String body;
  if (!httpsGet(GEO_HOST_OPENMETEO, path, body)) return -1;

  JsonDocument doc;   // count= bounds the results; a city's postcode array
                      // can still be a few KB — fine for a one-shot lookup
  if (ArduinoJson::deserializeJson(doc, body)) return -1;

  JsonVariant rv = doc["results"];
  if (!rv.is<JsonArray>()) return 0;

  int n = 0;
  for (JsonVariant v : rv.as<JsonArray>()) {
    if (n >= maxResults) break;
    const char* name = v["name"].as<const char*>();
    if (!name || !*name) continue;
    float lat = v["latitude"].as<float>();
    float lon = v["longitude"].as<float>();
    if (isnan(lat) || isnan(lon)) continue;
    GeoResult* g = &res[n++];
    g->lat = lat;
    g->lon = lon;
    fillPlace(g, name,
              v["admin1"].as<const char*>(),
              v["country"].as<const char*>(),
              v["country_code"].as<const char*>());
  }
  Serial.printf("[GEO] open-meteo \"%s\": %d results\n", query, n);
  return n;
}

// ---- Forward: Nominatim (OpenStreetMap) fallback ---------------------------
// jsonv2: the response root is the result array. The policy is respected:
// a descriptive User-Agent and at most one request per user action (well
// under the 1 req/s limit).
static int searchNominatim(const char* query, GeoResult* res, int maxResults) {
  String enc, path;
  urlEncode(query, enc);
  path = String("/search?q=") + enc + "&format=jsonv2&limit=" + maxResults
       + "&addressdetails=1";

  String body;
  if (!httpsGet(GEO_HOST_NOMINATIM, path, body, GEO_UA_NOMINATIM)) return -1;

  JsonDocument doc;
  if (ArduinoJson::deserializeJson(doc, body)) return -1;
  if (!doc.is<JsonArray>()) return 0;

  int n = 0;
  for (JsonVariant v : doc.as<JsonArray>()) {
    if (n >= maxResults) break;
    float lat = v["lat"].as<float>();   // "lat"/"lon" arrive as strings
    float lon = v["lon"].as<float>();
    if (isnan(lat) || isnan(lon)) continue;
    JsonVariant ad = v["address"];
    const char* city = ad["city"].as<const char*>();
    if (!city || !*city) city = ad["town"].as<const char*>();
    if (!city || !*city) city = ad["village"].as<const char*>();
    if (!city || !*city) city = ad["hamlet"].as<const char*>();
    const char* region = ad["state"].as<const char*>();
    if (!region || !*region) region = ad["county"].as<const char*>();
    const char* country = ad["country"].as<const char*>();
    const char* pc = ad["postcode"].as<const char*>();
    if (!city || !*city) {
      // No named settlement: use the first display_name part.
      const char* dn = v["display_name"].as<const char*>();
      if (!dn || !*dn) continue;
      char part[40];
      snprintf(part, sizeof(part), "%.39s", dn);
      char* comma = strchr(part, ',');
      if (comma) *comma = 0;
      city = part;
    }
    GeoResult* g = &res[n++];
    g->lat = lat;
    g->lon = lon;
    fillPlace(g, city, region, country, nullptr);
    if (pc && *pc) snprintf(g->postcode, sizeof(g->postcode), "%.11s", pc);
  }
  Serial.printf("[GEO] nominatim \"%s\": %d results\n", query, n);
  return n;
}

// ---- Forward: public entry points -----------------------------------------
// One request per call; the settings flow runs Open-Meteo first and, only on
// a definite 0-match, lets Nominatim have one look (a network failure, -1,
// is not retried against a second service).
int geocodePostalOpenMeteo(const char* query, GeoResult* results, int maxResults) {
  if (!query || !*query || !results || maxResults < 1) return 0;
  if (!wifiEnsureConnected()) return -1;
  return searchOpenMeteo(query, results, maxResults);
}

int geocodePostalNominatim(const char* query, GeoResult* results, int maxResults) {
  if (!query || !*query || !results || maxResults < 1) return 0;
  if (!wifiEnsureConnected()) return -1;
  return searchNominatim(query, results, maxResults);
}
