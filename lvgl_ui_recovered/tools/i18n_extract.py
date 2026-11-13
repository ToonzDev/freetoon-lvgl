#!/usr/bin/env python3
"""Extract every tr("nl", "en") pair from the LVGL UI sources.

Used by i18n_fr_check.py to verify that the French table (src/i18n_fr.c)
covers every UI string and keeps printf format specifiers intact.
Only literal arguments are extracted; tr() calls with non-literal arguments
are reported separately (they must be covered by hand).
"""
import os, re, sys

def _skip_ws_comments(s, i):
    n = len(s)
    while i < n:
        if s[i].isspace():
            i += 1
        elif s.startswith('//', i):
            j = s.find('\n', i); i = n if j < 0 else j + 1
        elif s.startswith('/*', i):
            j = s.find('*/', i + 2); i = n if j < 0 else j + 2
        elif s[i] == '#' and (i == 0 or s[i-1] == '\n'):
            # preprocessor line inside a call (rare): skip it
            j = s.find('\n', i); i = n if j < 0 else j + 1
        else:
            break
    return i

def _read_literal(s, i):
    """s[i] == '"'. Return (raw_body, next_index)."""
    j = i + 1
    while s[j] != '"':
        if s[j] == '\\':
            j += 2
        else:
            j += 1
    return s[i+1:j], j + 1

def _read_arg(s, i):
    """Read one argument. Returns (value_or_None, next_index, end_char)."""
    i = _skip_ws_comments(s, i)
    parts = []
    while i < len(s):
        if s[i] == '"':
            body, i = _read_literal(s, i)
            parts.append(body)
        else:
            m = SYM.match(s, i)
            if not m:
                break
            parts.append('{' + m.group(0) + '}')   # LV_SYMBOL_xxx macro
            i = m.end()
        i = _skip_ws_comments(s, i)
    if parts and i < len(s) and s[i] in ',)}':
        return ''.join(parts), i + 1, s[i]
    # non-literal argument: skip to the matching , or )
    depth = 0
    while i < len(s):
        c = s[i]
        if c == '"':
            _, i = _read_literal(s, i); continue
        if c == "'":
            j = i + 1
            while s[j] != "'":
                j += 2 if s[j] == '\\' else 1
            i = j + 1; continue
        if c in '([{': depth += 1
        elif c in ')]}':
            if depth == 0: return None, i + 1, c
            depth -= 1
        elif c == ',' and depth == 0:
            return None, i + 1, c
        i += 1
    return None, i, ''

SYM = re.compile(r'LV_SYMBOL_[A-Z0-9_]+')
CALL = re.compile(r'(?<![A-Za-z0-9_])tr\s*\(')

def extract_file(path):
    s = open(path, encoding='utf-8').read()
    out, dyn = [], []
    i = 0
    n = len(s)
    while i < n:
        c = s[i]
        if s.startswith('//', i):
            j = s.find('\n', i); i = n if j < 0 else j + 1; continue
        if s.startswith('/*', i):
            j = s.find('*/', i + 2); i = n if j < 0 else j + 2; continue
        if c == '"':
            _, i = _read_literal(s, i); continue
        if c == "'":
            j = i + 1
            while s[j] != "'":
                j += 2 if s[j] == '\\' else 1
            i = j + 1; continue
        m = CALL.match(s, i) if c == 't' else None
        if m and (i == 0 or not (s[i-1].isalnum() or s[i-1] == '_')):
            line = s.count('\n', 0, i) + 1
            j = m.end()
            nl, j, e1 = _read_arg(s, j)
            if e1 != ',':
                i = j; continue          # declaration / other arity
            en, j, e2 = _read_arg(s, j)
            if nl is not None and en is not None:
                out.append((nl, en, path, line))
            else:
                dyn.append((path, line))
            i = j; continue
        i += 1
    return out, dyn

# Non-literal tr() call sites that read their (nl, en) pair from a static
# table. Each entry: file, regex with groups (nl, en). Keep in sync with the
# "non-literal" report below: a new one there needs a pattern here.
TABLES = [
    # weather.c  BR_COND[] = { {'a', "nl", "en"}, ... }  (via tr() in br_desc)
    ('weather.c', r"\{\s*'.'\s*,\s*\"([^\"]*)\"\s*,\s*\"([^\"]*)\"\s*\}"),
    # screen_home_stock.c  TM[] = { { "key", "nl", "en", LINK_x }, ... }
    ('screen_home_stock.c', r'\{\s*"[^"]*"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*LINK_'),
    # screen_vent_remote.c  btns[] = { { "nl", "en", 0xRRGGBB, ... }, ... }
    ('screen_vent_remote.c', r'\{\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*0x'),
]

def _string_array(s, name):
    m = re.search(r'\b' + name + r'\s*\[[^\]]*\]\s*=\s*\{(.*?)\};', s, re.S)
    return re.findall(r'"([^"]*)"', m.group(1)) if m else []

def extract_tables(srcdir):
    out = []
    for fname, rx in TABLES:
        path = os.path.join(srcdir, fname)
        s = open(path, encoding='utf-8').read()
        for m in re.finditer(rx, s):
            out.append((m.group(1), m.group(2), path, s.count('\n', 0, m.start()) + 1))
    # update_modal.c  tr(UPD_STEPS[i], UPD_STEPS_EN[i])  - two parallel arrays
    path = os.path.join(srcdir, 'update_modal.c')
    s = open(path, encoding='utf-8').read()
    for nl, en in zip(_string_array(s, 'UPD_STEPS'), _string_array(s, 'UPD_STEPS_EN')):
        out.append((nl, en, path, 0))
    return out

def extract(srcdir):
    pairs, dyn = [], []
    for name in sorted(os.listdir(srcdir)):
        if not name.endswith(('.c', '.h')) or name.startswith('lv_font') \
           or name in ('i18n.h', 'i18n_fr.c'):
            continue
        p, d = extract_file(os.path.join(srcdir, name))
        pairs += p; dyn += d
    pairs += extract_tables(srcdir)
    return pairs, dyn

if __name__ == '__main__':
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', 'src')
    pairs, dyn = extract(src)
    uniq = {}
    for nl, en, p, l in pairs:
        uniq.setdefault((nl, en), (p, l))
    print(f'{len(pairs)} literal tr() calls, {len(uniq)} unique pairs, {len(dyn)} non-literal', file=sys.stderr)
    for (p, l) in dyn:
        print(f'non-literal: {os.path.basename(p)}:{l}', file=sys.stderr)
