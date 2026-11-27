# SERIALIZABLE on a table with a sift_col index, of Format 2.0 (iso_sh2) and of Format 1 (iso_sh1), through the scans col/ssi does not: a plain index scan that finds no row,
# then a write of the mark the other transaction read, is a write skew with the insert of that key (index_beginscan takes the SIREAD lock of the index, which index_insert checks);
# an index-only scan that counts rows of all-visible pages is one with the delete of such a row (the scan locks the heap page it did not read). Each fails with 40001.
# requires: col

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
	SET sift.test_create_format1 = off;
	CREATE TABLE iso_sh2 (id int, k int) WITH (autovacuum_enabled = false);
	INSERT INTO iso_sh2 SELECT g, g FROM generate_series(1, 2000) g;
	CREATE INDEX iso_sh2_idx ON iso_sh2 USING sift_col (k, id);
	SET sift.test_create_format1 = on;
	CREATE TABLE iso_sh1 (id int, k int) WITH (autovacuum_enabled = false);
	INSERT INTO iso_sh1 SELECT g, g FROM generate_series(1, 2000) g;
	CREATE INDEX iso_sh1_idx ON iso_sh1 USING sift_col (k, id);
	CREATE TABLE iso_shm (n int);
	INSERT INTO iso_shm VALUES (0);
}

setup
{
	VACUUM (FREEZE, ANALYZE) iso_sh2, iso_sh1;
}

teardown
{
	DROP TABLE iso_sh2, iso_sh1, iso_shm;
}

session s1
setup		{ SET enable_seqscan = off; SET enable_bitmapscan = off; SET max_parallel_workers_per_gather = 0; SET sift.enable_agg = off; }
step s1b	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1ix2	{ SET LOCAL enable_indexonlyscan = off; SELECT id FROM iso_sh2 WHERE k = 5000; }
step s1ix1	{ SET LOCAL enable_indexonlyscan = off; SELECT id FROM iso_sh1 WHERE k = 5000; }
step s1io2	{ SELECT count(k) FROM iso_sh2 WHERE k BETWEEN 5 AND 6; }
step s1io1	{ SELECT count(k) FROM iso_sh1 WHERE k BETWEEN 5 AND 6; }
step s1w	{ UPDATE iso_shm SET n = 1; }
step s1c	{ COMMIT; }

session s2
step s2b	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SELECT n FROM iso_shm; }
step s2i2	{ INSERT INTO iso_sh2 VALUES (5001, 5000); }
step s2i1	{ INSERT INTO iso_sh1 VALUES (5001, 5000); }
step s2d2	{ DELETE FROM iso_sh2 WHERE id = 5; }
step s2d1	{ DELETE FROM iso_sh1 WHERE id = 5; }
step s2c	{ COMMIT; }

permutation s1b s1ix2 s2b s2i2 s2c s1w s1c
permutation s1b s1ix1 s2b s2i1 s2c s1w s1c
permutation s1b s1io2 s2b s2d2 s2c s1w s1c
permutation s1b s1io1 s2b s2d1 s2c s1w s1c
