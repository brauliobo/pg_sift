-- The grouping of an aggregate scan. The tests that were one file each, one segment for each, each in a schema of its own with the settings reset between them; every segment runs as its test did.
CREATE EXTENSION pg_sift;

-- >>> agg_group_nospill_count
CREATE SCHEMA s1;
GRANT USAGE ON SCHEMA s1 TO PUBLIC;
SET search_path = s1, public;
-- The bound on the group table of a scan that cannot spill counts the table: the groups at the bytes the planner prices them, not the sets, values and
-- dictionaries that the aggregates keep beside it. A prepared (generic) aggregate whose groups take a few kB runs under a bound of 64 kB, where the memory of its
-- table context (sets, dictionaries) is more; the same statement runs again as PostgreSQL plans it when the bound is under its groups.
SET max_parallel_workers_per_gather = 0;
SET client_min_messages = warning;

CREATE TABLE events (id int, k int, v int, w int);
INSERT INTO events SELECT i, (i % 100) * 1000, ((i::bigint * 7919) % 1000)::int, i % 7 FROM generate_series(1, 20000) i;
CREATE INDEX events_col ON events USING sift_col (id, k, v, w);
VACUUM (FREEZE, ANALYZE) events;
INSERT INTO events SELECT i, (i % 3000) * 1000, ((i::bigint * 7919) % 1000)::int, i % 7 FROM generate_series(20001, 60000) i;

SET sift.enable_agg = off;
CREATE TEMP TABLE want AS SELECT k, string_agg(v::text, ',' ORDER BY v) AS b, count(DISTINCT w) AS c FROM events GROUP BY k;
RESET sift.enable_agg;

SET plan_cache_mode = force_generic_plan;
SET sift.agg_force = on;
PREPARE q AS SELECT k, string_agg(v::text, ',' ORDER BY v) AS b, count(DISTINCT w) AS c FROM events GROUP BY k;
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;

CREATE TEMP TABLE got AS EXECUTE q;
SELECT count(*) AS groups, (SELECT count(*) FROM (TABLE want EXCEPT TABLE got) d) AS differ FROM got;

-- the bound under the groups the table holds: the statement runs again as PostgreSQL plans it, and so does a cursor, on its first fetch
CREATE TEMP TABLE again AS EXECUTE q;
SELECT count(*) AS groups, (SELECT count(*) FROM (TABLE want EXCEPT TABLE again) d) AS differ FROM again;
DO $$
DECLARE n bigint;
BEGIN
	EXECUTE 'DECLARE c CURSOR FOR SELECT k, string_agg(v::text, '','' ORDER BY v) AS b, count(DISTINCT w) AS c FROM events GROUP BY k';
	EXECUTE 'FETCH 10 FROM c';
	GET DIAGNOSTICS n = ROW_COUNT;
	RAISE WARNING 'fetched % rows', n;
END $$;

DROP TABLE events;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s1 CASCADE;
RESET client_min_messages;
-- <<< agg_group_nospill_count

-- >>> agg_group_order
CREATE SCHEMA s2;
GRANT USAGE ON SCHEMA s2 TO PUBLIC;
SET search_path = s2, public;
-- A scan under an ORDER BY or a window emits its groups in the order the parent sorts by, so that no Sort is put above it: the distinct values of each key are
-- sorted once with the comparator of the pathkey (its sort operator, collation and nulls order) and the groups by the ranks. The rows are the normal plan's in the same
-- order (the key columns, row by row), for text, numeric, date, float and jsonb keys, DESC, NULLS FIRST and LAST, a prefix of the group keys, and with workers.

SET seq_page_cost = 10000;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET extra_float_digits = 0;

CREATE TABLE o (id int, a text, b numeric(8,2), c date, e int, f float8, x float8) WITH (fillfactor = 20, autovacuum_enabled = false);
INSERT INTO o
SELECT i, CASE WHEN i % 17 = 0 THEN NULL ELSE 'k' || (i * 7 % 301) END, (i % 40) / 4.0, DATE '2024-01-01' + (i % 30), CASE WHEN i % 11 = 0 THEN NULL ELSE i % 6 END,
       CASE i % 9 WHEN 0 THEN 0.0 WHEN 1 THEN -0.0 WHEN 2 THEN 'NaN' WHEN 3 THEN NULL WHEN 4 THEN 'Infinity' WHEN 5 THEN '-Infinity' ELSE i % 5 - 2.5 END, i * 0.1
FROM generate_series(1, 30000) i;
CREATE INDEX o_col ON o USING sift_col (id, a, b, c, e, f, x);
VACUUM ANALYZE o;

-- jsonb keys (a dimension built from columns): many distinct values, so the ranks come from the byte keys (gorder.c) and the check of the comparator; numbers that are one double
-- (9007199254740992 and 9007199254740993), integers and fractions, strings of both cases, with accents and empty, true, false and null elements, nested arrays and objects
CREATE TABLE oj (id int, a int, b text, c numeric, d bool, e int) WITH (fillfactor = 20, autovacuum_enabled = false);
INSERT INTO oj
SELECT i, CASE WHEN i % 23 = 0 THEN NULL ELSE i % 19 - 9 END,
       (ARRAY['a', 'A', 'b', 'B', 'é', 'e', '', 'z', 'Z', '10', '9', '_', 'ab', 'aB', 'Ab'])[1 + i % 15],
       (ARRAY[9007199254740992, 9007199254740993, 9007199254740994, 0, -1.5, 2.25, 1e20, -1e20, 0.1, 0.5])[1 + i % 10]::numeric,
       CASE i % 3 WHEN 0 THEN true WHEN 1 THEN false END, i % 3
