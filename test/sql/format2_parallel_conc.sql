-- Concurrency of format major 2: the parallel CREATE INDEX and CREATE INDEX CONCURRENTLY (every participant writes its runs straight into the index, no temporary file), the pass of a vacuum
-- or of CREATE INDEX CONCURRENTLY that reads the slab of a partition without writing it, the compaction that holds no partition lock while it writes (the deletes that reach its inputs
-- meanwhile go into the vectors of its outputs), and a scan whose collected addresses are bounded by work_mem. Every answer is the heap's.
CREATE EXTENSION pg_sift_test CASCADE;
CREATE EXTENSION pg_sift_helpers;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse

-- The rows an index scan (not a bitmap one) returns, and the temporary blocks the plan wrote: the addresses are collected in a sort that spills past work_mem
CREATE FUNCTION idx_scan(q text, OUT n bigint, OUT temp_written bool) LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexscan = on; SET LOCAL enable_indexonlyscan = off;
	EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) ' || q INTO j;
	n := (SELECT sum(("x"->>'Actual Rows')::numeric * ("x"->>'Actual Loops')::numeric) FROM jsonb_path_query(j, 'strict $.** ? (@."Node Type" == "Index Scan")') x);
	temp_written := (SELECT coalesce(sum(("x"->>'Temp Written Blocks')::numeric), 0) > 0 FROM jsonb_path_query(j, 'strict $.** ? (@."Node Type" == "Index Scan")') x);
END $$;

CREATE TABLE pc (id bigint, tenant int, status text, amount numeric(12, 2), score float8, pad int);
INSERT INTO pc SELECT i, i % 41, (ARRAY['new', 'open', 'closed', 'paused', 'failed'])[1 + i % 5], (i % 9000) / 100.0, (i % 977) / 10.0, i % 7 FROM generate_series(1, 120000) i;
DELETE FROM pc WHERE id % 11 = 0;
ALTER TABLE pc SET (parallel_workers = 4);
SET min_parallel_table_scan_size = 0;

-- CREATE INDEX with 0, 2 and 4 workers: the workers that started are the ones asked for, no temporary file is written, and the index holds the rows of the heap
SELECT pg_stat_force_next_flush();
SELECT temp_bytes AS before FROM pg_stat_database WHERE datname = current_database() \gset
SET max_parallel_maintenance_workers = 0;
SET sift.test_par_workers = 0;
CREATE INDEX pc_w0 ON pc USING sift_col (id, tenant, status, amount, score) WITH (key_order = 'tenant, id');
SELECT current_setting('sift.test_par_workers')::int AS workers_started;
SELECT sift._test_f2_check_keys('pc_w0') ~ ' exact$' AS keys_exact_w0;
SET max_parallel_maintenance_workers = 2;
SET sift.test_par_workers = 0;
CREATE INDEX pc_w2 ON pc USING sift_col (id, tenant, status, amount, score) WITH (key_order = 'tenant, id');
SELECT current_setting('sift.test_par_workers')::int AS workers_started;
SELECT sift._test_f2_check_keys('pc_w2') ~ ' exact$' AS keys_exact_w2;
SET max_parallel_maintenance_workers = 4;
SET sift.test_par_workers = 0;
CREATE INDEX pc_w4 ON pc USING sift_col (id, tenant, status, amount, score) WITH (key_order = 'tenant, id');
SELECT current_setting('sift.test_par_workers')::int AS workers_started;
SELECT sift._test_f2_check_keys('pc_w4') ~ ' exact$' AS keys_exact_w4;
SELECT pg_stat_force_next_flush();
SELECT temp_bytes - :before AS temp_bytes_written FROM pg_stat_database WHERE datname = current_database();

