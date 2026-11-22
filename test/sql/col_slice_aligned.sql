-- The merge of a key_cut index by slices (a worker and the backend each write the rows of a slice of the key range, col/v1/order_bound.c) cuts the slices between values of the first key leaf
-- when a value that fits a slice is within the slack of the exact point: a value that fits is in one segment, and only a value bigger than the slack allows for is cut inside. The keys
-- are of skewed sizes (the first is 29% of the rows, the second 12%), so that the exact points of the slices fall inside values; the answers and the rows never change.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET sift.async_maintenance = off;
SET client_min_messages = warning;

CREATE TABLE sa (id int, dim int, day date, v int, w int) WITH (autovacuum_enabled = false);
INSERT INTO sa SELECT g, 1 + floor(12 * power(((g::bigint * 2654435761) % 1000003) / 1000003.0, 2))::int,
       date '2018-01-01' + (((g::bigint * 2654435761) % 4294967296 / 65536) % 2900)::int, g % 100, (g::bigint * 40503 % 1000003)::int
FROM generate_series(1, 150000) g;
VACUUM (FREEZE, ANALYZE) sa;

SET max_parallel_maintenance_workers = 0;
SET sift.test_col_segment_kb = 32;
CREATE INDEX sa_idx ON sa USING sift_col (dim, day, v, w) WITH (key_cut = 'dim, day');
SELECT count(*) > 3 AS built_in_segments FROM sift._test_col_segment_keys('sa_idx');

SET sift.test_col_merge_par_kb = 128;
SET max_parallel_maintenance_workers = 2;
SELECT sift.col_merge('sa_idx', true) IS NOT NULL AS merged;
SHOW sift.test_col_merge_slices;

-- the segments of the merge, in the order of their keys, share no value of the first key leaf that fits a slice
CREATE TEMP TABLE sa_segs AS SELECT *, lead(lo) OVER (ORDER BY lo, hi) AS next_lo FROM sift._test_col_segment_keys('sa_idx') WHERE lo <= hi;
SELECT count(*) AS segments, sum(rows) AS rows FROM sa_segs;
SELECT count(*) AS split_values_that_fit
FROM sa_segs s JOIN LATERAL (SELECT count(*) AS n FROM sa WHERE dim = s.hi) k ON true
WHERE s.hi >= s.next_lo AND k.n * 3 <= (SELECT sum(rows) FROM sa_segs) / 3;

SELECT sift.col_verify('sa_idx', true);
SET enable_seqscan = off;
SET max_parallel_workers_per_gather = 0;
SELECT dim, count(*), sum(v), sum(w) FROM sa WHERE dim IN (1, 2, 3, 7) GROUP BY dim ORDER BY dim;
RESET enable_seqscan;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT dim, count(*), sum(v), sum(w) FROM sa WHERE dim IN (1, 2, 3, 7) GROUP BY dim ORDER BY dim;

DROP TABLE sa;

-- a first value of 44% of the rows is bigger than a slice and its slack: the first slice is cut inside it, at its exact point, the others between values, and no slice holds half the rows
CREATE TABLE sb (id int, dim int, day date, v int, w int) WITH (autovacuum_enabled = false);
INSERT INTO sb SELECT g, 1 + floor(12 * power(((g::bigint * 2654435761) % 1000003) / 1000003.0, 3))::int,
       date '2018-01-01' + (((g::bigint * 2654435761) % 4294967296 / 65536) % 2900)::int, g % 100, (g::bigint * 40503 % 1000003)::int
FROM generate_series(1, 150000) g;
VACUUM (FREEZE, ANALYZE) sb;
SET max_parallel_maintenance_workers = 0;
CREATE INDEX sb_idx ON sb USING sift_col (dim, day, v, w) WITH (key_cut = 'dim, day');
SET max_parallel_maintenance_workers = 2;
SELECT sift.col_merge('sb_idx', true) IS NOT NULL AS merged;
SHOW sift.test_col_merge_slices;
CREATE TEMP TABLE sb_segs AS SELECT *, lead(lo) OVER (ORDER BY lo, hi) AS next_lo FROM sift._test_col_segment_keys('sb_idx') WHERE lo <= hi;
SELECT count(*) AS segments, sum(rows) AS rows, max(rows) * 2 < sum(rows) AS none_holds_half FROM sb_segs;
SELECT count(*) AS values_in_two_segments FROM sb_segs WHERE hi >= next_lo;
SELECT sift.col_verify('sb_idx', true);
RESET enable_indexscan;
RESET enable_bitmapscan;
SELECT dim, count(*), sum(v) FROM sb WHERE dim IN (1, 2, 5) GROUP BY dim ORDER BY dim;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT dim, count(*), sum(v) FROM sb WHERE dim IN (1, 2, 5) GROUP BY dim ORDER BY dim;
DROP TABLE sb;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
