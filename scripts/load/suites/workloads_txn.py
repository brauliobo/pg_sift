"""The transactional workloads of the load gate: hot narrow updates beside analytics, serializable write skew, two-phase commit, deep savepoints and held cursors, online DDL of
partitioned tables beside an unprivileged lock probe, and a pool of many sessions that are terminated while they commit (scripts/load/suites, test/t/core/load_workloads.sh).
"""
from __future__ import annotations

import sys
import threading
import time

sys.dont_write_bytecode = True

from cases import LoadCase  # noqa: E402
from collect import AGG, BM, N, add, base, engines, kept, ledger_writes, nxt, oracle, p99_ratio, row3, tbl, zipf  # noqa: E402
from suites.common import Limits, prepared_left  # noqa: E402

SPACE = "SELECT coalesce(sum(pending_blocks), 0) FROM sift.space('{r}')"
MANIFEST = "SELECT used_bytes, refusals FROM sift.shared_memory_pools() WHERE pool = 'manifest'"
LOST = ('57P01', '08006')


def delta_hot_update_olap(ctx):
    """6 writers update narrow columns of a Zipf-hot key set of a sift_table and insert beside it, under a REPEATABLE READ snapshot held for a while, with async maintenance on;
    2 readers aggregate the table, 2 look its keys up: the p99 of the readers under the writers against the same readers alone, the oracle and the delta left after maintenance
    (the p99 passes its ceiling in about one run of three: the case is intermittent, a run that meets it proves no fix)"""
    db = ctx.db
    db.run(tbl('h_tbl'))
    s0 = int(db.val('SELECT sum(v) FROM h_tbl'))
    hold = ctx.sessions(1)[0]
    ws = ctx.sessions(10, 'SET sift.async_maintenance = on')

    def write(w, i):
        if i % 4 == 3:
            k = nxt(w)
            w.ids = getattr(w, 'ids', set()) | ({k} if w.do(row3('h_tbl', k)) is not None else set())
        else:
            w.ledger += len(w.do(f'UPDATE h_tbl SET v = v + 1 WHERE id = {zipf(w.seed * 7919 + i, N, 1.1)} RETURNING 1') or [])

    def read(w, i):
        k = zipf(w.seed * 104729 + i, N, 1.1)
        w.do(AGG.format(t='h_tbl') if w.seed % 2 else f'SELECT v FROM h_tbl WHERE id = {k}; SELECT count(*) FROM h_tbl WHERE id BETWEEN {k} AND {k + 32}')

    def held(w, i):
        w.do('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM h_tbl; SELECT pg_sleep(0.7); COMMIT')
        return False

    quiet = ctx.phase({'r': (ws[6:], read)}, 0.6)
    busy = ctx.phase({'w': (ws[:6], write), 'r': (ws[6:], read), 'h': ([hold], held)}, 1.5)
    ids = set().union(*(getattr(w, 'ids', set()) for w in ws))
    total = int(db.val(f'SELECT sum(v) FROM h_tbl WHERE id <= {N}')) - s0
    db.run("VACUUM h_tbl; SELECT count(*) FROM sift.maintain('h_tbl')")
    return base(ctx, busy, 'w', **add(kept(db, 'h_tbl', ids, 10 ** 6), {'lost_rows': abs(total - sum(w.ledger for w in ws))}),
                oracle_mismatch=oracle(db, AGG.format(t='h_tbl'), 'sift.enable_agg'), p99_ratio=p99_ratio(quiet['r'].lat, busy['r'].lat),
                pending_after=int(db.val(SPACE.format(r='h_tbl'))))


SHIFTS = 4096


