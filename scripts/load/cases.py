"""The catalog of the load gate (scripts/load/run.py): one frozen LoadCase a production shape of load, its body, and the ceilings its counts may not pass.

A body builds its tables in the schema load_case (from load_base_fact of fixtures.sql), runs worker sessions in phases (ctx.phase: every role at once for a few seconds, a
sampler beside them) and returns its counts; run.py adds the hygiene (sift.check, quarantine, sift.health, temporary files, the server log) and judges them all. A worker keeps
the rows it committed (its ledger): the tables must hold exactly those. `open` names the Spec 106 defect (LC-NN) of a case whose ceilings wait for its fix;
an `intermittent` one stays open in a run that does not meet its defect (one pass proves no fix: the closing commit drops both after a stress run of the case).
"""
from __future__ import annotations

import sys
import threading
import time
from dataclasses import dataclass
from typing import Callable

sys.dont_write_bytecode = True

from collect import (AGG, BM, IN_CASE, META, N, add, base, bm, col, ctx_kb, engines, hygiene, kept, ledger_writes, mem, mem_delta, multi_kept,  # noqa: E402
                     nxt, oracle, oracle_heap, oracles, p99_ratio, row3, tbl, unexpected_of, until, writers, zipf)

CLASSES = ('conc', 'pool', 'tenant', 'mix', 'bp', 'fault')
HARD = ('lost_rows', 'dup_rows', 'oracle_mismatch', 'verify_errors', 'false_quarantines', 'health_errors', 'unexpected_errors', 'tmp_files_left', 'traps_and_leaks')
KILLED = ('57014', '55P03', '57P01', '08006')
STATUS = f'FROM sift.status() WHERE relation::oid {IN_CASE}'
DRAINED = "SELECT count(*) FILTER (WHERE kind = 'queued') + max(workers_active) FROM sift.activity()"


@dataclass(frozen=True)
class Ceilings:
    lost_rows: int | None = 0
    dup_rows: int | None = 0
    oracle_mismatch: int | None = 0
    verify_errors: int | None = 0
    false_quarantines: int | None = 0
    health_errors: int | None = 0
    unexpected_errors: int | None = 0
    tmp_files_left: int | None = 0
    traps_and_leaks: int | None = 0
    deadlocks: int | None = 0
    maint_failures: int | None = 0
    coarse_escalations: int | None = None
    lock_wait_pct: float | None = None
    lwlock_wait_pct: float | None = None
    p99_ratio: float | None = None
    p99_read_ratio: float | None = None
    scale_ratio: float | None = None
    ctx_kb_delta: float | None = None
    rss_mb_delta: float | None = None
    shm_used_after: int | None = None
    shm_refusals: int | None = 0
    unconverged: int | None = 0
    drop_in_use: int | None = 0
    rowlock_left: int | None = 0
    l0_per_kop: float | None = None
    pending_after: int | None = None
    quarantine_missed: int | None = 0
    blocks_kept: int | None = 0
    not_restarted: int | None = 0
    meta_corrupt: int | None = 0


@dataclass(frozen=True)
class LoadCase:
    id: str
    cls: str
    body: Callable
    limits: Ceilings
    formats: tuple = (1, 2)
    testing: bool = False
    last: bool = False
    open: str = ''
    intermittent: bool = False


def conc_tbl_hotkey_zipf(ctx):
    ctx.db.run(tbl('acc') + '; UPDATE acc SET v = 0')
    ws = ctx.sessions(10)
    stmts = ('UPDATE acc SET v = v + 1 WHERE id = {k} RETURNING 1',
             "INSERT INTO acc VALUES ({k}, 0, 1, '') ON CONFLICT (id) DO UPDATE SET v = acc.v + 1 RETURNING 1",
             'UPDATE acc SET v = v + 1 WHERE id = (SELECT id FROM acc WHERE id >= {k} ORDER BY id LIMIT 1 FOR UPDATE SKIP LOCKED) RETURNING 1')

    def write(w, i):
        w.ledger += len(w.do(stmts[i % 3].format(k=zipf(w.seed * 7919 + i, N, 1.2))) or [])

    def read(w, i):
        k = zipf(w.seed * 104729 + i, N, 1.2)
        w.do(f'SELECT sum(v) FROM acc WHERE id BETWEEN {k} AND {k + 64}')

    quiet = ctx.phase({'r': (ws[8:], read)}, 0.6)
    busy = ctx.phase({'w': (ws[:8], write), 'r': (ws[8:], read)}, 1.5)
    total, n, d = (int(x) for x in ctx.db.row('SELECT sum(v), count(*), count(DISTINCT id) FROM acc'))
    return base(ctx, busy, 'w', lost_rows=abs(sum(w.ledger for w in ws) - total), dup_rows=n - d + abs(n - N), p99_ratio=p99_ratio(quiet['r'].lat, busy['r'].lat))


