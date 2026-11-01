-- VACUUM truncates the heap behind the rows an aborted UPDATE and INSERT left, and the global order of the index still lists their addresses (a vacuum clears the liveness of the
-- rows of a segment, it does not take them out of its order). The top-N that reads the order skips an address past the end of the heap, as it skips one of a dead row.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET enable_sort = off;

CREATE TABLE t (id int, ts timestamp, pad int);
INSERT INTO t SELECT i, TIMESTAMP '2026-01-01' + random() * 300 * INTERVAL '1 day', 0 FROM generate_series(1, 6000) i;
CREATE INDEX t_ix ON t USING sift_col (id, ts, pad) WITH (order_columns = 'ts', order_array_columns = 'ts', write_buffer_limit = 64);
INSERT INTO t SELECT 6000 + i, TIMESTAMP '2026-01-01' + random() * 300 * INTERVAL '1 day', 0 FROM generate_series(1, 1500) i;
SELECT sift.col_promote('t_ix') >= 0 AS promoted;
INSERT INTO t SELECT 8000 + i, TIMESTAMP '2026-01-01' + random() * 300 * INTERVAL '1 day', 0 FROM generate_series(1, 100) i;
UPDATE t SET ts = ts + INTERVAL '3 days' WHERE id % 13 = 0;
UPDATE t SET pad = 1 WHERE id % 5 = 0;
BEGIN;
UPDATE t SET ts = ts + INTERVAL '1 year' WHERE id % 3 = 0;
INSERT INTO t SELECT 20000 + i, TIMESTAMP '2027-06-01', 0 FROM generate_series(1, 200) i;
ROLLBACK;
ANALYZE t;
SELECT pg_relation_size('t') AS before_vacuum \gset
VACUUM t;
SELECT pg_relation_size('t') < :before_vacuum AS truncated;
CREATE TABLE p AS SELECT * FROM t;
SELECT (SELECT string_agg(ts::text, ',') FROM (SELECT ts FROM t ORDER BY ts DESC LIMIT 20 OFFSET 30) s) =
	   (SELECT string_agg(ts::text, ',') FROM (SELECT ts FROM p ORDER BY ts DESC LIMIT 20 OFFSET 30) s) AS desc_same;
SELECT (SELECT string_agg(ts::text, ',') FROM (SELECT ts FROM t ORDER BY ts DESC NULLS FIRST LIMIT 20) s) =
	   (SELECT string_agg(ts::text, ',') FROM (SELECT ts FROM p ORDER BY ts DESC NULLS FIRST LIMIT 20) s) AS desc_head_same;
-- a range and a count over the keys the aborted rows had hand the heap the addresses between the bounds: the pages that are gone hold no row
SELECT (SELECT count(*) FROM t WHERE ts >= TIMESTAMP '2026-12-01') = (SELECT count(*) FROM p WHERE ts >= TIMESTAMP '2026-12-01') AS count_same;
SELECT (SELECT string_agg(id::text, ',' ORDER BY id) FROM t WHERE ts BETWEEN TIMESTAMP '2026-11-01' AND TIMESTAMP '2027-12-31') =
	   (SELECT string_agg(id::text, ',' ORDER BY id) FROM p WHERE ts BETWEEN TIMESTAMP '2026-11-01' AND TIMESTAMP '2027-12-31') AS range_same;
-- the order is what answered them
CREATE FUNCTION global_order(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF l ~ 'Global Order: true' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;
SELECT global_order('SELECT ts FROM t ORDER BY ts DESC LIMIT 20 OFFSET 30') AS from_the_order;

DROP FUNCTION global_order(text);
DROP TABLE p, t;
DROP EXTENSION pg_sift;
