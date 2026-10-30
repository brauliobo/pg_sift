-- ORDER BY a leaf, then columns the index does not hold LIMIT n: the scan gives the rows in the order of the first keys and the Sort above puts the others in order.
-- The rows that tie with the last wanted one on the first keys must all be there: the heap has room for the ties the statistics say, and a group larger than that
-- starts the scan again with a heap 16 times larger, twice, then keeps every row. Every query runs twice, the plain plan as the reference (sift.enable_agg off)
-- and the pushed one, and the rows are compared in order, after the build, with rows in the write buffer, after updates (a tie group the statistics do not know), deletes and VACUUM.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET sift.async_maintenance = off;

-- k: 400 values over 6000 rows (15 rows tie), NULLs; t: seconds, so that a few rows share one; w: the key the index does not hold; s: strings
CREATE TABLE tt (id int PRIMARY KEY, k int, k2 int, t timestamp, w bigint, x double precision, s text, pad text DEFAULT repeat('x', 600));
CREATE FUNCTION tt_rows(lo int, hi int) RETURNS void LANGUAGE sql AS $$
	INSERT INTO tt (id, k, k2, t, w, x, s)
	SELECT i, CASE WHEN i % 41 = 0 THEN NULL ELSE i % 400 END, i % 3,
		timestamp '2020-01-01' + (i / 4) * interval '1 second',
		CASE WHEN i % 37 = 0 THEN NULL ELSE (i * 7919) % 1000 END, (i % 17) - 8.0, md5((i % 211)::text)
	FROM generate_series(lo, hi) i $$;
SELECT tt_rows(1, 6000);
CREATE INDEX tt_col ON tt USING sift_col (id, k, k2, t);
VACUUM (ANALYZE) tt;

CREATE FUNCTION tt_mode(pushed bool) RETURNS void LANGUAGE sql AS $$ SELECT sift_helpers.agg_mode(pushed) $$;

