#!/usr/bin/env python3
"""Phase 5 host-side tests: NWS feels-like + dashboard source acceptance.

Mirrors 1:1 the pure logic added to src/nws_station.cpp (Phase 5):
  nwsWindChillF / nwsHeatIndexF / nwsFeelsLikeF, the unit-aware obs value
  parsing (nwsObsNum / nwsObsValueF2C / nwsObsWindMph), the acceptance rule
  (nwsObsUsable + nwsObsHasValidTemp), and the preferred->backup resolution
  order used by fetchWeather().

Formula references (NWS-published):
  wind chill   T <= 50F, wind >= 3 mph:
               35.74 + 0.6215T - 35.75V^0.16 + 0.4275TV^0.16
  heat index   T >= 80F, RH >= 40%: Rothfusz (2005) regression + the two
               published adjustments: low-humidity (RH < 13%, 80F <= T <= 112F)
               and high-humidity (RH > 85%, 80F <= T <= 87F).
Reference values below were computed in Python from those published
formulas; they pin the firmware port, not just re-derive it.

Run:  python test/nws/test_feels.py
"""

import math

NAN = float("nan")

NWS_OBS_FRESH_MAX_S = 90 * 60   # nws_station.h


# ---------------------------------------------------------------------------
#  Mirrors of the firmware logic (src/nws_station.cpp)
# ---------------------------------------------------------------------------

def nws_obs_usable(st):
    return st["hasObs"] and st["obsAgeSec"] >= 0 and st["obsAgeSec"] <= NWS_OBS_FRESH_MAX_S


def nws_obs_has_valid_temp(st):
    return st["hasObs"] and not math.isnan(st["obsTempC"])


def nws_obs_acceptable(st):
    """Phase 5 dashboard acceptance: usable AND valid temperature."""
    return nws_obs_usable(st) and nws_obs_has_valid_temp(st)


def nws_resolve_index(stations):
    """fetchWeather() resolution: first acceptable station, or -1."""
    for i, st in enumerate(stations):
        if nws_obs_acceptable(st):
            return i
    return -1


def nws_obs_num(obj):
    v = obj.get("value")
    return NAN if v is None else float(v)


def nws_obs_value_f2c(obj):
    v = nws_obs_num(obj)
    if math.isnan(v):
        return v
    u = obj.get("unitCode")
    if u and "degF" in u:
        v = (v - 32.0) * 5.0 / 9.0
    return v


def nws_obs_wind_mph(obj):
    v = nws_obs_num(obj)
    if math.isnan(v):
        return v
    u = obj.get("unitCode")
    if u and "km_h" in u:
        v /= 1.609344
    elif u is None or "mi_h" not in u:
        return NAN
    return v


def nws_wind_chill_f(temp_f, wind_mph):
    v = wind_mph ** 0.16
    return 35.74 + 0.6215 * temp_f - 35.75 * v + 0.4275 * temp_f * v


def nws_heat_index_raw(temp_f, rh):
    """Rothfusz (2005) regression only - no NWS adjustments."""
    return (-42.379
            + 2.04901523 * temp_f
            + 10.14333127 * rh
            - 0.22475541 * temp_f * rh
            - 6.83783e-3 * temp_f * temp_f
            - 5.481717e-2 * rh * rh
            + 1.22874e-3 * temp_f * temp_f * rh
            + 8.5282e-4 * temp_f * rh * rh
            - 1.99e-6 * temp_f * temp_f * rh * rh)


def nws_heat_index_f(temp_f, rh):
    hi = nws_heat_index_raw(temp_f, rh)
    # Published low-humidity adjustment: RH < 13%, 80F <= T <= 112F.
    # (The 14 inside the sqrt is part of the formula, not the threshold.)
    if rh < 13.0 and 80.0 <= temp_f <= 112.0:
        hi += math.sqrt((14.0 - rh) / 4.0) * math.sqrt((170.0 - temp_f) / 17.0)
    # Published high-humidity adjustment: RH > 85%, 80F <= T <= 87F.
    if rh > 85.0 and 80.0 <= temp_f <= 87.0:
        hi += ((rh - 85.0) / 10.0) * ((87.0 - temp_f) / 5.0)
    return hi