def txn_ssi_2pc_savepoint(ctx):
    """2 SERIALIZABLE workers take themselves off the same shift together when two are on it (a write skew through a sift_col scan: a shift left empty), 1 worker prepares
    and commits or rolls back updates of a sift_table beside 1 that updates the same rows, 2 nest 16 savepoints and PL/pgSQL exception blocks in a sift_table and a sift_col
    table, 1 holds a cursor over VACUUM and sift.maintain: every committed row kept once, the cursor whole, no prepared transaction left"""
    db = ctx.db
    db.run(f"CREATE TABLE duty (shift int, doc int); INSERT INTO duty SELECT s, d FROM generate_series(0, {SHIFTS - 1}) s, generate_series(1, 2) d; "
           f"CREATE INDEX duty_ix ON duty USING sift_col (shift, doc); ANALYZE duty; {tbl('acc', 512)}; UPDATE acc SET v = 0; {tbl('sp', 0)}; "
           'CREATE TABLE spc (id int8, grp int, v int8, body text); CREATE INDEX spc_ix ON spc USING sift_col (id, grp, v)')
    ws = ctx.sessions(7)
    pair = threading.Barrier(2)

    def skew(w, i):
        try:
            pair.wait(1.0)
        except threading.BrokenBarrierError:
            return False
        if i >= SHIFTS:
            return False
        w.do(f'BEGIN ISOLATION LEVEL SERIALIZABLE; SET LOCAL enable_seqscan = off; '
             f'DELETE FROM duty WHERE shift = {i} AND doc = {w.seed} AND (SELECT count(*) FROM duty WHERE shift = {i}) >= 2; COMMIT', ('40001',))
        w.top = i

    def twopc(w, i):
        gid = f'load_{w.seed}_{i}'
        if w.do(f"BEGIN; UPDATE acc SET v = v + 1 WHERE id = {1 + i % 512}; PREPARE TRANSACTION '{gid}'") is None:
            return
        commit = i % 3 != 2
        w.ledger += w.do(f"{'COMMIT' if commit else 'ROLLBACK'} PREPARED '{gid}'") is not None and commit

    def plain(w, i):
        w.ledger += len(w.do(f'UPDATE acc SET v = v + 1 WHERE id = {1 + i * 7 % 512} RETURNING 1') or [])

    def nest(w, i):
        ks = [nxt(w) for _ in range(16)]
        if i % 2:
            sql = 'BEGIN; ' + '; '.join(f"SAVEPOINT s{j}; {row3('sp', k)}; {row3('spc', k)}" for j, k in enumerate(ks)) + '; ROLLBACK TO s8; COMMIT'
            keep = ks[:8]
        else:
            body = ' '.join(f"BEGIN {row3('sp', k)}; {row3('spc', k)}; {'RAISE EXCEPTION $e$undo$e$;' if j % 4 == 3 else ''} "
                            'EXCEPTION WHEN raise_exception THEN NULL; END;' for j, k in enumerate(ks))
            sql, keep = f'DO $$ BEGIN {body} END $$', [k for j, k in enumerate(ks) if j % 4 != 3]
        if w.do(sql) is not None:
            w.ids = getattr(w, 'ids', set()) | set(keep)

    def cursor(w, i):
        out = w.do("BEGIN; DECLARE c CURSOR WITH HOLD FOR SELECT id, v FROM acc ORDER BY id; COMMIT; VACUUM acc; SELECT count(*) FROM sift.maintain('acc'); "
                   'FETCH ALL FROM c; CLOSE c')
        w.ledger += out is not None and len(out) != 513

    busy = ctx.phase({'s': (ws[:2], skew), 't': (ws[2:3], twopc), 'p': (ws[3:4], plain), 'n': (ws[4:6], nest), 'c': (ws[6:], cursor)}, 2.0)
    pair.abort()
    top = min(getattr(w, 'top', -1) for w in ws[:2])
    skewed = int(db.val(f'SELECT {top + 1} - count(DISTINCT shift) FROM duty WHERE shift <= {top}'))
    ids = set().union(*(getattr(w, 'ids', set()) for w in ws[4:6]))
    ups = abs(int(db.val('SELECT sum(v) FROM acc')) - ws[2].ledger - ws[3].ledger)
    return base(ctx, busy, 'n', **add(kept(db, 'sp', ids, 10 ** 6), kept(db, 'spc', ids, 10 ** 6), {'lost_rows': ups}), write_skew=skewed,
                oracle_mismatch=ws[6].ledger + oracle(db, AGG.format(t='spc'), 'sift.enable_agg'), prepared_left=prepared_left(db), shifts=top + 1,
                serialization_failures=busy['s'].expected, prepared=busy['t'].ops)


