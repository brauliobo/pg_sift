-- sift_table: a TID range scan takes the bounds PostgreSQL derives from the qual, which may have offset 0 (ctid < '(1,1)' is (1,0), no lower bound is (0,0)), and
-- reads the rows the same qual selects without a TID range scan, in both formats. No pageinspect. About 2 s. Run on a build where new tables take Format 2.0 (sift.test_create_format1 off).
CREATE EXTENSION IF NOT EXISTS pg_sift CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET enable_seqscan = off;

CREATE TABLE quals (id int PRIMARY KEY, qual text NOT NULL);
INSERT INTO quals VALUES
	(1, $$ctid >= '(0,1)' AND ctid < '(1,1)'$$),
	(2, $$ctid < '(1,1)'$$),
	(3, $$ctid <= '(1,0)'$$),
	(4, $$ctid > '(0,0)'$$),
	(5, $$ctid >= '(0,0)'$$),
	(6, $$ctid < '(0,1)'$$),
	(7, $$ctid >= '(0,0)' AND ctid <= '(0,0)'$$),
	(8, $$ctid > '(1,0)' AND ctid <= '(2,0)'$$),
	(9, $$ctid >= '(1,1)'$$);

CREATE FUNCTION rows_of(rel regclass, qual text, tidscan bool) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	PERFORM set_config('enable_tidscan', tidscan::text, true);
	PERFORM set_config('enable_seqscan', (NOT tidscan)::text, true);
	EXECUTE format('SELECT count(*) FROM %s WHERE %s', rel, qual) INTO n;
	RETURN n;
END $$;

CREATE FUNCTION scan_plan(rel regclass, qual text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
	PERFORM set_config('enable_tidscan', 'true', true);
	FOR line IN EXECUTE format('EXPLAIN (COSTS OFF) SELECT count(*) FROM %s WHERE %s', rel, qual) LOOP
		IF line LIKE '%Tid Range Scan%' THEN RETURN 'Tid Range Scan'; END IF;
	END LOOP;
	RETURN 'other';
END $$;

CREATE TABLE t (id int PRIMARY KEY, g int, s text) USING sift_table;
INSERT INTO t SELECT i, i % 10, 'row ' || i FROM generate_series(1, 20000) i;
DELETE FROM t WHERE g = 7;
UPDATE t SET s = s || '!' WHERE g = 3;
VACUUM t;

SELECT id, scan_plan('t', qual) AS plan, rows_of('t', qual, true) = rows_of('t', qual, false) AS same_rows FROM quals ORDER BY id;
SELECT rows_of('t', $$ctid >= '(0,0)'$$, true) = (SELECT count(*) FROM t) AS whole_table;
SELECT rows_of('t', $$ctid < '(0,1)'$$, true) AS below_first;
SELECT count(*) > 0 AS probe_in_range FROM t WHERE ctid >= '(0,1)' AND ctid < '(1,1)' AND id = 5;

SET sift.test_create_format1 = on;
CREATE TABLE t1 (id int PRIMARY KEY, g int, s text) USING sift_table;
INSERT INTO t1 SELECT i, i % 10, 'row ' || i FROM generate_series(1, 20000) i;
DELETE FROM t1 WHERE g = 7;
SELECT id, rows_of('t1', qual, true) = rows_of('t1', qual, false) AS same_rows FROM quals ORDER BY id;
SELECT rows_of('t1', $$ctid >= '(0,0)'$$, true) = (SELECT count(*) FROM t1) AS whole_table;
SELECT rows_of('t1', $$ctid < '(0,1)'$$, true) AS below_first;

DROP TABLE t, t1, quals;
DROP FUNCTION rows_of(regclass, text, bool), scan_plan(regclass, text);
DROP EXTENSION pg_sift CASCADE;
