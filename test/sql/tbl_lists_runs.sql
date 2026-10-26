-- The key lists and sorted runs of a sift_table: its column groups keep, per unit, the key list or bitmap of
-- each int2, int4 or date foreign key or *_id column that is not unique, and the sorted run of each timestamp column whose run takes at most the share of the unit
-- the key lists take. count(DISTINCT key) over whole units then reads no row, and ORDER BY a timestamp LIMIT n reads the head of each unit's run. Written by every
-- writer of the groups (INSERT, COPY and CREATE TABLE AS, VACUUM and its merge); the answers are the heap plan's, with deletes and updates in between.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET statement_timeout = '240s';
\ir test/agg_check_query.sql

-- 100,000 rows. cust_id: 25,000 values in runs of 4 (a *_id column), ref: a foreign key of 200 values scattered over the units, plain: 60,000 scattered values (no name,
-- no constraint), uniq_id: unique in tf; ts: ascending with ties of 5; tsn: the same with a NULL in every 17th row (its run takes more than the share of a unit,
-- where ts has none to write); tsr: scattered (the same)
CREATE TABLE tl_ref (id int PRIMARY KEY);
INSERT INTO tl_ref SELECT i FROM generate_series(0, 199) i;
CREATE TABLE tl_src AS
SELECT i AS id, i / 4 AS cust_id, (hashint4(i) & 2147483647) % 200 AS ref, (hashint4(i + 1) & 2147483647) % 60000 AS plain, i AS uniq_id, i % 9 AS g,
       timestamp '2024-01-01' + (i / 5) * interval '37 seconds' AS ts,
       CASE WHEN i % 17 = 0 THEN NULL ELSE timestamp '2024-01-01' + (i / 5) * interval '37 seconds' END AS tsn,
       timestamptz '2024-01-01' + ((hashint4(i + 2) & 2147483647) % 900000) * interval '1 second' AS tsr, repeat('x', 40) AS pad
FROM generate_series(1, 100000) i;

CREATE TABLE tl (id int, cust_id int, ref int, plain int, uniq_id int, g int, ts timestamp, tsn timestamp, tsr timestamptz, pad text) USING sift_table;
INSERT INTO tl SELECT * FROM tl_src ORDER BY id;
-- a foreign key and a unique index keep the rows in the write buffers until a VACUUM writes the groups
CREATE TABLE tf (id int, cust_id int, ref int REFERENCES tl_ref, plain int, uniq_id int UNIQUE, g int, ts timestamp, tsn timestamp, tsr timestamptz, pad text) USING sift_table;
INSERT INTO tf SELECT * FROM tl_src ORDER BY id;
CREATE TABLE tc USING sift_table AS SELECT * FROM tl_src ORDER BY id;
VACUUM (FREEZE, ANALYZE) tl;
VACUUM (FREEZE, ANALYZE) tf;
VACUUM (FREEZE, ANALYZE) tc;

-- the column format minor of the groups (18 with the heads of the sorted runs: a unit has one only when none of its rows is NULL; 22 where the key table of a foreign key fits)
SELECT t, array_agg(DISTINCT format) AS minors FROM (SELECT 'tl' AS t, format FROM sift.table_segments('tl') WHERE kind = 'columns'
	UNION ALL SELECT 'tf', format FROM sift.table_segments('tf') WHERE kind = 'columns'
	UNION ALL SELECT 'tc', format FROM sift.table_segments('tc') WHERE kind = 'columns') s GROUP BY t ORDER BY t;

-- the units of a query that a list or a run answered
CREATE FUNCTION tlr_units(q text, OUT units int, OUT preagg int, OUT heads int, OUT runs int) LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, TIMING OFF, COSTS OFF, FORMAT JSON) ' || q INTO j;
	units := coalesce(jsonb_path_query_first(j, '$.**."Units"')::int, 0);
	preagg := coalesce(jsonb_path_query_first(j, '$.**."Units From Pre-aggregates"')::int, 0);
	heads := coalesce(jsonb_path_query_first(j, '$.**."Units Answered By Run Head"')::int, 0);
	runs := coalesce(jsonb_path_query_first(j, '$.**."Units Read By Sorted Run"')::int, 0);
END $$;

-- whether most units of the query were answered from their pre-aggregates (a unit that holds rows of the write buffer is read)
CREATE FUNCTION tlr_most_units(q text) RETURNS bool LANGUAGE sql AS $$ SELECT preagg * 100 >= units * 80 AND units > 0 FROM tlr_units(q) $$;

-- which columns the units hold the keys of, and which sorted runs: the foreign key and *_id columns, not the unique one nor the unnamed one; the ascending timestamp, not the scattered one
SELECT t, c,
       tlr_most_units(format('SELECT count(DISTINCT %s) FROM %s', c, t)) AS key_list
FROM unnest(ARRAY['tl', 'tf', 'tc']) t, unnest(ARRAY['cust_id', 'ref', 'plain', 'uniq_id']) c ORDER BY t, c;
SELECT t, c,
       (tlr_units(format('SELECT id FROM %s ORDER BY %s DESC LIMIT 10', t, c))).heads > 0 AS run_head
FROM unnest(ARRAY['tl', 'tf', 'tc']) t, unnest(ARRAY['ts', 'tsn', 'tsr']) c ORDER BY t, c;

