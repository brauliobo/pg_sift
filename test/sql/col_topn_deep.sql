-- ORDER BY <columns> LIMIT n OFFSET m beyond what the top-N heap holds (100000 rows, 4 sort keys): the scan keeps every row that passes in a
-- sort of keys and TIDs (work_mem, then disk), and the plan is priced with that sort. Every query runs twice, the plain plan as the reference
-- (sift.enable_agg off) and the pushed one, and the rows are compared in order.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;

CREATE TABLE cd (id int PRIMARY KEY, a int, b int, c int, d int, e int, f int, amount numeric(10,2), ts timestamp, kind text, note text);
INSERT INTO cd
SELECT i, (hashint4(i) & 2147483647) % 50, (hashint4(i + 1) & 2147483647) % 7, (hashint4(i + 2) & 2147483647) % 5,
	   (hashint4(i + 3) & 2147483647) % 3, (hashint4(i + 4) & 2147483647) % 2, CASE WHEN i % 13 = 0 THEN NULL ELSE (hashint4(i + 5) & 2147483647) % 4 END,
	   CASE WHEN i % 29 = 0 THEN NULL ELSE ((hashint4(i + 6) & 2147483647) % 3000)::numeric / 10 END,
	   timestamp '2024-01-01' + (hashint4(i + 7) & 2147483647) % 6000000 * interval '1 second',
	   (ARRAY['cat_a', 'cat_b', 'cat_c', 'cat_d'])[1 + i % 4], 'n' || i
FROM generate_series(1, 200000) i;
CREATE INDEX cd_col ON cd USING sift_col (id, a, b, c, d, e, f, amount, ts, kind);
VACUUM (ANALYZE) cd;

CREATE FUNCTION cd_mode(pushed bool) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.enable_agg', pushed::text, true);
	PERFORM set_config('enable_seqscan', (NOT pushed)::text, true);
	PERFORM set_config('enable_indexscan', (NOT pushed)::text, true);
	PERFORM set_config('enable_bitmapscan', (NOT pushed)::text, true);
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
END $$;

