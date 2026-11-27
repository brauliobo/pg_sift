"""The second wave of production shapes of the load gate (Spec 114): COPY streams into the three engines beside a racing unique insert and COPY FREEZE, wide out-of-line values
and shredded JSONB under churn at a tight work_mem, tenants under row-level security reading through views while their tables change shape, a storm of every BM25 query form
under churn, a parallel star join under timeouts and cancels at a tight work_mem, and a standby promoted while it reads tiered parts and the primary writes to a subscriber
(scripts/load/suites: WORKLOADS on test/t/core/load_workloads.sh, REPL on test/t/core/load_repl.sh).
"""
from __future__ import annotations

import sys
import time

sys.dont_write_bytecode = True

from cases import KILLED, PAR, LoadCase  # noqa: E402
from collect import BM, N, add, base, checks, ctx_kb, engines, halves_differ, hygiene, kept, mem_delta, multi_kept, nxt, oracle, oracles, p99_ratio, snap, zipf  # noqa: E402
from suites.common import Limits, fd_delta, fds, twin_do, twin_mismatch  # noqa: E402
from suites.repl import CAP, CDC, SHAPE, catchup, conninfo, standby  # noqa: E402
from suites.workloads_olap import WORKERS, cancels, tiered  # noqa: E402

TIGHT = "SET work_mem = '512kB'"


def snap_pair(w, a: str, b: str, allow: tuple = ()) -> int:
    """1 when the statements A and B answer differently in one REPEATABLE READ snapshot of worker W, 0 when they agree or the snapshot failed with an error of ALLOW"""
    return halves_differ(w.do(snap(a, b), allow))


def snap_oracle(w, sql: str, guc: str, allow: tuple = ()) -> int:
    """oracle() in a worker: SQL with the index path GUC on and off in one snapshot"""
    return snap_pair(w, f'SET LOCAL {guc} = on; {sql}', f'SET LOCAL {guc} = off; {sql}', allow)


BATCH = 96


def copy_in(t: str, ids, opts: str = '') -> str:
    """a COPY FROM STDIN of the rows IDS into T (the shape of load_base_fact), its data inline"""
    rows = '\n'.join(f'{k}\t{k % 64}\t{k % 1000}\twire tail{k % 97}' for k in ids)
    return f'COPY {t} FROM STDIN{opts};\n{rows}\n\\.\n'


def frozen(t: str, ids) -> str:
    """a sift_table T and a heap T_h with a sift_col index, created and filled by COPY FREEZE in one transaction"""
    return (f'BEGIN;\nCREATE TABLE {t} (id int8 PRIMARY KEY, grp int, v int8, body text) USING sift_table; CREATE TABLE {t}_h (id int8, grp int, v int8, body text); '
            f'CREATE INDEX {t}_ix ON {t}_h USING sift_col (id, grp, v);\n{copy_in(t, ids, " (FREEZE)")}{copy_in(t + "_h", ids, " (FREEZE)")}COMMIT')


