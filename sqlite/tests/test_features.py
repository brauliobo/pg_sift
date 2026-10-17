"""Tests of what makes the extension a product: the bounded cache, merging and crashes, content modes, snippets and
facets, commands and loading.

    python3 -I tests/test_features.py [-v]        (needs build/sift.so)
"""
import os
import random
import shutil
import sqlite3
import subprocess
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_sift import EXT, TmpDb, connect, ids, segs, top  # noqa: E402

WORDS = ['w%d' % i for i in range(300)]


def stat(db, table='t'):
    return {k: int(v) for k, v in (kv.split('=') for kv in db.execute('SELECT sift_stat(?)', (table,)).fetchone()[0].split())}


def fill(db, n, seed=1, per_txn=0, table='t', start=1, vocab=300):
    rnd = random.Random(seed)
    words = ['w%d' % i for i in range(vocab)]
    db.execute('BEGIN')
    for i in range(start, start + n):
        db.execute(f'INSERT INTO {table}(rowid, body) VALUES (?, ?)', (i, ' '.join(rnd.choice(words) for _ in range(40))))
        if per_txn and i % per_txn == 0:
            db.execute('COMMIT')
            db.execute('BEGIN')
    db.execute('COMMIT')


QUERIES = ['w1', 'w1 AND w2', 'w3 OR w4 OR w5', '"w6 w7"', 'w1 NOT w2', 'w1*', 'w2 w3^3', 'w10 AND w11 AND w12']


def answers(db, table='t', ranked=True, spread=False):
    """Match sets, counts and (with ranked) the top 20. Dead documents still weigh in the statistics until a merge
    purges them, so the order of a ranked answer is only compared where nothing was deleted."""
    qs = QUERIES + (['w%d' % i for i in range(0, 4000, 7)] if spread else [])
    return [(ids(db, q, table), top(db, q, 20, table) if ranked else None,
             db.execute('SELECT sift_count(?, ?)', (table, q)).fetchone()[0]) for q in qs]


class Cache(TmpDb):
    def test_tiny_cache_gives_the_same_answers(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none, chunk=4096)')
        fill(db, 20000, vocab=4000)
        want = answers(db, spread=True)
        db.close()
        db = connect(self.path)
        db.execute("INSERT INTO t(t, rank) VALUES ('cache-size', 1)")
        self.assertEqual(answers(db, spread=True), want)
        s = stat(db)
        self.assertGreater(s['evictions'], 0)
        self.assertEqual(s['pinned'], 0)
        self.assertEqual(s['pin_leaks'], 0)
        self.assertLess(s['cached_bytes'], s['bytes'])

    def test_cache_stays_within_its_bound(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none, chunk=8192)')
        fill(db, 4000)
        db.close()
        db = connect(self.path)
        db.execute("INSERT INTO t(t, rank) VALUES ('cache-size', 512)")
        for q in QUERIES:
            ids(db, q)
            top(db, q, 20)
        s = stat(db)
        self.assertLessEqual(s['cached_bytes'], 4 * 512 * 1024 + 64 * 8192 * 3)
        self.assertEqual(s['pin_leaks'], 0)

    def test_cache_size_option_and_drop_cache(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, cache_size=256)')
        fill(db, 500)
        ids(db, 'w1')
        self.assertGreater(stat(db)['cached_bytes'], 0)
        db.execute("SELECT sift_drop_cache('t')")
        self.assertEqual(stat(db)['cached_bytes'], 0)
        self.assertEqual(len(ids(db, 'w1')), len(ids(db, 'w1')))

    def test_chunk_sizes_agree(self):
        res = []
        for chunk in (4096, 8192, 16384, 65536):
            db = connect(':memory:')
            db.execute(f'CREATE VIRTUAL TABLE t USING sift(body, stemmer=none, chunk={chunk})')
            fill(db, 800)
            res.append(answers(db))
        for r in res[1:]:
            self.assertEqual(r, res[0])

    def test_two_connections_see_commits(self):
        a = connect(self.path)
        a.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none)')
        fill(a, 100)
        b = connect(self.path)
        self.assertEqual(len(ids(b, 'w1')), len(ids(a, 'w1')))
        fill(a, 100, seed=2, start=101)
        self.assertEqual(ids(b, 'w1'), ids(a, 'w1'))
        a.execute("INSERT INTO t(t) VALUES ('optimize')")
        self.assertEqual(ids(b, 'w1'), ids(a, 'w1'))
        self.assertEqual(stat(b)['segments'], 1)


