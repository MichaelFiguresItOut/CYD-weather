"""Host-side check of the nws_station.cpp Phase 4 fallback logic:
backup-list building (nwsBuildFallbackList), observation usability
(nwsObsUsable), and resolution (nwsFindUsable). The Python below mirrors
the C++ 1:1 and runs against the real NWS station-list response captured
in test/nws/ (40 stations, NWS order = nearest first) plus synthetic
edge cases (CoCoRaHS, >100 km, duplicates, stale/failed observations).
The capture is the Chicago (60613) station list: 37 stations, NWS order
= nearest first.
"""
import json

NWS_BACKUP_MAX = 3
NWS_BACKUP_MAX_KM = 100.0
NWS_OBS_FRESH_MAX_S = 90 * 60

def suitable(st):
    # C++: st && st->id[0] && !strstr(upper(provider), "COCORAH")
    return bool(st['id']) and 'COCORAH' not in st['provider'].upper()

def build_fallback(cands, preferred_id, max_backups):
    # C++: nwsBuildFallbackList(cands, n, preferredId, out, maxBackups)
    if max_backups < 0:
        return []
    if max_backups > NWS_BACKUP_MAX:
        max_backups = NWS_BACKUP_MAX
    out = []
    for c in cands:
        if len(out) >= max_backups:
            break
        if not suitable(c):
            continue
        if c['distKm'] > NWS_BACKUP_MAX_KM:
            continue
        if preferred_id and c['id'] == preferred_id:
            continue
        if any(o['id'] == c['id'] for o in out):
            continue
        out.append(dict(c))
    return out

def obs_usable(st):
    # C++: hasObs && obsAgeSec >= 0 && obsAgeSec <= NWS_OBS_FRESH_MAX_S
    return bool(st['hasObs']) and 0 <= st['obsAgeSec'] <= NWS_OBS_FRESH_MAX_S

def find_usable(lst):
    # C++: nwsFindUsable(list, n) -> first usable index, else -1
    for i, st in enumerate(lst):
        if obs_usable(st):
            return i
    return -1

def mkstn(id, dist_km, provider='ASOS', name=None, has_obs=False, age=-1):
    return {'id': id, 'name': name or id, 'provider': provider,
            'distKm': dist_km, 'hasObs': has_obs, 'obsFail': not has_obs,
            'obsAgeSec': age}

def from_json(path):
    doc = json.loads(open(path, 'rb').read().decode('utf-8'))
    out = []
    for f in doc['features']:
        p = f['properties']
        # distance is properties.distance.value (meters), like C++ parseStationsInto
        out.append(mkstn(p['stationIdentifier'],
                         p.get('distance', {}).get('value', 0.0) / 1000.0,
                         p.get('provider', ''), p['name']))
    return out

