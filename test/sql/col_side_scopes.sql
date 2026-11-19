-- An index with several scopes has a record in its sidecar for each of them, and a read takes the runs of its own scope and key: here a scope of one path (tenant), a second one (region) over the
-- same members, a third of two paths (org: a direct grant and a group grant) and a fourth whose member is not the order of the units (device: its record has no run, its reads run the join). An
-- edit of the pairs of one scope leaves the runs of the others as they were, because each has its journals and its stamp. Every state is read for every scope and key: the rows are the join's, with
-- the runs and without them, and the runs each scope's read took.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.col_unit_rows = 150;
SET timezone = 'UTC';
SET jit = off;
SET client_min_messages = warning;

CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO tenant_scope SELECT m, 1 FROM generate_series(1, 400) m WHERE m % 7 <> 0;
INSERT INTO tenant_scope SELECT m, 2 FROM generate_series(1, 400) m WHERE m % 3 = 0;
CREATE TABLE region_scope (dim_a_id int NOT NULL, region_id int NOT NULL, PRIMARY KEY (dim_a_id, region_id)) WITH (autovacuum_enabled = false);
INSERT INTO region_scope SELECT m, 1 FROM generate_series(1, 200) m;
INSERT INTO region_scope SELECT m, 2 FROM generate_series(150, 400) m;
CREATE TABLE device_scope (dim_b_id int NOT NULL, device_id int NOT NULL, PRIMARY KEY (dim_b_id, device_id)) WITH (autovacuum_enabled = false);
INSERT INTO device_scope SELECT m, 1 FROM generate_series(1, 90) m WHERE m % 2 = 0;
CREATE TABLE org_grants (id serial PRIMARY KEY, org_id int NOT NULL, dim_a_id int, group_id int, deleted_at timestamptz) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX org_grants_direct ON org_grants (dim_a_id, org_id) WHERE deleted_at IS NULL;
CREATE UNIQUE INDEX org_grants_group ON org_grants (group_id, org_id) WHERE deleted_at IS NULL;
CREATE TABLE org_members (id serial PRIMARY KEY, group_id int NOT NULL, dim_a_id int NOT NULL, deleted_at timestamptz) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX org_members_active ON org_members (group_id, dim_a_id) WHERE deleted_at IS NULL;
INSERT INTO org_members (group_id, dim_a_id) SELECT 1 + (m - 1) / 10, m FROM generate_series(1, 400) m;
INSERT INTO org_grants (org_id, dim_a_id, group_id) VALUES (1, NULL, 2), (1, 55, NULL), (1, NULL, 6), (2, 4, NULL), (2, 12, NULL), (2, NULL, 4);
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX region_scope_journal ON region_scope USING sift_col (dim_a_id, region_id) WITH (track_changes = 'ctid');
CREATE INDEX device_scope_journal ON device_scope USING sift_col (dim_b_id, device_id) WITH (track_changes = 'ctid');
CREATE INDEX org_grants_by_member ON org_grants USING sift_col (dim_a_id, org_id) WITH (track_changes = 'ctid');
CREATE INDEX org_grants_by_group ON org_grants USING sift_col (group_id, org_id) WITH (track_changes = 'ctid');
CREATE INDEX org_members_by_member ON org_members USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int NOT NULL, dim_b_id int NOT NULL, created_at timestamptz NOT NULL, kind smallint NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 1 + i % 400, 1 + (i * 7) % 90, timestamptz '2025-01-01' + i * interval '17 minutes', i % 7, (i * 37) % 1000 FROM generate_series(1, 24000) i;
VACUUM (FREEZE, ANALYZE) events, tenant_scope, region_scope, device_scope, org_grants, org_members;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, dim_b_id, created_at, kind, v)
	WITH (key_order = 'dim_a_id, created_at', key_cut = 'dim_a_id, created_at',
		scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id); region_id: dim_a_id -> region_scope(dim_a_id, region_id);
			org_id: dim_a_id -> org_grants(dim_a_id, org_id) | org_grants(group_id, org_id) via org_members(group_id, dim_a_id);
			device_id: dim_b_id -> device_scope(dim_b_id, device_id)');
SELECT s.format >= 53 AS sidecar_written_by_the_build FROM sift._test_col_formats('events_col') s;
SELECT ord AS scope, count(*) FILTER (WHERE lineage <> 0) AS journals_vouching FROM generate_series(0, 3) ord, LATERAL sift._test_scope_stamps('events_col', ord) GROUP BY ord ORDER BY ord;

CREATE VIEW org_pairs AS
	SELECT dim_a_id AS member, org_id FROM org_grants WHERE deleted_at IS NULL AND dim_a_id IS NOT NULL
	UNION
	SELECT m.dim_a_id, g.org_id FROM org_grants g JOIN org_members m ON m.group_id = g.group_id WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL;

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

