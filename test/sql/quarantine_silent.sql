-- The limit of the quarantine, pinned: a changed byte inside the bits of a BM25 posting list block that keeps the structure of the page and of the blob readable is read as other postings. The
-- read path gives rows that differ from the heap's, with no error, so nothing is recorded; sift.check and heapallindexed report the blob, and REINDEX gives the rows of the heap
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET sift.on_corruption = quarantine;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE t (id int PRIMARY KEY, v int, g int, s text, body text);
INSERT INTO t SELECT i, i % 13, i % 5, 'w' || (i % 17), 'alpha beta ' || (i % 10) || ' gamma' || (i % 3) FROM generate_series(1, 6000) i;
CREATE INDEX t_bm25 ON t USING sift (body);
ANALYZE t;
CREATE TABLE heap_rows AS SELECT id FROM t WHERE body @@> 'alpha 3 gamma1' ORDER BY id LIMIT 30;

SELECT sift._test_poke('t_bm25', 4, 1906, 4049895176, 4);
CREATE TABLE index_rows AS SELECT id FROM t WHERE body @@> 'alpha 3 gamma1' ORDER BY id LIMIT 30;
SELECT (SELECT count(*) FROM index_rows) AS index_count, (SELECT count(*) FROM heap_rows h FULL JOIN index_rows i USING (id) WHERE h.id IS NULL OR i.id IS NULL) > 0 AS differs_from_the_heap;
SELECT count(*) AS listed FROM sift.quarantine;
SELECT problem FROM sift.check('t_bm25');
SELECT problem FROM sift.check('t_bm25', true);

REINDEX INDEX t_bm25;
SELECT count(*) AS problems_after_reindex FROM sift.check('t_bm25', true);
SELECT count(*) AS differing_rows FROM (SELECT id FROM t WHERE body @@> 'alpha 3 gamma1' ORDER BY id LIMIT 30) r FULL JOIN heap_rows USING (id) WHERE r.id IS NULL OR heap_rows.id IS NULL;

DROP TABLE t, heap_rows, index_rows;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
