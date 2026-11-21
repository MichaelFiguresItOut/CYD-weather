#pragma once

// ---------------------------------------------------------------------------
//  Geocoding helpers for the LOCATION settings page (location roadmap,
//  Phase 1):
//
//    geocodePostalOpenMeteo() / geocodePostalNominatim() — forward:
//                       postal code or city name -> candidate places (US
//                       ZIP, ZIP+4, international codes, "CITY, ST"); the
//                       settings flow runs them in sequence (Open-Meteo
//                       first, Nominatim on a definite no-match) and shows
//                       which service is querying.
//    geocodeReverse() — reverse: coordinates -> human-readable location
//
//  Services (all free, no API key, HTTPS):
//    Open-Meteo Geocoding API   primary forward lookup
//    Nominatim (OpenStreetMap)  fallback when Open-Meteo has no match
//    BigDataCloud (keyless)     reverse geocoding (Open-Meteo has none)
//
//  Every function is blocking with bounded timeouts (same 5 s connect /
//  10 s handshake as the weather clients) and is intended to be called
//  from the settings flow, which shows a status line while it runs.
// ---------------------------------------------------------------------------

// One resolved place. `place` is the compact display string
// ("City, Region, CC" — country code preferred so the LOCATION page's
// display row fits city + region + country + postal code), `name` the
// primary name (list rows), `meta` a short "Region, CC" suffix, `postcode`
// the resolved postal code (may be empty).
struct GeoResult {
  float lat, lon;
  char name[40];
  char place[56];
  char meta[24];
  char postcode[12];
};

// Resolve `query` (a postal code or city name) into up to `maxResults`
// using one specific service. Returns the number of results (0 = a definite
// no-match from that service) or -1 on network/parse failure. Results are
// stored in caller-owned `results`. Each call makes at most one request, so
// the settings flow can draw a status line between the two.
int geocodePostalOpenMeteo(const char* query, GeoResult* results, int maxResults);
int geocodePostalNominatim(const char* query, GeoResult* results, int maxResults);

// Resolve coordinates into a readable location. Returns true on success
// (`out` is filled; `postcode` may be empty) or false on network/parse
// failure or when no readable place comes back — the caller then keeps
// the coordinates as-is.
bool geocodeReverse(float lat, float lon, GeoResult* out);