-- 'same' / 'DIFFERENT' (the rows and their order), '+topn' when the pushed plan is the top-N scan, and the number of rows
CREATE FUNCTION cd_check(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE n bigint; plan text := ''; line text;
BEGIN
	DROP TABLE IF EXISTS cd_a, cd_b;
	PERFORM cd_mode(true);
	FOR line IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP plan := plan || line || E'\n'; END LOOP;
	EXECUTE 'CREATE TEMP TABLE cd_a AS SELECT row_number() OVER () AS pos, row_to_json(x)::text AS r FROM (' || q || ') x';
	PERFORM cd_mode(false);
	EXECUTE 'CREATE TEMP TABLE cd_b AS SELECT row_number() OVER () AS pos, row_to_json(x)::text AS r FROM (' || q || ') x';
	SELECT count(*) INTO n FROM ((SELECT pos, r FROM cd_a EXCEPT ALL SELECT pos, r FROM cd_b) UNION ALL (SELECT pos, r FROM cd_b EXCEPT ALL SELECT pos, r FROM cd_a)) d;
	RETURN CASE WHEN n = 0 THEN 'same' ELSE 'DIFFERENT' END || CASE WHEN plan ~ 'Top-N: ' THEN '+topn' ELSE '' END || ' ' || (SELECT count(*) FROM cd_b);
END $$;

-- the offset past the heap: asc and desc, NULLs first and last, a key with ties (ties by id), a search-free filter, a numeric and a timestamp key
CREATE TABLE cd_q (tag text PRIMARY KEY, q text NOT NULL);
INSERT INTO cd_q VALUES
	('off150k_asc',   $q$SELECT id, ts FROM cd ORDER BY ts, id LIMIT 10 OFFSET 150000$q$),
	('off150k_desc',  $q$SELECT id, ts FROM cd ORDER BY ts DESC, id DESC LIMIT 10 OFFSET 150000$q$),
	('off0',          $q$SELECT id, ts FROM cd ORDER BY ts DESC, id LIMIT 10$q$),
	('off50k',        $q$SELECT id, ts FROM cd ORDER BY ts, id LIMIT 10 OFFSET 50000$q$),
	('edge_cap',      $q$SELECT id, ts FROM cd ORDER BY ts, id LIMIT 1 OFFSET 99999$q$),
	('edge_cap_1',    $q$SELECT id, ts FROM cd ORDER BY ts, id LIMIT 1 OFFSET 100000$q$),
	('limit_150k',    $q$SELECT id, ts FROM cd ORDER BY ts, id LIMIT 150000$q$),
	('ties_asc',      $q$SELECT id, a FROM cd ORDER BY a, id LIMIT 20 OFFSET 120000$q$),
	('ties_desc',     $q$SELECT id, a FROM cd ORDER BY a DESC, id LIMIT 20 OFFSET 120000$q$),
	('nulls_first',   $q$SELECT id, f FROM cd ORDER BY f NULLS FIRST, id LIMIT 20 OFFSET 105000$q$),
	('nulls_last',    $q$SELECT id, f FROM cd ORDER BY f DESC NULLS LAST, id LIMIT 20 OFFSET 105000$q$),
	('numeric',       $q$SELECT id, amount FROM cd ORDER BY amount DESC NULLS LAST, id LIMIT 15 OFFSET 130000$q$),
	('filter',        $q$SELECT id, ts FROM cd WHERE kind IN ('cat_a', 'cat_b') AND b < 5 ORDER BY ts DESC, id LIMIT 10 OFFSET 60000$q$),
	('filter_past',   $q$SELECT id, ts FROM cd WHERE a < 40 ORDER BY ts, id LIMIT 10 OFFSET 150000$q$),
	('beyond_rows',   $q$SELECT id, ts FROM cd ORDER BY ts, id LIMIT 10 OFFSET 250000$q$),
	('expr_out',      $q$SELECT id, ts, amount * 2 AS a2, upper(kind) AS k FROM cd ORDER BY ts, id LIMIT 10 OFFSET 140000$q$),
	('five_keys',     $q$SELECT id FROM cd ORDER BY a, b, c, d, e, id LIMIT 30 OFFSET 1000$q$),
	('six_keys',      $q$SELECT id, a, b, c, d, e, f FROM cd ORDER BY a DESC, b, c DESC, d, e DESC, f NULLS FIRST, id LIMIT 30$q$),
	('seven_keys',    $q$SELECT id FROM cd WHERE a < 20 ORDER BY b DESC, c, d DESC, e, f DESC NULLS LAST, a, ts, id DESC LIMIT 25 OFFSET 7$q$),
	('five_keys_mix', $q$SELECT id, amount FROM cd ORDER BY kind, amount DESC NULLS LAST, ts, a, id LIMIT 10 OFFSET 20$q$);

SELECT tag, cd_check(q) FROM cd_q ORDER BY tag COLLATE "C";

-- rows past hash_mem (work_mem times hash_mem_multiplier) move to a tuplesort: the same rows, from the sort that writes runs to disk
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT cd_check($q$SELECT id, ts FROM cd ORDER BY ts DESC, id LIMIT 10 OFFSET 150000$q$) AS spilled_desc,
	   cd_check($q$SELECT id, f FROM cd ORDER BY f NULLS FIRST, id LIMIT 20 OFFSET 105000$q$) AS spilled_nulls,
	   cd_check($q$SELECT id FROM cd ORDER BY a, b, c, d, e, id LIMIT 30 OFFSET 1000$q$) AS spilled_five_keys;
RESET work_mem;
RESET hash_mem_multiplier;

-- the plan prices the sort and the rows it reads: a deep offset on a few rows chosen is the top-N scan, the heap plan stays for a small n
CREATE FUNCTION cd_plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE plan text := ''; line text;
BEGIN
	PERFORM cd_mode(true);
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || line || E'\n'; END LOOP;
	RETURN CASE WHEN plan ~ 'Top-N: ' THEN 'top-n' ELSE 'other' END;
END $$;
SELECT cd_plan('SELECT id, ts FROM cd ORDER BY ts, id LIMIT 10 OFFSET 150000') AS deep_offset,
	   cd_plan('SELECT id, ts FROM cd ORDER BY ts, id LIMIT 10') AS small_limit,
	   cd_plan('SELECT id FROM cd ORDER BY a, b, c, d, e, id LIMIT 30') AS five_keys;

-- workers allowed: a top-N that sorts every row that passes is one process (the heaps of the workers are what the leader merges)
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
CREATE FUNCTION cd_workers(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE plan text := ''; line text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || line || E'\n'; END LOOP;
	RETURN CASE WHEN plan ~ 'Top-N: .*sorted' THEN 'sorted' WHEN plan ~ 'Top-N: ' THEN 'heap' ELSE 'other' END || CASE WHEN plan ~ 'Workers Planned' THEN ' with workers' ELSE ' alone' END;
END $$;
SELECT cd_workers('SELECT id, ts FROM cd ORDER BY ts, id LIMIT 10 OFFSET 150000') AS deep_offset_workers;
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;

-- the rows that are skipped by the OFFSET are not read from the heap
CREATE FUNCTION cd_stat(q text, field text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE line text; v bigint;
BEGIN
	PERFORM cd_mode(true);
	FOR line IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF line ~ ('^ *' || field || ': ') THEN v := regexp_replace(line, '^ *[A-Za-z -]+: ', '')::bigint; END IF;
	END LOOP;
	RETURN v;
END $$;
SELECT cd_stat($q$SELECT id, ts FROM cd ORDER BY ts, id LIMIT 10 OFFSET 150000$q$, 'Heap Fetches') AS fetches_plain_columns,
	   cd_stat($q$SELECT id, ts FROM cd ORDER BY ts, id LIMIT 10 OFFSET 150000$q$, 'Top-N Sorted Rows') AS sorted_rows,
	   cd_stat($q$SELECT id, ts, amount * 2 AS a2 FROM cd ORDER BY ts, id LIMIT 10 OFFSET 150000$q$, 'Heap Fetches') AS fetches_expression;

-- a generic plan: LIMIT and OFFSET as parameters, the scan reads their values when it starts
SET plan_cache_mode = force_generic_plan;
PREPARE cd_g (bigint, bigint) AS SELECT id, ts FROM cd ORDER BY ts, id LIMIT $1 OFFSET $2;
CREATE TEMP TABLE cd_g_on AS EXECUTE cd_g(10, 150000);
SET sift.enable_agg = off;
DEALLOCATE cd_g;
PREPARE cd_g (bigint, bigint) AS SELECT id, ts FROM cd ORDER BY ts, id LIMIT $1 OFFSET $2;
CREATE TEMP TABLE cd_g_off AS EXECUTE cd_g(10, 150000);
RESET sift.enable_agg;
RESET plan_cache_mode;
SELECT (SELECT count(*) FROM cd_g_on) AS generic_rows, NOT EXISTS (TABLE cd_g_on EXCEPT TABLE cd_g_off) AND NOT EXISTS (TABLE cd_g_off EXCEPT TABLE cd_g_on) AS generic_same;

-- a scroll cursor over the sort reads the rows forward and back (the rows the OFFSET skips are rows of NULLs each time)
DO $$
DECLARE
	c SCROLL CURSOR FOR SELECT id, ts FROM cd ORDER BY ts DESC, id LIMIT 5 OFFSET 150000;
	want int[]; got int[] := '{}'; r record;
BEGIN
	PERFORM set_config('sift.enable_agg', 'off', true);
	want := ARRAY(SELECT id FROM cd ORDER BY ts DESC, id LIMIT 5 OFFSET 150000);
	PERFORM cd_mode(true);
	OPEN c;
	LOOP FETCH c INTO r; EXIT WHEN NOT FOUND; got := got || r.id; END LOOP;
	LOOP FETCH PRIOR FROM c INTO r; EXIT WHEN NOT FOUND; got := got || r.id; END LOOP;
	CLOSE c;
	RAISE NOTICE 'cursor % %', cardinality(want), got = want || ARRAY(SELECT x FROM unnest(want) WITH ORDINALITY u(x, o) ORDER BY o DESC);
END $$;
