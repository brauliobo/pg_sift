-- The month of a timestamp column is a dim of the cube of a sift_table (column format minor 8, docs/COL-PREAGG.md section 2): a group key that is the
-- same all month (date_trunc of a month or longer, extract of a month, quarter or year) is answered from the units' groups of months. Every query
-- runs three ways, which must give the same rows: from the pre-aggregates, scanning every unit (sift.agg_preagg off), and the plain plan
-- (sift.enable_agg off). `pre` says how many units the first one answered from metadata: all, some, none ('scan': the shape has no pre-aggregate to use).
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;

CREATE TABLE tm (id int, ts timestamp, cat int, amt numeric(10,2), qty int, f float8) USING sift_table;

CREATE FUNCTION pm_rows(q text, pre bool, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.agg_preagg', pre::text, true);
	PERFORM set_config('sift.enable_agg', push::text, true);
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO r;
	RETURN r;
END $$;

CREATE FUNCTION pm_units(q text, OUT units int, OUT pre int) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	PERFORM set_config('sift.agg_preagg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		units := coalesce(substring(l from '^\s*Units: (\d+)')::int, units);
		pre := coalesce(substring(l from 'Units From Pre-aggregates: (\d+)')::int, pre);
	END LOOP;
END $$;

CREATE TABLE pm_q (n int, shape text, q text);
INSERT INTO pm_q VALUES
	(1, 'month, date',        'SELECT date_trunc(''month'', ts)::date, count(*), sum(amt), count(amt) FROM %s GROUP BY 1'),
	(2, 'month, timestamp',   'SELECT date_trunc(''month'', ts), sum(qty), avg(amt) FROM %s GROUP BY 1'),
	(3, 'quarter',            'SELECT date_trunc(''quarter'', ts)::date, count(*), sum(amt) FROM %s GROUP BY 1'),
	(4, 'year',               'SELECT date_trunc(''year'', ts)::date, count(*), sum(amt) FROM %s GROUP BY 1'),
	(5, 'extract month',      'SELECT extract(month FROM ts), count(*), sum(qty) FROM %s GROUP BY 1'),
	(6, 'extract year',       'SELECT extract(year FROM ts)::int, count(*), sum(amt) FROM %s GROUP BY 1'),
	(7, 'cut then month',     'SELECT date_trunc(''month'', date_trunc(''hour'', ts)), count(*) FROM %s GROUP BY 1'),
	(8, 'count of the key',   'SELECT date_trunc(''month'', ts)::date, count(ts), count(*) FROM %s GROUP BY 1'),
	(9, 'zone on id',         'SELECT date_trunc(''month'', ts)::date, count(*), sum(amt) FROM %s WHERE id < 1000000 GROUP BY 1'),
	(10, 'zone on the key',   'SELECT date_trunc(''month'', ts)::date, count(*), sum(amt) FROM %s WHERE ts >= ''2000-01-01'' GROUP BY 1'),
	(11, 'test of a dim',     'SELECT date_trunc(''month'', ts)::date, count(*) FROM %s WHERE cat = 3 GROUP BY 1'),
	(12, 'week: scan',        'SELECT date_trunc(''week'', ts)::date, count(*) FROM %s GROUP BY 1'),
	(13, 'day: scan',         'SELECT date_trunc(''day'', ts)::date, count(*) FROM %s GROUP BY 1'),
	(14, 'week then month',   'SELECT date_trunc(''month'', date_trunc(''week'', ts)), count(*) FROM %s GROUP BY 1'),
	(15, 'extract day: scan', 'SELECT extract(day FROM ts), count(*) FROM %s GROUP BY 1'),
	(16, 'min: scan',         'SELECT date_trunc(''month'', ts)::date, min(qty) FROM %s GROUP BY 1'),
	(17, 'float: scan',       'SELECT date_trunc(''month'', ts)::date, sum(f) FROM %s GROUP BY 1'),
	(18, 'having, limit',     'SELECT date_trunc(''month'', ts)::date, count(*) FROM %s GROUP BY 1 HAVING count(*) > 100 ORDER BY 2 DESC, 1 LIMIT 5'),
	(19, 'two keys: scan',    'SELECT date_trunc(''month'', ts)::date, cat, count(*) FROM %s GROUP BY 1, 2'),
	(20, 'filter: scan',      'SELECT date_trunc(''month'', ts)::date, count(*) FILTER (WHERE cat = 1) FROM %s GROUP BY 1');

CREATE FUNCTION pm_check(tbl text, skip int DEFAULT 0) RETURNS TABLE (n int, shape text, pre text, same bool) LANGUAGE sql AS $$
	SELECT z.n, z.shape, CASE WHEN u.units IS NULL THEN '-' WHEN u.pre IS NULL THEN 'scan' WHEN u.pre = 0 THEN 'none' WHEN u.pre = u.units THEN 'all' ELSE 'some' END,
	       pm_rows(z.q, true, true) IS NOT DISTINCT FROM pm_rows(z.q, false, true) AND pm_rows(z.q, false, true) IS NOT DISTINCT FROM pm_rows(z.q, false, false)
	FROM (SELECT n, shape, format(q, tbl) AS q FROM pm_q WHERE n <> skip) z, LATERAL pm_units(z.q) u ORDER BY z.n
$$;

-- rows in 1999 to 2027 (before and after the epoch of the timestamps, leap years, a month with one row), NULLs in every column
INSERT INTO tm SELECT i, CASE WHEN i % 23 = 0 THEN NULL ELSE timestamp '1999-01-31 23:59:59' + (i::bigint * 20011 % 400000000) * interval '1 second' END, i % 10,
	CASE WHEN i % 31 = 0 THEN NULL ELSE ((i::bigint * 37) % 100000)::numeric / 100 END, i % 7, i / 8.0
FROM generate_series(1, 150000) i;
VACUUM (ANALYZE, DISABLE_PAGE_SKIPPING) tm;
SELECT count(*) > 1 AS units, array_agg(DISTINCT format) AS minors FROM sift.table_segments('tm') WHERE kind = 'columns';

-- 1. promoted, no write buffer row, no tombstone
SELECT * FROM pm_check('tm');

-- 2. rows in the write buffer: their superblocks are scanned, the others answer
INSERT INTO tm SELECT 200000 + i, timestamp '2024-02-28' + i * interval '97 minutes', i % 10, i % 997, i % 7, i FROM generate_series(1, 3000) i;
SELECT * FROM pm_check('tm');

-- 3. promoted again
VACUUM (DISABLE_PAGE_SKIPPING) tm;
SELECT * FROM pm_check('tm');

-- 4. tombstones and updates: a unit with a dead row is scanned, the others answer, and the key of an updated row is its new one
DELETE FROM tm WHERE id BETWEEN 100 AND 140;
UPDATE tm SET ts = ts + interval '70 days' WHERE id BETWEEN 200 AND 260;
SELECT * FROM pm_check('tm');
DELETE FROM tm WHERE id % 29 = 0;
UPDATE tm SET ts = ts + interval '70 days' WHERE id % 31 = 1;
SELECT * FROM pm_check('tm');
VACUUM (DISABLE_PAGE_SKIPPING) tm;
SELECT * FROM pm_check('tm');

-- 5. a transaction's own rows (not final: their units are scanned), then its rollback
BEGIN;
INSERT INTO tm SELECT 400000 + i, timestamp '2030-07-01' + i * interval '3 hours', i % 10, i, i % 7, i FROM generate_series(1, 500) i;
DELETE FROM tm WHERE id % 37 = 0;
SELECT * FROM pm_check('tm');
ROLLBACK;
SELECT * FROM pm_check('tm');

-- 6. infinities and the ends of the range: a unit that holds one has no month dim (the integer cast of extract(year) of an infinity is an error of the plain plan)
CREATE TABLE tmi (LIKE tm) USING sift_table;
INSERT INTO tmi SELECT i, timestamp '2015-03-10' + i * interval '2 hours', i % 10, i % 997, i % 7, i FROM generate_series(1, 40000) i;
INSERT INTO tmi VALUES (500001, 'infinity', 1, 1.5, 1, 1), (500002, '-infinity', 2, 2.5, 2, 2), (500003, '4713-01-01 BC', 3, 3.5, 3, 3), (500004, '294276-12-31', 4, 4.5, 4, 4);
VACUUM (ANALYZE, DISABLE_PAGE_SKIPPING) tmi;
SELECT * FROM pm_check('tmi', 6);
DELETE FROM tmi WHERE id > 500000;
VACUUM (FULL, ANALYZE) tmi;
SELECT * FROM pm_check('tmi');

-- 7. a segment written under sift.col_format_minor 7 has no month dim: the same table reads both, and a rewrite writes minor 8 again
CREATE TABLE tm7 (id int, ts timestamp, cat int, amt numeric(10,2), qty int, f float8) USING sift_table;
SET sift.col_format_minor = 7;
INSERT INTO tm7 SELECT i, timestamp '2020-01-15' + i * interval '2 hours', i % 10, i % 997, i % 7, i FROM generate_series(1, 60000) i;
VACUUM (ANALYZE, DISABLE_PAGE_SKIPPING) tm7;
RESET sift.col_format_minor;
SELECT array_agg(DISTINCT format) AS minors FROM sift.table_segments('tm7') WHERE kind = 'columns';
SELECT * FROM pm_check('tm7') WHERE n <= 8;
INSERT INTO tm7 SELECT 100000 + i, timestamp '2024-03-15' + i * interval '3 hours', i % 10, i % 997, i % 7, i FROM generate_series(1, 60000) i;
VACUUM (DISABLE_PAGE_SKIPPING) tm7;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift.table_segments('tm7') WHERE kind = 'columns';
SELECT * FROM pm_check('tm7') WHERE n <= 8;
VACUUM (FULL) tm7;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift.table_segments('tm7') WHERE kind = 'columns';
SELECT * FROM pm_check('tm7') WHERE n <= 8;

-- the units a plan reads, and those it answered from the groups of months
SELECT * FROM pm_units('SELECT date_trunc(''month'', ts)::date, sum(amt) FROM tmi GROUP BY 1');

DROP TABLE tm, tm7, tmi;
DROP FUNCTION pm_check(text, int), pm_units(text), pm_rows(text, bool, bool);
