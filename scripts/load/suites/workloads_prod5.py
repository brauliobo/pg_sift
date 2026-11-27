"""The fifth wave of production shapes of the load gate (Spec 117, test/t/core/load_stress.sh): serializable write skew through the three engines (and the projections of a
sift_table), cursors held across commits, VACUUM and maintenance beside spilling cursors closed early, high-cardinality analytics that spill under cancels and timeouts,
databases cloned from a template that holds every engine beside busy sibling databases, MERGE with NOT MATCHED BY SOURCE and COPY that skips bad rows beside hybrid readers,
access method, column type and tablespace changes under a concurrent pg_dump, and temporary and unlogged objects through a kill -9. Every case runs on Format 2.0 and Format
1.0 and on the release library; what only Format 2.0 has (projections, tiering) joins on Format 2.0. Two cases wait for a finding, each intermittent (met on one format
or one plan only): the write skew case for S117-21 (a SERIALIZABLE read of a sift_table through its btree primary key misses the rw-conflict with a concurrent DELETE: both
transactions of a pair commit and the shift is left empty, on Format 2.0 by index, index-only and bitmap scans, on Format 1.0 by index and index-only scans), the temporary
tables for S117-20 (on Format 2.0 the commit of a temporary table ON COMMIT DELETE ROWS with a sift_col index fails with XX002 "Metadata block 5 is freed twice" in
col/f2 chain.c flip and quarantines the index, from the first commit that wrote a row).
"""
from __future__ import annotations

import os
import subprocess
import threading
import time
from dataclasses import dataclass

from cases import KILLED, MANIFEST, LoadCase
from collect import AGG, BM, add, base, ctx_kb, engines, kept, ledger_writes, mem_delta, multi_kept, nxt, oracle, oracles
from suites.common import Limits, fd_delta, fds
from suites.workloads_olap import cancels, tiered
from suites.workloads_prod2 import snap_oracle
from suites.workloads_prod3 import ROW_DIGEST, digest_differs, dups
from suites.workloads_prod4 import own, paced, pools, snap_digest


@dataclass(frozen=True)
class Wave5(Limits):
    spill_unseen: int | None = 0
    clone_errors: int | None = 0
    restore_mismatch: int | None = 0
    unlogged_errors: int | None = 0


ENGINES = ('tbl', 'col', 'bm')
PAIRS = 1024
ON_SHIFT = {'tbl': 'shift = {i}', 'col': 'shift = {i}', 'bm': "body @@> 's{i}x'"}
OFF_SHIFT = 'DELETE FROM s_{e} WHERE {on} AND doc = {d} AND (SELECT count(*) FROM s_{e} WHERE {on}) >= 2'
PROJ = (('p_agg', 'GROUP BY shift MEASURES count(*)'), ('p_sort', 'ORDER BY shift'))


def duty(fmt: int) -> str:
    rows = f'SELECT s, d FROM generate_series(0, {PAIRS - 1}) s, generate_series(1, 2) d'
    proj = ''.join(f"SELECT sift.create_projection('s_tbl', '{n}', '{p}'); " for n, p in PROJ) if fmt == 2 else ''
    return (f'CREATE TABLE s_tbl (shift int, doc int, PRIMARY KEY (shift, doc)) USING sift_table; INSERT INTO s_tbl {rows}; {proj}'
            f'CREATE TABLE s_col (shift int, doc int); INSERT INTO s_col {rows}; CREATE INDEX s_col_ix ON s_col USING sift_col (shift, doc); '
            f"CREATE TABLE s_bm (doc int, body text); INSERT INTO s_bm SELECT d, 's' || s || 'x' FROM ({rows}) r(s, d); CREATE INDEX s_bm_ix ON s_bm USING sift (body); "
            'ANALYZE s_tbl, s_col, s_bm')


