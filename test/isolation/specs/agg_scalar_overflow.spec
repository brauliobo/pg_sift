# A SiftAgg plan that reads a path declared float8 as `(doc ->> 'v')::float8` or as the cast of `doc -> 'v'`, kept by a transaction (a prepared
# statement with a generic plan: the locks it holds keep it), when another session commits a value the leaf cannot store: the scan reads the
# row again from the heap and answers as the normal plan does, a numeric string converted, a value SQL cannot convert raising its error; a row the
# statement does not see raises nothing.
# requires: build col

setup
{
	CREATE TABLE iso_sc (id int PRIMARY KEY, grp int, doc jsonb) WITH (autovacuum_enabled = off);
	INSERT INTO iso_sc SELECT i, i % 3, jsonb_build_object('v', i % 10) FROM generate_series(1, 2000) i;
	CREATE INDEX iso_sc_col ON iso_sc USING sift_col (id, grp, doc) WITH (paths = 'doc.v float8');
	ANALYZE iso_sc;
}

teardown
{
	DROP TABLE iso_sc;
}

session s1
setup		{ SET max_parallel_workers_per_gather = 0; SET seq_page_cost = 1000; SET random_page_cost = 1000; SET plan_cache_mode = force_generic_plan; }
step s1prep	{ BEGIN;
		  PREPARE io AS SELECT grp, sum((doc ->> 'v')::float8) AS s FROM iso_sc GROUP BY grp ORDER BY grp;
		  PREPARE jc AS SELECT grp, sum((doc -> 'v')::float8) AS s FROM iso_sc GROUP BY grp ORDER BY grp;
		  EXPLAIN (COSTS OFF) EXECUTE io;
		  EXPLAIN (COSTS OFF) EXECUTE jc; }
step s1io	{ EXECUTE io; }
step s1jc	{ EXECUTE jc; }
step s1end	{ ROLLBACK; DEALLOCATE ALL; }

session s2
step s2num	{ INSERT INTO iso_sc VALUES (100001, 0, jsonb_build_object('v', '7.5')); }
step s2text	{ INSERT INTO iso_sc VALUES (100002, 0, jsonb_build_object('v', 'x')); }
step s2begin	{ BEGIN; }
step s2abort	{ ROLLBACK; }

# committed after the plan was made, before the statement: the statement sees the row
permutation s1prep s2num s1io s1jc s1end
permutation s1prep s2text s1io s1end
# in flight, then rolled back: the statement does not see it
permutation s1prep s2begin s2text s1io s1jc s2abort s1io s1end
