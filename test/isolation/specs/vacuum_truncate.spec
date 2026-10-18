# VACUUM giving the free blocks at the end of the index file back to the operating system, against the
# sessions that use the index: a session holding any lock on it keeps VACUUM from truncating (it never waits), and a session
# that appended to the mutable segment before the truncation appends after it, although the blocks its lane tails name are gone.
# requires: build insert vacuum scan promote merge verify index_info

setup
{
	CREATE TABLE iso_tr (id int, body text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_tr SELECT g, 'k' || (g % 7) || ' g' || (g % 1000) || ' common word' || (g % 50) || ' u' || g FROM generate_series(1, 60000) g;
	CREATE INDEX iso_tr_idx ON iso_tr USING sift (body) WITH (build_segments = 4);
	DELETE FROM iso_tr WHERE id % 20 <> 0;
}

teardown
{
	DROP TABLE iso_tr;
	DROP TABLE IF EXISTS iso_tr_size;
}

# appends to the mutable segment of the index, for good
session s1
setup		{ SET sift.async_maintenance = off; }
step s1i	{ INSERT INTO iso_tr SELECT g, 'common appended ' || g FROM generate_series(100001, 140000) g; }
step s1j	{ INSERT INTO iso_tr SELECT g, 'common again ' || g FROM generate_series(150001, 150100) g; }

# a reader of the index, or a transaction whose snapshot keeps the segments the merge freed from being reused
session s3
setup		{ SET enable_seqscan = off; SET sift.async_maintenance = off; }
step s3snap	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) > 0 AS snapshot FROM iso_tr; }
step s3read	{ BEGIN; SELECT count(*) AS hits FROM iso_tr WHERE body @@> 'common'; }
step s3c	{ COMMIT; }

session s2
setup		{ SET sift.async_maintenance = off; }
step s2v	{ VACUUM iso_tr; }
step s2m	{ SELECT sift.merge('iso_tr_idx', true) > 0 AS merged; }
step s2p	{ SELECT sift.promote('iso_tr_idx') > 0 AS promoted; }
step s2rec	{ CREATE TABLE iso_tr_size AS SELECT relation_blocks AS blocks FROM sift.index_info('iso_tr_idx'); }
step s2size	{ SELECT i.relation_blocks = s.blocks AS unchanged, i.relation_blocks < s.blocks AS shrunk, i.relation_blocks = i.high_water AS sized
			  FROM sift.index_info('iso_tr_idx') i, iso_tr_size s; }
step s2check	{ SELECT iso_agree('iso_tr', 'common') AS agree, iso_agree('iso_tr', 'appended') AS agree_appended, iso_verify('iso_tr_idx') AS verified; }

# the merge leaves the end of the file free; a reader holding the index keeps VACUUM from cutting it and VACUUM does not wait for it
permutation s2v s2m s2rec s3read s2v s2size s3c s2v s2size s2check

# the segments freed by a merge are not reusable while an older snapshot runs, so the appends of s1 extend the file; once it ends,
# the promotion of the appended segment lands in the space in front, the end of the file is free, VACUUM cuts it and s1 appends again
permutation s3snap s2v s1i s3c s2p s2rec s2v s2size s1j s2check
