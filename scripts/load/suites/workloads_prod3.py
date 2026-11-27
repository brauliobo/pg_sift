"""The third wave of production shapes of the load gate (Spec 115): MERGE, deferred keys, generated columns and transition triggers on a sift_table, column type changes,
tablespace moves and UNLOGGED switches under writers, concurrent index builds, reindexes and parallel VACUUM of the three engines, quantile sketches and every observability
surface beside temporary-table churn, a kill -9 with UNLOGGED objects of every engine, and logical decoding of a REPLICA IDENTITY FULL sift_table and of primary-key updates
through changes of the table shape (scripts/load/suites: WORKLOADS on test/t/core/load_workloads.sh, REPL on test/t/core/load_repl.sh). The observability case waits for
S115-35: sift.indexes opens the temporary relations of other sessions (0A000) and relations dropped while it reads (42P01); the logical decoding case waits for S115-36:
the apply worker of a subscriber never finds the old row of an UPDATE or DELETE of a sift_table under REPLICA IDENTITY FULL (it scans the granules without end); the
column type case waits for S115-37: a rewrite of a sift_table (ALTER COLUMN ... TYPE) after ADD COLUMN ... NOT NULL DEFAULT finds the column NULL (23502), and the session
that ran it reads the column NULL afterwards (one session alone meets it every time; under the load of the case, about one run of three).
"""
from __future__ import annotations

import os
import shutil
import sys
import time
from dataclasses import dataclass

sys.dont_write_bytecode = True

from cases import LoadCase  # noqa: E402
from collect import AGG, BM, N, add, base, checks, ctx_kb, engines, halves_differ, kept, ledger_writes, mem_delta, multi_kept, nxt, oracle, oracles, snap  # noqa: E402
from suites.common import Limits, twin_do  # noqa: E402
from suites.repl import CAP, conninfo  # noqa: E402
from suites.workloads_prod2 import DUTY, copy_in, snap_oracle, snap_pair  # noqa: E402


@dataclass(frozen=True)
class Wave3(Limits):
    invalid_indexes: int | None = 0
    unlogged_left: int | None = 0


ROW_DIGEST = "SELECT count(*), count(DISTINCT id), coalesce(md5(string_agg({row}, ',' ORDER BY id)), '') FROM {t}"


def digest_differs(db, a: str, b: str, row: str) -> int:
    """1 when the tables A and B hold other rows (ROW: the text of a row) in one REPEATABLE READ snapshot"""
    out = db.run(f'BEGIN ISOLATION LEVEL REPEATABLE READ; {ROW_DIGEST.format(row=row, t=a)}; {ROW_DIGEST.format(row=row, t=b)}; COMMIT')
    return int(out[0] != out[1])


def dups(db, t: str) -> int:
    n, d = (int(x) for x in db.row(f'SELECT count(*), count(DISTINCT id) FROM {t}'))
    return n - d


M_SHAPE = 'id int8, grp int, v int8, w int8 GENERATED ALWAYS AS (v * 2 + grp) STORED, body text, CONSTRAINT {t}_pk PRIMARY KEY (id) DEFERRABLE INITIALLY DEFERRED'
M_FUNCS = ('CREATE FUNCTION m_norm() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN NEW.body := left(NEW.body, 24); RETURN NEW; END $$; '
           'CREATE FUNCTION m_row() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN INSERT INTO m_rowlog VALUES (TG_TABLE_NAME, TG_OP); RETURN NULL; END $$; '
           'CREATE FUNCTION m_note() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN '
           'INSERT INTO m_audit SELECT TG_TABLE_NAME, TG_OP, count(*), coalesce(sum(v), 0) FROM m_rows; RETURN NULL; END $$')
