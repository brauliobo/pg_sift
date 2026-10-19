-- Calendar keys of a timestamp column (date_trunc, ::date, extract) are evaluated once per second, minute, hour or day the key depends
-- on, not once per distinct timestamp. Every query runs with and without the pushdown; the results must be the same.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;

CREATE TABLE b_t (id int, ts timestamp, tz timestamptz, d date, v int, n numeric(10,2)) USING sift_table;
CREATE TABLE b_h (LIKE b_t);

-- three years of rows (hours and fractions of a second), a few on the boundaries of days, months and years, the ends of the range, NULLs and
-- infinities, and a stretch before 1970
INSERT INTO b_t SELECT i, timestamp '2023-01-01' + i * interval '97 minutes 13.123457 seconds', timestamptz '2023-01-01 00:00:00+00' + i * interval '97 minutes',
	date '2023-01-01' + i % 900, i % 17, i / 7.0 FROM generate_series(1, 9000) i;
INSERT INTO b_t VALUES
	(10001, '2024-02-29 23:59:59.999999', '2024-02-29 23:59:59.999999+00', '2024-02-29', 1, 1.5),
	(10002, '2024-03-01 00:00:00', '2024-03-01 00:00:00+00', '2024-03-01', 2, 2.5),
	(10003, '2024-12-31 23:59:59.999999', NULL, NULL, 3, 3.5),
	(10004, '2025-01-01 00:00:00', NULL, NULL, 4, 4.5),
	(10005, '1969-12-31 23:59:59.999999', NULL, NULL, 5, 5.5),
	(10006, '1970-01-01 00:00:00', NULL, NULL, 6, 6.5),
	(10007, '0044-03-15 12:00:00 BC', NULL, NULL, 7, 7.5),
	(10008, '4000-06-15 00:00:00 BC', NULL, NULL, 8, 8.5),
	(10009, '294276-12-31 23:59:59.999999', NULL, NULL, 9, 9.5),
	(10010, 'infinity', 'infinity', 'infinity', 10, 10.5),
	(10011, '-infinity', '-infinity', '-infinity', 11, 11.5),
	(10012, NULL, NULL, NULL, 12, 12.5),
	(10013, NULL, NULL, NULL, 13, NULL);
INSERT INTO b_h SELECT * FROM b_t;
ANALYZE b_t;

