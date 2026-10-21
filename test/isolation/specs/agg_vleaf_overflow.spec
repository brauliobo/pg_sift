# A SiftAgg plan that reads a key of an object leaf (`m.* float8`) or the length of an array leaf, kept by a transaction (a prepared
# statement with a generic plan: the locks it holds keep it), when another session commits a value the leaf cannot store: the
# scan raises its error for the row the statement sees instead of reading the value as NULL; a row it does not see raises nothing.
# requires: build

setup
{
	CREATE TABLE iso_vl (id int PRIMARY KEY, grp int, m jsonb, doc jsonb) WITH (autovacuum_enabled = off);
	INSERT INTO iso_vl SELECT i, i % 3, jsonb_build_object('a', i % 10), jsonb_build_object('l', jsonb_build_array(i, i + 1)) FROM generate_series(1, 2000) i;
	-- the spec syntax has no braces inside a step: the path comes from chr()
	DO $do$ BEGIN
		EXECUTE format('CREATE INDEX iso_vl_col ON iso_vl USING sift_col (id, grp, m, doc) WITH (paths = %L)',
		               'm.* float8, doc.l[]' || chr(123) || 's float8' || chr(125));
	END $do$;
	ANALYZE iso_vl;
}

teardown
{
	DROP TABLE iso_vl;
}

session s1
setup		{ SET max_parallel_workers_per_gather = 0; SET seq_page_cost = 1000; SET random_page_cost = 1000; SET plan_cache_mode = force_generic_plan; }
step s1prep	{ BEGIN;
		  PREPARE member AS SELECT grp, sum((m ->> 'a')::float8) AS s FROM iso_vl GROUP BY grp ORDER BY grp;
		  PREPARE len AS SELECT grp, sum(jsonb_array_length(doc -> 'l')) AS s FROM iso_vl GROUP BY grp ORDER BY grp;
		  EXPLAIN (COSTS OFF) EXECUTE member;
		  EXPLAIN (COSTS OFF) EXECUTE len; }
step s1member	{ EXECUTE member; }
step s1len	{ EXECUTE len; }
step s1end	{ ROLLBACK; DEALLOCATE ALL; }

session s2
step s2text	{ INSERT INTO iso_vl VALUES (100001, 0, jsonb_build_object('a', 'x'), jsonb_build_object('l', jsonb_build_array(1))); }
step s2scalar	{ INSERT INTO iso_vl VALUES (100002, 0, jsonb_build_object('a', 1), jsonb_build_object('l', 'x')); }
step s2begin	{ BEGIN; }
step s2abort	{ ROLLBACK; }

# committed after the plan was made, before the statement: the statement sees the row
permutation s1prep s2text s1member s1end
permutation s1prep s2scalar s1len s1end
# in flight, then rolled back: the statement does not see it
permutation s1prep s2begin s2text s1member s2abort s1member s1end
