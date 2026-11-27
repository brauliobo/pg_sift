"""The next wave of the load gate (Spec 111): the operators of the V6 executor under a tight work_mem, the rewrites and the splits and merges of a sift_table under its writers,
upserts against foreign keys and unique sift_col and BM25 indexes, a storm of evictions of the tier cache, workers on the high process numbers of a full pool, and a hot standby
whose queries the recovery cancels (scripts/load/suites: WORKLOADS on test/t/core/load_workloads.sh, REPL on test/t/core/load_repl.sh).
"""
from __future__ import annotations

import os
import sys
import time
from concurrent.futures import ThreadPoolExecutor

sys.dont_write_bytecode = True

from cases import KILLED, LoadCase  # noqa: E402
from collect import (AGG, BM, add, base, checks, ctx_kb, engines, halves_differ, kept, ledger_writes, mem_delta, multi_kept, nxt, oracle, oracles, row3,  # noqa: E402
                     snap, tbl, until, zipf)
from suites.common import TWIN, Limits, fd_delta, fds, twin_mismatch  # noqa: E402
from suites.repl import DIGEST, catchup, snapshot_oracle, standby  # noqa: E402
from suites.workloads_olap import cancels  # noqa: E402

V6_OPS = ('SELECT DISTINCT ON (grp) grp, id, v FROM e_col ORDER BY grp, v DESC, id',
          'SELECT grp, percentile_cont(0.5) WITHIN GROUP (ORDER BY v), percentile_disc(0.9) WITHIN GROUP (ORDER BY v) FROM e_col GROUP BY grp ORDER BY grp',
          'SELECT grp % 8, mode() WITHIN GROUP (ORDER BY v) FROM e_col GROUP BY 1 ORDER BY 1',
          'SELECT id, count(*) OVER (PARTITION BY grp), sum(v) OVER (PARTITION BY grp) FROM e_col ORDER BY id DESC LIMIT 8',
          'SELECT d.g, x.n, x.s FROM generate_series(0, 63, 9) d(g) LEFT JOIN LATERAL (SELECT count(*) n, sum(v) s FROM e_col WHERE grp = d.g) x ON true ORDER BY 1',
          "SELECT d.name, count(*), sum(e.v) FROM e_col e JOIN nd d USING (grp) WHERE d.name LIKE 'n1%' GROUP BY 1 ORDER BY 1",
          'WITH s AS (SELECT grp, sum(v) v FROM e_col GROUP BY grp) SELECT a.grp, a.v, b.v FROM s a JOIN s b ON b.grp = (a.grp + 1) % 64 ORDER BY 1',
          "SELECT 'a', count(*), sum(v) FROM e_col WHERE grp < 32 UNION ALL SELECT 'b', count(*), sum(v) FROM e_col WHERE grp >= 32")


def olap_v6_ops_tight_mem(ctx):
    """6 analytic sessions at work_mem 512kB cycle the operators of the V6 executor (DISTINCT ON, percentile_cont, mode(), an unsorted window, LEFT JOIN LATERAL, a join
    with a filtered dimension, a CTE read twice, a UNION ALL of two aggregates) and are cancelled at random beside 2 writers of the three engines: every operator answers in one
    snapshot what the heap does, the memory of their backends after the load and the temporary files left"""
    db = ctx.db
    engines(db)
    db.run("CREATE TABLE nd (grp int, name text); INSERT INTO nd SELECT g, 'n' || g FROM generate_series(0, 63) g; ANALYZE nd")
    ws = ctx.sessions(9, "SET work_mem = '512kB'")

    def olap(w, i):
        w.pid
        w.do(V6_OPS[(w.seed + i) % len(V6_OPS)], KILLED)

    ctx.phase({'r': (ws[:6], olap)}, 0.4)
    before = [ctx_kb(w) for w in ws[:6]], ctx.rss(ws[:6])
    busy = ctx.phase({'w': (ws[6:8], ledger_writes('e')), 'r': (ws[:6], olap), 'c': (ws[8:], cancels(ws[:6]))}, 1.8)
    after = [ctx_kb(w) for w in ws[:6]], ctx.rss(ws[:6])
    db.run("SET work_mem = '512kB'")
    parity = sum(oracle(db, q, 'sift.enable_agg') for q in V6_OPS)
    db.run('RESET work_mem')
    return base(ctx, busy, 'w', **add(multi_kept(db, 'e', ws[6:8]), {'oracle_mismatch': oracles(db) + parity}), cancelled=busy['r'].expected, olap=busy['r'].ops,
                **mem_delta(ctx, before, after))


