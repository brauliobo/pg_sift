"""The replication workloads of the load gate: a hot standby read under the writes, the VACUUM and the maintenance of the primary and promoted, and logical decoding of a sift_table
to a subscriber through bulk inserts of toasted rows, churn, TRUNCATE and changes of the publication (scripts/load/suites, test/t/core/load_repl.sh: wal_level logical, a standby
at LOAD_STANDBY_HOST and LOAD_STANDBY_PORT, LOAD_PROMOTE and LOAD_RESTANDBY to promote it and clone a new one).
"""
from __future__ import annotations

import os
import sys
import time

sys.dont_write_bytecode = True

from cases import LoadCase  # noqa: E402
from collect import AGG, BM, add, base, checks, engines, hygiene, ledger_writes, multi_kept, oracles  # noqa: E402
from suites.common import Limits, anon_peak_mb  # noqa: E402

DIGEST = ("SELECT count(*), md5(string_agg(id || ':' || v, ',' ORDER BY id)) FROM load_case.e_tbl", AGG.format(t='load_case.e_col'), BM.format(t='load_case.e_bm'))
CDC = "SELECT count(*), coalesce(md5(string_agg(id || ':' || v || ':' || md5(body), ',' ORDER BY id)), '') FROM load_case.cdc"
CAP = 60.0


def standby() -> dict:
    host, port = os.environ.get('LOAD_STANDBY_HOST'), os.environ.get('LOAD_STANDBY_PORT')
    if not host or not port:
        raise RuntimeError('the case needs LOAD_STANDBY_HOST and LOAD_STANDBY_PORT: the hot standby of the test')
    return {'PGHOST': host, 'PGPORT': port}


def catchup(db, st) -> int:
    """0 once the standby ST replayed what the primary DB wrote so far, 1 after CAP seconds (a liveness cap, not a measured time)"""
    lsn = db.val('SELECT pg_current_wal_insert_lsn()')
    end = time.monotonic() + CAP
    while st.val(f"SELECT pg_last_wal_replay_lsn() >= '{lsn}'") != 't':
        if time.monotonic() > end:
            return 1
        time.sleep(0.02)
    return 0


