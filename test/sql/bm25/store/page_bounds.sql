CREATE EXTENSION pg_sift_test CASCADE;

-- The header page of a segment holds its extents and the bytes after them (the mirror of the header). A count of extents or a length of extra bytes that does not fit
-- the page is refused before a byte is copied: the largest count and the longest extra bytes are written, one more of either is an error.
CREATE TABLE t (v int);
CREATE INDEX t_idx ON t USING sift_store_test (v);

SELECT used AS full, 8152 - used AS spare FROM sift._test_seghdr('t_idx', 0, 0) \gset
SELECT :spare < 8 AS spare_below_one_extent;

SELECT 'max extents' AS what, used = :full AS fits FROM sift._test_seghdr('t_idx', 0, 0);
SELECT 'one extent fewer, 8 bytes after' AS what, used = :full AS fits FROM sift._test_seghdr('t_idx', -1, 8);
SELECT 'extra bytes to the end of the page' AS what, used = 8152 AS fits FROM sift._test_seghdr('t_idx', -10, 80 + :spare);

SELECT used FROM sift._test_seghdr('t_idx', 1, 0);
SELECT used FROM sift._test_seghdr('t_idx', 0, :spare + 1);
SELECT used FROM sift._test_seghdr('t_idx', -10, 80 + :spare + 1);
SELECT used FROM sift._test_seghdr('t_idx', 100000, 0);

DROP TABLE t;

-- A change of a page that reaches the special space, the bytes past pd_lower or the page header is refused before it is logged, whichever
-- offset computed it; a change inside the payload goes through
CREATE TABLE t (v int);
CREATE INDEX t_idx ON t USING sift_store_test (v);

CREATE FUNCTION expect(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	d text;
BEGIN
	EXECUTE q;
	RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
	GET STACKED DIAGNOSTICS d = PG_EXCEPTION_DETAIL;
	RETURN SQLSTATE || ': ' || SQLERRM || ' | ' || d;
END $$;
SELECT lower, upper FROM sift._test_page('t_idx', 1);

-- block 1 is the directory: version, nsegs and the flags are in its payload, 8152 bytes from offset 24
SELECT w.what, expect(format($$ SELECT sift._test_write('t_idx', 1, %s, %s) $$, w.off, w.len))
FROM (VALUES ('payload, in use', 24, 2), ('last payload byte, past pd_lower', 8175, 1), ('across the end of the payload', 8174, 4),
			 ('the generation word', 8184, 1), ('past pd_lower', 200, 4), ('pd_lower', 12, 2), ('pd_upper', 14, 2),
			 ('pd_special', 16, 2)) w (what, off, len);

-- an append edit inside the head it logs goes through (past it, or in the special space, the server restarts: test/t/bm25/store/edit_guard.sh)
SELECT expect($$ SELECT sift._test_write('t_idx', 1, 24, 2, true) $$);

-- the refused changes left the page as it was
SELECT lower, upper, type, gen FROM sift._test_page('t_idx', 1);
SELECT sift._test_check('t_idx');

DROP TABLE t;
DROP FUNCTION expect(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
