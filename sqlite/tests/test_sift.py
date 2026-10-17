"""Functional tests of the sift SQLite extension: DML, transactions, merge, reopen, queries, errors, and an oracle
(a brute-force BM25 over the same documents) for match sets and scores.

    python3 -I tests/test_sift.py [-v]            (needs build/sift.so)
"""
import math
import os
import random
import re
import sqlite3
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
EXT = os.environ.get('SIFT_EXT', os.path.join(HERE, '..', 'build', 'sift'))


def connect(path=':memory:'):
    db = sqlite3.connect(path, isolation_level=None)
    db.enable_load_extension(True)
    db.load_extension(EXT)
    unittest.addModuleCleanup(db.close)
    return db


def ids(db, q, table='t'):
    return [r[0] for r in db.execute(f'SELECT rowid FROM {table} WHERE {table} MATCH ? ORDER BY rowid', (q,))]


def top(db, q, k=10, table='t'):
    return [r[0] for r in db.execute(f'SELECT rowid FROM {table} WHERE {table} MATCH ? ORDER BY rank LIMIT ?', (q, k))]


def segs(db, table='t'):
    return db.execute(f'SELECT count(*) FROM {table}_segs').fetchone()[0]


class TmpDb(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.dir.name, 'x.db')

    def tearDown(self):
        self.dir.cleanup()


class Basics(TmpDb):
    def test_insert_search_update_delete(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none)')
        db.execute("INSERT INTO t(rowid, body) VALUES (1, 'the quick brown fox'), (2, 'a quick red fox'), (3, 'lazy dogs sleep')")
        self.assertEqual(ids(db, 'quick'), [1, 2])
        self.assertEqual(ids(db, 'quick AND red'), [2])
        self.assertEqual(ids(db, 'quick NOT red'), [1])
        self.assertEqual(ids(db, '"quick brown"'), [1])
        self.assertEqual(ids(db, '"brown quick"'), [])
        self.assertEqual(ids(db, 'qui*'), [1, 2])
        self.assertEqual(ids(db, 'f?x'), [1, 2])
        self.assertEqual(ids(db, 'dogz~1'), [3])
        self.assertEqual(db.execute("SELECT sift_count('t', 'quick OR lazy')").fetchone()[0], 3)
        db.execute("UPDATE t SET body = 'now it is lazy' WHERE rowid = 2")
        self.assertEqual(ids(db, 'quick'), [1])
        self.assertEqual(ids(db, 'lazy'), [2, 3])
        db.execute('DELETE FROM t WHERE rowid = 3')
        self.assertEqual(ids(db, 'lazy'), [2])
        self.assertEqual(db.execute("SELECT sift_count('t', 'lazy')").fetchone()[0], 1)
        self.assertEqual(db.execute('SELECT body FROM t WHERE rowid = 2').fetchone()[0], 'now it is lazy')
        self.assertEqual(db.execute('SELECT count(*) FROM t').fetchone()[0], 2)

    def test_rank_is_negated_bm25_and_ordered(self):
        db = connect()
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none)')
        db.executemany('INSERT INTO t(rowid, body) VALUES (?, ?)',
                       [(1, 'fox fox fox'), (2, 'fox'), (3, 'fox and many other words in this one'), (4, 'dog')])
        r = db.execute("SELECT rowid, rank FROM t WHERE t MATCH 'fox' ORDER BY rank").fetchall()
        self.assertEqual([x[0] for x in r], [1, 2, 3])
        self.assertTrue(all(x[1] < 0 for x in r) and r[0][1] < r[1][1] < r[2][1])
        self.assertEqual([x[0] for x in db.execute("SELECT rowid FROM t WHERE t MATCH 'fox' ORDER BY rank LIMIT 2 OFFSET 1")], [2, 3])

    def test_columns_and_fields(self):
        db = connect()
        db.execute('CREATE VIRTUAL TABLE t USING sift(title, body, stemmer=none)')
        db.execute("INSERT INTO t(rowid, title, body) VALUES (1, 'alpha', 'beta gamma'), (2, 'beta', 'alpha gamma')")
        self.assertEqual(ids(db, 'alpha'), [1, 2])
        self.assertEqual(ids(db, 'title:alpha'), [1])
        self.assertEqual(top(db, 'alpha OR (title:alpha)^5')[0], 1)
        self.assertEqual(db.execute('SELECT title, body FROM t WHERE rowid = 2').fetchone(), ('beta', 'alpha gamma'))

    def test_full_scan_and_rowid_lookup(self):
        db = connect()
        db.execute('CREATE VIRTUAL TABLE t USING sift(body)')
        db.executemany('INSERT INTO t(rowid, body) VALUES (?, ?)', [(i, f'doc {i}') for i in (5, 3, 9)])
        self.assertEqual([r[0] for r in db.execute('SELECT rowid FROM t')], [3, 5, 9])
        self.assertEqual(db.execute('SELECT body FROM t WHERE rowid = 5').fetchone()[0], 'doc 5')

    def test_auto_rowid(self):
        db = connect()
        db.execute('CREATE VIRTUAL TABLE t USING sift(body)')
        db.execute("INSERT INTO t(body) VALUES ('first')")
        db.execute("INSERT INTO t(body) VALUES ('second')")
        self.assertEqual(ids(db, 'second'), [2])

    def test_rowid_limits(self):
        db = connect()
        db.execute('CREATE VIRTUAL TABLE t USING sift(body)')
        big = (1 << 37) - 1
        db.execute("INSERT INTO t(rowid, body) VALUES (?, 'edge')", (big,))
        db.execute("INSERT INTO t(rowid, body) VALUES (0, 'edge')")
        self.assertEqual(ids(db, 'edge'), [0, big])
        for bad in (1 << 37, -1):
            with self.assertRaises(sqlite3.Error):
                db.execute("INSERT INTO t(rowid, body) VALUES (?, 'x')", (bad,))

    def test_errors(self):
        db = connect()
        db.execute('CREATE VIRTUAL TABLE t USING sift(body)')
        db.execute("INSERT INTO t(rowid, body) VALUES (1, 'x')")
        for q in ('AND', '(', '"unterminated', 'NOT x'):
            with self.assertRaises(sqlite3.Error, msg=q):
                ids(db, q)
        with self.assertRaises(sqlite3.Error):
            db.execute("INSERT INTO t(t) VALUES ('nonsense')")
        self.assertEqual(ids(db, 'x'), [1])

    def test_regex_and_wildcard(self):
        db = connect()
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none)')
        db.executemany('INSERT INTO t(rowid, body) VALUES (?, ?)', [(1, 'river'), (2, 'rover'), (3, 'rivet'), (4, 'history')])
        self.assertEqual(ids(db, '/r[io]ver/'), [1, 2])
        self.assertEqual(ids(db, 'r?v*'), [1, 2, 3])
        self.assertEqual(ids(db, '/h.*y/'), [4])

    def test_case_accents_unicode(self):
        db = connect()
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none)')
        db.execute("INSERT INTO t(rowid, body) VALUES (1, 'Café MÜNCHEN straße'), (2, '東京 tokyo')")
        self.assertEqual(ids(db, 'cafe'), [1])
        self.assertEqual(ids(db, 'munchen'), [1])
        self.assertEqual(ids(db, 'strasse'), [1])
        self.assertEqual(ids(db, '東'), [2])

    def test_stemmer(self):
        db = connect()
        db.execute('CREATE VIRTUAL TABLE t USING sift(body)')
        db.execute("INSERT INTO t(rowid, body) VALUES (1, 'running dogs'), (2, 'he runs')")
        self.assertEqual(ids(db, 'run'), [1, 2])
        self.assertEqual(ids(db, 'dog'), [1])


