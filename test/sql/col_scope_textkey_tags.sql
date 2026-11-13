-- The journal of a scope relation tags each record with the scope key of its row, so that a read of one key is not charged for the pairs that changed in other keys. A text, varchar or uuid key is
-- tagged by the word of its bytes, as an integer key is by its value: a read of one key lists as changed, and patches, the members of the pairs that changed in that key only. A record of a journal made
-- before the tags (none) stays one of every key. Each state (they accumulate) is read for each key: the rows are the join's, and the changed and patched members are those of the key (-1: the read has no member to patch).
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;
SET sift.test_scope_patch = on;

-- 60 members of 100 facts, the groups of ten members, the tenants 1 to 6 (a text key) each with the members of one group
CREATE TABLE facts (id int PRIMARY KEY, m text, at timestamptz, amount int) WITH (autovacuum_enabled = false);
INSERT INTO facts SELECT i, 'acct-' || (1 + (i - 1) / 100), timestamptz '2026-01-01' + i * interval '9 minutes', i % 10 FROM generate_series(1, 6000) i;
CREATE TABLE pairs (m text NOT NULL, k text NOT NULL, PRIMARY KEY (m, k)) WITH (autovacuum_enabled = false);
INSERT INTO pairs SELECT 'acct-' || m, 'tenant-' || (1 + (m - 1) / 10) FROM generate_series(1, 60) m;
CREATE TABLE grants (m text NOT NULL, k text NOT NULL, PRIMARY KEY (m, k)) WITH (autovacuum_enabled = false);
INSERT INTO grants SELECT 'acct-' || m, 'tenant-' || (1 + (m - 1) / 10) FROM generate_series(1, 60) m WHERE m % 2 = 0;
CREATE TABLE group_grants (g int NOT NULL, k text NOT NULL, PRIMARY KEY (g, k)) WITH (autovacuum_enabled = false);
INSERT INTO group_grants SELECT g, 'tenant-' || g FROM generate_series(1, 6) g;
CREATE TABLE memberships (g int NOT NULL, m text NOT NULL, PRIMARY KEY (g, m)) WITH (autovacuum_enabled = false);
INSERT INTO memberships SELECT 1 + (m - 1) / 10, 'acct-' || m FROM generate_series(1, 60) m WHERE m % 2 = 1;
CREATE VIEW both_pairs AS SELECT m, k FROM grants UNION SELECT s.m, g.k FROM group_grants g JOIN memberships s ON s.g = g.g;
CREATE TABLE var_pairs (m text NOT NULL, k varchar(200) NOT NULL, PRIMARY KEY (m, k)) WITH (autovacuum_enabled = false);
INSERT INTO var_pairs SELECT * FROM pairs;
CREATE TABLE u_pairs (m uuid NOT NULL, k uuid NOT NULL, PRIMARY KEY (m, k)) WITH (autovacuum_enabled = false);
INSERT INTO u_pairs SELECT md5(m)::uuid, md5(k)::uuid FROM pairs;
CREATE TABLE u_facts (id int PRIMARY KEY, m uuid, amount int) WITH (autovacuum_enabled = false);
INSERT INTO u_facts SELECT id, md5(m)::uuid, amount FROM facts;

CREATE INDEX pairs_j ON pairs USING sift_col (m, k) WITH (track_changes = 'ctid');
CREATE INDEX var_pairs_j ON var_pairs USING sift_col (m, k) WITH (track_changes = 'ctid');
CREATE INDEX u_pairs_j ON u_pairs USING sift_col (m, k) WITH (track_changes = 'ctid');
CREATE INDEX grants_j ON grants USING sift_col (m, k) WITH (track_changes = 'ctid');
CREATE INDEX group_grants_j ON group_grants USING sift_col (g, k) WITH (track_changes = 'ctid');
CREATE INDEX memberships_j ON memberships USING sift_col (m, g) WITH (track_changes = 'ctid');
CREATE INDEX facts_one ON facts USING sift_col (id, m, at, amount) WITH (scope = 'k: m -> pairs(m, k)');
REINDEX INDEX facts_one;
CREATE INDEX facts_var ON facts USING sift_col (id, m, at, amount) WITH (scope = 'k: m -> var_pairs(m, k)');
REINDEX INDEX facts_var;
CREATE INDEX facts_two ON facts USING sift_col (id, m, at, amount) WITH (scope = 'k: m -> grants(m, k) | group_grants(g, k) via memberships(g, m)');
REINDEX INDEX facts_two;
CREATE INDEX u_facts_one ON u_facts USING sift_col (id, m, amount) WITH (scope = 'k: m -> u_pairs(m, k)');
REINDEX INDEX u_facts_one;
VACUUM (FREEZE, ANALYZE) facts, pairs, var_pairs, u_pairs, u_facts, grants, group_grants, memberships;

