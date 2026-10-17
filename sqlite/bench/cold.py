"""First-touch cost, warm cost and memory of the top-10 AND/OR/phrase queries, in a fresh process.

    python3 -I cold.py --engine sift|fts5 --db FILE --trace trace.tsv [--cache-kib N] [--drop-os] [--cpu N] [--n 1000]
                       [--ext build/sift] [--simd LEVEL] [--parallel N]

Pass 1 starts with an empty sift page cache (--drop-os also drops the database file from the OS page cache first, so
it reads from disk), pass 2 repeats the same queries. One TSV row per pass: seconds, queries per second, the peak RSS
in MiB and, for sift, the cache's hits, misses and evictions after the pass.
"""
import argparse
import os
import resource
import sqlite3
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench import query_text, statements  # noqa: E402


def drop_os_cache(path):
    for p in (path, path + '-wal'):
        if os.path.exists(p):
            fd = os.open(p, os.O_RDONLY)
            os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
            os.close(fd)


def rss_mib():
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--engine', required=True, choices=['sift', 'fts5'])
    ap.add_argument('--db', required=True)
    ap.add_argument('--trace', required=True)
    ap.add_argument('--cache-kib', type=int)
    ap.add_argument('--drop-os', action='store_true')
    ap.add_argument('--cpu', type=int)
    ap.add_argument('--n', type=int, default=1000)
    ap.add_argument('--ext', default=os.path.join(os.path.dirname(__file__), '..', 'build', 'sift'))
    ap.add_argument('--simd')
    ap.add_argument('--parallel', type=int)
    a = ap.parse_args()
    if a.cpu is not None:
        os.sched_setaffinity(0, {a.cpu})
    if a.drop_os:
        drop_os_cache(a.db)
    db = sqlite3.connect(a.db)
    db.execute('PRAGMA cache_size = -8192')
    if a.engine == 'sift':
        db.enable_load_extension(True)
        db.load_extension(a.ext)
        if a.simd:
            db.execute('SELECT sift_simd(?)', (a.simd,)).fetchall()
        if a.cache_kib is not None:
            db.execute("INSERT INTO ft(ft, rank) VALUES('cache-size', ?)", (a.cache_kib,))
        if a.parallel is not None:
            db.execute("INSERT INTO ft(ft, rank) VALUES('parallel', ?)", (a.parallel,))
    sql = statements(a.engine, 'ft')[0]
    qs = []
    with open(a.trace, encoding='utf-8') as f:
        for line in f:
            _, mode, terms = line.rstrip('\n').split('\t')
            qs.append(query_text(a.engine, mode, terms.split(' ')))
    qs = qs[:a.n * 3]
    print('engine\tpass\tseconds\tqps\tpeak_rss_mib\tcache')
    for p in (1, 2):
        t0 = time.perf_counter()
        for q in qs:
            db.execute(sql, (q,)).fetchall()
        dt = time.perf_counter() - t0
        stat = db.execute("SELECT sift_stat('ft')").fetchone()[0] if a.engine == 'sift' else ''
        keep = ' '.join(kv for kv in stat.split() if kv.split('=')[0] in ('cache_hits', 'cache_misses', 'evictions', 'cached_bytes', 'pin_leaks'))
        print(f'{a.engine}\t{p}\t{dt:.2f}\t{len(qs) / dt:.0f}\t{rss_mib():.0f}\t{keep}')


main()
