-- An equality on a column with an order array is a seek in the sorted keys, like the range [x, x]: the plan is an index scan that hands the heap the addresses of the key, not an index-only scan
-- that reads the key column of every unit. The answers are the table's (a copy without an index is the oracle), whatever the key: a hit, a miss, a duplicate, NULL, the ends of the range.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.async_maintenance = off;
SELECT setseed(0.42);

-- k is a permutation of 1..200000 in no order of the heap; d repeats each value 4 times; n has NULLs
CREATE TABLE t AS SELECT i AS id, 1 + (random() * 40)::int AS g, k, k / 4 AS d, CASE WHEN i % 7 = 0 THEN NULL ELSE k END AS n
FROM (SELECT i, row_number() OVER (ORDER BY random())::int AS k FROM generate_series(1, 200000) i) perm ORDER BY i;
CREATE INDEX t_ix ON t USING sift_col (id, g, k, d, n) WITH (order_columns = 'k, d, n', order_array_columns = 'k, d, n', point_tid_columns = 'off', key_cut = 'off');
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
	EXECUTE format('SELECT string_agg(id::text, '','' ORDER BY id) FROM (%s) s', replace(q, '@', 't')) INTO a;
	EXECUTE format('SELECT string_agg(id::text, '','' ORDER BY id) FROM (%s) s', replace(q, '@', 'p')) INTO b;
	IF a IS DISTINCT FROM b THEN RAISE NOTICE '% differs: %', q, left(a, 80); END IF;
	RETURN a IS NOT DISTINCT FROM b;
END $$;

SELECT plan_of('SELECT id, k FROM t WHERE k = 123457') AS equality_plan;
SELECT blocks_of('SELECT id, k FROM t WHERE k = 123457') < pg_relation_size('t_ix') / 8192 / 8 AS equality_reads_few_blocks;
SELECT q, same(q) AS answers
FROM unnest(ARRAY['SELECT id, k FROM @ WHERE k = 123457', 'SELECT id, k FROM @ WHERE k = 1', 'SELECT id, k FROM @ WHERE k = 200000', 'SELECT id, k FROM @ WHERE k = 0',
	'SELECT id, k FROM @ WHERE k = 200001', 'SELECT id, d FROM @ WHERE d = 1000', 'SELECT id, d FROM @ WHERE d = 0', 'SELECT id, n FROM @ WHERE n = 7', 'SELECT id, n FROM @ WHERE n = 8',
	'SELECT id, k FROM @ WHERE k = 123457 AND g < 100', 'SELECT id, k FROM @ WHERE k = 123457 AND g > 100', 'SELECT id, k FROM @ WHERE k = 123457 OR k = 5']) q;

-- an IN list is a seek for each key, the rows of the keys together in the order of the heap: the blocks are few, as many keys as the order hands over (512), more are read from the units
CREATE FUNCTION keys(n int, step int) RETURNS text LANGUAGE sql AS $$ SELECT string_agg((1 + (i * step) % 200000)::text, ', ') FROM generate_series(1, n) i $$;
SELECT plan_of('SELECT id, k FROM t WHERE k IN (' || keys(4, 7919) || ')') AS in4_plan;
SELECT blocks_of('SELECT id, k FROM t WHERE k IN (' || keys(4, 7919) || ')') < pg_relation_size('t_ix') / 8192 / 8 AS in4_reads_few_blocks;
SELECT n, same('SELECT id, k FROM @ WHERE k IN (' || keys(n, 7919) || ')') AS answers FROM unnest(ARRAY[1, 2, 3, 16, 60, 300, 511, 512, 513, 600, 2000]) n;
SELECT q, same(q) AS answers
FROM unnest(ARRAY['SELECT id, k FROM @ WHERE k IN (1, 1, 2, 2, 200000, 0, -5, 200001, 123457)', 'SELECT id, k FROM @ WHERE k = ANY (ARRAY[5, 6, NULL, 7])', 'SELECT id, k FROM @ WHERE k IN (5, 6) AND g < 20',
	'SELECT id, k FROM @ WHERE k IN (5, 6, 7) OR k = 9', 'SELECT id, n FROM @ WHERE n IN (7, 14, 21, 8, 9)', 'SELECT id, d FROM @ WHERE d IN (0, 1, 1000, 50000, 70000)',
	'SELECT id, k FROM @ WHERE k NOT IN (1, 2, 3) AND k < 10', 'SELECT id, k FROM @ WHERE k = ALL (ARRAY[5, 5])', 'SELECT id, k FROM @ WHERE k IN (SELECT 5 UNION SELECT 6)',
	'SELECT id, k FROM @ WHERE k IN (5, 6, 7) AND k > 5']) q;

-- rows of the table that changed after the build: updated, deleted, a rolled-back update, inserted into the write buffer
UPDATE t SET k = 300000 WHERE id % 1000 = 3;
UPDATE p SET k = 300000 WHERE id % 1000 = 3;
DELETE FROM t WHERE id % 1000 = 5;
DELETE FROM p WHERE id % 1000 = 5;
BEGIN; UPDATE t SET k = 400000 WHERE id % 1000 = 7; ROLLBACK;
INSERT INTO t SELECT 200000 + i, 1, 500000 + i % 10, 1, 1 FROM generate_series(1, 100) i;
INSERT INTO p SELECT 200000 + i, 1, 500000 + i % 10, 1, 1 FROM generate_series(1, 100) i;
SELECT q, same(q) AS answers
FROM unnest(ARRAY['SELECT id, k FROM @ WHERE k = 300000', 'SELECT id, k FROM @ WHERE k = 400000', 'SELECT id, k FROM @ WHERE k = 500003', 'SELECT id, k FROM @ WHERE k = 3', 'SELECT id, k FROM @ WHERE k = 5',
	(SELECT 'SELECT id, k FROM @ WHERE k = ' || k FROM t WHERE id = 3), (SELECT 'SELECT id, k FROM @ WHERE k = ' || k FROM t WHERE id = 1006)]) q;
SELECT n, same('SELECT id, k FROM @ WHERE k IN (' || keys(n, 7919) || ', 300000, 400000, 500003, 500004)') AS answers FROM unnest(ARRAY[1, 16, 300, 512]) n;
SELECT same('SELECT id, k FROM @ WHERE k IN (SELECT k FROM t WHERE id IN (3, 1003, 2003, 1006))') AS answers;

DROP TABLE t, p;
DROP FUNCTION plan_of(text), blocks_of(text), same(text), keys(int, int);
DROP EXTENSION pg_sift;
