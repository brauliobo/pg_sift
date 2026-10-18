-- Format violations raise ERRCODE_INDEX_CORRUPTED (XX002) with a precise detail
CREATE EXTENSION pg_sift_test CASCADE;
CREATE FUNCTION expect(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	d text;
	h text;
BEGIN
	EXECUTE q;
	RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
	GET STACKED DIAGNOSTICS d = PG_EXCEPTION_DETAIL, h = PG_EXCEPTION_HINT;
	RETURN SQLSTATE || ': ' || SQLERRM || coalesce(' | ' || nullif(d, ''), '') || coalesce(' | ' || nullif(h, ''), '');
END $$;

-- Fresh index: segment 1 = 20000 bytes (header block 5, data 2-4), segment 2 = 100 bytes
CREATE TABLE t (v int);
INSERT INTO t VALUES (20000), (100);
CREATE FUNCTION fresh() RETURNS void LANGUAGE sql AS $$ REINDEX INDEX t_idx $$;
CREATE INDEX t_idx ON t USING sift_store_test (v);
SELECT gen, header, size, nextents FROM sift._test_segments('t_idx');
SELECT expect($$ SELECT sift._test_check('t_idx') $$);

-- Data page type in the special space
SELECT sift._test_poke('t_idx', 3, 8176, 9, 2);
SELECT expect($$ SELECT sift._test_seg_read('t_idx', 1) $$);
-- Data page generation
SELECT fresh();
SELECT sift._test_poke('t_idx', 4, 8184, 77, 8);
SELECT expect($$ SELECT sift._test_seg_read('t_idx', 1) $$);
-- Truncated page: pd_lower below the bytes the stream needs
SELECT fresh();
SELECT sift._test_poke('t_idx', 4, 12, 1000, 2);
SELECT expect($$ SELECT sift._test_seg_read('t_idx', 1) $$);
-- Broken layout: pd_upper moved
SELECT fresh();
SELECT sift._test_poke('t_idx', 2, 14, 8000, 2);
SELECT expect($$ SELECT sift._test_seg_read('t_idx', 1, 2) $$);
-- Segment header type
SELECT fresh();
SELECT sift._test_poke('t_idx', 5, 8176, 4, 2);
SELECT expect($$ SELECT sift._test_seg_read('t_idx', 1) $$);

-- Metapage magic, format version, high-water mark
SELECT fresh();
SELECT sift._test_poke('t_idx', 0, 24, 0, 4);
SELECT expect($$ SELECT sift._test_seg_read('t_idx', 1) $$);
SELECT fresh();
SELECT sift._test_poke('t_idx', 0, 28, 2, 2);
SELECT expect($$ SELECT sift._test_check('t_idx') $$);
SELECT fresh();
SELECT sift._test_poke('t_idx', 0, 30, 3, 2);
SELECT expect($$ SELECT sift._test_check('t_idx') $$);
SELECT fresh();
SELECT sift._test_poke('t_idx', 0, 30, 0, 2);
SELECT sift._test_check('t_idx') >= 0 AS format_1_0_is_read;
SELECT fresh();
SELECT sift._test_poke('t_idx', 0, 30, 1, 2);
SELECT sift._test_check('t_idx') >= 0 AS format_1_1_is_read;
SELECT fresh();
SELECT sift._test_poke('t_idx', 0, 76, 1, 4);
SELECT expect($$ SELECT sift._test_check('t_idx') $$);
SELECT fresh();
SELECT sift._test_poke('t_idx', 0, 76, 100, 4);
SELECT expect($$ SELECT sift._test_check('t_idx') $$);

-- Directory: segment count and entry generation
SELECT fresh();
SELECT sift._test_poke('t_idx', 1, 28, 129, 2);
SELECT expect($$ SELECT sift._test_check('t_idx') $$);
SELECT fresh();
SELECT sift._test_poke('t_idx', 1, 32 + 56, 1, 8);
SELECT expect($$ SELECT sift._test_check('t_idx') $$);

-- Segment header extent past the high-water mark, and overlapping another segment
SELECT fresh();
SELECT sift._test_poke('t_idx', 5, 160, 50, 4);
SELECT expect($$ SELECT sift._test_check('t_idx') $$);
SELECT fresh();
SELECT sift._test_poke('t_idx', 5, 156, 4, 4);
SELECT expect($$ SELECT sift._test_check('t_idx') $$);

-- Free list: a record covering a live block is caught by check and by a release
SELECT fresh();
SELECT sift._test_seg_write('t_idx', 1, 1) AS g \gset
SELECT sift._test_seg_drop('t_idx', :g);
SELECT key, value FROM sift._test_info('t_idx') WHERE key IN ('freelist_pages', 'free_extents');
SELECT value AS fl FROM sift._test_info('t_idx') WHERE key = 'freelist_head' \gset
SELECT (sift._test_page('t_idx', :fl)).type AS fl_type;
SELECT sift._test_poke('t_idx', :fl, 32, 5, 4);
SELECT expect($$ SELECT sift._test_check('t_idx') $$);
SELECT expect($$ SELECT sift._test_seg_drop('t_idx', 1) $$);
-- Free list record past the high-water mark, and a chain pointing at itself
SELECT sift._test_poke('t_idx', :fl, 32, 5000, 4);
SELECT expect($$ SELECT sift._test_alloc('t_idx', 1, 1) $$);
SELECT sift._test_poke('t_idx', :fl, 28, 0, 4), sift._test_poke('t_idx', :fl, 12, 32, 2);
SELECT sift._test_poke('t_idx', :fl, 24, :fl, 4);
SELECT expect($$ SELECT sift._test_check('t_idx') $$);
SELECT expect($$ SELECT sift._test_alloc('t_idx', 1, 1) $$);

-- A fresh build is clean again
SELECT fresh();
SELECT expect($$ SELECT sift._test_check('t_idx') $$);

DROP TABLE t;
DROP FUNCTION expect(text), fresh();
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