class Transactions(TmpDb):
    def test_rollback_and_commit(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none)')
        db.execute("INSERT INTO t(rowid, body) VALUES (1, 'kept document')")
        db.execute('BEGIN')
        db.execute("INSERT INTO t(rowid, body) VALUES (2, 'rolled back document')")
        db.execute("DELETE FROM t WHERE rowid = 1")
        self.assertEqual(ids(db, 'document'), [2])
        db.execute('ROLLBACK')
        self.assertEqual(ids(db, 'document'), [1])
        self.assertEqual(db.execute('SELECT count(*) FROM t').fetchone()[0], 1)
        db.execute('BEGIN')
        db.execute("INSERT INTO t(rowid, body) VALUES (3, 'committed document')")
        db.execute('COMMIT')
        self.assertEqual(ids(db, 'document'), [1, 3])
        db.close()
        db = connect(self.path)
        self.assertEqual(ids(db, 'document'), [1, 3])
        self.assertEqual(top(db, '"committed document"'), [3])

    def test_rollback_of_a_pending_delete_and_segment_numbers(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none)')
        db.executemany('INSERT INTO t(rowid, body) VALUES (?, ?)', [(i, f'alpha{i % 3} beta') for i in range(1, 100)])
        n = len(ids(db, 'beta'))
        for _ in range(3):
            db.execute('BEGIN')
            db.execute("DELETE FROM t WHERE rowid < 50")
            db.execute("INSERT INTO t(rowid, body) VALUES (1000, 'beta beta')")
            self.assertEqual(len(ids(db, 'beta')), n - 49 + 1)
            db.execute('ROLLBACK')
            self.assertEqual(len(ids(db, 'beta')), n)
        self.assertEqual(db.execute("SELECT sift_count('t', 'beta')").fetchone()[0], n)

    def test_savepoints(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none)')
        db.execute('BEGIN')
        db.execute("INSERT INTO t(rowid, body) VALUES (1, 'one')")
        db.execute('SAVEPOINT s1')
        db.execute("INSERT INTO t(rowid, body) VALUES (2, 'two one')")
        self.assertEqual(ids(db, 'one'), [1, 2])
        db.execute('SAVEPOINT s2')
        db.execute("DELETE FROM t WHERE rowid = 1")
        db.execute('ROLLBACK TO s2')
        self.assertEqual(ids(db, 'one'), [1, 2])
        db.execute('ROLLBACK TO s1')
        self.assertEqual(ids(db, 'one'), [1])
        db.execute("INSERT INTO t(rowid, body) VALUES (3, 'three one')")
        db.execute('RELEASE s1')
        db.execute('COMMIT')
        self.assertEqual(ids(db, 'one'), [1, 3])

    def test_statement_rollback_on_error(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none)')
        db.execute("INSERT INTO t(rowid, body) VALUES (1, 'a')")
        with self.assertRaises(sqlite3.Error):
            db.execute("INSERT INTO t(rowid, body) VALUES (2, 'b'), (-5, 'c')")
        self.assertEqual(ids(db, 'b'), [])
        self.assertEqual(db.execute('SELECT count(*) FROM t').fetchone()[0], 1)

    def test_two_connections_wal(self):
        w = connect(self.path)
        w.execute('PRAGMA journal_mode=wal')
        w.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none)')
        w.execute("INSERT INTO t(rowid, body) VALUES (1, 'old')")
        r = connect(self.path)
        self.assertEqual(ids(r, 'old'), [1])
        w.execute('BEGIN')
        w.execute("INSERT INTO t(rowid, body) VALUES (2, 'old new')")
        w.execute('DELETE FROM t WHERE rowid = 1')
        self.assertEqual(ids(r, 'old'), [1])
        w.execute('COMMIT')
        self.assertEqual(ids(r, 'old'), [2])
        self.assertEqual(r.execute("SELECT sift_count('t', 'old')").fetchone()[0], 1)
        w.execute('BEGIN')
        w.execute("INSERT INTO t(rowid, body) VALUES (3, 'old again')")
        w.execute('ROLLBACK')
        w.execute("INSERT INTO t(rowid, body) VALUES (3, 'different')")
        self.assertEqual(ids(r, 'old'), [2])
        self.assertEqual(ids(r, 'different'), [3])
        self.assertEqual(ids(r, 'again'), [])

    def test_many_statements_in_a_transaction_merge_automatically(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none)')
        db.execute('BEGIN')
        for i in range(1, 60):
            db.execute('INSERT INTO t(rowid, body) VALUES (?, ?)', (i, f'word{i} common'))
            if i % 3 == 0:
                ids(db, 'common')
        db.execute('COMMIT')
        self.assertLess(segs(db), 16)
        self.assertEqual(len(ids(db, 'common')), 59)
        self.assertEqual(ids(db, 'word17'), [17])


