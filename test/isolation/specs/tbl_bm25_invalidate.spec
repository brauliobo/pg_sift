# The heap check of a BM25 count over a sift_table (sift_tbl_filter_page) judges the pages with the tombstones its scan began with, whatever
# the cache entry of the table is when it asks: a VACUUM invalidates it while the count waits, and an older snapshot keeps the log entries
# of the rows deleted before the count's snapshot, so the new entry would have to read them again.
# requires: tam gates

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE iso_bd (id int, body text) USING sift_table WITH (autovacuum_enabled = false);
	INSERT INTO iso_bd SELECT g, 'order number ' || g || CASE WHEN g % 2 = 0 THEN ' express shipping' ELSE ' standard shipping' END
		FROM generate_series(1, 3000) g;
	CREATE INDEX iso_bd_idx ON iso_bd USING sift (body);

	CREATE FUNCTION iso_bd_q(gate bool) RETURNS text LANGUAGE plpgsql AS $$
	DECLARE
		a bigint;
		b bigint;
	BEGIN
		PERFORM set_config('enable_seqscan', 'off', true);
		PERFORM set_config('sift.test_count_gate', gate::text, true);
		SELECT count(*) INTO a FROM iso_bd WHERE body @@> 'express';
		PERFORM set_config('sift.test_count_gate', 'off', true);
		PERFORM set_config('enable_indexscan', 'off', true);
		PERFORM set_config('enable_bitmapscan', 'off', true);
		PERFORM set_config('sift.enable_count', 'off', true);
		PERFORM set_config('sift.enable_scan', 'off', true);
		PERFORM set_config('enable_seqscan', 'on', true);
		SELECT count(*) INTO b FROM iso_bd WHERE body ~ '(^| )express( |$)';
		RETURN CASE WHEN a = b THEN 'agree' ELSE 'count ' || a || ' heap ' || b END;
	END $$;
}

teardown
{
	DROP TABLE iso_bd;
	DROP FUNCTION iso_bd_q(bool);
}

session s1
step s1b	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM iso_bd; }
step s1q	{ SELECT iso_bd_q(true); }
step s1c	{ COMMIT; }

session s2
step s2l	{ SELECT pg_advisory_lock(20983, 1); }
step s2u	{ SELECT pg_advisory_unlock(20983, 1); }

session s3
step s3d	{ DELETE FROM iso_bd WHERE id % 4 = 0; }
step s3v	{ VACUUM iso_bd; }

session s4
step s4b	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM iso_bd; }
step s4c	{ COMMIT; }

permutation s4b s3d s1b s2l s1q s3v s2u s1c s4c
