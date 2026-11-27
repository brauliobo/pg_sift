# The promotion of a write buffer whose doc stands for a row a Format 2.0 sift_table still holds in the stage of its inserter, which writes it at
# its commit: the promotion waits for the inserter and keeps the doc of a row it commits (S119-27), and drops the doc of one it rolls back.
# requires: build insert promote verify

setup
{
	SET sift.test_create_format1 = off;
	CREATE TABLE iso_stg (id int, body text) USING sift_table;
	CREATE INDEX iso_stg_idx ON iso_stg USING sift (body) WITH (write_buffer_limit = 64);
	INSERT INTO iso_stg SELECT g, 'seed doc ' || g FROM generate_series(1, 20) g;
}

teardown
{
	DROP TABLE iso_stg;
}

session s1
step s1b	{ BEGIN; }
step s1i	{ INSERT INTO iso_stg SELECT g, 'alpha staged ' || g FROM generate_series(100, 109) g; }
step s1c	{ COMMIT; }
step s1a	{ ROLLBACK; }

session s2
step s2p	{ SELECT sift.promote('iso_stg_idx') >= 0 AS promoted; }

session s3
setup		{ SET enable_seqscan = off; }
step s3q	{ SELECT count(*) AS problems FROM sift.check('iso_stg_idx', true); SELECT count(*) AS alpha FROM iso_stg WHERE body @@> 'alpha'; }

permutation s1b s1i s2p s1c s3q
permutation s1b s1i s2p s1a s3q
