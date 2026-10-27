-- the segments a parallel build leaves cover the same superblocks, and the rewrite that merges them is not held by the WAL budget of the churn: the index right after
-- its build is due for the automatic rewrite, which leaves one segment (the budget of the churn is tested by col_auto_merge)
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 4;
SET sift.col_auto_merge_wal_pct = 5;

CREATE TABLE nk (id int, cust int, amt int, q int) WITH (autovacuum_enabled = false);
INSERT INTO nk SELECT i, ((i::bigint * 7919) % 20000)::int, ((i::bigint * 31) % 1000)::int, i % 100 FROM generate_series(1, 1000000) i;
VACUUM (FREEZE, ANALYZE) nk;
CREATE INDEX nk_idx ON nk USING sift_col (id, cust, amt, q) WITH (key_cut = 'cust');

SELECT count(*) > 1 AS peers_from_workers FROM sift.col_segments('nk_idx') WHERE kind = 'immutable';
SELECT sift.col_maintenance_due('nk_idx', true) AS due_automatic, sift.col_maintenance_due('nk_idx', false) AS due_manual;
SELECT sift.col_maintain('nk_idx') AS rewrote;
SELECT count(*) AS segments, sum(units) <= 2 * (SELECT units FROM sift.col_index_info('nk_idx')) AS listed_within_threshold FROM sift.col_segments('nk_idx') WHERE kind = 'immutable';
SELECT sift.col_maintenance_due('nk_idx', true) IS NULL AS clean;
SELECT sift.col_verify('nk_idx', true);

DROP TABLE nk;
DROP EXTENSION pg_sift;
