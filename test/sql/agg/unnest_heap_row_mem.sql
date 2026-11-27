-- The rows of a lateral unnest that a Format 2 index cannot hold are taken apart one at a time in memory of their own (scope agg_unnest_heap_row of sift._test_mem_highwater()), which a chunk
-- of their elements reuses: what the scan holds for them is one row's, not a batch's. 1,000 rows of the heap in one batch of the index, each a document of 100 kB.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET work_mem = '256kB';
-- a list whose first element is no object is the heap's
CREATE TABLE hm (id int, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO hm SELECT i, jsonb_build_object('parts', jsonb_build_array(5, jsonb_build_object('kind', 'k' || i % 4, 'score', i % 9, 'pad', repeat('x', 100000))))
FROM generate_series(1, 1000) i;
CREATE INDEX hm_col ON hm USING sift_col (id, doc) WITH (paths = 'doc.parts[]{kind text, score float8}');
VACUUM (FREEZE, ANALYZE) hm;
SET enable_seqscan = off;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT count(*) AS reset FROM sift._test_mem_highwater(true) \gset
SELECT e ->> 'kind' AS kind, count(*), sum((e ->> 'score')::float8) FROM hm t CROSS JOIN LATERAL jsonb_array_elements(t.doc -> 'parts') e GROUP BY 1 ORDER BY 1;
SELECT scope, peak BETWEEN 1 AND 4 * 1024 * 1024 AS one_row FROM sift._test_mem_highwater() WHERE scope = 'agg_unnest_heap_row';

DROP TABLE hm;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