def txn_ssi_all_engines_proj(ctx):
    """three pairs of SERIALIZABLE workers, one pair a table (a sift_table with a GROUP BY and an ORDER BY projection on Format 2.0, a heap with a sift_col index, a heap
    with a BM25 index): both workers of a pair take themselves off the same shift together when two are on it, each reading the shift through the index of its engine (a write
    skew: a shift left empty), beside 1 session that vacuums and maintains the three; no shift is ever left empty, only 40001 refused, the index paths answer what the heap does"""
    db = ctx.db
    db.run(duty(ctx.fmt))
    ws = ctx.sessions(7, 'SET enable_seqscan = off')
    bars = {e: threading.Barrier(2) for e in ENGINES}

    def skew(e):
        def go(w, i):
            try:
                bars[e].wait(1.0)
            except threading.BrokenBarrierError:
                return False
            if i >= PAIRS:
                return False
            off = OFF_SHIFT.format(e=e, on=ON_SHIFT[e].format(i=i), d=w.seed % 2 + 1)
            w.do(f'BEGIN ISOLATION LEVEL SERIALIZABLE; {off}; COMMIT', ('40001',))
            w.top = i
        return go

    def maint(w, i):
        w.do(('VACUUM s_tbl, s_col, s_bm', "SELECT count(*) FROM sift.maintain('s_col_ix')", "SELECT count(*) FROM sift.maintain('s_tbl')",
              "SELECT count(*) FROM sift.maintain('s_bm_ix')")[i % 4])
        time.sleep(0.05)

    roles = {e: (ws[2 * j:2 * j + 2], skew(e)) for j, e in enumerate(ENGINES)}
    busy = ctx.phase(dict(roles, m=(ws[6:], maint)), 2.5)
    for b in bars.values():
        b.abort()
    ctx.close_sessions()
    tops = {e: min(getattr(w, 'top', -1) for w in roles[e][0]) for e in ENGINES}
    skewed = {e: int(db.val(f'SELECT {tops[e] + 1} - count(DISTINCT shift) FROM s_{e} WHERE shift <= {tops[e]}' if e != 'bm' else
                            f'SELECT {tops[e] + 1} - count(DISTINCT body) FROM s_bm WHERE substr(body, 2, length(body) - 2)::int <= {tops[e]}')) for e in ENGINES}
    mismatch = oracle(db, 'SELECT shift, count(*) FROM s_tbl GROUP BY shift ORDER BY shift', 'sift.enable_agg') + \
        oracle(db, 'SELECT shift % 16, count(*) FROM s_col GROUP BY 1 ORDER BY 1', 'sift.enable_agg') + oracle(db, "SELECT count(*) FROM s_bm WHERE body @@> 's1*'", 'sift.enable_scan')
    return base(ctx, busy, 'tbl', write_skew=sum(skewed.values()), oracle_mismatch=mismatch, shifts=min(tops.values()) + 1,
                serialization_failures=sum(busy[e].expected for e in ENGINES), **{f'skew_{e}': n for e, n in skewed.items()})


HOLD = {'tbl': 'SELECT id, v FROM e_tbl ORDER BY id', 'col': 'SELECT id, v FROM e_col WHERE grp < 32 ORDER BY id', 'bm': "SELECT id, v FROM e_bm WHERE body @@> 'wire' ORDER BY id",
        'tv': 'SELECT id, v FROM tv ORDER BY id'}
SPILL = ('SELECT id, body FROM e_col ORDER BY body, id', 'SELECT grp, count(DISTINCT body) FROM e_col GROUP BY grp ORDER BY 2 DESC, 1',
         "SELECT id, v FROM e_bm WHERE body @@> 'wire' ORDER BY md5(body), id")
TIGHT = "SET work_mem = '128kB'"
CACHE = 'SET sift.backend_cache_kb = 1024'


def held(t: str) -> str:
    """a cursor WITH HOLD over T declared in a REPEATABLE READ transaction beside the count and sum of the same rows, fetched after the commit"""
    q = HOLD[t]
    return (f'BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*), coalesce(sum(v), 0) FROM ({q}) s; DECLARE c CURSOR WITH HOLD FOR {q}; COMMIT; FETCH 64 FROM c; '
            'FETCH ALL FROM c; CLOSE c')


def torn(out: list | None) -> int:
    """1 when the rows a held cursor gave are not the count and sum its transaction saw"""
    if out is None:
        return 0
    n, s = (int(x) for x in out[0].split('|'))
    rows = [r.split('|') for r in out[1:]]
    return int(len(rows) != n or sum(int(v) for _, v in rows) != s)


def txn_with_hold_cursor_maint(ctx):
    """3 sessions declare cursors WITH HOLD over a sift_table, a sift_col and a BM25 table (and a tiered sift_table on Format 2.0), commit and fetch them across the writes of 2
    writers, VACUUM and sift.maintain; 2 sessions at work_mem 128kB open NO SCROLL cursors that spill, close them after a few rows and DISCARD ALL: every held cursor gives
    the rows its snapshot saw, no temporary file and no file of the store left open, the memory of the cursor backends after the load"""
    db = ctx.db
    engines(db)
    kinds, store = list(ENGINES), ctx.store('load_tier_hold')
    if ctx.fmt == 2:
        db.run(tiered('tv', 'file://' + store, 2048))
        kinds.append('tv')
    ws = ctx.sessions(8)

    def hold(w, i):
        w.ledger += torn(w.do(held(kinds[(w.seed + i) % len(kinds)])))

    def spill(w, i):
        w.do(f'{TIGHT}; BEGIN; DECLARE s NO SCROLL CURSOR FOR {SPILL[(w.seed + i) % len(SPILL)]}; FETCH 5 FROM s; CLOSE s; COMMIT; DISCARD ALL; '
             'SET search_path = load_case, public')

    def maint(w, i):
        w.do(('VACUUM e_tbl, e_col, e_bm', "SELECT count(*) FROM sift.maintain('e_tbl')", "SELECT count(*) FROM sift.maintain('e_col_ix')")[i % 3])
        time.sleep(0.05)

    roles = {'w': (ws[:2], ledger_writes('e')), 'h': (ws[2:5], hold), 's': (ws[5:7], spill), 'm': (ws[7:], maint)}
    probe = ws[2:7]
    ctx.phase(roles, 0.5)
    before, f0 = ([ctx_kb(w) for w in probe], ctx.rss(probe)), [fds(w.pid, store) for w in probe]
    busy = ctx.phase(roles, 2.0)
    after, f1 = ([ctx_kb(w) for w in probe], ctx.rss(probe)), [fds(w.pid, store) for w in probe]
    return base(ctx, busy, 'h', **add(multi_kept(db, 'e', ws[:2]), {'oracle_mismatch': oracles(db) + sum(w.ledger for w in ws[2:5])}), fd_delta=fd_delta(f0, f1),
                spilled=busy['s'].ops, **mem_delta(ctx, before, after))


