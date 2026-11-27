"""The sixth wave of production shapes of the load gate (Spec 118, test/t/core/load_stress.sh and load_repl.sh): parts dropped by retention and a table re-keyed under live
writers, shredded JSONB of drifting shapes under tight analytics, connected indexes two hops deep beside an unprivileged role, VACUUM at the least maintenance_work_mem
beside OLTP writers, 64 pooled sessions under row-level security with prepared statements and DISCARD ALL, and logical replication of a schema of every engine into a
subscriber read while it applies. Every case runs on Format 2.0 and Format 1.0 and on the release library; what only Format 2.0 has (dropping parts, the per-table options
paths and connect, projections) joins on Format 2.0. One case waits for a finding, intermittent (met on Format 1.0): the shredded JSONB for S118-23 (on Format 2.0 a
sift_col index answers doc @> '{"n": 3}' without the documents whose n is a JSON 3.0, a number with a fraction of zeros: 40 rows where the heap has 41, with or without
paths, before and after maintenance; a sift_table and Format 1.0 answer as the heap does).
"""
from __future__ import annotations

import time
from dataclasses import dataclass

from cases import KILLED, LoadCase
from collect import AGG, BM, add, base, checks, ctx_kb, engines, halves_differ, kept, mem_delta, multi_kept, nxt, oracle, oracles, p99_ratio, row3, snap, zipf
from suites.common import Limits
from suites.repl import CAP, conninfo
from suites.workloads_olap import cancels
from suites.workloads_prod2 import snap_oracle, snap_pair
from suites.workloads_prod3 import dups, settled
from suites.workloads_prod4 import paced, pools


@dataclass(frozen=True)
class Wave6(Limits):
    acl_leaks: int | None = 0
    torn_days: int | None = 0


D_DAYS, D_DAY = 8, 512
D_AT = "timestamptz '2030-01-01' + {d} * interval '1 day'"
D_TORN = f'SELECT count(*) FROM (SELECT count(*) c FROM d_tbl WHERE id <= {D_DAYS * D_DAY} GROUP BY at::date) s WHERE c <> {D_DAY}'


def d_setup() -> str:
    """a sift_table of D_DAYS days of D_DAY rows, a day inserted and sealed by its own VACUUM (FREEZE): a part a day"""
    days = ''.join(f'INSERT INTO d_tbl SELECT g, g % 64, g % 1000, {D_AT.format(d=d)} FROM generate_series({d * D_DAY + 1}, {(d + 1) * D_DAY}) g; VACUUM (FREEZE) d_tbl; '
                   for d in range(D_DAYS))
    return f'CREATE TABLE d_tbl (id int8 PRIMARY KEY, grp int, v int8, at timestamptz NOT NULL) USING sift_table; {days}ANALYZE d_tbl'


def d_steps(fmt: int, cut: int) -> tuple:
    drop = (f"SELECT parts_dropped FROM sift.drop_parts_before('d_tbl', {D_AT.format(d=cut)})",) if fmt == 2 else ()
    return drop + ("SELECT sift.table_set_option('d_tbl', 'key_order', 'grp, id')", "SELECT count(*) FROM sift.maintain('d_tbl')",
                   'BEGIN; ALTER TABLE d_tbl DROP CONSTRAINT d_tbl_pkey; ALTER TABLE d_tbl ADD PRIMARY KEY (id); COMMIT', "SELECT sift.table_reset_option('d_tbl', 'key_order')")


