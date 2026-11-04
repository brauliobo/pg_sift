#!/usr/bin/env python3
"""What a table that keeps changing leaves behind with the default `track_changes` and with it off, and what the server's own maintenance does about it: the same updates on two tables of one
database, each with a sift_col index (`default`: the option as it is, `track_off`: track_changes = 'off'), the reads of the table in four states, and in a sidecar the size of the index, the
entries of the tombstone log and the time of the settling in each.

    python3 -I bench/churn/heapfree_settle.py --host DIR --port N --out FILE [--rows 100000] [--rounds 10] [--reps 3]

Per variant: load and VACUUM FREEZE (state fresh_vacuumed), ROUNDS rounds of an UPDATE of every row with autovacuum off for the table (churned_unvacuumed), then autovacuum on for the table and
the wait until it has vacuumed, the worker of `sift.col_auto_merge` has rewritten the index and nothing is due (churned_vacuumed, `settle_s` in the sidecar), then REINDEX and the reads again (the size the index
has when built again over the same rows, the reference of the fragmentation). The server runs with its defaults for autovacuum and `sift.col_auto_merge`: the harness switches both on, with
`autovacuum_naptime` 1 s, `sift.col_auto_merge_wal_pct` and `sift.table_auto_defrag` at their defaults (a test cluster sets them off), and says so in `harness`. Records of the records library (the reads, cold and warm, with the blocks and the index
bytes); FILE.state.tsv has one row per (variant, state): the updates and their WAL, the entries of the log (`sift.col_segments` kind `tombstones`), the bytes of the index and of the table,
the segments, the autovacuums and the seconds the settling took; `fragmentation_pct` is the index of that state over the REINDEX one.
"""
import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
STEPS = os.path.join(os.path.dirname(HERE), 'steps')
sys.path[:0] = [STEPS, os.path.join(STEPS, 'records')]

import iolib  # noqa: E402
import records.collect as collect  # noqa: E402
import records.schema as schema  # noqa: E402
from duckdb import Session  # noqa: E402

REPO = os.path.dirname(os.path.dirname(HERE))
STATEMENTS = (('group_all_tenants', 'SELECT tenant_id, count(*), sum(counter_a) FROM {t} GROUP BY tenant_id'),
              ('one_tenant', 'SELECT count(*), sum(counter_a) FROM {t} WHERE tenant_id = 7'))
BASE = ['SET max_parallel_workers_per_gather = 0', 'SET sift.enable_agg = on']
# (variant, the table, the index options)
VARIANTS = (('default', 'w_default', ''), ('track_off', 'w_off', " WITH (track_changes = 'off')"))
CONFIG = ('ALTER SYSTEM SET autovacuum = on', "ALTER SYSTEM SET autovacuum_naptime = '1s'", 'ALTER SYSTEM SET sift.col_auto_merge = on', 'ALTER SYSTEM SET sift.col_auto_merge_wal_pct = 5',
          'ALTER SYSTEM SET sift.table_auto_defrag = on', 'SELECT pg_reload_conf()')
COLUMNS = 'variant state rounds updates churn_s churn_wal_bytes log_entries index_bytes table_bytes segments autovacuums settle_s fragmentation_pct'.split()


