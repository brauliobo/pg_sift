#!/usr/bin/env python3
"""The records of a heap table whose sift_col index (track_changes) answers without the heap: the same aggregates read with the heap-free read, with the heap checking
every page (sift.test_no_heapfree) and with no sift plan, on a fresh, a churned (no VACUUM) and a settled table.

    python3 -I bench/churn/heapfree_records.py --host DIR --port N --out FILE [--rows 200000] [--upd 5] [--dele 2] [--reps 3]

The server is a test cluster (autovacuum off, `maintenance` = `disabled`: the churned state must stay as the churn made it). Blocks of heap relations are the columns `heap_hit` and
`heap_read` of each row; an arm that does not read the heap says it with zero there. Each state is a dataset of its own: a comparison is one state.
Arms: `sift` with option `heap_free` (the read under test), `sift` with option `heap_checked` (the heap checks every page: what the read did before) and `heap` (no sift plan).
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
STEPS = os.path.join(os.path.dirname(HERE), 'steps')
sys.path[:0] = [STEPS, os.path.join(STEPS, 'records')]

import iolib  # noqa: E402
import records.collect as collect  # noqa: E402
import records.schema as schema  # noqa: E402
from duckdb import Session  # noqa: E402

STATEMENTS = (('group_all_tenants', 'SELECT tenant_id, count(*), sum(counter_a) FROM events GROUP BY tenant_id'),
              ('one_tenant', 'SELECT count(*), sum(counter_a) FROM events WHERE tenant_id = 7'))
BASE = ['SET max_parallel_workers_per_gather = 0', 'SET sift.enable_agg = on']


def harness(a, name):
    return '%s (%s)' % (name, a.note) if a.note else name


def arms():
    return [collect.Arm('sift', 'heap_free', settings=BASE + ['SET sift.test_no_heapfree = off']),
            collect.Arm('sift', 'heap_checked', settings=BASE + ['SET sift.test_no_heapfree = on']),
            collect.Arm('heap', settings=['SET sift.enable_agg = off', 'SET max_parallel_workers_per_gather = 0', 'SET enable_indexscan = off', 'SET enable_bitmapscan = off',
                                          'SET enable_indexonlyscan = off'])]


def measure(s, env, rows, state, settled, churn, reps):
    out = []
    for name, sql in STATEMENTS:
        subject = collect.Subject('heapfree_%s' % state, rows, 'arrival', state, 'events', key='tenant_id', keys=10, churn=churn, settled=settled, structures='constraint,heap,sift')
        out += collect.measure_statement(s, name, sql, subject, arms(), env)
    return out


A_INDEX = "sift_col (id, tenant_id, counter_a) WITH (track_changes = 'id')"
# (name, index, settings of the statement): the last three isolate what the options cost: the directory of heap addresses that track_changes turns on, the option without the replacement of the older
# entry (sift.test_no_supersede), and the triggers of track_changes without their work (sift.test_no_track: it fires, captures the old rows and returns)
VARIANTS = (('heap_only', None, []), ('plain', 'sift_col (id, tenant_id, counter_a)', []), ('track_changes', A_INDEX, []),
            ('point_tid_only', "sift_col (id, tenant_id, counter_a) WITH (point_tid_columns = 'id')", []),
            ('track_changes_no_supersede', A_INDEX, ['SET sift.test_no_supersede = on']),
            ('track_changes_trigger_idle', A_INDEX, ['SET sift.test_no_track = on']),
            ('plain_row_trigger_floor', 'sift_col (id, tenant_id, counter_a)', []))
# a trigger the table would have anyway: the cost of calling one C row trigger for each updated row (it compares old and new and returns the new row), the least a design that fires per row pays
EXTRA = {'plain_row_trigger_floor': 'CREATE TRIGGER floor BEFORE UPDATE ON w FOR EACH ROW EXECUTE FUNCTION suppress_redundant_updates_trigger()'}
WRITES = (('insert_5pct', 'INSERT INTO {t} SELECT i, i % 10, i % 1000, md5(i::text) FROM generate_series({n} + 1, {n} + {n} / 20) i'),
          ('update_1_row', 'UPDATE {t} SET counter_a = counter_a + 1 WHERE id = {n} / 2'),
          ('update_100_rows', 'UPDATE {t} SET counter_a = counter_a + 1 WHERE id BETWEEN {n} / 2 AND {n} / 2 + 99'),
          ('update_5pct_covered', 'UPDATE {t} SET counter_a = counter_a + 1 WHERE id % 20 = 0'),
          ('update_5pct_uncovered', 'UPDATE {t} SET label = label WHERE id % 20 = 0'),
          ('update_key_1pct', 'UPDATE {t} SET id = id + {n} * 10 WHERE id % 100 = 1'),
          ('delete_2pct', 'DELETE FROM {t} WHERE id % 50 = 0'))


def write_mode(a):
    """what a write costs in each configuration of the index: no index (the heap alone), sift_col without track_changes, with track_changes (the default). Each repetition
    builds the table again (200,000 rows, the index, VACUUM FREEZE), so the statement runs on the same state (every table has the primary key track_changes needs: a constraint, in the cost of every arm, declared as `constraint`); time, instructions of the cluster, WAL and index size of the statement"""
    admin = Session(a.port, 600, None, db='postgres', host=a.host)
    admin.run('DROP DATABASE IF EXISTS %s' % a.db)
    admin.run('CREATE DATABASE %s' % a.db)
    s = Session(a.port, 600, None, db=a.db, host=a.host)
    s.run('CREATE EXTENSION pg_sift')
    t = collect.git_tip(a.tip)
    env = collect.Env(t, harness(a, 'heapfree write'), schema.run_id(t), collect.host_cpus(collect.postmaster_pid(collect.backend_pid(s))), reps=a.reps, cool=iolib.cool, maintenance='disabled')
    n = a.rows
    out = []

    def run(sql):
        _, _, err, *_ = s.run(sql)
        if err:
            raise RuntimeError('%s: %s' % (sql, err))

    def fresh(name, index):
        def prepare():
            run('DROP TABLE IF EXISTS w')
            run('CREATE TABLE w (id int PRIMARY KEY, tenant_id int NOT NULL, counter_a int, label text) WITH (autovacuum_enabled = false, fillfactor = 100)')
            run("INSERT INTO w SELECT i, i %% 10, i %% 1000, md5(i::text) FROM generate_series(1, %d) i" % n)
            if index:
                run('CREATE INDEX w_c ON w USING ' + index)
            if name in EXTRA:
                run(EXTRA[name])
            run('VACUUM (FREEZE, ANALYZE) w')
            s.run('CHECKPOINT')
        return prepare

    for stmt, text in WRITES:
        sql = text.format(t='w', n=n)
        for variant, index, sets in VARIANTS:
            if a.variants and variant not in a.variants.split(','):
                continue
            subject = collect.Subject('heapfree_write', n, 'arrival', 'fresh_vacuumed', 'w', kind='load', key='tenant_id', keys=10, structures='constraint,heap' + (',sift' if index else ''), variant=variant)
            arm = collect.Arm('sift' if index else 'heap', variant, settings=sets, prepare=fresh(variant, index), structures=subject.structures)
            out += collect.measure_statement(s, stmt, sql, subject, [arm], env, temps=('warm',), hash_text=text)
    schema.write(a.out, out)
    from records import check
    for p in check.check(schema.read(a.out)):
        print(p)
    print('run_id %s: %d records' % (env.run_id, len(out)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', required=True)
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--rows', type=int, default=200000)
    ap.add_argument('--upd', type=float, default=5)
    ap.add_argument('--dele', type=float, default=2)
    ap.add_argument('--reps', type=int, default=3)
    ap.add_argument('--db', default='heapfree_records')
    ap.add_argument('--tip', default=None, help='the commit of the build when the tree is not a git work tree (12 hex digits)')
    ap.add_argument('--note', default=None, help='what the tree holds beyond the commit (the patch and its sha), written in `harness`')
    ap.add_argument('--variants', default=None, help='with --write: only these variants (comma separated), for a build that lacks a variant')
    ap.add_argument('--write', action='store_true', help='the cost of a write for each configuration of the index, not the reads')
    a = ap.parse_args()
    if a.write:
        return write_mode(a)
    admin = Session(a.port, 600, None, db='postgres', host=a.host)
    admin.run('DROP DATABASE IF EXISTS %s' % a.db)
    admin.run('CREATE DATABASE %s' % a.db)
    s = Session(a.port, 600, None, db=a.db, host=a.host)
    s.run('CREATE EXTENSION pg_sift')
    s.run('CREATE EXTENSION IF NOT EXISTS pg_buffercache')
    t = collect.git_tip(a.tip)
    env = collect.Env(t, harness(a, 'heapfree'), schema.run_id(t), collect.host_cpus(collect.postmaster_pid(collect.backend_pid(s))), reps=a.reps, cool=iolib.cool, maintenance='disabled')
    s.run('CREATE TABLE events (id int PRIMARY KEY, tenant_id int NOT NULL, counter_a int, label text) WITH (autovacuum_enabled = false, fillfactor = 100)')
    s.run("INSERT INTO events SELECT i, i %% 10, i %% 1000, md5(i::text) FROM generate_series(1, %d) i" % a.rows)
    s.run("CREATE INDEX events_c ON events USING sift_col (id, tenant_id, counter_a) WITH (track_changes = 'id')")
    s.run('VACUUM (FREEZE, ANALYZE) events')
    s.run('CHECKPOINT')
    out = measure(s, env, a.rows, 'fresh_vacuumed', None, None, a.reps)
    churn = collect.apply_churn(s, 'events', 'counter_a', upd=a.upd, dele=a.dele)
    s.run("SELECT sift.col_promote('events_c')")
    s.run('ANALYZE events')
    s.run('CHECKPOINT')
    out += measure(s, env, a.rows, 'churned_unvacuumed', False, churn, a.reps)
    s.run('VACUUM events')
    s.run('CHECKPOINT')
    out += measure(s, env, a.rows, 'churned_vacuumed', True, churn, a.reps)
    schema.write(a.out, out)
    from records import check
    for p in check.check(schema.read(a.out)):
        print(p)
    print('run_id %s: %d records' % (env.run_id, len(out)))


if __name__ == '__main__':
    main()
