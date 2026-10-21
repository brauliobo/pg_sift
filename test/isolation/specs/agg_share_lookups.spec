# Two UNION ALL arms of a cursor share the rows of the table they join (sift.agg_share_lookups): the arm that starts after the first one and the writer reads
# the cursor's snapshot, as in the normal plan, with the writer in flight, committed or rolled back.
# requires: build col

setup
{
	CREATE TABLE iso_sh (id int PRIMARY KEY, dim_a_id int, kind text, payload text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_sh SELECT i, i % 40, (array['a', 'b', 'c'])[1 + i % 3], repeat('p', 40) FROM generate_series(1, 4000) i;
	CREATE INDEX iso_sh_col ON iso_sh USING sift_col (id, dim_a_id, kind);
	CREATE TABLE iso_sh_dim (id int, country int, label text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_sh_dim SELECT i, i % 4, 'e' || i FROM generate_series(0, 39) i;
	CREATE INDEX iso_sh_dim_country ON iso_sh_dim (country);
	ANALYZE iso_sh;
	ANALYZE iso_sh_dim;

	-- the rows of the cursor, fetched to the end, are those of the normal plan under the same snapshot
	CREATE FUNCTION iso_sh_same(cur refcursor) RETURNS bool LANGUAGE plpgsql AS $$
	DECLARE
		r record;
		got text[] := ARRAY[current_setting('iso.first')];
		ref text[];
	BEGIN
		LOOP
			FETCH cur INTO r;
			EXIT WHEN NOT FOUND;
			got := got || r::text;
		END LOOP;
		PERFORM set_config('sift.enable_agg', 'off', true);
		SELECT array_agg(x::text ORDER BY x::text) INTO ref FROM (
			SELECT d.label, f.kind, count(*) FROM iso_sh f JOIN iso_sh_dim d ON d.id = f.dim_a_id WHERE d.country = 1 GROUP BY d.label, f.kind UNION ALL SELECT d.label, f.kind, count(*) + 1000 FROM iso_sh f JOIN iso_sh_dim d ON d.id = f.dim_a_id WHERE d.country = 1 GROUP BY d.label, f.kind) x;
		RETURN cardinality(got) > 0 AND (SELECT array_agg(g ORDER BY g) FROM unnest(got) g) = ref;
	END $$;

	-- the first row: the first arm has run (and loaded the table) before the writer
	CREATE FUNCTION iso_sh_first(cur refcursor) RETURNS void LANGUAGE plpgsql AS $$
	DECLARE
		r record;
	BEGIN
		FETCH cur INTO r;
		PERFORM set_config('iso.first', r::text, false);
	END $$;

	-- the cursor's query has the scan
	CREATE FUNCTION iso_sh_pushed() RETURNS bool LANGUAGE plpgsql AS $$
	DECLARE
		l text;
	BEGIN
		FOR l IN EXPLAIN (COSTS OFF) SELECT d.label, f.kind, count(*) FROM iso_sh f JOIN iso_sh_dim d ON d.id = f.dim_a_id WHERE d.country = 1 GROUP BY d.label, f.kind UNION ALL SELECT d.label, f.kind, count(*) + 1000 FROM iso_sh f JOIN iso_sh_dim d ON d.id = f.dim_a_id WHERE d.country = 1 GROUP BY d.label, f.kind LOOP
			IF l ~ 'Custom Scan \(SiftAgg\)' THEN RETURN true; END IF;
		END LOOP;
		RETURN false;
	END $$;
}

teardown
{
	DROP FUNCTION iso_sh_pushed();
	DROP FUNCTION iso_sh_first(refcursor);
	DROP FUNCTION iso_sh_same(refcursor);
	DROP TABLE iso_sh, iso_sh_dim;
}

session s1
setup		{ SET max_parallel_workers_per_gather = 0; SET seq_page_cost = 1000; SET random_page_cost = 1000; }
step s1decl	{ BEGIN ISOLATION LEVEL REPEATABLE READ;
		  DECLARE c CURSOR FOR SELECT d.label, f.kind, count(*) FROM iso_sh f JOIN iso_sh_dim d ON d.id = f.dim_a_id WHERE d.country = 1 GROUP BY d.label, f.kind UNION ALL SELECT d.label, f.kind, count(*) + 1000 FROM iso_sh f JOIN iso_sh_dim d ON d.id = f.dim_a_id WHERE d.country = 1 GROUP BY d.label, f.kind; }
step s1pushed	{ SELECT iso_sh_pushed() AS pushed; }
step s1first	{ SELECT iso_sh_first('c'); }
step s1same	{ SELECT iso_sh_same('c') AS same; }
step s1end	{ COMMIT; }

session s2
step s2begin	{ BEGIN; }
step s2add	{ INSERT INTO iso_sh_dim VALUES (2, 1, 'late'); UPDATE iso_sh_dim SET country = 1 WHERE id = 3; }
step s2del	{ DELETE FROM iso_sh_dim WHERE id = 5; }
step s2commit	{ COMMIT; }
step s2abort	{ ROLLBACK; }

# the writer commits after the cursor took its snapshot: its rows are not in the cursor's snapshot
permutation s1pushed s1decl s1first s2add s2del s1same s1end
# the writer is still in flight while the cursor runs
permutation s1pushed s1decl s1first s2begin s2add s2del s1same s2commit s1end
# the writer rolled back
permutation s1pushed s1decl s1first s2begin s2add s2del s2abort s1same s1end
