#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SD.h>
#include <puff.h>
#include "weather.h"
#include "metar.h"
#include "nws_station.h"
#include "rain_timeline.h"
#include "time_util.h"
#include "wifi_conn.h"
#include "dog_image.h"
#include "config.h"
#include "led_status.h"

// ---------------------------------------------------------------------------
//  SD card slot (from the CYD MCU schematic ESP32-2432S032-MCU-V1.0):
//    TF_CS = GPIO5   TF_CLK = GPIO18   MCU_MISO = GPIO19   MCU_MOSI = GPIO23
//  The TFT owns VSPI (SPI3); the SD card gets a dedicated HSPI (SPI2) instance
//  so the two buses never share hardware.
// ---------------------------------------------------------------------------
#define SD_CS         5
#define SD_SCK        18
#define SD_MISO       19
#define SD_MOSI       23

// Site location (lat/lon) and the fetch cadence are configuration: see
// userSettings in config.cpp (defaults from local_config.h) and config.h.

// ---------------------------------------------------------------------------
//  Live observation sources (METAR via NOAA Aviation Weather Center)
//  KJRB (Wall Street / Manhattan) is primary; KLGA and KEWR are nearby airports
//  used for "RAIN NEARBY" when KJRB itself reports dry.
// ---------------------------------------------------------------------------
static const char*    METAR_URL         = "https://aviationweather.gov/api/data/metar?ids=KJRB,KLGA,KEWR&format=json";
static const uint32_t METAR_MAX_AGE_S   = 90UL * 60;                // obs older than this -> fall back to model data
// The station set above is fixed to the NYC metro. It is only a valid
// "current conditions / rain nearby" source when the user is actually near
// those airports; from anywhere else it would describe a different city.
static const double   METAR_VALID_RADIUS_KM = 50.0;

// True when the user's coordinates are within METAR_VALID_RADIUS_KM of KJRB
// (Teterboro NJ, 40.8503 N / 74.0607 W). KLGA and KEWR are both < 20 km from
// KJRB, so one anchor covers all three stations.
static bool userNearMetarStations(double lat, double lon) {
  const double d2r  = 3.14159265358979 / 180.0;
  const double dLat = (40.8503 - lat) * d2r;
  const double dLon = (-74.0607 - lon) * d2r;
  double a = sin(dLat / 2.0) * sin(dLat / 2.0) +
             cos(lat * d2r) * cos(40.8503 * d2r) * sin(dLon / 2.0) * sin(dLon / 2.0);
  return 6371.0 * 2.0 * atan2(sqrt(a), sqrt(1.0 - a)) <= METAR_VALID_RADIUS_KM;
}

// HTTPS protection: bound every phase of a fetch so a stalled server can never
// freeze the loop. (Core defaults are connect 30 s and TLS handshake 120 s!)
static const uint16_t        HTTP_TIMEOUT_MS   = 5000;              // TCP connect + socket read (SO_RCVTIMEO)
static const unsigned long   HTTP_HANDSHAKE_S  = 10;                // TLS handshake

// NBM cross-check (independent NOAA blend, hourly trailing sums in inches):
// if RRFS declares the current rain period ends at E but NBM still shows >=
// this amount in any of the next RAIN_NBM_CHECK_HOURS hours after E, the
// definitive end time is suppressed and "RAIN NOW - MAY CONTINUE" is shown.
static const float RAIN_NBM_HOURLY_THRESH_IN = 0.004f;   // per hour (~0.1 mm/hr)
static const int   RAIN_NBM_CHECK_HOURS      = 6;        // hours past the RRFS end to check

WeatherData wx;

// ---------------------------------------------------------------------------
//  NBM cross-check (second precipitation source, hourly only)
//
//  Independent NOAA blend product fetched from a SEPARATE request so the main
//  best-match RRFS response stays exactly what it is today. NBM's hourly value
//  stamped T is a trailing sum over (T-60 min, T], i.e. stamp E+1 h covers
//  exactly the hour after an RRFS end time E - directly comparable to RRFS.
//
//  If any of the next RAIN_NBM_CHECK_HOURS hourly values past the RRFS end is
//  >= RAIN_NBM_HOURLY_THRESH_IN, rain "may continue" and the definitive end
//  time is suppressed on display ("RAIN NOW - MAY CONTINUE"). No new end time
//  is ever derived from NBM.
//
//  Every failure path (no WiFi, HTTP error, timeout, bad/missing JSON) returns
//  false -> existing RRFS-only behavior is retained and the normal update is
//  unaffected. The JsonDocument is local to this function, so its heap memory
//  (~3-5 KB peak on a ~1.5 KB body) is released when it returns.
// ---------------------------------------------------------------------------