-- The three indexes agree with each other and with the heap on every shape of query
SELECT (SELECT sum(total_rows) FROM sift._test_f2_partitions('pc_w0')) = (SELECT count(*) FROM pc) AS rows_w0,
	   (SELECT sum(total_rows) FROM sift._test_f2_partitions('pc_w2')) = (SELECT count(*) FROM pc) AS rows_w2,
	   (SELECT sum(total_rows) FROM sift._test_f2_partitions('pc_w4')) = (SELECT count(*) FROM pc) AS rows_w4;
DROP INDEX pc_w0, pc_w2;
SELECT sift_helpers.f2_same('SELECT id, amount FROM pc WHERE tenant = 7'), sift_helpers.f2_same('SELECT id FROM pc WHERE tenant BETWEEN 3 AND 9 AND status = ''open'''),
	   sift_helpers.f2_same('SELECT id, score FROM pc WHERE id BETWEEN 5000 AND 5400'), sift_helpers.f2_same('SELECT tenant, count(*), sum(amount) FROM pc WHERE score > 90 GROUP BY tenant');

-- CREATE INDEX CONCURRENTLY, in parallel
DROP INDEX pc_w4;
SET sift.test_par_workers = 0;
CREATE INDEX CONCURRENTLY pc_cic ON pc USING sift_col (id, tenant, status, amount, score) WITH (key_order = 'tenant, id');
SELECT current_setting('sift.test_par_workers')::int AS workers_started, indisvalid FROM pg_index WHERE indexrelid = 'pc_cic'::regclass;
SELECT sift._test_f2_check_keys('pc_cic') ~ ' exact$' AS keys_exact_cic;
SELECT sift_helpers.f2_same('SELECT id, amount FROM pc WHERE tenant = 7'), sift_helpers.f2_same('SELECT id FROM pc WHERE id BETWEEN 70000 AND 70500'), sift_helpers.f2_same('SELECT status, count(*) FROM pc GROUP BY status');

-- The slab of a partition: single-row transactions stage rows in it, a few deletes wait in the pending buffer, and a vacuum offers the rows of the slab to the callback as they are
SELECT format('INSERT INTO pc VALUES (%s, %s, ''open'', 1.5, 2.5, 0)', 200000 + i, i % 41) FROM generate_series(1, 12) i \gexec
SELECT has_slab FROM sift._test_f2_partitions('pc_cic');
SELECT sift_helpers.f2_same('SELECT id FROM pc WHERE id > 199990');
DELETE FROM pc WHERE id BETWEEN 200003 AND 200006;
DELETE FROM pc WHERE id % 13 = 0 AND id < 3000;
VACUUM pc;
SELECT sift._test_f2_check_keys('pc_cic') ~ ' exact$' AS keys_exact_after_vacuum;
SELECT sift_helpers.f2_same('SELECT id FROM pc WHERE id > 199990'), sift_helpers.f2_same('SELECT id, amount FROM pc WHERE tenant = 7');
SELECT (SELECT sum(total_rows - total_dead) FROM sift._test_f2_partitions('pc_cic')) = (SELECT count(*) FROM pc) AS index_rows_are_heap_rows;
DROP INDEX pc_cic;

-- Compaction takes no partition lock while it writes: the parts it joins, then the parts it merges, with deletes that reach their inputs meanwhile (as a settled delete of the bulk path
-- does); the vector of the part that replaces them holds those rows, and no row is lost or doubled
SET sift.enable_agg = off;
CREATE TABLE cc (id bigint, tenant int, v int);
CREATE INDEX cc_i ON cc USING sift_col (id, tenant, v) WITH (key_order = 'id');
INSERT INTO cc SELECT i, i % 17, i % 100 FROM generate_series(1, 6000) i;
INSERT INTO cc SELECT i, i % 17, i % 100 FROM generate_series(6001, 12000) i;
INSERT INTO cc SELECT i, i % 17, i % 100 FROM generate_series(12001, 18000) i;
SELECT count(*) AS parts_before, sum(nrows) AS rows_before FROM sift._test_f2_parts('cc_i', 1, NULL);
SELECT sift._test_f2_compact_racing('cc_i', 1, NULL, (SELECT array_agg(part_gen ORDER BY part_gen) FROM sift._test_f2_parts('cc_i', 1, NULL)), 25) AS joined_done;
SELECT count(*) AS parts_after, sum(nrows) AS rows_after, sum(ndead) AS dead_after FROM sift._test_f2_parts('cc_i', 1, NULL);
SELECT sift._test_f2_check_keys('cc_i') ~ ' exact$' AS keys_exact_joined;
SELECT (SELECT n FROM idx_scan('SELECT id FROM cc WHERE id >= 0')) AS index_rows_joined, (SELECT count(*) FROM cc) - 25 AS heap_rows_less_deleted;

