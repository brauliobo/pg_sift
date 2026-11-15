-- ALTER INDEX .. SET (key_cut) changes the cut of the units written from now on and leaves the segments as they are. A merge that meets a segment cut by another
-- key than the index's now (or by none: the units of the floor are cut by TID range) sorts its rows again: it must not read the units as a stream in the order of
-- the key or of the TIDs, which they are not in ("Segment N is not in the order of the TIDs" from every VACUUM of the table).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;

-- 100 tenants in heap order but for 15% of the rows, which land anywhere: the units of a cut by tenant_id each span the heap (100 superblocks: fillfactor 10 keeps the rows few)
CREATE TABLE ks (id int, tenant_id int, a1 int, a2 int, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
SELECT setseed(0.5);
INSERT INTO ks SELECT i, CASE WHEN random() < 0.15 THEN (random() * 100)::int ELSE i / 1250 END, (i * 31) % 1000, (i * 37) % 1000, repeat('x', 100)
FROM generate_series(1, 125000) i;
VACUUM (FREEZE, ANALYZE) ks;
CREATE INDEX ks_col ON ks USING sift_col (tenant_id, a1, a2) WITH (key_cut = 'tenant_id', order_array_columns = 'off');
SELECT sift_helpers.index_key_cut_reloption('ks_col');

-- the cut is switched off: the next merges and the VACUUM meet segments cut by tenant_id
ALTER INDEX ks_col SET (key_cut = 'off');
INSERT INTO ks SELECT 200000 + i, (random() * 100)::int, i % 1000, i % 999, 'x' FROM generate_series(1, 5000) i;
SELECT sift.col_merge('ks_col', true) > 0 AS merged;
SELECT sift.col_verify('ks_col', true);
DELETE FROM ks WHERE id % 7 = 3;
INSERT INTO ks SELECT 300000 + i, (random() * 100)::int, i % 1000, i % 999, 'x' FROM generate_series(1, 5000) i;
VACUUM ks;
SELECT sift.col_verify('ks_col', true);

-- the other way: the cut is switched on over the segments the merge above wrote, cut by TID range
ALTER INDEX ks_col SET (key_cut = 'tenant_id');
INSERT INTO ks SELECT 400000 + i, (random() * 100)::int, i % 1000, i % 999, 'x' FROM generate_series(1, 5000) i;
SELECT sift.col_merge('ks_col', true) > 0 AS merged;
SELECT sift.col_verify('ks_col', true);

-- the rows are the table's, with the index and without
SELECT sift_helpers.differences_agg('SELECT tenant_id, count(*) AS n, sum(a1) AS s, max(id) AS m FROM ks GROUP BY tenant_id') AS differences;
SELECT sift_helpers.differences_agg('SELECT count(*) AS n, sum(a2) AS s FROM ks WHERE tenant_id BETWEEN 10 AND 20') AS differences;

DROP TABLE ks;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