// t += h hours with day/month/year rollover (h < 24). Mirrors the month-length
// handling in stepMinus15min() without touching that function.
static void stepAddHours(StepTime& t, int h) {
  t.hh += h;
  while (t.hh >= 24) {
    t.hh -= 24;
    static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (++t.d > mdays[t.mo - 1]) {
      t.d = 1;
      if (++t.mo > 12) { t.mo = 1; t.y++; }
    }
  }
}

// Pre-resolve a host so that a subsequent HTTP error -1 can be attributed to
// TCP/TLS rather than DNS (DNS failures are reported separately by the caller).
static bool dnsResolve(const char* host) {
  IPAddress ip;
  return WiFi.hostByName(host, ip);
}

static bool nbmIndicatesContinuation(const StepTime& end) {
  WiFiClientSecure nClient;
  nClient.setInsecure();   // same self-contained tradeoff as the other clients
  nClient.setConnectionTimeout(HTTP_TIMEOUT_MS);
  nClient.setHandshakeTimeout(HTTP_HANDSHAKE_S);

  HTTPClient nHttp;
  String url = String("https://api.open-meteo.com/v1/forecast?latitude=")
             + String(userSettings.lat, 4) + "&longitude=" + String(userSettings.lon, 4)
             + "&models=ncep_nbm_conus&hourly=precipitation"   // NBM hourly trailing sums (inches)
             + "&timezone=auto&forecast_days=2"
             + "&temperature_unit=fahrenheit&precipitation_unit=inch";
  if (!nHttp.begin(nClient, url)) {
    Serial.println("[NBM] http.begin failed");
    return false;
  }
  nHttp.addHeader("User-Agent", "CYD-WeatherStation/1.0");

  Serial.printf("[NBM] freeHeap=%lu\n", ESP.getFreeHeap());   // diagnostic: heap at request time
  if (!dnsResolve("api.open-meteo.com")) {
    Serial.println("[NBM] DNS failed");
    return false;
  }

  int code = nHttp.GET();
  String body = (code == HTTP_CODE_OK) ? nHttp.getString() : "";
  nHttp.end();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[NBM] HTTP error %d\n", code);
    return false;
  }

  JsonDocument nbmDoc;   // ~1.5 KB response; freed when this function returns
  ArduinoJson::DeserializationError err = ArduinoJson::deserializeJson(nbmDoc, body);
  if (err) {
    Serial.printf("[NBM] JSON parse error: %s\n", err.c_str());
    return false;
  }

  JsonVariant vT = nbmDoc["hourly"]["time"];          // flat root-level arrays (single model)
  JsonVariant vP = nbmDoc["hourly"]["precipitation"];
  if (!vT.is<JsonArray>() || !vP.is<JsonArray>()) {
    Serial.println("[NBM] missing hourly time/precipitation arrays");
    return false;
  }
  size_t n = vT.as<JsonArray>().size();
  if (n > vP.as<JsonArray>().size()) n = vP.as<JsonArray>().size();

  // Window: stamps strictly after the RRFS end, up to and including end + RAIN_NBM_CHECK_HOURS.
  StepTime winEnd = end;
  stepAddHours(winEnd, RAIN_NBM_CHECK_HOURS);

  bool mayContinue = false;
  for (size_t i = 0; i < n && !mayContinue; i++) {
    const char* ts = vT.as<JsonArray>()[i].as<const char*>();
    if (!ts) continue;
    StepTime st;
    if (!parseStepTime(ts, st)) continue;
    if (!stepBefore(end, st)) continue;        // must be strictly after the RRFS end time
    if (stepBefore(winEnd, st)) break;         // hourly stamps are ascending -> past window
    JsonVariant pv = vP.as<JsonArray>()[i];
    if (pv.isNull()) continue;
    float p = pv.as<float>();
    mayContinue = !isnan(p) && p >= RAIN_NBM_HOURLY_THRESH_IN;
  }

  Serial.printf("[NBM] %d h after RRFS end: %s\n", RAIN_NBM_CHECK_HOURS,
                mayContinue ? "MAY CONTINUE" : "dry");
  return mayContinue;
}

