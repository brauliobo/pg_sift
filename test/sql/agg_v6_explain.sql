-- the compact lines of EXPLAIN (ANALYZE) of the Format 2.0 engine: four scans in text, the pruning one in JSON, and the zero counters of VERBOSE
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SELECT sift._test_explain_v6(s, 'text') FROM generate_series(1, 4) s;
SELECT sift._test_explain_v6(4, 'json');
SELECT sift._test_explain_v6(2, 'text', true);
SELECT s, array_length(string_to_array(trim(trailing E'\n' from sift._test_explain_v6(s, 'text')), E'\n'), 1) <= 6 AS at_most_six_lines FROM generate_series(1, 4) s;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