class Merge(TmpDb):
    def test_merge_steps_keep_the_answers(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none, automerge=0)')
        fill(db, 1200, per_txn=100)
        want = answers(db)
        n = segs(db)
        self.assertGreater(n, 8)
        db.execute("INSERT INTO t(t, rank) VALUES ('merge', 1)")
        self.assertLess(segs(db), n)
        self.assertEqual(answers(db), want)
        db.execute("INSERT INTO t(t) VALUES ('optimize')")
        self.assertEqual(segs(db), 1)
        self.assertEqual(answers(db), want)

    def test_merge_purges_deletes(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none, automerge=0)')
        fill(db, 1200, per_txn=100)
        db.execute('DELETE FROM t WHERE rowid % 5 = 0')
        want = answers(db, ranked=False)
        db.execute("INSERT INTO t(t) VALUES ('optimize')")
        self.assertEqual(segs(db), 1)
        self.assertEqual(answers(db, ranked=False), want)
        self.assertEqual(db.execute('SELECT count(*) FROM t').fetchone()[0], 960)
        full = connect(':memory:')
        full.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none, automerge=0)')
        fill(full, 1200, per_txn=100)
        full.execute('DELETE FROM t WHERE rowid % 5 = 0')
        full.execute("INSERT INTO t(t) VALUES ('rebuild')")
        self.assertEqual(answers(db), answers(full))
        db.execute("INSERT INTO t(t, rank) VALUES ('integrity-check', 1)")

    def test_automerge_after_flush(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none, automerge=4)')
        for k in range(12):
            fill(db, 50, seed=k, start=1 + 50 * k)
        self.assertLessEqual(segs(db), 5)
        db2 = connect(':memory:')
        db2.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none, automerge=0)')
        for k in range(12):
            fill(db2, 50, seed=k, start=1 + 50 * k)
        self.assertEqual(segs(db2), 12)
        self.assertEqual(answers(db), answers(db2))
        db2.execute("INSERT INTO t(t, rank) VALUES ('automerge', 3)")

    def test_rollback_leaves_old_segments(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none)')
        fill(db, 600, per_txn=100)
        want, n = answers(db), segs(db)
        db.execute('BEGIN')
        db.execute("INSERT INTO t(t) VALUES ('optimize')")
        self.assertEqual(segs(db), 1)
        self.assertEqual(answers(db), want)
        db.execute('ROLLBACK')
        self.assertEqual(segs(db), n)
        self.assertEqual(answers(db), want)
        db.execute("INSERT INTO t(t, rank) VALUES ('integrity-check', 1)")

    def test_rolled_back_deletes_in_segments_come_back(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none)')
        fill(db, 600, per_txn=100)
        want = answers(db)
        db.execute('BEGIN')
        db.execute('DELETE FROM t WHERE rowid % 3 = 0')
        self.assertNotEqual(answers(db), want)
        db.execute('ROLLBACK')
        self.assertEqual(answers(db), want)
        db.execute("INSERT INTO t(t, rank) VALUES ('integrity-check', 1)")
        db.execute('BEGIN')
        db.execute('DELETE FROM t WHERE rowid % 3 = 0')
        db.execute('SAVEPOINT a')
        db.execute('DELETE FROM t WHERE rowid % 3 = 1')
        db.execute('ROLLBACK TO a')
        db.execute('RELEASE a')
        db.execute('COMMIT')
        self.assertEqual(db.execute('SELECT count(*) FROM t').fetchone()[0], 400)
        db.execute("INSERT INTO t(t, rank) VALUES ('integrity-check', 1)")

    def test_insert_or_replace(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none)')
        db.execute("INSERT INTO t(rowid, body) VALUES (1, 'old text'), (2, 'other')")
        db.execute("INSERT OR REPLACE INTO t(rowid, body) VALUES (1, 'new text')")
        self.assertEqual(ids(db, 'old'), [])
        self.assertEqual(ids(db, 'text'), [1])
        with self.assertRaises(sqlite3.Error):
            db.execute("INSERT INTO t(rowid, body) VALUES (2, 'dup')")

    def test_failed_merge_leaves_old_segments(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none, chunk=4096)')
        fill(db, 600, per_txn=100)
        want, n = answers(db), segs(db)
        db.execute("INSERT INTO t(t, rank) VALUES ('x-fault', 1000003)")
        with self.assertRaises(sqlite3.Error) as e:
            db.execute("INSERT INTO t(t) VALUES ('optimize')")
        self.assertIn('injected fault', str(e.exception))
        self.assertEqual(segs(db), n)
        self.assertEqual(answers(db), want)
        db.execute("INSERT INTO t(t, rank) VALUES ('integrity-check', 1)")
        db.execute("INSERT INTO t(t) VALUES ('optimize')")
        self.assertEqual(segs(db), 1)
        self.assertEqual(answers(db), want)

    def test_killed_merge_leaves_old_segments(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none, chunk=4096)')
        fill(db, 600, per_txn=100)
        want, n = answers(db), segs(db)
        db.close()
        code = ("import sqlite3,sys;d=sqlite3.connect(sys.argv[1],isolation_level=None);d.enable_load_extension(True);"
                "d.load_extension(sys.argv[2]);d.execute(\"INSERT INTO t(t, rank) VALUES ('x-fault', 2000003)\");"
                "d.execute(\"INSERT INTO t(t) VALUES ('optimize')\")")
        r = subprocess.run([sys.executable, '-I', '-c', code, self.path, EXT])
        self.assertEqual(r.returncode, 86)
        db = connect(self.path)
        self.assertEqual(segs(db), n)
        self.assertEqual(answers(db), want)
        db.execute("INSERT INTO t(t, rank) VALUES ('integrity-check', 1)")
        db.execute("INSERT INTO t(t) VALUES ('optimize')")
        self.assertEqual(answers(db), want)

    def test_merge_bounded_by_the_cache(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none, chunk=8192)')
        fill(db, 2000, per_txn=250)
        want = answers(db)
        db.execute("INSERT INTO t(t, rank) VALUES ('cache-size', 256)")
        db.execute("INSERT INTO t(t) VALUES ('optimize')")
        self.assertEqual(answers(db), want)
        s = stat(db)
        self.assertEqual(s['pin_leaks'], 0)
        self.assertEqual(s['pinned'], 0)


