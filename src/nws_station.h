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
  bool  hasObs;        // true when the obs fields below are valid
  bool  obsFail;       // true when the per-station fetch failed (network/parse)
  long  obsAgeSec;     // age in seconds at fetch time; -1 = unknown (no NTP)
  float obsTempC;      // latest temperature in °C (unit-normalized); NaN when unreported
  float obsWindMph;    // latest wind speed in mph (unit-normalized); NaN when unreported
  float obsRelHum;     // latest relative humidity in %; NaN when unreported
  long  obsEpochSec;   // observation timestamp (UTC epoch seconds); 0 when unparseable
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
// hasObs / obsAgeSec / obsEpochSec / obsTempC / obsWindMph / obsRelHum /
// obsText (obsAgeSec = -1 without NTP; NWS reports temperature and wind
// speed with a unitCode - both are normalized to °C / mph).
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

// ---------------------------------------------------------------------------
//  Phase 5: NWS temperature + feels-like as the dashboard reading
//
//  The dashboard's current temperature and feels-like come from the first
//  station in the fallback-ordered list (preferred, then backups) whose
//  latest observation is both usable (nwsObsUsable: fresh, NTP-synced age)
//  and carries a valid temperature (nwsObsHasValidTemp). If none qualifies,
//  the Open-Meteo reading stays. Feels-like is NWS wind chill (T <= 50F,
//  wind >= 3 mph) or NWS heat index (T >= 80F, RH >= 40%), else the air
//  temperature itself - always computed from the same observation, never
//  mixed with Open-Meteo's apparent temperature. All three functions below
//  are pure (no network/NVS) and host-testable (test/nws/test_feels.py).
// ---------------------------------------------------------------------------

// True when the observation carries a real temperature value.
bool nwsObsHasValidTemp(const NwsStation* st);

// NWS wind-chill temperature (F) for air temp (F) and wind speed (mph).
// The published formula is valid for wind >= 3 mph; callers check that.
float nwsWindChillF(float tempF, float windMph);

// NWS heat index (F) (Rothfusz regression + the published low-humidity
// and high-humidity adjustments) for air temp (F) and relative humidity (%).
float nwsHeatIndexF(float tempF, float relHumPct);

// NWS "feels-like" (F) for one observation: wind chill when temp <= 50F
// and wind >= 3 mph, heat index when temp >= 80F and RH >= 40%, otherwise
// the air temperature itself. A NaN wind/RH input makes the corresponding
// term inapplicable (falls through to air temperature).
float nwsFeelsLikeF(float tempF, float windMph, float relHumPct);

// ---------------------------------------------------------------------------
//  Phase 6.5: quality-based preferred + backup selection
//
//  Replaces "first eligible station wins": the observation state of up to
//  NWS_RANK_CHECK_MAX nearby eligible stations (found nearest-first within
//  NWS_RANK_RADIUS_MAX_KM, i.e. a 10 km radius grown in 10 km steps) is
//  checked, and the preferred station plus up to NWS_BACKUP_MAX backups are
//  picked from them with nwsRankStations(), balancing distance and
//  observation freshness:
//
//    - The nearest candidate with a USABLE observation (nwsObsUsable:
//      fetched, NTP-synced age, <= NWS_OBS_FRESH_MAX_S) is the anchor.
//    - A usable candidate at most NWS_RANK_COMPETE_KM farther may replace
//      the anchor only if its observation is at least NWS_RANK_MIN_NEWER_S
//      newer. Equal ages (or a smaller difference) keep the nearer one.
//    - The same process repeats on the remaining candidates for the
//      backups.
//    - Candidates whose observation is stale, failed or not fetched are
//      never excluded - they are ranked after all usable ones, nearest
//      first, so a temporary API failure only costs one selection cycle.
//    - Without NTP no age can be known, and a round with no usable
//      observation falls back to plain nearest order (never unreliable
//      freshness comparisons).
//
//  Pure/host-testable: test/nws/test_rank.py mirrors this 1:1.
// ---------------------------------------------------------------------------

// Number of nearby eligible stations whose observation is checked during
// selection (budget: one HTTP request per station).
#define NWS_RANK_CHECK_MAX 8
// Maximum search radius (km) when gathering the check set. Kept in step
// with NWS_BACKUP_MAX_KM: beyond it a station could not become a backup
// anyway, so checking it could only ever cost a request for no benefit.
#define NWS_RANK_RADIUS_MAX_KM 100.0f
// A usable candidate may compete for the anchor slot only if it is at most
// this much farther away...
#define NWS_RANK_COMPETE_KM 10.0f
// ...and its observation is at least this much newer than the anchor's.
#define NWS_RANK_MIN_NEWER_S (20L * 60)

// Great-circle distance (km) between two WGS84 points.
float nwsHaversineKm(float lat1, float lon1, float lat2, float lon2);

// Phase 6.5 preparation: replace each station's distKm with the haversine
// distance from (lat, lon) and reorder the array nearest-first (in
// place). The ranking always works on distances computed from the user's
// own coordinates, never on the NWS-provided field or the API's order.
void nwsRankPrepare(NwsStation* st, int n, float lat, float lon);

// Phase 6.5 ranking (rules in the section doc above). `cands` must be
// nearest-first (nwsRankPrepare); CoCoRaHS entries and `excludeId`
// (NULL/"" = none) are ignored. Fills `out` with up to `maxOut` stations
// in selection order: [preferred, backup 1..N]. Returns the count stored
// (0 = no eligible candidate).
int nwsRankStations(const NwsStation* cands, int nCands, bool ntpOK,
                    const char* excludeId, NwsStation* out, int maxOut);
