-- A comparison of a column of a sift_table with a subquery or a parameter is decided by the heap, not read as a constant: the counts equal those of a heap twin for an int, a float, a numeric and a text column.
-- requires: build col
CREATE EXTENSION IF NOT EXISTS pg_sift;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE pq (id int PRIMARY KEY, g int, f8 float8, m numeric(12,2), t text) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE ph (LIKE pq INCLUDING ALL);
INSERT INTO pq SELECT i, i % 7, (i % 40)::float8, (i % 40) / 4.0, 't' || i % 40 FROM generate_series(1, 30000) i;
INSERT INTO ph SELECT * FROM pq;
ANALYZE pq;
ANALYZE ph;

SELECT (SELECT count(*) FROM pq WHERE g = (SELECT g FROM pq WHERE id = 5)) = (SELECT count(*) FROM ph WHERE g = (SELECT g FROM ph WHERE id = 5)) AS int_subquery;
SELECT (SELECT count(*) FROM pq WHERE f8 = (SELECT f8 FROM pq WHERE id = 5)) = (SELECT count(*) FROM ph WHERE f8 = (SELECT f8 FROM ph WHERE id = 5)) AS float_subquery;
SELECT (SELECT count(*) FROM pq WHERE m = (SELECT m FROM pq WHERE id = 5)) = (SELECT count(*) FROM ph WHERE m = (SELECT m FROM ph WHERE id = 5)) AS numeric_subquery;
SELECT (SELECT count(*) FROM pq WHERE t = (SELECT t FROM pq WHERE id = 5)) = (SELECT count(*) FROM ph WHERE t = (SELECT t FROM ph WHERE id = 5)) AS text_subquery;
SELECT (SELECT count(*) FROM pq WHERE t = (SELECT t FROM pq WHERE id = 5)) AS text_subquery_rows;
PREPARE by_text(text) AS SELECT g, count(*) FROM pq WHERE t = $1 GROUP BY g ORDER BY g;
PREPARE by_text_heap(text) AS SELECT g, count(*) FROM ph WHERE t = $1 GROUP BY g ORDER BY g;
SET plan_cache_mode = force_generic_plan;
EXECUTE by_text('t7');
EXECUTE by_text_heap('t7');

DROP TABLE pq, ph;
DROP EXTENSION pg_sift;
