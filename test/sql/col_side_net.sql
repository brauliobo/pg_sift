-- A segment whose dead rows VACUUM answered with net pre-aggregates keeps its scope sidecar in use and gets a new one when the pairs change: a read takes the runs whose units are clean (the planner counts
-- them, so the join is not run for want of runs), the runs of changed members are read by reference until the maintenance step writes the sidecar again, and that step keeps the net pre-aggregates.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.col_unit_rows = 150;
SET timezone = 'UTC';
SET jit = off;
SET client_min_messages = warning;

CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id));
INSERT INTO tenant_scope SELECT m, 1 FROM generate_series(1, 60) m WHERE m % 6 < 5;
INSERT INTO tenant_scope SELECT m, 2 FROM generate_series(1, 60) m WHERE m % 3 = 0;
INSERT INTO tenant_scope SELECT m, 3 FROM generate_series(1, 60) m WHERE m IN (7, 8);
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int NOT NULL, created_at timestamptz NOT NULL, kind smallint NOT NULL, v int NOT NULL);
INSERT INTO events SELECT i, 1 + i % 60, timestamptz '2025-01-01' + i * interval '17 minutes', i % 7, (i * 37) % 1000 FROM generate_series(1, 24000) i;
VACUUM (FREEZE, ANALYZE) events, tenant_scope;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, v)
	WITH (key_order = 'dim_a_id, created_at', key_cut = 'dim_a_id, created_at', scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');

CREATE FUNCTION q(key int) RETURNS text LANGUAGE sql AS
$$ SELECT format('SELECT kind, count(*) AS n, sum(v) AS s FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = %s) GROUP BY kind', key) $$;

CREATE FUNCTION rows_md5(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	DROP TABLE IF EXISTS side_rows;
	EXECUTE 'CREATE TEMP TABLE side_rows AS ' || q;
	SELECT md5(coalesce(string_agg(t::text, '|' ORDER BY t::text), '')) INTO r FROM side_rows t;
	DROP TABLE side_rows;
	RETURN r;
END $$;

-- the statement as the planner chooses it: the runs the scan took, and whether units answered net of their dead rows
CREATE FUNCTION plan_of(q text, OUT runs int, OUT net_units bool, OUT joined bool) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	runs := 0; net_units := false; joined := false;
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ 'Scope Sidecar Runs' THEN runs := regexp_replace(l, '^.*: ', '')::int; END IF;
		IF l ~ '[1-9][0-9]* net of dead rows' THEN net_units := true; END IF;
		IF l ~ 'Scope Fallback' THEN joined := true; END IF;
	END LOOP;
END $$;

CREATE FUNCTION state(key int) RETURNS TABLE (k int, same_as_join bool, runs_taken bool, net_units_answer bool, join_run bool) LANGUAGE plpgsql AS $$
DECLARE join_md5 text; scope_md5 text; p record;
BEGIN
	SET LOCAL sift.enable_agg = off;
	join_md5 := rows_md5(q(key));
	SET LOCAL sift.enable_agg = on;
	scope_md5 := rows_md5(q(key));
	p := plan_of(q(key));
	RETURN QUERY SELECT key, scope_md5 = join_md5, p.runs > 0, p.net_units, p.joined;
END $$;

CREATE FUNCTION runs_valid(key int) RETURNS TABLE (runs bigint, valid_runs bigint) LANGUAGE sql AS
$$ SELECT count(*), count(*) FILTER (WHERE valid) FROM sift._test_side_valid('events_col', 0, key) $$;

SELECT * FROM state(1);

-- every row of one member of key 1 deleted: its units hold dead rows, which VACUUM answers net, and no other run of the key is touched
DELETE FROM events WHERE dim_a_id = 1;
VACUUM events;
SELECT dirty AS has_dead_rows, dead_rows > 0 AS counts_them FROM sift.col_segments('events_col') WHERE kind = 'immutable';
SELECT * FROM state(1);
SELECT * FROM runs_valid(1);

-- pairs of key 1 change: the runs they lie in are not taken, and the maintenance step that follows (a merge step of a segment with net pre-aggregates) writes the sidecar again
DELETE FROM tenant_scope WHERE tenant_id = 1 AND dim_a_id IN (10, 11, 40);
INSERT INTO tenant_scope VALUES (1, 3);
SELECT * FROM runs_valid(1);
SELECT * FROM state(1);
SELECT sift.col_merge('events_col') AS merge_steps;
SELECT * FROM runs_valid(1);
SELECT * FROM state(1);
SELECT * FROM state(2);
SELECT * FROM state(3);
SELECT sift.col_verify('events_col', true) IS NOT NULL AS verified;
SELECT sift.col_merge('events_col') AS second_call_writes;

-- a delete burst that leaves a dead row in every unit: no run stands for a dirty unit, so the rewrite that the VACUUM finds worth its rows brings the runs back
DROP INDEX events_col;
INSERT INTO events SELECT i, 1 + i % 60, timestamptz '2025-01-01' + i * interval '17 minutes', i % 7, (i * 37) % 1000 FROM generate_series(24001, 36000) i;
VACUUM (FREEZE, ANALYZE) events;
SET sift.col_unit_rows = 1500;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, v)
	WITH (key_order = 'dim_a_id, created_at', key_cut = 'dim_a_id, created_at', scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
SELECT * FROM state(1);
DELETE FROM events WHERE (hashint4(id) & 2147483647) % 50 = 0;
SELECT * FROM state(1);
VACUUM events;
SELECT count(*) FILTER (WHERE dirty) AS dirty_segments FROM sift.col_segments('events_col') WHERE kind = 'immutable';
SELECT * FROM runs_valid(1);
SELECT * FROM state(1);
SELECT * FROM state(2);
SELECT sift.col_verify('events_col', true) IS NOT NULL AS verified;

-- a delete burst and a key update burst that no VACUUM has followed: pages are not all-visible, so the units are read by reference and every row is vetted, whole key and month cell
CREATE FUNCTION month_same(key int) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE q text; join_md5 text; scope_md5 text;
BEGIN
	q := format('SELECT kind, count(*) AS n, sum(v) AS s FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = %s) AND e.created_at >= %L AND e.created_at < %L GROUP BY kind',
				key, '2025-06-01', '2025-07-01');
	SET LOCAL sift.enable_agg = off;
	join_md5 := rows_md5(q);
	SET LOCAL sift.enable_agg = on;
	scope_md5 := rows_md5(q);
	RETURN scope_md5 = join_md5;
END $$;
DELETE FROM events WHERE (hashint4(id) & 2147483647) % 50 = 1;
UPDATE events SET created_at = created_at + interval '1 day' WHERE (hashint4(id) & 2147483647) % 100 = 2;
UPDATE events SET dim_a_id = 1 + dim_a_id % 60 WHERE (hashint4(id) & 2147483647) % 200 = 3;
SELECT * FROM state(1);
SELECT * FROM state(2);
SELECT month_same(1) AS month_1, month_same(2) AS month_2;
SELECT sift.col_verify('events_col', true) IS NOT NULL AS verified;

DROP FUNCTION month_same(int);
DROP FUNCTION state(int);
DROP FUNCTION runs_valid(int);
DROP FUNCTION plan_of(text);
DROP FUNCTION rows_md5(text);
DROP FUNCTION q(int);
DROP TABLE events, tenant_scope;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
