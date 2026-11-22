-- The scope directory of format major 2: a copy on write chain of pages the metapage names, with no ceiling on the number of scopes.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse
CREATE FUNCTION expect(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
	RETURN SQLSTATE || ' ' || SQLERRM;
END $$;

CREATE TABLE t (v int);
CREATE INDEX t_idx ON t USING sift_store_test (v);
SELECT sift._test_f2_init('t_idx');
SELECT sift._test_f2_add_part('t_idx', 1, NULL, 100, 0, 1) > 0 AS added;
SELECT struct_size FROM sift._test_f2_meta('t_idx');

-- No scope: no page read, no row
SELECT count(*) AS scopes FROM sift._test_f2_scope_dir('t_idx');

-- More scopes than the four of the first layout, kept in order whatever the order they come in
SELECT sift._test_f2_scopes('t_idx', 10, 6) > 0 AS six;
SELECT sift._test_f2_scopes('t_idx', 0, 3) > 0 AS three_more;
SELECT scope_ord, flags, header_blk, meta_gen FROM sift._test_f2_scope_dir('t_idx');

-- A ref of an ordinal that is there is replaced, not added
SELECT sift._test_f2_scopes('t_idx', 12, 1) > 0 AS replaced;
SELECT count(*) AS scopes, min(scope_ord) AS lo, max(scope_ord) AS hi FROM sift._test_f2_scope_dir('t_idx');

-- One page holds 507 refs: 1200 scopes are a chain of three pages
SELECT sift._test_f2_scopes('t_idx', 100, 1200) > 0 AS many;
SELECT count(*) AS scopes, min(scope_ord) AS lo, max(scope_ord) AS hi, bool_and(flags = 1) AS active,
	   bool_and(header_blk = 1000 + scope_ord) AS blocks, bool_and(meta_gen = 7 + scope_ord) AS gens FROM sift._test_f2_scope_dir('t_idx');
SELECT sift._test_f2_check('t_idx');

-- The pages of the directories a commit replaced are queued for reuse, and the allocator is whole
SELECT sift._test_f2_fsm('t_idx') IS NOT NULL AS fsm_ok;
SELECT sift._test_f2_reclaim('t_idx') >= 0 AS reclaimed;
SELECT sift._test_f2_check('t_idx');

-- The largest ordinal is 65534
SELECT sift._test_f2_scopes('t_idx', 65534, 1) > 0 AS last_ord;
SELECT regexp_replace(expect('SELECT sift._test_f2_scopes(''t_idx'', 65535, 1)'), 'index "[^"]*"', 'index X');
SELECT count(*) AS scopes, max(scope_ord) AS hi FROM sift._test_f2_scope_dir('t_idx');

DROP TABLE t;
DROP FUNCTION expect(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
