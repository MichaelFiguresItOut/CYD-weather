// ---------------------------------------------------------------------------
//  Phase 7: saved location profiles — NVS storage ("cydprof").
//  See profile.h for the layout and association semantics.
// ---------------------------------------------------------------------------

#include "profile.h"
#include "config.h"    // UserSettings, saveLocation(), saveNwsStation()

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>

namespace {

Preferences profPrefs;   // NVS namespace "cydprof"
LocationProfile profs[PROFILE_MAX];
int profActive = -1;    // -1 = no association

// ---- serialization: 12 |-separated fields, fixed order ------------------
// 0 name | 1 postalCode | 2 placeName | 3 lat | 4 lon |
// 5 stnId | 6 stnName | 7 dist | 8 provider | 9..11 backup 0..2
//
// '|' can never appear in these fields: the on-screen keyboard has no such
// key (user-entered name / postal code) and the resolved NWS fields do not
// contain it in practice. A blob that does not parse into exactly these
// fields — e.g. an interrupted or corrupted save — fails validation and the
// slot is treated as EMPTY.
#define PROF_FIELDS 12
#define PROF_BLOB_MAX 320
#define PROF_FLD_MAX  64

void profSlotKey(int i, char* key, size_t n) { snprintf(key, n, "p%d", i); }

// Copy a parsed/source field into a fixed-size destination, NUL-terminated
// (truncating if ever too long; real values are far shorter). A plain
// snprintf "%s" copy trips -Wformat-truncation on the oversized source
// buffers.
void copyField(char* dst, size_t n, const char* src) {
  size_t l = strlen(src);
  if (l >= n) l = n - 1;
  memcpy(dst, src, l);
  dst[l] = 0;
}

int serialize(const LocationProfile* p, char* buf, size_t len) {
  return snprintf(buf, len, "%s|%s|%s|%.6f|%.6f|%s|%s|%.2f|%s|%s|%s|%s",
                  p->name, p->postalCode, p->placeName,
                  (double)p->lat, (double)p->lon,
                  p->nwsStationId, p->nwsStationName, (double)p->nwsStationDistKm,
                  p->nwsStationProvider,
                  p->nwsStationBackup[0], p->nwsStationBackup[1], p->nwsStationBackup[2]);
}

bool parse(const char* s, LocationProfile* p) {
  char f[PROF_FIELDS][PROF_FLD_MAX];
  int n = 0;
  const char* start = s;
  for (const char* c = s;; c++) {
    if (*c == 0 || *c == '|') {
      size_t flen = (size_t)(c - start);
      if (n >= PROF_FIELDS || flen >= PROF_FLD_MAX) return false;
      memcpy(f[n], start, flen);
      f[n][flen] = 0;
      n++;
      if (*c == 0) break;
      start = c + 1;
    }
  }
  if (n != PROF_FIELDS) return false;

  size_t nl = strlen(f[0]);
  if (nl == 0 || nl > PROFILE_NAME_MAX) return false;

  double lat = strtod(f[3], nullptr);
  double lon = strtod(f[4], nullptr);
  if (!isfinite(lat) || !isfinite(lon) || lat < -90.0 || lat > 90.0 ||
      lon < -180.0 || lon > 180.0) {
    return false;
  }
  double dist = strtod(f[7], nullptr);
  if (!isfinite(dist) || dist < 0.0) return false;

  memset(p, 0, sizeof(*p));
  copyField(p->name, sizeof(p->name), f[0]);
  copyField(p->postalCode, sizeof(p->postalCode), f[1]);
  copyField(p->placeName, sizeof(p->placeName), f[2]);
  p->lat = (float)lat;
  p->lon = (float)lon;
  copyField(p->nwsStationId, sizeof(p->nwsStationId), f[5]);
  copyField(p->nwsStationName, sizeof(p->nwsStationName), f[6]);
  p->nwsStationDistKm = (float)dist;
  copyField(p->nwsStationProvider, sizeof(p->nwsStationProvider), f[8]);
  for (int k = 0; k < NWS_BACKUP_MAX; k++)
    copyField(p->nwsStationBackup[k], sizeof(p->nwsStationBackup[k]), f[9 + k]);
  return true;
}

// Persist one slot blob (write mode). Returns false on NVS failure.
bool persistSlot(int i) {
  char blob[PROF_BLOB_MAX];
  serialize(&profs[i], blob, sizeof(blob));
  profPrefs.begin("cydprof", false);
  char key[8];
  profSlotKey(i, key, sizeof(key));
  bool ok = profPrefs.putString(key, blob);
  profPrefs.end();
  if (!ok) Serial.printf("[PROF] slot %d save failed\n", i);
  return ok;
}

void persistActive() {
  profPrefs.begin("cydprof", false);
  if (!profPrefs.putInt("active", profActive))
    Serial.println("[PROF] active save failed");
  profPrefs.end();
}

bool locationConfigured() {
  return userSettings.locationMode != SettingMode::UNCONFIGURED &&
         (userSettings.lat != 0.0f || userSettings.lon != 0.0f ||
          userSettings.postalCode[0] != 0);
}

// Copy the station configuration from userSettings into a profile.
void copyStationsTo(LocationProfile* p) {
  snprintf(p->nwsStationId, sizeof(p->nwsStationId), "%s", userSettings.nwsStationId);
  snprintf(p->nwsStationName, sizeof(p->nwsStationName), "%s", userSettings.nwsStationName);
  p->nwsStationDistKm = userSettings.nwsStationDistKm;
  snprintf(p->nwsStationProvider, sizeof(p->nwsStationProvider), "%s", userSettings.nwsStationProvider);
  for (int k = 0; k < NWS_BACKUP_MAX; k++)
    snprintf(p->nwsStationBackup[k], sizeof(p->nwsStationBackup[k]), "%s", userSettings.nwsStationBackup[k]);
}

// Same-place test: coordinates + resolved place name. The postal code
// is deliberately excluded — the same place can be entered under
// different codes (or none). Used to validate a loaded "active"
// association and to derive the active profile from the current location.
bool profileMatchesCurrent(const LocationProfile* p) {
  return p->lat == userSettings.lat && p->lon == userSettings.lon &&
         strcmp(p->placeName, userSettings.placeName) == 0;
}

}   // namespace

