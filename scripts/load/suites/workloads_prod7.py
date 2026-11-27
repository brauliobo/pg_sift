"""The seventh wave of production shapes of the load gate (Spec 119, test/t/core/load_stress.sh and load_repl.sh): BM25 over documents of tens of kilobytes read by
parallel ranked scans under cancels, a sift_col index with a pre-aggregate cube, HyperLogLog sketches and a Bloom skip index under churn and rolled-back outliers,
materialized views stored as sift_tables refreshed beside bitmap scans of a bulk-written table, every row-lock mode across 20 sift_tables in one transaction (past the
16 fast-path slots of a backend), a hybrid of BM25, a column index and an as-of join of a time series beside sift.explain_why, and index-only scans of a hot standby
through VACUUM (FREEZE) and checkpoints. Every case runs on Format 2.0 and Format 1.0 and on the release library. Three cases wait for a finding, each intermittent (met
on the other format): the large documents for S119-27 (on Format 2.0 a BM25 index of a sift_table with write_buffer_limit = 64 over documents larger than its buffer loses
entries when 2 sessions insert while VACUUM and sift.maintain run, or while they update the same rows: sift.check finds a heap tuple with no entry in the index and a
query finds fewer documents than the heap twin; one writer, small documents or the default buffer are clean), the row-lock modes for S119-28 (on Format 2.0 a
transaction that locks one row in each of more than 16 sift_tables turns row locks into locks of their relations: 3 coarse locks at 17 tables, 6 at 18, 12 to 40 at
20, in every mode; none at 16), and the hot standby for S119-29 (on Format 1.0 a REPEATABLE READ snapshot of a hot standby answers a GROUP BY of a sift_table
by an index-only scan of its covering btree and by a sequential scan with a row of difference, either way, while the primary replays inserts and updates beside VACUUM
(FREEZE) and checkpoints; the heap twin, the primary and Format 2.0 agree).
"""
from __future__ import annotations

import time

from cases import KILLED, LoadCase
from collect import add, base, checks, ctx_kb, halves_differ, kept, mem_delta, nxt, snap
from suites.common import Limits
from suites.repl import catchup, standby
from suites.workloads_olap import cancels
from suites.workloads_prod2 import snap_oracle, snap_pair
from suites.workloads_prod3 import ROW_DIGEST, digest_differs, dups
from suites.workloads_prod4 import paced

CACHE = 'SET sift.backend_cache_kb = 1024'
TIGHT = "SET work_mem = '256kB'"
WORDS = 400


def z_doc(k: str, n: int) -> str:
    """the text of document K: N words of a vocabulary of WORDS, about 4.5 bytes each"""
    return f"(SELECT string_agg('w' || (({k}) * 31 + j) % {WORDS}, ' ') FROM generate_series(1, {n}) j)"


Z_SHAPE = 'id int8 PRIMARY KEY, grp int, body text'
Z_TOPK = ("SELECT count(*), count(*) FILTER (WHERE NOT (body ~ '\\mw3\\M' AND body ~ '\\mw17\\M')) FROM (SELECT body FROM {t} WHERE body @@> 'w3 AND w17' "
          'ORDER BY sift.score(ctid) DESC LIMIT 10) s')
Z_HEAP = "SELECT least(10, count(*)), 0 FROM {t} WHERE body ~ '\\mw3\\M' AND body ~ '\\mw17\\M'"
Z_BOOL = "SELECT count(*), sum(id) FROM {t} WHERE body @@> 'w5 OR (w9 AND NOT w11)'"
PAR = 'SET max_parallel_workers_per_gather = 2; SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0'


