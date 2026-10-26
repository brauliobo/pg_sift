-- The cube of a sift_table keeps the minimum and maximum of each integer measure per group, as the one of a sift_col index does (column format minor 7, docs/COL-PREAGG.md
-- section 2): GROUP BY with min and max over whole units reads no row. The calendar cells of a date or timestamp column hold none (their bytes fit the share of the unit
-- without them). Written by every writer of the groups; the answers are the heap plan's, with deletes, updates and rows of the write buffer in between.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
\ir test/agg_check_query.sql

CREATE TABLE tm_src AS
SELECT i AS id, i % 6 AS kind, 'g' || lpad((i % 30)::text, 2, '0') AS grp, CASE WHEN i % 7 = 0 THEN NULL ELSE (i * 977) % 100000 - 50000 END AS val,
       CASE WHEN i % 11 = 0 THEN NULL ELSE (i * 37) % 1000 - 400 END AS small, ((i * 91) % 20000) / 100.0 - 50 AS amt, (i % 71) / 7.0 - 3 AS flt,
       timestamp '2024-01-01' + (i / 40) * interval '1 hour' AS ts, repeat('p', 100) AS pad
FROM generate_series(1, 60000) i;

-- the minors of the groups: 7 or more, and none under the setting 6 (which a 3.2 library reads)
CREATE TABLE tm USING sift_table AS SELECT * FROM tm_src ORDER BY id;
SET sift.col_format_minor = 6;
CREATE TABLE tm_six USING sift_table AS SELECT * FROM tm_src ORDER BY id;
RESET sift.col_format_minor;
VACUUM (FREEZE, ANALYZE) tm;
VACUUM (FREEZE, ANALYZE) tm_six;
SELECT t, min(format) >= 7 AS minmax_minor, max(format) <= 6 AS old_minor
FROM (SELECT 'tm' AS t, format FROM sift.table_segments('tm') WHERE kind = 'columns' UNION ALL SELECT 'tm_six', format FROM sift.table_segments('tm_six') WHERE kind = 'columns') s
GROUP BY t ORDER BY t;

-- the units of a query that were answered from their pre-aggregates
CREATE FUNCTION tmm_units(q text, OUT units int, OUT preagg int) LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, TIMING OFF, COSTS OFF, FORMAT JSON) ' || q INTO j;
	units := coalesce(jsonb_path_query_first(j, '$.**."Units"')::int, 0);
	preagg := coalesce(jsonb_path_query_first(j, '$.**."Units From Pre-aggregates"')::int, 0);
END $$;

-- whether most units of the query were answered from their pre-aggregates
CREATE FUNCTION tmm_most_units(q text) RETURNS bool LANGUAGE sql AS $$ SELECT preagg * 100 >= units * 80 AND units > 0 FROM tmm_units(q) $$;

CREATE TABLE tmm_q (n int, shape text, q text);
INSERT INTO tmm_q VALUES
	(1, 'min, max by a number', 'SELECT kind, min(val), max(val), count(*) FROM @ GROUP BY kind'),
	(2, 'min, max by a text', 'SELECT grp, min(small) AS lo_s, max(small) AS hi_s, min(val) AS lo_v, max(val) AS hi_v FROM @ GROUP BY grp'),
	(3, 'two measures, a filter on the key', 'SELECT kind, min(val) AS lo, max(small) AS hi FROM @ WHERE kind IN (1, 4) GROUP BY kind'),
	(4, 'whole table', 'SELECT min(val) AS lo_v, max(val) AS hi_v, min(small) AS lo_s, max(small) AS hi_s FROM @'),
	(5, 'min of a month', 'SELECT date_trunc(''month'', ts)::date, min(val), max(val) FROM @ GROUP BY 1'),
	(6, 'numeric', 'SELECT kind, min(amt), max(amt) FROM @ GROUP BY kind'),
	(7, 'float', 'SELECT kind, min(flt), max(flt) FROM @ GROUP BY kind'),
	(8, 'a range of the row', 'SELECT kind, min(val), max(val) FROM @ WHERE id BETWEEN 10000 AND 40000 GROUP BY kind');

-- which queries are answered from the units' pre-aggregates, with and without the min and max in them (the others read the rows: a float or numeric measure has no min and
-- max in the cube, a month is a calendar cell)
SELECT n, shape, tmm_most_units(replace(q, '@', 'tm')) AS from_cube, tmm_most_units(replace(q, '@', 'tm_six')) AS from_cube_old_minor FROM tmm_q WHERE n IN (1, 2, 4) ORDER BY n;

-- the answers are the heap plan's: of the groups as written, then with deleted, updated and buffered rows, then after VACUUM FULL wrote the groups again
CREATE FUNCTION tmm_compare(tbl text, OUT queries int, OUT pushed int, OUT differences bigint) LANGUAGE plpgsql AS $$
DECLARE q record; c record;
BEGIN
	queries := 0; pushed := 0; differences := 0;
	FOR q IN SELECT * FROM tmm_q ORDER BY n LOOP
		SELECT * INTO c FROM check_query(replace(q.q, '@', tbl));
		queries := queries + 1;
		pushed := pushed + c.pushed::int;
		differences := differences + c.differences;
	END LOOP;
END $$;
SELECT * FROM tmm_compare('tm');
SELECT * FROM tmm_compare('tm_six');
DELETE FROM tm WHERE id % 11 = 0;
UPDATE tm SET val = val + 100000, kind = kind + 1 WHERE id % 13 = 0;
INSERT INTO tm SELECT i + 100000, i % 6, 'g' || lpad((i % 30)::text, 2, '0'), i - 500, NULL, i / 3.0, i / 5.0, timestamp '2024-03-01' + i * interval '1 minute', 'new' FROM generate_series(1, 300) i;
SELECT * FROM tmm_compare('tm');
BEGIN;
DELETE FROM tm WHERE id % 5 = 0;
SELECT * FROM tmm_compare('tm');
ROLLBACK;
VACUUM (FULL, ANALYZE) tm;
SELECT * FROM tmm_compare('tm');
SELECT tmm_most_units('SELECT kind, min(val), max(val) FROM tm GROUP BY kind') AS from_cube_again;

DROP TABLE tm, tm_six, tm_src, tmm_q;
DROP FUNCTION tmm_units(text), tmm_most_units(text), tmm_compare(text);
DROP FUNCTION check_query(text);
DROP EXTENSION pg_sift;
