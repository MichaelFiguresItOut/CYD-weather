// ---------------------------------------------------------------------------
//  Timezone catalog (automatic resolution, Phase 2A/2B)
//
//  TimezoneRules (config.h) is a small fixed-size struct — no Tz rules, no TZ
//  strings: stdOffsetSec/dstOffsetSec in seconds (negative = west of UTC),
//  DST transitions as "Nth Sunday of month at local wall-clock time"
//  (dstStartWeek/dstEndWeek 1..5, 5 = last Sunday), evaluated by
//  localOffsetSec() in time_util.cpp.
//
//  The timezone is automatic only: it always follows the active location
//  (the IANA id Open-Meteo reports for the coordinates, mapped through
//  IANA_MAP below). The catalog is the rule set an IANA id can resolve to;
//  there is no manual selection. Zones without DST (UTC, MSK, India, ...)
//  set dstOffsetSec = stdOffsetSec so the DST window is a no-op. Keep every
//  entry valid under validTimezone() (config.cpp).
//
//  Every entry is a distinct real-world rule: no-DST zones are never
//  approximated by a neighboring DST zone (Phoenix keeps plain MST, Honolulu
//  plain HST, Kathmandu +05:45) and Alaska gets its own -9/-8 entry instead
//  of being mapped onto US Mountain.
//
//  DST calendar data follows the current real-world rules (checked 2026):
//  US: 2nd Sun Mar 02:00 -> 1st Sun Nov 02:00.
//  UK: last Sun Mar 01:00 -> last Sun Oct 02:00.
//  EU: last Sun Mar 01:00/02:00/03:00 UTC -> last Sun Oct (CEST 02:00/
//      EEST 03:00 local). AU East: 1st Sun Oct 02:00 -> 1st Sun Apr 03:00.
//  NZ: last Sun Sep 02:00 -> 1st Sun Apr 03:00.
// ---------------------------------------------------------------------------

#pragma once

#include <Arduino.h>
#include "config.h"

struct TimezoneEntry {
  const char* name;    // shown on the Settings list rows
  TimezoneRules rules; // applied to userSettings.timezone on selection
};

static const TimezoneEntry TIMEZONES[] = {
  { "UTC",                   { "UTC",                   0,      0,      3, 2, 2, 11, 1, 2 } },
  { "US Eastern",            { "US Eastern",          -18000, -14400,  3, 2, 2, 11, 1, 2 } },
  { "US Central",            { "US Central",          -21600, -18000,  3, 2, 2, 11, 1, 2 } },
  { "US Mountain",           { "US Mountain",         -25200, -21600,  3, 2, 2, 11, 1, 2 } },
  { "US Pacific",            { "US Pacific",          -28800, -25200,  3, 2, 2, 11, 1, 2 } },
  { "UK (GMT/BST)",          { "UK (GMT/BST)",            0,   3600,  3, 5, 1, 10, 5, 2 } },
  { "Central Europe (CET)",  { "Central Europe (CET)", 3600,   7200,  3, 5, 2, 10, 5, 3 } },
  { "Eastern Europe (EET)",  { "Eastern Europe (EET)", 7200,  10800,  3, 5, 3, 10, 5, 4 } },
  { "Russia (MSK)",          { "Russia (MSK)",        10800,  10800,  3, 2, 2, 11, 1, 2 } },
  { "India",                 { "India",               19800,  19800,  3, 2, 2, 11, 1, 2 } },
  { "China",                 { "China",               28800,  28800,  3, 2, 2, 11, 1, 2 } },
  { "Japan",                 { "Japan",               32400,  32400,  3, 2, 2, 11, 1, 2 } },
  { "Australia East (AEST)", { "Australia East (AEST)", 36000, 39600, 10, 1, 2,  4, 1, 3 } },
  { "New Zealand (NZST)",    { "New Zealand (NZST)",  43200,  46800,  9, 5, 2,  4, 1, 3 } },
  // No-DST and special offsets: Alaska follows the US DST dates at -9/-8;
  // the rest never observe DST (dst = std makes the window a no-op).
  { "Alaska (AKST/AKDT)",    { "Alaska (AKST/AKDT)", -32400, -28800,  3, 2, 2, 11, 1, 2 } },
  { "Arizona (MST)",         { "Arizona (MST)",       -25200, -25200,  3, 2, 2, 11, 1, 2 } },
  { "Hawaii (HST)",          { "Hawaii (HST)",        -36000, -36000,  3, 2, 2, 11, 1, 2 } },
  { "Nepal (NPT)",           { "Nepal (NPT)",          20700,  20700,  3, 2, 2, 11, 1, 2 } },
};

