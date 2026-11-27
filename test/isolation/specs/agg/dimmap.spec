# A Custom Scan (SiftAggV6) that joins a dimension with a column index of its own, run by two backends in turn, reads the dimension in the snapshot of each
# statement: a write in flight, committed or rolled back, an update, a delete, an insert, and a snapshot older than a committed write all give the rows of the normal
# plan, vacuumed or not.
# requires: build col

setup
{
	CREATE TABLE iso_dm (id int PRIMARY KEY, k int, v int) WITH (autovacuum_enabled = off);
	INSERT INTO iso_dm SELECT i, i % 450, i % 7 FROM generate_series(1, 5000) i;
	CREATE INDEX iso_dm_col ON iso_dm USING sift_col (id, k, v);
	CREATE TABLE iso_dm_dim (k int PRIMARY KEY, a int, b int) WITH (autovacuum_enabled = off);
	INSERT INTO iso_dm_dim SELECT i, i % 5, i % 3 FROM generate_series(0, 399) i;
	CREATE INDEX iso_dm_dim_col ON iso_dm_dim USING sift_col (k, a, b);
}

setup		{ VACUUM (FREEZE, ANALYZE) iso_dm; }
setup		{ VACUUM (FREEZE, ANALYZE) iso_dm_dim; }

setup
{
	-- the SiftAggV6 nodes of the plan, and whether its rows are those of the normal plan, in the snapshot of the statement
	CREATE FUNCTION iso_dm_check(q text) RETURNS text LANGUAGE plpgsql AS $$
	DECLARE
		l text;
		nodes int := 0;
		differences bigint;
	BEGIN
		FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
			IF l LIKE '%Custom Scan (SiftAggV6)%' THEN nodes := nodes + 1; END IF;
		END LOOP;
		EXECUTE 'CREATE TEMP TABLE iso_dm_on AS ' || q;
		PERFORM set_config('sift.enable_agg', 'off', true);
		EXECUTE 'CREATE TEMP TABLE iso_dm_off AS ' || q;
		PERFORM set_config('sift.enable_agg', 'on', true);
		differences := (SELECT count(*) FROM ((TABLE iso_dm_on EXCEPT TABLE iso_dm_off) UNION ALL (TABLE iso_dm_off EXCEPT TABLE iso_dm_on)) d);
		DROP TABLE iso_dm_on, iso_dm_off;
		RETURN format('%s SiftAgg nodes, %s differences from the normal plan', nodes, differences);
	END $$;
	CREATE FUNCTION iso_dm_run() RETURNS text LANGUAGE sql AS $$
		SELECT iso_dm_check('SELECT d.a, d.b, count(*) AS n, sum(f.v) AS s FROM iso_dm f JOIN iso_dm_dim d ON d.k = f.k GROUP BY d.a, d.b') $$;
	CREATE FUNCTION iso_dm_runl() RETURNS text LANGUAGE sql AS $$
		SELECT iso_dm_check('SELECT d.a, count(*) AS n FROM iso_dm f LEFT JOIN iso_dm_dim d ON d.k = f.k GROUP BY d.a') $$;
}

teardown
{
	DROP FUNCTION iso_dm_run(), iso_dm_runl(), iso_dm_check(text);
	DROP TABLE iso_dm, iso_dm_dim;
}

session s1
setup		{ SET max_parallel_workers_per_gather = 0; SET seq_page_cost = 1000; SET random_page_cost = 1000; SET sift.backend_cache_kb = 0; SET sift.shared_cache_kb = 0; SET sift.shared_cache_kb = 8192; }
step s1run	{ SELECT iso_dm_run(); }
step s1runl	{ SELECT iso_dm_runl(); }

session s2
setup		{ SET max_parallel_workers_per_gather = 0; SET seq_page_cost = 1000; SET random_page_cost = 1000; SET sift.backend_cache_kb = 0; SET sift.shared_cache_kb = 0; SET sift.shared_cache_kb = 8192; }
step s2run	{ SELECT iso_dm_run(); }
step s2runl	{ SELECT iso_dm_runl(); }
step s2rr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM iso_dm_dim; }
step s2end	{ COMMIT; }

session s3
step s3begin	{ BEGIN; }
step s3upd	{ UPDATE iso_dm_dim SET a = 4, b = 2 WHERE k = 3; }
step s3ins	{ INSERT INTO iso_dm_dim VALUES (420, 1, 1); }
step s3del	{ DELETE FROM iso_dm_dim WHERE k = 5; }
step s3commit	{ COMMIT; }
step s3abort	{ ROLLBACK; }
step s3vacuum	{ VACUUM iso_dm_dim; }

# one backend, the other, and the first one again
permutation s1run s2run s1run s2run
# a write in flight, then committed, then vacuumed
permutation s1run s2run s3begin s3upd s3ins s2run s3commit s1run s3vacuum s2run s1run s2run
# rolled back
permutation s1run s2run s3begin s3ins s3del s3abort s2run s3vacuum s1run s2run
# a delete, committed; the facts of the deleted key are dropped by the join and kept by the left join
permutation s1run s1runl s2runl s3del s1runl s2run s3vacuum s2runl s1runl s2run
# a snapshot older than a committed write and its vacuum reads the rows of its own snapshot
permutation s1run s2run s2rr s3upd s3del s3vacuum s2run s1run s2end s2run s1run