class MergeAndReopen(TmpDb):
    def test_merge_keeps_results_and_purges_deletes(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none)')
        rnd = random.Random(3)
        words = ['w%d' % i for i in range(60)]
        for chunk in range(5):
            db.execute('BEGIN')
            for i in range(chunk * 200 + 1, chunk * 200 + 201):
                db.execute('INSERT INTO t(rowid, body) VALUES (?, ?)', (i, ' '.join(rnd.choice(words) for _ in range(12))))
            db.execute('COMMIT')
        db.execute('DELETE FROM t WHERE rowid % 7 = 0')
        self.assertEqual(segs(db), 5)
        queries = ['w1', 'w1 AND w2', 'w3 OR w4 OR w5', '"w6 w7"', 'w1 NOT w2', 'w1*', 'w2 w3^3']
        before = {q: (ids(db, q), top(db, q, 20), db.execute("SELECT sift_count('t', ?)", (q,)).fetchone()[0]) for q in queries}
        db.execute("INSERT INTO t(t) VALUES ('merge')")
        self.assertEqual(segs(db), 1)
        after = {q: (ids(db, q), top(db, q, 20), db.execute("SELECT sift_count('t', ?)", (q,)).fetchone()[0]) for q in queries}
        for q in queries:
            self.assertEqual(before[q][0], after[q][0], q)
            self.assertEqual(before[q][2], after[q][2], q)
            self.assertEqual(len(before[q][0]), before[q][2], q)
        self.assertEqual(db.execute('SELECT count(*) FROM t').fetchone()[0], 1000 - len(range(7, 1001, 7)))
        db.close()
        db = connect(self.path)
        self.assertEqual(ids(db, 'w1 AND w2'), after['w1 AND w2'][0])
        self.assertEqual(top(db, '"w6 w7"', 20), after['"w6 w7"'][1])

    def test_merge_roundtrip_positions(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none)')
        db.execute("INSERT INTO t(rowid, body) VALUES (1, 'a b c d'), (2, 'c b a')")
        db.execute("INSERT INTO t(rowid, body) VALUES (40, 'b c a b')")
        self.assertEqual(ids(db, '"b c"'), [1, 40])
        db.execute("INSERT INTO t(t) VALUES ('merge')")
        self.assertEqual(ids(db, '"b c"'), [1, 40])
        self.assertEqual(ids(db, '"c b"'), [2])
        self.assertEqual(ids(db, '"a b"'), [1, 40])

    def test_rebuild_and_flush_commands(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none)')
        db.executemany('INSERT INTO t(rowid, body) VALUES (?, ?)', [(i, f'term{i % 4} x') for i in range(1, 50)])
        db.execute("INSERT INTO t(t) VALUES ('rebuild')")
        self.assertEqual(segs(db), 1)
        self.assertEqual(len(ids(db, 'term1')), 13)
        db.execute("INSERT INTO t(t) VALUES ('flush')")

    def test_without_positions(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none, positions=0)')
        db.execute("INSERT INTO t(rowid, body) VALUES (1, 'a b c'), (2, 'c b a')")
        self.assertEqual(ids(db, '"a b"'), [1])
        self.assertEqual(top(db, '"b a"'), [2])
        self.assertEqual(db.execute("SELECT sift_count('t', '\"a b\"')").fetchone()[0], 1)

    def test_drop_and_rename(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body)')
        db.execute("INSERT INTO t(rowid, body) VALUES (1, 'hello world')")
        db.execute('ALTER TABLE t RENAME TO u')
        self.assertEqual(ids(db, 'hello', 'u'), [1])
        db.close()
        db = connect(self.path)
        self.assertEqual(ids(db, 'hello', 'u'), [1])
        db.execute('DROP TABLE u')
        left = [r[0] for r in db.execute("SELECT name FROM sqlite_master WHERE name LIKE 'u%' OR name LIKE 't%'")]
        self.assertEqual(left, [])

    def test_read_modes_agree(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body)')
        db.executemany('INSERT INTO t(rowid, body) VALUES (?, ?)', [(i, f'term{i % 9} text{i % 5}') for i in range(1, 3000)])
        db.close()
        res = []
        for mode in (0, 1):
            db = connect(self.path)
            db.execute('SELECT sift_read_mode(?)', (mode,)).fetchall()
            res.append(top(db, 'term1 text2', 50))
        self.assertEqual(res[0], res[1])


