# A membership list read through its btree takes the keys from the index entries (agg_lookup_index_keys): an entry of a write that another session has in flight (an
# insert, a delete, an update of the group) is not the snapshot's, whether the write is committed or rolled back afterwards, and the rows are the normal plan's.
# requires: build col

setup
{
	CREATE TABLE iso_lk (id int PRIMARY KEY, k int, kind int2) WITH (autovacuum_enabled = off);
	INSERT INTO iso_lk SELECT i, i % 500, (i % 5)::int2 FROM generate_series(1, 5000) i;
	CREATE INDEX iso_lk_col ON iso_lk USING sift_col (id, k, kind);
	CREATE TABLE iso_lk_pair (grp int, k int) WITH (autovacuum_enabled = off);
	INSERT INTO iso_lk_pair SELECT g, (g * 37 + i * 11) % 500 FROM generate_series(1, 3) g, generate_series(1, 100) i;
	CREATE INDEX iso_lk_pair_gk ON iso_lk_pair (grp, k);
	CREATE FUNCTION iso_lk_run() RETURNS text LANGUAGE plpgsql AS $$
	DECLARE
		l text;
		nodes int := 0;
		q text := 'SELECT kind, count(*) AS n FROM iso_lk WHERE k IN (SELECT k FROM iso_lk_pair WHERE grp = 2) GROUP BY kind';
		events bigint;
		differences bigint;
	BEGIN
		FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
			IF l LIKE '%Custom Scan (SiftAgg)%' THEN nodes := nodes + 1; END IF;
		END LOOP;
		EXECUTE 'CREATE TEMP TABLE iso_lk_on AS ' || q;
		PERFORM set_config('sift.enable_agg', 'off', true);
		EXECUTE 'CREATE TEMP TABLE iso_lk_off AS ' || q;
		PERFORM set_config('sift.enable_agg', 'on', true);
		differences := (SELECT count(*) FROM ((TABLE iso_lk_on EXCEPT TABLE iso_lk_off) UNION ALL (TABLE iso_lk_off EXCEPT TABLE iso_lk_on)) d);
		events := (SELECT sum(n) FROM iso_lk_on);
		DROP TABLE iso_lk_on, iso_lk_off;
		RETURN format('%s SiftAgg nodes, %s events, %s differences from the normal plan', nodes, events, differences);
	END $$;
}

setup		{ VACUUM (FREEZE, ANALYZE) iso_lk; }
setup		{ VACUUM (FREEZE, ANALYZE) iso_lk_pair; }

teardown
{
	DROP FUNCTION iso_lk_run();
	DROP TABLE iso_lk, iso_lk_pair;
}

session s1
setup		{ SET max_parallel_workers_per_gather = 0; SET sift.agg_force = on; SET sift.agg_lookup_cache_kb = 0; SET sift.agg_lookup_shared_kb = 0; }
step s1run	{ SELECT iso_lk_run(); }

session s2
step s2begin	{ BEGIN; }
step s2ins	{ INSERT INTO iso_lk_pair SELECT 2, 400 + i FROM generate_series(0, 49) i; }
step s2del	{ DELETE FROM iso_lk_pair WHERE grp = 2 AND k < 200; }
step s2upd	{ UPDATE iso_lk_pair SET k = k + 1 WHERE grp = 2; }
step s2commit	{ COMMIT; }
step s2abort	{ ROLLBACK; }
step s2vacuum	{ VACUUM iso_lk_pair; }

# the group as vacuumed, and an insert in flight, committed or rolled back
permutation s1run s2begin s2ins s1run s2commit s1run s2vacuum s1run
permutation s1run s2begin s2ins s1run s2abort s1run s2vacuum s1run
# a delete and an update of the group in flight
permutation s1run s2begin s2del s1run s2commit s1run s2vacuum s1run
permutation s1run s2begin s2upd s1run s2abort s1run s2vacuum s1run