def ddl_part_online_mix(ctx):
    """4 writers insert and update rows of a partitioned table with a sift_col and a sift (BM25) index and of a sift_table, 2 readers aggregate and search it, 1 DDL worker
    attaches, detaches, reindexes concurrently and builds an index concurrently, while 1 session holds a read of the sift_table and a role that does not own it tries to
    ALTER it: the ALTER must be refused for the privilege at once, never wait for the lock (unpriv_waits)"""
    db = ctx.db
    db.run('CREATE TABLE pe (id int8, grp int, v int8, body text) PARTITION BY RANGE (id); '
           "DO $$ BEGIN FOR i IN 0..3 LOOP EXECUTE format('CREATE TABLE pe_%s PARTITION OF pe FOR VALUES FROM (%s) TO (%s)', i, i * 2048 + 1, (i + 1) * 2048 + 1); "
           'END LOOP; END $$; CREATE TABLE pe_hot PARTITION OF pe FOR VALUES FROM (1000000) TO (100000000); INSERT INTO pe SELECT * FROM load_base_fact; '
           f'CREATE INDEX pe_ix ON pe USING sift_col (id, grp, v); CREATE INDEX pe_bm ON pe USING sift (body); ANALYZE pe; {tbl("solo")}; '
           'DROP ROLE IF EXISTS load_probe; CREATE ROLE load_probe; GRANT USAGE ON SCHEMA load_case TO load_probe')
    ws = ctx.sessions(9)

    def write(w, i):
        k = nxt(w)
        sql = f"BEGIN; {row3('pe', k)}; UPDATE pe SET v = v + 1 WHERE id = {k - 1}; UPDATE solo SET v = v + 1 WHERE id = {1 + (k * 31) % N} RETURNING 1; COMMIT"
        out = w.do(sql)
        if out is not None:
            w.ids, w.ledger = getattr(w, 'ids', set()) | {k}, w.ledger + len(out)

    def read(w, i):
        w.do((AGG.format(t='pe'), BM.format(t='pe'), 'SELECT count(*) FROM pe WHERE grp BETWEEN 3 AND 9')[i % 3])

    def ddl(w, i):
        lo = 2 * 10 ** 8 + i * 1000
        w.do((f'CREATE TABLE pe_x{i} (LIKE pe); INSERT INTO pe_x{i} SELECT g, g % 64, g % 1000, $$wire x$$ FROM generate_series({lo}, {lo + 199}) g; '
              f'ALTER TABLE pe ATTACH PARTITION pe_x{i} FOR VALUES FROM ({lo}) TO ({lo + 1000})',
              f'ALTER TABLE pe DETACH PARTITION pe_x{i - 1} CONCURRENTLY; DROP TABLE pe_x{i - 1}',
              'REINDEX TABLE CONCURRENTLY pe_0',
              f'CREATE INDEX CONCURRENTLY pe_1_c{i} ON pe_1 USING sift_col (grp, v); DROP INDEX CONCURRENTLY pe_1_c{i}')[i % 4])

    def held(w, i):
        w.do('BEGIN; SELECT count(*) FROM solo; SELECT pg_sleep(2); COMMIT')
        return False

    def probe(w, i):
        time.sleep(0.05)
        w.do("SET ROLE load_probe; SET lock_timeout = '300ms'; " + ('ALTER TABLE solo RENAME COLUMN body TO b2', 'ALTER TABLE solo SET SCHEMA public')[i % 2], ('42501', '55P03'))
        w.ledger += bool(w.last) and w.last[0] == '55P03'
        w.run('RESET ROLE; RESET lock_timeout')

    busy = ctx.phase({'w': (ws[:4], write), 'r': (ws[4:6], read), 'd': (ws[6:7], ddl), 'h': (ws[7:8], held), 'u': (ws[8:], probe)}, 1.8)
    ids = set().union(*(getattr(w, 'ids', set()) for w in ws[:4]))
    solo = int(db.val('SELECT sum(v) FROM solo')) - int(db.val('SELECT sum(v) FROM load_base_fact'))
    m = base(ctx, busy, 'w', **add(kept(db, 'pe', ids, 10 ** 6), {'lost_rows': abs(solo - sum(w.ledger for w in ws[:4]))}), unpriv_waits=ws[8].ledger,
             oracle_mismatch=oracle(db, AGG.format(t='pe'), 'sift.enable_agg') + oracle(db, BM.format(t='pe'), 'sift.enable_scan'), ddl=busy['d'].ops, probes=busy['u'].ops)
    ctx.close_sessions()
    db.run('REVOKE USAGE ON SCHEMA load_case FROM load_probe; DROP ROLE load_probe')
    return m


