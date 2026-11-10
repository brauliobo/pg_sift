#!/usr/bin/env python3
"""Differential fuzz of the scope index (test/t/col_scope_fuzz.sh runs it).

  test/fuzz/col_scope_fuzz.py [--seeds 1-24] [--rows 12000] [--steps 8]     needs PGHOST / PGPORT of a cluster with pg_sift and pg_sift_test, SIFT_FUZZ_OUT

A seed builds events (facts with an entity key, a time, a kind and a value), a table of (entity, tenant) pairs in which an entity is in 0 to 6 tenants, and a sift_col index with a scope on the pairs,
and draws steps: batches of fact INSERTs, DELETEs, UPDATEs (the value, and the entity: a fact moves between dim_b), upserts, pair INSERTs, DELETEs, moves of an entity between tenants, UPDATEs of the
tenant of pairs, a reload of the pairs, rolled back transactions with savepoints, reads inside the transaction that edited, promotion, merge, VACUUM of the facts and of the pairs and REINDEX.
After every step a battery is the plain join: through the scope path (sift.in_scope), through the planned plan and through the plain plan (sift.enable_agg off): one tenant, sets of tenants counted once
(a fact once though its entity is in several), per pair, count/sum/distinct/top-N, and the joins as written. A read must be stored from the scope units or say a documented Scope Fallback reason
(scope_reason_ok in test/scope_helpers.sql); right after a build every read of one tenant that has pairs must be stored. A REPEATABLE READ reader opened before the churn must read, through the scope
path and through the plain plan, the rows of its first read. The seed draws the types of the keys, of the time column, the order of the columns of the unique pair index, the number of tenants per
entity (0..6 evenly, a mostly-one profile, and 2..6 for every entity), the segment size and track_changes.

The journal index of the pairs is made first and the scope index is made again (REINDEX) after its first build, which starts the journal. 'planner' (probed: the SQL forms IN and EXISTS have a
scope conjunct; the planner's recognition of the join forms is not in every tree) asserts that, and half of the seeds declare a cols(..) subset (a read of another column must fall back with the
column reason).
"""
import argparse, collections, os, random, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from col_twin_fuzz import Sess, Fail, ERR, SETUP, OUT, sqlstr

HELPERS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'scope_helpers.sql')
FUZZ_SETUP = SETUP + " SET sift.agg_force = on; SET jit = off; SET sift.scope_max_multiplicity = 8;"

