-- The sidecar of a scope of two paths whose three relations hold their pairs in several rows (a grant for each source of it, a row of the group for each source): the pairs are the DISTINCT union
-- of the direct pairs and of the joined ones, so a pair reached by both paths and by several rows of each is one pair in the runs and in every read, and a fact is counted once. Each state (they
-- accumulate, each made against a sidecar just made) is read for each tenant: the rows are the join's with the runs and without them, a row of a pair that has another row leaves the pair as it was, and
-- the runs a read took.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.col_unit_rows = 150;
SET timezone = 'UTC';
SET jit = off;
SET client_min_messages = warning;

CREATE TABLE grants (id serial PRIMARY KEY, tenant_id int NOT NULL, dim_a_id int, group_id int, deleted_at timestamptz) WITH (autovacuum_enabled = false);
CREATE INDEX grants_direct ON grants (dim_a_id, tenant_id) WHERE deleted_at IS NULL;
CREATE INDEX grants_group ON grants (group_id, tenant_id) WHERE deleted_at IS NULL;
CREATE TABLE memberships (id serial PRIMARY KEY, group_id int NOT NULL, dim_a_id int NOT NULL, deleted_at timestamptz) WITH (autovacuum_enabled = false);
CREATE INDEX memberships_active ON memberships (group_id, dim_a_id) WHERE deleted_at IS NULL;
CREATE INDEX grants_by_member ON grants USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX grants_by_group ON grants USING sift_col (group_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX memberships_by_member ON memberships USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid');
-- members 1 to 400 in groups of ten, every fifth in a row twice, every seventh in a second group (that one twice)
INSERT INTO memberships (group_id, dim_a_id) SELECT 1 + (m - 1) / 10, m FROM generate_series(1, 400) m;
INSERT INTO memberships (group_id, dim_a_id) SELECT 1 + (m - 1) / 10, m FROM generate_series(5, 400, 5) m;
INSERT INTO memberships (group_id, dim_a_id) SELECT 1 + (m + 20) / 10, m FROM generate_series(7, 400, 7) m;
INSERT INTO memberships (group_id, dim_a_id) SELECT 1 + (m + 20) / 10, m FROM generate_series(7, 400, 14) m;
INSERT INTO grants (tenant_id, dim_a_id, group_id) VALUES
	(1, 5, NULL), (1, 5, NULL), (1, NULL, 3), (1, NULL, 3),		-- tenant 1: a member directly twice, and a group twice
	(2, NULL, 2), (2, 8, NULL), (2, 12, NULL),				-- tenant 2: a group and two members
	(3, 31, NULL), (3, 32, NULL), (3, 32, NULL),			-- tenant 3: two members, one of them twice
	(4, NULL, 5), (4, 45, NULL), (4, 45, NULL), (4, NULL, 5);	-- tenant 4: a group and a member of it, each twice
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int, created_at timestamptz NOT NULL, kind smallint NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO facts SELECT i, CASE WHEN i % 17 = 0 THEN NULL ELSE 1 + i % 400 END, timestamptz '2025-01-01' + i * interval '17 minutes', i % 7, (i * 37) % 1000 FROM generate_series(1, 24000) i;
VACUUM (FREEZE, ANALYZE) facts, grants, memberships;
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, created_at, kind, v)
	WITH (key_order = 'dim_a_id, created_at', key_cut = 'dim_a_id, created_at',
		scope = 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, dim_a_id)');
SELECT s.format >= 53 AS sidecar_written_by_the_build FROM sift._test_col_formats('facts_col') s;
CREATE VIEW pair_oracle AS
	SELECT dim_a_id AS member, tenant_id FROM grants WHERE deleted_at IS NULL AND dim_a_id IS NOT NULL
	UNION
	SELECT m.dim_a_id, g.tenant_id FROM grants g JOIN memberships m ON m.group_id = g.group_id WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL;
SELECT count(*) AS pairs, (SELECT count(*) FROM grants g JOIN memberships m ON m.group_id = g.group_id) AS joined_rows FROM pair_oracle;