H_ROWS = 16384
H_KEYS = 4096
H_SETUP = (f'CREATE TABLE h_fact (id int8, k int, d int, v int8, tag text, arr int[], doc jsonb); '
           f"INSERT INTO h_fact SELECT g, hashint4(g) & {H_KEYS - 1}, (hashint4(g * 7) & 4095) % (1 + g % 64), g % 1000, repeat(md5(g::text), 1 + g % 4), "
           f"ARRAY[g % 7, g % 11], jsonb_build_object('k', g % 97, 'w', repeat('x', g % 64)) FROM generate_series(1, {H_ROWS}) g; "
           'CREATE INDEX h_fact_ix ON h_fact USING sift_col (id, k, d, v); '
           f"CREATE TABLE h_dim (d int, name text); INSERT INTO h_dim SELECT g % 4096, 'n' || g FROM generate_series(0, 4095) g; ANALYZE h_fact, h_dim")
H_OPS = ('SELECT k, count(*), sum(v) FROM h_fact GROUP BY k ORDER BY 2 DESC, 1 LIMIT 20', 'SELECT count(DISTINCT k), count(DISTINCT tag), count(DISTINCT v) FROM h_fact',
         'SELECT id, v FROM h_fact ORDER BY tag DESC, id LIMIT 10', 'SELECT k, sum(v) OVER (PARTITION BY k ORDER BY id) s FROM h_fact ORDER BY s DESC, k LIMIT 10',
         'SELECT count(*), count(n.d) FROM h_fact f LEFT JOIN h_dim n ON n.d = f.d', 'SELECT count(*) FROM h_fact f WHERE EXISTS (SELECT 1 FROM h_dim n WHERE n.d = f.d + 1)',
         'SELECT count(*) FROM h_fact f WHERE NOT EXISTS (SELECT 1 FROM h_dim n WHERE n.d = f.k)',
         'SELECT d % 8, percentile_disc(0.5) WITHIN GROUP (ORDER BY v), mode() WITHIN GROUP (ORDER BY k) FROM h_fact GROUP BY 1 ORDER BY 1',
         'SELECT a, count(*), sum(v) FROM h_fact, unnest(arr) a GROUP BY a ORDER BY a', "SELECT doc->>'k', count(*) FROM h_fact GROUP BY 1 ORDER BY 2 DESC, 1 LIMIT 10")
TEMP_BYTES = 'SELECT coalesce(sum(temp_bytes), 0) FROM pg_stat_database'


def olap_highcard_spill_cancel(ctx):
    """a 16K-row fact of 4K keys with wide text, arrays and JSON under a sift_col index and a skewed 4K dimension: 6 sessions at work_mem 128kB cycle grouping by the keys,
    distinct counts, a top-N, a window, outer, semi and anti joins, ordered-set aggregates and unnest, each with the index path and the heap in one snapshot, half of them under
    a SET LOCAL statement_timeout of 30 ms, while 1 cancels them and 1 writes the fact: every answer agrees, they spilled, no temporary file is left, the memory after the load"""
    db = ctx.db
    db.run(H_SETUP)
    ws = ctx.sessions(8, TIGHT)
    t0 = int(db.val(TEMP_BYTES))

    def olap(w, i):
        w.pid
        q = H_OPS[(w.seed + i) % len(H_OPS)]
        if w.seed % 2:
            w.do(f"BEGIN; SET LOCAL statement_timeout = '30ms'; {q}; COMMIT", KILLED)
        else:
            w.ledger += snap_oracle(w, q, 'sift.enable_agg')

    def write(w, i):
        k = nxt(w)
        w.do(f"BEGIN; INSERT INTO h_fact VALUES ({k}, {k % H_KEYS}, {k % 64}, {k % 1000}, 'w{k}', ARRAY[{k % 7}], '{{}}'); UPDATE h_fact SET v = v + 1 WHERE id = {1 + i * 37 % H_ROWS}; COMMIT")

    roles = {'r': (ws[:6], olap), 'c': (ws[6:7], cancels(ws[:6])), 'w': (ws[7:], write)}
    ctx.phase(roles, 0.5)
    before = [ctx_kb(w) for w in ws[:6]], ctx.rss(ws[:6])
    busy = ctx.phase(roles, 2.0)
    after = [ctx_kb(w) for w in ws[:6]], ctx.rss(ws[:6])
    ctx.close_sessions()
    db.run('SELECT pg_stat_force_next_flush()')
    spilled = int(db.val(TEMP_BYTES)) - t0
    mismatch = sum(w.ledger for w in ws[:6]) + sum(oracle(db, q, 'sift.enable_agg') for q in H_OPS[::3])
    return base(ctx, busy, 'r', oracle_mismatch=mismatch, spill_unseen=int(spilled <= 0), spilled_kb=spilled // 1024, cancelled=busy['r'].expected,
                **mem_delta(ctx, before, after))


