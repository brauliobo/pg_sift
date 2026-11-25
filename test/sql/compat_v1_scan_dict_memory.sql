-- The segments of a Format 1 sift_table (the 5.x layout) that a backend opens hold no table of the positions a search of a dictionary of tens of thousands of strings looks at until it
-- searches: a merge of many such segments keeps its inputs within the maintenance budget, and a scan holds nothing of the kind.
CREATE EXTENSION pg_sift;
SET sift.test_create_format1 = on;
SET sift.async_maintenance = off;
SET client_min_messages = warning;
SET sift.enable_agg = off;

CREATE TABLE sd (id int, s text, c text) USING sift_table;
INSERT INTO sd SELECT i, md5(i::text), 'c' || i % 7 FROM generate_series(1, 50000) i;
SELECT busy FROM sift.table_maintain('sd');
SELECT count(*) AS strings, sum(length(s)) AS bytes, count(DISTINCT s) AS distinct_strings, min(c), max(c) FROM sd;
SELECT count(*) FILTER (WHERE s = md5(id::text)) AS equal_to_the_source FROM sd;
SELECT coalesce(sum(total_bytes), 0) < 65536 AS no_bisection_table
  FROM pg_backend_memory_contexts WHERE name LIKE 'sift cache segment bisection%';

DROP TABLE sd;
DROP EXTENSION pg_sift;