def conc_col_slab_comb_scale(ctx):
    ctx.db.run('CREATE TABLE ev (id int8, grp int, v int8) PARTITION BY HASH (id); '
               "DO $$ BEGIN FOR i IN 0..15 LOOP EXECUTE format('CREATE TABLE ev_%s PARTITION OF ev FOR VALUES WITH (MODULUS 16, REMAINDER %s)', i, i); END LOOP; END $$; "
               'INSERT INTO ev SELECT id, grp, v FROM load_base_fact; CREATE INDEX ev_ix ON ev USING sift_col (id, grp, v); ANALYZE ev')
    n = writers(ctx.cores)
    ws = ctx.sessions(n + 3)

    def ins(w, i):
        lo = w.seed * 10 ** 7 + getattr(w, 'seq', 0) * 10
        w.seq = getattr(w, 'seq', 0) + 1
        if w.do(f'INSERT INTO ev SELECT g, g % 64, g % 1000 FROM generate_series({lo}, {lo + 9}) g') is not None:
            w.ledger += 10

    def scan(w, i):
        w.do(f'SELECT grp, count(*), sum(v) FROM ev WHERE grp < {8 + i % 8} GROUP BY grp')

    def maint(w, i):
        w.do("SELECT count(*) FROM pg_inherits h, LATERAL sift.maintain(h.inhrelid::regclass) WHERE h.inhparent = 'ev_ix'::regclass")

    trials = [(ctx.phase({'w': (ws[:1], ins)}, 0.5), ctx.phase({'w': (ws[:n], ins), 'r': (ws[n:n + 2], scan), 'm': (ws[n + 2:], maint)}, 0.5)) for _ in range(2)]
    one, many = max((t[0] for t in trials), key=lambda r: r['w'].ops), max((t[1] for t in trials), key=lambda r: r['w'].ops)
    rows = int(ctx.db.val('SELECT count(*) FROM ev'))
    return base(ctx, many, 'w', lost_rows=abs(N + sum(w.ledger for w in ws) - rows), oracle_mismatch=oracle(ctx.db, AGG.format(t='ev'), 'sift.enable_agg'),
                scale_ratio=round(many['w'].ops / max(one['w'].ops, 1), 2), writers=n)


def conc_rowlock_overflow_mix(ctx):
    ctx.db.run("CREATE TABLE acc (id int8 PRIMARY KEY, grp int, v int8, body text) USING sift_table; "
               "INSERT INTO acc SELECT g, g % 64, 0, '' FROM generate_series(1, 20480) g")
    ws = ctx.sessions(8)

    def lock(w, i):
        a = (w.seed - 1) * 4608 + 1
        sql = f'BEGIN; SELECT count(*) FROM (SELECT 1 FROM acc WHERE id BETWEEN {a} AND {a + 4599} FOR UPDATE) s; UPDATE acc SET v = v + 1 WHERE id = {a + i % 4600}; COMMIT'
        w.ledger += w.do(sql) is not None

    def oltp(w, i):
        k = 18433 + (w.seed - 5) * 512 + i % 512
        sql = f'BEGIN; UPDATE acc SET v = v + 1 WHERE id = {k}; SAVEPOINT s; UPDATE acc SET v = v + 1000 WHERE id = {k}; ROLLBACK TO s; COMMIT'
        w.ledger += w.do(sql) is not None

    busy = ctx.phase({'w': (ws[4:], oltp), 'l': (ws[:4], lock)}, 1.5)
    left = sum(int(x) for x in ctx.db.row('SELECT in_chunks, chunks FROM sift._test_rowlock_stats()')) if ctx.testing else None
    return base(ctx, busy, 'w', lost_rows=abs(sum(w.ledger for w in ws) - int(ctx.db.val('SELECT sum(v) FROM acc'))), rowlock_left=left, locked=busy['l'].ops)


def conc_multi_engine_dml(ctx):
    engines(ctx.db)
    ws = ctx.sessions(10)

    def check(w, i):
        w.ledger += oracles(w)

    busy = ctx.phase({'w': (ws[:8], ledger_writes('e')), 'c': (ws[8:], check)}, 1.5)
    return base(ctx, busy, 'w', **add(multi_kept(ctx.db, 'e', ws[:8]), {'oracle_mismatch': sum(w.ledger for w in ws[8:]) + oracles(ctx.db)}))


def tenants(db, n: int, rows: int):
    for j in range(n):
        db.run(f'CREATE SCHEMA load_case_t{j}; SET search_path = load_case_t{j}, public; '
               f'CREATE TABLE acc (id int8 PRIMARY KEY, tenant int, v int8) USING sift_table; INSERT INTO acc SELECT g, {j}, 0 FROM generate_series(0, {rows - 1}) g; '
               f'CREATE TABLE ev (id int8, tenant int, v int8); INSERT INTO ev SELECT g, {j}, g FROM generate_series(0, {rows - 1}) g; CREATE INDEX ev_ix ON ev USING sift_col (tenant, v); '
               f"CREATE TABLE docs (id int8, body text); INSERT INTO docs SELECT g, 'wire t{j}' FROM generate_series(0, {rows - 1}) g; CREATE INDEX docs_ix ON docs USING sift (body); "
               f'GRANT USAGE ON SCHEMA load_case_t{j} TO load_tenant; GRANT SELECT, UPDATE ON ALL TABLES IN SCHEMA load_case_t{j} TO load_tenant')
    db.run('SET search_path = load_case, public')


