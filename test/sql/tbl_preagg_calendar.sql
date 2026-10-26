-- The calendar cells of a date, timestamp or timestamptz column are a dim of the cube of a sift_table (column format minor 8, docs/COL-PREAGG.md section 2):
-- a unit stores the groups of its days, else of its months, else of its years (the finest that fits), and a group key that is a function of the cell
-- (date_trunc, extract, ::date), a range on the column or a test of its calendar parts is answered from them. Every query runs three ways, which must give
-- the same rows: from the pre-aggregates, scanning every unit (sift.agg_preagg off), and the plain plan (sift.enable_agg off). `pre` says how many units
-- the first one answered from metadata: all, some, none ('scan': the shape has no pre-aggregate to use).
SET client_min_messages = warning;
CREATE EXTENSION pg_sift_test CASCADE;
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;

CREATE TABLE tcd (id int, ts timestamp, d date, tz timestamptz, cat int, amt numeric(10,2), qty int, f float8) USING sift_table;
CREATE TABLE tcm (LIKE tcd) USING sift_table;
CREATE TABLE tcy (LIKE tcd) USING sift_table;

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

-- the rows of a table in a window of days from a day: a timestamp, a date and a timestamptz of the same instants, NULLs in every column
CREATE FUNCTION pm_fill(tbl regclass, base timestamp, span int, lo int, hi int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format($q$INSERT INTO %s SELECT i,
		CASE WHEN i %% 23 = 0 THEN NULL ELSE %L::timestamp + (i::bigint * 20011 %% (%s::bigint * 86400)) * interval '1 second' END,
		CASE WHEN i %% 29 = 0 THEN NULL ELSE %L::date + (i::bigint * 7919 %% %s)::int END,
		CASE WHEN i %% 31 = 0 THEN NULL ELSE (%L::timestamp AT TIME ZONE 'UTC') + (i::bigint * 10007 %% (%s::bigint * 86400)) * interval '1 second' END,
		i %% 10, CASE WHEN i %% 37 = 0 THEN NULL ELSE ((i::bigint * 37) %% 100000)::numeric / 100 END, i %% 7, i / 8.0
		FROM generate_series(%s, %s) i$q$, tbl, base, span, base, span, base, span, lo, hi);
END $$;

-- the leap days of 1900 (not), 2000 and 2024, the weeks that belong to the year before or after (ISO years 2015 to 2021), century ends
CREATE FUNCTION pm_edges(tbl regclass, lo timestamp, hi timestamp) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format($q$INSERT INTO %s SELECT 800000 + row_number() OVER (ORDER BY t), t, t::date, t AT TIME ZONE 'UTC', 1, 1.5, 1, 1.5
		FROM unnest(ARRAY['1899-12-31 23:59:59.999999', '1900-02-28 23:59:59', '1900-03-01', '1999-12-31 23:59:59', '2000-01-01', '2000-01-02', '2000-01-03', '2000-02-28 12:00',
			'2000-02-29 23:59:59', '2000-03-01', '2000-12-31 23:59:59', '2001-01-01', '2001-12-30', '2001-12-31', '2015-12-31', '2016-01-01', '2016-01-03',
			'2016-01-04', '2019-12-29', '2019-12-30', '2019-12-31', '2020-01-01', '2020-12-31', '2021-01-01', '2021-01-03', '2021-01-04', '2024-02-29',
			'2100-02-28', '2100-03-01']::timestamp[]) t WHERE t >= %L AND t < %L$q$, tbl, lo, hi);
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
	(9, 'day',                'SELECT date_trunc(''day'', ts)::date, count(*), sum(amt) FROM %s GROUP BY 1'),
	(10, 'cast to date',      'SELECT ts::date, count(*), sum(qty) FROM %s GROUP BY 1'),
	(11, 'week',              'SELECT date_trunc(''week'', ts)::date, count(*), sum(amt) FROM %s GROUP BY 1'),
	(12, 'extract dow',       'SELECT extract(dow FROM ts), count(*), sum(qty) FROM %s GROUP BY 1'),
	(13, 'extract isodow',    'SELECT extract(isodow FROM ts), count(*), sum(qty) FROM %s GROUP BY 1'),
	(14, 'extract doy',       'SELECT extract(doy FROM ts), count(*), sum(qty) FROM %s GROUP BY 1'),
	(15, 'extract day',       'SELECT extract(day FROM ts), count(*), sum(qty) FROM %s GROUP BY 1'),
	(16, 'extract week',      'SELECT extract(week FROM ts), count(*), sum(qty) FROM %s GROUP BY 1'),
	(17, 'extract isoyear',   'SELECT extract(isoyear FROM ts), count(*), sum(qty) FROM %s GROUP BY 1'),
	(18, 'extract quarter',   'SELECT extract(quarter FROM ts), count(*), sum(qty) FROM %s GROUP BY 1'),
	(19, 'extract decade',    'SELECT extract(decade FROM ts), count(*), sum(qty) FROM %s GROUP BY 1'),
	(20, 'year of the week',  'SELECT extract(year FROM date_trunc(''week'', ts)), count(*) FROM %s GROUP BY 1'),
	(21, 'hour: refused',     'SELECT date_trunc(''hour'', ts), count(*) FROM %s WHERE id < 5000 GROUP BY 1'),
	(22, 'zone on id',        'SELECT date_trunc(''month'', ts)::date, count(*), sum(amt) FROM %s WHERE id < 1000000 GROUP BY 1'),
	(23, 'range of days',     'SELECT date_trunc(''month'', ts)::date, count(*), sum(amt) FROM %s WHERE ts >= ''2000-03-01'' AND ts < ''2001-03-01'' GROUP BY 1'),
	(24, 'range cuts a day',  'SELECT date_trunc(''month'', ts)::date, count(*), sum(amt) FROM %s WHERE ts >= ''2000-03-01 06:00'' AND ts <= ''2001-03-01 18:30'' GROUP BY 1'),
	(25, 'open range',        'SELECT extract(year FROM ts), count(*), sum(amt) FROM %s WHERE ts >= ''2000-01-01'' GROUP BY 1'),
	(26, 'range, no key',     'SELECT count(*), sum(amt), count(amt), sum(qty) FROM %s WHERE ts >= ''2000-03-01'' AND ts < ''2000-09-01'''),
	(27, 'range cuts, no key', 'SELECT count(*), sum(amt) FROM %s WHERE ts >= ''2000-03-01 12:00'' AND ts < ''2000-09-01 12:00'''),
	(28, 'range, other key',  'SELECT cat, count(*), sum(amt) FROM %s WHERE ts >= ''2000-03-01'' AND ts < ''2000-09-01'' GROUP BY 1'),
	(29, 'test of a dim',     'SELECT date_trunc(''month'', ts)::date, count(*) FROM %s WHERE cat = 3 GROUP BY 1'),
	(30, 'min: none',         'SELECT date_trunc(''month'', ts)::date, min(qty) FROM %s GROUP BY 1'),
	(31, 'float: scan',       'SELECT date_trunc(''month'', ts)::date, sum(f) FROM %s GROUP BY 1'),
	(32, 'having, limit',     'SELECT date_trunc(''month'', ts)::date, count(*) FROM %s GROUP BY 1 HAVING count(*) > 100 ORDER BY 2 DESC, 1 LIMIT 5'),
	(33, 'two keys: scan',    'SELECT date_trunc(''month'', ts)::date, cat, count(*) FROM %s GROUP BY 1, 2'),
	(34, 'filter: scan',      'SELECT date_trunc(''month'', ts)::date, count(*) FILTER (WHERE cat = 1) FROM %s GROUP BY 1'),
	(35, 'date, month',       'SELECT date_trunc(''month'', d)::date, count(*), sum(amt) FROM %s GROUP BY 1'),
	(36, 'date, year',        'SELECT extract(year FROM d), count(*), sum(qty) FROM %s GROUP BY 1'),
	(37, 'date, dow',         'SELECT extract(dow FROM d), count(*), sum(qty) FROM %s GROUP BY 1'),
	(38, 'date, via timestamp', 'SELECT date_trunc(''quarter'', d::timestamp)::date, count(*), sum(amt) FROM %s GROUP BY 1'),
	(39, 'date, range',       'SELECT extract(month FROM d), count(*), sum(amt) FROM %s WHERE d >= ''2000-03-01'' AND d < ''2001-03-01'' GROUP BY 1'),
	(40, 'date, test',        'SELECT extract(year FROM d), count(*), sum(amt) FROM %s WHERE extract(month FROM d) = 3 GROUP BY 1'),
	(41, 'date, test in',     'SELECT date_trunc(''month'', d::timestamp)::date, count(*) FROM %s WHERE extract(dow FROM d) IN (0, 6) GROUP BY 1'),
	(42, 'tz at UTC, month',  'SELECT date_trunc(''month'', tz AT TIME ZONE ''UTC'')::date, count(*), sum(amt) FROM %s GROUP BY 1'),
	(43, 'tz at UTC, year',   'SELECT extract(year FROM tz AT TIME ZONE ''UTC''), count(*), sum(qty) FROM %s GROUP BY 1'),
	(44, 'tz session: scan',  'SELECT date_trunc(''month'', tz)::date, count(*), sum(amt) FROM %s GROUP BY 1'),
	(45, 'tz, range',         'SELECT count(*), sum(amt) FROM %s WHERE tz >= ''2000-03-01 00:00+00'' AND tz < ''2000-09-01 00:00+00'''),
	(46, 'tz, range at +03',  'SELECT count(*), sum(amt) FROM %s WHERE tz >= ''2000-03-01 00:00+03'' AND tz < ''2000-09-01 00:00+03'''),
	(47, 'tz at UTC, range',  'SELECT date_trunc(''quarter'', tz AT TIME ZONE ''UTC'')::date, count(*) FROM %s WHERE tz >= ''2000-01-01+00'' GROUP BY 1'),
	(48, 'tz 3-arg UTC',      'SELECT date_trunc(''month'', tz, ''UTC''), count(*), sum(amt) FROM %s GROUP BY 1');

-- the shapes n of lo..hi but those in skip
CREATE FUNCTION pm_check(tbl text, lo int DEFAULT 1, hi int DEFAULT 100, skip int[] DEFAULT '{}') RETURNS TABLE (n int, shape text, pre text, same bool) LANGUAGE sql AS $$
	SELECT z.n, z.shape, CASE WHEN u.units IS NULL THEN '-' WHEN u.pre IS NULL THEN 'scan' WHEN u.pre = 0 THEN 'none' WHEN u.pre = u.units THEN 'all' ELSE 'some' END,
	       pm_rows(z.q, true, true) IS NOT DISTINCT FROM pm_rows(z.q, false, true) AND pm_rows(z.q, false, true) IS NOT DISTINCT FROM pm_rows(z.q, false, false)
	FROM (SELECT n, shape, format(q, tbl) AS q FROM pm_q WHERE n BETWEEN lo AND hi AND n <> ALL (skip)) z, LATERAL pm_units(z.q) u ORDER BY z.n
$$;

-- three windows of data: 45 days (a unit has a few dozen days: cells of days), 3 years (months), 40 years (years); a cell costs a few dozen bytes of the
-- 5% a unit may spend on cells, and rows of other years in the last unit (the edges) do not take its cells from the others
SELECT pm_fill('tcd', '2000-02-10', 45, 1, 150000);
SELECT pm_fill('tcm', '1999-01-31 23:59:59', 1100, 1, 150000);
SELECT pm_fill('tcy', '1980-01-01', 14600, 1, 150000);
SELECT pm_edges('tcd', '2000-02-10', '2000-03-27');
SELECT pm_edges('tcm', '1999-01-01', '2003-01-01');
SELECT pm_edges('tcy', '1000-01-01', '3000-01-01');
VACUUM (ANALYZE, DISABLE_PAGE_SKIPPING) tcd, tcm, tcy;
SELECT 'tcd' AS tbl, count(*) > 1 AS units, array_agg(DISTINCT format) AS minors FROM sift.table_segments('tcd') WHERE kind = 'columns'
UNION ALL SELECT 'tcm', count(*) > 1, array_agg(DISTINCT format) FROM sift.table_segments('tcm') WHERE kind = 'columns'
UNION ALL SELECT 'tcy', count(*) > 1, array_agg(DISTINCT format) FROM sift.table_segments('tcy') WHERE kind = 'columns' ORDER BY 1;
-- the finest grain that fits: days of 45 days, months of 3 years, years of 40 (the unit with the edges may have a coarser one)
SELECT 'tcd' AS tbl, days > months + years AS days_most FROM sift._test_tbl_cal_grains('tcd')
UNION ALL SELECT 'tcm', months > days + years FROM sift._test_tbl_cal_grains('tcm')
UNION ALL SELECT 'tcy', years > days + months FROM sift._test_tbl_cal_grains('tcy') ORDER BY 1;

-- 1. cells of days
SELECT * FROM pm_check('tcd');
-- 2. cells of months
SELECT * FROM pm_check('tcm');
-- 3. cells of years
SELECT * FROM pm_check('tcy');

-- 4. rows in the write buffer: their superblocks are scanned, the others answer
INSERT INTO tcm SELECT 200000 + i, timestamp '2024-02-28' + i * interval '97 minutes', date '2024-02-28' + i / 8, timestamptz '2024-02-28+00' + i * interval '97 minutes', i % 10, i % 997, i % 7, i FROM generate_series(1, 3000) i;
SELECT * FROM pm_check('tcm');

-- 5. promoted again
VACUUM (DISABLE_PAGE_SKIPPING) tcm;
SELECT * FROM pm_check('tcm');

-- 6. tombstones and updates: a unit with a dead row is scanned, the others answer, and the key of an updated row is its new one
DELETE FROM tcm WHERE id BETWEEN 100 AND 140;
UPDATE tcm SET ts = ts + interval '70 days', d = d + 70, tz = tz + interval '70 days' WHERE id BETWEEN 200 AND 260;
SELECT * FROM pm_check('tcm');
DELETE FROM tcm WHERE id % 29 = 0;
UPDATE tcm SET ts = ts + interval '70 days', d = d + 70, tz = tz + interval '70 days' WHERE id % 31 = 1;
SELECT * FROM pm_check('tcm');
VACUUM (DISABLE_PAGE_SKIPPING) tcm;
SELECT * FROM pm_check('tcm');

-- 7. a transaction's own rows (not final: their units are scanned), then its rollback
BEGIN;
INSERT INTO tcm SELECT 400000 + i, timestamp '2030-07-01' + i * interval '3 hours', date '2030-07-01' + i / 8, timestamptz '2030-07-01+00' + i * interval '3 hours', i % 10, i, i % 7, i FROM generate_series(1, 500) i;
DELETE FROM tcm WHERE id % 37 = 0;
SELECT * FROM pm_check('tcm');
ROLLBACK;
SELECT * FROM pm_check('tcm');

-- 8. infinities and the ends of the range: a column of a unit that holds one has no cells, the others keep theirs (shape 6 casts the extract of an infinity to integer, which raises)
CREATE TABLE tci (LIKE tcd) USING sift_table;
SELECT pm_fill('tci', '2015-03-10', 60, 1, 40000);
INSERT INTO tci VALUES (500001, 'infinity', 'infinity', 'infinity', 1, 1.5, 1, 1), (500002, '-infinity', '-infinity', '-infinity', 2, 2.5, 2, 2),
	(500003, '4713-01-01 BC', '4713-01-01 BC', '4713-01-01 BC', 3, 3.5, 3, 3), (500004, '294276-12-31', '294276-12-31', '294276-12-31', 4, 4.5, 4, 4);
-- the planner keeps a lookup table off a date key when the statistics of the column span more than 4 million days, and the statistics are a sample of the table
-- (40,004 rows against 30,000 sampled): the sample leaves out the three rows that make the span in 4 analyses of 100, and the shapes then use the cells of the
-- unit without them. The whole table is the sample.
SET default_statistics_target = 1000;
VACUUM (ANALYZE, DISABLE_PAGE_SKIPPING) tci;
SELECT * FROM pm_check('tci', 1, 100, '{6}');
DELETE FROM tci WHERE id > 500000;
VACUUM (FULL, ANALYZE) tci;
RESET default_statistics_target;
SELECT * FROM pm_check('tci');

-- 9. the time zone of the session changes the date a timestamptz and a date are cut at, not the cells (UTC days) nor the result of a key that reads UTC
VACUUM (DISABLE_PAGE_SKIPPING) tcm;
SET TimeZone = 'Pacific/Kiritimati';
SELECT * FROM pm_check('tcm', 35, 48);
SET TimeZone = 'America/Sao_Paulo';
SELECT * FROM pm_check('tcd', 35, 48);
SET TimeZone = 'America/Los_Angeles';
SELECT * FROM pm_check('tcy', 35, 48);
RESET TimeZone;

-- 10. a segment written under sift.col_format_minor 7 has no calendar dim: the same table reads both, and a rewrite writes minor 8 again
CREATE TABLE tc7 (LIKE tcd) USING sift_table;
SET sift.col_format_minor = 7;
SELECT pm_fill('tc7', '2020-01-15', 60, 1, 60000);
VACUUM (ANALYZE, DISABLE_PAGE_SKIPPING) tc7;
RESET sift.col_format_minor;
SELECT array_agg(DISTINCT format) AS minors FROM sift.table_segments('tc7') WHERE kind = 'columns';
SELECT no_cal > 0 AS none_have_cells, days + months + years = 0 AS no_cells FROM sift._test_tbl_cal_grains('tc7');
SELECT * FROM pm_check('tc7', 1, 12);
SELECT pm_fill('tc7', '2024-03-15', 60, 100001, 160000);
VACUUM (DISABLE_PAGE_SKIPPING) tc7;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift.table_segments('tc7') WHERE kind = 'columns';
SELECT * FROM pm_check('tc7', 1, 12);
VACUUM (FULL) tc7;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift.table_segments('tc7') WHERE kind = 'columns';
SELECT days > 0 AS has_cells FROM sift._test_tbl_cal_grains('tc7');
SELECT * FROM pm_check('tc7', 1, 12);

-- the units a plan reads, and those it answered from the cells
SELECT * FROM pm_units('SELECT date_trunc(''month'', ts)::date, sum(amt) FROM tcd GROUP BY 1');
SELECT * FROM pm_units('SELECT extract(isoyear FROM ts), sum(amt) FROM tcm GROUP BY 1');
SELECT * FROM pm_units('SELECT extract(isoyear FROM ts), sum(amt) FROM tcy GROUP BY 1');

DROP TABLE tcd, tcm, tcy, tci, tc7;
DROP FUNCTION pm_check(text, int, int, int[]), pm_units(text), pm_rows(text, bool, bool), pm_fill(regclass, timestamp, int, int, int), pm_edges(regclass, timestamp, timestamp);
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
DROP TABLE pm_q;
