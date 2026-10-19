# A SiftAgg scan already running (its cursor planned before the write) when another session writes a value the
# index cannot store: the rows are the normal plan's, with the writer in flight, committed or rolled back, in the
# write buffer and once promoted into a segment.
# requires: build

setup
{
	CREATE TABLE iso_ovf (id int PRIMARY KEY, doc jsonb) WITH (autovacuum_enabled = off);
	INSERT INTO iso_ovf
	SELECT i, jsonb_build_object('lines', (SELECT jsonb_agg(jsonb_build_object('k', (array['a', 'b', 'c'])[1 + (i + j) % 3], 'n', to_jsonb(j % 10)))
	                                       FROM generate_series(1, 1 + i % 4) j))
	FROM generate_series(1, 2000) i;
	-- the spec syntax has no braces inside a step: the path comes from chr()
	DO $do$ BEGIN
		EXECUTE format('CREATE INDEX iso_ovf_col ON iso_ovf USING sift_col (id, doc) WITH (paths = %L)',
		               'doc.lines[]' || chr(123) || 'k text, n float8' || chr(125));
	END $do$;
	ANALYZE iso_ovf;
	-- rows of the cursor's snapshot in the write buffer, where the writer's row goes too, and in the segment it is promoted to
	INSERT INTO iso_ovf SELECT 50000 + i, jsonb_build_object('lines', jsonb_build_array(jsonb_build_object('k', 'c', 'n', i))) FROM generate_series(1, 10) i;

	-- the rows of the cursor, fetched to the end, are those of the normal plan under the same snapshot
	CREATE FUNCTION iso_ovf_same(cur refcursor) RETURNS bool LANGUAGE plpgsql AS $$
	DECLARE
		r record;
		got text[] := ARRAY[]::text[];
		ref text[];
	BEGIN
		LOOP
			FETCH cur INTO r;
			EXIT WHEN NOT FOUND;
			got := got || r::text;
		END LOOP;
		PERFORM set_config('sift.enable_agg', 'off', true);
		SELECT array_agg(x::text ORDER BY x::text) INTO ref
		FROM (SELECT l.k, avg(l.n), count(*) FROM iso_ovf t, jsonb_to_recordset(t.doc -> 'lines') AS l(k text, n float8) GROUP BY l.k) x;
		RETURN cardinality(got) > 0 AND (SELECT array_agg(g ORDER BY g) FROM unnest(got) g) = ref;
	END $$;

	-- the cursor's query has the scan
	CREATE FUNCTION iso_ovf_pushed() RETURNS bool LANGUAGE plpgsql AS $$
	DECLARE
		l text;
	BEGIN
		FOR l IN EXPLAIN (COSTS OFF) SELECT l.k, avg(l.n), count(*) FROM iso_ovf t, jsonb_to_recordset(t.doc -> 'lines') AS l(k text, n float8) GROUP BY l.k LOOP
			IF l ~ 'Custom Scan \(SiftAgg\)' THEN RETURN true; END IF;
		END LOOP;
		RETURN false;
	END $$;
}

teardown
{
	DROP FUNCTION iso_ovf_pushed();
	DROP FUNCTION iso_ovf_same(refcursor);
	DROP TABLE iso_ovf;
}

session s1
setup		{ SET max_parallel_workers_per_gather = 0; SET seq_page_cost = 1000; SET random_page_cost = 1000; }
step s1decl	{ BEGIN ISOLATION LEVEL REPEATABLE READ;
		  DECLARE c CURSOR FOR SELECT l.k, avg(l.n), count(*) FROM iso_ovf t, jsonb_to_recordset(t.doc -> 'lines') AS l(k text, n float8) GROUP BY l.k; }
step s1pushed	{ SELECT iso_ovf_pushed() AS pushed; }
step s1same	{ SELECT iso_ovf_same('c') AS same; }
step s1end	{ COMMIT; }

session s2
step s2begin	{ BEGIN; }
step s2ins	{ INSERT INTO iso_ovf VALUES (100001, jsonb_build_object('lines', jsonb_build_array(
		  jsonb_build_object('k', 'a', 'n', '7'), jsonb_build_object('k', 'b', 'n', 1)))); }
step s2commit	{ COMMIT; }
step s2abort	{ ROLLBACK; }
step s2promote	{ SELECT sift.col_promote('iso_ovf_col') >= 0 AS promoted; }

# the writer commits after the cursor was planned: the row is not in the cursor's snapshot
permutation s1pushed s1decl s2ins s1same s1end
# the writer is still in flight while the cursor runs
permutation s1pushed s1decl s2begin s2ins s1same s2commit s1end
# the writer rolled back
permutation s1pushed s1decl s2begin s2ins s2abort s1same s1end
# the value is promoted into a segment before the cursor runs
permutation s1pushed s1decl s2ins s2promote s1same s1end