M_TRIGS = ('CREATE TRIGGER {t}_norm BEFORE INSERT OR UPDATE ON {t} FOR EACH ROW EXECUTE FUNCTION m_norm(); '
           'CREATE TRIGGER {t}_row AFTER INSERT OR UPDATE OR DELETE ON {t} FOR EACH ROW EXECUTE FUNCTION m_row(); '
           'CREATE TRIGGER {t}_ins AFTER INSERT ON {t} REFERENCING NEW TABLE AS m_rows FOR EACH STATEMENT EXECUTE FUNCTION m_note(); '
           'CREATE TRIGGER {t}_upd AFTER UPDATE ON {t} REFERENCING NEW TABLE AS m_rows FOR EACH STATEMENT EXECUTE FUNCTION m_note(); '
           'CREATE TRIGGER {t}_del AFTER DELETE ON {t} REFERENCING OLD TABLE AS m_rows FOR EACH STATEMENT EXECUTE FUNCTION m_note()')
AUDIT = ("SELECT op, sum(n), sum(s) FROM m_audit WHERE tbl = '{t}' GROUP BY op ORDER BY op; SELECT op, count(*) FROM m_rowlog WHERE tbl = '{t}' GROUP BY op ORDER BY op")
M_MERGE = ('MERGE INTO {{t}} t USING (VALUES ({a}, {ga}, 1), ({b}, {gb}, -1), ({n}, {gn}, 5)) s(id, grp, d) ON t.id = s.id WHEN MATCHED AND s.d < 0 THEN DELETE '
           "WHEN MATCHED THEN UPDATE SET v = t.v + s.d WHEN NOT MATCHED THEN INSERT (id, grp, v, body) VALUES (s.id, s.grp, s.d, 'merged ' || s.id)")
M_ROW = "id || ':' || v || ':' || w || ':' || body"


