#!/usr/bin/env python3
"""Read penalty and cleanup cost of a heap table with a sift_col index after churn (CH-15): the constants of the priced maintenance gate (sift_col_vacuum_tick) and its cells.

    python3 -I bench/churn/gate_records.py constants --host DIR --port N --out FILE [--rows 300000] [--shares 1,5,20] [--reps 3]

The server is a plain default cluster. `constants` takes, for each share of the rows UPDATEd (a covered column, so no HOT update) and each selection (recent contiguous ids, or a hash of
the id), the instructions of three reads while the churn is unrepaired (the table has autovacuum off and the session sift.col_auto_merge off), the rows each read checked in the heap, the
instructions and WAL of the VACUUM that repairs it (cluster-wide, workers included) and the reads after it. Each state is a dataset of its own: a comparison is one state.
"""
import argparse
import copy
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
STEPS = os.path.join(os.path.dirname(HERE), 'steps')
sys.path[:0] = [STEPS, os.path.join(STEPS, 'records')]

import iolib  # noqa: E402
import records.collect as collect  # noqa: E402
import records.schema as schema  # noqa: E402
from duckdb import Session  # noqa: E402

READS = (('agg_group', 'SELECT k, count(*), sum(v) FROM t GROUP BY k', []),
         ('ioss_range', 'SELECT k, v FROM t WHERE k BETWEEN 100 AND 129', ['SET enable_seqscan = off', 'SET enable_bitmapscan = off', 'SET enable_indexscan = off', 'SET sift.enable_agg = off']),
         ('ioss_key', 'SELECT k, v FROM t WHERE k = 7', ['SET enable_seqscan = off', 'SET enable_bitmapscan = off', 'SET enable_indexscan = off', 'SET sift.enable_agg = off']))
OFF = ['SET sift.col_auto_merge = off', 'SET max_parallel_workers_per_gather = 0']


def build(s, n, row_key, autovac=False):
    s.run('DROP TABLE IF EXISTS t')
    s.run('CREATE TABLE t (id int PRIMARY KEY, k int, v int, pad text) WITH (autovacuum_enabled = %s)' % ('true' if autovac else 'false'))
    s.run('INSERT INTO t SELECT g, (g::bigint * 7919 %% 3000)::int, g %% 1000, md5(g::text) FROM generate_series(1, %d) g' % n)
    s.run("CREATE INDEX t_c ON t USING sift_col (id, k, v)%s" % (" WITH (track_changes = 'id')" if row_key else ''))
    s.run('VACUUM (ANALYZE, FREEZE) t')
    s.run('CHECKPOINT')