def drop_tenants(db, n: int):
    db.run('; '.join(f'DROP SCHEMA load_case_t{j} CASCADE' for j in range(n)) + '; DROP OWNED BY load_tenant; DROP ROLE load_tenant')


def pool_tx_multiplex_iso(ctx):
    n, rows = 16, 128
    ctx.db.run('DROP ROLE IF EXISTS load_tenant; CREATE ROLE load_tenant')
    tenants(ctx.db, n, rows)
    ws = ctx.sessions(4)

    def tx(w, i):
        j = (w.seed * 5 + i) % n
        reset = 'DISCARD ALL' if i % 2 else 'RESET ALL; RESET ROLE; DEALLOCATE ALL'
        out = w.do(f'SET ROLE load_tenant; SET search_path = load_case_t{j}; PREPARE p AS SELECT min(tenant), max(tenant), count(*) FROM acc; EXECUTE p; '
                   f"SELECT min(tenant), max(tenant), count(*) FROM ev WHERE v >= 0; SELECT count(*) FROM docs WHERE body @@> 't{j}'; "
                   f'UPDATE acc SET v = v + 1 WHERE id = {i % rows} RETURNING tenant; {reset}')
        if out is not None:
            w.ledger += out != [f'{j}|{j}|{rows}', f'{j}|{j}|{rows}', str(rows), str(j)]
            w.ups = getattr(w, 'ups', 0) + 1

    ctx.phase({'w': (ws, tx)}, 0.5)
    before = mem(ctx, ws)
    busy = ctx.phase({'w': (ws, tx)}, 1.5)
    after = mem(ctx, ws)
    total = sum(int(ctx.db.val(f'SELECT sum(v) FROM load_case_t{j}.acc')) for j in range(n))
    m = base(ctx, busy, 'w', oracle_mismatch=sum(w.ledger for w in ws), lost_rows=abs(total - sum(getattr(w, 'ups', 0) for w in ws)), **mem_delta(ctx, before, after))
    m.update({k: v for k, v in hygiene(ctx.db).items() if v})
    ctx.close_sessions()
    drop_tenants(ctx.db, n)
    return m


def pool_prepared_ddl_inval(ctx):
    engines(ctx.db, kinds=('tbl', 'col'))
    ws = ctx.sessions(5)
    prep = ('PREPARE ins(int8) AS INSERT INTO e_col (id, grp, v) VALUES ($1, $1 % 64, $1 % 1000); PREPARE upd(int8) AS UPDATE e_tbl SET v = v + 1 WHERE id = $1 RETURNING 1; '
            'PREPARE agg AS SELECT grp, sum(v) FROM e_col GROUP BY grp ORDER BY grp')
    for w in ws[:4]:
        w.run(prep)

    def pooled(w, i):
        k = nxt(w)
        if w.do(f'BEGIN; EXECUTE ins({k}); EXECUTE upd({1 + i % N}); COMMIT') is not None:
            w.ids, w.ups = getattr(w, 'ids', set()) | {k}, getattr(w, 'ups', 0) + 1
        if w.do('EXECUTE agg', ('40001',)) is None:
            w.do('EXECUTE agg')

    ddl = ('VACUUM e_col, e_tbl', "SELECT busy FROM sift.maintain('e_col_ix')", 'REINDEX INDEX CONCURRENTLY e_col_ix', 'ALTER TABLE e_col ADD COLUMN c{i} int',
           "SELECT busy FROM sift.maintain('e_tbl')", 'ALTER TABLE e_tbl ADD COLUMN c{i} int')

    def churn(w, i):
        w.do(ddl[i % len(ddl)].format(i=i))

    ctx.phase({'w': (ws[:4], pooled)}, 0.3)
    k0 = [ctx_kb(w) for w in ws[:4]]
    busy = ctx.phase({'w': (ws[:4], pooled), 'd': (ws[4:], churn)}, 1.5)
    total = int(ctx.db.val('SELECT sum(v) FROM e_tbl')) - int(ctx.db.val('SELECT sum(v) FROM load_base_fact'))
    ids = set().union(*(getattr(w, 'ids', set()) for w in ws))
    return base(ctx, busy, 'w', **add(kept(ctx.db, 'e_col', ids, 10 ** 6), {'lost_rows': abs(total - sum(getattr(w, 'ups', 0) for w in ws))}),
                oracle_mismatch=oracle(ctx.db, AGG.format(t='e_col'), 'sift.enable_agg'), ctx_kb_delta=max(ctx_kb(w) - a for w, a in zip(ws[:4], k0)), ddl=busy['d'].ops,
                replanned=busy['w'].expected)


