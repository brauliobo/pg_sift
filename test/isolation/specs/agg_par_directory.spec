# The participants of a parallel aggregate read one directory: the leader's. A key-cut index numbers the units of its segments by their key range and the batches of
# its write buffer by the superblock of the heap, so a promotion between the open of the leader and the open of a worker moves rows from one item of the scan to
# another, and the rows of the batches the leader read were taken by the leader's copy while the segment the workers read took them from the items of the other
# numbering: rows were counted twice or not at all. The leader opens and holds on the test gate while the write buffer is promoted, then the workers start; the
# aggregate is the heap's at the snapshot.
# requires: col gates

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE iso_pd (id int PRIMARY KEY, k int, v int, pad text) WITH (autovacuum_enabled = off);
	ALTER TABLE iso_pd ALTER COLUMN pad SET STORAGE PLAIN;
	INSERT INTO iso_pd SELECT g, (g * 7919) % 1000, g % 100, repeat('x', 200) FROM generate_series(1, 20000) g;
	CREATE INDEX iso_pd_idx ON iso_pd USING sift_col (id, k, v) WITH (key_cut = 'k');
}

setup
{
	VACUUM (FREEZE, ANALYZE) iso_pd;
}

setup
{
	INSERT INTO iso_pd SELECT g, (g * 7919) % 1000, g % 100, repeat('x', 200) FROM generate_series(20001, 26000) g;

	CREATE FUNCTION iso_pd_q(gate int, OUT agree bool, OUT parallel bool) LANGUAGE plpgsql AS $$
	DECLARE
		q text := 'SELECT k % 7, count(*), sum(v) FROM iso_pd GROUP BY 1 ORDER BY 1';
		a text;
		b text;
		l text;
	BEGIN
		PERFORM set_config('max_parallel_workers_per_gather', '2', true);
		PERFORM set_config('parallel_setup_cost', '0', true);
		PERFORM set_config('parallel_tuple_cost', '0', true);
		PERFORM set_config('min_parallel_table_scan_size', '0', true);
		PERFORM set_config('sift.agg_par_min_rows', '0', true);
		parallel := false;
		FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
			parallel := parallel OR l ~ 'Workers Planned: ';
		END LOOP;
		PERFORM set_config('sift.test_col_open_gate', gate::text, true);
		PERFORM set_config('sift.test_par_leader_wait', 'on', true);
		EXECUTE 'SELECT string_agg(x::text, '' '') FROM (' || q || ') x' INTO a;
		PERFORM set_config('sift.test_col_open_gate', '0', true);
		PERFORM set_config('sift.enable_agg', 'off', true);
		PERFORM set_config('sift.enable_col_scan', 'off', true);
		PERFORM set_config('max_parallel_workers_per_gather', '0', true);
		EXECUTE 'SELECT string_agg(x::text, '' '') FROM (' || q || ') x' INTO b;
		agree := a = b;
	END $$;
}

teardown
{
	DROP TABLE iso_pd;
	DROP FUNCTION iso_pd_q(int);
}

session s1
step s1b	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM iso_pd; }
step s1q	{ SELECT agree, parallel FROM iso_pd_q(7002); }
step s1c	{ COMMIT; }

session s2
step s2l	{ SELECT pg_advisory_lock(7002); }
step s2u	{ SELECT pg_advisory_unlock(7002); }

session s3
step s3p	{ SELECT sift.col_promote('iso_pd_idx') >= 0 AS promoted; }
step s3i	{ INSERT INTO iso_pd SELECT g, (g * 7919) % 1000, g % 100, repeat('x', 200) FROM generate_series(30001, 31000) g; }

# the leader holds its copy of the directory (the rows of the tail are in the write buffer) while the buffer is promoted into a segment and more rows follow
permutation s1b s2l s1q s3p s3i s2u s1c
