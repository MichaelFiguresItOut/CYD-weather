#!/usr/bin/env python3
"""
Phase 2B host-side timezone verification.

Mirrors localOffsetSec() from src/time_util.cpp 1:1 (same formulas, same
integer arithmetic), parses the real catalog + IANA map from src/timezones.h,
and compares the mirrored offsets against the OS tz database (zoneinfo) for
every mapped IANA zone across 2024-2033, hour by hour. Also checks exact
transition seconds for the DST zones and the arithmetic helpers against the
standard library.

Usage: python test/tz_verify.py
Exit code 0 = all checks pass.
"""

import re
import sys
import time
import calendar
import datetime
from datetime import timezone as _utczone
from zoneinfo import ZoneInfo

SRC = open("src/timezones.h").read()
CPP = open("src/time_util.cpp").read()

# ---------------------------------------------------------------------------
# Parse the catalog and IANA map out of the real header.
# ---------------------------------------------------------------------------

TZ_RE = re.compile(
    r'\{\s*"([^"]+)",\s*\{\s*"([^"]+)",\s*(-?\d+),\s*(-?\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}\s*\},')
IANA_RE = re.compile(r'\{\s*"([^"]+)",\s*(\d+)\s*\},')

zones = []  # (list name, rule name, std, dst, sm, sw, sh, em, ew, eh)
for m in TZ_RE.finditer(SRC):
    zones.append((m.group(1), m.group(2)) + tuple(int(g) for g in m.groups()[2:]))
assert len(zones) >= 14, f"catalog parse: expected >=14 entries, got {len(zones)}"

iana_block = SRC.split("IANA_MAP[] = {", 1)[1].split("};", 1)[0]
iana_map = {}  # id -> catalog index
for m in IANA_RE.finditer(iana_block):
    iana_map[m.group(1)] = int(m.group(2))
for i, z in enumerate(zones):
    assert z[0] is not None
for iana, idx in iana_map.items():
    assert 0 <= idx < len(zones), f"{iana} -> index {idx} out of range"

print(f"parsed {len(zones)} catalog zones, {len(iana_map)} IANA mappings")

# ---------------------------------------------------------------------------
# Mirror of the C++ code in src/time_util.cpp (kept in sync manually).
# ---------------------------------------------------------------------------

