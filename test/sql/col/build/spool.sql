-- The temporary files of a build by key ranges (sift_col build spill.c, col/v1/colspool.c, col/v1/rowfile.c) take a fraction of what the rows are in memory. The bytes of the files of the last
-- build are sift.test_col_spill_bytes, the chunk rows the plan chose sift.test_col_spool_used_rows (0: the rows were packed in blocks) and the price it told the user sift.test_col_spool_est_bytes.
-- A row of this table packs to at most 230 bytes (8 integer or timestamp leaves of at most 6 bytes, 5 float leaves of 8 bytes, 5 short strings, the elements of the arrays) and takes about 130
-- as an uncompressed column chunk (8 bytes for each of the 13 numeric leaves and the codes and elements of the rest). A budget that holds no chunk for each range packs the rows, in blocks that share the budget between the ranges: those must be
-- under 60% of the packed row (98 bytes a row at 6,000 rows, 130 at 24,000: the more ranges, the smaller the blocks). A budget that holds the chunks writes columns: under 48 bytes a row (the cascade codecs alone make 25), whatever the number of rows, the chunk size or the layer;
-- a build that does not run the cascade (layer 4) takes the 130 and fails the bound. The rows of the index are the table's and a key is in one unit, at every size.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
-- Format 1: the build paths and spools are Format 1.
SET sift.test_create_format1 = on;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.async_maintenance = off;
SET sift.test_col_segment_kb = 400;
SET sift.test_col_spill_run_kb = 400;

CREATE TABLE sp_all (id int, tenant_id int, parent_id int, status smallint, created_at timestamptz, updated_at timestamptz, a_count int, counter_a int,
	kind text, label text, body text, doc jsonb) WITH (autovacuum_enabled = false);
-- the document of a row depends on i % 300 only: the 300 documents are built once
INSERT INTO sp_all
WITH d AS MATERIALIZED (SELECT i AS r,
	jsonb_build_object('metrics', (SELECT jsonb_object_agg('m' || k, ((i * k) % 50) / 10.0) FROM generate_series(1, 4 + i % 5) k),
		'items', (SELECT jsonb_agg(jsonb_build_object('category', 'cat_' || ((i + k) % 30), 'strength', CASE (i + k) % 3 WHEN 0 THEN 'pos' WHEN 1 THEN 'neg' ELSE 'neu' END,
			'score', ((i + k) % 10) / 2.0, 'sub', 'sub_' || ((i * k) % 12))) FROM generate_series(1, 1 + i % 4) k),
		'label', 'doc ' || (i % 20), 'note', 'note ' || (i % 100)) AS doc
	FROM generate_series(0, 299) i)
SELECT i, i % 40, (i::bigint * 7919 % 3000)::int, (i % 5)::smallint, timestamptz '2026-01-01' + (i * 37 % 100000) * interval '1 minute',
	timestamptz '2026-02-01' + (i % 777) * interval '1 hour', i % 11, i % 7, 'kind_' || (i % 9), 'label ' || (i % 300), 'sample text number ' || (i % 4000), d.doc
FROM generate_series(1, 96000) i JOIN d ON d.r = i % 300 ORDER BY i;

CREATE FUNCTION sp_same(t regclass) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a text; b text; q text;
BEGIN
	FOREACH q IN ARRAY ARRAY['SELECT count(*), sum(a_count), min(label), max(created_at) FROM %s WHERE parent_id = 42',
		'SELECT status, count(*), sum(counter_a) FROM %s WHERE parent_id < 20 GROUP BY 1 ORDER BY 1', 'SELECT count(*), sum(id) FROM %s'] LOOP
		SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexscan = off; SET LOCAL sift.enable_agg = on;
		EXECUTE format('SELECT string_agg(x::text, E''\n'') FROM (' || q || ') x', t) INTO a;
		SET LOCAL sift.enable_agg = off; SET LOCAL enable_seqscan = on;
		EXECUTE format('SELECT string_agg(x::text, E''\n'') FROM (' || q || ') x', t) INTO b;
		IF a IS DISTINCT FROM b THEN RETURN false; END IF;
	END LOOP;
	RETURN true;
