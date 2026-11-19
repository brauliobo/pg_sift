CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_buffercache;
SET client_min_messages = warning;
SET sift.col_build_source = auto;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.async_maintenance = off;
SELECT version();

CREATE FUNCTION rh_forks(t regclass) RETURNS TABLE (fork text, blocks bigint, block_list text) LANGUAGE sql AS $$
	SELECT (ARRAY['main', 'fsm', 'vm', 'init'])[relforknumber + 1], count(*), string_agg(relblocknumber::text, ',' ORDER BY relblocknumber)
	FROM pg_buffercache WHERE relfilenode = pg_relation_filenode(t) AND reldatabase = (SELECT oid FROM pg_database WHERE datname = current_database())
	GROUP BY relforknumber ORDER BY relforknumber $$;
CREATE FUNCTION rh_evict(t regclass) RETURNS bigint LANGUAGE sql AS $$
	SELECT count(*) FROM (SELECT pg_buffercache_evict(bufferid) FROM pg_buffercache
	WHERE relfilenode = pg_relation_filenode(t) AND reldatabase = (SELECT oid FROM pg_database WHERE datname = current_database())) e $$;
CREATE FUNCTION rh_acc(t regclass) RETURNS bigint LANGUAGE sql AS $$
	SELECT coalesce(sum(heap_blks_read + heap_blks_hit), 0)::bigint FROM pg_statio_user_tables WHERE relid = t $$;

-- with a visibility map (vacuumed) and without (never vacuumed)
CREATE TABLE rh_v (id int, k int, g int, note text) WITH (autovacuum_enabled = false);
CREATE TABLE rh_n (id int, k int, g int, note text) WITH (autovacuum_enabled = false);
INSERT INTO rh_v SELECT i, i, i % 7, 'note ' || (i % 50) FROM generate_series(1, 20000) i;
INSERT INTO rh_n SELECT * FROM rh_v;
VACUUM (FREEZE, ANALYZE) rh_v;
ANALYZE rh_n;
CREATE INDEX rh_v_idx ON rh_v USING sift_col (id, k, g, note) WITH (key_cut = 'k', track_changes = 'off');
CREATE INDEX rh_n_idx ON rh_n USING sift_col (id, k, g, note) WITH (key_cut = 'k', track_changes = 'off');
CHECKPOINT;

SELECT 'v' AS tbl, rh_evict('rh_v'::regclass) AS evicted;
SELECT 'v before' AS step, * FROM rh_forks('rh_v');
SELECT pg_stat_force_next_flush() \gset
CREATE TEMP TABLE rh_mark AS SELECT rh_acc('rh_v') AS acc;
REINDEX INDEX rh_v_idx;
SELECT pg_stat_force_next_flush() \gset
SELECT current_setting('sift.test_col_from_index_used') AS used, current_setting('sift.test_col_build_table_buffers') AS build_buffers, rh_acc('rh_v') - acc AS accesses FROM rh_mark;
SELECT 'v after' AS step, * FROM rh_forks('rh_v');

SELECT 'n' AS tbl, rh_evict('rh_n'::regclass) AS evicted;
SELECT 'n before' AS step, * FROM rh_forks('rh_n');
SELECT pg_stat_force_next_flush() \gset
TRUNCATE rh_mark;
INSERT INTO rh_mark SELECT rh_acc('rh_n');
REINDEX INDEX rh_n_idx;
SELECT pg_stat_force_next_flush() \gset
SELECT current_setting('sift.test_col_from_index_used') AS used, current_setting('sift.test_col_build_table_buffers') AS build_buffers, rh_acc('rh_n') - acc AS accesses FROM rh_mark;
SELECT 'n after' AS step, * FROM rh_forks('rh_n');

DROP TABLE rh_v, rh_n CASCADE;
DROP FUNCTION rh_forks(regclass), rh_evict(regclass), rh_acc(regclass);
DROP EXTENSION pg_buffercache;
DROP EXTENSION pg_sift;