CREATE FUNCTION rows_md5(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	DROP TABLE IF EXISTS side_rows;
	EXECUTE 'CREATE TEMP TABLE side_rows AS ' || q;
	SELECT md5(coalesce(string_agg(t::text, '|' ORDER BY t::text), '')) INTO r FROM side_rows t;
	DROP TABLE side_rows;
	RETURN r;
END $$;

CREATE FUNCTION runs_of(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; r int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ 'Scope Sidecar Runs' THEN r := regexp_replace(l, '^.*: ', '')::int; END IF;
	END LOOP;
	RETURN r;
END $$;

CREATE FUNCTION q(k int) RETURNS text LANGUAGE sql AS
$$ SELECT format('SELECT kind, count(*) AS n, sum(v) AS s FROM facts f WHERE f.dim_a_id IN (SELECT member FROM pair_oracle WHERE tenant_id = %s) GROUP BY kind', k) $$;

CREATE FUNCTION state_line(OUT same_with_runs boolean, OUT same_without_runs boolean, OUT runs_by_tenant int[]) LANGUAGE plpgsql AS $$
DECLARE k int; join_md5 text; with_md5 text; without_md5 text;
BEGIN
	same_with_runs := true; same_without_runs := true; runs_by_tenant := '{}';
	FOR k IN 1..4 LOOP
		SET LOCAL sift.enable_agg = off;
		join_md5 := rows_md5(q(k));
		SET LOCAL sift.enable_agg = on;
		SET LOCAL sift.test_scope_entity = on;
		SET LOCAL sift.test_scope_no_side = off;
		with_md5 := rows_md5(q(k));
		runs_by_tenant := runs_by_tenant || runs_of(q(k));
		SET LOCAL sift.test_scope_no_side = on;
		without_md5 := rows_md5(q(k));
		same_with_runs := same_with_runs AND with_md5 = join_md5;
		same_without_runs := same_without_runs AND without_md5 = join_md5;
	END LOOP;
END $$;

CREATE TABLE states (n int, label text, edit text);
INSERT INTO states VALUES
	(1, 'clean', 'SELECT 1'),
	(2, 'one of the two direct rows of a pair ended', 'UPDATE grants SET deleted_at = now() WHERE id = (SELECT min(id) FROM grants WHERE tenant_id = 1 AND dim_a_id = 5)'),
	(3, 'the other row ended: the pair is reached by its group only', 'UPDATE grants SET deleted_at = now() WHERE tenant_id = 1 AND dim_a_id = 5 AND deleted_at IS NULL'),
	(4, 'one of the two rows of a group ended', 'UPDATE grants SET deleted_at = now() WHERE id = (SELECT min(id) FROM grants WHERE tenant_id = 4 AND group_id = 5)'),
	(5, 'one of the two memberships of a member ended', 'UPDATE memberships SET deleted_at = now() WHERE id = (SELECT min(id) FROM memberships WHERE group_id = 1 AND dim_a_id = 5)'),
	(6, 'the other membership ended: the member left the group', 'UPDATE memberships SET deleted_at = now() WHERE group_id = 1 AND dim_a_id = 5 AND deleted_at IS NULL'),
	(7, 'a third row of a pair made', 'INSERT INTO grants (tenant_id, dim_a_id) VALUES (3, 32), (3, 32)'),
	(8, 'a member of the group of tenant 2 left it, and is in another group of it', 'UPDATE memberships SET deleted_at = now() WHERE group_id = 2 AND dim_a_id = 14 AND deleted_at IS NULL'),
	(9, 'REINDEX', 'REINDEX INDEX facts_col');
SELECT c FROM states, LATERAL unnest(ARRAY[format('SELECT %s AS n, %L AS state', n, label), CASE WHEN n < 9 THEN 'REINDEX INDEX facts_col' ELSE 'SELECT 1' END, edit, 'SELECT * FROM state_line()'])
	WITH ORDINALITY u(c, o) ORDER BY n, o \gexec

SELECT sift.col_verify('facts_col', true);

DROP TABLE states;
DROP FUNCTION state_line(), q(int), runs_of(text), rows_md5(text);
DROP VIEW pair_oracle;
DROP TABLE facts, grants, memberships;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
