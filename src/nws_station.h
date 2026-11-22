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
// "AGE UNKNOWN", "JUST NOW", "5M AGO", "1H 05M AGO", "1D 3H AGO".
void nwsAgeText(long ageSec, char* out, size_t outLen);

// ---------------------------------------------------------------------------
//  Phase 4: automatic backup stations + observation availability
//
//  When the user picks a preferred station (Settings -> STATIONS), the
//  firmware automatically records up to NWS_BACKUP_MAX backup stations
//  from the same NWS station list (nearest first, no extra requests). The
//  ordered fallback list is [preferred, backup 1..3]; it is persisted in
//  NVS (config.cpp) and rebuilt on every station selection. The user only
//  ever sees/configures the preferred station.
//
//  A station's latest observation is USABLE when it was fetched
//  successfully and is no older than NWS_OBS_FRESH_MAX_S. nwsFindUsable()
//  walks the fallback-ordered list and returns the first usable station,
//  or -1 = no usable NWS station (the caller then uses Open-Meteo as the
//  source, e.g. outside NWS coverage or during an NWS outage).
// ---------------------------------------------------------------------------

// Number of automatic backup stations kept behind the preferred one.
#define NWS_BACKUP_MAX 3
// Maximum distance (km from the queried point) for a station to qualify
// as a backup: NWS orders the list nearest first, so anything beyond this
// is geographically unrelated rather than a fallback for the same area
// (sparse rural areas simply end up with fewer backups).
#define NWS_BACKUP_MAX_KM 100.0f

// Usable as a current-weather source: not a CoCoRaHS (volunteer
// precipitation) station - those only report during rain/snow, so their
// "latest" observation can be days old. All other provider types
// (ASOS, ASOS-HFM, AWOS variants, MesoWest, ...) are automated.
bool nwsStationSuitable(const NwsStation* st);

// Fill `out` (up to maxBackups entries) with backup stations picked from
// `candidates` (NWS order = nearest first): suitable, within
// NWS_BACKUP_MAX_KM, not the preferred station, no duplicate IDs. The
// result keeps the nearest-first order. Returns the number of backups
// stored (0 when none qualify).
int nwsBuildFallbackList(const NwsStation* candidates, int nCandidates,
                         const char* preferredId, NwsStation* out,
                         int maxBackups);

// Latest observation usable: fetched successfully, age known (NTP-synced),
// and no older than NWS_OBS_FRESH_MAX_S. A failed fetch (obsFail) or an
// unknown age is never usable.
bool nwsObsUsable(const NwsStation* st);

// First usable station in a fallback-ordered list (preferred first), or
// -1 when none is usable (Open-Meteo is then the source).
int nwsFindUsable(const NwsStation* list, int n);
