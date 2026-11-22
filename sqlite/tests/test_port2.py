"""Tests of the write path and caches that the large-table work changed: batched deletes, the statements kept
for DML and queries across a rename, repeated queries over a bounded segment cache, facets after deletes, and
quantifiers of empty bodies.

    python3 -I tests/test_port2.py [-v]           (needs build/sift.so)
"""
import os
import random
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_sift import TmpDb, connect, ids, top  # noqa: E402

WORDS = ['w%d' % i for i in range(120)]


def fill(db, n, seed=3, table='t'):
    rnd = random.Random(seed)
    db.execute('BEGIN')
    for i in range(1, n + 1):
        db.execute(f'INSERT INTO {table}(rowid, body) VALUES (?, ?)', (i, ' '.join(rnd.choice(WORDS) for _ in range(30))))
    db.execute('COMMIT')


def answers(db, table='t'):
    return [(ids(db, q, table), top(db, q, 15, table)) for q in ('w1', 'w2 AND w3', 'w4 OR w5', '"w6 w7"', 'w8*')]


class BatchedDeletes(TmpDb):
    def test_one_transaction_of_deletes_equals_one_by_one(self):
        gone = random.Random(9).sample(range(1, 2001), 600)
        outcomes = []
        for batched in (True, False):
            db = connect(os.path.join(self.dir.name, f'{batched}.db'))
            db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none)')
            fill(db, 2000)
            if batched:
                db.execute('BEGIN')
            for rowid in gone:
                db.execute('DELETE FROM t WHERE rowid = ?', (rowid,))
            if batched:
                db.execute('COMMIT')
            outcomes.append(answers(db))
            self.assertEqual(db.execute('SELECT count(*) FROM t').fetchone()[0], 1400)
            db.execute("INSERT INTO t(t, rank) VALUES ('integrity-check', 1)")
        self.assertEqual(outcomes[0], outcomes[1])

    def test_delete_then_reinsert_in_one_transaction(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none)')
        fill(db, 500)
        db.execute('BEGIN')
        for rowid in range(1, 201):
            db.execute('DELETE FROM t WHERE rowid = ?', (rowid,))
            db.execute('INSERT INTO t(rowid, body) VALUES (?, ?)', (rowid, 'fresh w1 token%d' % rowid))
        db.execute('COMMIT')
        self.assertEqual(ids(db, 'fresh'), list(range(1, 201)))
        self.assertEqual(db.execute("SELECT sift_count('t', 'fresh')").fetchone()[0], 200)
        db.execute("INSERT INTO t(t, rank) VALUES ('integrity-check', 1)")


class StatementsAcrossDdl(TmpDb):
    def test_dml_and_queries_follow_a_rename_and_a_new_table_of_the_old_name(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none)')
        db.execute("INSERT INTO t(rowid, body) VALUES (1, 'alpha beta')")
        self.assertEqual(ids(db, 'alpha'), [1])
        db.execute('UPDATE t SET body = ? WHERE rowid = 1', ('alpha gamma',))
        db.execute('ALTER TABLE t RENAME TO u')
        db.execute("INSERT INTO u(rowid, body) VALUES (2, 'alpha delta')")
        db.execute('DELETE FROM u WHERE rowid = 1')
        self.assertEqual(ids(db, 'alpha', 'u'), [2])
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none)')
        db.execute("INSERT INTO t(rowid, body) VALUES (7, 'alpha only here')")
        self.assertEqual(ids(db, 'alpha'), [7])
        self.assertEqual(ids(db, 'alpha', 'u'), [2])
        db.execute('DROP TABLE u')
        self.assertEqual(ids(db, 'alpha'), [7])

    def test_repeated_queries_agree_when_the_table_changes_between_them(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none)')
        fill(db, 800)
        first = answers(db)
        for _ in range(3):
            self.assertEqual(answers(db), first)
        db.execute('DELETE FROM t WHERE rowid <= 100')
        after = answers(db)
        self.assertTrue(all(r > 100 for ids_, _ in after for r in ids_))
        db.execute("INSERT INTO t(t) VALUES ('merge')")
        self.assertEqual([a[0] for a in answers(db)], [a[0] for a in after])


class Facets(TmpDb):
    def test_facets_after_deletes_and_updates_equal_group_by(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none)')
        db.execute("CREATE VIRTUAL TABLE cat USING sift(c, analyzer='keyword', stemmer=none, positions=0)")
        db.execute('CREATE TABLE plain(id INTEGER PRIMARY KEY, body TEXT, c TEXT)')
        rnd = random.Random(5)
        db.execute('BEGIN')
        for i in range(1, 3001):
            body, c = ' '.join(rnd.choice(WORDS) for _ in range(20)), 'c%d' % rnd.randrange(40)
            db.execute('INSERT INTO t(rowid, body) VALUES (?, ?)', (i, body))
            db.execute('INSERT INTO plain VALUES (?, ?, ?)', (i, body, c))
            db.execute('INSERT INTO cat(rowid, c) VALUES (?, ?)', (i, c))
        db.execute('COMMIT')
        for table in ('t', 'cat', 'plain'):
            db.execute(f'DELETE FROM {table} WHERE {"id" if table == "plain" else "rowid"} % 7 = 0')
        for q in ('w1', 'w2 OR w3', 'w4 AND w5', 'w6*'):
            matched = ids(db, q)
            want = db.execute(f"SELECT c, count(*) n FROM plain WHERE id IN ({','.join(map(str, matched)) or 'NULL'}) "
                              "GROUP BY c ORDER BY n DESC, c").fetchall()
            got = db.execute("SELECT value, n FROM sift_facets('t', 'cat', ?, -1)", (q,)).fetchall()
            self.assertEqual(sorted(got), sorted(want), q)
        top3 = db.execute("SELECT value, n FROM sift_facets('t', 'cat', 'w1 OR w2', 3)").fetchall()
        self.assertEqual(len(top3), 3)
        self.assertEqual([n for _, n in top3], sorted((n for _, n in top3), reverse=True))


class Quantifiers(TmpDb):
    def test_nested_empty_quantifiers_compile_at_once(self):
        db = connect()
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none)')
        db.execute("INSERT INTO t(rowid, body) VALUES (1, 'abc'), (2, 'abd')")
        self.assertEqual(ids(db, '/(?:(?:(?:(?:){255}){255}){255}){255}ab./'), [1, 2])
        self.assertEqual(ids(db, '/(?:(?:(?:(?:){255}){255}){255}){255}abc/'), [1])


if __name__ == '__main__':
    unittest.main()