def nws_feels_like_f(temp_f, wind_mph, rh):
    if math.isnan(temp_f):
        return temp_f
    if not math.isnan(wind_mph) and temp_f <= 50.0 and wind_mph >= 3.0:
        return nws_wind_chill_f(temp_f, wind_mph)
    if not math.isnan(rh) and temp_f >= 80.0 and rh >= 40.0:
        return nws_heat_index_f(temp_f, rh)
    return temp_f


def c_to_f(c):
    """The existing firmware C->F convention (settings.cpp / weather.cpp)."""
    return c * 9.0 / 5.0 + 32.0


# ---------------------------------------------------------------------------
#  Tests
# ---------------------------------------------------------------------------

def make_st(age, temp_c, has_obs=True):
    return {"hasObs": has_obs, "obsAgeSec": age, "obsTempC": temp_c}


def approx(a, b, tol=0.011):
    if math.isnan(a) or math.isnan(b):
        return math.isnan(a) and math.isnan(b)
    return abs(a - b) <= tol


def test_wind_chill_formula():
    # Pinned from the published NWS wind-chill formula.
    for t, v, want in [(30, 10, 21.25), (50, 3, 49.68), (40, 15, 31.84),
                       (20, 15, 6.22), (45, 5, 42.35)]:
        got = nws_wind_chill_f(t, v)
        assert approx(got, want), f"WC({t},{v}) = {got}, want {want}"


def test_heat_index_formula():
    # Pinned from the published NWS heat-index (Rothfusz) formula,
    # including the low-humidity adjustment case (85F, 10%).
    for t, rh, want in [(90, 50, 94.60), (100, 40, 109.26), (85, 10, 84.12),
                        (95, 30, 94.41), (80, 40, 79.93), (110, 40, 135.66)]:
        got = nws_heat_index_f(t, rh)
        assert approx(got, want), f"HI({t},{rh}) = {got}, want {want}"
    # Low-humidity adjustment applies: HI(85,10) > raw regression(85,10).
    assert nws_heat_index_f(85, 10) > nws_heat_index_raw(85, 10)
    # ... but not outside the published 80-112F band.
    assert approx(nws_heat_index_f(115.0, 10), nws_heat_index_raw(115.0, 10)), \
        "low-RH adjustment must not apply at 115F"


def test_feels_like_dispatch():
    f = nws_feels_like_f
    # Wind chill (cold + wind)
    assert approx(f(30, 10, 50), 21.25)
    assert approx(f(50, 3, 90), 49.68)          # boundary: both limits apply
    # Cold but wind below 3 mph -> air temperature
    assert f(40, 2, 50) == 40
    # Cold with missing wind -> air temperature
    assert f(40, NAN, 50) == 40
    # Above 50F -> never wind chill, even with strong wind
    assert f(51, 30, 90) == 51
    # Heat index (hot + humid enough)
    assert approx(f(90, NAN, 50), 94.60)
    assert approx(f(90, 10, 50), 94.60)         # wind irrelevant for heat index
    assert approx(f(80, 5, 40), 79.93)          # boundary applies (may be < air temp)
    assert f(85, 10, 10) == 85                  # hot but RH < 40% -> air temperature
                                               # (the low-RH adjustment lives in
                                               # nws_heat_index_f itself, tested above)
    assert approx(f(115, NAN, 40), nws_heat_index_f(115, 40))
    # Below 80F -> never heat index, even with high RH
    assert f(79, 5, 90) == 79
    # 80F but RH below 40% -> air temperature
    assert f(90, 5, 39) == 90
    # Hot with missing RH -> air temperature
    assert f(95, 10, NAN) == 95
    # Mild mid-range -> air temperature
    assert f(65, 10, 50) == 65
    # NaN temperature passes through (caller should not use such an obs)
    assert math.isnan(f(NAN, 10, 50))


