"""End-to-end check of the nws_station.cpp nwsGetCapped() chunked decoder.

Pulls the REAL wire bytes from api.weather.gov (TLS, HTTP/1.1, no
chunked decoding by the client), then runs a 1:1 Python mirror of the
C++ decoder (same framing detection, same 24576-byte cap) and verifies
the result is valid JSON with the expected features.

Run: python test/nws/test_chunked.py
"""
import json
import socket
import ssl

URL = "https://api.weather.gov/gridpoints/LOT/77,69/stations"
CAP = 24576   # NWS_LIST_BODY_CAP


def raw_get():
    """Return the raw response bytes (headers + chunked body, undecoded)."""
    ctx = ssl.create_default_context()
    with socket.create_connection(("api.weather.gov", 443), timeout=20) as s:
        with ctx.wrap_socket(s, server_hostname="api.weather.gov") as ss:
            req = (b"GET /gridpoints/LOT/77,69/stations HTTP/1.1\r\n"
                   b"Host: api.weather.gov\r\n"
                   b"User-Agent: CYD-Weather/1.0 (personal ESP32 weather dashboard)\r\n"
                   b"Accept: application/geo+json\r\n"
                   b"Connection: close\r\n\r\n")
            ss.sendall(req)
            data = b""
            while True:
                try:
                    c = ss.recv(65536)
                except socket.timeout:
                    break
                if not c:
                    break
                data += c
    return data


def decoder(body, cap):
    """1:1 mirror of nwsGetCapped's body loop (post-header)."""
    out = bytearray()
    total = 0
    first = -1
    i = 0
    L = len(body)
    while i < L:  # the C++ 'for (;;)' with stream->read()
        c = body[i]
        i += 1
        if c not in (0x20, 0x09, 0x0D, 0x0A):
            first = c
            break
    plain = (first == 0x7B)  # '{'
    if plain:
        out.append(first)
        total += 1
        while total < cap:
            take = min(256, len(body) - i)
            if take <= 0:
                break  # C++: readBytes <= 0 -> break
            out += body[i:i + take]
            i += take
            total += take
        return bytes(out)

    def hexval(c):
        if 0x30 <= c <= 0x39: return c - 0x30
        if 0x61 <= c <= 0x66: return c - 0x61 + 10
        if 0x41 <= c <= 0x46: return c - 0x41 + 10
        return -1

    done = (first < 0)
    chunk_left = 0
    in_data = False
    while not done and total < cap:
        if not in_data:
            line = bytearray()
            if first >= 0:
                line.append(first)
                first = -1
            eol = False
            while len(line) < 31:
                if i >= L:
                    done = True
                    break
                c = body[i]
                i += 1
                line.append(c)
                if c == 0x0A:
                    eol = True
                    break
            if not eol:
                done = True
                break
            s = line.decode("ascii", "replace").rstrip("\r\n")
            s = s.split(";")[0]
            # strtoul(line, &endp, 16): parse leading hex, require the rest empty
            n = 0
            j = 0
            while j < len(s) and hexval(ord(s[j])) >= 0:
                n = (n << 4) | hexval(ord(s[j]))
                j += 1
            if j == 0 or j < len(s):
                done = True
                break
            if n == 0:
                break
            chunk_left = n
            in_data = True
            continue
        want = min(chunk_left, cap - total)
        got = 0
        while got < want:
            take = min(256, want - got, len(body) - i)
            if take <= 0:
                done = True
                break
            out += body[i:i + take]
            i += take
            got += take
            total += take
        if done:
            break
        chunk_left -= got
        if chunk_left == 0:
            i += 2  # swallow CRLF
            in_data = False
    return bytes(out)


def main():
    raw = raw_get()
    head, _, body = raw.partition(b"\r\n\r\n")
    headers = head.decode("latin1")
    assert "200" in headers.split("\r\n")[0], headers.split("\r\n")[0]
    print("status ok; Transfer-Encoding:",
          [l for l in headers.split("\r\n") if l.lower().startswith("transfer-encoding")])

    got = decoder(body, CAP)
    # Firmware pipeline: decode+cap -> trimFeatures -> parse. Mirror it.
    import os, sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from test_trim import trim
    trimmed, kept = trim(got.decode("utf-8"), 15)
    doc = json.loads(trimmed)   # raises if the decoder left framing garbage in
    feats = doc["features"]
    print(f"decoded {len(got)} bytes (cap {CAP}), features={len(feats)} "
          f"(trimmed kept={kept}), "
          f"first={feats[0]['properties']['stationIdentifier']}")

    # Reference: the captured unchunked body must contain the same stations
    ref = json.loads(open('test/nws/stations.json', 'rb').read())
    ref_ids = [f['properties']['stationIdentifier'] for f in ref['features']]
    got_ids = [f['properties']['stationIdentifier'] for f in feats]
    assert got_ids == ref_ids[:len(got_ids)], (got_ids, ref_ids)
    print("ids match the unchunked reference:", got_ids)

    # Plain-mode discriminator: a body starting with '{' passes through.
    got2 = decoder(b"  \n{" + b'"hello":1}', CAP)
    assert json.loads(got2) == {"hello": 1}
    print("plain-mode branch ok")

    # Mid-chunk cap truncation still yields valid JSON after trimFeatures
    # (covered by test_trim.py on the decoded output).
    print("ALL OK")


if __name__ == "__main__":
    main()