T_TPL, T_SIBS = 'load_case_tpl', ('load_case_s1', 'load_case_s2')
T_DIGEST = ("SELECT count(*), sum(v) FROM public.t_tbl", "SELECT grp, count(*), sum(v) FROM public.t_col GROUP BY grp ORDER BY grp",
            "SELECT count(*), sum(id) FROM public.t_bm WHERE body @@> 'wire'")


def t_setup(store: str) -> str:
    """the tables of a tenant database, a tiered sift_table among them with a STORE"""
    tier = f"; {tiered('t_tv', 'file://' + store, 1024)}" if store else ''
    return (f"CREATE EXTENSION pg_sift; CREATE TABLE t_tbl (id int8 PRIMARY KEY, grp int, v int8, body text) USING sift_table; "
            f"INSERT INTO t_tbl SELECT g, g % 64, g % 1000, 'wire t' || g FROM generate_series(1, 4096) g; CREATE TABLE t_col AS SELECT * FROM t_tbl; "
            f'CREATE INDEX t_col_ix ON t_col USING sift_col (id, grp, v); CREATE TABLE t_bm AS SELECT * FROM t_tbl; CREATE INDEX t_bm_ix ON t_bm USING sift (body); '
            f'ANALYZE t_tbl, t_col, t_bm{tier}')


def on(ctx, name: str, fn):
    """FN of a session of the database NAME, closed after it"""
    s = type(ctx.db)(db=name, init='SET search_path = public')
    try:
        return fn(s)
    finally:
        s.close()


def clone_check(s, want: list) -> tuple:
    """(1 when the database of S answers other than WANT, by the index paths too; the counts of its checks)"""
    got = [s.run(q) for q in T_DIGEST]
    return int(got != want) + sum(oracle(s, q, g) for q, g in zip(T_DIGEST[1:], ('sift.enable_agg', 'sift.enable_scan'))), checks_public(s)


def checks_public(s, nsp: str = 'public') -> dict:
    """sift.check of every sift relation of the schema NSP of the database of S, its quarantine and its health"""
    rels = s.run(f"SELECT c.oid FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE a.amname IN ('sift', 'sift_col', 'sift_table') AND c.relnamespace = '{nsp}'::regnamespace")
    return {'verify_errors': sum(int(s.val(f'SELECT count(*) FROM sift.check({r}::regclass, true)')) for r in rels),
            'false_quarantines': int(s.val('SELECT count(*) FROM sift.quarantine')), 'health_errors': int(s.val("SELECT count(*) FROM sift.health() WHERE severity = 'error'"))}


def tenant_template_db_tier_iso(ctx):
    """a template database of a sift_table, a sift_col and a BM25 table (and a tiered sift_table on Format 2.0): 1 session clones it (CREATE DATABASE ... TEMPLATE), reads
    and checks the clone and drops it, again and again, while 2 sibling databases with async maintenance take writes and maintenance: every clone answers what the template
    does, nothing refused (the database in use, the shared memory), the template and the siblings clean after the load"""
    db = ctx.db
    store = ctx.store('load_tier_tpl')
    for d in (T_TPL,) + T_SIBS:
        db.run(f'DROP DATABASE IF EXISTS {d} WITH (FORCE); CREATE DATABASE {d}')
    try:
        want = on(ctx, T_TPL, lambda s: (s.run(t_setup(store if ctx.fmt == 2 else '')), [s.run(q) for q in T_DIGEST])[1])
        for d in T_SIBS:
            on(ctx, d, lambda s: s.run(t_setup('')))
        ws = [w for d in T_SIBS for w in ctx.sessions(2, 'SET sift.async_maintenance = on', d, 'public')]
        r0, wrong = sum(r for _, r in pools(db).values()), []

        def churn(w, i):
            k = nxt(w)
            w.do(f"BEGIN; INSERT INTO t_tbl VALUES ({k}, {k % 64}, 1, 'wire c'); INSERT INTO t_col VALUES ({k}, {k % 64}, 1, 'wire c'); "
                 f"UPDATE t_bm SET v = v + 1 WHERE id = {1 + i % 4096}; COMMIT" if w.seed == 1 else
                 ('VACUUM t_tbl, t_col, t_bm', "SELECT count(*) FROM sift.maintain('t_col_ix')", "SELECT count(*) FROM sift.maintain('t_bm_ix')")[i % 3])

        def clone(w, i):
            name = f'load_case_c{i}'
            out = w.do(f'CREATE DATABASE {name} TEMPLATE {T_TPL}', ('55006',))
            if out is None:
                w.ledger += w.last[0] == '55006'
                return
            wrong.append(on(ctx, name, lambda s: clone_check(s, want)))
            w.do(f'DROP DATABASE {name}', ('55006',))
            w.ledger += w.last is not None

        ctl = ctx.sessions(1)[0]
        busy = ctx.phase({'w': (ws, churn), 'c': ([ctl], clone)}, 3.0)
        refused = sum(r for _, r in pools(db).values()) - r0
        ctx.close_sessions()
        tpl = on(ctx, T_TPL, lambda s: clone_check(s, want))
        sib = [on(ctx, d, checks_public) for d in T_SIBS]
        return base(ctx, busy, 'c', **add(*(c for _, c in wrong), tpl[1], *sib), oracle_mismatch=sum(b for b, _ in wrong) + tpl[0], drop_in_use=ctl.ledger,
                    shm_refusals=refused, clones=len(wrong), sib_ops=busy['w'].ops)
    finally:
        ctx.close_sessions()
        for d in [r for r in db.run("SELECT datname FROM pg_database WHERE datname LIKE 'load\\_case\\_%'")]:
            db.run(f'DROP DATABASE IF EXISTS {d} WITH (FORCE)')