END $$;

CREATE TABLE sp_sizes (n int, spill_rows int, spill_bytes int, units numeric, due text, same bool, chunk_rows int, est_bytes int);
\set n 6000
CREATE TABLE sp_t AS SELECT * FROM sp_all WHERE id <= :n;
VACUUM (FREEZE, ANALYZE) sp_t;
CREATE INDEX sp_idx ON sp_t USING sift_col (id, tenant_id, parent_id, status, created_at, updated_at, a_count, counter_a, kind, label, body, doc)
	WITH (key_cut = 'parent_id', paths = 'doc.metrics.* float8, doc.items[]{category text, strength text, score float8, sub text}, doc.label text, doc.note text');
INSERT INTO sp_sizes
SELECT :n, current_setting('sift.test_col_spill_rows')::int, current_setting('sift.test_col_spill_bytes')::int,
	(SELECT round(point_units::numeric, 1) FROM sift.col_clustering('sp_idx', 1) WHERE path = 'parent_id'), sift.col_maintenance_due('sp_idx')::text, sp_same('sp_t'),
	current_setting('sift.test_col_spool_used_rows')::int, current_setting('sift.test_col_spool_est_bytes')::int;
DROP TABLE sp_t;
\set n 12000
CREATE TABLE sp_t AS SELECT * FROM sp_all WHERE id <= :n;
VACUUM (FREEZE, ANALYZE) sp_t;
CREATE INDEX sp_idx ON sp_t USING sift_col (id, tenant_id, parent_id, status, created_at, updated_at, a_count, counter_a, kind, label, body, doc)
	WITH (key_cut = 'parent_id', paths = 'doc.metrics.* float8, doc.items[]{category text, strength text, score float8, sub text}, doc.label text, doc.note text');
INSERT INTO sp_sizes
SELECT :n, current_setting('sift.test_col_spill_rows')::int, current_setting('sift.test_col_spill_bytes')::int,
	(SELECT round(point_units::numeric, 1) FROM sift.col_clustering('sp_idx', 1) WHERE path = 'parent_id'), sift.col_maintenance_due('sp_idx')::text, sp_same('sp_t'),
	current_setting('sift.test_col_spool_used_rows')::int, current_setting('sift.test_col_spool_est_bytes')::int;
DROP TABLE sp_t;
\set n 24000
CREATE TABLE sp_t AS SELECT * FROM sp_all WHERE id <= :n;
VACUUM (FREEZE, ANALYZE) sp_t;
CREATE INDEX sp_idx ON sp_t USING sift_col (id, tenant_id, parent_id, status, created_at, updated_at, a_count, counter_a, kind, label, body, doc)
	WITH (key_cut = 'parent_id', paths = 'doc.metrics.* float8, doc.items[]{category text, strength text, score float8, sub text}, doc.label text, doc.note text');
INSERT INTO sp_sizes
SELECT :n, current_setting('sift.test_col_spill_rows')::int, current_setting('sift.test_col_spill_bytes')::int,
	(SELECT round(point_units::numeric, 1) FROM sift.col_clustering('sp_idx', 1) WHERE path = 'parent_id'), sift.col_maintenance_due('sp_idx')::text, sp_same('sp_t'),
	current_setting('sift.test_col_spool_used_rows')::int, current_setting('sift.test_col_spool_est_bytes')::int;
DROP TABLE sp_t;

SELECT n, spill_rows = n AS every_row_spooled, chunk_rows = 0 AS packed_rows, spill_bytes < spill_rows * 138 AS under_60_percent_of_the_packed_row, units AS units_for_a_key, due, same AS same_rows
FROM sp_sizes ORDER BY n;
SELECT max(spill_bytes::numeric / spill_rows) < 1.5 * min(spill_bytes::numeric / spill_rows) AS bytes_a_row_do_not_grow_with_rows FROM sp_sizes;
TRUNCATE sp_sizes;

