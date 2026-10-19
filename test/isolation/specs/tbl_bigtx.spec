# A sift_table written by one transaction that needs more write buffers than the directory has entries for (the limit is lowered): once
# the directory is half full the transaction's write buffers become column groups that carry the writer. Other sessions see none of its
# rows while it runs (at any isolation level, through a scan, an index and a VACUUM that runs meanwhile), all of them once it commits at
# the snapshots taken after, and none if it rolls back.
# requires: tam gates

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE EXTENSION IF NOT EXISTS pg_sift_test;
	CREATE TABLE iso_big (id int, v text) USING sift_table WITH (autovacuum_enabled = false);
	CREATE INDEX iso_big_id ON iso_big (id);
	INSERT INTO iso_big SELECT g, 'before' FROM generate_series(1, 10) g;
}

teardown
{
	DROP TABLE iso_big;
}

session w
step wb		{ BEGIN; SET sift.table_max_segments = 16; SET sift.table_write_buffer_limit = 2048; }
step wi		{ INSERT INTO iso_big SELECT g, repeat('x', 1000) FROM generate_series(1001, 41000) g; }
step wtag	{ SELECT sift._test_tbl_tagged('iso_big') > 0 AS carries_its_writer; }
step wown	{ SELECT count(*) FROM iso_big; }
step wc		{ COMMIT; }
step wa		{ ROLLBACK; }
step wv		{ VACUUM iso_big; }

session r
step rscan	{ SET enable_seqscan = on; SET enable_indexscan = off; SET enable_bitmapscan = off; SELECT count(*), sum(id) FROM iso_big; }
step ridx	{ SET enable_seqscan = off; SET enable_indexscan = on; SET enable_bitmapscan = off; SELECT count(*) FROM iso_big WHERE id > 0; }
step rbm	{ SET enable_seqscan = off; SET enable_indexscan = off; SET enable_bitmapscan = on; SELECT count(*) FROM iso_big WHERE id > 20000; }
step ragg	{ SET sift.enable_agg = on; SET cpu_tuple_cost = 0.5; SELECT id % 3 AS g, count(*), sum(id) FROM iso_big GROUP BY 1 ORDER BY 1; }
step rrr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM iso_big; }
step rser	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SELECT count(*) FROM iso_big; }
step rend	{ COMMIT; }
step rv		{ VACUUM iso_big; }

# read committed: none of the rows while the writer runs, all of them after its commit
permutation wb wi wtag wown rscan ridx rbm ragg wc rscan ridx rbm ragg
# a rollback leaves nothing, and VACUUM drops the groups
permutation wb wi wtag rscan wa rscan ridx rv rscan
# a snapshot taken before the commit never sees the rows; one taken after sees them all
permutation rrr wb wi wc rscan ragg rend rscan ragg
permutation rser wb wi wc rscan rend rscan
# VACUUM while the writer runs leaves its groups alone
permutation wb wi rv rscan ragg wtag wc rscan ragg wv rscan ragg
