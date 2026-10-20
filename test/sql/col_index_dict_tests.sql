-- The test of a predicate on a dictionary leaf (text, or a numeric of any shape stored as images) is made once per segment and predicate: the
-- units of a segment share its dictionary, so a LUT over all of it per unit costs the scan its units times the dictionary.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;

CREATE TABLE dt (id int, k text, n numeric);
INSERT INTO dt SELECT i, 'key-' || lpad(i::text, 6, '0'), i::numeric / 7 FROM generate_series(1, 330000) i;
CREATE INDEX dt_idx ON dt USING sift_col (k, n);

SELECT segments, units > 9 AS several_units FROM sift._test_col_counters('dt_idx', '', 0);
-- leaf 0 is k, leaf 1 the head of n (its second lane follows)
SELECT rows, tests_made FROM sift._test_col_counters('dt_idx', '0:ge:key-165000', 0);
SELECT rows, tests_made FROM sift._test_col_counters('dt_idx', '1:gt:4000;1:le:20000', 0);
SELECT rows, tests_made FROM sift._test_col_counters('dt_idx', '0:in:key-000001|key-250000|key-329999;1:ne:1', 0);
SELECT count(*) FROM dt WHERE k >= 'key-165000';
SELECT count(*) FROM dt WHERE n > 4000 AND n <= 20000;
SELECT count(*) FROM dt WHERE k IN ('key-000001', 'key-250000', 'key-329999') AND n <> 1;

-- a second segment: its dictionary is its own, so it has its tests too
INSERT INTO dt SELECT i, 'key-' || lpad(i::text, 6, '0'), i::numeric / 7 FROM generate_series(330001, 500000) i;
SELECT sift.col_promote('dt_idx') > 0 AS promoted;
SELECT segments, units > 14 AS several_units FROM sift._test_col_counters('dt_idx', '', 0);
SELECT rows, tests_made FROM sift._test_col_counters('dt_idx', '0:ge:key-165000;1:le:60000', 0);
SELECT count(*) FROM dt WHERE k >= 'key-165000' AND n <= 60000;

-- more predicates than the source keeps tests for still gives the rows of the heap
SELECT rows FROM sift._test_col_counters('dt_idx', (SELECT string_agg('0:ne:key-' || lpad((i * 1000)::text, 6, '0'), ';') FROM generate_series(1, 24) i), 0);
SELECT count(*) FROM dt WHERE k NOT IN (SELECT 'key-' || lpad((i * 1000)::text, 6, '0') FROM generate_series(1, 24) i);
DROP TABLE dt;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
