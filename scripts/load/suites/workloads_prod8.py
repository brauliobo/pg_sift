"""The eighth wave of production shapes of the load gate (Spec 120, test/t/core/load_stress.sh and load_repl.sh): partial sift_col and BM25 indexes beside a partial
expression btree, a trigram skip index and top-N WITH TIES while updates move rows across the predicate, a two-level subpartitioned table of sift_table and heap leaves
under cascading foreign keys while a leaf is detached concurrently and attached again and CHECK and NOT NULL constraints come and go, BM25 indexes of non-default
analyzers (an English stemmer with accent folding, prefixes, no positions, a keyword analyzer) beside explicit promotes and merges, rich types (uuid, inet, numeric,
date, timestamptz, bytea, arrays, a domain, a builtin collation) under live codec changes and TABLESAMPLE, pooled unprivileged sessions calling SECURITY DEFINER
functions under a hostile search_path and advisory locks beside refused privilege probes, and a hot standby promoted and then written, rewritten and decoded by a
logical slot. Every case runs on Format 2.0 and Format 1.0 and on the release library.
"""
from __future__ import annotations

import time

from cases import KILLED, LoadCase
from collect import add, base, halves_differ, hygiene, kept, nxt, snap
from suites.common import Limits
from suites.repl import CAP, catchup, standby
from suites.workloads_olap import cancels
from suites.workloads_prod2 import snap_oracle, snap_pair
from suites.workloads_prod3 import ROW_DIGEST, digest_differs, dups
from suites.workloads_prod4 import Wave4, own, paced

P_SHAPE = 'id int8 PRIMARY KEY, grp int, v int8, live bool, body text'
P_ROW = "{k}, {k} % 32, {k} % 50, {k} % 3 > 0, 'wire t' || {k} % 97 || CASE WHEN {k} % 5 = 0 THEN ' cable' ELSE '' END"
P_READS = (("SELECT grp, count(*), sum(v) FROM {t} WHERE live AND body LIKE '%t1%' GROUP BY grp ORDER BY grp", 'sift.enable_agg'),
           ("SELECT count(*), sum(id) FROM {t} WHERE live AND body @@> 'cable'", 'sift.enable_scan'),
           ('SELECT count(*), sum(id) FROM (SELECT id FROM {t} WHERE live ORDER BY v DESC FETCH FIRST 10 ROWS WITH TIES) s', 'sift.enable_agg'),
           ("SELECT count(*), sum(v) FROM {t} WHERE live AND body ILIKE '%CABLE%' AND grp < 8", 'sift.enable_agg'),
           ("SELECT count(*), sum(v) FROM {t} WHERE NOT live AND lower(body) = 'wire t5'", 'enable_indexscan'))


def olap_partial_expr_trigram_ties(ctx):
    """a heap with a partial sift_col index (WHERE live) with a trigram skip index of body, a partial BM25 index and a partial expression btree (WHERE NOT live), and its
    twin without indexes: 3 writers insert, flip rows across the predicate, move their values onto ties and delete them in both in one transaction, 1 vacuums and
    maintains, 4 readers compare LIKE, ILIKE, BM25, top-N WITH TIES and the expression with the twin and the heap in one snapshot while 1 cancels them: every answer
    agrees, every committed row kept once"""
    db = ctx.db
    db.run(f'CREATE TABLE pt ({P_SHAPE}); CREATE TABLE pt_tw ({P_SHAPE}); '
           + ''.join(f'INSERT INTO {t} SELECT {P_ROW.format(k="g")} FROM generate_series(1, 8192) g; ' for t in ('pt', 'pt_tw')) +
           "CREATE INDEX pt_ix ON pt USING sift_col (id, grp, v, body) WITH (skip_index = 'body: trigram') WHERE live; CREATE INDEX pt_bm ON pt USING sift (body) "
           'WHERE live; CREATE INDEX pt_lw ON pt (lower(body)) WHERE NOT live; ANALYZE pt, pt_tw')
    ws = ctx.sessions(9)

    def write(w, i):
        w.ids, k, o = getattr(w, 'ids', set()), nxt(w), own(w, i, 3)
        old = sorted(w.ids)[:1] if i % 4 == 3 else []
        sql = ''.join(f'; INSERT INTO {t} VALUES ({P_ROW.format(k=k)}); UPDATE {t} SET live = NOT live, v = {49 - i % 3} WHERE id = {o}'
                      + (f'; DELETE FROM {t} WHERE id = {old[0]}' if old else '') for t in ('pt', 'pt_tw'))
        if w.do(f'BEGIN{sql}; COMMIT') is not None:
            w.ids = (w.ids | {k}) - set(old)

    def maint(w, i):
        w.do(('VACUUM pt', "SELECT count(*) FROM sift.maintain('pt_ix')", "SELECT count(*) FROM sift.maintain('pt_bm')")[i % 3])
        time.sleep(0.05)

    def read(w, i):
        q, guc = P_READS[(w.seed + i) % len(P_READS)]
        w.ledger += snap_pair(w, q.format(t='pt'), q.format(t='pt_tw'), KILLED) if i % 2 else snap_oracle(w, q.format(t='pt'), guc, KILLED)

    busy = ctx.phase({'w': (ws[:3], write), 'm': (ws[3:4], maint), 'r': (ws[4:8], read), 'c': (ws[8:], cancels(ws[4:8]))}, 2.5)
    ctx.close_sessions()
    chk = ctx.sessions(1)[0]
    ids = set().union(*(getattr(w, 'ids', set()) for w in ws[:3]))
    mismatch = sum(w.ledger for w in ws[4:8]) + digest_differs(chk, 'pt', 'pt_tw', "id || ':' || v || ':' || live")
    mismatch += sum(snap_pair(chk, q.format(t='pt'), q.format(t='pt_tw')) + snap_oracle(chk, q.format(t='pt'), guc) for q, guc in P_READS)
    return base(ctx, busy, 'w', **add(kept(chk, 'pt', ids, 10 ** 6), kept(chk, 'pt_tw', ids, 10 ** 6)), oracle_mismatch=mismatch, reads=busy['r'].ops,
                cancelled=busy['r'].expected)