def ddl_rekey_drop_parts_live(ctx):
    """a sift_table of 8 days, a part each: 4 writers insert rows of a later day and update their rows before, 2 REPEATABLE READ readers compare the analytic path with the
    heap and count the rows of every old day, while 1 session drops the parts before a cutoff that moves a day at a time (Format 2.0), changes the key order and back, drops
    and adds the primary key in one transaction and maintains: no reader sees a day partly dropped (torn_days), every day at or after the cutoff whole, every committed row
    kept once"""
    db = ctx.db
    db.run(d_setup())
    ws = ctx.sessions(7)

    def write(w, i):
        k = nxt(w)
        w.ids = getattr(w, 'ids', set())
        upd = f'; UPDATE d_tbl SET v = v + 1 WHERE id = {k - 1}' if k - 1 in w.ids else ''
        if w.do(f"BEGIN; INSERT INTO d_tbl VALUES ({k}, {k % 64}, {k % 1000}, timestamptz '2031-01-01'){upd}; COMMIT") is not None:
            w.ids.add(k)

    def read(w, i):
        if i % 2:
            w.ledger += snap_oracle(w, AGG.format(t='d_tbl'), 'sift.enable_agg', ())
        else:
            out = w.do(f'BEGIN ISOLATION LEVEL REPEATABLE READ; {D_TORN}; COMMIT')
            w.torn = getattr(w, 'torn', 0) + int(out is not None and out[0] != '0')

    def ddl(w, i):
        cut = min(D_DAYS // 2, 1 + i // 5)
        steps = d_steps(ctx.fmt, cut)
        if w.do(steps[i % len(steps)]) is not None and ctx.fmt == 2 and i % 5 == 0:
            w.cut = cut
        paced(w)

    busy = ctx.phase({'w': (ws[:4], write), 'r': (ws[4:6], read), 'd': (ws[6:], ddl)}, 3.0)
    ctx.close_sessions()
    db = ctx.sessions(1)[0]
    ids = set().union(*(getattr(w, 'ids', set()) for w in ws[:4]))
    cut = getattr(ws[6], 'cut', 0)
    short = int(db.val(f'SELECT {D_DAYS - cut} - count(DISTINCT at::date) FROM d_tbl WHERE id <= {D_DAYS * D_DAY} AND at >= {D_AT.format(d=cut)}'))
    return base(ctx, busy, 'w', **add(kept(db, 'd_tbl', ids, 10 ** 6), {'lost_rows': short, 'dup_rows': dups(db, 'd_tbl')}),
                oracle_mismatch=oracle(db, AGG.format(t='d_tbl'), 'sift.enable_agg') + sum(w.ledger for w in ws[4:6]),
                torn_days=int(db.val(D_TORN)) + sum(getattr(w, 'torn', 0) for w in ws[4:6]), cutoff=cut, ddl=busy['d'].ops,
                days_dropped=D_DAYS - int(db.val(f'SELECT count(DISTINCT at::date) FROM d_tbl WHERE id <= {D_DAYS * D_DAY}')))


J_TABLES = ('j_col', 'j_tbl', 'j_tw')
J_DOCS = ("jsonb_build_object('n', {k} % 50, 's', 'v' || {k} % 9, 'tags', jsonb_build_array('t' || {k} % 5))",
          "jsonb_build_object('n', ({k} % 50) / 8.0, 'f', ({k} % 7) / 4.0, 's', 'v' || {k} % 9)",
          "jsonb_build_object('n', {k} % 50, 'w', repeat(md5({k}::text), 300), 's', 'w' || {k} % 3)",
          "jsonb_build_object('tags', jsonb_build_array(jsonb_build_array({k} % 3, 't1'), 't' || {k} % 5), 'n', jsonb_build_object('deep', {k} % 4))",
          'NULL')
J_OPS = ("SELECT count(*) FROM {t} WHERE doc @> '{{\"n\": 3}}'", "SELECT count(*) FROM {t} WHERE doc ? 'f'",
         "SELECT count(*) FROM {t} WHERE doc @? '$.tags[*] ? (@ == \"t1\")'",
         "SELECT grp, sift.arg_max(doc->>'s', id), sift.arg_min(doc->>'n', id) FROM {t} GROUP BY grp ORDER BY grp",
         "SELECT doc->>'s', count(*), sum(length(doc::text)) FROM {t} GROUP BY 1 ORDER BY 2 DESC, 1 LIMIT 10",
         "SELECT sum(CASE WHEN jsonb_typeof(doc->'f') = 'number' THEN (doc->>'f')::numeric END), count(doc->'tags') FROM {t}")
J_SHAPE = 'id int8 PRIMARY KEY, grp int, doc jsonb'
J_ROWS = 2048


def j_setup(fmt: int) -> str:
    fill = ' '.join(f'INSERT INTO {t} SELECT g, g % 16, {J_DOCS[0].format(k="g")} FROM generate_series(1, {J_ROWS}) g;' for t in J_TABLES)
    paths = "SELECT sift.table_set_option('j_tbl', 'paths', 'auto'); " if fmt == 2 else ''
    return (f'CREATE TABLE j_col ({J_SHAPE}); CREATE TABLE j_tbl ({J_SHAPE}) USING sift_table; CREATE TABLE j_tw ({J_SHAPE}); {paths}{fill} '
            "CREATE INDEX j_col_ix ON j_col USING sift_col (id, grp, doc) WITH (paths = 'auto'); ANALYZE j_col, j_tbl, j_tw")


def olap_shred_jsonb_argext_churn(ctx):
    """a heap with a sift_col index that shreds its JSONB (paths auto), a sift_table (shredded on Format 2.0) and a heap twin: 2 writers insert and update documents whose
    shape drifts (integers, fractions, wide unshreddable values above 8 kB, nested arrays, NULL) in all three in one transaction, 4 readers at work_mem 256kB (a backend
    cache of 1 MB) run containment, key existence, JSON path, arg_max and arg_min, and a GROUP BY of strings against the twin in one snapshot while 1 cancels them: every
    answer agrees, no temporary file left, the memory of the readers after the load"""
    db = ctx.db
    db.run(j_setup(ctx.fmt))
    ws = ctx.sessions(2) + ctx.sessions(4, "SET work_mem = '256kB'; SET sift.backend_cache_kb = 1024") + ctx.sessions(1)

    def write(w, i):
        k, doc = nxt(w), J_DOCS[i % len(J_DOCS)]
        old = 1 + (w.seed * 101 + i * 37) % J_ROWS
        w.do('BEGIN; ' + '; '.join(f'INSERT INTO {t} VALUES ({k}, {k % 16}, {doc.format(k=k)}); UPDATE {t} SET doc = {J_DOCS[(i + 1) % len(J_DOCS)].format(k=old)} '
                                   f'WHERE id = {old}' for t in J_TABLES) + '; COMMIT')

    def read(w, i):
        q, t = J_OPS[(w.seed + i) % len(J_OPS)], J_TABLES[i % 2]
        w.ledger += snap_pair(w, q.format(t=t), q.format(t='j_tw'), KILLED)

    roles = {'w': (ws[:2], write), 'r': (ws[2:6], read), 'c': (ws[6:], cancels(ws[2:6]))}
    ctx.phase(roles, 0.5)
    before = [ctx_kb(w) for w in ws[2:6]], ctx.rss(ws[2:6])
    busy = ctx.phase(roles, 2.5)
    after = [ctx_kb(w) for w in ws[2:6]], ctx.rss(ws[2:6])
    ctx.close_sessions()
    db = ctx.sessions(1)[0]
    twin = sum(snap_pair(db, q.format(t=t), q.format(t='j_tw'), ()) for q in J_OPS for t in J_TABLES[:2])
    return base(ctx, busy, 'r', oracle_mismatch=twin + sum(w.ledger for w in ws[2:6]) + oracle(db, J_OPS[3].format(t='j_col'), 'sift.enable_agg'),
                writes=busy['w'].ops, cancelled=busy['r'].expected, **mem_delta(ctx, before, after))


CN_SETUP = ('CREATE TABLE cn_reg (id int8 PRIMARY KEY, name text); INSERT INTO cn_reg SELECT g, $$r$$ || g % 4 FROM generate_series(1, 16) g; '
            'CREATE INDEX cn_reg_ix ON cn_reg USING sift_col (id, name); '
            'CREATE TABLE cn_dim (id int8 PRIMARY KEY, reg_id int8, label text); INSERT INTO cn_dim SELECT g, 1 + g % 16, $$l$$ || g % 20 FROM generate_series(1, 300) g; '
            "CREATE INDEX cn_dim_ix ON cn_dim USING sift_col (id, reg_id, label) WITH (connect = 'reg_id -> cn_reg(id)'); "
            'CREATE TABLE cn_fact (id int8 PRIMARY KEY, dim_id int8, v int8); INSERT INTO cn_fact SELECT g, CASE WHEN g % 37 = 0 THEN NULL ELSE 1 + g % 320 END, g % 10 '
            "FROM generate_series(1, 4096) g; CREATE INDEX cn_fact_ix ON cn_fact USING sift_col (id, dim_id, v) WITH (connect = 'dim_id -> cn_dim(id)'); "
            'ANALYZE cn_reg, cn_dim, cn_fact')
CN_TBL = ("CREATE TABLE cn_tbl (id int8 PRIMARY KEY, dim_id int8, v int8) USING sift_table; SELECT sift.table_set_option('cn_tbl', 'connect', 'dim_id -> cn_dim(id)'); "
          'INSERT INTO cn_tbl SELECT * FROM cn_fact; ANALYZE cn_tbl')
CN_JOINS = ('SELECT d.label, count(*), sum(f.v) FROM {f} f JOIN cn_dim d ON d.id = f.dim_id GROUP BY d.label ORDER BY d.label',
            'SELECT count(*) FROM {f} f WHERE NOT EXISTS (SELECT 1 FROM cn_dim d WHERE d.id = f.dim_id)',
            'SELECT r.name, count(*), sum(f.v) FROM {f} f JOIN cn_dim d ON d.id = f.dim_id JOIN cn_reg r ON r.id = d.reg_id GROUP BY r.name ORDER BY r.name')
CN_PROBES = ('UPDATE cn_fact SET v = v WHERE id = 1', 'SELECT count(*) FROM cn_fact', "SELECT count(*) FROM sift.maintain('cn_fact_ix')",
             'TRUNCATE cn_dim', 'ALTER TABLE cn_dim ADD COLUMN x int', 'DELETE FROM cn_reg WHERE id = 1', 'BEGIN; LOCK TABLE cn_fact IN SHARE MODE; COMMIT')


def feat_connect_follow_acl_churn(ctx):
    """a heap of facts whose sift_col index is connected to a dimension whose index is connected to regions (two hops; on Format 2.0 also a connected sift_table): 2
    writers insert, re-point and delete facts, 1 relabels, moves, adds and deletes dimensions, 2 readers compare the joins, the anti join and the two-hop rollup with the
    heap in one snapshot, 1 vacuums and maintains, while a role without privileges writes, reads, maintains and alters them under a lock_timeout: every attempt refused
    with 42501 (acl_leaks), none waits for a lock first (unpriv_waits), every committed fact kept, every join answers what the heap does"""
    db = ctx.db
    db.run(CN_SETUP + ('; ' + CN_TBL if ctx.fmt == 2 else ''))
    db.run('DROP ROLE IF EXISTS load_cn_probe; CREATE ROLE load_cn_probe; GRANT USAGE ON SCHEMA load_case TO load_cn_probe')
    facts = ('cn_fact', 'cn_tbl') if ctx.fmt == 2 else ('cn_fact',)
    try:
        ws = ctx.sessions(7)

        def write(w, i):
            w.ids = getattr(w, 'ids', set())
            k, f = nxt(w), facts[i % len(facts)]
            old = sorted(w.ids)[:1] if i % 4 == 3 else []
            drop = ''.join(f'; DELETE FROM {t} WHERE id = {old[0]}' for t in facts) if old else ''
            sql = (f"BEGIN; {'; '.join(f'INSERT INTO {t} VALUES ({k}, {1 + k % 330}, {k % 10})' for t in facts)}; "
                   f'UPDATE {f} SET dim_id = 1 + (dim_id + 7) % 320 WHERE id = {1 + (w.seed * 53 + i * 29) % 4096}{drop}; COMMIT')
            if w.do(sql) is not None:
                w.ids = (w.ids | {k}) - set(old)

        def dims(w, i):
            d = 1 + (i * 37) % 330
            w.do((f"UPDATE cn_dim SET label = 'l' || {i % 20} WHERE id = {d}", f'UPDATE cn_dim SET reg_id = 1 + (reg_id + 3) % 16 WHERE id = {d}',
                  f"INSERT INTO cn_dim VALUES ({d}, {1 + d % 16}, 'n{d % 7}') ON CONFLICT (id) DO NOTHING", f'DELETE FROM cn_dim WHERE id = {d} AND id > 300')[i % 4])

        def read(w, i):
            w.ledger += snap_oracle(w, CN_JOINS[(w.seed + i) % len(CN_JOINS)].format(f=facts[i % len(facts)]), 'sift.enable_agg', ())

        def maint(w, i):
            w.do(('VACUUM cn_fact, cn_dim, cn_reg', "SELECT count(*) FROM sift.maintain('cn_fact_ix')", "SELECT count(*) FROM sift.maintain('cn_dim_ix')")[i % 3])
            time.sleep(0.05)

        def probe(w, i):
            w.do(f"SET ROLE load_cn_probe; SET lock_timeout = '300ms'; {CN_PROBES[i % len(CN_PROBES)]}", ('42501', '55P03'))
            w.ledger += w.last is None
            w.waits = getattr(w, 'waits', 0) + int(bool(w.last) and w.last[0] == '55P03')
            w.run('RESET ROLE; RESET lock_timeout')
            time.sleep(0.02)

        busy = ctx.phase({'w': (ws[:2], write), 'd': (ws[2:3], dims), 'r': (ws[3:5], read), 'm': (ws[5:6], maint), 'u': (ws[6:], probe)}, 2.5)
        ctx.close_sessions()
        chk = ctx.sessions(1)[0]
        mismatch = sum(oracle(chk, q.format(f=f), 'sift.enable_agg') for q in CN_JOINS for f in facts) + sum(w.ledger for w in ws[3:5])
        ids = set().union(*(getattr(w, 'ids', set()) for w in ws[:2]))
        return base(ctx, busy, 'w', **add(*(kept(chk, f, ids, 10 ** 6) for f in facts)), oracle_mismatch=mismatch, acl_leaks=ws[6].ledger,
                    unpriv_waits=getattr(ws[6], 'waits', 0), refused=busy['u'].expected, dim_edits=busy['d'].ops)
    finally:
        ctx.close_sessions()
        db.run('DROP OWNED BY load_cn_probe; DROP ROLE load_cn_probe')


B_HOT = 512
B_SUM = "SELECT coalesce(sum(v), 0) FROM e_{e} WHERE id < 1000000"
B_MAINT = ('VACUUM e_tbl, e_col, e_bm', 'VACUUM (FREEZE) e_tbl, e_col, e_bm', "SELECT count(*) FROM sift.maintain('e_tbl')", "SELECT count(*) FROM sift.maintain('e_col_ix')",
           "SELECT count(*) FROM sift.maintain('e_bm_ix')")
B_PENDING = "SELECT (SELECT pending_blocks FROM sift.space('e_tbl')) + (SELECT pending_blocks FROM sift.space('e_col_ix')) + (SELECT pending_blocks FROM sift.space('e_bm_ix'))"


def bp_low_mwm_vacuum_oltp_iso(ctx):
    """a sift_table, a sift_col and a BM25 table with 40% of their rows dead: 4 writers insert rows and update Zipf-hot rows of all three in one transaction, alone and then
    beside 1 session at maintenance_work_mem 1MB that runs VACUUM, VACUUM (FREEZE) and sift.maintain of the three without a pause: every insert and every increment kept,
    the index paths answer what the heap does, the p99 of the writers beside the maintenance within 3 times their p99 alone, nothing pending once maintained"""
    db = ctx.db
    engines(db)
    db.run('; '.join(f'DELETE FROM e_{e} WHERE id % 5 < 2' for e in ('tbl', 'col', 'bm')))
    s0 = [int(db.val(B_SUM.format(e=e))) for e in ('tbl', 'col', 'bm')]
    ws = ctx.sessions(4) + ctx.sessions(1, "SET maintenance_work_mem = '1MB'")
    hot = [k for k in range(1, 4 * B_HOT) if k % 5 >= 2][:B_HOT]

    def write(w, i):
        w.ids, k, h = getattr(w, 'ids', set()), nxt(w), hot[zipf(w.seed * 7919 + i, B_HOT, 1.2) - 1]
        if w.do('BEGIN; ' + '; '.join(f'{row3(f"e_{e}", k)}; UPDATE e_{e} SET v = v + 1 WHERE id = {h}' for e in ('tbl', 'col', 'bm')) + '; COMMIT') is not None:
            w.ids.add(k)
            w.ledger += 1

    def maint(w, i):
        w.do(B_MAINT[i % len(B_MAINT)])

    quiet = ctx.phase({'w': (ws[:4], write)}, 1.0)
    busy = ctx.phase({'w': (ws[:4], write), 'm': (ws[4:], maint)}, 2.5)
    ctx.close_sessions()
    db = ctx.sessions(1)[0]
    incs = sum(w.ledger for w in ws[:4])
    drift = sum(abs(int(db.val(B_SUM.format(e=e))) - s - incs) for e, s in zip(('tbl', 'col', 'bm'), s0))
    db.run("VACUUM e_tbl, e_col, e_bm; SELECT count(*) FROM sift.maintain('e_tbl'); SELECT count(*) FROM sift.maintain('e_col_ix'); "
           "SELECT count(*) FROM sift.maintain('e_bm_ix'); VACUUM e_tbl, e_col, e_bm")
    m = add(multi_kept(db, 'e', ws[:4]), {'lost_rows': drift})
    return base(ctx, busy, 'w', **m, oracle_mismatch=oracles(db), p99_ratio=p99_ratio(quiet['w'].lat, busy['w'].lat), pending_after=int(db.val(B_PENDING)),
                maint=busy['m'].ops)


P_TENANTS = 4
P_SHAPE = 'id int8 PRIMARY KEY, tenant int, grp int, v int8, body text'
P_READS = {'tbl': 'SELECT grp, count(*), sum(v) FROM p_tbl WHERE grp < 16{f} GROUP BY grp ORDER BY grp',
           'col': 'SELECT grp, count(*), sum(v) FROM p_col WHERE grp < 16{f} GROUP BY grp ORDER BY grp',
           'bm': "SELECT count(*), sum(id) FROM p_bm WHERE body @@> 'wire'{f}"}
P_GUC = {'tbl': 'sift.enable_agg', 'col': 'sift.enable_agg', 'bm': 'sift.enable_scan'}
P_INVAL = ('ANALYZE p_tbl, p_col, p_bm', 'ALTER TABLE p_col ADD COLUMN x int', 'ALTER TABLE p_tbl ADD COLUMN x int', 'ALTER TABLE p_bm ADD COLUMN x int',
           'ALTER TABLE p_col DROP COLUMN x', 'ALTER TABLE p_tbl DROP COLUMN x', 'ALTER TABLE p_bm DROP COLUMN x')
P_RESET = 'DISCARD ALL; SET search_path = load_case, public; SET sift.backend_cache_kb = 512'


def p_setup() -> str:
    rows = "SELECT g, g % 4, g % 64, g % 1000, 'wire t' || g % 97 FROM generate_series(1, 8192) g"
    tables = (f'CREATE TABLE p_tbl ({P_SHAPE}) USING sift_table; CREATE TABLE p_col ({P_SHAPE}); CREATE TABLE p_bm ({P_SHAPE}); '
              f'INSERT INTO p_tbl {rows}; INSERT INTO p_col {rows}; INSERT INTO p_bm {rows}; CREATE INDEX p_col_ix ON p_col USING sift_col (id, tenant, grp, v); '
              'CREATE INDEX p_bm_ix ON p_bm USING sift (body); ANALYZE p_tbl, p_col, p_bm')
    roles = ''.join(f'DROP ROLE IF EXISTS load_t{k}; CREATE ROLE load_t{k}; GRANT USAGE ON SCHEMA load_case TO load_t{k}; GRANT SELECT ON p_tbl, p_col, p_bm TO load_t{k}; '
                    for k in range(P_TENANTS))
    rls = ''.join(f"ALTER TABLE {t} ENABLE ROW LEVEL SECURITY; CREATE POLICY {t}_t ON {t} USING (tenant = substr(current_user, 7)::int); " for t in ('p_tbl', 'p_col', 'p_bm'))
    return tables + '; ' + roles + rls


def tenant_read(e: str, k: int) -> tuple:
    """the read of tenant K of engine E prepared under its role by the index path, and the same rows read by the owner from the heap with the filter of the policy"""
    q = P_READS[e]
    return (f'SET LOCAL ROLE load_t{k}; SET LOCAL {P_GUC[e]} = on; PREPARE q AS {q.format(f="")}; EXECUTE q; DEALLOCATE q',
            f'SET LOCAL ROLE postgres; SET LOCAL {P_GUC[e]} = off; {q.format(f=f" AND tenant = {k}")}')


def pool_64sess_prep_rls_discard(ctx):
    """a sift_table, a sift_col and a BM25 table under row-level security of 4 tenants: 64 pooled sessions (a backend cache of 512 kB) take the role of a tenant, prepare and
    run its read of an engine, compare it with the rows the policy gives in one snapshot and DISCARD ALL, beside 1 writer of the three and 1 session that analyzes them and
    adds and drops a column of each (invalidating every plan): every tenant read answers what the policy gives, nothing refused by the shared memory, the memory of the
    pooled backends after the load"""
    db = ctx.db
    db.run(p_setup())
    try:
        r0 = sum(r for _, r in pools(db).values())
        ws = ctx.sessions(64, 'SET sift.backend_cache_kb = 512') + ctx.sessions(2, "SET lock_timeout = '2s'")
        kinds = tuple(P_READS)

        def read(w, i):
            w.ledger += snap_pair(w, *tenant_read(kinds[(w.seed + i) % 3], (w.seed + i // 3) % P_TENANTS))
            w.do(P_RESET)

        def write(w, i):
            k = nxt(w)
            w.do('BEGIN; ' + '; '.join(f"INSERT INTO p_{e} VALUES ({k}, {k % P_TENANTS}, {k % 64}, {k % 1000}, 'wire n{k % 97}')" for e in kinds) + '; COMMIT')

        def inval(w, i):
            w.do(P_INVAL[i % len(P_INVAL)], ('55P03', '42701', '42703'))
            paced(w)

        probe = ws[:4]
        roles = {'r': (ws[:64], read), 'w': (ws[64:65], write), 'i': (ws[65:], inval)}
        ctx.phase(roles, 0.5)
        before = [ctx_kb(w) for w in probe], ctx.rss(probe)
        busy = ctx.phase(roles, 2.5)
        after = [ctx_kb(w) for w in probe], ctx.rss(probe)
        ctx.close_sessions()
        chk = ctx.sessions(1)[0]
        mismatch = sum(w.ledger for w in ws[:64]) + sum(halves_differ(chk.run(snap(*tenant_read(e, k)))) for e in kinds for k in range(P_TENANTS))
        return base(ctx, busy, 'r', oracle_mismatch=mismatch, shm_refusals=sum(r for _, r in pools(db).values()) - r0, invalidations=busy['i'].ops,
                    **mem_delta(ctx, before, after))
    finally:
        ctx.close_sessions()
        db.run('; '.join(f'DROP OWNED BY load_t{k}; DROP ROLE load_t{k}' for k in range(P_TENANTS)))


L_TABLES = ('l_tbl', 'l_col', 'l_bm', 'l_sib')
L_SHAPE = 'id int8 PRIMARY KEY, grp int, v int8, body text'
L_DIGEST = "SELECT count(*), coalesce(md5(string_agg(id || ':' || grp || ':' || v || ':' || body, ',' ORDER BY id)), '') FROM load_case.{t}"
L_READS = ((AGG.format(t='l_col'), 'sift.enable_agg'), (BM.format(t='l_bm'), 'sift.enable_scan'), (AGG.format(t='l_tbl'), 'sift.enable_agg'),
           ('SELECT grp, count(*) FROM l_tbl GROUP BY grp ORDER BY grp', 'sift.enable_agg'))


def l_tables(fmt: int, fill: bool) -> str:
    """the published tables (a sift_table with projections on Format 2.0, a heap with a sift_col index, one with a BM25 index, a sibling sift_table), 4096 rows each (FILL)"""
    ddl = (f'CREATE TABLE load_case.l_tbl ({L_SHAPE}) USING sift_table; CREATE TABLE load_case.l_col ({L_SHAPE}); CREATE TABLE load_case.l_bm ({L_SHAPE}); '
           f'CREATE TABLE load_case.l_sib ({L_SHAPE}) USING sift_table; CREATE INDEX l_col_ix ON load_case.l_col USING sift_col (id, grp, v); '
           'CREATE INDEX l_bm_ix ON load_case.l_bm USING sift (body)')
    proj = ("; SELECT sift.create_projection('load_case.l_tbl', 'l_agg', 'GROUP BY grp MEASURES count(*), sum(v)'); "
            "SELECT sift.create_projection('load_case.l_tbl', 'l_sort', 'ORDER BY grp')") if fmt == 2 else ''
    rows = ''.join(f'; INSERT INTO load_case.{t} SELECT * FROM load_base_fact WHERE id <= 4096' for t in L_TABLES[:3]) if fill else ''
    return ddl + proj + rows


def repl_logical_sub_all_engines_olap(ctx):
    """the schema of the case published (FOR TABLES IN SCHEMA) to a subscriber (another database of the cluster) whose tables carry a sift_table with projections on Format
    2.0, a sift_col and a BM25 index: 2 writers commit rows of all three with a nested savepoint rolled back and now and then a batch of 512, 1 fills and truncates a sibling
    sift_table, while 3 REPEATABLE READ sessions of the subscriber compare its index paths with its heap as it applies: every committed row on the subscriber once caught
    up, every answer agrees, its sift.check clean"""
    db = ctx.db
    if db.val('SHOW wal_level') != 'logical':
        raise RuntimeError('the case needs wal_level = logical')
    db.run(f"{l_tables(ctx.fmt, True)}; DROP PUBLICATION IF EXISTS load_pub_all; CREATE PUBLICATION load_pub_all FOR TABLES IN SCHEMA load_case; "
           "SELECT 1 FROM pg_create_logical_replication_slot('load_all', 'pgoutput'); DROP DATABASE IF EXISTS load_case_lsub WITH (FORCE); CREATE DATABASE load_case_lsub")
    sub, end = ctx.sessions(1, db='load_case_lsub', path='public')[0], time.monotonic() + CAP
    try:
        sub.run(f"CREATE EXTENSION pg_sift; CREATE SCHEMA load_case; {l_tables(ctx.fmt, False)}; CREATE SUBSCRIPTION load_sub_all CONNECTION '{conninfo()}' "
                "PUBLICATION load_pub_all WITH (create_slot = false, slot_name = 'load_all')")
        synced = settled(end, lambda: sub.val("SELECT count(*) FROM pg_subscription_rel WHERE srsubstate <> 'r'") == '0')
        ws, rs = ctx.sessions(3), ctx.sessions(3, db='load_case_lsub')

        def write(w, i):
            w.ids, k = getattr(w, 'ids', set()), nxt(w)
            ins = [row3(t, k) for t in L_TABLES[:3]]
            lost = '; '.join(row3(t, k + 500000) for t in L_TABLES[:3])
            lo = 5 * 10 ** 7 + w.seed * 10 ** 6 + 512 * i
            bulk = '; '.join(f"INSERT INTO {t} SELECT {lo} + g, g % 64, g, 'wire bulk' FROM generate_series(1, 512) g" for t in L_TABLES[:3]) if i % 16 == 15 else ''
            sql = f"BEGIN; {'; '.join(ins)}; SAVEPOINT a; {lost}; SAVEPOINT b; UPDATE l_tbl SET v = v + 1 WHERE id = {k}; ROLLBACK TO a; {bulk or 'SELECT 1'}; COMMIT"
            if w.do(sql) is not None:
                w.ids |= {k} | (set(range(lo + 1, lo + 513)) if bulk else set())

        def sibling(w, i):
            w.do(("INSERT INTO l_sib SELECT g, g % 64, g, 'wire sib' FROM generate_series(1, 256) g", 'TRUNCATE l_sib')[i % 2])
            time.sleep(0.05)

        def read(w, i):
            w.ledger += snap_oracle(w, *L_READS[(w.seed + i) % len(L_READS)], ())

        busy = ctx.phase({'w': (ws[:2], write), 's': (ws[2:], sibling), 'r': (rs, read)}, 2.5)
        both = '; '.join(L_DIGEST.format(t=t) for t in L_TABLES)
        lag = synced or settled(end, lambda: db.run(both) == sub.run(both))
        differ = int(db.run(both) != sub.run(both)) + sum(oracle(rs[0], q, g) for q, g in L_READS)
        ids = set().union(*(getattr(w, 'ids', set()) for w in ws[:2]))
        return base(ctx, busy, 'w', **add(*(kept(db, t, ids, 10 ** 6) for t in L_TABLES[:3])), oracle_mismatch=differ + sum(w.ledger for w in rs), standby_mismatch=lag,
                    standby_reads=busy['r'].ops, **checks(sub))
    finally:
        sub.q('DROP SUBSCRIPTION IF EXISTS load_sub_all')
        ctx.close_sessions()
        db.run('DROP DATABASE IF EXISTS load_case_lsub WITH (FORCE)')
        db.run("SELECT pg_drop_replication_slot(slot_name) FROM pg_replication_slots WHERE slot_name = 'load_all'; DROP PUBLICATION IF EXISTS load_pub_all")


STRESS = (
    LoadCase('ddl_rekey_drop_parts_live', 'ddl', ddl_rekey_drop_parts_live, Wave6()),
    LoadCase('olap_shred_jsonb_argext_churn', 'olap', olap_shred_jsonb_argext_churn, Wave6(ctx_kb_delta=1280, rss_mb_delta=16), open='S118-23', intermittent=True),
    LoadCase('feat_connect_follow_acl_churn', 'feat', feat_connect_follow_acl_churn, Wave6()),
    LoadCase('bp_low_mwm_vacuum_oltp_iso', 'bp', bp_low_mwm_vacuum_oltp_iso, Wave6(lwlock_wait_pct=30, p99_ratio=3.0, pending_after=0)),
    LoadCase('pool_64sess_prep_rls_discard', 'pool', pool_64sess_prep_rls_discard, Wave6(ctx_kb_delta=1280, rss_mb_delta=16, lwlock_wait_pct=45)),
)
REPL = (
    LoadCase('repl_logical_sub_all_engines_olap', 'repl', repl_logical_sub_all_engines_olap, Wave6()),
)
