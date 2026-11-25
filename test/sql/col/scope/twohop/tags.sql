-- The journal of the via relation of a scope of two paths tags each record with the group of its row, so that a read of some keys is not charged for the memberships that changed in the groups
-- of others: the stored entries of a member whose group has no pair with the key read are still right for it, and a read of that key lists no changed member for them. The groups of a key are those
-- the joined relation lists for it now and those its journal says changed since the stamp (a group granted to the key and then left changes the dim_a_id values of the key as the grant does). A journal
-- made before the scope (an older one, of no tags) is tagged by the scope index that finds it. Table-driven: each state (they accumulate) read for each key: the rows are the join's, the changed
-- dim_a_id values and the patched ones are those of the pairs that changed in the key. Counts only, no timings.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;
SET sift.test_scope_patch = on;

CREATE TABLE grants (id serial PRIMARY KEY, tenant_id int NOT NULL, dim_a_id int, group_id int) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX grants_direct ON grants (dim_a_id, tenant_id);
CREATE UNIQUE INDEX grants_group ON grants (group_id, tenant_id);
CREATE TABLE memberships (id serial PRIMARY KEY, group_id int NOT NULL, dim_a_id int NOT NULL) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX memberships_by_group ON memberships (group_id, dim_a_id);
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, amount int) WITH (autovacuum_enabled = false);
-- groups 1..6 of ten dim_a_id values each; tenant t has group t, tenant 1 a member of group 6 as well
INSERT INTO memberships (group_id, dim_a_id) SELECT 1 + (m - 1) / 10, m FROM generate_series(1, 60) m;
INSERT INTO grants (tenant_id, group_id) SELECT g, g FROM generate_series(1, 6) g;
INSERT INTO grants (tenant_id, dim_a_id) VALUES (1, 55);
INSERT INTO facts SELECT i, 1 + (i * 7) % 60, timestamptz '2026-01-01' + i * interval '9 minutes', i % 10 FROM generate_series(1, 3000) i;
VACUUM (FREEZE, ANALYZE) facts, grants, memberships;
CREATE VIEW pair_oracle AS
	SELECT dim_a_id AS member, tenant_id FROM grants WHERE dim_a_id IS NOT NULL
	UNION
	SELECT m.dim_a_id, g.tenant_id FROM grants g JOIN memberships m ON m.group_id = g.group_id;
CREATE INDEX grants_by_member ON grants USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX grants_by_group ON grants USING sift_col (group_id, tenant_id) WITH (track_changes = 'ctid');
-- the journal of the memberships exists before the scope does, with records of no tag
CREATE INDEX memberships_by_member ON memberships USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid');
SELECT sift._test_conn_enable('memberships_by_member') <> 0 AS journal_without_tags;
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, created_at, amount)
	WITH (scope = 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, dim_a_id)');
VACUUM (FREEZE, ANALYZE) facts;

-- one read of the key: the rows against the join, whether the units were read, and the dim_a_id values the read listed as changed and patched
CREATE FUNCTION read_key(k int8, OUT same boolean, OUT stored boolean, OUT changed int, OUT patched int) LANGUAGE plpgsql AS $$
DECLARE
	p jsonb;
	a text;
	b text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	EXECUTE format('EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) SELECT count(*), sum(amount) FROM facts f WHERE sift.in_scope(''facts_col''::regclass, ''tenant_id'', f.dim_a_id, ARRAY[%s]::int8[])', k) INTO p;
	EXECUTE format('SELECT json_agg(t)::text FROM (SELECT count(*), sum(amount) FROM facts f WHERE sift.in_scope(''facts_col''::regclass, ''tenant_id'', f.dim_a_id, ARRAY[%s]::int8[])) t', k) INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('SELECT json_agg(t)::text FROM (SELECT count(*), sum(amount) FROM facts f WHERE f.dim_a_id IN (SELECT member FROM pair_oracle WHERE tenant_id = %s)) t', k) INTO b;
	SET LOCAL sift.enable_agg = on;
	same := a = b;
	stored := jsonb_path_exists(p, '$.**."Scope Units Read"');
	changed := coalesce((jsonb_path_query_first(p, '$.**."Scope Changed Keys"') #>> '{}')::int, 0);
	patched := coalesce((jsonb_path_query_first(p, '$.**."Scope Patched Keys"') #>> '{}')::int, 0);
END $$;

-- the line of a state: the rows are the join's and the units are read for every key 1..6; the dim_a_id values changed and patched for each key
CREATE FUNCTION state_line(OUT same boolean, OUT stored boolean, OUT changed int[], OUT patched int[]) LANGUAGE plpgsql AS $$
BEGIN
	SELECT bool_and(r.same), bool_and(r.stored), array_agg(r.changed ORDER BY k.k), array_agg(r.patched ORDER BY k.k) INTO same, stored, changed, patched
	FROM generate_series(1, 6) k(k), LATERAL read_key(k.k) r;
END $$;

CREATE TABLE states (n int, label text, edit text);
INSERT INTO states VALUES
	(1, 'clean', 'SELECT 1'),
	(2, 'the memberships of group 1 deleted and inserted again',
		'WITH d AS (DELETE FROM memberships WHERE group_id = 1 RETURNING *) INSERT INTO memberships SELECT * FROM d'),
	(3, 'a member moved from group 2 to group 3', 'UPDATE memberships SET group_id = 3 WHERE group_id = 2 AND dim_a_id = 11'),
	(4, 'group 4 granted to tenant 5', 'INSERT INTO grants (tenant_id, group_id) VALUES (5, 4)'),
	(5, 'a member of group 4 deleted: tenants 4 and 5 have it', 'DELETE FROM memberships WHERE group_id = 4 AND dim_a_id = 31'),
	(6, 'the grant of group 4 to tenant 5 deleted: the dim_a_id values of the group left it', 'DELETE FROM grants WHERE tenant_id = 5 AND group_id = 4'),
	(7, 'a member of group 5 added to group 2', 'INSERT INTO memberships (group_id, dim_a_id) VALUES (2, 45)');
SELECT c FROM states, LATERAL unnest(ARRAY[format('SELECT %s AS n, %L AS state', n, label), edit, 'SELECT * FROM state_line()']) WITH ORDINALITY u(c, o) ORDER BY n, o \gexec

SELECT sift.col_verify('facts_col', true);

-- a refresh makes the stamps current: no key lists a changed dim_a_id
SELECT sift.col_maintain('facts_col') AS rewritten;
SELECT * FROM state_line();
DROP TABLE states;
DROP FUNCTION state_line();
DROP FUNCTION read_key(int8);
DROP VIEW pair_oracle;
DROP TABLE facts, grants, memberships;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