S_FK = ('ALTER TABLE {t} ADD FOREIGN KEY (pid) REFERENCES {r} ON DELETE CASCADE ON UPDATE CASCADE, '
        'ADD FOREIGN KEY (aux) REFERENCES {r} ON DELETE SET NULL ON UPDATE SET NULL')
S_SHAPE = 'id int8, region int, pid int8, aux int8, k int, v int8, PRIMARY KEY (id, region)'
S_LEAVES = ("DO $$ BEGIN FOR r IN 0..2 LOOP EXECUTE format('CREATE TABLE sp_r%s PARTITION OF sp FOR VALUES IN (%s) PARTITION BY HASH (id)', r, r); "
            "FOR h IN 0..1 LOOP EXECUTE format('CREATE TABLE sp_r%s_h%s PARTITION OF sp_r%s FOR VALUES WITH (MODULUS 2, REMAINDER %s) %s', r, h, r, h, "
            "CASE WHEN h = 0 THEN 'USING sift_table' ELSE '' END); END LOOP; END LOOP; END $$")
S_PARENTS, S_LEAF_PARENTS = 510, 30
S_ROW = "id || ':' || region || ':' || pid || ':' || coalesce(aux, 0) || ':' || v"
S_READ = 'SELECT region, count(*), sum(v), count(aux), sum(pid) FROM {t} WHERE region < 2 GROUP BY region ORDER BY region'
S_ORPHANS = 'SELECT count(*) FROM {t} c WHERE NOT EXISTS (SELECT 1 FROM {r} p WHERE p.pid = c.pid) OR (c.aux IS NOT NULL AND NOT EXISTS (SELECT 1 FROM {r} p WHERE p.pid = c.aux))'
S_PARENT = '(SELECT pid AS id, name FROM {t}) p'
S_KEY = f'CASE WHEN g % 3 = 2 THEN 1 + g * {{m}} % {S_LEAF_PARENTS} ELSE {S_LEAF_PARENTS + 1} + g * {{m}} % {S_PARENTS - S_LEAF_PARENTS} END'
S_STATE = "SELECT coalesce((SELECT inhdetachpending::text FROM pg_inherits WHERE inhrelid = 'sp_r2'::regclass), 'detached')"
S_LEAF = {'false': 'ALTER TABLE sp DETACH PARTITION sp_r2 CONCURRENTLY', 'true': 'ALTER TABLE sp DETACH PARTITION sp_r2 FINALIZE',
          'detached': 'BEGIN; ALTER TABLE sp DROP CONSTRAINT IF EXISTS sp_v_ck; ALTER TABLE sp ALTER COLUMN k DROP NOT NULL; '
                      'ALTER TABLE sp ATTACH PARTITION sp_r2 FOR VALUES IN (2); COMMIT'}
S_DDL = ('ALTER TABLE sp ADD CONSTRAINT sp_v_ck CHECK (v >= 0) NOT VALID', 'ALTER TABLE sp VALIDATE CONSTRAINT sp_v_ck', 'ALTER TABLE sp DROP CONSTRAINT sp_v_ck',
         'ALTER TABLE sp ALTER COLUMN k SET NOT NULL', 'ALTER TABLE sp ALTER COLUMN k DROP NOT NULL')


