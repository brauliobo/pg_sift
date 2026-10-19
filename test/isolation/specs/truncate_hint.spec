# The blocks a session cached for appending to the write buffer (the tails of its lanes) can lie in the part of the file a
# truncation cuts: the session appends again afterwards. Nothing but the truncation invalidates what it cached here (VACUUM
# updates the statistics of the index at its commit, which would), so the truncation is the test function's and not VACUUM's.
# requires: build insert promote verify index_info gates

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift_test;
	CREATE TABLE iso_th (id int, body text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_th SELECT g, 'k' || (g % 7) || ' g' || (g % 1000) || ' common word' || (g % 50) || ' u' || g FROM generate_series(1, 60000) g;
	CREATE INDEX iso_th_idx ON iso_th USING sift (body) WITH (build_segments = 4);
	DELETE FROM iso_th WHERE id % 20 <> 0;
}

teardown
{
	DROP TABLE iso_th;
	DROP TABLE IF EXISTS iso_th_size;
}

session s1
setup		{ SET sift.async_maintenance = off; }
step s1i	{ INSERT INTO iso_th SELECT g, 'common appended ' || g FROM generate_series(100001, 140000) g; }
step s1j	{ INSERT INTO iso_th SELECT g, 'common again ' || g FROM generate_series(150001, 150100) g; }

session s3
step s3snap	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) > 0 AS snapshot FROM iso_th; }
step s3c	{ COMMIT; }

session s2
setup		{ SET sift.async_maintenance = off; }
step s2v	{ VACUUM iso_th; }
step s2p	{ SELECT sift.promote('iso_th_idx') > 0 AS promoted; }
step s2rec	{ CREATE TABLE iso_th_size AS SELECT relation_blocks AS blocks FROM sift.index_info('iso_th_idx'); }
step s2t	{ SELECT sift._test_truncate('iso_th_idx') > 0 AS truncated; }
step s2size	{ SELECT i.relation_blocks < s.blocks AS shrunk, i.relation_blocks = i.high_water AS sized
			  FROM sift.index_info('iso_th_idx') i, iso_th_size s; }
step s2check	{ SELECT iso_agree('iso_th', 'common') AS agree, iso_agree('iso_th', 'appended') AS agree_appended, iso_verify('iso_th_idx') AS verified; }

# the segments freed by VACUUM's merge are not reusable while the snapshot of s3 runs, so the appends of s1 extend the file; once it
# ends, the promotion of that segment lands in the space in front and the end of the file, the blocks s1 cached, is free
permutation s3snap s2v s1i s3c s2p s2rec s2t s2size s1j s2check
