# The rewrite of a scattered sift_col index (sift.col_maintain) against readers and writers: it seals the write buffer, promotes it and merges all the
# segments under the maintenance lock only, so a snapshot taken before it reads the same rows through it, a writer in flight does not wait for it and
# its rows (committed or rolled back afterwards) are counted as they should be, and no session waits for another.
# requires: col

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE iso_am (id int, kind text, amount numeric(10,2), pad text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_am SELECT g, 'cat_' || (g % 4), g % 97, repeat('x', 150) FROM generate_series(1, 20000) g;
	CREATE INDEX iso_am_idx ON iso_am USING sift_col (id, kind, amount) WITH (write_buffer_limit = 4096);
}

setup
{
	VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) iso_am;
}

setup
{
	-- churn spread over the table: a dead row in every unit, the new versions in the write buffer
	UPDATE iso_am SET amount = amount + 1 WHERE id % 100 = 1;
	DELETE FROM iso_am WHERE id % 100 = 51;
}

setup
{
	VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) iso_am;
}

setup
{
	CREATE FUNCTION iso_am_q(OUT n bigint, OUT total numeric, OUT agree bool) LANGUAGE plpgsql AS $$
	DECLARE q text := 'SELECT count(*), sum(amount) FROM iso_am'; a text; b text;
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
	DROP TABLE iso_am;
	DROP FUNCTION iso_am_q();
}

session s1
step s1b	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT n, total, agree FROM iso_am_q(); }
step s1q	{ SELECT n, total, agree FROM iso_am_q(); }
step s1c	{ COMMIT; }
step s1n	{ SELECT n, total, agree FROM iso_am_q(); }

session s2
step s2b	{ BEGIN; }
step s2i	{ INSERT INTO iso_am SELECT 100000 + g, 'cat_' || (g % 4), 5, repeat('x', 150) FROM generate_series(1, 300) g; }
step s2j	{ INSERT INTO iso_am SELECT 200000 + g, 'cat_' || (g % 4), 7, repeat('x', 150) FROM generate_series(1, 100) g; }
step s2c	{ COMMIT; }
step s2a	{ ROLLBACK; }

session s3
step s3d	{ SELECT sift.col_maintenance_due('iso_am_idx') AS due; }
step s3m	{ SELECT sift.col_maintain('iso_am_idx') AS rewritten; }
step s3v	{ SELECT count(*) FILTER (WHERE kind = 'immutable') AS segments, count(*) FILTER (WHERE kind <> 'immutable' AND rows > 0) AS buffers, bool_or(dirty) AS dirty, (SELECT sift.col_verify('iso_am_idx')::text = '') AS verified FROM sift.col_segments('iso_am_idx'); }

# a snapshot taken before the rewrite reads the same rows after it, and so does the next one
permutation s3d s1b s3m s1q s1c s1n s3v
# a writer in flight: its rows are in the buffer the rewrite seals and promotes; they count once it commits
permutation s3d s2b s2i s3m s2j s2c s1n s3v
# ... and not when it rolls back
permutation s3d s2b s2i s3m s2j s2a s1n s3v
# a reader whose snapshot is older than the writer's commit sees neither the rows nor a difference between the plans
permutation s1b s2b s2i s3m s2c s1q s1c s1n
