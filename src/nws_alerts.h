#pragma once

// ---------------------------------------------------------------------------
//  NWS active alerts (api.weather.gov)
//
//  GET https://api.weather.gov/alerts/active?point={lat},{lon}
//  returns every alert currently in effect at the configured location
//  (geo+json FeatureCollection). The firmware keeps a RAM cache that drives
//  the dashboard's "WEATHER ALERT" button (render.cpp) and the full-screen
//  alert list (alerts_screen.cpp):
//
//    - cadence: every 5 min while at least one alert is active, else every
//      15 min (ALERTS_REFRESH_*_MS in config.h), plus forced checks after
//      boot, a Wi-Fi connect (alertsForceDue) and a location change
//      (alertsLocationChanged);
//    - a failed fetch (no network, timeout, unparseable or truncated
//      response) always keeps the previous cache — stale alerts are shown
//      rather than silently disappearing;
//    - a location change clears the cache first: an alert that does not
//      apply to the new location must not keep showing.
//
//  Heap discipline follows nws_station.cpp: a scoped WiFiClientSecure, a
//  byte-capped download (NWS pretty-prints its JSON, ~3 KB raw per alert),
//  and an ArduinoJson DeserializationFilter that keeps only
//  features[].properties (skips geometry/polygon, @context, ids).
// ---------------------------------------------------------------------------

#include <stdint.h>

// Cache size: point queries return few alerts; a heavy regional storm might
// list more, but the extras are dropped (the screen paginates what is kept).
#define NWS_ALERTS_MAX 8

// One cached alert (display-ready subset of the NWS properties).
struct NwsAlert {
  char event[28];        // uppercased, e.g. "HIGH WIND WATCH"
  char severity[9];      // "EXTREME" | "SEVERE" | "MODERATE" | "MINOR"
  char area[48];         // affected area description (uppercased, truncated)
  char description[1280]; // HTML stripped, truncated (NWS watches run ~1.2 KB)
  char instructions[120];// HTML stripped, truncated
  long ends;             // event end (NWS "ends"), UTC epoch seconds; 0 = unknown
};

int   alertsCount();              // cached alerts
const NwsAlert* alertsAt(int i);  // i in [0, alertsCount()); nullptr otherwise
bool  alertsActive();             // at least one alert cached
// Most severe cached alert: 0 = EXTREME, 1 = SEVERE, 2 = MODERATE,
// 3 = MINOR; -1 when none (the button colors map these).
int   alertsHighestSeverity();
// True when a fetch should be attempted (cadence above; false while Wi-Fi
// is down or a recent attempt is still young).
bool  alertsDue();
// Perform the fetch. Returns true when the cached set changed, so callers
// can redraw the dashboard (button appears/disappears/recolors). On any
// failure the previous cache is kept.
bool  alertsFetch();
// Force the next check (boot, Wi-Fi connect).
void  alertsForceDue();
// The weather location changed: drop the stale cache and force a re-fetch.
void  alertsLocationChanged();