def tenant_part_cache_churn(ctx):
    ctx.db.run('CREATE TABLE pt (tenant int, id int8, v int8, PRIMARY KEY (tenant, id)) PARTITION BY LIST (tenant); '
               'CREATE TABLE pe (tenant int, id int8, grp int, v int8) PARTITION BY LIST (tenant); '
               "DO $$ BEGIN FOR i IN 0..63 LOOP EXECUTE format('CREATE TABLE pt_%s PARTITION OF pt FOR VALUES IN (%s) USING sift_table', i, i); "
               "EXECUTE format('CREATE TABLE pe_%s PARTITION OF pe FOR VALUES IN (%s)', i, i); END LOOP; END $$; "
               'INSERT INTO pt SELECT id % 64, id, 0 FROM load_base_fact; INSERT INTO pe SELECT id % 64, id, grp, v FROM load_base_fact; '
               'CREATE INDEX pe_ix ON pe USING sift_col (tenant, grp, v); ANALYZE pt, pe')
    ws = ctx.sessions(6, 'SET sift.backend_cache_kb = 1024')

    def work(w, i):
        t = (w.seed * 13 + i * 7) % 64
        out = w.do(f'SELECT sum(v) FROM pt WHERE tenant = {t}; UPDATE pt SET v = v + 1 WHERE tenant = {t} AND id = {t + 64 * (i % 128)} RETURNING 1; '
                   f'SELECT grp, sum(v) FROM pe WHERE tenant = {(t + 32) % 64} GROUP BY grp')
        w.ledger += out is not None

    ctx.phase({'w': (ws, work)}, 0.5)
    before = [ctx_kb(w) for w in ws], ctx.rss(ws)
    busy = ctx.phase({'w': (ws, work)}, 1.5)
    after = [ctx_kb(w) for w in ws], ctx.rss(ws)
    return base(ctx, busy, 'w', lost_rows=abs(sum(w.ledger for w in ws) - int(ctx.db.val('SELECT sum(v) FROM pt'))),
                oracle_mismatch=oracle(ctx.db, 'SELECT tenant, sum(v) FROM pe GROUP BY tenant ORDER BY 1', 'sift.enable_agg'), **mem_delta(ctx, before, after))


MANIFEST = "SELECT used_bytes, refusals FROM sift.shared_memory_pools() WHERE pool = 'manifest'"


def tenant_ddl_drop_shm_reclaim(ctx):
    engines(ctx.db, 'sib', kinds=('tbl', 'col'))
    ws = ctx.sessions(6)

    def churn(w, i):
        if i >= 8:
            return False
        t = f'tmp_{w.seed}_{i}'
        load = f'INSERT INTO {t} SELECT id, grp, v, body FROM load_base_fact WHERE id <= 256; INSERT INTO {t}_h SELECT * FROM load_base_fact WHERE id <= 256'
        w.do(f'{tbl(t, 256)}; {col(t + "_h", 256)}; SELECT grp, sum(v) FROM {t}_h GROUP BY grp; SELECT count(*) FROM {t}; '
             f'TRUNCATE {t}, {t}_h; {load}; SELECT count(*) FROM {t}_h WHERE v > 10; DROP TABLE {t}, {t}_h')

    def read(w, i):
        w.do('SELECT count(*) FROM sib_tbl; SELECT grp, sum(v) FROM sib_col GROUP BY grp; SELECT count(*) FROM sift.status(); SELECT count(*) FROM sift.health()')

    ctx.phase({'r': (ws[4:], read)}, 0.3)
    u0, r0 = (int(x) for x in ctx.db.row(MANIFEST))
    busy = ctx.phase({'w': (ws[:4], churn), 'r': (ws[4:], read)}, 10.0, lead='w')
    ctx.close_sessions()
    u1, r1 = (int(x) for x in ctx.db.row(MANIFEST))
    left = int(ctx.db.val("SELECT count(*) FROM pg_class WHERE relname LIKE 'tmp\\_%' AND relnamespace = 'load_case'::regnamespace"))
    return base(ctx, busy, 'w', shm_used_after=max(0, u1 - u0), shm_refusals=r1 - r0, cycles=busy['w'].ops, lost_rows=left)


DBS = ('load_case_hot', 'load_case_q1', 'load_case_q2')
QUIET = 'SELECT count(*) FROM sift.status() WHERE dead_pct >= 15 OR l0_slabs > 0'


