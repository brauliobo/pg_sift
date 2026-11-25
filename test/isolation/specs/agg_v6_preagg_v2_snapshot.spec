# A SiftAggV6 aggregate answered from the cubes of SEC_PREAGG version 2 shows the rows of its snapshot, as the normal plan does, with a writer in flight, committed after the snapshot,
# rolled back, and with VACUUM run while an old snapshot still sees the rows it removes: a granule whose heap pages are not all visible is scanned.
# requires: build col

setup
{
	DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;
	CREATE TABLE iso_pa2 (id int PRIMARY KEY, g text, v int, w float8, pad text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_pa2 SELECT i, 'g' || (i % 5), i, (i % 100) * 0.25,
		encode(sha256((i::text || 'a')::bytea), 'base64') || encode(sha256((i::text || 'b')::bytea), 'base64') || encode(sha256((i::text || 'c')::bytea), 'base64')
	FROM generate_series(1, 40000) i;
	CREATE INDEX iso_pa2_col ON iso_pa2 USING sift_col (id, g, v, w, pad) WITH (key_order = 'id');

	-- the answer of the node is the one of the normal plan under the snapshot of the session, and the node is in the plan
	CREATE FUNCTION iso_pa2_same() RETURNS bool LANGUAGE plpgsql AS $$
	DECLARE
		a text;
		b text;
		l text;
		node bool := false;
	BEGIN
		PERFORM set_config('sift.enable_agg', 'on', true);
		SELECT string_agg(x::text, ',' ORDER BY x::text) INTO a FROM (SELECT g, count(*), sum(v), min(v), max(v), sum(w) FROM iso_pa2 GROUP BY g) x;
		FOR l IN EXPLAIN (COSTS OFF) SELECT g, count(*), sum(v), min(v), max(v), sum(w) FROM iso_pa2 GROUP BY g LOOP
			node := node OR l ~ 'Custom Scan \(SiftAggV6\)';
		END LOOP;
		PERFORM set_config('sift.enable_agg', 'off', true);
		SELECT string_agg(x::text, ',' ORDER BY x::text) INTO b FROM (SELECT g, count(*), sum(v), min(v), max(v), sum(w) FROM iso_pa2 GROUP BY g) x;
		PERFORM set_config('sift.enable_agg', 'on', true);
		RETURN node AND a = b;
	END $$;
}

teardown
{
	DROP FUNCTION iso_pa2_same();
	DROP TABLE iso_pa2;
}

session s1
setup		{ SET max_parallel_workers_per_gather = 0; }
step s1begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM iso_pa2; }
step s1same	{ SELECT iso_pa2_same() AS same; }
step s1rows	{ SELECT count(*) AS rows FROM iso_pa2; }
step s1end	{ COMMIT; }

session s2
setup		{ VACUUM (FREEZE, ANALYZE) iso_pa2; }
step s2begin	{ BEGIN; }
step s2delete	{ DELETE FROM iso_pa2 WHERE id % 3 = 0; }
step s2update	{ UPDATE iso_pa2 SET v = v + 1000000, w = w + 0.5 WHERE id % 7 = 0; }
step s2commit	{ COMMIT; }
step s2abort	{ ROLLBACK; }
step s2vacuum	{ VACUUM iso_pa2; }

# the clean table: the cubes answer
permutation s1same
# the writer is in flight, then committed: a snapshot taken before sees the rows, one taken after sees the change
permutation s1begin s2begin s2delete s2update s1same s2commit s1same s1rows s1end s1same
# the writer rolled back
permutation s1begin s2begin s2delete s2update s1same s2abort s1same s1end s1same
# the writer committed and VACUUM ran while the snapshot of s1 still sees the removed rows
permutation s1begin s2delete s2update s2vacuum s1same s1rows s1end s2vacuum s1same