// ---------------------------------------------------------------------------
//  Open-Meteo fetch (HTTPS)
// ---------------------------------------------------------------------------
static bool fetchWeatherRun() {   // renamed: wrapped by fetchWeather() below
  if (!wifiEnsureConnected()) return false;

  Serial.printf("[WX] rssi=%d dBm freeHeap=%lu\n", WiFi.RSSI(), ESP.getFreeHeap());   // diagnostic

  // ---- Fetch JSON body over HTTPS ----
  // The TLS client is scoped to this block so its socket and mbedTLS context are
  // released before the NBM/METAR requests start (each of which opens its own).
  String body;
  {
    WiFiClientSecure secureClient;
    // NOTE: server certificate verification is disabled to keep the firmware
    // self-contained (no embedded CA bundle). The traffic is still encrypted.
    // For full verification, embed the ISRG Root X1 cert and call setCACert().
    secureClient.setInsecure();
    secureClient.setConnectionTimeout(HTTP_TIMEOUT_MS);   // TCP connect + socket read <= 5 s
    secureClient.setHandshakeTimeout(HTTP_HANDSHAKE_S);   // TLS handshake <= 10 s (core default: 120 s!)

    HTTPClient http;
    String url = String("https://api.open-meteo.com/v1/forecast?latitude=")
               + String(userSettings.lat, 4) + "&longitude=" + String(userSettings.lon, 4)
               + "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,is_day,rain"
               + "&daily=temperature_2m_max,temperature_2m_min"
               + "&hourly=rain"                    // rain beyond the 15-min window (local wall-clock hours)
               + "&minutely_15=precipitation"      // NOAA RRFS 15-min precip sums, local time (~48 h)
               + "&timezone=auto&forecast_days=2"
               + "&temperature_unit=fahrenheit&precipitation_unit=inch";   // temps °F, rain in inches

    Serial.printf("[WX] GET %s\n", url.c_str());
    if (!dnsResolve("api.open-meteo.com")) {
      Serial.println("[WX] DNS failed");
      return false;
    }
    if (!http.begin(secureClient, url)) {
      Serial.println("[WX] http.begin failed");
      return false;
    }
    http.addHeader("User-Agent", "CYD-WeatherStation/1.0");

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
      Serial.printf("[WX] HTTP error %d\n", code);
      http.end();
      return false;
    }

    body = http.getString();
    http.end();
  }   // secureClient destroyed here: socket + TLS context freed before NBM/METAR

  // ---- Parse JSON ----
  JsonDocument doc;   // ArduinoJson v7
  ArduinoJson::DeserializationError err = ArduinoJson::deserializeJson(doc, body);
  if (err) {
    Serial.printf("[WX] JSON parse error: %s\n", err.c_str());
    return false;
  }

  float curTemp   = doc["current"]["temperature_2m"];
  float curFeels  = doc["current"]["apparent_temperature"];
  int   curHum    = doc["current"]["relative_humidity_2m"];
  int   wCode     = doc["current"]["weather_code"];
  float curRain   = doc["current"]["rain"];      // inches this hour (null if unavailable)
  // is_day arrives as a JSON NUMBER (0/1). `variant | true` would reject it:
  // ArduinoJson's bool check only accepts JSON boolean literals, so test the value.
  JsonVariant vIsDay = doc["current"]["is_day"];
  bool  isDayOk   = !vIsDay.isNull() && (vIsDay == 0 || vIsDay == 1);
  float dMax      = doc["daily"]["temperature_2m_max"][0];
  float dMin      = doc["daily"]["temperature_2m_min"][0];

  if (isnan(curTemp)) {
    Serial.println("[WX] Missing current temperature in response");
    return false;
  }

  // ---- Phase 2A: automatic timezone from the same request ----
  // timezone=auto makes Open-Meteo report the IANA zone for the coordinates
  // (top-level doc["timezone"]); the timezone is automatic only, so this
  // always applies. A failed fetch or an unsupported id keeps the last valid
  // rules (applyAutoTimezoneFromIana).
  const char* iana = doc["timezone"].as<const char*>();
  if (iana) applyAutoTimezoneFromIana(iana);

  wx.valid        = true;
  wx.temp         = curTemp;
  wx.feelsLike    = curFeels;
  wx.humidity     = curHum;
  wx.isDay        = isDayOk ? ((int)vIsDay != 0) : true;   // unknown -> assume day
  wx.isDayKnown   = isDayOk;
  wx.tMax         = dMax;
  wx.tMin         = dMin;
  wx.description  = wmoText(wCode);
  wx.wmoCode      = wCode;
  wx.lastUpdateMs = millis();
  // Data timestamp: Open-Meteo's `current.time` is the model step's local
  // wall clock ("YYYY-MM-DDTHH:MM", the response's timezone); subtract the
  // response's utc_offset_seconds to get the UTC epoch of the model data
  // shown. Falls back to the fetch time when either field is missing. An
  // NWS station observation time replaces this later in this same fetch.
  {
    const char* ct = doc["current"]["time"].as<const char*>();
    JsonVariant off = doc["utc_offset_seconds"];
    int y, mo, d, h, mi;
    if (ntpIsSynced() && ct && !off.isNull() &&
        sscanf(ct, "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) == 5) {
      wx.omModelEpoch =
          (time_t)(makeUtcTime(y, mo, d, h, mi, 0) - off.as<long>());
      wx.lastUpdateEpoch = wx.omModelEpoch;
    } else {
      wx.omModelEpoch = 0;                  // model step time unavailable -> age shows "--"
      wx.lastUpdateEpoch = time(nullptr);   // 0/garbage until NTP has synced
    }
  }

  // ---- Rain status: RRFS 15-min periods + hourly extension (needs NTP for "now") ----
  bool rainingNow = (!isnan(curRain) && curRain > 0.0f) || wmoIsRain(wCode);   // model fallback

  char nextRain[16] = {0};
  bool nextTomorrow = false;
  bool nextFrom15min = false;
  char rainEnd[16] = {0};
  bool rainMayContinue = false;   // NBM cross-check result (false on any NBM failure)
  if (ntpIsSynced()) {
    struct tm lt;
    nowLocal(&lt);
    RainTimeline tl;
    analyzeRainTimeline(doc, lt, tl);
    snprintf(nextRain, sizeof(nextRain), "%s", tl.nextRain);
    snprintf(rainEnd, sizeof(rainEnd), "%s", tl.rainEnd);
    nextTomorrow  = tl.nextTomorrow;
    nextFrom15min = tl.nextFrom15min;

    // doc/body are no longer needed: free them before the NBM TLS handshake.
    doc.clear();
    body = "";

    // NBM cross-check: only when RRFS declared a concrete end time for the
    // current period. A failed/missing NBM fetch leaves rainMayContinue false,
    // i.e. the existing RRFS-only behavior is retained (see nbmIndicatesContinuation).
    if (tl.hasEndTime) {
      rainMayContinue = nbmIndicatesContinuation(tl.endTime);
    }
  } else {
    Serial.println("[WX] rain timeline skipped (no NTP)");
    doc.clear();   // no longer needed; free before the METAR TLS handshake
    body = "";
  }

  // ---- METAR observations: KJRB (primary) + KLGA/KEWR for "RAIN NEARBY" ----
  // Fetched only when the user is near the (NYC metro) stations; elsewhere
  // the Open-Meteo model (user's own coordinates) drives every
  // current-condition field, so the live dog/text rain state can never
  // diverge from the local forecast.
  bool metarApplies = userNearMetarStations(userSettings.lat, userSettings.lon);
  bool metarOk = false;      // fresh KJRB obs parsed
  int  kjrbCover = -1;       // sky cover from a fresh KJRB obs (-1 = none)
  String metarDesc;
  bool kjrbRain = false, lgaRain = false, ewrRain = false;   // precip in fresh obs
  if (metarApplies) {
    WiFiClientSecure mClient;
    mClient.setInsecure();   // same self-contained tradeoff as the Open-Meteo client
    mClient.setConnectionTimeout(HTTP_TIMEOUT_MS);
    mClient.setHandshakeTimeout(HTTP_HANDSHAKE_S);
    HTTPClient mHttp;
    Serial.printf("[METAR] freeHeap=%lu\n", ESP.getFreeHeap());   // diagnostic: heap at request time
    if (!dnsResolve("aviationweather.gov")) {
      Serial.println("[METAR] DNS failed");
    } else if (mHttp.begin(mClient, METAR_URL)) {
      mHttp.addHeader("User-Agent", "CYD-WeatherStation/1.0");
      int mCode = mHttp.GET();
      String mBody = (mCode == HTTP_CODE_OK) ? mHttp.getString() : "";
      mHttp.end();
      if (mCode != HTTP_CODE_OK) {
        Serial.printf("[METAR] HTTP error %d\n", mCode);
      } else {
        JsonDocument mDoc;   // small response (~3 KB); top level is a JSON ARRAY
        ArduinoJson::DeserializationError mErr = ArduinoJson::deserializeJson(mDoc, mBody);
        if (mErr) {
          Serial.printf("[METAR] JSON parse error: %s\n", mErr.c_str());
        } else if (!mDoc.is<JsonArray>() || mDoc.as<JsonArray>().size() == 0) {
          Serial.println("[METAR] No observations in response");
        } else {
          int64_t nowEpoch = ntpIsSynced() ? (int64_t)time(nullptr) : 0;
          const JsonArray arr = mDoc.as<JsonArray>();
          for (size_t i = 0; i < arr.size(); i++) {
            JsonObject obs = arr[i];
            const char* id     = obs["icaoId"];              // "KJRB" / "KLGA" / "KEWR"
            const char* raw    = obs["rawOb"];               // full METAR/SPECI string
            int64_t   obsEpoch = (int64_t)obs["obsTime"];    // UTC epoch seconds
            if (!id || !raw || !*raw) continue;
            long age = (nowEpoch > 0 && obsEpoch > 1700000000LL) ? (long)(nowEpoch - obsEpoch) : -1;
            Serial.printf("[METAR] %s (%ld s old): %s\n", id, age, raw);
            bool fresh = age >= 0 && age <= (long)METAR_MAX_AGE_S;
            if (!fresh) continue;   // stale: never drives RAIN NOW / NEARBY
            bool rain = metarHasRain(raw);
            if (strcmp(id, "KJRB") == 0) {
              metarOk   = true;
              metarDesc = metarText(raw);
              kjrbRain  = rain;
              kjrbCover = metarSkyCover(raw);
            } else if (rain && strcmp(id, "KLGA") == 0) {
              lgaRain = true;
            } else if (rain && strcmp(id, "KEWR") == 0) {
              ewrRain = true;
            }
          }
        }
      }
    } else {
      Serial.println("[METAR] http.begin failed");
    }
  } else {
    Serial.println("[METAR] skipped: user not near configured stations");
  }

  if (metarOk) {
    wx.description = metarDesc;   // live observation replaces WMO model text
    rainingNow     = kjrbRain;    // never report RAIN NOW from a stale obs
  }

  // Sun determination for the dog outfit: a fresh KJRB sky group wins over the
  // WMO model code. Sunny = known daytime + clear (SKC/CLR) or mainly clear (FEW);
  // with no usable METAR sky group, fall back to WMO codes 0/1.
  if (isDayOk && wx.isDay) {
    int cover = kjrbCover;        // -1 when no fresh KJRB obs / no sky group
    wx.sunny = (cover >= 0) ? (cover <= 1) : ((wCode == 0 || wCode == 1));
  } else {
    wx.sunny = false;             // night or unknown day/night -> never "_sun"
  }

  // RAIN NEARBY: fresh precip at KLGA/KEWR while KJRB is not reporting rain now.
  bool rainNearby = !rainingNow && (lgaRain || ewrRain);

  wx.rainingNow   = rainingNow;
  wx.rainNowIn    = curRain;
  strncpy(wx.nextRain, nextRain, sizeof(wx.nextRain) - 1);
  wx.nextRain[sizeof(wx.nextRain) - 1] = '\0';
  wx.nextRainTomorrow = nextTomorrow;
  wx.nextRainFrom15min = nextFrom15min;
  strncpy(wx.rainEnd, rainEnd, sizeof(wx.rainEnd) - 1);
  wx.rainEnd[sizeof(wx.rainEnd) - 1] = '\0';
  wx.rainMayContinue = rainMayContinue;
  wx.rainNearby   = rainNearby;
  if (rainNearby) {
    snprintf(wx.nearbyStns, sizeof(wx.nearbyStns), "%s",
             (lgaRain && ewrRain) ? "LGA/EWR" : lgaRain ? "LGA" : "EWR");
  } else {
    wx.nearbyStns[0] = '\0';
  }

  // ---- Phase 5: NWS preferred/backup station as the temperature source ----
  // Tries [preferred, backup1..3] in that order; the first station whose
  // latest observation is usable (nwsObsUsable: fresh, NTP-synced age) and
  // carries a valid temperature supplies the dashboard temperature and the
  // NWS feels-like (wind chill / heat index computed from that same
  // observation's wind + humidity). When none qualifies, the Open-Meteo
  // values above stand (outside NWS coverage, NWS outage, all stations
  // stale, or no station configured). The dog outfit is selected from
  // wx.feelsLike, so it always follows whichever source won here.
  static NwsStation cand[1 + NWS_BACKUP_MAX];
  wx.nwsSourceId[0] = '\0';
  if (userSettings.nwsStationId[0] != '\0') {
    memset(cand, 0, sizeof(cand));
    snprintf(cand[0].id, sizeof(cand[0].id), "%s", userSettings.nwsStationId);
    int n = 1;
    for (int k = 0; k < NWS_BACKUP_MAX &&
                  n < (int)(sizeof(cand) / sizeof(cand[0])); k++) {
      if (!userSettings.nwsStationBackup[k][0]) break;   // "" = empty slot
      snprintf(cand[n].id, sizeof(cand[n].id), "%s", userSettings.nwsStationBackup[k]);
      n++;
    }
    int used = -1;
    for (int i = 0; i < n; i++) {
      if (nwsFetchLatestObs(&cand[i]) == 0 &&
          nwsObsUsable(&cand[i]) && nwsObsHasValidTemp(&cand[i])) {
        used = i;
        break;
      }
    }
    if (used >= 0) {
      const float tempF = cand[used].obsTempC * 9.0f / 5.0f + 32.0f;   // C->F, the existing convention
      wx.temp       = tempF;
      wx.feelsLike  = nwsFeelsLikeF(tempF, cand[used].obsWindMph, cand[used].obsRelHum);
      wx.lastUpdateEpoch = (time_t)cand[used].obsEpochSec;   // observation time, not fetch time
      snprintf(wx.nwsSourceId, sizeof(wx.nwsSourceId), "%s", cand[used].id);
      Serial.printf("[NWS] dashboard temp from %s (%s)\n", cand[used].id,
                    strcmp(cand[used].id, userSettings.nwsStationId) == 0 ? "preferred" : "backup");
    } else {
      Serial.println("[NWS] no usable station observation; keeping Open-Meteo temp");
    }
  }

  Serial.printf("[WX] %.1f F feels %.1f F hum %d%% %s (H:%.1f L:%.1f) day=%s sun=%s rain=%s next=%s%s end=%s metar=%s src=%s\n",
                wx.temp, wx.feelsLike, wx.humidity, wx.description.c_str(), wx.tMax, wx.tMin,
                isDayOk ? (wx.isDay ? "1" : "0") : "?", wx.sunny ? "1" : "0",
                rainingNow ? "NOW" : "-",
                wx.nextRain[0] ? wx.nextRain : "-", nextFrom15min ? "(15m)" : "",
                rainEnd[0] ? rainEnd : "-", metarOk ? "ok" : "fallback",
                wx.nwsSourceId[0] ? wx.nwsSourceId : "open-meteo");
  return true;
}