def churn(s, n, share, sel):
    where = 'id > %d' % (n - n * share // 100) if sel == 'recent' else '((hashint8(id)::bigint & 2147483647) %% 100) < %d' % share
    s.run('UPDATE t SET v = v + 1 WHERE %s' % where)
    s.run('CHECKPOINT')
    return 'upd=%g;sel=%s;noHOT' % (share, sel)


def vetted(s, sql, sets):
    """the rows a read checked in the heap: `vetted by heap` of the aggregate scan, Heap Fetches of an executor index-only scan"""
    for x in sets + OFF:
        s.run(x)
    out = s.run('EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' + sql)
    s.run('RESET ALL')
    text = ' '.join(out[1])
    m = re.search(r'(\d+) vetted by heap', text) or re.search(r'Heap Fetches: (\d+)', text)
    return int(m.group(1)) if m else 0


def reads(s, env, n, table_state, tag, declared, rows_out, vet_out, reps):
    for name, sql, sets in READS:
        sub = collect.Subject('gate_%s' % tag, n, 'other', table_state, 't', key='k', keys=3000, structures='constraint,heap,sift', churn=declared, settled=table_state.endswith('vacuumed'))
        arm_sets = sets + OFF
        for x in arm_sets:
            s.run(x)
        rows_out += collect.measure_statement(s, name, sql, sub, [collect.Arm('sift', settings=arm_sets)], env, temps=('warm',))
        s.run('RESET ALL')
        vet_out.append((tag, table_state, name, vetted(s, sql, sets)))


def constants(a):
    admin = Session(a.port, 600, None, db='postgres', host=a.host)
    admin.run('DROP DATABASE IF EXISTS %s' % a.db)
    admin.run('CREATE DATABASE %s' % a.db)
    s = Session(a.port, 600, None, db=a.db, host=a.host)
    s.run('CREATE EXTENSION pg_sift')
    s.run('CREATE EXTENSION IF NOT EXISTS pg_visibility')
    mon = Session(a.port, 600, None, db=a.db, host=a.host)
    tip = subprocess.run(['git', '-C', REPO, 'rev-parse', '--short=12', 'HEAD'], capture_output=True, text=True).stdout.strip() or 'unknown'
    env = collect.Env(tip, 'gate constants', schema.run_id(tip), collect.host_cpus(collect.postmaster_pid(collect.backend_pid(s))), reps=a.reps, cool=iolib.cool, maintenance='default')
    off = copy.copy(env)
    off.maintenance = 'disabled'
    rows_out, vet_out = [], []
    for row_key in a.row_key:
        for sel in a.sel:
            for share in a.shares:
                tag = '%s_rk%d_%s_%g' % (a.rows, row_key, sel, share)
                build(s, a.rows, row_key)
                size = s.run("SELECT pg_relation_size('t'), pg_relation_size('t_c')")[1]
                declared = churn(s, a.rows, share, sel)
                summary = s.run("SELECT count(*) FILTER (WHERE NOT all_visible), count(*) FROM pg_visibility_map('t')")[1]
                reads(s, off, a.rows, 'churned_unvacuumed', tag, declared, rows_out, vet_out, a.reps)
                prepare = lambda: (churn(s, a.rows, share, sel), s.run('CHECKPOINT'))   # noqa: E731
                sub = collect.Subject('gate_%s_vacuum' % tag, a.rows, 'other', 'churned_unvacuumed', 't', kind='maintenance', churn=declared, settled=False, structures='constraint,heap,sift')
                rows_out += collect.measure_statement(s, 'vacuum', 'VACUUM t', sub, [collect.Arm('sift')], env, prepare=prepare, monitor=mon, temps=('warm',))
                s.run('VACUUM t')
                s.run('CHECKPOINT')
                reads(s, off, a.rows, 'churned_vacuumed', tag, declared, rows_out, vet_out, a.reps)
                print('cell %s sizes %s notallvisible/pages %s' % (tag, size, summary), flush=True)
    schema.write(a.out, rows_out)
    with open(a.out + '.vetted', 'w') as f:
        f.write('cell\tstate\tstatement\tvetted\n' + ''.join('%s\t%s\t%s\t%d\n' % v for v in vet_out))
    from records import check
    for p in check.check(schema.read(a.out)):
        print(p)


ARMS = {'stopped': ['sift.col_auto_merge = off'], 'tip': ['sift.col_auto_merge = on'], 'priced': ['sift.col_auto_merge = on', "sift.eval_methods = 'penalty_maintenance'"]}


def quiet(s, limit=90):
    """until no maintenance worker of the database is alive"""
    end = time.time() + limit
    while time.time() < end:
        n = s.run("SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'sift maintenance' OR backend_type = 'autovacuum worker'")[1]
        if n and n[0] == '0':
            return True
        time.sleep(0.5)
    return False


def window(a):
    """churn, then READS per second for SECONDS under each arm of the gate: the instructions of the reader, the postmaster and its workers over the window and until the maintenance drained, the WAL,
    the VACUUMs, then the reads the table gives afterwards"""
    admin = Session(a.port, 600, None, db='postgres', host=a.host)
    admin.run('DROP DATABASE IF EXISTS %s' % a.db)
    admin.run('CREATE DATABASE %s' % a.db)
    s = Session(a.port, 600, None, db=a.db, host=a.host)
    s.run('CREATE EXTENSION pg_sift')
    s.run('CREATE EXTENSION IF NOT EXISTS pg_visibility')
    tip = subprocess.run(['git', '-C', REPO, 'rev-parse', '--short=12', 'HEAD'], capture_output=True, text=True).stdout.strip() or 'unknown'
    post = collect.postmaster_pid(collect.backend_pid(s))
    env = collect.Env(tip, 'gate window', schema.run_id(tip), collect.host_cpus(post), reps=a.reps, cool=iolib.cool, maintenance='default')
    off = copy.copy(env)
    off.maintenance = 'disabled'
    rows_out, side = [], []
    for row_key in a.row_key:
        for sel in a.sel:
            for share in a.shares:
                for rate in a.rates:
                    for arm in a.arms:
                        tag = '%s_rk%d_%s_%g_r%g_%s' % (a.rows, row_key, sel, share, rate, arm)
                        s.run('ALTER DATABASE %s RESET ALL' % a.db)
                        for x in ARMS[arm]:
                            s.run('ALTER DATABASE %s SET %s' % (a.db, x))
                        w = Session(a.port, 600, None, db=a.db, host=a.host)
                        build(w, a.rows, row_key, arm != 'stopped')
                        declared = churn(w, a.rows, share, sel)
                        quiet(w, 30)
                        v0 = w.run("SELECT vacuum_count + autovacuum_count FROM pg_stat_user_tables WHERE relname = 't'")[1][0]
                        lsn0 = collect.wal_lsn(w)
                        rd = Session(a.port, 600, None, db=a.db, host=a.host)
                        rpid = collect.backend_pid(rd)
                        collect.check_perf_free()
                        pf = subprocess.Popen(['bash', collect.PERF, '%d' % rpid], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
                        pm = subprocess.Popen(['bash', collect.PERF, '%d' % post], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
                        n = 0
                        t_read = 0.0
                        try:
                            collect.wait_attached(pf)
                            collect.wait_attached(pm)
                            t0 = time.time()
                            while time.time() - t0 < a.seconds:
                                if rate > 0:
                                    name, sql, sets = READS[n % len(READS)]
                                    t1 = time.time()
                                    rd.run(';'.join(sets + [sql, 'RESET ALL']))
                                    t_read += time.time() - t1
                                    n += 1
                                    time.sleep(max(0.0, 1.0 / rate - (time.time() - t1)))
                                else:
                                    time.sleep(0.5)
                            drained = quiet(w, a.drain)
                        finally:
                            out = collect.stop_perf(pf)
                            outm = collect.stop_perf(pm)
                        f, fm = out.split(), outm.split()
                        instr = int(f[1]) / 1e6 if len(f) == 3 and f[1].isdigit() else None
                        minstr = int(fm[1]) / 1e6 if len(fm) == 3 and fm[1].isdigit() else None
                        v1 = w.run("SELECT vacuum_count + autovacuum_count FROM pg_stat_user_tables WHERE relname = 't'")[1][0]
                        wal = collect.wal_lsn(w) - lsn0 if isinstance(lsn0, int) else None
                        notav = w.run("SELECT count(*) FILTER (WHERE NOT all_visible) FROM pg_visibility_map('t')")[1][0]
                        dirty_units = w.run("SELECT coalesce(sum(units) FILTER (WHERE dirty), 0), coalesce(sum(units), 0) FROM sift.col_segments('t_c')")[1][0]
                        side.append((tag, a.rows, row_key, sel, share, rate, arm, n, round(t_read, 3), instr, minstr, wal, int(v1) - int(v0), notav, dirty_units, drained))
                        print('window', side[-1], flush=True)
                        for x in OFF:
                            w.run(x)
                        reads(w, off, a.rows, 'churned_%s' % arm, tag, declared, rows_out, [], a.reps)
                        quiet(w, 30)
    schema.write(a.out, rows_out)
    with open(a.out + '.windows', 'w') as f:
        f.write('cell\trows\trow_key\tsel\tshare\treads_per_s\tgate\treads\tread_s\treader_minstr\tserver_minstr\twal_bytes\tvacuums\tnot_allvisible_pages\tdirty_units\tdrained\n')
        for r in side:
            f.write('\t'.join(str(x) for x in r) + '\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mode', choices=['constants', 'window'])
    ap.add_argument('--host', required=True)
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--rows', type=int, default=300000)
    ap.add_argument('--shares', type=lambda x: [float(v) for v in x.split(',')], default=[1, 5, 20])
    ap.add_argument('--sel', type=lambda x: x.split(','), default=['recent', 'hash'])
    ap.add_argument('--row-key', type=lambda x: [int(v) for v in x.split(',')], default=[0])
    ap.add_argument('--reps', type=int, default=3)
    ap.add_argument('--db', default='gate_records')
    ap.add_argument('--rates', type=lambda x: [float(v) for v in x.split(',')], default=[0, 2, 20])
    ap.add_argument('--arms', type=lambda x: x.split(','), default=['stopped', 'tip', 'priced'])
    ap.add_argument('--seconds', type=int, default=30)
    ap.add_argument('--drain', type=int, default=60)
    a = ap.parse_args()
    constants(a) if a.mode == 'constants' else window(a)


if __name__ == '__main__':
    main()