CREATE FUNCTION b_rows(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO r;
	RETURN r;
END $$;

-- the key lines of the SiftAgg plan of a query ('' when it is not one)
CREATE FUNCTION b_keys(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := '';
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Group Keys:%' THEN r := r || trim(l); END IF;
	END LOOP;
	RETURN r;
END $$;

CREATE TABLE b_q (q text);
INSERT INTO b_q VALUES
	('SELECT date_trunc(''second'', ts) AS k, count(*), sum(v) FROM b_t GROUP BY 1'),
	('SELECT date_trunc(''minute'', ts) AS k, count(*), sum(n) FROM b_t GROUP BY 1'),
	('SELECT date_trunc(''hour'', ts) AS k, count(*), min(v), max(v) FROM b_t GROUP BY 1'),
	('SELECT date_trunc(''day'', ts) AS k, count(*), sum(v) FROM b_t GROUP BY 1'),
	('SELECT date_trunc(''week'', ts) AS k, count(*), sum(n) FROM b_t GROUP BY 1'),
	('SELECT date_trunc(''month'', ts) AS k, count(*), sum(v), avg(n) FROM b_t GROUP BY 1'),
	('SELECT date_trunc(''quarter'', ts) AS k, count(*), sum(v) FROM b_t GROUP BY 1'),
	('SELECT date_trunc(''year'', ts) AS k, count(*), sum(v) FROM b_t GROUP BY 1'),
	('SELECT date_trunc(''decade'', ts) AS k, count(*), sum(v) FROM b_t GROUP BY 1'),
	('SELECT date_trunc(''century'', ts) AS k, count(*), sum(v) FROM b_t GROUP BY 1'),
	('SELECT date_trunc(''millennium'', ts) AS k, count(*), sum(v) FROM b_t GROUP BY 1'),
	('SELECT date_trunc(''month'', ts)::date AS k, count(*), sum(n) FROM b_t GROUP BY 1'),
	('SELECT ts::date AS k, count(*), sum(v) FROM b_t GROUP BY 1'),
	('SELECT date_trunc(''day'', date_trunc(''hour'', ts)) AS k, count(*) FROM b_t GROUP BY 1'),
	('SELECT date_trunc(''hour'', date_trunc(''month'', ts)) AS k, count(*) FROM b_t GROUP BY 1'),
	('SELECT date_trunc(''month'', ts)::date AS k, count(DISTINCT v), count(DISTINCT n) FROM b_t GROUP BY 1'),
	('SELECT date_trunc(''month'', ts) AS k, v % 3 AS g, count(*), sum(n) FROM b_t GROUP BY 1, 2'),
	('SELECT date_trunc(''month'', ts) AS k, count(*), sum(v) FROM b_t WHERE v > 5 AND id % 3 = 0 GROUP BY 1'),
	('SELECT date_trunc(''month'', ts) AS k, count(*) FILTER (WHERE v < 8), sum(n) FILTER (WHERE id % 2 = 0) FROM b_t GROUP BY 1'),
	('SELECT date_trunc(''month'', ts) AS k, count(*) FROM b_t GROUP BY 1 HAVING count(*) > 100'),
	('SELECT extract(year FROM ts) AS k, count(*), sum(v) FROM b_t GROUP BY 1'),
	('SELECT extract(month FROM ts) AS k, count(*), sum(v) FROM b_t GROUP BY 1'),
	('SELECT extract(dow FROM ts) AS k, count(*), sum(n) FROM b_t GROUP BY 1'),
	('SELECT extract(hour FROM ts) AS k, count(*) FROM b_t GROUP BY 1'),
	('SELECT date_part(''quarter'', ts) AS k, count(*) FROM b_t GROUP BY 1'),
	('SELECT extract(isoyear FROM ts) AS k, extract(week FROM ts) AS w, count(*) FROM b_t GROUP BY 1, 2'),
	('SELECT date_trunc(''month'', ts) AS k, sum(v) OVER () FROM b_t GROUP BY 1, v'),
	('SELECT date_trunc(''year'', ts) AS k, date_trunc(''month'', ts) AS m, count(*) FROM b_t GROUP BY 1, 2'),
	('SELECT date_trunc(''month'', ts) AS k, count(*) FROM b_t GROUP BY 1 ORDER BY 2 DESC, 1 LIMIT 5'),
	('SELECT count(DISTINCT date_trunc(''month'', ts)) FROM b_t'),
	('SELECT date_trunc(''month'', tz) AS k, count(*) FROM b_t GROUP BY 1'),
	('SELECT date_trunc(''month'', d) AS k, count(*) FROM b_t GROUP BY 1'),
	('SELECT to_char(ts, ''YYYY-MM'') AS k, count(*) FROM b_t GROUP BY 1'),
	('SELECT date_trunc(''microseconds'', ts) AS k, count(*) FROM b_t GROUP BY 1'),
	('SELECT date_trunc(''month'', ts + interval ''1 day'') AS k, count(*) FROM b_t WHERE ts < ''3000-01-01'' GROUP BY 1');

CREATE FUNCTION b_compare(OUT query text, OUT same bool, OUT keys text) RETURNS SETOF record LANGUAGE plpgsql AS $$
BEGIN
	FOR query IN SELECT q FROM b_q ORDER BY q COLLATE "C" LOOP
		keys := b_keys(query);
		same := b_rows(query, true) IS NOT DISTINCT FROM b_rows(query, false);
		RETURN NEXT;
	END LOOP;
END $$;

-- in the write buffer, then sealed in a segment
SELECT query, same, keys FROM b_compare();
SELECT count(*) > 0 AS has_wbuf FROM sift.table_segments('b_t') WHERE kind = 'buffer';
VACUUM b_t;
SELECT query, same, keys FROM b_compare();
SELECT count(*) > 0 AS has_segment FROM sift.table_segments('b_t') WHERE kind <> 'buffer';

-- the groups of the pushdown are the ones of the heap table
SELECT b_rows('SELECT date_trunc(''month'', ts)::date AS k, count(*), sum(n) FROM b_t GROUP BY 1', true)
	= b_rows('SELECT date_trunc(''month'', ts)::date AS k, count(*), sum(n) FROM b_h GROUP BY 1', false) AS vs_heap;

-- rows changed after the seal
UPDATE b_t SET ts = ts + interval '40 days' WHERE id % 11 = 0;
DELETE FROM b_t WHERE id % 13 = 0;
INSERT INTO b_t SELECT 20000 + i, timestamp '2026-06-01' + i * interval '1 hour', NULL, NULL, i % 5, i FROM generate_series(1, 500) i;
SELECT query, same FROM b_compare();
