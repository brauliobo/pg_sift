# A SiftAgg scan whose lateral child (a LIMIT 1 mapping) and UNION ALL plan child read other tables, while another session writes them: the
# child's rows are those of the cursor's snapshot, as in the normal plan, with the writer in flight, committed or rolled back.
# requires: build col

setup
{
	CREATE TABLE iso_corr (id int PRIMARY KEY, kind text, entity_id int, payload text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_corr SELECT i, (array['a', 'b', 'c'])[1 + i % 3], i % 20, repeat('p', 40) FROM generate_series(1, 3000) i;
	CREATE INDEX iso_corr_col ON iso_corr USING sift_col (id, kind, entity_id);
	CREATE TABLE iso_corr_map (kind text, name text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_corr_map VALUES ('a', 'alpha'), ('b', 'beta');
	CREATE TABLE iso_corr_names (id int, name text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_corr_names SELECT i, 'n' || i % 5 FROM generate_series(0, 9) i;
	ANALYZE iso_corr;

	-- the rows of the cursor, fetched to the end, are those of the normal plan under the same snapshot
	CREATE FUNCTION iso_corr_same(cur refcursor) RETURNS bool LANGUAGE plpgsql AS $$
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
		SELECT array_agg(x::text ORDER BY x::text) INTO ref FROM (
			SELECT m.name, n.name AS other, count(*) FROM iso_corr f
			LEFT JOIN LATERAL (SELECT name FROM iso_corr_map m WHERE m.kind = f.kind ORDER BY name LIMIT 1) m ON true
			JOIN (SELECT id, name FROM iso_corr_names UNION ALL SELECT id, name FROM iso_corr_names) n ON n.id = f.entity_id GROUP BY m.name, n.name) x;
		RETURN cardinality(got) > 0 AND (SELECT array_agg(g ORDER BY g) FROM unnest(got) g) = ref;
	END $$;

	-- the cursor's query has the scan
	CREATE FUNCTION iso_corr_pushed() RETURNS bool LANGUAGE plpgsql AS $$
	DECLARE
		l text;
	BEGIN
		FOR l IN EXPLAIN (COSTS OFF) SELECT m.name, n.name AS other, count(*) FROM iso_corr f
			LEFT JOIN LATERAL (SELECT name FROM iso_corr_map m WHERE m.kind = f.kind ORDER BY name LIMIT 1) m ON true
			JOIN (SELECT id, name FROM iso_corr_names UNION ALL SELECT id, name FROM iso_corr_names) n ON n.id = f.entity_id GROUP BY m.name, n.name LOOP
			IF l ~ 'Custom Scan \(SiftAgg\)' THEN RETURN true; END IF;
		END LOOP;
		RETURN false;
	END $$;
}

teardown
{
	DROP FUNCTION iso_corr_pushed();
	DROP FUNCTION iso_corr_same(refcursor);
	DROP TABLE iso_corr, iso_corr_map, iso_corr_names;
}

session s1
setup		{ SET max_parallel_workers_per_gather = 0; SET seq_page_cost = 1000; SET random_page_cost = 1000; }
step s1decl	{ BEGIN ISOLATION LEVEL REPEATABLE READ;
		  DECLARE c CURSOR FOR SELECT m.name, n.name AS other, count(*) FROM iso_corr f
		  LEFT JOIN LATERAL (SELECT name FROM iso_corr_map m WHERE m.kind = f.kind ORDER BY name LIMIT 1) m ON true
		  JOIN (SELECT id, name FROM iso_corr_names UNION ALL SELECT id, name FROM iso_corr_names) n ON n.id = f.entity_id GROUP BY m.name, n.name; }
step s1pushed	{ SELECT iso_corr_pushed() AS pushed; }
step s1same	{ SELECT iso_corr_same('c') AS same; }
step s1end	{ COMMIT; }

session s2
step s2begin	{ BEGIN; }
step s2map	{ INSERT INTO iso_corr_map VALUES ('c', 'gamma'), ('a', 'aardvark'); }
step s2names	{ INSERT INTO iso_corr_names VALUES (3, 'late'); }
step s2commit	{ COMMIT; }
step s2abort	{ ROLLBACK; }

# the writer commits after the cursor was planned: its rows are not in the cursor's snapshot
permutation s1pushed s1decl s2map s2names s1same s1end
# the writer is still in flight while the cursor runs
permutation s1pushed s1decl s2begin s2map s2names s1same s2commit s1end
# the writer rolled back
permutation s1pushed s1decl s2begin s2map s2names s2abort s1same s1end
