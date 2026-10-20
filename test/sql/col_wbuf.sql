-- The write buffer lanes of the column engine (src/col/wbuf.c): row records appended to the lanes of the active segment, sealing, the
-- reader, the sorted per-superblock batches, VACUUM's flags, torn rows, verification. The index is a sift_col index on an empty table
-- (a valid metapage and directory for the store layer); the test functions give it the schema hash of an orders table with line items.
-- Every row is checked in C against a generator, down to the bits of its floats and the bytes of its strings.
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
	RETURN regexp_replace(SQLSTATE || ': ' || SQLERRM || coalesce(' | ' || nullif(d, ''), '') || coalesce(' | ' || nullif(h, ''), ''), '[0-9]+', 'N', 'g');
END $$;

CREATE TABLE t (doc int);
CREATE INDEX t_idx ON t USING sift_col (doc) WITH (write_buffer_limit = 64);
SELECT count(*) AS segments FROM sift._test_col_wbuf_segments('t_idx');

-- a definition that is not the index's is refused with the REINDEX hint
SELECT sift._test_col_wbuf_prepare('t_idx', 1) IS NOT NULL AS stale;
SELECT expect($$ SELECT sift._test_col_wbuf_append('t_idx', 7, 0, 1) $$);
SELECT sift._test_col_wbuf_prepare('t_idx') IS NOT NULL AS prepared;

-- 3000 rows from this backend: one active segment, one lane, the segment found full (every 1000th row is a record of many pages)
SELECT sift._test_col_wbuf_append('t_idx', 7, 0, 3000) > 0 AS found_full;
SELECT kind, nrows, live, lanes, npages > 16 AS over_limit, npages = extents AS extents_cover
FROM sift._test_col_wbuf_segments('t_idx');
CREATE TABLE r AS SELECT * FROM sift._test_col_wbuf_rows('t_idx', 7);
SELECT count(*) AS records, count(DISTINCT id) AS ids, min(id), max(id), count(*) FILTER (WHERE deleted) AS deleted FROM r;
SELECT sift._test_check('t_idx') AS orphans;

-- batches: one per superblock in ascending order, keys ascending, the same rows
CREATE TABLE b AS SELECT * FROM sift._test_col_wbuf_batches('t_idx', 7);
SELECT count(*) AS rows, count(DISTINCT id) AS ids, count(DISTINCT sb) > 1 AS superblocks FROM b;
SELECT (SELECT count(*) FROM (SELECT id FROM r EXCEPT SELECT id FROM b) x) AS only_in_records,
	   (SELECT count(*) FROM (SELECT id FROM b EXCEPT SELECT id FROM r) x) AS only_in_batches;

-- sealing: above the limit only, then the sealed segment is read as before and a fresh one takes the appends
SELECT sift._test_col_wbuf_seal('t_idx') AS sealed;
SELECT sift._test_col_wbuf_seal('t_idx') AS sealed_again;
SELECT sift._test_col_wbuf_seal('t_idx', true) AS forced_empty;
SELECT kind, nrows, live FROM sift._test_col_wbuf_segments('t_idx') ORDER BY gen;
SELECT count(*) AS records, count(DISTINCT id) AS ids FROM sift._test_col_wbuf_rows('t_idx', 7);

-- appends that find the lane tails locked go to other lanes
SELECT sift._test_col_wbuf_append('t_idx', 7, 3000, 4000, true) >= 0 AS appended;
SELECT kind, nrows, live, lanes > 1 AS several_lanes FROM sift._test_col_wbuf_segments('t_idx') ORDER BY gen;
SELECT count(*) AS records, count(DISTINCT id) AS ids FROM sift._test_col_wbuf_rows('t_idx', 7);
SELECT sift._test_check('t_idx') AS orphans;

-- a big row spans pages and is read back whole
CREATE TABLE p AS SELECT npages FROM sift._test_col_wbuf_segments('t_idx') WHERE kind = 'active';
SELECT sift._test_col_wbuf_append('t_idx', 7, 8999, 1) >= 0 AS appended;
SELECT (SELECT npages FROM sift._test_col_wbuf_segments('t_idx') WHERE kind = 'active') - (SELECT npages FROM p) >= 3 AS several_pages;
SELECT count(*) AS found FROM sift._test_col_wbuf_rows('t_idx', 7) WHERE id = 8999;

-- a row whose writer died after some of its pieces is skipped, and the lane goes on
SELECT expect($$ SELECT sift._test_col_wbuf_torn('t_idx', 7, 9999, 1) $$);
SELECT expect($$ SELECT sift._test_col_wbuf_torn('t_idx', 7, 10999, 3) $$);
SELECT sift._test_col_wbuf_append('t_idx', 7, 11000, 50) >= 0 AS appended;
SELECT count(*) AS torn_seen FROM sift._test_col_wbuf_rows('t_idx', 7) WHERE id IN (9999, 10999);
SELECT count(*) AS torn_batched FROM sift._test_col_wbuf_batches('t_idx', 7) WHERE id IN (9999, 10999);
SELECT count(*) AS records, count(DISTINCT id) AS ids FROM sift._test_col_wbuf_rows('t_idx', 7) WHERE id >= 11000;
SELECT kind, live FROM sift._test_col_wbuf_segments('t_idx') ORDER BY gen;
SELECT sift._test_check('t_idx') AS orphans;

-- a repeated TID keeps its first record in the batches
SELECT sift._test_col_wbuf_append('t_idx', 7, 12000, 1) + sift._test_col_wbuf_append('t_idx', 7, 12000, 1) >= 0 AS twice;
SELECT (SELECT count(*) FROM sift._test_col_wbuf_rows('t_idx', 7) WHERE id = 12000) AS records,
	   (SELECT count(*) FROM sift._test_col_wbuf_batches('t_idx', 7) WHERE id = 12000) AS batched;

