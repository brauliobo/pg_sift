-- The column a second order is grouped by (order_array_group) has no order of its own, and an equality, an IN list or a range on it is still a lookup: the table of groups lists the rows of each key
-- (the NULL-key rows of the group too; the groups of a range are one span of the table), so the plan is an index scan that hands the heap the addresses, not an index-only scan that reads every unit. The answers are the table's (a copy
-- without an index is the oracle): a hit, a miss, a duplicate, rows with a NULL key, a write buffer, deleted and updated rows, a promotion.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;

-- written as built, with no UPDATE before the index: a dead version the index build meets sets indcheckxmin, and a session of another test that holds an older
-- snapshot then plans the table without the index (a sequential scan) until it ends
-- the group 1000 has three rows in three places of the table; the groups 2000 to 2099 have one row each in every 1,500: every unit holds one, so no unit is skipped by its bounds
CREATE TABLE t AS SELECT i AS id,
	   CASE WHEN i IN (11, 50011, 100011) THEN 1000 WHEN i % 1500 = 0 THEN 2000 + i / 1500 % 100 WHEN i % 53 = 0 THEN NULL ELSE 1 + abs(hashint4(i)) % 300 END AS g,
	   CASE WHEN i % 29 = 0 THEN NULL ELSE timestamp '2024-01-01' + abs(hashint4(i + 7)) % 5000000 * interval '1 second' END AS created, abs(hashint4(i + 3)) % 1000 AS score
FROM generate_series(1, 150000) i;
CREATE INDEX t_ix ON t USING sift_col (id, g, created, score) WITH (order_columns = 'created', order_array_columns = 'created', order_array_group = 'g', point_tid_columns = 'off', key_columns = 'off', key_cut = 'off');
VACUUM ANALYZE t;
CREATE TABLE p AS SELECT * FROM t;
ANALYZE p;

CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Scan' THEN RETURN trim(regexp_replace(l, ' using .*', '')); END IF;
	END LOOP;
	RETURN NULL;
END $$;
CREATE FUNCTION blocks_of(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE l text; b bigint := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF l ~ 'Buffers: shared' AND b = 0 THEN b := coalesce(substring(l from 'hit=(\d+)')::bigint, 0) + coalesce(substring(l from 'read=(\d+)')::bigint, 0); END IF;
	END LOOP;
	RETURN b;
END $$;
CREATE FUNCTION same(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	EXECUTE format('SELECT string_agg(t::text, ''|'' ORDER BY t::text) FROM (%s) t', replace(q, '@', 't')) INTO a;
	EXECUTE format('SELECT string_agg(t::text, ''|'' ORDER BY t::text) FROM (%s) t', replace(q, '@', 'p')) INTO b;
	IF a IS DISTINCT FROM b THEN RAISE NOTICE '% differs: %', q, left(a, 80); END IF;
	RETURN a IS NOT DISTINCT FROM b;
END $$;
CREATE TABLE qs (q text);
INSERT INTO qs VALUES ('SELECT id, g, created FROM @ WHERE g = 7'), ('SELECT id, g, created FROM @ WHERE g = 301'), ('SELECT id, g, created FROM @ WHERE g = 0'),
	('SELECT id, g, created FROM @ WHERE g IN (7, 8, 9, 7, 400, 12)'), ('SELECT id, g, created FROM @ WHERE g IN (1, 150, 300)'),
	('SELECT id, g, created FROM @ WHERE g = 7 AND score < 500'), ('SELECT id, g, created FROM @ WHERE g IN (7, 8) AND created IS NULL'),
	('SELECT id, g, created FROM @ WHERE g IN (7, 8) AND created > ''2024-03-01'''), ('SELECT id, g, created FROM @ WHERE g = 7 OR g = 8'), ('SELECT id, g, created FROM @ WHERE g IN (1000, 1001)'), ('SELECT id, g, created FROM @ WHERE g IS NULL');

INSERT INTO qs VALUES ('SELECT id, g, created FROM @ WHERE g BETWEEN 7 AND 9'), ('SELECT id, g, created FROM @ WHERE g >= 299'), ('SELECT id, g, created FROM @ WHERE g < 3'),
	('SELECT id, g, created FROM @ WHERE g > 300 AND g < 1000'), ('SELECT id, g, created FROM @ WHERE g BETWEEN 1000 AND 2000'), ('SELECT id, g, created FROM @ WHERE g > 9 AND g < 9'),
	('SELECT id, g, created FROM @ WHERE g BETWEEN 7 AND 9 AND score < 500'), ('SELECT id, g, created FROM @ WHERE g BETWEEN 7 AND 9 AND created IS NULL');

SELECT plan_of('SELECT id, g, created FROM t WHERE g = 7') AS equality_plan;
-- an index scan of the planner's own choice or forced: the IN list reads the rows of its groups, not the units
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexonlyscan = off;
SELECT blocks_of('SELECT id, g, created FROM t WHERE g IN (1000, 1001)') < pg_relation_size('t_ix') / 8192 / 8 AS in_list_reads_few_blocks;
SELECT q, same(q) AS answers FROM qs ORDER BY q;

-- rows written after the build, deleted and updated rows, a promotion
INSERT INTO t SELECT 200000 + i, CASE WHEN i % 17 = 0 THEN NULL ELSE 1 + i % 300 END, CASE WHEN i % 13 = 0 THEN NULL ELSE timestamp '2024-06-01' + i * interval '1 second' END, i % 1000 FROM generate_series(1, 3000) i;
INSERT INTO p SELECT * FROM t WHERE id > 200000;
DELETE FROM t WHERE id % 97 = 0;
DELETE FROM p WHERE id % 97 = 0;
UPDATE t SET g = 7 WHERE id % 211 = 0;
UPDATE p SET g = 7 WHERE id % 211 = 0;
SELECT q, same(q) AS answers FROM qs ORDER BY q;
SELECT sift.col_promote('t_ix') >= 0 AS promoted;
VACUUM t;
SELECT q, same(q) AS answers FROM qs ORDER BY q;

DROP TABLE t, p, qs;
DROP FUNCTION plan_of(text), blocks_of(text), same(text);
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
