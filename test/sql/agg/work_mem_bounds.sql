-- What SiftAggV6 holds at once stays within work_mem, or the statement fails naming it: a dimension of a left star join that one key makes too large to split
-- (the plan was made at a larger work_mem) and the rows the heap of an ORDER BY .. LIMIT of the groups copies.
SET client_min_messages = warning;
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int, counter_a int, label text) WITH (autovacuum_enabled = false);
INSERT INTO facts SELECT g, g % 4, g % 10, repeat(chr(97 + g % 26), 2000) FROM generate_series(1, 4000) g;
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, counter_a, label) WITH (key_order = id);
-- 3000 rows of key 1: no number of parts splits them
CREATE TABLE dim_a (id int, label_a text);
INSERT INTO dim_a SELECT CASE WHEN g <= 3000 THEN 1 ELSE g END, 'label_' || g % 7 FROM generate_series(1, 3100) g;
ANALYZE facts;
ANALYZE dim_a;
RESET client_min_messages;

SET plan_cache_mode = force_generic_plan;
PREPARE left_star AS SELECT d.label_a, count(*) FROM facts f LEFT JOIN dim_a d ON d.id = f.dim_a_id GROUP BY d.label_a ORDER BY 1;
EXPLAIN (COSTS OFF) EXECUTE left_star;
EXECUTE left_star;
SET work_mem = '64kB';
\set VERBOSITY terse
EXECUTE left_star;
\set VERBOSITY default
RESET work_mem;
DEALLOCATE left_star;
RESET plan_cache_mode;

-- 200 groups of a 2000-byte label: their copies are 400 kB
EXPLAIN (COSTS OFF) SELECT id % 1000 AS k, max(label) FROM facts GROUP BY 1 ORDER BY 1 LIMIT 200;
SELECT count(*), sum(length(m)) FROM (SELECT id % 1000 AS k, max(label) AS m FROM facts GROUP BY 1 ORDER BY 1 LIMIT 200) t;
SET work_mem = '256kB';
\set VERBOSITY terse
SELECT count(*), sum(length(m)) FROM (SELECT id % 1000 AS k, max(label) AS m FROM facts GROUP BY 1 ORDER BY 1 LIMIT 200) t;
\set VERBOSITY default
SELECT count(*), sum(length(m)) FROM (SELECT id % 1000 AS k, max(label) AS m FROM facts GROUP BY 1 ORDER BY 1 LIMIT 20) t;
RESET work_mem;

DROP TABLE facts, dim_a;
DROP EXTENSION pg_sift;