SPLIT = "SELECT sift._test_f2_split('d_tbl', part_id, key_lo, {k}) FROM sift._test_f2_partitions('d_tbl') WHERE key_lo IS NULL AND key_hi IS NULL"
UPPER = 2 * 10 ** 8
REWRITE = ('VACUUM FULL d_tbl', 'CLUSTER d_tbl USING d_tbl_pkey')
SIDE = ('TRUNCATE d_x; INSERT INTO d_x SELECT * FROM load_base_fact WHERE id <= 256',
        'ALTER TABLE d_x SET UNLOGGED; INSERT INTO d_x SELECT id + 10000, grp, v, body FROM load_base_fact WHERE id <= 64; ALTER TABLE d_x SET LOGGED')
FILL = 'INSERT INTO {t} SELECT {k} + g, g % 64, g, $$wire upper$$ FROM generate_series(1, 64) g'
MERGE = ("SELECT sift._test_f2_merge('d_tbl', a.part_id, a.key_lo, b.part_id, b.key_lo) FROM sift._test_f2_partitions('d_tbl') a, sift._test_f2_partitions('d_tbl') b "
         'WHERE a.key_lo IS NULL AND b.key_lo IS NOT NULL ORDER BY b.key_lo LIMIT 1')


def held_twin(w, a: str, b: str) -> int:
    """twin_mismatch in a worker whose REPEATABLE READ snapshot is taken on another table a moment before it reads A and B (a rewrite of A may pass in between)"""
    out = w.do(snap(*('; '.join(q.format(t=t) for q in TWIN) for t in (a, b)), 'SELECT count(*) FROM d_snap; SELECT pg_sleep(0.03); '), KILLED)
    return halves_differ(None if out is None else out[2:])


