# The same for a partitioned table: the workers of a parallel aggregate open the source of every partition on the directory the leader read (the first
# partition is opened by the leader before its gate, the others when the scan is counted), so a promotion of the write buffer of a partition between the open
# of the leader and the open of a worker does not move its rows from one item of the scan to another. The aggregate is the heap's at the snapshot.
# requires: col gates

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE iso_pp (id int, k int, v int, pad text) PARTITION BY RANGE (id);
	CREATE TABLE iso_pp_a PARTITION OF iso_pp FOR VALUES FROM (0) TO (100000) WITH (autovacuum_enabled = off);
	CREATE TABLE iso_pp_b PARTITION OF iso_pp FOR VALUES FROM (100000) TO (200000) WITH (autovacuum_enabled = off);
	ALTER TABLE iso_pp_a ALTER COLUMN pad SET STORAGE PLAIN;
	ALTER TABLE iso_pp_b ALTER COLUMN pad SET STORAGE PLAIN;
	INSERT INTO iso_pp SELECT g, (g * 7919) % 1000, g % 100, repeat('x', 200) FROM generate_series(1, 20000) g;
	INSERT INTO iso_pp SELECT g, (g * 7919) % 1000, g % 100, repeat('x', 200) FROM generate_series(100001, 120000) g;
	CREATE INDEX iso_pp_idx ON iso_pp USING sift_col (id, k, v) WITH (key_cut = 'k');
}

setup
{
	VACUUM (FREEZE, ANALYZE) iso_pp;
}

setup
{
	INSERT INTO iso_pp SELECT g, (g * 7919) % 1000, g % 100, repeat('x', 200) FROM generate_series(20001, 26000) g;

	CREATE FUNCTION iso_pp_q(gate int, OUT agree bool, OUT parallel bool) LANGUAGE plpgsql AS $$
	DECLARE
		q text := 'SELECT k % 7, count(*), sum(v) FROM iso_pp GROUP BY 1 ORDER BY 1';
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
	DROP TABLE iso_pp;
	DROP FUNCTION iso_pp_q(int);
}

session s1
step s1b	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM iso_pp; }
step s1q	{ SELECT agree, parallel FROM iso_pp_q(7003); }
step s1c	{ COMMIT; }

session s2
step s2l	{ SELECT pg_advisory_lock(7003); }
step s2u	{ SELECT pg_advisory_unlock(7003); }

session s3
step s3p	{ SELECT sift.col_promote(indexrelid) >= 0 AS promoted FROM pg_index WHERE indrelid = 'iso_pp_a'::regclass; }
step s3i	{ INSERT INTO iso_pp SELECT g, (g * 7919) % 1000, g % 100, repeat('x', 200) FROM generate_series(30001, 31000) g; }

permutation s1b s2l s1q s3p s3i s2u s1c
