"""First-touch cost and memory: one pass of the top-10 AND/OR/phrase queries in a fresh process (OS file cache warm).

    python3 -I cold.py --engine sift|fts5 --db FILE --trace trace.tsv [--read-mode 0|1] [--cpu N] [--n 1000] [--ext build/sift]

sift reads a segment page from its shadow table on first use and keeps it in memory (read mode 0: SELECT of the page
row; 1: sqlite3_blob_open + read). Prints seconds for the first pass, then for a second pass, and the peak RSS in MiB.
"""
import argparse
import os
import resource
import sqlite3
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench import query_text, statements  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--engine', required=True, choices=['sift', 'fts5'])
    ap.add_argument('--db', required=True)
    ap.add_argument('--trace', required=True)
    ap.add_argument('--read-mode', type=int, default=0)
    ap.add_argument('--cpu', type=int)
    ap.add_argument('--n', type=int, default=1000)
    ap.add_argument('--ext', default=os.path.join(os.path.dirname(__file__), '..', 'build', 'sift'))
    a = ap.parse_args()
    if a.cpu is not None:
        os.sched_setaffinity(0, {a.cpu})
    db = sqlite3.connect(a.db)
    db.execute('PRAGMA cache_size = -1048576')
    if a.engine == 'sift':
        db.enable_load_extension(True)
        db.load_extension(a.ext)
        db.execute('SELECT sift_read_mode(?)', (a.read_mode,)).fetchall()
    sql = statements(a.engine, 'ft')[0]
    qs = []
    with open(a.trace, encoding='utf-8') as f:
        for line in f:
            _, mode, terms = line.rstrip('\n').split('\t')
            qs.append(query_text(a.engine, mode, terms.split(' ')))
    qs = qs[:a.n * 3]
    for p in (1, 2):
        t0 = time.perf_counter()
        for q in qs:
            db.execute(sql, (q,)).fetchall()
        print(f'{a.engine} read_mode={a.read_mode} pass{p}: {time.perf_counter() - t0:.2f}s')
    print(f'peak RSS {resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024:.0f} MiB')


main()
