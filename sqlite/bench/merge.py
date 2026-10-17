"""Merge cost on a database that holds many segments: time, peak memory, and the answers before and after.

    python3 -I merge.py --db FILE --trace trace.tsv [--cache-kib N] [--steps N] [--n 300] [--ext build/sift]

Build the many-segment database with build.py --no-merge --opts 'stemmer=english,pending_postings=2000000,automerge=0'.
Runs in a fresh process, so the peak RSS is that of the merge alone. --steps merges that many steps ('merge', N), 0 = all
with optimize. Prints one TSV row; exits 1 when the match sets, counts or the top-10 differ after the merge.
"""
import argparse
import os
import resource
import sqlite3
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench import query_text  # noqa: E402


def answers(db, qs):
    return [(db.execute('SELECT rowid FROM ft WHERE ft MATCH ? ORDER BY rank LIMIT 10', (q,)).fetchall(),
             db.execute("SELECT sift_count('ft', ?)", (q,)).fetchone()[0]) for q in qs]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--db', required=True)
    ap.add_argument('--trace', required=True)
    ap.add_argument('--cache-kib', type=int)
    ap.add_argument('--steps', type=int, default=0)
    ap.add_argument('--n', type=int, default=300)
    ap.add_argument('--ext', default=os.path.join(os.path.dirname(__file__), '..', 'build', 'sift'))
    a = ap.parse_args()
    db = sqlite3.connect(a.db, isolation_level=None)
    db.enable_load_extension(True)
    db.load_extension(a.ext)
    if a.cache_kib is not None:
        db.execute("INSERT INTO ft(ft, rank) VALUES('cache-size', ?)", (a.cache_kib,))
    qs = []
    with open(a.trace, encoding='utf-8') as f:
        for line in f:
            _, mode, terms = line.rstrip('\n').split('\t')
            qs.append(query_text('sift', mode, terms.split(' ')))
    qs = qs[:a.n]
    segs0 = db.execute('SELECT count(*) FROM ft_segs').fetchone()[0]
    before = answers(db, qs)
    rss0 = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024
    t0 = time.perf_counter()
    if a.steps:
        db.execute("INSERT INTO ft(ft, rank) VALUES('merge', ?)", (a.steps,))
    else:
        db.execute("INSERT INTO ft(ft) VALUES('optimize')")
    dt = time.perf_counter() - t0
    rss1 = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024
    segs1 = db.execute('SELECT count(*) FROM ft_segs').fetchone()[0]
    after = answers(db, qs)
    db.execute("INSERT INTO ft(ft, rank) VALUES('integrity-check', 1)")
    same = [b[0] == x[0] and b[1] == x[1] for b, x in zip(before, after)]
    print('segments_before\tsegments_after\tmerge_s\trss_before_mib\tpeak_rss_mib\tcache_kib\tidentical')
    print(f'{segs0}\t{segs1}\t{dt:.2f}\t{rss0:.0f}\t{rss1:.0f}\t{a.cache_kib}\t{sum(same)}/{len(same)}')
    sys.exit(0 if all(same) else 1)


main()
