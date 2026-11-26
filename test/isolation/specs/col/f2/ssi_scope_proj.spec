# SERIALIZABLE write skew through the reads of a sift_col index of format major 2 that leave the heap out: the scope aggregate that scans the facts of a key (SiftAggV6Scope), the
# summary of a scope, and the scan of a sort projection (SiftProjSortScan). Two transactions each read the rows of one key and delete a row the other one read. The delete writes no index
# entry, so the SIREAD lock of the index relation that index_beginscan takes does not see it: every read must leave the lock of the whole table, and the second committer fails with 40001.
# requires: col

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	DO $$ BEGIN
		IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN
			PERFORM set_config('sift.test_create_format1', 'off', false);
		END IF;
	END $$;
	CREATE TABLE iso_sp (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = off);
	INSERT INTO iso_sp SELECT i, 1 + i % 10 FROM generate_series(1, 360) i;
	CREATE INDEX iso_sp_j ON iso_sp USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
	CREATE TABLE iso_sf (id int PRIMARY KEY, dim_a_id int, kind text, amount int, at timestamp NOT NULL) WITH (autovacuum_enabled = off);
	INSERT INTO iso_sf SELECT i, 1 + (i * 7919) % 360, 'k' || i % 4, i % 100, TIMESTAMP '2025-01-01' + i * interval '1 hour' FROM generate_series(1, 6000) i;
	INSERT INTO iso_sf VALUES (90001, 2, 'k0', 7, '2025-03-01'), (90002, 3, 'k0', 7, '2025-03-01');
	CREATE INDEX iso_sf_col ON iso_sf USING sift_col (id, dim_a_id, kind, amount, at) WITH (scope = 'tenant_id: dim_a_id -> iso_sp(dim_a_id, tenant_id)', track_changes = 'ctid');
	CREATE TABLE iso_pj (id int PRIMARY KEY, k int4, v int4) WITH (autovacuum_enabled = off);
	CREATE INDEX iso_pj_idx ON iso_pj USING sift_col (id, k, v) WITH (key_order = 'id');
	INSERT INTO iso_pj SELECT i, i % 97, i % 50 FROM generate_series(1, 2000) i;
	INSERT INTO iso_pj VALUES (90001, 1000, 7), (90002, 1001, 7);
	SELECT sift.create_projection('iso_pj_idx', 'by_k', 'ORDER BY k, id');
}

setup
{
	VACUUM (FREEZE, ANALYZE) iso_sf, iso_sp, iso_pj;
}

setup
{
	CREATE FUNCTION iso_node(q text) RETURNS text LANGUAGE plpgsql AS $$
	DECLARE l text; r text := 'heap';
	BEGIN
		FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
			IF l ~ 'Custom Scan \(Sift' THEN r := substring(l from 'Custom Scan \((Sift[A-Za-z0-9]+)\)'); END IF;
			IF l ~ 'Index Only Scan using ' THEN r := substring(l from '(Index Only Scan using [a-z_]+)'); END IF;
			IF l ~ 'Scope Read: ' THEN r := r || ', ' || split_part(trim(substr(l, position('Scope Read: ' in l) + 12)), ' (', 1); END IF;
		END LOOP;
		RETURN r;
	END $$;
}

teardown
{
	DROP TABLE iso_sf, iso_sp, iso_pj;
	DROP FUNCTION iso_node(text);
}

session s1
setup		{ SET max_parallel_workers_per_gather = 0; SET seq_page_cost = 1000; SET random_page_cost = 1000; }
step s1b	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1bp	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL sift.enable_agg = off; }
step s1scan	{ SELECT iso_node($$SELECT kind, sum(amount) FROM iso_sf e JOIN iso_sp s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 3 AND e.amount >= 0 GROUP BY kind$$) AS node; }
step s1sum	{ SELECT iso_node($$SELECT sum(e.amount) FROM iso_sf e JOIN iso_sp s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 3$$) AS node; }
step s1proj	{ SELECT iso_node($$SELECT id FROM iso_pj WHERE k BETWEEN 1000 AND 1000 ORDER BY k, id LIMIT 5$$) AS node; }
step s1ios	{ SELECT iso_node($$SELECT id FROM iso_sf WHERE dim_a_id = 2$$) AS node; }
step s1df	{ DELETE FROM iso_sf WHERE id = 90002; }
step s1dp	{ DELETE FROM iso_pj WHERE id = 90002; }
step s1c	{ COMMIT; }

session s2
setup		{ SET max_parallel_workers_per_gather = 0; SET seq_page_cost = 1000; SET random_page_cost = 1000; }
step s2b	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2bp	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL sift.enable_agg = off; }
step s2scan	{ SELECT iso_node($$SELECT kind, sum(amount) FROM iso_sf e JOIN iso_sp s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 4 AND e.amount >= 0 GROUP BY kind$$) AS node; }
step s2sum	{ SELECT iso_node($$SELECT sum(e.amount) FROM iso_sf e JOIN iso_sp s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 4$$) AS node; }
step s2proj	{ SELECT iso_node($$SELECT id FROM iso_pj WHERE k BETWEEN 1001 AND 1001 ORDER BY k, id LIMIT 5$$) AS node; }
step s2ios	{ SELECT iso_node($$SELECT id FROM iso_sf WHERE dim_a_id = 3$$) AS node; }
step s2df	{ DELETE FROM iso_sf WHERE id = 90001; }
step s2dp	{ DELETE FROM iso_pj WHERE id = 90001; }
step s2c	{ COMMIT; }

session s3
step s3sum	{ SELECT iso_node($$SELECT sum(e.amount) FROM iso_sf e JOIN iso_sp s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 3$$) AS node; }

# the facts of the key, read heap-free
permutation s1b s2b s1scan s2scan s1df s2df s1c s2c
# the summary answers outside SERIALIZABLE, and a SERIALIZABLE read runs the join instead
permutation s3sum s1b s2b s1sum s2sum s1df s2df s1c s2c
# the scan of the sort projection
permutation s1bp s2bp s1proj s2proj s1dp s2dp s1c s2c
# the index only scan of the index, whose rows on all-visible pages leave the SIREAD lock of their heap page
permutation s1bp s2bp s1ios s2ios s1df s2df s1c s2c
