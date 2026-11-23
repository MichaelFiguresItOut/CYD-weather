# Host-side check of the nws_station.cpp Phase 6.5 ranking logic:
# haversine distance, nearest-first preparation (nwsRankPrepare), and the
# distance/freshness quality ranking (nwsRankStations). The Python below
# mirrors the C++ 1:1 and runs synthetic cases (incl. the spec example)
# plus the real NWS station-list response captured in test/nws/.
import json
import math

# --- constants, mirrored from nws_station.h -----------------------------
NWS_OBS_FRESH_MAX_S = 90 * 60
NWS_RANK_CHECK_MAX = 8
NWS_RANK_RADIUS_MAX_KM = 100.0
NWS_RANK_COMPETE_KM = 10.0
NWS_RANK_MIN_NEWER_S = 20 * 60
NWS_BACKUP_MAX = 3

# --- mirrors of nws_station.cpp ------------------------------------------

def suitable(st):
    # C++: st && st->id[0] && !strstr(upper(provider), "COCORAH")
    return bool(st['id']) and 'COCORAH' not in st['provider'].upper()

def obs_usable(st):
    # C++: hasObs && obsAgeSec >= 0 && obsAgeSec <= NWS_OBS_FRESH_MAX_S
    return bool(st['hasObs']) and 0 <= st['obsAgeSec'] <= NWS_OBS_FRESH_MAX_S

def haversine_km(lat1, lon1, lat2, lon2):
    # C++: nwsHaversineKm
    R = 6371.0088
    dlat = math.radians(lat2 - lat1)
    dlon = math.radians(lon2 - lon1)
    a = (math.sin(dlat / 2.0) ** 2 +
         math.cos(math.radians(lat1)) * math.cos(math.radians(lat2)) *
         math.sin(dlon / 2.0) ** 2)
    return 2.0 * R * math.asin(min(1.0, math.sqrt(a)))

def rank_prepare(sts, lat, lon):
    # C++: nwsRankPrepare - replace distKm with haversine from (lat, lon),
    # reorder nearest-first (insertion sort == stable full sort here).
    for s in sts:
        s['distKm'] = haversine_km(lat, lon, s['lat'], s['lon'])
    return sorted(sts, key=lambda s: s['distKm'])

def rank_stations(cands, ntp_ok, exclude_id=None, max_out=NWS_BACKUP_MAX + 1):
    # C++: nwsRankStations(cands, nCands, ntpOK, excludeId, out, maxOut)
    if max_out < 1 or not cands:
        return []
    rem = [c for c in cands
           if suitable(c) and not (exclude_id and c['id'] == exclude_id)]
    out = []
    while len(out) < max_out and rem:
        best = None
        if ntp_ok:
            anchor = next((c for c in rem if obs_usable(c)), None)
            if anchor is not None:
                best = anchor
                win = anchor['distKm'] + NWS_RANK_COMPETE_KM
                fresh = min(c['obsAgeSec'] for c in rem
                            if obs_usable(c) and c['distKm'] <= win)
                if anchor['obsAgeSec'] - fresh >= NWS_RANK_MIN_NEWER_S:
                    best = next(c for c in rem
                                if obs_usable(c) and c['distKm'] <= win
                                and c['obsAgeSec'] == fresh)
        if best is None:
            best = rem[0]   # no usable (or no NTP): nearest remaining
        out.append(best)
        rem.remove(best)
    return out

def rank_check_set(sts):
    # C++ (settings.cpp): stnRankCheckSet - the NWS_RANK_CHECK_MAX nearest
    # suitable stations within NWS_RANK_RADIUS_MAX_KM (sts nearest-first;
    # equivalent to a 10 km radius grown in 10 km steps to 100 km).
    idx = []
    for i, s in enumerate(sts):
        if not suitable(s):
            continue
        if s['distKm'] > NWS_RANK_RADIUS_MAX_KM:
            break
        if len(idx) >= NWS_RANK_CHECK_MAX:
            break
        idx.append(i)
    return idx

# --- helpers -------------------------------------------------------------

def mkstn(id, dist_km, provider='ASOS', name=None, has_obs=False, age=-1,
          lat=40.0, lon=-73.0):
    return {'id': id, 'name': name or id, 'provider': provider,
            'distKm': dist_km, 'lat': lat, 'lon': lon,
            'hasObs': has_obs, 'obsFail': not has_obs, 'obsAgeSec': age}

