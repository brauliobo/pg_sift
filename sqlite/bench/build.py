"""Builds an index over the `articles(id, body)` table of BASE_DB and reports build time and index size.

    python3 -I build.py --engine sift|fts5 --base BASE_DB --out OUT_DB [--opts 'stemmer=none,positions=0'] [--detail full|none]
                        [--no-merge] [--ext build/sift] [--page-size N] [--sqlite-cache-mb N]

OUT_DB starts as a copy of BASE_DB (the articles table stays; both engines index it from there); with --page-size it
starts empty (sift only) with that SQLite page size and the articles are read through ATTACH.
sift: CREATE VIRTUAL TABLE ft USING sift(body, <opts>) + INSERT ... SELECT, then merge to one segment (timed apart).
fts5: external content table, tokenize='porter unicode61 remove_diacritics 2' (--detail none drops positions).
Prints one TSV row: engine opts build_s merge_s bytes segments peak_rss_mib content_bytes (bytes: the index; content_bytes:
the sift table's own copy of the text, 0 for content='tbl' and content='').
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
    ap.add_argument('--page-size', type=int)
    ap.add_argument('--sqlite-cache-mb', type=int, default=1024, help="SQLite's own page cache while building (counts in the peak RSS)")
    ap.add_argument('--ext', default=os.path.join(os.path.dirname(__file__), '..', 'build', 'sift'))
    a = ap.parse_args()
    if os.path.exists(a.out):
        os.remove(a.out)
    if a.page_size:
        db = sqlite3.connect(a.out)
        db.execute(f'PRAGMA page_size = {a.page_size}')
        db.execute(f"ATTACH '{a.base}' AS src")
    else:
        shutil.copyfile(a.base, a.out)
        db = sqlite3.connect(a.out)
    db.enable_load_extension(True)
    db.load_extension(a.ext)
    db.execute('PRAGMA journal_mode = wal')
    db.execute(f'PRAGMA cache_size = -{a.sqlite_cache_mb * 1024}')
    opts = ', ' + a.opts if a.opts else ''
    t0 = time.perf_counter()
    if a.engine == 'sift':
        db.execute(f'CREATE VIRTUAL TABLE ft USING sift(body{opts})')
    else:
        db.execute("CREATE VIRTUAL TABLE ft USING fts5(body, content='articles', content_rowid='id', "
                   f"detail={a.detail}, tokenize='porter unicode61 remove_diacritics 2')")
    db.execute(f"INSERT INTO ft(rowid, body) SELECT id, body FROM {'src.' if a.page_size else ''}articles")
    db.commit()
    build = time.perf_counter() - t0
    t0 = time.perf_counter()
    if a.engine == 'sift':
        if not a.no_merge:
            db.execute("INSERT INTO ft(ft) VALUES('optimize')")
            db.commit()
        segs = db.execute('SELECT count(*) FROM ft_segs').fetchone()[0]
        size = db.execute("SELECT sum(pgsize) FROM dbstat WHERE name IN ('ft_chunks', 'ft_segs', 'ft_meta', 'ft_config')").fetchone()[0]
        content = db.execute("SELECT coalesce(sum(pgsize), 0) FROM dbstat WHERE name = 'ft_content'").fetchone()[0]
    else:
        db.execute("INSERT INTO ft(ft) VALUES('optimize')")
        db.commit()
        segs = 1
        content = 0
        size = db.execute("SELECT sum(pgsize) FROM dbstat WHERE name = 'ft_data'").fetchone()[0]
    merge = time.perf_counter() - t0
    rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss // 1024
    print('engine\topts\tbuild_s\tmerge_s\tbytes\tsegments\tpeak_rss_mib\tcontent_bytes')
    print(f'{a.engine}\t{a.opts or a.detail}\t{build:.2f}\t{merge:.2f}\t{size}\t{segs}\t{rss}\t{content}')


main()
