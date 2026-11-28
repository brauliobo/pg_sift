-- count(DISTINCT text) of groups of two integer keys, some of them NULL, with workers: the values of a group reach the worker that owns its other aggregates, so each group is one row
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;

CREATE TABLE facts (id int, a int, b int, k text) WITH (autovacuum_enabled = false);
INSERT INTO facts SELECT i, CASE WHEN i % 13 = 0 THEN NULL ELSE i % 50 END, CASE WHEN i % 11 = 0 THEN NULL ELSE i % 7 END, 'k' || i % 3
FROM generate_series(1, 40000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, a, b, k);
VACUUM ANALYZE facts;

SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
SET max_parallel_workers_per_gather = 2;

CREATE TABLE shapes (n serial, name text, q text);
INSERT INTO shapes (name, q) VALUES
	('groups',  'SELECT a, b, count(*) AS c, count(DISTINCT k) AS d FROM facts GROUP BY a, b'),
	('having',  'SELECT a, b, count(*) AS c FROM facts GROUP BY a, b HAVING count(DISTINCT k) <> 2'),
	('outer',   'SELECT * FROM (SELECT a, b, count(*) AS c FROM facts GROUP BY a, b HAVING count(DISTINCT k) < 3) g WHERE g.c > 1');

CREATE FUNCTION par_scan(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Parallel Custom Scan (SiftAgg%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;
CREATE FUNCTION diff_rows(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	EXECUTE 'CREATE TEMP TABLE got AS ' || q;
	PERFORM set_config('sift.enable_agg', 'off', true);
	EXECUTE 'CREATE TEMP TABLE want AS ' || q;
	PERFORM set_config('sift.enable_agg', 'on', true);
	SELECT count(*) INTO n FROM ((TABLE got EXCEPT ALL TABLE want) UNION ALL (TABLE want EXCEPT ALL TABLE got)) x;
	DROP TABLE got, want;
	RETURN n;
END $$;

SELECT n, name, par_scan(q) AS workers, diff_rows(q) AS rows_off FROM shapes ORDER BY n;

DROP FUNCTION par_scan(text), diff_rows(text);
DROP TABLE facts, shapes;
DROP EXTENSION pg_sift;
