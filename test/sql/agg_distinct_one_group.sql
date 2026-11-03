-- count(DISTINCT x) of an integer or date column with no GROUP BY sets the bits of one bitmap with the directory of the group read once, not for each value:
-- values that span several chunks of the bitmap (4,096 values each), negative ones, a column of few values, NULLs, a filter that keeps none, some or all of
-- the rows, and a FILTER on the aggregate. Every query runs as the scan and as the normal plan (outcome), and the rows must be the same; `scan` says
-- the plan is the scan's.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET client_min_messages = warning;

CREATE TABLE do1 (id int, a int, few int, wide int, d date, n int, k int) WITH (autovacuum_enabled = false);
INSERT INTO do1
SELECT i, (i::bigint * 7919 % 20011)::int - 10000, i % 7, (i::bigint * 104729 % 1000003)::int * 1000 - 500000000, date '2020-01-01' + i % 3000,
       CASE WHEN i % 5 = 0 THEN NULL ELSE i % 1000 END, i % 3
FROM generate_series(1, 30000) i;
CREATE INDEX do1_col ON do1 USING sift_col (id, a, few, wide, d, n, k);
VACUUM (FREEZE, ANALYZE) do1;

\ir test/agg_helpers.sql

SELECT w, pushed(q) AS scan, outcome(q, false) = outcome(q, true) AS same
FROM (SELECT w, format('SELECT row(count(DISTINCT a), count(DISTINCT few), count(DISTINCT wide), count(DISTINCT d), count(DISTINCT n), count(DISTINCT k), count(*))::text FROM do1 WHERE %s', w) AS q
	  FROM unnest(ARRAY['id > 0', 'id % 7 = 0', 'id < 0', 'k = 1', 'a > 9900', 'n IS NULL', 'id > 4000 AND id < 12000', 'id % 2 = 0 AND k <> 2', 'wide < 0']) w) t;

SELECT w, pushed(q) AS scan, outcome(q, false) = outcome(q, true) AS same
FROM (SELECT w, q FROM unnest(ARRAY[
	'SELECT row(count(DISTINCT a) FILTER (WHERE k = 0), count(DISTINCT wide) FILTER (WHERE id % 13 = 0), count(DISTINCT d) FILTER (WHERE a < 0))::text FROM do1 WHERE id > 5',
	'SELECT row(count(DISTINCT a), sum(a), count(DISTINCT n), min(wide))::text FROM do1 WHERE id > 5'
]) WITH ORDINALITY AS t(q, w)) t;

DROP FUNCTION outcome, runq, runs_in_scan, pushed;
DROP TABLE do1;
DROP EXTENSION pg_sift;
