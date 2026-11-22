-- JSONB variant exception streams read without a heap fetch (src/agg/variant_eval.c) and the sorted streaming GROUP BY and Top-N
-- (src/agg/stream_exec.c), each against a loop over the rows. Each is the number of failed cases.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

SELECT sift._test_variant_eval(4, 400) AS variant_failures;
SELECT sift._test_variant_eval(21, 400) AS variant_failures;
SELECT sift._test_stream_exec(6, 300) AS stream_failures;
SELECT sift._test_stream_exec(33, 300) AS stream_failures;

DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