# ---------- oracle: brute-force BM25 over the same documents ----------

TOKEN = re.compile(r'[^\W_]+', re.UNICODE)


class Oracle:
    """Documents are bags of lowercase words (no stemmer). k1 = 1.2, b = 0.75, scores as DESIGN.md (a)."""

    def __init__(self, docs, k1=1.2, b=0.75):
        self.docs = {i: [w.lower() for w in TOKEN.findall(t)] for i, t in docs.items()}
        self.k1, self.b = k1, b
        self.n = len(self.docs)
        self.avgdl = sum(min(len(t), 65535) for t in self.docs.values()) / self.n
        self.df = {}
        for t in self.docs.values():
            for w in set(t):
                self.df[w] = self.df.get(w, 0) + 1

    def idf(self, w):
        df = self.df.get(w, 0)
        return math.log(1 + (self.n - df + 0.5) / (df + 0.5))

    def term_score(self, i, w):
        t = self.docs[i]
        tf = min(t.count(w), 255)
        if tf == 0:
            return None
        k = self.k1 * (1 - self.b + self.b * len(t) / self.avgdl)
        return self.idf(w) * (self.k1 + 1) * tf / (tf + k)

    def has_phrase(self, i, ws):
        t = self.docs[i]
        return any(t[k:k + len(ws)] == ws for k in range(len(t) - len(ws) + 1))

    # q: ('t', w) | ('p', [w..]) | ('and', [q..]) | ('or', [q..]) | ('not', q) | ('pre', prefix)
    def ev(self, q, i):
        """(matches, score)"""
        kind = q[0]
        if kind == 't':
            s = self.term_score(i, q[1])
            return (s is not None, s or 0.0)
        if kind == 'p':
            ok = self.has_phrase(i, q[1])
            return (ok, sum(self.term_score(i, w) for w in set(q[1])) if ok else 0.0)
        if kind == 'pre':
            best = [self.term_score(i, w) for w in self.df if w.startswith(q[1]) and self.term_score(i, w)]
            return (bool(best), max(best) if best else 0.0)
        if kind == 'and':
            rs = [self.ev(c, i) for c in q[1] if c[0] != 'not']
            nots = [self.ev(c[1], i)[0] for c in q[1] if c[0] == 'not']
            return (all(r[0] for r in rs) and not any(nots), sum(r[1] for r in rs))
        rs = [self.ev(c, i) for c in q[1]]
        return (any(r[0] for r in rs), sum(r[1] for r in rs if r[0]))

    def deparse(self, q):
        kind = q[0]
        if kind == 't':
            return q[1]
        if kind == 'p':
            return '"' + ' '.join(q[1]) + '"'
        if kind == 'pre':
            return q[1] + '*'
        if kind == 'not':
            return 'NOT ' + self.deparse(q[1])
        sep = ' AND ' if kind == 'and' else ' OR '
        return '(' + sep.join(self.deparse(c) for c in q[1]) + ')'


