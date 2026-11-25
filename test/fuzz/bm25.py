#!/usr/bin/env python3
"""Differential fuzz of the BM25 `sift` index in the shapes that run a scan again and again (docs/AGG.md, "Transactions").

  test/fuzz/bm25_fuzz.py [--seeds 1-20] [--queries 40] [--rows 600] [--out DIR] [--stop-after N] [--keep]
  test/fuzz/bm25_fuzz.py --selftest

Needs the scripts/pgtest.sh cluster (SIFT_TMP, SIFT_PORT). A seed gives a table pd(id, grp, body) of random sentences over a small vocabulary with a BM25 index (English or German
stemmer) and a list of statements: one query (word, phrase, NOT, OR, AND, prefix, a set of words) and the same query inside a correlated subquery, a LATERAL join, an EXISTS and an
IN subplan, `@@> ANY (ARRAY[..])`, a SCROLL cursor with FETCH BACKWARD, MOVE, ABSOLUTE, LAST and FIRST, a WITH HOLD cursor, and a prepared statement run seven times (the plan switches
after five). Each statement runs in a NEW backend (the first statement of a backend finds what a warm one hides: the crash of 775018b54 was in a cold session) under the plan that reads
the heap (the operator on every row, no index scan), under the index scan and under the index scan with parallel workers; the rows must be equal. The first statement whose answers
differ (or that raises under one plan only, or crashes a backend) ends the seed; the list of queries in it is cut down and the statement is written to DIR/bm25_fuzz_N.sql.
"""
import argparse
import os
import random
import re
import subprocess
import sys
import time

TMP = os.environ.get('SIFT_TMP', '/tmp/sift-arch')
ENV = dict(os.environ, LC_ALL='C', PGHOST=f'{TMP}/sock', PGPORT=os.environ.get('SIFT_PORT', '56100'), PGUSER='postgres',
           PGOPTIONS='-c client_min_messages=warning -c lc_messages=C')
EN = ['battery', 'case', 'clip', 'friendly', 'support', 'squeaky', 'running', 'quick', 'brown', 'fox', 'lazy', 'dogs', 'the', 'and']
DE = ['Hund', 'Katze', 'Straße', 'Straßen', 'Kinder', 'laufen', 'Haus', 'Häuser', 'Wetter', 'gestern', 'schlecht', 'schön', 'und', 'die']
PLANS = {'heap': ['SET enable_indexscan = off', 'SET enable_bitmapscan = off', 'SET enable_indexonlyscan = off', 'SET max_parallel_workers_per_gather = 0'],
         'index': ['SET enable_seqscan = off', 'SET max_parallel_workers_per_gather = 0'],
         'parallel': ['SET enable_seqscan = off', 'SET max_parallel_workers_per_gather = 2', 'SET parallel_setup_cost = 0', 'SET parallel_tuple_cost = 0',
                      'SET min_parallel_table_scan_size = 0', 'SET min_parallel_index_scan_size = 0']}
CRASH = re.compile(r'server closed the connection|connection to server was lost|terminating connection|crash of another server|was terminated by signal')


def psql(db, sql, timeout=90):
    """-> (exit code, output lines); stdout and stderr together"""
    try:
        p = subprocess.run(['psql', '-X', '-q', '-At', '-d', db, '-v', 'ON_ERROR_STOP=0'], input=sql, capture_output=True, text=True, env=ENV, timeout=timeout)
    except subprocess.TimeoutExpired:
        return 124, ['TIMEOUT']
    return p.returncode, (p.stdout + p.stderr).splitlines()


