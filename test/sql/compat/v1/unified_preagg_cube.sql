-- Format 1 on the unified aggregate scan (sift.test_unified_agg) answers the stored units of a sift_col index and a sift_table from their pre-aggregates (col/v1_engine/store_ops_preagg.c): a unit
-- whose rows are all the snapshot sees is answered from its cube or its zone (a test of the key of the cube on each of its groups), one whose zones do not decide every other test of the statement, or with a dead row, a page not all-visible or a pending
-- write, is read as it was. Every answer is the heap's (sift.enable_agg = off), after DML, an aborted insert and VACUUM alike, and the units the cubes answered are counted by EXPLAIN ANALYZE.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
\set VERBOSITY terse
SET sift.test_create_format1 = on;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;

-- NULL keys and measures, long text keys, NaN and infinite floats, a soft-delete stamp on the first rows only, days that follow the id (units cover few of them), a pad that makes few rows a page
CREATE FUNCTION gen(a int, b int, stamp bool DEFAULT true) RETURNS TABLE (id int, kind text, grp int, ts date, amount int, f float8, deleted_at date, pad text) LANGUAGE sql AS $$
	SELECT i, CASE WHEN i % 17 = 0 THEN NULL ELSE 'kind-of-event-' || i % 5 END, i % 7, date '2024-01-01' + i / 500, CASE WHEN i % 11 = 0 THEN NULL ELSE i % 100 - 20 END,
		CASE WHEN i % 19 = 0 THEN NULL WHEN i % 997 = 0 THEN 'NaN' WHEN i % 1009 = 0 THEN 'Infinity' WHEN i % 1013 = 0 THEN '-Infinity' ELSE (i % 13) * 1.5 END,
		CASE WHEN stamp AND i <= 3000 AND i % 50 = 0 THEN date '2024-06-01' END, repeat('x', 450)
	FROM generate_series(a, b) i
$$;

CREATE TABLE ev (id int, kind text, grp int, ts date, amount int, f float8, deleted_at date, pad text);
INSERT INTO ev SELECT * FROM gen(1, 20000);
CREATE INDEX ev_col ON ev USING sift_col (id, kind, grp, ts, amount, f, deleted_at);
VACUUM (FREEZE, ANALYZE) ev;

-- the rows of q as text, in the order of their text
CREATE FUNCTION rows_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), '''') FROM (%s) x', q) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION heap_rows_of(q text) RETURNS text LANGUAGE sql SET sift.enable_agg = off AS $$ SELECT rows_of(q) $$;

-- the units the cubes answered (the preagg count of EXPLAIN ANALYZE) and whether the rows are the heap's
CREATE FUNCTION cube(q text) RETURNS TABLE (units int, same bool) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	units := 0;
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		units := units + coalesce((regexp_match(l, 'preagg=(\d+)'))[1]::int, 0);
	END LOOP;
	same := rows_of(q) = heap_rows_of(q);
	RETURN NEXT;
END $$;

-- the first n of the shapes, in the order of the first n being the ones every section looks at
CREATE FUNCTION queries(tbl text, n int DEFAULT 14) RETURNS TABLE (shape text, units int, same bool) LANGUAGE plpgsql AS $$
DECLARE s text[] := ARRAY[
	'count_by_kind', 'SELECT kind, count(*) FROM %s GROUP BY 1',
	'by_kind_sums', 'SELECT kind, count(*), count(amount), sum(amount), min(amount), max(amount), avg(amount) FROM %s GROUP BY 1',
	'global', 'SELECT count(*), count(deleted_at), count(kind), sum(amount), min(ts), max(ts), min(f), max(f), sum(f), avg(f), min(amount), max(amount) FROM %s',
	'count_live', 'SELECT count(*) FROM %s WHERE deleted_at IS NULL',
	'range_part', 'SELECT kind, count(*), sum(amount) FROM %s WHERE ts >= date ''2024-01-20'' AND ts < date ''2024-02-05'' GROUP BY 1',
	'by_grp_float', 'SELECT grp, sum(f), count(f), avg(f) FROM %s GROUP BY 1',
	'point_days', 'SELECT count(*), sum(amount) FROM %s WHERE ts IN (date ''2024-01-05'', date ''2024-02-04'', date ''2024-01-12'')',
	'live_by_kind', 'SELECT kind, count(*), sum(amount) FROM %s WHERE deleted_at IS NULL AND ts >= date ''2024-01-01'' GROUP BY 1',
	'by_day', 'SELECT ts, count(*), sum(amount) FROM %s GROUP BY 1',
	'count_deleted', 'SELECT count(*) FROM %s WHERE deleted_at IS NOT NULL',
	'range_covers', 'SELECT kind, count(*), sum(amount) FROM %s WHERE ts >= date ''2024-01-01'' GROUP BY 1',
	'range_none', 'SELECT kind, count(*) FROM %s WHERE ts > date ''2030-01-01'' GROUP BY 1',
	'point_day', 'SELECT count(*), sum(amount) FROM %s WHERE ts = date ''2024-02-04''',
	'int_range', 'SELECT grp, count(*) FROM %s WHERE id <= 10000 AND grp <> 3 GROUP BY 1'];