def s_step(w, i: int) -> str:
    """a transaction of writer W: on a parent of its own (in both twins) a delete that cascades and the parent again, a key update that cascades, or children of it; the
    parents of the leaf that comes and goes (no key of it refers to them: its concurrent detach would wait for the writers) are no writer's"""
    p, k = S_LEAF_PARENTS + 1 + w.seed % 3 + 3 * ((i * 37 + w.seed) % ((S_PARENTS - S_LEAF_PARENTS) // 3)), nxt(w)
    one = ('DELETE FROM {r} WHERE pid IN ({p}, {q}); INSERT INTO {r} VALUES ({p}, {p})',
           'UPDATE {r} SET pid = CASE WHEN pid > 1000 THEN pid - 1000 ELSE pid + 1000 END WHERE pid IN ({p}, {q})',
           'INSERT INTO {c} SELECT {k} + g, g % 2, pid, pid, g, {k} % 100 FROM {r}, generate_series(0, 3) g WHERE pid IN ({p}, {q})')[i % 3]
    return 'BEGIN; ' + '; '.join(one.format(r=r, c=c, p=p, q=p + 1000, k=k * 4) for r, c in (('sr', 'sp'), ('sr_tw', 'sp_tw'))) + '; COMMIT'


def s_setup() -> str:
    parents = f'SELECT g, g FROM generate_series(1, {S_PARENTS}) g'
    keys = '; '.join(S_FK.format(t=t, r=r) for t, r in (('sp_r0', 'sr'), ('sp_r1', 'sr'), ('sp_tw', 'sr_tw')))
    return (f'CREATE TABLE sr (pid int8 PRIMARY KEY, name text) USING sift_table; CREATE TABLE sr_tw (pid int8 PRIMARY KEY, name text); INSERT INTO sr {parents}; '
            f'INSERT INTO sr_tw {parents}; CREATE TABLE sp ({S_SHAPE}) PARTITION BY LIST (region); {S_LEAVES}; CREATE TABLE sp_tw ({S_SHAPE}); {keys}; '
            f'INSERT INTO sp SELECT g, g % 3, {S_KEY.format(m=1)}, {S_KEY.format(m=7)}, g % 10, g % 100 FROM generate_series(1, 6000) g; INSERT INTO sp_tw SELECT * FROM sp; '
            'ANALYZE sr, sr_tw, sp, sp_tw')


def ddl_subpart_fk_cascade_check(ctx):
    """a table partitioned by list and by hash (sift_table and heap leaves) and its heap twin, a cascading and a SET NULL foreign key from its two first lists and
    from the twin to a parent table (a sift_table, a heap for the twin): 3 writers delete, re-key and insert parents and children of their own in both in one
    transaction, 1 session detaches a leaf concurrently and attaches it again and, while the leaf is attached, adds a CHECK NOT VALID, validates and drops it and sets
    and drops NOT NULL, 2 readers compare the two in one snapshot: the twin holds the same rows at the end, no child orphaned"""
    db = ctx.db
    db.run(s_setup())
    ws = ctx.sessions(5) + ctx.sessions(1, "SET lock_timeout = '1s'")

    def write(w, i):
        w.do(s_step(w, i))

    def ddl(w, i):
        state = w.val(S_STATE)
        w.do(S_LEAF[state] if state != 'false' or i % 2 == 0 else S_DDL[i // 2 % len(S_DDL)], ('55P03', '42704', '42710'))
        paced(w)

    def read(w, i):
        w.ledger += snap_pair(w, S_READ.format(t='sp'), S_READ.format(t='sp_tw')) if i % 2 else snap_oracle(w, S_READ.format(t='sp'), 'sift.enable_agg')

    busy = ctx.phase({'w': (ws[:3], write), 'r': (ws[3:5], read), 'd': (ws[5:], ddl)}, 3.0)
    ctx.close_sessions()
    chk = ctx.sessions(1)[0]
    while (state := chk.val(S_STATE)) != 'false':
        chk.run(S_LEAF[state])
    orphans = sum(int(chk.val(S_ORPHANS.format(t=t, r=r))) for t, r in (('sp', 'sr'), ('sp_tw', 'sr_tw')))
    rows = abs(int(chk.val('SELECT count(*) FROM sp')) - int(chk.val('SELECT count(*) FROM sp_tw')))
    mismatch = sum(w.ledger for w in ws[3:5]) + digest_differs(chk, 'sp', 'sp_tw', S_ROW) + orphans
    mismatch += digest_differs(chk, S_PARENT.format(t='sr'), S_PARENT.format(t='sr_tw'), "id || ':' || name")
    return base(ctx, busy, 'w', lost_rows=rows, oracle_mismatch=mismatch, ddl=busy['d'].ops, refused=busy['d'].expected, orphans=orphans)


B_OPTS = {'b_en': "stemmer = 'en', accent_folding = true", 'b_pre': 'prefix_index = 4', 'b_np': 'positions = false, merge_fanin = 4', 'b_kw': "analyzer = 'keyword'"}
B_DOC = ("(ARRAY['Running', 'runs', 'Café', 'cafe', 'wire', 'wired', 'wiring', 'cable'])[1 + {k} % 8] || ' tail' || {k} % 97 || ' ' || "
         "(ARRAY['Éclair', 'eclair', 'switch', 'switches'])[1 + {k} % 4]")
B_READS = {'b_en': ("'runs OR eclair'", "'run AND switch'"), 'b_pre': ("'wir* AND swi*'", "'ca*'"), 'b_np': ('\'"wire tail1"\'', "'wiring AND NOT switch'"),
           'b_kw': ('\'"cable tail7 switches"\'', '\'"Café tail2 switch"\'')}
B_COUNT = 'SELECT count(*), sum(id) FROM {t} WHERE body @@> {q}'
B_COPY = 'COPY (SELECT id, body FROM {t} WHERE body @@> {q} ORDER BY id) TO STDOUT'


def conc_bm25_opts_stem_prefix_nopos(ctx):
    """four heaps of the same documents, each with a BM25 index of other options (an English stemmer with accent folding, prefixes of 4, no positions with a merge fan-in of
    4, the keyword analyzer): 3 writers insert, rewrite and delete documents of all four in one transaction, 1 session promotes, merges and vacuums them by turns, 3 readers
    compare stemmed, prefix, phrase and keyword queries and a COPY of the matches with the heap in one snapshot: every committed document kept once, every answer agrees"""
    db = ctx.db
    db.run(''.join(f'CREATE TABLE {t} (id int8 PRIMARY KEY, body text); INSERT INTO {t} SELECT g, {B_DOC.format(k="g")} FROM generate_series(1, 4096) g; '
                   f'CREATE INDEX {t}_ix ON {t} USING sift (body) WITH ({o}); ' for t, o in B_OPTS.items()) + f'ANALYZE {", ".join(B_OPTS)}')
    ws = ctx.sessions(7)

    def write(w, i):
        w.ids, k, o = getattr(w, 'ids', set()), nxt(w), own(w, i, 3)
        old = sorted(w.ids)[:1] if i % 3 == 2 else []
        sql = ''.join(f'; INSERT INTO {t} VALUES ({k}, {B_DOC.format(k=k)}); UPDATE {t} SET body = {B_DOC.format(k=o + i)} WHERE id = {o}'
                      + (f'; DELETE FROM {t} WHERE id = {old[0]}' if old else '') for t in B_OPTS)
        if w.do(f'BEGIN{sql}; COMMIT') is not None:
            w.ids = (w.ids | {k}) - set(old)

    def maint(w, i):
        t = tuple(B_OPTS)[i % len(B_OPTS)]
        w.do((f"SELECT sift.promote('{t}_ix')", f"SELECT sift.merge('{t}_ix', true)", f'VACUUM {t}')[i // len(B_OPTS) % 3])
        time.sleep(0.05)

    def read(w, i):
        t = tuple(B_OPTS)[(w.seed + i) % len(B_OPTS)]
        q = B_READS[t][i // len(B_OPTS) % 2]
        w.ledger += snap_oracle(w, (B_COUNT, B_COPY)[i % 2].format(t=t, q=q), 'sift.enable_scan')

    busy = ctx.phase({'w': (ws[:3], write), 'm': (ws[3:4], maint), 'r': (ws[4:], read)}, 2.5)
    ctx.close_sessions()
    chk = ctx.sessions(1)[0]
    ids = set().union(*(getattr(w, 'ids', set()) for w in ws[:3]))
    mismatch = sum(w.ledger for w in ws[4:]) + sum(snap_oracle(chk, B_COUNT.format(t=t, q=q), 'sift.enable_scan') for t, qs in B_READS.items() for q in qs)
    mismatch += sum(digest_differs(chk, 'b_en', t, "id || ':' || body") for t in tuple(B_OPTS)[1:])
    return base(ctx, busy, 'w', **add(*(kept(chk, t, ids, 10 ** 6) for t in B_OPTS)), oracle_mismatch=mismatch, maint=busy['m'].ops, reads=busy['r'].ops)


R_SHAPE = 'id int8 PRIMARY KEY, u uuid, ip inet, n numeric(20,4), d date, ts timestamptz, b bytea, tags text[], q load_case.qty, name text COLLATE "pg_c_utf8"'
R_ROW = ("{k}, md5({k}::text)::uuid, ('203.0.113.' || {k} % 256)::inet, {k} * 1.2345, date '2030-01-01' + {k} % 400, timestamptz '2030-01-01' + {k} * "
         "interval '17 minutes', decode(md5({k}::text), 'hex'), ARRAY['t' || {k} % 5, 'é' || {k} % 3], {k} % 50, (ARRAY['Zeta', 'alpha', 'Éclair', 'beta', '_x'])[1 + {k} % 5] "
         "|| {k} % 11")
R_TABLES = ('rt_tbl', 'rt_col', 'rt_tw')
R_READS = ('SELECT name, count(*), sum(n), min(ts), max(d), count(DISTINCT u) FROM {t} GROUP BY name ORDER BY name',
           "SELECT host(ip), count(*), sum(q) FROM {t} WHERE ip << '203.0.113.0/28' GROUP BY 1 ORDER BY 1",
           "SELECT count(*), sum(n) FROM {t} WHERE tags @> ARRAY['t1'] AND b > '\\x80'::bytea AND d >= date '2030-06-01'",
           'COPY (SELECT * FROM {t} WHERE id % 16 = {r} ORDER BY id) TO STDOUT')
R_SAMPLE = 'SELECT count(*) FROM (SELECT * FROM {t} TABLESAMPLE {m} (30) REPEATABLE ({r})) s LEFT JOIN rt_tw w USING (id) WHERE w.id IS NULL OR s::text <> w::text'
R_CODECS = ('zstd', 'lz4', 'none')


def olap_richtypes_coll_codec_sample(ctx):
    """a sift_table, a heap with a sift_col index and their heap twin of uuid, inet, numeric, date, timestamptz, bytea, text[], a domain and a text of the builtin C.UTF-8
    collation: 3 writers insert, update and delete rows of all three in one transaction, 1 session switches the codec of the table and the index (zstd, lz4, none; the
    index refuses with 55P03 while a writer holds its table) and vacuums, 4 readers compare grouping by the collated text, inet containment, array, bytea and date
    filters and COPY TO STDOUT with the twin in one snapshot and check that TABLESAMPLE SYSTEM and BERNOULLI only return rows the twin holds, while 1 cancels them:
    every answer agrees, every committed row kept once"""
    db = ctx.db
    db.run(f'CREATE DOMAIN qty AS int CHECK (VALUE >= 0); {"; ".join(f"CREATE TABLE {t} ({R_SHAPE}){am}" for t, am in zip(R_TABLES, (" USING sift_table", "", "")))}; '
           + ''.join(f'INSERT INTO {t} SELECT {R_ROW.format(k="g")} FROM generate_series(1, 4096) g; ' for t in R_TABLES) +
           'CREATE INDEX rt_col_ix ON rt_col USING sift_col (id, u, n, d, ts, q, name); ANALYZE rt_tbl, rt_col, rt_tw')
    ws = ctx.sessions(9)

    def write(w, i):
        w.ids, k, o = getattr(w, 'ids', set()), nxt(w), own(w, i, 3)
        old = sorted(w.ids)[:1] if i % 4 == 3 else []
        sql = ''.join(f"; INSERT INTO {t} VALUES ({R_ROW.format(k=k)}); UPDATE {t} SET n = n + 0.0001, q = (q + 1) % 50, tags = tags || 't{i % 5}'::text, "
                      f"name = name || 'ß' WHERE id = {o}" + (f'; DELETE FROM {t} WHERE id = {old[0]}' if old else '') for t in R_TABLES)
        if w.do(f'BEGIN{sql}; COMMIT') is not None:
            w.ids = (w.ids | {k}) - set(old)

    def codec(w, i):
        c = R_CODECS[i // 3 % len(R_CODECS)]
        w.do((f"SELECT sift.table_set_option('rt_tbl', 'compression', '{c}')", f"ALTER INDEX rt_col_ix SET (compression = '{c}')", 'VACUUM rt_tbl, rt_col')[i % 3],
             ('55P03',))
        paced(w)

    def read(w, i):
        n = (w.seed + i) % 5
        if n == 4:
            out = w.do(f"BEGIN ISOLATION LEVEL REPEATABLE READ; {R_SAMPLE.format(t=R_TABLES[i % 2], m=('SYSTEM', 'BERNOULLI')[i // 2 % 2], r=i)}; COMMIT", KILLED)
            w.ledger += out is not None and out != ['0']
        else:
            w.ledger += snap_pair(w, R_READS[n].format(t=R_TABLES[i % 2], r=i % 16), R_READS[n].format(t='rt_tw', r=i % 16), KILLED)

    busy = ctx.phase({'w': (ws[:3], write), 'k': (ws[3:4], codec), 'r': (ws[4:8], read), 'c': (ws[8:], cancels(ws[4:8]))}, 2.5)
    ctx.close_sessions()
    chk = ctx.sessions(1)[0]
    ids = set().union(*(getattr(w, 'ids', set()) for w in ws[:3]))
    mismatch = sum(w.ledger for w in ws[4:8]) + sum(digest_differs(chk, f'{t} x', 'rt_tw x', 'x::text') for t in R_TABLES[:2])
    mismatch += sum(snap_pair(chk, q.format(t=t, r=3), q.format(t='rt_tw', r=3)) for q in R_READS[:3] for t in R_TABLES[:2])
    return base(ctx, busy, 'w', **add(*(kept(chk, t, ids, 10 ** 6) for t in R_TABLES)), oracle_mismatch=mismatch, codecs=busy['k'].ops, cancelled=busy['r'].expected)


D_ROLE = 'load_sd8'
D_FN = (f'CREATE FUNCTION sd_credit(k int8, d int8, n int8) RETURNS int8 LANGUAGE sql SECURITY DEFINER SET search_path = load_case, pg_catalog, pg_temp AS $$ '
        "UPDATE sd_acc SET v = v + d WHERE id = k; UPDATE sd_tw SET v = v + d WHERE id = k; INSERT INTO sd_log VALUES (n, k, d) RETURNING d $$; "
        f'REVOKE ALL ON FUNCTION sd_credit(int8, int8, int8) FROM PUBLIC; GRANT EXECUTE ON FUNCTION sd_credit(int8, int8, int8) TO {D_ROLE}; '
        f'GRANT USAGE ON SCHEMA load_case TO {D_ROLE}')
D_PROBES = ('SELECT count(*) FROM load_case.sd_secret', "SELECT count(*) FROM sift.maintain('load_case.sd_acc')", "SELECT count(*) FROM sift.check('load_case.sd_log_ix')",
            "SELECT sift.table_set_option('load_case.sd_acc', 'compression', 'zstd')", "SELECT sift.promote('load_case.sd_log_ix')")
D_RESET = 'DISCARD ALL; SET search_path = load_case, public'
D_LOG = 'SELECT k, count(*), sum(d) FROM sd_log GROUP BY k ORDER BY k'


def pool_secdef_searchpath_advisory(ctx):
    """16 pooled sessions take an unprivileged role, put pg_temp first in their search_path beside a temporary table that shadows the ledger, take an advisory lock of a key,
    thrash SET LOCAL settings and credit the key through a SECURITY DEFINER function (a sift_table, its heap twin and a log under a sift_col index), then DISCARD ALL;
    1 session holds share row locks of the tables and vacuums, 1 compares the table with its twin and the log index with the heap, while the role probes the tables and
    the functions of sift it has no right to under a lock_timeout of 300 ms: every credit kept once, every probe refused with 42501 (acl_leaks) and none waits (unpriv_waits)"""
    db = ctx.db
    db.run(f'DROP ROLE IF EXISTS {D_ROLE}; CREATE ROLE {D_ROLE}; CREATE TABLE sd_acc (id int8 PRIMARY KEY, v int8) USING sift_table; '
           'CREATE TABLE sd_tw (id int8 PRIMARY KEY, v int8); CREATE TABLE sd_log (id int8, k int8, d int8); CREATE INDEX sd_log_ix ON sd_log USING sift_col (id, k, d); '
           'CREATE TABLE sd_secret (id int8, v int8) USING sift_table; INSERT INTO sd_secret VALUES (1, 1); INSERT INTO sd_acc SELECT g, 0 FROM generate_series(1, 64) g; '
           f'INSERT INTO sd_tw SELECT * FROM sd_acc; {D_FN}; ANALYZE sd_acc, sd_tw, sd_log')
    try:
        ws = ctx.sessions(19)
        probe = ws[18]
        probe.waits = 0

        def credit(w, i):
            k, d = 1 + (w.seed * 7 + i) % 64, 1 + i % 5
            out = w.do(f"BEGIN; SET LOCAL ROLE {D_ROLE}; SET LOCAL search_path = pg_temp, load_case; CREATE TEMP TABLE IF NOT EXISTS sd_acc (id int8, v int8); "
                       f"SET LOCAL work_mem = '{64 << i % 4}kB'; SET LOCAL enable_seqscan = {('on', 'off')[i % 2]}; SELECT pg_advisory_xact_lock({k}); "
                       f'SELECT load_case.sd_credit({k}, {d}, {nxt(w)}); COMMIT')
            w.ledger += d if out is not None else 0
            w.credited = getattr(w, 'credited', 0) + (out is not None)
            if i % 3 == 2:
                w.do(D_RESET)

        def hold(w, i):
            w.do(('BEGIN; LOCK TABLE sd_acc, sd_log IN SHARE ROW EXCLUSIVE MODE; SELECT count(*) FROM sd_acc; COMMIT', 'VACUUM sd_acc, sd_log',
                  "SELECT count(*) FROM sift.maintain('sd_log_ix')")[i % 3])
            paced(w)

        def read(w, i):
            w.ledger += snap_pair(w, 'SELECT sum(v) FROM sd_acc', 'SELECT sum(v) FROM sd_tw') if i % 2 else snap_oracle(w, D_LOG, 'sift.enable_agg')

        def unpriv(w, i):
            w.do(f"BEGIN; SET LOCAL ROLE {D_ROLE}; SET LOCAL lock_timeout = '300ms'; {D_PROBES[i % len(D_PROBES)]}; COMMIT", ('42501', '55P03'))
            w.ledger += w.last is None
            w.waits += w.last is not None and w.last[0] == '55P03'
            time.sleep(0.02)

        busy = ctx.phase({'w': (ws[:16], credit), 'h': (ws[16:17], hold), 'r': (ws[17:18], read), 'u': ([probe], unpriv)}, 2.5)
        ctx.close_sessions()
        chk = ctx.sessions(1)[0]
        credits, logged = int(chk.val('SELECT sum(v) FROM sd_acc')), int(chk.val('SELECT count(*) FROM sd_log'))
        lost = abs(credits - sum(w.ledger for w in ws[:16])) + abs(logged - sum(getattr(w, 'credited', 0) for w in ws[:16]))
        mismatch = ws[17].ledger + digest_differs(chk, 'sd_acc', 'sd_tw', "id || ':' || v") + snap_oracle(chk, D_LOG, 'sift.enable_agg')
        return base(ctx, busy, 'w', lost_rows=lost, dup_rows=dups(chk, 'sd_acc'), oracle_mismatch=mismatch, acl_leaks=probe.ledger, unpriv_waits=probe.waits,
                    refused=busy['u'].expected, credits=credits)
    finally:
        ctx.close_sessions()
        db.run(f'DROP OWNED BY {D_ROLE}; DROP ROLE {D_ROLE}')


Q_SHAPE = 'id int8 PRIMARY KEY, grp int, v int8, body text'
Q_ROW = "id || ':' || grp || ':' || v"
Q_READ = 'SELECT grp, count(*), sum(v) FROM {t} WHERE grp < 16 GROUP BY grp ORDER BY grp'
Q_DDL = ('ALTER TABLE pr_tbl ADD COLUMN x int DEFAULT 7', 'REINDEX INDEX CONCURRENTLY pr_tbl_pkey', 'ALTER TABLE pr_tbl DROP COLUMN x', 'CREATE INDEX CONCURRENTLY pr_tbl_g ON pr_tbl (grp)',
         'VACUUM FULL pr_tbl', 'DROP INDEX CONCURRENTLY pr_tbl_g')
Q_DECODE = "SELECT count(*) FROM pg_logical_slot_get_binary_changes('load_p8', NULL, NULL, 'proto_version', '1', 'publication_names', 'load_pub8')"


def q_write(w, i):
    """a writer of the sift_table and its twin: an insert, a savepoint, an update of a row of its own and every third time the update rolled back"""
    w.ids, k = getattr(w, 'ids', set()), nxt(w)
    back = 'ROLLBACK TO s; ' if i % 3 == 2 else ''
    sql = ''.join(f"INSERT INTO {t} (id, grp, v, body) VALUES ({k}, {k % 64}, {k % 1000}, 'wire p{k % 97}'); " for t in ('pr_tbl', 'pr_tw'))
    upd = ''.join(f'UPDATE {t} SET v = v + 1 WHERE id = {own(w, i, 3)}; ' for t in ('pr_tbl', 'pr_tw'))
    if w.do(f'BEGIN; {sql}SAVEPOINT s; {upd}{back}COMMIT') is not None:
        w.ids.add(k)


def repl_promote_ddl_rewrite_slot(ctx):
    """a sift_table and its heap twin: 3 writers insert and update both on the primary while 3 REPEATABLE READ sessions of the hot standby compare them in one snapshot;
    after a catch-up the standby holds what the primary does and is promoted; there 3 writers insert and update both under savepoints, 1 session adds and drops a column,
    reindexes and builds indexes concurrently and rewrites the table by VACUUM FULL, and 1 decodes a logical slot of a publication of the table: every committed row kept
    once, the twin agrees, the slot decodes the changes, sift.check, quarantine, health and temporary files of the promoted server clean; a new standby is cloned for the
    next case, whatever the case met"""
    try:
        return promoted(ctx, ctx.db, standby())
    finally:
        ctx.close_sessions()
        ctx.hook('LOAD_RESTANDBY')


def promoted(ctx, db, conn: dict) -> dict:
    db.run(''.join(f'CREATE TABLE {t} ({Q_SHAPE}){am}; INSERT INTO {t} SELECT * FROM load_base_fact; ' for t, am in (('pr_tbl', ' USING sift_table'), ('pr_tw', '')))
           + 'ANALYZE pr_tbl, pr_tw')
    st = ctx.sessions(1, conn=conn)[0]
    lag = catchup(db, st)
    ws, rs = ctx.sessions(3), ctx.sessions(3, conn=conn)

    def read(w, i):
        w.ledger += halves_differ(w.do(snap(Q_READ.format(t='pr_tbl'), Q_READ.format(t='pr_tw')), ('40001',)))

    first = ctx.phase({'w': (ws, q_write), 'r': (rs, read)}, 1.5)
    lag += catchup(db, st)
    digest = '; '.join(ROW_DIGEST.format(row=Q_ROW, t=t) for t in ('pr_tbl', 'pr_tw'))
    lag += int(db.run(digest) != st.run(digest)) + sum(w.ledger for w in rs)
    ids = set().union(*(getattr(w, 'ids', set()) for w in ws))
    ctx.hook('LOAD_PROMOTE')
    end = time.monotonic() + CAP
    while st.val('SELECT pg_is_in_recovery()') == 't' and time.monotonic() < end:
        time.sleep(0.02)
    st.run("CREATE PUBLICATION load_pub8 FOR TABLE pr_tbl; SELECT 1 FROM pg_create_logical_replication_slot('load_p8', 'pgoutput')")
    ps = ctx.sessions(5, conn=conn)
    for w in ps[:3]:
        w.seq = 10 ** 5

    def ddl(w, i):
        w.do(Q_DDL[i % len(Q_DDL)], ('42701', '42703', '42P07', '42704'))
        paced(w)

    def decode(w, i):
        out = w.do(Q_DECODE)
        w.ledger += int(out[0]) if out else 0
        time.sleep(0.05)

    busy = ctx.phase({'w': (ps[:3], q_write), 'd': (ps[3:4], ddl), 'l': (ps[4:], decode)}, 2.5)
    decoded = ps[4].ledger + int(st.val(Q_DECODE))
    st.run("SELECT pg_drop_replication_slot('load_p8'); DROP PUBLICATION load_pub8")
    ids |= set().union(*(getattr(w, 'ids', set()) for w in ps[:3]))
    m = add(kept(st, 'pr_tbl', ids, 10 ** 6), kept(st, 'pr_tw', ids, 10 ** 6), {'dup_rows': dups(st, 'pr_tbl')})
    return base(ctx, busy, 'w', **m, oracle_mismatch=digest_differs(st, 'pr_tbl', 'pr_tw', Q_ROW), standby_mismatch=lag, standby_reads=first['r'].ops,
                promoted_errors=sum(hygiene(st).values()) + int(decoded == 0), decoded=decoded, ddl=busy['d'].ops)


STRESS = (
    LoadCase('olap_partial_expr_trigram_ties', 'olap', olap_partial_expr_trigram_ties, Limits()),
    LoadCase('ddl_subpart_fk_cascade_check', 'ddl', ddl_subpart_fk_cascade_check, Limits()),
    LoadCase('conc_bm25_opts_stem_prefix_nopos', 'conc', conc_bm25_opts_stem_prefix_nopos, Limits()),
    LoadCase('olap_richtypes_coll_codec_sample', 'olap', olap_richtypes_coll_codec_sample, Limits()),
    LoadCase('pool_secdef_searchpath_advisory', 'pool', pool_secdef_searchpath_advisory, Wave4()),
)
REPL = (
    LoadCase('repl_promote_ddl_rewrite_slot', 'repl', repl_promote_ddl_rewrite_slot, Limits()),
)
