-- The latest rows of a scope key (ORDER BY the time leaf DESC LIMIT n) are the join's whatever the filter keeps (all rows, 10%, 1%, 0.1%, none), with the runs of the sidecar, without them and as the
-- planner picks the arm, and the entity arm reads no more rows than the key has: a page with no filter reads the ends of a few units, a filter that keeps few rows reads the units that hold them.
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
SELECT sift.col_merge('events_col') IS NOT NULL AS merged;

CREATE TABLE shapes (label text, cond text, lim int);
INSERT INTO shapes VALUES ('all rows', 'true', 20), ('10%', 'v >= 900', 20), ('1%', 'v >= 990', 20), ('0.1%', 'v >= 999', 20),
	('dim and range', 'kind = 3 AND v >= 500', 20), ('no row', 'v >= 1000', 20), ('page of 100, 1%', 'v >= 990', 100), ('one row, 10%', 'v >= 900', 1);

CREATE FUNCTION q(key int, cond text, lim int) RETURNS text LANGUAGE sql AS
$$ SELECT format('SELECT e.id, e.created_at FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = %s) AND %s ORDER BY e.created_at DESC, e.id LIMIT %s', key, cond, lim) $$;

CREATE FUNCTION rows_md5(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	DROP TABLE IF EXISTS latest_rows;
	EXECUTE 'CREATE TEMP TABLE latest_rows AS ' || q;
	SELECT md5(coalesce(string_agg(t::text, '|' ORDER BY t::text), '')) INTO r FROM latest_rows t;
	DROP TABLE latest_rows;
	RETURN r;
END $$;

CREATE FUNCTION scanned_of(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE l text; r bigint := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ '^ *Rows: ' THEN r := substring(l from 'Rows: ([0-9]+) scanned')::bigint; END IF;
	END LOOP;
	RETURN r;
END $$;

CREATE FUNCTION latest(key int) RETURNS TABLE (k int, shape text, entity_is_join bool, no_runs_is_join bool, planner_is_join bool, page_rows bigint, scanned_le_key bool) LANGUAGE plpgsql AS $$
DECLARE s record; join_md5 text; key_rows bigint;
BEGIN
	SELECT count(*) INTO key_rows FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = key);
	FOR s IN SELECT * FROM shapes LOOP
		SET LOCAL sift.agg_scope = on;
		SET LOCAL sift.enable_agg = off;
		join_md5 := rows_md5(q(key, s.cond, s.lim));
		EXECUTE 'SELECT count(*) FROM (' || q(key, s.cond, s.lim) || ') x' INTO page_rows;
		SET LOCAL sift.enable_agg = on;
		SET LOCAL sift.test_scope_entity = on;
		SET LOCAL sift.test_scope_no_side = off;
		entity_is_join := rows_md5(q(key, s.cond, s.lim)) = join_md5;
		scanned_le_key := scanned_of(q(key, s.cond, s.lim)) <= key_rows;
		SET LOCAL sift.test_scope_no_side = on;
		no_runs_is_join := rows_md5(q(key, s.cond, s.lim)) = join_md5;
		SET LOCAL sift.test_scope_entity = off;
		SET LOCAL sift.test_scope_no_side = off;
		planner_is_join := rows_md5(q(key, s.cond, s.lim)) = join_md5;
		k := key;
		shape := s.label;
		RETURN NEXT;
	END LOOP;
END $$;

SELECT * FROM latest(1) UNION ALL SELECT * FROM latest(2) UNION ALL SELECT * FROM latest(3) ORDER BY 1, 2;

-- with no filter the entity arm reads the ends of a few units, not the rows of the key
SET sift.test_scope_entity = on;
SELECT scanned_of(q(1, 'true', 20)) BETWEEN 1 AND (SELECT count(*) FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 1)) / 2 AS page_reads_few_rows;
RESET sift.test_scope_entity;

-- a key with no member reads no unit: the page is empty and the scan opens nothing, for every shape and for an aggregate
SET sift.test_scope_entity = on;
SELECT shape, page_rows, entity_is_join, scanned_le_key FROM latest(99) ORDER BY 1;
SELECT scanned_of(q(99, cond, lim)) AS rows_scanned, label FROM shapes ORDER BY label;
SELECT scanned_of('SELECT kind, count(*) FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 99) GROUP BY kind') AS aggregate_rows_scanned;
RESET sift.test_scope_entity;

-- the same after pairs and rows changed
DELETE FROM tenant_scope WHERE tenant_id = 1 AND dim_a_id IN (10, 11, 40);
INSERT INTO tenant_scope VALUES (5, 1), (17, 1), (12, 3);
INSERT INTO events SELECT i, 1 + i % 60, timestamptz '2025-01-01' + i * interval '17 minutes', i % 7, (i * 37) % 1000 FROM generate_series(24001, 24300) i;
DELETE FROM events WHERE id % 97 = 0;
SELECT * FROM latest(1) UNION ALL SELECT * FROM latest(3) ORDER BY 1, 2;

DROP FUNCTION latest(int);
DROP FUNCTION scanned_of(text);
DROP FUNCTION rows_md5(text);
DROP FUNCTION q(int, text, int);
DROP TABLE shapes, events, tenant_scope;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
