#!/usr/bin/env python3
"""Simulator for CYD alert screen: HTML -> text -> wrap -> page.

Mirrors the firmware pipeline (nws_alerts.cpp htmlToText, alerts_screen.cpp
wrapBff / buildStream) and prints the resulting pages plus regression checks:

  * descriptions wrap without truncation (DESC_MAX_LINES budget),
  * instructions parse from the NWS "instruction" field and wrap without
    truncation (INSTR_MAX_LINES budget),
  * each alert's stream contains exactly its own description + instructions
    (individual records stay separate),
  * the "WHAT TO DO" label is never left alone on the bottom row of a page
    (it keeps at least its first instruction line),
  * no section heading ("*" bullet line) ends a page with its content
    starting on the next one.

Usage:
    python3 tools/simulate_alerts.py [alerts.json ...]

Defaults to the captured NWS API response in scratch-folder/alerts/
(raw_alerts.json); every feature in the response is processed.
"""

import json
import os
import re
import sys

# ---------------------------------------------------------------------------
# Layout constants (mirror alerts_screen.cpp)
# ---------------------------------------------------------------------------
BODY_LINES = 8            # body lines per page
BFF_MAX_W = 312           # body width in px
BFF_MAX_CHARS = 64        # char cap per line
DESC_MAX_LINES = 72       # line budget for a full description buffer
INSTR_MAX_LINES = 8       # line budget for a full instruction buffer

# Nunito em13 advance table: width(px) per codepoint, 32..126
# (generated from tools/inspect_bff.py).
F13_ADV = {
    32: 4, 33: 4, 34: 6, 35: 9, 36: 9, 37: 6, 38: 12, 39: 4, 40: 6, 41: 6,
    42: 7, 43: 9, 44: 4, 45: 7, 46: 4, 47: 6, 48: 9, 49: 9, 50: 9, 51: 9,
    52: 9, 53: 9, 54: 9, 55: 9, 56: 9, 57: 5, 58: 4, 59: 6, 60: 9, 61: 9,
    62: 9, 63: 8, 64: 12, 65: 11, 66: 9, 67: 10, 68: 10, 69: 10, 70: 8,
    71: 9, 72: 10, 73: 4, 74: 5, 75: 9, 76: 9, 77: 14, 78: 12, 79: 12,
    80: 9, 81: 12, 82: 9, 83: 9, 84: 11, 85: 11, 86: 10, 87: 11, 88: 10,
    89: 10, 90: 9, 91: 6, 92: 6, 93: 6, 94: 8, 95: 8, 96: 4, 97: 9, 98: 9,
    99: 8, 100: 9, 101: 9, 102: 5, 103: 9, 104: 9, 105: 4, 106: 4, 107: 7,
    108: 4, 109: 14, 110: 9, 111: 9, 112: 9, 113: 9, 114: 6, 115: 7,
    116: 6, 117: 9, 118: 9, 119: 13, 120: 8, 121: 9, 122: 8, 123: 6,
    124: 4, 125: 6, 126: 4,
}


def bff_width(s):
    """Pixel width of s at Nunito em13 (unknown codepoints count as 9 px,
    the widest common glyph - conservative)."""
    w = 0
    for ch in s:
        o = ord(ch)
        if o < 32 or o > 126:
            o = 63
        w += F13_ADV[o]
    return w


# ---------------------------------------------------------------------------
# htmlToText (mirror of nws_alerts.cpp)
# ---------------------------------------------------------------------------

class Emitter:
    """Char-level writer with word-boundary rollback + '...' at the cap."""

    def __init__(self, n):
        self.n = n
        self.o = 0
        self.cap_hit = False
        self.prev_space = False

    def _emit(self, s):
        for ch in s:
            if self.o < self.n - 1:
                self.buf[self.o] = ch
                self.o += 1
            self.prev_space = (ch == ' ')

    def finish(self):
        # Roll back to a word boundary, then leave room for the marker
        # (mirror of the capHit block in htmlToText).
        if self.cap_hit:
            k = self.o
            while k > 0 and self.buf[k - 1] not in ' \n':
                k -= 1
            self.o = k
        while self.o > 0 and self.buf[self.o - 1] in ' \n':
            self.o -= 1
        if self.cap_hit:
            b = self.o
            while b + 3 > self.n - 1 and b > 0:
                o2 = b
                while o2 > 0 and self.buf[o2 - 1] not in ' \n':
                    o2 -= 1
                b = (o2 - 1) if o2 > 0 else 0
            self.o = b
            if self.n >= 4:
                self.buf[self.o] = '.'
                self.buf[self.o + 1] = '.'
                self.buf[self.o + 2] = '.'
                self.o += 3
        return ''.join(self.buf[:self.o])