def tenant_multidb_bgw_fair(ctx):
    db = ctx.db
    for d in DBS:
        db.run(f'DROP DATABASE IF EXISTS {d} WITH (FORCE); CREATE DATABASE {d}')
        ctx.sessions(1, db=d, path='public')[0].run(f'CREATE EXTENSION pg_sift; {tbl("acc", 4096, False)}; {col("ev", 4096, False)}')
    db.run('ALTER SYSTEM SET sift.max_maint_workers = 1; SELECT pg_reload_conf()')
    try:
        hot = ctx.sessions(1, 'SET sift.async_maintenance = on', DBS[0], 'public')
        quiet = [ctx.sessions(1, 'SET sift.async_maintenance = on', d, 'public')[0] for d in DBS[1:]]
        watch = [ctx.sessions(1, db=d, path='public')[0] for d in DBS[1:]]

        def churn(w, i):
            w.do(f'UPDATE acc SET v = v + 1 WHERE id = {1 + i * 37 % 4096}; INSERT INTO ev VALUES ({10 ** 6 + i}, 1, 1)')

        def burst(w, i):
            w.do('UPDATE acc SET v = v + 1 WHERE id % 2 = 0; DELETE FROM ev WHERE id % 3 = 0')
            return False

        quiet_sql = QUIET if ctx.fmt == 2 else DRAINED

        def watcher(w, i):
            time.sleep(0.1)
            return int((w.do(quiet_sql) or ['1'])[0]) > 0

        busy = ctx.phase({'h': (hot, churn), 'q': (quiet, burst), 'x': (watch, watcher)}, 20.0, lead='x')
        unconv = sum(int(w.val(quiet_sql)) > 0 for w in watch)
        checks = sum(int(w.val("SELECT count(*) FROM sift.check('acc', true)")) + int(w.val("SELECT count(*) FROM sift.check('ev_ix', true)")) for w in hot + watch)
        failures = sum(int(w.val('SELECT coalesce(sum(maint_failures), 0) FROM sift.status()')) for w in hot + watch)
        stuck = [f'note: {w.dbname} {r}' for w in watch for r in w.run(QUIET.replace('count(*)', "format('%s dead_pct=%s l0_slabs=%s', relation, dead_pct, l0_slabs)"))] if unconv else []
        for w in [w for w in ctx.open_sessions if w.dbname == DBS[2]]:
            w.close()
            ctx.open_sessions.remove(w)
        _, err, _ = db.q(f'DROP DATABASE {DBS[2]}')
        m = base(ctx, busy, 'h', unconverged=unconv, verify_errors=checks, maint_failures=failures, drop_in_use=int(err is not None and err[0] == '55006'))
        return dict(m, notes=m['notes'] + stuck)
    finally:
        ctx.close_sessions()
        db.run('ALTER SYSTEM RESET sift.max_maint_workers; SELECT pg_reload_conf()')
        for d in DBS:
            db.run(f'DROP DATABASE IF EXISTS {d} WITH (FORCE)')


PAR = 'SET max_parallel_workers_per_gather = 2; SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0; SET min_parallel_index_scan_size = 0'
OLAP = (AGG.format(t='e_col'), 'SELECT id, v FROM e_col ORDER BY v DESC, id LIMIT 10', 'SELECT count(*), sum(v) FROM e_col WHERE grp BETWEEN 3 AND 9',
        'SELECT grp, count(*) FROM e_tbl GROUP BY grp ORDER BY grp', BM.format(t='e_bm'))


def mix_oltp_par_olap(ctx):
    engines(ctx.db)
    ws = ctx.sessions(6, PAR)

    def olap(w, i):
        if i % 5 == 4:
            w.ledger += oracle(w, AGG.format(t='e_col'), 'sift.enable_agg')
        else:
            w.do(OLAP[i % len(OLAP)])

    quiet = ctx.phase({'r': (ws[4:], olap)}, 0.6)
    busy = ctx.phase({'w': (ws[:4], ledger_writes('e')), 'r': (ws[4:], olap)}, 1.5)
    obs = ctx.obs
    after = ctx.phase({'r': (ws[4:], olap)}, 0.6)
    ctx.obs = obs
    return base(ctx, busy, 'w', **add(multi_kept(ctx.db, 'e', ws[:4]), {'oracle_mismatch': sum(w.ledger for w in ws[4:]) + oracles(ctx.db)}),
                p99_ratio=p99_ratio(quiet['r'].lat + after['r'].lat, busy['r'].lat))


def mix_pacer_autovac_iso(ctx):
    db = ctx.db
    engines(db)
    db.run('UPDATE e_tbl SET v = v + 1 WHERE id % 2 = 0; DELETE FROM e_col WHERE id % 4 = 0; DELETE FROM e_bm WHERE id % 4 = 1')
    ws = ctx.sessions(6)

    def read(w, i):
        w.do(OLAP[i % len(OLAP)])

    write = ledger_writes('e')
    quiet = ctx.phase({'w': (ws[:4], write), 'r': (ws[4:], read)}, 1.0)
    db.run("ALTER SYSTEM SET autovacuum = on; ALTER SYSTEM SET autovacuum_naptime = '1s'; SELECT pg_reload_conf()")
    try:
        for w in ws:
            w.run('SET sift.async_maintenance = on')
        busy = ctx.phase({'w': (ws[:4], write), 'r': (ws[4:], read)}, 1.5, pacer=True)
        throttled, seen = ctx.obs.throttled, ctx.obs.busy
        unconv = until(db, DRAINED, drain_cap(ctx))
    finally:
        db.run('ALTER SYSTEM RESET autovacuum; ALTER SYSTEM RESET autovacuum_naptime; SELECT pg_reload_conf()')
    return base(ctx, busy, 'w', **multi_kept(db, 'e', ws[:4]), oracle_mismatch=oracles(db), unconverged=unconv, pacer=f'{throttled}/{seen}',
                p99_ratio=p99_ratio(quiet['w'].lat, busy['w'].lat), p99_read_ratio=p99_ratio(quiet['r'].lat, busy['r'].lat))


