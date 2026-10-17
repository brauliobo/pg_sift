"""Latency and throughput of sift and FTS5 on the benchmark's query trace, one thread, warm cache.

    python3 -I bench.py --engine sift|fts5 --db FILE --trace trace.tsv [--ext build/sift] [--cpu N]
                        [--n 1000] [--classes and,or,phrase,count_and,count_or,count_phrase] [--table ft]

Per class one pass over the first N subs to warm, one timed pass; one TSV row per class:
engine class n median_ms p95_ms qps total_s load1 (qps = n / total time of the pass, i.e. one query after the other).
"""
import argparse
import os
import sqlite3
import sys
import time


def query_text(engine, mode, terms, bare=False):
    if mode == 'phrase':
        return '"' + ' '.join(terms) + '"'
    if engine == 'fts5' and not bare:
        terms = ['"' + t.replace('"', '""') + '"' for t in terms]
    return (' AND ' if mode == 'and' else ' OR ').join(terms)


def statements(engine, table):
    top = f'SELECT rowid FROM {table} WHERE {table} MATCH ? ORDER BY rank LIMIT 10'
    cnt = f'SELECT count(*) FROM {table} WHERE {table} MATCH ?'
    sift_cnt = f"SELECT sift_count('{table}', ?)"
    return top, (sift_cnt if engine == 'sift' else cnt), cnt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--engine', required=True, choices=['sift', 'fts5'])
    ap.add_argument('--db', required=True)
    ap.add_argument('--trace', required=True)
    ap.add_argument('--ext', default=os.path.join(os.path.dirname(__file__), '..', 'build', 'sift'))
    ap.add_argument('--cpu', type=int)
    ap.add_argument('--n', type=int, default=1000)
    ap.add_argument('--table', default='ft')
    ap.add_argument('--classes', default='and,or,phrase,count_and,count_or,count_phrase')
    ap.add_argument('--cache-mb', type=int, default=1024)
    ap.add_argument('--cache-kib', type=int, help='sift: cache size of the table, KiB')
    ap.add_argument('--simd', help='sift: scalar, avx2, avx512 or auto')
    ap.add_argument('--parallel', type=int, help='sift: worker threads of a scan')
    ap.add_argument('--bare', action='store_true', help='fts5: no quotes around terms (detail=none rejects phrases); both: only subs whose words are all alphanumeric')
    a = ap.parse_args()
    if a.cpu is not None:
        os.sched_setaffinity(0, {a.cpu})
    db = sqlite3.connect(a.db)
    db.execute(f'PRAGMA cache_size = -{a.cache_mb * 1024}')
    if a.engine == 'sift':
        db.enable_load_extension(True)
        db.load_extension(a.ext)
        if a.simd:
            db.execute('SELECT sift_simd(?)', (a.simd,)).fetchall()
        if a.cache_kib is not None:
            db.execute(f"INSERT INTO {a.table}({a.table}, rank) VALUES('cache-size', ?)", (a.cache_kib,))
        if a.parallel is not None:
            db.execute(f"INSERT INTO {a.table}({a.table}, rank) VALUES('parallel', ?)", (a.parallel,))
    trace = {}
    with open(a.trace, encoding='utf-8') as f:
        for line in f:
            sub, mode, terms = line.rstrip('\n').split('\t')
            trace.setdefault(mode, []).append(terms.split(' '))
    top, cnt_native, cnt_match = statements(a.engine, a.table)
    load1 = os.getloadavg()[0]
    print('engine\tclass\tn\tmedian_ms\tp95_ms\tqps\ttotal_s\tload1')
    for cls in a.classes.split(','):
        mode = cls.replace('count_', '')
        sql = top if not cls.startswith('count_') else cnt_native
        subs = [t for t in trace[mode] if not a.bare or all(w.isalnum() for w in t)]
        qs = [query_text(a.engine, mode, t, a.bare) for t in subs[:a.n]]
        for q in qs:
            db.execute(sql, (q,)).fetchall()
        lat = []
        t0 = time.perf_counter()
        for q in qs:
            t = time.perf_counter()
            db.execute(sql, (q,)).fetchall()
            lat.append(time.perf_counter() - t)
        total = time.perf_counter() - t0
        lat.sort()
        print(f'{a.engine}\t{cls}\t{len(qs)}\t{lat[len(lat) // 2] * 1e3:.4f}\t{lat[int(len(lat) * 0.95)] * 1e3:.4f}\t'
              f'{len(qs) / total:.1f}\t{total:.3f}\t{load1:.2f}')
        sys.stdout.flush()


if __name__ == '__main__':
    main()
