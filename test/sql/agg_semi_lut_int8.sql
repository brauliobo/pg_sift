-- The byte table of a semi, anti or inner join by an integer key (agg_semi_lut) is also made for a key that is an int8 leaf of the rows, over the span of the
-- keys of the joined table (up to sift.agg_semi_lut_size), where the join was answered by a hash probe of every tuple. Each statement: the answer of the scan
-- against the normal plan's and against the generic join (sift.agg_semi_lut = off), and whether the byte table was used.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;
SET sift.agg_probe_rewrite = off;
SET sift.enable_col_scan = off;

-- the same keys as an int4 leaf and as an int8 leaf, a key beyond int32 in the int8 one, NULLs
CREATE TABLE ev (id int PRIMARY KEY, k4 int, k8 int8, kind text, amount numeric(10, 2)) WITH (autovacuum_enabled = false);
INSERT INTO ev
SELECT i, 1 + (i::bigint * 7919) % 30000,
       CASE WHEN i % 997 = 0 THEN NULL WHEN i <= 5 THEN -i::int8 * 10000000000 WHEN i <= 10 THEN 9000000000 + i
            WHEN i <= 15 THEN 9223372036854775807 - i ELSE 1 + (i::bigint * 7919) % 30000 END,
       (ARRAY['a', 'b', 'c', 'd'])[1 + i % 4], (i % 1000) / 10.0
FROM generate_series(1, 200000) i;
CREATE INDEX ev_col ON ev USING sift_col (k4, k8, kind, amount);
ALTER TABLE ev ALTER COLUMN k8 SET STATISTICS 1000;
VACUUM (FREEZE, ANALYZE) ev;

-- sets of keys: int8 keys (a duplicate, a NULL, keys beyond int32 and at the ends of int8), an int4 table, an int2 table, a table with no rows
CREATE TABLE keys8 (grp int, k int8);
INSERT INTO keys8 SELECT 1, 1 + i * 13 FROM generate_series(0, 2000) i;
INSERT INTO keys8 VALUES (1, 11), (1, 11), (1, NULL), (2, -50000000000), (2, 9000000007), (2, 9000000008), (2, 9223372036854775800), (2, 5), (2, 6);
INSERT INTO keys8 SELECT 3, 3000000000 + i * 5 FROM generate_series(0, 50) i;
INSERT INTO keys8 VALUES (4, -9223372036854775808), (4, 9223372036854775807), (4, 0);
CREATE TABLE keys8_nn (k int8 NOT NULL);
INSERT INTO keys8_nn SELECT 1 + i * 17 FROM generate_series(0, 1500) i;
CREATE TABLE dims (id int PRIMARY KEY, country int);
INSERT INTO dims SELECT i, i % 60 FROM generate_series(1, 30000) i;
CREATE TABLE dims8 (id int8 PRIMARY KEY, country int);
INSERT INTO dims8 SELECT i, i % 60 FROM generate_series(1, 30000) i;
CREATE TABLE nothing (k int8);
ANALYZE keys8; ANALYZE keys8_nn; ANALYZE dims; ANALYZE dims8; ANALYZE nothing;

