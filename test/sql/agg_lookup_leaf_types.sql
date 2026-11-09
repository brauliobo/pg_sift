-- The columns of a joined table are read from the leaves of its sift_col index for every type whose leaf the load decodes: float4, float8 and time besides the integers, dates, timestamps, booleans,
-- numerics and texts. The values are those of the heap, the specials of the floats included (NaN, the infinities, -0, a denormal, NULL); a column of a type the load does not decode (uuid) is read
-- from the heap and EXPLAIN (VERBOSE) says which column and why.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.agg_force = on;

CREATE TABLE pf (id int PRIMARY KEY, f4 real, f8 float8, t time, u uuid) WITH (autovacuum_enabled = false);
INSERT INTO pf SELECT i,
	(ARRAY['NaN', 'Infinity', '-Infinity', '0', '-0', NULL, '1.5', '-1.5', '1e-40', '3.4e38', '0.1']::real[])[i % 11 + 1],
	(ARRAY['NaN', 'Infinity', '-Infinity', '0', '-0', NULL, '1.5', '-1.5', '1e-310', '1e300', '0.1']::float8[])[(i / 3) % 11 + 1],
	(ARRAY['00:00:00', '23:59:59.999999', '12:00:00', NULL, '00:00:00.000001']::time[])[i % 5 + 1],
	CASE WHEN i % 7 = 0 THEN NULL ELSE md5(i::text)::uuid END
FROM generate_series(1, 6000) i;
-- the child has events of no row and events with no key
CREATE TABLE pc (id int, pid int, v int) WITH (autovacuum_enabled = false);
INSERT INTO pc SELECT i, CASE WHEN i % 50 = 0 THEN NULL ELSE 1 + (i * 7919) % 6500 END, i % 13 FROM generate_series(1, 30000) i;
CREATE INDEX pf_col ON pf USING sift_col (id, f4, f8, t);
CREATE INDEX pf_u ON pf USING sift_col (id, f8, u);
CREATE INDEX pc_col ON pc USING sift_col (id, pid, v);
VACUUM (FREEZE, ANALYZE) pf, pc;

CREATE TABLE shapes (n int, sql text);
INSERT INTO shapes VALUES
 (1, $q$SELECT p.f8::text, count(*) FROM pc LEFT JOIN pf p ON p.id = pc.pid GROUP BY 1 ORDER BY 1$q$),
 (2, $q$SELECT p.f4::text, count(*), sum(pc.v) FROM pc LEFT JOIN pf p ON p.id = pc.pid GROUP BY 1 ORDER BY 1$q$),
 (3, $q$SELECT p.t::text, count(*) FROM pc LEFT JOIN pf p ON p.id = pc.pid GROUP BY 1 ORDER BY 1$q$),
 (4, $q$SELECT p.f4::text, p.f8::text, p.t::text, count(*) FROM pc JOIN pf p ON p.id = pc.pid GROUP BY 1, 2, 3 ORDER BY 1, 2, 3$q$),
 (5, $q$SELECT count(*) FILTER (WHERE p.f8 > 1), count(*) FILTER (WHERE p.f4 = 'NaN'), count(*) FILTER (WHERE p.t >= '12:00'), count(p.f8), count(DISTINCT p.f8::text) FROM pc JOIN pf p ON p.id = pc.pid$q$),
 (6, $q$SELECT min(p.f8), max(p.f8), min(p.f4), max(p.f4), min(p.t), max(p.t) FROM pc LEFT JOIN pf p ON p.id = pc.pid$q$),
 (7, $q$SELECT count(*) FROM pc JOIN pf p ON p.id = pc.pid WHERE p.f8 IS NULL OR p.f4 < 0 OR p.t = '00:00:00'$q$),
 (8, $q$SELECT p.u IS NULL, count(*) FROM pc LEFT JOIN pf p ON p.id = pc.pid GROUP BY 1 ORDER BY 1$q$);

CREATE FUNCTION answer(sql text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || sql || ') t' INTO r;
	RETURN r;
END $$;
-- how the scan read the joined table: 'leaves' (column loads), 'heap' (read, and the reason EXPLAIN gives when it has one), 'normal' without the scan
CREATE FUNCTION got(sql text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal'; BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF) ' || sql LOOP
		IF l ~ 'Custom Scan \(SiftAgg\)' THEN r := 'heap'; END IF;
		IF l ~ '[1-9]\d* column loads' THEN r := 'leaves'; END IF;
		IF l ~ 'Lookup Heap Reason: ' THEN r := r || ' (' || regexp_replace(l, '^ *Lookup Heap Reason: ', '') || ')'; END IF;
	END LOOP;
	RETURN r;
END $$;
CREATE FUNCTION report(st text) RETURNS TABLE (stage text, shape int, read text, same_answers bool) LANGUAGE plpgsql AS $$
DECLARE s record; BEGIN
	FOR s IN SELECT n, sql FROM shapes ORDER BY n LOOP
		RETURN QUERY SELECT st, s.n, got(s.sql), answer(s.sql, true) = answer(s.sql, false);
	END LOOP;
END $$;

SET sift.agg_lookup_cache_kb = 0;
SET sift.agg_lookup_shared_kb = 0;
SELECT * FROM report('serial');

-- the leader loads the table and the workers read it from the shared memory
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET sift.agg_par_min_rows = 0;
SELECT * FROM report('workers');

-- rows written since: the table is read again
UPDATE pf SET f8 = 'NaN', f4 = '-0', t = '23:59:59.999999' WHERE id % 97 = 0;
DELETE FROM pf WHERE id % 89 = 0;
VACUUM pf;
SELECT * FROM report('written');

DROP FUNCTION report(text), got(text), answer(text, bool);
DROP TABLE shapes, pc, pf;
DROP EXTENSION pg_sift;