def pool_high_conn_term_shm(ctx):
    """32 pooled sessions commit rows of a sift_table and a sift_col table with a savepoint in each transaction and DISCARD ALL after it, while 1 session terminates one of them
    every 50 ms (during the commits of the combiner, the refills of the leases, the publishes of the manifest): every acknowledged row kept, the two tables agree, no refusal of
    the shared memory, no row lock and no session left (a row lock is left in about one run of three, with the testing library: intermittent)"""
    db = ctx.db
    engines(db, kinds=('tbl', 'col'))
    u0, r0 = (int(x) for x in db.row(MANIFEST))
    ws = ctx.sessions(33)

    def write(w, i):
        w.ids = getattr(w, 'ids', set())
        if w._pid is None:
            w.pid
        k = nxt(w)
        sql = f"BEGIN; {row3('e_tbl', k)}; SAVEPOINT a; {row3('e_col', k)}; UPDATE e_tbl SET v = v + 1 WHERE id = {k}; RELEASE a; COMMIT; DISCARD ALL; SET search_path = load_case, public"
        if w.do(sql, LOST) is not None:
            w.ids.add(k)

    def kill(w, i):
        time.sleep(0.05)
        pid = ws[(i * 7) % 32]._pid
        if pid:
            w.do(f'SELECT pg_terminate_backend({pid})')

    busy = ctx.phase({'w': (ws[:32], write), 'k': (ws[32:], kill)}, 2.0)
    ctx.close_sessions()
    acked = set().union(*(w.ids for w in ws[:32]))
    have = {t: {int(x) for x in db.run(f'SELECT id FROM e_{t} WHERE id >= 1000000')} for t in ('tbl', 'col')}
    n, d = (int(x) for x in db.row('SELECT count(*), count(DISTINCT id) FROM e_col WHERE id >= 1000000'))
    u1, r1 = (int(x) for x in db.row(MANIFEST))
    left = sum(int(x) for x in db.row('SELECT in_chunks, chunks FROM sift._test_rowlock_stats()')) if ctx.testing else None
    wedged = int(db.val("SELECT count(*) FROM pg_stat_activity WHERE application_name LIKE 'load_w%' OR (backend_type = 'client backend' AND wait_event LIKE 'Sift%')"))
    return base(ctx, busy, 'w', lost_rows=len(acked - have['tbl']) + len(have['tbl'] ^ have['col']), dup_rows=n - d, shm_refusals=r1 - r0, rowlock_left=left,
                wedged=wedged, terminated=busy['w'].expected, oracle_mismatch=oracle(db, AGG.format(t='e_col'), 'sift.enable_agg'), shm_kb_after=(u1 - u0) // 1024)


CASES = (
    LoadCase('delta_hot_update_olap', 'delta', delta_hot_update_olap, Limits(p99_ratio=3.0, lwlock_wait_pct=30, pending_after=0), formats=(2,),
             open='S107-01,S107-28', intermittent=True),
    LoadCase('txn_ssi_2pc_savepoint', 'txn', txn_ssi_2pc_savepoint, Limits()),
    LoadCase('ddl_part_online_mix', 'ddl', ddl_part_online_mix, Limits()),
    LoadCase('pool_high_conn_term_shm', 'pool', pool_high_conn_term_shm, Limits(lwlock_wait_pct=40), formats=(2,), open='S107-20',
             intermittent=True),
)
