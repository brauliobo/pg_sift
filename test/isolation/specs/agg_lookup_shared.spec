# The rows a Custom Scan (SiftAgg) kept of a joined table in shared memory (sift.agg_lookup_shared_kb) serve the queries of the other backends only while the table
# is what it was: a write in flight, committed or rolled back, an update, a delete, and a snapshot older than a committed write all give the rows of the normal plan.
# The sessions keep nothing of their own (sift.agg_lookup_cache_kb = 0): a cached table is one another backend loaded.
# requires: build col

setup
{
	CREATE TABLE iso_ls (id int PRIMARY KEY, dim_a_id int, kind text, code text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_ls SELECT i, i % 40, (array['a', 'b', 'c'])[1 + i % 3], 'c' || (i % 40) FROM generate_series(1, 4000) i;
	CREATE INDEX iso_ls_col ON iso_ls USING sift_col (id, dim_a_id, kind, code);
	CREATE TABLE iso_ls_dim (id int, country int, label text, code text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_ls_dim SELECT i, i % 4, 'e' || i, 'c' || i FROM generate_series(0, 39) i;
}

setup		{ VACUUM (FREEZE, ANALYZE) iso_ls; }
setup		{ VACUUM (FREEZE, ANALYZE) iso_ls_dim; }

setup
{
	-- the tables the node took from the shared cache, and whether its rows are those of the normal plan, in the snapshot of the statement
	CREATE FUNCTION iso_ls_check(q text) RETURNS text LANGUAGE plpgsql AS $$
	DECLARE
		l text;
		nodes int := 0;
		kept int := 0;
		differences bigint;
	BEGIN
		FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
			IF l LIKE '%Custom Scan (SiftAgg)%' THEN nodes := nodes + 1; END IF;
			IF l LIKE '%Cached Lookup Tables:%' THEN kept := kept + trim(split_part(l, ':', 2))::int; END IF;
		END LOOP;
		EXECUTE 'CREATE TEMP TABLE iso_ls_on AS ' || q;
		PERFORM set_config('sift.enable_agg', 'off', true);
		EXECUTE 'CREATE TEMP TABLE iso_ls_off AS ' || q;
		PERFORM set_config('sift.enable_agg', 'on', true);
		differences := (SELECT count(*) FROM ((TABLE iso_ls_on EXCEPT TABLE iso_ls_off) UNION ALL (TABLE iso_ls_off EXCEPT TABLE iso_ls_on)) d);
		DROP TABLE iso_ls_on, iso_ls_off;
		RETURN format('%s SiftAgg nodes, %s cached tables, %s differences from the normal plan', nodes, kept, differences);
	END $$;
	CREATE FUNCTION iso_ls_run() RETURNS text LANGUAGE sql AS $$
		SELECT iso_ls_check('SELECT d.label, f.kind, count(*) AS n FROM iso_ls f JOIN iso_ls_dim d ON d.id = f.dim_a_id WHERE d.country = 1 GROUP BY d.label, f.kind') $$;
	CREATE FUNCTION iso_ls_runt() RETURNS text LANGUAGE sql AS $$
		SELECT iso_ls_check('SELECT d.label, f.kind, count(*) AS n FROM iso_ls f LEFT JOIN iso_ls_dim d ON d.code = f.code GROUP BY d.label, f.kind') $$;
}

teardown
{
	DROP FUNCTION iso_ls_run(), iso_ls_runt(), iso_ls_check(text);
	DROP TABLE iso_ls, iso_ls_dim;
}

session s1
setup		{ SET max_parallel_workers_per_gather = 0; SET seq_page_cost = 1000; SET random_page_cost = 1000; SET sift.agg_lookup_cache_kb = 0; SET sift.agg_lookup_shared_kb = 8192; }
step s1run	{ SELECT iso_ls_run(); }
step s1runt	{ SELECT iso_ls_runt(); }

session s2
setup		{ SET max_parallel_workers_per_gather = 0; SET seq_page_cost = 1000; SET random_page_cost = 1000; SET sift.agg_lookup_cache_kb = 0; SET sift.agg_lookup_shared_kb = 8192; }
step s2run	{ SELECT iso_ls_run(); }
step s2runt	{ SELECT iso_ls_runt(); }
step s2rr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM iso_ls_dim; }
step s2end	{ COMMIT; }

session s3
step s3begin	{ BEGIN; }
step s3upd	{ UPDATE iso_ls_dim SET country = 1 WHERE id = 3; }
step s3updt	{ UPDATE iso_ls_dim SET label = 'renamed', code = 'c4' WHERE id = 7; }
step s3ins	{ INSERT INTO iso_ls_dim VALUES (2, 1, 'late'); }
step s3del	{ DELETE FROM iso_ls_dim WHERE id = 5; }
step s3commit	{ COMMIT; }
step s3abort	{ ROLLBACK; }
step s3vacuum	{ VACUUM iso_ls_dim; }

# one backend loads the table, the other reads it, and so does the first one again (each backend of them has nothing of its own)
permutation s1run s2run s1run s2run
# a write in flight, then committed: the rows are loaded again by whoever runs next; vacuumed, they are kept again for the other
permutation s1run s2run s3begin s3upd s3ins s2run s3commit s1run s3vacuum s2run s1run s2run
# rolled back
permutation s1run s2run s3begin s3ins s3del s3abort s2run s3vacuum s1run s2run
# a delete, committed
permutation s1run s2run s3del s1run s3vacuum s2run s1run
# a snapshot older than a committed write and its vacuum reads the rows of its own snapshot, whatever the other backend kept
permutation s1run s2run s2rr s3upd s3del s3vacuum s2run s1run s2end s2run s1run
# a text key: a changed label and key give the rows of the normal plan, and the rows are kept again once vacuumed
permutation s1runt s2runt s3begin s3updt s2runt s3commit s1runt s3vacuum s2runt s1runt