M_SHAPE = 'id int8 PRIMARY KEY, grp int, v int8, body text, tags int[], doc jsonb, blob bytea'
M_TABLES = ('m_tbl', 'm_tw', 'm_hy')
M_ROW = "format('%s:%s:%s:%s:%s:%s:%s', id, grp, v, body, tags, doc, md5(blob))"
M_SPAN = 64
M_WINDOW = 16


def m_source(a: int, i: int) -> str:
    """rows A to A + M_WINDOW - 1 of merge I: wide values (blobs of 8 to 32 kB, arrays, JSON)"""
    return (f"SELECT g AS id, g % 64 AS grp, {i} AS v, 'wire m' || g || ' i{i}' AS body, ARRAY[g % 7, {i}] AS tags, jsonb_build_object('i', {i}, 'g', g) AS doc, "
            f"decode(repeat(md5(g || ':' || {i}), 512 * (1 + g % 4)), 'hex') AS blob FROM generate_series({a}, {a + M_WINDOW - 1}) g")


def merge(t: str, lo: int, a: int, i: int) -> str:
    return (f'MERGE INTO {t} t USING ({m_source(a, i)}) s ON t.id = s.id WHEN MATCHED THEN UPDATE SET v = t.v + s.v, body = s.body, tags = s.tags, doc = s.doc, blob = s.blob '
            'WHEN NOT MATCHED THEN INSERT VALUES (s.id, s.grp, s.v, s.body, s.tags, s.doc, s.blob) '
            f'WHEN NOT MATCHED BY SOURCE AND t.id >= {lo} AND t.id < {lo + M_SPAN} THEN DELETE')


def copy_err(t: str, ids) -> str:
    """a COPY of IDS into T that skips its bad rows (every fifth id has a grp that is no integer)"""
    rows = '\n'.join(f'{k}\t{k % 64}\t{k % 1000}\twire copy{k}' if k % 5 else f'{k}\tnot-an-int\t0\tbad' for k in ids)
    return f'COPY {t} (id, grp, v, body) FROM STDIN WITH (ON_ERROR ignore);\n{rows}\n\\.\n'