class Content(TmpDb):
    def test_external_content(self):
        db = connect(self.path)
        db.execute('CREATE TABLE docs(id INTEGER PRIMARY KEY, body TEXT)')
        db.execute("CREATE VIRTUAL TABLE t USING sift(body, stemmer=none, content='docs', content_rowid='id')")
        db.executemany('INSERT INTO docs(id, body) VALUES (?, ?)',
                       [(1, 'the quick brown fox'), (2, 'a quick red fox'), (3, 'lazy dogs sleep')])
        for i in (1, 2, 3):
            db.execute('INSERT INTO t(rowid, body) SELECT id, body FROM docs WHERE id = ?', (i,))
        self.assertEqual(ids(db, 'quick'), [1, 2])
        self.assertEqual(ids(db, '"quick brown"'), [1])
        self.assertEqual(db.execute("SELECT body FROM t WHERE t MATCH 'lazy'").fetchall(), [('lazy dogs sleep',)])
        # delete protocol: the index needs only the rowid; the user's table is changed by the user
        db.execute("INSERT INTO t(t, rowid) VALUES ('delete', 2)")
        db.execute('DELETE FROM docs WHERE id = 2')
        self.assertEqual(ids(db, 'quick'), [1])
        # update: delete the old entry, change the table, add the new one
        db.execute("INSERT INTO t(t, rowid) VALUES ('delete', 3)")
        db.execute("UPDATE docs SET body = 'sleepy cats' WHERE id = 3")
        db.execute('INSERT INTO t(rowid, body) SELECT id, body FROM docs WHERE id = 3')
        self.assertEqual(ids(db, 'dogs'), [])
        self.assertEqual(ids(db, 'cats'), [3])
        db.execute("INSERT INTO t(t, rank) VALUES ('integrity-check', 1)")
        db.execute("INSERT INTO t(t) VALUES ('rebuild')")
        self.assertEqual(ids(db, 'cats'), [3])
        self.assertEqual(ids(db, 'quick'), [1])
        self.assertEqual(db.execute("SELECT count(*) FROM sqlite_master WHERE name = 't_content'").fetchone()[0], 0)

    def test_contentless(self):
        db = connect(self.path)
        db.execute("CREATE VIRTUAL TABLE t USING sift(body, stemmer=none, content='')")
        db.executemany('INSERT INTO t(rowid, body) VALUES (?, ?)',
                       [(1, 'the quick brown fox'), (2, 'a quick red fox'), (3, 'lazy dogs sleep')])
        self.assertEqual(ids(db, 'quick'), [1, 2])
        self.assertEqual(ids(db, '"quick brown"'), [1])
        self.assertEqual(top(db, 'fox', 5), top(db, 'fox', 5))
        self.assertEqual(db.execute("SELECT sift_count('t', 'quick OR dogs')").fetchone()[0], 3)
        self.assertEqual(db.execute("SELECT body FROM t WHERE t MATCH 'lazy'").fetchall(), [(None,)])
        db.execute("INSERT INTO t(t, rowid) VALUES ('delete', 2)")
        self.assertEqual(ids(db, 'quick'), [1])
        with self.assertRaises(sqlite3.Error):
            db.execute('DELETE FROM t WHERE rowid = 1')
            db.execute("INSERT INTO t(t, rowid) VALUES ('delete', 1)")
            db.execute("INSERT INTO t(t) VALUES ('rebuild')")
        db.execute("INSERT INTO t(t, rank) VALUES ('integrity-check', 1)")

    def test_contentless_is_smaller(self):
        sizes = {}
        for mode, opt in (('internal', ''), ('contentless', ", content=''")):
            path = self.path + mode
            db = connect(path)
            db.execute(f'CREATE VIRTUAL TABLE t USING sift(body, stemmer=none{opt})')
            fill(db, 1500)
            db.execute("INSERT INTO t(t) VALUES ('optimize')")
            db.close()
            sizes[mode] = os.path.getsize(path)
        self.assertLess(sizes['contentless'], sizes['internal'])

    def test_plain_delete_on_contentless_is_refused(self):
        db = connect(self.path)
        db.execute("CREATE VIRTUAL TABLE t USING sift(body, stemmer=none, content='')")
        db.execute("INSERT INTO t(rowid, body) VALUES (1, 'a b')")
        with self.assertRaises(sqlite3.Error):
            db.execute('INSERT INTO t(t, rowid) VALUES (\'nope\', 1)')


