#!/usr/bin/env python3
"""The reads a sift index answers without the heap, on a table whose rows were updated and not yet vacuumed, for one configuration of the index: no tracking, track_updates alone, or
track_updates with track_deletes (the only one whose log is complete, so the only one that can leave the heap check out).

    python3 -I bench/churn/heapfree_churn_reads.py --host DIR --port N --out FILE --track off|updates|on [--rows 300000] [--shares 1,5,20] [--patterns recent,random] [--reps 3] [--reference]

For each (which rows, share) a table `w` with a sift_col index is loaded, updated in a covered column (a new entry, and with track_updates a tombstone for the old one) and read in four states:
fresh (VACUUM FREEZE), updated and promoted, merged, vacuumed. Every class is a read that the index answers by itself when the table is clean (aggregates and top-N of SiftAgg, an Index Only
Scan); the plan of each (state, class) is written to FILE.plans.tsv and compared with the shape the class has (`plan_ok`), so a mode that changes the plan says so in the sidecar and in the summary.
Records of the records library; the update is declared in `churn` and the mode in `variant`. Warm only: a RAM disk shows no device time, the blocks (heap_hit, heap_read) are in every row.
--reference adds, with --track on, the arms that read the heap for every page (sift.test_no_heapfree) and no sift plan; --twin the b-tree twin.
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
# (class, statement, settings that force the Index Only Scan, text the plan must hold): the others are SiftAgg plans
STATEMENTS = (('group_all_tenants', 'SELECT tenant_id, count(*), sum(counter_a) FROM {t} GROUP BY tenant_id', None, 'SiftAgg'),
              ('one_tenant', 'SELECT count(*), sum(counter_a) FROM {t} WHERE tenant_id = 7', None, 'SiftAgg'),
              ('point', 'SELECT count(*), sum(counter_a) FROM {t} WHERE tenant_id = 7 AND counter_a = 123', None, 'SiftAgg'),
              ('top_n', 'SELECT id, ts FROM {t} ORDER BY ts DESC LIMIT 20', None, 'SiftAgg'),
              ('index_only', 'SELECT count(ts), min(ts) FROM {t} WHERE ts BETWEEN 400000 AND 410000', None, 'SiftAgg'),
              ('ios_range', 'SELECT id, ts FROM {t} WHERE ts BETWEEN 400000 AND 401000', ['SET sift.enable_agg = off', 'SET enable_seqscan = off', 'SET enable_bitmapscan = off'], 'Index Only Scan'))
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
    ap.add_argument('--track', choices=('on', 'updates', 'off'), default='on')   # on: track_updates and track_deletes; updates: track_updates alone; off: no tracking
    ap.add_argument('--reference', action='store_true', help='with --track on: also the heap-checked and the heap-only arms')
    ap.add_argument('--twin', action='store_true', help='with --track on: the b-tree twin')
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
    opts = {'on': "track_updates = 'id', track_deletes = on, ", 'updates': "track_updates = 'id', ", 'off': "track_updates = 'off', "}[a.track]
    arm_name = {'on': 'heap_free', 'updates': 'track_updates', 'off': 'track_off'}[a.track]
    twin = a.twin and a.track == 'on'
    out, plans = [], []

    def run(sql):
        _, _, err, *_ = s.run(sql)
        if err:
            raise RuntimeError('%s: %s' % (sql, err))

    def settings_of(forcing):
        base = BASE if forcing is None else ['SET max_parallel_workers_per_gather = 0'] + forcing
        return base + (['SET sift.test_no_heapfree = off'] if a.track == 'on' else [])

    def arms(forcing):
        out_ = [collect.Arm('sift', arm_name, settings=settings_of(forcing))]
        if a.reference and a.track == 'on' and forcing is None:
            out_ += [collect.Arm('sift', 'heap_checked', settings=BASE + ['SET sift.test_no_heapfree = on']), collect.Arm('heap')]
        return out_

    def plan_of(sql, settings):
        for st in settings:
            run(st)
        lines = s.run('EXPLAIN (COSTS OFF) ' + sql)[1]
        run('RESET ALL')
        return ' | '.join(l.strip() for l in lines)

    def measure(pattern, share, stage, state, settled):
        churn = None if stage == 'fresh' else 'upd=%g;ins=0;del=0;rounds=1' % share
        for name, sql, forcing, shape in STATEMENTS:
            text = sql.format(t='w')
            plan = plan_of(text, settings_of(forcing))
            plans.append((pattern, share, stage, name, a.track, shape in plan, plan))
            subject = collect.Subject('heapfree_%s%d_%s' % (pattern, share, stage), a.rows, 'arrival', state, 'w', key='tenant_id', keys=10, churn=churn, settled=settled, structures='constraint,heap,sift',
                                      variant='track_%s' % a.track)
            out.extend(collect.measure_statement(s, name, text, subject, arms(forcing), env, temps=('warm',), hash_text=sql))
            if twin and forcing is None:   # the same statement on b-trees, one record set of its own
                tw = collect.Subject('heapfree_%s%d_%s' % (pattern, share, stage), a.rows, 'arrival', state, 'wb', key='tenant_id', keys=10, churn=churn, settled=settled, structures='constraint,btree,heap',
                                     variant='btree')
                out.extend(collect.measure_statement(s, name, sql.format(t='wb'), tw, [collect.Arm('btree')], env, temps=('warm',), hash_text=sql))

    tables = ('w', 'wb') if twin else ('w',)
    for pattern, pred in PATTERNS.items():
        if pattern not in a.patterns.split(','):
            continue
        for share in map(int, a.shares.split(',')):
            pred_sql = pred.format(rows=a.rows, share=share)
            for t_ in tables:
                run('DROP TABLE IF EXISTS %s' % t_)
                run(LOAD.format(t=t_))
                run(FILL.format(t=t_, rows=a.rows))
            run('CREATE INDEX w_c ON w USING sift_col (id, ts, tenant_id, counter_a) WITH (%skey_cut = \'off\')' % opts)
            if twin:
                run('CREATE INDEX wb_ts ON wb (ts)')
                run('CREATE INDEX wb_tc ON wb (tenant_id, counter_a)')
            for t_ in tables:
                run('VACUUM (FREEZE, ANALYZE) %s' % t_)
            run('CHECKPOINT')
            measure(pattern, share, 'fresh', 'fresh_vacuumed', None)
            for t_ in tables:
                run('UPDATE %s SET counter_a = counter_a + 1 WHERE %s' % (t_, pred_sql))
            run("SELECT sift.col_promote('w_c')")
            for t_ in tables:
                run('ANALYZE %s' % t_)
            run('CHECKPOINT')
            measure(pattern, share, 'updated', 'churned_unvacuumed', False)
            run("SELECT sift.col_merge('w_c', true)")
            run('CHECKPOINT')
            measure(pattern, share, 'merged', 'churned_unvacuumed', False)
            for t_ in tables:
                run('VACUUM %s' % t_)
            run('CHECKPOINT')
            measure(pattern, share, 'vacuumed', 'churned_vacuumed', True)
            schema.write(a.out, out)
    with open(a.out + '.plans.tsv', 'w') as f:
        f.write('pattern\tshare\tstate\tclass\tmode\tplan_is_the_class_shape\tplan\n')
        for p in plans:
            f.write('\t'.join(map(str, p)) + '\n')
    off_shape = [p for p in plans if not p[5]]
    print('plans: %d of %d are not the shape of their class' % (len(off_shape), len(plans)))
    for p in off_shape[:20]:
        print('  ', p[:5], p[6][:160])
    from records import check
    for p in check.check(schema.read(a.out)):
        print(p)
    print('run_id %s: %d records' % (env.run_id, len(out)))


if __name__ == '__main__':
    main()
