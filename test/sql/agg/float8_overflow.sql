-- SUM and AVG of a float8 column in SiftAggV6 raise what float8pl raises: a sum of finite values that leaves the range is the error 22003 "value out of range: overflow", in a batch, across
-- batches and across groups, while an infinity among the values is the sum Infinity (and an Infinity with its opposite NaN) with no error. The AVG is tried where the sum decides (PostgreSQL's
-- float8_accum also keeps a sum of squares, which the node does not). Every query is run with the node and without it (sift.enable_agg) and the two outcomes are compared; the first word says
-- they agree, the second that the plan is the node's.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET sift.test_create_format1 = off;
SET max_parallel_workers_per_gather = 0;
SET jit = off;

CREATE TABLE fl (id int PRIMARY KEY, grp int NOT NULL, f8 float8) WITH (autovacuum_enabled = false);
-- the values are powers of two: the sums are exact in every order, 4 of 2^1022 are 2^1024, which is out of the range
INSERT INTO fl SELECT i, 1, 2.0 ^ 1022 FROM generate_series(1, 4) i;
INSERT INTO fl SELECT 10 + i, 2, 2.0 ^ 1022 FROM generate_series(1, 3) i;
INSERT INTO fl VALUES (20, 3, 'Infinity'), (21, 3, 2.0 ^ 1022), (22, 3, 2.0 ^ 1022), (23, 3, 2.0 ^ 1022);
INSERT INTO fl VALUES (30, 4, 'Infinity'), (31, 4, '-Infinity'), (32, 4, 1);
INSERT INTO fl VALUES (40, 5, '-Infinity'), (41, 5, 2.0 ^ 1022), (42, 5, 2.0 ^ 1022), (43, 5, 2.0 ^ 1022);
INSERT INTO fl SELECT 1000 + i, 6, CASE WHEN i IN (1, 2500, 5000, 7500) THEN 2.0 ^ 1022 ELSE 1 END FROM generate_series(1, 8000) i;
INSERT INTO fl SELECT 20000 + i, 7, CASE WHEN i IN (1, 2500, 5000) THEN 2.0 ^ 1022 ELSE 1 END FROM generate_series(1, 8000) i;
CREATE INDEX fl_col ON fl USING sift_col (id, grp, f8);
VACUUM (FREEZE, ANALYZE) fl;

CREATE FUNCTION outcome(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE 'SELECT string_agg(r::text, ''|'' ORDER BY r::text) FROM (' || q || ') r' INTO r;
	RETURN r;
EXCEPTION WHEN OTHERS THEN
	RETURN 'error ' || SQLSTATE;
END $$;
CREATE FUNCTION same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text; plan text := ''; l text;
BEGIN
	SET LOCAL enable_seqscan = off;
	SET LOCAL enable_indexscan = off;
	SET LOCAL enable_indexonlyscan = off;
	SET LOCAL enable_bitmapscan = off;
	SET LOCAL sift.enable_agg = on;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || l; END LOOP;
	a := outcome(q);
	SET LOCAL enable_seqscan = on;
	SET LOCAL sift.enable_agg = off;
	b := outcome(q);
	RETURN (CASE WHEN a IS DISTINCT FROM b THEN 'DIFFERENT' ELSE 'same' END) || ' ' || (CASE WHEN plan LIKE '%SiftAggV6%' THEN 'node' ELSE 'normal' END) || ' ' || b;
END $$;

SELECT g, same(format('SELECT sum(f8) FROM fl WHERE grp = %s', g)) AS sum, CASE WHEN g < 7 THEN same(format('SELECT avg(f8) FROM fl WHERE grp = %s', g)) END AS avg FROM generate_series(1, 7) g;

SELECT same(q) FROM unnest(ARRAY[
	'SELECT grp, sum(f8) FROM fl WHERE grp IN (2, 3, 4, 5) GROUP BY grp',
	'SELECT grp, sum(f8) FROM fl WHERE grp IN (2, 3, 1) GROUP BY grp',
	'SELECT grp, sum(f8), count(*) FROM fl WHERE grp BETWEEN 2 AND 5 GROUP BY grp',
	'SELECT sum(f8) FILTER (WHERE grp = 2), sum(f8) FILTER (WHERE grp = 3) FROM fl'
]) q;

DROP TABLE fl;
DROP FUNCTION same(text);
DROP FUNCTION outcome(text);
DROP EXTENSION pg_sift;
