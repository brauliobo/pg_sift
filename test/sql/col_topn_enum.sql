-- ORDER BY an enum column LIMIT n: the column units hold the OIDs of the values, whose order is not the order of the type (ALTER TYPE ... ADD VALUE BEFORE/AFTER changes the
-- order without touching a row), so the top-N ranks the OIDs by pg_enum.enumsortorder when it opens and sorts by the rank. Every query runs twice, the plain plan as the
-- reference (sift.enable_agg off) and the pushed one, and the rows are compared in order, after the build, with more segments, with rows in the write buffer, after
-- updates and deletes, a merge and VACUUM, after values are added to the type before and after others, with workers and a spilling sort, on a partitioned table and a sift_table.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET sift.async_maintenance = off;

-- the order of the type is neither the order of the OIDs nor the alphabetical one: warn < debug < info < error < fatal
CREATE TYPE sev AS ENUM ('warn', 'info', 'error');
ALTER TYPE sev ADD VALUE 'debug' BEFORE 'info';
ALTER TYPE sev ADD VALUE 'fatal';
-- a type of many values
CREATE TYPE big AS ENUM ('v000');
DO $$ BEGIN FOR i IN 1..300 LOOP EXECUTE format('ALTER TYPE big ADD VALUE %L', 'v' || lpad((i * 7919 % 301)::text, 3, '0')); END LOOP; END $$;

-- pad makes a unit hold about 1300 rows, so that a segment has several units and the bar of one is a value of another
CREATE TABLE ev (id int PRIMARY KEY, g int, s sev, w sev, b big, pad text DEFAULT repeat('x', 1500));
CREATE FUNCTION ev_rows(lo int, hi int) RETURNS void LANGUAGE sql AS $$
	INSERT INTO ev (id, g, s, w, b)
	SELECT i, i % 7,
		CASE WHEN i % 17 = 0 THEN NULL ELSE (enum_range(NULL::sev))[1 + (hashint4(i) & 2147483647) % 5] END,
		(enum_range(NULL::sev))[1 + i % 3],
		CASE WHEN i % 13 = 0 THEN NULL ELSE (enum_range(NULL::big))[1 + (hashint4(i * 3) & 2147483647) % 301] END
	FROM generate_series(lo, hi) i $$;
SELECT ev_rows(1, 6000);
CREATE INDEX ev_col ON ev USING sift_col (id, g, s, w, b);
VACUUM (ANALYZE) ev;

