#!/usr/bin/env python3
"""Differential fuzz of the scope index (test/t/col_scope_fuzz.sh runs it).

  test/fuzz/col_scope_fuzz.py [--seeds 1-24] [--rows 12000] [--steps 8] [--family all|pairs|union] [--twohop-strict] [--twohop-reason REGEX]
                                                                           needs PGHOST / PGPORT of a cluster with pg_sift and pg_sift_test, SIFT_FUZZ_OUT

A seed builds events (facts with an entity key, a time, a kind and a value), a table of (entity, tenant) pairs in which an entity is in 0 to 6 tenants, and a sift_col index with a scope on the pairs,
and draws steps: batches of fact INSERTs, DELETEs, UPDATEs (the value, and the entity: a fact moves between dim_b), upserts, pair INSERTs, DELETEs, moves of an entity between tenants, UPDATEs of the
tenant of pairs, a reload of the pairs, rolled back transactions with savepoints, reads inside the transaction that edited, promotion, merge, VACUUM of the facts and of the pairs and REINDEX.
After every step (a 'buffer' step leaves its rows in the write buffer, and the read of a key whose dim_b have some must take them as its delta) a battery is the plain join: through the scope path (sift.in_scope), through the planned plan and through the plain plan (sift.enable_agg off): one tenant, sets of tenants counted once
(a fact once though its entity is in several), per pair, count/sum/distinct/top-N, and the joins as written. A read must be stored from the scope units or say a documented Scope Fallback reason (a read of every key grouped by its key, 'group key', raises it as the error that makes the join run)
(scope_reason_ok in test/scope_helpers.sql); right after a build every read of one tenant that has pairs must be stored. A REPEATABLE READ reader opened before the churn must read, through the scope
path and through the plain plan, the rows of its first read. The seed draws the types of the keys, of the time column, the order of the columns of the unique pair index, the number of tenants per
entity (0..6 evenly, a mostly-one profile, and 2..6 for every entity), the segment size, track_changes, key_cut by the member column and order arrays named for the columns
(the entries of a fact repeat its address in the scope segments, which keep no order arrays, and the verification after every step covers both).

A third of the seeds make the pairs SOFT deleted: the pair table has a deleted_at column, its pairs are the rows of a partial unique btree WHERE deleted_at IS NULL (the primary key over all rows stays),
the journal index is plain or partial on the same predicate, and the steps soft delete, restore, insert soft deleted pairs and update a column no index holds besides the others; the plain join
reads the active rows.

The journal index of the pairs is made first and the scope index is made again (REINDEX) after its first build, which starts the journal. 'planner' (probed: the SQL forms IN and EXISTS have a
scope conjunct; the planner's recognition of the join forms is not in every tree) asserts that, and half of the seeds declare a cols(..) subset drawn per seed from the columns of the index (with or without the primary key,
the member key and the time column; a read of a column the scope units do not keep must fall back with the column reason).

The union family (a third of the seeds, those with seed % 3 == 0; --family union makes every seed one, --family pairs none) is the scope of two paths: the pairs are the distinct union of the direct
grants of a member and the grants of the groups the member is in. A seed builds a `grants` table (tenant, a member or a group, deleted_at, optionally with CHECK member IS NULL OR group IS NULL, else
a row may be both), a `memberships` table (group, member, deleted_at), the three partial unique btrees (column order drawn) and the scope
'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, dim_a_id)'. The three journals (grants by member, grants by group, memberships by member)
are plain or partial on the pairs' predicate; some seeds omit one, and then no read may be stored and every one says a documented reason. The steps: grants inserted, soft deleted, restored, moved
between direct and group, group grants on and off, memberships inserted, soft deleted, restored, moved to another group, a member reached by both paths, a group whose dim_b all leave and
return, HOT updates of a column no btree holds, reload of one table, REINDEX of a journal, besides the steps of the other seeds (aborted, savepoints, reads inside the transaction, promotion,
merge, VACUUM, REINDEX, the REPEATABLE READ reader). The oracle is the view pair_set, the SQL UNION of the three tables, written once (Seed.pair_set_sql): the plain forms IN, EXISTS and the join
(one row of a fact for each DISTINCT pair) read it, and the final entries of the scope units must be the join of the facts with it. A scope read of a union seed is accepted as stored, or with a
documented reason, or with --twohop-reason (default: the reason of a tree without the units reader, "no journal for two paths"). --twohop-strict is for the tree that reads the units of two paths:
the reads of one tenant right after a build must be stored (journals all present), the --twohop-reason is no longer accepted, and a seed that read no stored unit at all fails.
"""
import argparse, collections, os, random, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from col_twin_fuzz import Sess, Fail, ERR, SETUP, OUT, sqlstr

TWOHOP_REASON = '^no journal for two paths$'   # the reason of a tree without the reader of the units of two paths
HELPERS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'scope_helpers.sql')
FUZZ_SETUP = SETUP + " SET sift.agg_force = on; SET jit = off; SET sift.scope_max_multiplicity = 8;"

