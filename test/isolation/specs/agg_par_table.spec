# The participants of a parallel aggregate over a sift_table scan what the snapshot of the leader sees, whenever each of them opened the table: the leader
# opens and holds on the test gate, the table is maintained and vacuumed (write buffers promoted, tombstones and liveness rewritten), then the workers open and
# take the items. The aggregate (a join of the table to a dimension through a lookup) is the heap's at the snapshot. With the item gate every participant has
# opened before the table changes.
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

	CREATE TABLE iso_pd (id int PRIMARY KEY, g int NOT NULL);
	INSERT INTO iso_pd SELECT g, g % 7 FROM generate_series(0, 1100) g;
	CREATE INDEX iso_pd_col ON iso_pd USING sift_col (id, g);
	ANALYZE iso_pt;
	ANALYZE iso_pd;

	CREATE FUNCTION iso_pt_q(gate int, igate int, OUT agree bool, OUT parallel bool) LANGUAGE plpgsql AS $$
	DECLARE
		q text := 'SELECT d.g, count(*), sum(p.v) FROM iso_pt p JOIN iso_pd d ON d.id = p.k GROUP BY 1 ORDER BY 1';
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
		PERFORM set_config('sift.test_item_gate', igate::text, true);
		PERFORM set_config('sift.test_par_leader_wait', (gate > 0)::text, true);
		EXECUTE 'SELECT string_agg(x::text, '' '') FROM (' || q || ') x' INTO a;
		PERFORM set_config('sift.test_col_open_gate', '0', true);
		PERFORM set_config('sift.test_item_gate', '0', true);
		PERFORM set_config('sift.enable_agg', 'off', true);
		PERFORM set_config('max_parallel_workers_per_gather', '0', true);
		EXECUTE 'SELECT string_agg(x::text, '' '') FROM (' || q || ') x' INTO b;
		agree := a = b;
	END $$;
}

teardown
{
	DROP TABLE iso_pt, iso_pd;
	DROP FUNCTION iso_pt_q(int, int);
}

session s1
step s1b	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM iso_pt; }
step s1q	{ SELECT agree, parallel FROM iso_pt_q(7004, 0); }
step s1g	{ SELECT agree, parallel FROM iso_pt_q(0, 7004); }
step s1c	{ COMMIT; }

session s2
step s2l	{ SELECT pg_advisory_lock(7004); }
step s2u	{ SELECT pg_advisory_unlock(7004); }

session s3
step s3u	{ UPDATE iso_pt SET v = v + 1000, k = k + 1 WHERE id % 5 = 0; }
step s3d	{ DELETE FROM iso_pt WHERE id % 7 = 0; }
step s3i	{ INSERT INTO iso_pt SELECT g, (g * 7919) % 1000, g % 100, repeat('x', 200) FROM generate_series(30001, 31000) g; }
step s3m	{ SELECT busy FROM sift.table_maintain('iso_pt'); }
step s3v	{ VACUUM iso_pt; }

permutation s1b s2l s1q s3u s3d s3i s3m s3v s2u s1c
permutation s1b s2l s1q s3u s3d s3m s3v s2u s1c
permutation s1b s2l s1q s3u s3i s3m s3v s2u s1c
permutation s1b s2l s1q s3d s3i s3m s3v s2u s1c
permutation s1b s2l s1q s3i s3m s3v s2u s1c
permutation s1b s2l s1q s3m s3v s2u s1c
permutation s1b s2l s1g s3u s3d s3i s3m s3v s2u s1c
