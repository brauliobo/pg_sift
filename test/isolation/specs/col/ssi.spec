# SERIALIZABLE on a table with a sift_col index, of Format 2.0 (iso_cs2) and of Format 1 (iso_cs1): two transactions that each look for a key no row has and then insert it are a
# write skew, and the second one to commit fails with 40001, whether the look is an index-only scan or a bitmap scan (index_beginscan takes the SIREAD lock of
# the index, which has no predicate locks of its own).
# requires: col

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
	SET sift.test_create_format1 = off;
	CREATE TABLE iso_cs2 (id int, k int) WITH (autovacuum_enabled = false);
	INSERT INTO iso_cs2 SELECT g, g FROM generate_series(1, 2000) g;
	CREATE INDEX iso_cs2_idx ON iso_cs2 USING sift_col (k, id);
	SET sift.test_create_format1 = on;
	CREATE TABLE iso_cs1 (id int, k int) WITH (autovacuum_enabled = false);
	INSERT INTO iso_cs1 SELECT g, g FROM generate_series(1, 2000) g;
	CREATE INDEX iso_cs1_idx ON iso_cs1 USING sift_col (k, id);
}

setup
{
	VACUUM (FREEZE, ANALYZE) iso_cs2, iso_cs1;
}

teardown
{
	DROP TABLE iso_cs2, iso_cs1;
}

session s1
setup		{ SET enable_seqscan = off; SET max_parallel_workers_per_gather = 0; }
step s1b	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1io2	{ SET LOCAL sift.enable_agg = off; SET LOCAL enable_bitmapscan = off; SELECT k FROM iso_cs2 WHERE k = 5000; }
step s1bm2	{ SET LOCAL sift.enable_agg = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_indexonlyscan = off; SELECT id FROM iso_cs2 WHERE k = 5000; }
step s1io1	{ SET LOCAL sift.enable_agg = off; SET LOCAL enable_bitmapscan = off; SELECT k FROM iso_cs1 WHERE k = 5000; }
step s1bm1	{ SET LOCAL sift.enable_agg = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_indexonlyscan = off; SELECT id FROM iso_cs1 WHERE k = 5000; }
step s1i2	{ INSERT INTO iso_cs2 VALUES (5001, 5000); }
step s1i1	{ INSERT INTO iso_cs1 VALUES (5001, 5000); }
step s1c	{ COMMIT; }

session s2
setup		{ SET enable_seqscan = off; SET max_parallel_workers_per_gather = 0; }
step s2b	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2io2	{ SET LOCAL sift.enable_agg = off; SET LOCAL enable_bitmapscan = off; SELECT k FROM iso_cs2 WHERE k = 5000; }
step s2bm2	{ SET LOCAL sift.enable_agg = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_indexonlyscan = off; SELECT id FROM iso_cs2 WHERE k = 5000; }
step s2io1	{ SET LOCAL sift.enable_agg = off; SET LOCAL enable_bitmapscan = off; SELECT k FROM iso_cs1 WHERE k = 5000; }
step s2bm1	{ SET LOCAL sift.enable_agg = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_indexonlyscan = off; SELECT id FROM iso_cs1 WHERE k = 5000; }
step s2i2	{ INSERT INTO iso_cs2 VALUES (5002, 5000); }
step s2i1	{ INSERT INTO iso_cs1 VALUES (5002, 5000); }
step s2c	{ COMMIT; }

permutation s1b s2b s1io2 s2io2 s1i2 s1c s2i2 s2c
permutation s1b s2b s1bm2 s2bm2 s1i2 s1c s2i2 s2c
permutation s1b s2b s1io1 s2io1 s1i1 s1c s2i1 s2c
permutation s1b s2b s1bm1 s2bm1 s1i1 s1c s2i1 s2c
