# The dimension map of a dimension that has a journal of its changed keys (a child index is connected to it): the map one backend read from the leaves is stamped with the
# horizon of its snapshot and serves the queries of the others while no row was inserted into the dimension since and no key was changed in it, vacuum or none. A write
# in flight, committed or rolled back, an update, a delete, an insert and a snapshot older than the horizon of the map all give the rows of the normal plan.
# requires: build col

setup
{
	CREATE TABLE iso_dm_dim (k int PRIMARY KEY, a int, b int) WITH (autovacuum_enabled = off);
	INSERT INTO iso_dm_dim SELECT i, i % 5, i % 3 FROM generate_series(0, 399) i;
	CREATE INDEX iso_dm_dim_col ON iso_dm_dim USING sift_col (k, a, b);
	CREATE TABLE iso_dm (id int PRIMARY KEY, k int, v int) WITH (autovacuum_enabled = off);
	INSERT INTO iso_dm SELECT i, i % 450, i % 7 FROM generate_series(1, 5000) i;
	CREATE INDEX iso_dm_col ON iso_dm USING sift_col (id, k, v) WITH (connect = 'k -> iso_dm_dim(k)');
}

setup		{ VACUUM (FREEZE, ANALYZE) iso_dm; }
setup		{ VACUUM (FREEZE, ANALYZE) iso_dm_dim; }

setup
{
	-- the maps the node shared and made, and whether its rows are those of the normal plan, in the snapshot of the statement
	CREATE FUNCTION iso_dm_check(q text) RETURNS text LANGUAGE plpgsql AS $$
	DECLARE
		l text;
		shared int := 0;
		built int := 0;
		differences bigint;
	BEGIN
		FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
			IF l ~ '(\d+) maps shared' THEN shared := substring(l from '(\d+) maps shared')::int; END IF;
			IF l ~ '(\d+) maps built' THEN built := substring(l from '(\d+) maps built')::int; END IF;
		END LOOP;
		EXECUTE 'CREATE TEMP TABLE iso_dm_on AS ' || q;
		PERFORM set_config('sift.enable_agg', 'off', true);
		EXECUTE 'CREATE TEMP TABLE iso_dm_off AS ' || q;
		PERFORM set_config('sift.enable_agg', 'on', true);
		differences := (SELECT count(*) FROM ((TABLE iso_dm_on EXCEPT TABLE iso_dm_off) UNION ALL (TABLE iso_dm_off EXCEPT TABLE iso_dm_on)) d);
		DROP TABLE iso_dm_on, iso_dm_off;
		RETURN format('%s maps shared, %s made, %s differences from the normal plan', shared, built, differences);
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
setup		{ SET max_parallel_workers_per_gather = 0; SET seq_page_cost = 1000; SET random_page_cost = 1000; SET sift.agg_lookup_cache_kb = 0; SET sift.agg_lookup_shared_kb = 0; SET sift.agg_dimmap_kb = 8192; }
step s1run	{ SELECT iso_dm_run(); }
step s1runl	{ SELECT iso_dm_runl(); }

session s2
setup		{ SET max_parallel_workers_per_gather = 0; SET seq_page_cost = 1000; SET random_page_cost = 1000; SET sift.agg_lookup_cache_kb = 0; SET sift.agg_lookup_shared_kb = 0; SET sift.agg_dimmap_kb = 8192; }
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

# one backend makes the map, the other reads it, and so does the first one again
permutation s1run s2run s1run s2run
# a committed update that nobody vacuums: the next statement makes a map of its own, the others share it
permutation s1run s2run s3upd s1run s2run s1run
# a write in flight: no map is the one of the readers while it lasts; committed, the next statement makes one and the other shares it
permutation s1run s2run s3begin s3upd s3ins s2run s1run s3commit s1run s2run
# rolled back: the map of before is still the table's
permutation s1run s2run s3begin s3ins s3del s3abort s2run s1run
# a delete, committed; the facts of the deleted key are dropped by the join and kept by the left join
permutation s1run s1runl s2runl s3del s1runl s2run s2runl s1run
# an insert of a key the facts name (an orphan until now)
permutation s1run s2run s3ins s1runl s2runl s1run s2run
# a snapshot older than a committed write reads the rows of its own snapshot, whatever the other backend made after the write
permutation s1run s2run s2rr s3upd s3del s1run s2run s2end s2run s1run
