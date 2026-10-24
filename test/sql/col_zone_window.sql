-- A begin of a scan tests the zones of the units of its range only: an aggregate begins for every morsel of two superblocks, and a begin that walked the zones of
-- the whole segment made the scan quadratic in its units. Over a segment of 130 units (three words of the keep bitmap, the last one partial) SiftAgg reads the units
-- the zones keep, for a range, an IN list, NULLs, an equality and an OR, under the vector kernels of the CPU and under the scalar ones, and its rows are those of
-- the normal plan. A page holds one row, so unit n holds the rows 256 * n .. 256 * n + 255 (k = n, j = n % 17 and NULL in every third unit).
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;
-- PostgreSQL 19 offers an index-only scan of the sift_col index, which prices a filter that prunes every unit (k BETWEEN 500 AND 600) under the fixed charge of the
-- scan; the statements here are about the scan, so the other plan is off
SET enable_indexonlyscan = off;

CREATE TABLE zw (id int, k int, j int, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO zw SELECT i, i / 256, CASE WHEN i / 256 % 3 = 0 THEN NULL ELSE i / 256 % 17 END, repeat('x', 600) FROM generate_series(0, 130 * 256 - 1) i;
CREATE INDEX zw_idx ON zw USING sift_col (k sift.col_int4_ops, j sift.col_int4_ops, id sift.col_int4_ops);
VACUUM (FREEZE, ANALYZE) zw;
SELECT units FROM sift.col_segments('zw_idx') WHERE kind = 'immutable';

-- the units a statement reads, and those it prunes ('normal' when SiftAgg does not run it)
CREATE FUNCTION units_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal'; read text; pruned text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN r := 'SiftAgg'; END IF;
		IF l LIKE '%Units: %' THEN read := trim(split_part(l, ':', 2)); END IF;
		IF l LIKE '%Units Pruned: %' THEN pruned := trim(split_part(l, ':', 2)); END IF;
	END LOOP;
	RETURN CASE WHEN r = 'normal' THEN r ELSE format('%s read %s pruned %s', r, read, pruned) END;
END $$;

CREATE FUNCTION same(cond text) RETURNS TABLE (plan text, result_rows bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
DECLARE q text := format('SELECT j, count(*) AS n, sum(id) AS s FROM zw WHERE %s GROUP BY j', cond);
BEGIN
	plan := units_of(q);
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	result_rows := (SELECT count(*) FROM r_on);
	vs_normal_plan := (SELECT count(*) FROM ((SELECT * FROM r_on EXCEPT ALL SELECT * FROM r_off) UNION ALL (SELECT * FROM r_off EXCEPT ALL SELECT * FROM r_on)) d);
	DROP TABLE r_on, r_off;
	RETURN NEXT;
END $$;

CREATE TABLE zw_q (n serial, cond text);
INSERT INTO zw_q (cond) VALUES
	('k BETWEEN 40 AND 99'), ('k BETWEEN 60 AND 70'), ('k = 63'), ('k = 64'), ('k IN (0, 63, 64, 127, 129)'), ('k >= 127'), ('k < 2'),
	('k BETWEEN 500 AND 600'), ('j BETWEEN 3 AND 5'), ('j IS NULL'), ('j IS NOT NULL AND k BETWEEN 100 AND 129'), ('k = 5 OR k = 70 OR k = 129'),
	('k BETWEEN 10 AND 20 OR j = 16'), ('id BETWEEN 8000 AND 9000'), ('k BETWEEN 30 AND 100 AND j = 4');

-- the plan, the units it reads and prunes, and whether its rows are those of the normal plan
SELECT n, cond, s.* FROM zw_q, LATERAL same(cond) s ORDER BY n;
SET sift.simd = scalar;
SELECT n, cond, s.* FROM zw_q, LATERAL same(cond) s ORDER BY n;
RESET sift.simd;

-- the units a begin keeps are kept for the next begin while the predicates stay the same: a scan that is run again with other values (a lateral parameter, a
-- different list for each outer row) begins with them from scratch, and a begin that is the same as the last one reads its units again
CREATE FUNCTION lat(agg bool) RETURNS TABLE (g int, n bigint, s bigint) LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.enable_agg', agg::text, true);
	RETURN QUERY EXECUTE 'SELECT g, c.n, c.s FROM generate_series(0, 129, 9) g, LATERAL (SELECT count(*) AS n, sum(id) AS s FROM zw WHERE k = g OR (j = g % 17 AND k > g)) c';
END $$;
CREATE FUNCTION lat_in(agg bool) RETURNS TABLE (g int, n bigint, s bigint) LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.enable_agg', agg::text, true);
	RETURN QUERY EXECUTE 'SELECT g, c.n, c.s FROM generate_series(0, 120, 10) g, LATERAL (SELECT count(*) AS n, sum(id) AS s FROM zw WHERE k IN (g, g + 1, 129 - g) AND j IS NOT NULL) c';
END $$;
SELECT (SELECT count(*) FROM lat(true)) AS rows_pushed, (SELECT count(*) FROM ((SELECT * FROM lat(true) EXCEPT ALL SELECT * FROM lat(false)) UNION ALL (SELECT * FROM lat(false) EXCEPT ALL SELECT * FROM lat(true))) d) AS or_differences;
SELECT (SELECT count(*) FROM lat_in(true)) AS rows_pushed, (SELECT count(*) FROM ((SELECT * FROM lat_in(true) EXCEPT ALL SELECT * FROM lat_in(false)) UNION ALL (SELECT * FROM lat_in(false) EXCEPT ALL SELECT * FROM lat_in(true))) d) AS in_differences;

DROP FUNCTION lat(bool), lat_in(bool);
DROP FUNCTION same(text), units_of(text);
DROP TABLE zw_q;
DROP TABLE zw;
DROP EXTENSION pg_sift;
