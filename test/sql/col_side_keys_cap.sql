-- What a scope sidecar of a segment holds of a scope, and what says so when it holds less. (1) The run records of a scope take an eighth of maintenance_work_mem, and a run takes 56 bytes and its key 16, so at 768 kB
-- (98,304 bytes) the first 1,365 keys of two units each have a run and the key after them has none: the build says so (NOTICE), the read of that key runs the join and says why (Scope Fallback), and the rows are the
-- join's. The bound follows maintenance_work_mem: at twice that every key has its run. (2) The cube of a run keeps the histogram of a dim only when each of its groups stands for 8 rows or more: a key of 7 kinds needs
-- 56 rows, so a key of 54 rows has its run and no histogram of the kind, and the read of the kinds takes the units of the run. (3) A forced merge writes the sidecar of every segment of 16 units or more it leaves.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET jit = off;
SET timezone = 'UTC';
SET sift.col_unit_rows = 150;
SET client_min_messages = warning;

-- the members k and k + n of the key k, 90,000 events of the 2n members in the order of the units
CREATE TABLE cap_pairs (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
CREATE INDEX cap_pairs_journal ON cap_pairs USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE cap_events (id int PRIMARY KEY, dim_a_id int NOT NULL, created_at timestamptz NOT NULL, kind smallint NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false);

CREATE FUNCTION cap_build(mwm_kb int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	DROP INDEX IF EXISTS cap_col;
	EXECUTE format('SET maintenance_work_mem = %s', mwm_kb);
	CREATE INDEX cap_col ON cap_events USING sift_col (id, dim_a_id, created_at, kind, v)
		WITH (key_order = 'dim_a_id, created_at', key_cut = 'dim_a_id, created_at', scope = 'tenant_id: dim_a_id -> cap_pairs(dim_a_id, tenant_id)');
	RESET maintenance_work_mem;
END $$;

-- the keys that have a run in the sidecar, and those whose runs hold the histogram of the kind (attribute 4)
CREATE FUNCTION cap_keys(n int, OUT keys_with_runs int, OUT keys_with_kinds int) LANGUAGE plpgsql AS $$
BEGIN
	SELECT count(*) INTO keys_with_runs FROM generate_series(1, n) k WHERE EXISTS (SELECT 1 FROM sift._test_side_valid('cap_col', 0, k));
	SELECT count(DISTINCT key) INTO keys_with_kinds FROM sift._test_side_cells('cap_col', 0, 4, 5);
END $$;

-- the read of one key by kind: the join's rows or not, the runs the scan took, the rows it read, and the reason it gave when it ran the join
CREATE FUNCTION cap_read(key int, OUT same_as_join boolean, OUT runs int, OUT rows_read int, OUT why text) LANGUAGE plpgsql AS $$
DECLARE
	q text := format('SELECT kind, count(*) AS n, sum(v) AS s FROM cap_events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM cap_pairs WHERE tenant_id = %s) GROUP BY kind', key);
	p jsonb;
	a text;
	b text;
BEGIN
	SET LOCAL sift.agg_scope = on;
	SET LOCAL sift.agg_force = on;
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || q INTO p;
	EXECUTE 'SELECT md5(string_agg(t::text, ''|'' ORDER BY t::text)) FROM (' || q || ') t' INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT md5(string_agg(t::text, ''|'' ORDER BY t::text)) FROM (' || q || ') t' INTO b;
	same_as_join := a = b;
	runs := coalesce(jsonb_path_query_first(p, '$.**."Scope Sidecar Runs"')::text::int, 0);
	rows_read := coalesce(jsonb_path_query_first(p, '$.**."Scope Rows Read"')::text::int, 0);
	why := jsonb_path_query_first(p, '$.**."Scope Fallback"') #>> '{}';
END $$;

-- 768 kB: 98,304 bytes of run records, 72 bytes a key of one run: 1,365 keys. 1,365 keys of the pairs have a run each and the build says nothing
\set n 1365
TRUNCATE cap_events, cap_pairs;
INSERT INTO cap_pairs SELECT k + j * :n, k FROM generate_series(1, :n) k, generate_series(0, 1) j;
INSERT INTO cap_events SELECT i, 1 + i % (2 * :n), timestamptz '2025-01-01' + i * interval '17 minutes', i % 7, (i * 37) % 1000 FROM generate_series(1, 90000) i;
VACUUM (FREEZE, ANALYZE) cap_events, cap_pairs;
SET client_min_messages = notice;
\set VERBOSITY terse
SELECT cap_build(768);
\set VERBOSITY default
SET client_min_messages = warning;
SELECT * FROM cap_keys(1365);
SELECT 'the last key' AS key, r.* FROM cap_read(1365) r;

-- one key more: the build says that the sidecar keeps the first 1,365 of 1,366 keys, the key after them has no run, and its read runs the join and says why
\set n 1366
TRUNCATE cap_events, cap_pairs;
INSERT INTO cap_pairs SELECT k + j * :n, k FROM generate_series(1, :n) k, generate_series(0, 1) j;
INSERT INTO cap_events SELECT i, 1 + i % (2 * :n), timestamptz '2025-01-01' + i * interval '17 minutes', i % 7, (i * 37) % 1000 FROM generate_series(1, 90000) i;
VACUUM (FREEZE, ANALYZE) cap_events, cap_pairs;
SET client_min_messages = notice;
\set VERBOSITY terse
SELECT cap_build(768);
\set VERBOSITY default
SET client_min_messages = warning;
SELECT * FROM cap_keys(1366);
SELECT 'the last key kept' AS key, r.* FROM cap_read(1365) r;
SELECT 'the key after them' AS key, r.* FROM cap_read(1366) r;
SELECT sift.col_verify('cap_col', true);
SELECT count(*) AS scope_problems FROM sift.col_scope_verify('cap_col', 100, ARRAY['1365', '1366']);

-- twice the memory, twice the bound: every key of the 1,366 has its run, and the key that had none reads the runs
SELECT cap_build(1536);
SELECT * FROM cap_keys(1366);
SELECT 'the key that had none' AS key, r.* FROM cap_read(1366) r;

-- a key of 54 rows (90,000 events over 1,650 keys of two members) has its run and no histogram of its 7 kinds (7 groups of 8 rows are 56), so the read of the kinds takes the units of the run; at 1,600 keys (56 rows) the
-- runs hold the kinds and the read takes them without reading a row
\set n 1650
TRUNCATE cap_events, cap_pairs;
INSERT INTO cap_pairs SELECT k + j * :n, k FROM generate_series(1, :n) k, generate_series(0, 1) j;
INSERT INTO cap_events SELECT i, 1 + i % (2 * :n), timestamptz '2025-01-01' + i * interval '17 minutes', i % 7, (i * 37) % 1000 FROM generate_series(1, 90000) i;
VACUUM (FREEZE, ANALYZE) cap_events, cap_pairs;
SELECT cap_build(1536);
SELECT * FROM cap_keys(1650);
SELECT 'a key of 54 rows' AS key, r.same_as_join, r.runs > 0 AS takes_the_run, r.rows_read > 0 AS reads_its_units, r.why FROM cap_read(1) r;
\set n 1600
TRUNCATE cap_events, cap_pairs;
INSERT INTO cap_pairs SELECT k + j * :n, k FROM generate_series(1, :n) k, generate_series(0, 1) j;
INSERT INTO cap_events SELECT i, 1 + i % (2 * :n), timestamptz '2025-01-01' + i * interval '17 minutes', i % 7, (i * 37) % 1000 FROM generate_series(1, 90000) i;
VACUUM (FREEZE, ANALYZE) cap_events, cap_pairs;
SELECT cap_build(1536);
SELECT * FROM cap_keys(1600);
SELECT 'a key of 56 rows' AS key, r.same_as_join, r.runs > 0 AS takes_the_run, r.rows_read > 0 AS reads_its_units, r.why FROM cap_read(1) r;
DROP INDEX cap_col;

-- a forced merge leaves the sidecar on each entity segment of 16 units or more, and none on a smaller one: the build, a promoted segment of 43 units and one of 3 units, merged into one segment of 16 units or more
\set n 300
TRUNCATE cap_events, cap_pairs;
INSERT INTO cap_pairs SELECT k + j * :n, k FROM generate_series(1, :n) k, generate_series(0, 1) j;
INSERT INTO cap_events SELECT i, 1 + i % (2 * :n), timestamptz '2025-01-01' + i * interval '17 minutes', i % 7, (i * 37) % 1000 FROM generate_series(1, 90000) i;
VACUUM (FREEZE, ANALYZE) cap_events, cap_pairs;
SELECT cap_build(65536);
SELECT s.kind, s.units >= 16 AS sixteen_units_or_more, s.format >= 53 AS has_the_sidecar FROM sift.col_segments('cap_col') s WHERE s.kind = 'immutable';
INSERT INTO cap_events SELECT i, 1 + i % 600, timestamptz '2026-01-01' + i * interval '17 minutes', i % 7, (i * 37) % 1000 FROM generate_series(90001, 96000) i;
SELECT sift.col_promote('cap_col') IS NOT NULL AS promoted;
INSERT INTO cap_events SELECT i, 1 + i % 600, timestamptz '2027-01-01' + i * interval '17 minutes', i % 7, (i * 37) % 1000 FROM generate_series(96001, 96300) i;
SELECT sift.col_promote('cap_col') IS NOT NULL AS promoted;
SELECT s.kind, s.units >= 16 AS sixteen_units_or_more, s.format >= 53 AS has_the_sidecar FROM sift.col_segments('cap_col') s WHERE s.kind = 'immutable' ORDER BY s.units DESC;
SELECT sift.col_merge('cap_col', true) IS NOT NULL AS merged;
SELECT s.kind, s.units >= 16 AS sixteen_units_or_more, s.format >= 53 AS has_the_sidecar FROM sift.col_segments('cap_col') s WHERE s.kind = 'immutable';
SELECT * FROM cap_keys(300);
SELECT 'after the merge' AS key, r.same_as_join, r.runs > 0 AS takes_the_run, r.why FROM cap_read(1) r;
SELECT sift.col_verify('cap_col', true);

DROP FUNCTION cap_build(int), cap_keys(int), cap_read(int);
DROP TABLE cap_events, cap_pairs;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
