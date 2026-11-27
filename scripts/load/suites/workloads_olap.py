"""The analytic and feature workloads of the load gate: wide keys, grouping sets, distinct counts, windows, skewed outer joins and BM25 under a tight work_mem with cancels, projections
created and dropped under writes and maintenance, tiering and TTL under DML and a lost object, and eight sessions that each want four parallel workers of a pool of four
(scripts/load/suites, test/t/core/load_workloads.sh).
"""
from __future__ import annotations

import os
import sys
import time

sys.dont_write_bytecode = True

from cases import KILLED, PAR, LoadCase  # noqa: E402
from collect import AGG, BM, add, base, ctx_kb, engines, ledger_writes, mem_delta, multi_kept, oracle, oracles, tbl  # noqa: E402
from suites.common import Limits, fd_delta, fds, twin_do, twin_mismatch  # noqa: E402

WORKERS = "SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'parallel worker'"
WIDE = 'SELECT grp, body, count(*), sum(v) FROM e_col GROUP BY grp, body ORDER BY 1, 2'
TIGHT = (WIDE + ' LIMIT 5',
         'SELECT grp, v % 7, count(*), sum(v) FROM e_col GROUP BY GROUPING SETS ((grp), (v % 7), ()) ORDER BY 1, 2 LIMIT 5',
         'SELECT count(DISTINCT body), count(DISTINCT v) FROM e_col',
         'SELECT id, sum(v) OVER (PARTITION BY grp ORDER BY id ROWS 3 PRECEDING) FROM e_col ORDER BY id DESC LIMIT 5',
         'SELECT d.grp, count(*) FROM e_col e RIGHT JOIN dim d USING (grp) GROUP BY d.grp ORDER BY 1 LIMIT 5',
         'SELECT count(*), count(e.id), count(d.grp) FROM e_col e FULL JOIN dim d USING (grp)',
         "SELECT k, count(*) FROM e_col, jsonb_each_text(jsonb_build_object('g', grp, 'v', v)) AS j(k, x) GROUP BY k ORDER BY k",
         BM.format(t='e_bm'),
         "SELECT grp, count(*) FROM e_bm WHERE body @@> 'wire OR switch' GROUP BY grp ORDER BY 2 DESC, 1 LIMIT 5")


def cancels(victims: list):
    """a session that cancels the statement of one of VICTIMS every 30 ms (their backends read before the phase: a victim is busy in its own thread)"""
    pids = [v.pid for v in victims]

    def cancel(w, i):
        time.sleep(0.03)
        w.do(f'SELECT pg_cancel_backend({pids[i % len(pids)]})')
    return cancel


def olap_wide_tight_mem(ctx):
    """6 analytic sessions at work_mem 512kB (wide-key GROUP BY, GROUPING SETS, COUNT(DISTINCT), a window, RIGHT and FULL joins of a skewed dimension with NULL keys, JSONB
    unnest, BM25) are cancelled at random beside 2 writers of the three engines: the memory of their backends after the load, the temporary files left and the oracles"""
    db = ctx.db
    engines(db)
    db.run('CREATE TABLE dim (grp int, name text); '
           "INSERT INTO dim SELECT CASE WHEN g % 10 < 7 THEN 0 WHEN g % 10 = 7 THEN NULL ELSE g % 64 END, 'd' || g FROM generate_series(1, 2048) g; ANALYZE dim")
    ws = ctx.sessions(9, "SET work_mem = '512kB'")

    def olap(w, i):
        w.do(TIGHT[(w.seed + i) % len(TIGHT)], KILLED)

    ctx.phase({'r': (ws[:6], olap)}, 0.4)
    before = [ctx_kb(w) for w in ws[:6]], ctx.rss(ws[:6])
    busy = ctx.phase({'w': (ws[6:8], ledger_writes('e')), 'r': (ws[:6], olap), 'c': (ws[8:], cancels(ws[:6]))}, 1.8)
    after = [ctx_kb(w) for w in ws[:6]], ctx.rss(ws[:6])
    return base(ctx, busy, 'w', **add(multi_kept(db, 'e', ws[6:8]), {'oracle_mismatch': oracles(db) + oracle(db, WIDE, 'sift.enable_agg')}),
                cancelled=busy['r'].expected, olap=busy['r'].ops, **mem_delta(ctx, before, after))


