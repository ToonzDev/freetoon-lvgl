#!/usr/bin/env python3
"""Check the French UI table (src/i18n_fr.c) against the tr() calls in src/.

    python3 tools/i18n_fr_check.py [--stubs]

Errors (exit 1):
  - a tr("nl", "en") pair used in the sources has no French entry
    (--stubs prints ready-to-paste entries for them);
  - a French string does not keep the printf conversions of the English one
    ("%.0s" counts as "%s": it swallows an argument without printing it);
  - a French string contains characters the UI fonts cannot draw or the
    house style forbids (em dash, ellipsis character, curly quotes, oe).
Warnings: stale entries (no longer used), duplicate keys, non-literal tr()
call sites that are not covered by the table patterns in i18n_extract.py.
"""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from i18n_extract import extract, _read_arg, _skip_ws_comments, TABLES

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src')
FORBIDDEN = {'—': 'em dash', '…': 'ellipsis', '’': 'curly apostrophe',
             '‘': 'curly quote', '“': 'curly quote', '”': 'curly quote',
             'œ': 'oe ligature', 'Œ': 'OE ligature', '«': 'guillemet',
             '»': 'guillemet'}
FMT = re.compile(r'%(?:%|[-+ #0]*(?:\*|\d+)?(?:\.(?:\*|\d+))?(hh|h|ll|l|L|z|j|t)?([diouxXeEfFgGaAcspn]))')

def conversions(s):
    out = []
    for m in FMT.finditer(s):
        if m.group(0) == '%%':
            continue
        conv = m.group(2)
        if conv in 'di': conv = 'd'
        if conv in 'eEfFgGaA': conv = 'f'
        out.append((m.group(1) or '') + conv)
    return out

def parse_table(path):
    s = open(path, encoding='utf-8').read()
    start = s.index('TR_FR[] = {') + len('TR_FR[] = {')
    i, out = start, []
    while True:
        i = _skip_ws_comments(s, i)
        if s[i] == '}':
            break
        assert s[i] == '{', f'i18n_fr.c: unexpected {s[i]!r} at line {s.count(chr(10), 0, i) + 1}'
        line = s.count('\n', 0, i) + 1
        vals, i = [], i + 1
        for _ in range(3):
            v, i, end = _read_arg(s, i)
            vals.append(v)
        assert end == '}', f'i18n_fr.c:{line}: expected 3 fields'
        out.append((vals[0], vals[1], vals[2], line))
        i = _skip_ws_comments(s, i)
        if s[i] == ',':
            i += 1
    return out

def c_entry(nl, en, fr='TODO'):
    def expr(b):
        parts = re.split(r'\{(LV_SYMBOL_[A-Z0-9_]+)\}', b)
        return ' '.join(p if k % 2 else '"' + p + '"' for k, p in enumerate(parts) if k % 2 or p or len(parts) == 1)
    return f'    {{ {expr(nl)}, {expr(en)},\n      {expr(fr)} }},'

def main():
    pairs, dyn = extract(SRC)
    used = {}
    for nl, en, p, l in pairs:
        used.setdefault((nl, en), f'{os.path.basename(p)}:{l}')
    table = parse_table(os.path.join(SRC, 'i18n_fr.c'))
    errors = warnings = 0
    have = {}
    for nl, en, fr, line in table:
        if (nl, en) in have:
            print(f'warning: i18n_fr.c:{line}: duplicate entry for {en!r}'); warnings += 1
        have[(nl, en)] = (fr, line)
    missing = [k for k in used if k not in have]
    for nl, en in missing:
        print(f'error: no French for {en!r} ({used[(nl, en)]})'); errors += 1
    for (nl, en), (fr, line) in have.items():
        if (nl, en) not in used:
            print(f'warning: i18n_fr.c:{line}: stale entry {en!r}'); warnings += 1
        if fr is None:
            print(f'error: i18n_fr.c:{line}: French text is not a literal'); errors += 1; continue
        if conversions(fr) != conversions(en):
            print(f'error: i18n_fr.c:{line}: format mismatch {conversions(en)} vs {conversions(fr)} in {fr!r}')
            errors += 1
        for ch, what in FORBIDDEN.items():
            if ch in fr:
                print(f'error: i18n_fr.c:{line}: {what} in {fr!r}'); errors += 1
    covered = {t[0] for t in TABLES} | {'update_modal.c'}
    for p, l in dyn:
        if os.path.basename(p) not in covered:
            print(f'warning: non-literal tr() at {os.path.basename(p)}:{l} is not covered'); warnings += 1
    if missing and '--stubs' in sys.argv:
        print('\n/* paste into TR_FR[] in src/i18n_fr.c and translate */')
        for nl, en in missing:
            print(c_entry(nl, en))
    print(f'{len(used)} pairs used, {len(have)} French entries, {errors} error(s), {warnings} warning(s)')
    return 1 if errors else 0

if __name__ == '__main__':
    sys.exit(main())