CREATE FUNCTION ev_mode(pushed bool, workers int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.enable_agg', pushed::text, true);
	PERFORM set_config('enable_seqscan', (NOT pushed)::text, true);
	PERFORM set_config('enable_indexscan', (NOT pushed)::text, true);
	PERFORM set_config('enable_bitmapscan', (NOT pushed)::text, true);
	PERFORM set_config('max_parallel_workers_per_gather', CASE WHEN pushed THEN workers ELSE 0 END::text, true);
	PERFORM set_config('parallel_setup_cost', '0', true);
	PERFORM set_config('parallel_tuple_cost', '0', true);
	PERFORM set_config('min_parallel_table_scan_size', '0', true);
END $$;

-- 'same' / 'DIFFERENT' (the rows and their order), '+topn' when the pushed plan is the top-N scan, and the rows
CREATE FUNCTION ev_check(q text, workers int DEFAULT 0) RETURNS text LANGUAGE plpgsql AS $$
DECLARE n bigint; plan text := ''; line text;
BEGIN
	DROP TABLE IF EXISTS ev_a, ev_b;
	PERFORM ev_mode(true, workers);
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || line || E'\n'; END LOOP;
	EXECUTE 'CREATE TEMP TABLE ev_a AS SELECT row_number() OVER () AS pos, row_to_json(x)::text AS r FROM (' || q || ') x';
	PERFORM ev_mode(false, 0);
	EXECUTE 'CREATE TEMP TABLE ev_b AS SELECT row_number() OVER () AS pos, row_to_json(x)::text AS r FROM (' || q || ') x';
	SELECT count(*) INTO n FROM ((SELECT pos, r FROM ev_a EXCEPT ALL SELECT pos, r FROM ev_b) UNION ALL (SELECT pos, r FROM ev_b EXCEPT ALL SELECT pos, r FROM ev_a)) d;
	RETURN CASE WHEN n = 0 THEN 'same' ELSE 'DIFFERENT' END || CASE WHEN plan ~ 'Top-N: ' THEN '+topn' ELSE '' END || ' ' || (SELECT count(*) FROM ev_b);
END $$;

CREATE TABLE ev_q (tag text PRIMARY KEY, q text NOT NULL);
INSERT INTO ev_q VALUES
	('s_asc',          $q$SELECT id, s FROM ev ORDER BY s, id LIMIT 25$q$),
	('s_desc',         $q$SELECT id, s FROM ev ORDER BY s DESC, id LIMIT 25$q$),
	('s_nulls_first',  $q$SELECT id, s FROM ev ORDER BY s NULLS FIRST, id LIMIT 25$q$),
	('s_nulls_last',   $q$SELECT id, s FROM ev ORDER BY s DESC NULLS LAST, id DESC LIMIT 25$q$),
	('s_offset',       $q$SELECT id, s FROM ev ORDER BY s, id LIMIT 20 OFFSET 900$q$),
	('s_deep_offset',  $q$SELECT id, s FROM ev ORDER BY s DESC, id LIMIT 30 OFFSET 5000$q$),
	('s_filter',       $q$SELECT id, s FROM ev WHERE g IN (1, 3) AND id > 2000 ORDER BY s DESC, id LIMIT 30$q$),
	('s_same_leaf',    $q$SELECT id, s FROM ev WHERE s > 'info' ORDER BY s, id LIMIT 25$q$),
	('s_ties',         $q$SELECT id, g, s FROM ev ORDER BY g, s, id LIMIT 40 OFFSET 11$q$),
	('s_then_w',       $q$SELECT id, s, w FROM ev ORDER BY s, w DESC, id LIMIT 30$q$),
	('w_then_s',       $q$SELECT id, s, w FROM ev ORDER BY w DESC, s NULLS FIRST, id LIMIT 30$q$),
	('w_few',          $q$SELECT id, w FROM ev ORDER BY w, id DESC LIMIT 30$q$),
	('b_asc',          $q$SELECT id, b FROM ev ORDER BY b, id LIMIT 40$q$),
	('b_desc',         $q$SELECT id, b FROM ev ORDER BY b DESC NULLS LAST, id LIMIT 40$q$),
	('b_offset',       $q$SELECT id, b FROM ev ORDER BY b, id LIMIT 20 OFFSET 3000$q$),
	('second_key',     $q$SELECT id, g, s FROM ev ORDER BY g DESC, s, id LIMIT 30$q$),
	('all_rows',       $q$SELECT id, s FROM ev ORDER BY s, id LIMIT 200000$q$);

CREATE FUNCTION ev_stage(stage text, workers int DEFAULT 0) RETURNS TABLE (stage text, tag text, result text) LANGUAGE sql AS $$
	SELECT stage, tag, ev_check(q, workers) FROM ev_q ORDER BY tag COLLATE "C" $$;

SELECT * FROM ev_stage('build');
SELECT tag, ev_check(q, 2) FROM ev_q WHERE tag IN ('s_asc', 's_desc', 'b_asc', 's_then_w') ORDER BY tag COLLATE "C";

-- more segments: each promotion turns the rows of the write buffer into a segment of its own
SELECT ev_rows(6001, 9000);
SELECT sift.col_promote('ev_col') >= 0 AS promoted;
SELECT ev_rows(9001, 12000);
SELECT sift.col_promote('ev_col') >= 0 AS promoted;
SELECT * FROM ev_stage('segments');
SELECT tag, ev_check(q, 2) FROM ev_q WHERE tag IN ('s_asc', 'b_desc', 's_filter') ORDER BY tag COLLATE "C";

-- values added to the type after the index was built, before and after others: the rows keep their OIDs, the order of the type changes
ALTER TYPE sev ADD VALUE 'trace' BEFORE 'warn';
ALTER TYPE sev ADD VALUE 'notice' AFTER 'info';
SELECT ev_rows(12001, 12600);
UPDATE ev SET s = 'trace' WHERE id % 211 = 0;
UPDATE ev SET s = 'notice', w = 'notice' WHERE id % 223 = 0;
DELETE FROM ev WHERE id % 89 = 0;
SELECT * FROM ev_stage('buffer');
SELECT tag, ev_check(q, 2) FROM ev_q WHERE tag IN ('s_asc', 's_desc', 'w_then_s') ORDER BY tag COLLATE "C";
-- the order of the type is read when the scan opens, not when the plan is made: a cached plan sees the value added later
PREPARE ev_p AS SELECT s, count(*) AS n FROM (SELECT id, s FROM ev ORDER BY s DESC, id LIMIT 3000) x GROUP BY s ORDER BY 1 DESC;
SET plan_cache_mode = force_generic_plan;
EXECUTE ev_p;
ALTER TYPE sev ADD VALUE 'panic' AFTER 'fatal';
UPDATE ev SET s = 'panic' WHERE id % 307 = 0;
EXECUTE ev_p;
RESET plan_cache_mode;
DEALLOCATE ev_p;
SELECT ev_check($q$SELECT id, s FROM ev ORDER BY s DESC, id LIMIT 12$q$) AS after_panic;

-- a heap of keys wider than work_mem sorts instead, and the rows past work_mem spill to a tuplesort
SET work_mem = '64kB';
SELECT ev_check($q$SELECT id, s, b FROM ev ORDER BY s, b DESC, id LIMIT 30 OFFSET 9000$q$) AS spilled_two_keys,
	   ev_check($q$SELECT id, s FROM ev ORDER BY s, id LIMIT 4000$q$) AS wide_heap;
RESET work_mem;

SELECT sift.col_merge('ev_col', true) >= 0 AS merged;
SELECT * FROM ev_stage('merged');
VACUUM ev;
SELECT * FROM ev_stage('vacuumed');
SELECT sift.col_verify('ev_col', true);

-- a partitioned table
CREATE TABLE ep (id int, g int NOT NULL, s sev, w sev, b big, pad text DEFAULT repeat('x', 1500)) PARTITION BY RANGE (g);
CREATE TABLE ep_a PARTITION OF ep FOR VALUES FROM (0) TO (3) WITH (autovacuum_enabled = false);
CREATE TABLE ep_b PARTITION OF ep FOR VALUES FROM (3) TO (7) WITH (autovacuum_enabled = false);
INSERT INTO ep (id, g, s, w, b) SELECT id, g, s, w, b FROM ev WHERE id <= 5000;
CREATE INDEX ep_col ON ep USING sift_col (id, g, s, w, b);
VACUUM (ANALYZE) ep;
SELECT tag, ev_check(replace(q, 'FROM ev ', 'FROM ep ')) FROM ev_q WHERE tag IN ('s_asc', 's_desc', 's_filter', 's_then_w', 'b_asc') ORDER BY tag COLLATE "C";

-- a sift_table
CREATE TABLE et (id int, g int, s sev, b big, pad text DEFAULT repeat('x', 1500)) USING sift_table;
INSERT INTO et (id, g, s, b) SELECT id, g, s, b FROM ev WHERE id <= 5000;
VACUUM (ANALYZE) et;
SELECT ev_check($q$SELECT id, s FROM et ORDER BY s, id LIMIT 25$q$) AS table_s_asc,
	   ev_check($q$SELECT id, s FROM et ORDER BY s DESC NULLS LAST, id LIMIT 25$q$) AS table_s_desc,
	   ev_check($q$SELECT id, b FROM et WHERE g = 3 ORDER BY b DESC, id LIMIT 30 OFFSET 100$q$) AS table_filter;

DROP FUNCTION ev_check, ev_mode, ev_rows, ev_stage;
DROP TABLE ev, ev_q, ep, et;
DROP TYPE sev, big;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
