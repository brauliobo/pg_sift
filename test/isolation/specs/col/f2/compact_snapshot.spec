# A compaction of a Format 2 index while a reader holds a snapshot and rows are deleted. The VACUUM holds on the compaction gate (sift.test_phase_gate + 11) with its outputs written and
# the manifest not yet changed; meanwhile a delete commits that reaches the parts the compaction read, and a reader that took its snapshot before it goes on. The compaction carries the
# delete into the vectors of its outputs: the reader's snapshot still sees every row it saw, a statement after the delete sees the rest, the check finds nothing, and a second VACUUM
# with no reader frees what the compaction left.
# requires: col gates

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE EXTENSION IF NOT EXISTS pg_sift_test;
	SET sift.test_create_format1 = off;
	CREATE TABLE iso_fc (id int PRIMARY KEY, v int, pad text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_fc SELECT g, g % 7, md5(g::text) FROM generate_series(1, 20000) g;
	CREATE INDEX iso_fc_idx ON iso_fc USING sift_col (id, v, pad);
	DELETE FROM iso_fc WHERE id % 3 = 0;
	ANALYZE iso_fc;
	CREATE FUNCTION iso_fc_rows(OUT answer text) LANGUAGE plpgsql AS $$
	BEGIN
		PERFORM set_config('max_parallel_workers_per_gather', '0', true);
		SELECT count(*) || '/' || sum(v) INTO answer FROM iso_fc;
	END $$;
}

teardown
{
	DROP TABLE iso_fc;
	DROP FUNCTION iso_fc_rows();
}

session s1
setup	{ SET sift.test_create_format1 = off; SET sift.test_phase_gate = 8100; }
step s1vacuum	{ VACUUM iso_fc; }
step s1rows	{ SELECT answer FROM iso_fc_rows(); }

session s2
step s2lock	{ SELECT pg_advisory_lock(8111); }
step s2delete	{ DELETE FROM iso_fc WHERE id % 5 = 0; }
step s2unlock	{ SELECT pg_advisory_unlock(8111); }
step s2rows	{ SELECT answer FROM iso_fc_rows(); }
step s2check	{ SELECT count(*) AS problems FROM sift.check('iso_fc_idx'); }
step s2verify	{ SELECT sift.col_verify('iso_fc_idx'); }
step s2vacuum	{ VACUUM iso_fc; }

session s3
step s3begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT answer FROM iso_fc_rows(); }
step s3rows	{ SELECT answer FROM iso_fc_rows(); }
step s3end	{ COMMIT; }

permutation s2lock s3begin s1vacuum s2delete s3rows s2unlock s3rows s2rows s2check s2verify s3end s1rows s2vacuum s2rows s2check
