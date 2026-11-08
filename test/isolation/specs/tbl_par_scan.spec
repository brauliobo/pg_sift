# The workers of a parallel sequential scan of a sift_table read the rows the snapshot of the leader sees, whenever they open: the leader captures the write
# buffers and the directory when it prepares the scan and holds on the test gate, the table is maintained and vacuumed, then the workers open.
# requires: tam gates

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE iso_pt (id int, k int, v int, pad text) USING sift_table WITH (autovacuum_enabled = false);
	ALTER TABLE iso_pt ALTER COLUMN pad SET STORAGE PLAIN;
	INSERT INTO iso_pt SELECT g, (g * 7919) % 1000, g % 100, repeat('x', 200) FROM generate_series(1, 20000) g;
	SELECT busy FROM sift.table_maintain('iso_pt');
	INSERT INTO iso_pt SELECT g, (g * 7919) % 1000, g % 100, repeat('x', 200) FROM generate_series(20001, 26000) g;
	UPDATE iso_pt SET v = v + 1, k = k + 2 WHERE id % 3 = 0;
	DELETE FROM iso_pt WHERE id % 11 = 0;

	CREATE FUNCTION iso_ps_q(gate int, OUT agree bool, OUT parallel bool) LANGUAGE plpgsql AS $$
	DECLARE
		q text := 'SELECT count(*), sum(v), sum(k) FROM iso_pt';
		a text;
		b text;
		l text;
	BEGIN
		PERFORM set_config('sift.enable_agg', 'off', true);
		PERFORM set_config('max_parallel_workers_per_gather', '2', true);
		PERFORM set_config('parallel_setup_cost', '0', true);
		PERFORM set_config('parallel_tuple_cost', '0', true);
		PERFORM set_config('min_parallel_table_scan_size', '0', true);
		parallel := false;
		FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
			parallel := parallel OR l ~ 'Parallel Seq Scan';
		END LOOP;
		PERFORM set_config('sift.test_col_open_gate', gate::text, true);
		EXECUTE q INTO a;
		PERFORM set_config('sift.test_col_open_gate', '0', true);
		PERFORM set_config('max_parallel_workers_per_gather', '0', true);
		EXECUTE q INTO b;
		agree := a = b;
	END $$;
}

teardown
{
	DROP TABLE iso_pt;
	DROP FUNCTION iso_ps_q(int);
}

session s1
step s1b	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM iso_pt; }
step s1q	{ SELECT agree, parallel FROM iso_ps_q(7004); }
step s1c	{ COMMIT; }

session s2
step s2l	{ SELECT pg_advisory_lock(7004); }
step s2u	{ SELECT pg_advisory_unlock(7004); }

session s3
step s3u	{ UPDATE iso_pt SET v = v + 1000, k = k + 1 WHERE id % 5 = 0; }
step s3d	{ DELETE FROM iso_pt WHERE id % 7 = 0; }
step s3m	{ SELECT busy FROM sift.table_maintain('iso_pt'); }
step s3v	{ VACUUM iso_pt; }

permutation s1b s2l s1q s3m s3v s2u s1c
permutation s1b s2l s1q s3u s3d s3m s3v s2u s1c
