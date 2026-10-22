# A sift_table written by several transactions at once into the same write buffers, each with a command per row (the directory limit
# is lowered): once the directory is half full the write buffers are promoted with the version of every row, so that a writer that
# commits (A), one that rolls back (B) and one that is still running (C) share them. Other sessions see all of A, none of B and none
# of C at any isolation level, through a scan, an index, a bitmap and an aggregate scan, with VACUUM running meanwhile; a unique
# inserter waits for a writer whose row is promoted already; indexes are built over such a table; its rows are locked and updated.
# requires: tam gates

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE EXTENSION IF NOT EXISTS pg_sift_test;
	CREATE TABLE iso_wp (id int PRIMARY KEY, v text, w int) USING sift_table WITH (autovacuum_enabled = false);
	INSERT INTO iso_wp SELECT g, 'before', g % 100 FROM generate_series(1, 10) g;
}

teardown
{
	DROP TABLE iso_wp;
}

# the three writers: ids from 100001 (A), 200001 (B) and 300001 (C), a chunk of 6000 rows at a time (each chunk starts 10000 above the one before), each row a command
session a
setup		{ SET sift.table_max_segments = 16; SET sift.table_write_buffer_limit = 2048; }
step ab		{ BEGIN; }
step a1		{ DO $$ BEGIN FOR i IN 1..6000 LOOP INSERT INTO iso_wp VALUES (100000 + i, repeat('x', 1000), i % 100); END LOOP; END $$; }
step a2		{ DO $$ BEGIN FOR i IN 1..6000 LOOP INSERT INTO iso_wp VALUES (110000 + i, repeat('x', 1000), i % 100); END LOOP; END $$; }
step a3		{ DO $$ BEGIN FOR i IN 1..6000 LOOP INSERT INTO iso_wp VALUES (120000 + i, repeat('x', 1000), i % 100); END LOOP; END $$; }
step aupd	{ UPDATE iso_wp SET v = 'a-upd' WHERE id BETWEEN 100001 AND 100500; }
step adel	{ DELETE FROM iso_wp WHERE id BETWEEN 100501 AND 101000; }
step atag	{ SELECT sift._test_tbl_tagged('iso_wp') > 0 AS promoted_rows; }
step ac		{ COMMIT; }
step aa		{ ROLLBACK; }

session b
setup		{ SET sift.table_max_segments = 16; SET sift.table_write_buffer_limit = 2048; }
step bb		{ BEGIN; }
step b1		{ DO $$ BEGIN FOR i IN 1..6000 LOOP INSERT INTO iso_wp VALUES (200000 + i, repeat('x', 1000), i % 100); END LOOP; END $$; }
step b2		{ DO $$ BEGIN FOR i IN 1..6000 LOOP INSERT INTO iso_wp VALUES (210000 + i, repeat('x', 1000), i % 100); END LOOP; END $$; }
step b3		{ DO $$ BEGIN FOR i IN 1..6000 LOOP INSERT INTO iso_wp VALUES (220000 + i, repeat('x', 1000), i % 100); END LOOP; END $$; }
step ba		{ ROLLBACK; }

session c
setup		{ SET sift.table_max_segments = 16; SET sift.table_write_buffer_limit = 2048; }
step cb		{ BEGIN; }
step c1		{ DO $$ BEGIN FOR i IN 1..6000 LOOP INSERT INTO iso_wp VALUES (300000 + i, repeat('x', 1000), i % 100); END LOOP; END $$; }
step c2		{ DO $$ BEGIN FOR i IN 1..6000 LOOP INSERT INTO iso_wp VALUES (310000 + i, repeat('x', 1000), i % 100); END LOOP; END $$; }
step c3		{ DO $$ BEGIN FOR i IN 1..6000 LOOP INSERT INTO iso_wp VALUES (320000 + i, repeat('x', 1000), i % 100); END LOOP; END $$; }
step cc		{ COMMIT; }
step ca		{ ROLLBACK; }

# read committed: every step takes a new snapshot; g is 0 for the rows from before, 1 for A, 2 for B and 3 for C
session r
setup		{ SET sift.table_max_segments = 16; SET sift.table_write_buffer_limit = 2048; }
step rscan	{ SET enable_seqscan = on; SET enable_indexscan = off; SET enable_bitmapscan = off; SELECT count(*), sum(id) FROM iso_wp; }
step ridx	{ SET enable_seqscan = off; SET enable_indexscan = on; SET enable_bitmapscan = off; SELECT count(*) FROM iso_wp WHERE id > 0; }
step rbm	{ SET enable_seqscan = off; SET enable_indexscan = off; SET enable_bitmapscan = on; SELECT count(*) FROM iso_wp WHERE id > 0; }
step ragg	{ SET enable_seqscan = on; SET enable_indexscan = off; SET enable_bitmapscan = off; SET sift.enable_agg = on; SET cpu_tuple_cost = 0.5; SELECT id / 100000 AS g, count(*), sum(id) FROM iso_wp GROUP BY 1 ORDER BY 1; }
step rpar	{ SET sift.enable_agg = off; SET max_parallel_workers_per_gather = 2; SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0; SELECT count(*), sum(id) FROM iso_wp; }
step rupd	{ SELECT count(*) FILTER (WHERE v = 'a-upd') AS updated, count(*) FILTER (WHERE id BETWEEN 100501 AND 101000) AS deleted_ones FROM iso_wp; }
step rrow	{ SELECT id, left(v, 4) AS v FROM iso_wp WHERE id = 100002; }
step rw		{ SET enable_seqscan = off; SET enable_bitmapscan = off; SELECT count(*), sum(id) FROM iso_wp WHERE w = 7; }
step rwseq	{ SET enable_seqscan = on; SELECT count(*), sum(id) FROM iso_wp WHERE w = 7; }
step frozen	{ SELECT sift._test_tbl_tagged('iso_wp') = 0 AS frozen; }

