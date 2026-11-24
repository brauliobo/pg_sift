-- The scans of PostgreSQL are off, as in the plan-choice gate, so that the node plans where it can. A dimension of a join of Custom Scan (SiftAggV6) that the fact table's filter says the keys of (fact.k IN (...)) is read through its own index, one probe a key, and not whole.
-- Inner, left, semi and anti joins: the rows must be those of sift.enable_agg = off, with NULL keys on both sides, keys the dimension does not have, a NULL in the list, a list that
-- leaves no fact row, keys that match many dimension rows, a dimension changed by the transaction, and a list of most of the keys, which the node does not probe.
CREATE EXTENSION pg_sift;
DO $$ BEGIN
	IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF;
END $$;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET enable_seqscan = off;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SET enable_indexonlyscan = off;

CREATE TABLE f (id int PRIMARY KEY, k int, g text, v int) WITH (autovacuum_enabled = false);
INSERT INTO f SELECT i, CASE WHEN i % 13 = 0 THEN NULL WHEN i % 7 = 0 THEN 4900 + i % 3 ELSE 1 + i % 240 END, 'g' || (i % 4), i % 50 FROM generate_series(1, 6000) i;
CREATE INDEX f_col ON f USING sift_col (id, k, g, v);
VACUUM (FREEZE, ANALYZE) f;

-- 250 keys, 40 rows each (more than one match for every fact row), 20 NULL keys, keys no fact row has
CREATE TABLE d (id int PRIMARY KEY, k int, t text, w int) WITH (autovacuum_enabled = false);
INSERT INTO d SELECT i, 1 + i % 250, 't' || (i % 4), i % 30 FROM generate_series(1, 10000) i;
INSERT INTO d SELECT 10000 + i, NULL, 'tn', i FROM generate_series(1, 20) i;
INSERT INTO d SELECT 10100 + i, 9000 + i, 'tx', i FROM generate_series(1, 5) i;
CREATE INDEX d_k ON d (k);
VACUUM (FREEZE, ANALYZE) d;

CREATE FUNCTION same(q text) RETURNS TABLE (engine text, probed boolean, result_rows bigint, vs_heap bigint) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	engine := 'normal';
	probed := false;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAggV6)%' THEN engine := 'SiftAggV6'; END IF;
		IF l LIKE '%SiftAggV6Push%' THEN engine := 'push'; END IF;
		IF l ~ '(Index|Recheck) Cond: .*k = ANY' THEN probed := true; END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS SELECT x::text AS r FROM (%s) x', q);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS SELECT x::text AS r FROM (%s) x', q);
	RESET sift.enable_agg;
	result_rows := (SELECT count(*) FROM r_on);
	vs_heap := (SELECT count(*) FROM ((TABLE r_on EXCEPT ALL TABLE r_off) UNION ALL (TABLE r_off EXCEPT ALL TABLE r_on)) x);
	DROP TABLE r_on, r_off;
	RETURN NEXT;
END $$;

CREATE TABLE stmts (id int, q text);
INSERT INTO stmts VALUES
(1, 'SELECT f.g, count(*), sum(d.w) FROM f JOIN d ON d.k = f.k WHERE f.k IN (3, 7, 11, 13, 17) GROUP BY f.g'),
(2, 'SELECT f.g, count(*), count(d.t), sum(d.w) FROM f LEFT JOIN d ON d.k = f.k AND d.w > 20 WHERE f.k IN (3, 7, 11, 13, 17) GROUP BY f.g'),
(3, 'SELECT f.g, count(*) FROM f WHERE f.k IN (3, 10, 20, 30, 40) AND EXISTS (SELECT 1 FROM d WHERE d.k = f.k AND d.w > 28) GROUP BY f.g'),
(4, 'SELECT f.g, count(*) FROM f WHERE f.k IN (3, 7, 11, 13, 17) AND NOT EXISTS (SELECT 1 FROM d WHERE d.k = f.k AND d.w > 28 AND d.t = ''t1'') GROUP BY f.g'),
(5, 'SELECT f.g, count(*), sum(d.w) FROM f JOIN d ON d.k = f.k WHERE f.k IN (3, NULL, 7) GROUP BY f.g'),
(6, 'SELECT f.g, count(*), count(d.t) FROM f LEFT JOIN d ON d.k = f.k WHERE f.k IN (4900, 4901, 4902) GROUP BY f.g'),
(7, 'SELECT f.g, count(*), count(d.t) FROM f LEFT JOIN d ON d.k = f.k WHERE f.k IN (6000, 6001) GROUP BY f.g'),
(8, 'SELECT f.g, count(*) FROM f WHERE f.k IN (4900, 4901, 4902) AND NOT EXISTS (SELECT 1 FROM d WHERE d.k = f.k) GROUP BY f.g'),
(9, 'SELECT f.g, count(*) FROM f WHERE f.k IN (4903, 4904) AND EXISTS (SELECT 1 FROM d WHERE d.k = f.k AND d.w > 28) GROUP BY f.g'),
(10, 'SELECT d.t, count(*), sum(f.v) FROM f JOIN d ON d.k = f.k AND d.t = ''t2'' WHERE f.k IN (3, 7, 11, 13, 17, 19, 23) GROUP BY d.t'),
(11, 'SELECT f.g, count(*) FROM f JOIN d ON d.k = f.k WHERE f.k IN (3, 7) AND f.v > 10 AND d.w < 20 GROUP BY f.g'),
(12, 'SELECT f.g, count(*) FROM f JOIN d ON d.k = f.k WHERE f.k = ANY (ARRAY[3, 7, 11]) GROUP BY f.g');

SELECT id, (same(q)).* FROM stmts ORDER BY id;

-- a list of most of the distinct keys reads the dimension whole
SELECT (same(format('SELECT f.g, count(*), sum(d.w) FROM f JOIN d ON d.k = f.k WHERE f.k IN (%s) GROUP BY f.g', (SELECT string_agg(i::text, ',') FROM generate_series(1, 200) i)))).*;
SELECT (same(format('SELECT f.g, count(*) FROM f WHERE f.k IN (%s) AND NOT EXISTS (SELECT 1 FROM d WHERE d.k = f.k AND d.w > 28) GROUP BY f.g', (SELECT string_agg(i::text, ',') FROM generate_series(1, 200) i)))).*;

-- a dimension the transaction changed: the probe reads the rows of its snapshot
BEGIN;
DELETE FROM d WHERE k = 3 AND w < 15;
UPDATE d SET k = 7 WHERE k = 11;
UPDATE d SET w = w + 100 WHERE k = 13;
INSERT INTO d VALUES (20001, 3, 'tz', 1), (20002, NULL, 'tz', 2);
SELECT id, (same(q)).* FROM stmts WHERE id IN (1, 2, 3, 4) ORDER BY id;
ROLLBACK;

SELECT id, (same(q)).* FROM stmts WHERE id IN (1, 4) ORDER BY id;

DROP FUNCTION same(text);
DROP TABLE stmts, f, d;
DROP EXTENSION pg_sift;
