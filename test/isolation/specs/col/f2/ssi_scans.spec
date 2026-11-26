# SERIALIZABLE write skew through the reads of a sift_col index of format major 2: two transactions read a group that has no rows, then each inserts into it. Every read must leave the
# SIREAD lock that makes the second writer fail with 40001, whichever way the rows are read: an index only scan, a bitmap scan, the ordered scan of key_order and the SiftAggV6 grouping. The
# lock is the one of the index relation that index_beginscan takes for an access method without page predicate locks, which every scan of the index goes through. A delete writes
# no index entry: two transactions that each read a row of one group and delete the row of the other fail the same way, through the SIREAD locks the scans leave on the heap.
# requires: col

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	DO $$ BEGIN
		IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN
			PERFORM set_config('sift.test_create_format1', 'off', false);
		END IF;
	END $$;
	CREATE TABLE iso_k (id int, grp int, bal int) WITH (autovacuum_enabled = off);
	INSERT INTO iso_k SELECT i, 2 + i % 50, i % 100 FROM generate_series(1, 20000) i;
	INSERT INTO iso_k VALUES (90001, 60, 7), (90002, 61, 7);
	CREATE INDEX iso_k_idx ON iso_k USING sift_col (grp, bal);
	CREATE TABLE iso_o (id int, grp int, bal int) WITH (autovacuum_enabled = off);
	INSERT INTO iso_o SELECT i, 2 + i % 50, i % 100 FROM generate_series(1, 20000) i;
	INSERT INTO iso_o VALUES (90001, 60, 7), (90002, 61, 7);
	CREATE INDEX iso_o_idx ON iso_o USING sift_col (grp sift.col_int4_ops, bal) WITH (key_order = 'grp');
	ANALYZE iso_k;
	ANALYZE iso_o;
}

teardown
{
	DROP TABLE iso_k, iso_o;
}

session s1
step s1b_ios	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL sift.enable_agg = off; }
step s1b_bmp	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_indexonlyscan = off; SET LOCAL sift.enable_agg = off; }
step s1b_ord	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL sift.enable_agg = off; }
step s1b_grp	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1r_k		{ SELECT count(*) AS n FROM iso_k WHERE grp = 1; }
step s1r_ord	{ SELECT bal FROM iso_o WHERE grp = 1 ORDER BY grp; }
step s1r_grp	{ SELECT grp, sum(bal) AS total FROM iso_k WHERE grp = 1 GROUP BY grp; }
step s1w_k		{ INSERT INTO iso_k VALUES (100001, 1, 5); }
step s1w_o		{ INSERT INTO iso_o VALUES (100001, 1, 5); }
step s1r_dk		{ SELECT count(*) AS n FROM iso_k WHERE grp = 60; }
step s1r_dord	{ SELECT bal FROM iso_o WHERE grp = 60 ORDER BY grp; }
step s1r_dgrp	{ SELECT grp, sum(bal) AS total FROM iso_k WHERE grp = 60 GROUP BY grp; }
step s1d_k		{ DELETE FROM iso_k WHERE id = 90002; }
step s1d_o		{ DELETE FROM iso_o WHERE id = 90002; }
step s1c		{ COMMIT; }

session s2
step s2b_ios	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL sift.enable_agg = off; }
step s2b_bmp	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_indexonlyscan = off; SET LOCAL sift.enable_agg = off; }
step s2b_ord	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL sift.enable_agg = off; }
step s2b_grp	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2r_k		{ SELECT count(*) AS n FROM iso_k WHERE grp = 1; }
step s2r_ord	{ SELECT bal FROM iso_o WHERE grp = 1 ORDER BY grp; }
step s2r_grp	{ SELECT grp, sum(bal) AS total FROM iso_k WHERE grp = 1 GROUP BY grp; }
step s2w_k		{ INSERT INTO iso_k VALUES (200001, 1, 5); }
step s2w_o		{ INSERT INTO iso_o VALUES (200001, 1, 5); }
step s2r_dk		{ SELECT count(*) AS n FROM iso_k WHERE grp = 61; }
step s2r_dord	{ SELECT bal FROM iso_o WHERE grp = 61 ORDER BY grp; }
step s2r_dgrp	{ SELECT grp, sum(bal) AS total FROM iso_k WHERE grp = 61 GROUP BY grp; }
step s2d_k		{ DELETE FROM iso_k WHERE id = 90001; }
step s2d_o		{ DELETE FROM iso_o WHERE id = 90001; }
step s2c		{ COMMIT; }

permutation s1b_ios s2b_ios s1r_k s2r_k s1w_k s2w_k s1c s2c
permutation s1b_bmp s2b_bmp s1r_k s2r_k s1w_k s2w_k s1c s2c
permutation s1b_ord s2b_ord s1r_ord s2r_ord s1w_o s2w_o s1c s2c
permutation s1b_grp s2b_grp s1r_grp s2r_grp s1w_k s2w_k s1c s2c
permutation s1b_ios s2b_ios s1r_dk s2r_dk s1d_k s2d_k s1c s2c
permutation s1b_bmp s2b_bmp s1r_dk s2r_dk s1d_k s2d_k s1c s2c
permutation s1b_ord s2b_ord s1r_dord s2r_dord s1d_o s2d_o s1c s2c
permutation s1b_grp s2b_grp s1r_dgrp s2r_dgrp s1d_k s2d_k s1c s2c