LIB = r"""
DROP TABLE IF EXISTS sf_q, sf_res;
CREATE TABLE sf_q (qid int PRIMARY KEY, class text, q text, plain text, sql_form bool, stored_wanted bool, engage_wanted bool, want_why text);
CREATE TABLE sf_res (stage text, qid int, class text, ok bool, stored bool, why text, detail text);

-- the statements of sf_q through the scope path, the planned plan and the plain plan, and the verdict of the reader
DROP FUNCTION IF EXISTS sf_run(text, bool, bool);
CREATE OR REPLACE FUNCTION sf_run(stage text, strict bool, planner bool, ok_why text DEFAULT NULL, no_stored bool DEFAULT false) RETURNS void LANGUAGE plpgsql AS $$
DECLARE d record; r record; ok bool; det text;
BEGIN
	FOR d IN SELECT * FROM sf_q ORDER BY qid LOOP
		det := NULL; r := NULL; ok := true;
		BEGIN
			IF d.class = 'group key' THEN
				SELECT * INTO r FROM scope_group_run(d.q, d.plain);
			ELSE
				SELECT * INTO r FROM scope_run(d.q, d.plain);
			END IF;
			IF NOT r.same THEN ok := false; det := 'rows differ';
			ELSIF no_stored AND r.stored THEN ok := false; det := 'the units were read though a journal is missing';
			ELSIF NOT d.sql_form AND NOT (r.path_ok OR coalesce(r.why ~ ok_why, false)) THEN ok := false; det := 'the scope path is neither read nor accounted for';
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
		BEGIN
			EXECUTE 'SELECT coalesce(md5(json_agg(t)::text), '''') FROM (' || d.q || ') t' INTO r;
		EXCEPTION WHEN feature_not_supported THEN   -- the reader refused a read of every key: the join runs (the planner's choice), which the plain form is
			IF d.class <> 'group key' THEN RAISE; END IF;
			r := h;
		END;
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
		= (SELECT md5(string_agg(e.ctid::text || ':' || t.tenant_id, ',' ORDER BY e.ctid, t.tenant_id)) FROM events e JOIN @PAIRS@ t ON t.dim_a_id = e.dim_a_id @ACT_T@);
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
INDEX_COLS = ['id', 'dim_a_id', 'created_at', 'kind', 'v']
STEPS = [('buffer', 9), ('ins', 12), ('del', 8), ('upd_val', 7), ('upd_member', 7), ('upsert', 4), ('pair_ins', 11), ('pair_del', 9), ('pair_move', 11), ('pair_upd', 6), ('aborted', 5),
         ('savepoints', 5), ('in_txn', 5), ('promote', 7), ('merge', 5), ('vacuum', 6), ('vacuum_pairs', 3), ('reindex', 3), ('pair_reload', 2), ('analyze', 1)]
SOFT_STEPS = [('pair_soft', 9), ('pair_restore', 6), ('pair_ins_deleted', 3), ('pair_note', 3)]   # added to the steps of a seed whose pairs are soft deleted


class Seed:
    pairs, tables, union, marker = 'tenant_scope', 'tenant_scope', False, 'in_scope'   # the relation of the pairs, the tables that hold them

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
        self.cols = None
        if r.random() < 0.5:   # an own stream, so that the other draws of a seed stay: a subset of the columns, with or without the primary key, the member key and the time column
            self.cols = [c for c in INDEX_COLS if random.Random(f'{seed}cols{c}').random() < 0.5] or ['v']
        self.kept = None if self.cols is None else set(self.cols) | {'created_at'}   # the units are cut by the time column, which they keep
        self.cut = r.choice([None, None, 'dim_a_id'])
        self.gord = r.random() < 0.5
        self.next_id = self.n + 1
        self.grng = random.Random(f'{seed}group')   # its own stream: the draws of the other classes stay
        sr = random.Random(seed * 7919 + 13)   # its own stream: the seeds that are not soft draw what they drew before
        self.soft = seed % 3 == 2   # a third of the seeds, and seed 2 of the quick tier
        self.pk2 = sr.choice(['dim_a_id, tenant_id', 'tenant_id, dim_a_id'])
        self.jpartial = self.soft and sr.random() < 0.5
        self.steps = STEPS + SOFT_STEPS if self.soft else STEPS
        self.act = ' AND s.deleted_at IS NULL' if self.soft else ''
        self.omit = self.ok_why = None
        self.expects_stored = True

    def strict(self, flag):   # whether the reads right after a build must be stored
        return flag

    def miss(self):
        return self.nkeys + 5

    def scope_text(self):
        return "tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)" + (f" cols({', '.join(self.cols)})" if self.cols else '')

    def pair_sql(self):
        jr = ("CREATE INDEX tenant_scope_col ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid')"
              + (" WHERE deleted_at IS NULL" if self.jpartial else "") + ";\n")
        act = f"CREATE UNIQUE INDEX tenant_scope_active ON tenant_scope ({self.pk2}) WHERE deleted_at IS NULL;\n" if self.soft else ''
        return f"""CREATE TABLE tenant_scope (dim_a_id {self.mtype} NOT NULL, tenant_id {self.ktype} NOT NULL, deleted_at timestamptz, note int, PRIMARY KEY ({self.pk})) WITH (autovacuum_enabled = off);
INSERT INTO tenant_scope SELECT m, 1 + (m * 31 + j * 13) % {self.nkeys}, {"CASE WHEN (m + j) % 6 = 0 THEN now() END" if self.soft else "NULL"} FROM (SELECT m, {KEYS_PER[self.profile]} AS n FROM
	(SELECT m, (m * 7919) % 1000 AS h FROM generate_series(1, {self.dim_b}) m) x) y, generate_series(0, 5) j WHERE j < y.n ON CONFLICT DO NOTHING;
{act}{jr}"""

    def setup_sql(self):
        trk = ", track_changes = 'off'" if self.track == 'off' else ''
        trk += f", key_cut = '{self.cut}'" if self.cut else ''
        trk += ", order_array_columns = 'id, created_at, kind, v'" if self.gord else ''
        return f"""{f"SET sift.test_col_segment_kb = {self.seg_kb};" if self.seg_kb else ""}
