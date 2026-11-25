-- Format 1 behind the zone and parallel scans of the storage seam (core/store/ops.h, col/v1/store_ops_zone.c, v1_store_ops_par.c): the groups the aggregate scan of a sift_col index and of a
-- sift_table of Format 1 gives (count(*) and sum of an integer column by an integer or a text key, with a qual that prunes whole units), and the groups real parallel workers give over the
-- superblocks of the index, are the heap's own after inserts, updates, deletes, a rollback and VACUUM. Format 1 keeps no dictionary a batch aggregates by its codes: it has none to offer.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE FUNCTION sift._test_store_agg(rel regclass, is_table bool, key_att int4, sum_att int4, qual_att int4, qual_gt int8)
	RETURNS TABLE (k text, n int8, s int8, batches int8) AS '$libdir/pg_sift', 'sift_test_store_agg' LANGUAGE C;
CREATE FUNCTION sift._test_store_par(rel regclass, key_att int4, sum_att int4, qual_att int4, qual_gt int8, workers int4)
	RETURNS TABLE (workers int4, k int8, n int8, s int8, runs int8) AS '$libdir/pg_sift', 'sift_test_store_par' LANGUAGE C;
CREATE FUNCTION sift._test_store_dict(rel regclass, is_table bool, att int4) RETURNS bool AS '$libdir/pg_sift', 'sift_test_store_dict' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_store_ops(rel regclass, is_table bool, OUT owner text, OUT vec_types text, OUT live_rows float8, OUT stored_bytes int8, OUT allvis float8, OUT holds text)
	RETURNS record AS '$libdir/pg_sift', 'sift_test_store_ops' LANGUAGE C STRICT;

-- the rows of the relation that differ between the scan and the heap: the groups of key_expr with count and sum of sum_expr where pred
CREATE FUNCTION diff_agg(rel regclass, is_table bool, tbl regclass, key_att int4, key_expr text, sum_att int4, sum_expr text, qual_att int4, qual_gt int8, pred text) RETURNS int8 LANGUAGE plpgsql AS $$
DECLARE d int8;
BEGIN
	SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL sift.enable_agg = off;
	EXECUTE format('SELECT count(*) FROM (
		(SELECT k, n, s FROM sift._test_store_agg(%1$L, %2$L, %4$s, %6$L, %8$L, %9$L) EXCEPT ALL SELECT %5$s::text, count(*), %7$s FROM %3$s WHERE %10$s GROUP BY 1)
		UNION ALL
		(SELECT %5$s::text, count(*), %7$s FROM %3$s WHERE %10$s GROUP BY 1 EXCEPT ALL SELECT k, n, s FROM sift._test_store_agg(%1$L, %2$L, %4$s, %6$L, %8$L, %9$L))) x',
		rel, is_table, tbl, key_att, key_expr, sum_att, sum_expr, qual_att, qual_gt, pred) INTO d;
	RETURN d;
END $$;

CREATE FUNCTION diff_par(rel regclass, tbl regclass, key_att int4, key_expr text, sum_att int4, sum_expr text, qual_att int4, qual_gt int8, pred text, workers int4) RETURNS int8 LANGUAGE plpgsql AS $$
DECLARE d int8;
BEGIN
	SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL sift.enable_agg = off;
	EXECUTE format('SELECT count(*) FROM (
		(SELECT k, n, s FROM sift._test_store_par(%1$L, %3$s, %5$L, %7$L, %8$L, %10$s) EXCEPT ALL SELECT %4$s::int8, count(*), %6$s FROM %2$s WHERE %9$s GROUP BY 1)
		UNION ALL
		(SELECT %4$s::int8, count(*), %6$s FROM %2$s WHERE %9$s GROUP BY 1 EXCEPT ALL SELECT k, n, s FROM sift._test_store_par(%1$L, %3$s, %5$L, %7$L, %8$L, %10$s))) x',
		rel, tbl, key_att, key_expr, sum_att, sum_expr, qual_att, qual_gt, pred, workers) INTO d;
	RETURN d;
END $$;

SET sift.test_create_format1 = on;
CREATE TABLE c1 (id int, g int2, v int8, s text, pad text);
INSERT INTO c1 SELECT i, i % 7, i * 3, 'k' || (i % 5), repeat('p', 150) FROM generate_series(1, 30000) i;
INSERT INTO c1 SELECT i, NULL, NULL, NULL, repeat('p', 150) FROM generate_series(30001, 30100) i;
CREATE INDEX c1_idx ON c1 USING sift_col (id, g, v, s);
CREATE TABLE t1 (id int, g int2, v int8, s text, pad text) USING sift_table;
INSERT INTO t1 SELECT * FROM c1;
ANALYZE c1; ANALYZE t1;
SELECT 'sift_col' AS rel, owner, array_agg(sift._test_store_dict('c1_idx', false, a) ORDER BY a) AS has_dict32 FROM sift._test_store_ops('c1_idx', false), generate_series(1, 4) a GROUP BY owner
UNION ALL SELECT 'sift_table', owner, array_agg(sift._test_store_dict('t1', true, a) ORDER BY a) FROM sift._test_store_ops('t1', true), generate_series(1, 4) a GROUP BY owner;

