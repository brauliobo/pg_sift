-- The values the dimension map holds for the columns of a dimension that are floats, times, numerics and strings: the specials of the floats (NaN, the infinities, -0, a denormal), a
-- numeric NaN, NULLs, the empty string, strings that differ in bytes only, a multibyte string and one longer than a dictionary entry keeps inline. The answers are those of the normal
-- plan, and every shape is served by a map but shape 7, whose five columns take more tuples of values than a class table holds (4,096): the rows are loaded.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.agg_force = on;
SET sift.agg_lookup_cache_kb = 0;
SET sift.agg_lookup_shared_kb = 0;

CREATE TABLE pf (k int PRIMARY KEY, f4 real, f8 float8, tm time, tx text, vc varchar(300), nm numeric(12, 3)) WITH (autovacuum_enabled = false);
INSERT INTO pf SELECT i,
	(ARRAY['NaN', 'Infinity', '-Infinity', '0', '-0', NULL, '1.5', '-1.5', '1e-40', '3.4e38', '0.1']::real[])[i % 11 + 1],
	(ARRAY['NaN', 'Infinity', '-Infinity', '0', '-0', NULL, '1.5', '-1.5', '1e-310', '1e300', '0.1']::float8[])[(i / 3) % 11 + 1],
	(ARRAY['00:00:00', '23:59:59.999999', '12:00:00', NULL, '00:00:00.000001']::time[])[i % 5 + 1],
	(ARRAY['', 'a', 'A', 'é', NULL, E'tab\there', 'x y', repeat('long', 40)])[i % 8 + 1],
	(ARRAY['v1', 'v2', NULL, '', repeat('z', 250)])[(i / 7) % 5 + 1],
	(ARRAY['0.000', '-1.500', '99999999.999', NULL, '0.001', '-0.001'])[i % 6 + 1]::numeric
FROM generate_series(0, 4999) i;
CREATE INDEX pf_col ON pf USING sift_col (k, f4, f8, tm, tx, vc, nm);
-- events of keys with no row and events with no key
CREATE TABLE pe (id int PRIMARY KEY, k int, v int) WITH (autovacuum_enabled = false);
INSERT INTO pe SELECT i, CASE WHEN i % 50 = 0 THEN NULL ELSE i * 7919 % 5200 END, i % 13 FROM generate_series(1, 30000) i;
CREATE INDEX pe_col ON pe USING sift_col (id, k, v);
VACUUM (FREEZE, ANALYZE) pf, pe;

CREATE TABLE shapes (n int, sql text);
INSERT INTO shapes VALUES
 (1, $q$SELECT p.f4::text, count(*), sum(pe.v) FROM pe JOIN pf p ON p.k = pe.k GROUP BY 1 ORDER BY 1$q$),
 (2, $q$SELECT p.f8::text, count(*) FROM pe LEFT JOIN pf p ON p.k = pe.k GROUP BY 1 ORDER BY 1$q$),
 (3, $q$SELECT p.tm::text, count(*), max(pe.v) FROM pe JOIN pf p ON p.k = pe.k GROUP BY 1 ORDER BY 1$q$),
 (4, $q$SELECT p.tx, length(p.tx), count(*) FROM pe JOIN pf p ON p.k = pe.k GROUP BY 1, 2 ORDER BY 1, 2$q$),
 (5, $q$SELECT p.vc, count(*) FROM pe LEFT JOIN pf p ON p.k = pe.k GROUP BY 1 ORDER BY 1$q$),
 (6, $q$SELECT p.nm::text, count(*), sum(p.nm) FROM pe JOIN pf p ON p.k = pe.k GROUP BY 1 ORDER BY 1$q$),
 (7, $q$SELECT p.tx, p.vc, p.nm::text, p.f4::text, p.tm::text, count(*) FROM pe JOIN pf p ON p.k = pe.k GROUP BY 1, 2, 3, 4, 5 ORDER BY 1, 2, 3, 4, 5$q$),
 (8, $q$SELECT count(*) FILTER (WHERE p.tx = 'a'), count(*) FILTER (WHERE p.f8 > 1), count(DISTINCT p.vc), count(p.nm), max(p.tx), min(p.vc) FROM pe JOIN pf p ON p.k = pe.k$q$);

CREATE FUNCTION answer(sql text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || sql || ') t' INTO r;
	RETURN r;
END $$;
CREATE FUNCTION served(sql text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal'; BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || sql LOOP
		IF l ~ 'Custom Scan \(SiftAgg\)' THEN r := 'rows'; END IF;
		IF l ~ '[1-9]\d* rows classed' AND r = 'rows' THEN r := 'classes'; END IF;
		IF l ~ '[1-9]\d* maps (shared|built)' THEN r := 'map'; END IF;
	END LOOP;
	RETURN r;
END $$;
CREATE FUNCTION report(stage text) RETURNS TABLE (stage text, shape int, same_answers bool, served text) LANGUAGE sql AS $$
	SELECT stage, n, answer(sql, true) = answer(sql, false), served(sql) FROM shapes ORDER BY n
$$;

SELECT * FROM report('first');
SELECT * FROM report('shared');

-- the strings and floats written: the maps of the old rows are not read
UPDATE pf SET tx = tx || '!', f4 = -f4, nm = nm + 0.001, vc = NULL WHERE k % 17 = 0;
SELECT * FROM report('written');
VACUUM pf;
SELECT * FROM report('vacuumed');

SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET sift.agg_par_min_rows = 0;
SELECT * FROM report('workers');

DROP FUNCTION report(text), served(text), answer(text, bool);
DROP TABLE shapes, pe, pf;
DROP EXTENSION pg_sift;
