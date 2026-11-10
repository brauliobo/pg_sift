-- A table with a b-tree and a sift_col index (an order array on the key, no point directory) looks up an IN list of keys by the one that reads them for less. The index scan on the
-- order array is priced by its microseconds (a key sought in the sorted keys, 2.5 us measured) and the b-tree's by PostgreSQL's units of a cached probe (nine to the microsecond):
-- 500 scattered keys of 300,000 rows were taken from the b-tree for three times its time (0.8 ms against 0.4); the cost of the keys the index looks up in its order array or
-- its units now follows the time. The answers are the same either way.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;

CREATE TABLE pb (id int, v int, pad text DEFAULT repeat('x', 100));
INSERT INTO pb (id, v) SELECT (i::bigint * 7919) % 300007, i % 997 FROM generate_series(1, 300000) i;
CREATE INDEX pb_col ON pb USING sift_col (id, v) WITH (order_array_columns = 'id');
CREATE INDEX pb_bt ON pb (id);
VACUUM (FREEZE, ANALYZE) pb;
CREATE TABLE pb_keys AS SELECT n, string_agg(((i::bigint * 7919) % 300007)::text, ',' ORDER BY i) AS list
FROM unnest(ARRAY[1, 60, 500]) n, generate_series(1, 500) i WHERE i <= n GROUP BY n;

CREATE FUNCTION pb_path(sql text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'heap';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || sql LOOP
		IF l ~ 'SiftAgg' THEN r := 'siftagg'; EXIT;
		ELSIF l ~ 'pb_bt' THEN r := 'btree'; EXIT;
		ELSIF l ~ 'pb_col' THEN r := 'sift_col'; EXIT;
		END IF;
	END LOOP;
	RETURN r;
END $$;

SELECT k AS keys, pb_path(format('SELECT count(*), sum(v) FROM pb WHERE id IN (%s)', (SELECT list FROM pb_keys WHERE n = k))) AS path
FROM unnest(ARRAY[60, 500]) k;

CREATE FUNCTION pb_run(k int, agg boolean) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_col_scan', agg::text, true);
	PERFORM set_config('sift.enable_agg', agg::text, true);
	EXECUTE format('SELECT concat_ws(%L, count(*), sum(v)) FROM pb WHERE id IN (%s)', ' ', (SELECT list FROM pb_keys WHERE n = k)) INTO r;
	RETURN r;
END $$;
SELECT k AS keys, pb_run(k, true) = pb_run(k, false) AS same_rows FROM unnest(ARRAY[1, 60, 500]) k;

DROP FUNCTION pb_path(text), pb_run(int, boolean);
DROP TABLE pb, pb_keys;
DROP EXTENSION pg_sift;