-- First fragments of torn rows depend on the randomized lane tail room. VACUUM must preserve their physical count.
CREATE TABLE counts AS SELECT gen, nrows FROM sift._test_col_wbuf_segments('t_idx');

-- VACUUM: the callback flags the dead rows once, in the sealed and the active segment; batches leave them out
SELECT sift._test_col_wbuf_delete('t_idx', 5) AS flagged;
SELECT sift._test_col_wbuf_delete('t_idx', 5) AS flagged_again;
SELECT count(*) AS records, count(*) FILTER (WHERE deleted) AS deleted, count(*) FILTER (WHERE deleted AND id % 5 <> 0) AS wrongly_deleted
FROM sift._test_col_wbuf_rows('t_idx', 7);
SELECT count(*) AS batched, count(*) FILTER (WHERE id % 5 = 0) AS dead_batched FROM sift._test_col_wbuf_batches('t_idx', 7);
SELECT s.kind, s.nrows = c.nrows AS same_nrows, s.live
FROM sift._test_col_wbuf_segments('t_idx') s JOIN counts c USING (gen) ORDER BY gen;
SELECT sift._test_check('t_idx') AS orphans;

-- a string of any length is held: a record of 300 000 bytes spans pages
SELECT sift._test_col_wbuf_long('t_idx');
SELECT sift._test_check('t_idx') AS orphans;

-- A full tail cannot hold even the first fragment. Failing after its page link leaves no new record;
-- failing again on the fresh empty tail leaves one first fragment, but neither interruption leaves a complete row.
CREATE TABLE t3 (doc int);
CREATE INDEX t3_idx ON t3 USING sift_col (doc) WITH (write_buffer_limit = 64);
SELECT sift._test_col_wbuf_prepare('t3_idx') IS NOT NULL AS prepared;
-- Seed 7, ids 0..160 fill the lane tail exactly, without randomizing busy lanes.
SELECT sift._test_col_wbuf_append('t3_idx', 7, 0, 161) >= 0 AS appended;
SELECT pg.upper = pg.lower AS tail_full
FROM generate_series(0, pg_relation_size('t3_idx') / 8192 - 1) b, LATERAL sift._test_page('t3_idx', b) pg
WHERE pg.type = 7 AND pg.lower > 32 ORDER BY b DESC LIMIT 1;
SELECT expect($$ SELECT sift._test_col_wbuf_torn('t3_idx', 7, 9999, 1) $$);
SELECT nrows, live FROM sift._test_col_wbuf_segments('t3_idx');
SELECT expect($$ SELECT sift._test_col_wbuf_torn('t3_idx', 7, 10999, 1) $$);
SELECT nrows, live FROM sift._test_col_wbuf_segments('t3_idx');
SELECT sift._test_col_wbuf_append('t3_idx', 7, 11000, 1) >= 0 AS appended;
SELECT nrows, live FROM sift._test_col_wbuf_segments('t3_idx');
SELECT count(*) AS records, count(*) FILTER (WHERE id IN (9999, 10999)) AS torn_seen
FROM sift._test_col_wbuf_rows('t3_idx', 7);
SELECT count(*) AS batched, count(*) FILTER (WHERE id IN (9999, 10999)) AS torn_batched
FROM sift._test_col_wbuf_batches('t3_idx', 7);
SELECT sift._test_check('t3_idx') AS orphans;

-- corruption is found: a page header count, a flag byte, the length of a record
CREATE TABLE t2 (doc text);
CREATE INDEX t2_idx ON t2 USING sift_col (doc) WITH (write_buffer_limit = 64);
SELECT sift._test_col_wbuf_prepare('t2_idx') IS NOT NULL AS prepared;
SELECT sift._test_col_wbuf_append('t2_idx', 3, 0, 20) >= 0 AS appended;
SELECT live FROM sift._test_col_wbuf_segments('t2_idx');
CREATE TABLE pages AS SELECT b AS blk, pg.type, pg.lower FROM generate_series(0, pg_relation_size('t2_idx') / 8192 - 1) b, LATERAL sift._test_page('t2_idx', b) pg;
-- offsets: the data starts at 24, the page head is nrecs at +4, flags at +6, the first record's head follows at +8 (flags at +6, len at +8)
SELECT min(blk) AS empty_page FROM pages WHERE type = 7 AND lower = 32 \gset
SELECT min(blk) AS rec_page FROM pages WHERE type = 7 AND lower > 32 \gset
SELECT sift._test_poke('t2_idx', :empty_page, 28, 5, 2);
SELECT expect($$ SELECT * FROM sift._test_col_wbuf_segments('t2_idx') $$);
SELECT sift._test_poke('t2_idx', :empty_page, 28, 0, 2);
SELECT sift._test_poke('t2_idx', :rec_page, 38, 8, 2);
SELECT expect($$ SELECT * FROM sift._test_col_wbuf_segments('t2_idx') $$);
SELECT sift._test_poke('t2_idx', :rec_page, 38, 0, 2);
SELECT sift._test_poke('t2_idx', :rec_page, 40, 100000, 4);
SELECT expect($$ SELECT * FROM sift._test_col_wbuf_segments('t2_idx') $$);
SELECT expect($$ SELECT * FROM sift._test_col_wbuf_rows('t2_idx', 3) $$);

DROP TABLE t, t2, t3, r, b, p, pages, counts;
DROP FUNCTION expect(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