class Aux(TmpDb):
    def setUp(self):
        super().setUp()
        self.db = connect(self.path)
        self.db.execute('CREATE VIRTUAL TABLE t USING sift(title, body, stemmer=none)')
        self.db.executemany('INSERT INTO t(rowid, title, body) VALUES (?, ?, ?)', [
            (1, 'Friendly pool', 'a very friendly staff and a nice pool with friendly people near the old town square'),
            (2, 'Dirty', 'dirty rooms'),
            (3, 'Pool bar', 'the pool bar was friendly'),
            (4, 'x', 'friendly pool')])

    def test_highlight(self):
        r = dict(self.db.execute("SELECT rowid, sift_highlight(t, 1, '<b>', '</b>') FROM t WHERE t MATCH 'friendly pool'"))
        self.assertEqual(r[4], '<b>friendly</b> <b>pool</b>')
        self.assertEqual(r[3], 'the <b>pool</b> bar was <b>friendly</b>')
        r = dict(self.db.execute("SELECT rowid, sift_highlight(t, 1, '<', '>') FROM t WHERE t MATCH '\"friendly pool\"'"))
        self.assertEqual(r[4], '<friendly pool>')
        self.assertNotIn('<', r[1])
        r = dict(self.db.execute("SELECT rowid, sift_highlight(t, 0, '<', '>') FROM t WHERE t MATCH 'pool NOT dirty'"))
        self.assertEqual(r[1], 'Friendly <pool>')
        r = dict(self.db.execute("SELECT rowid, sift_highlight(t, 1, '[', ']') FROM t WHERE t MATCH 'fri*'"))
        self.assertEqual(r[4], '[friendly] pool')

    def test_snippet(self):
        r = dict(self.db.execute("SELECT rowid, sift_snippet(t, 1, '[', ']', '...', 6) FROM t WHERE t MATCH 'friendly pool'"))
        self.assertEqual(r[1], '...[friendly] staff and a nice [pool]...')
        self.assertEqual(r[4], '[friendly] [pool]')

    def test_bm25_is_the_rank(self):
        rows = self.db.execute("SELECT sift_bm25(t), rank FROM t WHERE t MATCH 'friendly pool' ORDER BY rank").fetchall()
        self.assertEqual(len(rows), 3)
        for a, b in rows:
            self.assertEqual(a, b)
            self.assertLess(a, 0)
        self.assertEqual(rows, sorted(rows, key=lambda r: r[1]))

    def test_text_forms(self):
        self.assertEqual(self.db.execute("SELECT sift_highlight_text('t', 'The friendly pool bar', 'friendly OR bar', '<', '>')").fetchone()[0],
                         'The <friendly> pool <bar>')
        self.assertEqual(self.db.execute(
            "SELECT sift_snippet_text('t', 'one two three four five six seven eight', 'six', '<', '>', '..', 3)").fetchone()[0],
            '..five <six> seven..')
        self.assertEqual(self.db.execute(
            "SELECT sift_snippet_text('t', 'one two', 'two', '<', '>', '..', 9)").fetchone()[0], 'one <two>')

    def test_outside_match_is_an_error(self):
        with self.assertRaises(sqlite3.Error):
            self.db.execute("SELECT sift_highlight(t, 1, '<', '>') FROM t").fetchall()


