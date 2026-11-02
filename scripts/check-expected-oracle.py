#!/usr/bin/env python3
"""check-expected-oracle.py [--selftest] [FILE...]   an expected output that records a failed comparison is a test that passes on a bug.

A regression test compares two plans or two engines in SQL and prints the verdict in a column (agree, same, equal, match, ok, identical,
equivalent, passed, or a name made of such words: rows_agree, same_order). The expected file is accepted from the first run; a verdict
of f / false in it pins the disagreement as the right answer. Lint fails on
  - a cell f / false (also 0 / no) in an oracle column of a table or an expanded record, and
  - a cell with the word FAIL, FAILED, MISMATCH or DIFFERENT in any result column.
A verdict that is f on purpose (a test of an operator that must not match, scores that differ within a stated bound) is listed in
scripts/expected-oracle.allow, one `file:column<TAB>hits<TAB>reason` a row: the exact number of cells, so a new one in the same column fails and is read."""
import re, sys, glob, os

ORACLE = {'agree', 'agrees', 'agreed', 'same', 'equal', 'equals', 'match', 'matches', 'matched', 'ok', 'identical', 'equivalent', 'passed', 'pass'}
BAD_CELL = re.compile(r'\b(FAIL|FAILED|MISMATCH|DIFFERENT)\b')
FALSE = {'f', 'false'}
SEP = re.compile(r'^-+(\+-+)+$|^-+$')
ROW_END = re.compile(r'^\(\d+ rows?\)$')
RECORD = re.compile(r'^-\[ RECORD \d+ \]-+')


def oracle_name(name):
    return any(t in ORACLE for t in re.split(r'[^a-z0-9]+', name.lower()) if t)


def cells(line):
    return [c.strip() for c in line.split('|')]


def scan(path):
    return scan_text(open(path, encoding='utf-8', errors='replace').read())


def scan_text(text):
    """Yield (line number, column, cell, why) for every hit of the text of one expected file."""
    lines = text.split('\n')
    i = 0
    while i < len(lines):
        l = lines[i]
        if RECORD.match(l):  # \x on: `name | value` rows until the next record or a blank line
            i += 1
            while i < len(lines) and lines[i].strip() and not RECORD.match(lines[i]):
                if '|' in lines[i]:
                    name, _, val = lines[i].partition('|')
                    name, val = name.strip(), val.strip()
                    yield from verdict(i + 1, name, val)
                i += 1
            continue
        if i + 1 < len(lines) and SEP.match(lines[i + 1]) and '|' in l and not l.startswith(('--', '+')) and '+' in lines[i + 1]:
            names = cells(l)
            i += 2
            while i < len(lines) and lines[i].strip() and not ROW_END.match(lines[i]):
                row = cells(lines[i])
                if len(row) == len(names):
                    for n, v in zip(names, row):
                        yield from verdict(i + 1, n, v)
                i += 1
            continue
        i += 1


def verdict(n, name, val):
    if oracle_name(name) and val.lower() in FALSE:
        yield (n, name, val, 'oracle column is false')
    m = BAD_CELL.search(val)
    if m:
        yield (n, name, val[:60], f'result cell holds {m.group(1)}')


def allowed(root):
    p = os.path.join(root, 'scripts/expected-oracle.allow')
    out = {}
    if os.path.exists(p):
        for l in open(p):
            if l.strip() and not l.startswith('#'):
                k, _, rest = l.rstrip('\n').partition('\t')
                n, _, why = rest.partition('\t')
                if not n.isdigit() or not why.strip():
                    sys.exit(f'{p}: "{k}" wants file:column<TAB>hits<TAB>reason')
                out[k] = (int(n), why)
    return out


def selftest():
    cases = [  # (text, expected number of hits)
        (' a | agree\n---+------\n 1 | t\n(1 row)\n', 0),
        (' a | agree\n---+------\n 1 | f\n(1 row)\n', 1),
        (' rows_same | n\n-----------+---\n f         | 3\n(1 row)\n', 1),
        (' a | verdict\n---+--------\n 1 | MISMATCH 3 rows\n(1 row)\n', 1),
        (' a | verdict\n---+--------\n 1 | different\n(1 row)\n', 0),  # lower case: a word of the data, not of a verdict
        ('-[ RECORD 1 ]--\nidentical | f\nn         | 1\n', 1),
        (' a | has_match\n---+------\n 1 | t\n 2 | t\n(2 rows)\n', 0),
        (' a | okay\n---+------\n 1 | f\n(1 row)\n', 0),  # `okay` is not an oracle word
        ('-- agree is f here\nSELECT 1;\n', 0),
    ]
    bad = 0
    for n, (text, want) in enumerate(cases):
        got = len(list(scan_text(text)))
        if got != want:
            print(f'selftest case {n}: {got} hits, want {want}', file=sys.stderr)
            bad += 1
    print('check-expected-oracle: selftest ' + ('FAILED' if bad else 'ok'))
    return 1 if bad else 0


def main(a):
    if a[:1] == ['--selftest']:
        return selftest()
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
    os.chdir(root)
    files = a or sorted(glob.glob('test/expected/*.out'))
    allow = allowed('.')
    hits, seen = 0, {}
    for f in files:
        for n, col, cell, why in scan(f):
            seen.setdefault(f'{f}:{col}', []).append((n, cell, why))
    for k, h in seen.items():
        if k in allow and allow[k][0] == len(h):
            continue
        hits += len(h)
        for n, cell, why in h:
            print(f'{k.rpartition(":")[0]}:{n}: column "{k.rpartition(":")[2]}" = {cell}: {why}')
        if k in allow:
            print(f'{k}: scripts/expected-oracle.allow lists {allow[k][0]} hit(s), the file has {len(h)}')
    if not a:       # an allow row is judged against the whole suite, not against the files named
        for k in allow:
            if k not in seen:
                hits += 1
                print(f'{k}: scripts/expected-oracle.allow lists a column that has no hit any more: remove the row')
    if hits:
        print(f'check-expected-oracle: {hits} hit(s): a verdict that disagrees is a bug the expected file pins; fix the code or the test, '
              'or list file:column and the exact number of cells in scripts/expected-oracle.allow with the reason', file=sys.stderr)
    return 1 if hits else 0


sys.exit(main(sys.argv[1:]))