DROP TABLE IF EXISTS events, tenant_scope, grants, memberships CASCADE;
{self.pair_sql()}CREATE TABLE events (id int PRIMARY KEY, dim_a_id {self.mtype}, created_at {self.ttype}, kind smallint NOT NULL, v int NOT NULL, pad text) WITH (autovacuum_enabled = off);
INSERT INTO events SELECT {self.fact_rows(1, self.n)};
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, v) WITH (scope = {sqlstr(self.scope_text())}{trk});
REINDEX INDEX events_col;
VACUUM (FREEZE, ANALYZE) events;
VACUUM (ANALYZE) {self.tables};"""

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

    def pair_soft(self, pct):   # soft delete (the rows that are not pairs yet are left alone)
        return f"UPDATE tenant_scope SET deleted_at = now() WHERE {self.window(pct, 'dim_a_id')} AND tenant_id % 2 = {self.rng.randint(0, 1)} AND deleted_at IS NULL"

    def pair_restore(self, pct):   # a pair that is soft deleted comes back, unless the member has an active one under that key already (the partial unique btree)
        return (f"UPDATE tenant_scope t SET deleted_at = NULL WHERE t.deleted_at IS NOT NULL AND {self.window(pct, 'dim_a_id')} AND NOT EXISTS "
                f"(SELECT 1 FROM tenant_scope s WHERE s.dim_a_id = t.dim_a_id AND s.tenant_id = t.tenant_id AND s.deleted_at IS NULL)")

    def pair_ins_deleted(self, pct):
        return (f"INSERT INTO tenant_scope SELECT m, k, now() FROM (SELECT m, {self.free_key('m')} AS k FROM generate_series(1, {self.dim_b + 20}) m WHERE {self.window(pct, 'm')}) x "
                f"WHERE k IS NOT NULL ON CONFLICT DO NOTHING")

    def pair_note(self, pct):   # a column no index holds
        return f"UPDATE tenant_scope SET note = coalesce(note, 0) + 1 WHERE {self.window(pct, 'dim_a_id')}"

    def pair_del(self, pct):
        return f"DELETE FROM tenant_scope WHERE {self.window(pct, 'dim_a_id')} AND tenant_id % 2 = {self.rng.randint(0, 1)}"

    def pair_move(self, pct):
        return (f"WITH d AS (DELETE FROM tenant_scope WHERE {self.window(pct, 'dim_a_id')} AND tenant_id % 3 = {self.rng.randint(0, 2)} RETURNING dim_a_id) "
                f"INSERT INTO tenant_scope SELECT m, {self.free_key('m')} FROM (SELECT DISTINCT dim_a_id AS m FROM d) x ON CONFLICT DO NOTHING")

    def pair_upd(self, pct):
        return (f"UPDATE tenant_scope t SET tenant_id = 1 + t.tenant_id % {self.nkeys} WHERE {self.window(pct, 'dim_a_id')} AND NOT EXISTS "
                f"(SELECT 1 FROM tenant_scope s WHERE s.dim_a_id = t.dim_a_id AND s.tenant_id = 1 + t.tenant_id % {self.nkeys})")

    pair_kinds = ('pair_ins', 'pair_del', 'pair_move', 'pair_upd', 'pair_soft', 'pair_restore', 'pair_ins_deleted', 'pair_note')
    blocked = ('reindex', 'pair_reload')   # their lock waits for the reader's transaction

    def pair_reload(self, pct):   # a new lineage of the pairs' journal, the same pairs
        return ("CREATE TEMP TABLE IF NOT EXISTS sf_pairs AS SELECT * FROM tenant_scope WITH NO DATA; DELETE FROM sf_pairs; INSERT INTO sf_pairs SELECT * FROM tenant_scope; "
                "TRUNCATE tenant_scope; INSERT INTO tenant_scope SELECT * FROM sf_pairs")

    def step(self, reader):
        r = self.rng
        kind = r.choices([k for k, _ in self.steps], [w for _, w in self.steps])[0]
        pct = r.choice([1, 3, 10, 30, 60])
        if reader and kind in self.blocked:
            kind = 'promote'
        if kind == 'ins':
            a, m = self.next_id, r.choice([50, 500, 2500])
            self.next_id += m
            return kind, f"INSERT INTO events SELECT {self.fact_rows(a, a + m - 1)}", 'facts'
        if kind == 'buffer':   # rows that stay in the write buffer (no promotion before the reads): inserted, a window updated and another deleted, whose reads must take their delta from it
            a, m = self.next_id, r.choice([30, 200])
            self.next_id += m
            return kind, (f"INSERT INTO events SELECT {self.fact_rows(a, a + m - 1)}; UPDATE events SET v = (v + 1) % 10 WHERE {self.window(pct)}; "
                          f"DELETE FROM events WHERE id > {a} AND id % 13 = 0"), f'buffer {a}'
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
        if kind in self.pair_kinds:
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
            return kind, f'VACUUM {self.tables}', 'none'
        if kind == 'reindex':
            return kind, 'REINDEX INDEX events_col', 'rebuild'
        if kind == 'pair_reload':
            return kind, self.pair_reload(pct), 'pairs'
        if kind == 'reindex_journal':   # a new lineage of one journal: the scope says so until it is rebuilt
            return kind, f'REINDEX INDEX {r.choice(self.journals)}', 'pairs'
        return kind, 'ANALYZE events', 'none'

    # ---- the battery of one check: (class, scoped, plain, sql_form, stored_wanted, engage_wanted, want_why)
    def battery(self, keys):
        r = self.rng
        pick = lambda: r.choice(keys) if keys and r.random() < 0.9 else self.miss()
        lst = lambda ks: ','.join(map(str, ks))
        mark = lambda ks: ("sift.in_scope('events_col'::regclass, 'tenant_id', e.dim_a_id, %d)" % ks[0] if len(ks) == 1 else
                           "sift.in_scope('events_col'::regclass, 'tenant_id', e.dim_a_id, ARRAY[%s]::int8[])" % lst(ks))
        plain = lambda ks: r.choice([
            "EXISTS (SELECT 1 FROM %s s WHERE s.dim_a_id = e.dim_a_id AND s.tenant_id = ANY (ARRAY[%s]::int8[])%s)" % (self.pairs, lst(ks), self.act),
            "e.dim_a_id IN (SELECT dim_a_id FROM %s s WHERE tenant_id = ANY (ARRAY[%s]::int8[])%s)" % (self.pairs, lst(ks), self.act)])
        m1 = r.randint(1, 8)
        shapes = [   # name, the columns it reads, the statement (@ is the conjunct)
            ('agg', {'id', 'kind', 'v'}, "SELECT count(*), sum(v), count(DISTINCT dim_a_id), count(DISTINCT kind), min(id), max(id) FROM events e WHERE @"),
            ('group', {'v'}, "SELECT v, count(*), sum(v) FROM events e WHERE @ GROUP BY v ORDER BY v"),
            ('time', {'created_at', 'v'}, f"SELECT extract(month FROM created_at) m, count(*), sum(v) FROM events e WHERE @ AND created_at >= DATE '2026-0{m1}-01' "
                                          f"AND created_at < DATE '2026-0{m1 + 1}-01' + 40 GROUP BY 1 ORDER BY 1"),
            ('top', {'created_at', 'id'}, "SELECT id, created_at FROM events e WHERE @ ORDER BY created_at DESC, id DESC LIMIT 20")]
        qs = []

        def add(cls, cols, q, ks, stored):
            subset = self.kept is None or cols <= self.kept
            stored = stored and subset and all(k in keys for k in ks)
            qs.append((cls, q.replace('@', mark(ks)), q.replace('@', plain(ks)), False, stored, False, None if subset else '^column .* not in the scope units$'))

        for name, cols, q in r.sample(shapes, 3):
            add('one key', cols, q, [pick()], True)
        add('one key', set(), "SELECT count(*) FROM events e WHERE @", [pick()], True)   # reads no column: stored whatever cols(..) keeps
        for name, cols, q in r.sample(shapes[:3], 2):
            add('set', cols, q, [pick() for _ in range(r.randint(2, 5))], False)
        add('set top', shapes[3][1], shapes[3][2], [pick(), pick()], False)
        ks = [pick() for _ in range(3)]
        pair = "SELECT k, (SELECT count(*) FROM events e %s), (SELECT sum(v) FROM events e %s) FROM unnest(ARRAY[%s]::int8[]) k ORDER BY k"
        scoped = f"WHERE sift.{self.marker}('events_col'::regclass, 'tenant_id', e.dim_a_id, k)"
        joined = f"JOIN {self.pairs} s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = k{self.act}"   # the pairs are distinct: a row once for each
        qs.append(('per pair', pair % ((scoped,) * 2 + (lst(ks),)), pair % ((joined,) * 2 + (lst(ks),)), False, False, False, None))
        P = self.pairs
        for cols, aggs, filt in self.grng.sample([({'v'}, "count(*), sum(v)", ''), ({'id', 'v'}, "min(id), max(id), min(v)", ''), (set(), "count(DISTINCT e.dim_a_id), count(*)", ''),
                                          ({'v'}, "count(*), sum(v)", ' AND v > 4'), ({'id', 'created_at'}, "min(id), max(id)", f" AND created_at >= DATE '2026-0{m1}-01'")], 3):
            subset = self.kept is None or cols <= self.kept   # every key by its key: stored right after a build, else the join
            qs.append(('group key', f"SELECT {gk} AS k, {aggs} FROM events e WHERE {ga}{filt} GROUP BY 1 ORDER BY 1",
                       f"SELECT s.tenant_id AS k, {aggs} FROM events e JOIN {P} s ON s.dim_a_id = e.dim_a_id{self.act} WHERE true{filt} GROUP BY 1 ORDER BY 1", False, subset, False,
                       None if subset else '^column .* not in the scope units$'))
        qs.append(('group key', f"SELECT count(*), sum(v), min(id) FROM events e WHERE {ga}",   # a row for each pair, no group
                   f"SELECT count(*), sum(v), min(id) FROM events e JOIN {P} s ON s.dim_a_id = e.dim_a_id{self.act}", False, self.kept is None or {'v', 'id'} <= self.kept, False,
                   None if self.kept is None or {'v', 'id'} <= self.kept else '^column .* not in the scope units$'))
        for q, engage in [("SELECT s.tenant_id, count(*), sum(v), count(DISTINCT e.dim_a_id) FROM events e JOIN %s s ON s.dim_a_id = e.dim_a_id%s GROUP BY 1 ORDER BY 1" % (P, self.act), False),
                          ("SELECT count(*), sum(v) FROM events e JOIN %s s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id IN (%s)%s" % (P, lst([pick() for _ in range(3)]), self.act), False),
                          ("SELECT count(*), sum(v), count(DISTINCT e.dim_a_id) FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM %s s WHERE tenant_id = %d%s)" % (P, pick(), self.act), True),
                          ("SELECT count(*), sum(v) FROM events e WHERE EXISTS (SELECT 1 FROM %s s WHERE s.dim_a_id = e.dim_a_id AND s.tenant_id = %d%s)" % (P, pick(), self.act), True)]:
            qs.append(('sql form', q, q, True, False, engage and self.cols is None and not self.soft and not self.union, None))   # the planner's proof of a predicate is a separate piece
        return qs


JOURNALS = [('grants_member_col', 'grants', 'dim_a_id, tenant_id'), ('grants_group_col', 'grants', 'group_id, tenant_id'), ('memberships_member_col', 'memberships', 'dim_a_id, group_id')]
HOP_STEPS = [('grant_ins', 5), ('group_grant_on', 5), ('grant_soft', 6), ('group_grant_off', 5), ('grant_restore', 6), ('grant_move', 7), ('grant_note', 3), ('member_ins', 6),
             ('member_soft', 6), ('member_restore', 5), ('member_move', 7), ('member_note', 3), ('group_empty', 4), ('group_return', 4), ('both_paths', 4), ('reindex_journal', 3)]
FIRST = ['dim_a_id, tenant_id', 'tenant_id, dim_a_id']


class UnionSeed(Seed):
    """The scope of two paths: grants of a member, grants of a group, and the memberships of the dim_b in the groups."""
    pairs, tables, union, marker = 'pair_set', 'grants, memberships', True, 'in_scope_pairs'
    pair_kinds = ('pair_ins', 'pair_del', 'pair_move', 'pair_upd', 'pair_reload', 'grant_ins', 'group_grant_on', 'grant_soft', 'group_grant_off', 'grant_restore', 'grant_move',
                  'grant_note', 'member_ins', 'member_soft', 'member_restore', 'member_move', 'member_note', 'group_empty', 'group_return', 'both_paths', 'grant_both')
    blocked = ('reindex', 'pair_reload', 'reindex_journal')

    def __init__(self, seed, rows, caps, reason):
        super().__init__(seed, rows, caps)
        h = random.Random(seed * 104729 + 31)   # its own stream
        self.soft, self.act, self.jpartial = False, '', None
        self.groups = h.choice([4, 12, 40])
        self.enforced = h.random() < 0.6   # CHECK member IS NULL OR group IS NULL
        self.omit = h.choice([None] * 9 + [j[0] for j in JOURNALS])   # one in four seeds
        self.partial = {j[0]: h.random() < 0.5 for j in JOURNALS}
        self.btree = {k: h.choice(FIRST) for k in ('direct', 'group', 'member')}
        self.journals = [j[0] for j in JOURNALS if j[0] != self.omit]
        self.steps = STEPS + HOP_STEPS + ([] if self.enforced else [('grant_both', 4)])
        self.emptied = []
        self.strict_flag = 'twohop_strict' in caps
        self.expects_stored = self.strict_flag and self.omit is None
        self.ok_why = None if self.strict_flag else reason

    def strict(self, flag):
        return flag and self.expects_stored

    def miss(self):
        return self.rng.randint(1, self.nkeys + 5)

    def scope_text(self):
        return ("tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, dim_a_id)"
                + (f" cols({', '.join(self.cols)})" if self.cols else ''))

    # the pairs as SQL says them: the one definition every plain form reads
    def pair_set_sql(self):
        return ("CREATE VIEW pair_set (dim_a_id, tenant_id) AS SELECT dim_a_id, tenant_id FROM grants WHERE deleted_at IS NULL AND dim_a_id IS NOT NULL "
                "UNION SELECT m.dim_a_id, g.tenant_id FROM grants g JOIN memberships m ON m.group_id = g.group_id WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL;")

    def pair_sql(self):
        G, D, b = self.groups, self.dim_b, self.btree
        chk = ', CHECK (dim_a_id IS NULL OR group_id IS NULL)' if self.enforced else ''
        jr = ''.join(f"CREATE INDEX {n} ON {t} USING sift_col ({c}) WITH (track_changes = 'ctid'){' WHERE deleted_at IS NULL' if self.partial[n] else ''};\n"
                     for n, t, c in JOURNALS if n != self.omit)
        return f"""CREATE TABLE grants (id serial PRIMARY KEY, tenant_id {self.ktype} NOT NULL, dim_a_id {self.mtype}, group_id {self.mtype}, deleted_at timestamptz, note int{chk}) WITH (autovacuum_enabled = off);