def conc_bm25_par_largedoc_cancel(ctx):
    """a sift_table and a heap twin of 64 documents of 40 to 64 kB, each with a BM25 index of a write buffer of 64 kB: 2 writers insert, extend and delete documents of
    both in one transaction, 4 readers (parallel plans, a backend cache of 1 MB) check that a ranked top 10 holds only documents that have both words and as many as the
    heap has, compare a boolean query with the heap and the two tables in one snapshot, while 1 cancels them and 1 vacuums and maintains: every committed document kept
    once, every answer agrees, the memory of the readers after the load"""
    db = ctx.db
    fill = ''.join(f'; INSERT INTO {t} SELECT g, g % 8, {z_doc("g", "9000 + g * 60")} FROM generate_series(1, 64) g' for t in ('z_tbl', 'z_hp'))
    db.run(f'CREATE TABLE z_tbl ({Z_SHAPE}) USING sift_table; CREATE TABLE z_hp ({Z_SHAPE}); CREATE INDEX z_tbl_bm ON z_tbl USING sift (body) WITH (write_buffer_limit = 64); '
           f'CREATE INDEX z_hp_bm ON z_hp USING sift (body) WITH (write_buffer_limit = 64){fill}; ANALYZE z_tbl, z_hp')
    ws = ctx.sessions(2) + ctx.sessions(4, f'{PAR}; {CACHE}') + ctx.sessions(2)

    def write(w, i):
        w.ids, k = getattr(w, 'ids', set()), nxt(w)
        old = sorted(w.ids)[:1] if i % 3 == 2 else []
        sql = ''.join(f'; INSERT INTO {t} VALUES ({k}, {k % 8}, {z_doc(str(k), 9000 + k % 5000)}); UPDATE {t} SET body = body || {z_doc(str(i), 50)} '
                      f'WHERE id = {1 + (w.seed * 13 + i) % 64}' + (f'; DELETE FROM {t} WHERE id = {old[0]}' if old else '') for t in ('z_tbl', 'z_hp'))
        if w.do(f'BEGIN{sql}; COMMIT') is not None:
            w.ids = (w.ids | {k}) - set(old)

    def read(w, i):
        t = ('z_tbl', 'z_hp')[i % 2]
        w.ledger += (snap_pair(w, Z_TOPK.format(t=t), Z_HEAP.format(t=t), KILLED), snap_oracle(w, Z_BOOL.format(t=t), 'sift.enable_scan', KILLED),
                     snap_pair(w, Z_BOOL.format(t='z_tbl'), Z_BOOL.format(t='z_hp'), KILLED))[(w.seed + i) % 3]

    def maint(w, i):
        w.do(('VACUUM z_tbl, z_hp', "SELECT count(*) FROM sift.maintain('z_tbl_bm')", "SELECT count(*) FROM sift.maintain('z_hp_bm')")[i % 3])
        time.sleep(0.05)

    roles = {'w': (ws[:2], write), 'r': (ws[2:6], read), 'c': (ws[6:7], cancels(ws[2:6])), 'm': (ws[7:], maint)}
    ctx.phase(roles, 0.5)
    before = [ctx_kb(w) for w in ws[2:6]], ctx.rss(ws[2:6])
    busy = ctx.phase(roles, 2.5)
    after = [ctx_kb(w) for w in ws[2:6]], ctx.rss(ws[2:6])
    ctx.close_sessions()
    chk = ctx.sessions(1)[0]
    ids = set().union(*(getattr(w, 'ids', set()) for w in ws[:2]))
    mismatch = sum(w.ledger for w in ws[2:6]) + sum(snap_pair(chk, Z_TOPK.format(t=t), Z_HEAP.format(t=t), ()) for t in ('z_tbl', 'z_hp'))
    mismatch += snap_pair(chk, Z_BOOL.format(t='z_tbl'), Z_BOOL.format(t='z_hp'), ())
    return base(ctx, busy, 'r', **add(kept(chk, 'z_tbl', ids, 10 ** 6), kept(chk, 'z_hp', ids, 10 ** 6), {'dup_rows': dups(chk, 'z_tbl')}), oracle_mismatch=mismatch,
                writes=busy['w'].ops, cancelled=busy['r'].expected, **mem_delta(ctx, before, after))