class Case:
    def __init__(self, seed, rows):
        self.r = random.Random(seed)
        self.rows = rows
        self.words = self.r.choice([EN, DE])
        self.stemmer = 'german' if self.words is DE else 'en'

    def setup(self):
        """A built index over 40% of the rows, the rest inserted after it in three batches (the write buffer), some rows deleted, VACUUM and promotion for some seeds"""
        w = ', '.join("'" + x + "'" for x in self.words)
        n, r = self.rows, self.r
        ins = lambda lo, hi: (f"INSERT INTO pd SELECT i, i % 7, (SELECT string_agg((ARRAY[{w}])[1 + (i * k * 7 + k * k) % {len(self.words)}], ' ' ORDER BY k) "
                              f"FROM generate_series(1, 3 + i % 4) k) FROM generate_series({lo}, {hi}) i;\n")
        cuts = [0, n * 4 // 10, n * 6 // 10, n * 8 // 10, n]
        sql = ("CREATE EXTENSION IF NOT EXISTS pg_sift;\nCREATE TABLE pd (id int PRIMARY KEY, grp int NOT NULL, body text NOT NULL) WITH (autovacuum_enabled = false);\n"
               + ins(1, cuts[1]) + f"CREATE INDEX pd_idx ON pd USING sift (body) WITH (stemmer = '{self.stemmer}');\n")
        for i in range(1, 4):
            sql += ins(cuts[i] + 1, cuts[i + 1])
            if i == 2 and r.random() < 0.5:
                sql += "SELECT sift.promote('pd_idx');\n"
        sql += f"DELETE FROM pd WHERE id % {r.choice([11, 13, 17])} = 0;\n"
        if r.random() < 0.6:
            sql += "VACUUM pd;\n"
        return sql + "ANALYZE pd;\n"

    def term(self):
        r, w = self.r, self.words
        k, a, b = r.random(), r.choice(w), r.choice(w)
        if k < 0.35:
            return a
        if k < 0.5:
            return f'"{a} {b}"'
        if k < 0.6:
            return f'{a} NOT {b}'
        if k < 0.7:
            return f'{a} OR {b}'
        if k < 0.78:
            return f'({a} OR {b}) AND {r.choice(w)}'
        if k < 0.86:
            return a[:max(3, len(a) - 2)] + '*'
        if k < 0.93:
            return '{' + f'{a} {b} {r.choice(w)}' + '}'
        return f'{a} AND {b}'

    def statement(self):
        r = self.r
        kind = r.choice(['one', 'sub', 'lateral', 'exists', 'in', 'any', 'cursor', 'hold', 'prepared', 'sub', 'lateral'])
        return kind, [self.term() for _ in range(r.randint(1, 5))]


def script(kind, qs):
    lit = lambda q: "'" + q.replace("'", "''") + "'"
    arr = 'ARRAY[' + ', '.join(lit(q) for q in qs) + ']'
    q0 = lit(qs[0])
    if kind == 'one':
        return [f"SELECT coalesce(string_agg(id::text, ',' ORDER BY id), '') FROM pd WHERE body @@> {q0}"]
    if kind == 'sub':
        return [f"SELECT q, (SELECT count(*) FROM pd WHERE body @@> q) FROM unnest({arr}) WITH ORDINALITY u(q, o) ORDER BY o"]
    if kind == 'lateral':
        return [f"SELECT u.q, c.n, c.m FROM unnest({arr}) WITH ORDINALITY u(q, o), LATERAL (SELECT count(*) n, min(id) m FROM pd WHERE body @@> u.q) c ORDER BY o"]
    if kind == 'exists':
        return [f"SELECT count(*) FROM pd p WHERE p.id % 3 = 0 AND EXISTS (SELECT 1 FROM pd x WHERE x.grp = p.grp AND x.id <> p.id AND x.body @@> {q0})"]
    if kind == 'in':
        return [f"SELECT count(*) FROM pd WHERE grp < 5 AND id IN (SELECT id FROM pd WHERE body @@> {q0})"]
    if kind == 'any':
        return [f"SELECT count(*) FROM pd WHERE body @@> ANY ({arr})"]
    if kind == 'cursor':
        return ["BEGIN", f"DECLARE c SCROLL CURSOR FOR SELECT id FROM pd WHERE body @@> {q0} ORDER BY id", "FETCH 3 FROM c", "FETCH BACKWARD 2 FROM c", "MOVE 2 FROM c",
                "FETCH ABSOLUTE 4 FROM c", "FETCH LAST FROM c", "FETCH FIRST FROM c", "FETCH 2 FROM c", "FETCH PRIOR FROM c", "CLOSE c", "COMMIT"]
    if kind == 'hold':
        return ["BEGIN", f"DECLARE c CURSOR WITH HOLD FOR SELECT id FROM pd WHERE body @@> {q0} ORDER BY id", "COMMIT", "FETCH 5 FROM c", "FETCH 5 FROM c", "CLOSE c"]
    return ["PREPARE p(text) AS SELECT count(*), min(id) FROM pd WHERE body @@> $1"] + [f"EXECUTE p({lit(qs[i % len(qs)])})" for i in range(7)]


def verdict(db, kind, qs):
    """The statement under each plan in a new connection -> None or a description of the difference"""
    res = {p: psql(db, ';\n'.join(sets + script(kind, qs)) + ';\n') for p, sets in PLANS.items()}
    for p, (rc, out) in res.items():
        if rc == 124:
            return f'{p}: timeout'
        if any(CRASH.search(l) for l in out):
            return f'{p}: the backend crashed: ' + ' | '.join(l for l in out if CRASH.search(l))[:200]
    ref = res['heap'][1]
    for p in ('index', 'parallel'):
        if res[p][1] != ref:
            return f'{p} differs from heap: heap {ref[:8]} | {p} {res[p][1][:8]}'
    return None


def shrink(db, kind, qs):
    while len(qs) > 1:
        for i in range(len(qs)):
            if verdict(db, kind, qs[:i] + qs[i + 1:]):
                qs = qs[:i] + qs[i + 1:]
                break
        else:
            break
    return qs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--seeds', default='1')
    ap.add_argument('--queries', type=int, default=40)
    ap.add_argument('--rows', type=int, default=600)
    ap.add_argument('--out', default='/tmp/bm25_fuzz')
    ap.add_argument('--db', default='bf')
    ap.add_argument('--stop-after', type=int, default=0)
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    seeds = []
    for part in a.seeds.split(','):
        lo, _, hi = part.partition('-')
        seeds += range(int(lo), int(hi or lo) + 1)
    if a.selftest:
        for s in range(30):
            c = Case(s, 100)
            assert c.setup().count('CREATE INDEX') == 1 and 'DELETE FROM pd' in c.setup()
            for _ in range(20):
                kind, qs = c.statement()
                assert script(kind, qs) and all(isinstance(q, str) for q in qs)
        print('bm25_fuzz selftest: 30 cases')
        return 0
    failed = 0
    for seed in seeds:
        c, db, t0, bad = Case(seed, a.rows), f'{a.db}_{seed}', time.time(), None
        psql('postgres', f'DROP DATABASE IF EXISTS {db} WITH (FORCE)')
        psql('postgres', f'CREATE DATABASE {db}')
        rc, out = psql(db, c.setup())
        if rc != 0 or any(l.startswith('ERROR') for l in out):
            print(f'seed {seed}: setup failed: {out[:3]}')
            continue
        for n in range(a.queries):
            kind, qs = c.statement()
            d = verdict(db, kind, qs)
            if d:
                qs = shrink(db, kind, qs)
                d = verdict(db, kind, qs) or d
                os.makedirs(a.out, exist_ok=True)
                path = f'{a.out}/bm25_fuzz_{seed}.sql'
                with open(path, 'w') as f:
                    f.write(f'-- bm25_fuzz seed {seed}, statement {n} ({kind}): {d}\n-- every statement below runs after \\c - (a new backend) with these settings: '
                            + ' | '.join(f"{p}: {'; '.join(v)}" for p, v in PLANS.items()) + '\n' + c.setup() + '\\c -\n' + ';\n'.join(PLANS[d.split()[0].rstrip(':')] + script(kind, qs)) + ';\n')
                bad = (n, kind, d, path)
                break
        print(f"seed {seed}: {'FAIL' if bad else 'ok'} {a.queries} statements, {time.time() - t0:.0f} s [{c.stemmer}]", flush=True)
        if bad:
            failed += 1
            print(f'  statement {bad[0]} ({bad[1]}): {bad[2][:400]}\n  reproducer: {bad[3]}', flush=True)
        if not a.keep:
            psql('postgres', f'DROP DATABASE IF EXISTS {db} WITH (FORCE)')
        if bad and a.stop_after and failed >= a.stop_after:
            break
    print(f'bm25_fuzz: {failed} failing seeds')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
