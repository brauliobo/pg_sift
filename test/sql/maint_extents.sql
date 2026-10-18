-- The runs of a segment stream grow with it, so a segment of any size fits the extents its header page
-- lists (about 1000): 8 GB took them all when runs stopped at 1024 blocks and another writer allocated
-- between two of them. Here the cap is 45 extents (sift.test_max_extents), a page taken by another
-- writer follows every run (sift.test_run_gap) and the segment is 400 MB: 55 extents of a fixed 1024
-- blocks, 41 of growing runs.
CREATE EXTENSION pg_sift_test CASCADE;
CREATE UNLOGGED TABLE ex (v int);
CREATE INDEX ex_idx ON ex USING sift_store_test (v);
SET sift.test_run_gap = on;
SET sift.test_max_extents = 45;
SELECT sift._test_seg_write('ex_idx', 400::bigint << 20, 0) > 0 AS written;
SELECT nextents BETWEEN 8 AND 45 AS within_cap, size >> 20 AS mb FROM sift._test_segments('ex_idx');
RESET sift.test_run_gap;
SELECT sift._test_seg_read('ex_idx', gen, 2) = payload AS reads_back FROM sift._test_segments('ex_idx');
SELECT sift._test_check('ex_idx') >= 0 AS consistent;
DROP TABLE ex;
DROP EXTENSION pg_sift_test;
