-- REINDEX of a sift_col index from the index it replaces reads no block of the table: with every buffer of the table evicted first, the build accesses none (sift.test_col_build_table_buffers)
-- and none of the main fork is in the cache afterwards. PostgreSQL itself may read a page of the visibility map after the build (17 does), so the other forks are only allowed to be that.
-- A build from the heap, which the same steps must tell from it, reads the pages.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_buffercache;
SET client_min_messages = warning;
SET sift.col_build_source = auto;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.async_maintenance = off;

CREATE TABLE rh_t (id int, k int, g int, note text) WITH (autovacuum_enabled = false);
INSERT INTO rh_t SELECT i, i, i % 7, 'note ' || (i % 50) FROM generate_series(1, 20000) i;
VACUUM (FREEZE, ANALYZE) rh_t;
CREATE INDEX rh_idx ON rh_t USING sift_col (id, k, g, note) WITH (key_cut = 'k', track_changes = 'off');

CREATE FUNCTION rh_cached() RETURNS TABLE (main_fork_blocks bigint, other_forks_are_the_map bool) LANGUAGE sql AS $$
	SELECT count(*) FILTER (WHERE relforknumber = 0), coalesce(bool_and(relforknumber = 2) FILTER (WHERE relforknumber <> 0), true)
	FROM pg_buffercache WHERE relfilenode = pg_relation_filenode('rh_t') AND reldatabase = (SELECT oid FROM pg_database WHERE datname = current_database()) $$;
CREATE FUNCTION rh_evict() RETURNS bigint LANGUAGE sql AS $$
	SELECT count(*) FROM (SELECT pg_buffercache_evict(bufferid) FROM pg_buffercache
	WHERE relfilenode = pg_relation_filenode('rh_t') AND reldatabase = (SELECT oid FROM pg_database WHERE datname = current_database())) e $$;

CHECKPOINT;
SELECT rh_evict() > 0 AS evicted;
SELECT * FROM rh_cached();
REINDEX INDEX rh_idx;
SELECT current_setting('sift.test_col_from_index_used') AS used, current_setting('sift.test_col_build_table_buffers')::int AS build_table_buffers;
SELECT * FROM rh_cached();

CHECKPOINT;
SELECT rh_evict() >= 0 AS evicted;
SET sift.test_col_from_index_off = on;
REINDEX INDEX rh_idx;
RESET sift.test_col_from_index_off;
SELECT current_setting('sift.test_col_from_index_used') AS used, current_setting('sift.test_col_build_table_buffers')::int > 0 AS build_reads_the_table;
SELECT main_fork_blocks > 0 AS main_fork_read FROM rh_cached();

DROP TABLE rh_t CASCADE;
DROP FUNCTION rh_cached(), rh_evict();
DROP EXTENSION pg_buffercache;
DROP EXTENSION pg_sift;