# a snapshot taken once and kept
session rr
setup		{ SET sift.table_max_segments = 16; SET sift.table_write_buffer_limit = 2048; }
step rrb	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM iso_wp; }
step rrscan	{ SELECT id / 100000 AS g, count(*) FROM iso_wp GROUP BY 1 ORDER BY 1; }
step rrupd	{ UPDATE iso_wp SET v = 'rr' WHERE id = 100002; }
step rre	{ COMMIT; }

session rs
setup		{ SET sift.table_max_segments = 16; SET sift.table_write_buffer_limit = 2048; }
step rsb	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SELECT count(*) FROM iso_wp; }
step rsscan	{ SELECT id / 100000 AS g, count(*) FROM iso_wp GROUP BY 1 ORDER BY 1; }
step rse	{ COMMIT; }

session v
setup		{ SET sift.table_max_segments = 16; SET sift.table_write_buffer_limit = 2048; }
step v		{ VACUUM iso_wp; }

# a unique inserter, on the key of a row of A (the first chunk is promoted by the time the others are written)
session u
setup		{ SET sift.table_max_segments = 16; SET sift.table_write_buffer_limit = 2048; }
step ub		{ BEGIN; }
step u1		{ INSERT INTO iso_wp VALUES (100003, 'dup', 3); }
step u2		{ INSERT INTO iso_wp VALUES (100003, 'dup', 3) ON CONFLICT (id) DO NOTHING; }
step uc		{ COMMIT; }
step ur		{ ROLLBACK; }

session i
setup		{ SET sift.table_max_segments = 16; SET sift.table_write_buffer_limit = 2048; }
step ci		{ CREATE INDEX iso_wp_w ON iso_wp (w); }
step cic	{ CREATE INDEX CONCURRENTLY iso_wp_w ON iso_wp (w); }

# row locks and updates of rows by other sessions
session l1
step l1b	{ BEGIN; }
step l1u	{ UPDATE iso_wp SET v = 'l1' WHERE id = 100002; }
step l1c	{ COMMIT; }

session l2
step lupd	{ UPDATE iso_wp SET v = 'l' WHERE id = 100002; }
step lfu	{ SELECT id FROM iso_wp WHERE id = 100002 FOR UPDATE; }
step l2nw	{ SELECT id FROM iso_wp WHERE id = 100002 FOR UPDATE NOWAIT; }
step l2sk	{ SELECT id FROM iso_wp WHERE id = 100002 FOR UPDATE SKIP LOCKED; }
step l2u	{ UPDATE iso_wp SET v = v || '+l2' WHERE id = 100002; }

# read committed: none of C while it runs, all of A and none of B once A committed and B rolled back, VACUUM in between
permutation ab bb cb a1 b1 c1 a2 b2 c2 v a3 b3 c3 atag v rscan ridx rbm ragg rpar ac ba rscan ridx rbm ragg rpar v rscan ridx rbm ragg rpar cc rscan ridx rbm ragg rpar v rscan ragg frozen
# the same when C rolls back
permutation ab bb cb a1 b1 c1 a2 b2 c2 a3 b3 c3 ac ba v rscan ragg ca rscan ragg v rscan ridx rbm ragg frozen

# a snapshot taken before any of them committed never sees them, VACUUM or not; one taken after A and B ended sees A and never C
permutation rrb ab bb cb a1 b1 c1 a2 b2 c2 a3 b3 c3 ac ba cc v rrscan rre rscan ragg
permutation ab bb cb a1 b1 c1 a2 b2 c2 a3 b3 c3 ac ba rrb cc v rrscan v rre rscan ragg
permutation ab bb cb a1 b1 c1 a2 b2 c2 a3 b3 c3 ac ba rsb cc v rsscan rse rscan ragg
permutation rsb ab bb cb a1 b1 c1 a2 b2 c2 a3 b3 c3 ac ba ca v rsscan rse ragg

# a unique inserter waits for the writer of a promoted row, then fails if it commits and goes on if it rolls back
permutation ab a1 a2 a3 v atag ub u1 ac ur rscan
permutation ab a1 a2 a3 v atag ub u1 aa uc rscan
permutation ab a1 a2 a3 v atag ub u2 ac uc rscan
permutation ab a1 a2 a3 v atag ub u2 aa uc rscan

# an index built over the table waits for the writers, then holds the rows of A and none of B (plain, and CONCURRENTLY)
permutation ab bb a1 b1 a2 b2 a3 b3 atag ci ac ba rw rwseq
permutation ab bb a1 b1 a2 b2 a3 b3 atag cic ac ba rw rwseq

# the rows of a running writer are not there to lock or update; those of a committed one are, frozen or not
permutation ab a1 a2 a3 atag lupd lfu ac rrow lupd rrow lfu
permutation ab a1 a2 a3 atag ac v lupd rrow lfu
permutation ab a1 a2 a3 atag aupd adel ac rupd rscan v rupd rscan
permutation ab a1 a2 a3 atag aupd adel aa v rscan
permutation ab a1 a2 a3 ac l1b l1u l2nw l2sk l2u l1c rrow
permutation ab a1 a2 a3 ac v l1b l1u l2nw l2sk l2u l1c rrow
# first updater wins: a repeatable read transaction that saw the row of A cannot update it after another committed an update
permutation ab a1 a2 a3 ac rrb l1b l1u l1c rrupd rre rrow