void profileLoad() {
  profActive = -1;
  for (int i = 0; i < PROFILE_MAX; i++) memset(&profs[i], 0, sizeof(profs[i]));

  profPrefs.begin("cydprof", true);   // read-only
  char blob[PROF_BLOB_MAX];
  for (int i = 0; i < PROFILE_MAX; i++) {
    char key[8];
    profSlotKey(i, key, sizeof(key));
    if (profPrefs.getString(key, blob, sizeof(blob)) > 0) {
      if (!parse(blob, &profs[i])) {
        Serial.printf("[PROF] slot %d blob invalid - treated as empty\n", i);
        memset(&profs[i], 0, sizeof(profs[i]));
      }
    }
  }
  int act = -1;
  profPrefs.getInt("active", act);
  if (act >= 0 && act < PROFILE_MAX && profs[act].name[0]) {
    if (profileMatchesCurrent(&profs[act])) {
      profActive = act;
    } else {
      Serial.println("[PROF] active no longer matches location - association dropped");
    }
  }
  profPrefs.end();

  Serial.printf("[PROF] %d/%d slots used, active=%d\n",
                profileCount(), PROFILE_MAX, profActive);
}

int profileCount() {
  int n = 0;
  for (int i = 0; i < PROFILE_MAX; i++) if (profs[i].name[0]) n++;
  return n;
}

const LocationProfile* profileGet(int i) {
  if (i < 0 || i >= PROFILE_MAX || !profs[i].name[0]) return nullptr;
  return &profs[i];
}

int profileActive() {
  // The current location is the ground truth for the active marker: the
  // persisted association wins only while it still points at a matching
  // profile, otherwise the first profile whose stored location is the
  // current one is active (the persisted index only breaks a tie between
  // profiles holding the same location).
  if (userSettings.locationMode == SettingMode::UNCONFIGURED) return -1;
  if (profActive >= 0 && profActive < PROFILE_MAX &&
      profs[profActive].name[0] && profileMatchesCurrent(&profs[profActive]))
    return profActive;
  for (int i = 0; i < PROFILE_MAX; i++)
    if (profs[i].name[0] && profileMatchesCurrent(&profs[i]))
      return i;
  return -1;
}

