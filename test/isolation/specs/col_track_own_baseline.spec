# A transaction that took its snapshot before it ran REINDEX, CLUSTER or ALTER INDEX SET (track_changes) on a sift_col index with track_changes, and reads after another session
# committed changes, reads the rows its snapshot sees: the baseline the command writes ends the versions that were already dead, and the transaction that wrote it must not take its own
# baseline for deletes that its older snapshot sees through. Every read is the plain heap plan, the scan forced and the index scan of the same query, and they agree.
# requires: col

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE iso_own_k (id text PRIMARY KEY, tenant int NOT NULL, amount int NOT NULL) WITH (autovacuum_enabled = off);
	CREATE TABLE iso_own_c (id text PRIMARY KEY, tenant int NOT NULL, amount int NOT NULL) WITH (autovacuum_enabled = off);
	INSERT INTO iso_own_k SELECT 'c' || i, i % 10, i FROM generate_series(1, 4000) i;
	INSERT INTO iso_own_c SELECT 'c' || i, i % 10, i FROM generate_series(1, 4000) i;
	CREATE INDEX iso_own_k_c ON iso_own_k USING sift_col (id, tenant, amount) WITH (track_changes = 'id');
	CREATE INDEX iso_own_c_c ON iso_own_c USING sift_col (id, tenant, amount) WITH (track_changes = 'ctid');
}

setup
{
	VACUUM (FREEZE, ANALYZE) iso_own_k;
}

setup
{
	VACUUM (FREEZE, ANALYZE) iso_own_c;
}

setup
{
	-- the answers of three plans of the same queries: the heap, the scan, the index scan
	CREATE FUNCTION iso_own_q(tbl text, mode text) RETURNS text LANGUAGE plpgsql AS $$
	DECLARE r text := ''; x text; q text;
	BEGIN
		PERFORM set_config('max_parallel_workers_per_gather', '0', true);
		IF mode = 'heap' THEN
			PERFORM set_config('sift.enable_agg', 'off', true);
			PERFORM set_config('sift.enable_col_scan', 'off', true);
			PERFORM set_config('enable_indexscan', 'off', true);
			PERFORM set_config('enable_bitmapscan', 'off', true);
		ELSIF mode = 'scan' THEN
			PERFORM set_config('sift.enable_agg', 'on', true);
			PERFORM set_config('sift.agg_force', 'on', true);
			PERFORM set_config('sift.enable_col_scan', 'on', true);
		ELSE
			PERFORM set_config('sift.enable_agg', 'off', true);
			PERFORM set_config('enable_seqscan', 'off', true);
			PERFORM set_config('enable_bitmapscan', 'off', true);
		END IF;
		FOREACH q IN ARRAY ARRAY['SELECT tenant, count(*), sum(amount) FROM %s GROUP BY 1 ORDER BY 1',
								 'SELECT id, tenant, amount FROM %s WHERE id IN (''c1003'', ''c1016'', ''c1029'', ''c1042'', ''c4010'', ''c4500'') ORDER BY 1'] LOOP
			EXECUTE format('SELECT coalesce(string_agg(t::text, '';'' ORDER BY t::text), ''none'') FROM (' || q || ') t', tbl) INTO x;
			r := r || x || '|';
		END LOOP;
		RETURN md5(r);
	END $$;
	CREATE FUNCTION iso_own_same(tbl text) RETURNS text LANGUAGE plpgsql AS $$
	DECLARE h text := iso_own_q(tbl, 'heap'); s text := iso_own_q(tbl, 'scan'); i text := iso_own_q(tbl, 'index');
	BEGIN
		RETURN format('%s: scan %s, index scan %s', tbl, CASE WHEN s = h THEN 'agrees' ELSE 'DIFFERS' END, CASE WHEN i = h THEN 'agrees' ELSE 'DIFFERS' END);
	END $$;
}

teardown
{
	DROP TABLE iso_own_k, iso_own_c;
	DROP FUNCTION iso_own_q(text, text);
	DROP FUNCTION iso_own_same(text);
}

session s1
step s1b	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1r	{ SELECT count(*) AS first_read FROM iso_own_k, iso_own_c WHERE iso_own_k.id = 'c1' AND iso_own_c.id = 'c1'; }
step s1k2c	{ ALTER INDEX iso_own_k_c SET (track_changes = 'ctid'); }
step s1c2k	{ ALTER INDEX iso_own_c_c SET (track_changes = 'id'); }
step s1reindex	{ REINDEX INDEX iso_own_k_c; }
step s1cluster	{ CLUSTER iso_own_k USING iso_own_k_pkey; }
step s1qk	{ SELECT iso_own_same('iso_own_k'); }
step s1qc	{ SELECT iso_own_same('iso_own_c'); }
step s1e	{ ROLLBACK; }

session s2
step s2w	{
	UPDATE iso_own_k SET amount = amount + 142 WHERE id IN ('c1003', 'c1016', 'c1029', 'c1042');
	UPDATE iso_own_c SET amount = amount + 142 WHERE id IN ('c1003', 'c1016', 'c1029', 'c1042');
	DELETE FROM iso_own_k WHERE id IN ('c4010', 'c4020');
	DELETE FROM iso_own_c WHERE id IN ('c4010', 'c4020');
	INSERT INTO iso_own_k SELECT 'c' || i, i % 10, i FROM generate_series(4501, 4520) i;
	INSERT INTO iso_own_c SELECT 'c' || i, i % 10, i FROM generate_series(4501, 4520) i;
}

permutation s1b s1r s2w s1qk s1qc s1e
permutation s1b s1r s2w s1k2c s1qk s1e
permutation s1b s1r s2w s1c2k s1qc s1e
permutation s1b s1r s2w s1reindex s1qk s1e
permutation s1b s1r s2w s1cluster s1qk s1e
