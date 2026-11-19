-- A run of the sidecar of a scope of two paths answers for its units only while no member in its range has a pair that changed since the stamp of any of the three journals, and none of its units is
-- dirty. Seeded random rounds (the first twenty edit the pairs only, with no maintenance, against the sidecar the build made; the others edit the facts every fifth round and keep the maintenance going) of direct grants made and ended, groups granted and revoked, members added to, removed from and moved between groups, a tenant scope beside it (the second scope of the
-- index) edited, row inserts, deletes and updates, VACUUM, promotion and merge: after every round the rows the entity scan gives for each key of each scope, with the runs and without them, are the join's.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.col_unit_rows = 150;
SET timezone = 'UTC';
SET jit = off;
SET client_min_messages = warning;

CREATE TABLE grants (id serial PRIMARY KEY, org_id int NOT NULL, dim_a_id int, group_id int, deleted_at timestamptz) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX grants_direct ON grants (dim_a_id, org_id) WHERE deleted_at IS NULL;
CREATE UNIQUE INDEX grants_group ON grants (group_id, org_id) WHERE deleted_at IS NULL;
CREATE TABLE memberships (id serial PRIMARY KEY, group_id int NOT NULL, dim_a_id int NOT NULL, deleted_at timestamptz) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX memberships_active ON memberships (group_id, dim_a_id) WHERE deleted_at IS NULL;
CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO memberships (group_id, dim_a_id) SELECT 1 + (m - 1) / 8, m FROM generate_series(1, 400) m;
INSERT INTO memberships (group_id, dim_a_id) SELECT 1 + m % 10, m FROM generate_series(1, 400, 3) m ON CONFLICT DO NOTHING;
INSERT INTO grants (org_id, dim_a_id, group_id) VALUES (1, NULL, 1), (1, NULL, 6), (1, 33, NULL), (2, NULL, 2), (2, 5, NULL), (2, 40, NULL), (3, NULL, 3), (3, NULL, 9), (4, 7, NULL), (4, 8, NULL), (4, 61, NULL);
INSERT INTO tenant_scope SELECT m, 1 FROM generate_series(1, 400) m WHERE m % 5 <> 0;
INSERT INTO tenant_scope SELECT m, 2 FROM generate_series(30, 200) m;
CREATE INDEX grants_by_member ON grants USING sift_col (dim_a_id, org_id) WITH (track_changes = 'ctid');
CREATE INDEX grants_by_group ON grants USING sift_col (group_id, org_id) WITH (track_changes = 'ctid');
CREATE INDEX memberships_by_member ON memberships USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid');
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int NOT NULL, created_at timestamptz NOT NULL, kind smallint NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 1 + i % 400, timestamptz '2025-01-01' + i * interval '17 minutes', i % 7, (i * 37) % 1000 FROM generate_series(1, 24000) i;
VACUUM (FREEZE, ANALYZE) events, grants, memberships, tenant_scope;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, v)
	WITH (key_order = 'dim_a_id, created_at', key_cut = 'dim_a_id, created_at',
		scope = 'org_id: dim_a_id -> grants(dim_a_id, org_id) | grants(group_id, org_id) via memberships(group_id, dim_a_id); tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
CREATE VIEW org_pairs AS
	SELECT dim_a_id AS member, org_id FROM grants WHERE deleted_at IS NULL AND dim_a_id IS NOT NULL
	UNION
	SELECT m.dim_a_id, g.org_id FROM grants g JOIN memberships m ON m.group_id = g.group_id WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL;

CREATE FUNCTION rows_md5(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	DROP TABLE IF EXISTS prop_rows;
	EXECUTE 'CREATE TEMP TABLE prop_rows AS ' || q;
	SELECT md5(coalesce(string_agg(t::text, '|' ORDER BY t::text), '')) INTO r FROM prop_rows t;
	DROP TABLE prop_rows;
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

CREATE FUNCTION q(scope int, key int) RETURNS text LANGUAGE sql AS
$$ SELECT format('SELECT kind, count(*) AS n, sum(v) AS s FROM events e WHERE e.dim_a_id IN (%s) GROUP BY kind', CASE scope
	WHEN 0 THEN format('SELECT member FROM org_pairs WHERE org_id = %s', key)
	ELSE format('SELECT dim_a_id FROM tenant_scope WHERE tenant_id = %s', key) END) $$;

CREATE TABLE verdicts (round int, scope int, key int, same_with_runs bool, same_without_runs bool, runs int);

CREATE FUNCTION check_round(round int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE r record; join_md5 text; with_md5 text; without_md5 text; runs int;
BEGIN
	FOR r IN SELECT s.scope, k FROM (VALUES (0, 1), (0, 2), (0, 3), (0, 4), (1, 1), (1, 2)) s (scope, k) LOOP
		SET LOCAL sift.enable_agg = off;
		join_md5 := rows_md5(q(r.scope, r.k));
		SET LOCAL sift.enable_agg = on;
		SET LOCAL sift.test_scope_entity = on;
		SET LOCAL sift.test_scope_no_side = off;
		with_md5 := rows_md5(q(r.scope, r.k));
		runs := runs_of(q(r.scope, r.k));
		SET LOCAL sift.test_scope_no_side = on;
		without_md5 := rows_md5(q(r.scope, r.k));
		INSERT INTO verdicts VALUES (round, r.scope, r.k, with_md5 = join_md5, without_md5 = join_md5, runs);
	END LOOP;
END $$;

CREATE FUNCTION step(round int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE op int := CASE WHEN round > 20 AND round % 5 = 0 THEN 7 + floor(random() * 3) ELSE (ARRAY[0, 1, 2, 3, 4, 5, 6, 10, 11])[1 + floor(random() * 9)] END; m int := 1 + floor(random() * 400); k int := 1 + floor(random() * 4); g int := coalesce((SELECT group_id FROM grants WHERE group_id IS NOT NULL AND deleted_at IS NULL ORDER BY random() LIMIT 1), 1); n int;
BEGIN
	IF op = 0 THEN INSERT INTO grants (org_id, dim_a_id) VALUES (k, m) ON CONFLICT DO NOTHING;
	ELSIF op = 1 THEN UPDATE grants SET deleted_at = now() WHERE id = (SELECT id FROM grants WHERE dim_a_id IS NOT NULL AND deleted_at IS NULL ORDER BY random() LIMIT 1);
	ELSIF op = 2 THEN INSERT INTO grants (org_id, group_id) VALUES (k, g) ON CONFLICT DO NOTHING;
	ELSIF op = 3 THEN UPDATE grants SET deleted_at = now() WHERE id = (SELECT id FROM grants WHERE group_id IS NOT NULL AND deleted_at IS NULL ORDER BY random() LIMIT 1);
	ELSIF op = 4 THEN INSERT INTO memberships (group_id, dim_a_id) VALUES (g, m) ON CONFLICT DO NOTHING;
	ELSIF op = 5 THEN UPDATE memberships SET deleted_at = now() WHERE id = (SELECT id FROM memberships WHERE group_id = g AND deleted_at IS NULL ORDER BY random() LIMIT 1);
	ELSIF op = 6 THEN UPDATE memberships s SET group_id = 1 + g % 10 WHERE s.id = (SELECT id FROM memberships WHERE group_id = g AND deleted_at IS NULL ORDER BY random() LIMIT 1)
		AND NOT EXISTS (SELECT 1 FROM memberships x WHERE x.group_id = 1 + g % 10 AND x.dim_a_id = s.dim_a_id AND x.deleted_at IS NULL);
	ELSIF op = 7 THEN
		SELECT max(id) INTO n FROM events;
		INSERT INTO events SELECT n + i, 1 + (n + i) % 400, timestamptz '2025-01-01' + (n + i) * interval '17 minutes', (n + i) % 7, ((n + i) * 37) % 1000 FROM generate_series(1, 200) i;
	ELSIF op = 8 THEN DELETE FROM events WHERE dim_a_id = m AND id % 3 = 0;
	ELSIF op = 9 THEN UPDATE events SET v = v + 1, kind = (kind + 1) % 7 WHERE dim_a_id = m AND id % 5 = 0;
	ELSIF op = 10 THEN DELETE FROM tenant_scope WHERE dim_a_id = m AND tenant_id = 1 + k % 2;
	ELSE INSERT INTO tenant_scope VALUES (m, 1 + k % 2) ON CONFLICT DO NOTHING;
	END IF;
END $$;

CREATE FUNCTION round_of(round int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	IF round > 20 AND round % 5 = 1 THEN
		PERFORM sift.col_promote('events_col');
		PERFORM sift.col_merge('events_col');
	END IF;
	IF round > 20 AND round % 7 = 0 THEN PERFORM sift.col_maintain('events_col'); END IF;
	PERFORM step(round);
	IF round % 2 = 0 THEN PERFORM step(round); END IF;
	PERFORM check_round(round);
END $$;

SELECT setseed(0.42);
SELECT check_round(0);
\set ECHO none
SELECT c FROM generate_series(1, 60) r, LATERAL unnest(ARRAY[CASE WHEN r > 20 AND r % 5 = 1 THEN 'VACUUM events' END, format('SELECT round_of(%s)', r)]) WITH ORDINALITY u(c, o)
	WHERE c IS NOT NULL ORDER BY r, o \gexec
\set ECHO all
VACUUM events, grants, memberships, tenant_scope;
SELECT check_round(61);
SELECT sift.col_merge('events_col') IS NOT NULL AS merged;
SELECT check_round(62);

SELECT scope, count(*) AS checks, count(*) FILTER (WHERE NOT same_with_runs) AS wrong_with_runs, count(*) FILTER (WHERE NOT same_without_runs) AS wrong_without_runs,
       count(*) FILTER (WHERE runs > 0) > 0 AS runs_taken FROM verdicts GROUP BY scope ORDER BY scope;
SELECT sift.col_verify('events_col', true);

DROP FUNCTION round_of(int), step(int), check_round(int), q(int, int), runs_of(text), rows_md5(text);
DROP VIEW org_pairs;
DROP TABLE verdicts, events, tenant_scope, grants, memberships;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