def map_unichar(ch):
    """Mirror of the firmware's 7-bit codepoint mapping (GLCD font)."""
    o = ord(ch)
    if o < 0x80:
        return ch if 0x20 <= o <= 0x7E else ' '
    if o in (0x2018, 0x2019, 0x201A):
        return "'"
    if o in (0x201C, 0x201D, 0x201E):
        return '"'
    if o in (0x2013, 0x2014, 0x2012, 0x2015, 0x2212, 0x2022, 0x00B7):
        return '-'
    if o == 0x2026:
        return '...'
    if o == 0x00B0:
        return 'deg'
    if o in (0x00A0, 0x2007, 0x202F):
        return ' '
    return ' '


def html_to_text(s, n):
    buf = [''] * n
    em = Emitter(n)
    em.buf = buf

    in_tag = False
    tag_start = 0
    p = 0
    L = len(s)
    while p < L and em.o < n - 1:
        c = s[p]
        if c == '<':
            in_tag = True
            tag_start = p
            p += 1
            continue
        if c == '>' and in_tag:
            tag = s[tag_start + 1:p].lower()
            if tag in ('br', 'p', 'div', 'li'):
                if not em.prev_space:
                    em._emit('\n')
                    em.prev_space = True
            in_tag = False
            p += 1
            continue
        if in_tag:
            p += 1
            continue
        if c == '&':
            m = re.match(r'&nbsp;?', s[p:p + 6])
            if m:
                em._emit(' ')
                p += len(m.group(0))
                continue
            for ent, ch in (('&amp;', '&'), ('&lt;', '<'), ('&gt;', '>'),
                            ('&quot;', '"'), ('&#39;', "'"), ('&rsquo;', "'"),
                            ('&lsquo;', "'"), ('&ldquo;', '"'), ('&rdquo;', '"'),
                            ('&mdash;', '-'), ('&ndash;', '-')):
                if s.startswith(ent, p):
                    em._emit(ch)
                    p += len(ent)
                    break
            else:
                em._emit(map_unichar(c))
                p += 1
            continue
        em._emit(map_unichar(c))
        p += 1
    if p < L:
        em.cap_hit = True
    text = em.finish()

    # NWS "* WHAT..." label style: a run of 3+ dots running straight into
    # the next word becomes a spaced dash; a trailing "..." (truncation
    # marker) is left as-is. Mirror of the firmware's final pass.
    out = []
    i = 0
    L = len(text)
    while i < L:
        if text[i] == '.' and i + 2 < L:
            j = i
            while j < L and text[j] == '.':
                j += 1
            if j - i >= 3 and j < L and text[j] not in (' ', '\n'):
                if i > 0 and text[i - 1] not in (' ', '\n'):
                    out.append(' ')
                out.append('-')
                out.append(' ')
                i = j
                continue
        out.append(text[i])
        i += 1
    return ''.join(out)


# ---------------------------------------------------------------------------
# wrapBff (mirror of alerts_screen.cpp - identical structure)
# ---------------------------------------------------------------------------