def snapshot_oracle(w, sql: str, guc: str) -> int:
    """oracle() for a session of the standby: a statement cancelled by a conflict with the recovery (40001) is expected and no mismatch"""
    out = w.do(f'BEGIN ISOLATION LEVEL REPEATABLE READ; SET LOCAL {guc} = on; {sql}; SET LOCAL {guc} = off; {sql}; COMMIT', ('40001',))
    return 0 if out is None else int(out[:len(out) // 2] != out[len(out) // 2:])


def repl_standby_failover(ctx):
    """4 writers of the three engines, VACUUM and the maintenance on the primary under a REPEATABLE READ snapshot held for the whole load, while 4 sessions of the hot standby
    compare the index paths with the heap in one snapshot and look keys up; then the standby catches up and answers what the primary does, is promoted, takes writes and its
    sift.check, quarantine and health are clean; a new standby is cloned for the next case"""
    db, conn = ctx.db, standby()
    engines(db)
    st = ctx.sessions(1, conn=conn)[0]
    lag = catchup(db, st)
    hold = ctx.sessions(1)[0]
    hold.run('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM e_tbl')
    ws = ctx.sessions(6, 'SET sift.async_maintenance = on')
    rs = ctx.sessions(4, conn=conn)

    def maint(w, i):
        w.do(('VACUUM e_tbl, e_col, e_bm', "SELECT count(*) FROM sift.maintain('e_col_ix')", "SELECT count(*) FROM sift.maintain('e_tbl')")[i % 3])

    def read(w, i):
        if i % 3 == 2:
            w.do(f'SELECT v FROM e_tbl WHERE id = {1 + (w.seed * 977 + i) % 8192}', ('40001',))
        else:
            w.ledger += snapshot_oracle(w, (AGG.format(t='e_col'), BM.format(t='e_bm'))[i % 3], ('sift.enable_agg', 'sift.enable_scan')[i % 3])

    busy = ctx.phase({'w': (ws[:4], ledger_writes('e')), 'm': (ws[4:5], maint), 'r': (rs, read)}, 2.0)
    hold.run('COMMIT')
    lag += catchup(db, st)
    differ = sum(db.run(q) != st.run(q) for q in DIGEST)
    m = base(ctx, busy, 'w', **add(multi_kept(db, 'e', ws[:4]), {'oracle_mismatch': oracles(db)}), standby_mismatch=differ + lag + sum(w.ledger for w in rs),
             conflicts=busy['r'].expected, standby_reads=busy['r'].ops)
    ctx.hook('LOAD_PROMOTE')
    end = time.monotonic() + CAP
    while st.val('SELECT pg_is_in_recovery()') == 't' and time.monotonic() < end:
        time.sleep(0.02)
    _, err, _ = st.q("INSERT INTO e_tbl VALUES (99999999, 1, 1, 'wire promoted'); INSERT INTO e_col VALUES (99999999, 1, 1, 'wire promoted'); "
                     "INSERT INTO e_bm VALUES (99999999, 1, 1, 'wire promoted')")
    m['promoted_errors'] = int(err is not None) + sum(hygiene(st).values())
    ctx.close_sessions()
    ctx.hook('LOAD_RESTANDBY')
    return m


def conninfo() -> str:
    return ' '.join(f'{k}={os.environ[e]}' for k, e in (('host', 'PGHOST'), ('port', 'PGPORT'), ('user', 'PGUSER')) if os.environ.get(e)) + ' dbname=postgres'


SHAPE = 'id int8 PRIMARY KEY, grp int, v int8, body text'


def repl_logical_cdc_churn(ctx):
    """a sift_table published through pgoutput to a sift_table of a subscriber (another database of the cluster): one INSERT ... SELECT of 512 toasted 32 kB rows, whose
    backend may not keep what it detoasted (rss_mb_delta: its peak memory above the same INSERT into a sift_table nobody publishes, each in a fresh backend), then 2 writers of 24 bulk inserts of 16 toasted rows each and 1 of updates and deletes beside 1 that truncates a second published table and adds it to
    the publication and drops it again: the subscriber ends with the rows of the primary, its sift.check clean"""
    db = ctx.db
    if db.val('SHOW wal_level') != 'logical':
        raise RuntimeError('the case needs wal_level = logical')
    db.run(f'CREATE TABLE big (id int8 PRIMARY KEY, body text); CREATE TABLE cdc ({SHAPE}) USING sift_table; CREATE TABLE cdc2 ({SHAPE}) USING sift_table; CREATE TABLE cdc_ctl ({SHAPE}) USING sift_table; '
           "INSERT INTO big SELECT g, (SELECT string_agg(md5((g * 1024 + j)::text), '') FROM generate_series(1, 1024) j) FROM generate_series(1, 512) g; "
           "DROP PUBLICATION IF EXISTS load_pub; CREATE PUBLICATION load_pub FOR TABLE cdc; SELECT 1 FROM pg_create_logical_replication_slot('load_cdc', 'pgoutput'); "
           'DROP DATABASE IF EXISTS load_case_sub WITH (FORCE); CREATE DATABASE load_case_sub')
    sub = ctx.sessions(1, db='load_case_sub', path='public')[0]
    try:
        sub.run(f'CREATE EXTENSION pg_sift; CREATE SCHEMA load_case; CREATE TABLE load_case.cdc ({SHAPE}) USING sift_table; '
                f"CREATE TABLE load_case.cdc2 ({SHAPE}) USING sift_table; CREATE SUBSCRIPTION load_sub CONNECTION '{conninfo()}' PUBLICATION load_pub "
                "WITH (create_slot = false, slot_name = 'load_cdc')")
        ws = ctx.sessions(5)
        ctl, pub = (anon_peak_mb(ctx, w, f'INSERT INTO {t} SELECT id, id % 64, id % 1000, body FROM big') for w, t in ((ws[4], 'cdc_ctl'), (ws[0], 'cdc')))

        def bulk(w, i):
            lo = w.seed * 10 ** 6 + i * 1000
            w.do(f'INSERT INTO cdc SELECT {lo} + id, id % 64, 0, body FROM big WHERE id <= 16')
            return i < 24

        def churn(w, i):
            k = 1 + (i * 37) % 512
            w.do(f'DELETE FROM cdc WHERE id = {k}' if i % 4 == 3 else f'UPDATE cdc SET v = v + 1, body = body || {i} WHERE id = {k}')

        def publication(w, i):
            w.do(("TRUNCATE cdc2; INSERT INTO cdc2 SELECT g, g % 64, g, 'wire' FROM generate_series(1, 64) g", 'ALTER PUBLICATION load_pub ADD TABLE cdc2',
                  'TRUNCATE cdc2', 'ALTER PUBLICATION load_pub DROP TABLE cdc2')[i % 4])

        busy = ctx.phase({'w': (ws[:2], bulk), 'u': (ws[2:3], churn), 'p': (ws[3:4], publication)}, 2.0)
        want, end = db.row(CDC), time.monotonic() + CAP
        while sub.row(CDC) != want and time.monotonic() < end:
            time.sleep(0.05)
        got, n = sub.row(CDC), int(want[0])
        return base(ctx, busy, 'w', lost_rows=abs(n - int(got[0])), oracle_mismatch=int(got != want), rss_mb_delta=None if pub is None else round(pub - ctl, 1), unpublished_mb=ctl,
                    rows=n, **checks(sub))
    finally:
        sub.q('DROP SUBSCRIPTION IF EXISTS load_sub')
        ctx.close_sessions()
        db.run('DROP DATABASE IF EXISTS load_case_sub WITH (FORCE)')
        db.run("SELECT pg_drop_replication_slot(slot_name) FROM pg_replication_slots WHERE slot_name = 'load_cdc'; DROP PUBLICATION IF EXISTS load_pub")


CASES = (
    LoadCase('repl_standby_failover', 'repl', repl_standby_failover, Limits()),
    LoadCase('repl_logical_cdc_churn', 'repl', repl_logical_cdc_churn, Limits(rss_mb_delta=8), formats=(2,)),
)
