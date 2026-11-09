-- As col_conn_relocate, with sift.col_wbuf_ref on and a write buffer of reference records (the rows of the buffer are the arena's, not the heap's) while the segments move: the stamps and the fill
-- state are the same before and after, and the answers are the normal plan's, with the buffered rows in them.
-- The stamps of a connected index are in the schema of each segment, so what writes the segment's header again without writing its rows keeps them (docs/COL-CONNECT.md): VACUUM writing a
-- net pre-aggregate sidecar, and the space step shifting the runs of a large segment down into free blocks. The segments keep their generation, their stamps and the fill state, and the
-- found-flag plan answers with the rows of the normal plan after each.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET sift.async_maintenance = off;
SET sift.col_wbuf_ref = on;
SET sift.col_auto_merge = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.agg_force = on;
SET sift.agg_lookup_cache_kb = 0;
SET sift.agg_lookup_shared_kb = 0;

CREATE FUNCTION found_run(q text, OUT same boolean, OUT proofs int) LANGUAGE plpgsql AS $$
DECLARE
	p jsonb;
	a text;
	b text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || q INTO p;
	EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (' || q || ') t' INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (' || q || ') t' INTO b;
	SET LOCAL sift.enable_agg = on;
	same := a = b;
	proofs := coalesce(jsonb_path_query_first(p, '$.**."Found Proofs"')::text::int, 0);
END $$;

CREATE FUNCTION check_all() RETURNS TABLE (joined boolean, anti boolean, flags_used boolean) LANGUAGE sql AS $$
	SELECT (SELECT same FROM found_run('SELECT count(*), sum(e.amount) FROM ev e JOIN dim d ON d.id = e.dim_id')),
		   (SELECT same FROM found_run('SELECT count(*) FROM ev e WHERE NOT EXISTS (SELECT 1 FROM dim d WHERE d.id = e.dim_id)')),
		   (SELECT proofs > 0 FROM found_run('SELECT count(*), sum(e.amount) FROM ev e JOIN dim d ON d.id = e.dim_id'))
$$;

CREATE TABLE dim (id int PRIMARY KEY, label text) WITH (autovacuum_enabled = false);
INSERT INTO dim SELECT i, 'l' || i FROM generate_series(1, 500) i;
CREATE INDEX dim_col ON dim USING sift_col (id, label);
CREATE TABLE ev (id int PRIMARY KEY, dim_id int, amount int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, CASE WHEN i % 97 = 0 THEN NULL WHEN i % 89 = 0 THEN 9999 ELSE 1 + i % 500 END, i % 100,
	   md5(i::text) || md5((i + 1)::text) || md5((i + 2)::text) FROM generate_series(1, 60000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, dim_id, amount, pad) WITH (connect = 'dim_id -> dim(id)');
INSERT INTO ev SELECT i, 1 + i % 500, i % 100, md5(i::text) || md5((i + 1)::text) || md5((i + 2)::text) FROM generate_series(60001, 120000) i;
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) ev;
SELECT sift.col_promote('ev_col') >= 0 AS promoted;
SELECT sift.col_merge('ev_col', true) > 0 AS merged;

CREATE TEMP TABLE kept AS SELECT rows_stored FROM sift.col_arena_status();
INSERT INTO ev SELECT i, 1 + i % 500, i % 100, 'buffered' FROM generate_series(120001, 123000) i;
SELECT rows AS buffered FROM sift.col_segments('ev_col') WHERE kind = 'buffer';
SELECT (SELECT rows_stored FROM sift.col_arena_status()) - (SELECT rows_stored FROM kept) AS rows_in_the_arena;
-- one segment, filled under one stamp
CREATE TEMP TABLE before AS SELECT * FROM sift._test_col_conn_stamps('ev_col');
SELECT count(*) AS stamps, count(*) FILTER (WHERE lineage <> 0) AS stamped FROM before;
SELECT fill FROM sift.col_connections WHERE child = 'ev_col'::regclass;
SELECT * FROM check_all();

CREATE FUNCTION file_x100() RETURNS int LANGUAGE sql AS $$
	SELECT (100 * relation_blocks * 8192 / (SELECT sum(bytes) FROM sift.col_sections('ev_col') WHERE section <> 'free'))::int FROM sift.col_index_info('ev_col') $$;
-- the space step shifts the runs of the segment down into the free blocks under it (the file is twice the segment after the merge)
SELECT file_x100() > 170 AS file_twice_the_segment;
SET sift.vacuum_rewrite_limit = 1;
VACUUM ev;
SELECT file_x100() < 115 AS file_near_the_segment;
SELECT (SELECT count(*) FROM sift._test_col_conn_stamps('ev_col') n JOIN before b USING (gen, ordinal, lineage, horizon)) AS same_stamps,
       (SELECT count(*) FROM before) AS stamps;
SELECT fill FROM sift.col_connections WHERE child = 'ev_col'::regclass;
SELECT * FROM check_all();
RESET sift.vacuum_rewrite_limit;

-- a few dead rows: VACUUM writes the net sidecar of the units that hold them and leaves the segment as it is
DELETE FROM ev WHERE id % 500 = 7;
VACUUM ev;
SELECT count(*) AS net_segments FROM sift.col_segments('ev_col') s WHERE kind = 'immutable' AND preagg_bytes > 0 AND dead_rows > 0;
SELECT (SELECT count(*) FROM sift._test_col_conn_stamps('ev_col') n JOIN before b USING (gen, ordinal, lineage, horizon)) AS same_stamps,
       (SELECT count(*) FROM before) AS stamps;
SELECT fill FROM sift.col_connections WHERE child = 'ev_col'::regclass;
SELECT * FROM check_all();

SELECT rows AS buffered_after FROM sift.col_segments('ev_col') WHERE kind = 'buffer';
SELECT sift.col_verify('ev_col', true);
SELECT sift.col_promote('ev_col') >= 0 AS promoted_buffer;
SELECT * FROM check_all();
SELECT sift.col_verify('ev_col', true);

DROP FUNCTION file_x100();
DROP FUNCTION check_all();
DROP TABLE ev, dim;
DROP FUNCTION found_run(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