def drain_cap(ctx) -> float:
    """the liveness cap of a drain: 15 s, four times that with assertions (the maintenance itself runs 5-15 times slower)"""
    return 15.0 * ctx.a.scale * (4 if ctx.assert_build else 1)


def bp_burst_l0_starvation(ctx):
    engines(ctx.db, kinds=('tbl', 'col'))
    ws = ctx.sessions(6, 'SET sift.async_maintenance = on')
    busy = ctx.phase({'w': (ws, ledger_writes('e', ('tbl', 'col')))}, 1.5, peak=f'SELECT coalesce(sum(l0_parts + l0_slabs), 0) {STATUS}')
    peak = ctx.obs.peak
    backlog = f"SELECT ({DRAINED}) + (SELECT count(*) FROM sift.health() WHERE check_name IN ('l0_backlog', 'worker_failing'))"
    unconv = until(ctx.db, backlog, drain_cap(ctx))
    left = int(ctx.db.val(f'SELECT coalesce(sum(l0_parts + l0_slabs), 0) {STATUS}'))
    return base(ctx, busy, 'w', **multi_kept(ctx.db, 'e', ws, ('tbl', 'col')), oracle_mismatch=oracle(ctx.db, AGG.format(t='e_col'), 'sift.enable_agg'),
                peak=peak, l0_per_kop=round(1000.0 * peak / max(busy['w'].ops, 1), 1), unconverged=unconv, l0_left=left)


def bp_burst_meta_race(ctx):
    """8 writers insert, update and delete in a sift_table and a sift_col index with async maintenance on, for 8 s: none may meet a false corruption (XX002) of the meta of the index"""
    engines(ctx.db, kinds=('tbl', 'col'))
    ws = ctx.sessions(8, 'SET sift.async_maintenance = on')

    def write(w, i):
        k = nxt(w)
        w.do(f"BEGIN; {row3('e_tbl', k)}; {row3('e_col', k)}; UPDATE e_tbl SET v = v + 1 WHERE id = {k - 1}; DELETE FROM e_tbl WHERE id = {k - 2}; COMMIT", (META,))
        return not any(x.expected for x in ws)

    busy = ctx.phase({'w': (ws, write)}, 8.0, lead='w')
    return base(ctx, busy, 'w', meta_corrupt=busy['w'].expected)


DIGEST = "SELECT md5(string_agg(id || ':' || v, ',' ORDER BY id)) FROM {t}"


def bp_long_snap_churn_reclaim(ctx):
    db = ctx.db
    db.run(f'{tbl("tb")}; CREATE TABLE tw AS SELECT * FROM tb; {col("ev")}')
    hold = ctx.sessions(1)[0]
    both = f'{DIGEST.format(t="tb")}; {AGG.format(t="ev")}'
    d0 = hold.run(f'BEGIN ISOLATION LEVEL REPEATABLE READ; {both}')
    ws = ctx.sessions(5, 'SET sift.async_maintenance = on')

    def churn(w, i):
        k = 1 + (i * 4 + w.seed - 1) % N
        op = f'DELETE FROM {{t}} WHERE id = {k}' if i % 2 else f'UPDATE {{t}} SET v = v + 1 WHERE id = {k}'
        w.do(f'BEGIN; {op.format(t="tb")}; {op.format(t="tw")}; {op.format(t="ev")}; COMMIT')

    def held(w, i):
        w.ledger += (w.do(both) or []) != d0
        time.sleep(0.02)

    def vacuum(w, i):
        w.do('VACUUM tb, ev')

    busy = ctx.phase({'w': (ws[:4], churn), 'h': ([hold], held), 'v': (ws[4:], vacuum)}, 1.5)
    snap = hold.ledger + (hold.run(both) != d0)
    hold.run('COMMIT')
    twin = int(db.val(DIGEST.format(t='tb')) != db.val(DIGEST.format(t='tw')))
    db.run("VACUUM tb, ev; SELECT busy FROM sift.maintain('tb'); SELECT busy FROM sift.maintain('ev_ix')")
    pending = int(db.val("SELECT (SELECT pending_blocks FROM sift.space('tb')) + (SELECT pending_blocks FROM sift.space('ev_ix'))"))
    return base(ctx, busy, 'w', oracle_mismatch=snap + twin + oracle(db, AGG.format(t='ev'), 'sift.enable_agg'), pending_after=pending)