def ddl_rewrite_split_merge(ctx):
    """4 writers insert, update and delete rows of a keyed sift_table and of its heap twin in one transaction, 2 readers compare them in a REPEATABLE READ snapshot taken before
    they read, while 1 session splits and merges the partitions of the table (with the testing library), rewrites it by VACUUM FULL and CLUSTER, and truncates, refills and
    switches to UNLOGGED and back a second sift_table: the twin agrees in every snapshot, every committed row kept once"""
    db = ctx.db
    db.run(f"{tbl('d_tbl')}; SELECT sift.table_set_option('d_tbl', 'key_order', 'id'); CREATE TABLE d_tw AS SELECT * FROM d_tbl; VACUUM d_tbl; "
           f"{tbl('d_x', 512)}; CREATE TABLE d_snap (k int); INSERT INTO d_snap VALUES (1)")
    ws = ctx.sessions(7)

    def read(w, i):
        w.ledger += held_twin(w, 'd_tbl', 'd_tw')

    def ddl(w, i):
        k = UPPER + i * 1000
        fill = f"BEGIN; {FILL.format(t='d_tbl', k=k)}; {FILL.format(t='d_tw', k=k)}; COMMIT"
        other = ((SPLIT.format(k=k), fill, f'VACUUM d_tbl; {MERGE}') if ctx.testing else (fill,)) + SIDE
        step = (REWRITE[i // 2 % 2], other[i // 2 % len(other)])[i % 2]
        w.ledger += w.do(step, ('55000',)) is not None and '_test_f2_' in step
        time.sleep(0.05)

    busy = ctx.phase({'w': (ws[:4], ledger_writes('d', ('tbl', 'tw'))), 'r': (ws[4:6], read), 'd': (ws[6:], ddl)}, 2.0)
    x = int(db.val('SELECT count(*) - count(DISTINCT id) FROM d_x'))
    return base(ctx, busy, 'w', **add(multi_kept(db, 'd', ws[:4], ('tbl', 'tw')), {'dup_rows': x}),
                oracle_mismatch=sum(w.ledger for w in ws[4:6]) + twin_mismatch(db, 'd_tbl', 'd_tw'), ddl=busy['d'].ops, splits_merges=ws[6].ledger, snapshots=busy['r'].ops)


KEYS = 512
UPSERT = ('INSERT INTO u_ch VALUES ({k}, {p}, 1) ON CONFLICT (id) DO UPDATE SET v = u_ch.v + 1',
          'MERGE INTO u_ch c USING (SELECT {k}::int8 AS id) s ON c.id = s.id WHEN MATCHED THEN UPDATE SET v = c.v + 1 WHEN NOT MATCHED THEN INSERT VALUES ({k}, {p}, 1)')
REST = ("INSERT INTO u_col VALUES ({k}, 1) ON CONFLICT (k) DO UPDATE SET v = u_col.v + 1; INSERT INTO u_bm VALUES ({k}, 'wire k{k}') ON CONFLICT (body) DO NOTHING; "
        'UPDATE u_par SET n = n + 1 WHERE id = {p}')
ROWLOCKS = 'SELECT in_chunks + chunks FROM sift._test_rowlock_stats()'


def conc_upsert_fk_unique(ctx):
    """6 sessions upsert a Zipf-hot key in one transaction into a child sift_table (ON CONFLICT DO UPDATE, or MERGE) whose foreign key locks its parent FOR KEY SHARE, into a
    table with a UNIQUE sift_col index (ON CONFLICT DO UPDATE) and one with a UNIQUE BM25 index (ON CONFLICT DO NOTHING), and count it on the parent; 1 session tries to delete
    referenced parents and adds and drops parents of its own: every committed upsert counted once in every table, no key twice, no orphan child, no deadlock, no row lock left"""
    db = ctx.db
    db.run('CREATE TABLE u_par (id int8 PRIMARY KEY, n int8) USING sift_table; INSERT INTO u_par SELECT g, 0 FROM generate_series(1, 64) g; '
           'CREATE TABLE u_ch (id int8 PRIMARY KEY, par int8 NOT NULL REFERENCES u_par (id), v int8) USING sift_table; '
           'INSERT INTO u_ch SELECT 100000 + g, g, 0 FROM generate_series(1, 64) g; '
           'CREATE TABLE u_col (k int8, v int8); CREATE UNIQUE INDEX u_col_ix ON u_col USING sift_col (k); '
           'CREATE TABLE u_bm (id int8, body text); CREATE UNIQUE INDEX u_bm_ix ON u_bm USING sift (body)')
    ws = ctx.sessions(7)

    def upsert(w, i):
        k = zipf(w.seed * 7919 + i, KEYS, 1.1)
        sql = f'BEGIN; {UPSERT[w.seed % 2]}; {REST}; COMMIT'.format(k=k, p=1 + k % 64)
        if w.do(sql, ('23505',) if w.seed % 2 else ()) is not None:
            w.ledger += 1
            w.ids = getattr(w, 'ids', set()) | {k}

    def parents(w, i):
        p = 10 ** 6 + i
        out = w.do(f'DELETE FROM u_par WHERE id = {1 + i % 64} RETURNING 1', ('23503',))
        w.ledger += len(out or [])
        w.do(f"BEGIN; INSERT INTO u_par VALUES ({p}, 0); INSERT INTO u_ch VALUES ({p}, {p}, 0); DELETE FROM u_ch WHERE id = {p}; DELETE FROM u_par WHERE id = {p}; COMMIT")

    busy = ctx.phase({'w': (ws[:6], upsert), 'p': (ws[6:], parents)}, 2.0)
    ups, keys = sum(w.ledger for w in ws[:6]), set().union(*(getattr(w, 'ids', set()) for w in ws[:6]))
    ctx.close_sessions()
    sums = [int(x or 0) for x in db.row('SELECT (SELECT sum(v) FROM u_ch), (SELECT sum(v) FROM u_col), (SELECT sum(n) FROM u_par)')]
    orphans = int(db.val('SELECT count(*) FROM u_ch c WHERE NOT EXISTS (SELECT 1 FROM u_par p WHERE p.id = c.par)'))
    lost = sum(abs(s - ups) for s in sums) + orphans + ws[6].ledger
    dup = {'dup_rows': sum(int(db.val(f'SELECT count(*) - count(DISTINCT {c}) FROM {t}')) for t, c in (('u_col', 'k'), ('u_ch', 'id')))}
    return base(ctx, busy, 'w', **add(*(kept(db, t, keys, 1, KEYS + 1) for t in ('u_ch', 'u_bm')), {'lost_rows': lost}, dup), rowlock_left=int(db.val(ROWLOCKS)) if ctx.testing else None,
                oracle_mismatch=oracle(db, 'SELECT count(*), sum(v) FROM u_col WHERE k BETWEEN 1 AND 256', 'sift.enable_agg') + oracle(db, BM.format(t='u_bm'), 'sift.enable_scan'),
                upserts=ups, conflicts=busy['w'].expected)


CACHE = 'pgsql_tmp_sift_tier.'
COLD = 20000
SPAN = 2500
PEEK = ('SELECT grp, count(*), sum(v), max({p}) FROM {t} WHERE id BETWEEN {lo} AND {hi} GROUP BY grp ORDER BY grp',
        'SELECT count(*), sum(v), min({p}) FROM {t} WHERE id BETWEEN {lo} AND {hi} AND id % 7 = 3')


def peek(i: int) -> str:
    """statement I of a reader: SPAN rows of the cold part (about 2.7 MB of the store) read from the sift_table and its twin in one snapshot"""
    q, lo = PEEK[i % 2], i * SPAN % COLD
    return snap(q.format(t='tc', p='md5(pad)', lo=lo, hi=lo + SPAN), q.format(t='tc_tw', p='pad', lo=lo, hi=lo + SPAN))


def cold(t: str, uri: str) -> str:
    """a sift_table T of COLD rows of 2 kB of hex (about 22 MB once moved to the store at URI) and 1024 recent ones; its heap twin T_tw keeps the md5 of each pad"""
    pad = "(SELECT string_agg(md5((g * 64 + j)::text), '') FROM generate_series(1, 64) j)"
    return (f'CREATE TABLE {t} (id int8 PRIMARY KEY, ts timestamp NOT NULL, grp int, v int8, pad text) USING sift_table; '
            f"INSERT INTO {t} SELECT g, timestamp '2020-01-01' + g * interval '1 minute', g % 64, g % 1000, {pad} FROM generate_series(1, {COLD}) g; VACUUM (FREEZE) {t}; "
            f"INSERT INTO {t} SELECT g, localtimestamp, g % 64, g % 1000, {pad} FROM generate_series({COLD + 1}, {COLD + 1024}) g; "
            f"SELECT sift.set_tier_policy('{t}', '{uri}', '30 days'); VACUUM {t}; CREATE TABLE {t}_tw AS SELECT id, ts, grp, v, md5(pad) AS pad FROM {t}")


def cache_kb(store: str) -> int:
    """the bytes in the tier cache directories of the cluster of STORE (the runner counts the .tmp. files left in them)"""
    root = os.path.join(os.path.dirname(store), 'base', 'pgsql_tmp')
    names = [os.path.join(root, d, f) for d in os.listdir(root) if d.startswith(CACHE) for f in os.listdir(os.path.join(root, d))] if os.path.isdir(root) else []
    return sum(os.path.getsize(p) for p in names if os.path.exists(p)) // 1024


def feat_tier_cache_evict_storm(ctx):
    """a tiered sift_table whose cold parts (about 22 MB) are larger than the 16 MB cache of its policy: 6 sessions read the cold and the warm rows and compare them with a
    heap twin in one snapshot, 1 cancels them while they fill the cache, 1 writer updates rows, 1 session rebinds the policy: the twin agrees, no backend keeps objects of the store open
    (fd_delta), no temporary file and no half-written chunk is left in the cache"""
    db = ctx.db
    store = ctx.store('load_tier_cache')
    uri = 'file://' + store
    db.run(f"{cold('tc', uri)}; UPDATE sift.tier_policies SET cache_mb = 16 WHERE relid = 'tc'::regclass")
    moved = int(db.val("SELECT count(*) FROM sift.tier_status WHERE relation = 'tc'::regclass"))
    ws = ctx.sessions(9)

    def read(w, i):
        w.pid
        w.ledger += halves_differ(w.do(peek(w.seed * 3 + i), KILLED))

    def write(w, i):
        k = 1 + (i * 2741) % (COLD + 1024)
        w.do(f'BEGIN; UPDATE tc SET v = v + 1 WHERE id = {k}; UPDATE tc_tw SET v = v + 1 WHERE id = {k}; COMMIT')

    def rebind(w, i):
        time.sleep(0.2)
        w.do(f"SELECT sift.set_tier_policy('tc', '{uri}', '{30 + i % 2 * 10} days'); UPDATE sift.tier_policies SET cache_mb = 16 WHERE relid = 'tc'::regclass")

    f0 = [fds(w.pid, store) for w in ws]
    busy = ctx.phase({'r': (ws[:6], read), 'c': (ws[6:7], cancels(ws[:6])), 'w': (ws[7:8], write), 'm': (ws[8:], rebind)}, 2.0)
    f1 = [fds(w.pid, store) for w in ws]
    ctx.close_sessions()
    twin = sum(w.ledger for w in ws[:6]) + sum(halves_differ(db.run(peek(i))) for i in range(0, COLD // SPAN + 1, 3))
    return base(ctx, busy, 'r', oracle_mismatch=twin, fd_delta=fd_delta(f0, f1), tiered=moved, cancelled=busy['r'].expected, cache_kb=cache_kb(store))


HIGH = 256
PROCNO = 'SELECT b - 1 FROM pg_stat_get_backend_idset() b WHERE pg_stat_get_backend_pid(b) = pg_backend_pid()'
FREE = "SELECT current_setting('max_connections')::int - count(*) FROM pg_stat_activity WHERE backend_type = 'client backend'"
IDLE = "SELECT count(*) - {n} FROM pg_stat_activity WHERE application_name LIKE 'load_widle%'"
POOLS = 'SELECT coalesce(sum(refusals), 0) FROM sift.shared_memory_pools()'


def idle_session(ctx, i: int):
    """an idle session of the pool (one of the open sessions of CTX), connected when it returns, and its process number"""
    s = type(ctx.db)(app=f'load_widle{i}')
    ctx.open_sessions.append(s)
    return s, int(s.val(PROCNO))


def high_procnos(ctx) -> int:
    """fills every free slot of the pool with idle sessions, then closes those on process numbers HIGH and above: the free list holds only those, so the next sessions take them;
    the idle sessions kept"""
    db = ctx.db
    with ThreadPoolExecutor(16) as ex:
        idle = list(ex.map(lambda i: idle_session(ctx, i), range(int(db.val(FREE)))))
    keep = [s for s, n in idle if n < HIGH]
    for s, n in idle:
        if n >= HIGH:
            s.close()
            ctx.open_sessions.remove(s)
    until(db, IDLE.format(n=len(keep)), 30.0)
    return len(keep)


def pool_high_procno_shm(ctx):
    """idle sessions hold the process numbers below 256 of a pool of 320 connections, so that 12 workers run on process numbers 256 and above: 10 commit rows of a sift_table and
    a sift_col table with a savepoint and lock hot rows FOR UPDATE, 2 vacuum and maintain them (the combiner, the manifest, the leases and the row locks indexed by process
    number): every acknowledged row kept, the tables agree, no refusal of the shared memory, no session left waiting"""
    db = ctx.db
    engines(db, kinds=('tbl', 'col'))
    r0 = int(db.val(POOLS))
    idle = high_procnos(ctx)
    ws = ctx.sessions(12)
    low = sum(int(w.val(PROCNO)) < HIGH for w in ws)
    if low:
        raise RuntimeError(f'{low} workers below process number {HIGH}: the idle sessions did not take the low ones')

    def write(w, i):
        k = nxt(w)
        hot = 1 + zipf(w.seed * 7919 + i, 64, 1.1)
        sql = (f"BEGIN; {row3('e_tbl', k)}; SAVEPOINT a; {row3('e_col', k)}; UPDATE e_tbl SET v = v + 1 WHERE id = {k}; RELEASE a; "
               f'SELECT id FROM e_tbl WHERE id = {hot} FOR UPDATE; COMMIT')
        if w.do(sql) is not None:
            w.ids = getattr(w, 'ids', set()) | {k}

    def maint(w, i):
        w.do(("VACUUM e_tbl; SELECT count(*) FROM sift.maintain('e_tbl')", "VACUUM e_col; SELECT count(*) FROM sift.maintain('e_col_ix')")[w.seed % 2])

    busy = ctx.phase({'w': (ws[:10], write), 'm': (ws[10:], maint)}, 2.0)
    ids = set().union(*(getattr(w, 'ids', set()) for w in ws[:10]))
    ctx.close_sessions()
    wedged = int(db.val("SELECT count(*) FROM pg_stat_activity WHERE application_name LIKE 'load_w%' OR (backend_type = 'client backend' AND wait_event LIKE 'Sift%')"))
    return base(ctx, busy, 'w', **add(kept(db, 'e_tbl', ids, 10 ** 6), kept(db, 'e_col', ids, 10 ** 6)), shm_refusals=int(db.val(POOLS)) - r0, wedged=wedged,
                rowlock_left=int(db.val(ROWLOCKS)) if ctx.testing else None, oracle_mismatch=oracle(db, AGG.format(t='e_col'), 'sift.enable_agg'), idle=idle)


STANDBY_CONF = ("ALTER SYSTEM SET max_standby_streaming_delay = '50ms'", 'ALTER SYSTEM SET hot_standby_feedback = off')


def repl_standby_conflict_storm(ctx):
    """a hot standby that gives the recovery precedence after 50 ms and sends no feedback: on the primary 3 writers of the three engines, 1 session that vacuums, maintains and
    expires rows of a sift_table by TTL, and a REPEATABLE READ snapshot taken again and again, while 4 REPEATABLE READ sessions of the standby compare the index paths with the
    heap and are cancelled by the conflicts with the recovery: no answer of the standby differs (an answer cancelled is no mismatch), no corruption, and after a catch-up the
    standby answers what the primary does"""
    db, conn = ctx.db, standby()
    engines(db)
    ttl = ctx.fmt == 2
    if ttl:
        db.run("CREATE TABLE r_ttl (id int8 PRIMARY KEY, ts timestamp NOT NULL, v int8) USING sift_table; "
               "INSERT INTO r_ttl SELECT g, CASE WHEN g % 2 = 0 THEN timestamp '2020-01-01' ELSE localtimestamp END, g FROM generate_series(1, 4096) g; VACUUM r_ttl")
    st = ctx.sessions(1, conn=conn)[0]
    for s in STANDBY_CONF:
        st.run(s)
    st.run('SELECT pg_reload_conf()')
    try:
        lag = catchup(db, st)
        ws = ctx.sessions(5)
        rs = ctx.sessions(4, conn=conn)

        def maint(w, i):
            steps = ('VACUUM e_tbl, e_col, e_bm', "SELECT count(*) FROM sift.maintain('e_col_ix')", "SELECT count(*) FROM sift.maintain('e_tbl')")
            steps += (f"SELECT sift.set_ttl('r_ttl', 'ts', interval '{30 + i % 2} days'); VACUUM r_ttl",) if ttl else ()
            w.do(steps[i % len(steps)])

        def snap(w, i):
            w.do('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM e_tbl; SELECT pg_sleep(0.1); COMMIT')

        def read(w, i):
            w.ledger += snapshot_oracle(w, (AGG.format(t='e_col'), BM.format(t='e_bm'), 'SELECT count(*), sum(v) FROM e_tbl WHERE id % 7 = 3')[i % 3],
                                        ('sift.enable_agg', 'sift.enable_scan', 'sift.enable_scan')[i % 3])

        busy = ctx.phase({'w': (ws[:3], ledger_writes('e')), 'm': (ws[3:4], maint), 's': (ws[4:5], snap), 'r': (rs, read)}, 2.0)
        lag += catchup(db, st)
        differ = sum(db.run(q) != st.run(q) for q in DIGEST + (("SELECT count(*), sum(v) FROM load_case.r_ttl",) if ttl else ()))
        return base(ctx, busy, 'w', **add(multi_kept(db, 'e', ws[:3]), checks(st), {'oracle_mismatch': oracles(db)}),
                    standby_mismatch=differ + lag + sum(w.ledger for w in rs), conflicts=busy['r'].expected, standby_reads=busy['r'].ops)
    finally:
        st.q('ALTER SYSTEM RESET max_standby_streaming_delay; ALTER SYSTEM RESET hot_standby_feedback; SELECT pg_reload_conf()')


WORKLOADS = (
    LoadCase('olap_v6_ops_tight_mem', 'olap', olap_v6_ops_tight_mem, Limits(ctx_kb_delta=1280, rss_mb_delta=16)),
    LoadCase('ddl_rewrite_split_merge', 'ddl', ddl_rewrite_split_merge, Limits(), formats=(2,)),
    LoadCase('conc_upsert_fk_unique', 'conc', conc_upsert_fk_unique, Limits(), formats=(2,), open='S111-34', intermittent=True),
    LoadCase('feat_tier_cache_evict_storm', 'feat', feat_tier_cache_evict_storm, Limits(fd_delta=0), formats=(2,)),
    LoadCase('pool_high_procno_shm', 'pool', pool_high_procno_shm, Limits(), formats=(2,)),
)
REPL = (
    LoadCase('repl_standby_conflict_storm', 'repl', repl_standby_conflict_storm, Limits()),
)