def mix_merge_copy_err_hybrid(ctx):
    """a sift_table, its heap twin and a heap with a sift_col and a BM25 index (hybrid), all of wide rows: 3 sessions MERGE a window of 16 rows of 8 to 32 kB into a span
    of their own of each table in one transaction (updated when matched, inserted when not, the rest of the span deleted WHEN NOT MATCHED BY SOURCE), 1 COPYs batches with bad
    rows (ON_ERROR ignore) into all three and deletes its batch of four COPYs before (the tables keep their size), 2 readers (a backend cache of 1 MB) compare the tables
    and the index paths with the heap in one snapshot: each span holds its last window, every good row of a committed COPY kept once and no bad one, every answer agrees,
    the memory of the readers after the load. The checks after the load run in a session the runner ends before its hygiene (a Format 1.0 backend spills the tombstones
    it read to temporary files it keeps while it lives)"""
    db = ctx.db
    db.run(f'CREATE TABLE m_tbl ({M_SHAPE}) USING sift_table; CREATE TABLE m_tw ({M_SHAPE}); CREATE TABLE m_hy ({M_SHAPE}); '
           'CREATE INDEX m_hy_ix ON m_hy USING sift_col (id, grp, v); CREATE INDEX m_hy_bm ON m_hy USING sift (body)')
    ws = ctx.sessions(4) + ctx.sessions(2, CACHE)

    def write(w, i):
        lo = w.seed * 10 ** 6
        a = lo + (i * 5) % (M_SPAN - M_WINDOW)
        if w.do('BEGIN; ' + '; '.join(merge(t, lo, a, i) for t in M_TABLES) + '; COMMIT') is not None:
            w.window = set(range(a, a + M_WINDOW))

    def copier(w, i):
        lo, live = 9 * 10 ** 7 + 50 * (nxt(w) % 10 ** 6), getattr(w, 'live', [])
        ids, old = range(lo, lo + 50), live[:-3]
        drop = ''.join(f'DELETE FROM {t} WHERE id >= {o} AND id < {o + 50};\n' for o in old for t in M_TABLES)
        if w.do('BEGIN;\n' + drop + ''.join(copy_err(t, ids) for t in M_TABLES) + 'COMMIT') is not None:
            w.live = live[-3:] + [lo]
            w.ids = {k for o in w.live for k in range(o, o + 50) if k % 5}

    def read(w, i):
        w.ledger += (snap_digest(w, 'm_tbl', 'm_tw', M_ROW), snap_digest(w, 'm_hy', 'm_tw', M_ROW), snap_oracle(w, BM.format(t='m_hy'), 'sift.enable_scan', ()),
                     snap_oracle(w, AGG.format(t='m_hy'), 'sift.enable_agg', ()))[i % 4]

    roles = {'w': (ws[:3], write), 'c': (ws[3:4], copier), 'r': (ws[4:], read)}
    ctx.phase(roles, 0.5)
    before = [ctx_kb(w) for w in ws[4:]], ctx.rss(ws[4:])
    busy = ctx.phase(roles, 2.0)
    after = [ctx_kb(w) for w in ws[4:]], ctx.rss(ws[4:])
    ctx.close_sessions()
    db = ctx.sessions(1)[0]
    spans = add(*(kept(db, t, getattr(w, 'window', set()), w.seed * 10 ** 6, w.seed * 10 ** 6 + M_SPAN) for w in ws[:3] for t in M_TABLES))
    copied = add(*(kept(db, t, getattr(ws[3], 'ids', set()), 9 * 10 ** 7) for t in M_TABLES))
    mismatch = digest_differs(db, 'm_tbl', 'm_tw', M_ROW) + digest_differs(db, 'm_hy', 'm_tw', M_ROW) + sum(w.ledger for w in ws[4:]) + \
        oracle(db, BM.format(t='m_hy'), 'sift.enable_scan') + oracle(db, AGG.format(t='m_hy'), 'sift.enable_agg')
    return base(ctx, busy, 'w', **add(spans, copied, {'dup_rows': sum(dups(db, t) for t in M_TABLES)}), oracle_mismatch=mismatch, merges=busy['w'].ops,
                copies=busy['c'].ops, **mem_delta(ctx, before, after))


A_SHAPE = 'id int8 PRIMARY KEY, grp int, v int8, body text'
A_ROW = "id || ':' || grp || ':' || v || ':' || body"
A_LOCK = 'LOCK TABLE a_sw, a_tw IN ACCESS SHARE MODE'
A_DDL = ('ALTER TABLE a_sw SET ACCESS METHOD heap', 'ALTER TABLE {t} ALTER COLUMN v TYPE numeric(18,0)', 'ALTER TABLE {t} SET TABLESPACE load_ts5',
         'ALTER TABLE a_sw SET ACCESS METHOD sift_table', 'ALTER TABLE {t} ALTER COLUMN v TYPE int8', 'ALTER TABLE {t} SET TABLESPACE pg_default')


def a_restore(ctx, w, i: int, d: str) -> int:
    """a pg_dump -Fc of the two tables (into the directory D) restored into a database of its own: 1 when the restored table and twin differ (one snapshot of the dump)
    or fail their checks"""
    name, env, dump = f'load_case_r{i}', ctx.db.env(), os.path.join(d, f'a{i}.dump')
    sh(['pg_dump', '-Fc', '-t', 'load_case.a_sw', '-t', 'load_case.a_tw', '-f', dump], env)
    w.run(f'DROP DATABASE IF EXISTS {name}; CREATE DATABASE {name}')
    on(ctx, name, lambda s: s.run('CREATE EXTENSION pg_sift; CREATE SCHEMA load_case'))
    sh(['pg_restore', '--no-tablespaces', '--no-owner', '--exit-on-error', '-d', name, dump], env)
    os.unlink(dump)
    bad = on(ctx, name, lambda s: digest_differs(s, 'load_case.a_sw', 'load_case.a_tw', A_ROW) + sum(checks_public(s, 'load_case').values()))
    w.run(f'DROP DATABASE {name}')
    return bad


def sh(cmd: list, env: dict):
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f'{cmd[0]} exited {r.returncode}: {" ".join(r.stderr.split())[:300]}')