def fault_cancel_timeout_storm(ctx):
    db = ctx.db
    engines(db)
    holder = ctx.sessions(1)[0]
    holder.run('BEGIN; SELECT id FROM e_tbl WHERE id = 1 FOR UPDATE')
    ws = ctx.sessions(9)
    for w in ws[4:8]:
        w.allow, w.stored = KILLED, set()
    storm = (lambda lo: f"SET statement_timeout = '30ms'; WITH i AS (INSERT INTO e_col SELECT g, g % 64, g % 1000 FROM generate_series({lo}, {lo + 49999}) g RETURNING 1) "
                        "SELECT count(*) FROM i; RESET statement_timeout",
             lambda lo: "SET lock_timeout = '20ms'; UPDATE e_tbl SET v = v + 1 WHERE id = 1; RESET lock_timeout",
             lambda lo: 'SELECT count(*) FROM e_col a JOIN e_col b USING (grp)',
             lambda lo: f"WITH i AS (INSERT INTO e_bm SELECT g, g % 64, 0, 'wire ' || md5(g::text) FROM generate_series({lo}, {lo + 19999}) g RETURNING 1) SELECT count(*) FROM i")

    def hit(w, i):
        w.pid
        lo = 10 ** 8 * w.seed + 10 ** 6 * i
        out = w.do(storm[i % 4](lo), KILLED)
        if out and i % 4 in (0, 3):
            w.stored.add(lo // 10 ** 6)

    def control(w, i):
        pid = ws[4 + i % 4]._pid
        if pid:
            w.do(f'SELECT pg_{"terminate" if i % 5 == 4 else "cancel"}_backend({pid})')
        time.sleep(0.02)

    busy = ctx.phase({'w': (ws[:4], ledger_writes('e')), 's': (ws[4:8], hit), 'c': (ws[8:], control)}, 1.5)
    holder.run('COMMIT')
    acked = set().union(*(w.stored for w in ws[4:8]))
    extra = 0
    for t, batch in (('col', 50000), ('bm', 20000)):
        done = {int(b): int(n) for b, n in (r.split('|') for r in db.run(f'SELECT id / {10 ** 6}, count(*) FROM e_{t} WHERE id >= {10 ** 8} GROUP BY 1'))}
        extra += sum(n != batch for n in done.values()) + len({b for b in acked if b % 4 == (0 if t == 'col' else 3)} - set(done))
    left = sum(int(x) for x in db.row('SELECT in_chunks, chunks FROM sift._test_rowlock_stats()')) if ctx.testing else None
    return base(ctx, busy, 'w', **add(multi_kept(db, 'e', ws[:4]), {'lost_rows': extra}), oracle_mismatch=oracles(db), rowlock_left=left, killed=busy['s'].expected)


def fault_enospc_quarantine_iso(ctx):
    db = ctx.db
    engines(db, kinds=('col', 'bm'))
    db.run(bm('h_bm'))
    ws = ctx.sessions(4)
    sort = "SET work_mem = '64kB'; SET temp_file_limit = '128kB'; SELECT count(*) FROM (SELECT * FROM e_col ORDER BY body, v) s"

    def spill(w, i):
        w.do(sort, ('53400',))
        w.run('RESET temp_file_limit; RESET work_mem')
        w.ledger += w.do('SELECT count(*) FROM (SELECT * FROM e_col ORDER BY body, v) s') != [str(N)]

    full = ctx.phase({'s': (ws, spill)}, 0.5)
    m = {'oracle_mismatch': sum(w.ledger for w in ws), 'ops': full['s'].ops, 'temp_limited': full['s'].expected, 'notes': [], 'unexpected_errors': unexpected_of(full), **ctx.obs.metrics()}
    if not ctx.testing:
        return dict(m, notes=['note: the ENOSPC and quarantine sub-checks need the testing library'])
    used, eof = (int(x) for x in db.row("SELECT used_blocks, relation_blocks FROM sift.space('e_col_ix')"))

    def enospc(w, i):
        lo = 10 ** 7 * w.seed + 10 ** 5 * i
        w.do(f'SET sift.test_disk_blocks = {eof}; INSERT INTO e_col SELECT g, g % 64, g FROM generate_series({lo}, {lo + 49999}) g', ('53100',))
        w.run('SET sift.test_disk_blocks = 0')
        return i < 2

    disk = ctx.phase({'s': (ws, enospc)}, 5.0, lead='s')
    kept_blocks = max(0, int(db.val("SELECT used_blocks FROM sift.space('e_col_ix')")) - used)
    db.run('INSERT INTO e_col SELECT g, g % 64, g FROM generate_series(1, 64) g')
    rows = abs(int(db.val('SELECT count(*) FROM e_col')) - N - 64 - 50000 * disk['s'].ops)
    want = {t: oracle_heap(db, t) for t in ('e_bm', 'h_bm')}
    db.run("SELECT sift._test_poke('e_bm_ix', 0, 24, 0, 4)")
    damaged = db.val("SELECT 'e_bm_ix'::regclass::oid")

    def read(w, i):
        t = ('e_bm', 'h_bm')[i % 2]
        out = w.do(BM.format(t=t), ('XX002',) if t == 'e_bm' else ())
        w.ledger += out is not None and out != [want[t]]

    reads = ctx.phase({'r': (ws, read)}, 1.0)
    missed = 1 - int(db.val(f'SELECT count(*) FROM sift.quarantine WHERE index::oid = {damaged}'))
    return add(m, {'oracle_mismatch': sum(w.ledger for w in ws) + oracle(db, AGG.format(t='e_col'), 'sift.enable_agg'), 'unexpected_errors': unexpected_of(disk, reads),
                   'disk_full': disk['s'].expected, 'quarantine_missed': missed, 'blocks_kept': kept_blocks, 'lost_rows': rows, 'damaged': (damaged,)})


def fault_kill9_multi_engine(ctx):
    db = ctx.db
    engines(db)
    up = db.val('SELECT pg_postmaster_start_time()')
    ws = ctx.sessions(7, 'SET sift.async_maintenance = on')
    write = ledger_writes('e', churn=False)
    for w in ws[:6]:
        w.allow = ('08006', '57P01')

    def crash(w, i):
        if int(w.val('SELECT count(*) FROM e_tbl WHERE id >= 1000000')) < 200 and i < 400:
            return time.sleep(0.01)
        ctx.hook('LOAD_CRASH')
        return False

    busy = ctx.phase({'w': (ws[:6], write), 'k': (ws[6:], crash)}, 30.0, lead='k')
    ctx.close_sessions()
    ctx.restart('LOAD_RESTART')
    acked = set().union(*(getattr(w, 'ids', set()) for w in ws[:6]))
    have = {int(x) for x in db.run('SELECT id FROM e_tbl WHERE id >= 1000000')}
    m = add(kept(db, 'e_col', have, 10 ** 6), kept(db, 'e_bm', have, 10 ** 6), {'lost_rows': len(acked - have)})
    return base(ctx, busy, 'w', **m, oracle_mismatch=oracles(db), acked=len(acked), not_restarted=int(db.val('SELECT pg_postmaster_start_time()') == up))


CASES = (
    LoadCase('conc_tbl_hotkey_zipf', 'conc', conc_tbl_hotkey_zipf, Ceilings(lwlock_wait_pct=30, coarse_escalations=0, p99_ratio=3.0)),
    LoadCase('conc_col_slab_comb_scale', 'conc', conc_col_slab_comb_scale, Ceilings(scale_ratio=2.5, lwlock_wait_pct=25), formats=(2,)),
    LoadCase('conc_rowlock_overflow_mix', 'conc', conc_rowlock_overflow_mix, Ceilings(coarse_escalations=0, lwlock_wait_pct=25), formats=(2,)),
    LoadCase('conc_multi_engine_dml', 'conc', conc_multi_engine_dml, Ceilings(lwlock_wait_pct=30)),
    LoadCase('pool_tx_multiplex_iso', 'pool', pool_tx_multiplex_iso, Ceilings(ctx_kb_delta=64, rss_mb_delta=8)),
    LoadCase('pool_prepared_ddl_inval', 'pool', pool_prepared_ddl_inval, Ceilings(ctx_kb_delta=1280)),
    LoadCase('tenant_part_cache_churn', 'tenant', tenant_part_cache_churn, Ceilings(ctx_kb_delta=1280, rss_mb_delta=8, lock_wait_pct=10)),
    LoadCase('tenant_ddl_drop_shm_reclaim', 'tenant', tenant_ddl_drop_shm_reclaim, Ceilings(shm_used_after=0)),
    LoadCase('tenant_multidb_bgw_fair', 'tenant', tenant_multidb_bgw_fair, Ceilings()),
    LoadCase('mix_oltp_par_olap', 'mix', mix_oltp_par_olap, Ceilings(lwlock_wait_pct=25, p99_ratio=2.5)),
    LoadCase('mix_pacer_autovac_iso', 'mix', mix_pacer_autovac_iso, Ceilings(p99_ratio=2.5, p99_read_ratio=2.0)),
    LoadCase('bp_burst_l0_starvation', 'bp', bp_burst_l0_starvation, Ceilings(l0_per_kop=50), formats=(2,)),
    LoadCase('bp_burst_meta_race', 'bp', bp_burst_meta_race, Ceilings(), formats=(2,)),
    LoadCase('bp_long_snap_churn_reclaim', 'bp', bp_long_snap_churn_reclaim, Ceilings(pending_after=0)),
    LoadCase('fault_cancel_timeout_storm', 'fault', fault_cancel_timeout_storm, Ceilings()),
    LoadCase('fault_enospc_quarantine_iso', 'fault', fault_enospc_quarantine_iso, Ceilings(), formats=(2,)),
    LoadCase('fault_kill9_multi_engine', 'fault', fault_kill9_multi_engine, Ceilings(), last=True),
)
