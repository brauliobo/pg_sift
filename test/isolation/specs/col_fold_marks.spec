# The fold of the marks of the tombstone log into the liveness runs (sift._test_col_fold_marks, which the maintenance worker runs when the reads have paid for it) clears the rows a delete
# ended only when every snapshot sees that transaction as committed: a REPEATABLE READ snapshot taken before the delete keeps reading the rows, so do a transaction that is prepared, one in
# progress and one that rolled back. After the transaction ends (or the snapshot goes) the rows are cleared and every read agrees with the heap. Every read compares the plans.
# requires: col

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE EXTENSION IF NOT EXISTS pg_sift_test;
	CREATE TABLE iso_fold (id int PRIMARY KEY, tenant_id int NOT NULL, counter_a int NOT NULL) WITH (autovacuum_enabled = off);
	INSERT INTO iso_fold SELECT i, i % 10, i FROM generate_series(1, 20000) i;
	CREATE INDEX iso_fold_c ON iso_fold USING sift_col (id, tenant_id, counter_a) WITH (track_changes = 'id');
}

setup
{
	VACUUM (FREEZE, ANALYZE) iso_fold;
}

setup
{
	CREATE FUNCTION iso_fold_q(OUT n bigint, OUT total bigint, OUT agree bool) LANGUAGE plpgsql AS $$
	DECLARE q text := 'SELECT count(*), sum(counter_a) FROM iso_fold'; a text; b text;
	BEGIN
		PERFORM set_config('max_parallel_workers_per_gather', '0', true);
		PERFORM set_config('sift.enable_agg', 'on', true);
		EXECUTE q INTO n, total;
		a := n || '/' || total;
		PERFORM set_config('sift.enable_agg', 'off', true);
		EXECUTE q INTO n, total;
		b := n || '/' || total;
		agree := a = b;
	END $$;
}

teardown
{
	DROP TABLE iso_fold;
	DROP FUNCTION iso_fold_q();
}

session s1
step s1b	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT n, total, agree FROM iso_fold_q(); }
step s1q	{ SELECT n, total, agree FROM iso_fold_q(); }
step s1c	{ COMMIT; }
step s1n	{ SELECT n, total, agree FROM iso_fold_q(); }

session s2
step s2d	{ DELETE FROM iso_fold WHERE id % 100 = 7; }
step s2b	{ BEGIN; }
step s2dd	{ DELETE FROM iso_fold WHERE id % 100 = 9; }
step s2p	{ PREPARE TRANSACTION 'iso_fold'; }
step s2cp	{ COMMIT PREPARED 'iso_fold'; }
step s2c	{ COMMIT; }
step s2a	{ ROLLBACK; }

session s3
step s3f	{ SELECT sift._test_col_fold_marks('iso_fold_c') AS folded; }

# a snapshot older than the delete: nothing is folded while it lasts, and it reads the rows; then they are folded
permutation s1b s2d s3f s1q s1c s3f s1n
# a transaction in progress: nothing, then its rows when it commits
permutation s2b s2dd s3f s1n s2c s3f s1n
# one that rolled back leaves its rows
permutation s2b s2dd s2a s3f s1n
# a prepared transaction, which holds the horizon back, and the commit of it
permutation s2b s2dd s2p s3f s1n s2cp s3f s1n