def wrap_bff(text, max_lines=DESC_MAX_LINES):
    """Mirror of wrapBff(): greedy pixel-width wrap (plus a char cap). Hard
    breaks stay breaks, a "*" bullet line gets a blank line before it, and
    "..." marks truncation. Returns (lines, truncated)."""
    lines = ['']
    li = 0
    o = 0
    truncated = False
    i = 0
    L = len(text)
    while i < L:
        had_space = False
        while i < L and text[i] == ' ':
            i += 1
            had_space = True
        if i >= L:
            break
        if text[i] == '\n':                          # hard break (kept by htmlToText)
            li += 1
            o = 0
            if li >= max_lines:
                truncated = True
                break
            lines.append('')
            i += 1
            continue
        if o == 0 and text[i] == '*' and li > 0 \
                and lines[li - 1] and lines[li - 1][0] != '*':
            li += 1                                   # current (empty) line = the blank
            o = 0
            if li >= max_lines:
                truncated = True
                break
            lines.append('')
        wlen = 0
        while i + wlen < L and text[i + wlen] not in ' \n':
            wlen += 1
        pos = 0                                       # progress within the word
        stop = False
        while pos < wlen:
            pad = 1 if (o > 0 and pos == 0 and had_space) else 0
            take = wlen - pos
            cap = BFF_MAX_CHARS - (o + pad)
            if take > cap:
                take = cap                            # char cap (buffer bound)
            while take > 0:
                # Candidate line = current line + pad + word chunk; shrink to fit.
                cand = lines[li] + (' ' if pad else '') + text[i + pos:i + pos + take]
                if bff_width(cand) <= BFF_MAX_W:
                    break
                take -= 1
            if pos == 0 and 0 < take < wlen and wlen <= BFF_MAX_CHARS:
                # A word prefix fit, but not the whole word. If the word fits
                # on a fresh line, move it there whole instead of breaking it.
                if bff_width(text[i:i + wlen]) <= BFF_MAX_W:
                    take = 0
            if take <= 0:                             # nothing fits: next line
                li += 1
                o = 0
                if li >= max_lines:
                    truncated = True
                    stop = True
                    break
                lines.append('')
                continue
            if pad:
                o += 1
                lines[li] += ' '
            lines[li] += text[i + pos:i + pos + take]
            o += take
            pos += take
        if stop:
            break
        i += wlen
    if li >= max_lines:
        li = max_lines - 1                            # overflow: last full line
    if truncated:
        line = lines[li]
        lines[li] = line[:-3] + '...' if len(line) >= 3 else '...'
    return lines[:li + 1], truncated


# ---------------------------------------------------------------------------
# buildStream (mirror of alerts_screen.cpp)
# ---------------------------------------------------------------------------

def build_stream(desc_lines, instr_lines):
    """Assemble the flat content stream: description lines, then (when the
    alert carries instructions) a blank line, the "WHAT TO DO" label, and
    the instruction lines.

    Pagination rules (mirror of the firmware):
      * a "*" section heading is never left alone on the bottom row of a
        page - a blank line is inserted before it, pushing the heading and
        the rest of its section to the top of the next page (forward pass
        over the description, before the label block is appended);
      * "WHAT TO DO" is never left on the bottom row either - the blank
        separator is dropped where it would be (its first instruction line
        then shares the page with it).
    """
    n = len(desc_lines)
    stream = list(desc_lines)
    for i in range(n - 1):
        if not stream[i].startswith('*'):
            continue
        if i % BODY_LINES != BODY_LINES - 1:
            continue
        if not any(stream[j] for j in range(i + 1, n)):
            continue
        stream.insert(i, '')
        n += 1
    if instr_lines:
        if n % BODY_LINES != BODY_LINES - 2:
            stream.append('')
        stream.append('WHAT TO DO')
        stream.extend(instr_lines)
    return stream


# ---------------------------------------------------------------------------
# Per-alert processing
# ---------------------------------------------------------------------------

def process_slot(desc_raw, instr_raw):
    desc = html_to_text(desc_raw, 2048)
    instr = html_to_text(instr_raw, 256)
    desc_lines, d_trunc = wrap_bff(desc, DESC_MAX_LINES)
    instr_lines, i_trunc = wrap_bff(instr, INSTR_MAX_LINES) if instr else ([], False)
    stream = build_stream(desc_lines, instr_lines)
    pages = (len(stream) + BODY_LINES - 1) // BODY_LINES
    return desc, instr, desc_lines, instr_lines, stream, pages


