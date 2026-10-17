"""Writes the expected file of compare_pg.py from the index itself: a regression oracle for the 3,044 queries.

    python3 -I golden.py DB TRACE_TSV OUT_TSV [--ext build/sift] [--table ft]

The queries are the trace's AND / OR / phrase renderings plus 44 hand-written ones (NOT, groups, phrases, prefix,
wildcard, regex, fuzzy, boosts). The index must be built with stemmer=none and merged to one segment, as the
comparison with pg_sift's evaluator was. Run it with the build under test once, keep the file, and run
`compare_pg.py DB OUT_TSV` with later builds: counts, the top-100 ids and every score must stay bit-equal.
"""
import argparse
import os
import sqlite3
import sys

EXTRA = [
    'the', 'history AND war', 'history OR war', 'history AND NOT war', '(city OR town) AND river', '"united states"',
    '"new york" AND city', 'new* AND york', 'cit*', 'te?t', '/^colo[u]?r$/', 'war~1', 'history^3 AND war', '{city town river}',
    'river AND (lake OR sea) AND NOT ocean', '"world war"', '"world war" AND NOT second', 'a AND b', 'zzzqqqxx', 'zzzqqqxx OR war',
    '"a b c"', 'unit*', 'sta*e', 'anarchism', 'anarchism OR autism', 'king AND queen AND prince', 'king OR queen OR prince',
    '"of the"', '"in the" AND war', 'NOT war', 'war AND NOT (history OR peace)', 'pea*e', '/^pe.*e$/', 'peac~2', 'river^2 OR lake',
    '(a OR b) AND (c OR d)', 'city AND town AND river AND lake', '"the city"', 'Paris', 'PARIS', 'caf*', '"war and peace"',
    'water AND "new york"', '"king queen"',
]


def f32(x):
    import struct
    return struct.unpack('f', struct.pack('f', x))[0]


def queries(trace):
    out = []
    with open(trace, encoding='utf-8') as f:
        for line in f:
            sub, mode, terms = line.rstrip('\n').split('\t')
            words = terms.split(' ')
            q = '"' + ' '.join(words) + '"' if mode == 'phrase' else (' AND ' if mode == 'and' else ' OR ').join(words)
            out.append((f'{mode}:{sub}', q))
    return out + [(f'extra:{i}', q) for i, q in enumerate(EXTRA)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('db')
    ap.add_argument('trace')
    ap.add_argument('out')
    ap.add_argument('--ext', default=os.path.join(os.path.dirname(__file__), '..', 'build', 'sift'))
    ap.add_argument('--table', default='ft')
    a = ap.parse_args()
    db = sqlite3.connect(a.db)
    db.enable_load_extension(True)
    db.load_extension(a.ext)
    t = a.table
    n = 0
    with open(a.out, 'w', encoding='utf-8') as f:
        for label, q in queries(a.trace):
            try:
                count = db.execute(f"SELECT sift_count('{t}', ?)", (q,)).fetchone()[0]
                rows = db.execute(f'SELECT rowid, rank FROM {t} WHERE {t} MATCH ? ORDER BY rank LIMIT 100', (q,)).fetchall()
            except sqlite3.Error:
                f.write(f'{label}\t{q}\tERR\t\n')
                continue
            top = ','.join(f'{i}:{f32(-r):.9g}' for i, r in rows)
            f.write(f'{label}\t{q}\t{count}\t{top}\n')
            n += 1
    print(f'{n} queries written to {a.out}', file=sys.stderr)


main()
