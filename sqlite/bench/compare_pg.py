"""Compares the SQLite extension with pg_sift's own evaluator on the same corpus and queries.

    python3 -I compare_pg.py DB EXPECTED_TSV [--ext build/sift] [--table ft] [--limit N]

EXPECTED_TSV (label, siftql, count, top100) comes from PostgreSQL: count(*) of `body @@> q` and
`id:score,...` of ORDER BY sift.score(ctid) DESC LIMIT 100 (ties by ascending id). Here the same query runs as
`sift_count(t, q)` and `SELECT rowid, rank FROM t WHERE t MATCH q ORDER BY rank LIMIT 100` (rank = -score). Match counts and
the order of the ids must be equal; scores must be equal as float32 (reported: bit-equal, and the largest relative difference).
"""
import argparse
import os
import sqlite3
import struct
import sys


def f32(x):
    return struct.unpack('f', struct.pack('f', x))[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('db')
    ap.add_argument('expected')
    ap.add_argument('--ext', default=os.path.join(os.path.dirname(__file__), '..', 'build', 'sift'))
    ap.add_argument('--table', default='ft')
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--tol', type=float, default=1e-5)
    a = ap.parse_args()
    db = sqlite3.connect(a.db)
    db.enable_load_extension(True)
    db.load_extension(a.ext)
    t = a.table
    n = bad = bit_equal = nscores = 0
    worst = 0.0
    classes = {}
    with open(a.expected, encoding='utf-8') as f:
        for line in f:
            label, q, count, top = line.rstrip('\n').split('\t')
            if a.limit and n >= a.limit:
                break
            n += 1
            cls = label.split(':')[0]
            c = classes.setdefault(cls, [0, 0])
            c[0] += 1
            try:
                got_count = db.execute(f"SELECT sift_count('{t}', ?)", (q,)).fetchone()[0]
                got = db.execute(f'SELECT rowid, rank FROM {t} WHERE {t} MATCH ? ORDER BY rank LIMIT 100', (q,)).fetchall()
                n_match = db.execute(f'SELECT count(*) FROM {t} WHERE {t} MATCH ?', (q,)).fetchone()[0]
                err = None
            except sqlite3.Error as e:
                err = str(e)
            if count == 'ERR':
                if err is None:
                    print(f'{label}\t{q}: PostgreSQL raised an error, SQLite returned {got_count}')
                    bad += 1
                    c[1] += 1
                continue
            if err is not None:
                print(f'{label}\t{q}: SQLite error {err}')
                bad += 1
                c[1] += 1
                continue
            want = [(int(i), float(s)) for i, s in (p.split(':') for p in top.split(',') if p)]
            problems = []
            if got_count != int(count):
                problems.append(f'count {got_count} != {count}')
            if n_match != int(count):
                problems.append(f'MATCH count {n_match} != {count}')
            if [r[0] for r in got] != [i for i, _ in want]:
                problems.append('ids differ')
            else:
                for (_, ws), (_, gs) in zip(want, got):
                    s = f32(-gs)
                    nscores += 1
                    if s == f32(ws):
                        bit_equal += 1
                    d = abs(s - ws) / max(abs(ws), 1e-30)
                    worst = max(worst, d)
                    if d > a.tol:
                        problems.append(f'score {s} != {ws}')
                        break
            if problems:
                bad += 1
                c[1] += 1
                print(f'{label}\t{q}: ' + '; '.join(problems))
    print(f'{n} queries, {bad} differ; scores compared {nscores}, bit-equal {bit_equal}, worst relative difference {worst:.3g}')
    for cls, (tot, b) in sorted(classes.items()):
        print(f'  {cls}: {tot} queries, {b} differ')
    sys.exit(1 if bad else 0)


main()