def run():
    stn = from_json('test/nws/stations.json')
    assert len(stn) == 37, f'expected 37 captured stations, got {len(stn)}'
    providers = {}
    for s in stn:
        providers[s['provider']] = providers.get(s['provider'], 0) + 1
    print(f'captured list: {len(stn)} stations, providers={providers}')

    # --- 1. Real list: every one of the 5 nearest stations as preferred.
    # Expect 3 backups each, strictly in nearest-first order, never the
    # pick itself, and only suitable providers.
    for pref in stn[:5]:
        bk = build_fallback(stn, pref['id'], NWS_BACKUP_MAX)
        ids = [b['id'] for b in bk]
        assert len(bk) == 3, f'{pref["id"]}: expected 3 backups, got {len(bk)}: {ids}'
        assert pref['id'] not in ids, f'{pref["id"]} picked as own backup'
        dists = [b['distKm'] for b in bk]
        assert dists == sorted(dists), f'{pref["id"]}: not nearest-first: {dists}'
        assert all(suitable(b) for b in bk), f'{pref["id"]}: unsuitable backup'
        # must match the nearest-3 of the filtered list exactly
        filtered = [s for s in stn if s['id'] != pref['id']
                    and suitable(s) and s['distKm'] <= NWS_BACKUP_MAX_KM]
        assert ids == [s['id'] for s in filtered[:3]], \
            f'{pref["id"]}: {ids} != {filtered[:3]}'
        dists = ", ".join(f'{b["id"]} {b["distKm"]:.1f} km' for b in bk)
        print(f'preferred {pref["id"]:<6} ({pref["distKm"]:>5.1f} km): '
              f'backups {ids}  ({dists})')

    # --- 2. Distance cap: 99.9 km qualifies, 100.1/150 km do not.
    cands = [mkstn('KP1', 1.0), mkstn('KP2', 5.0), mkstn('KP3', 99.9),
             mkstn('KP4', 100.1), mkstn('KP5', 150.0)]
    bk = build_fallback(cands, 'KP1', NWS_BACKUP_MAX)
    assert [b['id'] for b in bk] == ['KP2', 'KP3'], \
        f'distance cap wrong: {[b["id"] for b in bk]}'
    print(f'distance cap: within 100 km kept, beyond dropped: '
          f'{[b["id"] for b in bk]}')

    # --- 3. Sparse area: only one other station within the cap.
    cands = [mkstn('KP1', 1.0), mkstn('KP2', 40.0), mkstn('KP3', 200.0)]
    bk = build_fallback(cands, 'KP1', NWS_BACKUP_MAX)
    assert [b['id'] for b in bk] == ['KP2'], \
        f'sparse area: {[b["id"] for b in bk]}'
    print(f'sparse area: 1 backup where only 1 qualifies: {[b["id"] for b in bk]}')

    # --- 4. CoCoRaHS (any case) is skipped, even when nearest.
    cands = [mkstn('KP1', 1.0), mkstn('CC1', 2.0, 'cocorahs'),
             mkstn('CC2', 3.0, 'CoCoRaHS'), mkstn('KP2', 4.0), mkstn('KP3', 5.0)]
    bk = build_fallback(cands, 'KP1', NWS_BACKUP_MAX)
    assert [b['id'] for b in bk] == ['KP2', 'KP3'], \
        f'CoCoRaHS not skipped: {[b["id"] for b in bk]}'
    print(f'CoCoRaHS skipped (both case variants): {[b["id"] for b in bk]}')

    # --- 5. Duplicate ids (defensive; NWS does not duplicate) -> once.
    cands = [mkstn('KP1', 1.0), mkstn('KP2', 2.0), mkstn('KP2', 2.5), mkstn('KP3', 3.0)]
    bk = build_fallback(cands, 'KP1', NWS_BACKUP_MAX)
    assert [b['id'] for b in bk] == ['KP2', 'KP3'], \
        f'duplicate kept twice: {[b["id"] for b in bk]}'
    print(f'duplicate ids: kept once: {[b["id"] for b in bk]}')

    # --- 6. Empty candidate set beyond the pick -> zero backups (fine).
    bk = build_fallback([mkstn('KP1', 1.0)], 'KP1', NWS_BACKUP_MAX)
    assert bk == [], f'expected no backups, got {bk}'
    print('pick alone in the list: 0 backups (selection still valid)')

    # --- 7. maxBackups clamping.
    assert len(build_fallback(stn, 'KMDW', 5)) == NWS_BACKUP_MAX
    assert [b['id'] for b in build_fallback(stn, 'KMDW', 1)] == ['KGYY']
    assert build_fallback(stn, 'KMDW', 0) == []
    print(f'maxBackups clamped to {NWS_BACKUP_MAX}; 0 and 1 honored')

    # --- 8. Observation usability + resolution (freshness < 90 min).
    M = 60
    cases = [
        # (label, [stations], expected index, note)
        ('preferred fresh wins',
         [mkstn('A', 1, has_obs=True, age=1200),
          mkstn('B', 2, has_obs=True, age=60 * M)], 0, 'fresh preferred, stale backup'),
        ('stale preferred -> backup 1',
         [mkstn('A', 1, has_obs=True, age=100 * M),
          mkstn('B', 2, has_obs=True, age=5 * M)], 1, '100 min > 90 min limit'),
        ('failed preferred -> backup 2',
         [mkstn('A', 1, has_obs=False, age=-1),
          mkstn('B', 2, has_obs=True, age=100 * M),
          mkstn('C', 3, has_obs=True, age=30 * M)], 2, 'fetch failure never usable'),
        ('unknown age (no NTP) -> backup',
         [mkstn('A', 1, has_obs=True, age=-1),
          mkstn('B', 2, has_obs=True, age=9 * M)], 1, 'age -1 never usable'),
        ('all stale -> Open-Meteo',
         [mkstn('A', 1, has_obs=True, age=100 * M),
          mkstn('B', 2, has_obs=False, age=-1)], -1, '-1 = use Open-Meteo'),
        ('boundary: exactly 90 min usable',
         [mkstn('A', 1, has_obs=True, age=90 * M)], 0, '5400 s is inside the limit'),
        ('boundary: 90 min + 1 s not usable',
         [mkstn('A', 1, has_obs=True, age=90 * M + 1)], -1, '5401 s is outside'),
    ]
    for label, lst, want, note in cases:
        got = find_usable(lst)
        assert got == want, f'{label}: expected {want}, got {got}'
        print(f'usability: {label:<34} -> index {got}  ({note})')

    # Fallback-ordered list = [preferred, backups...], the exact shape
    # Phase 5 will resolve: build it from the captured list for KMDW.
    preferred = [s for s in stn if s['id'] == 'KMDW'][0]
    ordered = [preferred] + build_fallback(stn, 'KMDW', NWS_BACKUP_MAX)
    print(f'KMDW fallback order: {[s["id"] for s in ordered]} '
          f'(preferred first, then nearest backups)')
    print('ALL OK')


if __name__ == '__main__':
    run()