def feat_proj_scope_live(ctx):
    """4 writers insert, update and delete rows of a sift_table and of its heap twin in one transaction while 1 session creates and drops ORDER BY and GROUP BY projections of
    it and 1 runs VACUUM and sift.maintain; 2 readers compare the table with the twin in one snapshot: the projections never change an answer"""
    db = ctx.db
    db.run(f'{tbl("p_tbl")}; CREATE TABLE p_tw AS SELECT * FROM p_tbl; VACUUM p_tbl')
    ws = ctx.sessions(8)

    def proj(w, i):
        spec = ('ORDER BY v', 'GROUP BY grp MEASURES count(*), sum(v)')[i // 2 % 2]
        w.do(f"SELECT sift.create_projection('p_tbl', 'p{i}', '{spec}')" if i % 2 == 0 else f"SELECT sift.drop_projection('p_tbl', 'p{i - 1}')")

    def maint(w, i):
        w.do("VACUUM p_tbl; SELECT count(*) FROM sift.maintain('p_tbl')")

    def read(w, i):
        w.ledger += twin_do(w, 'p_tbl', 'p_tw')

    busy = ctx.phase({'w': (ws[:4], ledger_writes('p', ('tbl', 'tw'))), 'p': (ws[4:5], proj), 'm': (ws[5:6], maint), 'r': (ws[6:], read)}, 1.8)
    return base(ctx, busy, 'w', **add(multi_kept(db, 'p', ws[:4], ('tbl', 'tw')), {'oracle_mismatch': sum(w.ledger for w in ws[6:]) + twin_mismatch(db, 'p_tbl', 'p_tw')}),
                projections=busy['p'].ops, maintained=busy['m'].ops)


def tiered(t: str, uri: str, rows: int) -> str:
    """a sift_table T of ROWS old rows and ROWS recent ones, its old part moved to the store at URI by a VACUUM; its heap twin T_tw"""
    return (f'CREATE TABLE {t} (id int8 PRIMARY KEY, ts timestamp NOT NULL, grp int, v int8, pad text) USING sift_table; '
            f"INSERT INTO {t} SELECT g, timestamp '2020-01-01' + g * interval '1 minute', g % 64, g % 1000, md5(g::text) FROM generate_series(1, {rows}) g; VACUUM (FREEZE) {t}; "
            f"INSERT INTO {t} SELECT g, localtimestamp - (g - {rows}) * interval '1 second', g % 64, g % 1000, md5(g::text) FROM generate_series({rows + 1}, {2 * rows}) g; "
            f"VACUUM (FREEZE) {t}; SELECT sift.set_tier_policy('{t}', '{uri}', '30 days'); VACUUM {t}; CREATE TABLE {t}_tw AS SELECT * FROM {t}")


def feat_tier_ttl_fault(ctx):
    """3 writers update and delete rows of a tiered sift_table (in the parts in the store and in the local ones) and of its heap twin, 2 readers compare them in one snapshot,
    1 session vacuums, rebinds the policy and the TTL and tiers, truncates and drops tables of its own, while another tiered table has lost its object: the twin agrees,
    sift.check sees the lost object (fault_unseen), no backend keeps objects of the store open (fd_delta)"""
    db = ctx.db
    store = ctx.store('load_tier')
    uri = 'file://' + store
    db.run(f'{tiered("tv", uri, 4096)}; {tiered("tl", uri, 1024)}')
    moved = int(db.val("SELECT count(*) FROM sift.tier_status WHERE relation = 'tv'::regclass"))
    gone = [l[7:] for l in db.run("SELECT location FROM sift.tier_status WHERE relation = 'tl'::regclass")]
    for path in gone:
        os.unlink(path)
    ws = ctx.sessions(6)

    def write(w, i):
        k = 1 + (w.seed * 2741 + i * 37) % 8192
        op = f'DELETE FROM {{t}} WHERE id = {k}' if i % 5 == 4 else f'UPDATE {{t}} SET v = v + 1 WHERE id = {k}'
        w.do(f'BEGIN; {op.format(t="tv")}; {op.format(t="tv_tw")}; COMMIT')

    def read(w, i):
        w.ledger += twin_do(w, 'tv', 'tv_tw')

    def maint(w, i):
        t = f'tx{i}'
        w.do((f"VACUUM tv; SELECT sift.set_tier_policy('tv', '{uri}', '{30 + i % 2 * 10} days')",
              f"SELECT sift.set_ttl('tv', 'ts', interval '{3000 + i % 2 * 1000} days')",
              f'{tiered(t, uri, 600)}; TRUNCATE {t}; DROP TABLE {t}, {t}_tw',
              f'{tiered(t, uri, 600)}; DROP TABLE {t}, {t}_tw')[i % 4])

    f0 = [fds(w.pid, store) for w in ws]
    busy = ctx.phase({'w': (ws[:3], write), 'r': (ws[3:5], read), 'm': (ws[5:], maint)}, 1.8)
    f1 = [fds(w.pid, store) for w in ws]
    unseen = int(int(db.val("SELECT count(*) FROM sift.check('tl', true)")) == 0)
    return base(ctx, busy, 'w', oracle_mismatch=sum(w.ledger for w in ws[3:5]) + twin_mismatch(db, 'tv', 'tv_tw'), fault_unseen=unseen if gone else 1,
                fd_delta=fd_delta(f0, f1), tiered=moved, maint=busy['m'].ops, damaged=(db.val("SELECT 'tl'::regclass::oid"),))


def mix_par_pool_saturation(ctx):
    """8 sessions that each want 4 parallel workers (a pool of 4) run parallel SiftAgg, sift_table and BM25 scans beside VACUUM (PARALLEL 2) and cancels: the oracles, the
    parallel workers running at once at their peak (at most the pool)"""
    db = ctx.db
    engines(db)
    ws = ctx.sessions(10, PAR.replace('= 2', '= 4'))
    qs = (AGG.format(t='e_col'), 'SELECT grp, count(*) FROM e_tbl GROUP BY grp ORDER BY grp', BM.format(t='e_bm'), 'SELECT count(*), sum(v) FROM e_tbl WHERE v > 10')
    def par(w, i):
        w.do(qs[(w.seed + i) % len(qs)], ('57014',))

    def vac(w, i):
        w.do('VACUUM (PARALLEL 2) e_col, e_bm')

    busy = ctx.phase({'r': (ws[:8], par), 'v': (ws[8:9], vac), 'c': (ws[9:], cancels(ws[:8]))}, 1.8, peak=WORKERS)
    return base(ctx, busy, 'r', oracle_mismatch=oracles(db), cancelled=busy['r'].expected, vacuums=busy['v'].ops, par_workers_peak=ctx.obs.peak)


CASES = (
    LoadCase('olap_wide_tight_mem', 'olap', olap_wide_tight_mem, Limits(ctx_kb_delta=1280, rss_mb_delta=16)),
    LoadCase('feat_proj_scope_live', 'feat', feat_proj_scope_live, Limits(), formats=(2,)),
    LoadCase('feat_tier_ttl_fault', 'feat', feat_tier_ttl_fault, Limits(fd_delta=0), formats=(2,)),
    LoadCase('mix_par_pool_saturation', 'mix', mix_par_pool_saturation, Limits(par_workers_peak=4)),
)