FROM generate_series(1, 30000) i;
CREATE INDEX oj_col ON oj USING sift_col (id, a, b, c, d, e);
VACUUM ANALYZE oj;

-- five keys of about 12,000 values each: their ranks take more than 64 bits together, so the groups are sorted by the tuple of ranks and not by one packed integer
CREATE TABLE ow (id int, k1 int, k2 int, k3 int, k4 int, k5 int) WITH (fillfactor = 20, autovacuum_enabled = false);
INSERT INTO ow SELECT i, (i * 3) % 12007, (i * 5) % 12011, CASE WHEN i % 29 = 0 THEN NULL ELSE (i * 7) % 12013 END, (i * 11) % 12037, (i * 13) % 12041 FROM generate_series(1, 12000) i;
CREATE INDEX ow_col ON ow USING sift_col (id, k1, k2, k3, k4, k5);
VACUUM ANALYZE ow;

-- the key columns (the first k of the select list) of the statement, in the order it gives them, as a checksum
CREATE FUNCTION o_keys(q text, k int, push bool, workers int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	PERFORM set_config('max_parallel_workers_per_gather', workers::text, true);
	EXECUTE format('SELECT md5(string_agg(row(%s)::text, E''\n'')) FROM (%s) q', CASE WHEN k = 0 THEN 'q.*' ELSE (SELECT string_agg('q.c' || i, ', ') FROM generate_series(1, k) i) END, q) INTO r;
	RETURN r;
END $$;

-- the plan has the node and no Sort above it; the plan is serial (the ordered groups are offered to a serial scan, and a parallel scan under a Sort can cost less on a few thousand groups, which depends on the workers the server allows)
CREATE FUNCTION o_plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; scan bool := false; srt bool := false;
BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		scan := scan OR l LIKE '%Custom Scan (SiftAgg%';
		srt := srt OR l LIKE '%Sort%';
	END LOOP;
	RETURN CASE WHEN NOT scan THEN 'no scan' WHEN srt THEN 'sorted above' ELSE 'ordered' END;
END $$;

CREATE FUNCTION o_ordered(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; r bool := false;
BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		r := r OR l LIKE '%order keys)%';
	END LOOP;
	RETURN r;
END $$;

CREATE TABLE o_q (n int, k int, q text, want text);
INSERT INTO o_q VALUES
	(1, 2, 'SELECT a AS c1, c AS c2, count(*) AS n FROM o GROUP BY a, c ORDER BY a DESC NULLS LAST, c', 'ordered'),
	(2, 2, 'SELECT c AS c1, a AS c2, count(*) AS n FROM o GROUP BY a, c ORDER BY c DESC, a NULLS FIRST', 'ordered'),
	(3, 1, 'SELECT b AS c1, e AS c2, count(*) AS n FROM o GROUP BY b, e ORDER BY b DESC', 'ordered'),
	(4, 3, 'SELECT b AS c1, e AS c2, c AS c3, round(sum(x)::numeric, 2) AS s FROM o GROUP BY b, e, c ORDER BY b, e DESC NULLS LAST, c DESC', 'ordered'),
	(5, 1, 'SELECT f AS c1, count(*) AS n FROM o GROUP BY f ORDER BY f', 'ordered'),
	(6, 1, 'SELECT f AS c1, count(*) AS n FROM o GROUP BY f ORDER BY f DESC NULLS FIRST', 'ordered'),
	(7, 1, 'SELECT a COLLATE "C" AS c1, count(*) AS n FROM o GROUP BY a ORDER BY a COLLATE "C" DESC', 'ordered'),
	(8, 0, 'SELECT a AS c1, c AS c2, count(*) AS n, lag(count(*)) OVER (PARTITION BY a ORDER BY c DESC) AS l FROM o GROUP BY a, c', 'ordered'),
	(9, 2, 'SELECT e AS c1, b AS c2, count(*) AS n FROM o GROUP BY e, b ORDER BY e, b, count(*)', 'sorted above'),
	-- the window is in the query level that reads the groups (an inlined CTE, a sub-select), the keys in another order than the GROUP BY
	(11, 0, 'WITH g AS (SELECT a AS c1, c AS c2, e AS c3, count(*) AS n FROM o GROUP BY a, c, e) SELECT c1, c3, c2, n, lag(n) OVER (PARTITION BY c1, c3 ORDER BY c2 DESC) AS l FROM g', 'ordered'),
	(12, 0, 'SELECT * FROM (SELECT b AS c1, c AS c2, count(*) AS n FROM o GROUP BY b, c) q ORDER BY c1 DESC NULLS FIRST, c2', 'ordered'),
	-- a column of the sub-select that is a constant, a COALESCE of a key (the NULL group sorts as the constant), keys in the window's order
	(14, 0, 'WITH g AS (SELECT COALESCE(a, ''zz'') AS c1, NULL::date AS c2, e AS c3, c AS c4, count(*) AS n FROM o GROUP BY a, e, c) SELECT c1, c2, c3, c4, n, count(*) OVER (PARTITION BY c2, c3 ORDER BY c1 DESC, c4) AS w FROM g', 'ordered'),
	-- a group key that is itself a COALESCE (of two columns, of a column and a constant, of three columns): ordered by the whole key, NULL groups and DESC NULLS FIRST included
	(40, 0, 'WITH g AS (SELECT COALESCE(a, c::text) AS c1, e AS c2, count(*) AS n FROM o GROUP BY COALESCE(a, c::text), e) SELECT c1, c2, n, lag(n) OVER (PARTITION BY c1 ORDER BY c2 DESC NULLS FIRST) AS l FROM g', 'ordered'),
	(41, 0, 'SELECT * FROM (SELECT COALESCE(b, e) AS c1, a AS c2, count(*) AS n FROM o GROUP BY COALESCE(b, e), a) q ORDER BY c1 DESC NULLS FIRST, c2', 'ordered'),
	(42, 0, 'SELECT * FROM (SELECT COALESCE(a, ''zz'') AS c1, e AS c2, count(*) AS n FROM o GROUP BY COALESCE(a, ''zz''), e) q ORDER BY c1 COLLATE "C" DESC, c2 NULLS FIRST', 'ordered'),
	(43, 0, 'SELECT * FROM (SELECT COALESCE(a, c::text, ''x'') AS c1, COALESCE(e, 7) AS c2, COALESCE(b, 0) AS c3, count(*) AS n FROM o GROUP BY COALESCE(a, c::text, ''x''), COALESCE(e, 7), COALESCE(b, 0)) q ORDER BY c1, c2 DESC, c3', 'ordered'),
	(44, 0, 'WITH g AS (SELECT COALESCE(a, c::text) AS c1, COALESCE(NULLIF(e, 3), b::int) AS c2, c AS c3, count(*) AS n FROM o GROUP BY 1, 2, 3) SELECT c1, c2, c3, n, row_number() OVER (PARTITION BY c1, c2 ORDER BY c3 DESC) AS r FROM g', 'ordered'),
	-- the parent groups with an aggregate of its own that sorts (its ORDER BY or DISTINCT): PostgreSQL sorts for it and can not hash, the scan gives that order
	(45, 0, 'WITH g AS (SELECT a AS c1, c AS c2, e AS c3, count(*) AS n FROM o GROUP BY a, c, e) SELECT c1, string_agg(c3::text || ''/'' || n, '','' ORDER BY c2 DESC, c3 NULLS FIRST) AS s FROM g GROUP BY c1', 'ordered'),
	(46, 0, 'SELECT c1, count(DISTINCT c3) AS d, array_agg(c2 ORDER BY c2) AS arr FROM (SELECT b AS c1, c AS c2, e AS c3, count(*) AS n FROM o GROUP BY b, c, e) q GROUP BY c1', 'ordered'),
	(47, 0, 'SELECT string_agg(c1 || ''/'' || c2::text, '','' ORDER BY c1 DESC NULLS LAST, c2) AS s FROM (SELECT a AS c1, c AS c2, count(*) AS n FROM o GROUP BY a, c) q', 'ordered'),
	-- many groups (30,000) with a key of as many distinct values: the values of an integer or date key are sorted by the value with no call for a compare and the groups by a radix
	-- sort, which is much less than the Sort they replace: the scan is ordered where the cost said the Sort was cheaper
	(50, 2, 'SELECT id AS c1, a AS c2, count(*) AS n FROM o GROUP BY id, a ORDER BY a, id DESC', 'ordered'),
	(51, 0, 'WITH g AS (SELECT a AS c1, id AS c2, count(*) AS n FROM o GROUP BY a, id) SELECT c1, c2, n, lag(n) OVER (PARTITION BY c1 ORDER BY c2 DESC) AS l FROM g', 'ordered'),
	(52, 3, 'SELECT c AS c1, id AS c2, a AS c3, count(*) AS n FROM o GROUP BY c, id, a ORDER BY c DESC NULLS LAST, id, a', 'ordered'),
	-- grouping sets: a key a set leaves out is NULL in its groups, and sorts as NULL (a window in the query that reads them, an ORDER BY of the parent, a COALESCE of a key)
	(20, 0, 'WITH g AS (SELECT b AS c1, c AS c2, count(*) AS n FROM o GROUP BY GROUPING SETS ((b, c), (b))) SELECT c1, c2, n, lag(n) OVER (PARTITION BY c1 ORDER BY c2 DESC) AS l FROM g', 'ordered'),
	(21, 0, 'SELECT * FROM (SELECT b AS c1, c AS c2, count(*) AS n FROM o GROUP BY ROLLUP (b, c)) q ORDER BY c1 DESC NULLS FIRST, c2', 'ordered'),
	(22, 0, 'SELECT * FROM (SELECT COALESCE(b, -1) AS c1, c AS c2, count(*) AS n FROM o GROUP BY GROUPING SETS ((b, c), (c), (b))) q ORDER BY c2 NULLS LAST, c1 DESC', 'ordered'),
	-- the ORDER BY of the query that groups by sets: the keys a set leaves out are NULL, in the position the sort puts NULL
	(24, 2, 'SELECT b AS c1, c AS c2, count(*) AS n FROM o GROUP BY ROLLUP (b, c) ORDER BY b NULLS FIRST, c NULLS FIRST', 'ordered'),
	(25, 2, 'SELECT a AS c1, e AS c2, count(*) AS n FROM o GROUP BY GROUPING SETS ((a, e), (a), (e), ()) ORDER BY a DESC NULLS LAST, e NULLS FIRST', 'ordered'),
	(26, 1, 'SELECT c AS c1, b AS c2, count(*) AS n FROM o GROUP BY GROUPING SETS ((c, b), (c), (b)) ORDER BY c DESC NULLS FIRST', 'ordered'),
	(27, 0, 'SELECT b AS c1, c AS c2, count(*) AS n, lag(count(*)) OVER (PARTITION BY b ORDER BY c DESC) AS l FROM o GROUP BY GROUPING SETS ((b, c), (b))', 'ordered'),
	-- the NULL group and the grand total tie in the ORDER BY: the check reads the key column only (the order of the two rows is the executor's, and changed in PostgreSQL 19)
	(28, 1, 'SELECT a AS c1, count(*) AS n FROM o GROUP BY ROLLUP (a) ORDER BY a NULLS FIRST', 'ordered'),
	(23, 0, 'WITH g AS (SELECT b AS c1, c AS c2, a AS c3, count(*) AS n FROM o WHERE a IS NOT NULL GROUP BY GROUPING SETS ((b, c, a), (b, c))) SELECT c1, c2, c3, n, rank() OVER (PARTITION BY c1 ORDER BY c2, c3 NULLS FIRST) AS r FROM g', 'ordered'),
	-- jsonb keys: the rank of the 3,000 distinct values, ascending, descending, nulls first, with a window on top
	(30, 0, 'SELECT jsonb_build_array(a, b, c, d) AS c1, count(*) AS n FROM oj GROUP BY jsonb_build_array(a, b, c, d) ORDER BY 1', 'ordered'),
	(31, 0, 'SELECT jsonb_build_array(a, b, c, d) AS c1, count(*) AS n FROM oj GROUP BY jsonb_build_array(a, b, c, d) ORDER BY 1 DESC', 'ordered'),
	(32, 0, 'SELECT jsonb_build_array(jsonb_build_object(''k'', c), jsonb_build_array(b, d)) AS c1, e AS c2, count(*) AS n FROM oj GROUP BY 1, 2 ORDER BY c1 DESC NULLS FIRST, c2', 'ordered'),
	(33, 0, 'WITH g AS (SELECT to_jsonb(b) AS c1, to_jsonb(c) AS c2, jsonb_build_object(''a'', a, ''d'', d) AS c3, count(*) AS n FROM oj GROUP BY to_jsonb(b), to_jsonb(c), jsonb_build_object(''a'', a, ''d'', d)) SELECT c1, c2, c3, n, lag(n) OVER (PARTITION BY c3 ORDER BY c1 DESC, c2) AS l FROM g', 'ordered'),
	-- keys whose ranks do not fit 64 bits together
	(60, 5, 'SELECT k1 AS c1, k2 AS c2, k3 AS c3, k4 AS c4, k5 AS c5, count(*) AS n FROM ow GROUP BY k1, k2, k3, k4, k5 ORDER BY k1, k2 DESC, k3 NULLS FIRST, k4, k5 DESC', 'ordered'),
	(61, 5, 'SELECT k1 AS c1, k2 AS c2, k3 AS c3, k4 AS c4, k5 AS c5, count(*) AS n FROM ow GROUP BY k1, k2, k3, k4, k5 ORDER BY k5, k4 DESC, k3, k2, k1 DESC NULLS LAST', 'ordered'),
	-- a CTE read twice or materialized is a scan of its own: the Sort stays
	(13, 0, 'WITH g AS MATERIALIZED (SELECT a AS c1, c AS c2, count(*) AS n FROM o GROUP BY a, c) SELECT c1, c2, n, lag(n) OVER (PARTITION BY c1 ORDER BY c2) AS l FROM g', 'sorted above'),
	-- an order the keys do not decide (an Incremental Sort on the prefix, or a Sort): a Sort stays above
	(10, 1, 'SELECT a AS c1, count(*) AS n FROM o GROUP BY a ORDER BY count(*) DESC, a', 'sorted above');

SELECT n, o_plan(q) = want AS plan FROM o_q ORDER BY n;

-- the scan emits its groups in an order where a Sort above it was asked for; a parent that hashes (no sorting aggregate) gets them unordered
SELECT n, o_ordered(q) AS group_order FROM o_q WHERE n IN (1, 40, 45, 46, 47) ORDER BY n;

-- rows in the normal plan's order (key columns), with no worker and with three
SELECT n, o_keys(q, k, true, 0) = o_keys(q, k, false, 0) AS serial, o_keys(q, k, true, 3) = o_keys(q, k, false, 0) AS workers FROM o_q WHERE want = 'ordered' ORDER BY n;

-- the groups are the same rows
CREATE FUNCTION o_rows(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	EXECUTE format('SELECT md5(string_agg(q::text, E''\n'' ORDER BY q::text)) FROM (%s) q', q) INTO r;
	RETURN r;
END $$;
SELECT n, o_rows(q, true) = o_rows(q, false) AS rows FROM o_q ORDER BY n;

-- a parent that hashes gets the groups unordered, and the same rows
SELECT o_ordered('SELECT c1, sum(n) AS s FROM (SELECT a AS c1, c AS c2, count(*) AS n FROM o GROUP BY a, c) q GROUP BY c1') AS hashing_parent_ordered,
       o_rows('SELECT c1, sum(n) AS s FROM (SELECT a AS c1, c AS c2, count(*) AS n FROM o GROUP BY a, c) q GROUP BY c1', true) = o_rows('SELECT c1, sum(n) AS s FROM (SELECT a AS c1, c AS c2, count(*) AS n FROM o GROUP BY a, c) q GROUP BY c1', false) AS rows;


-- adversarial keys: strings that sort differently by collation (case, accents, punctuation, digits, the empty string), numeric with NaN, floats with -0, NaN and infinities,
-- dates and timestamps with infinity, booleans, negative bigints, NULLs everywhere; every key, both directions, both nulls orders, alone and with a second key that breaks the ties
CREATE TABLE o2 (id int, s text, n numeric, f float8, d date, t timestamptz, b bool, i8 bigint, g int) WITH (fillfactor = 20, autovacuum_enabled = false);
INSERT INTO o2
SELECT i, (ARRAY['a', 'A', 'b', 'B', 'a1', 'a10', 'a2', '_x', ' ', '', 'é', 'e', 'z', 'Z', '1', '10', '2'])[1 + i % 17] || CASE WHEN i % 5 = 0 THEN '' ELSE '' END,
       (ARRAY['0', '-1', '1.5', '1e20', 'NaN', '-0.001', '100', '7', '-1e10'])[1 + i % 9]::numeric,
       (ARRAY[0.0, -0.0, 1.5, -2.5, 'NaN', 'Infinity', '-Infinity', 3.0, 1e300, -1e300])[1 + i % 10],
       (ARRAY[DATE '2024-01-01', DATE '2023-12-31', 'infinity', '-infinity', DATE '1999-06-15', DATE '2024-02-29'])[1 + i % 6],
       (ARRAY[TIMESTAMPTZ '2024-01-01 00:00:00+00', 'infinity', '-infinity', TIMESTAMPTZ '2000-03-04 05:06:07+00', TIMESTAMPTZ '1970-01-01 00:00:00+00'])[1 + i % 5],
       (ARRAY[true, false])[1 + i % 2],
       (ARRAY[0, -1, 9223372036854775807, -9223372036854775807, 42, -42, 1000000007])[1 + i % 7]::bigint,
       i % 4
FROM generate_series(1, 30000) i;
UPDATE o2 SET s = NULL WHERE id % 31 = 0;
UPDATE o2 SET n = NULL WHERE id % 29 = 0;
UPDATE o2 SET f = NULL WHERE id % 23 = 0;
UPDATE o2 SET d = NULL WHERE id % 19 = 0;
UPDATE o2 SET t = NULL WHERE id % 37 = 0;
UPDATE o2 SET b = NULL WHERE id % 41 = 0;
UPDATE o2 SET i8 = NULL WHERE id % 43 = 0;
-- no dead version is left for the index build (a session of another test with an older snapshot makes the planner leave the index out: indcheckxmin)
CREATE TEMP TABLE o2_rows AS SELECT * FROM o2 ORDER BY ctid;
TRUNCATE o2;
INSERT INTO o2 SELECT * FROM o2_rows;
DROP TABLE o2_rows;
CREATE INDEX o2_col ON o2 USING sift_col (id, s, n, f, d, t, b, i8, g);
VACUUM ANALYZE o2;

-- a collation that is not the byte order, when the server has one
SELECT coalesce((SELECT collname FROM pg_collation WHERE collname IN ('en_US.utf8', 'und-x-icu') ORDER BY collname LIMIT 1), 'C') AS coll \gset

CREATE FUNCTION o2_check(col text, second text, dir text, nulls text, coll text) RETURNS TABLE (plan text, order_same bool, rows_same bool) LANGUAGE plpgsql AS $$
DECLARE q text; a text; b text;
BEGIN
	q := format('SELECT %1$s %5$s AS c1, %2$s AS c2, count(*) AS cnt FROM o2 GROUP BY %1$s, %2$s ORDER BY %1$s %5$s %3$s NULLS %4$s, %2$s DESC NULLS FIRST',
				col, second, dir, nulls, CASE WHEN coll = '' THEN '' ELSE 'COLLATE "' || coll || '"' END);
	plan := o_plan(q);
	order_same := o_keys(q, 2, true, 0) = o_keys(q, 2, false, 0) AND o_keys(q, 2, true, 3) = o_keys(q, 2, false, 0);
	rows_same := o_rows(q, true) = o_rows(q, false);
	RETURN NEXT;
END $$;

SELECT col, second, dir, nulls, (o2_check(col, second, dir, nulls, '')).*
FROM unnest(ARRAY['s', 'n', 'f', 'd', 't', 'b', 'i8']) col, unnest(ARRAY['g', 'b']) second, unnest(ARRAY['ASC', 'DESC']) dir, unnest(ARRAY['FIRST', 'LAST']) nulls
WHERE col <> second ORDER BY 1, 2, 3, 4;

-- text under the server's non-byte collation, and under C, a prefix of the keys (ties left to the second key's order only when it is in the ORDER BY)
SELECT dir, nulls, (o2_check('s', 'g', dir, nulls, :'coll')).*, (o2_check('s', 'g', dir, nulls, 'C')).rows_same AS c_rows
FROM unnest(ARRAY['ASC', 'DESC']) dir, unnest(ARRAY['FIRST', 'LAST']) nulls ORDER BY 1, 2;

-- values that compare equal but print differently (numeric 1.0 and 1.00, float -0 and 0, text equal under a nondeterministic collation): PostgreSQL merges them in one group,
-- so must the ordered scan, in the place the sort puts them. Which of the equal spellings a group prints is arbitrary in PostgreSQL itself (its group key is the value of
-- the first row of the group that it met, and the plans meet rows in another order), so the groups are compared by a canonical form of the key and their count
DO $$ BEGIN
	CREATE COLLATION o3_ci (provider = icu, locale = 'und-u-ks-level2', deterministic = false);
EXCEPTION WHEN OTHERS THEN
	CREATE COLLATION o3_ci FROM "C";
END $$;
CREATE TABLE o3 (id int, n numeric, f float8, s text COLLATE o3_ci, g int) WITH (fillfactor = 20, autovacuum_enabled = false);
INSERT INTO o3
SELECT i, (ARRAY['1.0', '1.00', '2', '2.000', '0', '0.0', '-0.0', '3.10', '3.1'])[1 + i % 9]::numeric, (ARRAY[0.0, -0.0, 1.5, -1.5])[1 + i % 4],
       (ARRAY['a', 'A', 'b', 'B', 'á', 'Á', 'c'])[1 + i % 7], i % 3
FROM generate_series(1, 20000) i;
UPDATE o3 SET n = NULL WHERE id % 13 = 0;
-- no dead version is left for the index build (a session of another test with an older snapshot makes the planner leave the index out: indcheckxmin)
CREATE TEMP TABLE o3_rows AS SELECT * FROM o3 ORDER BY ctid;
TRUNCATE o3;
INSERT INTO o3 SELECT * FROM o3_rows;
DROP TABLE o3_rows;
CREATE INDEX o3_col ON o3 USING sift_col (id, n, f, s, g);
VACUUM ANALYZE o3;

CREATE FUNCTION o3_check(col text, canon text, dir text) RETURNS TABLE (plan text, same_groups bool) LANGUAGE plpgsql AS $$
DECLARE q text; a text; b text;
BEGIN
	q := format('SELECT %1$s AS c1, count(*) AS c2 FROM o3 GROUP BY %1$s ORDER BY %1$s %2$s', col, dir);
	plan := o_plan(q);
	same_groups := true;
	FOR w IN 0..3 LOOP
		PERFORM set_config('max_parallel_workers_per_gather', w::text, true);
		PERFORM set_config('sift.enable_agg', 'true', true);
		EXECUTE format('SELECT md5(string_agg(coalesce((%s)::text, ''null'') || '':'' || c2, '','')) FROM (%s) q', replace(canon, 'c1', 'q.c1'), q) INTO a;
		PERFORM set_config('sift.enable_agg', 'false', true);
		EXECUTE format('SELECT md5(string_agg(coalesce((%s)::text, ''null'') || '':'' || c2, '','')) FROM (%s) q', replace(canon, 'c1', 'q.c1'), q) INTO b;
		same_groups := same_groups AND a = b;
	END LOOP;
	RETURN NEXT;
END $$;

SELECT col, dir, (o3_check(col, canon, dir)).*
FROM (VALUES ('n', 'c1::float8'), ('f', 'c1 + 0'), ('s', 'translate(c1 COLLATE "C", ''ABÁ'', ''abá'')')) k(col, canon), unnest(ARRAY['ASC', 'DESC']) dir ORDER BY 1, 2;

DROP TABLE o_q, o, o2, o3, oj, ow;
DROP COLLATION o3_ci;
DROP FUNCTION o_keys(text, int, bool, int), o_plan(text), o_ordered(text), o_rows(text, bool), o2_check(text, text, text, text, text), o3_check(text, text, text);
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s2 CASCADE;
RESET client_min_messages;
-- <<< agg_group_order

-- >>> agg_group_order_semi
CREATE SCHEMA s3;
GRANT USAGE ON SCHEMA s3 TO PUBLIC;
SET search_path = s3, public;
-- The ordered output (the groups emitted in the order the parent sorts by) of a scan that filters by a semi or anti join: the paths of the node keep the order
-- they promise until the plan is made, after the planner has gone on to other work, so the array that describes it must live as long as the paths. The rows are the
-- normal plan's, in the same order, for IN, NOT IN, EXISTS and NOT EXISTS, an ORDER BY of the same level and a window or an ORDER BY of the level above.
SET client_min_messages = warning;

SET seq_page_cost = 10000;
SET random_page_cost = 10000;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE ev (id int PRIMARY KEY, dim_a_id int, day date, kind text, amount numeric(10, 2)) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, CASE WHEN i % 997 = 0 THEN NULL ELSE 1 + (i::bigint * 7919) % 3000 END, date '2024-01-01' + i % 90, (ARRAY['a', 'b', 'c', 'd'])[1 + i % 4], (i % 1000) / 10.0
FROM generate_series(1, 30000) i;
CREATE INDEX ev_col ON ev USING sift_col (dim_a_id, day, kind, amount);
CREATE TABLE dim_b (group_id int, dim_a_id int);
INSERT INTO dim_b SELECT g, 1 + (g * 1000 + i * 37) % 3000 FROM generate_series(1, 4) g, generate_series(1, 300 * g) i;
CREATE TABLE holidays (d date);
INSERT INTO holidays SELECT date '2024-01-01' + i * 7 FROM generate_series(0, 12) i;
VACUUM (FREEZE, ANALYZE) ev; ANALYZE dim_b; ANALYZE holidays;

CREATE FUNCTION keys_of(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	EXECUTE format('SELECT md5(string_agg(row(q.*)::text, E''\n'')) FROM (%s) q', q) INTO r;
	RETURN r;
END $$;
-- the node with the groups ordered (the semi lookup is in the plan) and no Sort above it
CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; scan bool := false; srt bool := false;
BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		scan := scan OR l LIKE '%Custom Scan (SiftAgg%';
		srt := srt OR l LIKE '%Sort%';
	END LOOP;
	RETURN CASE WHEN NOT scan THEN 'no scan' WHEN srt THEN 'sorted above' ELSE 'ordered' END;
END $$;

CREATE TABLE q (n int, q text);
INSERT INTO q VALUES
	(1, 'SELECT kind, day, count(*) AS n FROM ev WHERE dim_a_id IN (SELECT dim_a_id FROM dim_b WHERE group_id = 2) GROUP BY kind, day ORDER BY day DESC, kind'),
	(2, 'SELECT kind, day, count(*) AS n FROM ev WHERE dim_a_id NOT IN (SELECT dim_a_id FROM dim_b WHERE group_id = 3 AND dim_a_id IS NOT NULL) GROUP BY kind, day ORDER BY kind DESC, day'),
	(3, 'SELECT kind, day, sum(amount) AS s FROM ev e WHERE EXISTS (SELECT 1 FROM dim_b m WHERE m.dim_a_id = e.dim_a_id AND m.group_id = 1) GROUP BY kind, day ORDER BY day, kind DESC'),
	(4, 'SELECT kind, day, count(*) AS n FROM ev e WHERE NOT EXISTS (SELECT 1 FROM dim_b m WHERE m.dim_a_id = e.dim_a_id AND m.group_id = 4) GROUP BY kind, day ORDER BY kind, day'),
	(5, 'SELECT day, kind, count(*) AS n FROM ev WHERE day IN (SELECT d FROM holidays) GROUP BY day, kind ORDER BY day DESC, kind'),
	(6, 'SELECT * FROM (SELECT kind, day, count(*) AS n FROM ev WHERE dim_a_id IN (SELECT dim_a_id FROM dim_b WHERE group_id = 2) GROUP BY kind, day) g ORDER BY day DESC, kind'),
	(7, 'WITH g AS (SELECT kind, day, count(*) AS n FROM ev WHERE dim_a_id IN (SELECT dim_a_id FROM dim_b WHERE group_id = 4) GROUP BY kind, day) SELECT kind, day, n, lag(n) OVER (PARTITION BY kind ORDER BY day DESC) AS l FROM g'),
	(8, 'WITH g AS (SELECT kind, day, count(*) AS n FROM ev e WHERE NOT EXISTS (SELECT 1 FROM dim_b m WHERE m.dim_a_id = e.dim_a_id AND m.group_id = 2) GROUP BY kind, day) SELECT kind, day, n, sum(n) OVER (PARTITION BY day ORDER BY kind DESC) AS w FROM g');

SELECT n, keys_of(q, true) = keys_of(q, false) AS same_rows, plan_of(q) AS plan FROM q ORDER BY n;

DROP FUNCTION keys_of(text, bool), plan_of(text);
DROP TABLE q, ev, dim_b, holidays;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s3 CASCADE;
RESET client_min_messages;
-- <<< agg_group_order_semi

-- >>> agg_group_order_sort
CREATE SCHEMA s4;
GRANT USAGE ON SCHEMA s4 TO PUBLIC;
SET search_path = s4, public;
-- A Sort above a window that the scan below it can do without. The scan under a window emits its groups in the window's order; an ORDER BY of the
-- sub-select that reads the window (the rows of one period, the flag of one category first) is another order, and PostgreSQL puts a Sort there that the plan
-- no longer needs once the scan emits the Sort's keys, then the rest of the window's partition, then the window's own keys. The rows are the normal plan's, in
-- the same order, for plain keys, a flag (key = constant), a COALESCE of a key, a jsonb key, DESC and NULLS FIRST; and the Sort stays where the keys are not
-- the window's partition (a key of the window's order, an expression of two columns).

SET seq_page_cost = 10000;

CREATE TABLE s (id int, a text, e int, d date, j int, x float8) WITH (fillfactor = 20, autovacuum_enabled = false);
INSERT INTO s
SELECT i, CASE WHEN i % 17 = 0 THEN NULL ELSE 'k' || (i * 7 % 11) END, CASE WHEN i % 13 = 0 THEN NULL ELSE i % 9 END, DATE '2024-01-01' + (i % 12), i % 5, i * 0.5
FROM generate_series(1, 20000) i;
CREATE INDEX s_col ON s USING sift_col (id, a, e, d, j, x);
VACUUM ANALYZE s;

CREATE FUNCTION s_keys(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	EXECUTE format('SELECT md5(string_agg(row(q.cat, q.ent, q.day, q.n, q.prev)::text, E''\n'')) FROM (%s) q', q) INTO r;
	RETURN r;
END $$;

-- the plan has the scan; the Sorts it has are counted
CREATE FUNCTION s_sorts(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; scan bool := false; n int := 0;
BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		scan := scan OR l LIKE '%Custom Scan (SiftAgg%';
		IF l LIKE '%Sort Key%' THEN n := n + 1; END IF;
	END LOOP;
	RETURN CASE WHEN scan THEN n ELSE -1 END;
END $$;

CREATE TABLE s_q (n int, q text, sorts int);
-- g: the groups; w: the window over them (the previous day of the same category and entity); the filter keeps the later days after the window
INSERT INTO s_q
SELECT n, 'WITH g AS (SELECT COALESCE(a, ''zz'') AS cat, e AS ent, d AS day, count(*) AS n FROM s GROUP BY a, e, d),
	w AS (SELECT g.*, lag(n) OVER (PARTITION BY ent, cat ORDER BY day DESC) AS prev FROM g)
	SELECT * FROM w WHERE day = DATE ''2024-01-04'' ORDER BY ' || ord, sorts
FROM (VALUES
	(1, '(cat = ''k3'') DESC, ent, cat', 0),
	(2, 'ent DESC NULLS LAST, cat', 0),
	(3, 'cat, ent NULLS FIRST', 0),
	(4, '(cat = ''zz'') DESC, (ent = 4) DESC NULLS FIRST, cat DESC, ent', 0),
	(5, 'cat DESC, ent', 0),
	-- the window's own key, an expression of two columns, or an aggregate: the Sort stays
	(6, 'ent, cat, prev', 1),
	(7, 'cat, ent, n DESC', 1),
	(8, 'cat || ent::text', 1)
) v(n, ord, sorts);

SELECT n, s_sorts(q) = sorts AS sorts_as_expected FROM s_q ORDER BY n;

-- the rows, in the normal plan's order
SELECT n, s_keys(q, true) = s_keys(q, false) AS same_order FROM s_q ORDER BY n;

-- no window: the scan is not ordered for anything, and the Sort stays; a window whose own order the Sort asks for (the scan serves it already), or asks for first
CREATE TABLE s_q2 (n int, q text, sorts int);
INSERT INTO s_q2 VALUES
	(1, 'WITH g AS (SELECT COALESCE(a, ''zz'') AS cat, e AS ent, d AS day, count(*) AS n, NULL::bigint AS prev FROM s GROUP BY a, e, d)
		SELECT * FROM g WHERE n > 3 ORDER BY (cat = ''k3'') DESC, day DESC, ent, cat', 1),
	(2, 'WITH g AS (SELECT COALESCE(a, ''zz'') AS cat, e AS ent, d AS day, count(*) AS n FROM s GROUP BY a, e, d),
		w AS (SELECT g.*, lag(n) OVER (PARTITION BY ent, cat ORDER BY day) AS prev FROM g) SELECT * FROM w ORDER BY ent, cat, day', 0),
	(3, 'WITH g AS (SELECT COALESCE(a, ''zz'') AS cat, e AS ent, d AS day, count(*) AS n FROM s GROUP BY a, e, d),
		w AS (SELECT g.*, lag(n) OVER (PARTITION BY ent, cat ORDER BY day) AS prev FROM g) SELECT * FROM w ORDER BY day, ent, cat', 1);

SELECT n, s_sorts(q) = sorts AS sorts_as_expected, s_keys(q, true) = s_keys(q, false) AS same_order FROM s_q2 ORDER BY n;

DROP TABLE s_q, s_q2, s;
DROP FUNCTION s_keys(text, bool), s_sorts(text);
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s4 CASCADE;
RESET client_min_messages;
-- <<< agg_group_order_sort

-- >>> agg_group_spill_order
CREATE SCHEMA s5;
GRANT USAGE ON SCHEMA s5 TO PUBLIC;
SET search_path = s5, public;
-- A scan that outgrows hash memory and gives its groups in the order of the ORDER BY: the partitions of its spill are ranges of the first key, merged in the
-- order of the result and each sorted by itself, so no Sort follows the scan (a spilled scan gave its groups by partition, and the plan sorted them). The first key is
-- an integer, a date or a timestamp, ascending or descending, NULLs first or last, with a value that many groups share (a range of one value cannot be cut) and
-- NULL keys. The partitions are equal widths of the range the zones of the leaf give for the key, or cuts of a stream by a sample when they give none. work_mem
-- 64kB with hash_mem_multiplier 1 puts the limit at the smallest the settings allow.
SET max_parallel_workers_per_gather = 0;
SET statement_timeout = '60s';
SET client_min_messages = warning;

CREATE TABLE ev (id int, ts timestamp, d date, k int, v int);
-- 60,000 groups of (ts, k): 20,000 minutes, 3 values of k, 1,000 rows without a timestamp, and one minute shared by 10,000 groups
INSERT INTO ev
SELECT i, CASE WHEN i % 60 = 0 THEN NULL WHEN i > 50000 THEN timestamp '2025-03-01' ELSE timestamp '2025-01-01' + (i / 3 % 20000) * interval '1 minute' END,
       date '2024-01-01' + i % 400, CASE WHEN i > 50000 THEN i ELSE i % 3 END, i % 1000
FROM generate_series(1, 60000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, ts, d, k, v);
VACUUM (FREEZE, ANALYZE) ev;

-- the query with the memory given: is there a Sort above the scan, did the scan spill, are the rows the normal plan's, in the same order
CREATE FUNCTION order_case(cols text, keys text, ord text, filt text DEFAULT '') RETURNS TABLE (shape text, sorted boolean, spilled boolean, ordered_by_scan boolean, same boolean) LANGUAGE plpgsql AS $$
DECLARE q text; l text; srt boolean := false; sp boolean := false; ok boolean := false; a text; b text;
BEGIN
	q := format('SELECT %s, count(*) AS c, sum(v) AS s FROM ev %s GROUP BY %s ORDER BY %s', cols, filt, keys, ord);
	PERFORM set_config('work_mem', '64kB', false), set_config('hash_mem_multiplier', '1', false), set_config('enable_hashagg', 'off', false), set_config('enable_sort', 'off', false);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		srt := srt OR l ~ '^\s*(->\s+)?Sort';
		sp := sp OR l ~ '[1-9][0-9]* group table spills';
		ok := ok OR l ~ '\([0-9]+ order keys';
	END LOOP;
	EXECUTE format('SELECT md5(string_agg(r::text, ''|'')) FROM (%s) r', q) INTO a;
	RESET work_mem; RESET hash_mem_multiplier; RESET enable_hashagg; RESET enable_sort;
	SET sift.enable_agg = off;
	EXECUTE format('SELECT md5(string_agg(r::text, ''|'')) FROM (%s) r', q) INTO b;
	RESET sift.enable_agg;
	RETURN QUERY SELECT ord, srt, sp, ok, a = b;
END $$;

SELECT * FROM order_case('ts, k', 'ts, k', 'ts, k');
SELECT * FROM order_case('ts, k', 'ts, k', 'ts DESC, k');
SELECT * FROM order_case('ts, k', 'ts, k', 'ts DESC NULLS LAST, k DESC');
SELECT * FROM order_case('ts, k', 'ts, k', 'ts NULLS FIRST, k');
SELECT * FROM order_case('d, k, ts', 'd, k, ts', 'd DESC, k, ts');
SELECT * FROM order_case('k, ts', 'k, ts', 'k DESC, ts');
SELECT * FROM order_case('date_trunc(''hour'', ts) AS h, k, ts', 'date_trunc(''hour'', ts), k, ts', 'h DESC, k, ts');
-- a first key that is not a step of a leaf of its type (a timestamp cast to a date): the zones do not give its range, and the stream is cut by a sample
SELECT * FROM order_case('ts::date AS dd, k, ts', 'ts::date, k, ts', 'dd DESC, k, ts');

-- a filter that leaves a tenth of the range the zones give: the ranges that hold the groups do not fit and are cut again by the sample
SELECT * FROM order_case('ts, k', 'ts, k', 'ts DESC, k', 'WHERE ts < timestamp ''2025-01-01 12:00''');

DROP FUNCTION order_case(text, text, text, text);
DROP TABLE ev;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s5 CASCADE;
RESET client_min_messages;
-- <<< agg_group_spill_order

DROP EXTENSION pg_sift;
