-- A projection holds what it reads to work_mem: the cube of a pre-aggregate and the rows of the rest a sort projection hands out without a bound go back to the plan of the relation once
-- they outgrow it, with the same answer, and what a part or a granule is read with is freed before the next one.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;
\set VERBOSITY terse

-- What the statement read: the projection, or the relation
CREATE FUNCTION fx_read(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'relation';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ '^\s*(Projection|Source): ' THEN r := trim(l); END IF;
	END LOOP;
	RETURN r;
END $$;

-- The rows of the statement run as it is planned, summed
CREATE FUNCTION fx_rows(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r record; n int8 := 0; s int8 := 0;
BEGIN
	FOR r IN EXECUTE q LOOP n := n + 1; s := s + r.id; END LOOP;
	RETURN n || ' rows, ids ' || s;
END $$;

CREATE TABLE facts (id int PRIMARY KEY, category_id int, counter_a int) USING sift_table WITH (autovacuum_enabled = off);
INSERT INTO facts SELECT i, i % 8000, i % 10 FROM generate_series(1, 24000) i;
SELECT sift.create_projection('facts', 'by_category', 'GROUP BY category_id MEASURES count(*), sum(counter_a)');
SELECT sift.create_projection('facts', 'by_counter', 'ORDER BY counter_a, id');
VACUUM (ANALYZE) facts;
-- rows the projections do not stand for, every one of them a row of the lookup below
INSERT INTO facts SELECT i, i % 8000, 3 FROM generate_series(24001, 36000) i;

\echo == the 8000 groups of the pre-aggregate fit 16MB and not 64kB: past it the aggregate reads the relation
SET work_mem = '16MB';
SELECT fx_read('SELECT category_id, count(*), sum(counter_a) FROM facts GROUP BY category_id');
SELECT count(*), sum(n), sum(s) FROM (SELECT category_id, count(*) n, sum(counter_a) s FROM facts GROUP BY category_id) g;
SET work_mem = '64kB';
SELECT fx_read('SELECT category_id, count(*), sum(counter_a) FROM facts GROUP BY category_id');
SELECT count(*), sum(n), sum(s) FROM (SELECT category_id, count(*) n, sum(counter_a) s FROM facts GROUP BY category_id) g;
SET sift.enable_agg = off;
SELECT count(*), sum(n), sum(s) FROM (SELECT category_id, count(*) n, sum(counter_a) s FROM facts GROUP BY category_id) g;
RESET sift.enable_agg;

\echo == a lookup has no bound: the 12000 rows of the rest fit 16MB and not 64kB, past it the relation is sorted
SET work_mem = '16MB';
SELECT fx_read('SELECT id FROM facts WHERE counter_a = 3');
SELECT fx_rows('SELECT id FROM facts WHERE counter_a = 3');
SET work_mem = '64kB';
SELECT fx_read('SELECT id FROM facts WHERE counter_a = 3');
SELECT fx_rows('SELECT id FROM facts WHERE counter_a = 3');
RESET work_mem;

\echo == the deletes of every granule are subtracted granule by granule: the statement holds one record at a time, not the 13 of the parts (5 MB together)
CREATE TABLE docs (id int PRIMARY KEY, category_id int, body text) USING sift_table WITH (autovacuum_enabled = off);
-- 82000 rows with 64 bytes of text each: 13 granules of about 400 kB in the 4 parts
INSERT INTO docs SELECT i, i % 4, md5(i::text) || md5((-i)::text) FROM generate_series(1, 82000) i;
SELECT sift.create_projection('docs', 'docs_by_category', 'GROUP BY category_id MEASURES count(*)');
VACUUM docs;
DELETE FROM docs WHERE id % 1000 = 0;
VACUUM docs;
SELECT fx_read('SELECT category_id, count(*) FROM docs GROUP BY category_id');
SELECT sum(total_bytes) AS before FROM pg_backend_memory_contexts \gset
SELECT category_id, n, (SELECT sum(total_bytes) FROM pg_backend_memory_contexts) - :before < 2 * 1024 * 1024 AS held_one_granule
FROM (SELECT category_id, count(*) n FROM docs GROUP BY category_id) g ORDER BY category_id LIMIT 1;

DROP TABLE facts, docs;
DROP FUNCTION fx_read(text), fx_rows(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
