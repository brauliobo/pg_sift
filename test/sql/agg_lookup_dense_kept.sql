-- A lookup table kept as classes of its keys (agg_lookup_dense) is kept for the next query like any other: the first query classes it, the next ones read it from the
-- cache, whatever the settings of the caches, and a query that cannot read the classes (the key as a column) neither takes nor replaces the classed table.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.agg_force = on;
SET sift.agg_dimmap_kb = 0;

CREATE TABLE ev (id int, k int, v int) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, i % 3300, i % 11 FROM generate_series(1, 40000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, k, v);
-- 3200 keys in 12 classes; the events of keys 3200 to 3299 have no row
CREATE TABLE dd (k int PRIMARY KEY, lvl int2, grade int) WITH (autovacuum_enabled = false);
INSERT INTO dd SELECT i, (i % 3)::int2, CASE WHEN i % 5 = 0 THEN NULL ELSE i % 4 END FROM generate_series(0, 3199) i;
CREATE INDEX dd_col ON dd USING sift_col (k, lvl, grade);
VACUUM (FREEZE, ANALYZE) ev, dd;

CREATE TABLE shapes (n int, sql text);
INSERT INTO shapes VALUES
 (1, $q$SELECT dd.lvl, dd.grade, count(*), sum(ev.v) FROM ev JOIN dd ON dd.k = ev.k GROUP BY 1, 2 ORDER BY 1, 2$q$),
 (2, $q$SELECT dd.lvl, count(*) FROM ev LEFT JOIN dd ON dd.k = ev.k GROUP BY 1 ORDER BY 1$q$),
 (3, $q$SELECT dd.lvl, sum(dd.k), count(DISTINCT dd.k) FROM ev LEFT JOIN dd ON dd.k = ev.k GROUP BY 1 ORDER BY 1$q$);

CREATE FUNCTION answer(sql text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || sql || ') t' INTO r;
	RETURN r;
END $$;
-- how the scan got the lookup table: 'classed' when it made the classes, 'cached' when it read a kept table, 'loaded' when it loaded the rows
CREATE FUNCTION got(sql text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'loaded'; BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) ' || sql LOOP
		IF l ~ '[1-9]\d* rows classed' THEN r := 'classed'; ELSIF l ~ '[1-9]\d* cached tables' THEN r := 'cached'; END IF;
	END LOOP;
	RETURN r;
END $$;
CREATE FUNCTION report(st text) RETURNS TABLE (stage text, shape int, first text, again text, same_answers bool) LANGUAGE plpgsql AS $$
DECLARE s record; BEGIN
	FOR s IN SELECT n, sql FROM shapes ORDER BY n DESC LOOP
		RETURN QUERY SELECT st, s.n, got(s.sql), got(s.sql), answer(s.sql, true) = answer(s.sql, false);
	END LOOP;
END $$;

-- the shape that reads the key (of the table, not of the scan: a left join) goes first: the table it loads row by row is not the classes the others take
SELECT * FROM report('settled');
SELECT * FROM report('kept');

-- a committed write: the kept tables are those of the old rows, the next run makes new ones
UPDATE dd SET lvl = 2, grade = 3 WHERE k % 31 = 0;
DELETE FROM dd WHERE k % 53 = 0;
VACUUM dd;
SELECT * FROM report('written');

-- the workers read the classes from the shared memory
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET sift.agg_par_min_rows = 0;
SELECT * FROM report('workers');

-- the caches off: the classes are made by every statement, as before
SET sift.agg_lookup_cache_kb = 0;
SET sift.agg_lookup_shared_kb = 0;
SELECT * FROM report('caches off');

DROP FUNCTION report(text), got(text), answer(text, bool);
DROP TABLE shapes, ev, dd;
DROP EXTENSION pg_sift;