-- 1. The zone scan: groups of an integer and of a text key, with and without a qual that leaves whole units out
SELECT diff_agg('c1_idx', false, 'c1', 2, 'g', 1, 'sum(id)', NULL, NULL, 'true') AS index_int_key;
SELECT diff_agg('c1_idx', false, 'c1', 4, 's', 1, 'sum(id)', NULL, NULL, 'true') AS index_text_key;
SELECT diff_agg('c1_idx', false, 'c1', 2, 'g', NULL, 'NULL::int8', NULL, NULL, 'true') AS index_count_only;
SELECT diff_agg('c1_idx', false, 'c1', 2, 'g', 1, 'sum(id)', 1, 29000, 'id > 29000') AS index_pruning_qual;
SELECT diff_agg('c1_idx', false, 'c1', 2, 'g', 1, 'sum(id)', 1, 100000, 'id > 100000') AS index_qual_none;
SELECT diff_agg('t1', true, 't1', 2, 'g', 1, 'sum(id)', NULL, NULL, 'true') AS table_int_key;
SELECT diff_agg('t1', true, 't1', 4, 's', 1, 'sum(id)', NULL, NULL, 'true') AS table_text_key;
SELECT diff_agg('t1', true, 't1', 2, 'g', 1, 'sum(id)', 1, 29000, 'id > 29000') AS table_pruning_qual;

SELECT sum(n) = (SELECT count(*) FROM c1) AS index_scan_counts_every_row, count(*) AS index_groups FROM sift._test_store_agg('c1_idx', false, 2, 1, NULL, NULL);
SELECT sum(n) = (SELECT count(*) FROM t1) AS table_scan_counts_every_row, count(*) AS table_groups FROM sift._test_store_agg('t1', true, 2, 1, NULL, NULL);

-- 2. Writes: new rows (the write buffer), new versions, dead rows, a rollback, VACUUM
INSERT INTO c1 SELECT i, i % 7, i, 'new' || (i % 3), repeat('p', 150) FROM generate_series(40001, 42000) i;
UPDATE c1 SET g = (g + 1) % 7, s = s || '!' WHERE id % 7 = 0;
DELETE FROM c1 WHERE id % 11 = 0;
INSERT INTO t1 SELECT i, i % 7, i, 'new' || (i % 3), repeat('p', 150) FROM generate_series(40001, 42000) i;
UPDATE t1 SET g = (g + 1) % 7, s = s || '!' WHERE id % 7 = 0;
DELETE FROM t1 WHERE id % 11 = 0;
SELECT diff_agg('c1_idx', false, 'c1', 2, 'g', 1, 'sum(id)', NULL, NULL, 'true') AS index_after_writes;
SELECT diff_agg('c1_idx', false, 'c1', 4, 's', 1, 'sum(id)', 1, 100, 'id > 100') AS index_text_qual_after_writes;
SELECT diff_agg('t1', true, 't1', 2, 'g', 1, 'sum(id)', NULL, NULL, 'true') AS table_after_writes;
SELECT diff_agg('t1', true, 't1', 4, 's', 1, 'sum(id)', 1, 100, 'id > 100') AS table_text_qual_after_writes;
VACUUM c1; VACUUM t1;
SELECT diff_agg('c1_idx', false, 'c1', 2, 'g', 1, 'sum(id)', NULL, NULL, 'true') AS index_after_vacuum;
SELECT diff_agg('t1', true, 't1', 2, 'g', 1, 'sum(id)', NULL, NULL, 'true') AS table_after_vacuum;
BEGIN;
INSERT INTO c1 SELECT i, 1, i, 'rolled', repeat('p', 150) FROM generate_series(50001, 50500) i;
DELETE FROM c1 WHERE id < 100;
SELECT diff_agg('c1_idx', false, 'c1', 2, 'g', 1, 'sum(id)', NULL, NULL, 'true') AS index_own_writes;
ROLLBACK;
SELECT diff_agg('c1_idx', false, 'c1', 2, 'g', 1, 'sum(id)', NULL, NULL, 'true') AS index_rolled_back;

-- 3. The parallel scan: workers claim runs of the superblocks of the index, and the groups are the heap's
SELECT workers > 0 AS workers_launched, runs > 1 AS many_runs FROM sift._test_store_par('c1_idx', 2, 1, NULL, NULL, 2) LIMIT 1;
SELECT sum(n) = (SELECT count(*) FROM c1) AS par_counts_every_row, count(*) AS par_groups, count(DISTINCT workers) AS one_worker_count FROM sift._test_store_par('c1_idx', 2, 1, NULL, NULL, 2);
SELECT diff_par('c1_idx', 'c1', 2, 'g', 1, 'sum(id)', NULL, NULL, 'true', 2) AS par_two_workers;
SELECT diff_par('c1_idx', 'c1', 2, 'g', 1, 'sum(id)', NULL, NULL, 'true', 3) AS par_three_workers;
SELECT diff_par('c1_idx', 'c1', 2, 'g', NULL, 'NULL::int8', 1, 29000, 'id > 29000', 2) AS par_pruning_qual;
INSERT INTO c1 SELECT i, i % 7, i, 'late', repeat('p', 150) FROM generate_series(60001, 61000) i;
DELETE FROM c1 WHERE id % 13 = 0;
SELECT diff_par('c1_idx', 'c1', 2, 'g', 1, 'sum(id)', NULL, NULL, 'true', 2) AS par_after_writes;
VACUUM c1;
SELECT diff_par('c1_idx', 'c1', 2, 'g', 1, 'sum(id)', 1, 500, 'id > 500', 2) AS par_after_vacuum;
SELECT workers > 0 AS workers_launched FROM sift._test_store_par('c1_idx', 2, 1, NULL, NULL, 2) LIMIT 1;

DROP TABLE c1, t1;
DROP FUNCTION diff_agg, diff_par, sift._test_store_agg, sift._test_store_par, sift._test_store_dict, sift._test_store_ops;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