-- the answers are the heap plan's
CREATE TABLE tlr_q (q text);
INSERT INTO tlr_q VALUES
	('SELECT count(DISTINCT cust_id) FROM @'),
	('SELECT count(DISTINCT ref) FROM @'),
	('SELECT count(DISTINCT cust_id) AS n, count(*) AS c, sum(g) AS s FROM @ WHERE id > 7000'),
	('SELECT count(DISTINCT ref) FROM @ WHERE g IN (1, 4) AND id < 30000'),
	('SELECT g, count(DISTINCT cust_id) FROM @ GROUP BY g'),
	('SELECT count(DISTINCT cust_id) FROM @ WHERE cust_id BETWEEN 100 AND 400'),
	('SELECT id, ts FROM @ ORDER BY ts DESC, id LIMIT 10'),
	('SELECT id, ts FROM @ ORDER BY ts, id LIMIT 25 OFFSET 5'),
	('SELECT id, tsn FROM @ ORDER BY tsn DESC NULLS LAST, id LIMIT 40'),
	('SELECT id, tsn FROM @ ORDER BY tsn NULLS FIRST, id LIMIT 40'),
	('SELECT id, ts FROM @ WHERE g = 3 ORDER BY ts DESC, id LIMIT 7'),
	('SELECT id, ts FROM @ WHERE ts > ''2024-01-05'' ORDER BY ts, id LIMIT 300'),
	('SELECT id, tsr FROM @ ORDER BY tsr DESC, id LIMIT 10');
-- the queries run on tbl, those the aggregate scan took, and the rows that differ from the plain plan's
CREATE FUNCTION tlr_compare(tbl text, OUT queries int, OUT pushed int, OUT differences bigint) LANGUAGE plpgsql AS $$
DECLARE q text; c record;
BEGIN
	queries := 0; pushed := 0; differences := 0;
	FOR q IN SELECT tlr_q.q FROM tlr_q ORDER BY tlr_q.q COLLATE "C" LOOP
		SELECT * INTO c FROM check_query(replace(q, '@', tbl));
		queries := queries + 1;
		pushed := pushed + c.pushed::int;
		differences := differences + c.differences;
	END LOOP;
END $$;
SELECT * FROM tlr_compare('tl');
SELECT * FROM tlr_compare('tf');
SELECT * FROM tlr_compare('tc');

-- rows that were deleted, updated (the foreign key table is deleted from and inserted into: it checks its references on update), and in the write buffers: the units with
-- a tombstone or a buffered row are read, the others still answer from their lists and runs
DELETE FROM tl WHERE id % 11 = 0;
UPDATE tl SET cust_id = cust_id + 5000, ts = ts + interval '3 days' WHERE id % 13 = 0;
INSERT INTO tl SELECT i + 100000, i % 700, i % 200, i, i + 100000, i % 9, timestamp '2024-03-01' + i * interval '1 minute', NULL, now(), 'new' FROM generate_series(1, 300) i;
DELETE FROM tf WHERE id % 11 = 0;
INSERT INTO tf SELECT i + 100000, i % 700, i % 200, i, i + 100000, i % 9, timestamp '2024-03-01' + i * interval '1 minute', NULL, now(), 'new' FROM generate_series(1, 300) i;
SELECT * FROM tlr_compare('tl');
SELECT * FROM tlr_compare('tf');
-- the transaction's own changes, and a rolled back one
BEGIN;
DELETE FROM tl WHERE id % 5 = 0;
SELECT * FROM tlr_compare('tl');
ROLLBACK;
SELECT * FROM tlr_compare('tl');

-- VACUUM writes the groups again, with lists and runs
VACUUM (FREEZE, ANALYZE) tl;
VACUUM (FREEZE, ANALYZE) tf;
SELECT * FROM tlr_compare('tl');
SELECT * FROM tlr_compare('tf');
-- the groups with dead rows are read, not answered from their lists; the groups that are mostly dead are rewritten by VACUUM (and by sift.table_maintain) with lists and runs again
SELECT tlr_most_units('SELECT count(DISTINCT cust_id) FROM tl') AS tl_lists_with_dead_rows;
DELETE FROM tl WHERE id % 3 <> 0;
DELETE FROM tf WHERE id % 3 <> 0;
DELETE FROM tc WHERE id % 3 <> 0;
VACUUM (FREEZE, ANALYZE) tl;
VACUUM (FREEZE, ANALYZE) tf;
SELECT merged > 0 AS tc_rewrote FROM sift.table_maintain('tc');
VACUUM (FREEZE, ANALYZE) tc;
SELECT * FROM tlr_compare('tc');
SELECT * FROM tlr_compare('tl');
SELECT * FROM tlr_compare('tf');
SELECT tlr_most_units('SELECT count(DISTINCT cust_id) FROM tl') AS tl_lists,
       (tlr_units('SELECT id FROM tl ORDER BY ts DESC LIMIT 10')).heads > 0 AS tl_heads,
       tlr_most_units('SELECT count(DISTINCT ref) FROM tf') AS tf_lists,
       tlr_most_units('SELECT count(DISTINCT cust_id) FROM tc') AS tc_lists;

-- under a lower setting the groups are written as before: no lists, no runs, the same answers
SET sift.col_format_minor = 8;
CREATE TABLE tlo USING sift_table AS SELECT * FROM tl_src ORDER BY id;
VACUUM (FREEZE, ANALYZE) tlo;
RESET sift.col_format_minor;
SELECT array_agg(DISTINCT format) AS minors FROM sift.table_segments('tlo') WHERE kind = 'columns';
SELECT tlr_most_units('SELECT count(DISTINCT cust_id) FROM tlo') AS key_list,
       (tlr_units('SELECT id FROM tlo ORDER BY ts DESC LIMIT 10')).heads > 0 AS run_head;
SELECT * FROM tlr_compare('tlo');

DROP TABLE tlo, tc, tf, tl, tl_src, tl_ref, tlr_q;
DROP FUNCTION tlr_units(text), tlr_most_units(text), tlr_compare(text);
DROP FUNCTION check_query(text);
DROP EXTENSION pg_sift;
