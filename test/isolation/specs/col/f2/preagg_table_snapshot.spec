# A SiftAggV6 aggregate of a Format 2.0 sift_table answered from the cubes of its parts shows the rows of its snapshot, as the normal plan does, with a writer in flight, committed after the snapshot,
# rolled back, and with VACUUM run while an old snapshot still sees the rows it removes: a granule whose rows are not settled for the snapshot is scanned.
# requires: tam

setup
{
	DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;
	CREATE TABLE iso_pt (id int PRIMARY KEY, g int, v int8, pad text) USING sift_table WITH (autovacuum_enabled = false);
	INSERT INTO iso_pt SELECT i, i % 5, i, md5(i::text) || md5((i + 1)::text) FROM generate_series(1, 40000) i;

	-- the node of the plan answers as the normal plan does under the snapshot of the session; the second column says whether granules came from the cubes
	CREATE FUNCTION iso_pt_check(OUT n bigint, OUT same bool, OUT source text) LANGUAGE plpgsql AS $$
	DECLARE
		q text := 'SELECT g, count(*), sum(v), min(v), max(v) FROM iso_pt GROUP BY g';
		a text;
		b text;
		l text;
		hits int := 0;
		node bool := false;
	BEGIN
		PERFORM set_config('max_parallel_workers_per_gather', '0', true);
		PERFORM set_config('sift.enable_agg', 'off', true);
		SELECT count(*) INTO n FROM iso_pt;
		EXECUTE format('SELECT string_agg(x::text, '','' ORDER BY x::text) FROM (%s) x', q) INTO b;
		PERFORM set_config('sift.enable_agg', 'on', true);
		EXECUTE format('SELECT string_agg(x::text, '','' ORDER BY x::text) FROM (%s) x', q) INTO a;
		FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
			node := node OR l ~ 'Custom Scan \(SiftAggV6\)';
			hits := hits + coalesce(substring(l from 'preagg=(\d+)')::int, 0);
		END LOOP;
		same := a = b;
		source := CASE WHEN NOT node THEN 'no node' WHEN hits > 0 THEN 'cubes' ELSE 'scan' END;
	END $$;
}

teardown
{
	DROP FUNCTION iso_pt_check();
	DROP TABLE iso_pt;
}

session s1
step s1begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM iso_pt; }
step s1check	{ SELECT * FROM iso_pt_check(); }
step s1end	{ COMMIT; }

session s2
setup		{ VACUUM (FREEZE, ANALYZE) iso_pt; }
step s2begin	{ BEGIN; }
step s2delete	{ DELETE FROM iso_pt WHERE id % 5000 = 7; }
step s2update	{ UPDATE iso_pt SET v = v + 1000000 WHERE id % 7000 = 3; }
step s2commit	{ COMMIT; }
step s2abort	{ ROLLBACK; }
step s2vacuum	{ VACUUM (FREEZE) iso_pt; }

# the clean table: the cubes answer
permutation s1check
# the writer is in flight, then committed: a snapshot taken before sees the rows, one taken after sees the change
permutation s1begin s2begin s2delete s2update s1check s2commit s1check s1end s1check
# the writer rolled back
permutation s1begin s2begin s2delete s2update s1check s2abort s1check s1end s1check
# the writer committed and VACUUM ran while the snapshot of s1 still sees the removed rows
permutation s1begin s2delete s2update s2vacuum s1check s1end s2vacuum s1check
