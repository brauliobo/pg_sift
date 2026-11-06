#!/usr/bin/env python3
"""What the lookups in the one tombstone log cost a heap-free read: the read, the same read with the log not consulted (sift.test_hf_ignore_tombs: a wrong answer, a measure of the lookups only) and the read that checks the heap, after an update of a share of the rows. The lookups are the most a log kept in the units could save.

    python3 -I bench/churn/heapfree_churn_reads.py --host DIR --port N --out FILE [--rows 300000] [--shares 1,5,20] [--patterns recent,random] [--reps 3]

For each (which rows, share) a table `w` with a sift_col index (`track_changes` on, or `off` with --track off) and a table `wb` with the same rows and b-trees (the twin) are loaded, updated in
a covered column (a new entry and a tombstone for each row) and read in four states: fresh (VACUUM FREEZE), updated and promoted, merged, vacuumed. Records of the records library; the update is
declared in `churn` and the state in `variant`. Warm only: a RAM disk shows no device time, the blocks (heap_hit, heap_read) are in every row.
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

PATTERNS = {'recent': 'ts > {rows} - {rows} * {share} / 100', 'random': '(hashint4(id) & 2147483647) % 10000 < {share} * 100'}
STATEMENTS = (('group_all_tenants', 'SELECT tenant_id, count(*), sum(counter_a) FROM {t} GROUP BY tenant_id'),
              ('one_tenant', 'SELECT count(*), sum(counter_a) FROM {t} WHERE tenant_id = 7'))
BASE = ['SET max_parallel_workers_per_gather = 0', 'SET sift.enable_agg = on']
LOAD = """CREATE TABLE {t} (id int PRIMARY KEY, ts int NOT NULL, tenant_id int NOT NULL, counter_a int, label text) WITH (autovacuum_enabled = false, fillfactor = 100)"""
FILL = "INSERT INTO {t} SELECT i, i, i % 10, i % 1000, md5(i::text) FROM generate_series(1, {rows}) i"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', required=True)
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--rows', default='300000')
    ap.add_argument('--shares', default='1,5,20')
    ap.add_argument('--patterns', default='recent,random')
    ap.add_argument('--reps', type=int, default=3)
    ap.add_argument('--db', default='heapfree_lookup_cost')
    ap.add_argument('--tip')
    ap.add_argument('--note')
    a = ap.parse_args()
    admin = Session(a.port, 600, None, db='postgres', host=a.host)
    admin.run('DROP DATABASE IF EXISTS %s' % a.db)
    admin.run('CREATE DATABASE %s' % a.db)
    s = Session(a.port, 600, None, db=a.db, host=a.host)
    s.run('CREATE EXTENSION pg_sift')
    t = collect.git_tip(a.tip)
    env = collect.Env(t, 'heapfree lookup cost' + (' (%s)' % a.note if a.note else ''), schema.run_id(t), collect.host_cpus(collect.postmaster_pid(collect.backend_pid(s))), reps=a.reps, cool=iolib.cool, maintenance='disabled')
    out = []

    def run(sql):
        _, _, err, *_ = s.run(sql)
        if err:
            raise RuntimeError('%s: %s' % (sql, err))

    arms = [collect.Arm('sift', 'heap_free', settings=BASE + ['SET sift.test_no_heapfree = off']),
            collect.Arm('sift', 'log_not_consulted', settings=BASE + ['SET sift.test_hf_ignore_tombs = on']),
            collect.Arm('sift', 'heap_checked', settings=BASE + ['SET sift.test_no_heapfree = on'])]
    for rows in map(int, a.rows.split(',')):
        for pattern, pred in PATTERNS.items():
            for share in map(int, a.shares.split(',')):
                run('DROP TABLE IF EXISTS w')
                run(LOAD.format(t='w'))
                run(FILL.format(t='w', rows=rows))
                run("CREATE INDEX w_c ON w USING sift_col (id, ts, tenant_id, counter_a) WITH (track_changes = 'id', key_cut = 'off')")
                run('VACUUM (FREEZE, ANALYZE) w')
                run('UPDATE w SET counter_a = counter_a + 1 WHERE %s' % pred.format(rows=rows, share=share))
                run("SELECT sift.col_promote('w_c')")
                run('ANALYZE w')
                run('CHECKPOINT')
                for name, sql in STATEMENTS:
                    subject = collect.Subject('heapfree_lookup_%s%d' % (pattern, share), rows, 'arrival', 'churned_unvacuumed', 'w', key='tenant_id', keys=10, churn='upd=%g;ins=0;del=0;rounds=1' % share, settled=False,
                                              structures='constraint,heap,sift')
                    out.extend(collect.measure_statement(s, name, sql.format(t='w'), subject, arms, env, temps=('warm',), hash_text=sql))
                schema.write(a.out, out)
    from records import check
    for p in check.check(schema.read(a.out)):
        print(p)
    print('run_id %s: %d records' % (env.run_id, len(out)))


if __name__ == '__main__':
    main()
