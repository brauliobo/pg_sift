-- The classes of a big table's keys made by workers together (src/agg/lookclassp.c): the answers are those of the normal plan and of the load by one process, whatever the table
-- holds (floats, numerics, times, strings, NULLs), a table that is not one of classes (a key twice, a negative key, more than 4,096 tuples of values) is told so by the workers as by
-- one process, and a key beyond the largest the index says (rows written since) is read by the process alone.
CREATE EXTENSION pg_sift;
SET jit = off;
SET sift.agg_force = on;
SET sift.agg_lookup_cache_kb = 0;
SET sift.agg_lookup_shared_kb = 0;
SET sift.agg_dimmap_kb = 0;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET sift.agg_par_min_rows = 0;

CREATE TABLE pg (k int PRIMARY KEY, f8 float8, tm time, tx text, nm numeric(12, 3), n int) WITH (autovacuum_enabled = false);
INSERT INTO pg SELECT i,
	(ARRAY['NaN', 'Infinity', '-0', '0', NULL, '1.5', '0.1']::float8[])[i % 7 + 1],
	(ARRAY['00:00:00', '23:59:59.999999', NULL, '12:00:00']::time[])[i % 4 + 1],
	(ARRAY['', 'a', 'A', 'é', NULL, 'x y', repeat('long', 40)])[i % 7 + 1],
	(ARRAY['0.000', '-1.500', '99999999.999', NULL, '0.001'])[i % 5 + 1]::numeric,
	i % 50
FROM generate_series(0, 299999) i;
CREATE INDEX pg_col ON pg USING sift_col (k, f8, tm, tx, nm, n);
-- many tuples of values: more than a table of classes keeps
CREATE TABLE pm (k int PRIMARY KEY, a int, b int) WITH (autovacuum_enabled = false);
INSERT INTO pm SELECT i, i % 97, i % 89 FROM generate_series(0, 299999) i;
CREATE INDEX pm_col ON pm USING sift_col (k, a, b);
-- a key twice
CREATE TABLE pd (k int, a int) WITH (autovacuum_enabled = false);
INSERT INTO pd SELECT i % 200000, i % 7 FROM generate_series(0, 299999) i;
CREATE INDEX pd_col ON pd USING sift_col (k, a);
CREATE TABLE pe (id int PRIMARY KEY, k int, v int) WITH (autovacuum_enabled = false);
INSERT INTO pe SELECT i, CASE WHEN i % 50 = 0 THEN NULL ELSE (i::bigint * 7919 % 320000)::int END, i % 13 FROM generate_series(1, 400000) i;
CREATE INDEX pe_col ON pe USING sift_col (id, k, v);
VACUUM (FREEZE, ANALYZE) pg, pm, pd, pe;

CREATE TABLE shapes (n int, sql text);
INSERT INTO shapes VALUES
 (1, $q$SELECT p.f8::text, p.tm::text, count(*), sum(pe.v) FROM pe LEFT JOIN pg p ON p.k = pe.k GROUP BY 1, 2 ORDER BY 1, 2$q$),
 (2, $q$SELECT p.tx, p.nm::text, count(*) FROM pe LEFT JOIN pg p ON p.k = pe.k GROUP BY 1, 2 ORDER BY 1, 2$q$),
 (3, $q$SELECT count(*) FILTER (WHERE p.tx = 'a'), count(*) FILTER (WHERE p.f8 > 1), count(DISTINCT p.n), max(p.tx) FROM pe LEFT JOIN pg p ON p.k = pe.k$q$),
 (4, $q$SELECT p.a, p.b, count(*) FROM pe LEFT JOIN pm p ON p.k = pe.k GROUP BY 1, 2 ORDER BY 1, 2$q$),
 (5, $q$SELECT p.a, count(*) FROM pe LEFT JOIN pd p ON p.k = pe.k GROUP BY 1 ORDER BY 1$q$),
 -- tests of the columns, computed for the rows of the table by the tuples of values they read (strings, floats, numerics, times, NULLs; several units)
 (6, $q$SELECT count(*) FILTER (WHERE p.tx = 'a' OR p.tx IS NULL), count(*) FILTER (WHERE p.f8 > 1 AND p.nm < 1), sum(pe.v) FILTER (WHERE p.tm > '12:00') FROM pe JOIN pg p ON p.k = pe.k WHERE pe.id > 0$q$),
 (7, $q$SELECT p.n > 25, p.tx = 'x y', p.nm IS NULL, count(*) FROM pe JOIN pg p ON p.k = pe.k WHERE pe.id > 0 GROUP BY 1, 2, 3 ORDER BY 1, 2, 3$q$);

CREATE FUNCTION answer(sql text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || sql || ') t' INTO r;
	RETURN r;
END $$;
-- how the scan got the table, and whether workers made its classes
CREATE FUNCTION served(sql text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal'; BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || sql LOOP
		IF l ~ 'Custom Scan \(SiftAgg\)' THEN r := 'rows'; END IF;
		IF l ~ '[1-9]\d* rows classed' AND r = 'rows' THEN r := 'classes'; END IF;
		IF l ~ '[1-9]\d* class workers' AND r = 'classes' THEN r := 'classes by workers'; END IF;
	END LOOP;
	RETURN r;
END $$;
-- every stage by one process (0 workers) and by workers: the table is told the same, what differs is who classed it
CREATE FUNCTION report(label text) RETURNS TABLE (stage text, workers int, shape int, same_answers bool, served text) LANGUAGE plpgsql AS $$
DECLARE w int; BEGIN
	FOREACH w IN ARRAY ARRAY[0, 3] LOOP
		PERFORM set_config('max_parallel_workers_per_gather', w::text, true);
		RETURN QUERY SELECT label, w, n, answer(sql, true) = answer(sql, false), served(sql) FROM shapes ORDER BY n;
	END LOOP;
END $$;

SELECT * FROM report('built');

-- keys written since the index says its largest: the workers' array is short for them, the process alone reads the table
INSERT INTO pg SELECT i, 1.5, '01:00:00', 'new', 1.000, 1 FROM generate_series(300000, 330000) i;
SELECT * FROM report('keys beyond');
VACUUM pg;
SELECT * FROM report('vacuumed');

-- a negative key, a key twice: not classes, by workers as by one process
INSERT INTO pg VALUES (-5, 0, '00:00:00', 'neg', 0, 0);
SELECT * FROM report('negative key');
DELETE FROM pg WHERE k = -5;
ALTER TABLE pg DROP CONSTRAINT pg_pkey;
INSERT INTO pg VALUES (7, 0, '00:00:00', 'again', 0, 0);
SELECT * FROM report('repeated key');


DROP FUNCTION report(text), served(text), answer(text, bool);
DROP TABLE shapes, pe, pd, pm, pg;
DROP EXTENSION pg_sift;