LIB = r"""
DROP TABLE IF EXISTS sf_q, sf_res;
CREATE TABLE sf_q (qid int PRIMARY KEY, class text, q text, plain text, sql_form bool, stored_wanted bool, engage_wanted bool, want_why text);
CREATE TABLE sf_res (stage text, qid int, class text, ok bool, stored bool, why text, detail text);

-- the statements of sf_q through the scope path, the planned plan and the plain plan, and the verdict of the reader
CREATE OR REPLACE FUNCTION sf_run(stage text, strict bool, planner bool) RETURNS void LANGUAGE plpgsql AS $$
DECLARE d record; r record; ok bool; det text;
BEGIN
	FOR d IN SELECT * FROM sf_q ORDER BY qid LOOP
		det := NULL; r := NULL; ok := true;
		BEGIN
			SELECT * INTO r FROM scope_run(d.q, d.plain);
			IF NOT r.same THEN ok := false; det := 'rows differ';
			ELSIF NOT d.sql_form AND NOT r.path_ok THEN ok := false; det := 'the scope path is neither read nor accounted for';
			ELSIF d.sql_form AND planner AND d.engage_wanted AND NOT r.engaged THEN ok := false; det := 'the planner did not recognise the form';
			ELSIF strict AND d.stored_wanted AND NOT r.stored THEN ok := false; det := 'not stored right after a build';
			ELSIF d.want_why IS NOT NULL AND (r.why IS NULL OR r.why !~ d.want_why) THEN ok := false; det := 'fallback reason is not ' || d.want_why;
			END IF;
		EXCEPTION WHEN OTHERS THEN
			ok := false; det := 'ERROR ' || SQLERRM;
		END;
		INSERT INTO sf_res VALUES (stage, d.qid, d.class, ok, coalesce(r.stored, false), r.why, det || ' [' || coalesce(r.why, 'stored') || '] ' || left(d.q, 200));
	END LOOP;
END $$;

-- the reader: the plain rows of the first read of a statement stay the rows of every later read, through the scope path and through the plain plan
CREATE OR REPLACE FUNCTION sf_snap() RETURNS text LANGUAGE plpgsql AS $$
DECLARE d record; r text; h text; h0 text; bad text := '';
BEGIN
	CREATE TEMP TABLE IF NOT EXISTS sf_snap0 (qid int PRIMARY KEY, h text);
	FOR d IN SELECT * FROM sf_q ORDER BY qid LOOP
		SET LOCAL sift.enable_agg = off;
		EXECUTE 'SELECT coalesce(md5(json_agg(t)::text), '''') FROM (' || d.plain || ') t' INTO h;
		SET LOCAL sift.enable_agg = on;
		EXECUTE 'SELECT coalesce(md5(json_agg(t)::text), '''') FROM (' || d.q || ') t' INTO r;
		INSERT INTO sf_snap0 VALUES (d.qid, h) ON CONFLICT DO NOTHING;
		SELECT s.h INTO h0 FROM sf_snap0 s WHERE s.qid = d.qid;
		IF r <> h OR h <> h0 THEN bad := bad || format('q%s scope=%s plain=%s first=%s; ', d.qid, r, h, h0); END IF;
	END LOOP;
	RETURN bad;
END $$;

CREATE OR REPLACE FUNCTION sf_verify() RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	PERFORM sift.col_verify('events_col', true);
	RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
	RETURN 'ERROR ' || SQLERRM;
END $$;

CREATE OR REPLACE FUNCTION sf_dead() RETURNS bigint LANGUAGE plpgsql AS $$
BEGIN
	RETURN (SELECT count(*) FROM sift._test_col_scope_entries('events_col') x LEFT JOIN events e ON e.ctid = x.tid WHERE e.id IS NULL);
END $$;

CREATE OR REPLACE FUNCTION sf_entries_are_the_join() RETURNS boolean LANGUAGE plpgsql AS $$
BEGIN
	RETURN (SELECT md5(string_agg(tid::text || ':' || key, ',' ORDER BY tid, key)) FROM sift._test_col_scope_entries('events_col'))
		= (SELECT md5(string_agg(e.ctid::text || ':' || t.tenant_id, ',' ORDER BY e.ctid, t.tenant_id)) FROM events e JOIN tenant_scope t ON t.dim_a_id = e.dim_a_id);
END $$;
"""

PROBE = """DROP TABLE IF EXISTS sfp, sfp_s CASCADE;
CREATE TABLE sfp_s (m int, k int, PRIMARY KEY (m, k));
INSERT INTO sfp_s SELECT m, 1 + (m + j) % 4 FROM generate_series(1, 40) m, generate_series(0, 2) j WHERE j < m % 4;
CREATE INDEX sfp_j ON sfp_s USING sift_col (m, k) WITH (track_changes = 'ctid');
CREATE TABLE sfp (id int PRIMARY KEY, m int, t timestamp, v int);
INSERT INTO sfp SELECT i, 1 + i % 40, TIMESTAMP '2026-01-01' + i * interval '1 hour', i % 7 FROM generate_series(1, 4000) i;
CREATE INDEX sfp_i ON sfp USING sift_col (id, m, t, v) WITH (scope = 'k: m -> sfp_s(m, k)');
REINDEX INDEX sfp_i;
VACUUM (FREEZE, ANALYZE) sfp, sfp_s;
SELECT 'planner', engaged FROM scope_run('SELECT count(*) FROM sfp e WHERE e.m IN (SELECT m FROM sfp_s WHERE k = 2)', 'SELECT 1');
"""