class OracleTest(TmpDb):
    def test_against_bruteforce(self):
        rnd = random.Random(11)
        vocab = ['alpha', 'beta', 'gamma', 'delta', 'omega', 'zeta', 'kappa', 'sigma', 'theta', 'lambda'] + \
                ['w%d' % i for i in range(40)]
        weights = [1 / (1 + i) for i in range(len(vocab))]
        docs = {}
        for i in range(1, 1500):
            if i % 97 == 0:
                continue
            docs[i * 3 if i % 5 == 0 else i] = ' '.join(rnd.choices(vocab, weights, k=rnd.randint(3, 60)))
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none)')
        items = sorted(docs.items())
        for lo in range(0, len(items), 400):
            db.execute('BEGIN')
            db.executemany('INSERT INTO t(rowid, body) VALUES (?, ?)', items[lo:lo + 400])
            db.execute('COMMIT')
        orc = Oracle(docs)
        qs = []
        for _ in range(250):
            def leaf():
                r = rnd.random()
                if r < 0.7:
                    return ('t', rnd.choices(vocab, weights)[0])
                if r < 0.9:
                    return ('p', [rnd.choices(vocab, weights)[0] for _ in range(rnd.randint(2, 3))])
                return ('pre', rnd.choice(['w1', 'w2', 'al', 'ga', 'w']))
            kind = rnd.choice(['and', 'or', 'and', 'or', 'notand'])
            kids = [leaf() for _ in range(rnd.randint(2, 4))]
            if kind == 'notand':
                qs.append(('and', kids[:-1] + [('not', kids[-1])]))
            else:
                qs.append((kind, kids))
        for phase in ('segments', 'merged'):
            for q in qs:
                text = orc.deparse(q)
                want = {i: orc.ev(q, i) for i in docs}
                want_ids = sorted(i for i, (ok, _) in want.items() if ok)
                self.assertEqual(ids(db, text), want_ids, f'{phase}: {text}')
                self.assertEqual(db.execute("SELECT sift_count('t', ?)", (text,)).fetchone()[0], len(want_ids), text)
                got = db.execute('SELECT rowid, rank FROM t WHERE t MATCH ? ORDER BY rank LIMIT 25', (text,)).fetchall()
                ranked = sorted(want_ids, key=lambda i: (-want[i][1], i))[:25]
                for (gid, grank), wid in zip(got, ranked):
                    self.assertAlmostEqual(-grank, want[wid][1], delta=1e-3 + 1e-4 * abs(want[wid][1]), msg=f'{phase}: {text}')
                # ties and float noise may order near-equal scores differently: the score vector must agree
                self.assertEqual(len(got), len(ranked), text)
            db.execute("INSERT INTO t(t) VALUES ('merge')")
            self.assertEqual(segs(db), 1)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]] + [a for a in sys.argv[1:] if a != '-v'], verbosity=2 if '-v' in sys.argv else 1)