def tip(a):
    return a.tip or subprocess.run(['git', '-C', REPO, 'rev-parse', '--short=12', 'HEAD'], capture_output=True, text=True).stdout.strip() or 'unknown'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', required=True)
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--rows', type=int, default=100000)
    ap.add_argument('--rounds', type=int, default=10, help='rounds of an UPDATE of every row')
    ap.add_argument('--reps', type=int, default=3)
    ap.add_argument('--db', default='heapfree_settle')
    ap.add_argument('--wait', type=int, default=600, help='seconds the settling may take')
    ap.add_argument('--quiet', type=int, default=30, help='seconds the index must keep one state for the table to be settled')
    ap.add_argument('--tip')
    ap.add_argument('--note')
    a = ap.parse_args()
    admin = Session(a.port, 600, None, db='postgres', host=a.host)
    for sql in CONFIG:
        admin.run(sql)
    admin.run('DROP DATABASE IF EXISTS %s' % a.db)
    admin.run('CREATE DATABASE %s' % a.db)
    s = Session(a.port, 600, None, db=a.db, host=a.host)
    s.run('CREATE EXTENSION pg_sift')
    s.run('CREATE EXTENSION IF NOT EXISTS pg_buffercache')
    t = tip(a)
    harness = 'heapfree settle (autovacuum on, naptime 1s, sift.col_auto_merge on' + (', %s' % a.note if a.note else '') + ')'
    env = collect.Env(t, harness, schema.run_id(t), collect.host_cpus(collect.postmaster_pid(collect.backend_pid(s))), reps=a.reps, cool=iolib.cool, maintenance='default')
    out, state = [], []

    def run(sql):
        _, _, err, *_ = s.run(sql)
        if err and 'ERROR' in err:
            raise RuntimeError('%s: %s' % (sql, err))

    def facts(variant, table, name, **extra):
        s.run('SELECT pg_stat_clear_snapshot()')
        index = table + '_c'
        log, segments = (int(x) for x in collect.one(s, "SELECT coalesce(sum(rows) FILTER (WHERE kind = 'tombstones'), 0), count(*) FILTER (WHERE kind = 'immutable') FROM sift.col_segments('%s')" % index))
        index_bytes, table_bytes, vacuums = (int(x) for x in collect.one(s, "SELECT pg_relation_size('%s'), pg_table_size('%s'), autovacuum_count FROM pg_stat_all_tables WHERE relid = '%s'::regclass"
                                                                         % (index, table, table)))
        state.append(dict(variant=variant, state=name, log_entries=log, index_bytes=index_bytes, table_bytes=table_bytes, segments=segments, autovacuums=vacuums, **extra))

    def reads(variant, table, name, churn, settled, state='fresh_vacuumed'):
        for stmt, sql in STATEMENTS:
            subject = collect.Subject('heapfree_settle_%s' % name, a.rows, 'arrival', state, table, key='tenant_id', keys=10, churn=churn, settled=settled, structures='constraint,heap,sift',
                                      variant=variant)
            out.extend(collect.measure_statement(s, stmt, sql.format(t=table), subject, [collect.Arm('sift', variant, settings=BASE)], env, temps=('cold', 'warm'), hash_text=sql))

    def due(index):
        return collect.one(s, "SELECT sift.col_maintenance_due('%s', true) IS NOT NULL OR EXISTS (SELECT 1 FROM pg_stat_activity WHERE backend_type = 'sift maintenance')" % index)[0] == 't'

    for variant, table, opts in VARIANTS:
        index = table + '_c'
        run('DROP TABLE IF EXISTS %s' % table)
        run('CREATE TABLE %s (id int PRIMARY KEY, tenant_id int NOT NULL, counter_a int, label text) WITH (autovacuum_enabled = false, fillfactor = 100)' % table)
        run("INSERT INTO %s SELECT i, i %% 10, i %% 1000, md5(i::text) FROM generate_series(1, %d) i" % (table, a.rows))
        run('CREATE INDEX %s ON %s USING sift_col (id, tenant_id, counter_a)%s' % (index, table, opts))
        run('VACUUM (FREEZE, ANALYZE) %s' % table)
        run('CHECKPOINT')
        facts(variant, table, 'fresh_vacuumed')
        reads(variant, table, 'fresh_vacuumed', None, None)
        lsn, t0 = collect.wal_lsn(s), time.time()
        churn = collect.apply_churn(s, table, 'counter_a', upd=100, rounds=a.rounds)
        elapsed, wal = time.time() - t0, collect.wal_lsn(s) - lsn
        run('ANALYZE %s' % table)
        run('CHECKPOINT')
        facts(variant, table, 'churned_unvacuumed', rounds=a.rounds, updates=a.rounds * a.rows, churn_s='%.1f' % elapsed, churn_wal_bytes=wal)
        reads(variant, table, 'churned_unvacuumed', churn, False, 'churned_unvacuumed')
        run('ALTER TABLE %s SET (autovacuum_enabled = true, autovacuum_vacuum_scale_factor = 0.02, autovacuum_vacuum_threshold = 50)' % table)
        last = None
        t0 = since = time.time()
        while True:   # settled: autovacuum has run, nothing is due and the index has kept one state for --quiet seconds (the worker of the rewrite starts after the VACUUM, not at it)
            s.run('SELECT pg_stat_clear_snapshot()')
            vacuumed = collect.one(s, "SELECT autovacuum_count FROM pg_stat_all_tables WHERE relid = '%s'::regclass" % table)[0] not in ('0', '')
            now = tuple(collect.one(s, "SELECT pg_relation_size('%s'), count(*), coalesce(sum(rows), 0) FROM sift.col_segments('%s')" % (index, index)))
            if vacuumed and not due(index):
                if now != last:
                    last, since = now, time.time()
                elif time.time() - since >= a.quiet:
                    break
            else:
                last = None
            if time.time() - t0 > a.wait:
                raise RuntimeError('%s not settled after %d s' % (table, a.wait))
            time.sleep(1)
        settle = since - t0
        run('ALTER TABLE %s SET (autovacuum_enabled = false)' % table)
        run('CHECKPOINT')
        facts(variant, table, 'churned_vacuumed', settle_s='%.1f' % settle)
        reads(variant, table, 'churned_vacuumed', churn, True, 'churned_vacuumed')
        run('REINDEX INDEX %s' % index)
        run('CHECKPOINT')
        facts(variant, table, 'reindexed')
        reads(variant, table, 'reindexed', churn, True, 'churned_vacuumed')
    ref = {r['variant']: r['index_bytes'] for r in state if r['state'] == 'reindexed'}
    for r in state:
        r['fragmentation_pct'] = '%.2f' % (100.0 * (r['index_bytes'] / ref[r['variant']] - 1)) if r['state'] != 'reindexed' else ''
    schema.write(a.out, out)
    with open(a.out + '.state.tsv', 'w') as f:
        f.write('\t'.join(COLUMNS) + '\n')
        for r in state:
            f.write('\t'.join(str(r.get(c, '')) for c in COLUMNS) + '\n')
    from records import check
    for p in check.check(schema.read(a.out)):
        print(p)
    print('run_id %s: %d records' % (env.run_id, len(out)))
    for r in state:
        print('  '.join('%s=%s' % (c, r.get(c, '')) for c in COLUMNS))


if __name__ == '__main__':
    main()
