#!/usr/bin/env python3
"""The records of a sift_table under churn (CH-8): the reads of a fresh, a churned (no pass) and a settled (VACUUMed) table, and the VACUUM that settles it.

    python3 -I bench/churn/tbl_records.py --host DIR --port N --out FILE [--rows 300000] [--deletes 1,2,4.5,10] [--reps 3] [--standby-dir D --standby-port P] [--col]

The server is a plain default cluster (autovacuum on, sift.col_auto_merge on; fsync off on tmpfs is the only change). The tables set autovacuum_enabled = false, and the arms of the churned
state run with sift.table_auto_defrag = off in the session (`maintenance` = `disabled` in those rows): the reads an arm makes are what pays for the worker's pass (`src/table/vacuum_maint.c`),
so with it on the state would not be the one the churn made. The fresh and the settled states run on the defaults.
Each state is a dataset of its own (`tbl_churn_del<share>_<state>`): a comparison is one state. Statements: Q1 a key of sixty, Q2 the grouped aggregate; the maintenance row is the VACUUM
of the churned table (a share of the rows UPDATEd before each repetition), instructions of the whole cluster, workers included, and its WAL. `--standby-dir` also reads the churned
and the settled state of the last share on a physical standby (its pg_stat has no VACUUM count: the rows of a settled standby read carry `vacuums` 0 and the checker says so); `--col` measures a sift_col index instead (below).
"""
import argparse
import copy
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

Q1 = 'SELECT count(*), avg(n) FROM {t} WHERE key = 7'
Q2 = 'SELECT key, count(*), sum(n) FROM {t} GROUP BY key'


class Standby(Session):
    """a session on a standby: the WAL position the collector reads is the replay position (a standby writes none, so a read has no WAL)"""

    def run(self, sql):
        return super().run(sql.replace('pg_current_wal_lsn()', 'pg_last_wal_replay_lsn()'))


def standby(a, primary_session, name, table):
    """a physical standby of the server in this state, started on a.standby_dir; the session on it once it replayed the state. The caller stops it with stop_standby"""
    d = '%s/%s' % (a.standby_dir, name)
    subprocess.run(['rm', '-rf', d], check=True)
    os.makedirs(a.standby_dir, exist_ok=True)
    primary_session.run('CHECKPOINT')
    subprocess.run(['pg_basebackup', '-h', a.host, '-p', str(a.port), '-U', 'postgres', '-D', d, '-R', '-c', 'fast', '-X', 'stream'], check=True, capture_output=True)
    with open(d + '/postgresql.conf', 'a') as f:
        f.write("port = %d\nunix_socket_directories = '%s'\n" % (a.standby_port, a.standby_dir))
    subprocess.run(['pg_ctl', '-D', d, '-l', d + '.log', '-w', 'start'], check=True, capture_output=True)
    return Standby(a.standby_port, 600, None, db=a.db, host=a.standby_dir), d


def stop_standby(d):
    subprocess.run(['pg_ctl', '-D', d, '-m', 'fast', 'stop'], capture_output=True)
    subprocess.run(['rm', '-rf', d])


def build(s, table, rows):
    s.run('DROP TABLE IF EXISTS %s' % table)
    s.run('CREATE TABLE %s (id bigint, key int, n int, ts timestamptz, pad text) USING sift_table WITH (autovacuum_enabled = false)' % table)
    third = rows // 3
    for b in range(3):
        lo, hi = b * third + 1, (b + 1) * third if b < 2 else rows
        s.run("INSERT INTO %s SELECT g, g %% 60, g %% 1000, timestamptz '2025-01-01' + g * interval '1 minute', md5(g::text) FROM generate_series(%d, %d) g" % (table, lo, hi))
    s.run('VACUUM (ANALYZE) %s' % table)
    s.run('CHECKPOINT')


def measure(s, env, rows, share, state, table, churn, settled, reps, state_name=None):
    out = []
    ds = 'tbl_churn_del%s_%s' % (share, state)
    for name, text in (('q1_one_key', Q1), ('q2_group_all_keys', Q2)):
        sql = text.format(t=table)
        subject = collect.Subject(ds, rows, 'arrival', state_name or state, table, key='key', keys=60, churn=churn, settled=settled, structures='sift')
        out += collect.measure_statement(s, name, sql, subject, [collect.Arm('sift'), collect.Arm('heap')], env, hash_text=text)
    return out


