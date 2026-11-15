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
	RETURN regexp_replace(SQLSTATE || ': ' || SQLERRM || coalesce(' | ' || nullif(d, ''), '') || coalesce(' | ' || nullif(h, ''), ''), 'pg_sift [0-9]+(\.[0-9]+)+', 'pg_sift <version>');
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
-- the used bytes of the metapage (pd_lower at offset 12, the data at 24): the 64 of a build without the failure note, the 272 with it and the 400 with the analyzer configuration are read, a shorter one is not
SELECT fresh();
SELECT sift._test_poke('t_idx', 0, 12, 24 + 40, 2);
SELECT expect($$ SELECT sift._test_check('t_idx') $$);
-- a metapage longer than the struct of this build is the extension of a newer one: it is read as it is, and a format this build cannot read is refused by format with its hint
SELECT fresh();
SELECT sift._test_poke('t_idx', 0, 12, 24 + 500, 2);
SELECT sift._test_check('t_idx') >= 0 AS longer_metapage_is_read;
SELECT sift._test_poke('t_idx', 0, 30, 7, 2);
SELECT expect($$ SELECT sift._test_check('t_idx') $$);
-- a write keeps the bytes past the struct and never lowers pd_lower over them (a failure note, at offset 64, makes it set pd_lower to the struct); the base of the page is written as always
SELECT fresh();
SELECT sift._test_poke('t_idx', 0, 12, 24 + 500, 2);
SELECT sift._test_poke('t_idx', 0, 24 + 492, 123456789, 8);
SELECT sift._test_poke('t_idx', 0, 24 + 64, 1, 8);
SELECT sift._test_peek('t_idx', 0, 24 + 24, 8) AS next_gen \gset
SELECT sift._test_seg_write('t_idx', 100, 1) > 0 AS written;
SELECT lower = 24 + 500 AS lower_kept, sift._test_peek('t_idx', 0, 24 + 492, 8) = 123456789 AS tail_kept,
	sift._test_peek('t_idx', 0, 24 + 24, 8) > :next_gen AS base_written
FROM sift._test_page('t_idx', 0);
-- the same for an index of the BM25 access method
CREATE TABLE r (d text);
INSERT INTO r VALUES ('a b c'), ('b c d');
CREATE INDEX r_idx ON r USING sift (d);
SELECT sift._test_poke('r_idx', 0, 12, 24 + 700, 2);
SELECT sift._test_check('r_idx') >= 0 AS bm25_longer_metapage_is_read;
SELECT sift._test_poke('r_idx', 0, 30, 99, 2);
SELECT expect($$ SELECT sift._test_check('r_idx') $$);
DROP TABLE r;
SELECT fresh();
SELECT sift._test_poke('t_idx', 0, 12, 24 + 272, 2);
SELECT sift._test_check('t_idx') >= 0 AS note_size_is_read;
SELECT fresh();
SELECT sift._test_poke('t_idx', 0, 12, 24 + 400, 2);
SELECT sift._test_check('t_idx') >= 0 AS config_size_is_read;
SELECT fresh();
SELECT sift._test_poke('t_idx', 0, 28, 2, 2);
SELECT expect($$ SELECT sift._test_check('t_idx') $$);
SELECT fresh();
SELECT sift._test_poke('t_idx', 0, 30, 7, 2);
SELECT expect($$ SELECT sift._test_check('t_idx') $$);
SELECT fresh();
SELECT sift._test_poke('t_idx', 0, 30, 0, 2);
SELECT sift._test_check('t_idx') >= 0 AS format_1_0_is_read;
SELECT fresh();
SELECT sift._test_poke('t_idx', 0, 30, 1, 2);
SELECT sift._test_check('t_idx') >= 0 AS format_1_1_is_read;
SELECT fresh();
SELECT sift._test_poke('t_idx', 0, 30, 3, 2);
SELECT sift._test_check('t_idx') >= 0 AS format_1_3_is_read;
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