// NOTE: the actual dog blitting (drawDogBox / drawDogImage) lives in the theme
// system section below - it needs per-theme background colors for compositing.

// ---------------------------------------------------------------------------
//  Outfit selection (chart in README.md)
//
//  Rain overrides every other condition: raining now -> rain_now.png; otherwise
//  any expected rain (nextRain set) or RAIN NEARBY -> rain_later.png. Otherwise
//  the band follows the FEELS-LIKE temperature and "_sun" variants are used for
//  clear / mainly-clear daytime (wx.sunny). No usable weather data -> error.png
//  so a broken state is visible on screen instead of a stale outfit.
// ---------------------------------------------------------------------------
static const char* selectDogImage() {
  if (!wx.valid || isnan(wx.feelsLike)) return "error.png";

  if (wx.rainingNow) return "rain_now.png";
  // Same rain predicate as buildRainLine() (display.cpp): the dog and the
  // rain text must never disagree about whether rain is expected.
  if (wx.nextRain[0] != '\0' || (wx.rainNearby && wx.nearbyStns[0])) return "rain_later.png";

  float f = wx.feelsLike;   // °F
  const char* band;
  if (f < 30.0f)      band = "extreme_cold";   // below 30 F
  else if (f < 46.0f) band = "very_cold";      // 31-45 F
  else if (f < 56.0f) band = "cold";           // 46-55 F
  else if (f < 66.0f) band = "cool";           // 56-65 F
  else if (f < 71.0f) band = "mild_cool";      // 66-70 F
  else if (f < 76.0f) band = "mild_warm";      // 71-75 F
  else                band = "hot";            // 76 F and above

  static char name[24];
  snprintf(name, sizeof(name), "%s%s.png", band, wx.sunny ? "_sun" : "");
  return name;
}