def test_low_rh_adjustment_boundaries():
    # Published condition: RH < 13% (strictly), 80F <= T <= 112F (inclusive).
    raw = nws_heat_index_raw
    # Applies inside the band.
    t, rh = 85.0, 12.0
    adj = math.sqrt((14.0 - rh) / 4.0) * math.sqrt((170.0 - t) / 17.0)
    assert approx(nws_heat_index_f(t, rh), raw(t, rh) + adj)
    # RH boundary: 12.999 applies, 13.0 and 13.1 must not.
    rh = 12.999
    assert approx(nws_heat_index_f(t, rh), raw(t, rh)
                  + math.sqrt((14.0 - rh) / 4.0) * math.sqrt((170.0 - t) / 17.0))
    for rh in (13.0, 13.1):
        assert approx(nws_heat_index_f(t, rh), raw(t, rh)), \
            f"low-RH adjustment must not apply at RH={rh}%"
    # T boundary: 80.0 and 112.0 apply, 79.9 and 112.1 must not.
    rh = 10.0
    for t in (80.0, 112.0):
        adj = math.sqrt((14.0 - rh) / 4.0) * math.sqrt((170.0 - t) / 17.0)
        assert approx(nws_heat_index_f(t, rh), raw(t, rh) + adj), \
            f"low-RH adjustment must apply at T={t}F"
    for t in (79.9, 112.1):
        assert approx(nws_heat_index_f(t, rh), raw(t, rh)), \
            f"low-RH adjustment must not apply at T={t}F"


def test_high_rh_adjustment_boundaries():
    # Published condition: RH > 85% (strictly), 80F <= T <= 87F (inclusive).
    raw = nws_heat_index_raw
    # Applies inside the band.
    t, rh = 84.0, 90.0
    adj = ((rh - 85.0) / 10.0) * ((87.0 - t) / 5.0)   # 0.5 * 0.6 = 0.3
    assert approx(adj, 0.3, 1e-9)
    assert approx(nws_heat_index_f(t, rh), raw(t, rh) + adj)
    # RH boundary: 85.1 applies, 85.0 and 84.9 must not.
    t = 84.0
    for rh in (85.1,):
        adj = ((rh - 85.0) / 10.0) * ((87.0 - t) / 5.0)
        assert approx(nws_heat_index_f(t, rh), raw(t, rh) + adj)
    for rh in (85.0, 84.9):
        assert approx(nws_heat_index_f(t, rh), raw(t, rh)), \
            f"high-RH adjustment must not apply at RH={rh}%"
    # T boundary: 80.0 and 86.9 apply, 79.9 and 87.1 must not.
    rh = 90.0
    for t in (80.0, 86.9):
        adj = ((rh - 85.0) / 10.0) * ((87.0 - t) / 5.0)
        assert approx(nws_heat_index_f(t, rh), raw(t, rh) + adj), \
            f"high-RH adjustment must apply at T={t}F"
    for t in (79.9, 87.1):
        assert approx(nws_heat_index_f(t, rh), raw(t, rh)), \
            f"high-RH adjustment must not apply at T={t}F"
    # The two adjustments' RH conditions are mutually exclusive, so they can
    # never stack: verify no interaction at a middle RH.
    assert approx(nws_heat_index_f(84.0, 50.0), raw(84.0, 50.0))