def mix_copy_stream_ingest(ctx):
    """3 sessions stream COPY batches of 96 rows into a sift_table, a sift_col index and a BM25 index in one transaction (and now and then into fresh tables by COPY FREEZE),
    1 updates and deletes the old rows of all three, 1 COPYs into a keyed sift_table while 1 inserts keys of the batch in flight, 1 vacuums and maintains, 2 compare the
    sift_table with the heap in one snapshot: every committed row kept once, no key twice, the index paths answer what the heap does, the memory of the COPY sessions
    after the stream"""
    db = ctx.db
    engines(db, 'c')
    db.run('CREATE TABLE c_uq (id int8 PRIMARY KEY, grp int, v int8, body text) USING sift_table')
    ws = ctx.sessions(9)
    for w in ws[:3]:
        w.ids, w.frozen = set(), []
    uq, racer = ws[4], ws[5]
    uq.cur, uq.acked, racer.acked = 9 * 10 ** 7, set(), set()

    def stream(w, i):
        n = nxt(w) % 10 ** 6
        ids = range(w.seed * 10 ** 7 + n * BATCH, w.seed * 10 ** 7 + (n + 1) * BATCH)
        if n % 4 == 3 and len(w.frozen) < 3:
            t = f'cf_{w.seed}_{n}'
            if w.do(frozen(t, ids)) is not None:
                w.frozen.append(t)
        elif w.do(f"BEGIN;\n{''.join(copy_in(f'c_{e}', ids) for e in ('tbl', 'col', 'bm'))}COMMIT") is not None:
            w.ids |= set(ids)
        time.sleep(0.03)

    def churn(w, i):
        k = 1 + (w.seed * 2741 + i * 37) % N
        op = f'DELETE FROM {{t}} WHERE id = {k}' if i % 5 == 4 else f'UPDATE {{t}} SET v = v + 1 WHERE id = {k}'
        w.do(f"BEGIN; {'; '.join(op.format(t=f'c_{e}') for e in ('tbl', 'col', 'bm'))}; COMMIT")

    def copier(w, i):
        w.cur = 9 * 10 ** 7 + nxt(w) % 10 ** 6 * BATCH
        ids = range(w.cur, w.cur + BATCH)
        if w.do(f'BEGIN;\n{copy_in("c_uq", ids)}SELECT pg_sleep(0.01);\nCOMMIT', ('23505',)) is not None:
            w.acked |= set(ids)

    def race(w, i):
        k = uq.cur + (i * 13) % BATCH
        if w.do(f"INSERT INTO c_uq VALUES ({k}, 0, 0, 'wire')", ('23505',)) is not None:
            w.acked.add(k)

    def maint(w, i):
        w.do(('VACUUM c_tbl, c_col, c_bm', "SELECT count(*) FROM sift.maintain('c_tbl')", "SELECT count(*) FROM sift.maintain('c_col_ix')")[i % 3])
        time.sleep(0.05)

    def read(w, i):
        w.ledger += twin_do(w, 'c_tbl', 'c_col') if i % 2 else snap_oracle(w, BM.format(t='c_bm'), 'sift.enable_scan')

    roles = {'s': (ws[:3], stream), 'u': (ws[3:4], churn), 'q': ([uq], copier), 'x': ([racer], race), 'm': (ws[6:7], maint), 'r': (ws[7:], read)}
    ctx.phase(roles, 0.6)
    before = [ctx_kb(w) for w in ws[:3]], ctx.rss(ws[:3])
    busy = ctx.phase(roles, 1.6)
    after = [ctx_kb(w) for w in ws[:3]], ctx.rss(ws[:3])
    tabs = [t for w in ws[:3] for t in w.frozen]
    froze = sum(int(db.val(f'SELECT count(DISTINCT id) FROM {t}')) + int(db.val(f'SELECT count(*) FROM {t}_h')) for t in tabs)
    twin = twin_mismatch(db, 'c_tbl', 'c_col') + oracles(db, 'c') + sum(oracle(db, f'SELECT count(*), sum(v) FROM {t}_h', 'sift.enable_agg') for t in tabs)
    return base(ctx, busy, 's', **add(multi_kept(db, 'c', ws[:3]), kept(db, 'c_uq', uq.acked | racer.acked, 0), {'lost_rows': abs(froze - 2 * BATCH * len(tabs))},
                                      {'oracle_mismatch': twin + sum(w.ledger for w in ws[7:])}),
                copies=busy['s'].ops, frozen=len(tabs), conflicts=busy['q'].expected + busy['x'].expected, **mem_delta(ctx, before, after))


WROWS = 128
DOC = "jsonb_build_object('k', 'k' || {g} % 8, 'n', ({g} + {i}) % 100, 'tags', jsonb_build_array({g} % 5, ({g} + {i}) % 7))"
WIDE_PAIRS = (('SELECT grp % 8, count(*), sum(v), sum(length(blob)) FROM w_tbl GROUP BY 1 ORDER BY 1', 'SELECT grp % 8, count(*), sum(v), sum(blen) FROM w_tw GROUP BY 1 ORDER BY 1'),
              ('SELECT id, length(blob), md5(blob) FROM w_tbl WHERE id % 16 = {r} ORDER BY id', 'SELECT id, blen, bmd5 FROM w_tw WHERE id % 16 = {r} ORDER BY id'),
              ("SELECT doc->>'k', count(*), sum((doc->>'n')::int) FROM w_tbl GROUP BY 1 ORDER BY 1", "SELECT doc->>'k', count(*), sum((doc->>'n')::int) FROM w_tw GROUP BY 1 ORDER BY 1"),
              ("SELECT t, count(*) FROM w_tbl, jsonb_array_elements_text(doc->'tags') t GROUP BY t ORDER BY t",
               "SELECT t, count(*) FROM w_tw, jsonb_array_elements_text(doc->'tags') t GROUP BY t ORDER BY t"))
WIDE_COL = ("SELECT doc->>'k', count(*), sum(v) FROM w_col GROUP BY 1 ORDER BY 1", 'SELECT grp, count(*), sum(v), max(id) FROM w_col GROUP BY grp ORDER BY grp',
            "SELECT count(*), sum(v) FROM w_col WHERE doc->>'k' = 'k3'")


