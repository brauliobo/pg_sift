"""Write-path check and timing on a built index: insert / update / delete / rollback / merge / reopen at corpus scale.

    python3 -I dml.py DB [--ext build/sift] [--batch 1000]

DB is a file built by build.py --engine sift (table ft, source table articles); it is modified, use a copy. Every step
asserts the counts it should produce (marker words that exist in no article) and prints its time.
"""
import argparse
import os
import sqlite3
import time


def timed(label, f):
    t = time.perf_counter()
    r = f()
    print(f'{label}: {time.perf_counter() - t:.3f}s')
    return r


def connect(path, ext):
    db = sqlite3.connect(path, isolation_level=None)
    db.enable_load_extension(True)
    db.load_extension(ext)
    return db


def count(db, q):
    return db.execute("SELECT sift_count('ft', ?)", (q,)).fetchone()[0]


def check(label, got, want):
    assert got == want, f'{label}: got {got}, want {want}'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('db')
    ap.add_argument('--ext', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'sift'))
    ap.add_argument('--batch', type=int, default=1000)
    a = ap.parse_args()
    n = a.batch
    db = connect(a.db, a.ext)
    base = count(db, 'river')
    top = db.execute('SELECT max(id) FROM articles').fetchone()[0]
    ids = [r[0] for r in db.execute('SELECT id FROM articles ORDER BY id LIMIT ?', (n,))]

    def insert():
        db.execute('BEGIN')
        db.executemany('INSERT INTO ft(rowid, body) VALUES (?, ?)', ((top + 1 + i, f'zqins river doc {i}') for i in range(n)))
        db.execute('COMMIT')
    timed(f'insert {n} docs', insert)
    check('insert', (count(db, 'zqins'), count(db, 'river')), (n, base + n))

    db.execute('BEGIN')
    db.executemany('INSERT INTO ft(rowid, body) VALUES (?, ?)', ((top + 1 + n + i, 'zqrolled river') for i in range(n)))
    db.execute('ROLLBACK')
    check('rollback', (count(db, 'zqrolled'), count(db, 'river')), (0, base + n))

    def update():
        db.execute('BEGIN')
        db.executemany('UPDATE ft SET body = ? WHERE rowid = ?', ((f'zqupd river {i}', top + 1 + i) for i in range(n)))
        db.execute('COMMIT')
    timed(f'update {n} docs', update)
    check('update', (count(db, 'zqins'), count(db, 'zqupd')), (0, n))

    def delete():
        db.execute('BEGIN')
        db.executemany('DELETE FROM ft WHERE rowid = ?', ((top + 1 + i,) for i in range(n)))
        db.execute('COMMIT')
    timed(f'delete {n} docs', delete)
    check('delete', (count(db, 'zqupd'), count(db, 'river')), (0, base))

    before = count(db, 'the OR city')
    db.execute('BEGIN')
    db.executemany('DELETE FROM ft WHERE rowid = ?', ((i,) for i in ids))
    db.execute('COMMIT')
    after = (count(db, 'the OR city'), count(db, 'river'))
    assert after[0] < before
    timed('merge', lambda: db.execute("INSERT INTO ft(ft) VALUES('merge')"))
    segs = db.execute('SELECT count(*) FROM ft_segs').fetchone()[0]
    check('merge keeps matches', (count(db, 'the OR city'), count(db, 'river')), after)
    db.close()
    db = timed('reopen', lambda: connect(a.db, a.ext))
    check('reopen', (count(db, 'the OR city'), count(db, 'river')), after)
    after = after[0]
    print(f'ok: segments after merge {segs}, matches {before} -> {after} after deleting {len(ids)} articles')


main()
