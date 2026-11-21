#pragma once

// ---------------------------------------------------------------------------
//  Phase 3: NWS weather-station discovery (README.md "Phase 3")
//
//  Three single-request stages, driven sequentially by the Settings UI:
//
//    nwsPointsLookup()    GET /points/{lat},{lon}
//                         -> NWS-provided observation-stations URL
//    nwsFetchStations()   GET <stations URL from the points response>
//                         -> nearby stations, NWS order (nearest first)
//    nwsFetchLatestObs()  GET /stations/{id}/observations/latest
//                         -> per-station freshness + latest temperature
//
//  The station features carry no link to their latest observation, so that
//  one request is built from the ICAO identifier against the endpoint NWS
//  documents for exactly this; every other request follows an NWS-provided
//  link.
//
//  All requests follow the geocode.cpp pattern: a scoped WiFiClientSecure
//  (socket + mbedTLS context freed on return), bounded timeouts,
//  setInsecure(), and an identifying User-Agent. All failures return
//  negative codes; nothing here crashes or throws, and a failed stage
//  leaves previously fetched data intact.
// ---------------------------------------------------------------------------

#include <stdint.h>
#include <stddef.h>   // size_t

// One observation station found near the configured location.
struct NwsStation {
  char  id[8];         // ICAO identifier, e.g. "KNYC"
  char  name[40];      // NWS display name, e.g. "New York City, Central Park"
  float lat, lon;      // station coordinates (geometry.coordinates)
  float distKm;        // distance from the queried point (NWS-provided)
  char  provider[16];  // e.g. "ASOS", "ASOS-HFM"
  // Latest observation - only valid after nwsFetchLatestObs() succeeded:
  bool  hasObs;        // true when obsAgeSec / obsTempC / obsText are valid
  bool  obsFail;       // true when the per-station fetch failed (network/parse)
  long  obsAgeSec;     // age in seconds at fetch time; -1 = unknown (no NTP)
  float obsTempC;      // latest temperature in °C; NaN when unreported
  char  obsText[24];   // e.g. "Clear"; "" when unreported
};

// "Fresh enough to be useful" - same 90-minute bar as the METAR source
// (weather.cpp). Automated stations report hourly, so this tolerates one
// late report while still flagging anything from a previous night.
#define NWS_OBS_FRESH_MAX_S  (90L * 60)

// GET /points/{lat},{lon}. On success writes the NWS-provided
// observation-stations URL to `stationsUrl` (must hold >= 64 chars).
// Returns 0 on success, or:
//   -1  network/HTTP failure (incl. no WiFi),
//   -2  point not covered by NWS (404 InvalidPoint - outside US coverage),
//   -3  unexpected response (no observation-stations link).
int nwsPointsLookup(float lat, float lon, char* stationsUrl, size_t urlLen);

// GET <stationsUrl> (the NWS-provided link from the points lookup).
// Fills `out` in NWS order (nearest first). Returns the number of stations
// stored (0 = empty list), or -1 network / -3 unparseable response.
// The download is capped and only whole features are parsed, so dense
// metro areas (40+ stations) cannot exhaust the heap.
int nwsFetchStations(const char* stationsUrl, NwsStation* out, int maxStations);

// GET /stations/{st->id}/observations/latest. On success fills
// hasObs / obsAgeSec / obsTempC / obsText (obsAgeSec = -1 without NTP).
// Returns 0 on success, -1 network/HTTP failure (obsFail set), -3 no
// observation or unparseable response (obsFail set).
int nwsFetchLatestObs(NwsStation* st);

// Human-readable age for display, all-caps to match the settings UI:
// "AGE UNKNOWN", "JUST NOW", "5M", "1H 05M", "1D 3H".
void nwsAgeText(long ageSec, char* out, size_t outLen);