BEGIN
	FOR i IN 1..n LOOP
		shape := s[2 * i - 1];
		SELECT c.units, c.same INTO units, same FROM cube(format(s[2 * i], tbl)) c;
		RETURN NEXT;
	END LOOP;
END $$;

-- 1. clean units: the cubes answer, and the rows are the heap's
SELECT * FROM queries('ev');

-- the distinct values of a dimension are the keys of its cells
SELECT * FROM cube('SELECT count(DISTINCT grp) FROM ev');
-- 2. a statement the cubes do not hold is read: a text minimum, two keys, a test the zones cannot decide on a row
SELECT * FROM cube('SELECT kind, min(kind) FROM ev GROUP BY 1');
SELECT * FROM cube('SELECT grp, kind, count(*) FROM ev GROUP BY 1, 2');
SELECT * FROM cube('SELECT count(*) FROM ev WHERE amount % 2 = 0');

-- 3. dead rows and pending writes: the units they touch are read, the others are still answered
UPDATE ev SET amount = amount + 1, kind = 'kind-of-event-9' WHERE id BETWEEN 100 AND 130;
DELETE FROM ev WHERE id BETWEEN 10000 AND 10010;
SELECT * FROM queries('ev', 5);
INSERT INTO ev SELECT * FROM gen(20001, 20500, false);
SELECT * FROM queries('ev', 5);

-- 4. an aborted insert and an aborted delete leave no row, and answer nothing of their own
BEGIN;
INSERT INTO ev SELECT * FROM gen(30001, 30300, false);
DELETE FROM ev WHERE id BETWEEN 16000 AND 16300;
ROLLBACK;
SELECT * FROM queries('ev', 5);

-- 5. after VACUUM the units the writes did not touch are clean again
VACUUM (FREEZE) ev;
SELECT * FROM queries('ev', 5);

-- 6. the rows a transaction deleted are not in its own answer, and the cubes of the units it left alone are
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT count(*) AS before_n FROM ev;
DELETE FROM ev WHERE id % 3 = 0 AND id < 3000;
SELECT * FROM cube('SELECT kind, count(*) FROM ev GROUP BY 1');
COMMIT;
VACUUM (FREEZE) ev;
SELECT * FROM cube('SELECT kind, count(*), sum(amount) FROM ev GROUP BY 1');

-- 7. a sift_table
CREATE TABLE tv (id int, kind text, grp int, ts date, amount int, f float8, deleted_at date, pad text) USING sift_table;
INSERT INTO tv SELECT * FROM gen(1, 20000);
VACUUM (FREEZE, ANALYZE) tv;
SELECT * FROM queries('tv', 5);
UPDATE tv SET amount = amount + 1 WHERE id BETWEEN 100 AND 130;
DELETE FROM tv WHERE id BETWEEN 10000 AND 10010;
SELECT * FROM queries('tv', 5);
BEGIN;
INSERT INTO tv SELECT * FROM gen(30001, 30300, false);
DELETE FROM tv WHERE id BETWEEN 16000 AND 16300;
ROLLBACK;
SELECT * FROM queries('tv', 5);
VACUUM (FREEZE) tv;
SELECT * FROM queries('tv', 5);

DROP TABLE ev, tv;
DROP FUNCTION queries, cube, heap_rows_of, rows_of, gen;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
