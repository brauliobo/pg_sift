-- A test of the columns of a joined table (a FILTER, a CASE, a group key, a condition) is one boolean column of the lookup table: the load computes it for each row of the
-- table, from the leaves of its index, and the table is kept as classes of its keys by the outcomes (a few) however many tuples of values its columns take (7,000 here).
-- The answers are those of the normal plan; a test that may raise an error, a table a preserving join leaves unmatched rows of, and the ON clause of the table stay as they were.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.agg_force = on;

CREATE TABLE ev (id int, k int, v int) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, i % 10500, i % 11 FROM generate_series(1, 450000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, k, v);
-- 10,000 keys, 7,000 tuples of (a, b, c); the events of keys 10000 to 10499 have no row
CREATE TABLE dd (k int PRIMARY KEY, a int, b int, c int, t text) WITH (autovacuum_enabled = false);
INSERT INTO dd SELECT i, CASE WHEN i % 13 = 0 THEN NULL ELSE i % 7 END, i % 40, i % 1000, CASE WHEN i % 17 = 0 THEN NULL ELSE 'k' || i % 5 END
FROM generate_series(0, 9999) i;
CREATE INDEX dd_col ON dd USING sift_col (k, a, b, c, t);
VACUUM (FREEZE, ANALYZE) ev, dd;

CREATE TABLE shapes (n int, sql text);
INSERT INTO shapes VALUES
 (1, $q$SELECT count(*) FILTER (WHERE dd.a = 1), count(*) FILTER (WHERE dd.b < 5), sum(ev.v) FILTER (WHERE dd.c = 7) FROM ev JOIN dd ON dd.k = ev.k$q$),
 (2, $q$SELECT dd.a > 3 AS hi, count(*), sum(ev.v) FROM ev JOIN dd ON dd.k = ev.k GROUP BY 1 ORDER BY 1$q$),
 (3, $q$SELECT ev.v, count(DISTINCT ev.k) FILTER (WHERE dd.a IS NULL OR dd.b = 0), count(*) FILTER (WHERE dd.t IS NULL) FROM ev JOIN dd ON dd.k = ev.k GROUP BY 1 ORDER BY 1$q$),
 (4, $q$SELECT sum(CASE WHEN dd.a = 1 THEN ev.v ELSE 0 END), sum(CASE WHEN dd.t = 'k2' THEN 1 END) FROM ev JOIN dd ON dd.k = ev.k$q$),
 (5, $q$SELECT count(*) FILTER (WHERE dd.k > 9000 AND dd.a IS NOT NULL) FROM ev JOIN dd ON dd.k = ev.k$q$),
 (6, $q$SELECT count(*) FILTER (WHERE 100 / (dd.b - 3) > 5) FROM ev JOIN dd ON dd.k = ev.k WHERE dd.b <> 3$q$),
 (7, $q$SELECT count(*) FILTER (WHERE dd.a IS NULL), count(dd.k) FROM ev LEFT JOIN dd ON dd.k = ev.k$q$),
 (8, $q$SELECT count(*) FILTER (WHERE dd.a IS NULL), count(*) FILTER (WHERE ev.v = 3) FROM dd RIGHT JOIN ev ON dd.k = ev.k$q$),
 (9, $q$SELECT count(*) FROM ev JOIN dd ON dd.k = ev.k AND dd.b = 5$q$);

CREATE FUNCTION answer(sql text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || sql || ') t' INTO r;
	RETURN r;
END $$;
-- how the scan got the lookup table: 'classed' when it made the classes, 'cached' when it read a kept table, 'loaded' when it loaded the rows, 'normal' without the scan
CREATE FUNCTION got(sql text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal'; BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) ' || sql LOOP
		IF l ~ 'Custom Scan \(SiftAgg\)' THEN r := 'loaded'; END IF;
		IF l ~ '[1-9]\d* rows classed' THEN r := 'classed'; ELSIF l ~ '[1-9]\d* cached tables' THEN r := 'cached'; END IF;
	END LOOP;
	RETURN r;
END $$;
CREATE FUNCTION report(st text) RETURNS TABLE (stage text, shape int, first text, again text, same_answers bool) LANGUAGE plpgsql AS $$
DECLARE s record; BEGIN
	FOR s IN SELECT n, sql FROM shapes ORDER BY n LOOP
		RETURN QUERY SELECT st, s.n, got(s.sql), got(s.sql), answer(s.sql, true) = answer(s.sql, false);
	END LOOP;
END $$;

SELECT * FROM report('settled');
SELECT * FROM report('kept');

-- committed writes: the kept tables are those of the old rows, the next run makes new ones
UPDATE dd SET a = 1, b = 3 WHERE k % 11 = 0;
DELETE FROM dd WHERE k % 53 = 0;
INSERT INTO dd SELECT i, NULL, NULL, NULL, NULL FROM generate_series(10000, 10099) i;
VACUUM dd;
SELECT * FROM report('written');

-- the transaction's own rows, not yet in the index's units
BEGIN;
UPDATE dd SET a = 2 WHERE k % 19 = 0;
INSERT INTO dd SELECT i, 1, 1, 1, 'k1' FROM generate_series(10100, 10199) i;
SELECT * FROM report('in the transaction');
ROLLBACK;
VACUUM dd;

-- the workers read the classes from the shared memory
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET sift.agg_par_min_rows = 0;
SELECT * FROM report('workers');

-- the caches off: the classes are made by every statement
SET sift.agg_lookup_cache_kb = 0;
SET sift.agg_lookup_shared_kb = 0;
SELECT * FROM report('caches off');

DROP FUNCTION report(text), got(text), answer(text, bool);
DROP TABLE shapes, ev, dd;
DROP EXTENSION pg_sift;