def col_mode(a):
    """the cost of the read-path tick of an index-only scan (CH-10) and of the VACUUM that seals a buffer by rows (CH-11), on the build the server runs, nothing dirty: `h`, a heap table
    of ROWS rows with 3,000 keys and a sift_col index on (k, v) for the index-only scans, and `pt`, the same rows with a nearly unique key u and an index on (u, v), probed by 1,000 keys
    of `probes` (the join reads pad, which the index does not hold: a plain index scan for each key). Compare the files of two builds by their statements and sizes; each is one run"""
    s = Session(a.port, 600, None, db='postgres', host=a.host)
    s.run('DROP DATABASE IF EXISTS %s' % a.db)
    s.run('CREATE DATABASE %s' % a.db)
    s = Session(a.port, 600, None, db=a.db, host=a.host)
    s.run('CREATE EXTENSION pg_sift')
    s.run('CREATE EXTENSION IF NOT EXISTS pg_buffercache')
    t = collect.git_tip()
    env = collect.Env(t, 'tbl churn col', schema.run_id(t), collect.host_cpus(collect.postmaster_pid(collect.backend_pid(s))), reps=a.reps, cool=iolib.cool)
    n = a.rows
    s.run('CREATE TABLE h (id int PRIMARY KEY, k int, v int, pad text) WITH (fillfactor = 70, autovacuum_enabled = false)')
    s.run('INSERT INTO h SELECT g, g % 3000, g % 1000, md5(g::text) FROM generate_series(1, ' + str(n) + ') g')
    s.run('CREATE INDEX h_c ON h USING sift_col (k, v)')
    for tb in ('pt', 'pt2'):
        s.run('CREATE TABLE %s (id int PRIMARY KEY, u int, v int, pad text) WITH (autovacuum_enabled = false)' % tb)
        s.run('INSERT INTO %s SELECT g, (g::bigint * 7919 %% 1000003)::int, g %% 1000, md5(g::text) FROM generate_series(1, %d) g' % (tb, n))
        s.run('CREATE INDEX %s_c ON %s USING sift_col (u, v)' % (tb, tb))
    s.run('CREATE TABLE probes AS SELECT (g::bigint * 7919 % 1000003)::int AS u FROM generate_series(1, 1000) g')
    s.run('VACUUM (ANALYZE, FREEZE) h')
    s.run('VACUUM (ANALYZE, FREEZE) pt')
    s.run('VACUUM (ANALYZE, FREEZE) pt2')
    s.run('ANALYZE probes')
    s.run('CHECKPOINT')
    ioss = ['SET enable_seqscan = off', 'SET enable_bitmapscan = off', 'SET enable_indexscan = off', 'SET sift.enable_agg = off', 'SET max_parallel_workers_per_gather = 0']
    probe = ['SET enable_hashjoin = off', 'SET enable_mergejoin = off', 'SET enable_seqscan = off', 'SET sift.enable_agg = off', 'SET max_parallel_workers_per_gather = 0']
    join = 'SELECT sum(length(t.pad)) FROM probes p JOIN %s t ON t.u = p.u'
    out = []
    for name, table, sql, sets in (('ioss_100k_rows', 'h', 'SELECT k, v FROM h WHERE k BETWEEN 100 AND 400', ioss), ('ioss_one_key', 'h', 'SELECT k, v FROM h WHERE k = 7', ioss),
                                   ('probe_join_1000', 'pt', join % 'pt', probe)):
        subject = collect.Subject('col_nothing_dirty', n, 'other', 'fresh_vacuumed', table, key='k' if table == 'h' else 'u', keys=3000 if table == 'h' else 1000003, structures='heap,sift')
        for st in sets:
            s.run(st)
        out += collect.measure_statement(s, name, sql, subject, [collect.Arm('sift', settings=sets), collect.Arm('heap')], env, temps=('warm',))
        s.run('RESET ALL')

    def insert45k(tb):
        s.run('INSERT INTO %s SELECT g + (SELECT max(id) FROM %s), (g::bigint * 7919 %% 1000003)::int, g %% 1000, md5(g::text) FROM generate_series(1, 45000) g' % (tb, tb))
        s.run('CHECKPOINT')
    # 45,000 rows written to a vacuumed table: the probes while they sit in the buffer (or in what the write made of them), then after the VACUUM
    insert45k('pt2')
    for st in probe:
        s.run(st)
    for state, settled, dataset in (('churned_unvacuumed', False, 'col_buffer_45k_probe_open'), ('churned_vacuumed', True, 'col_buffer_45k_probe_vacuumed')):
        if settled:
            s.run('VACUUM pt2')
        subject = collect.Subject(dataset, n, 'other', state, 'pt2', key='u', keys=1000003, churn='ins=45000 rows', settled=settled, structures='heap,sift')
        out += collect.measure_statement(s, 'probe_join_1000', join % 'pt2', subject, [collect.Arm('sift', settings=probe), collect.Arm('heap')], env, temps=('warm',))
    s.run('RESET ALL')
    subject = collect.Subject('col_buffer_45k', n, 'other', 'churned_unvacuumed', 'pt', kind='maintenance', churn='ins=45000 rows', settled=False, structures='heap,sift')
    out += collect.measure_statement(s, 'vacuum_after_45k_inserts', 'VACUUM pt', subject, [collect.Arm('sift')], env, prepare=lambda: insert45k('pt'), temps=('warm',))
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
    ap.add_argument('--rows', type=int, default=300000)
    ap.add_argument('--deletes', default='1,2,4.5,10')
    ap.add_argument('--reps', type=int, default=3)
    ap.add_argument('--db', default='tbl_churn_records')
    ap.add_argument('--standby-dir', default=None, help='measure the reads of the first share on a standby of the server in each state, started here')
    ap.add_argument('--standby-port', type=int, default=0)
    ap.add_argument('--col', action='store_true', help='the nothing-dirty cost of the read-path tick and of the row-sealing VACUUM on a sift_col index, not the sift_table states')
    a = ap.parse_args()
    if a.col:
        return col_mode(a)
    admin = Session(a.port, 600, None, db='postgres', host=a.host)
    admin.run('DROP DATABASE IF EXISTS %s' % a.db)
    admin.run('CREATE DATABASE %s' % a.db)
    s = Session(a.port, 600, None, db=a.db, host=a.host)
    s.run('CREATE EXTENSION pg_sift')
    s.run('CREATE EXTENSION IF NOT EXISTS pg_buffercache')
    mon = Session(a.port, 600, None, db=a.db, host=a.host)
    t = collect.git_tip()
    pid = collect.backend_pid(s)
    env = collect.Env(t, 'tbl churn', schema.run_id(t), collect.host_cpus(collect.postmaster_pid(pid)), reps=a.reps, cool=iolib.cool, maintenance='default')
    off = copy.copy(env)
    off.maintenance = 'disabled'
    rows_out = []
    for share in a.deletes.split(','):
        table = 'tt_%s' % share.replace('.', '_')
        build(s, table, a.rows)
        rows_out += measure(s, env, a.rows, share, 'fresh_vacuumed', table, None, None, a.reps)
        churn = collect.apply_churn(s, table, 'n', dele=float(share))
        s.run('CHECKPOINT')
        s.run('SET sift.table_auto_defrag = off')
        rows_out += measure(s, off, a.rows, share, 'churned_unvacuumed', table, churn, False, a.reps)
        s.run('RESET sift.table_auto_defrag')
        if a.standby_dir and share == a.deletes.split(',')[-1]:
            sb, d = standby(a, s, 'dirty', table)
            sb.run('SET sift.table_auto_defrag = off')
            rows_out += measure(sb, off, a.rows, share, 'churned_unvacuumed_standby', table, churn, False, a.reps, state_name='churned_unvacuumed')
            stop_standby(d)
        # the VACUUM that settles it: each repetition churns the table again (an UPDATE of the share) and VACUUMs it, instructions and WAL of the cluster
        prepare = lambda: (collect.apply_churn(s, table, 'n', upd=float(share)), s.run('CHECKPOINT'))   # noqa: E731
        subject = collect.Subject('tbl_churn_del%s_vacuum' % share, a.rows, 'arrival', 'churned_unvacuumed', table, kind='maintenance', churn=churn, settled=False, structures='sift')
        rows_out += collect.measure_statement(s, 'vacuum', 'VACUUM %s' % table, subject, [collect.Arm('sift')], env, prepare=prepare, monitor=mon, temps=('warm',))
        s.run('VACUUM %s' % table)
        s.run('CHECKPOINT')
        rows_out += measure(s, env, a.rows, share, 'churned_vacuumed', table, churn, True, a.reps)
        if a.standby_dir and share == a.deletes.split(',')[-1]:
            sb, d = standby(a, s, 'settled', table)
            rows_out += measure(sb, env, a.rows, share, 'churned_vacuumed_standby', table, churn, True, a.reps, state_name='churned_vacuumed')
            stop_standby(d)
    schema.write(a.out, rows_out)
    from records import check
    for p in check.check(schema.read(a.out)):
        print(p)
    print('run_id %s: %d records' % (env.run_id, len(rows_out)))


if __name__ == '__main__':
    main()
