-- Truncation of the free blocks at the end of the file (sift_store.h P7): only reusable blocks go, the free list and the
-- high-water mark follow the file, and what is live is untouched.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
CREATE FUNCTION info(idx regclass) RETURNS jsonb LANGUAGE sql
	AS $$ SELECT jsonb_object_agg(key, value) FROM sift._test_info(idx) $$;
-- the file ends where the store says (nblocks = high_water) and every block is accounted for
CREATE FUNCTION sized(idx regclass) RETURNS bool LANGUAGE sql
	AS $$ SELECT info(idx)->'nblocks' = info(idx)->'high_water' AND sift._test_check(idx) = 0 $$;

CREATE TABLE t (v int);
CREATE INDEX t_idx ON t USING sift_store_test (v);
-- nothing free: nothing to give back
SELECT sift._test_truncate('t_idx') AS freed, sized('t_idx');

-- four segments of 4 MB in a row
SELECT sift._test_seg_write('t_idx', 4 << 20, 1) AS a \gset
SELECT sift._test_seg_write('t_idx', 4 << 20, 2) AS b \gset
SELECT sift._test_seg_write('t_idx', 4 << 20, 3) AS c \gset
SELECT sift._test_seg_write('t_idx', 4 << 20, 4) AS d \gset
-- the writes extended the file in runs and gave back the unused end of the last one: that is cut first
SELECT sift._test_truncate('t_idx') > 0 AS slack, sized('t_idx');
SELECT (info('t_idx')->>'high_water')::int AS full \gset

-- the last two dropped: their blocks wait for the end of the dropping transaction (P4), so none goes yet
BEGIN;
SELECT sift._test_seg_drop('t_idx', :c) > 0 AS dropped_c, sift._test_seg_drop('t_idx', :d) > 0 AS dropped_d;
SELECT sift._test_truncate('t_idx') AS freed, (info('t_idx')->>'nblocks')::int = :full AS file_unchanged;
COMMIT;
SELECT sift._test_truncate('t_idx') > 2 * (4 << 20) / 8192 AS freed_both, sized('t_idx');
SELECT (info('t_idx')->>'nblocks')::int < :full * 0.6 AS half_the_file;
SELECT sift._test_seg_read('t_idx', :a) = (SELECT payload FROM sift._test_segments('t_idx') WHERE gen = :a) AS a_reads,
	sift._test_seg_read('t_idx', :b) = (SELECT payload FROM sift._test_segments('t_idx') WHERE gen = :b) AS b_reads;
-- again: nothing more to give
SELECT sift._test_truncate('t_idx') AS freed, sized('t_idx');

-- free space in front of a live segment is not the end of the file: it is reused, not truncated
SELECT sift._test_seg_drop('t_idx', :a) > 0 AS dropped_a;
SELECT sift._test_truncate('t_idx') AS freed, (info('t_idx')->>'nblocks')::int = (info('t_idx')->>'high_water')::int AS sized;
SELECT (info('t_idx')->>'high_water')::int AS hwm \gset
SELECT sift._test_seg_write('t_idx', 2 << 20, 5) AS e \gset
SELECT (info('t_idx')->>'high_water')::int = :hwm AS no_growth, sift._test_seg_read('t_idx', :e) > 0 AS e_reads;
SELECT sized('t_idx');

-- the blocks that grew the file again are given back too, and the file can grow after that
SELECT sift._test_seg_drop('t_idx', :e) > 0 AS dropped_e;
SELECT sift._test_truncate('t_idx') > 0 AS freed, sized('t_idx');
SELECT sift._test_seg_write('t_idx', 8 << 20, 6) AS g \gset
SELECT sift._test_seg_read('t_idx', :g) = (SELECT payload FROM sift._test_segments('t_idx') WHERE gen = :g) AS g_reads, sized('t_idx');

-- unreferenced blocks (a crash between allocation and publication) stop the truncation until swept
SELECT sift._test_alloc('t_idx', 8, 300, 0) > 0 AS leaked;
SELECT sift._test_truncate('t_idx') AS freed, sift._test_check('t_idx') > 0 AS orphans;
SELECT sift._test_sweep('t_idx') > 0 AS swept;
SELECT sift._test_truncate('t_idx') > 2000 AS freed, sized('t_idx');

-- a free list of many pages over a file that is all free but one segment
SELECT sift._test_alloc('t_idx', 1, 1200, 2) > 0 AS fragmented;
SELECT (info('t_idx')->>'freelist_pages')::int > 1 AS list_pages;
SELECT sift._test_sweep('t_idx') >= 0 AS swept;
SELECT sift._test_truncate('t_idx') > 1000 AS freed, sized('t_idx');
SELECT sift._test_seg_drop('t_idx', :g) > 0 AS dropped_g;
SELECT sift._test_truncate('t_idx') > 0 AS freed, sized('t_idx');
SELECT (info('t_idx')->>'nblocks')::int < :full * 0.45 AS only_b_left;
SELECT sift._test_alloc('t_idx', 4, 10, 1) > 0 AS reused;
SELECT sized('t_idx');

-- an unlogged index is cut the same way
CREATE UNLOGGED TABLE ul (v int);
CREATE INDEX ul_idx ON ul USING sift_store_test (v);
SELECT sift._test_alloc('ul_idx', 8, 300, 1) > 0 AS allocated;
SELECT sift._test_truncate('ul_idx') > 2000 AS freed, sized('ul_idx');

DROP TABLE t, ul;
DROP EXTENSION pg_sift_test;