bool profileNameTaken(const char* name) {
  if (!name || !name[0]) return false;
  for (int i = 0; i < PROFILE_MAX; i++)
    if (profs[i].name[0] && strcasecmp(profs[i].name, name) == 0) return true;
  return false;
}

int profileSaveCurrent(const char* name) {
  if (!name || !name[0] || strlen(name) > PROFILE_NAME_MAX) return -4;
  if (!locationConfigured()) return -3;
  for (int i = 0; i < PROFILE_MAX; i++)
    if (profs[i].name[0] && strcasecmp(profs[i].name, name) == 0) return -1;
  int slot = -1;
  for (int i = 0; i < PROFILE_MAX; i++)
    if (!profs[i].name[0]) { slot = i; break; }
  if (slot < 0) return -2;

  memset(&profs[slot], 0, sizeof(profs[slot]));
  snprintf(profs[slot].name, sizeof(profs[slot].name), "%s", name);
  profs[slot].lat = userSettings.lat;
  profs[slot].lon = userSettings.lon;
  copyField(profs[slot].postalCode, sizeof(profs[slot].postalCode),
            userSettings.postalCode);
  copyField(profs[slot].placeName, sizeof(profs[slot].placeName),
            userSettings.placeName);
  copyStationsTo(&profs[slot]);

  if (!persistSlot(slot)) {          // interrupted save: slot stays empty
    memset(&profs[slot], 0, sizeof(profs[slot]));
    return -5;
  }
  profActive = slot;
  persistActive();
  Serial.printf("[PROF] saved \"%s\" (slot %d)\n", profs[slot].name, slot);
  return 0;
}

bool profileRestore(int i) {
  const LocationProfile* p = profileGet(i);
  if (!p) return false;

  userSettings.lat = p->lat;
  userSettings.lon = p->lon;
  userSettings.locationMode = SettingMode::MANUAL;
  snprintf(userSettings.postalCode, sizeof(userSettings.postalCode), "%s", p->postalCode);
  snprintf(userSettings.placeName, sizeof(userSettings.placeName), "%s", p->placeName);
  saveLocation();

  snprintf(userSettings.nwsStationId, sizeof(userSettings.nwsStationId), "%s", p->nwsStationId);
  snprintf(userSettings.nwsStationName, sizeof(userSettings.nwsStationName), "%s", p->nwsStationName);
  userSettings.nwsStationDistKm = p->nwsStationDistKm;
  snprintf(userSettings.nwsStationProvider, sizeof(userSettings.nwsStationProvider), "%s", p->nwsStationProvider);
  for (int k = 0; k < NWS_BACKUP_MAX; k++)
    snprintf(userSettings.nwsStationBackup[k], sizeof(userSettings.nwsStationBackup[k]), "%s", p->nwsStationBackup[k]);
  saveNwsStation();

  profActive = i;
  persistActive();
  Serial.printf("[PROF] restored \"%s\": %s @ %.4f,%.4f stn=%s\n",
                p->name, p->placeName, (double)p->lat, (double)p->lon, p->nwsStationId);
  return true;
}

void profileClear(int i) {
  const LocationProfile* p = profileGet(i);
  if (!p) return;
  Serial.printf("[PROF] cleared \"%s\" (slot %d)\n", p->name, i);
  memset(&profs[i], 0, sizeof(profs[i]));

  profPrefs.begin("cydprof", false);
  char key[8];
  profSlotKey(i, key, sizeof(key));
  if (!profPrefs.remove(key)) Serial.println("[PROF] slot remove failed");
  if (profActive == i) {
    profActive = -1;
    if (!profPrefs.putInt("active", profActive)) Serial.println("[PROF] active save failed");
  }
  profPrefs.end();
}

void profileClearActive() {
  if (profActive < 0) return;
  Serial.printf("[PROF] association dropped (was \"%s\")\n", profs[profActive].name);
  profActive = -1;
  persistActive();
}

void profileSyncStations() {
  // Use the derived active (location is the ground truth, so a sync
  // happens even when the persisted association was dropped).
  int i = profileActive();
  if (i < 0) return;
  LocationProfile* p = &profs[i];
  copyStationsTo(p);
  if (!persistSlot(i))
    Serial.println("[PROF] station sync save failed");
  else
    Serial.printf("[PROF] stations synced to \"%s\" (stn=%s)\n", p->name, p->nwsStationId);
}
