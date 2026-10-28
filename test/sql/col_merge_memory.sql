-- the segments a merge writes are cut to what maintenance_work_mem holds (the writer keeps the key and address of every row of a global order until the last
-- unit of the segment is in): a small setting makes partitions of a few MB, a large one a single segment
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 4;
SET sift.col_auto_merge_wal_pct = 0;

CREATE TABLE mm (id int, cust int, amt int, q int) WITH (autovacuum_enabled = false);
INSERT INTO mm SELECT i, ((i::bigint * 7919) % 20000)::int, hashint8(i) % 1000000, hashint8(i + 7) FROM generate_series(1, 800000) i;
VACUUM (FREEZE, ANALYZE) mm;
CREATE INDEX mm_idx ON mm USING sift_col (id, cust, amt, q) WITH (key_cut = 'cust');
SELECT count(*) > 1 AS peers, sum(bytes) > 6 * 1024 * 1024 AS over_the_floor FROM sift.col_segments('mm_idx') WHERE kind = 'immutable';

SET maintenance_work_mem = '1MB';
SELECT sift.col_merge('mm_idx', true);
SELECT count(*) > 1 AS partitions, max(bytes) < 4 * 1024 * 1024 AS each_under_the_floor FROM sift.col_segments('mm_idx') WHERE kind = 'immutable';
SELECT sift.col_maintenance_due('mm_idx', false) IS NULL AS partitions_are_left_alone;
SELECT sift.col_verify('mm_idx', true);

SET maintenance_work_mem = '1GB';
SELECT sift.col_merge('mm_idx', true);
SELECT count(*) AS segments FROM sift.col_segments('mm_idx') WHERE kind = 'immutable';
SELECT sift.col_verify('mm_idx', true);

DROP TABLE mm;
DROP EXTENSION pg_sift;
