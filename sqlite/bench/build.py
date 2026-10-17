"""Builds an index over the `articles(id, body)` table of BASE_DB and reports build time and index size.

    python3 -I build.py --engine sift|fts5 --base BASE_DB --out OUT_DB [--opts 'stemmer=none,positions=0'] [--detail full|none]
                        [--no-merge] [--cpu N] [--ext build/sift]

OUT_DB starts as a copy of BASE_DB (the articles table stays; both engines index it from there).
sift: CREATE VIRTUAL TABLE ft USING sift(body, <opts>) + INSERT ... SELECT, then merge to one segment (timed apart).
fts5: external content table, tokenize='porter unicode61 remove_diacritics 2' (--detail none drops positions).
Prints one TSV row: engine opts build_s merge_s bytes segments peak_rss_mib.
"""
import argparse
import os
import resource
import shutil
import sqlite3
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--engine', required=True, choices=['sift', 'fts5'])
    ap.add_argument('--base', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--opts', default='')
    ap.add_argument('--detail', default='full')
    ap.add_argument('--no-merge', action='store_true')
    ap.add_argument('--cpu', type=int)
    ap.add_argument('--ext', default=os.path.join(os.path.dirname(__file__), '..', 'build', 'sift'))
    a = ap.parse_args()
    if a.cpu is not None:
        os.sched_setaffinity(0, {a.cpu})
    if os.path.exists(a.out):
        os.remove(a.out)
    shutil.copyfile(a.base, a.out)
    db = sqlite3.connect(a.out)
    db.enable_load_extension(True)
    db.load_extension(a.ext)
    db.execute('PRAGMA journal_mode = wal')
    db.execute('PRAGMA cache_size = -1048576')
    opts = ', ' + a.opts if a.opts else ''
    t0 = time.perf_counter()
    if a.engine == 'sift':
        db.execute(f'CREATE VIRTUAL TABLE ft USING sift(body{opts})')
    else:
        db.execute("CREATE VIRTUAL TABLE ft USING fts5(body, content='articles', content_rowid='id', "
                   f"detail={a.detail}, tokenize='porter unicode61 remove_diacritics 2')")
    db.execute('INSERT INTO ft(rowid, body) SELECT id, body FROM articles')
    db.commit()
    build = time.perf_counter() - t0
    t0 = time.perf_counter()
    if a.engine == 'sift':
        if not a.no_merge:
            db.execute("INSERT INTO ft(ft) VALUES('optimize')")
            db.commit()
        segs = db.execute('SELECT count(*) FROM ft_segs').fetchone()[0]
        size = db.execute("SELECT sum(pgsize) FROM dbstat WHERE name IN ('ft_chunks', 'ft_segs', 'ft_meta', 'ft_config')").fetchone()[0]
    else:
        db.execute("INSERT INTO ft(ft) VALUES('optimize')")
        db.commit()
        segs = 1
        size = db.execute("SELECT sum(pgsize) FROM dbstat WHERE name = 'ft_data'").fetchone()[0]
    merge = time.perf_counter() - t0
    rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss // 1024
    print('engine\topts\tbuild_s\tmerge_s\tbytes\tsegments\tpeak_rss_mib')
    print(f'{a.engine}\t{a.opts or a.detail}\t{build:.2f}\t{merge:.2f}\t{size}\t{segs}\t{rss}')


main()