CREATE TABLE cm (id bigint, tenant int, v int);
CREATE INDEX cm_i ON cm USING sift_col (id, tenant, v) WITH (key_order = 'tenant');
INSERT INTO cm SELECT i, i % 17, i % 100 FROM generate_series(1, 6000) i;
INSERT INTO cm SELECT i, i % 17, i % 100 FROM generate_series(6001, 12000) i;
INSERT INTO cm SELECT i, i % 17, i % 100 FROM generate_series(12001, 18000) i;
SELECT count(*) AS parts_before, sum(nrows) AS rows_before FROM sift._test_f2_parts('cm_i', 1, NULL);
SELECT sift._test_f2_compact_racing('cm_i', 1, NULL, (SELECT array_agg(part_gen ORDER BY part_gen) FROM sift._test_f2_parts('cm_i', 1, NULL)), 40) AS merged_done;
SELECT count(*) AS parts_after, sum(nrows) AS rows_after, sum(ndead) AS dead_after FROM sift._test_f2_parts('cm_i', 1, NULL);
SELECT sift._test_f2_check_keys('cm_i') ~ ' exact$' AS keys_exact_merged;
SELECT (SELECT n FROM idx_scan('SELECT id FROM cm WHERE id >= 0')) AS index_rows_merged, (SELECT count(*) FROM cm) - 40 AS heap_rows_less_deleted;
SELECT sift_helpers.f2_same('SELECT id, v FROM cm WHERE tenant = 5 AND id > 17000');
DROP TABLE cc, cm;
RESET sift.enable_agg;

-- A scan keeps no more addresses than work_mem holds: under 512kB (8 bytes an address) the addresses of 108,889 rows go through a sort that spills, and a bitmap scan hands them to the bitmap as they come
CREATE INDEX pc_scan ON pc USING sift_col (id, tenant, status, amount, score) WITH (key_order = 'id');
SET work_mem = '512kB';
SELECT n AS index_scan_rows, temp_written AS sort_spilled FROM idx_scan('SELECT id FROM pc WHERE id >= 0');
SELECT (SELECT n FROM idx_scan('SELECT id FROM pc WHERE id >= 0')) = (SELECT count(*) FROM pc) AS index_scan_is_heap;
SELECT sift_helpers.f2_same('SELECT id, amount FROM pc WHERE id >= 0 AND score >= 0');
SELECT sift_helpers.f2_same('SELECT tenant, count(*), sum(amount) FROM pc WHERE id >= 0 GROUP BY tenant');
VACUUM pc;
SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_indexscan = off; SET enable_indexonlyscan = on;
SELECT count(*) AS ioss_rows, sum(id) AS ioss_sum, sum(tenant) AS ioss_tenants FROM pc WHERE id >= 0;
RESET enable_seqscan; RESET enable_bitmapscan; RESET enable_indexscan; RESET enable_indexonlyscan;
SELECT count(*) AS heap_rows, sum(id) AS heap_sum, sum(tenant) AS heap_tenants FROM pc WHERE id >= 0;
RESET work_mem;

DROP TABLE pc;
DROP FUNCTION idx_scan(text);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
