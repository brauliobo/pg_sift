# A top-N scan with parallel workers while another session writes the rows at the top of the order (moved to the top, out of it, deleted,
# new): the rows of the cursor, fetched to the end, are those of the normal plan under the same snapshot, with the writer in flight,
# committed after the snapshot, rolled back, or committed before it. The workers read the snapshot the cursor's transaction holds.
# requires: build col

setup
{
	CREATE TABLE iso_tp (id int PRIMARY KEY, k int, pad text DEFAULT repeat('x', 1500)) WITH (autovacuum_enabled = off);
	ALTER TABLE iso_tp ALTER COLUMN pad SET STORAGE PLAIN;
	INSERT INTO iso_tp (id, k) SELECT i, (hashint4(i) & 2147483647) % 100000 FROM generate_series(1, 6000) i;
	UPDATE iso_tp SET k = 900000 + id WHERE id <= 20;
	CREATE INDEX iso_tp_col ON iso_tp USING sift_col (id, k);

	CREATE FUNCTION iso_tp_fetch(cur refcursor) RETURNS text LANGUAGE plpgsql AS $$
	DECLARE
		r record;
		got text[] := ARRAY[]::text[];
	BEGIN
		LOOP
			FETCH cur INTO r;
			EXIT WHEN NOT FOUND;
			got := got || r::text;
		END LOOP;
		RETURN array_to_string(got, ' ');
	END $$;

	CREATE FUNCTION iso_tp_normal() RETURNS text LANGUAGE plpgsql AS $$
	BEGIN
		PERFORM set_config('sift.enable_agg', 'off', true);
		RETURN (SELECT string_agg(z::text, ' ' ORDER BY pos) FROM (SELECT row_number() OVER () AS pos, ROW(id, k) AS z FROM (SELECT id, k FROM iso_tp ORDER BY k DESC NULLS LAST, id LIMIT 15) y) x);
	END $$;

	CREATE FUNCTION iso_tp_plan() RETURNS text LANGUAGE plpgsql AS $$
	DECLARE
		l text;
		topn bool := false;
		par bool := false;
	BEGIN
		FOR l IN EXPLAIN (COSTS OFF) SELECT id, k FROM iso_tp ORDER BY k DESC NULLS LAST, id LIMIT 15 LOOP
			topn := topn OR l ~ 'Top-N: ';
			par := par OR l ~ 'Workers Planned: ';
		END LOOP;
		RETURN CASE WHEN topn AND par THEN 'parallel top-N' WHEN topn THEN 'serial top-N' ELSE 'other plan' END;
	END $$;
}

setup
{
	VACUUM ANALYZE iso_tp;
}

teardown
{
	DROP FUNCTION iso_tp_plan();
	DROP FUNCTION iso_tp_normal();
	DROP FUNCTION iso_tp_fetch(refcursor);
	DROP TABLE iso_tp;
}

session s1
setup		{ SET max_parallel_workers_per_gather = 4; SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0;
			  SET enable_seqscan = off; SET enable_indexscan = off; SET enable_bitmapscan = off; }
step s1plan	{ SELECT iso_tp_plan() AS plan; }
step s1decl	{ BEGIN ISOLATION LEVEL REPEATABLE READ;
		  DECLARE c CURSOR FOR SELECT id, k FROM iso_tp ORDER BY k DESC NULLS LAST, id LIMIT 15; }
step s1got	{ CREATE TEMP TABLE got AS SELECT iso_tp_fetch('c') AS r; }
step s1same	{ SELECT (SELECT r FROM got) = iso_tp_normal() AS same, (SELECT r FROM got) <> '' AS rows; }
step s1end	{ COMMIT; DROP TABLE got; }

session s2
step s2begin	{ BEGIN; }
step s2write	{ UPDATE iso_tp SET k = 1000000 WHERE id IN (3000, 5000);
		  UPDATE iso_tp SET k = 0 WHERE id = 18;
		  DELETE FROM iso_tp WHERE id IN (19, 20);
		  INSERT INTO iso_tp (id, k) VALUES (100001, 2000000), (100002, NULL); }
step s2commit	{ COMMIT; }
step s2abort	{ ROLLBACK; }

# the writer commits after the cursor took its snapshot: none of its rows is in the result
permutation s1plan s1decl s2write s1got s1same s1end
# the writer is still in flight while the cursor runs
permutation s1plan s1decl s2begin s2write s1got s1same s2commit s1end
# the writer rolled back
permutation s1plan s1decl s2begin s2write s2abort s1got s1same s1end
# committed before the snapshot: all of its rows are
permutation s1plan s2write s1decl s1got s1same s1end
