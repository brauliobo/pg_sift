-- A change of a page that reaches the special space, the bytes past pd_lower or the page header is refused before it is logged, whichever
-- offset computed it; a change inside the payload goes through
CREATE EXTENSION pg_sift_test CASCADE;
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
CREATE TABLE t (v int);
CREATE INDEX t_idx ON t USING sift_store_test (v);
SELECT lower, upper FROM sift._test_page('t_idx', 1);

-- block 1 is the directory: version, nsegs and reserved are in its payload, 8152 bytes from offset 24
SELECT w.what, expect(format($$ SELECT sift._test_write('t_idx', 1, %s, %s) $$, w.off, w.len))
FROM (VALUES ('payload, in use', 30, 2), ('last payload byte, past pd_lower', 8175, 1), ('across the end of the payload', 8174, 4),
			 ('the generation word', 8184, 1), ('past pd_lower', 200, 4), ('pd_lower', 12, 2), ('pd_upper', 14, 2),
			 ('pd_special', 16, 2)) w (what, off, len);

-- an append edit inside the head it logs goes through (past it, or in the special space, the server restarts: test/t/store_edit_guard.sh)
SELECT expect($$ SELECT sift._test_write('t_idx', 1, 30, 2, true) $$);

-- the refused changes left the page as it was
SELECT lower, upper, type, gen FROM sift._test_page('t_idx', 1);
SELECT sift._test_check('t_idx');

DROP TABLE t;
DROP FUNCTION expect(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
