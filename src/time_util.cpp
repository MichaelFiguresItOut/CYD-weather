#include <Arduino.h>
#include <time.h>
#include "time_util.h"
#include "settings.h"

// The toolchain's tzset() parser is unreliable for TZ strings with DST rules,
// so the system clock is kept on UTC and the local offset is computed in
// software instead.
//
// US Eastern Time (America/New_York):
//   EST = UTC-5  (standard time)
//   EDT = UTC-4  (daylight saving time, since 2007: starts 02:00 local on the
//                second Sunday of March, ends 02:00 local on the first Sunday
//                of November)
static const char* NTP_TZ = "UTC";

// Day of week (0=Sunday .. 6=Saturday) – Tomohiko Ito's algorithm.
int dayOfWeek(int y, int m, int d) {
  static const int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
  if (m < 3) y--;
  return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
}

// Days since 1970-01-01 for a civil date – Howard Hinnant's algorithm.
long daysFromCivil(int y, int m, int d) {
  y -= (m <= 2);
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);                          // [0,399]
  unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)d - 1u; // [0,365]
  unsigned doe = yoe * 365u + yoe/4u - yoe/100u + doy;               // [0,146096]
  return era * 146097L + (long)doe - 719468;
}

long makeUtcTime(int y, int mo, int d, int h, int mi, int s) {
  return daysFromCivil(y, mo, d) * 86400L + h * 3600L + mi * 60L + s;
}

// Current US Eastern offset in seconds (negative = west of UTC).
long localOffsetSec(time_t utc) {
  struct tm t;
  gmtime_r(&utc, &t);
  int y = t.tm_year + 1900;

  // DST starts: second Sunday of March at 02:00 EST (= 07:00 UTC)
  int marFirstSun = 1 + ((7 - dayOfWeek(y, 3, 1)) % 7);
  time_t dstStartUtc = makeUtcTime(y, 3, marFirstSun + 7, 7, 0, 0);

  // DST ends: first Sunday of November at 02:00 EDT (= 06:00 UTC)
  int novFirstSun = 1 + ((7 - dayOfWeek(y, 11, 1)) % 7);
  time_t dstEndUtc   = makeUtcTime(y, 11, novFirstSun, 6, 0, 0);

  return (utc >= dstStartUtc && utc < dstEndUtc) ? -4L * 3600 : -5L * 3600;
}

bool ntpSynced = false;

// ---------------------------------------------------------------------------
//  US-style time formatting: "3:47 PM" / "12:05:09 AM"
//  (strftime %I zero-pads the hour; US style drops the leading zero)
// ---------------------------------------------------------------------------
void formatUS12(char* out, size_t n, const struct tm* lt, bool withSeconds) {
  if (withSeconds) strftime(out, n, "%I:%M:%S %p", lt);
  else             strftime(out, n, "%I:%M %p", lt);
  if (out[0] == '0') memmove(out, out + 1, strlen(out));
}

// Clock string in the currently selected mode (US 12-hour or 24-hour).
void formatClock(char* out, size_t n, const struct tm* lt) {
  if (use24Hour) snprintf(out, n, "%02d:%02d", lt->tm_hour, lt->tm_min);
  else           formatUS12(out, n, lt, false);
}

// Widest possible clock string in the current mode (partial-update box size).
const char* clockMaxString() { return use24Hour ? "23:59" : "12:59 PM"; }

// Last-update time in the currently selected clock mode (US 12h or 24h), so the
// "Updated ..." / "UPD ..." lines follow the same tap toggle as the main clock.
void formatUpdTime(char* out, size_t n, const struct tm* lt, bool withSeconds) {
  if (use24Hour) snprintf(out, n, withSeconds ? "%02d:%02d:%02d" : "%02d:%02d",
                          lt->tm_hour, lt->tm_min, lt->tm_sec);
  else           formatUS12(out, n, lt, withSeconds);
}

// Date line in the currently selected format. Format 0 keeps the original
// "Thursday 12 November 2026"; format 1 is US-style with the weekday kept in
// place, "Thursday November 12, 2026". Longest case ("Wednesday September 30,
// 2026") is 336 px at size 2 – dateLineFit() scales such lines down slightly.
void formatDateLine(char* out, size_t n, const struct tm* lt) {
  if (dateFormat == 0) { strftime(out, n, "%A %d %B %Y", lt); return; }
  strftime(out, n, "%A %B %d, %Y", lt);
}

// Temperature conversion for display only (wx values stay in °F internally).
float tempForDisplay(float f) { return useCelsius ? (f - 32.0f) * 5.0f / 9.0f : f; }
const char* tempUnitChar()    { return useCelsius ? "C" : "F"; }

// ---------------------------------------------------------------------------
//  NTP (configTzTime is provided by the ESP32 Arduino core itself)
// ---------------------------------------------------------------------------
void configureTime() {
  configTzTime(NTP_TZ, "pool.ntp.org", "time.google.com", "time.windows.com");
}

// The core has no timeStatus(); treat a post-2023 epoch value as "synced".
bool ntpIsSynced() {
  return time(nullptr) > 1700000000UL;
}

// Break the current local time (UTC + DST-aware offset) into a struct tm.
bool nowLocal(struct tm* out) {
  if (!ntpIsSynced()) return false;
  time_t utc   = time(nullptr);
  time_t local = utc + localOffsetSec(utc);
  localtime_r(&local, out);   // TZ is UTC, so this just decomposes the epoch
  return true;
}
