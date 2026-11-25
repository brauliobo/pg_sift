# A read of a scope key takes the directory of the index once: the buffered rows it copies for each pair of their member (the delta) and the scope units it reads belong to that one
# directory. A promotion between the two moved the buffered rows into the scope units the scan then read, and the delta copied them again: the read counted them twice. The read waits on the
# test gate right after it read the directory while the write buffer is promoted; the count is the join's.
# requires: col gates

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE EXTENSION IF NOT EXISTS pg_sift_test;
	CREATE TABLE iso_sd_pairs (dim_a_id int, group_id int, PRIMARY KEY (dim_a_id, group_id));
	INSERT INTO iso_sd_pairs SELECT m, 1 + (m * 5 + j * 7) % 6 FROM generate_series(1, 200) m, generate_series(0, m % 3) j ON CONFLICT DO NOTHING;
	CREATE INDEX iso_sd_pairs_journal ON iso_sd_pairs USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid');
	CREATE TABLE iso_sd (id int PRIMARY KEY, dim_a_id int, v int, pad text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_sd SELECT g, 1 + (g * 7919) % 200, g % 10, repeat('x', 100) FROM generate_series(1, 6000) g;
}

setup
{
	CREATE INDEX iso_sd_idx ON iso_sd USING sift_col (id, dim_a_id, v) WITH (scope = 'group_id: dim_a_id -> iso_sd_pairs(dim_a_id, group_id)');
}

setup
{
	REINDEX INDEX iso_sd_idx;
}

setup
{
	VACUUM (FREEZE, ANALYZE) iso_sd, iso_sd_pairs;
}

setup
{
	INSERT INTO iso_sd SELECT 6000 + g, 1 + (g * 31) % 200, 1, 'y' FROM generate_series(1, 300) g;

	CREATE FUNCTION iso_sd_q(gate int, OUT agree bool, OUT stored bool) LANGUAGE plpgsql AS $$
	DECLARE
		a text;
		b text;
		p text;
	BEGIN
		PERFORM set_config('sift.agg_force', 'on', true);
		PERFORM set_config('sift.test_scope_patch', 'on', true);
		PERFORM set_config('sift.test_col_open_gate', gate::text, true);
		EXECUTE 'SELECT count(*) || '':'' || sum(v) FROM iso_sd s WHERE sift.in_scope(''iso_sd_idx''::regclass, ''group_id'', s.dim_a_id, 3)' INTO a;
		PERFORM set_config('sift.test_col_open_gate', '0', true);
		EXECUTE 'EXPLAIN (COSTS OFF, TIMING OFF, SUMMARY OFF, ANALYZE, FORMAT JSON) SELECT count(*), sum(v) FROM iso_sd s WHERE sift.in_scope(''iso_sd_idx''::regclass, ''group_id'', s.dim_a_id, 3)' INTO p;
		stored := p LIKE '%Scope Units Read%';
		PERFORM set_config('sift.enable_agg', 'off', true);
		EXECUTE 'SELECT count(*) || '':'' || sum(v) FROM iso_sd s WHERE s.dim_a_id IN (SELECT dim_a_id FROM iso_sd_pairs WHERE group_id = 3)' INTO b;
		agree := a = b;
	END $$;
}

teardown
{
	DROP FUNCTION iso_sd_q(int);
	DROP TABLE iso_sd, iso_sd_pairs;
}

session s1
step s1b	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM iso_sd; }
step s1q	{ SELECT agree, stored FROM iso_sd_q(7003); }
step s1c	{ COMMIT; }

session s2
step s2l	{ SELECT pg_advisory_lock(7003); }
step s2u	{ SELECT pg_advisory_unlock(7003); }

session s3
step s3p	{ SELECT sift.col_promote('iso_sd_idx') >= 0 AS promoted; }

permutation s1b s2l s1q s3p s2u s1c