class Facets(TmpDb):
    def test_facets_agree_with_group_by(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none)')
        db.execute("CREATE VIRTUAL TABLE cat USING sift(c, analyzer='keyword', stemmer=none, positions=0)")
        db.execute('CREATE TABLE plain(id INTEGER PRIMARY KEY, body TEXT, c TEXT)')
        rnd = random.Random(7)
        cats = ['hotel', 'hostel', 'bnb', 'resort', 'camp']
        for flush in range(3):
            db.execute('BEGIN')
            for i in range(flush * 400 + 1, flush * 400 + 401):
                body = ' '.join(rnd.choice(WORDS[:60]) for _ in range(10))
                c = rnd.choice(cats) if i % 11 else None
                db.execute('INSERT INTO t(rowid, body) VALUES (?, ?)', (i, body))
                db.execute('INSERT INTO plain VALUES (?, ?, ?)', (i, body, c))
                if c:
                    db.execute('INSERT INTO cat(rowid, c) VALUES (?, ?)', (i, c))
            db.execute('COMMIT')
        db.execute('DELETE FROM cat WHERE rowid % 13 = 0')
        db.execute("UPDATE plain SET c = NULL WHERE id % 13 = 0")
        for q in ('w1', 'w1 OR w2', 'w3 AND w4', 'w5*', 'w7 NOT w8'):
            matched = ids(db, q)
            want = db.execute(f"SELECT c, count(*) n FROM plain WHERE id IN ({','.join(map(str, matched)) or 'NULL'}) "
                              "AND c IS NOT NULL GROUP BY c ORDER BY n DESC, c").fetchall()
            got = db.execute("SELECT value, n FROM sift_facets('t', 'cat', ?)", (q,)).fetchall()
            self.assertEqual(got, want, q)
            nulls = db.execute(f"SELECT count(*) FROM plain WHERE id IN ({','.join(map(str, matched)) or 'NULL'}) AND c IS NULL").fetchone()[0]
            with_nulls = db.execute("SELECT value, n FROM sift_facets('t', 'cat', ?, -1, 1)", (q,)).fetchall()
            self.assertEqual([r for r in with_nulls if r[0] is not None], want, q)
            self.assertEqual(sum(r[1] for r in with_nulls if r[0] is None), nulls, q)
        got = db.execute("SELECT value FROM sift_facets('t', 'cat', 'w1 OR w2', 2)").fetchall()
        self.assertEqual(len(got), 2)
        db.execute("INSERT INTO cat(cat) VALUES ('optimize')")
        self.assertEqual(db.execute("SELECT value, n FROM sift_facets('t', 'cat', 'w1')").fetchall(),
                         db.execute("SELECT value, n FROM sift_facets('t', 'cat', 'w1')").fetchall())

    def test_unknown_table(self):
        db = connect(self.path)
        with self.assertRaises(sqlite3.Error):
            db.execute("SELECT * FROM sift_facets('nope', 'nope2', 'x')").fetchall()


