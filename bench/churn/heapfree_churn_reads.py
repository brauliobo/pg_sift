#!/usr/bin/env python3
"""The reads of a table whose rows were updated, with the heap-free read, with the heap checking every page, with no track_updates on the index, on a b-tree twin and on the heap alone.

    python3 -I bench/churn/heapfree_churn_reads.py --host DIR --port N --out FILE [--rows 300000] [--shares 1,5,20] [--patterns recent,random] [--reps 3]

For each (which rows, share) a table `w` with a sift_col index (`track_updates` with track_deletes, or without, --track off) and a table `wb` with the same rows and b-trees (the twin) are loaded, updated in
a covered column (a new entry and a tombstone for each row) and read in four states: fresh (VACUUM FREEZE), updated and promoted, merged, vacuumed. Records of the records library; the update is
declared in `churn` and the state in `variant`. Warm only: a RAM disk shows no device time, the blocks (heap_hit, heap_read) are in every row.
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
PATTERNS = {'recent': 'ts > {rows} - {rows} * {share} / 100', 'random': '(hashint4(id) & 2147483647) % 10000 < {share} * 100'}
STATEMENTS = (('group_all_tenants', 'SELECT tenant_id, count(*), sum(counter_a) FROM {t} GROUP BY tenant_id'),
              ('one_tenant', 'SELECT count(*), sum(counter_a) FROM {t} WHERE tenant_id = 7'),
              ('point', 'SELECT count(*), sum(counter_a) FROM {t} WHERE tenant_id = 7 AND counter_a = 123'),
              ('top_n', 'SELECT id, ts FROM {t} ORDER BY ts DESC LIMIT 20'),
              ('index_only', 'SELECT count(ts), min(ts) FROM {t} WHERE ts BETWEEN 400000 AND 410000'))
BASE = ['SET max_parallel_workers_per_gather = 0', 'SET sift.enable_agg = on']
LOAD = """CREATE TABLE {t} (id int PRIMARY KEY, ts int NOT NULL, tenant_id int NOT NULL, counter_a int, label text) WITH (autovacuum_enabled = false, fillfactor = 100)"""
FILL = "INSERT INTO {t} SELECT i, i, i % 10, i % 1000, md5(i::text) FROM generate_series(1, {rows}) i"


def tip(a):
    return a.tip or subprocess.run(['git', '-C', REPO, 'rev-parse', '--short=12', 'HEAD'], capture_output=True, text=True).stdout.strip() or 'unknown'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', required=True)
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--rows', type=int, default=300000)
    ap.add_argument('--shares', default='1,5,20')
    ap.add_argument('--patterns', default='recent,random')
    ap.add_argument('--reps', type=int, default=3)
    ap.add_argument('--track', choices=('on', 'off'), default='on')
    ap.add_argument('--db', default='heapfree_churn_reads')
    ap.add_argument('--tip')
    ap.add_argument('--note')
    a = ap.parse_args()
    admin = Session(a.port, 600, None, db='postgres', host=a.host)
    admin.run('DROP DATABASE IF EXISTS %s' % a.db)
    admin.run('CREATE DATABASE %s' % a.db)
    s = Session(a.port, 600, None, db=a.db, host=a.host)
    s.run('CREATE EXTENSION pg_sift')
    t = tip(a)
    harness = 'heapfree churn reads' + (' (%s)' % a.note if a.note else '')
    env = collect.Env(t, harness, schema.run_id(t), collect.host_cpus(collect.postmaster_pid(collect.backend_pid(s))), reps=a.reps, cool=iolib.cool, maintenance='disabled')
    opts = "track_updates = 'id', track_deletes = on, " if a.track == 'on' else ''
    out = []

    def run(sql):
        _, _, err, *_ = s.run(sql)
        if err:
            raise RuntimeError('%s: %s' % (sql, err))

    def arms():
        if a.track == 'off':
            return [collect.Arm('sift', 'track_off', settings=BASE)]
        return [collect.Arm('sift', 'heap_free', settings=BASE + ['SET sift.test_no_heapfree = off']),
                collect.Arm('sift', 'heap_checked', settings=BASE + ['SET sift.test_no_heapfree = on']),
                collect.Arm('heap')]

    def measure(pattern, share, stage, state, settled):
        churn = None if stage == 'fresh' else 'upd=%g;ins=0;del=0;rounds=1' % share
        for name, sql in STATEMENTS:
            subject = collect.Subject('heapfree_%s%d_%s' % (pattern, share, stage), a.rows, 'arrival', state, 'w', key='tenant_id', keys=10, churn=churn, settled=settled, structures='constraint,heap,sift',
                                      variant='track_%s' % a.track)
            out.extend(collect.measure_statement(s, name, sql.format(t='w'), subject, arms(), env, temps=('warm',), hash_text=sql))
            if a.track == 'on':   # the twin: the same statement on b-trees, one record set of its own
                twin = collect.Subject('heapfree_%s%d_%s' % (pattern, share, stage), a.rows, 'arrival', state, 'wb', key='tenant_id', keys=10, churn=churn, settled=settled, structures='constraint,btree,heap',
                                       variant='btree')
                out.extend(collect.measure_statement(s, name, sql.format(t='wb'), twin, [collect.Arm('btree')], env, temps=('warm',), hash_text=sql))

    for pattern, pred in PATTERNS.items():
        for share in map(int, a.shares.split(',')):
            pred_sql = pred.format(rows=a.rows, share=share)
            for t_ in ('w', 'wb'):
                if t_ == 'wb' and a.track == 'off':
                    continue
                run('DROP TABLE IF EXISTS %s' % t_)
                run(LOAD.format(t=t_))
                run(FILL.format(t=t_, rows=a.rows))
            run('CREATE INDEX w_c ON w USING sift_col (id, ts, tenant_id, counter_a) WITH (%skey_cut = \'off\')' % opts)
            if a.track == 'on':
                run('CREATE INDEX wb_ts ON wb (ts)')
                run('CREATE INDEX wb_tc ON wb (tenant_id, counter_a)')
            for t_ in ('w', 'wb'):
                if t_ == 'w' or a.track == 'on':
                    run('VACUUM (FREEZE, ANALYZE) %s' % t_)
            run('CHECKPOINT')
            measure(pattern, share, 'fresh', 'fresh_vacuumed', None)
            for t_ in ('w', 'wb'):
                if t_ == 'w' or a.track == 'on':
                    run('UPDATE %s SET counter_a = counter_a + 1 WHERE %s' % (t_, pred_sql))
            run("SELECT sift.col_promote('w_c')")
            for t_ in ('w', 'wb'):
                if t_ == 'w' or a.track == 'on':
                    run('ANALYZE %s' % t_)
            run('CHECKPOINT')
            measure(pattern, share, 'updated', 'churned_unvacuumed', False)
            run("SELECT sift.col_merge('w_c', true)")
            run('CHECKPOINT')
            measure(pattern, share, 'merged', 'churned_unvacuumed', False)
            for t_ in ('w', 'wb'):
                if t_ == 'w' or a.track == 'on':
                    run('VACUUM %s' % t_)
            run('CHECKPOINT')
            measure(pattern, share, 'vacuumed', 'churned_vacuumed', True)
            schema.write(a.out, out)
    from records import check
    for p in check.check(schema.read(a.out)):
        print(p)
    print('run_id %s: %d records' % (env.run_id, len(out)))


if __name__ == '__main__':
    main()