CREATE FUNCTION run(q text, push bool) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.rows_agg(q, push) $$;
CREATE FUNCTION generic(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.agg_semi_lut', 'off', true);
	r := sift_helpers.rows_agg(q, true);
	PERFORM set_config('sift.agg_semi_lut', 'on', true);
	RETURN r;
END $$;
CREATE FUNCTION tables_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; n int := 0; scan bool := false;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN scan := true; END IF;
		IF l ~ '(\d+) semi tables' THEN n := substring(l from '(\d+) semi tables')::int; END IF;
	END LOOP;
	RETURN CASE WHEN NOT scan THEN 'normal plan' ELSE n || ' byte table' END;
END $$;

CREATE TABLE q (n int PRIMARY KEY, shape text, sql text);
INSERT INTO q VALUES
	(1, 'IN int8 set, a duplicate and a NULL', 'SELECT kind, count(*), sum(amount) FROM ev WHERE k8 IN (SELECT k FROM keys8 WHERE grp = 1) GROUP BY kind'),
	(2, 'EXISTS int8 set', 'SELECT kind, count(*), sum(amount) FROM ev WHERE EXISTS (SELECT 1 FROM keys8 m WHERE m.k = ev.k8 AND m.grp = 1) GROUP BY kind'),
	(3, 'NOT EXISTS int8 set', 'SELECT kind, count(*), sum(amount) FROM ev WHERE NOT EXISTS (SELECT 1 FROM keys8 m WHERE m.k = ev.k8 AND m.grp = 1) GROUP BY kind'),
	(4, 'NOT EXISTS, NULL leaf counted', 'SELECT count(*), count(k8) FROM ev WHERE NOT EXISTS (SELECT 1 FROM keys8 m WHERE m.k = ev.k8 AND m.grp = 2)'),
	(5, 'IN keys beyond int32, span 9e9', 'SELECT count(*) FROM ev WHERE k8 IN (SELECT k FROM keys8 WHERE grp = 2 AND k > 0 AND k < 9000000100)'),
	(6, 'NOT EXISTS keys beyond int32', 'SELECT count(*), sum(amount) FROM ev WHERE NOT EXISTS (SELECT 1 FROM keys8 m WHERE m.k = ev.k8 AND m.grp = 3)'),
	(7, 'IN a window beyond int32', 'SELECT count(*) FROM ev WHERE k8 IN (SELECT k FROM keys8 WHERE grp = 3)'),
	(8, 'IN the ends of int8, a span past the table', 'SELECT count(*) FROM ev WHERE k8 IN (SELECT k FROM keys8 WHERE grp = 4)'),
	(9, 'NOT EXISTS the ends of int8', 'SELECT count(*) FROM ev WHERE NOT EXISTS (SELECT 1 FROM keys8 m WHERE m.k = ev.k8 AND m.grp = 4)'),
	(10, 'IN the whole table, a span past the table', 'SELECT count(*) FROM ev WHERE k8 IN (SELECT k FROM keys8)'),
	(11, 'NOT IN a column without NULL', 'SELECT count(*), count(k8) FROM ev WHERE k8 NOT IN (SELECT k FROM keys8_nn)'),
	(12, 'IN int4 table, int8 leaf', 'SELECT kind, count(*) FROM ev WHERE k8 IN (SELECT id FROM dims WHERE country = 7) GROUP BY kind'),
	(13, 'NOT EXISTS int4 table, int8 leaf', 'SELECT kind, count(*) FROM ev WHERE NOT EXISTS (SELECT 1 FROM dims d WHERE d.id = ev.k8 AND d.country < 30) GROUP BY kind'),
	(14, 'IN int8 table, int4 leaf', 'SELECT kind, count(*) FROM ev WHERE k4 IN (SELECT id FROM dims8 WHERE country = 7) GROUP BY kind'),
	(15, 'inner join, int8 unique key, no column', 'SELECT kind, count(*) FROM ev JOIN dims8 d ON d.id = ev.k8 WHERE d.country < 20 GROUP BY kind'),
	(16, 'IN an empty table', 'SELECT count(*) FROM ev WHERE k8 IN (SELECT k FROM nothing)'),
	(17, 'NOT EXISTS an empty table', 'SELECT count(*) FROM ev WHERE NOT EXISTS (SELECT 1 FROM nothing x WHERE x.k = ev.k8)'),
	(19, 'inner join, unique wide keys, no column', 'SELECT count(*) FROM ev JOIN keys8 m ON m.k = ev.k8 AND m.grp = 2'),
	(20, 'inner join, a repeated key', 'SELECT count(*) FROM ev JOIN keys8 m ON m.k = ev.k8 AND m.grp = 1'),
	(18, 'two joins and a filter of the leaves', 'SELECT kind, count(*) FROM ev WHERE amount > 20 AND k8 IN (SELECT id FROM dims8 WHERE country < 40) AND NOT EXISTS (SELECT 1 FROM keys8_nn x WHERE x.k = ev.k8) GROUP BY kind');

-- each statement twice in a row (the second reads the table the first kept)
CREATE FUNCTION check_all(which text) RETURNS TABLE (stage text, n int, shape text, vs_normal bool, vs_generic bool, again bool, tables text) LANGUAGE plpgsql AS $$
BEGIN
	RETURN QUERY SELECT check_all.which, q.n, q.shape, run(q.sql, true) = run(q.sql, false), run(q.sql, true) = generic(q.sql), run(q.sql, true) = run(q.sql, false), tables_of(q.sql) FROM q ORDER BY q.n;
END $$;
-- with the scan's key filters (a semi join the filter has tested needs no table) and without them
SELECT * FROM check_all('key filters');
SET sift.agg_join_filter = off;
SELECT * FROM check_all('no key filters');

-- a table kept by a statement on an int4 leaf is not the table of an int8 leaf (it drops the keys outside int32)
SELECT run('SELECT count(*) FROM ev WHERE k4 IN (SELECT k FROM keys8 WHERE grp = 2)', true) = run('SELECT count(*) FROM ev WHERE k4 IN (SELECT k FROM keys8 WHERE grp = 2)', false) AS int4_leaf;
SELECT run('SELECT count(*) FROM ev WHERE k8 IN (SELECT k FROM keys8 WHERE grp = 2)', true) = run('SELECT count(*) FROM ev WHERE k8 IN (SELECT k FROM keys8 WHERE grp = 2)', false) AS int8_leaf_after;
SELECT run('SELECT count(*) FROM ev WHERE k8 IN (SELECT k FROM keys8 WHERE grp = 2)', true) = run('SELECT count(*) FROM ev WHERE k8 IN (SELECT k FROM keys8 WHERE grp = 2)', false) AS int8_leaf_again;
SELECT run('SELECT count(*) FROM ev WHERE k4 IN (SELECT k FROM keys8 WHERE grp = 2)', true) = run('SELECT count(*) FROM ev WHERE k4 IN (SELECT k FROM keys8 WHERE grp = 2)', false) AS int4_leaf_after;

-- the memory of the test is bounded by sift.agg_semi_lut_size: a byte table over the span of the keys, else a hash set of them, else the generic join
SELECT tables_of('SELECT count(*) FROM ev WHERE k8 IN (SELECT k FROM keys8 WHERE grp = 3)') AS window_default;
SET sift.agg_semi_lut_size = 1;
SELECT tables_of('SELECT count(*) FROM ev WHERE k8 IN (SELECT k FROM keys8 WHERE grp = 3)') AS window_1kb;
SELECT tables_of('SELECT count(*) FROM ev WHERE k8 IN (SELECT k FROM keys8 WHERE grp = 1)') AS set_2000_keys_1kb;
SELECT tables_of('SELECT count(*) FROM ev WHERE k8 IN (SELECT k FROM keys8 WHERE grp = 2)') AS set_6_keys_1kb;
SELECT run(sql, true) = run(sql, false) AS same_1kb FROM q WHERE n IN (1, 3, 5, 8, 19);
SET sift.agg_semi_lut_size = 0;
SELECT tables_of('SELECT count(*) FROM ev WHERE k8 IN (SELECT k FROM keys8 WHERE grp = 3)') AS size_0;
RESET sift.agg_semi_lut_size;

-- from workers
ALTER TABLE ev SET (parallel_workers = 2);
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SELECT run(sql, true) = run(sql, false) AS same_with_workers FROM q WHERE n IN (1, 3, 13);
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET sift.agg_join_filter;

DROP FUNCTION check_all(text), run(text, bool), generic(text), tables_of(text);
DROP TABLE q, ev, keys8, keys8_nn, dims, dims8, nothing;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