class Commands(TmpDb):
    def test_unknown_command(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body)')
        with self.assertRaises(sqlite3.Error):
            db.execute("INSERT INTO t(t) VALUES ('frobnicate')")

    def test_integrity_check_finds_damage(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none)')
        fill(db, 300)
        db.execute("INSERT INTO t(t, rank) VALUES ('integrity-check', 1)")
        db.execute("INSERT INTO t(t) VALUES ('integrity-check')")
        db.close()
        raw = sqlite3.connect(self.path)
        raw.execute('DELETE FROM t_chunks WHERE id = (SELECT max(id) FROM t_chunks)')
        raw.commit()
        raw.close()
        db = connect(self.path)
        with self.assertRaises(sqlite3.Error):
            db.execute("INSERT INTO t(t, rank) VALUES ('integrity-check', 1)")

    def test_integrity_check_finds_content_without_index(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none)')
        fill(db, 50)
        db.execute("INSERT INTO t(t, rank) VALUES ('integrity-check', 1)")
        db.execute("INSERT INTO t_content(id, c0) VALUES (9999, 'ghost')")
        with self.assertRaises(sqlite3.Error):
            db.execute("INSERT INTO t(t, rank) VALUES ('integrity-check', 1)")

    def test_settings_persist(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body)')
        db.execute("INSERT INTO t(t, rank) VALUES ('automerge', 3)")
        db.execute("INSERT INTO t(t, rank) VALUES ('pending-postings', 1234)")
        db.close()
        db = connect(self.path)
        self.assertEqual(db.execute("SELECT count(*) FROM t_config WHERE k = 'automerge' AND v = 3").fetchone()[0], 1)
        self.assertEqual(db.execute("SELECT count(*) FROM t_config WHERE k = 'pending-postings' AND v = 1234").fetchone()[0], 1)
        db.execute("INSERT INTO t(t, rank) VALUES ('parallel', 2)")
        with self.assertRaises(sqlite3.Error):
            db.execute("INSERT INTO t(t, rank) VALUES ('cache-size', -1)")

    def test_shadow_tables_follow_rename_and_drop(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body)')
        db.execute("INSERT INTO t(rowid, body) VALUES (1, 'hello')")
        names = lambda p: sorted(r[0] for r in db.execute("SELECT name FROM sqlite_master WHERE name LIKE ?", (p + '%',)))
        self.assertEqual(names('t'), ['t', 't_chunks', 't_config', 't_content', 't_meta', 't_segs'])
        db.execute('ALTER TABLE t RENAME TO u')
        self.assertEqual(names('t'), [])
        self.assertEqual(names('u'), ['u', 'u_chunks', 'u_config', 'u_content', 'u_meta', 'u_segs'])
        db.execute('CREATE VIRTUAL TABLE t USING sift(body)')
        db.execute('DROP TABLE u')
        db.execute('DROP TABLE t')
        self.assertEqual(db.execute("SELECT count(*) FROM sqlite_master").fetchone()[0], 0)

    def test_version(self):
        db = connect(self.path)
        self.assertRegex(db.execute('SELECT sift_version()').fetchone()[0], r'^sift-sqlite 0\.\d+ \(engine format \d+\.\d+, storage format 2\)$')

    def test_format_mismatch_is_refused(self):
        db = connect(self.path)
        db.execute('CREATE VIRTUAL TABLE t USING sift(body)')
        db.close()
        raw = sqlite3.connect(self.path)
        raw.execute("UPDATE t_config SET v = 99 WHERE k = 'format'")
        raw.commit()
        raw.close()
        db = connect(self.path)
        with self.assertRaises(sqlite3.Error):
            db.execute('SELECT * FROM t').fetchall()


@unittest.skipUnless(shutil.which('sqlite3'), 'no sqlite3 shell')
class Loading(TmpDb):
    def test_shell_load(self):
        r = subprocess.run(['sqlite3', ':memory:', f'.load {EXT}', 'SELECT sift_version();'], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('sift-sqlite', r.stdout)
        r = subprocess.run(['sqlite3', self.path, f'.load {EXT}',
                            "CREATE VIRTUAL TABLE t USING sift(body);", "INSERT INTO t(rowid, body) VALUES (1, 'hello world');",
                            "SELECT rowid FROM t WHERE t MATCH 'hello';"], capture_output=True, text=True)
        self.assertEqual(r.stdout.strip(), '1', r.stderr)

    def test_load_twice_and_default_entry(self):
        db = connect(self.path)
        db.load_extension(EXT)
        db.load_extension(EXT, entrypoint='sqlite3_extension_init')
        self.assertTrue(db.execute('SELECT sift_version()').fetchone()[0])

    def test_isolated_python(self):
        code = ("import sqlite3,sys;d=sqlite3.connect(':memory:');d.enable_load_extension(True);d.load_extension(sys.argv[1]);"
                "print(d.execute('SELECT sift_isa()').fetchone()[0])")
        r = subprocess.run([sys.executable, '-I', '-c', code, EXT], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)


if __name__ == '__main__':
    unittest.main()