def ddl_am_type_tblspc_dump(ctx):
    """a table that changes its access method (sift_table to heap and back), the type of a column (int8, numeric) and its tablespace (an in-place one and back), each change
    also on its heap twin in one transaction, idle DUTY times as long as it took: 3 writers insert and update both in one transaction, 2 readers lock both and compare them
    and the analytic path with the heap in one snapshot, while 1 session dumps both with pg_dump -Fc between the changes and restores them into a database of their own: the
    twin agrees in every snapshot, in every restore and at the end, every committed row kept once"""
    db = ctx.db
    db.run("SET allow_in_place_tablespaces = on; DROP TABLESPACE IF EXISTS load_ts5; CREATE TABLESPACE load_ts5 LOCATION ''; RESET allow_in_place_tablespaces")
    try:
        db.run(f'CREATE TABLE a_sw ({A_SHAPE}) USING sift_table; CREATE TABLE a_tw ({A_SHAPE}); INSERT INTO a_sw SELECT * FROM load_base_fact; '
               'INSERT INTO a_tw SELECT * FROM load_base_fact; ANALYZE a_sw, a_tw')
        ws, gate, dumps = ctx.sessions(7), threading.Lock(), ctx.store('load_dump')

        def write(w, i):
            k = nxt(w)
            ins = f"INSERT INTO {{t}} VALUES ({k}, {k % 64}, {k % 1000}, 'wire tail{k % 97}')"
            upd = f'UPDATE {{t}} SET v = v + 1 WHERE id = {own(w, i, 3)}'
            if w.do(f"BEGIN; {'; '.join(q.format(t=t) for q in (ins, upd) for t in ('a_sw', 'a_tw'))}; COMMIT") is not None:
                w.ids = getattr(w, 'ids', set()) | {k}

        def read(w, i):
            if i % 2:
                w.ledger += snap_oracle(w, f'{A_LOCK}; {AGG.format(t="a_sw")}', 'sift.enable_agg', ())
            else:
                out = w.do(f'BEGIN ISOLATION LEVEL REPEATABLE READ; {A_LOCK}; {ROW_DIGEST.format(row=A_ROW, t="a_sw")}; {ROW_DIGEST.format(row=A_ROW, t="a_tw")}; COMMIT')
                w.ledger += out is not None and out[0] != out[1]

        def ddl(w, i):
            step = A_DDL[i % len(A_DDL)]
            with gate:
                w.do(f"BEGIN; {'; '.join(dict.fromkeys(step.format(t=t) for t in ('a_sw', 'a_tw')))}; COMMIT")
            paced(w)

        def dump(w, i):
            with gate:
                w.ledger += a_restore(ctx, w, i, dumps)
            w.restored = i + 1
            time.sleep(0.05)

        busy = ctx.phase({'w': (ws[:3], write), 'r': (ws[3:5], read), 'd': (ws[5:6], ddl), 'p': (ws[6:], dump)}, 3.0)
        ctx.close_sessions()
        ids = set().union(*(getattr(w, 'ids', set()) for w in ws[:3]))
        return base(ctx, busy, 'w', **add(kept(db, 'a_sw', ids, 10 ** 6), {'dup_rows': dups(db, 'a_sw')}), oracle_mismatch=digest_differs(db, 'a_sw', 'a_tw', A_ROW) +
                    sum(w.ledger for w in ws[3:5]), restore_mismatch=ws[6].ledger, restores=getattr(ws[6], 'restored', 0), ddl=busy['d'].ops)
    finally:
        ctx.close_sessions()
        db.q('ALTER TABLE IF EXISTS a_sw SET TABLESPACE pg_default; ALTER TABLE IF EXISTS a_tw SET TABLESPACE pg_default')
        db.run('DROP TABLESPACE IF EXISTS load_ts5')


SUM = {n: f'SELECT count(*), sum(v) FROM load_base_fact WHERE id <= {n}' for n in (256, 512)}
TEMPS = ('BEGIN; CREATE TEMP TABLE t_d (id int8 PRIMARY KEY, grp int, v int8, body text) USING sift_table ON COMMIT DROP; '
         'INSERT INTO t_d SELECT * FROM load_base_fact WHERE id <= 512; SELECT count(*), sum(v) FROM t_d; COMMIT',
         'CREATE TEMP TABLE IF NOT EXISTS t_r (id int8, grp int, v int8, body text) ON COMMIT DELETE ROWS; CREATE INDEX IF NOT EXISTS t_r_ix ON t_r USING sift_col (id, grp, v); '
         'BEGIN; INSERT INTO t_r SELECT * FROM load_base_fact WHERE id <= 256; SET LOCAL enable_seqscan = off; SELECT count(*), sum(v) FROM t_r WHERE id > 0; COMMIT',
         'CREATE TEMP TABLE IF NOT EXISTS t_p (id int8, grp int, v int8, body text); CREATE INDEX IF NOT EXISTS t_p_ix ON t_p USING sift (body); '
         "TRUNCATE t_p; INSERT INTO t_p SELECT * FROM load_base_fact WHERE id <= 256; SELECT count(*), sum(v) FROM t_p WHERE body @@> 'tail*'",
         'DISCARD ALL; SET search_path = load_case, public')
