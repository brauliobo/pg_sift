-- A run of the scope sidecar is taken whichever item of the scan holds its units, and whichever of its units the time range of the scan leaves: a segment of more than 64 units is read in items of
-- 64 superblocks, the items end where no run of the key is cut (sift_col_super_align), and a run is taken at the first of its units the cursor reaches (col/scan/side.c), not at its first unit.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 2;
SET max_parallel_maintenance_workers = 0;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET sift.col_unit_rows = 150;
SET timezone = 'UTC';
SET jit = off;
SET client_min_messages = warning;

CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id));
INSERT INTO tenant_scope SELECT m, 1 FROM generate_series(1, 60) m WHERE m % 6 < 5;
INSERT INTO tenant_scope SELECT m, 2 FROM generate_series(1, 60) m WHERE m % 3 = 0;
INSERT INTO tenant_scope SELECT m, 3 FROM generate_series(1, 60) m WHERE m IN (7, 8, 41);
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int NOT NULL, created_at timestamptz NOT NULL, kind smallint NOT NULL, v int NOT NULL);
INSERT INTO events SELECT i, 1 + i % 60, timestamptz '2025-01-01' + i * interval '17 minutes', i % 7, (i * 37) % 1000 FROM generate_series(1, 24000) i;
VACUUM (FREEZE, ANALYZE) events, tenant_scope;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, v)
	WITH (key_order = 'dim_a_id, created_at', key_cut = 'dim_a_id, created_at', scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');

CREATE FUNCTION q(key int, t_from text, t_to text) RETURNS text LANGUAGE sql AS
$$ SELECT format('SELECT kind, count(*) AS n, sum(v) AS s FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = %s) AND e.created_at >= %L AND e.created_at < %L GROUP BY kind', key, t_from, t_to) $$;

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

-- the cubes the sidecar holds for the months of the range (a run hands over one for each: Scope Sidecar Runs), and the runs that lie over the item boundary (unit 64 holds the members from 25 on: 150-row
-- units, 400 rows a member)
CREATE FUNCTION state(key int, t_from text, t_to text)
RETURNS TABLE (k int, same_as_join bool, run_over_the_boundary bool, handed_over int, in_the_sidecar int, all_handed_over bool) LANGUAGE plpgsql AS $$
DECLARE join_md5 text; entity_md5 text; handed int; made int; over bool;
BEGIN
	SET LOCAL sift.agg_scope = on;
	SET LOCAL sift.enable_agg = off;
	join_md5 := rows_md5(q(key, t_from, t_to));
	SET LOCAL sift.enable_agg = on;
	SET LOCAL sift.test_scope_entity = on;
	entity_md5 := rows_md5(q(key, t_from, t_to));
	handed := runs_of(q(key, t_from, t_to));
	SELECT count(DISTINCT (c.run, c.cell)) INTO made FROM sift._test_side_cells('events_col', 0, 4, 5) c
	 WHERE c.key = state.key AND c.cell >= t_from::date - date '2000-01-01' AND c.cell < t_to::date - date '2000-01-01';
	SELECT EXISTS (SELECT 1 FROM sift._test_side_valid('events_col', 0, state.key) v WHERE v.lo <= 24 AND v.hi >= 25) INTO over;
	RETURN QUERY SELECT key, entity_md5 = join_md5, over, handed, made, handed = made;
END $$;

-- the whole history of the key, then months that are late in it: the first unit of each run holds rows of the first months only
SELECT * FROM state(1, '2025-01-01', '2025-10-01') UNION ALL SELECT * FROM state(2, '2025-01-01', '2025-10-01') UNION ALL SELECT * FROM state(3, '2025-01-01', '2025-10-01') ORDER BY 1;
SELECT * FROM state(1, '2025-08-01', '2025-10-01') UNION ALL SELECT * FROM state(2, '2025-08-01', '2025-10-01') UNION ALL SELECT * FROM state(3, '2025-08-01', '2025-10-01') ORDER BY 1;
SELECT * FROM state(1, '2025-09-01', '2025-10-01') UNION ALL SELECT * FROM state(2, '2025-09-01', '2025-10-01') UNION ALL SELECT * FROM state(3, '2025-09-01', '2025-10-01') ORDER BY 1;

DROP FUNCTION state(int, text, text);
DROP FUNCTION runs_of(text);
DROP FUNCTION rows_md5(text);
DROP FUNCTION q(int, text, text);
DROP TABLE events, tenant_scope;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
