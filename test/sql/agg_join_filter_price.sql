-- A join whose condition is no key (`k = n + 1`: an addition can raise) tests every tuple the scan keeps against every row of the joined table, so the scan
-- is priced for those tuples, not for the few the join lets through. With the page cost raised for the table scan, the plan with a column as the key stays the
-- scan and the one with the addition is the hash join, which runs in a fraction of the time. Every query gives the same rows either way.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 20;

CREATE TABLE facts (id int, k int, v int) WITH (autovacuum_enabled = false);
INSERT INTO facts SELECT i, i, i % 10 FROM generate_series(1, 20000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, k, v);
VACUUM (FREEZE, ANALYZE) facts;
CREATE TABLE keys (n int) WITH (autovacuum_enabled = false);
INSERT INTO keys SELECT i * 37 FROM generate_series(1, 200) i;
VACUUM (FREEZE, ANALYZE) keys;

\ir test/agg_helpers.sql

SELECT sub, pushed(q) AS scan, runq(q, false) = runq(q, true) AS same_rows
FROM (SELECT sub, format('SELECT count(*), sum(v) FROM facts WHERE k IN (SELECT %s FROM keys)', sub) AS q
	  FROM unnest(ARRAY['n', 'n + 1', 'n::bigint']) sub) t;

DROP FUNCTION outcome, runq, runs_in_scan, pushed;
DROP TABLE facts, keys;
DROP EXTENSION pg_sift;
