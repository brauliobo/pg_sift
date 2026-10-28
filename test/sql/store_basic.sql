-- Store round trips through the test access method and sift._test_* functions
CREATE EXTENSION pg_sift_test CASCADE;
CREATE FUNCTION info(idx regclass) RETURNS jsonb LANGUAGE sql
	AS $$ SELECT jsonb_object_agg(key, value) FROM sift._test_info(idx) $$;

-- Empty build: metapage + directory only
CREATE TABLE empty (v int);
CREATE INDEX empty_idx ON empty USING sift_store_test (v);
SELECT info('empty_idx');
SELECT * FROM sift._test_page('empty_idx', 0);
SELECT * FROM sift._test_page('empty_idx', 1);
SELECT sift._test_check('empty_idx');

-- BULK build: one segment per row, boundary sizes around the 8152-byte page payload
CREATE TABLE b (v int);
INSERT INTO b VALUES (0), (1), (8151), (8152), (8153), (16303), (16304), (16305),
	(130432), (130433), (1048576), (10485760);
CREATE INDEX b_idx ON b USING sift_store_test (v);
SELECT info('b_idx');
SELECT gen, size, nextents, sift._test_seg_read('b_idx', gen) = payload AS replay,
	sift._test_seg_read('b_idx', gen, 1) >= 0 AS random,
	sift._test_seg_read('b_idx', gen, 2) = size AS pages,
	sift._test_seg_read('b_idx', gen, 3) = size AS copies,
	sift._test_seg_read('b_idx', gen, 4) >= 0 AS ahead
FROM sift._test_segments('b_idx') ORDER BY gen;
SELECT sift._test_check('b_idx');
REINDEX INDEX b_idx;
SELECT count(*), bool_and(sift._test_seg_read('b_idx', gen) = payload) FROM sift._test_segments('b_idx');

-- SHARED writes: sizes x piece sizes x alignment
CREATE TABLE s (v int);
CREATE INDEX s_idx ON s USING sift_store_test (v);
CREATE TABLE w AS
SELECT size, piece, align, sift._test_seg_write('s_idx', size, size * 31 + piece * 7 + align + 1, piece, align) AS gen
FROM unnest(ARRAY[0, 1, 63, 64, 8151, 8152, 8153, 16304, 130432, 130433, 1048576]) size,
	(VALUES (0, 0), (1, 0), (8152, 0), (65536, 0), (0, 8), (0, 64), (7, 8), (100, 64), (8152, 64)) p(piece, align);
SELECT count(*) AS segments,
	bool_and(sift._test_seg_read('s_idx', gen) = w.size) AS replay,
	bool_and(w.align > 0 OR sift._test_seg_read('s_idx', gen, 1) >= 0) AS random,
	bool_and(sift._test_seg_read('s_idx', gen, 3) = t.size) AS copies
FROM w JOIN sift._test_segments('s_idx') t USING (gen);
-- padding only where alignment asked for it
SELECT w.align, bool_and(t.size = t.payload) AS unpadded
FROM w JOIN sift._test_segments('s_idx') t USING (gen) GROUP BY w.align ORDER BY 1;
SELECT sift._test_check('s_idx');

-- 100 MB stream: runs grow to 1024 blocks; contiguous runs merge into one extent
SELECT sift._test_seg_write('s_idx', 104857600, 99) AS big \gset
SELECT size, nextents FROM sift._test_segments('s_idx') WHERE gen = :big;
SELECT sift._test_seg_read('s_idx', :big) AS replay, sift._test_seg_read('s_idx', :big, 2) AS pages;

-- Drop: extents wait while the releasing transaction (whose xid is their
-- safexid) runs, then get reused instead of growing the index
SELECT (info('s_idx')->>'high_water')::int AS hwm \gset
BEGIN;
SELECT sift._test_seg_drop('s_idx', :big) AS released;
SELECT info('s_idx')->'pending_blocks' AS pending, sift._test_recycle('s_idx') AS recycled;
COMMIT;
SELECT sift._test_seg_drop('s_idx', :big) AS again;
SELECT info('s_idx')->'pending_blocks' AS pending, sift._test_recycle('s_idx') AS recycled;
SELECT sift._test_seg_write('s_idx', 104857600, 100) AS big2 \gset
SELECT sift._test_seg_read('s_idx', :big2, 2), (info('s_idx')->>'high_water')::int = :hwm AS no_growth;
SELECT sift._test_check('s_idx');

-- Fragmentation on a fresh index: 1200 single-page allocations, every second one
-- released (600 records, more than a free-list page holds), the rest swept as orphans
CREATE TABLE f (v int);
CREATE INDEX f_idx ON f USING sift_store_test (v);
SELECT sift._test_alloc('f_idx', 1, 1200, 2) AS last;
SELECT page, count(*), sum(nblocks), bool_and(pending) FROM sift._test_freelist('f_idx') GROUP BY page ORDER BY page;
SELECT sift._test_check('f_idx') AS orphans, sift._test_sweep('f_idx') AS swept, sift._test_check('f_idx') AS orphans;
SELECT info('f_idx')->'freelist_pages' AS freelist_pages;
-- recycle rewrites the list as one sorted chain and merges the interleaved records
SELECT sift._test_recycle('f_idx') AS recycled;
SELECT * FROM sift._test_freelist('f_idx');
SELECT sift._test_alloc('f_idx', 32, 37, 1) AS last, info('f_idx')->'high_water' AS high_water,
	sift._test_check('f_idx') AS orphans;

-- Every segment still reads back after all of the above
SELECT count(*), bool_and(sift._test_seg_read('s_idx', gen) = payload) FROM sift._test_segments('s_idx');

-- WAL: a SHARED write logs its pages; the directory switch is small
SELECT pg_current_wal_insert_lsn() AS l0 \gset
SELECT sift._test_seg_write('s_idx', 1048576, 5) AS g \gset
SELECT pg_current_wal_insert_lsn() AS l1 \gset
SELECT sift._test_seg_drop('s_idx', :g) > 0;
SELECT pg_current_wal_insert_lsn() AS l2 \gset
SELECT pg_wal_lsn_diff(:'l1', :'l0') > 1048576 AS logged, pg_wal_lsn_diff(:'l2', :'l1') < 16384 AS small;

DROP TABLE empty, b, s, w, f;
DROP FUNCTION info(regclass);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
