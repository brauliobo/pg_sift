-- Exact relocation reservations share one budget across rounds. No merge/promotion callback can add writes to a move.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET sift.vacuum_rewrite_limit = 1;
CREATE TABLE t (v int);
CREATE INDEX i ON t USING sift_store_test(v);
-- A large hole before each tail segment makes one move profitable per round. Resetting the 1 MiB allowance between rounds
-- copies all three 600 kB segments; sharing the remaining allowance admits just one.
SELECT sift._test_alloc('i', 512, 1, 0) > 0 AS reserved;
SELECT sift._test_seg_write('i', 600000, 17) AS a \gset
SELECT sift._test_alloc('i', 512, 1, 0) > 0 AS reserved;
SELECT sift._test_seg_write('i', 600000, 18) AS b \gset
SELECT sift._test_alloc('i', 512, 1, 0) > 0 AS reserved;
SELECT sift._test_seg_write('i', 600000, 19) AS c \gset
SELECT sift._test_sweep('i') > 0 AS freed;
SELECT sift._test_space('i') AS copied_pages \gset
SELECT :copied_pages BETWEEN 1 AND 128 AS bounded;
SELECT bool_and(sift._test_seg_read('i', gen) = payload) AS intact FROM sift._test_segments('i');
SELECT sift._test_check('i') = 0 AS accounted;
-- Zero disables moves, while retaining ordinary free-tail truncation.
SET sift.vacuum_rewrite_limit = 0;
SELECT sift._test_space('i') = 0 AS no_copy;
DROP INDEX i;
CREATE INDEX i ON t USING sift_store_test(v);
-- Fragmented 16-block holes and a larger tail segment. Split DATA extents across the smaller holes while preserving their logical
-- order; every reservation is planned before copying and the allocator cannot extend high_water.
SELECT sift._test_alloc('i', 16, 800, 2) > 0 AS fragmented;
SELECT sift._test_seg_write('i', 40 << 20, 23) AS tail \gset
CREATE TEMP TABLE size AS SELECT value FROM sift._test_info('i') WHERE key = 'high_water';
SET sift.vacuum_rewrite_limit = 128;
SELECT sift._test_space('i') > 0 AS attempted;
SELECT value <= (SELECT value FROM size) AS no_growth FROM sift._test_info('i') WHERE key = 'high_water';
SELECT bool_and(sift._test_seg_read('i', gen) = payload) AS intact FROM sift._test_segments('i');
SELECT sift._test_sweep('i') > 0 AS swept;
SELECT sift._test_space('i') >= 0 AS reclaimed;
SELECT sift._test_check('i') = 0 AS accounted;
-- A front hole only slightly larger than the segment is still worth reclaiming. Destination occupancy is already counted in the
-- resulting file end: charging the copy a second time makes the predicted benefit too small and rejects the move.
DROP INDEX i;
CREATE INDEX i ON t USING sift_store_test(v);
SELECT sift._test_alloc('i', 680, 1, 0) > 0 AS reserved;
SELECT sift._test_seg_write('i', 5 << 20, 29) AS tail \gset
SELECT sift._test_sweep('i') > 0 AS freed;
SELECT sift._test_space('i') > 0 AS profitable;
SELECT bool_and(sift._test_seg_read('i', gen) = payload) AS intact FROM sift._test_segments('i');
SELECT sift._test_check('i') = 0 AS accounted;
DROP TABLE t, size;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