def blob(g: str, i: str) -> str:
    """the SQL of a 64-96 kB text of hex for row G, version I"""
    return f"(SELECT string_agg(md5(({g} * 4096 + {i} * 7 + j)::text), '') FROM generate_series(1, 2048 + ({g} * 7 + {i}) % 1024) j)"


def wide_row(k: int, i: int) -> str:
    """row K, version I, of the sift_table w_tbl, the heap w_col with its sift_col index and their twin w_tw, which keeps the length and md5 of each value"""
    b = blob(str(k), str(i))
    return (f"INSERT INTO w_tbl VALUES ({k}, {k % 64}, {i}, {DOC.format(g=k, i=i)}, {b}); INSERT INTO w_col SELECT * FROM w_tbl WHERE id = {k}; "
            f'INSERT INTO w_tw SELECT id, grp, v, doc, length(blob), md5(blob) FROM w_tbl WHERE id = {k}')


def olap_wide_toast_jsonb_mix(ctx):
    """a sift_table and a heap with a sift_col index of 128 rows of 64-96 kB values, a JSONB document and its array: 2 writers rewrite the values, the documents, or delete and
    insert a row again, 5 readers at work_mem 512kB compare the lengths, hashes, keys and array elements with a heap twin in one snapshot and the sift_col paths with the heap,
    cancelled at random: every answer agrees, the memory of the readers after the load, and once the load is gone a VACUUM, the maintenance and a VACUUM after them
    (which reclaims what the first freed) leave no block pending"""
    db = ctx.db
    db.run('CREATE TABLE w_tbl (id int8 PRIMARY KEY, grp int, v int8, doc jsonb, blob text) USING sift_table; CREATE TABLE w_col (id int8, grp int, v int8, doc jsonb, blob text); '
           'CREATE TABLE w_tw (id int8, grp int, v int8, doc jsonb, blen int, bmd5 text); '
           f"INSERT INTO w_tbl SELECT g, g % 64, 0, {DOC.format(g='g', i=0)}, {blob('g', '0')} FROM generate_series(1, {WROWS}) g; "
           'INSERT INTO w_col SELECT * FROM w_tbl; INSERT INTO w_tw SELECT id, grp, v, doc, length(blob), md5(blob) FROM w_tbl; '
           'CREATE INDEX w_col_ix ON w_col USING sift_col (id, grp, v, doc); ANALYZE w_col, w_tw')
    ws = ctx.sessions(8, TIGHT)

    def write(w, i):
        k, j = 1 + w.seed % 2 + 2 * (i * 31 % (WROWS // 2)), w.seed * 1000 + i
        drop = '; '.join(f'DELETE FROM {t} WHERE id = {k}' for t in ('w_tbl', 'w_col', 'w_tw'))
        if i % 4 == 3:
            w.do(f'BEGIN; {drop}; {wide_row(k, j)}; COMMIT')
        else:
            sets = f"v = v + 1, doc = {DOC.format(g=k, i=j)}" + (f', blob = {blob(str(k), str(j))}' if i % 2 else '')
            w.do(f'BEGIN; UPDATE w_tbl SET {sets} WHERE id = {k}; UPDATE w_col c SET v = t.v, doc = t.doc, blob = t.blob FROM w_tbl t WHERE c.id = {k} AND t.id = {k}; '
                 f'UPDATE w_tw c SET v = t.v, doc = t.doc, blen = length(t.blob), bmd5 = md5(t.blob) FROM w_tbl t WHERE c.id = {k} AND t.id = {k}; COMMIT')

    def read(w, i):
        a, b = WIDE_PAIRS[(w.seed + i) % len(WIDE_PAIRS)]
        w.ledger += snap_oracle(w, WIDE_COL[i % len(WIDE_COL)], 'sift.enable_agg', KILLED) if i % 3 == 2 else snap_pair(w, a.format(r=i % 16), b.format(r=i % 16), KILLED)

    ctx.phase({'w': (ws[5:7], write), 'r': (ws[:5], read)}, 0.8)
    before = [ctx_kb(w) for w in ws[:5]], ctx.rss(ws[:5])
    busy = ctx.phase({'w': (ws[5:7], write), 'r': (ws[:5], read), 'c': (ws[7:], cancels(ws[:5]))}, 1.8)
    after = [ctx_kb(w) for w in ws[:5]], ctx.rss(ws[:5])
    ctx.close_sessions()
    twin = sum(halves_differ(db.run(snap(a.format(r=r), b.format(r=r)))) for a, b in WIDE_PAIRS for r in (0, 7))
    twin += sum(oracle(db, q, 'sift.enable_agg') for q in WIDE_COL)
    db.run("VACUUM w_tbl, w_col; SELECT busy FROM sift.maintain('w_tbl'); SELECT busy FROM sift.maintain('w_col_ix'); VACUUM w_tbl, w_col")
    pending = int(db.val("SELECT (SELECT pending_blocks FROM sift.space('w_tbl')) + (SELECT pending_blocks FROM sift.space('w_col_ix'))"))
    rows = abs(int(db.val('SELECT count(*) FROM w_tbl')) - WROWS) + abs(int(db.val('SELECT count(*) FROM w_col')) - WROWS)
    return base(ctx, busy, 'w', oracle_mismatch=twin + sum(w.ledger for w in ws[:5]), lost_rows=rows, pending_after=pending, cancelled=busy['r'].expected,
                **mem_delta(ctx, before, after))


TENANTS = {'load_ta': 0, 'load_tb': 1}
POLICY = "tenant = CASE current_user WHEN 'load_ta' THEN 0 WHEN 'load_tb' THEN 1 END"
RLS_READS = ("SELECT tenant, count(*), sum(v) FROM rt_v WHERE body @@> 'wire' GROUP BY tenant", 'SELECT tenant, count(*), sum(v) FROM rt_doc GROUP BY tenant',
             "SELECT tenant, id FROM rt_doc WHERE body @@> 'tail1* OR switch' ORDER BY sift.score(ctid) DESC, id LIMIT 5", 'SELECT tenant, count(*), sum(v) FROM rt_tv GROUP BY tenant',
             'SELECT tenant, grp, sum(v) FROM rt_doc WHERE grp < 4 GROUP BY 1, 2 ORDER BY 1, 2')
RLS_ORACLE = (("SELECT tenant, count(*), sum(id) FROM rt_doc WHERE body @@> 'wire' GROUP BY tenant", 'sift.enable_scan'),
              ('SELECT tenant, grp, count(*), sum(v) FROM rt_doc GROUP BY 1, 2 ORDER BY 1, 2', 'sift.enable_agg'))
DUTY = 4
ALTER = ('ALTER TABLE {t} ADD COLUMN c{n} int', 'ALTER TABLE {t} RENAME COLUMN c{n} TO d{n}', 'ALTER TABLE {t} DROP COLUMN d{n}')


def tenant_rls_view_col_ddl(ctx):
    """two login roles read a heap with a BM25 and a sift_col index and a sift_table under forced row-level security (a third tenant hidden from both), through SECURITY INVOKER
    views and directly, one with sift.rls_scores on: ranked, counted and grouped answers, and update their own rows; 1 session updates the rows of every tenant, 1 adds, renames
    and drops columns of both tables, idle DUTY times as long as its last statement took (a share of the wall clock on any host): no tenant sees a row of another, the index
    paths answer what the heap does, every update kept"""
    db = ctx.db
    db.run('DROP ROLE IF EXISTS load_ta, load_tb; CREATE ROLE load_ta LOGIN; CREATE ROLE load_tb LOGIN; GRANT pg_read_all_stats TO load_ta, load_tb; ALTER ROLE load_tb SET sift.rls_scores = on; '
           'CREATE TABLE rt_doc (id int8 PRIMARY KEY, tenant int, grp int, v int8, body text); INSERT INTO rt_doc SELECT id, id % 3, grp, v, body FROM load_base_fact; '
           'CREATE INDEX rt_bm ON rt_doc USING sift (body); CREATE INDEX rt_col ON rt_doc USING sift_col (tenant, grp, v); '
           'CREATE TABLE rt_tbl (id int8 PRIMARY KEY, tenant int, grp int, v int8) USING sift_table; INSERT INTO rt_tbl SELECT id, id % 3, grp, v FROM load_base_fact; '
           'ANALYZE rt_doc, rt_tbl')
    try:
        for t in ('rt_doc', 'rt_tbl'):
            db.run(f'ALTER TABLE {t} ENABLE ROW LEVEL SECURITY; ALTER TABLE {t} FORCE ROW LEVEL SECURITY; CREATE POLICY own ON {t} USING ({POLICY})')
        db.run('CREATE VIEW rt_v WITH (security_invoker = true) AS SELECT id, tenant, grp, v, body FROM rt_doc; '
               'CREATE VIEW rt_tv WITH (security_invoker = true) AS SELECT id, tenant, grp, v FROM rt_tbl; '
               'GRANT USAGE ON SCHEMA load_case TO load_ta, load_tb; GRANT SELECT, UPDATE ON rt_doc, rt_tbl TO load_ta, load_tb; GRANT SELECT ON rt_v, rt_tv TO load_ta, load_tb')
        v0 = {int(t): int(s) for t, s in (r.split('|') for r in db.run('SELECT tenant, sum(v) FROM rt_doc GROUP BY tenant'))}
        ts = [w for role in TENANTS for w in ctx.sessions(2, conn={'PGUSER': role})]
        for w in ts:
            w.tenant, w.ups = TENANTS[w.conn['PGUSER']], 0
        admin, ddl_s = ctx.sessions(2)
        admin.ups = {0: 0, 1: 0, 2: 0}

        def tenant(w, i):
            w.pid
            if i % 4 == 3:
                k = 3 * ((w.seed * 977 + i * 31) % (N // 3)) + w.tenant
                out = w.do(f'BEGIN; UPDATE rt_doc SET v = v + 1 WHERE id = {k} RETURNING 1; UPDATE rt_tbl SET v = v + 1 WHERE id = {k} RETURNING 1; COMMIT')
                w.ups += out == ['1', '1']
            elif i % 4 == 2:
                q, guc = RLS_ORACLE[i // 4 % 2]
                w.ledger += snap_oracle(w, q, guc, ())
            else:
                out = w.do(RLS_READS[(w.seed + i) % len(RLS_READS)]) or []
                w.ledger += sum(r.split('|')[0] != str(w.tenant) for r in out)

        def write(w, i):
            k = 1 + (i * 2741) % N
            if w.do(f'BEGIN; UPDATE rt_doc SET v = v + 1 WHERE id = {k}; UPDATE rt_tbl SET v = v + 1 WHERE id = {k}; COMMIT') is not None:
                w.ups[k % 3] += 1

        def ddl(w, i):
            w.do('; '.join(ALTER[i % 3].format(t=t, n=i // 3) for t in ('rt_doc', 'rt_tbl')))
            time.sleep(DUTY * max(w.lat[-1:] or [12.5]) / 1000)

        ctx.phase({'t': (ts, tenant)}, 0.4)
        before = [ctx_kb(w) for w in ts], ctx.rss(ts)
        busy = ctx.phase({'t': (ts, tenant), 'w': ([admin], write), 'd': ([ddl_s], ddl)}, 1.8)
        after = [ctx_kb(w) for w in ts], ctx.rss(ts)
        want = {t: v0[t] + admin.ups[t] + sum(w.ups for w in ts if w.tenant == t) for t in v0}
        lost = sum(abs(int(s) - want[int(t)]) for q in ('rt_doc', 'rt_tbl') for t, s in (r.split('|') for r in db.run(f'SELECT tenant, sum(v) FROM {q} GROUP BY tenant')))
        mismatch = sum(w.ledger for w in ts) + sum(oracle(db, q, guc) for q, guc in RLS_ORACLE)
        return base(ctx, busy, 't', lost_rows=lost, oracle_mismatch=mismatch, ddl=busy['d'].ops, updates=sum(w.ups for w in ts), **mem_delta(ctx, before, after))
    finally:
        ctx.close_sessions()
        db.run('DROP OWNED BY load_ta, load_tb; DROP ROLE load_ta, load_tb')


def storm_counts(t: str = 'q_doc') -> tuple:
    """the counted forms of the BM25 query language on T: booleans, NOT, a phrase, a prefix, a fuzzy word, a set, INCLUDE filters, sections, a grouping over a sift_col
    index and the facets"""
    qs = ("'wire OR tail1*'", "'wire NOT switch'", "'\"wire switch\"'", "'tail1*'", "'swich~1'", "'{switch tail7}'", "'(wire OR switch) NOT tail2*'")
    return tuple(f'SELECT count(*), sum(id) FROM {t} WHERE body @@> {q}' for q in qs) + (
        f"SELECT count(*), sum(id) FROM {t} WHERE body @@> 'wire' AND cat = 3 AND pub >= date '2024-01-20'",
        f"SELECT count(*), sum(id) FROM {t} WHERE sift.sections(title, body) @@> 'title:alpha body:wire'",
        f"SELECT grp, count(*), sum(v) FROM {t} WHERE body @@> 'wire' GROUP BY grp ORDER BY grp",
        f"""SELECT facet, value, count FROM sift.facet_counts('{t}', 'body', 'wire', '[{{"expr":"cat","size":3}}]') ORDER BY 1, 2""")


STORM = storm_counts()
RANKED = ("SELECT id FROM q_doc WHERE body @@> 'wire OR tail1*' ORDER BY sift.score(ctid) DESC LIMIT 10",
          "SELECT id FROM q_doc WHERE body @@> 'switch' AND cat = 2 ORDER BY sift.score(ctid) DESC LIMIT 5",
          "SELECT id FROM q_doc WHERE sift.sections(title, body) @@> sift.field_query('alpha wire', 'title^3, body') ORDER BY sift.score(ctid) DESC LIMIT 5")
QDOC = "{k}, {k} % 64, {k} % 7, date '2024-01-01' + ({k} % 60)::int, {k} % 1000, '{title} t' || {k} % 13, {body}"


def conc_bm25_query_storm_churn(ctx):
    """6 sessions run every form of the BM25 query language (ranked top-k, booleans and NOT, phrases, prefixes, fuzzy words, sets, INCLUDE filters, sections and weighted
    fields, the facets and a grouping over a sift_col index) alone and then beside 2 writers that rewrite texts and titles, delete documents and add new ones and 1 session
    that vacuums and maintains the indexes: every counted form answers in one snapshot what the heap does, every document added kept, the p99 of the storm under the churn"""
    db = ctx.db
    db.run('CREATE TABLE q_doc (id int8 PRIMARY KEY, grp int, cat int, pub date, v int8, title text, body text); '
           f"INSERT INTO q_doc SELECT {QDOC.format(k='id', title='alpha', body='body')} FROM load_base_fact; "
           'CREATE INDEX q_bm ON q_doc USING sift (body) INCLUDE (cat, pub); '
           "CREATE INDEX q_sec ON q_doc USING sift (sift.sections(title, body)) WITH (fields = 'title,body'); "
           'CREATE INDEX q_col ON q_doc USING sift_col (id, grp, cat, v); ANALYZE q_doc')
    ws = ctx.sessions(9)

    def storm(w, i):
        if i % 4 == 3:
            w.ledger += snap_oracle(w, STORM[(w.seed + i) % len(STORM)], 'sift.enable_scan', ())
        else:
            w.do((STORM + RANKED)[(w.seed * 5 + i) % (len(STORM) + len(RANKED))])

    def write(w, i):
        w.ids = getattr(w, 'ids', set())
        k = zipf(w.seed * 7919 + i, N, 1.1)
        if i % 3 == 2:
            n   = nxt(w)
            row = QDOC.format(k=n, title='beta', body=f"'wire switch tail{n % 97}'")
            if w.do(f"BEGIN; DELETE FROM q_doc WHERE id = {k}; INSERT INTO q_doc VALUES ({row}); COMMIT") is not None:
                w.ids.add(n)
        else:
            w.do(f"UPDATE q_doc SET v = v + 1, title = 'gamma t{i % 13}', body = 'switch tail{i % 97}' || CASE WHEN {i} % 2 = 0 THEN ' wire' ELSE '' END WHERE id = {k}")

    def maint(w, i):
        w.do(('VACUUM q_doc', "SELECT count(*) FROM sift.maintain('q_bm')", "SELECT count(*) FROM sift.maintain('q_sec')")[i % 3])
        time.sleep(0.1)

    quiet = ctx.phase({'r': (ws[:6], storm)}, 0.6)
    busy = ctx.phase({'r': (ws[:6], storm), 'w': (ws[6:8], write), 'm': (ws[8:], maint)}, 1.8)
    ids = set().union(*(getattr(w, 'ids', set()) for w in ws[6:8]))
    mismatch = sum(w.ledger for w in ws[:6]) + sum(oracle(db, q, 'sift.enable_scan') for q in STORM)
    return base(ctx, busy, 'r', **kept(db, 'q_doc', ids, 10 ** 6), oracle_mismatch=mismatch, p99_ratio=p99_ratio(quiet['r'].lat, busy['r'].lat), writes=busy['w'].ops)


STAR = ('SELECT a.region, b.cat, count(*), sum(f.v) FROM s_fact f JOIN s_d1 a USING (d1) JOIN s_d2 b USING (d2) WHERE a.region < 3 AND b.cat IN (1, 2) GROUP BY 1, 2 ORDER BY 1, 2',
        'SELECT d1, sum(v), rank() OVER (ORDER BY sum(v) DESC, d1) FROM s_fact GROUP BY d1 ORDER BY 3 LIMIT 10',
        'SELECT d1 % 8, count(DISTINCT v), count(DISTINCT d2) FROM s_fact GROUP BY 1 ORDER BY 1',
        'WITH m AS MATERIALIZED (SELECT d2, sum(v) s FROM s_fact GROUP BY d2) SELECT b.cat, sum(m.s) FROM m JOIN s_d2 b USING (d2) GROUP BY 1 ORDER BY 1',
        'SELECT id, v FROM s_fact ORDER BY v DESC, id LIMIT 20')
PAR4 = PAR.replace('= 2', '= 4')


def olap_par_star_timeout_spill(ctx):
    """6 analytic sessions that each want 4 parallel workers of a pool of 4 at work_mem 512kB run a star join of a sift_col fact with two dimensions, a window over a grouping,
    distinct counts, a materialized CTE joined back and a top-N, half of them under a statement_timeout of 25 ms and all cancelled at random, beside 1 writer of the fact: every
    query answers what the heap does, every row kept, the memory of the readers after the load, no temporary file left, the parallel workers at their peak (at most the pool)"""
    db = ctx.db
    db.run('CREATE TABLE s_fact (id int8, d1 int, d2 int, v int8); INSERT INTO s_fact SELECT id, grp, id % 97, v FROM load_base_fact; '
           'CREATE INDEX s_fact_ix ON s_fact USING sift_col (id, d1, d2, v); '
           "CREATE TABLE s_d1 (d1 int PRIMARY KEY, region int, name text); INSERT INTO s_d1 SELECT g, g % 5, 'r' || g FROM generate_series(0, 63) g; "
           'CREATE TABLE s_d2 (d2 int PRIMARY KEY, cat int); INSERT INTO s_d2 SELECT g, g % 4 FROM generate_series(0, 96) g; ANALYZE s_fact, s_d1, s_d2')
    ws = ctx.sessions(8, f'{PAR4}; {TIGHT}')

    def olap(w, i):
        q = STAR[(w.seed + i) % len(STAR)]
        w.do(f"BEGIN; SET LOCAL statement_timeout = '25ms'; {q}; COMMIT" if w.seed % 2 else q, KILLED)

    def write(w, i):
        w.ids = getattr(w, 'ids', set())
        k = nxt(w)
        if w.do(f'BEGIN; INSERT INTO s_fact VALUES ({k}, {k % 64}, {k % 97}, {k % 1000}); UPDATE s_fact SET v = v + 1 WHERE id = {1 + i * 37 % N}; COMMIT') is not None:
            w.ids.add(k)

    ctx.phase({'r': (ws[:6], olap)}, 0.4)
    before = [ctx_kb(w) for w in ws[:6]], ctx.rss(ws[:6])
    busy = ctx.phase({'r': (ws[:6], olap), 'w': (ws[6:7], write), 'c': (ws[7:], cancels(ws[:6]))}, 1.8, peak=WORKERS)
    peak = ctx.obs.peak
    after = [ctx_kb(w) for w in ws[:6]], ctx.rss(ws[:6])
    db.run(f'{PAR4}; {TIGHT}')
    mismatch = sum(oracle(db, q, 'sift.enable_agg') for q in STAR)
    db.run('RESET ALL; SET search_path = load_case, public')
    return base(ctx, busy, 'r', **kept(db, 's_fact', getattr(ws[6], 'ids', set()), 10 ** 6), oracle_mismatch=mismatch, par_workers_peak=peak, timed_out=busy['r'].expected,
                **mem_delta(ctx, before, after))


COLD_ROWS = 2048
COLD_PEEK = 'SELECT grp, count(*), sum(v) FROM {t} WHERE id BETWEEN {lo} AND {hi} GROUP BY grp ORDER BY grp'


def cold_twin(w, i: int, allow: tuple = ('40001',)) -> int:
    """a REPEATABLE READ compare of a span of the cold (tiered) rows of pt with its twin in worker W"""
    lo = 1 + i * 97 % COLD_ROWS
    return snap_pair(w, COLD_PEEK.format(t='pt', lo=lo, hi=lo + 256), COLD_PEEK.format(t='pt_tw', lo=lo, hi=lo + 256), allow)


def repl_inflight_promote_cdc_tier(ctx):
    """a tiered sift_table (its cold parts in a file:// store) and its heap twin, a sift_table published to a subscriber: 2 writers of the primary add published rows and update
    cold rows, 3 sessions of the hot standby compare cold spans with the twin, and the standby is promoted while they all run; then the promoted node updates a cold row, its
    twin and its checks are clean, no backend keeps objects of the store open, and the subscriber ends with the rows of the primary; a new standby is cloned for the next case"""
    db, conn = ctx.db, standby()
    if db.val('SHOW wal_level') != 'logical':
        raise RuntimeError('the case needs wal_level = logical')
    store = ctx.store('load_tier_promote')
    db.run(f"{tiered('pt', 'file://' + store, COLD_ROWS)}; CREATE TABLE cdc ({SHAPE}) USING sift_table; "
           "DROP PUBLICATION IF EXISTS load_pub_p; CREATE PUBLICATION load_pub_p FOR TABLE cdc; SELECT 1 FROM pg_create_logical_replication_slot('load_cdc_p', 'pgoutput'); "
           'DROP DATABASE IF EXISTS load_case_psub WITH (FORCE); CREATE DATABASE load_case_psub')
    moved = int(db.val("SELECT count(*) FROM sift.tier_status WHERE relation = 'pt'::regclass"))
    sub = ctx.sessions(1, db='load_case_psub', path='public')[0]
    try:
        sub.run(f"CREATE EXTENSION pg_sift; CREATE SCHEMA load_case; CREATE TABLE load_case.cdc ({SHAPE}) USING sift_table; "
                f"CREATE SUBSCRIPTION load_sub_p CONNECTION '{conninfo()}' PUBLICATION load_pub_p WITH (create_slot = false, slot_name = 'load_cdc_p')")
        st = ctx.sessions(1, conn=conn)[0]
        lag = catchup(db, st)
        ws, rs = ctx.sessions(2), ctx.sessions(3, conn=conn)

        def write(w, i):
            w.ids = getattr(w, 'ids', set())
            k, c = nxt(w), 1 + (w.seed * 389 + i * 61) % COLD_ROWS
            if w.do(f"BEGIN; INSERT INTO cdc VALUES ({k}, {k % 64}, {i}, 'wire {k}'); UPDATE pt SET v = v + 1 WHERE id = {c}; UPDATE pt_tw SET v = v + 1 WHERE id = {c}; "
                    'COMMIT') is not None:
                w.ids.add(k)

        def read(w, i):
            w.ledger += cold_twin(w, w.seed * 7 + i)

        def promote(w, i):
            time.sleep(0.8 * ctx.a.scale)
            ctx.hook('LOAD_PROMOTE')
            end = time.monotonic() + CAP
            while w.val('SELECT pg_is_in_recovery()') == 't' and time.monotonic() < end:
                time.sleep(0.02)
            return False

        f0 = [fds(w.pid, store) for w in rs]
        busy = ctx.phase({'w': (ws, write), 'r': (rs, read), 'p': ([st], promote)}, 2.5)
        f1 = [fds(w.pid, store) for w in rs]
        _, err, _ = st.q('BEGIN; UPDATE pt SET v = v + 1000 WHERE id = 5; UPDATE pt_tw SET v = v + 1000 WHERE id = 5; COMMIT')
        promoted = int(err is not None) + int(st.val('SELECT pg_is_in_recovery()') == 't') + sum(hygiene(st).values())
        drift = sum(cold_twin(st, i, ()) for i in range(0, COLD_ROWS, 256)) + twin_mismatch(st, 'pt', 'pt_tw')
        want, end = db.row(CDC), time.monotonic() + CAP
        while sub.row(CDC) != want and time.monotonic() < end:
            time.sleep(0.05)
        got = sub.row(CDC)
        ids = set().union(*(getattr(w, 'ids', set()) for w in ws))
        return base(ctx, busy, 'w', **add(kept(db, 'cdc', ids, 10 ** 6), {'lost_rows': abs(int(want[0]) - int(got[0]))}),
                    oracle_mismatch=int(got != want) + twin_mismatch(db, 'pt', 'pt_tw'), standby_mismatch=lag + drift + sum(w.ledger for w in rs), promoted_errors=promoted,
                    fd_delta=fd_delta(f0, f1), tiered=moved, standby_reads=busy['r'].ops, conflicts=busy['r'].expected, **checks(sub))
    finally:
        sub.q('DROP SUBSCRIPTION IF EXISTS load_sub_p')
        ctx.close_sessions()
        ctx.hook('LOAD_RESTANDBY')
        db.run('DROP DATABASE IF EXISTS load_case_psub WITH (FORCE)')
        db.run("SELECT pg_drop_replication_slot(slot_name) FROM pg_replication_slots WHERE slot_name = 'load_cdc_p'; DROP PUBLICATION IF EXISTS load_pub_p")


WORKLOADS = (
    LoadCase('mix_copy_stream_ingest', 'mix', mix_copy_stream_ingest, Limits(lwlock_wait_pct=35, ctx_kb_delta=1280, rss_mb_delta=16)),
    LoadCase('olap_wide_toast_jsonb_mix', 'olap', olap_wide_toast_jsonb_mix, Limits(ctx_kb_delta=2048, rss_mb_delta=24, pending_after=0)),
    LoadCase('tenant_rls_view_col_ddl', 'tenant', tenant_rls_view_col_ddl, Limits(ctx_kb_delta=1280, lock_wait_pct=25)),
    LoadCase('conc_bm25_query_storm_churn', 'conc', conc_bm25_query_storm_churn, Limits(lwlock_wait_pct=35, p99_ratio=3.5)),
    LoadCase('olap_par_star_timeout_spill', 'olap', olap_par_star_timeout_spill, Limits(ctx_kb_delta=1280, rss_mb_delta=16, par_workers_peak=4)),
)
REPL = (
    LoadCase('repl_inflight_promote_cdc_tier', 'repl', repl_inflight_promote_cdc_tier, Limits(fd_delta=0), formats=(2,)),
)
