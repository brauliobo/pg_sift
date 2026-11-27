# A scope read with workers is one read: the leader decides it from the directory and the pairs of its snapshot, and the participants read that decision and that directory. Pairs and facts edited, and the buffers
# promoted and the segments merged, while the participants are between the decision and their first item (or while the leader is at the open of the directory) change nothing of what the snapshot reads:
# the answer is the join's at the snapshot, and the workers started. The first read holds on the gate of the open of the directory, the second on the gate every participant takes before its first item, the third on the gate every worker takes before it opens anything (a worker that read the directory, the pairs or the snapshot itself would read the edits).
# requires: col gates

setup
{
	SET sift.test_create_format1 = off;
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE EXTENSION IF NOT EXISTS pg_sift_test;
	CREATE TABLE iso_sp_pairs (dim_a_id int, group_id int, PRIMARY KEY (dim_a_id, group_id));
	INSERT INTO iso_sp_pairs SELECT m, 1 + (m * 5 + j * 7) % 6 FROM generate_series(1, 400) m, generate_series(0, m % 3) j ON CONFLICT DO NOTHING;
	CREATE INDEX iso_sp_pairs_journal ON iso_sp_pairs USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid');
	CREATE TABLE iso_sp (id int PRIMARY KEY, dim_a_id int, v int, pad text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_sp SELECT g, 1 + (g * 7919) % 400, g % 10, repeat('x', 60) FROM generate_series(1, 30000) g;
}

setup
{
	CREATE INDEX iso_sp_idx ON iso_sp USING sift_col (id, dim_a_id, v) WITH (scope = 'group_id: dim_a_id -> iso_sp_pairs(dim_a_id, group_id)');
}

setup
{
	REINDEX INDEX iso_sp_idx;
}

setup
{
	VACUUM (FREEZE, ANALYZE) iso_sp, iso_sp_pairs;
}

# the read starts with changed pairs (the patch) and rows in the write buffer (the delta)
setup
{
	INSERT INTO iso_sp SELECT 30000 + g, 1 + (g * 31) % 400, 1, 'y' FROM generate_series(1, 300) g;
	INSERT INTO iso_sp_pairs SELECT m, 3 FROM generate_series(1, 400, 5) m ON CONFLICT DO NOTHING;
	DELETE FROM iso_sp_pairs WHERE group_id = 3 AND dim_a_id % 7 = 0;

	CREATE FUNCTION iso_sp_q(gate int, item_gate int, par_gate bool, OUT agree bool, OUT parallel bool, OUT v6 bool) LANGUAGE plpgsql AS $$
	DECLARE
		q text := 'SELECT count(*) || '':'' || sum(v) FROM iso_sp s WHERE sift.in_scope(''iso_sp_idx''::regclass, ''group_id'', s.dim_a_id, 3)';
		a text;
		b text;
		p jsonb;
	BEGIN
		PERFORM set_config('sift.agg_force', 'on', true);
		PERFORM set_config('sift.test_scope_patch', 'on', true);
		PERFORM set_config('max_parallel_workers_per_gather', '2', true);
		PERFORM set_config('parallel_setup_cost', '0', true);
		PERFORM set_config('parallel_tuple_cost', '0', true);
		PERFORM set_config('min_parallel_table_scan_size', '0', true);
		PERFORM set_config('sift.test_agg_par_min_rows', '0', true);
		PERFORM set_config('sift.test_col_open_gate', gate::text, true);
		PERFORM set_config('sift.test_item_gate', item_gate::text, true);
		PERFORM set_config('sift.test_par_gate', par_gate::text, true);
		EXECUTE q INTO a;
		PERFORM set_config('sift.test_col_open_gate', '0', true);
		PERFORM set_config('sift.test_item_gate', '0', true);
		PERFORM set_config('sift.test_par_gate', 'off', true);
		EXECUTE 'EXPLAIN (COSTS OFF, TIMING OFF, SUMMARY OFF, ANALYZE, FORMAT JSON) ' || q INTO p;
		parallel := coalesce(jsonb_path_query_first(p, '$.**."Workers Launched"')::text::int, 0) > 0;
		v6 := p::text LIKE '%SiftAggV6%';
		PERFORM set_config('sift.enable_agg', 'off', true);
		EXECUTE 'SELECT count(*) || '':'' || sum(v) FROM iso_sp s WHERE s.dim_a_id IN (SELECT dim_a_id FROM iso_sp_pairs s2 WHERE group_id = 3)' INTO b;
		agree := a = b;
	END $$;
}

teardown
{
	DROP FUNCTION iso_sp_q(int, int, bool);
	DROP TABLE iso_sp, iso_sp_pairs;
}

session s1
setup		{ SET sift.test_create_format1 = off; }
step s1b	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM iso_sp; }
step s1o	{ SELECT agree, parallel, v6 FROM iso_sp_q(7021, 0, false); }
step s1i	{ SELECT agree, parallel, v6 FROM iso_sp_q(0, 7022, false); }
step s1w	{ SELECT agree, parallel, v6 FROM iso_sp_q(0, 0, true); }
step s1c	{ COMMIT; }

session s2
setup		{ SET sift.test_create_format1 = off; }
step s2lo	{ SELECT pg_advisory_lock(7021); }
step s2uo	{ SELECT pg_advisory_unlock(7021); }
step s2li	{ SELECT pg_advisory_lock(7022); }
step s2ui	{ SELECT pg_advisory_unlock(7022); }
step s2lw	{ SELECT pg_advisory_lock(20983, 10); }
step s2uw	{ SELECT pg_advisory_unlock(20983, 10); }

session s3
setup		{ SET sift.test_create_format1 = off; }
step s3e	{ INSERT INTO iso_sp SELECT 30300 + g, 1 + (g * 17) % 400, 2, 'z' FROM generate_series(1, 200) g;
		  INSERT INTO iso_sp_pairs SELECT m, 3 FROM generate_series(2, 400, 9) m ON CONFLICT DO NOTHING;
		  DELETE FROM iso_sp_pairs WHERE group_id = 3 AND dim_a_id % 5 = 0; }
step s3b	{ INSERT INTO iso_sp SELECT 40000 + g, 1 + (g * 13) % 400, 3, repeat('w', 60) FROM generate_series(1, 60000) g; }
step s3p	{ SELECT sift.col_promote('iso_sp_idx') >= 0 AS promoted; }
step s3m	{ SELECT sift.col_merge('iso_sp_idx', true) >= 0 AS merged; }

permutation s1b s2lo s1o s3e s3p s2uo s1c
permutation s1b s2li s1i s3e s3p s3m s2ui s1c
permutation s1b s2lw s1w s3e s3b s3p s3m s2uw s1c