-- what a read of the key reports: the rows against the join's, the members changed and patched
CREATE FUNCTION tag_read(form text, key text, OUT same boolean, OUT changed_keys int, OUT patched_keys int)
LANGUAGE plpgsql AS $$
DECLARE
	q text := CASE form
		WHEN 'one' THEN format('SELECT count(*), sum(amount) FROM facts e WHERE e.m IN (SELECT m FROM pairs WHERE k = %L)', key)
		WHEN 'varchar' THEN format('SELECT count(*), sum(amount) FROM facts e WHERE e.m IN (SELECT m FROM var_pairs WHERE k = %L)', key)
		WHEN 'two' THEN format('SELECT count(*), sum(amount) FROM facts e WHERE e.m IN (SELECT m FROM both_pairs WHERE k = %L)', key)
		WHEN 'uuid' THEN format('SELECT count(*), sum(amount) FROM u_facts e WHERE e.m IN (SELECT m FROM u_pairs WHERE k = md5(%L)::uuid)', key)
	END;
	p jsonb;
	a text;
	b text;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || q INTO p;
	EXECUTE 'SELECT row(t.*)::text FROM (' || q || ') t' INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT row(t.*)::text FROM (' || q || ') t' INTO b;
	SET LOCAL sift.enable_agg = on;
	same := a = b;
	changed_keys := coalesce(jsonb_path_query_first(p, '$.**."Scope Changed Keys"')::text::int, -1);
	patched_keys := coalesce(jsonb_path_query_first(p, '$.**."Scope Patched Keys"')::text::int, -1);
END $$;

CREATE FUNCTION tag_states(state text) RETURNS TABLE (form text, key text, same boolean, changed_keys int, patched_keys int)
LANGUAGE sql AS $$
	SELECT f, k, r.same, r.changed_keys, r.patched_keys FROM (VALUES ('one'), ('varchar'), ('two'), ('uuid')) f (f),
		(VALUES ('tenant-1'), ('tenant-2'), ('tenant-6'), ('tenant-9')) k (k), LATERAL tag_read(f, k) r ORDER BY 1, 2
$$;

-- no change: nothing is changed in any key
SELECT * FROM tag_states('stored');

-- a member of tenant 2 gets a pair in tenant 1 (as a direct grant, a group grant and for the uuid key): only the reads of tenant 1 list a changed member
INSERT INTO pairs VALUES ('acct-15', 'tenant-1');
INSERT INTO var_pairs VALUES ('acct-15', 'tenant-1');
INSERT INTO u_pairs VALUES (md5('acct-15')::uuid, md5('tenant-1')::uuid);
INSERT INTO grants VALUES ('acct-15', 'tenant-1');
SELECT * FROM tag_states('one pair of tenant 1');

-- a pair of tenant 6 ends and a pair of a key no one reads begins: the reads of tenant 6 list one changed member, those of the other keys what they did
DELETE FROM pairs WHERE m = 'acct-51' AND k = 'tenant-6';
DELETE FROM var_pairs WHERE m = 'acct-51' AND k = 'tenant-6';
DELETE FROM u_pairs WHERE m = md5('acct-51')::uuid AND k = md5('tenant-6')::uuid;
DELETE FROM grants WHERE m = 'acct-52' AND k = 'tenant-6';
INSERT INTO pairs VALUES ('acct-30', 'tenant-3 other');
SELECT * FROM tag_states('one pair of tenant 6 ended');

DROP FUNCTION tag_states(text);
DROP FUNCTION tag_read(text, text);
DROP VIEW both_pairs;
DROP TABLE facts, u_facts, pairs, var_pairs, u_pairs, grants, group_grants, memberships CASCADE;
DROP EXTENSION pg_sift;