def check_alert(name, desc, instr, desc_lines, instr_lines, stream, pages, errors):
    dn, in_ = len(desc_lines), len(instr_lines)

    # 1. No truncation: full description and instructions fit the budgets.
    if len(desc_lines) >= DESC_MAX_LINES:
        errors.append(f'{name}: description truncated at {DESC_MAX_LINES} lines')
    if len(instr_lines) >= INSTR_MAX_LINES:
        errors.append(f'{name}: instructions truncated at {INSTR_MAX_LINES} lines')

    # 2. Instruction parsing: non-empty NWS field must yield wrapped lines.
    if instr and not instr_lines:
        errors.append(f'{name}: instruction text present but wrapped to nothing')

    # 3. Record separation: the stream is exactly this alert's own content.
    if stream[:dn] != desc_lines:
        errors.append(f'{name}: stream does not start with its own description lines')
    tail = stream[dn:]
    if instr:
        if tail[:2] not in (['', 'WHAT TO DO'], ['WHAT TO DO']):
            errors.append(f'{name}: expected blank + WHAT TO DO after description, got {tail[:2]!r}')
        if tail[2:] != instr_lines:
            errors.append(f'{name}: stream does not end with its own instruction lines')
        if sum(1 for l in stream if l == 'WHAT TO DO') != 1:
            errors.append(f'{name}: expected exactly one WHAT TO DO label')
    else:
        if tail != []:
            errors.append(f'{name}: stream has extra lines without instructions: {tail!r}')

    # 4. No orphaned heading on the bottom row of any page.
    for p in range(pages):
        page = stream[p * BODY_LINES:(p + 1) * BODY_LINES]
        nonblank = [l for l in page if l]
        if not nonblank:
            continue
        last = nonblank[-1]
        if last == 'WHAT TO DO':
            errors.append(f'{name}: page {p + 1}/{pages} ends with orphaned WHAT TO DO label')
        elif last.startswith('*'):
            errors.append(f'{name}: page {p + 1}/{pages} ends with section heading {last[:40]!r}')

    # 5. Label keeps at least its first instruction line on the same page.
    if instr:
        li_ = next((i for i, l in enumerate(stream) if l == 'WHAT TO DO'), None)
        first_instr = next((i for i in range(dn, len(stream)) if stream[i]), None)
        if li_ is None or first_instr is None:
            errors.append(f'{name}: WHAT TO DO label or instruction lines missing from stream')
        elif li_ // BODY_LINES != first_instr // BODY_LINES:
            errors.append(f'{name}: WHAT TO DO label on a different page than its first instruction line')


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    paths = sys.argv[1:] or [os.path.join(root, 'scratch-folder', 'alerts', 'raw_alerts.json')]

    errors = []
    n_alerts = 0
    for path in paths:
        if not os.path.exists(path):
            errors.append(f'{path}: file not found')
            continue
        with open(path, encoding='utf-8', errors='replace') as f:
            data = json.load(f)
        for feat in data.get('features', []):
            p = feat.get('properties', {})
            name = p.get('event', '?')
            sev = p.get('severity', '?')
            desc_raw = p.get('description') or ''
            instr_raw = p.get('instruction') or ''   # singular: NWS field name

            desc, instr, desc_lines, instr_lines, stream, pages = process_slot(desc_raw, instr_raw)
            check_alert(f'{name} ({sev})', desc, instr, desc_lines, instr_lines, stream, pages, errors)
            n_alerts += 1

            print(f'\n=== {name} ({sev}) ===')
            print(f'  desc: {len(desc_raw)} chars raw -> {len(desc)} chars text -> {len(desc_lines)} wrapped lines')
            print(f'  instr: {len(instr_raw)} chars raw -> {len(instr)} chars text -> {len(instr_lines)} wrapped lines')
            for pg in range(pages):
                print(f'\n  --- P{pg + 1}/{pages} ---')
                for line in stream[pg * BODY_LINES:(pg + 1) * BODY_LINES]:
                    print(f"    {line!r}")

    print()
    if errors:
        print('ERRORS:')
        for e in errors:
            print(f'  - {e}')
        sys.exit(1)
    print(f'All checks passed for {n_alerts} alert(s): no truncation, instructions parsed,')
    print('records separated, no orphaned WHAT TO DO label, no section heading left')
    print('alone on a bottom row.')


if __name__ == '__main__':
    main()
