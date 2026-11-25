# The keys of an inner join's table become a conjunct of the SiftAgg scan (sift.agg_join_filter): they are the rows the cursor's snapshot sees, as in
# the normal plan, with a writer that adds or removes the matching dim_a in flight, committed or rolled back.
# requires: build col

setup
{
	CREATE TABLE iso_jf (id int PRIMARY KEY, dim_a_id int, kind text, payload text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_jf SELECT i, i % 40, (array['a', 'b', 'c'])[1 + i % 3], repeat('p', 40) FROM generate_series(1, 4000) i;
	CREATE INDEX iso_jf_col ON iso_jf USING sift_col (id, dim_a_id, kind);
	CREATE TABLE iso_jf_dim (id int, country int, label text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_jf_dim SELECT i, i % 4, 'e' || i FROM generate_series(0, 39) i;
	CREATE INDEX iso_jf_dim_country ON iso_jf_dim (country);
	ANALYZE iso_jf;
	ANALYZE iso_jf_dim;

	-- the rows of the cursor, fetched to the end, are those of the normal plan under the same snapshot
	CREATE FUNCTION iso_jf_same(cur refcursor) RETURNS bool LANGUAGE plpgsql AS $$
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
			SELECT d.label, f.kind, count(*) FROM iso_jf f JOIN iso_jf_dim d ON d.id = f.dim_a_id WHERE d.country = 1 GROUP BY d.label, f.kind) x;
		RETURN cardinality(got) > 0 AND (SELECT array_agg(g ORDER BY g) FROM unnest(got) g) = ref;
	END $$;

	-- the cursor's query has the scan
	CREATE FUNCTION iso_jf_pushed() RETURNS bool LANGUAGE plpgsql AS $$
	DECLARE
		l text;
	BEGIN
		FOR l IN EXPLAIN (COSTS OFF) SELECT d.label, f.kind, count(*) FROM iso_jf f JOIN iso_jf_dim d ON d.id = f.dim_a_id WHERE d.country = 1 GROUP BY d.label, f.kind LOOP
			IF l ~ 'Custom Scan \(SiftAgg\)' THEN RETURN true; END IF;
		END LOOP;
		RETURN false;
	END $$;
}

teardown
{
	DROP FUNCTION iso_jf_pushed();
	DROP FUNCTION iso_jf_same(refcursor);
	DROP TABLE iso_jf, iso_jf_dim;
}

session s1
setup		{ SET max_parallel_workers_per_gather = 0; SET seq_page_cost = 1000; SET random_page_cost = 1000; }
step s1decl	{ BEGIN ISOLATION LEVEL REPEATABLE READ;
		  DECLARE c CURSOR FOR SELECT d.label, f.kind, count(*) FROM iso_jf f JOIN iso_jf_dim d ON d.id = f.dim_a_id WHERE d.country = 1 GROUP BY d.label, f.kind; }
step s1pushed	{ SELECT iso_jf_pushed() AS pushed; }
step s1same	{ SELECT iso_jf_same('c') AS same; }
step s1end	{ COMMIT; }

session s2
step s2begin	{ BEGIN; }
step s2add	{ INSERT INTO iso_jf_dim VALUES (2, 1, 'late'); UPDATE iso_jf_dim SET country = 1 WHERE id = 3; }
step s2del	{ DELETE FROM iso_jf_dim WHERE id = 5; }
step s2commit	{ COMMIT; }
step s2abort	{ ROLLBACK; }

# the writer commits after the cursor took its snapshot: its rows are not in the cursor's snapshot
permutation s1pushed s1decl s2add s2del s1same s1end
# the writer is still in flight while the cursor runs
permutation s1pushed s1decl s2begin s2add s2del s1same s2commit s1end
# the writer rolled back
permutation s1pushed s1decl s2begin s2add s2del s2abort s1same s1end
