#!/usr/bin/env python3
"""The records of a bitmap heap scan and of an index scan over the same rows of a sift_table (TB-1: the bitmap scan hands the slot its rows undecoded, src/table/fetch.c).

    python3 -I bench/table/bitmap_records.py --host DIR --port N --out FILE [--rows 300000] [--reps 3]

A default cluster. `events` is a sift_table of ROWS rows (ids in arrival order, 730 days, 5,000 parents) with a b-tree on (parent_id, ts) and one on (ts), its units written by
sift.table_maintain. Each statement runs on three arms of the b-tree structure: the planner's choice (`default`), the bitmap heap scan alone (`bitmap`) and the index scan alone (`index`);
warm and cold. The statements read few columns (kind and amount), then a string (memo) so that the columns a plan reads are still decoded.
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
STEPS = os.path.join(os.path.dirname(HERE), 'steps')
sys.path[:0] = [STEPS, os.path.join(STEPS, 'records')]

import iolib  # noqa: E402
import records.collect as collect  # noqa: E402
import records.schema as schema  # noqa: E402
from duckdb import Session  # noqa: E402

REPO = os.path.dirname(os.path.dirname(HERE))
STATEMENTS = (
    ('one_parent_narrow', "SELECT kind, count(*) AS n, sum(amount) AS total FROM events WHERE parent_id = 7 AND ts >= date '2025-03-01' AND ts < date '2025-06-01' GROUP BY 1 ORDER BY 1"),
    ('parents_2', "SELECT kind, count(*) AS n, sum(amount) AS total FROM events WHERE parent_id <= 2 AND ts >= date '2024-06-01' GROUP BY 1 ORDER BY 1"),
    ('parents_5', "SELECT kind, count(*) AS n, sum(amount) AS total FROM events WHERE parent_id <= 5 AND ts >= date '2024-06-01' GROUP BY 1 ORDER BY 1"),
    ('parents_15', "SELECT kind, count(*) AS n, sum(amount) AS total FROM events WHERE parent_id <= 15 AND ts >= date '2024-06-01' GROUP BY 1 ORDER BY 1"),
    ('parents_60', "SELECT kind, count(*) AS n, sum(amount) AS total FROM events WHERE parent_id <= 60 AND ts >= date '2024-06-01' GROUP BY 1 ORDER BY 1"),
    ('parents_150', "SELECT kind, count(*) AS n, sum(amount) AS total FROM events WHERE parent_id <= 150 AND ts >= date '2024-06-01' GROUP BY 1 ORDER BY 1"),
    ('ten_days', "SELECT kind, count(*) AS n, sum(amount) AS total FROM events WHERE ts >= date '2025-03-01' AND ts < date '2025-03-11' GROUP BY 1 ORDER BY 1"),
    ('ten_days_strings', "SELECT kind, count(*) AS n, max(memo) AS memo FROM events WHERE ts >= date '2025-03-01' AND ts < date '2025-03-11' GROUP BY 1 ORDER BY 1"),
)
BASE = ['SET max_parallel_workers_per_gather = 0', 'SET sift.enable_agg = off', 'SET enable_seqscan = off']
BITMAP = BASE + ['SET enable_indexscan = off', 'SET enable_indexonlyscan = off']
ARMS = (('default', BASE), ('bitmap', BITMAP), ('index', BASE + ['SET enable_bitmapscan = off']))


def build(s, rows):
    s.run('DROP TABLE IF EXISTS events')
    s.run('CREATE TABLE events (id int PRIMARY KEY, parent_id int NOT NULL, ts date NOT NULL, kind text NOT NULL, amount float8 NOT NULL, qty int NOT NULL, doc text NOT NULL, memo text NOT NULL) '
          'USING sift_table WITH (autovacuum_enabled = false)')
    s.run("INSERT INTO events SELECT g, 1 + (g::bigint * 7919) %% 5000, date '2024-01-01' + (g * 730 / %d)::int, (ARRAY['click','view','buy','share','like'])[1 + g %% 5], "
          "(g %% 997) / 10.0, g %% 20, md5(g::text) || md5((g * 3)::text), 'memo of event number ' || g || ' ' || md5((g * 7)::text) FROM generate_series(1, %d) g" % (rows, rows))
    s.run('CREATE INDEX events_parent ON events (parent_id, ts)')
    s.run('CREATE INDEX events_ts ON events (ts)')
    s.run("SELECT sift.table_maintain('events')")
    s.run('VACUUM (ANALYZE) events')
    s.run('CHECKPOINT')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', required=True)
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--rows', type=int, default=300000)
    ap.add_argument('--reps', type=int, default=3)
    ap.add_argument('--db', default='bitmap_records')
    ap.add_argument('--bulks', type=lambda x: [int(v) for v in x.split(',')], default=[], help='more bitmap arms, one for each sift.test_bitmap_bulk_rows (a test build): the rows of a superblock from which its pages are read through the group scan')
    ap.add_argument('--suffix', default='', help='appended to the statement names: the records of a build to compare with are one set (claims name a statement)')
    a = ap.parse_args()
    admin = Session(a.port, 600, None, db='postgres', host=a.host)
    admin.run('DROP DATABASE IF EXISTS %s' % a.db)
    admin.run('CREATE DATABASE %s' % a.db)
    s = Session(a.port, 600, None, db=a.db, host=a.host)
    s.run('CREATE EXTENSION pg_sift')
    s.run('CREATE EXTENSION IF NOT EXISTS pg_buffercache')
    tip = subprocess.run(['git', '-C', REPO, 'rev-parse', '--short=12', 'HEAD'], capture_output=True, text=True).stdout.strip() or 'unknown'
    env = collect.Env(tip, 'table bitmap records', schema.run_id(tip), collect.host_cpus(collect.postmaster_pid(collect.backend_pid(s))), reps=a.reps, cool=iolib.cool)
    build(s, a.rows)
    out = []
    for name, sql in STATEMENTS:
        subject = collect.Subject('table_bitmap', a.rows, 'arrival', 'fresh_vacuumed', 'events', key='parent_id', keys=5000, structures='btree,sift')
        arms = [collect.Arm('btree', option=opt, settings=sets) for opt, sets in ARMS + tuple(('bitmap_%d' % n, BITMAP + ['SET sift.test_bitmap_bulk_rows = %d' % n]) for n in a.bulks)]
        out += collect.measure_statement(s, name + a.suffix, sql, subject, arms, env, hash_text=sql)
        s.run('RESET ALL')
    schema.write(a.out, out)
    from records import check
    for p in check.check(schema.read(a.out)):
        print(p)


if __name__ == '__main__':
    main()