def from_json(path):
    # geometry coordinates [lon, lat] + NWS distance (m) + provider,
    # like C++ parseStationsInto (plus lat/lon for haversine checks).
    doc = json.loads(open(path, 'rb').read().decode('utf-8'))
    out = []
    for f in doc['features']:
        p = f['properties']
        lon, lat = f['geometry']['coordinates']
        out.append(mkstn(p['stationIdentifier'],
                         p.get('distance', {}).get('value', 0.0) / 1000.0,
                         p.get('provider', ''), p['name'],
                         lat=lat, lon=lon))
    return out

def ids(sel):
    return [s['id'] for s in sel]

def run():
    # --- 1. The Phase 6.5 spec example (the must-pass case).
    # A(8.6 km, 47 min), B(13.3 km, 18 min), C(13.7 km, 47 min),
    # D(16.5 km, 47 min)  ->  B, A, C, D.
    # B is 4.7 km farther than A (<= 10 km) and 29 min fresher (>= 20 min)
    # -> preferred. A is the nearest remaining; C and D follow distance.
    M = 60
    A = mkstn('A', 8.6, has_obs=True, age=47 * M)
    B = mkstn('B', 13.3, has_obs=True, age=18 * M)
    C = mkstn('C', 13.7, has_obs=True, age=47 * M)
    D = mkstn('D', 16.5, has_obs=True, age=47 * M)
    sel = rank_stations([A, B, C, D], ntp_ok=True)
    assert ids(sel) == ['B', 'A', 'C', 'D'], f'spec example: {ids(sel)}'
    print(f'spec example: preferred B, order {ids(sel)}')

    # --- 2. Replacement needs >= 20 min fresher.
    # 47 - 28 = 19 min < 20 -> keep the nearer anchor.
    sel = rank_stations([mkstn('A', 8.6, has_obs=True, age=47 * M),
                         mkstn('B', 13.3, has_obs=True, age=28 * M)],
                        ntp_ok=True)
    assert ids(sel) == ['A', 'B'], f'19 min not enough: {ids(sel)}'
    # 47 - 27 = 20 min, exactly the threshold -> replace.
    sel = rank_stations([mkstn('A', 8.6, has_obs=True, age=47 * M),
                         mkstn('B', 13.3, has_obs=True, age=27 * M)],
                        ntp_ok=True)
    assert ids(sel) == ['B', 'A'], f'20 min boundary: {ids(sel)}'
    print('freshness threshold: 19 min keeps anchor, exactly 20 min replaces')

    # --- 3. Replacement needs <= 10 km farther.
    # 18.7 - 8.6 = 10.1 km > 10 -> keep the anchor even at 5 min old.
    sel = rank_stations([mkstn('A', 8.6, has_obs=True, age=47 * M),
                         mkstn('B', 18.7, has_obs=True, age=5 * M)],
                        ntp_ok=True)
    assert ids(sel) == ['A', 'B'], f'10.1 km too far: {ids(sel)}'
    # 18.6 - 8.6 = 10.0 km, exactly the cap -> replace.
    sel = rank_stations([mkstn('A', 8.6, has_obs=True, age=47 * M),
                         mkstn('B', 18.6, has_obs=True, age=5 * M)],
                        ntp_ok=True)
    assert ids(sel) == ['B', 'A'], f'10 km boundary: {ids(sel)}'
    print('distance cap: 10.1 km keeps anchor, exactly 10 km replaces')

    # --- 4. Stale / failed observations are never excluded, just ranked
    # after all usable ones (temporary API failure costs one cycle only).
    sel = rank_stations([mkstn('A', 5.0, has_obs=True, age=91 * M),   # stale
                         mkstn('B', 13.0, has_obs=True, age=10 * M)],
                        ntp_ok=True)
    assert ids(sel) == ['B', 'A'], f'stale ranked last: {ids(sel)}'
    sel = rank_stations([mkstn('A', 5.0, has_obs=False),              # failed
                         mkstn('B', 8.0, has_obs=True, age=30 * M)],
                        ntp_ok=True)
    assert ids(sel) == ['B', 'A'], f'failed ranked last: {ids(sel)}'
    print('stale and failed observations ranked after usable, never excluded')

    # --- 5. No NTP: no reliable age -> plain nearest-first, unchanged.
    sel = rank_stations([mkstn('A', 8.6, has_obs=True, age=47 * M),
                         mkstn('B', 13.3, has_obs=True, age=18 * M)],
                        ntp_ok=False)
    assert ids(sel) == ['A', 'B'], f'no NTP: {ids(sel)}'
    # ...even when the observations WERE fetched (ages known but NTP was
    # not synced at fetch time, so obsAgeSec may be -1; no comparisons).
    sel = rank_stations([mkstn('A', 8.6, has_obs=True, age=-1),
                         mkstn('B', 13.3, has_obs=True, age=-1)],
                        ntp_ok=False)
    assert ids(sel) == ['A', 'B'], f'no NTP, unknown ages: {ids(sel)}'
    print('without NTP: plain nearest-first')

    # --- 6. All observations unusable -> nearest-first (no unreliable
    # freshness comparison).
    sel = rank_stations([mkstn('A', 5.0, has_obs=True, age=91 * M),
                         mkstn('B', 13.0, has_obs=False),
                         mkstn('C', 20.0, has_obs=True, age=120 * M)],
                        ntp_ok=True)
    assert ids(sel) == ['A', 'B', 'C'], f'all unusable: {ids(sel)}'
    print('all unusable: nearest-first fallback')

    # --- 7. CoCoRaHS never ranked, even when nearest and freshest.
    sel = rank_stations([mkstn('CC', 1.0, provider='CoCoRaHS',
                               has_obs=True, age=2 * M),
                         mkstn('A', 8.6, has_obs=True, age=47 * M),
                         mkstn('B', 13.3, has_obs=True, age=18 * M)],
                        ntp_ok=True)
    assert ids(sel) == ['B', 'A'], f'CoCoRaHS ranked: {ids(sel)}'
    print('CoCoRaHS excluded from ranking')

    # --- 8. excludeId (manual tap: backups from the same check set,
    # never the pick itself).
    sel = rank_stations([A, B, C, D], ntp_ok=True, exclude_id='B')
    assert ids(sel) == ['A', 'C', 'D'], f'excludeId: {ids(sel)}'
    print(f'excludeId=B: {ids(sel)}')

    # --- 9. Repeated rounds re-anchor (backups are not simply the
    # remaining nearest-first list).
    sel = rank_stations([mkstn('A', 5.0, has_obs=True, age=50 * M),
                         mkstn('B', 14.0, has_obs=True, age=10 * M),
                         mkstn('C', 15.0, has_obs=True, age=40 * M),
                         mkstn('D', 24.0, has_obs=True, age=5 * M)],
                        ntp_ok=True)
    # round 1: anchor A(50m), window <= 15 km {A,B,C}, freshest B(10m),
    #   40 min newer -> B.
    # round 2: remaining A(5,50) C(15,40) D(24,5): anchor A, window
    #   {A,C}, freshest C(40m), only 10 min newer -> A.
    # round 3: C(15,40) D(24,5): anchor C, window {C,D}, D 35 min newer -> D.
    # round 4: C.
    assert ids(sel) == ['B', 'A', 'D', 'C'], f're-anchoring: {ids(sel)}'
    print(f'repeated rounds re-anchor: {ids(sel)}')

    # --- 10. Equal ages never swap: the nearer station always stays ahead.
    sel = rank_stations([mkstn('A', 5.0, has_obs=True, age=20 * M),
                         mkstn('B', 12.0, has_obs=True, age=20 * M)],
                        ntp_ok=True)
    assert ids(sel) == ['A', 'B'], f'equal ages: {ids(sel)}'
    # Two equal-freshest in the window: the nearer of them wins the slot.
    sel = rank_stations([mkstn('A', 5.0, has_obs=True, age=50 * M),
                         mkstn('B', 9.0, has_obs=True, age=10 * M),
                         mkstn('C', 13.0, has_obs=True, age=10 * M)],
                        ntp_ok=True)
    assert ids(sel) == ['B', 'C', 'A'], f'freshest tie -> nearer: {ids(sel)}'
    print('equal ages keep the nearer station; freshest tie -> nearer')

    # --- 11. maxOut honored (1 = preferred only, 0 = none, clamped).
    assert ids(rank_stations([A, B, C, D], ntp_ok=True, max_out=1)) == ['B']
    assert rank_stations([A, B, C, D], ntp_ok=True, max_out=0) == []
    print('maxOut honored (1 = preferred only)')

    # --- 12. nwsRankPrepare: distances recomputed + nearest-first.
    base_lat, base_lon = 40.75, -73.97
    far = mkstn('F', 999.0, lat=base_lat + 0.5, lon=base_lon)      # ~55 km
    near = mkstn('N', 999.0, lat=base_lat, lon=base_lon + 0.01)    # ~0.8 km
    mid = mkstn('M', 999.0, lat=base_lat + 0.1, lon=base_lon)      # ~11 km
    prep = rank_prepare([far, near, mid], base_lat, base_lon)
    assert ids(prep) == ['N', 'M', 'F'], f'prepare order: {ids(prep)}'
    assert prep[0]['distKm'] < prep[1]['distKm'] < prep[2]['distKm']
    assert abs(prep[0]['distKm'] - 0.87) < 0.05, prep[0]['distKm']
    print(f'prepare: recomputed distances {["%.1f" % s["distKm"] for s in prep]} km')

    # --- 13. Check set: 8 nearest suitable within 100 km.
    lst = [mkstn(f'K{i}', 10.0 * i) for i in range(1, 13)]         # 10..120 km
    assert rank_check_set(lst) == list(range(8)), '12 in range -> first 8'
    sparse = [mkstn('S1', 120.0), mkstn('S2', 150.0)]
    assert rank_check_set(sparse) == [], 'nothing within 100 km -> empty'
    coco = [mkstn('CC', 5.0, provider='cocorahs')] + [mkstn(f'K{i}', 20.0 * i)
                                                      for i in range(1, 6)]
    # CoCoRaHS not counted, 5 ASOS within 100 km -> 5 in the set.
    assert len(rank_check_set(coco)) == 5
    print(f'check set: 8-of-12 kept, sparse -> empty, CoCoRaHS not counted')

    # --- 14. Real captured list (NYC area, 40 stations, NWS order).
    stn = from_json('test/nws/stations.json')
    assert len(stn) == 40, f'expected 40 captured stations, got {len(stn)}'

    # 14a. Haversine vs the NWS-provided distance (NWS uses a geodesic;
    # spherical haversine agrees to well under 1 km here).
    worst = max(abs(s['distKm'] - haversine_km(40.7517, -73.9707, s['lat'], s['lon']))
                for s in stn)
    assert worst < 0.6, f'haversine deviates {worst:.2f} km from NWS'
    print(f'haversine vs NWS distance on the capture: max deviation {worst:.2f} km')

    # 14b. All usable, equal age -> the 4 nearest suitable, plain order.
    eq = [dict(s, hasObs=True, obsFail=False, obsAgeSec=30 * M) for s in stn]
    eq.sort(key=lambda s: s['distKm'])
    sel = rank_stations(eq[:NWS_RANK_CHECK_MAX], ntp_ok=True)
    expect = [s['id'] for s in eq[:4]]
    assert ids(sel) == expect, f'capture equal age: {ids(sel)} != {expect}'
    print(f'capture, all equal age: {ids(sel)} (4 nearest)')

    # 14c. Nearest station stale (91 min), rest fresh (30 min): the stale
    # one is skipped by the usable rounds and ranked after all usable -
    # with 8 candidates and maxOut 4 it never makes it in.
    real = [dict(s, hasObs=True, obsFail=False, obsAgeSec=30 * M) for s in stn]
    real.sort(key=lambda s: s['distKm'])
    real[0]['obsAgeSec'] = 91 * M    # stale, just over the 90 min limit
    sel = rank_stations(real[:NWS_RANK_CHECK_MAX], ntp_ok=True)
    expect = [s['id'] for s in real[1:5]]
    assert ids(sel) == expect, f'capture stale #1: {ids(sel)} != {expect}'
    print(f'capture, nearest stale: {ids(sel)} (stale {real[0]["id"]} dropped)')

    # 14d. Manual-tap shape: backups = same ranking excluding the pick.
    pick = real[0]['id']
    sel = rank_stations(real[:NWS_RANK_CHECK_MAX], ntp_ok=True,
                        exclude_id=pick, max_out=NWS_BACKUP_MAX)
    assert len(sel) == 3 and pick not in ids(sel), f'tap backups: {ids(sel)}'
    print(f'tap {pick}: backups {ids(sel)}')

    print('ALL OK')


if __name__ == '__main__':
    run()