-- the budget holds a chunk for each range: columns. sp_build(rows, memory of the build in kB, rows of a chunk to force or 0, layer to force or 0)
CREATE TABLE sp_t24000 AS SELECT * FROM sp_all WHERE id <= 24000;
CREATE TABLE sp_t48000 AS SELECT * FROM sp_all WHERE id <= 48000;
CREATE TABLE sp_t96000 AS SELECT * FROM sp_all;
VACUUM (FREEZE, ANALYZE) sp_t24000;
VACUUM (FREEZE, ANALYZE) sp_t48000;
VACUUM (FREEZE, ANALYZE) sp_t96000;
CREATE PROCEDURE sp_build(n int, segkb int, chunk int, layer int) LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.test_col_segment_kb', segkb::text, false);
	PERFORM set_config('sift.test_col_spill_run_kb', '0', false);
	PERFORM set_config('sift.test_col_spool_chunk_rows', chunk::text, false);
	PERFORM set_config('sift.test_col_spool_layer', layer::text, false);
	EXECUTE format('CREATE INDEX sp_idx ON sp_t%s USING sift_col (id, tenant_id, parent_id, status, created_at, updated_at, a_count, counter_a, kind, label, body, doc)
		WITH (key_cut = ''parent_id'', paths = ''doc.metrics.* float8, doc.items[]{category text, strength text, score float8, sub text}, doc.label text, doc.note text'')', n);
	INSERT INTO sp_sizes
	SELECT n, current_setting('sift.test_col_spill_rows')::int, current_setting('sift.test_col_spill_bytes')::int,
		(SELECT round(point_units::numeric, 1) FROM sift.col_clustering('sp_idx', 1) WHERE path = 'parent_id'), sift.col_maintenance_due('sp_idx')::text, sp_same(('sp_t' || n)::regclass),
		current_setting('sift.test_col_spool_used_rows')::int, current_setting('sift.test_col_spool_est_bytes')::int;
	DROP INDEX sp_idx;
END $$;
CALL sp_build(24000, 12000, 0, 0);
CALL sp_build(48000, 24000, 0, 0);
CALL sp_build(96000, 24000, 0, 0);
-- a range with fewer rows than a chunk: one partial chunk each, and the same bound
CALL sp_build(24000, 12000, 16384, 0);
-- every layer the chunk can have, and the cascade alone
CALL sp_build(24000, 12000, 0, 1);
CALL sp_build(24000, 12000, 0, 2);
CALL sp_build(24000, 12000, 0, 3);

SELECT n, chunk_rows > 0 AS columns, spill_rows = n AS every_row_spooled, spill_bytes < 48 * spill_rows AS under_48_bytes_a_row, units AS units_for_a_key, due, same AS same_rows,
	est_bytes BETWEEN 0.7 * spill_bytes AND 1.5 * spill_bytes AS price_near_the_files
FROM sp_sizes ORDER BY ctid;
-- the layers are what they say: lz4 and zstd make the chunks smaller than the cascade alone
SELECT (SELECT spill_bytes FROM sp_sizes ORDER BY ctid OFFSET 5 LIMIT 1) < (SELECT spill_bytes FROM sp_sizes ORDER BY ctid OFFSET 4 LIMIT 1) AS lz4_under_the_cascade,
	(SELECT spill_bytes FROM sp_sizes ORDER BY ctid OFFSET 6 LIMIT 1) < (SELECT spill_bytes FROM sp_sizes ORDER BY ctid OFFSET 5 LIMIT 1) AS zstd_under_lz4;
TRUNCATE sp_sizes;
-- a build that does not run the cascade takes the uncompressed columns and fails the bound
CALL sp_build(24000, 12000, 0, 4);
SELECT spill_bytes < 48 * spill_rows AS no_cascade_fits_48_bytes_a_row, spill_bytes > 100 * spill_rows AS no_cascade_takes_the_plain_columns FROM sp_sizes;

RESET sift.test_col_segment_kb;
RESET sift.test_col_spill_run_kb;
RESET sift.test_col_spool_chunk_rows;
RESET sift.test_col_spool_layer;
DROP TABLE sp_sizes, sp_all, sp_t24000, sp_t48000, sp_t96000;
DROP PROCEDURE sp_build(int, int, int, int);
DROP FUNCTION sp_same(regclass);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