U_TABLES = ('CREATE UNLOGGED TABLE u_tbl (id int8 PRIMARY KEY, grp int, v int8, body text) USING sift_table; CREATE UNLOGGED TABLE u_col (id int8, grp int, v int8, body text); '
            'CREATE INDEX u_col_ix ON u_col USING sift_col (id, grp, v); CREATE UNLOGGED TABLE u_bm (id int8, grp int, v int8, body text); '
            'CREATE INDEX u_bm_ix ON u_bm USING sift (body)')


def fault_temp_unlogged_crash_shm(ctx):
    """first 4 sessions cycle temporary tables of the three engines (ON COMMIT DROP, ON COMMIT DELETE ROWS, kept and truncated) and DISCARD ALL: every answer what the rows
    give, nothing refused and nothing left in the manifest of the shared memory once they exit (Format 2.0); then 4 writers commit rows of logged and 2 of UNLOGGED tables of
    the three engines until the server is killed with -9 and started again: every acknowledged logged row kept, the unlogged tables empty, writable and readable by their
    index paths, nothing quarantined"""
    db = ctx.db
    want = {n: db.run(q) for n, q in SUM.items()}
    ws = ctx.sessions(4)
    u0, r0 = (int(x) for x in db.row(MANIFEST))

    def temp(w, i):
        out = w.do(TEMPS[(w.seed + i) % len(TEMPS)])
        n = (512, 256, 256, None)[(w.seed + i) % len(TEMPS)]
        w.ledger += out is not None and n is not None and out != want[n]

    temps = ctx.phase({'t': (ws, temp)}, 1.5)
    ctx.close_sessions()
    temp_wrong = sum(w.ledger for w in ws)
    u1, r1 = (int(x) for x in db.row(MANIFEST))
    engines(db, kinds=('tbl', 'col', 'bm'))
    db.run(U_TABLES)
    up = db.val('SELECT pg_postmaster_start_time()')
    ws = ctx.sessions(7, 'SET sift.async_maintenance = on')
    for w in ws[:6]:
        w.allow = ('08006', '57P01')

    def crash(w, i):
        if int(w.val('SELECT count(*) FROM e_tbl WHERE id >= 1000000')) < 200 and i < 400:
            return time.sleep(0.01)
        ctx.hook('LOAD_CRASH')
        return False

    busy = ctx.phase({'w': (ws[:4], ledger_writes('e', churn=False)), 'u': (ws[4:6], ledger_writes('u', churn=False)), 'k': (ws[6:], crash)}, 30.0, lead='k')
    ctx.close_sessions()
    ctx.restart('LOAD_RESTART')
    acked = set().union(*(getattr(w, 'ids', set()) for w in ws[:4]))
    have = {int(x) for x in db.run('SELECT id FROM e_tbl WHERE id >= 1000000')}
    m = add(kept(db, 'e_col', have, 10 ** 6), kept(db, 'e_bm', have, 10 ** 6), {'lost_rows': len(acked - have)})
    left = sum(int(db.val(f'SELECT count(*) FROM u_{e}')) for e in ENGINES)
    db.run("; ".join(f"INSERT INTO u_{e} VALUES (7, 7, 7, 'wire after')" for e in ENGINES))
    reads = db.run("SET enable_seqscan = off; SELECT count(*) FROM u_tbl WHERE id = 7; SELECT count(*) FROM u_col WHERE id = 7; "
                   "SELECT count(*) FROM u_bm WHERE body @@> 'after'; RESET enable_seqscan")
    return base(ctx, busy, 'w', **m, oracle_mismatch=oracles(db) + temp_wrong,
                unlogged_errors=left + sum(r != '1' for r in reads), shm_used_after=max(0, u1 - u0) if ctx.fmt == 2 else None, shm_refusals=r1 - r0,
                acked=len(acked), temp_cycles=temps['t'].ops, not_restarted=int(db.val('SELECT pg_postmaster_start_time()') == up))


STRESS = (
    LoadCase('txn_ssi_all_engines_proj', 'txn', txn_ssi_all_engines_proj, Wave5(), open='S117-21', intermittent=True),
    LoadCase('txn_with_hold_cursor_maint', 'txn', txn_with_hold_cursor_maint, Wave5(ctx_kb_delta=1280, rss_mb_delta=16, fd_delta=0)),
    LoadCase('olap_highcard_spill_cancel', 'olap', olap_highcard_spill_cancel, Wave5(ctx_kb_delta=2048, rss_mb_delta=24)),
    LoadCase('tenant_template_db_tier_iso', 'tenant', tenant_template_db_tier_iso, Wave5()),
    LoadCase('mix_merge_copy_err_hybrid', 'mix', mix_merge_copy_err_hybrid, Wave5(ctx_kb_delta=1280, rss_mb_delta=16)),
    LoadCase('ddl_am_type_tblspc_dump', 'ddl', ddl_am_type_tblspc_dump, Wave5()),
    LoadCase('fault_temp_unlogged_crash_shm', 'fault', fault_temp_unlogged_crash_shm, Wave5(shm_used_after=0), last=True, open='S117-20', intermittent=True),
)