CREATE TABLE memberships (id serial PRIMARY KEY, group_id {self.mtype} NOT NULL, dim_a_id {self.mtype} NOT NULL, deleted_at timestamptz, note int) WITH (autovacuum_enabled = off);
CREATE UNIQUE INDEX grants_direct ON grants ({b['direct']}) WHERE deleted_at IS NULL;
CREATE UNIQUE INDEX grants_group ON grants ({b['group'].replace('dim_a_id', 'group_id')}) WHERE deleted_at IS NULL;
CREATE UNIQUE INDEX memberships_active ON memberships ({'group_id, dim_a_id' if b['member'] == FIRST[0] else 'dim_a_id, group_id'}) WHERE deleted_at IS NULL;
INSERT INTO memberships (group_id, dim_a_id, deleted_at) SELECT g, m, CASE WHEN (m + g) % 11 = 0 THEN now() END FROM (SELECT m, 1 + m % {G} AS g FROM generate_series(1, {D}) m WHERE m % 7 <> 0
	UNION ALL SELECT m, 1 + (m * 3 + 1) % {G} FROM generate_series(1, {D}) m WHERE m % 5 = 0) x ON CONFLICT DO NOTHING;
INSERT INTO grants (tenant_id, dim_a_id, deleted_at) SELECT 1 + (m * 31 + j * 13) % {self.nkeys}, m, CASE WHEN (m + j) % 6 = 0 THEN now() END FROM (SELECT m, {KEYS_PER[self.profile]} AS n FROM
	(SELECT m, (m * 7919) % 1000 AS h FROM generate_series(1, {D}) m) x) y, generate_series(0, 5) j WHERE j < (y.n + 1) / 2 ON CONFLICT DO NOTHING;
