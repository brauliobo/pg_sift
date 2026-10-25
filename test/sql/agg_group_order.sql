-- A scan under an ORDER BY or a window emits its groups in the order the parent sorts by, so that no Sort is put above it: the distinct values of each key are
-- sorted once with the comparator of the pathkey (its sort operator, collation and nulls order) and the groups by the ranks. The rows are the normal plan's in the same
-- order (the key columns, row by row), for text, numeric, date, float and jsonb keys, DESC, NULLS FIRST and LAST, a prefix of the group keys, and with workers.
CREATE EXTENSION pg_sift;

SET seq_page_cost = 10000;
SET sift.agg_preagg = off;
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

-- the key columns (the first k of the select list) of the statement, in the order it gives them, as a checksum
CREATE FUNCTION o_keys(q text, k int, push bool, workers int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	PERFORM set_config('max_parallel_workers_per_gather', workers::text, true);
	EXECUTE format('SELECT md5(string_agg(row(%s)::text, E''\n'')) FROM (%s) q', CASE WHEN k = 0 THEN 'q.*' ELSE (SELECT string_agg('q.c' || i, ', ') FROM generate_series(1, k) i) END, q) INTO r;
	RETURN r;
END $$;

-- the plan has the node and no Sort above it
CREATE FUNCTION o_plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; scan bool := false; srt bool := false;
BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		scan := scan OR l LIKE '%Custom Scan (SiftAgg)%';
		srt := srt OR l LIKE '%Sort%';
	END LOOP;
	RETURN CASE WHEN NOT scan THEN 'no scan' WHEN srt THEN 'sorted above' ELSE 'ordered' END;
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
	-- a CTE read twice or materialized is a scan of its own: the Sort stays
	(13, 0, 'WITH g AS MATERIALIZED (SELECT a AS c1, c AS c2, count(*) AS n FROM o GROUP BY a, c) SELECT c1, c2, n, lag(n) OVER (PARTITION BY c1 ORDER BY c2) AS l FROM g', 'sorted above'),
	-- an order the keys do not decide (an Incremental Sort on the prefix, or a Sort): a Sort stays above
	(10, 1, 'SELECT a AS c1, count(*) AS n FROM o GROUP BY a ORDER BY count(*) DESC, a', 'sorted above');

SELECT n, o_plan(q) = want AS plan FROM o_q ORDER BY n;

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

DROP TABLE o_q, o, o2, o3;
DROP COLLATION o3_ci;
DROP FUNCTION o_keys(text, int, bool, int), o_plan(text), o_rows(text, bool), o2_check(text, text, text, text, text), o3_check(text, text, text);
DROP EXTENSION pg_sift;
