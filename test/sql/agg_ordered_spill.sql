-- The values of an ordered set that outgrow work_mem go to sorted runs in a temporary file, merged when the scan has ended (agg/osa_runs.c): the rows agree with
-- the normal plan when the hash spills (many values repeated), when the array spills (distinct values), with runs merged while the scan goes on (more
-- than 16 runs), with NULLs, a FILTER, a mode() on the same argument and groups that have no value.
-- (percentile_cont is asked for multiples of 1/4 of values that are multiples of 1/8: the server's own interpolation may be built with fused multiply-adds,
-- which this library is not)
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;

SET max_parallel_workers_per_gather = 0;
SET work_mem = '64kB';
SET sift.agg_force = on;

CREATE TABLE sp (id int, g int, x float8, d float8, b bigint, nx float8) WITH (fillfactor = 20, autovacuum_enabled = false);
INSERT INTO sp SELECT i, i % 7,
	(i * 7919 % 100003) / 8.0,
	(i * 31 % 3001)::float8,
	(i::bigint * 104729 % 1000003) - 500000,
	CASE WHEN i % 13 = 0 THEN NULL ELSE (i * 17 % 100019) / 8.0 END
FROM generate_series(1, 90000) i;
CREATE INDEX sp_col ON sp USING sift_col (id, g, x, d, b, nx);
VACUUM ANALYZE sp;

CREATE FUNCTION sp_rows(q text, push bool) RETURNS jsonb[] LANGUAGE sql AS $$ SELECT sift_helpers.rows_agg_as_jsonb_array(q, push) $$;

CREATE FUNCTION sp_same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a jsonb[]; b jsonb[];
BEGIN
	a := sp_rows(q, true);
	b := sp_rows(q, false);
	RETURN CASE WHEN a IS NOT DISTINCT FROM b THEN 'same ' || coalesce(cardinality(a), 0) ELSE 'differs: ' || a::text || ' vs ' || b::text END;
END $$;

CREATE FUNCTION sp_plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := '';
BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, BUFFERS off) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN r := r || 'scan '; END IF;
		IF l ~ 'ordered sets spilled' THEN r := r || 'spilled '; END IF;
	END LOOP;
	RETURN trim(r);
END $$;

CREATE TABLE sp_q (n int, q text);
INSERT INTO sp_q VALUES
	(1, 'SELECT g AS k, percentile_cont(0.5) WITHIN GROUP (ORDER BY x) AS a, percentile_disc(0.9) WITHIN GROUP (ORDER BY x) AS b FROM sp GROUP BY g'),
	(2, 'SELECT g AS k, percentile_cont(0.25) WITHIN GROUP (ORDER BY d) AS a, percentile_disc(0.75) WITHIN GROUP (ORDER BY d DESC) AS b FROM sp GROUP BY g'),
	(3, 'SELECT g AS k, mode() WITHIN GROUP (ORDER BY d) AS a, percentile_disc(0.5) WITHIN GROUP (ORDER BY d) AS b, percentile_cont(0.5) WITHIN GROUP (ORDER BY d) AS c FROM sp GROUP BY g'),
	(4, 'SELECT g AS k, percentile_disc(0.5) WITHIN GROUP (ORDER BY b) AS a, percentile_cont(0.25) WITHIN GROUP (ORDER BY b::float8) AS c FROM sp GROUP BY g'),
	(5, 'SELECT g AS k, percentile_cont(0.5) WITHIN GROUP (ORDER BY nx) AS a, percentile_disc(0.1) WITHIN GROUP (ORDER BY nx) AS b FROM sp GROUP BY g'),
	(6, 'SELECT g AS k, percentile_cont(0.5) WITHIN GROUP (ORDER BY x) FILTER (WHERE id % 3 <> 0) AS a, percentile_disc(0.5) WITHIN GROUP (ORDER BY x) FILTER (WHERE id > 60000) AS b FROM sp GROUP BY g'),
	(7, 'SELECT 1 AS k, percentile_cont(0.5) WITHIN GROUP (ORDER BY x) AS a, percentile_cont(0.75) WITHIN GROUP (ORDER BY x) AS b, mode() WITHIN GROUP (ORDER BY d) AS c FROM sp'),
	(8, 'SELECT id % 50 AS k, percentile_cont(0.5) WITHIN GROUP (ORDER BY x) AS a, percentile_disc(0.5) WITHIN GROUP (ORDER BY d) AS b FROM sp GROUP BY id % 50'),
	(9, 'SELECT g AS k, percentile_disc(0.5) WITHIN GROUP (ORDER BY x) AS a FROM sp WHERE id < 0 GROUP BY g');

SELECT n, sp_plan(q) AS plan, sp_same(q) AS same FROM sp_q ORDER BY n;

DROP TABLE sp_q;
DROP FUNCTION sp_rows(text, bool), sp_same(text), sp_plan(text);
DROP TABLE sp;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