// Load the outfit for the current weather state if it changed since the last
// load. Retries like at boot (marginal SD contact); on failure falls back to
// error.png so a bad state is visible instead of silently showing an old dog.
bool sdCardReady = false;   // set in setup() once SD.begin() succeeds

void refreshDogImage() {
  if (!sdCardReady) return;
  const char* want = selectDogImage();
  if (strcmp(want, dogCurrentName()) == 0) return;   // already showing it

  ledActivityBegin();   // SD read + PNG decode
  bool ok = false;
  for (int attempt = 1; attempt <= 3 && !ok; attempt++) {
    if (attempt > 1) delay(200);   // transient read error - try again
    ok = loadDogImage(want);
  }
  if (!ok && strcmp(want, "error.png") != 0) {
    Serial.printf("[DOG] %s failed to load - falling back to error.png\n", want);
    for (int attempt = 1; attempt <= 3 && !ok; attempt++) {
      if (attempt > 1) delay(200);
      ok = loadDogImage("error.png");
    }
  }
  ledActivityEnd();
}

// SD card bring-up + initial dog image (moved out of setup()).
void weatherInitSdAndDog() {
  ledActivityBegin();   // SD init + first dog image
  static SPIClass sdSpi(HSPI);   // SPI2 peripheral, separate from the TFT's SPI3
  sdSpi.begin(SD_SCK, SD_MISO, SD_MOSI);

  // The slot has marginal contact: a single init/read can fail on any boot
  // (most often noticed right after flashing, when you're watching the boot).
  // Retry instead of giving up for the whole session.
  bool cardOk = false;
  for (int attempt = 1; attempt <= 3 && !cardOk; attempt++) {
    if (attempt > 1) delay(500);
    cardOk = SD.begin(SD_CS, sdSpi, 16000000);
  }
  if (cardOk) {
    sdCardReady = true;
    Serial.println("[SD] card ready");
    // No weather data yet -> error.png until the first fetch picks a real outfit.
    refreshDogImage();
  } else {
    Serial.println("[SD] no card / init failed after 3 tries - continuing without dog image");
  }
  ledActivityEnd();
}

// Public entry point: one busy activity over the whole weather fetch (Wi-Fi
// reconnect if needed + Open-Meteo + NBM + METAR + station-chain handling),
// so a finishing sub-step never turns the status LED green mid-fetch.
bool fetchWeather() {
  ledActivityBegin();
  bool ok = fetchWeatherRun();
  ledActivityEnd();
  return ok;
}