# the number of tenants of entity m, by the distribution of the seed (h: a hash of m in 0..999)
KEYS_PER = {
    'spread': "m % 7",
    'real': "CASE WHEN h < 130 THEN 0 WHEN h < 910 THEN 1 WHEN h < 970 THEN 2 WHEN h < 990 THEN 3 WHEN h < 997 THEN 4 WHEN h < 999 THEN 5 ELSE 6 END",
    'heavy': "2 + m % 5",
}
STEPS = [('ins', 12), ('del', 8), ('upd_val', 7), ('upd_member', 7), ('upsert', 4), ('pair_ins', 11), ('pair_del', 9), ('pair_move', 11), ('pair_upd', 6), ('aborted', 5),
         ('savepoints', 5), ('in_txn', 5), ('promote', 7), ('merge', 5), ('vacuum', 6), ('vacuum_pairs', 3), ('reindex', 3), ('pair_reload', 2), ('analyze', 1)]


class Seed:
    def __init__(self, seed, rows, caps):
        r = self.rng = random.Random(seed)
        self.seed, self.caps = seed, caps
        self.n = r.choice([rows // 2, rows])
        self.dim_b = r.choice([60, 300, 1500])
        self.nkeys = r.choice([12, 16, 24, 40])
        self.mtype = r.choice(['int', 'bigint'])
        self.ktype = r.choice(['int', 'bigint', 'smallint'])
        self.ttype = r.choice(['timestamp', 'timestamptz', 'date'])
        self.pk = r.choice(['dim_a_id, tenant_id', 'tenant_id, dim_a_id'])
        self.profile = r.choice(list(KEYS_PER))
        self.track = r.choice(['on', 'on', 'off'])
        self.seg_kb = r.choice([None, None, None, 96])
        self.cols = ['created_at', 'v'] if r.random() < 0.5 else None
        self.next_id = self.n + 1

    def setup_sql(self):
        trk = ", track_changes = 'off'" if self.track == 'off' else ''
        text = "tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)" + (f" cols({', '.join(self.cols)})" if self.cols else '')
        jr = "CREATE INDEX tenant_scope_col ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');\n"
        return f"""{f"SET sift.test_col_segment_kb = {self.seg_kb};" if self.seg_kb else ""}
DROP TABLE IF EXISTS events, tenant_scope CASCADE;
CREATE TABLE tenant_scope (dim_a_id {self.mtype} NOT NULL, tenant_id {self.ktype} NOT NULL, PRIMARY KEY ({self.pk})) WITH (autovacuum_enabled = off);
INSERT INTO tenant_scope SELECT m, 1 + (m * 31 + j * 13) % {self.nkeys} FROM (SELECT m, {KEYS_PER[self.profile]} AS n FROM
	(SELECT m, (m * 7919) % 1000 AS h FROM generate_series(1, {self.dim_b}) m) x) y, generate_series(0, 5) j WHERE j < y.n ON CONFLICT DO NOTHING;
CREATE TABLE events (id int PRIMARY KEY, dim_a_id {self.mtype}, created_at {self.ttype}, kind smallint NOT NULL, v int NOT NULL, pad text) WITH (autovacuum_enabled = off);
INSERT INTO events SELECT {self.fact_rows(1, self.n)};
{jr}CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, v) WITH (scope = {sqlstr(text)}{trk});
REINDEX INDEX events_col;
VACUUM (FREEZE, ANALYZE) events;
VACUUM (ANALYZE) tenant_scope;"""

    # facts: some without an entity, some of an unknown one, a hot head, the rest spread
    def fact_rows(self, a, b):
        m = (f"CASE WHEN i % 97 = 0 THEN NULL WHEN i % 89 = 0 THEN {self.dim_b + 1000} WHEN i % 3 = 0 THEN 1 + i % 20 ELSE 1 + (i * 7919) % {self.dim_b} END")
        return f"i, {m}, DATE '2026-01-01' + (i * 7919) % 300, i % 7, i % 10, repeat('x', 60) FROM generate_series({a}, {b}) i"

    # ---- the steps: (kind, sql, effect); effect: facts, pairs, aborted, in_txn, maint, vacuum, rebuild, none
    def window(self, pct, col='id'):
        m = self.rng.randint(1, 1000)
        return f'({col}::bigint * {self.rng.choice([2654435761, 40503, 7919])} + {m}) % 1000 < {pct * 10}'

    def free_key(self, m):
        return (f"(SELECT k FROM generate_series(1, {self.nkeys}) k WHERE NOT EXISTS (SELECT 1 FROM tenant_scope s WHERE s.dim_a_id = {m} AND s.tenant_id = k) "
                f"ORDER BY random() LIMIT 1)")

    def pair_ins(self, pct):
        return (f"INSERT INTO tenant_scope SELECT m, k FROM (SELECT m, {self.free_key('m')} AS k FROM generate_series(1, {self.dim_b + 20}) m WHERE {self.window(pct, 'm')} "
                f"AND (SELECT count(*) FROM tenant_scope s WHERE s.dim_a_id = m) < 6) x WHERE k IS NOT NULL ON CONFLICT DO NOTHING")

    def pair_del(self, pct):
        return f"DELETE FROM tenant_scope WHERE {self.window(pct, 'dim_a_id')} AND tenant_id % 2 = {self.rng.randint(0, 1)}"

    def pair_move(self, pct):
        return (f"WITH d AS (DELETE FROM tenant_scope WHERE {self.window(pct, 'dim_a_id')} AND tenant_id % 3 = {self.rng.randint(0, 2)} RETURNING dim_a_id) "
                f"INSERT INTO tenant_scope SELECT m, {self.free_key('m')} FROM (SELECT DISTINCT dim_a_id AS m FROM d) x ON CONFLICT DO NOTHING")

    def pair_upd(self, pct):
        return (f"UPDATE tenant_scope t SET tenant_id = 1 + t.tenant_id % {self.nkeys} WHERE {self.window(pct, 'dim_a_id')} AND NOT EXISTS "
                f"(SELECT 1 FROM tenant_scope s WHERE s.dim_a_id = t.dim_a_id AND s.tenant_id = 1 + t.tenant_id % {self.nkeys})")

    def step(self, reader):
        r = self.rng
        kind = r.choices([k for k, _ in STEPS], [w for _, w in STEPS])[0]
        pct = r.choice([1, 3, 10, 30, 60])
        if reader and kind in ('reindex', 'pair_reload'):   # their lock waits for the reader's transaction
            kind = 'promote'
        if kind == 'ins':
            a, m = self.next_id, r.choice([50, 500, 2500])
            self.next_id += m
            return kind, f"INSERT INTO events SELECT {self.fact_rows(a, a + m - 1)}", 'facts'
        if kind == 'del':
            return kind, f"DELETE FROM events WHERE {self.window(pct)}", 'facts'
        if kind == 'upd_val':
            return kind, f"UPDATE events SET v = (v + 3) % 10, created_at = created_at + interval '30 hours' WHERE {self.window(pct)}", 'facts'
        if kind == 'upd_member':
            return kind, f"UPDATE events SET dim_a_id = 1 + (dim_a_id * 7 + id) % {self.dim_b} WHERE dim_a_id IS NOT NULL AND {self.window(pct)}", 'facts'
        if kind == 'upsert':
            a = r.randint(1, self.next_id - 1)
            self.next_id = max(self.next_id, a + 401)
            return kind, (f"INSERT INTO events SELECT i, 1 + i % {self.dim_b}, DATE '2027-01-01' + i % 20, 5, i % 10, 'u' FROM generate_series({a}, {a + 400}) i "
                          f"ON CONFLICT (id) DO UPDATE SET dim_a_id = excluded.dim_a_id, v = excluded.v + 1"), 'facts'
        if kind in ('pair_ins', 'pair_del', 'pair_move', 'pair_upd'):
            return kind, getattr(self, kind)(pct), 'pairs'
        if kind == 'aborted':
            return kind, f"BEGIN; DELETE FROM events WHERE {self.window(pct)}; {self.pair_move(pct)}; {self.pair_del(pct)}; ROLLBACK", 'aborted'
        if kind == 'savepoints':
            return kind, (f"BEGIN; SAVEPOINT a; {self.pair_move(pct)}; SAVEPOINT b; DELETE FROM events WHERE {self.window(pct)}; ROLLBACK TO b; {self.pair_ins(pct)}; "
                          f"ROLLBACK TO a; {self.pair_upd(pct)}; SAVEPOINT c; {self.pair_del(pct)}; RELEASE c; COMMIT"), 'pairs'
        if kind == 'in_txn':   # left open: the battery reads inside it, then it ends (check_in_txn)
            return kind, f"BEGIN; {self.pair_move(pct)}; {self.pair_ins(pct)}; DELETE FROM events WHERE {self.window(pct)}", 'in_txn'
        if kind == 'promote':
            return kind, "SELECT sift.col_promote('events_col')", 'maint'
        if kind == 'merge':
            return kind, "SELECT sift.col_merge('events_col', true)", 'maint'
        if kind == 'vacuum':
            return kind, 'VACUUM events', 'vacuum'
        if kind == 'vacuum_pairs':
            return kind, 'VACUUM tenant_scope', 'none'
        if kind == 'reindex':
            return kind, 'REINDEX INDEX events_col', 'rebuild'
        if kind == 'pair_reload':   # a new lineage of the pairs' journal, the same pairs
            return kind, ("CREATE TEMP TABLE IF NOT EXISTS sf_pairs AS SELECT * FROM tenant_scope WITH NO DATA; DELETE FROM sf_pairs; INSERT INTO sf_pairs SELECT * FROM tenant_scope; "
                          "TRUNCATE tenant_scope; INSERT INTO tenant_scope SELECT * FROM sf_pairs"), 'pairs'
        return kind, 'ANALYZE events', 'none'

    # ---- the battery of one check: (class, scoped, plain, sql_form, stored_wanted, engage_wanted, want_why)
    def battery(self, keys):
        r = self.rng
        pick = lambda: r.choice(keys) if keys and r.random() < 0.9 else self.nkeys + 5
        lst = lambda ks: ','.join(map(str, ks))
        mark = lambda ks: ("sift.in_scope('events_col'::regclass, 'tenant_id', e.dim_a_id, %d)" % ks[0] if len(ks) == 1 else
                           "sift.in_scope('events_col'::regclass, 'tenant_id', e.dim_a_id, ARRAY[%s]::int8[])" % lst(ks))
        plain = lambda ks: r.choice([
            "EXISTS (SELECT 1 FROM tenant_scope s WHERE s.dim_a_id = e.dim_a_id AND s.tenant_id = ANY (ARRAY[%s]::int8[]))" % lst(ks),
            "e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = ANY (ARRAY[%s]::int8[]))" % lst(ks)])
        m1 = r.randint(1, 8)
        shapes = [   # name, the columns it reads, the statement (@ is the conjunct)
            ('agg', {'id', 'kind', 'v'}, "SELECT count(*), sum(v), count(DISTINCT dim_a_id), count(DISTINCT kind), min(id), max(id) FROM events e WHERE @"),
            ('group', {'v'}, "SELECT v, count(*), sum(v) FROM events e WHERE @ GROUP BY v ORDER BY v"),
            ('time', {'created_at', 'v'}, f"SELECT extract(month FROM created_at) m, count(*), sum(v) FROM events e WHERE @ AND created_at >= DATE '2026-0{m1}-01' "
                                          f"AND created_at < DATE '2026-0{m1 + 1}-01' + 40 GROUP BY 1 ORDER BY 1"),
            ('top', {'created_at', 'id'}, "SELECT id, created_at FROM events e WHERE @ ORDER BY created_at DESC, id DESC LIMIT 20")]
        qs = []

        def add(cls, cols, q, ks, stored):
            subset = self.cols is None or cols <= set(self.cols)
            stored = stored and subset and all(k in keys for k in ks)
            qs.append((cls, q.replace('@', mark(ks)), q.replace('@', plain(ks)), False, stored, False, None if subset else '^column .* not in the scope units$'))

        for name, cols, q in r.sample(shapes, 3):
            add('one key', cols, q, [pick()], True)
        for name, cols, q in r.sample(shapes[:3], 2):
            add('set', cols, q, [pick() for _ in range(r.randint(2, 5))], False)
        add('set top', shapes[3][1], shapes[3][2], [pick(), pick()], False)
        ks = [pick() for _ in range(3)]
        pair = "SELECT k, (SELECT count(*) FROM events e WHERE %s), (SELECT sum(v) FROM events e WHERE %s) FROM unnest(ARRAY[%s]::int8[]) k ORDER BY k"
        qs.append(('per pair',
                   pair % (("sift.in_scope('events_col'::regclass, 'tenant_id', e.dim_a_id, k)",) * 2 + (lst(ks),)),
                   pair % (("EXISTS (SELECT 1 FROM tenant_scope s WHERE s.dim_a_id = e.dim_a_id AND s.tenant_id = k)",) * 2 + (lst(ks),)), False, False, False, None))
        for q, engage in [("SELECT s.tenant_id, count(*), sum(v), count(DISTINCT e.dim_a_id) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id GROUP BY 1 ORDER BY 1", False),
                          ("SELECT count(*), sum(v) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id IN (%s)" % lst([pick() for _ in range(3)]), False),
                          ("SELECT count(*), sum(v), count(DISTINCT e.dim_a_id) FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = %d)" % pick(), True),
                          ("SELECT count(*), sum(v) FROM events e WHERE EXISTS (SELECT 1 FROM tenant_scope s WHERE s.dim_a_id = e.dim_a_id AND s.tenant_id = %d)" % pick(), True)]:
            qs.append(('sql form', q, q, True, False, engage and self.cols is None, None))
        return qs


def insert_battery(qs):
    b = lambda x: str(x).lower()
    rows = ',\n'.join(f"({i}, {sqlstr(c)}, {sqlstr(q)}, {sqlstr(p)}, {b(s)}, {b(w)}, {b(e)}, {sqlstr(y) if y else 'NULL'})" for i, (c, q, p, s, w, e, y) in enumerate(qs, 1))
    return f"TRUNCATE sf_q; INSERT INTO sf_q VALUES {rows};"


def current_keys(a):
    out = a.run("SELECT coalesce(string_agg(DISTINCT tenant_id::text, ','), '') FROM tenant_scope;")
    return [int(x) for x in out.strip().split(',') if x]


def check(a, s, label, strict, stats):
    qs = s.battery(current_keys(a))
    out = a.run(insert_battery(qs) + f"\nSELECT sf_run({sqlstr(label)}, {str(strict).lower()}, {str('planner' in s.caps).lower()});\n"
                "SELECT 'verify', sf_verify();\nSELECT 'bad', stage, qid, class, left(detail, 500) FROM sf_res WHERE NOT ok;\n"
                "SELECT 'why', coalesce(why, CASE WHEN stored THEN 'stored' ELSE 'none' END), count(*) FROM sf_res GROUP BY 2;\nTRUNCATE sf_res;")
    if ERR.search(out):
        raise Fail(f'{label}: {out.strip()[:1500]}')
    for line in out.split('\n'):
        if line.startswith('verify|') and line != 'verify|ok':
            raise Fail(f'{label}: col_verify: {line}')
        if line.startswith('bad|'):
            raise Fail(f'{label}: {line}')
        if line.startswith('why|'):
            _, why, n = line.split('|')
            stats[why] += int(n)


def check_in_txn(a, s, label, stats, rng):
    """The reads inside the transaction that edited the pairs (its own changes visible), then it commits or rolls back."""
    check(a, s, f'{label} inside', False, stats)
    a.run('COMMIT;' if rng.random() < 0.5 else 'ROLLBACK;')
    check(a, s, f'{label} ended', False, stats)


def probe(a):
    """Whether the planner puts a scope conjunct on the SQL forms."""
    out = a.run(PROBE)
    a.run("DROP TABLE IF EXISTS sfp, sfp_s CASCADE;")
    return {'planner'} if re.search(r'^planner\|t$', out, re.M) else set()


def run_seed(seed, rows, steps, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    path = f'{out_dir}/seed_{seed}.log'
    log = open(path, 'w')
    a, rd = Sess('writer', log, FUZZ_SETUP), Sess('reader', log, FUZZ_SETUP)
    stats = collections.Counter()
    try:
        helpers = open(HELPERS).read().replace('\\set ECHO none', '').replace('\\set ECHO all', '')
        a.run("DROP FUNCTION IF EXISTS scope_reason_ok(text), scope_run(text, text) CASCADE;\n" + helpers)
        caps = probe(a)
        s = Seed(seed, rows, caps)
        log.write(f'-- seed {seed} planner={"planner" in caps} {vars(s)}\n')
        a.run(LIB)
        out = a.run(s.setup_sql())
        if ERR.search(out):
            raise Fail(f'setup: {out.strip()[:1500]}')
        check(a, s, 'base', True, stats)
        reader = s.rng.random() < 0.6
        if reader:
            rd.run('CREATE TEMP TABLE sf_q (LIKE public.sf_q);')   # the reader's own battery, in its own snapshot
            rd.run('BEGIN ISOLATION LEVEL REPEATABLE READ;')
            out = rd.run(insert_battery(s.battery(current_keys(a))) + f"\nSELECT 'snap', sf_snap();")
            if not re.search(r'^snap\|$', out, re.M):
                raise Fail(f'reader first read: {out.strip()[:500]}')
        for i in range(steps):
            kind, sql, effect = s.step(reader)
            out = a.run(sql + ';')
            if ERR.search(out):
                raise Fail(f'step {i} {kind}: {sql[:300]}: {out.strip()[:800]}')
            label = f'step {i} {kind}'
            if effect == 'in_txn':
                check_in_txn(a, s, label, stats, s.rng)
            else:
                check(a, s, label, effect == 'rebuild', stats)
            if effect == 'vacuum' and not reader:
                dead = a.run("SELECT 'dead', sf_dead();")
                if 'dead|0' not in dead:
                    raise Fail(f'{label}: entries of dead addresses after VACUUM: {dead.strip()[:200]}')
            if reader:
                out = rd.run(f"SELECT 'snap', sf_snap();")
                if ERR.search(out) or not re.search(r'^snap\|$', out, re.M):
                    raise Fail(f'{label}: reader snapshot changed: {out.strip()[:800]}')
        if reader:
            rd.run('COMMIT;')
        a.run('VACUUM events;')
        check(a, s, 'final', False, stats)
        a.run('REINDEX INDEX events_col;')
        check(a, s, 'final rebuild', True, stats)
        if s.cols is None and 'entries|t' not in a.run("SELECT 'entries', sf_entries_are_the_join();"):
            raise Fail('final rebuild: the scope entries are not the join of the facts and the pairs')
        if stats['stored'] == 0:
            raise Fail('no read of the scope units in the whole seed: the fuzz does not reach the feature')
        a.run('DROP TABLE events, tenant_scope CASCADE;')
        log.write(f'-- OK {dict(stats)}\n')
        return 0, path, f'planner={"planner" in caps} {s.profile}/{s.mtype}/{s.ktype}/{s.ttype} reads: ' + ' '.join(f'{k}={v}' for k, v in sorted(stats.items()))
    except Fail as e:
        log.write(f'-- FAIL {e}\n')
        print(f'seed={seed} FAIL: {e}', flush=True)
        return 1, path, ''
    finally:
        a.close(); rd.close(); log.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--seeds', default='1-3')
    ap.add_argument('--rows', type=int, default=12000)
    ap.add_argument('--steps', type=int, default=8)
    o = ap.parse_args()
    lo, _, hi = o.seeds.partition('-')
    bad = 0
    for seed in range(int(lo), int(hi or lo) + 1):
        t0 = time.time()
        rc, path, note = run_seed(seed, o.rows, o.steps, OUT)
        print(f'seed={seed} {"OK" if rc == 0 else "FAIL"} {time.time() - t0:.0f}s {note} log={path}', flush=True)
        bad += rc
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