def day_of_week(y, m, d):
    t = (0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4)
    if m < 3:
        y -= 1
    return (y + y // 4 - y // 100 + y // 400 + t[m - 1] + d) % 7

def days_in_month(y, m):
    dim = (31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31)
    if m == 2 and ((y % 4 == 0 and y % 100 != 0) or y % 400 == 0):
        return 29
    return dim[m - 1]

def sunday_day(y, m, week):
    first_sun = 1 + ((7 - day_of_week(y, m, 1)) % 7)
    if week < 5:
        return first_sun + 7 * (week - 1)
    return first_sun + 7 * ((days_in_month(y, m) - first_sun) // 7)

def days_from_civil(y, m, d):
    y -= 1 if m <= 2 else 0
    assert y > 0, "test range is 1970+"
    era = y // 400
    yoe = y - era * 400
    doy = (153 * (m + (-3 if m > 2 else 9)) + 2) // 5 + d - 1
    doe = yoe * 365 + yoe // 4 - yoe // 100 + doy
    return era * 146097 + doe - 719468

def make_utc_time(y, mo, d, h, mi, s):
    return days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s

def local_offset_sec(utc, z):
    _, _, std, dst, sm, sw, sh, em, ew, eh = z
    # C gmtime_r gives years-since-1900 (hence t.tm_year + 1900 in the C++);
    # Python struct_time.tm_year is already the full year.
    y = time.gmtime(utc).tm_year
    dst_start_utc = make_utc_time(y, sm, sunday_day(y, sm, sw), sh, 0, 0) - std
    dst_end_utc = make_utc_time(y, em, sunday_day(y, em, ew), eh, 0, 0) - dst
    if dst_start_utc < dst_end_utc:      # same-year window (US/UK/EU style)
        return dst if dst_start_utc <= utc < dst_end_utc else std
    # year-crossing window (AU/NZ style)
    return dst if (utc >= dst_start_utc or utc < dst_end_utc) else std

# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------

fails = []
def check(label, got, want):
    if got != want:
        fails.append(f"{label}: got {got}, want {want}")

# 1. Arithmetic helpers vs stdlib.
for (y, m, d) in [(2024, 2, 29), (2000, 2, 29), (1900, 2, 28), (2026, 3, 8),
                  (2026, 11, 1), (2026, 3, 29), (2026, 10, 25), (2025, 10, 26),
                  (2023, 3, 26), (2023, 10, 29), (2025, 4, 27), (2026, 4, 26),
                  (2025, 10, 5), (2026, 10, 4), (2026, 9, 6), (2026, 1, 1),
                  (2030, 6, 15), (2033, 12, 31)]:
    check(f"dayOfWeek {y}-{m:02d}-{d:02d}", day_of_week(y, m, d),
          (datetime.date(y, m, d).weekday() + 1) % 7)
for y in range(1990, 2040):
    for m in range(1, 13):
        check(f"daysInMonth {y}-{m:02d}", days_in_month(y, m),
              calendar.monthrange(y, m)[1])
for (y, m, d, h, mi, s) in [(1970, 1, 1, 0, 0, 0), (2026, 1, 1, 0, 0, 0),
                            (2024, 2, 29, 23, 59, 59), (2026, 10, 25, 1, 0, 0),
                            (2033, 12, 31, 23, 59, 59)]:
    check(f"makeUtcTime {y}-{m:02d}-{d:02d} {h:02d}:{mi:02d}:{s:02d}",
          make_utc_time(y, m, d, h, mi, s),
          calendar.timegm((y, m, d, h, mi, s, 0, 0, 0)))

# 2. Hourly sweep 2024-2033, per unique catalog zone (via its first IANA id).
zone_ids = {}
for iana, idx in iana_map.items():
    zone_ids.setdefault(idx, iana)

t0 = time.time()
samples = 0
for idx in sorted(zone_ids):
    iana = zone_ids[idx]
    z = zones[idx]
    zi = ZoneInfo(iana)
    mism = 0
    first = None
    for year in range(2024, 2034):
        for day in range(1, 367):
            try:
                dt = datetime.datetime(year, 1, 1) + datetime.timedelta(days=day - 1)
            except ValueError:
                break
            for hour in range(24):
                dt_h = dt.replace(hour=hour)
                utc = calendar.timegm(dt_h.timetuple())
                want = int(dt_h.replace(tzinfo=_utczone.utc).astimezone(zi).utcoffset().total_seconds())
                got = local_offset_sec(utc, z)
                samples += 1
                if got != want:
                    mism += 1
                    if first is None:
                        first = f"{dt_h.isoformat()}Z mirror={got} tzdb={want}"
    status = "OK" if mism == 0 else f"FAIL {mism} mismatches, first: {first}"
    print(f"  {iana:28s} zone[{idx:2d}] {z[0]:24s} {status}")
    if mism:
        fails.append(f"sweep {iana}: {mism} mismatches, first: {first}")
print(f"sweep done: {samples} samples in {time.time() - t0:.1f}s")

# 3. Exact transition seconds (DST zones) - mirror vs tzdb at the second.
# (year, month, day, hour, minute, second) -> catalog index
BOUNDARIES = {
    "US Eastern": (1, [(2026, 3, 8, 6, 59, 59), (2026, 3, 8, 7, 0, 0),
                       (2026, 11, 1, 5, 59, 59), (2026, 11, 1, 6, 0, 0)]),
    "UK": (5, [(2026, 3, 29, 0, 59, 59), (2026, 3, 29, 1, 0, 0),
               (2026, 10, 25, 0, 59, 59), (2026, 10, 25, 1, 0, 0),
               (2023, 3, 25, 23, 59, 59), (2023, 3, 26, 1, 0, 0),
               (2023, 10, 28, 23, 59, 59), (2023, 10, 29, 1, 0, 0),
               (2025, 10, 26, 0, 59, 59), (2025, 10, 26, 1, 0, 0)]),
    "CET": (6, [(2026, 3, 29, 0, 59, 59), (2026, 3, 29, 1, 0, 0),
                (2026, 10, 25, 0, 59, 59), (2026, 10, 25, 1, 0, 0),
                (2025, 10, 26, 0, 59, 59), (2025, 10, 26, 1, 0, 0)]),  # EU: 01:00 UTC
    "EET": (7, [(2026, 3, 29, 0, 59, 59), (2026, 3, 29, 1, 0, 0),
                (2026, 10, 25, 0, 59, 59), (2026, 10, 25, 1, 0, 0)]),
    "Sydney": (12, [(2025, 10, 4, 15, 59, 59), (2025, 10, 4, 16, 0, 0),  # start 1st Sun Oct (5th)
                    (2025, 4, 6, 15, 59, 59), (2025, 4, 6, 16, 0, 0),    # end 1st Sun Apr (6th)
                    (2025, 12, 31, 12, 0, 0), (2026, 2, 15, 12, 0, 0),   # year-crossing DST
                    (2026, 4, 5, 15, 59, 59), (2026, 4, 5, 16, 0, 0),    # end 1st Sun Apr (5th)
                    (2026, 6, 15, 12, 0, 0),                              # standard time
                    (2026, 10, 3, 15, 59, 59), (2026, 10, 3, 16, 0, 0)]),  # start 1st Sun Oct (4th)
    "Auckland": (13, [(2025, 9, 27, 13, 59, 59), (2025, 9, 27, 14, 0, 0),  # start last Sun Sep (28th) 02:00
                      (2025, 4, 5, 13, 59, 59), (2025, 4, 5, 14, 0, 0),    # end 1st Sun Apr (6th) 03:00
                      (2026, 1, 1, 0, 0, 0),                               # year-crossing DST
                      (2026, 4, 4, 13, 59, 59), (2026, 4, 4, 14, 0, 0),   # end 1st Sun Apr (5th) 03:00
                      (2026, 9, 26, 13, 59, 59), (2026, 9, 26, 14, 0, 0),  # start last Sun Sep (27th) 02:00
                      (2026, 12, 31, 12, 0, 0)]),
    "Alaska": (14, [(2026, 3, 8, 10, 59, 59), (2026, 3, 8, 11, 0, 0),
                    (2026, 11, 1, 9, 59, 59), (2026, 11, 1, 10, 0, 0),
                    (2026, 1, 15, 12, 0, 0), (2026, 7, 4, 12, 0, 0)]),
    "no-DST zones": (None, [(2026, 1, 15, 12, 0, 0), (2026, 7, 4, 12, 0, 0),
                            (2026, 3, 8, 7, 0, 0), (2026, 11, 1, 6, 0, 0)]),
}
for name, (idx, instants) in BOUNDARIES.items():
    if idx is None:  # every constant-offset zone stays constant
        for i, z in enumerate(zones):
            if z[2] == z[3]:   # std == dst: constant offset
                zi = ZoneInfo(zone_ids[i]) if i in zone_ids else None
                if zi is None:
                    continue
                for inst in instants:
                    dt = datetime.datetime(*inst)
                    utc = calendar.timegm(dt.timetuple())
                    check(f"constant {z[0]} @ {dt.isoformat()}Z",
                          local_offset_sec(utc, z),
                          int(dt.replace(tzinfo=_utczone.utc).astimezone(zi).utcoffset().total_seconds()))
        continue
    zi = ZoneInfo(zone_ids[idx])
    for inst in instants:
        dt = datetime.datetime(*inst)
        utc = calendar.timegm(dt.timetuple())
        check(f"{name} @ {dt.isoformat()}Z",
              local_offset_sec(utc, zones[idx]),
              int(dt.replace(tzinfo=_utczone.utc).astimezone(zi).utcoffset().total_seconds()))

# 4. The four Phase 2B special zones carry the exact expected offsets.
EXPECT = {  # iana: (std, dst)
    "America/Anchorage": (-9 * 3600, -8 * 3600),
    "America/Phoenix":   (-7 * 3600, -7 * 3600),
    "Pacific/Honolulu":  (-10 * 3600, -10 * 3600),
    "Asia/Kathmandu":    (5 * 3600 + 45 * 60, 5 * 3600 + 45 * 60),
}
for iana, (std, dst) in EXPECT.items():
    check(f"{iana} std", zones[iana_map[iana]][2], std)
    check(f"{iana} dst", zones[iana_map[iana]][3], dst)

# 5. Sanity of the C++ source itself: the mirror must be present in it.
for token in ["sundayDay", "daysInMonth", "dstStartUtc < dstEndUtc",
              "utc >= dstStartUtc || utc < dstEndUtc"]:
    if token not in CPP:
        fails.append(f"src/time_util.cpp missing token {token!r} (mirror out of sync?)")

print()
if fails:
    print(f"FAILURES ({len(fails)}):")
    for f in fails:
        print("  " + f)
    sys.exit(1)
print("ALL CHECKS PASSED")
