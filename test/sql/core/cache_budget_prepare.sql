-- PREPARE TRANSACTION ends the transaction of the backend as COMMIT does: what it kept in the backend caches past sift.backend_cache_kb is given up then
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE FUNCTION cbp_kept() RETURNS bigint LANGUAGE sql AS
$$ SELECT count(*) FROM pg_backend_memory_contexts WHERE name = 'sift cache f2 part pin' $$;
CREATE TABLE cbp_t (id int, v int) USING sift_table;
INSERT INTO cbp_t SELECT i, i % 97 FROM generate_series(1, 20000) i;
VACUUM cbp_t;
SET sift.backend_cache_kb = 32;
SELECT count(*), sum(v) FROM cbp_t WHERE id < 5000;
SELECT cbp_kept() AS kept_after_commit;
BEGIN;
SELECT count(*), sum(v) FROM cbp_t WHERE id < 5000;
PREPARE TRANSACTION 'cbp_p';
SELECT cbp_kept() AS kept_after_prepare;
COMMIT PREPARED 'cbp_p';
RESET sift.backend_cache_kb;
DROP TABLE cbp_t;
DROP FUNCTION cbp_kept();
DROP EXTENSION pg_sift;