K_SHAPE = 'id int8, grp int, v int8, ts timestamptz, body text'
K_ROWS = 8192
K_OPS = ('SELECT grp, count(*), sum(v) FROM {t} GROUP BY grp ORDER BY grp', "SELECT date_trunc('hour', ts), count(*), sum(v) FROM {t} GROUP BY 1 ORDER BY 1",
         "SELECT count(*), sum(v) FROM {t} WHERE body LIKE 'w1%'", "SELECT count(*), sum(v) FROM {t} WHERE body ILIKE 'W2%7'",
         'SELECT grp, count(*) FROM {t} WHERE grp IN (1, 5, 9, 30) GROUP BY grp ORDER BY grp', "SELECT count(*) FROM {t} WHERE body = 'w42'")
K_HLL = 'SELECT sift.approx_count_distinct(body), count(DISTINCT body), sift.approx_count_distinct(grp), count(DISTINCT grp) FROM k_col'


def k_row(k: str) -> str:
    return f"{k}, {k} % 32, {k} % 100, timestamptz '2030-01-01' + {k} * interval '1 minute', 'w' || {k} % 300"


def hll_off(out: list | None) -> int:
    """1 when an estimate of a HyperLogLog sketch is off its exact count by more than 2% (and 2)"""
    if out is None:
        return 0
    a, b, c, d = (int(x) for x in out[0].split('|'))
    return int(abs(a - b) > max(2, b // 50) or abs(c - d) > max(2, d // 50))


def olap_preagg_skip_hll_churn(ctx):
    """a heap with a sift_col index of a pre-aggregate cube (grp: v), HyperLogLog sketches of grp and body and a Bloom skip index of body, and a heap twin: 3 writers insert
    and update both in one transaction and roll back batches of outliers, 1 vacuums and maintains, 4 readers at work_mem 256kB (a backend cache of 1 MB) compare GROUP BY,
    date_trunc, LIKE, ILIKE, IN and equality with the twin and the heap in one snapshot and the sketch estimates with exact counts: every answer agrees, no estimate sees an
    outlier or strays, no temporary file left, the memory of the readers after the load"""
    db = ctx.db
    db.run(f'CREATE TABLE k_col ({K_SHAPE}); CREATE TABLE k_tw ({K_SHAPE}); INSERT INTO k_col SELECT {k_row("g")} FROM generate_series(1, {K_ROWS}) g; '
           "INSERT INTO k_tw SELECT * FROM k_col; CREATE INDEX k_col_ix ON k_col USING sift_col (id, grp, v, ts, body) WITH (preagg = 'cube(grp: v)', "
           "sketches = 'hll(grp, body)', skip_index = 'body: bloom'); ANALYZE k_col, k_tw")
    ws = ctx.sessions(4) + ctx.sessions(4, f'{TIGHT}; {CACHE}')

    def write(w, i):
        w.ids, k = getattr(w, 'ids', set()), nxt(w)
        if i % 4 == 3:
            w.do('BEGIN; ' + '; '.join(f"INSERT INTO {t} SELECT {k} + g, 999, 10 ^ 9, now(), 'outlier' || g FROM generate_series(1, 64) g" for t in ('k_col', 'k_tw')) +
                 '; ROLLBACK')
            return
        sql = '; '.join(f'INSERT INTO {t} VALUES ({k_row(str(k))}); UPDATE {t} SET v = v + 1 WHERE id = {1 + (w.seed * 37 + i * 11) % K_ROWS}' for t in ('k_col', 'k_tw'))
        if w.do(f'BEGIN; {sql}; COMMIT') is not None:
            w.ids.add(k)

    def maint(w, i):
        w.do(('VACUUM k_col', "SELECT count(*) FROM sift.maintain('k_col_ix')")[i % 2])
        time.sleep(0.05)

    def read(w, i):
        q = K_OPS[(w.seed + i) % len(K_OPS)]
        if i % 5 == 4:
            w.ledger += hll_off(w.do(f'BEGIN ISOLATION LEVEL REPEATABLE READ; {K_HLL}; COMMIT'))
        else:
            w.ledger += snap_pair(w, q.format(t='k_col'), q.format(t='k_tw')) if i % 2 else snap_oracle(w, q.format(t='k_col'), 'sift.enable_agg')

    roles = {'w': (ws[:3], write), 'm': (ws[3:4], maint), 'r': (ws[4:], read)}
    ctx.phase(roles, 0.5)
    before = [ctx_kb(w) for w in ws[4:]], ctx.rss(ws[4:])
    busy = ctx.phase(roles, 2.5)
    after = [ctx_kb(w) for w in ws[4:]], ctx.rss(ws[4:])
    ctx.close_sessions()
    chk = ctx.sessions(1)[0]
    ids = set().union(*(getattr(w, 'ids', set()) for w in ws[:3]))
    mismatch = sum(w.ledger for w in ws[4:]) + sum(snap_pair(chk, q.format(t='k_col'), q.format(t='k_tw'), ()) for q in K_OPS) + hll_off(chk.run(K_HLL))
    return base(ctx, busy, 'r', **add(kept(chk, 'k_col', ids, 10 ** 6), kept(chk, 'k_tw', ids, 10 ** 6)), oracle_mismatch=mismatch, writes=busy['w'].ops,
                **mem_delta(ctx, before, after))


X_SHAPE = 'id int8 PRIMARY KEY, a int, c int, v int8'
X_ROW = "id || ':' || a || ':' || c || ':' || v"
X_MV = 'SELECT a, count(*) AS n, sum(v) AS s FROM x_tbl GROUP BY a'
X_BITMAP = 'SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off; SET LOCAL sift.enable_agg = off; SELECT count(*), sum(v) FROM {t} WHERE {p}'


def x_setup() -> str:
    tables = ''.join(f'CREATE TABLE {t} ({X_SHAPE}){am}; INSERT INTO {t} SELECT g, g % 50, g % 77, g FROM generate_series(1, 8192) g; CREATE INDEX {t}_a ON {t} (a); '
                     f'CREATE INDEX {t}_c ON {t} (c); ' for t, am in (('x_tbl', ' USING sift_table'), ('x_tw', '')))
    return (f'{tables}CREATE MATERIALIZED VIEW mv_s USING sift_table AS {X_MV}; CREATE MATERIALIZED VIEW mv_h AS {X_MV}; CREATE UNIQUE INDEX mv_s_a ON mv_s (a); '
            'CREATE UNIQUE INDEX mv_h_a ON mv_h (a); ANALYZE x_tbl, x_tw, mv_s, mv_h')


def ddl_matview_bitmap_bulk_mix(ctx):
    """a sift_table with two btree indexes and its heap twin, a materialized view of its rollup stored as a sift_table and the same view as a heap: 2 writers insert,
    update and delete batches of 64 rows of both in one transaction, 1 refreshes both views in one REPEATABLE READ transaction (CONCURRENTLY and not by turns), 1 vacuums, 3 readers
    compare the two views and the BitmapAnd and BitmapOr scans of the table with those of the twin in one snapshot: every answer agrees, the table and its twin hold the
    same rows, sift.check of the views (materialized views among the relations it checks) clean"""
    db = ctx.db
    db.run(x_setup())
    ws = ctx.sessions(7)

    def write(w, i):
        lo, own, a, c = nxt(w) * 64, w.seed * 64 * 10 ** 6, i % 50, (w.seed * 7 + i) % 77
        steps = (f'INSERT INTO {{t}} SELECT {lo} + g, g % 50, g % 77, g FROM generate_series(1, 64) g',
                 f'UPDATE {{t}} SET v = v + 1 WHERE a = {a} AND id <= 8192 AND id % 9 = {i % 9}',
                 f'DELETE FROM {{t}} WHERE c = {c} AND id >= {own} AND id < {own + 64 * 10 ** 6} AND id % 5 = {i % 5}')
        w.do('BEGIN; ' + '; '.join(s.format(t=t) for s in steps for t in ('x_tbl', 'x_tw')) + '; COMMIT')

    def refresh(w, i):
        how = ('CONCURRENTLY ', '')[i % 2]
        w.do(f'BEGIN ISOLATION LEVEL REPEATABLE READ; REFRESH MATERIALIZED VIEW {how}mv_s; REFRESH MATERIALIZED VIEW {how}mv_h; COMMIT')
        paced(w)

    def vac(w, i):
        w.do(('VACUUM x_tbl, x_tw', 'VACUUM mv_s, mv_h', "SELECT count(*) FROM sift.maintain('x_tbl')")[i % 3])
        time.sleep(0.05)

    def read(w, i):
        p = (f'a = {i % 50} AND c = {(w.seed + i) % 77}', f'a = {i % 50} OR c = {(w.seed + i) % 77}')[i % 2]
        w.ledger += (snap_pair(w, 'SELECT * FROM mv_s ORDER BY a', 'SELECT * FROM mv_h ORDER BY a'),
                     snap_pair(w, X_BITMAP.format(t='x_tbl', p=p), X_BITMAP.format(t='x_tw', p=p)))[(w.seed + i) % 2]

    busy = ctx.phase({'w': (ws[:2], write), 'f': (ws[2:3], refresh), 'v': (ws[3:4], vac), 'r': (ws[4:], read)}, 3.0)
    ctx.close_sessions()
    chk = ctx.sessions(1)[0]
    chk.run('REFRESH MATERIALIZED VIEW mv_s; REFRESH MATERIALIZED VIEW mv_h')
    mismatch = sum(w.ledger for w in ws[4:]) + digest_differs(chk, 'x_tbl', 'x_tw', X_ROW) + snap_pair(chk, 'SELECT * FROM mv_s ORDER BY a', 'SELECT * FROM mv_h ORDER BY a', ())
    rows = abs(int(chk.val('SELECT count(*) FROM x_tbl')) - int(chk.val('SELECT count(*) FROM x_tw')))
    return base(ctx, busy, 'w', lost_rows=rows, dup_rows=dups(chk, 'x_tbl'), oracle_mismatch=mismatch, refreshes=busy['f'].ops, reads=busy['r'].ops)


F_TABLES = 20
F_ROWS = 64
F_MODES = ("SELECT 'l', count(*) FROM (SELECT 1 FROM {t} WHERE id = {k} FOR KEY SHARE) s",
           "SELECT 'l', count(*) FROM (SELECT 1 FROM {t} WHERE id = {k} FOR SHARE) s",
           "WITH l AS (SELECT id FROM {t} WHERE id = {k} FOR NO KEY UPDATE), u AS (UPDATE {t} SET v = v + 1 WHERE id IN (SELECT id FROM l) RETURNING 1) "
           "SELECT 'u', count(*) FROM u",
           "WITH l AS (SELECT id FROM {t} WHERE id = {k} FOR UPDATE NOWAIT), u AS (UPDATE {t} SET v = v + 1 WHERE id IN (SELECT id FROM l) RETURNING 1) "
           "SELECT 'u', count(*) FROM u",
           "WITH l AS (SELECT id FROM {t} WHERE id = {k} FOR UPDATE SKIP LOCKED), u AS (UPDATE {t} SET v = v + 1 WHERE id IN (SELECT id FROM l) RETURNING 1) "
           "SELECT 'u', count(*) FROM u")
F_SUM = ' + '.join(f'(SELECT coalesce(sum(v), 0) FROM f_{j})' for j in range(F_TABLES))
F_COUNT = ' + '.join(f'(SELECT count(*) - count(DISTINCT id) FROM f_{j})' for j in range(F_TABLES))


def txn_rowlock_modes_fastpath(ctx):
    """20 sift_tables of 64 rows: 12 sessions each lock a row of every table in one transaction, in the order of the tables, by turns FOR KEY SHARE, FOR SHARE, FOR NO KEY
    UPDATE, FOR UPDATE NOWAIT and FOR UPDATE SKIP LOCKED, and increment the rows they hold for update (20 relation locks: past the 16 fast-path slots of a backend), beside
    1 session that vacuums the tables: every committed increment kept, no row twice, no deadlock (the locks are taken in one order), only 55P03 refused, no lock of a row
    turned into a lock of its relation"""
    db = ctx.db
    db.run('; '.join(f'CREATE TABLE f_{j} (id int8 PRIMARY KEY, v int8) USING sift_table; INSERT INTO f_{j} SELECT g, 0 FROM generate_series(1, {F_ROWS}) g'
                     for j in range(F_TABLES)))
    ws = ctx.sessions(13)

    def lock(w, i):
        stmts = [F_MODES[(w.seed + i + j) % len(F_MODES)].format(t=f'f_{j}', k=1 + (w.seed * 7 + i * 3 + j) % F_ROWS) for j in range(F_TABLES)]
        out = w.do('BEGIN; ' + '; '.join(stmts) + '; COMMIT', ('55P03',))
        w.ledger += sum(int(r.split('|')[1]) for r in out or [] if r.startswith('u|'))

    def vac(w, i):
        w.do(f'VACUUM f_{i % F_TABLES}')
        time.sleep(0.02)

    busy = ctx.phase({'l': (ws[:12], lock), 'v': (ws[12:], vac)}, 2.5)
    ctx.close_sessions()
    chk = ctx.sessions(1)[0]
    rows = int(chk.val(f'SELECT {F_SUM}'))
    return base(ctx, busy, 'l', lost_rows=abs(rows - sum(w.ledger for w in ws[:12])), dup_rows=int(chk.val(f'SELECT {F_COUNT}')), increments=rows,
                nowait_refused=busy['l'].expected)


HY_SHAPE = 'id int8 PRIMARY KEY, grp int, v int8, ts timestamptz, body text'
HY_ROW = "{k}, {k} % 32, {k} % 1000, timestamptz '2030-01-01' + {k} * interval '1 second', CASE WHEN {k} % 3 = 0 THEN 'cable' ELSE 'wire' END || ' t' || {k} % 97"
HY_ASOF = ('SELECT count(*), sum(x.px) FROM {t} f CROSS JOIN LATERAL (SELECT px FROM hy_dim d WHERE d.sym = f.grp % 8 AND d.ts <= f.ts ORDER BY d.ts DESC LIMIT 1) x '
           'WHERE f.id % 61 = {r}')
HY_TEXT = {'hy_tbl': "body @@> 'wire'", 'hy_col': "body LIKE 'wire %'"}
HY_GROUP = 'SELECT grp, count(*), sum(v) FROM {t} WHERE {p} GROUP BY grp ORDER BY grp'
HY_WHY = "SELECT count(*) >= 0 FROM sift.explain_why('SELECT grp, count(*) FROM hy_col WHERE ts > now() - interval ''1 day'' GROUP BY grp')"


def olap_hybrid_asof_explain_storm(ctx):
    """a sift_table with a BM25 index and its twin, a heap with a sift_col index, beside a time series of 8 symbols with a sift_col index and a btree of (sym, ts): 2 writers insert facts into both
    and ticks into the series, 1 analyzes them, 4 readers at work_mem 256kB compare a BM25-filtered GROUP BY with the plain filter of the twin, an as-of LATERAL join of
    each table with the series, and ask sift.explain_why, while 1 cancels them: every answer agrees, no temporary file left, the memory of the readers after the load"""
    db = ctx.db
    db.run(f'CREATE TABLE hy_tbl ({HY_SHAPE}) USING sift_table; CREATE TABLE hy_col ({HY_SHAPE}); '
           + ''.join(f'INSERT INTO {t} SELECT {HY_ROW.format(k="g")} FROM generate_series(1, 8192) g; ' for t in ('hy_tbl', 'hy_col')) +
           'CREATE INDEX hy_tbl_bm ON hy_tbl USING sift (body); CREATE INDEX hy_col_ix ON hy_col USING sift_col (id, grp, v, ts); '
           "CREATE TABLE hy_dim (id int8, sym int, ts timestamptz, px int8); INSERT INTO hy_dim SELECT g, g % 8, timestamptz '2030-01-01' + g * interval '7 second', g "
           'FROM generate_series(1, 2048) g; CREATE INDEX hy_dim_ix ON hy_dim USING sift_col (id, sym, ts, px); CREATE INDEX hy_dim_at ON hy_dim (sym, ts); '
           'ANALYZE hy_tbl, hy_col, hy_dim')
    ws = ctx.sessions(3) + ctx.sessions(4, f'{TIGHT}; {CACHE}') + ctx.sessions(1)

    def write(w, i):
        w.ids, k = getattr(w, 'ids', set()), nxt(w)
        d = 10 ** 7 + k
        tick = f"; INSERT INTO hy_dim VALUES ({d}, {d % 8}, timestamptz '2030-01-01' + {d} * interval '7 second', {d})" if i % 8 == 0 else ''
        sql = '; '.join(f'INSERT INTO {t} VALUES ({HY_ROW.format(k=k)})' for t in ('hy_tbl', 'hy_col'))
        if w.do(f'BEGIN; {sql}{tick}; COMMIT') is not None:
            w.ids.add(k)

    def analyze(w, i):
        w.do(('ANALYZE hy_tbl, hy_col', 'ANALYZE hy_dim')[i % 2])
        time.sleep(0.1)

    def read(w, i):
        n = (w.seed + i) % 4
        if n == 3:
            return w.do(HY_WHY, KILLED)
        q = (HY_GROUP.format(t='hy_tbl', p=HY_TEXT['hy_tbl']), HY_ASOF.format(t='hy_tbl', r=i % 61), HY_ASOF.format(t='hy_col', r=i % 61))[n]
        twin = (HY_GROUP.format(t='hy_col', p=HY_TEXT['hy_col']), HY_ASOF.format(t='hy_col', r=i % 61), HY_ASOF.format(t='hy_tbl', r=i % 61))[n]
        w.ledger += snap_pair(w, q, twin, KILLED) if i % 2 else snap_oracle(w, q, ('sift.enable_scan', 'sift.enable_agg', 'sift.enable_agg')[n], KILLED)

    roles = {'w': (ws[:2], write), 'a': (ws[2:3], analyze), 'r': (ws[3:7], read), 'c': (ws[7:], cancels(ws[3:7]))}
    ctx.phase(roles, 0.5)
    before = [ctx_kb(w) for w in ws[3:7]], ctx.rss(ws[3:7])
    busy = ctx.phase(roles, 2.5)
    after = [ctx_kb(w) for w in ws[3:7]], ctx.rss(ws[3:7])
    ctx.close_sessions()
    chk = ctx.sessions(1)[0]
    ids = set().union(*(getattr(w, 'ids', set()) for w in ws[:2]))
    mismatch = sum(w.ledger for w in ws[3:7]) + snap_pair(chk, HY_GROUP.format(t='hy_tbl', p=HY_TEXT['hy_tbl']), HY_GROUP.format(t='hy_col', p=HY_TEXT['hy_col']), ())
    mismatch += sum(snap_pair(chk, HY_ASOF.format(t='hy_tbl', r=r), HY_ASOF.format(t='hy_col', r=r), ()) for r in (0, 17))
    return base(ctx, busy, 'r', **add(kept(chk, 'hy_tbl', ids, 10 ** 6), kept(chk, 'hy_col', ids, 10 ** 6)), oracle_mismatch=mismatch, writes=busy['w'].ops,
                cancelled=busy['r'].expected, **mem_delta(ctx, before, after))


V_SHAPE = 'id int8 PRIMARY KEY, grp int, v int8'
V_ROW = "id || ':' || grp || ':' || v"
V_READ = 'SELECT grp, count(*), sum(v) FROM {t} WHERE grp < 16 GROUP BY grp ORDER BY grp'
V_IOS = 'SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL sift.enable_agg = off; '
V_SEQ = 'SET LOCAL enable_indexscan = off; SET LOCAL enable_indexonlyscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL sift.enable_agg = off; '
V_MAINT = ('VACUUM (FREEZE) v_tbl, v_tw', 'CHECKPOINT', 'VACUUM v_tbl, v_tw', "SELECT count(*) FROM sift.maintain('v_tbl')")


def repl_vm_ios_ckpt_standby(ctx):
    """a sift_table and its heap twin, each with a covering btree (grp) INCLUDE (v): 3 writers insert and update both in one transaction on the primary, 1 freezes,
    vacuums, maintains and checkpoints them, while 4 REPEATABLE READ sessions of the hot standby compare an index-only scan with a sequential scan of each in one snapshot (a
    query cancelled by a conflict with the recovery is no mismatch): every committed row kept, every answer of the standby agrees, and after a catch-up the standby holds
    what the primary does and its sift.check, quarantine and health are clean"""
    db, conn = ctx.db, standby()
    db.run(''.join(f'CREATE TABLE {t} ({V_SHAPE}){am}; INSERT INTO {t} SELECT g, g % 64, g FROM generate_series(1, 8192) g; CREATE INDEX {t}_cov ON {t} (grp) INCLUDE (v); '
                   for t, am in (('v_tbl', ' USING sift_table'), ('v_tw', ''))) + 'VACUUM (FREEZE) v_tbl, v_tw; ANALYZE v_tbl, v_tw')
    st = ctx.sessions(1, conn=conn)[0]
    lag = catchup(db, st)
    ws, rs = ctx.sessions(4), ctx.sessions(4, conn=conn)

    def write(w, i):
        w.ids, k = getattr(w, 'ids', set()), nxt(w)
        sql = '; '.join(f'INSERT INTO {t} VALUES ({k}, {k % 64}, {k % 1000}); UPDATE {t} SET v = v + 1 WHERE id = {1 + (w.seed * 101 + i * 37) % 8192}' for t in ('v_tbl', 'v_tw'))
        if w.do(f'BEGIN; {sql}; COMMIT') is not None:
            w.ids.add(k)

    def maint(w, i):
        w.do(V_MAINT[i % len(V_MAINT)])
        paced(w)

    def read(w, i):
        q = V_READ.format(t=('v_tbl', 'v_tw')[i % 2])
        w.ledger += halves_differ(w.do(snap(V_IOS + q, V_SEQ + q), ('40001',)))

    busy = ctx.phase({'w': (ws[:3], write), 'm': (ws[3:], maint), 'r': (rs, read)}, 2.5)
    lag += catchup(db, st)
    digest = '; '.join(ROW_DIGEST.format(row=V_ROW, t=t) for t in ('v_tbl', 'v_tw'))
    differ = int(db.run(digest) != st.run(digest)) + digest_differs(db, 'v_tbl', 'v_tw', V_ROW)
    ids = set().union(*(getattr(w, 'ids', set()) for w in ws[:3]))
    return base(ctx, busy, 'w', **add(kept(db, 'v_tbl', ids, 10 ** 6), kept(db, 'v_tw', ids, 10 ** 6), checks(st)), oracle_mismatch=differ,
                standby_mismatch=lag + sum(w.ledger for w in rs), standby_reads=busy['r'].ops, conflicts=busy['r'].expected)


STRESS = (
    LoadCase('conc_bm25_par_largedoc_cancel', 'conc', conc_bm25_par_largedoc_cancel, Limits(ctx_kb_delta=2048, rss_mb_delta=24, lwlock_wait_pct=35), open='S119-27',
             intermittent=True),
    LoadCase('olap_preagg_skip_hll_churn', 'olap', olap_preagg_skip_hll_churn, Limits(ctx_kb_delta=1280, rss_mb_delta=16)),
    LoadCase('ddl_matview_bitmap_bulk_mix', 'ddl', ddl_matview_bitmap_bulk_mix, Limits()),
    LoadCase('txn_rowlock_modes_fastpath', 'txn', txn_rowlock_modes_fastpath, Limits(coarse_escalations=0, lwlock_wait_pct=35), open='S119-28', intermittent=True),
    LoadCase('olap_hybrid_asof_explain_storm', 'olap', olap_hybrid_asof_explain_storm, Limits(ctx_kb_delta=1280, rss_mb_delta=16)),
)
REPL = (
    LoadCase('repl_vm_ios_ckpt_standby', 'repl', repl_vm_ios_ckpt_standby, Limits(), open='S119-29', intermittent=True),
)
