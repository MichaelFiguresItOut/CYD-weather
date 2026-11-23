#pragma once

// ---------------------------------------------------------------------------
//  Phase 7: saved location profiles
//
//  Up to PROFILE_MAX named locations, each holding the full location +
//  NWS station configuration (preferred station + the ordered automatic
//  backups), so a profile can be restored without re-geocoding or station
//  rediscovery. Persisted in the NVS namespace "cydprof" — separate from
//  "cydwx" so a corrupted or interrupted profile save can never touch the
//  main settings, and vice versa.
//
//  Storage layout ("cydprof"):
//    "p0".."p2"  str  one serialized profile per slot: 12 |-separated
//                     fields (name, postal code, place name, lat, lon,
//                     preferred station id/name/distance/provider, the 3
//                     backup ids). Validated on load — a blob that does not
//                     parse exactly (e.g. an interrupted save) is treated
//                     as an EMPTY slot.
//    "active"    int  index of the profile the current location belongs
//                     to, or -1 (no association).
//
//  Association semantics (kept in sync by the settings flows):
//    - SAVE CURRENT            -> the new slot becomes active.
//    - RESTORE                 -> the restored slot becomes active.
//    - CLEAR (profile)         -> the slot is deleted; active becomes -1
//                                 if it was that profile. The current
//                                 location itself is never touched.
//    - ADD LOCATION / location reset -> active becomes -1 (the saved
//      profiles are kept, the location is simply unsaved).
//    - SCAN (station selection, or a scan that clears the selection) ->
//      the active profile's station configuration is updated in place.
//
//  The timezone is NEVER stored in a profile: it is automatic only and is
//  re-resolved from the restored coordinates on the next weather fetch
//  (applyAutoTimezoneFromIana, weather.cpp).
// ---------------------------------------------------------------------------

#include <stdint.h>
#include "nws_station.h"   // NWS_BACKUP_MAX (per-profile backup count)

// Number of save slots, and the user-defined name limit (also enforced by
// the UI keyboard buffer).
#define PROFILE_MAX      3
#define PROFILE_NAME_MAX 12

// One saved location: everything needed to restore it exactly.
struct LocationProfile {
  char  name[PROFILE_NAME_MAX + 1];      // user-defined, "" = unused slot
  char  postalCode[32];                  // entered postal code / city ("" if none)
  char  placeName[56];                   // resolved display name ("City, Region, CC")
  float lat, lon;
  // NWS station configuration: preferred + ordered automatic backups.
  // (Same field sizes as UserSettings — see config.h.)
  char  nwsStationId[8];
  char  nwsStationName[40];
  float nwsStationDistKm;
  char  nwsStationProvider[16];
  char  nwsStationBackup[NWS_BACKUP_MAX][8];
};

// Load the slots + active index from NVS (call once in setup, after
// loadSettings()). An "active" association whose stored location no longer
// matches the loaded location (inconsistent NVS state) is dropped.
void profileLoad();

int  profileCount();                         // number of used slots
const LocationProfile* profileGet(int i);    // slot, or NULL when unused
int  profileActive();                        // active slot index, or -1
bool profileNameTaken(const char* name);     // case-insensitive name match

// Save the current location + station configuration as a new profile
// (first empty slot). Returns 0 on success (the slot becomes active),
// -1 name already in use (case-insensitive), -2 all slots used,
// -3 no location configured to save, -4 bad name, -5 NVS save failed.
int  profileSaveCurrent(const char* name);

// Copy the slot's location + station configuration into userSettings
// (persisted via saveLocation()/saveNwsStation()) and make it active.
// Returns false when the slot is unused.
bool profileRestore(int i);

// Delete a slot; clears the active association when it was the active one.
// The current location is never touched.
void profileClear(int i);

// The current location is no longer a saved one (ADD LOCATION / location
// reset): drop the active association, keep the saved profiles.
void profileClearActive();

// SCAN changed the station configuration: update the active profile's
// station fields in place (no-op when there is no active profile).
void profileSyncStations();
