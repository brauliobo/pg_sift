-- A Format 1 scan folds the batches of its source, which hold 16 columns: a statement that reads more (twenty aggregates of twenty columns) overflowed the fold, an Assert on an
-- assert build and an internal error on any other. The plan is refused and the normal plan answers; a Format 2 index reads them all.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE wide (id int, m01 float8, m02 float8, m03 float8, m04 float8, m05 float8, m06 float8, m07 float8, m08 float8, m09 float8, m10 float8,
  n01 int, n02 int, n03 int, n04 int, n05 int, n06 int, n07 int, n08 int, n09 int, n10 int);
INSERT INTO wide SELECT g, g % 97, g % 89, g % 83, g % 79, g % 73, g % 71, g % 67, g % 61, g % 59, g % 53, g % 47, g % 43, g % 41, g % 37, g % 31, g % 29, g % 23, g % 19, g % 17, g % 13
FROM generate_series(1, 20000) g;
CREATE TABLE wide2 AS SELECT * FROM wide;
SET sift.test_create_format1 = on;
CREATE INDEX wide_col ON wide USING sift_col (id, m01, m02, m03, m04, m05, m06, m07, m08, m09, m10, n01, n02, n03, n04, n05, n06, n07, n08, n09, n10);
RESET sift.test_create_format1;
CREATE INDEX wide2_col ON wide2 USING sift_col (id, m01, m02, m03, m04, m05, m06, m07, m08, m09, m10, n01, n02, n03, n04, n05, n06, n07, n08, n09, n10);
VACUUM ANALYZE wide;
VACUUM ANALYZE wide2;
SET sift.agg_force = on;
EXPLAIN (COSTS OFF) SELECT count(*), sum(m01), sum(m02), sum(m03), sum(m04), avg(m05), avg(m06), avg(m07), avg(m08), min(m09), max(m10),
  sum(n01), sum(n02), sum(n03), avg(n04), avg(n05), min(n06), max(n07), sum(n08), sum(n09), sum(n10) FROM wide;
SELECT count(*), sum(m01), sum(m02), sum(m03), sum(m04), avg(m05), avg(m06), avg(m07), avg(m08), min(m09), max(m10),
  sum(n01), sum(n02), sum(n03), avg(n04), avg(n05), min(n06), max(n07), sum(n08), sum(n09), sum(n10) FROM wide;
SELECT count(*), sum(m01), sum(m02), sum(m03), sum(m04), avg(m05), avg(m06), avg(m07), avg(m08), min(m09), max(m10),
  sum(n01), sum(n02), sum(n03), avg(n04), avg(n05), min(n06), max(n07), sum(n08), sum(n09), sum(n10) FROM wide2;
SELECT count(*), sum(m01), sum(n01), sum(n10) FROM wide WHERE m02 > 10;
DROP TABLE wide, wide2;
DROP EXTENSION pg_sift;
