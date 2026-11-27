# SERIALIZABLE write skew through the primary key of a sift_table of format major 2: a shift has two doctors, and each of two transactions takes one off when it reads two on the
# shift. Whichever way the shift is read (a sequential, an index, an index only or a bitmap scan, or the index scans of a cursor that began before the other took its doctor off),
# one of the two fails with 40001 and the shift keeps a doctor. A delete is staged until its transaction writes it, and its writer checks for the SIREAD locks of the pages of
# its rows there: a reader that locked one after the delete, before it was written, did not see it.
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	DO $$ BEGIN
		IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN
			PERFORM set_config('sift.test_create_format1', 'off', false);
		END IF;
	END $$;
	CREATE TABLE p2 (shift int, doc int, PRIMARY KEY (shift, doc)) USING sift_table WITH (autovacuum_enabled = off);
	INSERT INTO p2 SELECT s, d FROM generate_series(0, 999) s, generate_series(1, 2) d;
	ANALYZE p2;
}

setup
{
	VACUUM p2;
}

teardown
{
	DROP TABLE p2;
}

session s1
setup		{ SET sift.enable_agg = off; }
step s1seq	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SET LOCAL enable_indexscan = off; SET LOCAL enable_indexonlyscan = off; SET LOCAL enable_bitmapscan = off; }
step s1idx	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SET LOCAL enable_seqscan = off; SET LOCAL enable_indexonlyscan = off; SET LOCAL enable_bitmapscan = off; }
step s1ios	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; }
step s1bmp	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_indexonlyscan = off; }
step s1off	{ DELETE FROM p2 WHERE shift = 7 AND doc = 1 AND (SELECT count(*) FROM p2 WHERE shift = 7) >= 2; }
step s1c	{ COMMIT; }

session s2
setup		{ SET sift.enable_agg = off; SET enable_hashjoin = off; SET enable_mergejoin = off; SET enable_material = off; SET enable_memoize = off; }
step s2seq	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SET LOCAL enable_indexscan = off; SET LOCAL enable_indexonlyscan = off; SET LOCAL enable_bitmapscan = off; }
step s2idx	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SET LOCAL enable_seqscan = off; SET LOCAL enable_indexonlyscan = off; SET LOCAL enable_bitmapscan = off; }
step s2ios	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; }
step s2bmp	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_indexonlyscan = off; }
step s2off	{ DELETE FROM p2 WHERE shift = 7 AND doc = 2 AND (SELECT count(*) FROM p2 WHERE shift = 7) >= 2; }
step s2dec	{ DECLARE c CURSOR FOR SELECT o.s, p.doc FROM (VALUES (-1), (7)) o(s) LEFT JOIN p2 p ON p.shift = o.s; FETCH 1 FROM c; }
step s2rest	{ FETCH ALL FROM c; }
step s2del	{ DELETE FROM p2 WHERE shift = 7 AND doc = 2; }
step s2c	{ COMMIT; }

session s3
step s3n	{ SELECT count(*) AS on_shift FROM p2 WHERE shift = 7; }

permutation s1seq s2seq s1off s2off s1c s2c s3n
permutation s1idx s2idx s1off s2off s1c s2c s3n
permutation s1ios s2ios s1off s2off s1c s2c s3n
permutation s1bmp s2bmp s1off s2off s1c s2c s3n
permutation s2idx s2dec s1idx s1off s1c s2rest s2del s2c s3n
permutation s2ios s2dec s1idx s1off s1c s2rest s2del s2c s3n