INSERT INTO grants (tenant_id, group_id, deleted_at) SELECT 1 + (g * 17 + j * 5) % {self.nkeys}, g, CASE WHEN (g + j) % 7 = 0 THEN now() END
	FROM generate_series(1, {G + 3}) g, generate_series(0, 2) j WHERE j < 1 + g % 3 ON CONFLICT DO NOTHING;
{self.pair_set_sql()}
{jr}"""

    def free(self, col, val, table, cands):   # a value of cands the row (col = val) of the table has no active row with
        return (f"(SELECT c FROM generate_series(1, {cands}) c WHERE NOT EXISTS (SELECT 1 FROM {table} s WHERE s.{col[0]} = {val} AND s.{col[1]} = c AND s.deleted_at IS NULL) ORDER BY random() LIMIT 1)")

    def grant_ins(self, pct):
        return (f"INSERT INTO grants (tenant_id, dim_a_id) SELECT k, m FROM (SELECT m, {self.free(('dim_a_id', 'tenant_id'), 'm', 'grants', self.nkeys)} AS k FROM "
                f"generate_series(1, {self.dim_b + 20}) m WHERE {self.window(pct, 'm')} AND (SELECT count(*) FROM pair_set p WHERE p.dim_a_id = m) < 6) x WHERE k IS NOT NULL ON CONFLICT DO NOTHING")

    def group_grant_on(self, pct):
        return (f"INSERT INTO grants (tenant_id, group_id) SELECT k, g FROM (SELECT g, {self.free(('group_id', 'tenant_id'), 'g', 'grants', self.nkeys)} AS k FROM "
                f"generate_series(1, {self.groups + 3}) g WHERE {self.window(pct, 'g')} AND (SELECT count(*) FROM grants s WHERE s.group_id = g AND s.deleted_at IS NULL) < 4) x "
                f"WHERE k IS NOT NULL ON CONFLICT DO NOTHING")

    def grant_soft(self, pct, col='dim_a_id'):
        return f"UPDATE grants SET deleted_at = now() WHERE deleted_at IS NULL AND {self.window(pct, col)} AND tenant_id % 2 = {self.rng.randint(0, 1)} AND {col} IS NOT NULL"

    def group_grant_off(self, pct):
        return self.grant_soft(max(pct, 30), 'group_id')

    # one soft deleted row for each key comes back (two of one key would break the unique btree), unless the key is active; a row of a column that is NULL has no key there
    def grant_restore(self, pct):
        act = lambda col: f"NOT EXISTS (SELECT 1 FROM grants s WHERE s.deleted_at IS NULL AND s.{col} = t.{col} AND s.tenant_id = t.tenant_id)"
        once = lambda col, cols, frm, where: (f"SELECT DISTINCT ON (coalesce({col}::bigint, -id), tenant_id) {cols} FROM {frm} WHERE {where} "
                                              f"ORDER BY coalesce({col}::bigint, -id), tenant_id, id")
        inner = once('dim_a_id', 'id, group_id, tenant_id', 'grants t', f"deleted_at IS NOT NULL AND {self.window(pct)} AND {act('dim_a_id')} AND {act('group_id')}")
        return f"UPDATE grants SET deleted_at = NULL WHERE id IN ({once('group_id', 'id', f'({inner}) a', 'true')})"

    def grant_move(self, pct):   # a direct grant to the group of its member, or a group grant to a member (one to one), as a row of one or the other
        G, D = self.groups, self.dim_b
        if self.rng.random() < 0.5:
            return (f"UPDATE grants SET group_id = x.gid, dim_a_id = NULL FROM (SELECT DISTINCT ON (gid, tenant_id) id, gid FROM (SELECT id, tenant_id, 1 + dim_a_id % {G} AS gid FROM grants t "
                    f"WHERE deleted_at IS NULL AND dim_a_id IS NOT NULL AND group_id IS NULL AND {self.window(pct, 't.dim_a_id')} AND NOT EXISTS (SELECT 1 FROM grants s "
                    f"WHERE s.deleted_at IS NULL AND s.group_id = 1 + t.dim_a_id % {G} AND s.tenant_id = t.tenant_id)) a ORDER BY gid, tenant_id, id) x WHERE grants.id = x.id")
        return (f"UPDATE grants SET dim_a_id = 1 + (group_id * 7) % {D}, group_id = NULL WHERE id IN (SELECT id FROM grants t WHERE deleted_at IS NULL AND group_id IS NOT NULL "
                f"AND dim_a_id IS NULL AND {self.window(pct, 't.group_id')} AND NOT EXISTS (SELECT 1 FROM grants s WHERE s.deleted_at IS NULL AND s.dim_a_id = 1 + (t.group_id * 7) % {D} "
                f"AND s.tenant_id = t.tenant_id))")

    def grant_both(self, pct):   # a direct grant that is a group grant too (no CHECK)
        return (f"UPDATE grants SET group_id = x.gid FROM (SELECT DISTINCT ON (gid, tenant_id) id, gid FROM (SELECT id, tenant_id, 1 + dim_a_id % {self.groups} AS gid FROM grants t "
                f"WHERE deleted_at IS NULL AND dim_a_id IS NOT NULL AND group_id IS NULL AND {self.window(pct, 't.dim_a_id')} AND NOT EXISTS (SELECT 1 FROM grants s "
                f"WHERE s.deleted_at IS NULL AND s.group_id = 1 + t.dim_a_id % {self.groups} AND s.tenant_id = t.tenant_id)) a ORDER BY gid, tenant_id, id) x WHERE grants.id = x.id")

    def both_paths(self, pct):   # a member gets directly what its group has
        return (f"INSERT INTO grants (tenant_id, dim_a_id) SELECT DISTINCT g.tenant_id, m.dim_a_id FROM grants g JOIN memberships m ON m.group_id = g.group_id "
                f"WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL AND {self.window(pct, 'm.dim_a_id')} LIMIT 400 ON CONFLICT DO NOTHING")

    def grant_del(self, pct):
        return f"DELETE FROM grants WHERE {self.window(pct)} AND tenant_id % 2 = {self.rng.randint(0, 1)}"

    def grant_note(self, pct):   # a column no index holds
        return f"UPDATE grants SET note = coalesce(note, 0) + 1 WHERE {self.window(pct)}"

    def member_ins(self, pct):
        return (f"INSERT INTO memberships (group_id, dim_a_id) SELECT g, m FROM (SELECT m, {self.free(('dim_a_id', 'group_id'), 'm', 'memberships', self.groups + 3)} AS g FROM "
                f"generate_series(1, {self.dim_b + 20}) m WHERE {self.window(pct, 'm')} AND (SELECT count(*) FROM memberships s WHERE s.dim_a_id = m AND s.deleted_at IS NULL) < 3) x "
                f"WHERE g IS NOT NULL ON CONFLICT DO NOTHING")

    def member_soft(self, pct):
        return f"UPDATE memberships SET deleted_at = now() WHERE deleted_at IS NULL AND {self.window(pct, 'dim_a_id')} AND group_id % 2 = {self.rng.randint(0, 1)}"

    def member_restore(self, pct, where=None):
        where = where or self.window(pct, 'dim_a_id')
        return (f"UPDATE memberships SET deleted_at = NULL WHERE id IN (SELECT DISTINCT ON (group_id, dim_a_id) id FROM memberships t WHERE deleted_at IS NOT NULL AND {where} AND NOT EXISTS "
                f"(SELECT 1 FROM memberships s WHERE s.deleted_at IS NULL AND s.group_id = t.group_id AND s.dim_a_id = t.dim_a_id) ORDER BY group_id, dim_a_id, id)")

    def member_move(self, pct):   # to the next group
        n = self.groups + 3
        return (f"UPDATE memberships t SET group_id = 1 + t.group_id % {n} WHERE deleted_at IS NULL AND {self.window(pct, 't.dim_a_id')} AND group_id % 3 = {self.rng.randint(0, 2)} "
                f"AND NOT EXISTS (SELECT 1 FROM memberships s WHERE s.deleted_at IS NULL AND s.dim_a_id = t.dim_a_id AND s.group_id = 1 + t.group_id % {n})")

    def member_del(self, pct):
        return f"DELETE FROM memberships WHERE {self.window(pct, 'dim_a_id')} AND group_id % 2 = {self.rng.randint(0, 1)}"

    def member_note(self, pct):
        return f"UPDATE memberships SET note = coalesce(note, 0) + 1 WHERE {self.window(pct)}"

    def group_empty(self, pct):   # every member of a group leaves
        g = self.rng.randint(1, self.groups)
        self.emptied.append(g)
        return f"UPDATE memberships SET deleted_at = now() WHERE deleted_at IS NULL AND group_id = {g}"

    def group_return(self, pct):   # and returns
        g = self.emptied.pop(0) if self.emptied else self.rng.randint(1, self.groups)
        return self.member_restore(pct, f'group_id = {g}')

    def pair_ins(self, pct):
        return getattr(self, self.rng.choice(['grant_ins', 'group_grant_on', 'member_ins']))(pct)

    def pair_del(self, pct):
        return getattr(self, self.rng.choice(['grant_del', 'member_del']))(pct)

    def pair_move(self, pct):
        return getattr(self, self.rng.choice(['grant_move', 'member_move']))(pct)

    def pair_upd(self, pct):
        return getattr(self, self.rng.choice(['grant_soft', 'member_soft', 'group_grant_off', 'both_paths']))(pct)

    def pair_reload(self, pct):
        t, tmp = self.rng.choice([('grants', 'sf_g'), ('memberships', 'sf_m')])
        return (f"CREATE TEMP TABLE IF NOT EXISTS {tmp} AS SELECT * FROM {t} WITH NO DATA; DELETE FROM {tmp}; INSERT INTO {tmp} SELECT * FROM {t}; TRUNCATE {t}; "
                f"INSERT INTO {t} SELECT * FROM {tmp}")


def make_seed(seed, rows, caps, family, reason):
    return UnionSeed(seed, rows, caps, reason) if family == 'union' or (family == 'all' and seed % 3 == 0) else Seed(seed, rows, caps)


def insert_battery(qs):
    b = lambda x: str(x).lower()
    rows = ',\n'.join(f"({i}, {sqlstr(c)}, {sqlstr(q)}, {sqlstr(p)}, {b(s)}, {b(w)}, {b(e)}, {sqlstr(y) if y else 'NULL'})" for i, (c, q, p, s, w, e, y) in enumerate(qs, 1))
    return f"TRUNCATE sf_q; INSERT INTO sf_q VALUES {rows};"


def current_keys(a, s):
    out = a.run("SELECT coalesce(string_agg(DISTINCT tenant_id::text, ','), '') FROM %s s WHERE true%s;" % (s.pairs, s.act))
    return [int(x) for x in out.strip().split(',') if x]


def check(a, s, label, strict, stats):
    qs = s.battery(current_keys(a, s))
    out = a.run(insert_battery(qs) + f"\nSELECT sf_run({sqlstr(label)}, {str(s.strict(strict)).lower()}, {str('planner' in s.caps).lower()}, "
                f"{sqlstr(s.ok_why) if s.ok_why else 'NULL'}, {str(s.omit is not None).lower()});\n"
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


def check_delta(a, s, label, first_id):
    """The rows inserted since first_id are in the write buffer: a read of a key whose dim_b have some, that takes the units and patches no member, reads them as its delta (Scope Delta Rows)."""
    keys = current_keys(a, s)
    if not keys:
        return
    k = s.rng.choice(keys)
    q = f"SELECT count(*), sum(v) FROM events e WHERE sift.in_scope('events_col'::regclass, 'tenant_id', e.dim_a_id, {k})"
    out = a.run(f"SELECT 'rows', count(*) FROM events e WHERE e.id >= {first_id} AND EXISTS (SELECT 1 FROM tenant_scope s WHERE s.dim_a_id = e.dim_a_id AND s.tenant_id = {k}{s.act});\n"
                "SELECT 'buffer', coalesce(sum(rows), 0) FROM sift.col_segments('events_col') WHERE kind = 'buffer';\n"
                "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) " + q + ";")
    rows, buf = (int(re.search(rf'^{w}\|(\d+)$', out, re.M).group(1)) for w in ('rows', 'buffer'))
    delta = re.search(r'Scope Delta Rows: (\d+)', out)
    if rows > 0 and buf > 0 and 'Scope Units Read' in out and 'Scope Patched Keys' not in out and not (delta and int(delta.group(1)) > 0):
        raise Fail(f'{label}: key {k} has {rows} buffered rows and the read of its units took none as its delta: {out.strip()[-700:]}')


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


def run_seed(seed, rows, steps, out_dir, family='all', twohop_strict=False, twohop_reason=TWOHOP_REASON):
    os.makedirs(out_dir, exist_ok=True)
    path = f'{out_dir}/seed_{seed}.log'
    log = open(path, 'w')
    a, rd = Sess('writer', log, FUZZ_SETUP), Sess('reader', log, FUZZ_SETUP)
    stats = collections.Counter()
    try:
        helpers = open(HELPERS).read().replace('\\set ECHO none', '').replace('\\set ECHO all', '')
        a.run("DROP FUNCTION IF EXISTS scope_reason_ok(text), scope_run(text, text), scope_group_run(text, text) CASCADE;\n" + helpers)
        caps = probe(a) | ({'twohop_strict'} if twohop_strict else set())
        s = make_seed(seed, rows, caps, family, twohop_reason)
        log.write(f'-- seed {seed} planner={"planner" in caps} {type(s).__name__} {vars(s)}\n')
        a.run(LIB.replace('@ACT_T@', 'AND t.deleted_at IS NULL' if s.soft else '').replace('@PAIRS@', s.pairs))
        out = a.run(s.setup_sql())
        if ERR.search(out):
            raise Fail(f'setup: {out.strip()[:1500]}')
        check(a, s, 'base', True, stats)
        reader = s.rng.random() < 0.6
        if reader:
            rd.run('CREATE TEMP TABLE sf_q (LIKE public.sf_q);')   # the reader's own battery, in its own snapshot
            rd.run('BEGIN ISOLATION LEVEL REPEATABLE READ;')
            out = rd.run(insert_battery(s.battery(current_keys(a, s))) + f"\nSELECT 'snap', sf_snap();")
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
            if effect.startswith('buffer '):
                check_delta(a, s, label, int(effect.split()[1]))
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
        out = a.run('SET sift.scope_max_multiplicity = 64; REINDEX INDEX events_col; SET sift.scope_max_multiplicity = 8;')   # churn raises the entries of a row above the limit of the build
        if ERR.search(out):
            raise Fail(f'final rebuild: {out.strip()[:500]}')
        check(a, s, 'final rebuild', True, stats)
        if s.cols is None and 'entries|t' not in a.run("SELECT 'entries', sf_entries_are_the_join();"):
            raise Fail('final rebuild: the scope entries are not the join of the facts and the pairs')
        if s.expects_stored and stats['stored'] == 0:
            raise Fail('no read of the scope units in the whole seed: the fuzz does not reach the feature')
        a.run(f'DROP TABLE events, {s.tables} CASCADE;')
        log.write(f'-- OK {dict(stats)}\n')
        return 0, path, f'{type(s).__name__} omit={s.omit} planner={"planner" in caps} {s.profile}/{s.mtype}/{s.ktype}/{s.ttype} reads: ' + ' '.join(f'{k}={v}' for k, v in sorted(stats.items()))
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
    ap.add_argument('--family', choices=['all', 'pairs', 'union'], default='all', help='all: every third seed is a union seed')
    ap.add_argument('--twohop-strict', action='store_true', help='the reads of one key right after a build are stored in a union seed, and the fallback reason of --twohop-reason is not accepted')
    ap.add_argument('--twohop-reason', default=TWOHOP_REASON, help='a fallback reason accepted in a union seed besides the documented ones')
    o = ap.parse_args()
    lo, _, hi = o.seeds.partition('-')
    bad = 0
    for seed in range(int(lo), int(hi or lo) + 1):
        t0 = time.time()
        rc, path, note = run_seed(seed, o.rows, o.steps, OUT, o.family, o.twohop_strict, o.twohop_reason)
        print(f'seed={seed} {"OK" if rc == 0 else "FAIL"} {time.time() - t0:.0f}s {note} log={path}', flush=True)
        bad += rc
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