-- 'same' / 'DIFFERENT' (the rows and their order), '+ties' when the pushed plan is the scan under a Sort (or '+all' when it keeps every row), '+restarts N' when it started again N times, and the number of rows
CREATE FUNCTION tt_check(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE n bigint; plan text := ''; line text;
BEGIN
	DROP TABLE IF EXISTS tt_a, tt_b;
	PERFORM tt_mode(true);
	FOR line IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP plan := plan || line || E'\n'; END LOOP;
	EXECUTE 'CREATE TEMP TABLE tt_a AS SELECT row_number() OVER () AS pos, row_to_json(tq)::text AS r FROM (' || q || ') tq';
	PERFORM tt_mode(false);
	EXECUTE 'CREATE TEMP TABLE tt_b AS SELECT row_number() OVER () AS pos, row_to_json(tq)::text AS r FROM (' || q || ') tq';
	SELECT count(*) INTO n FROM ((SELECT pos, r FROM tt_a EXCEPT ALL SELECT pos, r FROM tt_b) UNION ALL (SELECT pos, r FROM tt_b EXCEPT ALL SELECT pos, r FROM tt_a)) d;
	RETURN CASE WHEN n = 0 THEN 'same' ELSE 'DIFFERENT' END || CASE WHEN plan ~ ' candidates' THEN '+ties' WHEN plan ~ ' restarts' THEN '+all' ELSE '' END ||
		coalesce((SELECT ' restarts ' || (regexp_match(plan, '(\d+) restarts'))[1]), '') || ' ' || (SELECT count(*) FROM tt_b);
END $$;

CREATE TABLE tt_q (n serial, q text);
INSERT INTO tt_q (q) VALUES
	('SELECT id, k, w FROM tt ORDER BY k, w, id LIMIT 10'),
	('SELECT id, k, w FROM tt ORDER BY k DESC, w, id LIMIT 10'),
	('SELECT id, k, w FROM tt ORDER BY k, w DESC, id LIMIT 10'),
	('SELECT id, k, w FROM tt ORDER BY k DESC, w DESC, id DESC LIMIT 10'),
	('SELECT id, k, w FROM tt ORDER BY k NULLS FIRST, w NULLS FIRST, id LIMIT 10'),
	('SELECT id, k, w FROM tt ORDER BY k DESC NULLS LAST, w DESC NULLS LAST, id LIMIT 10'),
	('SELECT id, k, w FROM tt ORDER BY k NULLS FIRST, w DESC NULLS LAST, id LIMIT 40'),
	('SELECT id, k, w FROM tt ORDER BY k, w, id LIMIT 10 OFFSET 7'),
	('SELECT id, k, w FROM tt ORDER BY k, w, id LIMIT 100 OFFSET 30'),
	('SELECT id, k, w FROM tt ORDER BY k, w, id LIMIT 1'),
	('SELECT id, k, w FROM tt ORDER BY k, w, id LIMIT 5000'),
	('SELECT id, k, w FROM tt WHERE k2 = 1 ORDER BY k, w, id LIMIT 10'),
	('SELECT id, k, w FROM tt WHERE k BETWEEN 100 AND 300 AND k2 <> 2 ORDER BY k, w, id LIMIT 25'),
	('SELECT id, k, w FROM tt WHERE k > 1000 ORDER BY k, w, id LIMIT 10'),
	('SELECT id, k, t, w FROM tt ORDER BY k, t, w, id LIMIT 10'),
	('SELECT id, k, t, w FROM tt ORDER BY t DESC, w, id LIMIT 10'),
	('SELECT id, k, t, w FROM tt ORDER BY t, w DESC NULLS FIRST, id LIMIT 30'),
	('SELECT id, k, t, w FROM tt ORDER BY k2, k, w, id LIMIT 10'),
	('SELECT id, k, s FROM tt ORDER BY k, s, id LIMIT 10'),
	('SELECT id, k, s FROM tt ORDER BY k, s COLLATE "C" DESC, id LIMIT 10'),
	('SELECT id, k, x FROM tt ORDER BY k, x, id LIMIT 10'),
	('SELECT id, k, w FROM tt ORDER BY k, (w % 7), id DESC LIMIT 10'),
	('SELECT id, k, w FROM tt ORDER BY k, w * k2, id LIMIT 10'),
	('SELECT id, k + 1 AS a, w FROM tt ORDER BY k, w, id LIMIT 10'),
	('SELECT id FROM tt ORDER BY k, w, id LIMIT 10'),
	('SELECT w, s FROM tt ORDER BY k, w, id LIMIT 10'),
	('SELECT id, k, w FROM tt ORDER BY k, w * 1000 + id LIMIT 10');
CREATE FUNCTION tt_all() RETURNS TABLE (n int, result text, q text) LANGUAGE sql AS $$ SELECT tt_q.n, tt_check(tt_q.q), tt_q.q FROM tt_q ORDER BY tt_q.n $$;

SELECT * FROM tt_all();

-- rows in the write buffer
SELECT tt_rows(6001, 6600);
SELECT * FROM tt_all();

-- a tie group of 300 rows the statistics do not know: the first start's heap is too small, the second holds it
UPDATE tt SET k = -5 WHERE id BETWEEN 1000 AND 1299;
SELECT * FROM tt_all();

-- 1700 rows: neither start's heap holds them, the third keeps every row
UPDATE tt SET k = -6 WHERE id BETWEEN 2000 AND 3699;
SELECT * FROM tt_all();

-- deletes, then VACUUM, then new statistics
DELETE FROM tt WHERE id % 5 = 0;
SELECT * FROM tt_all();
VACUUM (ANALYZE) tt;
SELECT * FROM tt_all();

-- limits lowered (a testing build): a heap that cannot hold the rows, or the keys, does not make a plan with the sort of every row; the keys that follow stay with the Sort above
SET sift.test_limits = 'topn_rows=20';
SELECT * FROM tt_all() WHERE n IN (1, 7, 9, 15, 19);
SET sift.test_limits = 'topn_keys=1';
SELECT * FROM tt_all() WHERE n IN (1, 7, 9, 15, 19);
RESET sift.test_limits;

-- the plan is chosen by the cost: a first key that few values share gets the scan (query 18, whose first key has three values of 2,000 rows each, takes it: 'same+ties' above), and sift.enable_topn turns it off
SET sift.enable_agg = on;
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT id, k, w FROM tt ORDER BY k, w, id LIMIT 10;
RESET enable_seqscan;
SET sift.enable_topn = off;
EXPLAIN (COSTS OFF) SELECT id, k, w FROM tt ORDER BY k, w, id LIMIT 10;
RESET sift.enable_topn;

-- more than 8,192 rows on a value of the first key (two values over 20,000 rows) keep the normal plan: the keys that follow are read from the heap for every row of the tie
CREATE TABLE tt_big AS SELECT i AS id, i % 2 AS k, (i * 7919) % 1000 AS w FROM generate_series(1, 20000) i;
CREATE INDEX tt_big_col ON tt_big USING sift_col (id, k);
ANALYZE tt_big;
EXPLAIN (COSTS OFF) SELECT id, k, w FROM tt_big ORDER BY k, w, id LIMIT 10;
DROP TABLE tt_big;

DROP TABLE tt_q;
DROP FUNCTION tt_all();
DROP FUNCTION tt_check(text);
DROP FUNCTION tt_mode(bool);
DROP FUNCTION tt_rows(int, int);
DROP TABLE tt;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
