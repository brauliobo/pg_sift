# SERIALIZABLE write skew through the primary key of a sift_table of format major 1 (table/f2/ssi_pk_skew for format major 2): each of two transactions takes a doctor off a
# shift when it reads two on it, whichever way the shift is read; one of them fails with 40001. The index fetch of a cursor reads its tombstones when it begins, before its first
# fetch takes the SIREAD lock of the table: a delete in between neither meets the lock nor is in the tombstones read, unless the fetch reads them again once it holds the lock.
# requires: tam gates

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	SET sift.test_create_format1 = on;
	CREATE TABLE p1 (shift int, doc int, PRIMARY KEY (shift, doc)) USING sift_table WITH (autovacuum_enabled = off);
	INSERT INTO p1 SELECT s, d FROM generate_series(0, 999) s, generate_series(1, 2) d;
	ANALYZE p1;
}

setup
{
	VACUUM p1;
}

teardown
{
	DROP TABLE p1;
}

session s1
setup		{ SET sift.enable_agg = off; }
step s1seq	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SET LOCAL enable_indexscan = off; SET LOCAL enable_indexonlyscan = off; SET LOCAL enable_bitmapscan = off; }
step s1idx	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SET LOCAL enable_seqscan = off; SET LOCAL enable_indexonlyscan = off; SET LOCAL enable_bitmapscan = off; }
step s1ios	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; }
step s1bmp	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_indexonlyscan = off; }
step s1off	{ DELETE FROM p1 WHERE shift = 7 AND doc = 1 AND (SELECT count(*) FROM p1 WHERE shift = 7) >= 2; }
step s1c	{ COMMIT; }

session s2
setup		{ SET sift.enable_agg = off; SET enable_hashjoin = off; SET enable_mergejoin = off; SET enable_material = off; SET enable_memoize = off; }
step s2seq	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SET LOCAL enable_indexscan = off; SET LOCAL enable_indexonlyscan = off; SET LOCAL enable_bitmapscan = off; }
step s2idx	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SET LOCAL enable_seqscan = off; SET LOCAL enable_indexonlyscan = off; SET LOCAL enable_bitmapscan = off; }
step s2ios	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; }
step s2bmp	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_indexonlyscan = off; }
step s2off	{ DELETE FROM p1 WHERE shift = 7 AND doc = 2 AND (SELECT count(*) FROM p1 WHERE shift = 7) >= 2; }
step s2dec	{ DECLARE c CURSOR FOR SELECT o.s, p.doc FROM (VALUES (-1), (7)) o(s) LEFT JOIN p1 p ON p.shift = o.s; FETCH 1 FROM c; }
step s2rest	{ FETCH ALL FROM c; }
step s2del	{ DELETE FROM p1 WHERE shift = 7 AND doc = 2; }
step s2c	{ COMMIT; }

session s3
step s3n	{ SELECT count(*) AS on_shift FROM p1 WHERE shift = 7; }

permutation s1seq s2seq s1off s2off s1c s2c s3n
permutation s1idx s2idx s1off s2off s1c s2c s3n
permutation s1ios s2ios s1off s2off s1c s2c s3n
permutation s1bmp s2bmp s1off s2off s1c s2c s3n
permutation s2idx s2dec s1idx s1off s1c s2rest s2del s2c s3n
permutation s2ios s2dec s1idx s1off s1c s2rest s2del s2c s3n