def own_key(w, i: int) -> int:
    """a key of the fixture that only writer W touches (its residue mod 3)"""
    return 3 * (1 + (i * 37 + w.seed * 101) % (N // 3 - 1)) + w.seed % 3


def m_step(w, i: int) -> tuple:
    """statement I of a writer of m_tbl and its heap twin m_tw ({t}: one table, the statement terminated: psql drops what follows a line ';' after the end of COPY data) and
    the SQLSTATEs it may meet: MERGE, a swap of two keys under the deferred key, a COPY with a WHERE clause, a duplicate key resolved before COMMIT or one the deferred check
    refuses at COMMIT"""
    a, b, n = own_key(w, i), own_key(w, i + 1), nxt(w)
    if i % 4 == 0:
        return M_MERGE.format(a=a, b=b, n=n, ga=a % 64, gb=b % 64, gn=n % 64) + ';', ()
    if i % 4 == 1:
        return f'UPDATE {{t}} SET id = CASE WHEN id = {a} THEN {b} ELSE {a} END WHERE id IN ({a}, {b});', ()
    if i % 4 == 2:
        return copy_in('{t} (id, grp, v, body)', range(n * 16, n * 16 + 16), ' WHERE v % 2 = 0'), ()
    dup = f"INSERT INTO {{t}} (id, grp, v, body) VALUES ({a}, 0, 0, 'dup')"
    return (f"{dup}; UPDATE {{t}} SET id = {n} WHERE id = {a} AND body = 'dup';", ()) if i % 8 == 3 else (dup + ';', ('23505',))


def txn_merge_trig_gen_defer(ctx):
    """a sift_table and its heap twin with a deferred primary key, a stored generated column, a BEFORE row trigger, an AFTER row trigger and statement triggers over transition
    tables: 3 writers MERGE, swap keys, COPY with a WHERE clause and insert duplicate keys (resolved before COMMIT or refused by the deferred check), each statement on both
    tables in one transaction; 2 readers compare the tables and what their triggers recorded in one snapshot: every answer agrees, the generated column right, no key twice"""
    db = ctx.db
    db.run(f"CREATE TABLE m_tbl ({M_SHAPE.format(t='m_tbl')}) USING sift_table; CREATE TABLE m_tw ({M_SHAPE.format(t='m_tw')}); "
           'INSERT INTO m_tbl (id, grp, v, body) SELECT id, grp, v, body FROM load_base_fact; INSERT INTO m_tw (id, grp, v, body) SELECT id, grp, v, body FROM load_base_fact; '
           f"CREATE TABLE m_audit (tbl text, op text, n int8, s int8); CREATE TABLE m_rowlog (tbl text, op text); {M_FUNCS}; "
           f"{M_TRIGS.format(t='m_tbl')}; {M_TRIGS.format(t='m_tw')}; ANALYZE m_tbl, m_tw")
    ws = ctx.sessions(5)

    def write(w, i):
        sql, allow = m_step(w, i)
        w.do(f"BEGIN;\n{sql.format(t='m_tbl')}\n{sql.format(t='m_tw')}\nCOMMIT", allow)

    def read(w, i):
        w.ledger += twin_do(w, 'm_tbl', 'm_tw') if i % 2 else snap_pair(w, AUDIT.format(t='m_tbl'), AUDIT.format(t='m_tw'), ())

    busy = ctx.phase({'w': (ws[:3], write), 'r': (ws[3:], read)}, 2.0)
    gen = int(db.val('SELECT count(*) FROM m_tbl WHERE w IS DISTINCT FROM v * 2 + grp'))
    audit = halves_differ(db.run(snap(AUDIT.format(t='m_tbl'), AUDIT.format(t='m_tw'))))
    rows = abs(int(db.val('SELECT count(*) FROM m_tbl')) - int(db.val('SELECT count(*) FROM m_tw')))
    return base(ctx, busy, 'w', lost_rows=rows, dup_rows=dups(db, 'm_tbl'), oracle_mismatch=digest_differs(db, 'm_tbl', 'm_tw', M_ROW) + audit + gen + sum(w.ledger for w in ws[3:]),
                refused=busy['w'].expected, snapshots=busy['r'].ops)


A_SHAPE = 'id int8 PRIMARY KEY, grp int, v int4, body text'
A_DDL = ('ALTER TABLE {t} ALTER COLUMN v TYPE int8', 'ALTER TABLE {t} ADD COLUMN c{n} int NOT NULL DEFAULT {n}', 'ALTER TABLE {t} SET TABLESPACE load_ts',
         'ALTER TABLE {t} ALTER COLUMN v TYPE numeric(18,0)', 'ALTER TABLE {t} DROP COLUMN c{m}', 'ALTER TABLE {t} SET TABLESPACE pg_default')
A_SIDE = ('ALTER TABLE a_x SET UNLOGGED', 'ALTER TABLE a_x SET LOGGED')
A_LOCK = 'LOCK TABLE a_tbl, a_tw IN ACCESS SHARE MODE'
A_ROW = "id || ':' || grp || ':' || v || ':' || body"


def ddl_alter_type_tblspc_live(ctx):
    """a sift_table and its heap twin in one transaction: 3 writers update their narrow columns and insert rows, 2 readers lock both and compare them and the analytic path with
    the heap in one snapshot, 1 inserts into a side sift_table, 1 vacuums and maintains, while 1 session changes the type of a column (int4, int8, numeric), adds and drops
    columns with a default, moves both tables to a tablespace and back and switches the side table to UNLOGGED and back, idle DUTY times as long as its last statement took:
    the twin agrees in every snapshot and at the end, every committed row kept once"""
    db = ctx.db
    loc = os.path.join(db.val('SHOW data_directory'), 'load_ts')
    shutil.rmtree(loc, ignore_errors=True)
    os.makedirs(loc)
    db.run(f"DROP TABLESPACE IF EXISTS load_ts; CREATE TABLESPACE load_ts LOCATION '{loc}'")
    try:
        db.run(f'CREATE TABLE a_tbl ({A_SHAPE}) USING sift_table; CREATE TABLE a_tw ({A_SHAPE}); INSERT INTO a_tbl SELECT * FROM load_base_fact; '
               f'INSERT INTO a_tw SELECT * FROM load_base_fact; CREATE TABLE a_x ({A_SHAPE}) USING sift_table; ANALYZE a_tbl, a_tw')
        ws = ctx.sessions(8)

        def write(w, i):
            k = nxt(w)
            ins = f"INSERT INTO {{t}} (id, grp, v, body) VALUES ({k}, {k % 64}, {k % 1000}, 'wire tail{k % 97}')"
            upd = f'UPDATE {{t}} SET v = v + 1 WHERE id = {1 + (w.seed * 2741 + i * 37) % N}'
            w.do(f"BEGIN; {'; '.join(s.format(t=t) for s in (ins, upd) for t in ('a_tbl', 'a_tw'))}; COMMIT")

        def read(w, i):
            if i % 2:
                w.ledger += snap_oracle(w, f'{A_LOCK}; {AGG.format(t="a_tbl")}', 'sift.enable_agg', ())
            else:
                out = w.do(f'BEGIN ISOLATION LEVEL REPEATABLE READ; {A_LOCK}; {ROW_DIGEST.format(row=A_ROW, t="a_tbl")}; {ROW_DIGEST.format(row=A_ROW, t="a_tw")}; COMMIT')
                w.ledger += out is not None and out[0] != out[1]

        def side(w, i):
            k = nxt(w)
            if w.do(f"INSERT INTO a_x VALUES ({k}, {k % 64}, {k % 1000}, 'side')") is not None:
                w.ids = getattr(w, 'ids', set()) | {k}

        def maint(w, i):
            w.do(('VACUUM a_tbl, a_x', "SELECT count(*) FROM sift.maintain('a_tbl')", "SELECT count(*) FROM sift.maintain('a_x')")[i % 3])
            time.sleep(0.05)

        def ddl(w, i):
            step = A_DDL[i % len(A_DDL)]
            w.do(f"BEGIN; {'; '.join(step.format(t=t, n=i // 6, m=i // 6) for t in ('a_tbl', 'a_tw'))}; COMMIT; {A_SIDE[i % 2]}")
            time.sleep(DUTY * max(w.lat[-1:] or [12.5]) / 1000)

        busy = ctx.phase({'w': (ws[:3], write), 'r': (ws[3:5], read), 'x': (ws[5:6], side), 'm': (ws[6:7], maint), 'd': (ws[7:], ddl)}, 2.5)
        db.run('ALTER TABLE a_tbl SET TABLESPACE pg_default; ALTER TABLE a_tw SET TABLESPACE pg_default')
        mismatch = digest_differs(db, 'a_tbl', 'a_tw', A_ROW) + sum(w.ledger for w in ws[3:5])
        return base(ctx, busy, 'w', **add(kept(db, 'a_x', getattr(ws[5], 'ids', set()), 10 ** 6), {'dup_rows': dups(db, 'a_tbl')}), oracle_mismatch=mismatch,
                    ddl=busy['d'].ops, snapshots=busy['r'].ops)
    finally:
        ctx.close_sessions()
        db.q('ALTER TABLE IF EXISTS a_tbl SET TABLESPACE pg_default; ALTER TABLE IF EXISTS a_tw SET TABLESPACE pg_default')
        db.run('DROP TABLESPACE IF EXISTS load_ts')
        shutil.rmtree(loc, ignore_errors=True)


E_DDL = {t: (f'CREATE INDEX CONCURRENTLY {t}_c ON {t} USING {am} ({cols})', f'REINDEX INDEX CONCURRENTLY {ix}', f'DROP INDEX CONCURRENTLY {t}_c',
             f'VACUUM (PARALLEL 2) {t}', f"SELECT count(*) FROM sift.maintain('{m}')")
         for t, am, cols, ix, m in (('e_col', 'sift_col', 'grp, v', 'e_col_ix', 'e_col_ix'), ('e_bm', 'sift', 'body', 'e_bm_ix', 'e_bm_ix'),
                                    ('e_tbl', 'btree', 'grp, v', 'e_tbl_pkey', 'e_tbl'))}
INVALID = "SELECT count(*) FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid WHERE NOT (i.indisvalid AND i.indisready) AND c.relnamespace = 'load_case'::regnamespace"


def ddl_cic_ric_parvac_storm(ctx):
    """3 writers of the three engines, 2 sessions that compare their index paths with the heap in one snapshot (one planned on an index dropped before it runs is
    refused with 40001, to plan again), while 3 sessions, one for each table (two concurrent builds
    on one table wait for each other's transactions: a deadlock by design of PostgreSQL), build a second sift_col, BM25 or btree index concurrently, reindex the first
    concurrently, drop the second concurrently, run VACUUM (PARALLEL 2) and the maintenance: every committed row kept once, every answer agrees, no invalid index, no deadlock"""
    db = ctx.db
    engines(db)
    ws = ctx.sessions(8, 'SET min_parallel_index_scan_size = 0; SET max_parallel_maintenance_workers = 2')

    def read(w, i):
        sql, guc = ((AGG.format(t='e_col'), 'sift.enable_agg'), (BM.format(t='e_bm'), 'sift.enable_scan'))[i % 2]
        w.ledger += snap_oracle(w, sql, guc, ('40001',))

    def steps(seq):
        def run(w, i):
            w.do(seq[i % len(seq)])
            time.sleep(0.02)
        return run

    ddl = {t: ([w], steps(seq)) for w, (t, seq) in zip(ws[5:], E_DDL.items())}
    busy = ctx.phase({'w': (ws[:3], ledger_writes('e')), 'r': (ws[3:5], read), **ddl}, 2.5)
    ctx.close_sessions()
    invalid = int(db.val(INVALID))
    db.run('DROP INDEX IF EXISTS e_col_c, e_tbl_c, e_bm_c')
    return base(ctx, busy, 'w', **add(multi_kept(db, 'e', ws[:3]), {'oracle_mismatch': oracles(db) + sum(w.ledger for w in ws[3:5])}), invalid_indexes=invalid,
                ddl=sum(busy[t].ops for t in E_DDL))


K_ROWS = 40000
K_TOP = 999
KLL = "SELECT sift.approx_percentile('k_col', 'v', 1), sift.approx_quantiles('k_col', 'v', ARRAY[0.1, 0.5, 0.9, 1]), sift.approx_percentile('k_col', 'v', 0.5, 'grp < 8')"
SURFACES = ('SELECT count(*) FROM sift.health()', 'SELECT count(*) FROM sift.status()', "SELECT count(*) FROM sift.space('k_col_ix')", 'SELECT count(*) FROM sift.activity()',
            'SELECT count(*) FROM sift.indexes', 'SELECT count(*) FROM sift.locks', "SELECT count(*) FROM sift.parts('k_col_ix')", 'SELECT count(*) FROM sift.shared_memory_pools()')
K_TEMP = ('CREATE TEMP TABLE kt (id int8 PRIMARY KEY, v int8) USING sift_table; INSERT INTO kt SELECT g, g FROM generate_series(1, 512) g; '
          'CREATE TEMP TABLE kc (id int8, v int8); INSERT INTO kc SELECT g, g FROM generate_series(1, 512) g; CREATE INDEX kc_ix ON kc USING sift_col (id, v); '
          'SELECT count(*), sum(v) FROM kc WHERE v > 8; SELECT count(*) FROM kt; DROP TABLE kt, kc')


def kll_wrong(out: list | None) -> int:
    """1 when an answer of KLL is out of the committed values (0..K_TOP: a rolled-back outlier seen, a negative or NaN one) or its quantiles are not in order"""
    if out is None:
        return 0
    p1, qs, half = out[0].split('|')
    q = [float(x) for x in qs.strip('{}').split(',')]
    return int(len(q) != 4 or q != sorted(q) or not all(0 <= x <= K_TOP for x in q + [float(p1), float(half)]))


def feat_kll_obs_poll_ddl_storm(ctx):
    """a heap with a sift_col index that keeps quantile sketches: 2 writers commit rows of values 0..999 and roll back batches of outliers, 2 sessions ask the percentiles and
    quantiles (from the sketches and exactly, under a filter), 2 poll every observability surface (health, status, space, activity, indexes, locks, parts and the shared memory
    pools) while 2 create, fill, index and drop temporary sift tables: no answer sees an outlier, the quantiles in order, no error, the memory of the pollers after the load"""
    db = ctx.db
    db.run("CREATE TABLE k_col (id int8, grp int, v float8, pad text); CREATE INDEX k_col_ix ON k_col USING sift_col (id, grp, v, pad) WITH (sketches = 'kll(v)'); "
           f'INSERT INTO k_col SELECT g, g % 64, g % 1000, md5(g::text) FROM generate_series(1, {K_ROWS}) g; ANALYZE k_col')
    ws = ctx.sessions(8)

    def write(w, i):
        lo = nxt(w) * 256
        if i % 3 == 2:
            w.do(f'BEGIN; INSERT INTO k_col SELECT g, g % 64, 1e9 + g, md5(g::text) FROM generate_series({lo}, {lo + 4095}) g; ROLLBACK')
        else:
            w.do(f'INSERT INTO k_col SELECT g, g % 64, g % 1000, md5(g::text) FROM generate_series({lo}, {lo + 255}) g; '
                 f'UPDATE k_col SET v = (v + 1)::int % 1000 WHERE id = {1 + (w.seed * 389 + i * 61) % K_ROWS}')

    def ask(w, i):
        w.ledger += kll_wrong(w.do(KLL))

    def poll(w, i):
        w.pid
        w.do(SURFACES[(w.seed + i) % len(SURFACES)])

    def churn(w, i):
        w.do(K_TEMP)

    roles = {'w': (ws[:2], write), 'q': (ws[2:4], ask), 'p': (ws[4:6], poll), 't': (ws[6:], churn)}
    ctx.phase(roles, 0.5)
    before = [ctx_kb(w) for w in ws[4:6]], ctx.rss(ws[4:6])
    busy = ctx.phase(roles, 2.0)
    after = [ctx_kb(w) for w in ws[4:6]], ctx.rss(ws[4:6])
    ctx.close_sessions()
    wrong = sum(w.ledger for w in ws[2:4]) + kll_wrong(db.run(KLL))
    return base(ctx, busy, 'w', oracle_mismatch=wrong, polls=busy['p'].ops, sketch_reads=busy['q'].ops, temp_cycles=busy['t'].ops, **mem_delta(ctx, before, after))


U_SETUP = ('CREATE UNLOGGED TABLE u_tbl (id int8 PRIMARY KEY, grp int, v int8, body text) USING sift_table; '
           'CREATE UNLOGGED TABLE u_col (id int8, grp int, v int8, body text); CREATE INDEX u_col_ix ON u_col USING sift_col (id, grp, v); '
           'CREATE UNLOGGED TABLE u_bm (id int8, grp int, v int8, body text); CREATE INDEX u_bm_ix ON u_bm USING sift (body)')
U_KINDS = ('tbl', 'col', 'bm')
U_REFILL = 512


def fault_unlogged_kill9_reset(ctx):
    """an UNLOGGED sift_table, an UNLOGGED heap with a sift_col index and one with a BM25 index beside logged ones: 4 writers commit rows of the logged ones, 2 of the unlogged
    ones, with async maintenance on, until the server is killed with -9 and started again: every acknowledged logged row kept, the unlogged objects empty (their init forks),
    checked clean, and they take rows again and answer what the heap does"""
    db = ctx.db
    engines(db, kinds=('tbl', 'col'))
    db.run(f"{U_SETUP}; {'; '.join(f'INSERT INTO u_{e} SELECT * FROM load_base_fact' for e in U_KINDS)}; ANALYZE u_col, u_bm")
    up = db.val('SELECT pg_postmaster_start_time()')
    ws = ctx.sessions(7, 'SET sift.async_maintenance = on')
    for w in ws[:6]:
        w.allow = ('08006', '57P01')
    logged, unlogged = ledger_writes('e', ('tbl', 'col'), churn=False), ledger_writes('u')

    def crash(w, i):
        if int(w.val('SELECT count(*) FROM e_tbl WHERE id >= 1000000')) < 200 and i < 400:
            return time.sleep(0.01)
        ctx.hook('LOAD_CRASH')
        return False

    busy = ctx.phase({'w': (ws[:4], logged), 'u': (ws[4:6], unlogged), 'k': (ws[6:], crash)}, 30.0, lead='k')
    ctx.close_sessions()
    ctx.restart('LOAD_RESTART')
    acked = set().union(*(getattr(w, 'ids', set()) for w in ws[:4]))
    have = {int(x) for x in db.run('SELECT id FROM e_tbl WHERE id >= 1000000')}
    left = sum(int(db.val(f'SELECT count(*) FROM u_{e}')) for e in U_KINDS)
    db.run('; '.join(f'INSERT INTO u_{e} SELECT * FROM load_base_fact WHERE id <= {U_REFILL}' for e in U_KINDS))
    refill = sum(abs(int(db.val(f'SELECT count(*) FROM u_{e}')) - U_REFILL) for e in U_KINDS)
    return base(ctx, busy, 'w', **add(kept(db, 'e_col', have, 10 ** 6), {'lost_rows': len(acked - have) + refill}), unlogged_left=left,
                oracle_mismatch=oracles(db, 'u') + oracle(db, AGG.format(t='e_col'), 'sift.enable_agg'), acked=len(acked), unlogged_ops=busy['u'].ops,
                not_restarted=int(db.val('SELECT pg_postmaster_start_time()') == up))


R_PAD = "(SELECT string_agg(md5(({k} * 128 + j)::text), '') FROM generate_series(1, 128) j)"
R_SHAPE = 'id int8{pk}, grp int, v int8, body text'
R_FULL = "SELECT count(*), coalesce(md5(string_agg(t::text, ',' ORDER BY id)), '') FROM load_case.{t} t"
R_ROWS = 512
R_MOVED = 5 * 10 ** 7
R_MARK = 9 * 10 ** 8


def r_tables(pad: bool = True) -> str:
    """the published tables: r_full, a sift_table without a key under REPLICA IDENTITY FULL, and r_pk, a keyed one, each of R_ROWS rows of 4 kB of hex (PAD) or empty"""
    ddl = (f"CREATE TABLE load_case.r_full ({R_SHAPE.format(pk='')}) USING sift_table; ALTER TABLE load_case.r_full REPLICA IDENTITY FULL; "
           f"CREATE TABLE load_case.r_pk ({R_SHAPE.format(pk=' PRIMARY KEY')}) USING sift_table")
    fill = ''.join(f"; INSERT INTO load_case.{t} SELECT g, g % 64, 0, {R_PAD.format(k='g')} FROM generate_series(1, {R_ROWS}) g" for t in ('r_full', 'r_pk'))
    return ddl + (fill if pad else '')


def settled(end: float, done) -> int:
    """0 once DONE() holds before the monotonic time END (one liveness cap shared by every wait of a case, never a measured time), else 1"""
    while not done():
        if time.monotonic() > end:
            return 1
        time.sleep(0.05)
    return 0


def repl_logical_rif_pk_ddl(ctx):
    """a sift_table without a key under REPLICA IDENTITY FULL and a keyed one, published to sift_tables of a subscriber (another database of the cluster): 1 writer inserts
    wide rows, updates their narrow columns and deletes them, 1 inserts and moves rows to new primary keys, while 1 session adds a column with a default on both sides (the
    subscriber first) and drops it again (the publisher first, the subscriber once it applied that): the subscriber ends with every row of the primary, column for column, and
    its sift.check clean"""
    db = ctx.db
    if db.val('SHOW wal_level') != 'logical':
        raise RuntimeError('the case needs wal_level = logical')
    db.run(f"{r_tables()}; DROP PUBLICATION IF EXISTS load_pub_rif; CREATE PUBLICATION load_pub_rif FOR TABLE r_full, r_pk; "
           "SELECT 1 FROM pg_create_logical_replication_slot('load_rif', 'pgoutput'); DROP DATABASE IF EXISTS load_case_rsub WITH (FORCE); CREATE DATABASE load_case_rsub")
    sub, end = ctx.sessions(1, "SET lock_timeout = '5s'", db='load_case_rsub', path='public')[0], time.monotonic() + CAP
    try:
        sub.run(f"CREATE EXTENSION pg_sift; CREATE SCHEMA load_case; {r_tables(False)}; CREATE SUBSCRIPTION load_sub_rif CONNECTION '{conninfo()}' "
                "PUBLICATION load_pub_rif WITH (create_slot = false, slot_name = 'load_rif')")
        synced = settled(end, lambda: sub.val("SELECT count(*) FROM pg_subscription_rel WHERE srsubstate <> 'r'") == '0')
        ws = ctx.sessions(3)

        def full(w, i):
            w.ids = getattr(w, 'ids', [])
            if i % 4 == 0 or not w.ids:
                k = nxt(w)
                if w.do(f'INSERT INTO r_full (id, grp, v, body) VALUES ({k}, {k % 64}, 0, {R_PAD.format(k=k)})') is not None:
                    w.ids.append(k)
            elif i % 4 == 3:
                w.do(f'DELETE FROM r_full WHERE id = {w.ids.pop(0)}')
            else:
                w.do(f'UPDATE r_full SET v = v + 1 WHERE id = {w.ids[0] if i % 2 else 1 + i % R_ROWS}')

        def keyed(w, i):
            k = nxt(w)
            move = f'; UPDATE r_pk SET id = id + {R_MOVED}, v = v + 1 WHERE id = {1 + i % R_ROWS}' if i % 2 else ''
            w.do(f"BEGIN; INSERT INTO r_pk (id, grp, v, body) VALUES ({k}, {k % 64}, 0, 'k{k}'){move}; COMMIT")

        def ddl(w, i):
            n, cols = i // 2, ('r_full', 'r_pk')
            if i % 2 == 0:
                sub.run('; '.join(f'ALTER TABLE load_case.{t} ADD COLUMN c{n} int DEFAULT {n}' for t in cols))
                w.do('; '.join(f'ALTER TABLE {t} ADD COLUMN c{n} int DEFAULT {n}' for t in cols))
            else:
                w.do('; '.join(f'ALTER TABLE {t} DROP COLUMN c{n}' for t in cols) + f"; INSERT INTO r_pk (id, grp, v, body) VALUES ({R_MARK + n}, 0, 0, 'mark')")
                if settled(end, lambda: sub.val(f'SELECT count(*) FROM load_case.r_pk WHERE id = {R_MARK + n}') == '1'):
                    w.ledger += 1
                    return False
                sub.run('; '.join(f'ALTER TABLE load_case.{t} DROP COLUMN c{n}' for t in cols))
            time.sleep(DUTY * max(w.lat[-1:] or [12.5]) / 1000)

        busy = ctx.phase({'w': (ws[:1], full), 'k': (ws[1:2], keyed), 'd': (ws[2:], ddl)}, 2.5)
        both = f"{R_FULL.format(t='r_full')}; {R_FULL.format(t='r_pk')}"
        lag = ws[2].ledger or synced or settled(end, lambda: db.run(both) == sub.run(both))
        differ = int(db.run(both) != sub.run(both))
        n = int(db.val('SELECT count(*) FROM r_full')) + int(db.val('SELECT count(*) FROM r_pk'))
        return base(ctx, busy, 'w', oracle_mismatch=differ, standby_mismatch=lag, dup_rows=dups(db, 'r_pk'), rows=n, ddl=busy['d'].ops, **checks(sub))
    finally:
        sub.q('DROP SUBSCRIPTION IF EXISTS load_sub_rif')
        ctx.close_sessions()
        db.run('DROP DATABASE IF EXISTS load_case_rsub WITH (FORCE)')
        db.run("SELECT pg_drop_replication_slot(slot_name) FROM pg_replication_slots WHERE slot_name = 'load_rif'; DROP PUBLICATION IF EXISTS load_pub_rif")


WORKLOADS = (
    LoadCase('txn_merge_trig_gen_defer', 'txn', txn_merge_trig_gen_defer, Wave3(), formats=(2,)),
    LoadCase('ddl_alter_type_tblspc_live', 'ddl', ddl_alter_type_tblspc_live, Wave3(), formats=(2,), open='S115-37', intermittent=True),
    LoadCase('ddl_cic_ric_parvac_storm', 'ddl', ddl_cic_ric_parvac_storm, Wave3()),
    LoadCase('feat_kll_obs_poll_ddl_storm', 'feat', feat_kll_obs_poll_ddl_storm, Wave3(ctx_kb_delta=1280, rss_mb_delta=16), formats=(2,), open='S115-35'),
    LoadCase('fault_unlogged_kill9_reset', 'fault', fault_unlogged_kill9_reset, Wave3(), formats=(2,), last=True),
)
REPL = (
    LoadCase('repl_logical_rif_pk_ddl', 'repl', repl_logical_rif_pk_ddl, Wave3(), formats=(2,), open='S115-36'),
)
