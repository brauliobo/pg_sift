-- The WAL budget of the automatic rewrite (sift.col_auto_merge_wal_pct) defers a rewrite of the segments, not the promotion of the write buffer, which costs only its rows: while the
-- units with dead rows are due for a rewrite and the budget holds it back, the rows in the write buffer are due for their promotion, where they were read by every query until the budget came back.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET sift.async_maintenance = off;
SET sift.col_auto_merge = off;
SET sift.test_col_net = off;	-- the units with dead rows are scanned: the rule that counts dirty units is what this test is about
SET max_parallel_workers_per_gather = 0;

CREATE TABLE np (id int, grp int, v int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO np SELECT g, g % 20, g % 1000, repeat('x', 1500) FROM generate_series(1, 12000) g;
CREATE INDEX np_ix ON np USING sift_col (id, grp, v);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) np;

-- a dead row in every unit: the rewrite is due
DELETE FROM np WHERE id % 50 = 7;
VACUUM np;
SELECT rewrite_due FROM sift.col_maintenance_status('np_ix');

-- the budget holds it back: it counts the WAL of the whole server since the segments were written, so a budget of a thousandth of a percent is out of reach of the tests that run beside this one
SET sift.col_auto_merge_wal_pct = 0.001;
SELECT rewrite_due, 'the rewrite would take more than sift.col_auto_merge_wal_pct of the WAL written since the last' = ANY (blocked_by) AS wal_gate FROM sift.col_maintenance_status('np_ix');

-- and the buffer rows, 3% of the rows, are promoted nevertheless
INSERT INTO np SELECT g, g % 20, g % 1000, repeat('x', 1500) FROM generate_series(12001, 12360) g;
SELECT rewrite_due, 'the rewrite would take more than sift.col_auto_merge_wal_pct of the WAL written since the last' = ANY (blocked_by) AS wal_gate FROM sift.col_maintenance_status('np_ix');
SELECT sift.col_verify('np_ix', true);
DROP TABLE np;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