#define TIMEZONE_COUNT (sizeof(TIMEZONES) / sizeof(TIMEZONES[0]))

// Phase 2A (extended in Phase 2B): IANA identifiers (Open-Meteo
// `timezone=auto` response) -> catalog entries. Only ids whose real-world
// rules exactly match the catalog entry are mapped; everything else (e.g.
// "America/Mexico_City" - no DST since 2022, "Pacific/Fiji" - southern DST
// on different dates, "Australia/Brisbane" - no DST) is unsupported, in
// which case the caller keeps the last valid rules. Same-rule cities of an
// existing entry (Dublin/Lisbon on UK rules, Prague/Warsaw/... on CET,
// Montreal on US Eastern, Canberra/Hobart on AEST) are mapped to that
// entry.
struct IanaMap { const char* iana; int index; };
static const IanaMap IANA_MAP[] = {
  { "UTC", 0 }, { "Etc/UTC", 0 },
  { "America/New_York", 1 }, { "America/Toronto", 1 }, { "America/Detroit", 1 },
  { "America/Montreal", 1 },
  { "America/Chicago", 2 }, { "America/Winnipeg", 2 },
  { "America/Denver", 3 }, { "America/Edmonton", 3 },
  { "America/Los_Angeles", 4 }, { "America/Vancouver", 4 },
  { "Europe/London", 5 }, { "Europe/Dublin", 5 }, { "Europe/Lisbon", 5 },
  { "Europe/Berlin", 6 }, { "Europe/Paris", 6 }, { "Europe/Amsterdam", 6 },
  { "Europe/Brussels", 6 }, { "Europe/Madrid", 6 }, { "Europe/Rome", 6 },
  { "Europe/Prague", 6 }, { "Europe/Vienna", 6 }, { "Europe/Warsaw", 6 },
  { "Europe/Zurich", 6 }, { "Europe/Stockholm", 6 }, { "Europe/Copenhagen", 6 },
  { "Europe/Oslo", 6 }, { "Europe/Belgrade", 6 }, { "Europe/Zagreb", 6 },
  { "Europe/Ljubljana", 6 }, { "Europe/Luxembourg", 6 }, { "Europe/Monaco", 6 },
  { "Europe/Tallinn", 6 }, { "Europe/Riga", 6 }, { "Europe/Vilnius", 6 },
  { "Europe/Athens", 7 }, { "Europe/Helsinki", 7 }, { "Europe/Sofia", 7 },
  { "Europe/Bucharest", 7 }, { "Europe/Kyiv", 7 }, { "Europe/Nicosia", 7 },
  { "Europe/Moscow", 8 },
  { "Asia/Kolkata", 9 },
  { "Asia/Shanghai", 10 }, { "Asia/Hong_Kong", 10 }, { "Asia/Singapore", 10 },
  { "Asia/Tokyo", 11 }, { "Asia/Seoul", 11 },
  { "Australia/Sydney", 12 }, { "Australia/Melbourne", 12 },
  { "Australia/Canberra", 12 }, { "Australia/Hobart", 12 },
  { "Pacific/Auckland", 13 },
  { "America/Anchorage", 14 }, { "America/Juneau", 14 }, { "America/Fairbanks", 14 },
  { "America/Phoenix", 15 },
  { "Pacific/Honolulu", 16 },
  { "Asia/Kathmandu", 17 },
};

// Index of the catalog entry for an IANA id, -1 when unsupported.
inline int timezoneIndexByIana(const char* iana) {
  if (!iana) return -1;
  for (size_t i = 0; i < sizeof(IANA_MAP) / sizeof(IANA_MAP[0]); i++)
    if (strcmp(iana, IANA_MAP[i].iana) == 0) return IANA_MAP[i].index;
  return -1;
}