def test_unit_parsing():
    # Temperature: degF converted, degC kept, null -> NaN, no unitCode -> kept
    assert approx(nws_obs_value_f2c({"unitCode": "wmoUnit:degF", "value": 60}), 15.5556, 0.001)
    assert nws_obs_value_f2c({"unitCode": "wmoUnit:degC", "value": 15.6}) == 15.6
    assert math.isnan(nws_obs_value_f2c({"unitCode": "wmoUnit:degC", "value": None}))
    assert math.isnan(nws_obs_value_f2c({"value": None}))
    assert nws_obs_value_f2c({"value": 15.6}) == 15.6   # unrecognized -> assume C
    # Wind: km/h converted, mph kept, unknown/missing unit -> NaN
    assert approx(nws_obs_wind_mph({"unitCode": "wmoUnit:km_h-1", "value": 16}), 16 / 1.609344, 0.001)
    assert nws_obs_wind_mph({"unitCode": "wmoUnit:mi_h", "value": 10}) == 10
    assert math.isnan(nws_obs_wind_mph({"unitCode": "wmoUnit:kn", "value": 10}))
    assert math.isnan(nws_obs_wind_mph({"value": 10}))
    assert math.isnan(nws_obs_wind_mph({"unitCode": "wmoUnit:mi_h", "value": None}))
    # Generic numeric (relativeHumidity is always %)
    assert nws_obs_num({"value": 45.6}) == 45.6
    assert math.isnan(nws_obs_num({}))


def test_acceptance_and_resolution():
    # Fresh + valid temp -> acceptable; the fresh limit is inclusive.
    assert nws_obs_acceptable(make_st(0, 15.6))
    assert nws_obs_acceptable(make_st(NWS_OBS_FRESH_MAX_S, 15.6))
    # Stale -> not acceptable (same bar as Phase 4's nwsObsUsable).
    assert not nws_obs_acceptable(make_st(NWS_OBS_FRESH_MAX_S + 1, 15.6))
    # Fresh but temperature unreported -> not acceptable.
    assert not nws_obs_acceptable(make_st(300, NAN))
    # No NTP (age unknown) -> never acceptable.
    assert not nws_obs_acceptable(make_st(-1, 15.6))
    # Fetch failure (hasObs false) -> never acceptable.
    assert not nws_obs_acceptable(make_st(300, 15.6, has_obs=False))

    # Resolution order: preferred stale -> backup 1 missing temp -> backup 2 wins.
    cands = [make_st(6000, 15.0),             # preferred, stale
             make_st(300, NAN),               # backup 1, fresh, no temperature
             make_st(300, 15.6)]              # backup 2, fresh + valid
    assert nws_resolve_index(cands) == 2

    # All stale / all invalid -> -1 (Open-Meteo is the source).
    assert nws_resolve_index([make_st(99999, 15.0)]) == -1
    assert nws_resolve_index([make_st(300, NAN), make_st(-1, 15.0)]) == -1

    # Preferred wins when it is acceptable.
    assert nws_resolve_index([make_st(120, 15.0), make_st(300, 14.0)]) == 0


def test_dashboard_values_from_obs():
    # What fetchWeather() does once a station wins: C->F per the existing
    # convention, feels-like from the same observation, epoch = obs time.
    st = {"obsTempC": 15.6, "obsWindMph": 4.97, "obsRelHum": 45.6, "obsEpochSec": 1763703060}
    temp_f = c_to_f(st["obsTempC"])
    feels_f = nws_feels_like_f(temp_f, st["obsWindMph"], st["obsRelHum"])
    assert approx(temp_f, 60.08, 0.01)
    assert feels_f == temp_f          # mild: 50F < T < 80F -> air temperature
    # Cold + windy obs -> wind chill is what the dog outfit and screen see:
    # -1.1111 C is 30.0 F to float precision, so the pinned WC(30,10) applies.
    cold = {"obsTempC": -1.1111, "obsWindMph": 10.0, "obsRelHum": 50.0}
    tf = c_to_f(cold["obsTempC"])
    assert approx(tf, 30.0, 0.001)
    assert approx(nws_feels_like_f(tf, cold["obsWindMph"], cold["obsRelHum"]), 21.25)


if __name__ == "__main__":
    test_wind_chill_formula()
    test_heat_index_formula()
    test_low_rh_adjustment_boundaries()
    test_high_rh_adjustment_boundaries()
    test_feels_like_dispatch()
    test_unit_parsing()
    test_acceptance_and_resolution()
    test_dashboard_values_from_obs()
    print("All Phase 5 NWS feels-like tests passed.")