-- the statement of a scope (0 tenant, 1 region, 2 org, 3 device) and key, as an application writes it
CREATE FUNCTION q(scope int, k int) RETURNS text LANGUAGE sql AS
$$ SELECT format('SELECT kind, count(*) AS n, sum(v) AS s FROM events e WHERE %s GROUP BY kind', CASE scope
	WHEN 0 THEN format('e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = %s)', k)
	WHEN 1 THEN format('e.dim_a_id IN (SELECT dim_a_id FROM region_scope WHERE region_id = %s)', k)
	WHEN 2 THEN format('e.dim_a_id IN (SELECT member FROM org_pairs WHERE org_id = %s)', k)
	ELSE format('e.dim_b_id IN (SELECT dim_b_id FROM device_scope WHERE device_id = %s)', k) END) $$;

-- the line of a state: the rows against the join with the runs and without them, and the runs the read of each scope took (tenants 1, 2; regions 1, 2; orgs 1, 2; device 1)
CREATE FUNCTION state_line(OUT same_with_runs boolean, OUT same_without_runs boolean, OUT runs_by_read int[]) LANGUAGE plpgsql AS $$
DECLARE r record; join_md5 text; with_md5 text; without_md5 text;
BEGIN
	same_with_runs := true; same_without_runs := true; runs_by_read := '{}';
	FOR r IN SELECT s.scope, k FROM (VALUES (0, 1), (0, 2), (1, 1), (1, 2), (2, 1), (2, 2), (3, 1)) s (scope, k) LOOP
		SET LOCAL sift.enable_agg = off;
		join_md5 := rows_md5(q(r.scope, r.k));
		SET LOCAL sift.enable_agg = on;
		SET LOCAL sift.test_scope_entity = on;
		SET LOCAL sift.test_scope_no_side = off;
		with_md5 := rows_md5(q(r.scope, r.k));
		runs_by_read := runs_by_read || runs_of(q(r.scope, r.k));
		SET LOCAL sift.test_scope_no_side = on;
		without_md5 := rows_md5(q(r.scope, r.k));
		same_with_runs := same_with_runs AND with_md5 = join_md5;
		same_without_runs := same_without_runs AND without_md5 = join_md5;
	END LOOP;
END $$;

CREATE TABLE states (n int, label text, edit text);
INSERT INTO states VALUES
	(1, 'clean', 'SELECT 1'),
	(2, 'pairs of the tenant scope edited', 'DELETE FROM tenant_scope WHERE dim_a_id = 20 AND tenant_id = 1; INSERT INTO tenant_scope VALUES (20, 2)'),
	(3, 'pairs of the region scope edited', 'DELETE FROM region_scope WHERE dim_a_id = 5; INSERT INTO region_scope VALUES (5, 2)'),
	(4, 'a direct grant of the org scope ended', 'UPDATE org_grants SET deleted_at = now() WHERE org_id = 1 AND dim_a_id = 55'),
	(5, 'a member left a group of the org scope', 'UPDATE org_members SET deleted_at = now() WHERE group_id = 4 AND dim_a_id = 35'),
	(6, 'a group of the org scope granted', 'INSERT INTO org_grants (org_id, group_id) VALUES (2, 7)'),
	(7, 'pairs of the device scope edited', 'DELETE FROM device_scope WHERE dim_b_id = 10; INSERT INTO device_scope VALUES (11, 1)'),
	(8, 'rows inserted, deleted and updated', 'INSERT INTO events SELECT 24000 + i, 1 + i % 400, 1 + (i * 7) % 90, timestamptz ''2026-01-01'' + i * interval ''17 minutes'', i % 7, i % 1000 FROM generate_series(1, 200) i;
		DELETE FROM events WHERE dim_a_id IN (3, 25, 66) AND id % 3 = 0;
		UPDATE events SET v = v + 1, kind = (kind + 1) % 7 WHERE dim_a_id IN (7, 33) AND id % 5 = 0'),
	(9, 'VACUUM', 'VACUUM events, tenant_scope, region_scope, device_scope, org_grants, org_members'),
	(10, 'promoted and merged', 'SELECT sift.col_promote(''events_col'') IS NOT NULL AND sift.col_merge(''events_col'') IS NOT NULL'),
	(11, 'REINDEX', 'REINDEX INDEX events_col');
SELECT c FROM states, LATERAL unnest(ARRAY[format('SELECT %s AS n, %L AS state', n, label), edit, 'SELECT * FROM state_line()']) WITH ORDINALITY u(c, o) ORDER BY n, o \gexec

-- the records of the sidecar: a run for each key of the first three scopes, none for the fourth (its member is not in the order of the units)
SELECT sc AS scope, (SELECT count(*) FROM sift._test_side_valid('events_col', sc, 1)) > 0 AS has_runs FROM generate_series(0, 3) sc ORDER BY sc;
SELECT sift.col_verify('events_col', true);

DROP TABLE states;
DROP FUNCTION state_line(), q(int, int), runs_of(text), rows_md5(text);
DROP VIEW org_pairs;
DROP TABLE events, tenant_scope, region_scope, device_scope, org_grants, org_members;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
