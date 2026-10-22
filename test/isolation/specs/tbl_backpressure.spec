# sift.table_backpressure: while another session holds the maintenance lock (a long VACUUM or merge), a writer whose transaction fills write buffers
# that cannot be promoted stops at the percent of the directory it names, instead of at the last entries. The limit is lowered; the rows are the
# same either way once the holder is done.
# requires: tam gates

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE EXTENSION IF NOT EXISTS pg_sift_test;
	CREATE TABLE iso_bp (id int, v text) USING sift_table WITH (autovacuum_enabled = false);
}

teardown
{
	DROP TABLE iso_bp;
}

session h
step hb		{ BEGIN; SELECT sift._test_tbl_hold_maint('iso_bp'); }
step hc		{ COMMIT; }

session w
step wb		{ BEGIN; SET sift.table_max_segments = 16; SET sift.table_write_buffer_limit = 2048; }
step wbp	{ SET LOCAL sift.table_backpressure = 75; }
step wi		{ INSERT INTO iso_bp SELECT g, repeat('x', 1000) FROM generate_series(1, 40000) g; }
step wc		{ COMMIT; }

session o
step oseg	{ SELECT count(*) AS entries FROM sift.table_segments('iso_bp'); }
step orows	{ SELECT count(*) AS visible FROM iso_bp; }

# backpressure at 75% of 16 entries: the writer waits with 13 entries in the directory, and goes on when the holder commits
permutation hb wb wbp wi oseg hc wc orows
# without it the writer waits only at the last entries (14 of 16)
permutation hb wb wi oseg hc wc orows
