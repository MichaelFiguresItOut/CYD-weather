# Adding NBM (`ncep_nbm_conus`) as a second precipitation source — investigation report

Date: 2026-11-13 · Location: project site (coordinates in `src/local_config.h`) · Provider: Open-Meteo `/v1/forecast`
Scope: investigation only — **no firmware files were modified**.

## TL;DR / recommendation

- A single combined request `models=ncep_rrfs_conus,ncep_nbm_conus` is possible, but it does
  **not** preserve the RRFS data the firmware uses today. The multi-model response serves a
  *different* RRFS product (interpolated from hourly, different grid cell) than the current
  best-match request (native sub-hourly RRFS). Switching would silently change every number in
  `analyzeRainTimeline()`.
- **Recommended: keep the existing request byte-for-byte and add a second small GET** for NBM
  (`models=ncep_nbm_conus&hourly=precipitation`, ~1.5 KB body). Cost: one extra HTTP round-trip
  per refresh (~0.5–2 s) and ~4–5 KB of transient heap in a scoped helper. Zero risk to the
  existing RRFS path; NBM failures degrade gracefully to "no cross-check".
- NBM hourly `precipitation` at stamp T is a **trailing sum over (T−60 min, T]** — the same
  convention the firmware already assumes for RRFS steps and hourly values. The two series are
  directly comparable at identical local wall-clock stamps.

---

## Q1 — JSON response structure with multiple models

The multi-model response is **flat**, not nested per model. There is one shared `time[]` array
per interval group, and each requested variable appears once per model under a
`<variable>_<model_id>` key:

```jsonc
{
  "latitude": 40.76257,            // single grid point (one of the models'), not per-model
  "longitude": -73.97491,
  "current": {                      // NOT suffixed — one set of values in multi-model mode
    "time": "2026-11-12T19:45", "temperature_2m": 48.9, "rain": 0.075, ...
  },
  "minutely_15": {
    "time": ["2026-11-12T00:00", "..."],                    // shared, n=192 for forecast_days=2
    "precipitation_ncep_rrfs_conus": [ ... ],                // 192 values
    "precipitation_ncep_nbm_conus":   [ ... ]                // 192 values
  },
  "hourly": {
    "time": ["2026-11-12T00:00", "..."],                     // shared, n=48
    "rain_ncep_rrfs_conus":        [ ... ],                  // 48 values
    "precipitation_ncep_nbm_conus":[ ... ]                   // 48 values
  },
  "daily": {
    "time": ["2026-11-12", "2026-11-13"],                    // shared, n=2
    "temperature_2m_max_ncep_rrfs_conus": [ ... ],
    "temperature_2m_min_ncep_nbm_conus":  [ ... ]             // every requested var × model
  }
}
```

Verified facts:

- Single-model requests (`models=X`) are flat with **no** suffixes (root `hourly.<var>`,
  `minutely_15.<var>`).
- Variables a given model doesn't support are simply absent from that model's suffixed set.
- `current` is *not* suffixed in multi-model mode (single value set); `daily`, `hourly`, and
  `minutely_15` all are.
- `models=auto,ncep_nbm_conus` → **HTTP 400**. Best-match cannot be mixed with explicit models,
  so a single request can never return "today's best-match RRFS + NBM".

## Q2 — Can both models go in the existing request while preserving RRFS?

**Technically yes, semantically no.** The combined response does contain an RRFS series
(`precipitation_ncep_rrfs_conus`, `rain_ncep_rrfs_conus`, …), but it is *not* the data the
firmware receives today. Measured (requests issued seconds apart, each stable on repeat):

| | current firmware request (no `models=`) | explicit `ncep_rrfs_conus` (alone or in combined mode) |
|---|---|---|
| 15-min grid point served | 40.762505, −73.975105 | 40.76257, −73.97491 (different cell) |
| 15-min values vs baseline | — | **123 of 192 differ** (e.g. local 13:15–14:00: native `0.205/0.138/0.02/0.0` vs explicit `0.031×4`) |
| hourly `rain` values vs baseline | — | **35 of 48 differ** (e.g. local 14:00: 0.362 vs 0.126) |
| 15-min character | true sub-hourly variability | block-constant in groups of four = hourly/4 interpolation |

Open-Meteo's docs confirm the split: best-match `minutely_15` is "based on NOAA RRFS model for
North America" (a dedicated native sub-hourly feed), while 15-min data requested outside that
pipeline "will use interpolation". The combined-mode RRFS block was verified to match the
explicit single-model fetch exactly and to differ from best-match.

Consequences of switching the existing request to `models=ncep_rrfs_conus,ncep_nbm_conus`:

1. Every number in `analyzeRainTimeline()` changes (different product + grid cell) — thresholds
   tuned on native data (`RAIN_15MIN_THRESH_IN = 0.004`, dry-gap bridging) would run on a
   different series, and the 15-min resolution effectively degrades to hourly/4.
2. All JSON paths in `main.cpp` need suffix updates: `minutely_15.precipitation` →
   `minutely_15.precipitation_ncep_rrfs_conus`, `hourly.rain` → `hourly.rain_ncep_rrfs_conus`,
   `daily.temperature_2m_max/min` → suffixed. (`current.*` paths stay as-is.)

This violates the "preserve existing RRFS implementation" constraint, so it is **not** the
recommended path (though it remains a valid fallback if one HTTP round-trip per refresh becomes
a hard requirement).

## Q3 — Response sizes and RAM cost

Measured body sizes for this location, `forecast_days=2`, inches:

| Request | Body size |
|---|---|
| Current firmware request (best-match) | **6 929 B** |
| Combined multi-model (`models=ncep_rrfs_conus,ncep_nbm_conus`, same vars + NBM precip) | **9 520 B** (+2 591 B, +37 %) |
| NBM-only hourly (`models=ncep_nbm_conus&hourly=precipitation`) | **1 484 B** |

ArduinoJson v7 `JsonDocument` allocates dynamically from heap; peak ≈ raw body `String` plus the
document tree (~1.5–2× text size for this content), all freed when the document goes out of scope:

| Option | Transient peak in fetch path | Delta vs today |
|---|---|---|
| Today (one request) | ~6.9 KB String + ~11 KB doc ≈ **18 KB** | — |
| Two requests (recommended): existing parse untouched; NBM parsed in its own scoped helper with its own `JsonDocument` | 18 KB, then separately ~1.5 KB String + ~3 KB doc ≈ **+4–5 KB** (never concurrent) | negligible |
| One combined request | ~9.5 KB String + ~16 KB doc ≈ **25–26 KB** | +7–8 KB transient |

Either option is trivial against ESP32 free heap (hundreds of KB; static RAM usage 16 %). Flash:
the new code path (URL string, parse loop, cross-check logic) adds roughly 1–2 KB to a build at
44 % flash. The only real cost of the two-request option is one extra TLS/HTTP round-trip per
refresh (~0.5–2 s on WiFi).

## Q4 — Timestamp semantics: NBM hourly vs RRFS 15-min

**NBM hourly `precipitation` at stamp T = total precipitation over (T−60 min, T]** (trailing
sum), the same convention already documented in `main.cpp` for RRFS steps and hourly values.

Verification against METAR ground truth (KJRB, local 2026-11-12; P00xx accumulated per report):

| Local hour | Observed at KJRB | NBM-hr stamp that should carry it if trailing |
|---|---|---|
| 15–16 | 0.090 in | [16:00] = 0.079 ✓ (leading would put it at [15:00]=0.142, but obs for 16–17 was 0.000) |
| 18–19 | **2.370 in** (tornado cell, TSE warnings) | **[19:00] = 0.169 — the NBM maximum** ✓ (leading would require [18:00]=0.051 to be max ✗) |
| 19–20 | 0.030 in | [20:00] = 0.110 (NBM over-predicts the tail, but ordering is right) |

The peak-position test is decisive even though NBM's *magnitude* is far off for this event
(see caveats). Cross-model correlation tests were inconclusive because NBM and RRFS genuinely
disagree on amounts — that disagreement is precisely what makes a cross-check useful.

Additional findings:

- **NBM `minutely_15` in multi-model mode is derived, not native.** 47/47 four-step blocks
  ending at hour stamps are near-constant, and each block's sum ≈ the corresponding NBM hourly
  value (within rounding). I.e. `NBM15[T] ≈ NBM-hourly[ceil_hour(T)] / 4`. If the firmware ever
  consumes the NBM 15-min array with the existing step logic, the per-step threshold must be
  scaled ×¼ (0.004 in → ~0.001 in). Simpler: use the NBM **hourly** series directly with the
  existing `RAIN_HOURLY_THRESH_IN = 0.02` convention.
- Because both sources are trailing sums on local wall-clock stamps, they align exactly at the
  same timestamps: after RRFS declares rain ended at E, the window (E, E+1 h] is precisely what
  NBM's value at stamp E+1 h reports.

## Recommended implementation approach (no changes made yet)

1. **Leave `fetchWeather()` and its URL untouched** (`src/main.cpp` ~line 600). Best-match keeps
   serving native RRFS 15-min + hourly rain exactly as today.
2. Add a small helper, e.g. `bool fetchNbmHourly(JsonDocument& nbmDoc)`:
   - GET `https://api.open-meteo.com/v1/forecast?latitude=<LAT>&longitude=<LON>` (site coords from `src/local_config.h`)
     `&models=ncep_nbm_conus&hourly=precipitation&timezone=auto&forecast_days=2&precipitation_unit=inch`
   - Parse root-level `hourly.time[]` + `hourly.precipitation[]` (flat, unsuffixed — verified).
   - Scope the `JsonDocument` to the helper so its ~3–5 KB peak is freed on return.
3. Cross-check rule in/after `analyzeRainTimeline()`: once RRFS yields a rain end time E (or
   "rain runs to edge of data"), scan NBM hourly stamps T > E; if any `NBM[T] ≥ 0.02 in` within a
   horizon (e.g. 6 h), flag/annotate "NBM indicates continued rain until <last such stamp>".
   Optionally use a lower threshold (~0.01 in/h) for the continuation flag, since NBM's overnight
   drizzle sits at 0.004–0.02 in/h — keep it a named constant either way.
4. **Graceful degradation:** any fetch/parse failure or missing array → skip the cross-check;
   existing behavior is unchanged. This also insulates the firmware if Open-Meteo changes NBM
   availability or multi-model behavior later.

## Caveats

- **NBM is a blend product and under-predicts convective peaks.** For the 18–19 h tornado cell it
  reported 0.169 in vs 2.37 in observed (native RRFS said 0.094 in). Use NBM as an independent
  secondary check for *stratiform/continued* rain — exactly the "when does the rain end" use case
  — not as a primary source or for peak intensity.
- **NBM updates hourly**, so its end-time resolution is one hour; RRFS native remains the
  15-minute-resolution primary timeline.
- In today's event both models showed continued light rain after 19:00 (RRFS steps through
  19:45 at 0.03–0.05 in/step; NBM [20:00] = 0.11 in) while KJRB observed only traces — both
  sources can over-predict the tail of a convective event, so treat "NBM says it continues" as a
  soft flag, not an override.
