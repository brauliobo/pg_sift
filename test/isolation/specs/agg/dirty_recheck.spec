# A SiftAgg scan that reads rows again from the heap (a value the index cannot store) when another session writes a row
# whose value SQL rejects: the error is raised only for a row the scan's snapshot sees, as the normal plan does, with the
# writer in flight, committed after the snapshot, rolled back, or committed before it.
# requires: build col

setup
{
	CREATE TABLE iso_rc (id int PRIMARY KEY, doc jsonb) WITH (autovacuum_enabled = off);
	INSERT INTO iso_rc
	SELECT i, jsonb_build_object('lines', jsonb_build_array(jsonb_build_object('k', (array['a', 'b', 'c'])[1 + i % 3], 'n', CASE WHEN i % 50 = 0 THEN to_jsonb((i % 10)::text) ELSE to_jsonb(i % 10) END)))
	FROM generate_series(1, 2000) i;
	-- the spec syntax has no braces inside a step: the path comes from chr()
	DO $do$ BEGIN
		EXECUTE format('CREATE INDEX iso_rc_col ON iso_rc USING sift_col (id, doc) WITH (paths = %L)',
		               'doc.lines[]' || chr(123) || 'k text, n float8' || chr(125));
	END $do$;
	ANALYZE iso_rc;

	-- the rows of the cursor (or its error), fetched to the end, are those of the normal plan under the same snapshot
	CREATE FUNCTION iso_rc_fetch(cur refcursor) RETURNS text LANGUAGE plpgsql AS $$
	DECLARE
		r record;
		got text[] := ARRAY[]::text[];
	BEGIN
		LOOP
			FETCH cur INTO r;
			EXIT WHEN NOT FOUND;
			got := got || r::text;
		END LOOP;
		RETURN (SELECT string_agg(g, ' ' ORDER BY g) FROM unnest(got) g);
	EXCEPTION WHEN others THEN RETURN 'ERR ' || SQLSTATE || ' ' || SQLERRM;
	END $$;

	CREATE FUNCTION iso_rc_normal() RETURNS text LANGUAGE plpgsql AS $$
	BEGIN
		PERFORM set_config('sift.enable_agg', 'off', true);
		RETURN (SELECT string_agg(x::text, ' ' ORDER BY x::text)
		        FROM (SELECT l.k, avg(l.n), count(*) FROM iso_rc t, jsonb_to_recordset(t.doc -> 'lines') AS l(k text, n float8) GROUP BY l.k) x);
	EXCEPTION WHEN others THEN RETURN 'ERR ' || SQLSTATE || ' ' || SQLERRM;
	END $$;

	CREATE FUNCTION iso_rc_pushed() RETURNS bool LANGUAGE plpgsql AS $$
	DECLARE
		l text;
	BEGIN
		FOR l IN EXPLAIN (COSTS OFF) SELECT l.k, avg(l.n), count(*) FROM iso_rc t, jsonb_to_recordset(t.doc -> 'lines') AS l(k text, n float8) GROUP BY l.k LOOP
			IF l ~ 'Custom Scan \(SiftAgg\)' THEN RETURN true; END IF;
		END LOOP;
		RETURN false;
	END $$;
}

teardown
{
	DROP FUNCTION iso_rc_pushed();
	DROP FUNCTION iso_rc_normal();
	DROP FUNCTION iso_rc_fetch(refcursor);
	DROP TABLE iso_rc;
}

session s1
setup		{ SET max_parallel_workers_per_gather = 0; SET seq_page_cost = 1000; SET random_page_cost = 1000; }
step s1decl	{ BEGIN ISOLATION LEVEL REPEATABLE READ;
		  DECLARE c CURSOR FOR SELECT l.k, avg(l.n), count(*) FROM iso_rc t, jsonb_to_recordset(t.doc -> 'lines') AS l(k text, n float8) GROUP BY l.k; }
step s1pushed	{ SELECT iso_rc_pushed() AS pushed; }
step s1got	{ CREATE TEMP TABLE got AS SELECT iso_rc_fetch('c') AS r; }
step s1same	{ SELECT (SELECT r FROM got) = iso_rc_normal() AS same, (SELECT r FROM got) !~ '^ERR' AS rows; }
step s1end	{ COMMIT; DROP TABLE got; }

session s2
step s2begin	{ BEGIN; }
step s2ins	{ INSERT INTO iso_rc VALUES (100001, jsonb_build_object('lines', jsonb_build_array(jsonb_build_object('k', 'a', 'n', 'x')))); }
step s2commit	{ COMMIT; }
step s2abort	{ ROLLBACK; }

# the writer commits after the cursor was planned: the row is not in the cursor's snapshot
permutation s1pushed s1decl s2ins s1got s1same s1end
# the writer is still in flight while the cursor runs
permutation s1pushed s1decl s2begin s2ins s1got s1same s2commit s1end
# the writer rolled back
permutation s1pushed s1decl s2begin s2ins s2abort s1got s1same s1end
# the row is committed before the snapshot: the scan raises the error of the normal plan
permutation s1pushed s2ins s1decl s1got s1same s1end
