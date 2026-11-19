-- A run of the scope sidecar answers for its units only while no pair of a member in its range changed since the stamp and none of its units is dirty. Seeded random rounds of pair inserts, deletes
-- and moves, row inserts, deletes and updates, VACUUM, promotion and merge: after every round the rows the entity scan gives for each key, with the runs and without them, are the join's.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.col_unit_rows = 150;
SET timezone = 'UTC';
SET jit = off;
SET client_min_messages = warning;

CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO tenant_scope SELECT m, 1 FROM generate_series(1, 80) m WHERE m % 7 <> 0;
INSERT INTO tenant_scope SELECT m, 2 FROM generate_series(1, 80) m WHERE m % 3 = 0;
INSERT INTO tenant_scope SELECT m, 3 FROM generate_series(30, 50) m;
INSERT INTO tenant_scope SELECT m, 4 FROM generate_series(1, 80) m WHERE m % 4 = 1;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int NOT NULL, created_at timestamptz NOT NULL, kind smallint NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 1 + i % 80, timestamptz '2025-01-01' + i * interval '17 minutes', i % 7, (i * 37) % 1000 FROM generate_series(1, 24000) i;
VACUUM (FREEZE, ANALYZE) events, tenant_scope;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, v)
	WITH (key_order = 'dim_a_id, created_at', key_cut = 'dim_a_id, created_at', scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');

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

CREATE FUNCTION q(key int) RETURNS text LANGUAGE sql AS
$$ SELECT format('SELECT kind, count(*) AS n, sum(v) AS s FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = %s) GROUP BY kind', key) $$;

CREATE TABLE verdicts (round int, key int, same_with_runs bool, same_without_runs bool, runs int);

CREATE FUNCTION check_round(round int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE key int; join_md5 text; with_md5 text; without_md5 text; runs int;
BEGIN
	FOR key IN 1..4 LOOP
		SET LOCAL sift.enable_agg = off;
		join_md5 := rows_md5(q(key));
		SET LOCAL sift.enable_agg = on;
		SET LOCAL sift.test_scope_entity = on;
		SET LOCAL sift.test_scope_no_side = off;
		with_md5 := rows_md5(q(key));
		runs := runs_of(q(key));
		SET LOCAL sift.test_scope_no_side = on;
		without_md5 := rows_md5(q(key));
		INSERT INTO verdicts VALUES (round, key, with_md5 = join_md5, without_md5 = join_md5, runs);
	END LOOP;
END $$;

CREATE FUNCTION step(round int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE op int := floor(random() * 7); m int := 1 + floor(random() * 80); k int := 1 + floor(random() * 4); n int;
BEGIN
	IF op = 0 THEN DELETE FROM tenant_scope WHERE dim_a_id = m AND tenant_id = k;
	ELSIF op = 1 THEN INSERT INTO tenant_scope VALUES (m, k) ON CONFLICT DO NOTHING;
	ELSIF op = 2 THEN UPDATE tenant_scope SET tenant_id = k WHERE dim_a_id = m AND tenant_id = (SELECT min(tenant_id) FROM tenant_scope t WHERE t.dim_a_id = m AND t.tenant_id <> k) AND NOT EXISTS (SELECT 1 FROM tenant_scope t WHERE t.dim_a_id = m AND t.tenant_id = k);
	ELSIF op = 3 THEN
		SELECT max(id) INTO n FROM events;
		INSERT INTO events SELECT n + i, 1 + (n + i) % 80, timestamptz '2025-01-01' + (n + i) * interval '17 minutes', (n + i) % 7, ((n + i) * 37) % 1000 FROM generate_series(1, 200) i;
	ELSIF op = 4 THEN DELETE FROM events WHERE dim_a_id = m AND id % 3 = 0;
	ELSIF op = 5 THEN UPDATE events SET v = v + 1, kind = (kind + 1) % 7 WHERE dim_a_id = m AND id % 5 = 0;
	ELSE DELETE FROM tenant_scope WHERE dim_a_id = m AND tenant_id = k; INSERT INTO tenant_scope VALUES (m, 1 + k % 4) ON CONFLICT DO NOTHING;
	END IF;
END $$;

CREATE FUNCTION round_of(round int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	IF round % 3 <> 0 THEN
		PERFORM sift.col_promote('events_col');
		PERFORM sift.col_merge('events_col');
		PERFORM sift.col_maintain('events_col');
	END IF;
	PERFORM step(round);
	IF round % 2 = 0 THEN PERFORM step(round); END IF;
	PERFORM check_round(round);
END $$;

-- a merge and the maintenance of the index while a transaction holds a pair change back (its own here) end: a sidecar made under the same horizon would be as stale as the one it replaces, and what it
-- made from the pairs is that of what is committed, as the rollback leaves them
SET statement_timeout = '120s';
BEGIN;
DELETE FROM tenant_scope WHERE dim_a_id = 9;
SELECT sift.col_merge('events_col') >= 0 AS the_merge_ends;
ROLLBACK;
BEGIN;
DELETE FROM tenant_scope WHERE tenant_id <= 3;
SELECT sift.col_maintain('events_col') IS NOT NULL AS the_maintenance_ends;
SELECT sift.col_merge('events_col') >= 0 AS the_next_merge_ends;
ROLLBACK;
RESET statement_timeout;

SELECT setseed(0.42);
SELECT check_round(0);
\set ECHO none
SELECT format('SELECT round_of(%s)', r) FROM generate_series(1, 40) r \gexec
\set ECHO all
VACUUM events, tenant_scope;
SELECT check_round(41);
SELECT sift.col_merge('events_col') IS NOT NULL AS merged;
SELECT check_round(42);

SELECT count(*) AS checks, count(*) FILTER (WHERE NOT same_with_runs) AS wrong_with_runs, count(*) FILTER (WHERE NOT same_without_runs) AS wrong_without_runs,
       count(*) FILTER (WHERE runs > 0) > 0 AS runs_taken FROM verdicts;
SELECT sift.col_verify('events_col', true);

DROP FUNCTION round_of(int), step(int), check_round(int), q(int), runs_of(text), rows_md5(text);
DROP TABLE verdicts, events, tenant_scope;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
