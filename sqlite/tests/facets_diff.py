"""sift_facets against GROUP BY on a table with many values: several segments, deleted documents, ties, every bound of the answer.

    python3 -I tests/facets_diff.py [ROWS [VALUES]]        (needs build/sift.so; prints the time of the bounded answers)
"""
import os
import random
import sqlite3
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sift import EXT, ids  # noqa: E402

ROWS = int(sys.argv[1]) if len(sys.argv) > 1 else 20000
VALUES = int(sys.argv[2]) if len(sys.argv) > 2 else 3000
QUERIES = ['w1', 'w1 OR w2', 'w3 AND w4', 'w5*', 'w7 NOT w8', 'w9 OR w10 OR w11', '"w12 w13"', 'w999']


def main():
    path = os.path.join(tempfile.mkdtemp(prefix='sift-opt66b-facets-'), 'f.db')
    db = sqlite3.connect(path)
    db.enable_load_extension(True)
    db.load_extension(EXT)
    db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none)')
    db.execute("CREATE VIRTUAL TABLE cat USING sift(c, analyzer='keyword', stemmer=none, positions=0)")
    db.execute('CREATE TABLE plain(id INTEGER PRIMARY KEY, c TEXT)')
    rnd = random.Random(11)
    words = ['w%d' % i for i in range(40)]
    values = ['v%04d' % i for i in range(VALUES)]
    for batch in range(4):
        db.execute('BEGIN')
        for i in range(batch * ROWS // 4 + 1, (batch + 1) * ROWS // 4 + 1):
            c = values[min(int(rnd.expovariate(1.0 / (VALUES / 6))), VALUES - 1)] if i % 17 else None
            db.execute('INSERT INTO t(rowid, body) VALUES (?, ?)', (i, ' '.join(rnd.choice(words) for _ in range(8))))
            db.execute('INSERT INTO plain VALUES (?, ?)', (i, c))
            if c:
                db.execute('INSERT INTO cat(rowid, c) VALUES (?, ?)', (i, c))
        db.execute('COMMIT')
    db.execute('DELETE FROM cat WHERE rowid % 13 = 0')
    db.execute('UPDATE plain SET c = NULL WHERE id % 13 = 0')
    bad = 0
    spent = 0.0
    for q in QUERIES:
        matched = ids(db, q)
        db.execute('CREATE TEMP TABLE IF NOT EXISTS m(id INTEGER PRIMARY KEY)')
        db.execute('DELETE FROM m')
        db.executemany('INSERT INTO m VALUES (?)', [(i,) for i in matched])
        full = db.execute('SELECT c, count(*) n FROM plain JOIN m ON m.id = plain.id WHERE c IS NOT NULL GROUP BY c ORDER BY n DESC, c').fetchall()
        nulls = db.execute('SELECT count(*) FROM plain JOIN m ON m.id = plain.id WHERE c IS NULL').fetchone()[0]
        for lim in (None, 0, 1, 5, 10, 100, 100000):
            for with_nulls in (0, 1):
                args = (q,) if lim is None and not with_nulls else (q, -1 if lim is None else lim, with_nulls)
                sql = 'SELECT value, n FROM sift_facets(\'t\', \'cat\', ?' + (', ?, ?' if len(args) == 3 else '') + ')'
                t0 = time.perf_counter()
                got = db.execute(sql, args).fetchall()
                if lim == 10 and not with_nulls:
                    spent += time.perf_counter() - t0
                want = full if lim is None else full[:lim]
                if with_nulls:
                    want = want + ([(None, nulls)] if nulls else [])
                if got != want:
                    bad += 1
                    print('FAIL', q, lim, with_nulls, got[:3], want[:3])
    print('facets_diff: %d queries x 14 forms, %d wrong; top-10 answers took %.1f ms in all' % (len(QUERIES), bad, spent * 1000))
    return bad


if __name__ == '__main__':
    sys.exit(1 if main() else 0)
