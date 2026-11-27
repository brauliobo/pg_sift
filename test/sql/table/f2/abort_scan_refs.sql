-- A statement that fails in the middle of a scan of a sift_table gives back the granules its scan and its slot held: the error deletes the memory of the scan without ending
-- it, and the resource owner of the statement releases what it kept, so the point cache can still drop every granule (here under a budget of 1 kB). Run with Format 2.0 tables.
CREATE EXTENSION pg_sift;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;
SET client_min_messages = warning;
\set VERBOSITY terse
CREATE TABLE asr (id int, k int, t text) USING sift_table;
INSERT INTO asr SELECT g, g % 7, 'row ' || g FROM generate_series(1, 50000) g;
CREATE INDEX asr_k ON asr (k);
VACUUM ANALYZE asr;
\c -
SET enable_seqscan = off;
SET enable_indexscan = off;
SET sift.enable_agg = off;
SELECT count(t) FROM asr WHERE k = 3 AND 1 / (id - 25000) <> 7;
BEGIN;
SAVEPOINT s;
SELECT count(t) FROM asr WHERE k = 3 AND 1 / (id - 25000) <> 7;
ROLLBACK TO s;
SELECT count(t) FROM asr WHERE k = 4;
COMMIT;
SET sift.backend_cache_kb = 1;
SELECT count(t) FROM asr WHERE k = 2;
SELECT count(*) AS granules_kept FROM pg_backend_memory_contexts WHERE name = 'sift cache f2 granule';
DROP TABLE asr;
DROP EXTENSION pg_sift;
