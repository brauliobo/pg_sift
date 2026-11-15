-- A probe of the point directory is a block or more for each key and a unit it cuts spares a few: a list of many more keys than the units still kept (a scope list
-- joined to a table, a long IN list) cannot pay for its probes, and the units answer it (sift_col scan prune_point.c). A short list is still looked up. The rows are the heap's either way.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_col_legacy_options = on;
SET sift.async_maintenance = off;

CREATE TABLE pmk (id int, u int, v int, pad text DEFAULT repeat('x', 400));
ALTER TABLE pmk ALTER COLUMN pad SET STORAGE PLAIN;
-- u: a key of one row; the table is a few dozen units
INSERT INTO pmk (id, u, v) SELECT i, (i::bigint * 7919 % 30011)::int, i % 7 FROM generate_series(1, 30000) i;
CREATE INDEX pmk_ix ON pmk USING sift_col (id, u, v) WITH (key_columns = 'off', point_columns = 'u');
ANALYZE pmk;

CREATE FUNCTION pmk_from_point(w text) RETURNS text LANGUAGE plpgsql
	SET sift.agg_force = on SET max_parallel_workers_per_gather = 0 SET jit = off AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE format('EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) SELECT v, count(*) FROM pmk WHERE %s GROUP BY v', w) LOOP
		IF l ~ 'from point directory' THEN RETURN 'directory'; END IF;
	END LOOP;
	RETURN 'units';
END $$;

CREATE TABLE pmk_lists AS
SELECT n, (SELECT string_agg((g * 13)::text, ',') FROM generate_series(1, n) g) AS keys FROM (VALUES (5), (60), (200), (2000)) t(n);

SELECT n, pmk_from_point(format('u IN (%s)', keys)) AS answered_by FROM pmk_lists ORDER BY n;

-- the same rows with the directory ignored (a copy of the table without the option)
CREATE TABLE pmk_ref AS SELECT * FROM pmk;
SELECT n, (SELECT sum(v) FROM pmk WHERE u = ANY (string_to_array(keys, ',')::int[])) = (SELECT sum(v) FROM pmk_ref WHERE u = ANY (string_to_array(keys, ',')::int[])) AS same_rows
FROM pmk_lists ORDER BY n;

DROP FUNCTION pmk_from_point(text);
DROP TABLE pmk, pmk_ref, pmk_lists;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
