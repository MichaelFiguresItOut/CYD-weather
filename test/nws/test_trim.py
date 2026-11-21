"""Host-side check of the nws_station.cpp trimFeatures() scanner logic,
run against the real NWS station-list response captured in test/nws/.
The Python below mirrors the C++ character scanner 1:1."""
import json

body = open('test/nws/stations.json', 'rb').read().decode('utf-8')

def trim(b, n):
    fs = b.find('"features":')
    assert fs >= 0, 'no features key'
    p = fs + len('"features":')
    while p < len(b) and b[p] in ' \t':
        p += 1
    assert b[p] == '[', 'features not an array'
    p += 1
    depth, in_str, esc, kept, cut, natural = 0, False, False, 0, 0, False
    L = len(b)
    i = p
    while i < L:
        c = b[i]
        if in_str:
            if esc:
                esc = False
            elif c == chr(92):      # backslash
                esc = True
            elif c == '"':
                in_str = False
            i += 1
            continue
        if c == '"':
            in_str = True
        elif c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                kept += 1
                cut = i + 1
                if kept >= n:
                    break
        elif c == ']' and depth == 0:
            cut = i + 1
            natural = True
            break
        i += 1
    if cut == 0:    # C++: returns 0, body unchanged
        return b, 0
    t = b[:cut] + ('}' if natural else ']}' )
    return t, kept

def run():
    for n in (1, 4, 15, 40):
        t, kept = trim(body, n)
        doc = json.loads(t)                       # raises on invalid JSON
        ids = [f['properties']['stationIdentifier'] for f in doc['features']]
        print(f'full  n={n:>2}: kept={kept:<2} json=OK features={len(ids):<2} first3={ids[:3]}')

    # Capped-download simulation: the firmware stops reading at
    # NWS_LIST_BODY_CAP (24576) or a partial first feature (2048 -> none).
    for cap in (24576, 8192):
        capped = body[:cap]
        t, kept = trim(capped, 15)
        doc = json.loads(t)
        print(f'cap={cap}: kept={kept:<2} json=OK features={len(doc["features"])}')
    assert trim(body[:2048], 15)[1] == 0, 'expected no complete feature in 2048 B'

    # Natural end (full body, huge n)
    t, kept = trim(body, 1000)
    doc = json.loads(t)
    print(f'natural end: kept={kept} features={len(doc["features"])}')
    print('ALL OK')


if __name__ == '__main__':
    run()
